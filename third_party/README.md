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
| `sqlite/` | SQLite amalgamation | 3.54.1 (2026-10-16) | Public domain | sqlite.org/2026/sqlite-amalgamation-3540100.zip, SHA3-256 `92281c86…8e30` matches the download page |

## Local changes
Two upstream changes, marked `ion:` in the source:
- **Synth_Dexed `src/env.cpp`, `Env::update`.** Upstream sends every held note's envelopes to stage 3 whenever a voice parameter changes ("for now we simply reset ourselves at level 3"). Under a CC sweep that silences notes still in their attack. ion's version keeps each envelope in its stage and retargets it, and glides a standing level (sustain, or a hold) to its new value within about 0.1 s. Covered by `tests/test_fm_handover.cpp` ("macros: …"), which fails against the upstream code.

- **Synth_Dexed `src/dexed.h`, `src/dx7note.h/.cpp`, `src/dexed.cpp` (`getSamples`): the uniform amp envelope (DECISIONS D-50).** Each `Dx7Note` carries an `ion::dsp::Ahdsr` (from `src/dsp/ahdsr.h`, header-only): `init()` starts it, `keyup()` releases it, `transferState()` carries it to a legato note. `getSamples` multiplies each voice's output by it and by velocity → amp, and marks a voice not live once its envelope is done. Marked `ion:`. With the envelope's defaults (wide open) the output is unchanged; the golden FM render checks that.

Two other things are worked around from our side instead:
- **Synth_Dexed** includes `<arm_math.h>` and expects Teensy/Circle helpers (`millis`, `constrain`, `signed_saturate_rshift`). `src/engine/fm/dexed_port/arm_math.h` provides them.
- **FTXUI:** only `include/`, `src/` (tests removed), `cmake/` and the top-level CMake files are copied. `examples/` and `doc/` hold just their `CMakeLists.txt`, which return early when those options are off; upstream's CMake adds both directories unconditionally.

Only part of libsamplerate is copied: `include/samplerate.h`, `COPYING`, and from `src/` the converter sources with the fast and medium coefficient tables. `high_qual_coeffs.h` (the "best" converter, 9 MB of source) is left out; the build doesn't define `ENABLE_SINC_BEST_CONVERTER`.

## Not yet used
miniaudio (the ALSA backend for the Comet) is vendored now so the version is fixed early. dr_wav is used from Phase 1 (ion-render); dr_wav (WAV, AIFF) and dr_flac load samples from Phase 3.
