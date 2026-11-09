# Testing ion 0.9.0 — Phase 9: a 303-style bass

This checkpoint should show:
- **ACID** playing acid lines from the OXI: **slides** on held-over notes, **accents** on loud ones;
- the **Drive** insert, on ACID or anything else.

Allow about 15 minutes. Wired headphones or speakers. The OXI on channel 2.

## 0. Before you start
1. `ion --version` → `ion 0.9.0 (…)`.
2. `ion --project "demo v0.9.0"`

## 1. ACID (10 min)
1. Track 2 (channel 2) is ACID. `2`, then `I`: the 303's knobs in its order (waveform, cutoff, resonance, env mod, decay, accent), the amp envelope under them, and "play" beside it.
2. Program a 16-step line on the OXI on channel 2. Then:
   - **Slides:** tie a step into the next (or set its length past 100%). The pitch glides instead of jumping and the filter doesn't retrigger. Slide sets how long the glide takes.
   - **Accents:** steps at velocity **100 or more** hit harder and brighter. If the OXI's velocities don't land there, change **Accent at**.
3. cc **74** is cutoff, cc **71** resonance. A synced LFO is nudging the cutoff a little (`O` to see it, or turn its depth down).
4. Turn the knobs while it plays: cutoff, resonance, env mod and decay are the classic four.
5. A new ACID track: an empty track, ⏎, **ACID**.

## 2. Drive (5 min)
1. Track 2 has a **Drive** in insert 1. `F`, then the slot: drive, shape (soft / hard / fold), bias, tone, level, mix.
2. Put one on any other track (`F`, an empty slot, `a`, **DRIVE**): drums and Plaits are good tests. Fold at high drive is the wild one.

## What to send back
- Does it feel like a 303 from the OXI? Do slides and accents come out where you programmed them?
- Levels: is ACID too loud or quiet next to the other tracks, especially with accents?
- Drive: the sound of soft vs hard vs fold; anything harsh or fizzy at high drive.

## Known issues
- ACID is one voice (as a 303 is): chords play as slides between their notes.
- Accent comes from velocity only: there's no separate accent control from the OXI.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.8.0
```
Projects saved by 0.9.0 are format 9, and 0.8.x won't open them.
