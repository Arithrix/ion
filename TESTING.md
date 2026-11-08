# Testing ion 0.6.0 — Phase 6: resampling

This checkpoint should show:
- a take **starts on the OXI's next bar and ends exactly on a downbeat**, so a recorded loop plays back seamlessly;
- **chords resampled onto drum pads**, cut where the chords change, playable from the OXI;
- a take into the **sampler** that loops from C3, and **undo** that puts everything back.

Allow about 25 minutes. **Wired headphones or speakers please.** You'll want the OXI for this one: the clock is the point. If you haven't tried 0.4.0 (effects) or 0.5.0 (sampler) yet, their checklists are in `~/ion/releases/v0.4.0/` and `v0.5.0/`.

## 0. Before you start
1. Run `ion --version`. It should print `ion 0.6.0 (…)`.
2. Open the demo project:
   ```
   ion --project "demo v0.6.0"
   ```
3. On the OXI, set the tempo to something you like and make sure it **sends clock** to ion. The header should show `▶ 124.0 ext clock` (or `■` while stopped).

## 1. A take that's already made (3 min)
1. Track 7 (channel 7) is a drum track whose pads 1–4 are **slices of track 6's chord loop**. make-demo recorded it through the real recorder, 4 bars at 120 BPM, cut on its chord changes.
2. Play C1, C#1, D1, D#1 on channel 7 (or `7`, `` ` ``, then Z S X D on the laptop keys): one chord each. They're in choke group A, so a new one cuts the last.
3. `R` opens the Rec screen. **recent recordings** lists that take and where it went ("drum · track 7 · pads 1–4").

## 2. Chords onto pads, your own (10 min)
The Rec screen is already set up for this: **source** tracks 1 and 4 (the piano and the strings layered under it, both on **channel 1**), **4 bars**, **start** next bar, **send to** drum pads on track 7, from **pad 5**, sliced **on chord changes**, and **after**: mute the sources.
1. On the OXI, program four bars of chords on channel 1, one chord per bar (or two per bar, to see if it follows), and start it playing.
2. In ion, press `r`. It shows **◌ ARMED · starts on bar N**; the header shows `◌ ARMED` on every screen.
   - If it says **waiting for the OXI to start** while the OXI is playing, stop and start the OXI's transport once. ion counts bars from the OXI's Start message, so it can't know where bar 1 is if ion started (or the cable went in) mid-song.
3. On the downbeat it turns to **● RECORDING · bar 1 of 4**: the bars fill and the waveform draws in red. Let it run. It stops by itself exactly on the downbeat after bar 4.
4. Now it shows **SLICES**: the take with a line and a number at each cut. Check them:
   - ↑ ↓ moves between slices and plays each one; Space plays it again.
   - ← → nudges a cut by 1 ms (Shift: 10 ms), if one is early or late.
   - `x` removes a cut (two slices become one), `a` splits the slice you're on in half.
   - Did it find your chord changes? Tell me where it was wrong.
5. ⏎ puts them on **pads 5 onwards of track 7** (E1, F1, F#1, G1 on channel 7), one-shot, in a choke group of their own (B, since A is the demo's). Tracks 1 and 4 are now muted (M on the Tracks screen).
6. Play the slices from the OXI on channel 7: a chord per pad, re-sequenced however you like. Repitch them on the drum screen (`7`, `I`) with **tune**.
7. Esc instead of ⏎ keeps the recording but changes no pads.

## 3. Undo (2 min)
1. Press `u` on the Rec screen. The pads go back to how they were, tracks 1 and 4 are unmuted, and the take's files are deleted.
2. `u` only undoes the most recent take.

## 4. Into the sampler (5 min)
1. Set **send to** to **sampler** (↑ ↓ to the row, ← → to change it) and **track** to an empty one, say 8.
2. Set **length** to 2 bars. Press `r`, let it record.
3. Track 8 is now a sampler with the take, **looped from C3**. Send a long C3 on channel 8 from the OXI, starting on a bar: it should loop seamlessly in time with the OXI. `I` shows its bars.
4. Pick a single track as the source and the take is named after its sound (`e.piano-1_124bpm_2bar.wav`). The master mix is `mix_…`, and several tracks are `tracks-1-4_…`.

## 5. Other ways to record (5 min)
- **source: master mix** records everything you hear, reverb and delay included.
- **tap: pre-fx** records the engines dry (no EQ, inserts, fader or pan). Post-fx, the default, is what you hear from the track, even if it's muted.
- **length: until stopped**: `r` starts it on the next bar, and `r` again stops it on the next downbeat.
- **start: now** (no clock needed): `r` starts at once. With a length in bars it counts them at the OXI's tempo (120 without a clock); "until stopped" stops on `r`.
- **slice: at transients** for drum loops; **4 / 8 / 16 equal** for even chops.
- Stop the OXI mid-take: the whole bars recorded so far are kept.
- Esc while armed or recording throws the take away.
- Every take is in the sample browser too: on any drum or sampler track, `recordings (this project)` at the top.

## What to send back
- Did takes start on the right downbeat and loop without a gap or a click at the join?
- How well did chord-change slicing find your changes? Which sounds fooled it (pads with lots of chorus, arpeggios, bass moving under a held chord)?
- Transient slicing on a drum loop: early, late, or missed hits?
- Was the screen clear while armed and recording? Anything you reached for that wasn't there?
- Is muting the sources after a take the right default?
- From before: FM without the old Attack/Release macros (keep it or bring them back?), your notes on 0.4.0 and 0.5.0, `no RT`, latency, `ion --list-midi`.

## Known issues
- Chord-change slicing is tuned on synthesised chords; real material may need a nudge.
- No audio input recording yet.
- Undo covers the last take only, and not after switching project.
- Recordings are 48 kHz 32-bit float WAV (about 23 MB a minute).

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.5.0   # back to Phase 5
~/projects/ion/scripts/use-version.sh         # list installed versions
```
Projects saved by 0.6.0 are format 6, and **0.5.0 won't open them** ("saved by a newer ion"). Their recordings are plain WAV files in the project's `recordings/` folder, usable anywhere.

## Extra tools in this release folder
`ion-render`, `ion-bench`, `fake-oxi`, `make-demo`.
