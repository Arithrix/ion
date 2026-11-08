# Status

**Phase:** 4 · Effects. Built and tested on `phase/4-effects`; not released yet, so 0.3.0 stays installed for Michael's test tonight.
**Branch:** `phase/4-effects`
**Updated:** 2026-11-07

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
- Try 0.3.0 tonight (`~/ion/releases/v0.3.0/TESTING.md`): build a garage kit and play it from the OXI. Then 0.4.0 gets released for the Phase 4 checkpoint ("dials in a mix").
- Choose the reverb by ear (D-42); the 1 ms limiter latency (D-45).
- Tune by ear: velocity → level default (50%), drive amount, decay range, 4 voices per pad.
- Still open from earlier: real-time priority (`no RT` in the header; dropouts without it, see V-10), latency feel, `ion --list-midi` with the OXI, macro ranges.

## Next (Phase 5: sampler)
Keymapped sampler: one sample per track, root note and key range, loop (forward, ping-pong, crossfade, zero-crossing snap), amp and filter envelopes, SVF with key tracking, LFO, glide; the playback mode field with Stretch reserved.

## Known gaps carried forward
- Global default CC mappings (D-26).
- Near-duplicate FM patches aren't folded together (D-30).
- No samples index in library.db yet; the browser reads folders (D-35).
- Pads can't be copied or renamed yet; a pad's name is its file name.
- No effect presets yet (the FX mockup's "Preset"; D-06 says `o` / `w`).
- The Tracks detail pane at 80×24 cuts its last rows (inherited from 0.1; to fix with the narrow-layout polish in Phase 8).
