# Changelog

What changed in each version of ion, written for the person playing it.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions: HANDOFF §15.

## [Unreleased]

## [0.4.0] - 2026-11-07

### Added
- **Effects.** Every track, the master and both send returns have **three insert slots**. The effects:
  - **Compressor:** threshold, ratio, attack, release, knee, makeup, mix. It shows how much it's turning down.
  - **Chorus:** rate, depth, mix, width.
  - **Delay:** synced to the OXI's clock in note values (straight, dotted, triplet), or in ms. Feedback with high and low cut, ping-pong, width.
  - **Reverb:** Plate, Hall or Room; size, decay, pre-delay, damping, width, modulation, low and high cut.
- **The FX screen (`F`):** the selected track's chain (EQ, slots, sends), the send buses, and the master (EQ, slots, limiter).
  - ⏎ or `a` adds an effect, `x` removes one, `b` bypasses it, and `[ ]` moves it up or down.
  - Tab edits its settings. The reverb's tail is drawn as you change it, and a synced delay shows its time in ms.
- **Send buses.** Every track has Send A (to the delay) and Send B (to the reverb), after the fader by default (or before it, one switch per track).
  - The returns have their own fader, pan, mute and EQ in the Mixer.
  - The delay can send into the reverb.
  - Soloing a track never mutes the returns.
- **Master chain:** EQ, then your inserts, then the fader, then a **safety limiter** that never lets anything past −0.3 dBFS.
  - Every project has a glue compressor on the master, switched off until you turn it on (`b` on the FX screen).
- **Mixer:** delay and reverb return strips, send levels on every strip, and the EQ for the master and the returns.
- **Tracks** shows each track's inserts and its two sends.
- **Everything is MIDI-learnable** (`l`), including each slot's on/off.
- The demo project ("demo v0.4.0") adds:
  - chorus on the piano;
  - compressors on the bass and the drums;
  - piano, brass and drums sent to the reverb, brass and drums to the delay;
  - CC 75 / CC 76 on channel 1 mapped to the reverb's size and the delay's feedback.

### Changed
- Everything you hear is 1 ms later: the limiter looks ahead so it never clips. Tell me if you feel it.
- Projects are now format 4. Older projects open as before; 0.3.x can't open projects saved by 0.4.0.
- Idle tracks, returns and the master cost almost nothing now. Idle CPU is lower than in 0.3.0, even with both returns running.

### Known issues
- No effect presets yet.
- A big hall can't decay faster than about a second, whatever Decay says.

## [0.3.0] - 2026-11-07

### Added
- **Drum tracks.** Choose DRUM for any track: 16 pads, played on notes C1–D#2 (36–51) of the track's channel.
  - **The drum screen (`I` on a drum track):** the sample browser on the left, the 4×4 pads top right, and the selected pad's settings below.
  - **The browser:**
    - ↑ ↓ moves through folders and files, and **each file plays as you reach it**, with its length and a waveform.
    - ⏎ puts the file on the selected pad; ← goes up a folder; `/` filters.
    - WAV, AIFF and FLAC load at any sample rate, converted to 48 kHz without slowing anything down.
  - **Per pad:** tune (±24 semitones plus fine), level, pan, start point, reverse, one-shot or gate, hold and decay, filter (low-, high- or band-pass) with cutoff and resonance, drive, velocity → level and velocity → filter, and which note plays it.
    - Every one can be MIDI-learned with `l`.
  - **Choke groups A–D (`c`):** a hit stops the other pads in its group, e.g. closed hat stops open hat. A pad in a group of its own is mono.
  - A pad plays up to 4 overlapping hits. Stopping a sound (choke, a fifth hit) fades it in 5 ms instead of cutting it.
  - `x` clears a pad. Space plays the selected pad, and so does ⏎ on the pads.
- **Kits (`k`):** save the 16 pads, samples and settings, as a kit in `~/ion/kits/`. Load one from the browser's "drum kits" folder, or from Library → Drum kits.
- **Collect (`g`):** copies every sample the project uses into the project's own `samples/` folder, so it can be moved or backed up whole. Save as takes them along.
- **Piano mode on a drum track** starts at octave 1, so the Z row plays pads 1–16.
- **Tracks** shows the selected pad (◀ ▶ picks another) with its level, tune, decay, pan and cutoff.
- **Settings → Samples:** the folders the browser starts in, preview on/off, preview level.
- The demo project ("demo v0.3.0") adds a small synthesised kit on track 5 (channel 5), with the hats choked together and CC 74 on channel 5 mapped to the open hat's decay.

### Changed
- Projects are now format 3. Projects from 0.1.0 and 0.2.x open as before, but 0.2.x can't open projects saved by 0.3.0.
- The Mixer and Library show a drum track's kit name.

### Known issues
- A pad's name is its file's name, and pads can't be copied yet.
- The browser reads folders as they are; there's no search across your whole sample collection yet.
- Dropouts while the header says `no RT` are expected: they happen with or without drums (measured while idle). Real-time priority fixes them.

## [0.2.1] - 2026-11-07

### Fixed
- **Importing from a folder that doesn't exist failed almost silently.** The error flashed for 3 seconds and nothing was logged, so it looked as if the import had run and found nothing. Now:
  - the folder is checked when you press ⏎, and if it isn't there the prompt stays open, says "Not found", and keeps what you typed so you can fix it;
  - **Tab completes** folder names in the import prompt, and the prompt lists the folders and `.syx` files at the current path;
  - the prompt starts at `~/ion/downloads/`, or at the last folder you imported if it still exists;
  - the result stays on screen for 15 seconds and on the Library screen's FM page. "No .syx files found" is reported as such;
  - every import is written to `~/.local/share/ion/import.log`, failed ones included.

## [0.2.0] - 2026-11-07

### Added
- **The FM library.** One flat list of every DX7 patch you've imported. No banks to manage, and duplicates are merged.
  - Press `i` (Library or Instrument screen) and give it a folder: every `.syx` underneath is imported, sub-folders included, in about a second for a thousand files.
  - Corrupt banks are skipped and listed in `~/.local/share/ion/import.log`.
  - Importing the same folder again adds nothing.
  - Each patch remembers every bank it was found in (`from rom1a · +8 more`).
- **The FM browser (`I` on an FM track).**
  - Moving the cursor loads the patch onto the track **straight away**, while the OXI plays.
  - `/` filters as you type, by name, tag, category or bank. ← → switch between all, ★ favourites, recent, the program list, and rough categories (bass, keys, brass, …).
  - `*` makes a favourite, and `t` tags a patch.
  - The patch pane draws the algorithm as on the DX7 chart.
- **Changing patch never cuts a note.** Notes that are sounding finish with the old patch, and new notes play the new one. Scrolling fast through the list with notes held doesn't click.
- **Six macros** on every FM track: Bright, Attack, Release, Feedback, Detune and Velocity.
  - They adjust the patch without overwriting it, and respond at once and smoothly, even on held notes.
  - They're MIDI-learnable (`l`) and saved with the project. `u` puts them back to 0.
  - They also show on the Tracks screen under the patch.
- **Save as (`a`):** bakes the macros into a new patch in your library, which sounds exactly the same as the edited one.
- **Export (`e`):** writes whatever the browser is listing (a filter, your favourites, a category) as standard 32-voice `.syx` banks in `~/ion/exports/`. They load in Dexed or on a DX7.
- **Program change.** Each project has a program list of up to 128 patches: `+` in the browser adds the current one. Program change N from the OXI loads entry N onto the FM tracks on that channel. It can be turned off in Settings.
- **Library screen:** projects show what's on each track, and FM shows patch counts per category and the last import.
- **Settings:** program change on/off, and **FM preview** (play a short C3 whenever browsing loads a patch).
- `ion --import FOLDER` imports from the command line. `ion --syx FILE` now also adds the bank to the library.
- The demo project ("demo v0.2.0") adds macro edits on the bass, CC 74 / CC 71 on channel 1 mapped to the piano's Bright / Release, and an 8-patch program list.

### Changed
- Projects are now format 2. Format 1 projects from 0.1.0 open as before, but 0.1.0 can't open projects saved by 0.2.0.
- Choosing FM for an empty track goes straight to the browser.

### Fixed
- Synth_Dexed sent held notes straight to their sustain stage whenever a sound parameter changed. Notes still in their attack were cut short, and could go silent while a knob was being turned. ion's copy keeps each note where it is in its envelope.

### Known issues
- FM is the only engine so far (Drum is next, in Phase 3).
- Only exact duplicates are merged: different edits that share a name (there are six "E.PIANO 1"s in the collection) stay separate.
- A program change on a channel shared by two tracks loads on both; the status line names only one.

## [0.1.0] - 2026-11-06

### Added
- **Eight tracks, each on its own MIDI channel** (1–8 by default). Change the mapping in Settings (`,`). Two tracks on the same channel play together, for layering.
- **Any track can be FM.** Press ⏎ on an empty track to choose an engine. Drum, Sampler and Plaits are listed and arrive in later phases.
- **Mixer screen (`M`).** Eight channel strips plus the master, with a fader, constant-power pan, mute, solo, and meters showing RMS, peak and a held peak.
- **Channel EQ on every track:** high-pass (12 or 24 dB/oct), low shelf, two bells, high shelf and low-pass, each switchable, plus an EQ bypass to compare with and without (`b`). The curve is drawn as you edit, and every change glides, so nothing clicks.
- **Projects.**
  - `w` saves, `W` saves under a new name, and the Library screen (`L`) lists your projects and opens them.
  - ion reopens the last project at start-up.
  - Work is autosaved every 2 minutes and whenever you quit. If you quit without saving, or ion crashes, it offers your changes back next time.
- **MIDI learn.** Move to any value, press `l`, and turn a knob or move a CC lane on the OXI. The mapping is saved with the project and listed in Settings, where you can narrow its range or invert it.
- **Follows the OXI's clock.** The tempo shows in the header (`▶ 124.0 ext clock`), with start/stop and bar position. The tempo is steady to within a fraction of a BPM and follows tempo changes within two beats. Delays will sync to it in Phase 4.
- **Tighter MIDI timing.** Notes from the OXI now land where they were played, not at the next buffer boundary. That removes up to 2.7 ms of jitter in exchange for a fixed 2.7 ms delay. You can switch back to "immediate" in Settings.
- **Play from the laptop keyboard (`` ` ``)**, Renoise layout:
  - the Z and Q rows play two octaves;
  - `[` `]` change octave, and `\` steps the velocity through 64, 100 and 127;
  - each note plays for a set time (300 ms, adjustable in Settings);
  - Esc leaves piano mode.
- **MIDI monitor (`~`).** The last 20 messages from the OXI, with channel and value, plus clock tempo and transport.
- **Help (`?`)** lists every key, including the ones for the screen you're on.
- **Settings (`,`)** covers audio, MIDI input, timing, the piano gate, autosave, the track ↔ channel map and the CC map. Changes apply straight away.
- **Instrument screen (`I`).** Browse the bank you started with (`--syx FILE`). Moving the cursor loads each patch as the OXI plays.
- **Works on the Comet's small screen.** Below 80 columns the layout switches to one pane at a time with short tab names.
- **Demo project** "demo v0.1.0", installed with this release.
- **`ion-render`** turns a project and a MIDI file into a WAV without audio hardware. It's mainly for testing.

### Changed
- Screen keys are capitals (`T I M F R L`), so lowercase letters stay free for actions (`m` mute, `s` solo, `l` learn).

### Known issues
- FM is the only engine so far.
- Changing patch while notes are held cuts them off (Phase 2 fixes this).
- Program change and the FM macros arrive in Phase 2. Effects (inserts, sends, master compressor, limiter) arrive in Phase 4.
- Holding a key in piano mode plays the note once more after about half a second, when the keyboard starts repeating. A terminal can't tell that from a second press.
- CC mappings are per project; global default mappings aren't built yet.

## [0.0.1] - 2026-11-06

### Added
- **First playable prototype (Phase 0).** Track 1 is a DX7-compatible FM synth. Load any DX7 `.syx` bank with `ion --syx FILE` and play it from the OXI on MIDI channel 1.
- Step through the 32 voices in the bank with ← and →. Space plays a short C3 so you can hear a patch without the OXI.
- The OXI is found by name and connects by itself, including when you unplug it and plug it back in while ion is running.
- Sound goes out through PipeWire like any other app. It plays alongside YouTube and the rest, follows your default output (including Bluetooth headphones), and asks for a small 128-frame buffer (2.7 ms) without changing your system settings.
- Level meters per track and for the output, a CPU readout, and a count of audio dropouts.
- `p` stops every note at once (panic).
- A safety ceiling at −0.3 dBFS, so nothing louder than full scale reaches your ears or speakers.
- `ion --headless` runs without the screen, `ion --list-midi` shows MIDI inputs, and `ion --version` shows the exact build.

### Known issues
- Only track 1 makes sound; the other seven are placeholders until Phase 1.
- MIDI notes land on 1.3 ms boundaries (64 frames). Sample-accurate timing arrives in Phase 1.
- Changing patch while notes are held cuts them off instantly. Live browsing gets a smooth handover in Phase 2.
- MIDI clock is received but not used yet (Phase 1).
- The other screens in the tab bar (Instr, Mixer, Fx, Rec, Library) aren't built yet.
