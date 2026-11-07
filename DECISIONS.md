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

### V-07 · verified 2026-11-06 · Synth_Dexed quirks found while integrating
1. **Pitch-mod sensitivity decoded with 2 bits.** `Dexed::decodeVoice` masks byte 116 with `0x30`; the DX7 field is bits 4–6. Dexed's own `Cartridge::unpackProgram` reads 3 bits. Patches with PMS 4–7 would get the wrong vibrato depth. → We never use `decodeVoice`; `syx::unpack` matches Dexed, and a test covers PMS = 7.
2. **Post-load refresh breaks new notes.** `loadVoiceParameters()` flags a refresh. The next render calls `Env::update()` on live notes, and that jumps the envelope to stage 3 (upstream comment: "for now we simply reset ourselves at level 3"). Any note starting in the same block as a load lost its attack and decay. Most of ROM1A (pianos, clav, marimba…) went silent. → `FmEngine::loadVoice` clears the flag; the load has already stopped every voice. There's a regression test.
   - **Phase 2 consequence:** the same `Env::update` path runs whenever we refresh held notes, so live macro moves would snap held notes to their sustain stage. Macros will apply to new notes, or we patch `Env::update`. Decide in Phase 2 with listening tests.
3. `loadVoiceParameters()` calls `panic()`: held notes stop dead on patch change. Phase 2 (live browsing) needs a handover, e.g. two Dexed instances per FM track that crossfade.
4. `getSamples()` must be called with a multiple of 64 frames, and voices are only retired when `getNumNotesPlaying()` is called. `FmEngine` does both every block.
5. `getSamples()` is protected (it's meant to be subclassed, as MiniDexed does). → a small `DexedCore` subclass.
6. Synth_Dexed's own voice stealing uses `millis()`. → driven from the sample counter, so offline renders are deterministic.
7. Dexed's `PluginFx` adds a DC blocker after the voices, so after panic there's a −66 dB tail that decays in about 30 ms. Harmless; the tests allow for it.

### V-08 · verified 2026-11-06 · PipeWire behaviour, measured
Headless ion, PipeWire 1.4.5:
- The node appears as "ion sound module" (role Music) and linked itself to the default sink, which was the BT headphones (`bluez_output…`).
- The graph ran at our requested 128 frames with 0 errors in `pw-top`. The global `clock.quantum` setting stayed at 1024 (requested, not forced).
- Idle DSP load 0.4%. The data thread was `SCHED_OTHER` because Michael hadn't logged in again since joining the `pipewire` group; ion shows `no RT` until he does.

### V-09 · verified 2026-11-06 · FM cost on the laptop
`ion-bench`, Release, i7-1270P. E.PIANO 1 (six operators, algorithm 5), notes retriggered every 250 ms: **4–6 µs per voice per 128-frame buffer**, which is about 0.2% of the 2.67 ms period per voice. Eight FM tracks × 8 voices ≈ 12% of one core.
INIT VOICE (one operator) costs about 1 µs per voice. Expect the Comet's A53 to be several times slower; measure in Phase 8.

## Decided during Phase 0

### D-14 · decided · FM engine mode: Mark I
Synth_Dexed offers MSFA ("Modern"), Mark I and OPL. Mark I models the original DX7's quirks most closely, and patch accuracy is the stated priority. Confirm by ear against Dexed or hardware at the checkpoint.

### D-15 · decided · Fixed 64-frame engine blocks with a FIFO to the device
Engines always render 64 frames, which is Synth_Dexed's native block. `Module::process` takes any callback size; when it's a multiple of 64 (as at 128), no latency is added. MIDI is applied at block starts in Phase 0 (1.3 ms grid); Phase 1 schedules events at sample offsets.

### D-16 · decided · Release binaries link libstdc++ statically
`-static-libstdc++ -static-libgcc` in Release, so a build runs on the Comet's distro without matching compiler runtimes. glibc, PipeWire and ALSA stay dynamic as system libraries.

### D-17 · decided · Version: `ION_PRERELEASE` next to `project(VERSION)`
CMake's `VERSION` can't hold `-dev.N`, so `CMakeLists.txt` has a second line, `set(ION_PRERELEASE "")`. Both live in the one file, and `scripts/release.sh` edits both. A build counts as dirty if anything is uncommitted, untracked files included.

## Proposed (waiting for Michael)

### D-01 · implemented in Phase 0 · Direct ALSA sequencer instead of RtMidi
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

### D-12 · implemented in Phase 0 · doctest over Catch2
Faster compiles; same features for our needs.

### D-13 · decided 2026-11-06 · Private GitHub repo; arm64 deferred
The repo goes on private GitHub. No arm64 CI or cross-builds for now: get it good on the laptop first. Keep the code portable so arm64 is a build-only change later:
- no x86 intrinsics outside guarded fallbacks;
- denormal handling for both architectures behind one function;
- nothing that assumes x86 memory ordering (use proper atomics);
- no hard-coded library paths.
