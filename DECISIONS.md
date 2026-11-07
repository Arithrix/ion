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
- **Attack** ±50 on R1, and **Release** ±50 on R4, of every operator. + means slower or longer. DX7 rates are already logarithmic in time, so an offset scales time evenly.
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
- **Mode:**
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

### D-09 · proposed · Exact-length recording by construction
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
