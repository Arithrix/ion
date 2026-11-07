# Testing ion 0.0.1 — Phase 0 prototype

This build answers one question: **does the stack feel right?** Can you play a DX7 patch from the OXI with low enough latency and no glitches? The checkpoint is a go/no-go on that.

Allow about 15 minutes.

## Before you start
1. Log out and back in once, if you haven't since being added to the `pipewire` group. That's what gives ion real-time audio priority.
2. Check the version:
   ```
   ion --version
   ```
   It should print `ion 0.0.1 (…)` with no `-dirty`.

## 1. Sound without the OXI (2 min)
1. Run:
   ```
   ion --syx ~/ion/downloads/dx7/x/sysex/rom1a.syx --voice 11
   ```
   The header should read `cpu …  48k · 128 · pipewire`.
   - If it also says **`no RT`**, real-time priority isn't active: log out and back in.
2. Track 1 shows **E.PIANO 1**. Press **Space**: you should hear a short electric piano note, and the meter should move.
3. Press **→** a few times to step through the bank (E.PIANO 1 → GUITAR 1 → GUITAR 2 …), pressing Space on each.
4. Start a YouTube video at the same time. Both should play.

## 2. Play it from the OXI (5 min)
1. Plug the OXI into the laptop over USB.
   - Within a second the bottom line should change from `no OXI found` to its name.
2. Set one OXI track to **MIDI channel 1** and play some notes.
   - You should hear them, and `last:` shows each note as it arrives.
3. Try the FM patches you like: ←/→ while the OXI plays. (Held notes are cut when you change patch; that's a known Phase 0 limitation.)
4. **Latency feel:** play fast repeated notes or a tight rhythm with wired headphones or the speakers. Does it feel immediate? **This is the main thing to report.**
5. **Bluetooth:** switch to your Bluetooth headphones. The sound should move there by itself, and the delay will be clearly longer (Bluetooth adds 150–250 ms). Expected; tell me if anything else odd happens, like the headphones' quality dropping or another app losing sound.

## 3. Unplug and replug (1 min)
1. While ion is running, unplug the OXI. The bottom line goes back to `no OXI found`.
2. Plug it back in and play. It should reconnect within a second, without restarting ion.

## 4. Stability (5+ min)
1. Leave the OXI playing a busy pattern on channel 1 for five minutes or more.
2. Watch the header:
   - a red **`N dropouts`** means audio glitched (note roughly when, and what else was going on);
   - **cpu** should stay in the low single digits.
3. Press **p** at some point while notes are playing: everything should stop at once.
4. **q** quits.

## What to send back
- Go / no-go on the feel of the latency (wired, not Bluetooth).
- Any dropouts, and what you were doing at the time.
- Anything that sounded wrong compared with the same patch elsewhere (Dexed, hardware).
- The output of `ion --list-midi` with the OXI plugged in, so I can see how it names itself.

## Known issues in 0.0.1
- Only track 1 makes sound.
- Changing patch cuts held notes.
- MIDI clock isn't used yet.

## Rolling back
There's nothing earlier to go back to yet. From 0.0.2 on, `scripts/use-version.sh 0.0.1` switches `ion` back to this build, and running it with no arguments lists the installed versions.

## Extra tools in this release folder
- `ion-bench`: CPU cost per FM voice.
- `fake-oxi`: pretends to be an OXI on the ALSA sequencer, if you want to test without the hardware.
