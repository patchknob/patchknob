# Task C -- Chapter 28 (Editing Basics) + Chapter 31 (Editing Clips and Selections)

Manual: ch.28 = printed 621-638 (`pages/p653.txt`..`p670.txt`), ch.31 = printed 693-707
(`pages/p725.txt`..`p739.txt`). Figures: waveform views on p656-657, clip display options on
p662-664, Undo History window on p664-665, separate/trim/nudge/layered-editing figures on
p726-734.

## Files you own
`sdlui/views/arrange/**`, `sdlui/main.cpp`, `sdlui/CMakeLists.txt`, and any new files you add.
Another agent has already landed Chapter 29/30 work in the arrange view and Chapter 27 work in
main.cpp -- **read the current file contents before editing; do not revert their work**.
`src/audio_app.*` may be extended if you need engine support, keeping existing signatures
source-compatible.

## Chapter 28 -- Editing basics

* **Waveform views** (p655-657): View > Waveforms with **Peak** vs **Power** (RMS) overview
  calculation, **Rectified** display, **Outlines** on/off, and **Overlapped Crossfades**
  (draw both contributing clips' waveforms inside a crossfade). Peak view is forced while
  recording and at sample-level zoom. PatchKnob draws waveforms in
  `ArrangeView::draw_waveform` -- extend it, and cache overviews so this stays cheap.
* **Clip display options** (p662-664): View > Clip submenu toggles for **Name**, clip **times**
  (No Time / Current Time / Original Time Stamp / User Time Stamp), **Display on All Channels**,
  **Sync Points**, **Overlap Shadows** (overlapping clips cast a shadow over what they cover),
  **Transparency** (transparent overlay while dragging a clip over another),
  **Clip Overwrite Indicator** (a clip being dragged highlights when it is fully covering an
  off-screen smaller clip), **Clip Gain Info** (gain fader glyph + static dB value at the clip's
  lower-left), and **Rating**. Also the **Time Stamp** concept: every clip carries an original
  time stamp (where it was recorded/created) and a user time stamp that can be redefined.
* **Naming clips** (p662): rename a selected clip (menu, right-click > Rename,
  `Ctrl+Shift+R`, Grabber double-click); for a whole-file clip ask whether to rename the disk
  file too; renaming an auto-created clip promotes it to user-defined.
* **Multiple Undo** (p664-666): a proper undo *queue* (default 32 levels, settable 1-64 in
  preferences) replacing the current single-step undo, with Redo, and an **Undo History**
  window listing undoable operations in bold and redoable ones in italics, clicking any entry
  to jump to that state, optional creation times, **Undo All**, **Redo All**, **Clear Undo
  Queue**, the oldest-about-to-fall-off entry drawn in the "warning" role. Note which
  operations clear the queue (deleting a track, clearing a clip from the clip list).
  This is the biggest single item here -- design it as a command/state stack the arrange view
  and shell both push onto, and convert the existing ad-hoc undo paths
  (`ArrangeView::on_undo`, trigger undo, the disk-cached audio-clip delete) onto it.
* **Basic edit commands** (p666-670): Cut / Copy / Paste / Clear that operate on either an
  object selection (whole clips) or a time-range selection, across multiple tracks, and that
  respect the current Edit mode (Shuffle closes/opens gaps; otherwise cut leaves a hole and
  paste overwrites). Cut removes underlying clip data, **Clear** removes the selection without
  touching the clipboard. Auto-create the leftover clips on either side of a cut/clear.
  Selections in a track's master view carry the underlying automation with the edit
  ("automation follows edit"); when a lane shows automation only, edits touch just that lane
  and must write bounding breakpoints at both ends of the cut range to preserve the slope.
* **Special commands** (p669): Cut Special / Copy Special / Paste Special / Clear Special for
  automation and clip gain (All Automation, Pan/Volume/Mute...), plus **Repeat to Fill
  Selection** -- copy a clip, select a range, and fill it with repeats, trimming the last
  repeat to fit, offering the Batch Fades dialog for the crossfades between repeats
  (coordinate with the fades work: call the batch-fade entry point if it exists, otherwise
  leave a clearly-marked hook).
* **Editing across multiple tracks** (p670): paste into several tracks at once (Shift-click an
  insertion in each, or select in the ruler), routing each data type into the matching playlist.

## Chapter 31 -- Editing clips and selections

* **Capture Clip** (`Ctrl+R`) -- define the current selection as a new named clip in the clip
  list without altering the track.
* **Separate commands**: **At Selection** (`Ctrl+E`, splits at the selection start/end, or at
  the edit cursor), **On Grid**, **At Transients** -- the last two prompting for a
  **Pre-Separate Amount** in ms that pads the start of each new clip. Plus the
  **Auto-Name Separated Clips** preference and the "Separate Clip operates on all related
  takes" preference. Works across multiple tracks at once.
* **Separation Grabber** integration: separating by dragging (Task B built the tool; make the
  separate/heal commands agree with it).
* **Heal Separation** (`Ctrl+H`) -- rejoin two clips that are still adjacent, unmodified and
  from the same source file; refuse cleanly otherwise.
* **Trim commands**: Trim to Selection; Trim Start/End To Insertion; **Trim to Fill Selection**
  (Start to Fill / End to Fill / To Fill Selection -- expand a clip to cover a gap or the
  selection, as far as the underlying source allows); **Trim Clip to File** Start / End /
  Boundaries (expanding only as far as the neighbouring clip); trimming by the Nudge value.
* **Nudging** (p731-733): a **Nudge value** selector (its own time scale, "Follow Main Time
  Scale", typed custom values, `Shift+Alt+=` / `Shift+Alt+-` to step it), nudge selected clips
  by +/- on the keypad in any edit mode, **nudge by the next-larger nudge value** (`/` and `M`),
  and **nudge the contents of a clip** (slide the audio/MIDI inside fixed clip boundaries).
* **Layered Editing** option (p734-735): with it on, cutting/clearing/moving a clip reveals the
  partially overlapped clip underneath (fully covered clips are gone); with it off, overlapped
  clips stay trimmed to the overlapper. Add the toolbar toggle, and
  `Ctrl+Delete` = clear all track data in the edit selection regardless of the option.
* **Quantize to Grid** (`Ctrl+0`) -- snap whole selected clips' start points (or sync points)
  to the nearest grid boundary, moving the contents rigidly with the clip.
* **Stereo / multichannel** (p736): selections and trims always affect every channel;
  **Split Into Mono** to split a stereo/multichannel track into mono tracks named `.L`/`.R`,
  keeping output/send assignments, and the drag rules between mono and multichannel tracks.
* **Consolidate** (`Alt+Shift+3`) -- render the selected range of a track (including the silence
  between clips, treating muted clips as silence) into one new whole-file clip.
* **Compact** -- destructively delete the unused parts of an audio file with a user-entered pad
  in ms, deleting files no clip references. Must warn that it cannot be undone, and save the
  session afterwards.
* **TCE Edit to Timeline Selection** -- with Timeline and Edit selections unlinked, time
  compress/expand the edit selection to the length of the timeline selection (use
  `src/engine/audioclip/warp_stretch.*`), by an equal percentage across all selected
  tracks/channels; and **Fit to Selection** when dragging a clip in from the clip list with
  Ctrl+Alt.
* **Rating Clips** 1-5 (menu, right-click > Rate, and Ctrl+Alt+Start+number during playback),
  displayed on clips when View > Clip > Rating is on.
