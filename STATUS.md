# Status

**Phase:** 1 · engine skeleton — done; release 0.1.0 is the checkpoint build.
**Branch:** `phase/1-engine-skeleton`, merged to `main` for the release.
**Updated:** 2026-11-06

## Done (Phase 1)
- **Engine:** 8 tracks, channel map with layering, timestamped sample-accurate MIDI (tight/immediate), clock follower (regression tempo, transport, bars), panic.
- **Mixer:** fader, pan, mute, solo-in-place, RMS/peak/hold meters, channel EQ (RBJ, glides, click-free switches), master fader, safety ceiling.
- **Parameters:** stable keys, ParamStore, CC map and learn.
- **Projects:** JSON format 1 with migration hook, save/load/save-as, autosave, recovery, last-project start-up; `config.toml`; headless mode.
- **UI:** Tracks, Mixer (braille EQ curve), Instrument (live bank browsing), Settings, Library (projects), help, MIDI monitor, engine picker, piano mode, wide/narrow layouts.
- **Tools:** `ion-render`, SMF reader/writer, golden WAV test, UI snapshot goldens at 4 sizes, ALSA virtual-port test (routing, CC map, clock, hot-plug), `make-demo`.
- 60 test cases.

## Waiting on Michael
- Try 0.1.0 (`TESTING.md`).
- Still unconfirmed from 0.0.1: real-time priority after logging in again (the header shows `no RT` if not), latency feel, and `ion --list-midi` with the OXI.

## Next (Phase 2: FM + library)
syx import into SQLite with dedupe, the FM browser (filter, favourites, tags, categories, live-load with a click-free handover), the six macros, save-as/bake, bank export, the favourites list and program change.

## Known gaps carried forward
- Global default CC mappings (D-26).
- Shared voice allocator: Phase 3 (D-26).
