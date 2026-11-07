# Vendored code

Pinned copies with their license files intact (HANDOFF §13). Sources were checked at origin on 2026-11-06; see DECISIONS.md V-01 to V-04.

| Directory | Project | Version | License | Source |
|---|---|---|---|---|
| `synth_dexed/` | Synth_Dexed | `e3f524c` (2025-09-10) | GPL-3.0; MSFA parts Apache-2.0 | codeberg.org/dcoredump/Synth_Dexed |
| `ftxui/` | FTXUI | v7.0.3 (`f921fad`) | MIT | github.com/ArthurSonzogni/FTXUI |
| `miniaudio/` | miniaudio | 0.11.26 (`877ea4a`) | Public domain / MIT-0 | github.com/mackron/miniaudio |
| `dr_libs/` | dr_wav, dr_flac | `35b5c39` (2026-10-31) | Public domain / MIT-0 | github.com/mackron/dr_libs |
| `doctest/` | doctest | v2.5.3 (`2d0a935`) | MIT | github.com/doctest/doctest |

## Local changes
None to upstream files. Two things are worked around from our side instead:
- **Synth_Dexed** includes `<arm_math.h>` and expects Teensy/Circle helpers (`millis`, `constrain`, `signed_saturate_rshift`). `src/engine/fm/dexed_port/arm_math.h` provides them.
- **FTXUI:** only `include/`, `src/` (tests removed), `cmake/` and the top-level CMake files are copied. `examples/` and `doc/` hold just their `CMakeLists.txt`, which return early when those options are off; upstream's CMake adds both directories unconditionally.

## Not yet used
miniaudio (the ALSA backend for the Comet) and dr_libs (sample loading, Phase 3) are vendored now so the versions are fixed early.
