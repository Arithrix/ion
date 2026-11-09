# Testing ion 0.10.0 — Phase 10: the chip synth

This checkpoint should show **CHIP**, a lo-fi wave synth in the spirit of the M8's WavSynth: a little wave you shape as it plays, crunchy on purpose.

Allow about 15 minutes. If you haven't tried 0.9.0 (ACID and Drive), its checklist is in `~/ion/releases/v0.9.0/TESTING.md`.

## 0. Before you start
1. `ion --version` → `ion 0.10.0 (…)`.
2. `ion --project "demo v0.10.0"`

## 1. CHIP (10 min)
1. Track 3 (channel 3) is CHIP. `3`, then `I`. At the top, **the wave as it is now**, drawn from its table; under it the eight settings that make it.
2. Play it (OXI channel 3, or `` ` `` and the keys). Each note has a blip of pitch: envelope 1 is on **tune** (`O` shows the route).
3. Shape the wave while it plays:
   - **Shape:** pulses 12–75%, saw, triangle, sine, **noise tone** (pitched noise: it plays the note) and **noise**.
   - **Size:** 4–256 steps; low is crude and buzzy.
   - **Mult:** repeats the wave inside itself: a hard-sync sound.
   - **Warp:** squashes it to one side. **Mirror:** folds it back on itself.
   - **Amp** pushes it into the **Limit** (clip, sine, fold, wrap); fold and wrap at high amp are the wild ones.
   - **Bits:** 8 down to 1.
4. **Filter** (low/high/band-pass, notch) with cutoff and resonance; **Rate** brings the whole track's sample rate down for crunch.
5. The amp envelope sits beside the filter, as on the other instruments; Vel→amp is 50% here.
6. `v` on any of them modulates it (warp and mult with an LFO are fun).

## What to send back
- Does it feel like the M8's WavSynth territory? What's missing?
- Which shapes and limits are useful, and is anything just noise?
- Levels next to the other tracks.

## Known issues
- The wave is read without interpolation, so high notes alias: that's the chip sound, on purpose.
- Rate and the filter are per track and per voice respectively; there's no per-voice rate.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.9.0
```
Projects saved by 0.10.0 are format 10, and 0.9.x won't open them.
