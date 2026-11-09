# Testing ion 0.8.0 — Phase 8: modulation

This checkpoint should show:
- **LFOs and envelopes moving any value** on a track, locked to the OXI when you want;
- **`v` on any value** starting modulation at once, and the **Mod screen** to shape it;
- the value you set **staying put** (saved, and what Ctrl+Z goes back to) while modulation moves around it.

Allow about 20 minutes. Wired headphones or speakers. The OXI helps (clock for synced LFOs, notes for envelopes), but piano mode (`` ` ``) works for notes.

If you haven't tried 0.7.x yet (Plaits, undo/redo, the mouse), its checklist is in `~/ion/releases/v0.7.1/TESTING.md`.

## 0. Before you start
1. `ion --version` should print `ion 0.8.0 (…)`.
2. `ion --project "demo v0.8.0"`
3. Run the OXI's clock to ion if you can: synced LFOs follow it (120 BPM without it).

## 1. The demo's modulation (5 min)
1. Press `8` (track 8, Plaits on String machine), then `O`. The header now reads `Tracks Instr Mod Mixer …`: **Mod** is the new screen.
2. Left: **LFO 1** (a triangle, synced, two bars), **LFO 2**, **env 1**, **env 2**, each with its shape and a little bar showing what it's doing now. Right: the **routes**: LFO 1 → Timbre +25%, env 1 → Morph +35%, with **now** showing where each target is.
3. Play chords on channel 8. Timbre sweeps slowly with the bars; each new chord's morph opens and settles.
4. `I` (the Plaits screen): the Timbre and Morph bars have a **lilac marker** that moves. The bar itself is the value you set; the marker is where modulation has it.

## 2. `v`: modulate anything (5 min)
1. On any screen, put the cursor on a value: a fader on Tracks (Tab to the detail pane), a sampler's cutoff, a drum pad's tune, an EQ gain, a reverb's size, a Plaits macro.
2. Press **`v`**. The status line says "LFO 1 → … +25% (route N) · O edits it", and you hear it move straight away.
3. `O` takes you to it. On the routes:
   - **← →** changes the depth (negative works too);
   - **`[ ]`** changes the source: LFO 1, LFO 2, env 1, env 2, velocity, mod wheel, aftertouch, key, random;
   - **⏎** picks a different target from a list of the track's values; type to narrow it (`cut`, `fader`, `pad 3`);
   - **`x`** clears a route.
4. Switches and choices (mute, filter type, LPG mode…) can't be modulated: `v` says so.

## 3. The LFOs and envelopes (5 min)
Tab to the left pane on the Mod screen:
- **LFO:** shape, rate (Hz), **sync** on (then **division** sets the speed: 4 bars … 1/32, triplets, dotted), **restart** (free / every note / every bar), **phase**. A synced LFO stays locked to the OXI's bars.
- **Envelope:** attack, hold, decay, sustain, release (the same envelope as the instruments), and **trigger**: every note, or only the first note of a phrase (legato playing doesn't restart it).
- The mod wheel and aftertouch from the OXI (if it sends them) are sources too.

## 4. With everything else (5 min)
- **Ctrl+Z** undoes route changes and LFO settings like anything else.
- **`l`** on a route's depth learns an OXI knob for it: a "how much" knob.
- Change a track's engine: routes onto the old engine's settings go (others stay). Ctrl+Z brings them back.
- Save, quit, reopen: modulation comes back as it was.

## What to send back
- Does anything zipper, click or step when modulated? Which value, which source, how fast?
- Is 25% from LFO 1 the right thing for `v` to start with?
- Anything you reached for that isn't there (more LFOs, an LFO per voice, a step/sequence source, modulating another track)?
- From before: Plaits (every model, LPG gate vs ping), FM's old Attack/Release macros, chord slicing on your material.

## Known issues
- Modulation is per track: an envelope moves all held notes of a chord together (as agreed).
- The sampler's own LFO is separate (it was there first); both work.
- Values update every 1.3 ms; very fast LFOs on some values (a delay's time) can sound stepped.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.7.1
```
Projects saved by 0.8.0 are format 8, and 0.7.x won't open them.
