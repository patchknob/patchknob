# Task B -- Chapter 29 (Edit Modes and Tools) + Chapter 30 (Making Selections)

Manual: ch.29 = printed 639-667 (`pages/p671.txt`..`p699.txt`), ch.30 = printed 669-691
(`pages/p701.txt`..`p723.txt`). Key figures: `pt-718-106.png` (the whole Edit window -- mode
block, tool strip, zoom presets, counters, Grid/Nudge fields, Universe strip),
`pt-674-037.png` (Grid value menu), `pt-674-038.png` (tool strip), plus the tool/cursor figures
on p673-690 and the Universe figures on p718-720.

## Files you own
`sdlui/views/arrange/**` and `sdlui/CMakeLists.txt` (if you add files). You may read anything.
**Do NOT edit `sdlui/main.cpp`, `sdlui/transport_bar.*`, `src/audio_app.*`** -- another agent is
in those files this round. Everything here must be implementable inside the arrange view, which
already owns the clip model, snap, zoom, selection, tool strip and context menus. If a feature
truly needs shell wiring, add the `std::function` hook to `arrange_view.h` with a default
no-op/fallback so the view still works unbound, and list it in your report for wiring later.

## Where you are starting from
`arrange_view.h` documents the model -- read it fully first. Today there are four tools
(`EditTool { Grab, Range, Cut, Draw }`), a snap table + snap menu, zoom in/out/fit, a lasso,
clip selection with a clipboard, a ruler with L/R loop markers, per-lane heights, and audio
regions in `m_region` (Ardour laws: position / source / length, half-open spans).

## Chapter 29 -- Edit modes

### Edit modes (p671-673)
Four modes: **Shuffle, Slip, Spot, Grid**, shown as a 2x2 button block in the tool strip like
the figure. `F1..F4` select them, the accent key `` ` `` cycles. Behaviour:
* **Shuffle** -- clips snap to each other; moving/trimming/deleting/pasting ripples subsequent
  clips on the lane (cut closes the gap, paste pushes material right, trimming a start/end
  slides the neighbours). Existing silence between clips is preserved, not removed. Clips
  cannot overlap in Shuffle. MIDI note placement is unaffected by Shuffle.
* **Slip** -- free placement, gaps and overlaps allowed (today's behaviour).
* **Spot** -- placing/moving/trimming a clip opens a Spot dialog to type an exact location
  (bar|beat|tick and m:ss.mmm, plus the clip's original/user time stamp as reference).
* **Grid** -- snap to the grid value, with **Absolute** and **Relative** sub-modes (Relative
  moves by grid increments preserving the clip's offset from the grid). A Grid-mode selector
  chooses Absolute/Relative.
* **Snap To Grid** available while in Shuffle/Slip/Spot: `Shift+F4`, or Shift-click the Grid
  button; then the edit cursor and selections are grid-constrained while clip editing still
  follows the other mode. `Shift`-click Shuffle/Slip/Spot while in Grid to combine.
* **Shuffle Lock** -- Ctrl-click the Shuffle button while in another mode locks Shuffle out
  (a lock glyph appears; key commands for Shuffle stop working). Ctrl-click again unlocks.
* Grid value menu (figure `pt-674-037.png`): 1 bar / 1/2 / 1/4 / 1/8 / 1/16 / 1/32 / 1/64,
  dotted + triplet modifiers, time-scale choice (Bars|Beats, Min:Secs, Samples,
  Clips/Markers), "Follow Main Time Scale". `Shift+=` / `Shift+-` step the grid value.
  **Clips/Markers** grid: free placement that snaps to clip starts/ends/sync points, markers
  and edit-selection boundaries when near them.

### Edit tools (p674-699)
Full tool set with pop-up mode menus (click-and-hold on the tool, and a right-click
`Tools >` submenu), `F5`-`F10` to select and toggle each tool's modes, `Esc` cycles tools:
* **Zoomer** -- Normal / Single (Single returns to the previous tool after one zoom). Click =
  zoom in one level centred on the click; Alt-click = zoom out; drag = zoom to that range;
  Ctrl+drag = horizontal + vertical. Ctrl+drag up/down/left/right = continuous zoom.
  Double-click the tool = fit whole session. Ctrl+drag in the ruler zooms the ruler range.
* **Trim tools** -- Standard, TCE (time compression/expansion), Scrub, Loop:
  * Standard trims to the source-file bounds; Alt reverses the trim direction; trims *all*
    selected clips, not just the one grabbed; in Shuffle it slides neighbours, in Grid it
    snaps, in Spot it opens the Spot dialog.
  * **TCE Trim** -- dragging an edge time-compresses/expands the audio to the new length
    (PatchKnob has `src/engine/audioclip/warp_stretch.*` -- use it). Works in Grid (snap to
    grid, Relative Grid keeps the offset), Slip (free) and Spot (dialog for start/end/duration).
  * **Scrub Trim** -- drag inside a clip to audition (audio routed through the track path),
    release to trim there. Ctrl = finer resolution.
  * **Loop Trim** -- top half of a clip loop-trims (changes how long the clip is looped, adding
    or removing loop iterations); bottom half / the loop glyph trims the *source iteration*
    while the overall looped length stays constant. Ctrl constrains to whole iterations.
    PatchKnob already models looped regions (`AudioRegion::loop`, `loopLength`).
  * **Tandem trimming** -- Ctrl+Start-trim between two adjacent overlapping clips trims both
    ends together (works with Loop and Scrub trim, not TCE).
* **Selector** -- edit cursor placement, drag = range selection (across tracks by dragging
  vertically), double-click = select whole clip, triple-click = select whole track.
* **Grabber** -- Time / Separation / Object:
  * Time Grabber selects and moves whole clips,
  * Separation Grabber drags an edit selection out as a new clip (Alt-drag copies without
    disturbing the original),
  * Object Grabber Shift-clicks noncontiguous clips across tracks into one selection
    (disabled in Shuffle and Spot; ignores edit groups). Converting Object<->Time selection by
    double-clicking the Grabber / Selector icon in the strip.
* **Smart Tool** -- one tool whose function follows the cursor position inside the clip:
  upper-middle = Selector, lower-middle = Grabber, near start/end = Trim, near the top corners
  = fade-in/fade-out drag, near the bottom between two adjacent clips = crossfade drag,
  over an existing fade in the vertical middle = adjust fade shape. `F6+F7` (or `F7+F8`)
  selects it. Ctrl temporarily switches it to the Scrubber. Reproduce the zone map in the
  figure on p693 and mirror it for automation lanes (bottom 75% = selector, top 25% = trim,
  Ctrl = insert/grab breakpoints, Shift constrains vertically).
  *(Fade creation/adjustment gestures themselves are Task D's; implement the Smart Tool zone
  map + cursor feedback and route the fade zones through the existing
  `m_fade_grab` / `commit_fade` machinery that is already in the view.)*
* **Scrubber** -- drag to scrub one track (or two, by dragging between adjacent tracks) at
  playback speed or slower; Alt-drag = Shuttle mode (several times normal speed);
  "Edit Insertion Follows Scrub/Shuttle" option leaves the cursor where scrubbing stopped;
  Shuttle Lock on the numeric keypad (Start+0..9, 5 = normal, 9 = fastest, 0 stops; +/- flips
  direction) with a Custom Shuttle Lock Speed setting (50-800%).
* **Pencil** -- draws MIDI/automation data; on audio, only active when zoomed to sample level,
  where it destructively redraws the waveform (waveform repair). Warn the user (status line)
  that pencil audio editing is destructive.
* **Edit/Tool Mode Keyboard Lock** -- an option that locks the current tool *modes* (and the
  Grid mode) so F-keys switch tools but never cycle their sub-modes; mouse / right-click can
  still change them.

### Zooming (p675-684)
Horizontal zoom in/out buttons (click, or drag for continuous), vertical audio zoom and vertical
MIDI zoom buttons, per-track vertical zoom (Ctrl+drag with the Zoomer), the documented zoom key
commands (fit session, fit selection, previous zoom level, overview scale = 256 samples/pixel,
reset waveform heights), **five Zoom Presets** (click-and-hold a preset button to store,
click to recall, `Ctrl+1..5`), and **Zoom Toggle**: stores/recalls vertical zoom, horizontal
zoom, track height, track view and grid setting, with the documented preferences
(Vertical/Horizontal Zoom = Selection or Last Used; Track Height = Last Used/Medium/Large/
Jumbo/Extreme/Fit To Window; Track View; Remove Range Selection After Zooming In; Separate Grid
Settings When Zoomed In; Zoom Toggle Follows Edit Selection), the toggle button in the strip,
`E` to toggle, Alt-click to clear the stored state, and auto-toggle-when-changing-selection
behaviour. Scroll-wheel zoom modifiers: Alt = horizontal, Alt+Shift = audio vertical,
Alt+Ctrl = MIDI vertical, Shift = horizontal scroll.

## Chapter 30 -- Making selections

* **Link Timeline and Edit Selection** toggle (button + `Shift+/`), and **Link Track and Edit
  Selection** (button + `Shift+T`). When unlinked, the timeline selection (play/record range)
  and the edit selection are independent: draw **Timeline Selection Markers** (arrows, tinted
  "recording" when any track is record-armed) and, when unlinked, **Edit Markers** (brackets)
  in the ruler; both draggable, and Alt-drag slides a whole selection preserving its length.
* Selecting: portion of a clip, whole clip (Time Grabber click / Selector double-click), whole
  track (triple-click / Select All), two clips + the range between them (Shift-click),
  select-all-from-ruler (double-click a ruler), selections across multiple tracks (drag
  vertically, Shift-click other tracks), making selections during playback with Down/Up arrows.
* **Object selections** with the Object Grabber, and Object<->Time conversion (see above).
* Changing selection length (Shift-click / Shift-drag an end, drag the markers), **nudging**
  the selection range and its start/end points by the Nudge value, extending the selection to
  clip start/end (`Shift+Tab`, `Ctrl+Shift+Tab`) and to an adjacent clip, **Double Selection**
  and **Halve Selection**, **Duplicate and Extend Selection**, **Move Edit Left/Right** by the
  selection amount, moving/extending the selection across tracks (`Start+P` / `Start+;`,
  Shift variants), **Remove Edit From Top / Bottom**.
* **Edit Selection indicators**: Start / End / Length fields at the top of the window in the
  main time scale, typed entry with `/` to cycle fields, `.` and arrows to move between and
  change subfields, scroll-wheel edit, drag-to-scrub a field, and calculator entry (`+`/`-`
  then a number then Enter). Same shortcuts for the Main / Sub counters, which navigate the
  transport when a value is entered.
* **Tab to Transients**: a toggle button; `Tab` / `Alt+Tab` move the cursor to the next /
  previous transient (and to clip boundaries when the toggle is off), `Shift+Tab` extends the
  selection to it; works with the cursor spanning multiple tracks (first transient on any of
  them). Implement real transient detection on the region's audio.
* **Timeline selections**: drag in a ruler, type into the transport start/end fields, drag or
  Alt-drag the markers, **Change Timeline to Match Edit** / **Change Edit to Match Timeline**.
* **Universe view**: a session overview strip above the ruler showing every visible track's
  material as coloured lines in track order, with a frame showing what the canvas is currently
  displaying; click/drag the frame to navigate horizontally and vertically; resizable by
  dragging its bottom edge; show/hide toggle (`Alt+7` and a View menu entry / button).
* **Auto-scroll tracks** so a track selected elsewhere scrolls into view, plus **Restore Last
  Selection** (`Ctrl+Alt+Z`) and the track-height / track-view keyboard commands from p690-691.

Take the whole list. Where a key command collides with an existing PatchKnob binding, keep the
existing one, choose a near equivalent, and document the change in the on-screen help overlay
(`draw_help`) as well as your report -- and keep that help overlay up to date with everything
you add.
