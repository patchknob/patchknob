# Task D -- Chapter 32: Fades and Crossfades

Manual: printed 709-729 = `pages/p741.txt` .. `p761.txt`. Figures: the Fade Out and Crossfade
dialogs (`images/pt-743-148.png`, `pt-743-149.png`) and their view/zoom/audition button glyphs
(`pt-743-150..154.png`, `pt-744-155..160.png`), the fade-shape preset thumbnails on p745-746,
the Batch Fades dialog on p753, crossfade type diagrams on p741-742, and the move/nudge/trim
figures on p757-760.

## Files you own
`sdlui/views/arrange/**`, plus new files for the fade dialog (add them to
`sdlui/CMakeLists.txt`), `sdlui/main.cpp`, and `src/audio_app.*` / `src/engine/audioclip/**` if
the engine needs new fade shapes. Other agents have already landed Chapter 27/29/30/31 work in
these files -- **read them as they are now; do not revert anything**.

## What exists today
The arrange view already has per-clip fades: `ClipFade { inTicks, outTicks, inK, outK }`,
corner drag handles, curve-tension dragging, `commit_fade()`, and the engine side
`audio_app_project_set_fades(track, clip, fadeInFrames, fadeOutFrames, inTension, outTension)`.
Crossfades between adjacent clips are NOT modelled. You are extending this into the full
Pro Tools fade model.

## Features

### 1. Fade model (p741-742, 745-747)
* Fade-in, fade-out, and **crossfade** between two adjacent clips, with the three crossfade
  placements from the manual -- **Centered** (spans the splice, needs material either side),
  **Pre** (ends at the splice; preserves clip 2's attack) and **Post** (starts at the splice).
* A crossfade cannot be created where a clip has no audio beyond its boundary; when a selected
  batch contains such clips, prompt to skip those fades or adjust the selection bounds.
* **Shapes**: Standard, S-Curve, and the seven preset parabolic curves, separately for the
  fade-out (Out Shape) and fade-in (In Shape) halves -- reproduce the seven curves' characters
  as described on p745-746 (1 = full volume then instant drop ... 7 = silent immediately).
  Standard and S-Curve are hand-editable by dragging; the seven presets are not.
* **Slope**: Equal Power vs Equal Gain, and for crossfades a **Link** setting
  (Equal Power / Equal Gain / None). With None the two halves move independently, including
  their start and end points.

### 2. Fade dialogs (p743-747, 754-755)
A Fade In / Fade Out / Crossfade dialog modelled on `pt-743-148.png`, in the app's own two-tone
style: a large curve+waveform plot, and
* **Audition** (plays the fade through the real signal path, honouring the fade preview
  pre/post-roll preference),
* view mode buttons: fade **curves only** (default), curves + **separate** waveforms, curves +
  **superimposed** waveforms, curves + **summed** waveform, and for multitrack crossfades
  **view first track / second track / both**,
* waveform amplitude **Zoom In / Zoom Out** (Ctrl-click resets),
* Out Shape / In Shape selectors with the preset thumbnails, Slope radio group, Link group,
* dragging the curve to reshape it (Alt-drag edits only the fade-in half, Ctrl-drag only the
  fade-out half; Alt-click resets a curve to its default), and dragging the black square
  handles to move a fade's start/end point when Link = None,
* **Presets 1-5** (Ctrl-click a preset button to store, click to recall, `Ctrl+1..5`), a
  settings menu with `<factory default>` / Save Settings / Save Settings As / Import Settings /
  Delete Current Settings File / Save Fade Settings To (session folder vs root settings
  folder), and a COMPARE control, persisting to `.fdpreset`-equivalent files using whatever
  settings-file mechanism PatchKnob already has.

### 3. Creating fades (p749-753)
* `Ctrl+F` (Edit > Fades > Create) on a selection: at a clip start -> fade-in, at a clip end ->
  fade-out, across a splice -> crossfade; selection length = fade length.
* `Ctrl+Start+F` applies the **default** shape from preferences without opening the dialog.
* **Fade To Start** / **Fade To End** from the edit cursor (`Start+D` / `Start+G`).
* **Batch Fades** (p753): selecting across several whole clips and creating fades opens a Batch
  Fades dialog with Create New Fades / Adjust Existing Shape & Slope / Adjust Existing Length
  checkboxes for fade-ins, crossfades and fade-outs, a placement choice (Pre-Splice / Centered /
  Post-Splice) and lengths in ms for each. Creates a crossfade at every internal boundary, a
  fade-in at the first clip and a fade-out at the last.
* **AutoFades** (p752): a preference (0-10 ms, 0 = off) applying real-time fade-in/out at every
  free-standing clip boundary during playback, not drawn in the arrangement and not rendered to
  disk (they must apply in the audio clip player, and be baked in when bouncing/freezing).
* Fade and crossfade **preferences** (p748): default Fade In shape, default Fade Out shape,
  default Crossfade shape (used by the Smart Tool and the default-fade command), and pre/post
  roll for fade previews.

### 4. Editing fades in the arrangement (p755-761)
* **Smart Tool** fade zones: drag near a clip's top corner to create a fade-in/out with the
  default settings; drag between two adjacent clips near the bottom to create a crossfade;
  drag over an existing fade in the vertical middle to reshape it (the fade highlights while
  dragging); Shift applies the same change to every fade in a multi-track edit selection;
  Ctrl-click with the Selector does the same as the Smart Tool. A preference chooses whether
  the fade-adjust gesture needs the Ctrl modifier.
* **Change fade shape by keyboard**: `Alt+Start+Left/Right` cycles a selected fade through
  Standard, S-Curve and the seven presets.
* **Right-click Fades submenu** on an edit selection: Shape (Standard / S-Curve), Slope
  (Equal Power / Equal Gain) with the current value ticked and italics when the selection mixes
  values, Batch Fades..., Create..., Delete.
* **Delete fades**: Edit > Fades > Delete on a selection, select a crossfade with the Grabber
  and press Delete, or right-click > Delete Fades.
* **Trim a crossfade**: select it and trim either side with any Trim tool; the crossfade is
  recalculated for the new length.
* **Moving and nudging fades** independently of their clips: dragging or nudging a fade-in /
  fade-out reveals or hides audio; moving a crossfade changes the clips' overlap point;
  movement is constrained by the underlying clip boundaries. Moving/nudging a *clip* carries
  its fades; nudging a clip adjacent to a fade stretches or shrinks that fade to keep the fade's
  outer point; nudging a clip that contributes to a crossfade stretches the crossfade, and
  removes it if pushed past the available overlap.
* **Preserve Fades when Editing** preference: whether separating/moving crossfaded clips keeps
  the corresponding fades or drops them; separating across a crossfade splits it into a
  fade-out plus a fade-in at the selection boundary, and separating across a fade trims the
  fade to the new clip.
* **Overlapping Crossfades view** (View > Waveforms) -- show both clips' waveforms inside the
  crossfade, and draw **fade boundaries and shapes in automation lanes** as well.

Engine side: crossfades must actually be heard -- extend the audio clip player / region model
so overlapping regions crossfade with the chosen curve pair, and make sure fades survive project
save/load (`sdlui/project_io.*` and `audio_app_project_add_audio_region`, which already carries
fadeIn/fadeOut/tension) -- add the new shape/slope/link fields there too, keeping older projects
loadable.
