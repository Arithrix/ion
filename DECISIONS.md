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

### V-10 · verified 2026-11-06 · Dropouts without real-time priority
128-frame buffer, fake OXI playing notes and 124 BPM clock, audio thread **not** RT (Michael not yet logged in again):
- idle machine, 3 min: **0 dropouts**, worst callback 11% of the period;
- while compiling on all 16 cores, 2 min: **438 dropouts**, worst callback 261% of the period.
The DSP is cheap (2% average); the thread is simply being preempted. Real-time priority is the fix. Michael must confirm after logging in again: `no RT` gone from the header, and 0 dropouts under the same load. If dropouts remain with RT, the fallback is `--latency 256`.

## Decided during Phase 0

### D-14 · decided · FM engine mode: Mark I
Synth_Dexed offers MSFA ("Modern"), Mark I and OPL. Mark I models the original DX7's quirks most closely, and patch accuracy is the stated priority. Confirm by ear against Dexed or hardware at the checkpoint.

### D-15 · decided · Fixed 64-frame engine blocks with a FIFO to the device
Engines always render 64 frames, which is Synth_Dexed's native block. `Module::process` takes any callback size; when it's a multiple of 64 (as at 128), no latency is added. MIDI is applied at block starts in Phase 0 (1.3 ms grid); Phase 1 schedules events at sample offsets.

### D-16 · decided · Release binaries link libstdc++ statically
`-static-libstdc++ -static-libgcc` in Release, so a build runs on the Comet's distro without matching compiler runtimes. glibc, PipeWire and ALSA stay dynamic as system libraries.

### D-17 · decided · Version: `ION_PRERELEASE` next to `project(VERSION)`
CMake's `VERSION` can't hold `-dev.N`, so `CMakeLists.txt` has a second line, `set(ION_PRERELEASE "")`. Both live in the one file, and `scripts/release.sh` edits both. A build counts as dirty if anything is uncommitted, untracked files included.

## Decided during Phase 1

### D-18 · decided · Engines are pure DSP; saving goes through the Session
The handoff puts `serialize`/`deserialize` on the engine interface. The audio thread owns each engine, so no other thread may read one, and serializing from the UI thread would be a data race. So the interface has `setParam` plus a static parameter table per engine type, and `Session` keeps the savable state (engine type, voice data, parameter values) and writes it. Adding an engine still means one class plus its table.

### D-19 · decided · Live MIDI timing: "tight" by default
Each event is timestamped (CLOCK_MONOTONIC) on arrival and placed one buffer later at its exact offset: +2.7 ms constant at 128 frames, no jitter. "Immediate" (block start, up to 2.7 ms jitter, no added delay) is in Settings. Engines get blocks split at event frames, so they're sample-exact. **FM note-ons stay on Synth_Dexed's 64-frame grid** (1.3 ms); that's inherent to MSFA's block size.

### D-20 · decided · Tempo: least-squares slope over the last 48 ticks
A one-pole filter on tick intervals didn't work: interval jitter is twice the timestamp jitter (about ±10% at ±1 ms), so it can't tell jitter from a tempo change. A regression over two beats of timestamps gives under 0.3 BPM of wobble with ±1 ms jitter, and fully follows a tempo change two beats later (tested).

### D-21 · decided · Session keeps the intended parameter values
Edits update `Session::values_` at once and go to the audio thread as commands. CC moves made on the audio thread are pulled from the ParamStore each tick. "Dirty" means the values differ from the last save, plus a flag for non-parameter edits. This means repeated key presses never read a value the audio thread hasn't applied yet, and edits work even with no audio device running.

### D-22 · decided · Settings apply at once and save themselves
No "write" key: `w` is "save project" everywhere (D-06). Audio buffer changes apply on restart and say so.

### D-23 · decided · New projects, autosave, recovery
- A fresh start creates "sketch YYYY-MM-DD" (numbered if taken), not saved until `w`.
- Autosave every 2 minutes (configurable) and on every exit with changes. Writing an autosave also records the project as the last one, so an unsaved sketch comes back next start with a recovery offer.
- `q` with unsaved changes asks first; "quit anyway" keeps the autosave. Headless never recovers by itself; it prints a notice.

### D-24 · decided · Piano repeat window 75 ms
X repeats at 20 Hz after 500 ms. Events for the same key within 75 ms are repeats, which still lets fast deliberate taps through. The first repeat at ~500 ms looks exactly like a second tap to a terminal, so holding a key retriggers once. That's accepted under D-05.

### D-25 · decided · The demo project is built at release time from ROM1A
Yamaha's factory voices stay out of the repo. `make-demo` builds "demo vX.Y.Z" from `~/ion/downloads/dx7/x/sysex/rom1a.syx`, installs it as a normal project, and copies it into the release folder.

### D-26 · decided · Voice stealing and global CC defaults
- FM uses Synth_Dexed's own allocator, which reuses released voices first and then the oldest held one (it has a minor bookkeeping bug when every voice is held). The shared allocator (`engine/voice_alloc.h`: free, then oldest released, then oldest held) arrived with the drum engine in Phase 3 and will serve the Sampler.
- Global default CC mappings (handoff §5, "optional") are deferred. `Session::addMapping` is the hook they'll use.

## Decided during Phase 2

### D-27 · decided · Patch changes never cut a note: three Synth_Dexed instances per FM track
Synth_Dexed's voice load stops every voice (`panic()`), so live browsing would chop whatever the OXI is holding. Each FM track keeps three instances:
- **active** gets new notes;
- **tail** finishes notes held when the patch changed (note-offs, the sustain pedal, bend and mod wheel still reach it);
- **spare** is silent, ready for the next patch.

A tail still sounding at the next change fades out over 10 ms. Only instances with sounding notes are rendered, plus 100 ms for Synth_Dexed's DC blocker to settle, so idle ones cost memory and no CPU. Mono mode releases notes on older instances when a new note starts.
Tested: held notes survive a change; fast scrolling with notes held never steps by more than the signal's own slope; the same note retriggered across a change isn't left stuck.

### D-28 · decided · Synth_Dexed `Env::update` patched (the only upstream change)
Upstream sends every held note's envelopes to stage 3 whenever any voice parameter changes. A macro moved under a held pad cut its attack short, and under a CC sweep the note fell silent (measured). ion's version keeps the stage and retargets it. A standing level (sustain, or a hold between equal levels) glides to its new value at rate 75 or faster, about 0.1 s, so the macros respond at once even on slow patches. A running hold keeps its timer, so a sweep can't freeze a note in place. Recorded in `third_party/README.md`; two tests fail against the upstream code.

### D-29 · decided · Macros: ranges and what they touch
Integer offsets in DX7 units, clamped to valid ranges, so every result is a valid voice and "save as" can bake it:
- **Bright** ±50 on the output level of modulators. Modulators at level 0 are left alone: bringing in an unused operator would change the patch rather than brighten it.
- ~~**Attack** ±50 on R1, and **Release** ±50 on R4~~: removed in 0.5.0 in favour of the uniform amp envelope (D-50).
- **Feedback** ±7.
- **Detune** ±7. + spreads the operators apart in alternating directions, outer operators furthest; − pulls them towards centre (−7 = all centred).
- **Velocity** ±7 on the carriers' key velocity sensitivity.

Macros are track parameters: CC-mappable, saved with the project, and they stay where they are when you browse to another patch. That makes it possible to audition the library "with +10 bright". `u` resets them; save-as bakes them into a new library voice and zeroes them.
To tune by ear with Michael: the ranges, and whether Bright should scale rather than offset.

### D-30 · decided · Library: SQLite, de-duplicated on normalised parameters
- `~/.local/share/ion/library.db`, SQLite 3.54.1 vendored (public domain), WAL, one connection per thread. Import runs on a worker with its own connection; the UI keeps browsing.
- Identity is a 64-bit FNV-1a over the unpacked voice minus the name, after `unpack()` masks don't-care bits and clamps ranges (supersedes D-08's "store raw bytes"). Voices are stored normalised and exported spec-clean, so an export loads anywhere. Junk bits in old carts are dropped; they never affected the sound.
- The first-seen name wins. Files are imported in sorted path order, so this is repeatable. Every source bank is kept ("from cart_17 · +3 more").
- Your 1,040 downloaded files: 33,248 voices, 12,876 unique, imported in 0.25 s. Re-importing adds nothing.
- Categories are keyword rules on the name (first match wins). About 38% of the collection stays "other". Tags are the real tool.
- Near-duplicates (the same name, a few parameters different) are kept as separate patches. E.PIANO 1 has six variants across the collection. Exact de-duplication is what the handoff asks for; folding them would hide genuine edits.

### D-31 · decided · Corrupt banks are skipped (supersedes D-07)
The handoff says "log and skip", and only one bank in the collection has a bad checksum, so ion skips it rather than importing it with a flag. A headerless 4096-byte file is accepted only if at least 24 of its 32 names are printable text, otherwise random data would import as noise. Problems go to `~/.local/share/ion/import.log`.

### D-32 · decided · Program change: a per-project list, loaded on the audio thread
Up to 128 voices, stored in the project with their data, so the list survives on a machine whose library lacks them. `+` in the browser adds or removes a patch. Program N on a channel loads entry N onto every FM track listening on that channel (layered tracks change together). The table lives on the audio thread (swapped whole, freed on the control thread), so a program change before a note takes effect before that note. Settings can turn it off.

### D-33 · decided · Project format 2
Format 2 adds the macros (as FM parameters) and the program list. Both are optional with defaults, so format 1 files open unchanged (tested). Saving writes format 2. ion 0.1.0 refuses format 2 with "saved by a newer ion" rather than silently dropping the new fields. After a rollback to 0.1.0, projects saved by 0.2.0 won't open there.

### D-34 · decided · `--syx` feeds the library
`ion --syx FILE` imports the file into the library (idempotent) and, on an empty project, puts voice N (`--voice`) on track 1, as before. `ion --import PATH` imports and exits with a summary, for scripting.

## Decided during Phase 3

### D-35 · decided · Samples: decoded and converted to 48 kHz on a disk thread (supersedes D-10)
- WAV (8/16/24/32-bit, float), AIFF and FLAC, through dr_wav and dr_flac (already vendored). The first two channels are kept; longer than 5 minutes is refused, not truncated.
- Anything not at 48 kHz goes through **libsamplerate 0.2.2** (BSD-2), medium sinc converter: 97 dB SNR, flat to 90% of Nyquist. It's offline, so quality costs load time, not CPU. Measured on the laptop with your deep house pack: 3–8 ms for a one-shot, about 17 ms per second of stereo 44.1 kHz audio. Expect roughly ten times that on an A53; fine for loading, and on the Comet the fast converter is the fallback for previews. Only the fast and medium tables are vendored; the "best" table is 9 MB of source.
- Playback is 4-point Hermite interpolation, so tuned pads cost about 1.5 µs per voice per 128-frame buffer on the laptop (`ion-bench`).
- Loads run on one disk thread with a cache keyed by path, modification time and size, so a sample on several pads is decoded once. Previews coalesce: only the newest waiting preview is kept, so holding ↓ never builds a backlog.
- Not done: the samples index in library.db (HANDOFF §9). The browser reads folders directly. An index arrives when there's something to search across folders (Phase 5/6).

### D-36 · decided · Who owns a sample, and when it's freed
The audio thread gets a plain pointer. The Session keeps a reference ("pin") for every pointer it sends, and drops it when the audio thread hands that pointer back (`SampleReleased`). A replaced pad sample is handed back only once no voice is still playing it, so a long tail finishes with its old sample. A retired drum engine's samples are unpinned when its retirement notice arrives. Nothing is ever freed on the audio thread, and a sample is never freed while it plays (tests cover both, and ASan is clean).

### D-37 · decided · Drum pads: parameters and behaviour
17 parameters per pad, every one a normal engine parameter: CC-learnable, saved, in the CC map as "Track 4 · DRUM Pad 5 decay". That needed the parameter index to grow from 8 to 16 bits (288 engine parameters a track).
- **Tune** ±24 st plus **Fine** ±50 cents. Both apply to notes already sounding, so a pitch CC bends a tail.
- **Level** −∞…+6 dB and **Pan** (the mixer's laws). **Velocity → level** 0–100%: gain = 1 − a + a·v², default 50% (a soft hit at velocity 64 is −4 dB). **Velocity → filter** 0–100%: soft hits up to 4 octaves darker.
- **Start** 0–1000 ms. A moved start fades in over 1 ms so it doesn't click. **Reverse** plays from the end.
- **Mode** (the envelope itself is now the uniform AHDSR, D-50; hold and decay below are its Hold and Decay):
  - **one-shot** ignores note-off. **Hold** is how long the sample plays at full level before **Decay** (−60 dB in that time) begins; hold "full" (the default) plays the whole sample.
  - **gate** holds while the key is down and decays from note-off; Hold is ignored and shows "gate".
- **Filter** off / LP / HP / BP, a topology-preserving SVF (Simper); **Resonance** 0–100% gives Q 0.5…16. Band-pass is unity at its centre, so switching type doesn't drop the level. **Drive** 0–100% blends in a soft clipper (tanh-like) with make-up gain.
- **Choke** none / A–D: a hit stops every voice in its group, **its own pad included**, with a 5 ms fade. A pad alone in a group is therefore monophonic. That's the way to make any pad mono, without a separate "voices" setting.
- **Voices:** 64 per kit, at most 4 per pad. A fifth hit fades the pad's oldest voice over 5 ms. The pool is shared through the new allocator (D-26: free, then oldest released, then oldest).
- **Note** per pad, default C1–D#2 (36–51). Two pads on one note both play.
- Pads are numbered left to right, top to bottom, 1–16, as in the mockup.

### D-38 · decided · Kits, collect, and where sample paths point
- **Kits** are `~/ion/kits/<name>.json`: for each pad, its sample (an absolute path, written `~/…` when under home) and all 17 settings. Loading a kit replaces all 16 pads; pads the kit leaves out are cleared, so a kit sounds the same wherever it's loaded. Saving under an existing name replaces it (the prompt says so).
- **Projects** store each pad's path as it was chosen. **Collect** (`g`) copies every sample from outside the project into `<project>/samples/` and repoints the pads at `samples/<file>`. Same name, different file → `kick 2.wav`; the same file twice is copied once. After collecting, the project can be moved or backed up whole. **Save as** copies the project's `samples/` along with it.
- A missing sample keeps its path; its pad shows "missing" in red and is silent. Loading the project again after the file is restored brings it back.

### D-39 · decided · Project format 3
Format 3 adds drum tracks (pads and kit name; pad settings are ordinary engine parameters). Formats 1 and 2 open unchanged. ion 0.2.x refuses format 3 with "saved by a newer ion" rather than losing the drum tracks. Loading sends only the parameters that differ from the defaults, so eight full kits fit through the command queue even with no audio thread running (ion-render).

### D-40 · decided · Sample preview goes straight to the master
Moving onto a file in the browser plays it once into the master (after its EQ and inserts, since 0.4.0: D-45), before the master fader and the safety ceiling, at −6 dB (Settings → Samples). It bypasses the track, so its fader and EQ don't colour it. A new preview fades the previous one out over 5 ms. Moving off a file, Esc and panic stop it. "Preview on move" can be turned off; the waveform still loads.

### D-41 · decided · Sample roots
The browser's top level lists the roots from the config (`[samples] roots`, default `~/ion/samples`), then the kits folder, then home, so any folder can be reached. Roots are added and removed in Settings → Samples. Up from a root returns to the top level.

## Decided during Phase 4

### D-42 · decided · The effects are written for ion, from published designs
The handoff prefers proven open-source DSP. What I found when looking:
- DaisySP's compressor has no knee, and its good reverb (ReverbSc) is in the LGPL part.
- Airwindows effects are whole plugins with fixed internal choices and their own parameter conventions.
- Clouds' reverb (Mutable, MIT) is the same Dattorro family as below.

So ion has its own, each a short, well-known design with tests:
- **Compressor:** feed-forward and stereo-linked. Peak detector, soft knee and log-domain attack/release, after Giannoulis, Massberg & Reiss (JAES 2012). Threshold, ratio, attack, release, knee, makeup, mix (for parallel compression). It reports its gain reduction.
- **Chorus:** two delay taps of 7 ms ± up to 4 ms, sine LFO, the right side up to half a cycle behind (Width), Hermite reads.
- **Delay:** sync to the clock (1/32 to 1/1, dotted and triplet), or 1 ms–5 s. Low and high cut, plus soft saturation in the feedback, so 100% feedback rings without exploding. Ping-pong and width. A time or tempo change glides over about 50 ms instead of jumping.
- **Reverb:** Dattorro's plate tank (JAES 1997) in three characters:
  - Plate: the paper's figure;
  - Hall: tank 1.5× larger and more diffusion;
  - Room: tank half size, with early reflections.

  Size, decay (RT60 within about 7% of the setting, measured), pre-delay, damping, width, modulation, low cut, high cut. A big hall can't decay faster than its tank allows: set to 0.5 s, it rings about 1.2 s.
- **Limiter:** the master's safety limiter (D-45).

Costs on the laptop, per 128-frame buffer: compressor 2.7 µs, chorus 3.3, delay 2.7, reverb 13–17, limiter 0.8. The reverb is about 0.6% of the budget, so roughly 8% on an A53.
**To choose by ear with Michael** ("sound first"): the reverb. If it doesn't please, the next candidates are an FDN hall, or Airwindows' reverbs wrapped behind the same interface.

### D-43 · decided · Insert slots
Every scope (tracks, master, both returns) has 3 slots. Each slot holds one effect plus its own **on** switch, which can be learned like any parameter.
- Switching in or out crossfades over one block (1.3 ms). Switched out, it costs nothing, and it's cleared when it comes back, so no stale tail plays.
- A return's first slot is its own delay or reverb and can't be changed or bypassed.
- Reordering (`[ ]`) moves the effect with its settings.
- Parameter numbering: mixer 0–63, inserts from 64 (20 per slot: on, then up to 19 settings), engine from 128. Keys in files and the CC map read `track3.fx1.ratio`, `master.fx1.on`, `reverb.fx1.decay`.

### D-44 · decided · Sends and returns
- **Sends:** each track has Send A (delay) and Send B (reverb), −∞ (the default) to +6 dB. One switch per track makes both pre-fader (after the inserts, before fader and pan); the default is post-fader.
- **Mute and solo:** muting a track silences its sends. Solo on another track silences it and its sends, but never the returns (they're solo-safe).
- **Returns:** each has fader, pan, mute and EQ. The delay return's Send B feeds the reverb after its fader, so echoes can sit in the room.
- **Tempo:** synced times follow the OXI's clock, or 120 BPM without one. The FX screen says which.
- **Idle strips sleep:** a strip whose input is silent, and whose output has stayed below −140 dB for longer than its effects can hold sound (a delay's time, a reverb's pre-delay plus one trip round its tank), skips its EQ and inserts until sound arrives.
  - Live idle CPU went from 6.3% to 1.3% with both returns running.
  - A test checks that a 3 s echo still arrives after 3 s of silence.

### D-45 · decided · Master chain and the limiter's latency
- **Chain:** EQ → 3 inserts → fader → safety limiter.
- **Glue compressor:** every project has one in master slot 1 (−12 dB, 2:1, 10/200 ms), switched out. Projects therefore sound as they did until it's turned on.
- **Limiter:** −0.3 dBFS ceiling with a 1 ms lookahead.
  - The lookahead means it never clips: the gain ramps down before the peak arrives.
  - The cost is 1 ms of latency on everything you hear. 1 ms rather than the usual 1.5–5 ms because playing feel matters here (Michael's latency question is still open).
  - It's always on. Its reduction shows on the FX screen.
- **Sample preview** (D-40) now enters after the master inserts, before the fader and the limiter.

### D-46 · decided · Project format 4
Format 4 adds:
- per-track inserts;
- the sends (mixer parameters);
- the master's whole strip (EQ too) and its inserts;
- the two returns.

Older formats open with no inserts, sends off and the glue compressor out, and keep their master fader. 0.3.x refuses format 4 with "saved by a newer ion".

## Decided during Phase 5

### D-47 · decided · The sampler
- **What it is:** one sample per track, played chromatically from a **root** note (default C3, so a loop recorded "at pitch" plays back as recorded) across a key range. Mode is **Repitch**. The mode field exists in the parameters and in project files with Stretch reserved (HANDOFF §6.2); Stretch isn't selectable yet, and the screen says "stretch: later".
- **Points:** start, loop start and loop end are fractions of the sample's length, so they survive collecting or a re-render at another tempo. On screen they're seconds.
  - ← → moves a point by one column of the waveform at the current zoom (`z`: ×1, ×4, ×16, ×64 round the point being edited); Shift moves ten.
  - With **Snap** on (the default), a point lands on the nearest rising zero crossing within that column.
  - The loop is kept at least 16 frames long and the right way round.
- **Loop:** off, forward or ping-pong. Forward can crossfade (0–500 ms) the end of the loop into what comes before its start, so a loop that isn't cut at a zero crossing doesn't click (tested).
- **Envelopes:** amp and filter ADSR. Attack is linear; decay and release fall exponentially, reaching −60 dB in the time set. Sustain follows the knob smoothly while held.
- **Filter:** SVF LP12, LP24 (two stages), HP, BP. Its cutoff is moved in octaves:
  - by the filter envelope (±5 octaves at ±100%);
  - by key tracking (100% = one octave per octave);
  - by velocity (soft notes up to 4 octaves darker);
  - by the LFO (±4 octaves).
- **LFO:** one per track, not per voice: sine, triangle, square or sample & hold. Free (0.05–20 Hz) or synced (4 bars … 1/32, dotted and triplet). It goes to pitch (up to ±1200 cents), cutoff, pan and level.
- **Glide:** from the last note played, in poly too. With Voices = 1 the sampler is mono.
- **Voices:** up to 16, default 8; the oldest goes, released ones first, faded over 5 ms. The sustain pedal holds notes. Bend range defaults to ±2 semitones.
- **Sections:** Tab moves between them, as Tab moves between panes everywhere else. The mockup's ←→ for sections would clash with ← → changing values.

### D-48 · decided · Engines that play samples share one mechanism
`SampleRetirement` holds samples taken off a pad or zone until no voice plays them (D-36). The Engine interface has `setSample`, `popReleased`, `heldSamples` and `setTempo`, so the Module and Session treat Drum and Sampler alike. A sampler's sample is kept as pad 1 in the track's state, so loading, missing-file handling and collect work the same. Sampler tracks bring **project format 5**: 0.4.x would otherwise open one as an empty track and lose it on save; instead it refuses with "saved by a newer ion".

### D-50 · decided with Michael · One envelope for every instrument
- **The envelope:** every instrument's amp envelope, and the sampler's filter envelope, is the same AHDSR (`src/dsp/ahdsr.h`):
  - attack 0–10 s, rising in a straight line;
  - hold 0–10 s, or "full": until note-off, or to the end of the sample;
  - decay 1 ms–20 s, falling exponentially to the sustain level (−60 dB of the way there in the time set);
  - sustain 0–100%;
  - release 1 ms–20 s, falling the same way, or "off": no fade, so the sound ends on its own.
- **Velocity → amp** (gain = 1 − a + a·v²) sits with it everywhere.
- **Same everywhere:** the keys (`amp.attack` … `vel.amp`; drum pads `pad5.amp.decay`), ranges, curves, labels and the drawing of its shape. A test checks they match across engines. Only the defaults differ:
  - **FM:** wide open (no attack, sustain 100%, release off, no velocity), so a patch sounds as stored until the envelope is used.
  - **Drums:** no attack, hold full, decay 500 ms, sustain 0. One-shot ignores note-off, so Release shows greyed; gate releases at note-off.
  - **Sampler:** 2 ms attack, sustain 100%, 200 ms release.
- **FM:** the envelope runs per voice inside Synth_Dexed, after the patch (a second local change, in `third_party/README.md`). It can shape a patch and shorten it. A release shorter than the patch's ends the voice and frees it. A steady envelope is applied a block at a time, so FM costs the same as before (`ion-bench`).
- **FM's Attack and Release macros are gone** (Michael's choice: simplest for now, revert if it plays badly). They could lengthen a patch, which the outer envelope can't. Bright, Feedback, Detune and Velocity stay.
- **Older files convert exactly** (format < 5, kit version 1):
  - pad `vel.level` → `vel.amp`;
  - `hold`/`decay` → `amp.hold`/`amp.decay`, with hold "full" moving from 5000 to 10000;
  - a gate pad's decay becomes its release, with hold full;
  - CC mappings follow the rename.
  - FM's old Attack/Release values and their CC mappings are dropped.
- **Plaits (Phase 7)** gets the same envelope after its own low-pass gate, open by default.

### D-49 · decided · Recording sidecars
A recording `name.wav` carries `name.json`: `{"ion_recording": 1, "tempo": 124.0, "bars": 4, "beats_per_bar": 4, "sources": "tracks 3 + 6", "recorded_at": "…"}`. The Rec screen will write them (Phase 6). The sampler already reads them: it shows bars, tempo and sources, and draws a bar ruler.

## Decided during Phase 6

### D-51 · decided · The recorder: where it listens, when it starts, how long it is
A take is a buffer the control thread allocates (zeroed, so the audio thread never faults pages in) and hands to the Module, like a sample. The audio thread writes it and publishes how far it has got; the UI draws the live waveform from what's below that line. When the take ends the Module hands it back (`TakeDone`) and only then does the Session touch it. If the Session goes away mid-take the buffer leaks rather than being freed under the audio thread.
- **Taps.** Master: after the limiter, read `Limiter::kLookahead` (48) frames late, so a take of the master lines up with the bar exactly as a take of tracks does. Tracks, post-fx: after EQ and inserts, with fader and pan, but mute and solo ignored (you can record a track you've muted or soloed out). Tracks, pre-fx: the engine's dry output. Sends aren't in a track take; record the master for those. The browser's preview is held back while the master records. Audio input slots into `RecSource` later.
- **Start.** Synced: on the downbeat tick (a tick landing on a multiple of 96 while the transport runs), at that tick's frame. A note the OXI sends with that tick lands at frame 0 of the take. Armed with the transport stopped, it waits. "Now": from the next 64-frame block.
- **Length (refines D-09).** A synced take records every tick's frame and runs ~2048 frames past its last downbeat. The Session then fits a least-squares line through all the ticks and trims to `bars × 96 × frames-per-tick`. That's the tempo measured over the whole take, not the smoothed tempo at the start, so a jittery tick at either end can't make the loop the wrong length. "Now" with bars counts them at the current tempo (the clock's, else 120). "Until stopped" runs until `r` (synced: until the next downbeat after `r`), at most 3 minutes.
- **Stopping early.** `r` during a synced take makes the bar under way the last. The transport stopping, or the clock going away, keeps the whole bars recorded (none: nothing is kept).
- **Room.** Synced takes get room for their bars at 30 BPM. Below that a take runs out of room and keeps what fits. 16 bars is 49 MB.

### D-52 · decided · Slicing
All three are offline on the control thread, after the take (well under 100 ms for 16 bars).
- **Equal:** 4, 8 or 16 (the handoff's 8 or 16, plus 4 for a 4-bar take).
- **Chord changes:** 12-bin chroma from 4096-point frames (100 Hz–5 kHz), each frame normalised, near-silent frames dropped. With a tempo the candidates are the beats (the take starts on a downbeat). Each beat's novelty is 1 − cosine similarity between the mean chroma of the beat before and the beat after. A change counts if it clears 4× the 30th-percentile novelty (the in-chord noise floor), 10% of the biggest, and 0.06. Without a tempo the candidates are every 21 ms with ½ s each side, and peaks ¼ s apart win. Tested: C → Am → F → G struck on every beat lands exactly on the bars (Am → F shares two notes and scores only 0.15, which is why the threshold is relative to the floor and not the peak); one chord restruck gives one slice; a change on beat 3 is found there. Real pads with chorus are the open question; tune by ear with Michael.
- **Transients:** log-magnitude spectral flux (1024/256), peaks over 1.5× the local median plus 10% of the biggest, at least 40 ms apart. Each is moved back to where its rise begins (20% of the way from the floor to the peak, in 32-sample chunks) and then to the zero crossing before it. Tested within 2 ms on noise bursts over silence and over tails.
- At most as many slices as pads from the first pad; the strongest win.

### D-53 · decided · Where a take goes, and undo
- **Files:** `recordings/<what>_<tempo>bpm_<bars>bar.wav` in the project (48 kHz float, stereo) plus the D-49 sidecar. `<what>` is the sound's name for one track (the FM patch, the kit, the sample), `tracks-3-6`, or `mix`. The sidecar adds `frames`, `start_bar`, `tap`, `sent_to`, `slices`, and `settings`: each source track as it is saved in project.json (every track, for the master), so a bounced track can be rebuilt. Save-as copies `recordings/` with the project.
- **Sampler:** the track becomes a sampler if it isn't one (other settings stay if it is), and plays the take from C3, looped forward over the whole take, no crossfade. Repitch.
- **Drum pads:** the take waits for review. ↑↓ picks a slice, ← → nudges it 1 ms (Shift 10 ms), `a` splits, `x` merges, Space plays it, ⏎ commits. Each slice is written as `recordings/<take>/slice NN.wav`, with a 0.5 ms fade-in (except the first) and 2 ms fade-out, so a cut mid-waveform doesn't click. On the pads each slice starts from the pad defaults (the note stays), one-shot, in the first choke group no other loaded pad on the track uses. Esc keeps the recording and changes no pads.
- **Mute sources** (on by default) mutes the recorded tracks, except the destination track itself. That way bouncing track 3 into a sampler on track 3 frees the track instead of silencing it.
- **Undo** (`u`, the last take only, this session): the tracks the take changed come back from snapshots taken just before (the destination as project JSON, the sources' mute), and its files and slices are deleted. *Superseded in 0.7.0 by D-54: a take is undone like any edit, and its recording stays.*
- **Project format 6:** the Rec setup is saved with the project (`"rec"`). Format 5 files read as they are, with the default setup. Editing the setup doesn't mark the project unsaved.

## Decided during Phase 7

### D-54 · decided (Michael asked for it) · Undo and redo for the whole project
- **What's a step:** anything that changes the project: values (faders, macros, pad settings, EQ, sends, insert settings), engines, patches, samples, kits, inserts, the CC map, the program list, the Plaits patch list and presets, collect, and what a take changed (its mutes; a sampler destination; slices put on pads). **Not steps:** CC moves and program changes from the OXI (playing, not editing), selection, screens, the Rec setup, and settings in config.toml.
- **How:** Session keeps the document outside values as plain data (tracks, inserts, CC map, program list) and a copy of every value. After each key or click the UI calls `commitEdit()`, which compares and records what changed; `tick()` does too, so headless edits and takes are steps. A step stores the document before and after (only when more than values changed) and each changed value. Undo restores the document part by part, touching only what differs (a pad whose sample didn't change isn't reloaded; a patch list that didn't change isn't resent), then sets back the values that step changed. Values it didn't change keep their current value, so a CC move made since isn't undone.
- **Merging:** presses on one value within 1.5 s are one step (holding ← → or dragging a fader is one undo). Switches (mute, solo, toggles) never merge.
- **Depth:** 256 steps. Loading a project, starting a new one or recovering an autosave clears the history; saving doesn't.
- **Keys:** Ctrl+Z undo, Ctrl+Y redo, everywhere (also in piano mode, where letters are notes). Ctrl+Z no longer suspends ion (FTXUI's default). `↶ undo ↷ redo` in the header for the mouse, dimmed when there's nothing to undo or redo. The status line says what was undone ("undid Track 3 · Fader").
- **Takes:** the Rec screen's `u` (D-53) is gone; a take is undone like anything else. Undoing a take no longer deletes its recording: it stays in `recordings/` (redo needs it, and a recording is never lost by undo). Its sidecar still says where it was sent.

### D-55 · decided (Michael asked for it) · Mouse, as in Midnight Commander
- **Additive.** Every action stays on the keyboard (HANDOFF §8 still holds). The mouse is another way to press the same keys, so each screen keeps one code path.
- **How:** while a screen draws, it marks targets (`ctx.item`, `ctx.value`, `ctx.button`); FTXUI's `reflect` records where each one landed. On a mouse event the app finds the smallest target under the pointer in the last frame and acts:
  - **click:** focus it (the row, the pane, the pad, the model); a button presses its key;
  - **double-click** (400 ms): ⏎ (open, load, play);
  - **wheel:** over a list, ↑ ↓ in that list; over a value, → ← on it;
  - **drag:** on a value, one ← → step per cell; on a mixer strip, up and down move its fader.
- **Clickable everywhere:** the header's screen tabs and ↶ ↷; every hint in the hint bar that names one key; track rows (and drag for the fader); mixer strips, their M and S, the EQ table and its bypass; the FX chain, parameters and picker; FM library rows (a click loads, like moving there); sample browser rows; drum pads (a click selects and plays); every parameter row; the Rec setup, its track buttons, and the take's waveform under review (a click picks and plays the slice under it); Library lists; Settings fields; the engine picker; Plaits models and presets. A click outside the help, the MIDI monitor or the engine picker closes it.
- **Terminal text selection** needs Shift+drag while ion has the mouse (xfce4-terminal and most others).

### D-56 · decided · Plaits: the engine
- **Source:** Plaits 1.2 from `pichenettes/eurorack` @ `f4cfa76`, with the stmlib it uses, MIT (V-01). One local change: the sample rate correction (V-01). Upstream's `voice.cc` is replaced by ion's own voice (`src/engine/plaits/plaits_voice.*`, adapted from it, MIT notice kept), because upstream's reads the 6-op banks from STM32 flash and outputs 16-bit. What ion's voice changes: float output; no 1 ms trigger delay (a MIDI note's pitch and gate arrive together); the trigger is the note-on, so restriking a held voice always fires; three LPG modes (below).
- **Polyphony:** 1–8 voices (default 6), each a full Plaits voice with every model (16 KB of scratch RAM each). Voices are taken as the other engines take them (D-26); the same note restrikes its own voice. 1 voice is mono, retriggering.
- **Model order** in the UI and in files: the mockup's grid, green, red, orange, 8 each (0–23). Upstream registers orange first; `PlaitsEngine::plaitsIndex` maps. Files store the model as a number in this order; the track's "name" is the model's name.
- **The LPG** has three modes:
  - **gate** (default): it opens to the note's velocity while the key is held and closes after; LPG decay is how slowly. Plays like a keyboard synth: held notes sustain.
  - **ping:** each note strikes it and it dies away by itself, held or not. This is Plaits with only TRIG patched: the classic plucked sound.
  - **off:** the model straight out. The amp envelope shapes it; with release "off", a note-off fades it in 5 ms so a drone never hangs.
  The models that make their own envelope (kick, snare, hi-hat, string, modal, 6-op FM, and speech when it's saying words) bypass the LPG as they do on the module.
- **Velocity:** sets the note's accent (the drums and strings strike harder), and in gate mode how far the LPG opens (Vel→LPG, default 70%). Vel→amp (the uniform envelope's, D-50) is there too, default 0.
- **The internal decay envelope** (fired by every note) reaches pitch, timbre and morph through Env→pitch / Env→timbre / Env→morph (−100…+100%, default 0): Plaits' three attenuverters with nothing patched into FM, TIMBRE and MORPH. LPG decay sets its length, as on the module.
- **After the LPG:** the uniform amp envelope (D-50), wide open by default (attack 0, hold full, sustain 100%, release off). Out ↔ aux blends each model's two outputs. Glide (from the last note played) and bend range (default 2 semitones). Each voice is −6 dB, so six voices of a chord have headroom.
- **A voice is freed** once its key is up and it's been below −80 dB for 50 ms (or its envelope has finished).
- **Rendering:** Plaits renders 12 samples at a time; each voice keeps its own 12-sample buffer, so a note lands within 0.25 ms of where MIDI put it.
- **Cost** (`ion-bench`, this laptop): 6 voices take 0.7–6% of a 128-frame buffer. Speech and chiptune are cheapest, then most models at 2–3.5%; 2-op FM is the dearest (161 µs, 6%) because it oversamples. Plaits voices only render while they sound.
- **Tuning:** A4 plays at 440.00 Hz (tested).

### D-57 · decided · Plaits: the 6-op patch list, presets, the screen
- **6-op FM a** plays the track's own list of up to 32 patches from the FM library (HANDOFF §6.4); `f` on the Plaits screen fills it from your FM favourites (library order, the first 32), `x` clears it. With an empty list it plays Plaits' own first bank. **6-op FM b and c** play Plaits' own second and third banks. The list is spread over Plaits' 32 slots, so Harmonics sweeps the whole list however long it is. The screen shows the patch Harmonics is on. The list is saved in the project (unpacked voices, hex) and in presets.
- **Presets:** `~/ion/presets/plaits/<name>.json`: every Plaits setting and the patch list. `a` saves (the same name replaces), `o` opens the list, ⏎ loads. A setting the preset leaves out goes to its default. Loading a preset is one undo step. (D-06 wanted `w` for write, but `w` saves the project everywhere; `a` matches FM's "save as".)
- **The screen** (Instrument, on a PLTS track): the 3 × 8 model grid by bank (↑ ↓ ← →; moving selects, as browsing FM patches does), the model's description and what each macro does on it; the three macros with what they do (and, on chord models, the chord; on 6-op models, the patch); then the voice, the decay envelope's amounts and the amp envelope. Below 110 columns it's one pane at a time (Tab swaps).
- **Project format 7** adds Plaits tracks. Format 6 files read as they are.

## Decided during Phase 8

### D-58 · decided with Michael · Modulation: per track, assignable to any value
- **Per track, not per voice** (Michael's call, 2026-11-08). One envelope moves every held note together; that's right for mono, sweeps and the coming 303. Per-voice envelopes stay inside the engines that have them (amp everywhere, the sampler's filter).
- **Sources, on each track:**
  - **2 LFOs:** sine, triangle, saw up, saw down, square, sample & hold, smooth random. Free (0.01–50 Hz) or synced to the OXI (4 bars … 1/32, triplets, dotted). Restart free, on every note, or on every bar; a start phase. A synced free-running LFO is lined up with the bar at each downbeat, so it stays locked however long it runs. Without a clock, synced LFOs follow 120 BPM.
  - **2 envelopes:** the uniform AHDSR (D-50), fired by the track's notes, on every note or only the first of a phrase (legato), released when the last note comes up.
  - **Velocity, mod wheel (cc 1), aftertouch, key, random-per-note.**
  - This is 2 LFOs + 2 envelopes rather than PLAN's "4 modulators, each either": simpler to show and to learn, and nothing is lost in practice.
- **8 routes per track:** source → target → depth (±100% of the target's own range). Routes onto one target add up. Targets: any continuous value on the same track (mixer, EQ, sends, insert settings, engine settings). Not: switches, choices (a stepping model or filter type would be more surprise than use), another track's values, or modulation's own settings.
- **Where it runs:** in the Module, once per 64-frame block (1.3 ms), after commands and before the engines render. A note-on updates its track's modulation before the engine hears the note, so an envelope's attack is already on the targets for the note's first sample. The ParamStore keeps the value you set (what's saved, undone, CC-moved and shown); the DSP gets value set + modulation. When a route stops, its target goes back to the value set. A CC or edit on a modulated value takes effect at once, modulated from its new setting.
- **Model in files and undo:** LFO, envelope and route settings are ordinary values (`track3.lfo1.rate`, `track3.env2.decay`, `track3.mod4.depth`), so they're saved, undone (D-54) and CC-learnable like anything else (route depth on a knob is the useful one). Route targets are part of the track: saved as keys within the track (`"target": "plaits.timbre"`), undone as part of the document.
- **Keeping routes valid:** a new engine on a track lets go of routes onto the old engine's values; replacing an insert lets go of routes onto it; moving an insert takes its routes along. All of it is one undo step with the change that caused it.
- **UI:** the Mod screen (`O`, between Instr and Mixer in the header): the two LFOs and two envelopes with their shapes and live output; the routes with source, target, depth and where the target is now. ⏎ picks a target from a list of the track's values (type to filter); `[ ]` changes the source; ← → the depth; `x` clears. **`v` on any value, on any screen**, adds a route from LFO 1 at 25% on the first free route. Value bars (Tracks, FX, drum pads, Plaits macros) show a lilac marker where modulation has the value now.
- **Project format 8.** Format 7 files read as they are, with nothing routed.
- **Cost:** 2.1 µs per 128-frame buffer for all 64 routes with every source running (`ion-bench`): 0.08%.

### D-59 · decided with Michael · The chip synth will be ion's own, in the spirit of the M8's WavSynth
Michael wants "something like what Dirtywave uses on the M8". As far as I can tell from the M8 manuals, that's the WavSynth: Dirtywave's own deliberately 8-bit wavetable synth, not an emulation of a real chip. So Phase 10 is an ion-native lo-fi wave synth (simple waves and our own wavetables, low bit depth and rate, ion's filter, envelopes and modulation), not reSIDfp, ayumi or an APU emulator. No vendored library; polyphonic and cheap. Read the M8 manual closely at the start of that phase; take the idea, not its tables.

## Decided during Phase 9

### D-60 · decided · ACID: a 303-style bass on Open303, and a Drive insert
- **Engine:** Open303 (Robin Schmidt), MIT, **verified at source** (`License.txt` in github.com/RobinSchmidt/Open303 @ `313bf0d`). Proven in JC303, Zynthian and on an ESP32. Four local changes (third_party/README.md): an allocation-free note list (upstream's `std::list` allocated on the audio thread), a settable accent threshold, the slide-back tuning bug, two includes.
- **Name:** ACID ("303-style bass"), not TB-303 (Roland's trademark). Engine colour: a pale yellow.
- **Playing from the OXI:** mono, last-note priority. **Overlapping notes slide** (the 303 convention; the OXI's own glide is CV-only, so a slide is a note held into the next: tie or note length over 100%). **Velocity ≥ "Accent at" (default 100) is an accent:** a harder filter and amp hit. Both conventions confirmed against the TD-3's MIDI behaviour; the OXI manual doesn't say how it sends accents, hence the settable threshold.
- **Controls:** the 303's front panel in its order (waveform saw ↔ square, cutoff 200 Hz–5 kHz, resonance, env mod, decay 200–2000 ms, accent), then slide time (10–500 ms), accent threshold, tune ±24, fine ±50 ct, bend range, level. After the voice, the uniform amp envelope (D-50), wide open by default, as on FM and Plaits. Open303's square-wave shaping settings aren't exposed: changing them rebuilds wavetables with an FFT, too heavy for the audio thread.
- **Level:** Open303's accents add a lot (≈ +9 dB on an accented note with no resonance). The engine sits 16 dB down so a typical squelchy line peaks around −15 dBFS and accents around −7; the extreme case can still reach the master limiter.
- **Cost:** one voice, 4× oversampled inside, ≈ 15 µs per 128-frame buffer (0.6%). Open303 never idles by itself; ion stops calling it after 200 ms of silence with no key down, so a resting ACID track costs nothing.
- **Drive** (a new insert effect, for any track; a 303 lives through distortion): soft (tanh), hard (clip) or fold, 0–48 dB, bias (asymmetry, with a DC blocker), tone (a low-pass after it), level and mix. 2× oversampled with a 47-tap half-band, done polyphase (24 multiplies each way), plus first-order antiderivative anti-aliasing: a hard-clipped 4.7 kHz sine's aliases are 40 dB under its harmonics. ≈ 17 µs per buffer.
- **Project format 9.** Nothing to convert, but 0.8 would load an ACID track as empty and a Drive slot as empty, and lose them on save; the bump makes it refuse the file instead ("saved by a newer ion").

## Decided during Phase 10

### D-61 · decided · CHIP: ion's lo-fi wave synth
- **Idea** (D-59): the M8's WavSynth, as its manual describes it: "a wave table synthesizer and noise generator… an 8-bit sample buffer that is generated in realtime from one of 9 basic shapes that can be repeated, skewed/biased, mirrored, filtered and overdriven." ion's is its own code, shapes and choices; nothing of the M8's is copied.
- **The wave:** a table of 4–256 steps rebuilt from a basic shape (pulse 12/25/50/75%, saw, triangle, sine) whenever a setting that shapes it changes, at most once a block, shared by every voice. Then: **mult** repeats it (1–16, a hard-sync sound), **warp** pushes it to one side (a power curve on the phase), **mirror** runs it back on itself past a point, **amp** (0–24 dB) drives it into a **limiter** (clip, sine, fold, wrap), and **bits** (1–8) quantises it. It's read **without interpolation**: it steps and aliases the way chip sound does; that's the character, not a bug.
- **Noise:** two 15-bit shift registers, as the NES's: **noise tone** (the short 93-step loop, clocked 93× a cycle, so it's pitched at the note) and **noise** (the long one, clocked 8× a cycle: higher notes are brighter). Both go through amp, limiter and bits like the table.
- **After the wave:** a per-voice filter (off, low-pass, high-pass, band-pass, notch; a new shared `dsp::Svf`, Simper's TPT form, stable under modulation), the uniform amp envelope (D-50; release 40 ms by default because the oscillator never ends; with release "off" a note-off fades it in 5 ms), velocity → amp (50% by default), and a track-wide **rate** (1–48 kHz, a sample-and-hold) for crunch.
- **Playing:** 1–8 voices (6), glide, tune ±24 (continuous, so an envelope on it makes chip pitch sweeps), fine, bend range, level. Each voice −9 dB: full-scale squares are loud.
- **Cost:** six voices with the filter and a reduced rate, 4.2 µs per 128-frame buffer (0.16%); 5.9 µs with the table rebuilt every block (as when modulation moves warp).
- **Screen:** the wave drawn from the table, the eight wave settings, the amp envelope beside the filter and rate (as FM and Plaits place it), then playing.
- **Project format 10** (0.9 would load a CHIP track as empty).

## Decided during the 0.10.1 pass (Michael's overnight list: efficiency, then test everything)

### D-62 · decided · Efficiency
Measured first, changed second, measured again; anything that didn't help or changed the sound was dropped.
- **Compiler (every target, vendored code included):** `-O3 -fno-math-errno -fno-trapping-math`, and on x86-64 `-march=x86-64-v3` (AVX2/FMA, CPUs from 2013 on; this laptop's i7-1270P has it). Five builds compared pinned to one core (ion-bench): -O3 alone gained little and LTO nothing; the AVX2/FMA generation is the win. Reverb ~45% faster, 16 drum pads with filter and drive ~30%, the idle mixer ~35%, Plaits ~12%, delay/chorus ~20%. The dev build uses the same flags, so the tests test what ships. A full demo render differs from 0.10.0 by −98.5 dB (fused multiply-add rounding): inaudible. `ion` refuses to start on a CPU without AVX2/FMA, with a message; `-DION_PORTABLE=ON` builds for any x86-64. The Comet (ARM) will get its own flags.
- **The UI:** a frame costs 0.1–0.4 ms to build, but the app redrew and rewrote the whole terminal 30 times a second regardless. Now it redraws at 30 fps only while something on screen moves (sound, meters falling, the clock, a take, modulation markers on the selected track, a message, notes waiting for their gate) and at 4 fps otherwise; keys and clicks always redraw at once. FTXUI asked the terminal for every mouse movement (mode 1003) and redrew on each; it now asks only for movement with a button held (1002, enough for dragging). Idle UI thread: 7–11% → 2% of a core (the terminal app's own CPU drops with it).
- **Drive:** its oversampling done polyphase (the half-band's zero taps skipped, the antiderivative cached): 62 → 17 µs per buffer, the same output.
- **Looked at and left:** the mixer (0.25% of the period when idle on the demo), memory (25 MB resident with the demo; no engine is large), Plaits' heaviest models (upstream DSP; the flags already help), the audio thread's live load in the header (4–5%, mostly PipeWire's own work and an efficiency core's clock).
- `ion-bench` now also times each screen's frame and the cost of modulation.

### D-63 · decided · The test sweep, and what it found
- **New tests:**
  - randomised: hundreds of edits while playing, then undo all and redo all (must match exactly); random projects through save and load; every engine and effect under random settings (finite, bounded, voices freed); random MIDI including clock and transport with a take running; random keys, clicks, wheel and drags on every screen at three sizes. Fixed seeds; `ION_SWEEP_DEEP=N` runs N more (used for long hunts: 150 extra seeds under the sanitizers);
  - a real-time guard: operator new/delete counted while `Module::process` runs, with everything busy (zero allowed);
  - app flows driven with keys (engine picker, piano, save as, Library load, mute/solo, effects, a take, Settings);
  - outside the suite: the real app through a pty at three sizes with PipeWire audio (every screen, piano on every track, overlays, undo, mouse, save, quit), a 90-second playing soak (memory flat, 0 dropouts), every command-line mode.
- **Bugs found and fixed** (each with a regression test):
  - **Synth_Dexed:** a heap overflow when stealing a voice with all busy, plus the wrong voice stamped as newest; `monoMode` read uninitialised; a negative shift from an LFO delay byte over 99; built with `-fwrapv` (its accumulators wrap by design).
  - **Plaits:** the wavetable model read past its wave map at the top of the quantised banks (a crash); the speech model read past its phoneme table twice (a struck consonant; a word's position surviving a move back to the vowels); the 6-op models read past their velocity table on every full-velocity note, and could start on another model's leftovers in the RAM all models share (NaN; ion now clears it on a model switch); output now clips at full scale as the module's own does. stmlib's `InterpolateWrap` left a negative position negative and read before its table (the wave terrain model's sine); it now wraps into 0…1.
  - **Open303:** an array zeroed past its end.
  - **ion:** undo/redo of a step that changes what an effect slot holds or which engine a track runs could bring back a stale value (a moved, switched-off effect came back on); a removed master glue compressor came back after save and load; CHIP and Plaits glide ran away when the glide time was shortened mid-glide; the sampler's LP24 resonated in both stages (+49 dB at full resonance; now the second stage stops at Butterworth, unchanged below ~30%).
- **Clean:** 214 test cases, all passing in the normal and Release builds and under AddressSanitizer + UBSan with no findings, vendored code included.

## Decided for 1.0 (Michael, 2026-11-12: call it 1.0 and publish it)

### D-64 · decided · 1.0.0, public on GitHub, GPL-3.0
- **Version 1.0.0** is 0.10.1's code with a license and a README; the project format stays 10.
- **License: GPL-3.0** (`LICENSE`, the FSF text, checked against Debian's `common-licenses/GPL-3`). Synth_Dexed is GPL-3.0, so ion as a whole must be; everything else vendored is compatible (third_party/README.md). Supersedes the "private" part of D-13.
- **README.md** for the GitHub front page: what ion is, engines and features, requirements, building, getting started, keys, file locations, the third-party credits, and a note that ion isn't affiliated with the companies whose products it names.
- **Release tarballs** now carry `LICENSE`, `README.md` and `licenses/` (every vendored license file), since the binary includes that code.
- The working documents (HANDOFF, PLAN, STATUS, DECISIONS, TESTING) stay in the repo as they are.
- **History rewritten once, before going public (Michael's choice, 2026-11-12):** every commit and tag had Michael's personal email, so all branches and tags were rewritten to his GitHub noreply address (`16479827+Arithrix@users.noreply.github.com`) and force-pushed. The file contents are identical (same trees) but every commit ID changed. A bundle of the old history is at `~/projects/ion-before-email-rewrite-2026-11-12.bundle`. The repo-local git email is now the noreply address. This is the only exception to never rewriting main.
- **The GitHub Release's tarball** (`ion-1.0.0-linux-x86_64.tar.gz`) is built from the tag, without `demo/` (it holds Yamaha's ROM1A patches), the debug file or the test tools. `release.sh`'s local tarball still has them and stays local.

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
### D-07 · superseded by D-31 · syx import: bad checksum ≠ corrupt
Skip a bank only if its data fails range validation. A bad checksum alone imports with a flag shown in the import log.
Real data (V-06): only 1 of 1,008 banks had a bad checksum, so this is minor. Format tolerance matters more: accept a second sysex message after the bulk dump, a missing `F7`, headerless 4096-byte files, and `.snd`/`.dx7` extensions.

### D-08 · superseded by D-30 · Dedupe on normalised parameters, store raw bytes
Hash the unpacked 155-byte voice minus the name (bytes 145–154), after masking unused bits and clamping out-of-range values. Otherwise junk bits in old carts defeat dedupe. Store the original packed bytes and export those, so export round-trips byte-exact.

### D-09 · implemented as D-51 · Exact-length recording by construction
The record start is the audio frame that matches the bar tick, mapped through timestamps. The length is `bars × 4 × 60 / bpm × 48000` frames from the smoothed tempo, captured in-engine. The loop is the right length without depending on the stop tick's jitter.

### D-10 · superseded by D-35 · Samples are converted to 48 kHz float at load (disk thread)
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
