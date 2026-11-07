# Decisions

Format: **ID · status · decision**, then why. Status is `verified` (checked at the source), `proposed` (waiting for Michael), or `decided`.

## Verified on 2026-11-06

### V-01 · verified · Plaits 1.2 source is public and complete
`pichenettes/eurorack` @ `f4cfa76` (2026-10-12): `plaits/dsp/engine2/` contains `virtual_analog_vcf`, `phase_distortion`, `six_op` (×3 instances), `wave_terrain`, `string_machine`, `chiptune`. `voice.cc` registers all 24 models. The orange bank is registered first (indices 0–7), then green, then red.
- License: the README says "Code (STM32F projects): MIT license"; MIT headers are in the source files. `stmlib` (a submodule, also needed) is MIT.
- `kBlockSize = 12`, `kMaxBlockSize = 24`, `kSampleRate = 48000`.
- **Catch:** `kCorrectedSampleRate = 47872.34f`. The hardware's real rate was slightly below 48 kHz, and pitch constants derive from it. Unpatched at a true 48 kHz, Plaits plays about **+4.6 cents sharp**. We patch it to 48000 and record the patch.
- The 6-op models fall back to built-in banks (`fm_patches_table`) when no user data is loaded. Our FM-library link replaces the user data.

### V-02 · verified · Synth_Dexed
`codeberg.org/dcoredump/Synth_Dexed` @ `e3f524c` (2025-09-10). Ships `LICENSE-GPL3.txt` and `LICENSE-APACHE2.txt` (the MSFA part).
- **Catch:** `dexed.h`, `dexed.cpp` and `compressor.h` `#include <arm_math.h>` (CMSIS-DSP) unconditionally. Only about 12 functions are used (`arm_scale_f32`, `arm_sin_f32`, biquad cascade…), mostly in its built-in compressor, which we don't use. Plan: a small portable `arm_math.h` shim in our tree.
- It renders in MSFA blocks of 64 (`LG_N 6`), so we need an internal block loop.

### V-03 · verified · Other licenses (LICENSE files at upstream HEAD)
DaisySP MIT · Airwindows MIT · FTXUI MIT (latest tag v7.0.3) · miniaudio and dr_libs public domain / MIT-0 · RtMidi MIT-style · toml++ MIT · nlohmann/json MIT · doctest MIT · Dexed GPL-3.0. All are compatible with GPL-3.0.
- Still to confirm when vendoring: MSFA's own license file (the fetch from the google repo root came back empty; Synth_Dexed carries Apache-2.0 for it), and where DaisySP's LGPL `ReverbSc` lives now.

### V-04 · verified · FTXUI has no kitty keyboard protocol support
Checked `src/ftxui/component/terminal_input_parser.cpp` at v7.0.3. There's no `CSI u` / event-type parsing, so no key releases. → D-04.

### V-05 · verified · This laptop (2026-11-06)
- MX-25.3 (Debian 13 trixie), kernel 6.12 `PREEMPT_DYNAMIC`, SysVinit, X11 session, 16 threads.
- **Audio:** PipeWire 1.4.5 + pipewire-pulse + WirePlumber, `pipewire-jack` installed, no jackd running. Rate locked to 48000; quantum default 1024, min 32. Card: `sof-hda-dsp`.
- **JACK lib:** `libjack.so.0` resolves to `/usr/lib/x86_64-linux-gnu/libjack.so.0.1.0` (jackd2's), not PipeWire's (`/usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/`). JACK clients need `pw-jack` or an ld.so.conf entry.
- **Real-time:** user is in `audio`, but `ulimit -r` = 0 and memlock = 8192 KB. The only RT limits file is `25-pw-rlimits.conf` for `@pipewire`, and the user isn't in that group. rtkit isn't installed. **No RT scheduling is possible for ion right now.**
- **MIDI:** ALSA sequencer present; OXI not connected at inspection time.
- **Toolchain:** gcc/g++ 14.2, make, git 2.47, python3. **Missing:** cmake, ninja, pkg-config, libasound2-dev, clang, ccache, gh, and an aarch64 cross toolchain and qemu-user.
- **Terminals:** xfce4-terminal, truecolor. That's fine now that piano mode is gate-only (D-05).

### V-06 · verified · Test sounds downloaded (personal use, not for the repo)
`~/ion/downloads/`, with provenance in `~/ion/downloads/SOURCES.md`.
- DX7: 1,008 bulk dumps, 32,256 voices, ~12.5k unique by a crude packed-minus-name hash. Format variants found: 5232-byte files (a bulk dump plus a second sysex message), 4106-byte `.snd` files with no `F7`.
- Drums: 176 deep house one-shots, all 44.1 kHz, which confirms we need sample-rate conversion at load (D-10).
- No free UK garage pack was available without a login; Michael will bring those from Splice.
- Repo test fixtures will be generated (synthetic valid DX7 banks, synthetic WAVs), so redistribution never comes up.

## Proposed (waiting for Michael)

### D-01 · proposed · Direct ALSA sequencer instead of RtMidi
Hot-plug needs the `System:Announce` port (client/port start and exit events), and RtMidi doesn't expose it, so we'd end up polling. Direct ALSA seq is ~500 lines and also gives us event timestamps, clean virtual ports for tests, and no extra dependency. ALSA is a runtime system library either way.

### D-02 · decided 2026-11-06 · Native PipeWire output on the laptop; miniaudio for ALSA
Michael wants PipeWire, with no trouble with Bluetooth headphones or alongside other audio (YouTube). miniaudio 0.11.26 has no native PipeWire backend (only Pulse, JACK and ALSA), so ion gets a thin `pw_stream` backend of its own (~300 lines, links the system `libpipewire-0.3`). That makes ion an ordinary PipeWire client:
- it mixes with everything else; nothing gets exclusive access to the device;
- it follows the default output, so connecting or disconnecting BT headphones moves ion like any other app, and it shows up with a volume slider in pavucontrol;
- it **never opens an input stream**, so it can't flip BT headphones into the low-quality headset (call) profile;
- it **requests** 128 frames with `node.latency = 128/48000` and never **forces** the quantum (no `force-quantum`, no global settings change). When ion quits, the graph goes back to normal.
- Bluetooth adds ~150–250 ms of codec latency that no software can remove. That's fine for sound design; for playing live from the OXI, use wired output. ion shows the real output latency in the status bar.
- JACK isn't used at all, so the `libjack.so.0` problem from V-05 doesn't apply.
- miniaudio stays for raw ALSA (the Comet, if it has no PipeWire) and as a fallback (its Pulse backend).

### D-03 · superseded by D-02
(This was about reaching PipeWire through its JACK shim.)

### D-04 · decided 2026-11-06 · Use FTXUI as-is; no kitty keyboard protocol
Michael: the computer keyboard is a backup for messing about with sounds and for troubleshooting, so "let's not make a big deal about it". So there's no custom input layer: FTXUI's `ScreenInteractive` handles input and rendering normally, and nothing needs a particular terminal.
If key releases are ever wanted, the kitty-protocol work can be added later, under FTXUI.

### D-05 · decided 2026-11-06 · Piano mode is gate-only
Each note plays for a set length (default 300 ms, adjustable in Settings), and auto-repeat is ignored. It works in any terminal, including xfce4-terminal. The status line shows `♪ PIANO · oct 3 · vel 100 · gate 300 ms`. The layout, `[ ]` octave and `\` velocity are as in the handoff. Esc leaves piano mode, then use `1`–`8` to change track.

### D-06 · decided 2026-11-06 · Keymap rules (approved as proposed)
Global keys are never rebound by a screen. Screens are **uppercase** (`T I M F R L`), because lowercase `t m l r f` are already actions. Conflicts in the mockups, with the fixes I propose:
- `p` panic vs `p` Preset (FX, Plaits) → presets use `o` open / `w` write, matching Library's `w` save and Sampler's `o` open.
- `l` learn vs `l` Length (Resample) → length is an option row changed with ←→.
- `t` tag vs `t` Timbre in Plaits' `h t o` → macros are ↑↓ rows like everywhere else.
- `q` (Mixer `f g q`) vs `q` Quit → `Tab` cycles freq/gain/Q in the band table.
- Quit isn't in the handoff keymap: `q` with a confirm if unsaved, plus Ctrl+C.
### D-07 · proposed · syx import: bad checksum ≠ corrupt
Skip a bank only if its data fails range validation. A bad checksum alone imports with a flag shown in the import log.
Real data (V-06): only 1 of 1,008 banks had a bad checksum, so this is minor. Format tolerance matters more: accept a second sysex message after the bulk dump, a missing `F7`, headerless 4096-byte files, and `.snd`/`.dx7` extensions.

### D-08 · proposed · Dedupe on normalised parameters, store raw bytes
Hash the unpacked 155-byte voice minus the name (bytes 145–154), after masking unused bits and clamping out-of-range values. Otherwise junk bits in old carts defeat dedupe. Store the original packed bytes and export those, so export round-trips byte-exact.

### D-09 · proposed · Exact-length recording by construction
The record start is the audio frame that matches the bar tick, mapped through timestamps. The length is `bars × 4 × 60 / bpm × 48000` frames from the smoothed tempo, captured in-engine. The loop is the right length without depending on the stop tick's jitter.

### D-10 · proposed · Samples are converted to 48 kHz float at load (disk thread)
Use a windowed-sinc SRC (libsamplerate BSD-2 or r8brain MIT, to evaluate). Playback interpolation: cubic Hermite by default for A53 cost.

### D-11 · proposed · UI is in-process for v1; the protocol is serialisable
Headless mode doesn't start the UI. Commands and snapshots are plain structs that could go over a Unix socket later, so a UI can attach to a headless engine started as a Comet service (Phase 8) without a rewrite.

### D-12 · proposed · doctest over Catch2
Faster compiles; same features for our needs.

### D-13 · decided 2026-11-06 · Private GitHub repo; arm64 deferred
The repo goes on private GitHub. No arm64 CI or cross-builds for now: get it good on the laptop first. Keep the code portable so arm64 is a build-only change later:
- no x86 intrinsics outside guarded fallbacks;
- denormal handling for both architectures behind one function;
- nothing that assumes x86 memory ordering (use proper atomics);
- no hard-coded library paths.
