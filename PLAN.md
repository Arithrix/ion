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

### Phase 8 · Modulation — `v0.8.0` · M (agreed with Michael 2026-11-08)
Per track: 2 LFOs, 2 envelopes, velocity / mod wheel / aftertouch / key / random, and 8 routes onto any continuous value on the track. Applied in the Module each block over the value set. Mod screen (`O`), `v` on any value, live markers on bars. See D-58.

### Phase 9 · 303-style bass — `v0.9.0` · S–M (done: D-60)
- Engine: **Open303** (Robin Schmidt; C++; the engine is MIT according to its ports JC303 and esp32_open303: **verify at source**). Proven in JC303, Zynthian and on an ESP32.
- Named "ACID" / "303-style", not "TB-303" (Roland's trademark).
- Mono; **slide** when notes overlap (the OXI's ties), **accent** from velocity over a settable threshold. Check the OXI manual for how it sends both.
- Tune, cutoff, resonance, env mod, decay, accent, saw ↔ square, slide time; a **drive** after it, probably as a general insert effect.
- The OXI is the sequencer. Modulation (Phase 8) works on it from day one.

### Phase 10 · Chip synth — `v0.10.0` · M (done: D-59, D-61)
An ion-native lo-fi wave synth in the spirit of the M8's WavSynth: pulse (12.5/25/50/75%), triangle, saw, sine, noise, and a set of our own small wavetables; low bit depth and sample rate as character controls; wave shaping ideas (size, multiply, warp, scan through a table); ion's filter and the uniform envelope; polyphonic. No chip emulator. Read the M8 manual at the start; take the idea, not its tables.

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
