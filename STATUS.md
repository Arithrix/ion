# Status

**Phase:** 2 · FM + library — done; release 0.2.0 is the checkpoint build.
**Branch:** `phase/2-fm-library`, merged to `main` for the release.
**Updated:** 2026-11-07

## Done (Phase 2)
- **Library:** SQLite (vendored 3.54.1).
  - Recursive `.syx`/`.snd` import on a worker thread. Bulk, single, headerless, concatenated and unterminated dumps all import; corrupt dumps are logged and skipped.
  - De-duplicated on normalised parameters; the first-seen name and every source bank are kept.
  - Favourites, tags, recent, rough categories.
  - The 1,040 downloaded files give 12,876 unique voices in 0.25 s.
- **FM engine:** click-free patch handover (three Synth_Dexed instances, D-27) and six macros as CC-mappable parameters (D-29). Synth_Dexed's `Env::update` is patched so parameter changes don't restart held envelopes (D-28).
- **UI:**
  - FM browser: filter, views, auto-load, favourites, tags, program list, the 32 algorithm diagrams (boxed or compact), macro pane, save as, export, import with progress. Comet layout too.
  - Library screen with project contents and FM counts.
  - Macros on Tracks.
  - Settings for program change and FM preview.
- **Program change:** a per-project list of up to 128 voices, applied on the audio thread (D-32).
- **Projects:** format 2, which reads format 1 unchanged (D-33).
- **Tests:**
  - handover (held notes, fast scrolling, retriggers, sustain);
  - macro regressions against upstream Synth_Dexed;
  - import edge cases and fuzzing, dedupe, export round-trip;
  - session save/load of macros and programs, bake, program change;
  - algorithm topology against the DX7 chart, and a golden file of all 32 diagrams;
  - UI snapshots of the browser at 130×36, 80×24 and 60×35.
- 82 test cases.

## Waiting on Michael
- Try 0.2.0 (`TESTING.md`).
- Tune the macros by ear together: ranges, and whether they should reset on a patch change.
- Still unconfirmed from 0.0.1: real-time priority (`no RT` in the header), latency feel, `ion --list-midi` with the OXI.

## Next (Phase 3: drum sampler)
Disk thread and sample cache (decode and resample to 48 kHz off the audio thread), sample browser with preview, 16 pads with all per-pad parameters, SVF, choke groups, kits as JSON, collect. The shared voice allocator arrives with it (D-26).

## Known gaps carried forward
- Global default CC mappings (D-26).
- Near-duplicate patches aren't folded together (D-30).
