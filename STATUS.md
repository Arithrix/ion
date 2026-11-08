# Status

**Phase:** 5 done · 0.4.0 (effects) and 0.5.0 (sampler + uniform envelope, D-50) are released and pushed. Next: Phase 6, not started.
**Branch:** `main` (Phase 6 will be `phase/6-resampling`)
**Updated:** 2026-11-07

## Done (Phase 5)
- **One envelope for every instrument** (D-50, Michael's call):
  - the same AHDSR and velocity → amp on FM, drum pads and the sampler (amp and filter);
  - FM's Attack/Release macros replaced by it, applied per voice inside Synth_Dexed, wide open by default;
  - 0.3/0.4 drum kits and projects convert exactly.
- **Sampler engine** (D-47):
  - one sample played chromatically from a root note across a key range;
  - loop off, forward (with crossfade) or ping-pong;
  - amp and filter ADSR;
  - SVF LP12/LP24/HP/BP with key tracking, envelope and velocity;
  - LFO (sine, triangle, square, S&H; free or synced) onto pitch, cutoff, pan and level;
  - glide, 1–16 voices, sustain pedal, bend;
  - Repitch mode, with Stretch reserved.
- **Shared sample handling** for Drum and Sampler (D-48). Sampler tracks load, save, go missing and collect like pads.
- **Sampler screen:**
  - waveform with loop region, start/loop markers and zoom round the point being edited;
  - a bar ruler for recordings that know their tempo (D-49), otherwise seconds;
  - Play, Amp, Filter and Mod sections with envelope and LFO shapes and the CCs mapped;
  - points snap to zero crossings; `n` normalizes;
  - `o` opens the browser at the sample's folder, and an empty sampler starts in it.
  - Four sections side by side when wide, two at 80 columns, one at a time on narrow screens.
- **Elsewhere:** Space plays the root note; the engine picker offers SMPL; Tracks shows the sample and its main settings.
- **Tests:** 132 cases.
  - Pitch by note and key range; loops; crossfade against a click; ADSR timing; filter, key tracking and velocity.
  - Synced LFO, glide, voice limit, sample hand-back, extremes.
  - Session round trip; snapshots at 130×36, 80×24 and 60×35.

## Done (Phase 4)
- **Effects** (D-42), written for ion from published designs:
  - a compressor with soft knee and gain-reduction readout;
  - a chorus;
  - a tempo-synced delay with filtered, saturated feedback, ping-pong and width;
  - a Dattorro-tank reverb as plate, hall and room (RT60 within ~7% of the setting);
  - a 1 ms lookahead safety limiter at −0.3 dBFS.
- **Inserts:** 3 slots in every scope, each with a click-free bypass. Reorder and replace from the FX screen (D-43).
- **Sends and returns:** sends A/B per track (pre or post fader), delay and reverb returns with fader, pan, mute and EQ, and a delay → reverb send. Returns are solo-safe (D-44).
- **Master:** EQ → inserts (a glue compressor, switched out by default) → fader → limiter (D-45).
- **Idle strips sleep:** idle CPU live is 1.3% with both returns running (0.3.0: 2.2%; before the fix: 6.3%). Under load, three pretend OXIs into the effects demo: 14% CPU, 0 dropouts without RT.
- **UI:**
  - FX screen: chain, picker, parameters, gain-reduction meters, the reverb's tail drawn from its settings, the synced delay time in ms.
  - Mixer: return strips, send levels, EQ for master and returns, compact at 80 columns.
  - Tracks: inserts and sends.
- **Project format 4** (D-46). Every effect parameter is CC-learnable (`track3.fx1.ratio`).
- **Tools:**
  - `ion-bench` reports each effect: reverb 13–17 µs per 128-frame buffer, the others under 4.
  - The demo adds chorus, compressors, sends, and CC 75/76 mapped to reverb size and delay feedback.
- **Tests:** 119 cases.
  - Compressor curve and timing; chorus; delay timing, sync, ping-pong, glide, stability.
  - Reverb RT60 for every type, and stability under sweeps.
  - Limiter: the ceiling holds, it's transparent below it, the gain is smooth.
  - Routing: sends pre/post, mute, returns, delay → reverb, solo-safety, sleep safety.
  - Session: save/load, keys, reorder, format-3 projects.
  - Snapshots of FX and Mixer.

## Done (Phase 3, released as 0.3.0)
- **Samples:**
  - WAV, AIFF and FLAC are decoded on a disk thread and converted to 48 kHz with libsamplerate (vendored 0.2.2, BSD-2).
  - They're cached, so a sample on several pads is decoded once.
  - Previews coalesce, so scrolling never queues a backlog (D-35).
  - Samples are pinned by the Session until the audio thread hands them back, and a replaced sample finishes its tails first (D-36).
- **Drum engine:**
  - 16 pads, each with 17 CC-mappable settings: note, tune and fine, level, pan, start, reverse, one-shot or gate, hold, decay, filter (LP/HP/BP SVF) with cutoff and resonance, drive, velocity to level and to filter, choke group (D-37).
  - Up to 64 voices, 4 per pad; chokes and steals fade over 5 ms.
  - The shared voice allocator (D-26) arrived with it.
- **Kits** in `~/ion/kits` with every pad setting. **Collect** copies samples into the project, and save-as takes them along (D-38).
- **Project format 3** (D-39). The parameter index grew to 16 bits, so every pad setting is a normal parameter.
- **UI:**
  - Drum screen: browser with preview and waveform, 4×4 pads with activity and choke groups, two-column pad editor. Narrow layout too.
  - Space and piano mode play pads (octave 1 = pads 1–16).
  - Tracks shows the selected pad.
  - Library lists kits.
  - Settings has sample roots, preview and preview level (D-40, D-41).
- **Tools:**
  - `ion-render` waits for samples.
  - `ion-bench` has drum scenes. 16 pads with every voice busy use 3.4% of a 128-frame buffer, or 9.7% with filter and drive.
  - The demo project has a synthesised kit on track 5, collected into the project.
- **Tests:** 103 cases.
  - Loader formats and rates, cache and preview coalescing.
  - Engine: mapping, tune, reverse, start, chokes without clicks, voice caps, gate and hold/decay, velocity, filter response and stability, sample release.
  - Module hand-over; session pads, projects, kits, collect, previews.
  - UI snapshots of the drum screen at 130×36, 80×24 and 60×35, with scripted key presses.
  - Clean under AddressSanitizer and UBSan, apart from Synth_Dexed's own upstream warnings.

## Waiting on Michael
- Say whether FM without its old Attack/Release macros is OK, or bring them back (D-50; revert is possible).
- Try 0.4.0 (effects: "dials in a mix") and 0.5.0 (sampler: "plays a sample chromatically"; the uniform envelope).
- Choose the reverb by ear (D-42); the 1 ms limiter latency (D-45).
- Tune by ear: velocity → level default (50%), drive amount, decay range, 4 voices per pad.
- Still open from earlier: real-time priority (`no RT` in the header; dropouts without it, see V-10), latency feel, `ion --list-midi` with the OXI, macro ranges.

## Next (Phase 6: resampling)
Clock-synced recording of a track, tracks or the master (1–16 bars, from the next downbeat, sample-accurate, D-09). WAV + sidecar (D-49). Send to the library, a sampler track (loop set, root C3), or drum pads as slices (equal, per chord change, transients) with markers to nudge. Optional mute of the sources; undo the last take.

## Known gaps carried forward
- Global default CC mappings (D-26).
- Near-duplicate FM patches aren't folded together (D-30).
- No samples index in library.db yet; the browser reads folders (D-35).
- Pads can't be copied or renamed yet; a pad's name is its file name.
- No effect presets yet (the FX mockup's "Preset"; D-06 says `o` / `w`).
- The Tracks detail pane at 80×24 cuts its last rows (inherited from 0.1; to fix with the narrow-layout polish in Phase 8).
