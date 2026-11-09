# Changelog

What changed in each version of ion, written for the person playing it.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions: HANDOFF §15.

## [Unreleased]

## [0.9.0] - 2026-11-09

### Added (Phase 9: 303-style bass)
- **ACID tracks: a 303-style bass**, on Robin Schmidt's Open303. Choose ACID in the engine picker.
  - **Play it from the OXI like a 303:** a note held into the next one **slides** to it (use a tie, or a note length over 100%); a note at velocity **100 or more is an accent** (a harder, brighter hit). "Accent at" changes the threshold if the OXI's velocities sit differently.
  - **The front panel, in the 303's order:** waveform (saw ↔ square), cutoff, resonance, env mod, decay, accent. Then slide time, tune, fine, bend range and level.
  - The same **amp envelope** as every other instrument, after the voice, wide open by default: laid out under the knobs as on FM and Plaits.
  - Everything can be MIDI-learned (`l`) and modulated (`v`), like the rest of ion.
  - A resting ACID track costs no CPU at all.
- **Drive: a new insert effect** for any track. Soft (warm), hard (clipped) or fold (wavefolding), up to 48 dB, with bias (asymmetric, more even harmonics), tone, level and mix. It's oversampled, so heavy settings stay clean rather than gritty.
- **The demo** ("demo v0.9.0"): **track 2 is now ACID** on channel 2 (it was FM BASS 1), through a Drive, with cc 74 on cutoff, cc 71 on resonance, and a slow synced LFO nudging the cutoff.

### Changed (Phase 9)
- Projects are now format 9; 0.8.x can't open projects saved by 0.9.0 (it would lose ACID tracks and Drive inserts).

## [0.8.0] - 2026-11-08

### Added (Phase 8: modulation)
- **Modulation on every track.** Two **LFOs**, two **envelopes**, and **8 routes** that send them to any value on the track: a filter cutoff, a Plaits macro, a pad's tune, the fader, pan, a send, an EQ band, an effect's setting.
  - **LFOs:** sine, triangle, saw up and down, square, sample & hold, smooth random. Free in Hz, or **synced to the OXI** from 4 bars down to 1/32 (with triplets and dotted). They can restart on every note or every bar, from a start phase you choose.
  - **Envelopes:** the same envelope as every instrument, fired by the track's notes: on every note, or only the first note of a phrase (legato).
  - Velocity, the mod wheel, aftertouch, key and a random value per note can drive routes too.
- **`v` on any value, on any screen,** sends LFO 1 to it at 25%: hear it straight away, then go to **Mod** (`O`, in the header between Instr and Mixer) to change the source, depth or LFO.
- **The Mod screen (`O`):** the LFOs and envelopes with their shapes and what they're doing right now; the routes with source, target, depth and where the target has got to. ⏎ picks a target from a list of the track's values (type to narrow it); `[ ]` changes the source; ← → the depth; `x` clears a route.
- **You can see it move:** value bars on Tracks, FX, the drum pads and Plaits show a **lilac marker** where modulation has the value now. The value you set stays put: that's what's saved and what Ctrl+Z goes back to.
- Route depth (and every LFO or envelope setting) can be MIDI-learned (`l`): a modulation amount on an OXI knob.
- **The demo** ("demo v0.8.0"): track 8's Plaits has LFO 1 sweeping timbre over two bars, and envelope 1 opening morph on every note.

### Changed (Phase 8)
- Projects are now format 8 (they keep modulation); 0.7.x can't open projects saved by 0.8.0.
- A new engine on a track drops routes onto the old engine's settings; replacing an effect drops routes onto it; moving an effect takes its routes along. Ctrl+Z puts them back.

## [0.7.1] - 2026-11-08

### Changed
- **Plaits' amp envelope is laid out like FM's:** its shape is drawn ("amp ▁▄█▇▅"), and it comes straight after the macros (↓ from Morph is Attack), beside the low-pass gate. The voice settings and the decay envelope's amounts sit below. Everything fits on an 80 × 24 screen now without scrolling.

## [0.7.0] - 2026-11-08

### Added (Phase 7: Plaits)
- **Plaits tracks.** Choose PLTS for any track: Mutable Instruments' Plaits macro-oscillator, all **24 models** of firmware 1.2, ported from Émilie Gillet's code and tuned to a true 48 kHz.
  - **Polyphonic:** up to 8 voices (6 by default), each a whole Plaits. 1 voice is mono.
  - **The model grid** (`I` on a PLTS track): green (the originals), red (noise, strings, drums) and orange (1.2: VA + filter, phase distortion, three 6-op FM, wave terrain, string machine, chiptune). ↑ ↓ ← → moves and the model changes as you move, so you can play while you browse.
  - **Harmonics, Timbre, Morph** are described for the model you're on ("chord · chorus and filter · waveform" on String machine). On the chord models it shows the chord, on the 6-op models the patch.
  - **The low-pass gate**, three ways: **gate** (opens while the key is held, so notes sustain; the default), **ping** (struck, dies away by itself: the classic Plaits pluck), **off** (the raw model, shaped by the amp envelope). LPG decay and colour as on the module.
  - **Velocity** opens the gate further and strikes the drums and strings harder. The **decay envelope** can reach pitch, timbre and morph (the module's attenuverters). Out ↔ aux, glide, bend range, and the same amp envelope as every other instrument, after the gate.
  - **6-op FM a plays your FM library:** `f` fills its list from your FM favourites (up to 32), and Harmonics picks among them. b and c play Plaits' own banks.
  - **Presets:** `a` saves the track's Plaits settings (and its patch list) by name, `o` opens them.
  - Every setting can be MIDI-learned (`l`), like everything else.
- **Undo and redo, everywhere.** **Ctrl+Z** undoes and **Ctrl+Y** redoes, on any screen, even in piano mode. It covers anything that changes the project: values, patches, samples, kits, engines, effects, CC mappings, the program list, presets, and what a take did. Holding ← → on one value is a single undo. Moves of the OXI's knobs aren't undone (that's playing, not editing). The status line says what was undone. Up to 256 steps; loading a project starts afresh.
- **The mouse**, Midnight Commander style. Click to select (tracks, rows, pads, models, files), double-click to open or load, scroll the wheel over a list to move through it or over a value to change it, and drag a value (or a mixer fader, up and down) to set it. Click the screen names at the top, or any key in the hint bar at the bottom. **↶ undo ↷ redo** in the header are buttons. Clicking a drum pad plays it. Everything still works from the keyboard. To select text in the terminal while ion is running, hold Shift.
- **The demo project** ("demo v0.7.0") adds **track 8: Plaits on String machine** on channel 8, with CCs 20, 21 and 22 on Harmonics, Timbre and Morph, and the DX7's first factory bank as its 6-op list.

### Changed (Phase 7)
- Ctrl+Z no longer suspends ion to the shell; it's undo.
- The Rec screen's `u` is gone: Ctrl+Z undoes a take like anything else, and **the recording now stays** in the project's recordings (it used to be deleted).
- Projects are now format 7 (they can hold Plaits tracks); 0.6.x can't open projects saved by 0.7.0.
- The Library screen counts your Plaits presets and the project's recordings.

### Known issues (Phase 7)
- Plaits presets open from the Plaits screen only (not the Library screen yet).
- The 6-op list fills from favourites as a whole; there's no picking patches one by one yet.
- Model descriptions paraphrase the Plaits manual; a few may read oddly on first sight.

## [0.6.0] - 2026-11-07

### Added (Phase 6: resampling)
- **The Rec screen (`R`)** records ion's own sound back into a sample: the master mix, or any tracks you pick.
  - **Starts on the next bar** of the OXI's clock and **stops exactly on the downbeat**, so a loop is the right length with nothing to trim. 1, 2, 4, 8 or 16 bars, or "until stopped" (it still ends on a downbeat).
  - No clock? Set **start** to "now": `r` starts and stops it.
  - **Tracks** are recorded as you hear them: fader, pan and inserts, even if they're muted or another track is soloed. Or **pre-fx**, straight from the engine. The reverb and delay returns are only in a recording of the master.
  - While it records you see the bars fill and the waveform draw. `r` stops on the next downbeat; Esc throws the take away. The header shows `● REC 3/4` on every screen.
- **Where it goes:**
  - **library:** the project's `recordings/` folder. The sample browser lists it at the top ("recordings (this project)").
  - **sampler → a track:** it plays from C3, looped. The track becomes a sampler if it wasn't one.
  - **drum pads → a track:** cut into slices **on chord changes**, **at transients**, or 4, 8 or 16 equal.
    - You see the slices before anything changes: ↑↓ picks one (and plays it), ← → nudges it (1 ms, Shift 10 ms), `a` splits, `x` merges, Space plays.
    - ⏎ puts them on the pads from the one you chose, one-shot, in a choke group of their own. Esc keeps just the recording.
- **After a take** the tracks you recorded are muted (switch it off under "after"), unless a track is playing its own recording.
- **`u` undoes the last take:** its files go, and the tracks it changed are back as they were.
- **Every recording carries its tempo, bars and sources** in a `.json` beside it, plus the settings of the tracks that made it. In the sampler it shows its bars.
- **The demo project** ("demo v0.6.0") has a take already made: track 6's chord loop, resampled onto **pads 1–4 of track 7** (channel 7, C1–D#1). The Rec screen is set up for one of your own: the piano and strings on channel 1, four bars, onto pads 5–8 of track 7.

### Changed (Phase 6)
- Projects are now format 6 (they keep the Rec setup); 0.5.x can't open projects saved by 0.6.0.
- Save as also copies the project's recordings.

### Known issues (Phase 6)
- Chord-change slicing is tuned on synthesised chords. Real pads with lots of chorus or movement may need nudging; tell me how it does.
- No audio input recording yet (designed for, not built).
- Undo covers the last take only, and not after you switch project.

## [0.5.0] - 2026-11-07

### Added (Phase 5: sampler)
- **Sampler tracks.** Choose SMPL for any track and pick a sample (the browser opens by itself). It plays chromatically from its root note (C3: as recorded), across a key range.
  - **Play:** start point, loop off / forward / ping-pong, loop start and end, loop crossfade, tune, glide, voices (1 = mono), level.
  - **Amp and Filter:** ADSR envelopes. The filter is LP12, LP24, HP or BP, with resonance, key tracking, envelope amount and velocity.
  - **Mod:** an LFO (sine, triangle, square, sample & hold), free or synced to the clock, onto pitch, cutoff, pan and level.
  - **The waveform** shows the loop and the start point.
    - Recordings that know their tempo get a bar ruler.
    - `z` zooms round the point you're moving, and points snap to zero crossings so loops don't click.
    - `n` normalizes; `o` opens another file.
  - Every setting is MIDI-learnable.
- Space on a sampler track plays its root note.

### Changed (Phase 5)
- **Every instrument has the same envelope** (attack, hold, decay, sustain, release, plus velocity → amp): FM, drum pads, the sampler, and Plaits when it arrives. Same names, same ranges, same picture; learn it once and one OXI knob does the same on every track.
  - **FM:** it sits after the patch and starts wide open, so patches sound exactly as before until you use it. It can make a patch shorter or softer, but not longer.
  - **FM's Attack and Release macros are gone**, replaced by it. Bright, Feedback, Detune and Velocity stay. Any Attack/Release macro settings and their CC mappings in older projects are dropped.
  - **Drum pads:** hold and decay are now attack, hold, decay, sustain and release. Your 0.3/0.4 kits and projects convert exactly (a gate pad's decay becomes its release).
- Projects are now format 5 (sampler tracks, the envelope); 0.4.x can't open projects saved by 0.5.0. Kits are saved as version 2; 0.4.x won't read their new envelope settings.

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
