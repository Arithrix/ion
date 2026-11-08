# Testing ion 0.4.0 — Phase 4: effects

This checkpoint should show:
- you can **dial in a mix**: compression, chorus, delay and reverb on the tracks that need them, sends to shared delay and reverb, and a glued master;
- nothing clicks when effects are added, bypassed or tweaked while playing;
- synced delays follow the OXI's tempo.

Allow about 30 minutes. **Wired headphones or speakers please**: Bluetooth adds 150–250 ms of delay.

## 0. Before you start
1. Run `ion --version`. It should print `ion 0.4.0 (…)`.
2. Start the OXI's clock. Then open the demo project, which was installed with this release:
   ```
   ion --project "demo v0.4.0"
   ```
   - The header should show the OXI's tempo. Without a clock, synced delays use 120 BPM.

## 1. The demo's mix (5 min)
Play the OXI on channels 1, 2, 3 and 5 (piano with strings, bass, brass, the drum kit).
1. Press `F` (FX). On the left is track 1's chain:
   - its EQ;
   - three insert slots (slot 1 has a **CHORUS**);
   - its two sends (B → reverb at −10 dB);
   - below them the two send buses (**A DELAY 1/8D**, **B REVERB hall**);
   - then the master: EQ, a **COMP** switched out (○), the **LIMIT**.
2. `1`–`8` switch tracks: track 2 has a compressor on the bass, track 5 one on the drums.
3. On track 1, ↓ to the chorus and press `b` repeatedly while the piano plays. It switches in and out **without a click**.
4. ↓ to **B REVERB** and press Tab: its settings.
   - Change **Type** (Plate / Hall / Room), **Size** and **Decay**. The drawing underneath shows the tail. Listen for clicks or zipper noise while you sweep them; there should be none.
   - The demo maps **CC 75 on channel 1 → reverb size** and **CC 76 → delay feedback**. Try them from the OXI.
5. ↓ to **A DELAY**: Division 1/8D. The line below it gives its time in ms at the current tempo. Change the OXI's tempo: the echoes follow, gliding rather than jumping.

## 2. Build a mix of your own (15 min)
Start from your garage kit from 0.3.0, or any project.
1. **Add an effect:** on FX, pick a track (`1`–`8`), ↓ to an empty slot, ⏎. Choose one from the list, ⏎. It's in, at its defaults, ready to adjust (Tab).
2. **A compressor on the drums:**
   - Threshold −20, Ratio 4:1, Attack 10–30 ms (higher keeps the kick's punch), Release 100–200 ms.
   - The `reduction` bar shows how much it's turning down. **Makeup** brings the level back; **Mix** below 100% is parallel compression.
3. **Sends:** in a track's chain, ↓ to **→ delay** or **→ reverb** and use ← → (Shift for bigger steps).
   - Try the claps and the snare into the reverb, a little of the hats into the delay.
   - **sends** (⏎) switches between post-fader (they follow the track's fader) and pre-fader.
4. **Delay settings:**
   - Sync on: Division picks the note value. Sync off: Time in ms.
   - Feedback, High cut / Low cut (the repeats get darker or thinner), Ping-pong, Width.
   - 100% feedback rings forever without blowing up; `p` (panic) stops it.
5. **Reorder:** with two effects on a track, `[` and `]` move the selected one up or down, settings and all. `x` removes one.
6. **The master:**
   - ↓ to the master's **COMP** and press `b` to switch the glue compressor in. Is it a useful default (−12 dB, 2:1)?
   - The **LIMIT** row shows when the safety limiter is turning things down. If it's often busy, turn the master or the loudest track down.
7. **Mixer (`M`):**
   - ← → now also reaches **D** (delay return) and **R** (reverb return) before **M** (master). Each has a fader (↑ ↓), pan (`[ ]`), mute (`m`) and its own EQ (Tab).
   - The delay return's **→R** feeds the reverb.
   - Each track strip shows its sends (`A−12 B−8`).
8. **Solo** a track (`s`): the returns keep playing (they're solo-safe). **Mute** a track: its sends go quiet too.
9. Save (`w`), quit, and open the project again: everything is as you left it.

## 3. Feel (2 min)
The safety limiter adds **1 ms** to everything you hear. Play the piano and the drums from the OXI and from the laptop keys (`` ` ``): does it feel any different from 0.3.0? If so, tell me; it can go lower.

## What to send back
- **The reverb**, by ear: does it sound good? Plate, hall or room: which works and which doesn't? This is the one I most need your ears for (the handoff says "sound first").
- Did anything click: adding, bypassing, reordering, or sweeping a setting?
- Did the synced delay stay in time with the OXI?
- Is the glue compressor's default useful?
- Anything you reached for that wasn't there (effect presets are coming)?
- From earlier: `no RT`, latency feel, `ion --list-midi` with the OXI, macro ranges; and your 0.3.0 notes if you haven't sent them.

## Known issues
- No effect presets yet.
- A big hall can't decay faster than about a second, whatever Decay says (the room is too big).
- At 80×24 the Tracks screen cuts the last rows of its detail pane.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.3.0   # back to Phase 3
~/projects/ion/scripts/use-version.sh         # list installed versions
```
Projects saved by 0.4.0 are format 4, and **0.3.0 won't open them** ("saved by a newer ion"). It refuses rather than silently dropping the effects. Projects from 0.3.0 and earlier open fine in 0.4.0.

## Extra tools in this release folder
`ion-render` (project + .mid → .wav), `ion-bench` (now with every effect), `fake-oxi` (a pretend OXI), `make-demo`.
