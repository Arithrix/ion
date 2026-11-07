# Testing ion 0.2.1 — Phase 2: FM library and macros

This checkpoint should show:
- your DX7 collection is in one searchable list, with no banks and no duplicates;
- scrolling loads each patch while the OXI plays, **without cutting notes or clicking**;
- the six macros shape a patch without overwriting it;
- an edited patch can be saved as a new one and exported as a `.syx` that loads in Dexed.

Allow about 30 minutes. **Wired headphones or speakers please**: Bluetooth adds 150–250 ms of delay.

## 0. Before you start
1. Run `ion --version`. It should print `ion 0.2.1 (…)`.
2. Open the demo project, which was installed with this release:
   ```
   ion --project "demo v0.2.1"
   ```
   - In the header, `no RT` must **not** appear. If it does, tell me; it's still unconfirmed from 0.0.1.

## 1. Import the collection (3 min)
Your library starts empty. The DX7 banks I downloaded are in `~/ion/downloads/dx7/x`:
- the Yamaha factory ROMs (ROM1A has E.PIANO 1);
- the big public archives;
- the 33 banks that ship with Dexed.

1. Press `L` (Library), then `i`. The prompt starts at `~/ion/downloads/` and lists what's there.
   - Type `dx`, press **Tab** (→ `dx7/`), press **Tab** again (→ `dx7/x/`), then ⏎.
   - Or clear it and type the whole path: `~/ion/downloads/dx7/x`.
   - A folder that doesn't exist is caught: the prompt says "Not found" and lets you fix it.
2. It takes about a second. The status line should say `1040 files · 1039 banks · 33248 voices · 12876 new · 1 skipped`.
   - The one skipped bank has a bad checksum. It's listed in `~/.local/share/ion/import.log`.
3. Press `i` again with the same folder. This time it should report **0 new**: duplicates are merged.
4. Later you can import anything else the same way: your own `.syx` files, or folders full of them. Sub-folders are searched.

## 2. Browse while the OXI plays (8 min)
1. Press `1` for track 1 (E.PIANO 1), then `I`. This is the FM browser: one list of every unique patch.
2. Start a sequence on OXI track 1 (channel 1) and press ↓ repeatedly. Each move **loads the patch straight away**.
   - Notes already sounding finish with the old patch. Listen for **cut-off notes and clicks**; there should be none, even when you hold ↓ to scroll fast.
   - Track 4 (strings) is layered on channel 1. Turn it down in the Mixer if it's in the way.
3. Press `/` and type `bass`. The list filters as you type. ⏎ finishes typing; Esc clears the filter.
   - The filter matches names, tags, categories and bank names: try `rom1a`, `dexed`, `piano`, `gar`.
4. ← → change the view: `all`, `★ favourites`, `recent`, `program list`, then the rough categories (`bass`, `keys`, `brass`…).
   - Categories come from patch names and are rough by design; tags are better.
5. On a patch you like:
   - press `*` to make it a favourite (★);
   - press `t` and type a few tags, e.g. `garage bass dark`;
   - check that the favourites view and the filter find it.
6. ⏎ (or Space) plays a short C3 of the current patch.
   - To hear a preview every time the cursor moves: Settings (`,`) → Keyboard + project → **FM preview**.
7. On the right: the algorithm drawn as on the DX7 chart, which banks the patch came from (`from rom1a · +8 more`), and its tags.

## 3. Macros (7 min)
1. In the browser press Tab to reach the macros. ↑ ↓ picks one; ← → adjusts it (Shift = bigger steps).
2. Hold a long note or chord on the OXI and move each macro:
   - **Bright**: modulator levels. More bite and harmonics.
   - **Attack**: + is slower, − is snappier.
   - **Release**: + is longer tails. It applies to the next notes you release.
   - **Feedback**: the patch's feedback (grit on most patches).
   - **Detune**: + spreads the operators for a chorus-like width; − pulls them together.
   - **Velocity**: how much your touch changes the level.
   - Changes should be heard **at once, smoothly, with no clicks**. On held notes nothing should jump or restart.
3. The patch pane says `● edited: bright +12, …`. The stored patch is untouched: `u` puts every macro back to 0.
4. Browse to other patches with macros set. They stay where they are, so you can audition the library "brighter". Is that what you want, or should they reset per patch?
5. MIDI-learn a macro: in the macros pane, press `l`, then move an OXI knob or CC lane. The demo already has **CC 74 on channel 1 → piano Bright** and **CC 71 → Release**. Try those first.
6. The Tracks screen (`T`) also shows the macros for the selected track, under the patch. ◀ ▶ on the patch row steps through the browser's list.

## 4. Save as, export (5 min)
1. With some macros set, press `a` and name the result, e.g. `DARK EP` (10 characters).
   - The macros go back to 0, and the patch sounds **exactly** the same: the edits are baked in.
   - It's in the library now with `from saved`. Filter for it.
2. Make a filter or view you'd like as a bank, e.g. `★ favourites`, then press `e` and give it a name.
   - The files land in `~/ion/exports/` (`name.syx`, or `name 1.syx`, `name 2.syx` … for more than 32).
3. If you have Dexed (or a DX7), load the exported `.syx`. The patches, including `DARK EP`, should sound the same as in ion.

## 5. Program change (3 min)
The demo has an 8-entry program list: E.PIANO 1, BASS 1, BRASS 1, STRINGS 1, PIANO 1, VIBE 1, HARPSICH 1, TUB BELLS.
1. From the OXI, send program change 3 on channel 2. The bass track becomes BRASS 1, and the status line says so. Program change 2 brings BASS 1 back.
2. Program changes on channel 1 change **both** tracks 1 and 4 (they share the channel).
3. In the browser, `+` adds or removes the current patch; the `pc` column shows its number. In the `program list` view, Shift+↑ ↓ reorders.
4. Settings → MIDI + clock → **program change** turns it off.

## 6. Library screen and small screen (2 min)
1. `L`: Projects lists each project's tracks. FM patches shows counts per view and the last import (`… banks → … voices → … new`).
2. Narrow the terminal below 80 columns (or `ion --ui-snapshot 60x35 --screen instrument`). The browser becomes the Comet layout: list with the macros in one line, Tab for the macros pane.

## What to send back
- Did any note get cut off, or any click happen, while scrolling or moving macros?
- The macro ranges: too much, too little, which feel useless? (I'll tune them by ear with you.)
- Should macros stay put when you change patch (now), or reset?
- Do the categories help at all, or is the filter plus tags enough?
- Did the exported bank load in Dexed and sound right?
- From earlier: is `no RT` gone, how does the latency feel, and what does `ion --list-midi` print with the OXI connected?

## Known issues
- FM is the only engine (Drum is Phase 3).
- Near-identical patches stay separate: six different "E.PIANO 1" edits are six entries, because only exact duplicates are merged.
- A patch's "from" names the first bank it was found in by path order, not necessarily the factory one.
- A program change on a channel shared by two tracks loads on both; the status line names only the last one.
- Holding a key in piano mode plays the note once more after about half a second (key repeat).

## Rolling back
```
~/projects/ion/scripts/use-version.sh 0.1.0   # back to Phase 1
~/projects/ion/scripts/use-version.sh         # list installed versions
```
Projects saved by 0.2.0 are format 2, and **0.1.0 won't open them** ("saved by a newer ion"). It refuses rather than silently dropping the macros and program list. Projects from 0.1.0 open fine in 0.2.0. The library (`~/.local/share/ion/library.db`) is only used by 0.2.0 and later.

## Extra tools in this release folder
`ion-render` (project + .mid → .wav), `ion-bench`, `fake-oxi` (a pretend OXI; `--programs N` sends program changes), `make-demo`.
