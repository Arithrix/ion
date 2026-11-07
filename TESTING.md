# Testing ion 0.1.0 — Phase 1: tracks, mixer, projects

This checkpoint should show:
- the OXI's tracks route to ion's tracks;
- the mixer and EQ work;
- projects save and come back;
- you can play everything from the laptop keyboard with the OXI unplugged.

Allow about 30 minutes. **Wired headphones or speakers please**: Bluetooth adds 150–250 ms of delay.

## 0. Before you start
1. If you haven't logged out and back in since joining the `pipewire` group, do that now.
2. Run `ion --version`. It should print `ion 0.1.0 (…)`.
3. The demo project was installed with this release. Open it:
   ```
   ion --project "demo v0.1.0"
   ```
   - In the header, `no RT` must **not** appear. If it does, real-time priority isn't active; tell me.

## 1. The demo, from the OXI (5 min)
The demo has four FM tracks: E.PIANO 1 on channel 1, BASS 1 on 2, BRASS 1 on 3, and STRINGS 1 on track 4, **also on channel 1**.
1. Set OXI track 1 to MIDI channel 1 and play.
   - You should hear piano with strings underneath (the layer).
   - Tracks 1 and 4 both light up in the track list.
2. Set OXI track 2 to channel 2 and play: bass. Set OXI track 3 to channel 3: brass.
3. Start the OXI's transport.
   - The header should show `▶ 124.0 ext clock` (or whatever tempo you're at), steady to the decimal.
   - Stop the transport: the ▶ changes to ■.
4. Two CC mappings are already in the demo:
   - an OXI CC lane or knob sending **CC 74 on channel 1** sweeps the piano's presence EQ (watch Mixer → track 1 → bell 1);
   - **CC 7 on channel 2** rides the bass fader.

## 2. Mixer and EQ (8 min)
1. Press `M`. Use ← → to pick a channel and ↑ ↓ to move its fader; Shift moves it faster. `[` `]` pan.
2. `m` mutes the selected track, and `s` solos it: everything else goes quiet, solo-in-place. Press again to undo.
3. Press Tab to reach the EQ:
   - ↑ ↓ picks a band, ⏎ moves between on/freq/gain/Q, and ← → changes the value.
   - Sweep bell 1's gain and frequency while playing. It should be smooth, with **no clicks or zipper noise**.
4. Press `b` to bypass the EQ and compare. Switching should also be click-free.
5. Turn on the 24 dB high-pass and drag it up to about 500 Hz on the bass.

## 3. MIDI learn (3 min)
1. On the Mixer, select track 3's fader and press `l`. The bottom line asks you to move a control on the OXI.
2. Move any OXI CC lane or knob. The status line confirms the mapping, and the fader now follows the OXI.
3. Press `,` (Settings) → Tab to **CC map**. Your mapping is listed.
   - ← → changes the top of its range, ⏎ inverts it, and `x` removes it.

## 4. Projects (4 min)
1. Press `W` and save as "test one". The header shows the name; the ● (unsaved) dot disappears.
2. Change something (a fader), then quit with `q`. ion asks what to do; choose `q` (quit anyway).
3. Run `ion` again.
   - It reopens "test one" and **offers to recover** the change.
   - Press `r`: the fader is back where you left it. `w` saves it.
4. Press `L` (Library) and open the demo again with ⏎.
   - It warns about unsaved changes first; press ⏎ again to go ahead.

## 5. Without the OXI: laptop keyboard (5 min)
1. Unplug the OXI. On the Tracks screen select track 1 (`1`) and press `` ` `` (backtick).
   - The status line reads `♪ PIANO · oct 3 · vel 100 · gate 300 ms` and the pane borders change colour.
2. Play: the Z-row is C3 upwards, and the Q-row is an octave higher.
   - `[` `]` change octave, `\` changes velocity.
   - ← → still change the focused value, so you can tweak while playing.
3. Each note plays for 300 ms. Settings → Keyboard changes that.
4. Esc leaves piano mode. Select track 2 (bass) and play again.

## 6. MIDI monitor and panic (2 min)
1. Plug the OXI back in; it reconnects by itself.
2. Press `~`: the last 20 messages scroll by, with the clock tempo on top. Esc closes it.
3. Play a big chord and press `p`: everything stops at once.

## 7. Small screen (2 min)
Make the terminal narrow, under 80 columns (or try `ion --ui-snapshot 60x35`).
- The tabs shorten to `Trk Ins Mix…` and one pane shows at a time; Tab swaps.
- It should stay readable. This is the Comet layout.

## What to send back
- Did the four tracks route correctly, and did the layer on channel 1 work?
- Is the tempo display steady with your OXI? How did timing feel compared with 0.0.1?
- Any clicks when moving faders or the EQ.
- Did recovery bring your change back?
- Anything confusing about the keys. The `?` overlay lists them all.

## Known issues
- FM is the only engine.
- Changing patch while notes are held cuts them off (Phase 2).
- Holding a key in piano mode plays the note once more after about half a second (key repeat; see CHANGELOG).
- The demo needs `~/ion/downloads/dx7/x/sysex/rom1a.syx`. If the release didn't install "demo v0.1.0", that file was missing.

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.0.1   # back to the Phase 0 build
~/projects/ion/scripts/use-version.sh         # list installed versions
```
0.1.0 projects don't affect 0.0.1, which doesn't read projects.

## Extra tools in this release folder
`ion-render` (project + .mid → .wav), `ion-bench`, `fake-oxi` (a pretend OXI for testing without the hardware), `make-demo`.
