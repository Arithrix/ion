# ion — plan

Status: **draft, waiting for Michael's go-ahead.** Companion files: `DECISIONS.md` (what I verified and what I propose), `STATUS.md` (where we are).

## Shape of the code

```
ion/
  CMakeLists.txt            project(ion VERSION 0.0.1) — single source of truth for the version
  cmake/                    version header generation (git hash, dirty flag, date), toolchain-aarch64.cmake
  third_party/              pinned copies, LICENSE files intact, each listed in DECISIONS.md with commit hash
  src/
    core/                   lock-free SPSC queues, snapshot triple-buffer, param smoothing, denormal guard, time/clock
    audio/                  device backends (native PipeWire; miniaudio for ALSA), block scheduler
    midi/                   ALSA sequencer client: input, hot-plug, virtual port, clock follower
    engine/                 Engine interface, Track, voice allocator, Mixer, EQ, FX, recorder
      fm/ drum/ sampler/ plaits/
    library/                syx parse/export, dedupe, SQLite index, sample index
    project/                project.json load/save/migrate, autosave, config.toml
    ui/                     FTXUI screens, layouts, piano mode
    app/                    main: ion, ion --headless, ion-render, ion-bench
  tests/                    doctest unit tests, golden WAVs, UI snapshots, MIDI virtual-port tests
  scripts/                  release.sh, use-version.sh, cross-build.sh
```

The engine is a library (`libion_engine`) with no UI dependency. `ion`, `ion-render` and `ion-bench` all link it; the UI talks to it only through the command queue and state snapshots.

## Phases

Sizes are relative effort: S < M < L < XL.

### Phase 0 · Prototype gate — `v0.0.x` · M
1. Install build prerequisites (Michael runs the `apt` line in STATUS.md) and fix real-time limits (§ RT below).
2. `git init`, CMake skeleton, version header, `ion --version`, doctest, `scripts/release.sh` minimal (so 0.0.1 is already a real release).
3. Vendor and pin: miniaudio, Synth_Dexed (+ a small `arm_math.h` shim for x86), FTXUI, doctest, dr_libs. Record hashes and licenses.
4. Native PipeWire output at a requested 128 frames (D-02). Measure callback time and xruns over 10 minutes, with YouTube playing at the same time, and on BT headphones.
5. ALSA sequencer input: find the OXI by name, subscribe, survive unplug/replug.
6. One Synth_Dexed instance, load a voice from a `.syx`, play from the OXI. Measure CPU per voice.
7. Bare FTXUI shell with a track list, resize handling, and the 60×35 narrow layout.
8. aarch64 cross-build of everything above (local `aarch64-linux-gnu` toolchain + `qemu-user` to run tests), plus CI if/when there's a remote.
9. Checkpoint: go/no-go on the stack with numbers.

### Phase 1 · Engine skeleton — `v0.1.0` · XL (largest phase)
- Engine interface, 8 tracks, channel map (incl. layering), voice allocator (oldest released first).
- Sample-accurate MIDI scheduling inside a block; clock follower (tempo estimate + start/stop + bar position); panic.
- Mixer: fader, constant-power pan, mute, solo-in-place, peak/RMS/hold meters; channel EQ (RBJ biquads, smoothed, per-band on/off, bypass).
- Project save/load (`"format": 1`), autosave + recovery, config.toml, headless mode.
- **CC learn + CC map** (the handoff doesn't assign it to a phase; it belongs here, with mixer params as its first targets).
- UI: Tracks, Mixer, Settings, help overlay, hint bar, wide/narrow layouts, piano mode (gate only, D-05), MIDI monitor.
- `ion-render` (project + .mid → .wav) and the first golden tests. Snapshot tests at 60×35, 80×24, 130×36, 200×50.
- The FM engine from Phase 0 stays in as the test sound so Phase 1 isn't silent.

### Phase 2 · FM + library — `v0.2.0` · L
syx parser (bulk, single, headerless 4096-byte, concatenated messages), checksum handling, normalised-param dedupe, SQLite library, import log, browser with filter/favourites/tags/auto-categories, live-load on cursor, six macros, save-as (bake), export 32-voice banks, favourites list + program change, algorithm diagrams (32 box-drawing glyph maps).

### Phase 3 · Drum sampler — `v0.3.0` · L
Disk thread + sample cache (decode + resample to 48 kHz off the audio thread), file browser with preview, 16 pads with all per-pad params, SVF, choke groups, kits as JSON, collect.

### Phase 4 · Effects — `v0.4.0` · L
Uniform effect interface, compressor, chorus, synced delay, reverb shoot-out (sound first, A53 CPU second — numbers at the checkpoint), insert slots, send buses with return EQ, master EQ → glue comp → limiter, FX screen, `ion-bench` per effect.

### Phase 5 · Sampler — `v0.5.0` · M
Keymapped one-sample instrument, loop modes with zero-crossing snap and crossfade, amp/filter envelopes, SVF with key tracking, LFO (synced), glide, velocity routing, `Stretch` mode field reserved, waveform view with bar ruler.

### Phase 6 · Resampling — `v0.6.0` · L
Recorder tap at every source point, bar-quantised start, exact-length capture, WAV + sidecar, destinations, slicing (equal / chord-change / transient) with preview and nudge, mute-and-save sources, undo.

### Phase 7 · Plaits — `v0.7.0` · M
Port all 24 models (1.2 is public, confirmed), N-voice polyphony, per-model macro descriptions, 3×8 grid, FM-library link for the three 6-op models, presets.

### Comet — when the device arrives · M (mostly unknowns)
**On hold until Michael has the device** (2026-11-08). Native aarch64 build and measurements, raw ALSA tuning at 256 frames, narrow layout polish, CPU guard with voice stealing, autostart service, measured voice limits. It takes the next free version number when it starts.

### Proposed (2026-11-08, discussing with Michael; nothing started)
Order proposed: modulation first, because it's shared infrastructure and every engine after it gets it for free; then the 303 (well-defined, small); then the chip synth (needs a choice of chip first).

**Phase 8 · Modulation — `v0.8.0` · M**
- Per track: 4 modulators, each an **LFO** (sine, triangle, saw, square, S&H, smooth random; free in Hz or synced to the OXI's clock; restart free / on note / on bar; uni- or bipolar) or an **envelope** (the uniform AHDSR, D-50, fired by the track's notes: on every note, or only on the first of a legato phrase).
- Plus fixed sources: velocity, mod wheel (cc 1), aftertouch, key (note number), random per note.
- A **mod matrix** of 8 routes per track: source → any value on that track (engine, mixer, inserts, sends), depth ±100% in the value's own 0…1 range. Switches and choice values can't be targets, except Plaits' model, stepped.
- Applied in the Module once per 64-frame block, on top of the value you set. The base value is what's saved, undone, CC-moved and shown; bars get a live marker for where modulation has it.
- Per-track, not per-voice: one envelope moves every held note together. That's right for mono (the 303) and for sweeps; per-voice envelopes are already in the engines that need them (amp everywhere, the sampler's filter). Per-voice routing can follow engine by engine.
- UI: assign from the value itself (focus it, press a key, as `l` learns a CC), and a Mod page per track for the modulators and routes. Routes are part of the project (undoable, D-54).
- Cost: negligible next to any engine.

**Phase 9 · 303-style bass — `v0.9.0` · S–M**
- Engine: **Open303** (Robin Schmidt; C++; the engine is MIT according to its ports JC303 and esp32_open303: **verify at source**). Proven in JC303, Zynthian and on an ESP32, so it should be fine on the Comet.
- Named "ACID" or "303-style" in the UI, not "TB-303" (Roland's trademark).
- Mono, with 303 behaviour from the OXI: **slide** when notes overlap (the OXI's ties/glide), **accent** from velocity above a threshold (settable). To check against the OXI manual: how it sends accent and slide.
- Controls: tune, cutoff, resonance, env mod, decay, accent, waveform (saw ↔ square), slide time, plus a **drive** after it (303s live through distortion). The drive could be a general insert effect, useful on every track.
- The OXI is the sequencer: no pattern sequencer in ion.

**Phase 10 · Chip synth — `v0.10.0` · M**
Pick one sound first:
- **C64 SID** via reSIDfp (GPL-2+, fits ion's GPL-3): the most characterful (its filter, ring mod, sync, PWM). 3 voices a chip, so 2–3 chips for polyphony; ion writes the note → register driver. Its cost needs measuring for the Comet.
- **AY-3-8910 / YM2149** (Spectrum, Atari ST) via ayumi (MIT, tiny): squares, noise, the envelope "buzzer" bass.
- **NES / Game Boy** (Blargg's APU emulators, LGPL): two pulses, triangle, noise.
- **ion's own chip-style voice**: pulse at 12.5/25/50%, 4-bit triangle, LFSR noise, crush, fast arpeggios, pitch sweeps. Polyphonic and cheap, but not an emulation.
Plaits' Chiptune model already covers NES-style chords and arpeggios, which is an argument for SID.

## Where the hard parts are

1. **Chord-change slicing (Phase 6).** Spectral flux is weak on pads: chord changes on sustained sounds have soft onsets. I plan to use chroma-vector novelty, snapped to the beat grid we already know from the clock. Expect tuning by ear with Michael.
2. **FM patch accuracy and live macros (Phase 2).** The DX7 engine itself is proven. The risks are in how we use it: changing a patch or macro while notes are held, macro sweeps over 0–99 integer DX7 params stepping audibly, and choosing the engine mode (Modern / Mark I).
3. **Real-time discipline across threads (Phase 1 on).** Snapshots, queues, sample-buffer handover and reclaiming freed buffers without allocating on the audio thread. Not novel, but bugs here are intermittent and nasty, so the engine gets a debug-build "RT sanitiser" (malloc hooks that abort if called from the audio thread).
4. **Comet performance (when the device arrives).** Unknown hardware variant, and the A53 is slow for Plaits' string/modal models and reverb. Mitigation: `ion-bench` from Phase 0, qemu for correctness only, and a Raspberry Pi as a stand-in if Michael has one.
5. **Reverb that sounds good and runs on an A53 (Phase 4).**

Lower risk than it looks: the Plaits port (portable C++, MIT, 1.2 is public), the syx format, the mixer and EQ.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| No RT priority on this laptop today (rtprio 0, memlock 8 MB, no rtkit) | Michael applies the limits change in STATUS.md before the Phase 0 latency tests |
| Bluetooth headphones add ~150–250 ms of latency | Fine for sound design; use wired output when playing the OXI. ion shows the real latency |
| No git remote yet, so no CI | Local cross-build + qemu now; GitHub Actions arm64 runner once there's a remote |
| Phase 1 is very large | Internal milestones with `-dev.N` builds if Michael wants to try things mid-phase |
