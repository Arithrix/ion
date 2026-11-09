# Vendored code

Pinned copies with their license files intact (HANDOFF §13). Sources were checked at origin on 2026-11-06; see DECISIONS.md V-01 to V-04.

| Directory | Project | Version | License | Source |
|---|---|---|---|---|
| `synth_dexed/` | Synth_Dexed | `e3f524c` (2025-09-10) | GPL-3.0; MSFA parts Apache-2.0 | codeberg.org/dcoredump/Synth_Dexed |
| `ftxui/` | FTXUI | v7.0.3 (`f921fad`) | MIT | github.com/ArthurSonzogni/FTXUI |
| `miniaudio/` | miniaudio | 0.11.26 (`877ea4a`) | Public domain / MIT-0 | github.com/mackron/miniaudio |
| `dr_libs/` | dr_wav, dr_flac | `35b5c39` (2026-10-31) | Public domain / MIT-0 | github.com/mackron/dr_libs |
| `doctest/` | doctest | v2.5.3 (`2d0a935`) | MIT | github.com/doctest/doctest |
| `nlohmann/` | nlohmann/json | v3.12.0 (`55f9368`) | MIT | github.com/nlohmann/json |
| `tomlplusplus/` | toml++ | v3.4.0 (`3017243`) | MIT | github.com/marzer/tomlplusplus |
| `libsamplerate/` | libsamplerate | 0.2.2 | BSD-2-Clause | github.com/libsndfile/libsamplerate release tarball, SHA-256 `3258da28…3893`; `src/` and `include/` identical to Debian's `libsamplerate_0.2.2.orig.tar.gz` |
| `plaits/` | Plaits (from `pichenettes/eurorack`) and the part of stmlib it uses | eurorack `f4cfa76` (2026-10-12), stmlib `e3bd7c9` (2023-05-30) | MIT | github.com/pichenettes/eurorack, github.com/pichenettes/stmlib |
| `open303/` | Open303 (Robin Schmidt), the DSP code | `313bf0d` (2024-03-29) | MIT (`fft4g.c`: Takuya Ooura's FFT, "free for any purpose") | github.com/RobinSchmidt/Open303 |
| `sqlite/` | SQLite amalgamation | 3.54.1 (2026-10-16) | Public domain | sqlite.org/2026/sqlite-amalgamation-3540100.zip, SHA3-256 `92281c86…8e30` matches the download page |

## Local changes
Two upstream changes, marked `ion:` in the source:
- **Synth_Dexed `src/env.cpp`, `Env::update`.** Upstream sends every held note's envelopes to stage 3 whenever a voice parameter changes ("for now we simply reset ourselves at level 3"). Under a CC sweep that silences notes still in their attack. ion's version keeps each envelope in its stage and retargets it, and glides a standing level (sustain, or a hold) to its new value within about 0.1 s. Covered by `tests/test_fm_handover.cpp` ("macros: …"), which fails against the upstream code.

- **Synth_Dexed `src/dexed.h`, `src/dx7note.h/.cpp`, `src/dexed.cpp` (`getSamples`): the uniform amp envelope (DECISIONS D-50).** Each `Dx7Note` carries an `ion::dsp::Ahdsr` (from `src/dsp/ahdsr.h`, header-only): `init()` starts it, `keyup()` releases it, `transferState()` carries it to a legato note. `getSamples` multiplies each voice's output by it and by velocity → amp, and marks a voice not live once its envelope is done. Marked `ion:`. With the envelope's defaults (wide open) the output is unchanged; the golden FM render checks that.

- **Plaits `plaits/dsp/dsp.h`: `kCorrectedSampleRate` is 48000** (upstream 47872.34, the module's real codec rate). At ion's true 48 kHz the upstream value plays everything 4.6 cents sharp (DECISIONS V-01). Marked `ion:`. `tests/test_plaits.cpp` checks A4 is 440 Hz.

- **FTXUI `src/ftxui/component/app.cpp`: mouse tracking mode 1002 (motion only while a button is held) instead of 1003 (all motion).** Every event FTXUI receives redraws the whole screen, and with 1003 a mouse crossing the terminal sends dozens. ion only needs motion for dragging. Marked `ion:` (DECISIONS D-62).

- **Open303** (marked `ion:`):
  - `rosic_Open303.h`: the held-note list was a `std::list`, which allocates on every note-on; it's a fixed 16-note stack with the same calls.
  - `rosic_Open303.h/.cpp`: the accent threshold (velocity 100) is settable (`setAccentThreshold`).
  - `rosic_Open303.cpp`: sliding back to a still-held note ignored the master tuning (`pitchToFreq(currentNote)` → `pitchToFreq(currentNote, tuning)`); `tests/test_acid.cpp` covers it.
  - `rosic_FunctionTemplates.h` and `rosic_Open303.h`: `<string.h>` and `<climits>` included (newer compilers don't pull them in).
  - `rosic_MipMappedWaveTable.cpp`, `initPrototypeTable`: zeroed 4 entries past the end of the 2048-entry table (into the next member); found by UBSan in the 0.10.1 test sweep.
  Only `Source/DSPCode/` is copied (no VST plug-in), with `License.txt` as `LICENSE`. Open303 never sets itself idle; ion's engine stops calling it after 200 ms of silence with no key down.

- **Plaits `plaits/dsp/fm/dx_units.h`, `NormalizeVelocity`:** a velocity of exactly 1.0 (every full-velocity note on the 6-op models) read one entry past the 17-entry cube-root table. Clamped just under 1. Found by the deep sweep. Marked `ion:`.
- **Plaits `plaits/dsp/speech/lpc_speech_synth_controller.cc`, `Render`:** the frame position of a word being spoken carried over when Harmonics moved from the word banks back to the vowels, and read far past the 15-frame phoneme table. An out-of-range position now goes back to scanning. Found by the deep sweep. Marked `ion:`.
- **Plaits `plaits/dsp/speech/lpc_speech_synth.h`, `PlayFrame`:** a triggered consonant (one frame, no blend) still read the frame after it, past the end of the phoneme table for the last consonant. It now reads the same frame twice. Found by the deep sweep (AddressSanitizer). Marked `ion:`.
- **Plaits `plaits/dsp/engine/wavetable_engine.cc`: the wave coordinates are kept under 7.0** (marked `ion:`). With Harmonics high (the quantised banks), the quantisation could make a coordinate exactly 7.0, and the reads went past the end of the 256-entry wave map: garbage pointers. Silent on the module, a crash on a computer. Found by the 0.10.1 sweep (random settings, AddressSanitizer).

- **Synth_Dexed: fixes from the 0.10.1 sweep** (marked `ion:`):
  - `dexed.cpp`, `Dexed::keydown`: the new note's time stamp was written to `voices[i]` (the loop counter) instead of `voices[note]`. When every voice was busy and one was stolen, `i` was one past the end of the voice array: a heap overflow (found by AddressSanitizer under random playing). Otherwise it stamped the wrong voice, so the next steal could take a note that wasn't the oldest.
  - `dexed.h` initialises `monoMode` (setMonoMode read it uninitialised); `lfo.cpp` clamps the LFO delay byte to 99 (above that it made a negative shift).
  - Built with `-fwrapv`: MSFA's phase accumulators wrap by design.

Two other things are worked around from our side instead:
- **Synth_Dexed** includes `<arm_math.h>` and expects Teensy/Circle helpers (`millis`, `constrain`, `signed_saturate_rshift`). `src/engine/fm/dexed_port/arm_math.h` provides them.
- **FTXUI:** only `include/`, `src/` (tests removed), `cmake/` and the top-level CMake files are copied. `examples/` and `doc/` hold just their `CMakeLists.txt`, which return early when those options are off; upstream's CMake adds both directories unconditionally.

Only part of Plaits is copied: `plaits/dsp/` without `voice.cc`/`voice.h`, and `resources.cc/h`. ion's own voice (`src/engine/plaits/plaits_voice.*`, adapted from upstream's `voice.cc`, MIT) replaces the voice, so nothing pulls in the STM32 flash code (`user_data.h`) or the firmware's UI, settings and drivers. From stmlib: `stmlib.h`, `dsp/*.h`, `dsp/units.cc`, `utils/{buffer_allocator,dsp,random}.h`, `utils/random.cc`, `LICENSE`. The eurorack repo has no LICENSE file (its README says "Code (STM32F projects): MIT license"), so `plaits/LICENSE` is the MIT notice from the source headers. Built with `TEST` defined: stmlib's host build.

Only part of libsamplerate is copied: `include/samplerate.h`, `COPYING`, and from `src/` the converter sources with the fast and medium coefficient tables. `high_qual_coeffs.h` (the "best" converter, 9 MB of source) is left out; the build doesn't define `ENABLE_SINC_BEST_CONVERTER`.

## Not yet used
miniaudio (the ALSA backend for the Comet) is vendored now so the version is fixed early. dr_wav is used from Phase 1 (ion-render); dr_wav (WAV, AIFF) and dr_flac load samples from Phase 3.
