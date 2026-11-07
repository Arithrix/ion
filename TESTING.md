# Testing ion 0.3.0 — Phase 3: drum sampler

This checkpoint should show:
- you can build a garage kit from your deep house pack by browsing and listening, without leaving the keyboard;
- the kit plays from the OXI: pads fire on time, hats choke each other, nothing clicks;
- the kit saves, loads onto another track, and the project can carry its own copies of the samples.

Allow about 30 minutes. **Wired headphones or speakers please**: Bluetooth adds 150–250 ms of delay.

## 0. Before you start
1. Run `ion --version`. It should print `ion 0.3.0 (…)`.
2. Open the demo project, which was installed with this release:
   ```
   ion --project "demo v0.3.0"
   ```
   - If the header says `no RT`, there will be the odd dropout (they happen with or without drums). Tell me if you see it; it's still unconfirmed from 0.0.1.

## 1. Point the browser at your samples (1 min)
The deep house pack I downloaded is in `~/ion/downloads/drums/`. The browser starts in `~/ion/samples`, which doesn't exist yet, so add the pack:
1. Press `,` (Settings), then Tab until **Samples** is lit.
2. ↓ to **add a root**, ⏎, type `~/ion/downloads/drums` (Tab completes folder names), ⏎.
3. Your own sample folders can go in the same way. `x` on a root removes it (it only forgets the folder; nothing is deleted).

## 2. The demo kit (3 min)
1. Press `5` (track 5, a DRUM track), then `I`. On the left is the sample browser; on the right the 16 pads, and the selected pad's settings below them.
2. Press Tab to reach the pads. Arrows move between pads; ⏎ or Space plays the selected one. The pad's light flashes when it plays.
3. Press `` ` `` (piano mode): the Z row plays pads 1–16 (`Z` = kick, `S` = snare, `X` = clap …). Esc leaves piano mode.
4. From the OXI: send notes **C1–D#2** on **channel 5**.
   - If the OXI sends other notes, `~` shows exactly what's arriving.
   - Either set the OXI to C1–D#2, or change each pad's **Note** (last row of the pad settings) to match.

## 3. Build a garage kit (12 min)
1. Make an empty track a drum track: `T`, select track 6, ⏎, choose **DRUM**, ⏎. You land on its drum screen.
2. In the browser (left), ⏎ into `~/ion/downloads/drums/`, then `soundpacks-deep-house/x/Deep House Drum Samples/`. There are folders of kicks, claps, hats, percussion, rides, shakers, snares and toms.
3. In `bd_kick/`, press ↓ through the files. **Each one plays as you reach it**, with its length and a waveform at the bottom.
   - Scrolling fast should never stutter or pile up sounds.
   - Settings → Samples turns the preview off, or changes its level.
4. On the kick you want, press ⏎: it goes onto the **selected pad** (pad 1 to start), and the status line says `pad 1 ← …`.
5. To fill other pads, Tab to the pads, move to the next pad, then Tab Tab back to the browser and ⏎ on a file. A layout that suits the OXI's drum lanes:
   - 1 kick, 2 snare, 3 clap, 4 rim;
   - 5 closed hat, 6 open hat, 7 shaker, 8 ride;
   - 9–12 percussion and toms.
6. **Choke the hats:** select pad 5 and press `c` (choke group A), then pad 6 and `c`. Now play the open hat and then the closed hat: the open hat stops, quickly but **without a click**.
7. **Shape the pads** (Tab to the pad settings; ↑ ↓ picks a row, ← → changes it, Shift for bigger steps):
   - **Tune** and **Fine**: pitch the snare up a couple of semitones, the kick down.
   - **Hold** / **Decay**: hold "full" plays the whole sample. Shorten Hold, and Decay then fades it out: tight hats, short kicks.
   - **Mode** gate: the pad plays only while the note is held (from the OXI, gate length matters).
   - **Filter** LP/HP/BP with **Cutoff** and **Resonance**: high-pass a clap, low-pass a shaker.
   - **Drive**: grit on the kick.
   - **Start**: skip into a sample. **Reverse**: play it backwards.
   - **Vel→level** / **Vel→filter**: how much the OXI's velocity changes level and brightness.
   - Changes apply **to sounds already playing**, smoothly. Turn Tune or Cutoff while a long sample rings.
8. **MIDI-learn** any pad setting: focus it, press `l`, move an OXI knob. The demo kit already has **CC 74 on channel 5 → open hat decay**.
9. Program a garage beat on the OXI on channel 6 and play it for a while: kick, clap on 2 and 4, shuffled hats with the open hat choked.

## 4. Save, load, collect (5 min)
1. Press `k` and give the kit a name, e.g. `Garage 01`. It's saved in `~/ion/kits/`, and the pads' title shows it.
2. `L` (Library), Tab, ↓ ↓ to **Drum kits**, →: the kit is listed, and its 16 pads on the right. Select another drum track (e.g. `5`), and ⏎ on the kit loads it there, replacing that track's pads.
3. The kit is also in the browser's top level under **drum kits**: ⏎ on it loads it onto the current track.
4. Back on track 6, press `g` (collect). The samples are copied into the project's own `samples/` folder; then `w` saves. The project no longer depends on `~/ion/downloads`.
5. `W` (save as) under a new name copies the samples folder along with it.
6. Tracks (`T`) on a drum track shows the selected pad (◀ ▶ picks another) with its level, tune, decay, pan and cutoff.

## 5. Small screen (1 min)
Narrow the terminal below 80 columns, or run `ion --ui-snapshot 60x35 --screen instrument --track 5`. The browser gets the whole screen, and Tab switches to the pads with their settings underneath.

## What to send back
- Did anything click, cut off oddly, or arrive late from the OXI?
- The defaults: Vel→level is 50%. Too much or too little touch? Is a decay of 500 ms after a short hold a useful start?
- Is 4 overlapping hits per pad enough for rides and open hats, or do you hear them being cut?
- Drive: useful amount, or too much at the top?
- How did building the kit feel? What slowed you down?
- From earlier: is `no RT` gone, how does the latency feel, and what does `ion --list-midi` print with the OXI connected? The macro ranges too.

## Known issues
- A pad's name is its file's name, and pads can't be copied to another pad yet.
- The browser reads folders as they are; there's no search across the whole collection yet.
- While the header says `no RT`, expect the occasional dropout. They happen with nothing playing too; real-time priority fixes them.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.2.1   # back to Phase 2
~/projects/ion/scripts/use-version.sh         # list installed versions
```
Projects saved by 0.3.0 are format 3, and **0.2.x won't open them** ("saved by a newer ion"). It refuses rather than silently losing the drum tracks. Projects from 0.1.0 and 0.2.x open fine in 0.3.0. Kits in `~/ion/kits` are only used by 0.3.0 and later.

## Extra tools in this release folder
`ion-render` (project + .mid → .wav), `ion-bench` (now with drum scenes), `fake-oxi` (a pretend OXI), `make-demo`.
