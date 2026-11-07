# ion — terminal sound module for the OXI One MK2

**Claude Code handoff · November 2026 · owner: Michael**

`ion` is a working name. It is a standalone, keyboard-driven sound module that runs in a Linux terminal. Michael's OXI One MK2 sequencer plays it over USB MIDI. It runs on his laptop now and on his Mecha Comet handheld once that ships.

---

## 0. Read this first: how we work

1. **Do not start building when you receive this.** Read the whole document and the mockups in `mockups/`. Then reply with:
   - a short `PLAN.md` covering phases, tasks, risks and your estimate of where the hard parts are;
   - every point where you disagree with this document, and why;
   - the open questions in §14 that block Phase 0, plus any new ones.
   
   Wait for Michael's go-ahead.
2. **Be direct.** Michael prefers honest pushback to agreement. If a choice in this doc is wrong, say so and propose the fix.
3. **Checkpoints.** Each phase in §12 ends with a checkpoint. Stop, write a short note (what works, what to try with the OXI, known issues, what's next) and wait for feedback before the next phase. Within a phase, work autonomously.
4. **Keep state on disk.** Maintain `STATUS.md` (current phase, done, next, blockers) and `DECISIONS.md` (choices made and why), so you can resume cleanly after interruptions.
5. **Git and versions:** small commits with clear messages; never rewrite history on `main`. Every checkpoint ships a numbered, compiled build Michael can run. Follow §15 exactly.
6. **Verify, don't assume.** Repository URLs, licenses and file paths in this document are from research and memory. Confirm each one at the source before vendoring anything. Record what you confirmed in `DECISIONS.md`.

---

## 1. What it is

A multitimbral sound module with eight tracks. Each track holds one of four engines and receives MIDI on its own channel from the OXI.

| Engine | What it is | Reference |
|---|---|---|
| **Drum** | 16-pad drum sampler | TAL-Drum (feature reference only) |
| **Sampler** | Keymapped single-sample instrument with loop, envelope and filter | TAL-Sampler (feature reference only) |
| **FM** | DX7-patch-compatible 6-operator FM, simplified front panel | Dexed, via Synth_Dexed |
| **Plaits** | Port of the Mutable Instruments Plaits macro-oscillator, all models, polyphonic | Plaits firmware source |

Around the engines:
- a **mixer** with per-channel EQ, faders, pan, mute/solo and meters;
- **effects**: compressor, chorus, delay and reverb;
- **resampling**: record any track or the mix into a new loop, then load it into the sampler or slice it onto drum pads;
- a **flat library**: every FM patch, sample, kit and recording browsable in one place, with no banks.

### Non-goals (do not build)
- **No sequencer.** The OXI is the sequencer.
- **No plugin hosting** (VST, LV2, CLAP). It's a standalone instrument, now and probably always.
- **No GUI toolkit.** Terminal only.

### Later, not v1 (but design so they slot in)
- Time-stretching (§7.6).
- External audio input for recording (open question).
- Multi-zone, multi-sample sampler instruments; SFZ import via sfizz.
- More synth engines.

---

## 2. Hardware and environment

### Development machine: the laptop
- ThinkPad X1 Carbon Gen 10 (i7-1270P, 32 GB). **Settled:** Claude Code runs on this laptop, under **MX Linux** (Debian-based, Xfce, SysVinit — no systemd).
- Michael knows PipeWire and JACK configuration well.
- **Real-time audio setup:** check that the user is in the `audio` group with real-time priority and memlock limits (`/etc/security/limits.d/`). If not, tell Michael the exact change; don't make system changes yourself without asking.

### Controller: OXI One MK2
- Connects over USB as a class-compliant MIDI device; the laptop is the USB host.
- Sends notes on several channels, CCs from its CC lanes and LFOs, MIDI clock and start/stop.
- Michael picks sounds by hand, so program change support is low priority but should exist (§7.3).

### Second target: Mecha Comet (when it ships)
Published specs:
- NXP i.MX 8M Plus, **4 × Cortex-A53 at 1.8 GHz**; an i.MX 95 variant ships later. Michael's variant is unknown.
- 1080 × 1240 touch display, physically small. Plan for roughly **60 × 35 character cells** at a readable font size.
- 3.5 mm headphone jack, 2 × USB-C (both can take power), HDMI.
- Debian-based OS ("Mechanix").
- Keyboard extension. Michael will control ion with this: **no F-keys** assumed.

**Implications:**
- Build for **aarch64 from day one in CI**, even before the device arrives.
- Keep DSP cheap: per-voice cost matters, and the A53 is in-order with modest NEON.
- One USB-C port powers the device, the other takes the OXI.
- Treat these specs as "verify on arrival."

---

## 3. Recommended stack

The choices below are recommendations; push back in your plan if you'd choose differently.

| Concern | Choice | Why |
|---|---|---|
| Language | **C++20**, CMake | Synth_Dexed, Plaits, DaisySP and Airwindows are all C++ and drop in directly |
| Audio I/O | **miniaudio** (or RtAudio) | One API over JACK (and PipeWire's JACK), ALSA and Pulse. On the Comet, use raw ALSA for lowest latency |
| MIDI I/O | **RtMidi** (ALSA sequencer) or direct ALSA seq | Hot-plug of the OXI must work |
| TUI | **FTXUI** | Component model, truecolor, and a canvas with braille drawing for waveforms, EQ curves and meters. Evaluate notcurses if FTXUI can't deliver |
| WAV I/O | **dr_wav** (dr_libs) | Single header, permissive. Support FLAC via dr_flac |
| Library index | **SQLite** | Thousands of patches and samples with tags, favourites and search |
| Config / project | **toml++** for config, **nlohmann/json** for project files | Human-readable |
| Tests | Catch2 or doctest | Plus an offline render harness (§11) |

**Project license: GPL-3.0.** Synth_Dexed and Dexed are GPL-3, so the app must be GPL-3 if shared. Michael is fine with that. Everything else listed is compatible.

---

## 4. Architecture

```
            ┌───────────── UI thread (FTXUI) ─────────────┐
            │ screens · reads engine snapshots · sends cmds│
            └───────▲──────────────────────────────┬──────┘
                    │ state snapshots (lock-free)   │ commands (SPSC queue)
┌──────────┐  SPSC  ┌──────────────── audio thread (real-time) ───────────────┐
│ MIDI in  ├───────►│ MIDI dispatch → 8 tracks → engine → EQ → inserts →       │
│ thread   │        │ sends → fader/pan → buses → master → out + recorder tap  │
└──────────┘        └───────────────────────┬──────────────────────────────────┘
                                            │ SPSC ring buffers
                              ┌─────────────▼─────────────┐
                              │ disk thread: sample load,  │
                              │ recording write, library   │
                              └────────────────────────────┘
```

### Real-time rules for the audio thread
These are non-negotiable:
- no allocation, locks, file I/O or logging;
- all parameter changes arrive through lock-free queues and are smoothed to avoid zipper noise;
- sample loading happens on the disk thread and hands over finished buffers.

### Headless mode
The engine must run without the UI: `ion --headless --project <name>`. The UI is a client of the engine, so the engine never depends on it. This matters for the Comet and for testing.

### Engine interface
Every engine implements one interface: `noteOn`, `noteOff`, `cc`, `pitchBend`, `setParam`, `render(block)`, `voiceCount`, `serialize`/`deserialize`. Adding synths later must be cheap.

### Audio format
- 48 kHz internal sample rate. Plaits is designed around 48 kHz.
- Float32 throughout, with an internal block size the engines can share. Plaits renders in small fixed blocks (12 samples in the original source — verify), so adapt with an internal block loop.
- **Denormals:** set flush-to-zero / denormals-are-zero on both x86 and ARM.

---

## 5. Tracks and MIDI

- **Eight tracks.** Each holds one engine or is empty. The track ↔ MIDI channel mapping defaults to 1–8 and is configurable; two tracks may share a channel to layer.
- Accept MIDI from the OXI by name, plus optionally all other inputs. Handle hot-plug: if the OXI is unplugged and replugged, reconnect without restarting.
- **Clock:** follow external MIDI clock for tempo, and start/stop. Smooth the tempo estimate so delay times don't wobble. Tempo drives tempo-synced delay, resample start/stop and LFO sync.
- **CC learn:** select any parameter, press `l`, move a control on the OXI, and the CC is mapped. Set a range per mapping. Store mappings per project, with optional global defaults.
- **Panic:** `p` sends all notes off and resets voices.
- **Voice handling:** each engine has a polyphony setting and steals the oldest released voice first.

---

## 6. Instruments

### 6.1 Drum sampler (track type DRUM)
- **16 pads** mapped to notes C1–D#2 (36–51); the mapping is editable.
- **Per pad:** sample, tune (±24 st plus cents), level, pan, decay (amp envelope with hold/decay), filter (LP/HP/BP state-variable filter with cutoff and resonance), drive, start offset, reverse, one-shot or gate mode, velocity→level and velocity→filter amounts.
- **Choke groups** (A–D), e.g. closed hat chokes open hat.
- **Kits** save as JSON referencing sample paths. "Collect" copies samples into the project folder.
- **Loading:** the sample browser is the left pane (Midnight Commander style); ⏎ loads onto the selected pad, and moving the cursor previews the file.

### 6.2 Sampler (track type SAMPLER)
- **v1 is one sample per track, keymapped:** root note, key range, pitch by note (repitch).
- **Playback mode setting from day one.** The values are `Repitch` (v1) and `Stretch`, which is reserved; show it as "later" in the UI.
- **Loop:** off, forward, ping-pong; start, loop start and loop end points with snap to zero crossings; optional crossfade.
- **Envelopes:** amp ADSR and filter envelope.
- **Filter:** SVF LP12/LP24/HP/BP with key tracking.
- **LFO:** shapes sine/triangle/square/sample-and-hold, tempo-syncable, with targets pitch/cutoff/pan/level.
- **Glide.** Velocity → amp and filter.
- **Recordings** carry tempo and bar metadata (§7.6) and show it.

### 6.3 FM synth (track type FM)
This is the most opinionated part. Michael found Dexed too complex and DX7 banks clunky.

**Engine:** Synth_Dexed, an embedded-oriented port of Dexed built on the MSFA engine. MiniDexed runs up to eight instances on Raspberry Pi hardware, which suggests the Comet has headroom (MiniDexed runs bare-metal, so verify under Linux). Patch accuracy matters more than anything else here: existing DX7 patches must sound right.

**No banks in the UI. One flat library:**
- **Import** a folder of `.syx` files recursively. Parse both formats:
  - 32-voice bulk dumps: `F0 43 0n 09 20 00`, 4096 data bytes, checksum, `F7` — 4104 bytes total;
  - single-voice dumps: `F0 43 0n 00 01 1B`, 155 bytes, checksum, `F7` — 163 bytes.
- **Validate checksums.** Log and skip corrupt banks; never crash.
- **De-duplicate** by hashing the unpacked voice parameters *excluding the 10-character name*. Big DX7 archives are mostly duplicates. Keep the first-seen name. Record every source bank, shown as "from cart_17" on the patch.
- **Store in SQLite:** voice data, name, source(s), user tags, favourite flag, last used.
- **Browse:** type-to-filter on name and tags, favourites, rough auto-categories from name keywords (BASS, PIANO/EP, BRASS, STR, ORGAN, BELL…). Expect that to be rough; user tags matter more.
- Moving the cursor loads the patch onto the track **immediately**, so whatever the OXI is playing changes as you scroll. An optional preview note (default C3) is a setting.

**Macros (front panel)** are non-destructive offsets in DX7 parameter space, applied over the stored patch:

| Macro | Effect (clamp all results to valid DX7 ranges) |
|---|---|
| Bright | Offset output level of **modulator** operators (non-carriers for the patch's algorithm) |
| Attack | Scale EG rate 1 of all operators (slower or faster attacks) |
| Release | Scale EG rate 4 of all operators |
| Feedback | Offset the patch feedback (0–7) |
| Detune | Spread operator detune around centre |
| Velocity | Offset key velocity sensitivity of carriers |

All macros are CC-mappable. Tune the exact curves by ear in Phase 2.

**Saving and exchange:**
- **Save as** bakes the macro offsets into a new, valid DX7 voice in the library, so a patch never loses compatibility.
- **Export** any selection as standard 32-voice `.syx` banks that load in Dexed or on hardware.
- **Program change** indexes a **per-project favourites list** (up to 128), not banks. It's low priority, since Michael picks sounds by hand.
- **Full operator editor:** not in v1. Leave room for an "advanced" page later.

**Voices:** polyphony is configurable, default 8, with pitch bend range and mono/legato option.

### 6.4 Plaits (track type PLAITS)
- **Port the Plaits DSP** from Émilie Gillet's `pichenettes/eurorack` repository (MIT-licensed STM32 code). **Verify the license** in the repo.
- **Verify that the public source includes firmware 1.2** (the "orange" bank: classic waveshapes with filter, phase distortion, 6-op FM ×3, wave terrain, string machine, chiptune square voices). At release, the 1.2 source was not yet public. If it's missing or incomplete, implement the two original banks and report back before going further.
- **All models** (up to 24) are selectable from a 3 × 8 grid in the UI.
- **Polyphony** by running N independent Plaits voices (default 6). The original is monophonic (its 6-op mode is 2-voice) only because of the module's chip.
- **Controls:**
  - Harmonics, Timbre and Morph, each CC-mappable;
  - LPG decay and colour;
  - Out/Aux blend;
  - velocity → level and LPG.
  - The UI shows what each macro does **for the selected model** (e.g. String machine: harmonics = chord, timbre = chorus/filter, morph = waveform).
- **6-op FM models read patches from the FM library.** Each Plaits track keeps its own list of up to 32 patches chosen from favourites, and Harmonics selects among them.
- **Presets:** save and load per track.

---

## 7. Mixer, EQ, effects, resampling

### 7.1 Channel strip (×8)
`engine → EQ → 3 insert slots → sends (A: delay, B: reverb) → fader + pan → master`

- **EQ:** HPF (12/24 dB/oct) + 4 bands (low shelf, 2 × bell, high shelf) + LPF. Use RBJ-cookbook biquads with parameter smoothing. Each band has an on/off; the EQ has a bypass for A/B comparison.
- **Fader** −∞…+6 dB. Constant-power pan. Mute and solo (solo-in-place).
- **Meters:** peak and RMS, with peak hold.

### 7.2 Inserts (per track, 3 slots)
- **Compressor:** feed-forward; threshold, ratio, attack, release, knee, makeup; gain-reduction readout.
- **Chorus:** rate, depth, mix, stereo width.
- Delay and reverb also work as inserts, but are normally sends.

### 7.3 Send buses (shared, to save CPU)
- **A — Delay:** tempo-synced divisions (straight, dotted, triplet) or ms; feedback; filter in the feedback path; ping-pong; width.
- **B — Reverb:** Plate / Hall / Room; size, decay, pre-delay, damping, width, modulation, low and high cut.
  - Candidates: DaisySP's ReverbSc (note it lives in DaisySP's LGPL portion), the Clouds/Rings reverb from the Mutable code (MIT), a Dattorro plate, or Airwindows reverbs (MIT).
  - Pick by **sound first, CPU on an A53 second**. Report the choice and numbers at the Phase 4 checkpoint.
- Each return has a fader, pan and its own EQ. Delay can send into reverb.

### 7.4 Master
EQ → compressor (glue) → **safety limiter** (ceiling −0.3 dB) → output. The limiter is an addition not discussed with Michael; it protects ears and speakers. Mention it at the checkpoint.

### 7.5 DSP sources
Prefer proven open-source DSP over writing from scratch:
- **DaisySP** (MIT; LGPL add-on separate);
- **Airwindows** (MIT), which has many high-quality compressors, choruses and reverbs;
- the Mutable code (MIT).

Wrap each so the effect interface stays uniform.

### 7.6 Resampling and recording
This is the feature that frees tracks and enables sample tricks.

- **Source:** any single track (pre- or post-FX), any set of tracks, or the master mix. External audio input is an open question; design the source list so an input can be added.
- **Length:** 1, 2, 4, 8 or 16 bars.
- **Start:** on the next bar from MIDI clock, ending exactly on the downbeat, so loops are the right length with no trimming. Manual start/stop is the fallback when no clock is present.
- **Capture is sample-accurate** inside the engine; there is no latency compensation because it never leaves the engine.
- **Write** WAV 48 kHz float32, plus a JSON sidecar: tempo, bars, sources, timestamp and the source tracks' settings (so a bounced track can be rebuilt).
- **Send to:**
  - library only;
  - **Sampler** → chosen track, with loop points set and root note C3;
  - **Drum** → slices onto chosen pads, in a choke group.
- **Slicing:** 8 or 16 equal, **per chord change** (onset/spectral-flux detection tuned for pads and chords), or at transients. Show slice markers before committing; allow nudging.
- **After recording,** optionally mute the source tracks, saving their settings for later.
- **Undo** last take.

**Time-stretch (later, but plan for it now):**
- The sampler has the playback-mode field, and recordings carry tempo metadata, so stretch is an addition, not a rewrite.
- **Library:** Rubber Band (GPL, compatible).
- **On the Comet,** its higher-quality modes are likely too heavy to run live on several tracks. Plan to re-render a loop at the new tempo in the background when the tempo changes. Live stretching is fine on the laptop.
- **Meanwhile,** chopped loops on drum pads already follow tempo, because the OXI triggers each slice.

---

## 8. User interface

### Principles
- **Midnight Commander's** two-pane, arrow-key model, plus the **Dirtywave M8's** small-screen discipline: arrows, a few modifier keys, values edited in place.
- **Every action is reachable from the keyboard.** No mouse needed; no F-keys required.
- **Bottom hint bar** on every screen shows the keys that work right now: key in bold, label in an inverted cyan block. `?` opens a full help overlay.
- **"Slick" means:**
  - consistent box-drawing panes with titles set into the top border;
  - one accent colour for focus;
  - values shown as bars and numbers;
  - braille-drawn waveforms and EQ curves;
  - smooth meters (UI refresh ≤ 30 fps, meters with decay);
  - nothing flickers. Use double-buffered rendering.
- **The UI never blocks audio.** Sample loads show progress; they never freeze input.

### Keymap
| Keys | Action |
|---|---|
| `T I M F R L` | Screens: Tracks, Instrument (engine of the selected track), Mixer, FX, Rec, Library |
| `,` | Settings |
| `1`–`8` | Select track |
| `↑ ↓ ← →` | Move; on a value, `← →` changes it (Shift = coarse) |
| `Tab` | Switch pane · `Esc` back · `⏎` edit/audition |
| `/` · `*` · `t` | Filter · favourite · tag |
| `m` · `s` | Mute · solo |
| `l` | MIDI learn on the focused parameter |
| `Space` | Audition: play the selected sound or pad |
| `p` | MIDI panic |
| `` ` `` | Piano mode on/off (see below) · `` Shift+` `` MIDI monitor |
| `?` | Help overlay |

### Computer keyboard as piano (Renoise-style)
Michael wants to play any track from the laptop or Comet keyboard, the way Renoise does, so he can test sounds and the whole engine with no OXI plugged in.

**Toggle:** `` ` `` (backtick) switches piano mode on and off. While it's on:
- a clear indicator shows in the status line (`♪ PIANO · oct 3 · vel 100`) and the active pane border changes colour;
- letter and number keys play notes instead of their normal shortcuts;
- arrows, `Tab`, `←→` value changes and `Esc` keep working, so Michael can tweak parameters while playing;
- `Esc` also leaves piano mode.

**Layout** (Renoise's default two-row layout):

```
Upper octave:   2 3   5 6 7   9 0          (black keys)
               Q W E R T Y U I O P         (white keys)  C … E

Lower octave:   S D   G H J   L ;          (black keys)
               Z X C V B N M , . /         (white keys)  C … E
```

- The `Z` row starts at the base octave; the `Q` row starts one octave higher. Each row covers C up to the E above (17 semitones).
- `[` / `]` move the base octave down/up (range 0–8).
- `\` cycles velocity through 64 / 100 / 127.
- **Drum tracks** default to base octave 1, so the `Z` row plays pads 1–16 (C1–D#2) in order.
- Keep the layout in config so it can be remapped (e.g. for non-US keyboards).

**Routing:** piano notes enter the **same MIDI dispatch path** as the OXI, on the selected track's channel. A working piano therefore proves everything from MIDI dispatch onward. It does not test the USB MIDI input itself; that's what the MIDI monitor is for (below).

**The hard part — key releases (read before building):**
- Ordinary terminals send key *presses* and auto-repeat, but **not key releases**. Without releases, a note can't know when to stop.
- **Preferred:** the **kitty keyboard protocol**, which reports key release events. It's supported by terminals such as kitty, foot, WezTerm and Ghostty. Check whether FTXUI can pass these events through; if not, parse the protocol in a thin input layer under FTXUI.
- **Fallback, when release events aren't available** (e.g. Xfce Terminal, MX's default): **gate mode**. Each note plays for a set length (default 300 ms, adjustable), and auto-repeat is ignored rather than retriggering.
- Detect which mode works at start-up and show it in the indicator (`hold` vs `gate 300 ms`).
- Tell Michael at the Phase 1 checkpoint which terminal to use on MX Linux to get true hold-to-sustain, and whether it needs installing.
- On the Comet, verify its terminal at Phase 8.
- **Do not read raw `/dev/input` devices** to get key releases. That needs extra permissions and captures keystrokes meant for other windows.

### MIDI monitor
A small overlay (`` Shift+` `` or from Settings) listing the last ~20 incoming MIDI messages with source, channel, type and value, plus clock tempo and start/stop state. This is how Michael confirms the OXI's input is arriving correctly, as the counterpart to piano mode.

### Colour tokens (truecolor; provide a 256-colour fallback)
| Token | Hex | Use |
|---|---|---|
| bg | `#0F1218` | Background |
| surface | `#141A22` | Waveform and graph wells |
| raised | `#1B222C` | Focused parameter row |
| border | `#39424F` | Pane borders |
| border-subtle | `#2A3140` | Dividers |
| text | `#D8DEE9` | Primary text |
| text-dim | `#8B95A7` | Labels |
| text-faint | `#5C6677` | Empty, disabled |
| accent | `#F0A04B` | Focus, selection bar, active pane border |
| value | `#6CC5D9` | Values, meters, hint labels |
| lilac | `#C3A6FF` | Sampler engine, sends |
| green | `#9BD48A` | Plaits engine |
| record | `#FF7A7A` | Recording state |

Engine colours: DRUM amber, FM cyan, SMPL lilac, PLTS green.

### Layout modes
- **Wide (≥ 110 columns):** two panes side by side, as in the laptop mockups.
- **Narrow (< 80 columns, the Comet):** one pane at a time, with a tab strip for the screens and `Tab` to swap panes. Compact rows; meters shrink to 6 cells.
- **Between 80 and 110:** degrade gracefully. Test at 60×35, 80×24, 130×36 and 200×50.

### Screens
Twelve mockups are in `mockups/` and on Michael's design canvas. They show layout and intent; they are not pixel specs, and a terminal draws waveforms and curves in braille.

| Mockup | Screen |
|---|---|
| `Main` | **Tracks (home):** track list with engine, sound, meter, dB, activity, mute/solo; detail pane for the selected track (macros, EQ summary, inserts, sends, MIDI); master strip; MIDI status line |
| `FMBrowser` | **FM library:** filter, list with favourites/source/algorithm; detail pane with algorithm diagram (box drawing), macros, edited-state line, save/export |
| `DrumKit` | **Drum sampler:** sample file browser with waveform preview; 4×4 pad grid (note, name, choke group, activity); per-pad parameter grid |
| `Sampler` | **Sampler:** waveform with bar ruler and loop markers; Play / Amp / Filter / Mod sections |
| `Resample` | **Record:** setup options; live recording state, bar progress, waveform with slice markers, destination; recent recordings |
| `Mixer` | **Mixer + EQ:** 8 channel strips + delay/reverb returns + master (EQ glyph, sends, pan, meter, fader, M/S); EQ curve and band table for the selected channel |
| `FX` | **Effects:** signal-chain list (track inserts, send buses, master); parameter pane for the selected effect, with a decay visualisation for reverb |
| `Plaits` | **Plaits:** 3 × 8 model grid by bank; macros with per-model descriptions; voices, LPG, out/aux; CC mapping |
| `Library` | **Library + projects:** categories with counts; project list; project detail; FM import summary (banks → voices → unique) |
| `Settings` | **Settings:** audio backend/device/rate/buffer; MIDI input, clock, program change; track ↔ channel map; CC map with learn |
| `CometTracks`, `CometFM` | **Narrow layout** of Tracks and the FM browser |

---

## 9. Files and data

```
~/.config/ion/config.toml        audio, midi, ui settings, global CC defaults
~/.local/share/ion/library.db    SQLite: fm voices, samples index, tags, favourites
~/.local/share/ion/import.log    import results incl. skipped/corrupt files
~/ion/projects/<name>/
    project.json                 tracks, engines, params, mixer, fx, cc map, favourites list
    samples/                     collected samples (optional)
    recordings/                  resampled WAVs + .json sidecars
~/ion/samples/                   default sample root (configurable, multiple roots allowed)
```

- **Project files are versioned:** include a `"format": 1` field and write migration code when the format changes.
- **Autosave** to `project.autosave.json` every few minutes and on clean exit. Offer recovery on start.

---

## 10. Performance budget

**Laptop:**
- 128-frame buffer at 48 kHz (≈ 2.7 ms) with no xruns under a full eight-track project.
- Report CPU per engine voice and per effect.

**Comet:**
- Target a 256-frame buffer.
- The UI should use ≤ 1 core.
- Add a **CPU guard**: when DSP load exceeds a threshold, steal voices, and warn in the status bar.

**General:**
- Build a `ion-bench` tool that renders fixed scenes offline and reports real-time factor per engine and effect. Run it on the laptop and, via CI, on aarch64. The CI numbers aren't meaningful for speed, but they catch build breaks.
- If Michael has a Raspberry Pi 3 or 4 (Cortex-A53/A72), it's a useful stand-in for Comet performance before the device arrives — ask him.

---

## 11. Testing strategy

You need to work without Michael at the keyboard, so make the engine testable offline:

- **`ion-render`:** takes a project plus a MIDI file and renders a WAV. Use it for golden-file tests of every engine and effect, compared within tolerance.
- **DSP unit tests:**
  - no NaN/denormal blowups;
  - silence in → silence out where expected;
  - no clicks on note on/off or parameter jumps (check for discontinuities);
  - EQ curves match the RBJ formulas.
- **FM tests:**
  - parse/export round-trip `.syx` byte-exact;
  - checksum handling;
  - dedupe correctness;
  - macro baking produces valid voices.
  - Find a small, freely redistributable DX7 bank to use as a fixture.
- **MIDI tests** using a virtual ALSA sequencer port: channel routing, clock following, CC learn, hot-plug.
- **UI tests:** snapshot the rendered screen at fixed sizes (FTXUI renders to a string buffer) for the narrow and wide layouts.

---

## 12. Phases and checkpoints

Each phase ends with a **checkpoint**: stop, summarise, wait for Michael.

| Phase | Deliverable | Checkpoint: Michael tries… |
|---|---|---|
| **0 · Prototype gate** (throwaway allowed) | Audio out via miniaudio/JACK; OXI MIDI in; one Synth_Dexed voice playing a patch loaded from a `.syx`; a bare FTXUI shell with a track list. Measure round-trip feel, xruns and CPU. Confirm Plaits 1.2 source availability and all licenses | Plays a DX7 patch from the OXI. **Go/no-go on the stack** |
| **1 · Engine skeleton** | 8 tracks, MIDI routing and channel map, clock following, mixer (faders/pan/mute/solo/meters), channel EQ, project save/load, autosave, headless mode, Tracks + Mixer screens, help overlay, settings, **computer-keyboard piano** and **MIDI monitor** | Routes OXI tracks, mixes, saves/loads; plays tracks from the laptop keyboard with the OXI unplugged |
| **2 · FM + library** | Import/dedupe/SQLite library, browser with filter/favourites/tags, live-load on cursor, macros, save-as, export, favourites list for program change | Browses his DX7 collection, tweaks macros, saves |
| **3 · Drum sampler** | Sample browser with preview, 16 pads, all pad params, choke groups, kits | Builds a garage kit and plays it |
| **4 · Effects** | Compressor, chorus, delay (synced), reverb; insert slots, send buses, master chain with limiter; FX screen | Dials in a mix |
| **5 · Sampler** | Keymapped sampler with loop, envelopes, filter, LFO, mode field reserved for Stretch | Plays a sample chromatically |
| **6 · Resampling** | Clock-synced recording, destinations, slicing (equal / per-chord / transient), mute sources, undo, sidecars | Resamples chords onto pads |
| **7 · Plaits** | Ported engine, all available models, polyphony, per-model UI descriptions, FM-library link for 6-op models | Plays every model |
| **8 · Comet** | aarch64 build, ALSA backend tuning, narrow layout polish, CPU guard, autostart as a service, measured voice limits | Runs it on the Comet |
| **Later** | Time-stretch, audio input recording, multi-zone sampler / SFZ, more synths | — |

Order rationale: Phase 2 comes early because loaded patches give instant, real sounds for testing everything after it.

Each checkpoint is a release: Phase 0 ships as `v0.0.x`, Phase 1 as `v0.1.0`, Phase 2 as `v0.2.0`, and so on through Phase 8 as `v0.8.0` (§15).

---

## 13. Open-source components to evaluate

**Verify every license at the source before vendoring.** Vendor as git submodules or pinned copies with their LICENSE files intact.

| Component | Use | License (verify) | Source |
|---|---|---|---|
| Synth_Dexed | FM engine | GPL-3.0 | codeberg.org/dcoredump/Synth_Dexed |
| MSFA | Underlying DX7 engine | Apache-2.0 | github.com/google/music-synthesizer-for-android |
| Dexed | Reference for behaviour and sysex handling | GPL-3.0 | github.com/asb2m10/dexed |
| MiniDexed | Reference: embedded multi-instance Dexed | GPL-3.0 | github.com/probonopd/MiniDexed |
| Mutable Instruments eurorack | Plaits DSP, possibly Clouds/Rings reverb | MIT (STM32 projects) | github.com/pichenettes/eurorack |
| DaisySP | Oscillators, filters, envelopes, chorus, delay, compressor | MIT (+ separate LGPL repo) | github.com/electro-smith/DaisySP |
| Airwindows | Compressors, chorus, reverbs | MIT | github.com/airwindows/airwindows |
| FTXUI | TUI | MIT | github.com/ArthurSonzogni/FTXUI |
| miniaudio, dr_libs | Audio I/O, WAV/FLAC | Public domain / MIT-0 | github.com/mackron |
| RtMidi | MIDI I/O | MIT-style | github.com/thestk/rtmidi |
| SQLite | Library index | Public domain | sqlite.org |
| toml++, nlohmann/json | Config, projects | MIT | GitHub |
| Rubber Band | Time-stretch (later) | GPL-2.0+ | breakfastquay.com |
| sfizz | SFZ sampler (later) | BSD-2 | github.com/sfztools/sfizz |

---

## 14. Open questions for Michael

**Settled:** Claude Code runs on the ThinkPad under MX Linux.

**Check in Phase 0 (inspect the system first, then confirm with Michael):**
1. **Audio stack:** PipeWire with its JACK shim, or real JACK?
2. **Real-time permissions:** in place, or does Michael need to change limits (§2)?

**Not blocking:**
3. **External audio input** (mic or line) as a recording source in v1, or later?
4. **Name:** keep `ion`, or something else?
5. **Track count:** fixed 8, or allow up to 16?
6. **Comet model:** i.MX 8M Plus or i.MX 95 variant?
7. **Sample collection:** where his sample library and DX7 `.syx` collection live, for default library roots.
8. **Stand-in hardware:** any Raspberry Pi to use for ARM performance testing before the Comet arrives?

---

## 15. Versioning and test builds

Michael must always be able to run a numbered build, know exactly what's in it, and go back to an earlier one.

### Version numbers
- **Semantic versioning, `MAJOR.MINOR.PATCH`,** starting at `0.0.1`.
- **MINOR = phase.** Each phase checkpoint is a minor release: Phase 1 → `0.1.0`, Phase 2 → `0.2.0` … Phase 8 → `0.8.0`.
- **PATCH = fixes** after Michael's feedback on a checkpoint: `0.2.1`, `0.2.2`…
- **`1.0.0`** only when Michael calls it ready for regular use.
- **Pre-releases** for in-phase builds he asks for: `0.3.0-dev.4`.
- The **project file `"format"` number** (§9) is separate from the app version. Bump it only when the file format changes, and ship migration code with it.

### In the code and the app
- **One source of truth:** the version lives in `CMakeLists.txt` (`project(ion VERSION …)`). Generate a header from it at build time containing the version, the short git commit hash, a dirty-tree flag and the build date.
- `ion --version` prints e.g. `ion 0.2.1 (a1b2c3d, 2026-12-04)`. A dirty-tree build shows `-dirty` and is never released.
- The version shows in the help overlay (`?`) and at start-up in the status line.
- Project files record the app version that saved them.

### Git
- **Branch per phase** (`phase/2-fm-library`); merge to `main` at the checkpoint.
- **Annotated tag for every release** (`git tag -a v0.2.0`). The tag message is that version's changelog entry.
- **`CHANGELOG.md`** in Keep a Changelog style: Added / Changed / Fixed / Known issues per version. Write it for Michael, not for developers.

### The compiled build: one command, every checkpoint
Provide `scripts/release.sh <version>`. It must:
1. Refuse to run on a dirty tree or with failing tests.
2. Build **Release** (`-O2`, with debug symbols kept in a separate file for crash reports). Statically link the vendored libraries so `ion` is a **single executable**. System audio and MIDI libraries (ALSA, JACK) load at runtime as usual.
3. Run the full test suite and `ion-bench` against the Release binary.
4. Tag the commit, update `CHANGELOG.md`.
5. **Install** to `~/ion/releases/v0.2.0/` containing the `ion` binary, `CHANGELOG.md` and `TESTING.md`. Point `~/.local/bin/ion` at the newest release via a symlink, so Michael always types `ion`.
6. Keep every previous release. `scripts/use-version.sh 0.1.0` switches the symlink back, for side-by-side comparison or rollback.
7. Also produce a `.tar.gz` of the release folder (handy for copying to the Comet later; that build will be aarch64).

### What Michael gets at each checkpoint
A short message containing:
- the version number and the command to run (`ion`, or `ion --project demo`);
- **`TESTING.md`**: a checklist of what to try with the OXI for this version, written as steps ("set OXI track 2 to channel 2, load a kit, play pads 1–4…"), plus known issues;
- a **demo project** for the new features, installed with the release, so he can hear it working before setting anything up himself;
- how to roll back if something is broken.

### Dev builds
- Day-to-day builds go in `build/` and are never installed over a release.
- If Michael wants to try work in progress mid-phase, cut a `-dev.N` pre-release with the same script, so it's still numbered and reversible.

---

## 16. Glossary
- **OXI One MK2:** Michael's hardware sequencer; the only expected controller.
- **Resampling:** recording ion's own output back into a sample.
- **Send bus:** a shared effect fed by several tracks at different amounts.
- **LPG:** low-pass gate, Plaits' combined filter/VCA "pluck" response.
- **Macro:** a single front-panel control that moves several underlying parameters at once.
