# Status

**Phase:** pre-0 — plan written, waiting for Michael's go-ahead.
**Updated:** 2026-11-06

## Done
- Read HANDOFF.md and all 12 mockups.
- Inspected the laptop (DECISIONS.md V-05).
- Verified Plaits 1.2 source, Synth_Dexed, and dependency licenses (V-01 to V-04).
- Wrote PLAN.md and DECISIONS.md.
- Michael's answers: the keyboard is gate-only and low priority (D-04, D-05); audio goes out through native PipeWire (D-02).
- Downloaded DX7 banks and deep house drums to `~/ion/downloads` (V-06).

## Next (Phase 0, once approved)
1. `git init`, CMake skeleton, version header, `ion --version`.
2. Vendor miniaudio, Synth_Dexed, FTXUI, doctest, dr_libs at pinned commits.
3. PipeWire audio out, OXI in, one Dexed voice from a .syx, bare FTXUI shell.

## Blockers: Michael to run
Build prerequisites:
```
sudo apt install cmake ninja-build pkg-config libasound2-dev libpipewire-0.3-dev \
                 ccache g++-aarch64-linux-gnu qemu-user
```
Real-time permission. This reuses PipeWire's own limits file, `/etc/security/limits.d/25-pw-rlimits.conf`. Log out and back in afterwards.
```
sudo usermod -aG pipewire arithrix
```

## Open
- Keymap fixes (D-06) and the import/dedupe policy (D-07, D-08): waiting on Michael.
- Git remote / CI: waiting on Michael.
