# Changelog

What changed in each version of ion, written for the person playing it.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions: HANDOFF §15.

## [Unreleased]

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
