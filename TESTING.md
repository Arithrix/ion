# Testing ion 1.0.0: the first public release

1.0.0 is 0.10.1 with a license and a README. Nothing in the sound or the screens has changed, so there's little to test.

## Checks (2 min)
1. `ion --version` prints `ion 1.0.0 (…)`.
2. `ion --project "demo v1.0.0"`: it opens and all eight tracks play from the OXI.
3. In `~/ion/releases/v1.0.0/` you'll find `LICENSE`, `README.md` and a `licenses/` folder next to `ion`.

If you haven't tried 0.9.0 (ACID, Drive) and 0.10.0 (CHIP) yet, their checklists are still valid on this build:
`~/ion/releases/v0.9.0/TESTING.md` and `~/ion/releases/v0.10.0/TESTING.md`.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.10.1
```
Project files are the same format as 0.10.1 (10).
