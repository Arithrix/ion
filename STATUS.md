# Status

**Phase:** 3 · Drum sampler — done; release 0.3.0 is the checkpoint build.
**Branch:** `phase/3-drum-sampler`, merged to `main` for the release.
**Updated:** 2026-11-07

## Done (Phase 3)
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
- Try 0.3.0 (`TESTING.md`): build a garage kit from the deep house pack and play it from the OXI.
- Tune by ear: velocity → level default (50%), drive amount, decay range, 4 voices per pad.
- Still open from earlier: real-time priority (`no RT` in the header; dropouts without it, see V-10), latency feel, `ion --list-midi` with the OXI, macro ranges.

## Next (Phase 4: effects)
Compressor, chorus, tempo-synced delay and reverb, 3 insert slots per track, two send buses, master chain with limiter, FX screen. Reverb chosen by sound first, A53 cost second; numbers at the checkpoint.

## Known gaps carried forward
- Global default CC mappings (D-26).
- Near-duplicate FM patches aren't folded together (D-30).
- No samples index in library.db yet; the browser reads folders (D-35).
- Pads can't be copied or renamed yet; a pad's name is its file name.
