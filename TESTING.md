# Testing ion 0.10.1 — faster, and the fixes from a full test sweep

Three builds landed overnight. Try them in this order:
1. **0.9.0: ACID, a 303-style bass, and the Drive effect.** Checklist: `~/ion/releases/v0.9.0/TESTING.md` (15 min).
2. **0.10.0: CHIP, a lo-fi wave synth in the M8's spirit.** Checklist: `~/ion/releases/v0.10.0/TESTING.md` (15 min).
3. **0.10.1: this one.** It's 0.10.0, faster, with the bugs a full test sweep found fixed. Both checklists above work on it (use `demo v0.10.1`).

## 0. Before you start
1. `ion --version` → `ion 0.10.1 (…)`.
2. `ion --project "demo v0.10.1"`

## 1. Nothing should sound different (5 min)
The speed-up comes from building ion for this laptop's CPU generation. A full render of the demo differs from 0.10.0 by about −100 dB, which is far below anything audible. Play the demo's tracks and tell me if anything changed.

## 2. Less CPU (2 min)
- With nothing playing and nothing moving, the screen redraws 4 times a second instead of 30. ion's `cpu` in the header and the system monitor should both be lower, and so should xfce4-terminal's.
- Play something and it's back to 30 straight away. Meters, the clock and modulation all move smoothly.
- Moving the mouse over ion no longer makes it redraw.

## 3. The fixes, if you want to poke at them (5 min)
- **FM:** set Voices low (Instrument screen, voice section) and play more notes than that, fast: it steals the oldest note cleanly. (Before, this could corrupt memory.)
- **Plaits Wavetable** (green, 6th) with Harmonics, Timbre and Morph all at the top: it plays. (Before, it could crash ion.)
- **Plaits Speech** (green, 8th): sweep Harmonics from the words back to the vowels while a word plays.
- **Plaits 6-op FM** at full velocity: fine. (It read past a table on every such note.)
- **Glide** on CHIP or Plaits: set a long glide, play, and turn glide off mid-note: the pitch lands where it should.
- **Sampler, LP24 filter, resonance at maximum:** strong, but no longer dangerous. Below about 30% resonance it's exactly as before.
- **Master glue compressor:** remove it (`F`, the master's slot 1, `x`), save, quit, reopen: it stays removed.
- **Undo after moving a switched-off effect** between slots: it stays off.

## Known issues
- ion now needs an x86-64 CPU from 2013 or later (AVX2). On an older one it says so and stops. `ION_PORTABLE` builds one that runs anywhere.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.10.0
```
Project files are the same format as 0.10.0 (10).
