# Task — the app bogs down with large soundfonts / large WAVs

## The report
Loading a large soundfont (or several), or large WAVs anywhere, makes the program
sluggish to the point of being unusable. This is a **regression** — recent work
added per-zone sampler parameters, shared sample buffers, SoundFont import, and a
soundfont browser, so suspect those first, but do not assume: measure.

## Reproduction material is real and on this machine
    /home/lain/Downloads/HQ Orchestral Soundfont Collection v3.0.sf2   (499 MB, 195 presets, 13228 zones)
    /home/lain/Downloads/Live HQ Natural SoundFont GM 2.sf2            (798 MB, 186 presets, 13083 zones)
The worst preset in both is a ~2976-zone concert grand over ~192 distinct samples.

## MEASURE BEFORE YOU CHANGE ANYTHING
A guess that lands on the wrong bottleneck costs more than the profiling does.
- Time the phases separately: parse, PCM decode, zone loading, first note-on,
  steady-state render, UI redraw.
- `perf record` / `perf report` is available. So is plain `std::chrono`
  instrumentation in a scratch harness.
- Prefer a HEADLESS harness over the GUI (do not launch the GUI — see rules).
  The existing headless harnesses are the model:
  `src/engine/sampler/sampler_declick_test.cpp` (`-DPATCHKNOB_SAMPLER_TESTS=ON`),
  `src/engine/sf2/sf2_reader_test.cpp` (`-DPATCHKNOB_SF2_TESTS=ON`).
- **Report numbers before and after.** "Feels faster" is not a result.

## Known suspicious shapes (starting points, not conclusions)
- `SamplerInstrument::zoneForMidi()` is a LINEAR scan over every zone, per
  note-on. With 2976 zones loaded that is 2976 range tests per key press, and
  the voice allocator, the exclusive-class choke scan and voice stealing each
  walk their own lists on top of it.
- The import path calls seven `sampler_set_zone_*` setters per zone, each taking
  the instrument mutex and each calling `sanitizeZoneParams()`. For 2976 zones
  that is ~21,000 lock acquisitions and sanitise passes for one patch.
- Per-zone state is copied into the shell (`g_samplerZones`) as well as the
  engine; check whether PCM is being duplicated there, and whether
  `sampler_get_zone` (which copies PCM) is being called where
  `sampler_get_zone_meta` (which does not) would do.
- Waveform peak building and zone-list drawing are per-frame costs that scale
  with zone count and sample length.
- Project save/load may be writing or re-reading very large buffers.

## Rules (all agents)
* **Never run a git command that writes history** (no add/commit/branch/stash).
* **Cross-platform**: Linux + Windows MinGW. No Linux-only APIs in shipped code
  (profiling scaffolding you delete afterwards is fine).
* **Do not launch or drive the GUI.** Another agent may be sharing this desktop.
  Verify by headless harness, by build, and by measurement.
* `process()` and anything else on the audio thread stays realtime-safe: no
  allocation, no locking, no I/O.
* Keep the existing suites green:
  - sampler: `-DPATCHKNOB_SAMPLER_TESTS=ON`, all checks (36 at time of writing)
  - soundfont: `-DPATCHKNOB_SF2_TESTS=ON`, all checks
  - arrange: standalone harness from `sdlui/views/arrange/CMakeLists.txt`,
    `--selftest` under `SDL_VIDEODRIVER=dummy`, 82/82
* Build must pass: `cmake --build /home/lain/vibe/patchknob/build-linux --target PatchKnob -j$(nproc)`
* **Stay inside your own files.** Four agents run in parallel; editing another's
  files will collide. If a fix belongs in someone else's file, write the exact
  proposed patch in your report instead of applying it.
* If a pinned/public signature must change, say so in the report rather than
  changing it unilaterally.

## Ownership split
| Agent | Owns | Focus |
|---|---|---|
| **A — sampler engine** | `src/engine/sampler/**` | note-on cost with thousands of zones (zone lookup, voice allocation, choke scan, stealing), per-sample render cost, lock granularity in the setters, `sanitizeZoneParams` cost |
| **B — soundfont pipeline** | `src/engine/sf2/**` | parse, PCM decode, interning, and the import loop's cost per zone; batching; avoiding repeated parses of the same file |
| **C — sampler & browser UI** | `sdlui/views/sampler_editor/**`, `sdlui/views/sample_slot/**` | per-frame cost that scales with zone count or sample length: zone lists, waveform peaks, browser scans/caches, redraw triggers |
| **D — audio clips & arrangement** | `src/engine/audioclip/**`, `sdlui/views/arrange/**` | large WAVs anywhere: decode, peak/overview building, waveform drawing caches, clip player behaviour with long files |

Each agent: diagnose with measurements, fix what is yours, re-measure, and report
before/after numbers plus what you ruled out.
