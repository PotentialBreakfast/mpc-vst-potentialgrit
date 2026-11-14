/* PotentialGrit (PotentialBreaks): a lo-fi multi-effect for MPC OS standalone devices.
 * Seven effects, one at a time, picked from a row of pads. Each keeps its own settings, and switching crossfades (~25 ms):
 *   DUST    glue compression with saturation and a darker top, surface hiss / crackle / rumble that follows the music
 *           (it fades out when the track goes quiet), and wobble (wow and flutter)
 *   CRUSH   bit depth and sample-rate reduction, then a tone (low-pass) control
 *   FILTER  a resonant state-variable filter (low / band / high pass) into a saturator
 *   KILL    a three-band kill EQ (below ~250 Hz, 250 Hz - 3 kHz, above ~3 kHz), each band -40 dB (KILL) to +6 dB
 *   ECHO    a dark, saturating tape-style echo locked to the tempo (1/16 .. 1/2, dotted and triplet); the dry sound passes
 *   CHOP    a tempo gate that follows the song position while MPC plays, with left / right bounce
 *   RING    ring modulation by a sine whose pitch an LFO can sweep, two octaves each way
 * then MIX (dry / effect) and OUTPUT. ROLL picks new settings (MILD / MEDIUM / WILD; WILD may change the effect too).
 * 44.1 kHz, interleaved int16 stereo, 128-frame blocks (wrapper/engine.h). Original code; the idea of a pad-switched
 * lo-fi box is a classic of groove samplers. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "../shared/looks/potential_theme.h"   /* the LOOK switch (parameter "theme") */

#define FS 44100.0f
#define PI_F 3.14159265f

/* ---- parameters: same order and keys as vst/params.json (LOOK, ROLL and WILD are handled on their own) ---- */
enum {
    P_MIX, P_OUT, P_FX,
    P_DUST_COMP, P_DUST_NOISE, P_DUST_WOB,
    P_CRUSH_BITS, P_CRUSH_RATE, P_CRUSH_TONE,
    P_FILT_MODE, P_FILT_CUT, P_FILT_RES, P_FILT_DRIVE,
    P_KILL_LOW, P_KILL_MID, P_KILL_HIGH,
    P_ECHO_TIME, P_ECHO_FB, P_ECHO_LEVEL,
    P_CHOP_RATE, P_CHOP_DEPTH, P_CHOP_PAN,
    P_RING_FREQ, P_RING_SWEEP, P_RING_SPEED,
    NP
};
static const struct { const char *key; float def; } PDEF[NP] = {
    {"mix", 100}, {"output", 0}, {"fx", 0},
    {"dust_comp", 60}, {"dust_noise", 30}, {"dust_wobble", 25},
    {"crush_bits", 10}, {"crush_rate", 62}, {"crush_tone", 60},
    {"filt_mode", 0}, {"filt_cutoff", 62}, {"filt_res", 30}, {"filt_drive", 35},
    {"kill_low", 0}, {"kill_mid", 0}, {"kill_high", 0},
    {"echo_time", 3}, {"echo_fb", 40}, {"echo_level", 50},
    {"chop_rate", 3}, {"chop_depth", 100}, {"chop_pan", 0},
    {"ring_freq", 62}, {"ring_sweep", 0}, {"ring_speed", 43},
};
enum { FX_DUST, FX_CRUSH, FX_FILTER, FX_KILL, FX_ECHO, FX_CHOP, FX_RING, NFX };
static const int FX_FIRST[NFX] = {P_DUST_COMP, P_CRUSH_BITS, P_FILT_MODE, P_KILL_LOW, P_ECHO_TIME, P_CHOP_RATE, P_RING_FREQ};
static const int FX_LAST[NFX] = {P_DUST_WOB, P_CRUSH_TONE, P_FILT_DRIVE, P_KILL_HIGH, P_ECHO_LEVEL, P_CHOP_PAN, P_RING_SPEED};
static const float ECHO_BEATS[7] = {0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};    /* 1/16 1/8T 1/8 1/8D 1/4 1/4D 1/2 */
static const float CHOP_BEATS[6] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};  /* 1/4 1/8 1/8T 1/16 1/16T 1/32 */

/* knob position 0..100 -> its value on a log scale */
static inline float logmap(float pct, float lo, float hi) { return lo * powf(hi / lo, pct / 100.0f); }
static inline float crush_khz(float pct) { return logmap(pct, 1.5f, 44.1f); }
static inline float cutoff_hz(float pct) { return logmap(pct, 40.0f, 18000.0f); }
static inline float ring_hz(float pct) { return logmap(pct, 20.0f, 3000.0f); }
static inline float speed_hz(float pct) { return logmap(pct, 0.05f, 10.0f); }

#define ECHO_LEN 131072          /* 2.97 s: a 1/2 note down to 40 BPM */
#define WOB_LEN 2048
#define XFADE 1100               /* samples (~25 ms) */

typedef struct { float ic1, ic2; } svf_t;   /* one TPT state-variable filter's memory */

typedef struct {
    float p[NP];
    pt_theme_t theme;
    int wild;
    uint64_t pending;
    uint32_t dice, rng;
    /* tempo (song_pos) */
    double ppq; float bpm; int playing, have_pos;
    double free_beats;           /* CHOP's own clock while MPC is stopped */
    /* switching */
    int cur, prev, xf;           /* xf: samples left in the crossfade from prev to cur */
    float s_mix, s_out;
    /* DUST */
    float d_env, d_gate, d_lp[2], d_hiss_hp[2], d_hiss_lp[2], d_rumble, d_crack, d_crack_sign;
    float d_wbuf[2][WOB_LEN]; int d_wpos; float d_wow, d_flut, d_drift, d_drift_t; int d_drift_n;
    /* CRUSH */
    float c_hold[2], c_ph, c_lp[2];
    /* FILTER: TPT state-variable filter */
    float f_ic1[2], f_ic2[2], f_cut;
    /* KILL: Linkwitz-Riley 24 dB/oct crossovers, each two Butterworth state-variable filters in a row */
    svf_t k_svf[2][8];
    /* ECHO */
    float e_buf[2][ECHO_LEN]; int e_pos; float e_del, e_lp[2], e_hp[2];
    /* RING */
    float r_ph, r_lfo;
} grit_t;

static inline float frand(uint32_t *s) {   /* xorshift32 -> -1..1 */
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
static inline float urand(uint32_t *s) { return 0.5f * (frand(s) + 1.0f); }
static inline float onepole_k(float hz) { return 1.0f - expf(-2.0f * PI_F * hz / FS); }
static inline int ival(const grit_t *s, int i) { return (int)lroundf(s->p[i]); }
static inline int is_option(int i) { return i == P_FX || i == P_FILT_MODE || i == P_ECHO_TIME || i == P_CHOP_RATE || i == P_CRUSH_BITS; }

/* an effect's own memory, cleared when it comes on, so it never starts with an old tail */
static void fx_reset(grit_t *s, int fx) {
    switch (fx) {
    case FX_DUST:
        s->d_env = 0; memset(s->d_lp, 0, sizeof s->d_lp); memset(s->d_wbuf, 0, sizeof s->d_wbuf);
        s->d_crack = 0; break;
    case FX_CRUSH: memset(s->c_hold, 0, sizeof s->c_hold); s->c_ph = 0; memset(s->c_lp, 0, sizeof s->c_lp); break;
    case FX_FILTER: memset(s->f_ic1, 0, sizeof s->f_ic1); memset(s->f_ic2, 0, sizeof s->f_ic2); break;
    case FX_KILL: memset(s->k_svf, 0, sizeof s->k_svf); break;
    case FX_ECHO: memset(s->e_buf, 0, sizeof s->e_buf); memset(s->e_lp, 0, sizeof s->e_lp); memset(s->e_hp, 0, sizeof s->e_hp); s->e_del = 0; break;
    default: break;
    }
}

static void reset(grit_t *s) {
    pt_theme_t theme = s->theme;
    memset(s, 0, sizeof *s);
    s->theme = theme;
    for (int i = 0; i < NP; i++) s->p[i] = PDEF[i].def;
    s->rng = 0x1234567u;
    s->dice = 0x9e3779b9u ^ (uint32_t)(uintptr_t)s;
    s->wild = 1;
    s->bpm = 90; s->d_crack_sign = 1;
    s->cur = s->prev = FX_DUST;
    s->s_mix = 1; s->s_out = 1;
    s->f_cut = cutoff_hz(PDEF[P_FILT_CUT].def);
}

/* ---- ROLL: new settings for the effect that's on; WILD may pick another effect first ---- */
static float dice_u(grit_t *s) { uint32_t x = s->dice; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->dice = x; return (x >> 8) / 16777216.0f; }
static float dice_in(grit_t *s, float lo, float hi) { return roundf(lo + (hi - lo) * dice_u(s)); }
static int dice_opt(grit_t *s, int lo, int hi) { return lo + (int)(dice_u(s) * (float)(hi - lo + 1) * 0.9999f); }
static void roll(grit_t *s) {
    int w = s->wild;
    if (w == 2 && dice_u(s) < 0.6f) { s->p[P_FX] = (float)dice_opt(s, 0, NFX - 1); s->pending |= 1ull << P_FX; }
    int fx = ival(s, P_FX);
    /* how far each setting may go: MILD keeps to the useful middle, WILD to the ends */
    const float a = w == 0 ? 0.25f : w == 1 ? 0.5f : 0.9f;
    float lo = 50 - 50 * a, hi = 50 + 50 * a;
    switch (fx) {
    case FX_DUST:
        s->p[P_DUST_COMP] = dice_in(s, 30 + 10 * (1 - a), hi + 10 > 100 ? 100 : hi + 10);
        s->p[P_DUST_NOISE] = dice_in(s, 0, 25 + 60 * a);
        s->p[P_DUST_WOB] = dice_in(s, 0, 20 + 70 * a);
        break;
    case FX_CRUSH:
        s->p[P_CRUSH_BITS] = dice_in(s, w == 2 ? 3 : w == 1 ? 6 : 9, w == 0 ? 13 : 12);
        s->p[P_CRUSH_RATE] = dice_in(s, w == 2 ? 15 : w == 1 ? 35 : 55, w == 0 ? 85 : 80);
        s->p[P_CRUSH_TONE] = dice_in(s, 35, 80);
        break;
    case FX_FILTER:
        s->p[P_FILT_MODE] = w == 0 ? 0 : (float)dice_opt(s, 0, 2);
        s->p[P_FILT_CUT] = dice_in(s, lo, hi);
        s->p[P_FILT_RES] = dice_in(s, 5, 25 + 55 * a);
        s->p[P_FILT_DRIVE] = dice_in(s, 10, 25 + 60 * a);
        break;
    case FX_KILL: {
        int kill = dice_opt(s, 0, 2);   /* one band goes (or nearly), the others move a little */
        for (int b = 0; b < 3; b++)
            s->p[P_KILL_LOW + b] = b == kill ? (w == 0 ? dice_in(s, -24, -12) : -40) : dice_in(s, -6 * a * 2, 3 * a + 1);
        break;
    }
    case FX_ECHO:
        s->p[P_ECHO_TIME] = (float)(w == 0 ? 2 + dice_opt(s, 0, 2) : dice_opt(s, 0, 6));   /* MILD: 1/8, 1/8D or 1/4 */
        s->p[P_ECHO_FB] = dice_in(s, 15, 35 + 55 * a);
        s->p[P_ECHO_LEVEL] = dice_in(s, 25, 40 + 50 * a);
        break;
    case FX_CHOP:
        s->p[P_CHOP_RATE] = (float)(w == 0 ? 1 + dice_opt(s, 0, 2) * 1 : dice_opt(s, 0, 5));
        s->p[P_CHOP_DEPTH] = dice_in(s, 40 + 20 * (1 - a), 100);
        s->p[P_CHOP_PAN] = dice_in(s, 0, 30 + 70 * a);
        break;
    default:
        s->p[P_RING_FREQ] = dice_in(s, lo, hi);
        s->p[P_RING_SWEEP] = dice_in(s, 0, 15 + 70 * a);
        s->p[P_RING_SPEED] = dice_in(s, 20, 70);
        break;
    }
    s->p[P_MIX] = dice_in(s, w == 0 ? 55 : 65, w == 0 ? 85 : 100);
    if (fx == FX_RING) s->p[P_MIX] = dice_in(s, 25, 30 + 30 * a);   /* ring mod is a lot at full mix */
    if (fx == FX_ECHO) s->p[P_MIX] = 100;                          /* the dry sound already passes through */
    for (int i = FX_FIRST[fx]; i <= FX_LAST[fx]; i++) s->pending |= 1ull << i;
    s->pending |= 1ull << P_MIX;
}

int mpc_engine_changed(void *inst, char *key, int len) {
    grit_t *s = inst;
    for (int i = 0; i < NP; i++)
        if (s->pending & (1ull << i)) { s->pending &= ~(1ull << i); snprintf(key, len, "%s", PDEF[i].key); return 1; }
    return 0;
}

static void *create(const char *data_dir) {
    grit_t *s = calloc(1, sizeof *s);
    if (s) { reset(s); pt_theme_init(&s->theme, data_dir); }
    return s;
}
static void destroy(void *inst) { free(inst); }
static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }

static int find(const char *key) {
    for (int i = 0; i < NP; i++) if (!strcmp(key, PDEF[i].key)) return i;
    return -1;
}

static void set_param(void *inst, const char *key, const char *val) {
    grit_t *s = inst;
    if (pt_theme_set(&s->theme, key, val)) return;
    if (!strcmp(key, "state")) {   /* "key=value;key=value;..." */
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", val);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            int i = find(tok);
            if (i >= 0) s->p[i] = (float)atof(eq + 1);
            else if (!strcmp(tok, "wild")) { int w = atoi(eq + 1); s->wild = w < 0 ? 0 : w > 2 ? 2 : w; }
        }
        int fx = ival(s, P_FX);
        if (fx < 0 || fx >= NFX) s->p[P_FX] = 0;
        return;
    }
    if (!strcmp(key, "dice")) { if (atof(val) > 0.5) roll(s); return; }
    if (!strcmp(key, "wild")) { int w = (int)lroundf((float)atof(val)); s->wild = w < 0 ? 0 : w > 2 ? 2 : w; return; }
    int i = find(key);
    if (i >= 0) s->p[i] = (float)atof(val);
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    grit_t *s = inst;
    { int n = pt_theme_get(&s->theme, key, buf, buf_len); if (n) return n; }
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 0; i < NP && n < buf_len; i++) n += snprintf(buf + n, buf_len - n, "%s=%g;", PDEF[i].key, (double)s->p[i]);
        if (n < buf_len) n += snprintf(buf + n, buf_len - n, "wild=%d;", s->wild);
        return n < buf_len ? n : buf_len - 1;
    }
    if (!strcmp(key, "dice")) return snprintf(buf, buf_len, "0");
    if (!strcmp(key, "wild")) return snprintf(buf, buf_len, "%d", s->wild);
    /* value text for the log-scale knobs and the kill bands */
    size_t kl = strlen(key);
    if (kl > 8 && !strcmp(key + kl - 8, "_display")) {
        char base[48];
        snprintf(base, sizeof base, "%.*s", (int)(kl - 8), key);
        int i = find(base);
        if (i < 0) return 0;
        float v = s->p[i];
        switch (i) {
        case P_CRUSH_RATE: return snprintf(buf, buf_len, "%.1f kHz", (double)crush_khz(v));
        case P_FILT_CUT: { float hz = cutoff_hz(v); return hz >= 1000 ? snprintf(buf, buf_len, "%.1f kHz", (double)(hz / 1000)) : snprintf(buf, buf_len, "%.0f Hz", (double)hz); }
        case P_RING_FREQ: { float hz = ring_hz(v); return hz >= 1000 ? snprintf(buf, buf_len, "%.2f kHz", (double)(hz / 1000)) : snprintf(buf, buf_len, "%.0f Hz", (double)hz); }
        case P_RING_SPEED: return snprintf(buf, buf_len, "%.2f Hz", (double)speed_hz(v));
        case P_KILL_LOW: case P_KILL_MID: case P_KILL_HIGH:
            return v <= -39.5f ? snprintf(buf, buf_len, "KILL") : snprintf(buf, buf_len, "%+.0f dB", (double)v);
        default: return 0;
        }
    }
    int i = find(key);
    if (i < 0) return 0;
    if (is_option(i)) return snprintf(buf, buf_len, "%d", ival(s, i));
    return snprintf(buf, buf_len, "%g", (double)s->p[i]);
}

static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    grit_t *s = inst;
    (void)frames;
    if (bpm > 20 && bpm < 400) s->bpm = (float)bpm;
    s->ppq = ppq;
    s->playing = playing && ppq >= 0;
    s->have_pos = ppq >= 0;
}

static void render(void *inst, int16_t *out_lr, int frames) {   /* an effect: silence without input */
    (void)inst;
    memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
}

static inline float sat(float x) { return tanhf(x); }

/* ---- the seven effects: one block of stereo float in place (x[2][n]) ---- */
static void fx_dust(grit_t *s, float *L, float *R, int n) {
    float comp = s->p[P_DUST_COMP] / 100.0f, noise = s->p[P_DUST_NOISE] / 100.0f, wob = s->p[P_DUST_WOB] / 100.0f;
    float thr_db = -8.0f - 28.0f * comp, ratio = 1.5f + 8.5f * comp;
    float makeup = powf(10.0f, (-thr_db * (1.0f - 1.0f / ratio) * 0.55f) / 20.0f);
    float drive = 1.0f + 2.5f * comp, norm = 1.0f / tanhf(drive);
    float atk = onepole_k(1.0f / 0.003f), rel = onepole_k(1.0f / 0.12f);
    float dark = onepole_k(15000.0f - 9500.0f * comp);
    float hp_k = onepole_k(900.0f), lp_k = onepole_k(7500.0f), rum_k = onepole_k(35.0f);
    float crack_rate = (1.5f + 38.0f * noise * noise) / FS;
    float wow_inc = 2 * PI_F * 0.55f / FS, flut_inc = 2 * PI_F * 6.3f / FS;
    float depth = 140.0f * wob;   /* samples (~3.2 ms) */
    const float gate_up = onepole_k(1.0f / 0.02f), gate_down = 1.0f / (1.5f * FS);
    for (int i = 0; i < n; i++) {
        float l = L[i], r = R[i];
        /* compressor */
        float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        /* the surface noise follows the music: in while audio comes in, out ~1.5 s after it stops, so a silent track stays silent */
        if (a > 3e-4f) s->d_gate += (1.0f - s->d_gate) * gate_up;
        else if ((s->d_gate -= gate_down) < 0) s->d_gate = 0;
        s->d_env += (a - s->d_env) * (a > s->d_env ? atk : rel);
        float env_db = 20.0f * log10f(s->d_env + 1e-6f), gr_db = env_db > thr_db ? (env_db - thr_db) * (1.0f - 1.0f / ratio) : 0;
        float g = powf(10.0f, -gr_db / 20.0f) * makeup;
        l = sat(l * g * drive) * norm; r = sat(r * g * drive) * norm;
        s->d_lp[0] += (l - s->d_lp[0]) * dark; s->d_lp[1] += (r - s->d_lp[1]) * dark;
        l = s->d_lp[0]; r = s->d_lp[1];
        /* surface noise: hiss, crackle, rumble */
        if (noise > 0.001f && s->d_gate > 0) {
            for (int c = 0; c < 2; c++) {
                float w = frand(&s->rng);
                s->d_hiss_hp[c] += (w - s->d_hiss_hp[c]) * hp_k;
                float h = w - s->d_hiss_hp[c];
                s->d_hiss_lp[c] += (h - s->d_hiss_lp[c]) * lp_k;
            }
            if (urand(&s->rng) < crack_rate) { s->d_crack = 0.15f + 0.6f * urand(&s->rng); s->d_crack_sign = frand(&s->rng) > 0 ? 1.0f : -1.0f; }
            float ck = s->d_crack * s->d_crack_sign;
            s->d_crack *= 0.82f;
            s->d_rumble += (frand(&s->rng) - s->d_rumble) * rum_k;
            float ng = noise * s->d_gate;
            float hl = s->d_hiss_lp[0] * 0.05f * ng, hr = s->d_hiss_lp[1] * 0.05f * ng;
            float rm = s->d_rumble * 0.4f * ng, cr = ck * 0.35f * ng;
            l += hl + rm + cr; r += hr + rm + cr * 0.8f;
        }
        /* wobble: a modulated delay */
        s->d_wbuf[0][s->d_wpos] = l; s->d_wbuf[1][s->d_wpos] = r;
        float m = 0;
        if (depth > 0.01f) {
            s->d_wow += wow_inc; if (s->d_wow > 2 * PI_F) s->d_wow -= 2 * PI_F;
            s->d_flut += flut_inc; if (s->d_flut > 2 * PI_F) s->d_flut -= 2 * PI_F;
            if (--s->d_drift_n <= 0) { s->d_drift_t = frand(&s->rng); s->d_drift_n = 22050; }
            s->d_drift += (s->d_drift_t - s->d_drift) * 0.00005f;
            m = depth * (0.7f * sinf(s->d_wow) + 0.12f * sinf(s->d_flut) + 0.3f * s->d_drift);
        }
        float d = 8.0f + depth + m;   /* the fixed part keeps the read behind the write */
        float rp = (float)s->d_wpos - d;
        while (rp < 0) rp += WOB_LEN;
        int i0 = (int)rp, i1 = (i0 + 1) & (WOB_LEN - 1);
        float f = rp - (float)i0;
        L[i] = s->d_wbuf[0][i0] + (s->d_wbuf[0][i1] - s->d_wbuf[0][i0]) * f;
        R[i] = s->d_wbuf[1][i0] + (s->d_wbuf[1][i1] - s->d_wbuf[1][i0]) * f;
        s->d_wpos = (s->d_wpos + 1) & (WOB_LEN - 1);
    }
}

static void fx_crush(grit_t *s, float *L, float *R, int n) {
    float bits = s->p[P_CRUSH_BITS], step = powf(2.0f, bits - 1.0f), inv = 1.0f / step;
    float inc = crush_khz(s->p[P_CRUSH_RATE]) / 44.1f;
    float tone = onepole_k(logmap(s->p[P_CRUSH_TONE], 500.0f, 20000.0f));
    for (int i = 0; i < n; i++) {
        s->c_ph += inc;
        if (s->c_ph >= 1.0f) {
            s->c_ph -= floorf(s->c_ph);
            s->c_hold[0] = roundf(L[i] * step) * inv;
            s->c_hold[1] = roundf(R[i] * step) * inv;
        }
        s->c_lp[0] += (s->c_hold[0] - s->c_lp[0]) * tone; s->c_lp[1] += (s->c_hold[1] - s->c_lp[1]) * tone;
        L[i] = s->c_lp[0]; R[i] = s->c_lp[1];
    }
}

static void fx_filter(grit_t *s, float *L, float *R, int n) {
    int mode = ival(s, P_FILT_MODE);
    float target = cutoff_hz(s->p[P_FILT_CUT]), res = s->p[P_FILT_RES] / 100.0f;
    float k = 2.0f - 1.96f * res;                     /* damping: 2 = none, ~0.04 = near self-oscillation */
    float drive = 1.0f + 6.0f * s->p[P_FILT_DRIVE] / 100.0f, norm = 1.0f / sqrtf(drive);
    float in_g = 1.0f - 0.45f * res;                 /* keep the peak in check as the resonance rises */
    for (int i = 0; i < n; i++) {
        if ((i & 15) == 0) s->f_cut += (target - s->f_cut) * 0.25f;   /* glide, no zipper noise */
        float fc = s->f_cut > 18000.0f ? 18000.0f : s->f_cut;
        float g = tanf(PI_F * fc / FS), a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float *x[2] = {&L[i], &R[i]};
        for (int c = 0; c < 2; c++) {
            float v0 = *x[c] * in_g;
            float v3 = v0 - s->f_ic2[c], v1 = a1 * s->f_ic1[c] + a2 * v3, v2 = s->f_ic2[c] + a2 * s->f_ic1[c] + a3 * v3;
            s->f_ic1[c] = 2 * v1 - s->f_ic1[c]; s->f_ic2[c] = 2 * v2 - s->f_ic2[c];
            float y = mode == 0 ? v2 : mode == 1 ? v1 * k : v0 - k * v1 - v2;
            *x[c] = sat(y * drive) * norm * (1.0f + 0.3f * (drive - 1.0f) / 6.0f);
        }
    }
}

static inline float band_gain(float db) { return db <= -39.5f ? 0.0f : powf(10.0f, db / 20.0f); }
/* a Butterworth (k = sqrt 2) state-variable filter step: low-pass (hp = 0) or high-pass (hp = 1) output */
static inline float svf2(svf_t *f, float x, float g, int hp) {
    const float k = 1.41421356f, a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - f->ic2, v1 = a1 * f->ic1 + a2 * v3, v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2 * v1 - f->ic1; f->ic2 = 2 * v2 - f->ic2;
    return hp ? x - k * v1 - v2 : v2;
}
static void fx_kill(grit_t *s, float *L, float *R, int n) {
    float gl = band_gain(s->p[P_KILL_LOW]), gm = band_gain(s->p[P_KILL_MID]), gh = band_gain(s->p[P_KILL_HIGH]);
    const float g1 = tanf(PI_F * 250.0f / FS), g2 = tanf(PI_F * 3000.0f / FS);
    for (int i = 0; i < n; i++) {
        float *x[2] = {&L[i], &R[i]};
        for (int c = 0; c < 2; c++) {
            svf_t *f = s->k_svf[c];
            float v = *x[c];
            float lo = svf2(&f[1], svf2(&f[0], v, g1, 0), g1, 0);       /* below 250 Hz */
            float rest = svf2(&f[3], svf2(&f[2], v, g1, 1), g1, 1);     /* above it */
            float mid = svf2(&f[5], svf2(&f[4], rest, g2, 0), g2, 0);   /* 250 Hz - 3 kHz */
            float hi = svf2(&f[7], svf2(&f[6], rest, g2, 1), g2, 1);    /* above 3 kHz */
            *x[c] = lo * gl + mid * gm + hi * gh;
        }
    }
}

static void fx_echo(grit_t *s, float *L, float *R, int n) {
    float beats = ECHO_BEATS[ival(s, P_ECHO_TIME) < 0 ? 0 : ival(s, P_ECHO_TIME) > 6 ? 6 : ival(s, P_ECHO_TIME)];
    float target = beats * 60.0f / s->bpm * FS;
    if (target > ECHO_LEN - 4) target = ECHO_LEN - 4;
    if (s->e_del <= 0) s->e_del = target;
    float fb = s->p[P_ECHO_FB] / 100.0f, lvl = s->p[P_ECHO_LEVEL] / 100.0f;
    float lp = onepole_k(3200.0f), hp = onepole_k(110.0f);
    for (int i = 0; i < n; i++) {
        s->e_del += (target - s->e_del) * 0.00012f;   /* a time change glides, bending the echoes like tape */
        float rp = (float)s->e_pos - s->e_del;
        while (rp < 0) rp += ECHO_LEN;
        int i0 = (int)rp, i1 = i0 + 1 >= ECHO_LEN ? 0 : i0 + 1;
        float f = rp - (float)i0;
        float in[2] = {L[i], R[i]}, out[2];
        for (int c = 0; c < 2; c++) {
            float e = s->e_buf[c][i0] + (s->e_buf[c][i1] - s->e_buf[c][i0]) * f;
            s->e_lp[c] += (e - s->e_lp[c]) * lp;
            s->e_hp[c] += (s->e_lp[c] - s->e_hp[c]) * hp;
            float wet = s->e_lp[c] - s->e_hp[c];
            s->e_buf[c][s->e_pos] = sat(in[c] + wet * fb * 1.05f);   /* tape: repeats saturate as they build */
            out[c] = in[c] + wet * lvl;
        }
        L[i] = out[0]; R[i] = out[1];
        if (++s->e_pos >= ECHO_LEN) s->e_pos = 0;
    }
}

static void fx_chop(grit_t *s, float *L, float *R, int n) {
    float step = CHOP_BEATS[ival(s, P_CHOP_RATE) < 0 ? 0 : ival(s, P_CHOP_RATE) > 5 ? 5 : ival(s, P_CHOP_RATE)];
    float depth = s->p[P_CHOP_DEPTH] / 100.0f, pan = s->p[P_CHOP_PAN] / 100.0f;
    double bps = (double)s->bpm / 60.0 / FS;
    double beats = s->playing ? s->ppq : s->free_beats;   /* follows the song while MPC plays, its own clock otherwise */
    double ramp = 0.0025 * s->bpm / 60.0 / step;          /* ~2.5 ms edges, as a fraction of a step */
    for (int i = 0; i < n; i++) {
        double pos = (beats + i * bps) / step;
        double ph = pos - floor(pos);
        long idx = (long)floor(pos);
        double gate = ph < 0.55 ? 1.0 : 0.0;
        if (ph < ramp) gate = ph / ramp;
        else if (ph >= 0.55 && ph < 0.55 + ramp) gate = 1.0 - (ph - 0.55) / ramp;
        float g = 1.0f - depth * (1.0f - (float)gate);
        float gl = g, gr = g;
        if (idx & 1) gl *= 1.0f - pan; else gr *= 1.0f - pan;
        L[i] *= gl; R[i] *= gr;
    }
    s->free_beats = (s->playing ? s->ppq : s->free_beats) + n * bps;
}

static void fx_ring(grit_t *s, float *L, float *R, int n) {
    float base = ring_hz(s->p[P_RING_FREQ]), sweep = s->p[P_RING_SWEEP] / 100.0f;
    float lfo_inc = 2 * PI_F * speed_hz(s->p[P_RING_SPEED]) / FS;
    for (int i = 0; i < n; i++) {
        s->r_lfo += lfo_inc; if (s->r_lfo > 2 * PI_F) s->r_lfo -= 2 * PI_F;
        float f = base * exp2f(2.0f * sweep * sinf(s->r_lfo));
        s->r_ph += 2 * PI_F * f / FS; if (s->r_ph > 2 * PI_F) s->r_ph -= 2 * PI_F;
        float c = sinf(s->r_ph);
        L[i] *= c; R[i] *= c;
    }
}

static void run_fx(grit_t *s, int fx, float *L, float *R, int n) {
    switch (fx) {
    case FX_DUST: fx_dust(s, L, R, n); break;
    case FX_CRUSH: fx_crush(s, L, R, n); break;
    case FX_FILTER: fx_filter(s, L, R, n); break;
    case FX_KILL: fx_kill(s, L, R, n); break;
    case FX_ECHO: fx_echo(s, L, R, n); break;
    case FX_CHOP: fx_chop(s, L, R, n); break;
    default: fx_ring(s, L, R, n); break;
    }
}

static void process_block(grit_t *s, const int16_t *in_lr, int16_t *out_lr, int frames);
static void process(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames) {
    for (int done = 0; done < frames; done += 128) {   /* MPC sends 128; any other block size is done in 128s */
        int n = frames - done < 128 ? frames - done : 128;
        process_block(inst, in_lr + 2 * done, out_lr + 2 * done, n);
    }
}
static void process_block(grit_t *s, const int16_t *in_lr, int16_t *out_lr, int frames) {
    float dl[128], dr[128], al[128], ar[128], bl[128], br[128];
    for (int i = 0; i < frames; i++) {
        dl[i] = in_lr[2 * i] * (1.0f / 32768.0f);
        dr[i] = in_lr[2 * i + 1] * (1.0f / 32768.0f);
    }
    int want = ival(s, P_FX);
    if (want < 0 || want >= NFX) want = 0;
    if (want != s->cur) {   /* a new pad: crossfade from the old effect, the new one starting clean */
        s->prev = s->cur;
        s->cur = want;
        fx_reset(s, want);
        s->xf = XFADE;
    }
    memcpy(al, dl, sizeof(float) * frames); memcpy(ar, dr, sizeof(float) * frames);
    run_fx(s, s->cur, al, ar, frames);
    if (s->xf > 0) {
        memcpy(bl, dl, sizeof(float) * frames); memcpy(br, dr, sizeof(float) * frames);
        run_fx(s, s->prev, bl, br, frames);
        for (int i = 0; i < frames; i++) {
            float t = s->xf > 0 ? (float)s->xf / XFADE : 0.0f;   /* 1 -> 0: the old effect's share */
            al[i] = al[i] * (1.0f - t) + bl[i] * t;
            ar[i] = ar[i] * (1.0f - t) + br[i] * t;
            if (s->xf > 0) s->xf--;
        }
    }
    float mix_t = s->p[P_MIX] / 100.0f, out_t = powf(10.0f, s->p[P_OUT] / 20.0f);
    for (int i = 0; i < frames; i++) {
        s->s_mix += (mix_t - s->s_mix) * 0.002f;
        s->s_out += (out_t - s->s_out) * 0.002f;
        float l = (dl[i] + (al[i] - dl[i]) * s->s_mix) * s->s_out;
        float r = (dr[i] + (ar[i] - dr[i]) * s->s_mix) * s->s_out;
        if (!(l == l)) l = 0;   /* never pass a NaN on */
        if (!(r == r)) r = 0;
        l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
        out_lr[2 * i] = l >= 1.0f ? 32767 : (int16_t)lrintf(l * 32768.0f);   /* the input's own scale */
        out_lr[2 * i + 1] = r >= 1.0f ? 32767 : (int16_t)lrintf(r * 32768.0f);
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, process, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
