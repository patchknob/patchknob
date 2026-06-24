# Porting the "1111" (ScaleJammer) Piano Roll Look & Feel into seq24

Design / research document. **No production code here.** This describes how to
reskin seq24's existing GTK piano-roll widgets (`seqroll`, `seqkeys`, `seqtime`,
`seqdata`, `seqevent`, composed by `seqedit`) to look and behave like the JUCE
`PianoRollComponent` in `C:\Users\lain\Desktop\1111\` while keeping seq24's
absolute-MIDI-pitch `sequence`/`event` data model.

Source files studied (read-only):

- JUCE: `1111/PianoRoll.h`, `1111/PianoRoll.cpp` (~2600 lines), `1111/PluginProcessor.h`, `1111/PluginProcessor.cpp` (tables).
- seq24: `src/seqroll.{h,cpp}`, `src/seqkeys.{h,cpp}`, `src/seqtime.{h,cpp}`, `src/seqdata.{h,cpp}`, `src/seqevent.{h,cpp}`, `src/seqedit.cpp`, `src/sequence.h`, `src/event.h`, `src/globals.h`.

Throughout, "1111" = the JUCE plugin; line citations like `PianoRoll.cpp:685`
refer to that file unless prefixed with a seq24 filename.

---

## 1. Feature inventory of the 1111 piano roll

### 1.1 Overall architecture

1111 is a **single monolithic `juce::Component`** (`PianoRollComponent`) that
draws *everything* itself in one `paint()` (`PianoRoll.cpp:202-232`) and handles
*all* input in one set of mouse/key handlers. There are no child widgets — every
"panel" is just a `juce::Rectangle<int>` computed in `resized()`
(`PianoRoll.cpp:23-147`) and drawn with absolute coordinates. This is the
opposite of seq24, which splits the same surface into 5 separate
`Gtk::DrawingArea` subclasses laid out by a `Gtk::Table` (`seqedit.cpp:171-193`).

The plugin pulls its data from `ChordMidiProcessor` once per timer tick into
**cached display copies** (`dispNotes`, `dispChordEvents`, `dispBarCount`,
`dispPlayhead`, …) in `updateState()` (`PianoRoll.cpp:162-196`), edits the cached
copy, then pushes back via `proc.setRiffNotes()` / `proc.setChordEvents()`.

### 1.2 Visual layout (top to bottom)

Layout constants are in `PianoRoll.h:27-37`; the stacking order is set in
`resized()` (`PianoRoll.cpp:23-62`) and `paint()` (`PianoRoll.cpp:202-232`):

| # | Region | Field | Height const | Drawn by |
|---|--------|-------|-------------|----------|
| 1 | Bar-count selector (16 tiles "1".."16") | `barBtnRow` | `kBarBtnH=20` | `drawBarBtnRow` (532) |
| 2 | Pattern selector `< C4 (61/128) >` + SLICE | `patternRow` | `kPatH=18` | `drawPatternRow` (551) |
| 3 | Chord timeline (chord-change blocks + labels) | `chordTimelineRow` | `kChordH=24` | `drawChordTimeline` (606) |
| 4 | Time ruler (bar numbers, beat lines) | `rulerArea` | `kRulerH=16` | `drawRuler` (656) |
| 5 | **Riff note grid** (the main scale-degree piano roll) | `riffArea` | flexible | `drawRiffPanel` (685) |
| 5a | — vertical scrollbar (octave range) | `riffScrollBar` | `kScrollW=12` | `drawScrollBar` (784) |
| 6 | Horizontal scrollbar | `riffHScroll` | `kHScrollH=8` | `drawHScrollBar` (1006) |
| 7 | Tab bar `KEY/SCALE · VELOCITY · CHORDS` | `tabBar` | `kTabBarH=18` | `drawTabBar` (240) |
| 8 | Tab body (one of three, same rect): | | | |
| 8a | KEY panel (12 chromatic root rows × time) | `scaleArea` | refH | `drawScalePanel` (872) |
| 8b | SCALE panel (12 scale-type rows × time) | `chordRefArea` | refH | `drawChordRefPanel` (1035) |
| 8c | VELOCITY panel (stem bars) | `velPanel` | rem | `drawVelocityPanel` (259) |
| 8d | CHORDS palette (12 chord-type rows × roots, drag-drop) | `chordTabBody` | rem | `drawChordTabPanel` (372) |
| 9 | Control row: ROOT −/+, snap buttons, CLR/LOAD/SAVE/COPY/PASTE, LINK | `ctrlRow` | `kCtrlH=22` | `drawCtrlRow` (1163) |
| — | SLICE → KEYS floating overlay (optional) | `sliceWin` | — | `drawSliceWindow` (480) |
| — | Chord drag ghost (floats during drag) | — | — | `drawChordDragGhost` (455) |

Every panel reserves a **`kKeyW=44` px left "key strip"** so the grids line up
vertically. The grid x-origin is always `panel.getX()+kKeyW` (see `beatToX`,
`PianoRoll.h:122`).

### 1.3 Color palette

Defined as ARGB `uint32` constants in `PianoRoll.h:235-248` (dark "synthwave"
theme):

| Const | Hex | Role |
|-------|-----|------|
| `cBg` | `0xFF0A0A14` | near-black window background |
| `cPanel` | `0xFF161626` | panel fill / even rows |
| `cAccent` | `0xFF7C4DFF` | violet — root rows, selection chrome, active tab |
| `cSel` | `0xFF00E5FF` | cyan — lasso, paste cursor, selected chord marker |
| `cActive` | `0xFF00E676` | green — playhead, "active/playing" |
| `cHi` | `0xFFE8DAEF` | light lavender text |
| `cDim` | `0xFF5A4A7A` | muted violet-grey — inactive |
| `cWhite` | `0xFFE8E8E8` | white text |
| `cBlk` | `0xFF1A1828` | key strip background |
| `cNote` | `0xFF9C6DFF` | note body (violet) |
| `cNoteSel` | `0xFF00E5FF` | selected note (cyan) |
| `cScale` | `0xFF0A1A10` | scale tint |
| `cChordBg` | `0xFF0A0A1E` | chord timeline bg |

Chord-type tint helper `chordTabTint()` (`PianoRoll.cpp:348-354`) maps chord
families to violet / cyan / orange / red.

Notable look details to reproduce:

- **Alternating row stripes** in the grid by scale degree parity:
  `(deg%2==0)?cPanel:cBg` (`PianoRoll.cpp:712`).
- **Octave separator line** every 7 rows at `deg==0` (`PianoRoll.cpp:715-720`).
- **Beat lines** with graded alpha per subdivision (bar 0.9, beat 0.5, half
  0.22, 16th 0.20/0.07) and graded thickness (`drawBeatLines`,
  `PianoRoll.cpp:1256-1282`).
- **Notes**: filled rounded-ish rect, 1px white-alpha outline, a brighter
  **resize-handle hint** strip on the right edge, and an **in-note label** with
  the *resolved absolute* note name (`PianoRoll.cpp:733-758`).
- **Playhead**: 2px green line (`drawPlayhead`, `PianoRoll.cpp:1286-1293`).
- **Key strip** in the riff panel is colored by harmonic role: root = `cAccent`,
  chord-tone = `cAccent@0.55`, else `cDim@0.4` (`drawRiffKeys`,
  `PianoRoll.cpp:809-868`).

### 1.4 Coordinate / zoom / scroll model

- **Horizontal:** continuous `hZoom` = *pixels per beat* (`PianoRoll.h:119`),
  `scrollBeat` = left-edge beat (`PianoRoll.h:120`). `beatToX`/`xToBeat`
  (`PianoRoll.h:122-129`). `refitZoom()` fits the whole loop to the panel width
  (`PianoRoll.cpp:149-156`).
- **Vertical (riff):** rows, not pixels. `visOctaves=4` rows shown of
  `kTotalOcts=9`; `lowestOctave` is the scroll position. Row math in
  `PianoRoll.h:69-87` (`noteToRow`, `rowDegree`, `rowOctave`, `rowToY`,
  `yToRow`, `rowH`). **7 rows per octave** (one per scale degree), bottom-up.
- **Key panel vertical:** separate scroll state `keyVisOcts`/`keyLowest` over
  `kKeyTotalOcts=11`, **12 chromatic rows per octave** (`PianoRoll.h:90-107`).
- **Snap:** `snapDiv` in beats (0.25/0.5/1/2/4), `snapToGrid()` rounds
  (`PianoRoll.h:110-116`; labels `kSnapDivs`/`kSnapLabels`, `PianoRoll.cpp:6-8`).

### 1.5 All interactions

Mouse handling is one big dispatch in `mouseDown` (`PianoRoll.cpp:1342-2025`),
`mouseDrag` (2027-2276), `mouseUp` (2278-2357), `mouseDoubleClick` (2359-2394),
`mouseWheelMove` (2396-2460), `keyPressed` (2466-2596). The `DragOp` enum
(`PianoRoll.h:175`) is the state machine.

**Note grid (riffArea):**

- **Add note:** *double-click* on empty grid (`mouseDoubleClick`,
  `PianoRoll.cpp:2379-2393`); length = current `snapDiv`, degree/octave from
  `yToRow`. (Chord drop also bulk-adds notes, see below.)
- **Delete note:** *double-click* on a note (`PianoRoll.cpp:2364-2377`), or
  Delete/Backspace on selection, or right-click → Delete.
- **Select:** single left-click selects one note; Ctrl+click toggles; clicking an
  already-selected note keeps the whole selection for dragging
  (`PianoRoll.cpp:1827-1877`).
- **Lasso / rubber-band:** left-drag on empty space (`DragOp::Lasso`); selection
  = pre-lasso set ∪ notes intersecting rect; Ctrl preserves prior selection
  (`PianoRoll.cpp:1880-1889`, drag math 2121-2140).
- **Move (single & multi):** left-drag a note; all selected notes move by the same
  beat delta (snapped) and row delta; original positions captured at drag start
  (`PianoRoll.cpp:1861-1875`, apply 2164-2209).
- **Resize:** drag the right-edge handle (within 10px, `isResizeHandle` 1330);
  end snapped to grid; same duration-delta applied to all selected
  (`PianoRoll.cpp:1847-1858`, apply 2240-2275).
- **Velocity edit:** on VELOCITY tab, click/drag sets velocity of selected notes
  (or nearest note) from y-position (`PianoRoll.cpp:1407-1436`, 2211-2238).
- **Copy / Cut / Paste:** Ctrl+C/X/V (`keyPressed` 2478-2547) and a right-click
  popup menu (`PianoRoll.cpp:1735-1823`). Clipboard stores notes relative to the
  earliest beat; paste lands at `pasteCursorBeat` (set by left-click on empty
  grid, `PianoRoll.cpp:1882`) or right-click position.
- **Select-all:** Ctrl+A (`PianoRoll.cpp:2471-2476`).

**Horizontal zoom / scroll:**

- **Mouse wheel** on grid: vertical wheel = *zoom* keeping beat-under-mouse fixed
  (`PianoRoll.cpp:2442-2459`); horizontal wheel = scroll (2432-2441).
- **H-scrollbar** drag (`DragOp::HScrollBar`, `PianoRoll.cpp:2102-2118`).

**Vertical scroll:**

- **Riff scrollbar** drag changes `lowestOctave` (`DragOp::ScrollBar`, 2079-2099).
- **Key-panel scrollbar** drag changes `keyLowest` (`DragOp::KeyScaleBar`,
  2143-2162); wheel on key panel also scrolls octaves (2421-2429).

**Chord-event editing (the scale-master surface):**

- **Chord timeline** (`chordTimelineRow`): click empty → add chord event
  inheriting active chord; click existing → select + drag to move (index 0 pinned
  at beat 0); right-click → delete (`PianoRoll.cpp:1610-1675`). Move drag
  re-sorts and re-finds index (2056-2076).
- **KEY panel** click sets the selected/added chord event's **root** (full MIDI
  incl. octave) at that beat (`PianoRoll.cpp:1904-1961`).
- **SCALE panel** click sets the chord event's **scaleType**
  (`PianoRoll.cpp:1964-2024`).
- **ROOT −/+** in ctrl row and **arrow keys** nudge selected chord root/type
  (`PianoRoll.cpp:1677-1693`, 2566-2593).
- **CHORDS palette drag-drop:** press a chord cell (root×type grid,
  `hitChordTab` 330), drag a ghost up onto the grid (`drawChordDragGhost` 455),
  drop to create/replace a chord event **and** bulk-insert that chord's tones as
  degree-relative riff notes spanning to the next chord (`mouseUp`
  `PianoRoll.cpp:2280-2347`). Auditions the chord while held
  (`setPreviewChord`).

**Other UI:**

- Bar-count tiles set loop length (`PianoRoll.cpp:1593-1608`).
- Pattern `< >` switch among 128 patterns (one per MIDI note) (1438-1459).
- SLICE overlay chops a pattern across keys (1346-1373; `drawSliceWindow` 480).
- LOAD/SAVE `.cmz` zip, COPY/PASTE pattern to system clipboard, CLR RIFF/PAT/ALL,
  LINK toggle (cross-instance chord sync) (1462-1579).

### 1.6 Data model (1111)

From `PluginProcessor.h`:

- **`RiffNote`** (`PluginProcessor.h:40-46`): `{beat, duration, degree(0..6),
  octave, velocity}` — **scale-degree relative**, *not* a MIDI pitch.
- **`ChordEvent`** (`PluginProcessor.h:49-57`): `{beat, rootNote(MIDI),
  chordType(0..11), scaleType(0..11)}`.
- **`Pattern`** (`PluginProcessor.h:76-81`): `notes[] + chords[] + barCount`;
  128 patterns, one per MIDI trigger note.
- **12 scales** `SCALE_TYPES` (`PluginProcessor.cpp:14-27`) — 7 semitone offsets
  each. **12 chord types** `CHORD_TYPES` (`PluginProcessor.cpp:29-42`) — intervals
  + an associated 7-note scale.
- **Resolution:** `computeMidiNote(degree,octave,root,chordType,scaleType)` =
  `root + octave*12 + SCALE_TYPES[scaleType].scale[degree]`
  (`PluginProcessor.h:247-255`). This is the *only* bridge from the relative model
  to absolute MIDI, and it's done **at draw time / play time**, never stored.

---

## 2. Mapping table: 1111 feature → seq24 widget

seq24's data model (`sequence.h`/`event.h`): notes are **paired note-on/note-off
`event`s** with absolute MIDI pitch (`event::get_note()`), velocity
(`get_note_velocity`), timestamp in **ticks** (`c_ppqn=192` per quarter). The
roll draws by walking linked note events via `get_next_note_event`
(`seqroll.cpp:523`). Geometry: `c_key_y=8` px per **chromatic** key, 128 keys,
`convert_xy`/`convert_tn` (`seqroll.cpp:756-771`). Zoom = **ticks per pixel**
(integer, menu 1..32, `seqedit.cpp:286-291`). Snap in ticks
(`seqedit.cpp:294-309`).

| 1111 feature | seq24 home | Status | Notes / conflicts |
|---|---|---|---|
| Note grid surface | `seqroll` | **Have** | Same purpose; reskin draw + add cosmetics. |
| Left key strip | `seqkeys` | **Have** | seq24 draws real black/white piano keys; 1111 draws degree-colored rows. |
| Time ruler / bar numbers | `seqtime` | **Have** | Equivalent (`seqtime.cpp:168-260`). |
| Velocity / data lane | `seqdata` (+`seqevent`) | **Have** | seqdata draws value bars already; restyle stems/caps. |
| Horizontal scroll | `m_hadjust` shared adjustment | **Have** | seq24 uses GTK `Adjustment`; 1111 uses custom `scrollBeat`. |
| Vertical scroll (chromatic) | `m_vadjust` | **Have** | But 1111's riff scroll is **per-octave of 7 degree rows**, not 128 chromatic keys — conflict (see §3). |
| Horizontal **zoom** | `seqedit` zoom menu → `set_zoom` | **Partial** | seq24 zoom is discrete ticks/pixel via menu; 1111 is continuous wheel zoom. Add wheel-zoom + finer steps. |
| Vertical zoom (rows/octave) | — | **Missing** | seq24 has fixed `c_key_y=8`. 1111 row height is elastic (`rowH`). |
| Add note (double-click) | `seqroll` right-drag paint (`m_adding`) | **Conflict** | seq24 adds via right-click "pencil" paint, not double-click. Need to add double-click-to-add or keep both. |
| Delete note | Delete key / paint-erase | **Partial** | No double-click-delete in seq24; add it. |
| Single select / Ctrl-toggle | `select_note_events` e_select_one | **Have** | `seqroll.cpp:932-956`; Ctrl handled at 936. |
| Lasso select | `m_selecting` rubber-band | **Have** | `seqroll.cpp:643-666`, 1060-1075. Visual restyle only. |
| Move (single+multi) | `move_selected_notes` | **Have** | `seqroll.cpp:1077-1092`. Multi-select move already works. |
| Resize / grow | middle-button `grow_selected`/`stretch_selected` | **Conflict** | seq24 grows with **middle button**; 1111 uses a **right-edge handle**. Add edge-handle resize. |
| Velocity drag | `seqdata` drag | **Have** | `seqdata` line-drag sets values. Restyle. |
| Copy/Cut/Paste | Ctrl+C/X/V | **Have** | `seqroll.cpp:1262-1294`; paste via `start_paste`. No right-click menu — optional add. |
| Select-all | — | **Missing** | seq24 has no Ctrl+A in seqroll; easy add via `select_note_events(...,e_select)` over full range. |
| Paste cursor preview | `m_paste` floating box | **Partial** | seq24 shows a moving paste rect on motion; 1111 shows a fixed cursor line. Cosmetic. |
| Snap-to-grid | `m_snap` (ticks) | **Have** | `snap_x` (`seqroll.cpp:1194-1205`). Map 1111's snapDiv → ticks. |
| Color palette | hardcoded `Gdk::Color("white"/"black"/…)` | **Missing** | seq24 uses named X colors (`seqroll.cpp:40-50`); must allocate the synthwave palette. |
| Alternating degree stripes | scale shading in `draw_background` | **Partial** | seq24 only greys out-of-scale rows (`seqroll.cpp:291-303`). Repurpose for degree striping. |
| Graded beat lines | `draw_background` beat loop | **Partial** | seq24 draws solid/dash lines (`seqroll.cpp:331-379`); add alpha grading (needs Cairo, see §4). |
| In-note labels | — | **Missing** | seq24 never labels notes. Add via Pango/`p_font_renderer`. |
| Chord timeline row | — | **Missing** | No equivalent. New widget or extend `seqtime`. (§3/§4 phase 5.) |
| KEY / SCALE panels | — | **Missing** | No scale-master surface yet. New widget(s). |
| CHORDS drag-drop palette | — | **Missing** | No equivalent. New widget. |
| Tab bar | — | **Missing** | seq24 stacks seqevent+seqdata always-visible; no tabs. |
| Bar-count selector | length via menu/measures | **Partial** | seq24 sets length elsewhere; could add tile row. |
| Pattern `< >` selector | seq24 = separate sequences in main grid | **Conflict** | seq24's "pattern per MIDI note" maps to seq24 sequences/sets, a different model. Out of scope for the roll reskin. |
| SLICE overlay | — | **Missing** | Out of scope for reskin. |
| LOAD/SAVE/LINK | seq24 file menu / no link | **Out of scope** | seq24 has its own .midi save. |
| Playhead | `draw_progress_on_window` | **Have** | `seqroll.cpp:447-470`; recolor to `cActive`. |

**Summary:** seq24 already owns the hard parts — a note grid, key strip, ruler,
data lane, selection/move/resize/copy/paste, shared scroll adjustments. The gaps
are (a) **cosmetics** (palette, stripes, labels, graded lines, rounded notes —
needs Cairo), (b) **interaction-model tweaks** (double-click add/delete,
edge-handle resize, wheel zoom, Ctrl+A), and (c) **entirely new scale-master
surfaces** (chord timeline, KEY/SCALE panels, chord palette) which are large and
tie into the planned "scale master" feature, not the basic reskin.

---

## 3. The fundamental data-model difference

**1111 stores notes as `(degree 0..6, octave)` relative to a per-region
`ChordEvent` (root + scale).** Absolute pitch is *derived* only when drawing or
playing, via `computeMidiNote()` (`PluginProcessor.h:247-255`). Consequences:

- The grid has **7 rows per octave** (one per scale degree); there are no
  "black keys" — every visible row is in-scale by construction.
- Transposing the chord root or swapping the scale **re-pitches every note**
  automatically; the riff is "key-agnostic".
- A note's screen row is `noteToRow(degree,octave)` (`PianoRoll.h:69`); its label
  is the *resolved* MIDI name at its beat's active chord (`PianoRoll.cpp:745-757`).

**seq24 stores notes as absolute MIDI pitch** in paired `event`s. The grid has
**12 chromatic rows per octave** (`c_key_y` each); scale-awareness is only a
*shading overlay* (`c_scales_policy`, `seqroll.cpp:291-303`) that greys
out-of-scale rows but does not constrain or relocate notes. There is exactly one
key/scale for the whole sequence (`m_key`, `m_scale`), set via menus
(`seqedit.cpp:330-354`), with only 3 scales defined (`globals.h:194-216`).

### Recommended approach: keep absolute pitch, add a scale-degree *view layer*

Do **not** change seq24's stored model to degree-relative — that would break
MIDI I/O, file format, playback, and the whole `event` infrastructure, and it
conflicts with seq24 being a general MIDI sequencer (chromatic notes must remain
representable). Instead, bring the **1111 look** on top of the absolute model:

1. **Adopt 1111's richer scale tables.** Replace/extend seq24's 3-scale
   `c_scales_policy`/`c_scales_text` (`globals.h:194-268`) with the 12
   `SCALE_TYPES` from `PluginProcessor.cpp:14-27`. seq24 already plumbs `m_scale`
   and `m_key` through `seqroll::set_scale/set_key` and `seqkeys` — only the table
   widens. This is the seed of the "scale master".

2. **Scale-collapsed row view (optional, the closest to the 1111 look).** Add a
   "degree view" toggle to `seqroll` that, when a scale is active, **renders only
   in-scale rows** at a taller row height and hides chromatic rows — mapping the
   128 chromatic keys to "7 visible rows per octave". This is a *projection*:
   - A row→pitch lookup table is built from `(m_key, m_scale)`:
     `degreeToMidi[octave][degree] = m_key + octave*12 + SCALE_TYPES[m_scale].scale[degree]`.
     This is exactly `computeMidiNote` with a fixed root and `chordType=-1`.
   - Replace `convert_xy`/`convert_tn` (`seqroll.cpp:756-771`) with versions that
     index that table instead of multiplying by `c_key_y`. Out-of-scale incoming
     notes (e.g. from MIDI import) snap to the nearest degree row for *display*
     but keep their true pitch in the event (mirrors
     `chordTabIntervalToDeg`, `PianoRoll.cpp:358-370`).
   - When the user adds/moves a note, write the **absolute MIDI pitch** from the
     degree-row table back into the `event` — so the file/model stays absolute.

   This gives the 7-rows-per-octave, no-black-keys, key-agnostic feel while every
   note remains a normal absolute-pitch seq24 event. Chromatic editing remains
   available by toggling the scale to "Off" (`c_scale_off`), which falls back to
   the 12-row chromatic view.

3. **Chord events / scale-master as an overlay track.** 1111's per-region
   `ChordEvent`s (root+scale changing over time) have **no home in seq24's model**.
   For the planned scale-master feature, store them in a *separate* structure
   (e.g. a dedicated meta-sequence or a side list keyed by tick) and let the
   degree-view lookup table become **time-varying**: `convert_*` consult "active
   chord at tick" (port `getActiveChord`, `PluginProcessor.h:231-243`) to pick the
   root/scale for that column. Notes still store absolute pitch; only the *row
   mapping* and *shading* follow the chord track. This is the natural extension
   point and should be designed but built last.

4. **Note labels** become trivial: with absolute pitch already stored, just
   render the MIDI note name (reuse `c_key_text` + octave like
   `seqkeys.cpp:166-183`) — no resolution step needed, unlike 1111.

Net: seq24 keeps its absolute-pitch truth; the degree model becomes a
*reversible view transform* `(key,scale[,chord-at-tick]) ↔ row`. The only stored
new data is the optional chord/scale-master track, which is additive.

---

## 4. Phased implementation plan (gtkmm-2.4, lowest risk first)

**Important rendering note.** seq24 draws with the legacy GDK API
(`Gdk::GC::set_foreground`, `draw_rectangle`, `draw_line`, named `Gdk::Color`)
and **`set_double_buffered(false)`** with a manual `Gdk::Pixmap` back-buffer
(`seqroll.cpp:101`, `205`, `440-444`). GDK `Gdk::Color` has **no alpha** and no
rounded rects, so the 1111 look (alpha-graded beat lines, translucent
selections, rounded notes, glows) requires **Cairo**, which gtkmm-2.4 supports
via `get_window()->create_cairo_context()` / `Gdk::Cairo::set_source_color`.
The plan introduces a Cairo path gradually rather than rewriting everything.

---

### Phase 0 — Palette & flat recolor (lowest risk, pure cosmetics)
**Effort: ~0.5 day.**
- Add the 13 synthwave colors (`PianoRoll.h:235-248`) as allocated `Gdk::Color`s
  alongside the existing `m_black/m_white/...` in `seqroll`, `seqkeys`, `seqtime`,
  `seqdata` constructors (pattern: `seqroll.cpp:38-50`). Use hex via
  `Gdk::Color::set_rgb_p()` or `"#0A0A14"`.
- Swap foregrounds in the existing draw routines: background `cBg`, notes `cNote`,
  selected `cNoteSel`, playhead `cActive` (`draw_progress_on_window`,
  `seqroll.cpp:463`), key strip `cBlk`.
- **Risk:** none — no geometry or interaction change. Immediately reads as "1111".

### Phase 1 — Cairo note & grid rendering in seqroll
**Effort: ~2 days.**
- In `seqroll::draw_events_on` (`seqroll.cpp:474-596`) and `draw_background`
  (261-386), create a Cairo context for the pixmap and draw: alternating row
  stripes, octave separators, **alpha-graded beat lines** (port `drawBeatLines`,
  `PianoRoll.cpp:1256-1282`), rounded note rects with 1px outline + right-edge
  resize-handle hint (port `drawRiffPanel` note loop, 727-759).
- Keep the pixmap double-buffer flow intact; only the painting calls change.
- **Risk:** low-medium — Cairo coexists with GDK; verify pixmap target works
  under gtkmm-2.4 (`Cairo::RefPtr` from `Gdk::Pixmap`).

### Phase 2 — Key strip & data lane restyle
**Effort: ~1 day.**
- `seqkeys::update_pixmap` (`seqkeys.cpp:122-192`): recolor; optionally tint rows
  by harmonic role using the (widened) scale table, echoing `drawRiffKeys`
  (`PianoRoll.cpp:809-868`).
- `seqdata`: redraw velocity bars as 1111 stems with top caps (port
  `drawVelocityPanel`, `PianoRoll.cpp:289-309`).
- **Risk:** low.

### Phase 3 — Interaction-model parity
**Effort: ~2 days.**
- **Double-click add/delete** in `seqroll::on_button_press_event` (detect
  `GDK_2BUTTON_PRESS`): empty → `add_note`; on note → mark+remove (mirror
  `mouseDoubleClick`, `PianoRoll.cpp:2359-2393`). Keep existing pencil/middle-grow
  as alternatives or gate behind a mode toggle.
- **Right-edge handle resize** with left button: in `on_button_press_event`,
  if click is within ~6px of a selected note's right edge, enter a grow state
  (reuse `grow_selected`, `seqroll.cpp:1098-1114`) instead of requiring middle
  button (`isResizeHandle`, `PianoRoll.cpp:1330-1336`).
- **Wheel zoom** keeping beat-under-cursor fixed: extend
  `seqroll::on_scroll_event` (`seqroll.cpp:1330-1346`) to adjust zoom + hadjust
  (port logic from `PianoRoll.cpp:2442-2459`). Add finer zoom steps to
  `seqedit`'s zoom menu (`seqedit.cpp:286-291`).
- **Ctrl+A select-all** in `on_key_press_event` (`seqroll.cpp:1247-1304`) via
  `select_note_events(0, 0, max, 127, e_select)`.
- **Risk:** medium — interaction changes need manual testing; behaviors are
  additive so existing workflows can be preserved.

### Phase 4 — Scale-degree view layer
**Effort: ~3-4 days.**
- Widen scale tables to 12 `SCALE_TYPES` in `globals.h` (from
  `PluginProcessor.cpp:14-27`) and extend `seqedit`'s scale menu
  (`seqedit.cpp:352-354`).
- Add `seqroll` "degree view" toggle; build the `(key,scale)→row` lookup; rewrite
  `convert_xy`/`convert_tn`/`snap_y` (`seqroll.cpp:756-771`, 1187-1191) to index
  it; mirror the same mapping in `seqkeys`. Notes still stored absolute (§3.2).
- Add note labels via `p_font_renderer` (as in `seqkeys.cpp:179`).
- **Risk:** medium-high — touches coordinate math used by add/move/select; needs
  careful round-trip tests (degree-row → MIDI → degree-row must be stable).

### Phase 5 — Scale-master / chord track surfaces (largest, design-only for now)
**Effort: ~5-8 days; gate behind the larger "scale master" project.**
- New chord-timeline strip (extend `seqtime` or a new `DrawingArea` row in
  `seqedit`'s table) holding root+scale change events over time (port
  `drawChordTimeline` + edit handlers, `PianoRoll.cpp:606-652`, 1610-1675).
- New KEY/SCALE editor panels (port `drawScalePanel`/`drawChordRefPanel`,
  872-1115) and optionally the CHORDS drag-drop palette (372-433, drop logic
  2280-2347).
- Make Phase-4's row lookup **time-varying** off the chord track (`getActiveChord`,
  `PluginProcessor.h:231-243`).
- Optional: tab bar to switch data/velocity/chord panels (port `drawTabBar`,
  240-255) instead of seq24's always-stacked layout.
- **Risk:** high — new persistent data, new widgets, file-format additions. Should
  be specced separately once Phases 0-4 land.

---

### Recommended order & rationale
Phases 0-2 deliver ~80% of the *visual* "this is the 1111 piano roll" impression
with near-zero behavioral risk, because seq24 already has the widgets and the
data model is untouched. Phase 3 closes the *feel* gap (double-click, handle
resize, wheel zoom) additively. Phase 4 introduces the scale-degree projection —
the conceptually novel part — as a reversible view over absolute pitch. Phase 5
(chord/scale-master track) is the big, model-extending work and should be its own
project, designed to plug into the Phase-4 lookup.

**First milestone to attempt:** Phase 0 + Phase 1 on `seqroll` only — palette
constants + Cairo grid/notes — verifiable purely by eye against the 1111
screenshots, with no risk to seq24's editing logic.
