# Common brief -- porting Pro Tools Reference Guide ch.27-32 features into PatchKnob

## Source material (already extracted, in this repo)
* `docs/protools/chapters_27-32.txt` -- full text of chapters 27-32 (two-column layout preserved).
* `docs/protools/pages/pNNN.txt` -- same text, one file per PDF page.
* `docs/protools/images/pt-NNN-IIII.png` -- all 212 figures/screenshots from those pages.
* `docs/protools/README.md` -- page-number map + an index of which figures sit on which page.

**Read the manual text for your chapters before writing code, and LOOK at the figures for the
UI you are building** (the Read tool renders PNGs). The figures are the design reference for
layout: e.g. `pt-718-106.png` is the whole Edit window (mode buttons, tool strip, zoom presets,
counters, Grid/Nudge fields, Universe strip), `pt-743-148.png` is the Fade Out dialog,
`pt-674-037.png` is the Grid value pop-up menu.

## The target app
PatchKnob: a C++17 SDL DAW. Build (must pass before you are done):

    cmake --build /home/lain/vibe/patchknob/build-linux --target PatchKnob -j$(nproc)

Key places:
* `sdlui/views/arrange/arrange_view.{h,cpp}` -- the arrangement/Edit window: track headers,
  ruler + tool strip, clip canvas, audio regions (Ardour-style position/source/length model),
  clip fades, snap, zoom, selection, context menus. ~5400 lines; read the header first, it
  documents the model carefully.
* `sdlui/main.cpp` -- the shell: wires the arrange view's `std::function` hooks to the engine,
  transport, record path, project I/O.
* `sdlui/transport_bar.{h,cpp}` -- transport bar widget (play/stop/rec/loop, BBT clock, tempo).
* `sdlui/gui.h` -- the two-tone widget toolkit (`ui::Widget`, `ui::App`, `ui::theme()`).
* `src/audio_app.{h,cpp}` -- engine facade: transport, tick<->sample, record arm, track capture,
  project audio regions (`audio_app_project_set_region/_set_fades/_partition_track` etc.).
* `src/perform.{h,cpp}`, `src/sequence.{h,cpp}` -- seq24-derived sequencer core (MIDI patterns,
  triggers, ticks; 192 PPQN).
* `src/engine/audioclip/`, `src/engine/automation/`, `src/engine/graph/` -- audio clip player,
  automation, mixer graph.

## House rules (non-negotiable)
1. **Do not run any git command that writes history** (no `add`, `commit`, `branch`, `stash`).
   Read-only git (`status`, `diff`) is fine. The user has said: no git until the project is done.
2. **Cross-platform**: PatchKnob ships a Linux build and a Windows (MinGW64/MSYS2) build. Put
   fixes/features in shared code. If you touch anything near `#ifdef _WIN32` / `__linux__`,
   make sure both sides work. No Linux-only APIs.
3. **Strictly two-tone theme**: every colour must come from `ui::theme()` so it flips with the
   Light/Midnight mode. Where Pro Tools says "blue" / "red", map it to distinct theme roles +
   shape/blink so the four states stay distinguishable without literal colour.
4. **No fake features.** If the engine genuinely cannot do something (e.g. no hardware audio
   input capture path), implement everything around it honestly against what does exist and
   say so in your final report + a code comment. Never simulate/stub behaviour that pretends
   to work.
5. Match the surrounding code's style: comment density, naming, `//!` doc comments, the
   "one definition of where a clip is" discipline in arrange_view.
6. Keep the app running: build after each meaningful chunk, and don't leave the tree broken.

## Definition of done
Every feature listed in your task file is implemented and reachable from the UI (button, menu,
key command, or context menu), the Linux build compiles clean of new warnings, and your final
report lists: what you implemented, where, what key/mouse commands it is on, and anything you
deliberately could not do (with the reason).
