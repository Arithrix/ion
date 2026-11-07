# Testing ion 0.5.0 — Phase 5: sampler

This checkpoint should show:
- a sample plays **chromatically** from the OXI or the laptop keys, in tune, from its root note;
- loops sustain cleanly (no click at the loop point) and the envelopes, filter and LFO shape it;
- a loop that knows its tempo shows its bars.

Allow about 20 minutes. **Wired headphones or speakers please.** If you haven't tried the effects yet, 0.4.0's checklist is in `~/ion/releases/v0.4.0/TESTING.md`.

## 0. Before you start
1. Run `ion --version`. It should print `ion 0.5.0 (…)`.
2. Open the demo project:
   ```
   ion --project "demo v0.5.0"
   ```

## 1. The demo's sampler (5 min)
1. Press `6` then `I`. Track 6 is a **SMPL** track with a 4-bar chord loop at 120 BPM.
   - At the top is its waveform, with a **bar ruler** (1 · · · 2 …) because the loop carries its tempo.
   - Below it are four sections: **Play**, **Amp**, **Filter**, **Mod**.
2. Play it from the OXI on **channel 6**, or press `` ` `` and use the laptop keys:
   - **C3 is the root**: it plays as recorded;
   - every other key plays it higher or lower (shorter or longer).
   - It's looped (forward), so held notes keep going; releasing fades out over the Release time.
3. Space plays the root note.
4. The demo maps **CC 74 on channel 6 → cutoff**, and the LFO moves the cutoff once a bar.

## 2. Your own sample (12 min)
1. Make an empty track a sampler: `T`, pick a track, ⏎, choose **SMPL**, ⏎. The sample browser opens by itself, the same one as on drum tracks.
2. Pick something tonal: a chord stab, a bass note, a pad. Each file previews as you reach it; ⏎ loads it.
   - `o` opens the browser again (at that sample's folder); `o` or Esc goes back.
3. **Root:** if it isn't a C, set **Root** (Play section) to its real note, so the keys play in tune.
4. **Loop it** (Play):
   - set **Loop** to forward;
   - ↓ to **Loop start** and **Loop end** and move them with ← → (Shift for bigger steps). They snap to zero crossings (**Snap** on) to avoid clicks.
   - `z` zooms the waveform round the point you're moving (×4, ×16, ×64); `z` again cycles back.
   - Still a click? Raise **Crossfade** (10–50 ms).
   - Try **ping-pong** too.
5. **Shape it:**
   - Tab moves between the sections; ↑ ↓ picks a row; ← → changes it.
   - **Amp:** a slow Attack for a pad, a long Release for a tail. **Vel→amp** sets how much touch matters.
   - **Filter:**
     - LP24 with a lower Cutoff, then **Env amount** for a filter sweep on each note;
     - **Key track** at 100% keeps the brightness even up the keyboard.
   - **Mod:** LFO shape, **LFO sync** on with **LFO length** 1/4 or 1 bar, and a little **→ pitch** (10–20 ct) for vibrato, or **→ cutoff** for movement.
   - **Glide** (Play) slides between notes; with **Voices** at 1 it's mono.
6. `n` normalizes the sample (its peak to 0 dB, through Level).
7. MIDI-learn anything with `l`.
8. `g` (collect) copies the sample into the project's own folder, as on drum tracks. Then save (`w`).

## 3. Recordings
Phase 6 (resampling) will make recordings that carry their tempo and bars. The sampler already reads them: that's the demo's bar ruler.

## What to send back
- Was it in tune across the keyboard, and did the root note play as recorded?
- Any click at a loop point, even with Snap on? Did Crossfade fix it?
- Did the envelopes and filter feel right (ranges, speeds)? The LFO?
- How was moving loop points with the keys? Is the zoom enough?
- From before: your notes on 0.3.0 (drums) and 0.4.0 (effects), `no RT`, latency, `ion --list-midi`.

## Known issues
- One sample per track (multi-zone and SFZ come later).
- Stretch mode is reserved: "stretch: later".
- One LFO per track, free-running (not restarted by each note).

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.4.0   # back to Phase 4
~/projects/ion/scripts/use-version.sh         # list installed versions
```
Projects saved by 0.5.0 are format 5, and **0.4.0 won't open them** ("saved by a newer ion"), rather than turning sampler tracks into empty ones.

## Extra tools in this release folder
`ion-render`, `ion-bench`, `fake-oxi`, `make-demo`.
