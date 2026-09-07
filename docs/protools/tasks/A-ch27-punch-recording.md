# Task A -- Chapter 27: Punch Recording Modes

Manual: printed pages 601-618 = `docs/protools/pages/p633.txt` .. `p650.txt`; figures on
p637, p639, p641, p643, p650 (see `docs/protools/README.md`).

## Files you own (nobody else is editing these this round)
`src/audio_app.{h,cpp}`, `sdlui/main.cpp`, `sdlui/transport_bar.{h,cpp}`, `src/perform.*`.
**Do NOT edit `sdlui/views/arrange/**` or `sdlui/CMakeLists.txt`** -- another agent is working
there in parallel. If you need something from the arrange view (e.g. a record-button state), use
or extend the existing `std::function` hooks that `main.cpp` already binds
(`is_track_record_armed`, `on_track_record_arm`, ...) -- but implement the change on the
main.cpp side only, and if you truly need a new hook in arrange_view.h, write it down in your
final report instead of editing that file.

## What exists today
* One global live-record target: `g_arrangeRecTarget` in `main.cpp`, armed via
  `vArrange.on_track_record_arm`; `audio_app_record_arm_at_tick(on, startTick)` opens capture;
  `punch_out_recording` in the app struct; `toggle_record` / `drain_record` build a sequence +
  trigger at the record position when recording stops.
* `audio_app_track_capture_begin/end/preview` -- realtime post-fader capture of one
  master-mixer track into an `AudioClip`.
* `audio_app_project_partition_track(track, start, end)` -- Ardour RecNonLayered playlist
  partition: removes a sample range from scheduled regions, trimming/splitting
  non-destructively. This is exactly what a nondestructive punch needs.
* Transport bar has rec/play/stop/loop, count-in, record quantise.
* NOTE: `audio_app.h` says the audio INPUT device path is "stored; capture path is future work".
  Read the code and find out what can actually be recorded today (live MIDI, post-fader track
  capture). Build the punch modes on the real capture source(s) that exist. Do not fabricate
  an input path; state clearly in your report what a punch actually records.

## Features to implement

### 1. Record modes (p633-634, 638-639, 642-643)
Four transport record modes: **Normal**, **QuickPunch**, **TrackPunch**, **DestructivePunch**.
* Cycle with Ctrl+click on the transport Record button; also a right-click pop-up menu on the
  Record button listing the modes with the active one checked; also a keyboard shortcut
  (`Ctrl+Shift+P` QuickPunch, `Ctrl+Shift+T` TrackPunch, `Ctrl+Shift+D` DestructivePunch --
  the manual's Windows bindings, adapted where they collide with existing PatchKnob keys;
  document any change).
* Transport Record button badge: **"P"**, **"T"**, **"DP"** drawn in the button when the
  corresponding mode is on (figures on p639, p643).

### 2. Transport record-button status display (p639-640, 643)
Implement the full state machine described on those pages:
* mode enabled, >=1 punch-enabled track -> button lit "engaged/secondary" (PT: solid blue);
* mode enabled + transport record-armed, no punch-enabled tracks -> flashing dim/record;
* >=1 punch-enabled track and armed -> flashing secondary/record;
* any track actually recording -> solid record (PT: solid red), not flashing.
Blink on a steady wall-clock cadence (the widget is redrawn every frame; do not add a timer
thread). Two-tone rules apply: use theme roles, and differentiate the "blue" vs "red" states
with fill/outline/glyph as well as shade, so all four states read apart in both themes.

### 3. Per-track punch enable, separate from record enable (p640, 644-645)
A track can be **punch-enabled** without being **record-enabled**. Track record button states:
punch-enabled only -> solid secondary; punch + record enabled -> flashing secondary/record;
record only -> flashing record; recording -> solid record.
Modifier gestures on a track's Record Enable button (Windows bindings from the manual):
* `Start(Super)+click` -> toggle punch-enable on that track,
* `Alt+Start+click` -> toggle punch-enable on all audio tracks,
* `Alt+Start+Shift+click` -> all selected tracks,
* `Alt+click` -> punch-enable AND record-enable all tracks,
* `Alt+Shift+click` -> punch+record-enable all selected tracks.
PatchKnob currently allows only ONE armed record target at a time (`g_arrangeRecTarget`) and
refuses to re-arm while recording -- punch modes need a *set* of punch-enabled tracks. Widen
the shell's model to a set of punch-enabled track ids plus the existing single capture target
where the engine can only capture one; if multi-track simultaneous capture is impossible today,
implement the full enable/UI model, punch the one track the engine can capture, and report the
limit explicitly.

### 4. QuickPunch (p638-639)
While the transport is rolling with QuickPunch on, clicking transport Record punches IN on all
record-enabled tracks without stopping; clicking again punches OUT. Up to 200 running punches
in one pass. All punches in a pass come from ONE continuous underlying capture (a whole-file
clip); each punch becomes its own clip cut from it, and the parent whole-file clip is kept so
the user can later Trim the head/tail open to reveal material recorded in the background.
Non-destructive: the punched range must be partitioned out of the existing playlist
(`audio_app_project_partition_track`) and the punch clip dropped in.

### 5. TrackPunch (p640-642)
Nondestructive, per-track. With TrackPunch on and the transport rolling, clicking an individual
track's Record Enable button punches that track in / out mid-pass, without interrupting
playback or the pass. Clicking transport Record punches in/out on all punch-enabled tracks at
once. Support: punch in on individual tracks, punch in on multiple tracks simultaneously, and
"start recording on all tracks then punch out and back in".

### 6. DestructivePunch (p642-647)
Destructive per-track punch into a single contiguous file per track, fixed **10 ms linear
crossfade** at each in and out point, no new clips created, up to 200 running punches.
Requirements/support commands:
* a track is DP-eligible only if it holds a contiguous audio file starting at sample 0 whose
  length >= the **DestructivePunch File Length** preference;
* **Prepare DPE Tracks** command (Options menu equivalent): consolidate audio on all
  DP-enabled tracks from session start to the DestructivePunch File Length, rendering any clip
  gain != 0 dB into the audio and resetting clip gain to 0 dB;
* refuse (with a clear status message) to DP-enable a track that does not meet the requirements
  and tell the user which remedy applies.

### 7. Preferences (p636-637, 640)
Add these to the app's preferences/settings surface (find how PatchKnob persists prefs --
`src/optionsfile.*` / `userfile.*` / project I/O -- and follow it):
* **QuickPunch/TrackPunch Crossfade Length** (ms, default 10, 0 = no written crossfades).
  When non-zero: write a *pre*-crossfade at punch-in (up to, not into, the punched clip) and a
  *post*-crossfade at punch-out. Regardless of the setting, always apply a 4 ms monitor-only
  crossfade that is NOT written to disk, so entering/leaving record does not click.
* **DestructivePunch File Length** (minutes/seconds).
* **Transport RecordLock** -- transport stays record-armed when the transport stops
  (auto-disabled and greyed when Destructive record mode is on).
* **Audio Track RecordLock** -- record-enabled tracks stay armed when playback/record stops;
  when off they disarm on stop (digital-dubber behaviour).
* **Mute Record-Armed Tracks While Stopped** -- Foley workflow (p649).

### 8. Clip / file naming for punches (p635)
Parent whole-file clip per pass: `Name_01`; punches inside it `Name_01-01`, `Name_01-02`, ...;
the next pass increments the first pair: `Name_02-01`, ... Auto-created punch clips should be
distinguishable from user-defined clips in whatever clip listing PatchKnob has.

Out of scope: MachineControl / 9-pin remote arming, timecode-chase RecordLock for dailies,
HDX "in-the-box" cascading, VCA groups (p647-650) -- unless PatchKnob already has the
underlying feature, in which case wire it up.
