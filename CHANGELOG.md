# Changelog

## 1.1.1 (2026-11)
- **Lighter on the CPU, without the spikes.** 1.1.0 measured 10.9 % of an audio block (99th percentile) and 28 % at worst on an MPC One. Almost all of that came from one thing: switching to ECHO cleared its 3-second (1 MB) echo memory in a single audio block. Grit now marks the old echo as silent instead of clearing it, so switching effects costs next to nothing. Re-run TEST in MPC TOOLS to see the new figure.
- KILL does a quarter less work (its two crossovers share their first filter stage), RING turns its tone with a rotating phasor instead of a sine every sample, and CHOP keeps its own position instead of recalculating it every sample.
- **The sound is the same.** DUST, CRUSH, FILTER, KILL and ECHO give bit-for-bit the same output as 1.1.0 (checked sample by sample, including switching between effects); RING and CHOP differ by rounding only, far below hearing. Controls, presets and saved projects are unchanged.

## 1.1.0 (2026-11)
- **USER presets.** A new USER page with 8 slots: tap **SAVE**, then a slot, to keep the sound you have now; tap a slot to bring it back. Filled slots read USER 1 to USER 8, empty ones EMPTY, and the slot you loaded last is lit. No typing.
- The slots are kept in a file beside your VST2 Plugins folder (`PotentialSuite Presets/PotentialGrit.txt`), so every project and every copy of the plugin sees the same slots, and updating the plugin keeps them.
- Use the same slot number in PotentialPatina, PotentialPunch, PotentialHeat and PotentialBleed to keep a chain of your own.
- **MANUAL page.** The plugin's manual is now on the MPC's screen: tap **MANUAL**, then the page buttons along the top (what it does, the controls, presets, the FX chains and their insert order, USER presets). The full PotentialSuite manual is on the [PotentialSuite page](https://github.com/PotentialBreakfast/potential-plugins).
- Ten parameters were added at the end of the list (Save User Preset, User 1 to User 8, Manual Page); saved projects open as before.

## 1.0.1 (2026-11)
- Much lighter on the MPC's CPU: DUST's compressor and wobble, FILTER and RING now do their heavy maths every few samples instead of every sample, and the saturation uses a fast curve. Version 1.0.0 measured 13% on an MPC One; this should be a fraction of that. Re-run TEST in MPC TOOLS to see.

## 1.0.0

First release: seven lo-fi effects on pads (DUST, CRUSH, FILTER, KILL, ECHO, CHOP, RING), each with its own settings, crossfaded switching, MIX and OUTPUT, ROLL (MILD / MEDIUM / WILD), presets, the four FX chain presets, and the Print Plate, Night Rack and Colour Block looks.
