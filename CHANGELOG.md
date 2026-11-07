# Changelog

What changed in each version of ion, written for the person playing it.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions: HANDOFF §15.

## [Unreleased]

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
