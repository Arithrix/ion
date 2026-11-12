# ion

**A keyboard-driven, multitimbral sound module that runs in a Linux terminal.**

ion is an eight-track instrument built for a hardware sequencer. Plug in a USB MIDI sequencer (it was built around the [OXI One MK2](https://oxiinstruments.com/), but any class-compliant MIDI device works), give each track its own MIDI channel, and play. ion has no sequencer of its own: your hardware is the sequencer and ion is the sound.

```
 ion  Tracks Instr Mod Mixer Fx Rec Library  ↶ ↷                       no clock   cpu 0%   no audio
╭ Track 3 · CHIP · lo-fi wave synth ───────────────────────────────────────────────────────────────╮
│██████                                                                                            │
│██████                                                                                            │
│██████                                                                                            │
│██████                                                                                            │
│wave · 32 steps · 4 bits                                                                          │
│                                                                                                  │
│▸ Shape      pulse 25%                             Mirror     ░░░░░░░░░░░░ off                    │
│  Size       █▍░░░░░░░░░░ 32                       Amp        ░░░░░░░░░░░░ 0.0 dB                 │
│  Mult       ░░░░░░░░░░░░ 1                        Limit      clip                                │
│  Warp       ███████▌░░░░ 25%                      Bits       █████▏░░░░░░ 4                      │
│                                                                                                  │
│amp ████████████████████████████████▇▃▁▁         filter · rate                                    │
│  Attack     0                                     Filter     off                                 │
│  Hold       full                                  Cutoff     ██████████░░ 6.00 kHz               │
│  Decay      300 ms                                Resonance  ██▍░░░░░░░░░ 20%                    │
│  Sustain    60%                                   Rate       █████████▋░░ 22.05 kHz              │
│  Release    40.0 ms                                                                              │
│  Vel→amp    50%                                                                                  │
╰──────────────────────────────────────────────────────────────────────────────────────────────────╯
 ↑↓  Setting   ←→  Change   l  Learn   v  Modulate   ⏎  Play   Esc  Back   ?  Help
```

## What's in it

**Six engines.** Any track can hold any of them.

| Engine | What it is |
|---|---|
| **FM** | 6-operator FM that loads DX7 patches (`.syx` banks), on [Synth_Dexed](https://codeberg.org/dcoredump/Synth_Dexed), with a simplified front panel and an algorithm diagram |
| **Plaits** | A polyphonic port of the Mutable Instruments Plaits macro-oscillator: all 24 models, presets, and its 6-op models play patches from your FM library |
| **Drum** | A 16-pad drum sampler with per-pad tuning, envelopes, filters and choke groups |
| **Sampler** | A keymapped single-sample instrument with loop points, envelope and filter |
| **ACID** | A 303-style bass on [Open303](https://github.com/RobinSchmidt/Open303). Ties slide and high velocities accent, so it plays well from a step sequencer |
| **CHIP** | ion's own lo-fi wave synth: a small wavetable you shape while it plays, plus bit depth, sample-rate reduction and NES-style noise |

**Around the engines:**
- **Mixer:** faders, pan, mute/solo and meters, a six-band EQ on every channel, two send buses and a master bus with a glue compressor.
- **Effects:** compressor, chorus, delay, reverb and Drive (oversampled soft clipping, hard clipping and wavefolding), as inserts or on the sends.
- **Modulation:** each track has two LFOs (free or tempo-synced) and two envelopes. Velocity, mod wheel, aftertouch, key and random also work as sources. Up to eight routes per track can reach any continuous setting.
- **Resampling:** record a track or the whole mix, then load the take into the sampler or slice it onto drum pads.
- **One flat library:** every FM patch, sample, kit and recording in one browsable, searchable list, with favourites and no banks.
- **Playing it:** MIDI learn on every setting (`l`), MIDI clock and start/stop, and program change. Without a controller you can play from the computer keyboard (`` ` ``).
- **Working with it:** undo and redo of every edit, mouse support (click, drag, wheel), autosave, and projects saved as readable JSON.
- **Offline tools:** `ion-render` renders a MIDI file through a project to WAV; `ion-bench` measures what each engine costs.

ion is built for real-time use. The audio thread never allocates or locks, and the test suite checks that. A six-voice CHIP track costs about 4 µs per 128-sample buffer on a laptop CPU.

## Requirements

- **Linux** with **PipeWire** for audio (native, not through JACK or Pulse) and **ALSA** for MIDI. ion is developed on MX Linux (Debian 13).
- **An x86-64 CPU from 2013 or later** (AVX2/FMA). ion checks at start-up and says so on an older CPU. Build with `-DION_PORTABLE=ON` for any 64-bit x86 CPU.
- **A terminal** with truecolor and mouse support, at least 80 × 24. 110 columns or more shows more at once.
- For real-time audio, your user should have real-time priority and memlock limits (usually from membership in the `audio` group).

## Building

You need CMake 3.25 or newer, Ninja, a C++20 compiler (developed with GCC 14), pkg-config, and the PipeWire and ALSA development headers. Every other dependency is vendored in `third_party/`.

On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libpipewire-0.3-dev libasound2-dev
```

Then:

```sh
git clone https://github.com/Arithrix/ion.git
cd ion
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build        # optional: over 200 test cases
./build/ion
```

## Getting started

```sh
ion                              # open the last project (or a new one)
ion --project "my song"          # open or create ~/ion/projects/my song
ion --syx rom1a.syx              # add a DX7 bank to the library and play it on track 1
ion --import ~/dx7-banks/        # add every .syx under a folder to the FM library
ion --list-midi                  # list MIDI inputs
ion --midi OXI                   # connect inputs whose name contains "OXI"
```

ion doesn't ship any patches or samples. Bring your own DX7 banks (many are freely available) and WAV, AIFF or FLAC samples. Set sample folders on the Settings screen (`,`).

**Keys.** Press `?` on any screen for help. The essentials:

| Key | Does |
|---|---|
| `T` `I` `O` `M` `F` `R` `L` | Tracks · Instrument · Modulation · Mixer · FX · Record · Library |
| `1`–`8` | select a track |
| arrows | move, and change the value (Shift for coarse steps) |
| `⏎` / `Esc` | edit or open / back |
| `Space` | audition the selected track or pad |
| `l` / `v` | MIDI-learn / modulate the focused value |
| `` ` `` | play from the computer keyboard |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo |
| `w` | save |
| `p` | panic: all notes off |
| `q` | quit |

**Where things live:**

| Path | What |
|---|---|
| `~/ion/projects/` | projects (one folder each, JSON plus audio) |
| `~/ion/kits/`, `~/ion/presets/`, `~/ion/exports/` | drum kits, presets, rendered audio |
| `~/.config/ion/config.toml` | settings (also editable on the Settings screen) |
| `~/.local/share/ion/` | the library index |

## Project documents

ion was planned and built in phases. The working documents are in the repository:

- [`CHANGELOG.md`](CHANGELOG.md): what changed in each version, written for the person playing it.
- [`DECISIONS.md`](DECISIONS.md): every design decision and the reason for it, including how each vendored library was checked at its source.
- [`HANDOFF.md`](HANDOFF.md): the original design brief. [`PLAN.md`](PLAN.md) and [`STATUS.md`](STATUS.md): the build plan and where it stands.
- [`third_party/README.md`](third_party/README.md): vendored code, versions, licenses and every local change.
- [`mockups/`](mockups/): the screen designs ion was built from.

## Status

1.0 is the first public release. ion is stable and in daily use with an OXI One MK2. Known gaps are listed in [`STATUS.md`](STATUS.md). The main ones are no audio input recording yet, no effect presets, and no sample index in the library (sample browsing reads folders directly).

Planned: support for the [Mecha Comet](https://mecha.so/) handheld (aarch64), time-stretching, multi-sample instruments, and more engines.

## License

ion is free software, licensed under the **GNU General Public License v3.0**. See [`LICENSE`](LICENSE).

It includes the work of others, each under its own license. Their license files are kept in `third_party/`:

| Project | Used for | License |
|---|---|---|
| [Synth_Dexed](https://codeberg.org/dcoredump/Synth_Dexed) (from Dexed and Google's MSFA) | FM engine | GPL-3.0 (MSFA parts Apache-2.0) |
| [Plaits](https://github.com/pichenettes/eurorack) and stmlib by Émilie Gillet / Mutable Instruments | Plaits engine | MIT |
| [Open303](https://github.com/RobinSchmidt/Open303) by Robin Schmidt | ACID engine | MIT |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | terminal interface | MIT |
| [libsamplerate](https://github.com/libsndfile/libsamplerate) | sample-rate conversion | BSD-2-Clause |
| [dr_libs](https://github.com/mackron/dr_libs), [miniaudio](https://github.com/mackron/miniaudio) | WAV/FLAC/AIFF files, audio | Public domain / MIT-0 |
| [SQLite](https://sqlite.org/) | library index | Public domain |
| [nlohmann/json](https://github.com/nlohmann/json), [toml++](https://github.com/marzer/tomlplusplus), [doctest](https://github.com/doctest/doctest) | projects, settings, tests | MIT |

ion is an independent project. It isn't affiliated with or endorsed by OXI Instruments, Mutable Instruments, Yamaha, Roland or Dirtywave. Product names are used only to describe compatibility and inspiration.
