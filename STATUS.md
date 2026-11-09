# Status

**Phase:** 10 done · 0.10.0 (chip synth) released. Michael asked (2026-11-08, overnight) for Phases 9 and 10, then a full test sweep and an efficiency pass, without stopping.
**Branch:** `main`
**Updated:** 2026-11-08

## Done (Phase 10)
- **CHIP** (D-61): ion's own lo-fi wave synth after the M8's WavSynth idea (its manual read for the idea; nothing copied). Table of 4–256 steps from 7 shapes plus two NES-style noises; mult, warp, mirror, amp into clip/sine/fold/wrap, 1–8 bits; read without interpolation. Per-voice SVF (new shared `dsp/svf.h`), uniform amp envelope, track-wide sample-rate reduction, 8 voices, glide, tune (modulatable). 4.2 µs per buffer for six voices.
- **Screen:** the wave drawn live; settings laid out like the other instruments.
- **Demo:** track 3 is CHIP with an envelope on tune.
- **Tests:** 188 cases (CHIP: tuning, the table's size/bits/mult/warp/mirror/limits, tonal vs plain noise, the rate hold, voices, release, every setting at its extremes; snapshots at three sizes).
- Project format 10.

## Done (Phase 9)
- **ACID** (D-60): Open303 vendored (MIT, verified at source) with four local changes (allocation-free note list, settable accent threshold, slide-back tuning fix, includes). Mono; overlapping notes slide; velocity ≥ threshold accents. The 303 front panel, playing settings, and the uniform amp envelope. Sleeps after 200 ms of silence. ACID screen laid out like FM/Plaits.
- **Drive insert** (D-60): soft/hard/fold, bias, tone, level, mix; 2× polyphase half-band oversampling + first-order ADAA (aliases 40 dB under the harmonics); 17 µs per buffer (3.6× faster than the first, naive version, same output).
- **Demo:** track 2 is ACID through a Drive.
- **Tests:** 183 cases: pitch, slides (and the tuning bug), accents, the threshold, sleep, every setting at its extremes; Drive's transparency, bounds, DC and aliasing; ACID screen snapshots at three sizes.
- **ion-bench:** ACID 15 µs per buffer; Drive 17 µs.
- Project format 9.

## Done (Phase 8)
- **Modulation** (D-58, Michael's request; per track by his choice):
  - per track: 2 LFOs (7 shapes; free or synced, locked to the bar; restart on note/bar; phase), 2 envelopes (uniform AHDSR; every note or legato), velocity, mod wheel, aftertouch, key, random per note;
  - 8 routes per track onto any continuous value on the track, applied in the Module each block over the value set; note-ons update modulation before the engine hears them;
  - values in files and undo like any other (and CC-learnable); route targets in the track, kept valid through engine and insert changes;
  - project format 8.
- **UI:** the Mod screen (`O`) with live sources and routes, a filterable target picker; `v` on any value; lilac live markers on value bars (Tracks, FX, drum pads, Plaits).
- **Demo:** track 8's Plaits has an LFO on timbre and an envelope on morph.
- **ion-bench:** modulation costs 2.1 µs per buffer with all 64 routes running.
- **Tests:** 177 cases, all passing (normal, Release, ASan + UBSan).
  - LFO shapes, rate, phase, sync, restarts; envelopes on every note and legato, release on the last note; sources.
  - The Module moving a target over the value set and letting go; an envelope on a Plaits macro fired by notes; CC on route depth.
  - Save/load, undo, engine and insert changes dropping, moving and restoring routes; keys and labels.
  - UI: `v` on a value (and refused on a switch), editing a route, the target picker; snapshots of the Mod screen at three sizes and its picker.

## Done (Phase 7)
- **Plaits** (D-56, D-57):
  - Plaits 1.2 vendored (eurorack `f4cfa76`, stmlib `e3bd7c9`, MIT), sample rate patched to a true 48 kHz (A4 = 440.00 Hz, tested);
  - ion's own voice, adapted from upstream `voice.cc`: float out, no trigger delay, LPG gate / ping / off;
  - 1–8 voices (default 6), the uniform amp envelope after the LPG, velocity → accent and LPG, decay envelope → pitch/timbre/morph, out ↔ aux, glide, bend;
  - the 6-op FM a model plays a per-track list from the FM library (`f` fills it from favourites); b and c play Plaits' banks;
  - presets in `~/ion/presets/plaits/`; project format 7.
  - Plaits screen: the 3 × 8 grid by bank, per-model macro descriptions, the chord or 6-op patch Harmonics is on, the voice/LPG/envelope rows, CCs from the OXI; one pane at a time below 110 columns.
  - `ion-bench`: 6 voices cost 0.7–6% of a 128-frame buffer (2-op FM the dearest at 161 µs).
- **Undo and redo** (D-54, Michael's request): every project edit is a step; Ctrl+Z / Ctrl+Y and `↶ ↷` in the header; repeated presses on a value merge; CC moves aren't steps and undo leaves them alone; 256 deep. Replaces the take-only undo (a take's recording now stays).
- **Mouse** (D-55, Michael's request): screens mark targets as they draw, and clicks, double-clicks, the wheel and drags become the keys they stand for. Tabs, hints, ↶ ↷, rows, values, pads, strips, models, browsers, the take's waveform. Checked in a real pty (SGR mouse): a click on a tab switches screen; Ctrl+Z undoes and doesn't suspend.
- **Demo:** track 8 is Plaits on String machine (ch 8, cc 20–22 on the macros, ROM1A as its 6-op list).
- **Tests:** 168 cases. All pass in the normal and Release builds, and under ASan + UBSan (nothing from the Plaits code; Synth_Dexed's own upstream UB warnings as before).
  - Plaits: every model plays, finite and bounded; tuning; the three LPG modes and voice freeing; polyphony, stealing, restrike, fewer voices; velocity and the amp envelope; the bank hand-over; a cost guard; project round trip with the patch list; presets; undo of presets and the list; format 6 still opens.
  - Undo: merging, switches, redo cleared by a new edit, engine changes with their patch and settings, inserts and moves, the program list, CC moves left alone, headless edits, loading clears; a take's two steps (slices, then mutes) and redo.
  - Mouse: tabs, row select, drag on the fader (one undo step), double-click opening the engine picker, a hint as a button, the wheel on a value; Ctrl+Z / Ctrl+Y and the ↶ ↷ buttons.
  - Snapshots of the Plaits screen at 130×36, 80×24 and 60×35 (models and params), a 6-op model, and Tracks with a Plaits track.

## Done (Phase 6)
- **Recorder in the Module** (D-51):
  - takes of the master or any set of tracks (post-fx with fader and pan, mute ignored; or pre-fx dry);
  - synced: armed until the downbeat tick, starting at its exact frame; the master read through the limiter's lookahead so it lines up too;
  - 1–16 bars or until stopped; "now" without clock;
  - trimmed to whole bars by a least-squares fit through every tick of the take (D-09 refined);
  - the transport stopping keeps whole bars; `r` mid-take stops on the next downbeat.
- **Slicing** (D-52): equal 4/8/16, chord changes (chroma novelty on the beat grid, relative to the in-chord noise floor), transients (spectral flux, moved back to the attack and a zero crossing).
- **Session** (D-53):
  - WAV + sidecar in `recordings/` (tempo, bars, sources, tap, start bar, slices, where it went, the source tracks' settings);
  - to the library, a sampler (looped from C3), or drum pads after review (nudge, split, merge, audition), in a free choke group;
  - mute sources (not the destination); undo the last take (tracks restored from snapshots, files deleted);
  - project format 6 keeps the Rec setup; save-as copies recordings.
- **Rec screen (`R`):** Setup rows; the take with bars filling and a live waveform; slice markers; recent recordings; `● REC 3/4` in the header everywhere. Wide, 80-column and narrow (Tab between panes) layouts. The sample browser lists the project's recordings.
- **Demo:** make-demo records track 6's chord loop through the real recorder onto pads 1–4 of track 7, and leaves the Rec screen set up for a take of the piano and strings.
- **Tests:** 148 cases.
  - Module: synced start on the exact downbeat frame, tick capture, master alignment, post/pre taps with fader and mute, track selection, "now" lengths, stop on downbeat, clock stop, disarm, cancel.
  - Slicer: equal; chord changes on bars (incl. two chords sharing notes), one chord restruck, a mid-bar change, no tempo; transients within 2 ms.
  - Session: end to end onto pads (trim, sidecar, mute, nudge/add/remove, commit, choke group, undo), into a sampler (several sources, the destination not muted, save-as, undo), arming checks, cancel, free takes, setup save/load.
  - Snapshots of the Rec screen idle at three sizes, recording, and reviewing.

## Done (Phase 5)
- **One envelope for every instrument** (D-50, Michael's call):
  - the same AHDSR and velocity → amp on FM, drum pads and the sampler (amp and filter);
  - FM's Attack/Release macros replaced by it, applied per voice inside Synth_Dexed, wide open by default;
  - 0.3/0.4 drum kits and projects convert exactly.
- **Sampler engine** (D-47):
  - one sample played chromatically from a root note across a key range;
  - loop off, forward (with crossfade) or ping-pong;
  - amp and filter ADSR;
  - SVF LP12/LP24/HP/BP with key tracking, envelope and velocity;
  - LFO (sine, triangle, square, S&H; free or synced) onto pitch, cutoff, pan and level;
  - glide, 1–16 voices, sustain pedal, bend;
  - Repitch mode, with Stretch reserved.
- **Shared sample handling** for Drum and Sampler (D-48). Sampler tracks load, save, go missing and collect like pads.
- **Sampler screen:**
  - waveform with loop region, start/loop markers and zoom round the point being edited;
  - a bar ruler for recordings that know their tempo (D-49), otherwise seconds;
  - Play, Amp, Filter and Mod sections with envelope and LFO shapes and the CCs mapped;
  - points snap to zero crossings; `n` normalizes;
  - `o` opens the browser at the sample's folder, and an empty sampler starts in it.
  - Four sections side by side when wide, two at 80 columns, one at a time on narrow screens.
- **Elsewhere:** Space plays the root note; the engine picker offers SMPL; Tracks shows the sample and its main settings.
- **Tests:** 132 cases.
  - Pitch by note and key range; loops; crossfade against a click; ADSR timing; filter, key tracking and velocity.
  - Synced LFO, glide, voice limit, sample hand-back, extremes.
  - Session round trip; snapshots at 130×36, 80×24 and 60×35.

## Done (Phase 4)
- **Effects** (D-42), written for ion from published designs:
  - a compressor with soft knee and gain-reduction readout;
  - a chorus;
  - a tempo-synced delay with filtered, saturated feedback, ping-pong and width;
  - a Dattorro-tank reverb as plate, hall and room (RT60 within ~7% of the setting);
  - a 1 ms lookahead safety limiter at −0.3 dBFS.
- **Inserts:** 3 slots in every scope, each with a click-free bypass. Reorder and replace from the FX screen (D-43).
- **Sends and returns:** sends A/B per track (pre or post fader), delay and reverb returns with fader, pan, mute and EQ, and a delay → reverb send. Returns are solo-safe (D-44).
- **Master:** EQ → inserts (a glue compressor, switched out by default) → fader → limiter (D-45).
- **Idle strips sleep:** idle CPU live is 1.3% with both returns running (0.3.0: 2.2%; before the fix: 6.3%). Under load, three pretend OXIs into the effects demo: 14% CPU, 0 dropouts without RT.
- **UI:**
  - FX screen: chain, picker, parameters, gain-reduction meters, the reverb's tail drawn from its settings, the synced delay time in ms.
  - Mixer: return strips, send levels, EQ for master and returns, compact at 80 columns.
  - Tracks: inserts and sends.
- **Project format 4** (D-46). Every effect parameter is CC-learnable (`track3.fx1.ratio`).
- **Tools:**
  - `ion-bench` reports each effect: reverb 13–17 µs per 128-frame buffer, the others under 4. (Re-measured at 0.6.0 and 0.7.0: reverb 28–35 µs on the same laptop, the same in 0.5.0, so the first figure was from a quieter machine, not a regression.)
  - The demo adds chorus, compressors, sends, and CC 75/76 mapped to reverb size and delay feedback.
- **Tests:** 119 cases.
  - Compressor curve and timing; chorus; delay timing, sync, ping-pong, glide, stability.
  - Reverb RT60 for every type, and stability under sweeps.
  - Limiter: the ceiling holds, it's transparent below it, the gain is smooth.
  - Routing: sends pre/post, mute, returns, delay → reverb, solo-safety, sleep safety.
  - Session: save/load, keys, reorder, format-3 projects.
  - Snapshots of FX and Mixer.

## Done (Phase 3, released as 0.3.0)
- **Samples:**
  - WAV, AIFF and FLAC are decoded on a disk thread and converted to 48 kHz with libsamplerate (vendored 0.2.2, BSD-2).
  - They're cached, so a sample on several pads is decoded once.
  - Previews coalesce, so scrolling never queues a backlog (D-35).
  - Samples are pinned by the Session until the audio thread hands them back, and a replaced sample finishes its tails first (D-36).
- **Drum engine:**
  - 16 pads, each with 17 CC-mappable settings: note, tune and fine, level, pan, start, reverse, one-shot or gate, hold, decay, filter (LP/HP/BP SVF) with cutoff and resonance, drive, velocity to level and to filter, choke group (D-37).
  - Up to 64 voices, 4 per pad; chokes and steals fade over 5 ms.
  - The shared voice allocator (D-26) arrived with it.
- **Kits** in `~/ion/kits` with every pad setting. **Collect** copies samples into the project, and save-as takes them along (D-38).
- **Project format 3** (D-39). The parameter index grew to 16 bits, so every pad setting is a normal parameter.
- **UI:**
  - Drum screen: browser with preview and waveform, 4×4 pads with activity and choke groups, two-column pad editor. Narrow layout too.
  - Space and piano mode play pads (octave 1 = pads 1–16).
  - Tracks shows the selected pad.
  - Library lists kits.
  - Settings has sample roots, preview and preview level (D-40, D-41).
- **Tools:**
  - `ion-render` waits for samples.
  - `ion-bench` has drum scenes. 16 pads with every voice busy use 3.4% of a 128-frame buffer, or 9.7% with filter and drive.
  - The demo project has a synthesised kit on track 5, collected into the project.
- **Tests:** 103 cases.
  - Loader formats and rates, cache and preview coalescing.
  - Engine: mapping, tune, reverse, start, chokes without clicks, voice caps, gate and hold/decay, velocity, filter response and stability, sample release.
  - Module hand-over; session pads, projects, kits, collect, previews.
  - UI snapshots of the drum screen at 130×36, 80×24 and 60×35, with scripted key presses.
  - Clean under AddressSanitizer and UBSan, apart from Synth_Dexed's own upstream warnings.

## Waiting on Michael
- Try 0.8.0 (modulation) and 0.7.x (Plaits; undo/redo; the mouse).
- Say whether FM without its old Attack/Release macros is OK, or bring them back (D-50; revert is possible).
- Chord-change slicing on real material (D-52).
- Still open from earlier: the reverb by ear (D-42), the 1 ms limiter latency (D-45), drum defaults by ear, real-time priority (`no RT`, V-10), latency feel, `ion --list-midi` with the OXI, macro ranges.

## Next
- **Phase 8 (Comet) is on hold until Michael has the device** (his call, 2026-11-08): aarch64 build, ALSA tuning, narrow-layout polish, CPU guard, autostart, measured voice limits.
- **The efficiency pass, then the full test sweep** (Michael's overnight list), as 0.10.1.

## Known gaps carried forward
- Recording: no audio input yet; a take's source settings are saved in its sidecar but there's no "bring the sources back" command yet; editing the Rec setup doesn't mark the project unsaved.
- Global default CC mappings (D-26).
- Near-duplicate FM patches aren't folded together (D-30).
- No samples index in library.db yet; the browser reads folders (D-35).
- Pads can't be copied or renamed yet; a pad's name is its file name.
- No effect presets yet (the FX mockup's "Preset"). Plaits has presets now (D-57); effects could use the same `o` / `a`.
- Plaits: presets only open from the Plaits screen; the 6-op list fills from favourites as a whole (no picking one patch at a time).
- The Tracks detail pane at 80×24 cuts its last rows (inherited from 0.1; to fix with the narrow-layout polish in Phase 8).
