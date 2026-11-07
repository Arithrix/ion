# Status

**Phase:** 0 · prototype gate — built; release 0.0.1 is the checkpoint build.
**Branch:** `phase/0-prototype`, merged to `main` for the release.
**Updated:** 2026-11-06

## Done (Phase 0)
- CMake build, generated version header, `ion --version`; release and rollback scripts.
- Vendored and pinned: Synth_Dexed, FTXUI 7.0.3, miniaudio, dr_libs, doctest (`third_party/README.md`).
- Synth_Dexed building on Linux via `arm_math.h` shim. Fixed the post-load envelope bug that silenced most decaying patches (V-07).
- syx parsing/packing, tolerant of the damage seen in the real archive.
- PipeWire playback: 128 frames, follows the default device, 0 errors (V-08).
- ALSA MIDI input with hot-plug, verified with the `fake-oxi` tool (unplug/replug, clock).
- Tracks screen at 130×36 / 80×24 / 60×35, meters, patch stepping, audition, panic. `--ui-snapshot WxH` prints a frame as text.
- `ion-bench`: 4–6 µs per FM voice per 128-frame buffer (V-09).
- 20 test cases (syx round-trip and damage, FM render, module routing, callback-size independence, ceiling, SPSC).
- Soak tests (V-10): 0 dropouts idle; many dropouts under a 16-core compile without RT priority. RT should fix it; Michael to confirm after logging in again.

## Waiting on Michael
- Try 0.0.1 with the OXI (`TESTING.md`): go/no-go on the stack.
- Log out and back in so ion gets real-time priority (it shows `no RT` until then).
- GitHub: `sudo apt install gh`, then `gh auth login`. After that I create the private repo and push.
- `ion --list-midi` output with the OXI plugged in.

## Next (Phase 1, after go)
Engine interface `setParam`/serialize, sample-accurate MIDI, clock follower, channel map, mixer + EQ, project save/load/autosave, CC learn, piano mode, MIDI monitor, help overlay, Settings, narrow layout, `ion-render` + golden tests.
