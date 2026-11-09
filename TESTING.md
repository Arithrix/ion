# Testing ion 0.7.1 — Phase 7: Plaits, undo and redo, the mouse

This checkpoint should show:
- **every Plaits model playing** from the OXI, polyphonic, with Harmonics, Timbre and Morph doing what the screen says they do;
- **Ctrl+Z / Ctrl+Y** undoing and redoing anything you change, on any screen;
- **the mouse** working the way it does in Midnight Commander.

Allow about 30 minutes. **Wired headphones or speakers please.** The OXI is useful for Plaits (chords and CCs) but not needed: piano mode (`` ` ``) plays any track from the laptop keys.

## 0. Before you start
1. Run `ion --version`. It should print `ion 0.7.1 (…)`.
2. Open the demo project:
   ```
   ion --project "demo v0.7.1"
   ```
3. Use your usual terminal (xfce4-terminal is fine). The mouse needs nothing set up.

## 1. Plaits (15 min)
1. Track 8 (channel 8) is **Plaits on String machine**. Press `8`, then `I`. On the left is the **model grid**: green (the originals), red (noise, strings, drums), orange (firmware 1.2). On the right are the macros with what each does on this model, then the voice, the LPG and the envelope.
2. Play chords on channel 8 from the OXI (or `` ` `` and the Z row). Harmonics shows the chord (`m7`). CCs **20, 21 and 22** on channel 8 move Harmonics, Timbre and Morph; the bars move with them.
3. **Every model.** With the cursor in the grid, ↑ ↓ ← → moves, and the model changes as you go, so keep playing while you browse. For each, check the three macros do roughly what the right-hand pane says. Tell me any that are silent, wrong, much too loud or too quiet.
   - The red drums (Kick, Snare, Hi-hat) and String / Modal make their own envelope: they ring out however long you hold.
   - **6-op FM a** plays the DX7's ROM1A bank on this track: Harmonics picks the patch (its name shows under the grid). b and c play Plaits' own banks.
4. **The LPG.** Tab to the right-hand pane and set **LPG**:
   - **gate** (default): notes sustain while held and close when you let go. LPG decay sets how slowly.
   - **ping:** every note is struck and dies away by itself, held or not. This is the classic Plaits pluck. Try it on Virtual analog or Waveshaper with LPG decay low.
   - **off:** the raw model, shaped only by the amp envelope (drawn as "amp" right under the macros, as on FM).
5. **Voices:** set Voices to 1 for mono, then back to 6. Play more notes than voices: the oldest goes.
6. **Your favourites in 6-op FM:** press `f`. The list fills from your FM favourites (★ on an FM track's library) and the model jumps to 6-op FM a. `x` puts Plaits' bank back. (The demo starts with ROM1A as its list.)
7. **Presets:** `a` saves the track's Plaits settings under a name; change some things; `o` lists the presets, ⏎ loads one.
8. **A Plaits track of your own:** on an empty track, `T`, ⏎ (or `e`), choose **PLTS**.
9. `l` on any Plaits value learns a CC, as everywhere else.

## 2. Undo and redo (5 min)
1. Change anything: move a fader (← → on Tracks), a macro, an EQ band, a pad's tune, an effect. Press **Ctrl+Z**. It goes back, and the status line says what was undone ("undid Track 3 · Fader"). **Ctrl+Y** puts it back.
2. Holding ← → for a while is **one** undo, not forty.
3. Bigger things are undone too: changing a track's engine (its patch and settings come back), loading a kit, adding or removing an effect, a CC mapping, a take (Rec screen). Undoing a take keeps its recording in the project's recordings now.
4. Moving a knob on the OXI is **not** undone: that's playing, not editing. Undo something else and the knob's value stays where you left it.
5. Ctrl+Z works in piano mode too, where the letters are notes.
6. Up to 256 steps. Opening a project starts a fresh history.

## 3. The mouse (10 min)
- **The header:** click Tracks, Instr, Mixer, Fx, Rec or Library to switch screens. **↶ undo** and **↷ redo** are buttons (grey when there's nothing to undo or redo).
- **The hint bar:** click any hint that names one key (`⏎ Edit`, `Tab Pane`, `? Help`…) and it presses it.
- **Lists** (tracks, FM patches, samples, kits, projects, the Plaits grid): click selects, double-click opens or loads (like ⏎), and the wheel moves through the list under the pointer.
- **Values** (any parameter row, the EQ table): the wheel changes the value under the pointer; click and drag sideways changes it a step per character.
- **Track rows:** drag sideways to move that track's fader.
- **Mixer:** click a strip to select it, the wheel or an up-and-down drag moves its fader, and M and S are buttons.
- **Drum pads:** a click selects the pad and plays it.
- **Rec screen, reviewing slices:** click on the waveform to pick the slice under the pointer and hear it.
- A click outside the help, the MIDI monitor or the engine picker closes it.
- **Selecting text in the terminal** while ion runs: hold **Shift** while you drag.

## What to send back
- Plaits: any model that's silent, broken, harsh or the wrong level; macro descriptions that are wrong; whether **gate** is the right default for the LPG or you'd rather have **ping**.
- How Plaits sits in CPU with your chords: the header's `cpu` figure with 6 voices going.
- Undo: anything you expected to undo that didn't, or that undid more than you expected.
- Mouse: anything you clicked that did nothing, or did the wrong thing.
- From before: FM without the old Attack/Release macros (keep it or bring them back?), chord slicing on your material, `no RT`, latency, `ion --list-midi`.

## Known issues
- Plaits presets open from the Plaits screen only, not the Library screen.
- The 6-op list fills from favourites as a whole: no picking one patch at a time yet.
- The macro descriptions paraphrase the Plaits manual.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.6.0   # back to Phase 6
~/projects/ion/scripts/use-version.sh         # list installed versions
```
Projects saved by 0.7.0 are format 7, and **0.6.0 won't open them** ("saved by a newer ion").

## Extra tools in this release folder
`ion-render`, `ion-bench` (now with every Plaits model), `fake-oxi`, `make-demo`.
