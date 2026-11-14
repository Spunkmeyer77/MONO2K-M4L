# MONO2K for Max for Live

[MONO2K](https://github.com/InigoHernandez/mono2k) (Iñigo Hernández's Monomachine-style instrument: six mono
machines, a 64-step sequencer with locks, conditions and ratchets, a lofi bus) as a free Max for Live instrument.
The MONO2K core runs **unchanged** inside a Max object, `mono2k_<version>~` (e.g. `mono2k_122~`: named per version so
an old frozen device can't lend a new one its code; the version lives in `VERSION`), so the device sounds exactly like the original.
GPL-3.0, like MONO2K.

```
core/               MONO2K's firmware core (firmware/src, firmware/gen), unchanged; MONO2K v0.4.1
external/mono2k~/   the Max object: m2k_engine.c (queue, Live clock, resampler, state), mono2k~.c (Max binding)
device/             MONO2K.amxd, mono2k_panel.js (the FM-1 panel), make_amxd.py (writes the .amxd)
tests/              engine_test.c: Live at 48 kHz, clock sync, start mid-song, save and reload
docs/               the user manual
```

## How it fits together

- **Audio**: the core renders 16-sample blocks at 44.1 kHz, as on the FM-1; the engine resamples (Hermite) to Live's rate.
- **Clock**: `plugphasor~` (Live's beat phase) into the object's inlet becomes MIDI clock, 24 pulses a beat, sample
  accurate. Live's `is_playing` and `current_song_time` (LOM) give start and stop and the song position, so starting
  Live mid-song starts the pattern on the matching step. The pulse offset within the step comes from the phase itself,
  so a late read of the song time doesn't shift the groove.
- **Controls**: the panel jsui sends `btn i down`, `key i down`, `enc i delta`; Live's MIDI comes in through
  `midiin` and is routed to the track in view or a fixed track (MIDI TO). Eight hidden `live.dial`s (`encabs`) let
  Push's knobs turn the encoders.
- **Screen**: the core's 240 x 240 1-bit frame at 60 fps, out as two jit_matrix (160 x 160 area-filtered for the
  device strip, 240 x 240 for the big window).
- **State**: the core's store (settings, patterns, projects, sounds; ~19 KB) is packed every 2 s when it changed, and
  goes into `[pattr m2kstate]`, a blob parameter, so it's saved in the Live set. 3 bytes an integer (exact in
  float32), with a length and a checksum.
- All core calls happen on the audio thread; Max's threads only queue events.

## Building

Push to GitHub: the workflow builds the Mac external (universal arm64 + x86_64, macOS 10.13+, minos checked
<= 11.0) and the Windows external (x64, clang-cl), and runs the engine test. A tag `vX.Y.Z` makes a release.

Locally (macOS):

```sh
git clone --depth 1 https://github.com/Cycling74/max-sdk-base external/max-sdk-base
cmake -S external/mono2k~ -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac
# -> build/externals/mono2k_<version>~.mxo
```

Engine test (any platform):

```sh
cc -O2 -Icore/src -Icore/gen -Iexternal/mono2k~ -o engine_test tests/engine_test.c \
   external/mono2k~/m2k_engine.c external/mono2k~/m2k_core_glue.c -lm && ./engine_test demo.wav
```

## Freezing the device for release (internal)

1. Put `MONO2K.amxd`, `mono2k_panel_<v>.js`, `mono2k_<v>~.mxo` **and** `mono2k_<v>~.mxe64` in one folder (the release's
   all-files zip; both externals, so the frozen device works on Mac and Windows). No older MONO2K files anywhere Max looks.
2. Load the device in Live, open it in Max, check the screen runs, click Freeze, save.
3. Test: copy only the frozen `.amxd` into an empty folder and load it from there, on a fresh Live set.

## Credits

MONO2K © Iñigo Hernández, GPL-3.0. Max for Live port: Ticco Ross. MONO2K's screen type: adapted from
"Elektron Pixel Font" by savingaurora (CC BY-SA 3.0; see `core/font-license`). Not affiliated with Elektron or M-VAVE.
