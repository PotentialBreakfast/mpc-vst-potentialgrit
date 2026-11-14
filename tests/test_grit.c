/* PotentialGrit's effects on the computer: each does what it says, nothing blows up, silence stays silent.
 * Build: gcc -fsanitize=address,undefined -O1 -std=gnu11 -I src -I vst/build -I <kit>/wrapper tests/test_grit.c -lm */
#include "../src/grit.c"
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } else if (0) {} } while (0)
static void set(grit_t *s, const char *k, double v) { char b[32]; snprintf(b, sizeof b, "%g", v); set_param(s, k, b); }

/* run n frames of a generator through; returns the output in out (float, L only) */
typedef float (*gen_t)(long t);
static float g_sine60(long t) { return 0.5f * sinf(2 * PI_F * 60.0f * t / FS); }
static float g_sine1k(long t) { return 0.5f * sinf(2 * PI_F * 1000.0f * t / FS); }
static float g_silence(long t) { (void)t; return 0; }
static float g_impulse(long t) { return t == 1000 ? 0.8f : 0.0f; }
static float g_noise(long t) { static uint32_t r = 7; (void)t; return 0.3f * frand(&r); }
static void run(grit_t *s, gen_t g, long n, float *outL, double ppq_start, int playing, float bpm) {
    int16_t in[256], out[256];
    for (long t = 0; t < n; t += 128) {
        double ppq = ppq_start + t / FS * bpm / 60.0;
        song_pos(s, playing ? ppq : -1, bpm, playing, 128);
        for (int i = 0; i < 128; i++) { float v = g(t + i); in[2 * i] = in[2 * i + 1] = (int16_t)lrintf(v * 32767.0f); }
        process(s, in, out, 128);
        for (int i = 0; i < 128 && t + i < n; i++) if (outL) outL[t + i] = out[2 * i] / 32768.0f;
    }
}
static double rms(const float *x, long a, long b) { double e = 0; for (long i = a; i < b; i++) e += x[i] * x[i]; return sqrt(e / (b - a)); }

int main(void) {
    static float y[44100 * 3];
    const long N = 44100;
    /* every effect at its defaults: bounded output, silence in = silence out */
    for (int fx = 0; fx < NFX; fx++) {
        grit_t *s = create(NULL); set(s, "fx", fx);
        run(s, g_noise, N, y, 0, 1, 100);
        float mx = 0; for (long i = 0; i < N; i++) { float a = fabsf(y[i]); if (!(a == a)) a = 9; if (a > mx) mx = a; }
        CHECK(mx <= 1.0f, "fx %d: output %f", fx, mx);
        destroy(s);
        s = create(NULL); set(s, "fx", fx); set(s, "dust_noise", 100);
        run(s, g_silence, N, y, 0, 1, 100);
        CHECK(rms(y, 0, N) < 1e-4, "fx %d: silence in gives %g out", fx, rms(y, 0, N));
        destroy(s);
    }
    /* DUST: noise stops after the music stops */
    { grit_t *s = create(NULL); set(s, "dust_noise", 100);
      run(s, g_sine1k, N, y, 0, 0, 90); run(s, g_silence, N * 2, y, 0, 0, 90);
      CHECK(rms(y, N + 70000, N * 2) < 1e-4, "dust: noise still playing 1.6 s after silence (%g)", rms(y, N + 70000, N * 2)); destroy(s); }
    /* KILL: flat at 0 dB, the low band gone at KILL */
    { grit_t *s = create(NULL); set(s, "fx", 3); static float x[44100];
      run(s, g_sine1k, N, y, 0, 0, 90); for (long i = 0; i < N; i++) x[i] = g_sine1k(i);
      double ry = rms(y, 4000, N), rx = rms(x, 4000, N);   /* a crossover is flat in level (its phase turns) */
      CHECK(fabs(20 * log10(ry / rx)) < 0.5, "kill: not flat at 0 dB (%+.2f dB)", 20 * log10(ry / rx));
      set(s, "kill_low", -40); run(s, g_sine60, N, y, 0, 0, 90);
      CHECK(rms(y, 10000, N) < 0.06, "kill: 60 Hz not killed (rms %g)", rms(y, 10000, N));
      set(s, "kill_low", 0); set(s, "kill_high", -40); run(s, g_sine60, N, y, 0, 0, 90);
      CHECK(rms(y, 10000, N) > 0.3, "kill: high kill took the lows (rms %g)", rms(y, 10000, N)); destroy(s); }
    /* ECHO: 1/4 at 120 BPM = 22050 samples later */
    { grit_t *s = create(NULL); set(s, "fx", 4); set(s, "echo_time", 4); set(s, "echo_fb", 0); set(s, "echo_level", 100);
      run(s, g_impulse, N, y, 0, 0, 120);
      long pk = 2000; float best = 0; for (long i = 2000; i < N; i++) if (fabsf(y[i]) > best) { best = fabsf(y[i]); pk = i; }
      CHECK(labs(pk - (1000 + 22050)) < 40, "echo: first repeat at %ld, want %d", pk, 1000 + 22050); destroy(s); }
    /* CHOP 1/16 at 120 BPM, full depth: the second half of each step is silent, following the song position */
    { grit_t *s = create(NULL); set(s, "fx", 5); set(s, "chop_rate", 3);
      run(s, g_sine1k, N, y, 0, 1, 120);   /* a 1/16 is 5512.5 samples */
      double on = rms(y, 5512 * 4 + 200, 5512 * 4 + 2800), off = rms(y, 5512 * 4 + 3300, 5512 * 5 - 200);
      CHECK(on > 0.25 && off < 0.01, "chop: gate on %g off %g", on, off); destroy(s); }
    /* CRUSH at 16 bit and full rate is nearly clean */
    { grit_t *s = create(NULL); set(s, "fx", 1); set(s, "crush_bits", 16); set(s, "crush_rate", 100); set(s, "crush_tone", 100);
      run(s, g_sine1k, N, y, 0, 0, 90); CHECK(fabs(rms(y, 4000, N) - 0.3536) < 0.03, "crush clean: rms %g", rms(y, 4000, N)); destroy(s); }
    /* switching pads crossfades: no jump bigger than the signal's own steps */
    { grit_t *s = create(NULL); int16_t in[256], out[256]; float last = 0, jump = 0; long t = 0;
      for (int blk = 0; blk < 2000; blk++) {
          if (blk % 100 == 50) set(s, "fx", (blk / 100) % NFX);
          song_pos(s, t / FS * 2.0, 120, 1, 128);
          for (int i = 0; i < 128; i++, t++) in[2 * i] = in[2 * i + 1] = (int16_t)lrintf(0.3f * sinf(2 * PI_F * 110.0f * t / FS) * 32767);
          process(s, in, out, 128);
          for (int i = 0; i < 128; i++) { float v = out[2 * i] / 32768.0f, d = fabsf(v - last); if (blk % 100 >= 50 && blk % 100 < 52 && d > jump) jump = d; last = v; }
      }
      CHECK(jump < 0.35f, "switching: a %.3f jump", jump); destroy(s); }
    /* ROLL: 3000 rolls at each wildness keep every value in its range */
    { grit_t *s = create(NULL); const char *keys[] = {"dust_comp","dust_noise","dust_wobble","crush_rate","crush_tone","filt_cutoff","filt_res","filt_drive","echo_level","chop_depth","chop_pan","ring_freq","ring_sweep","ring_speed","mix"};
      for (int w = 0; w < 3; w++) { set(s, "wild", w);
        for (int r = 0; r < 3000; r++) {
            set_param(s, "dice", "1");
            for (unsigned k = 0; k < sizeof keys / sizeof *keys; k++) { float v = s->p[find(keys[k])]; CHECK(v >= 0 && v <= 100, "roll %d: %s = %g", w, keys[k], v); }
            CHECK(s->p[P_CRUSH_BITS] >= 2 && s->p[P_CRUSH_BITS] <= 16, "roll bits %g", s->p[P_CRUSH_BITS]);
            for (int b = 0; b < 3; b++) CHECK(s->p[P_KILL_LOW + b] >= -40 && s->p[P_KILL_LOW + b] <= 6, "roll kill %g", s->p[P_KILL_LOW + b]);
            CHECK(s->p[P_ECHO_FB] <= 95 && ival(s, P_ECHO_TIME) <= 6 && ival(s, P_CHOP_RATE) <= 5 && ival(s, P_FX) < NFX, "roll options");
        } }
      char st[1024]; get_param(s, "state", st, sizeof st); grit_t *t2 = create(NULL); set_param(t2, "state", st);
      CHECK(!memcmp(s->p, t2->p, sizeof s->p) && s->wild == t2->wild, "state does not round-trip");
      destroy(s); destroy(t2); }
    /* value text */
    { grit_t *s = create(NULL); char b[64]; set(s, "kill_mid", -40); get_param(s, "kill_mid_display", b, sizeof b);
      CHECK(!strcmp(b, "KILL"), "kill text %s", b); get_param(s, "filt_cutoff_display", b, sizeof b); printf("cutoff at 62%%: %s\n", b); destroy(s); }
    /* CPU on this computer, per effect (the MPC is far slower; the number to compare is the ratio between effects) */
    for (int fx = 0; fx < NFX; fx++) {
        grit_t *s = create(NULL); set(s, "fx", fx); clock_t c0 = clock(); run(s, g_noise, N * 4, NULL, 0, 1, 100);
        printf("fx %d: %.2f %% of real time here\n", fx, 100.0 * (clock() - c0) / CLOCKS_PER_SEC / 4.0); destroy(s);
    }
    printf(fails ? "FAILED (%d)\n" : "OK: seven effects, kill, echo timing, chop on the song position, crossfade, roll, state\n", fails);
    return fails != 0;
}
