# Task — SoundFont import into the sampler + drag payload + expandable browser node

Three pieces of one workflow: **expand a `.sf2` in the disk browser like a folder,
drag a preset out of it into the sampler window, and have every zone, sample and
parameter land in the editor, ready to edit.**

Everything below already exists and works — do not rebuild any of it:

* `src/engine/sf2/sf2_reader.{h,cpp}` — parses `.sf2`/`.sf3`. Validated against two
  real banks (499 MB and 798 MB). Headers-only parse of an 800 MB bank costs
  **8 ms / 6.4 MB**; `readPresetPcm()` loads just one patch's samples (~22 MB).
  Zone fields are already in real units (seconds, Hz, cents, dB, -1..+1 pan).
* The sampler's per-zone model — `sampler_load_sample_ex` plus the
  `sampler_set_zone_*` setters (envelopes, filter, tuning, level, exclusive class,
  mod routes). Zones now SHARE sample buffers via `sampler_load_sample_shared`
  and `SharedPcm` — see `src/engine/sampler/sampler_instrument.h`.
* The sampler editor rebuilds itself from `audio_app_sampler_zone_count`/`get_zone`
  (`sdlui/main.cpp:2616`), so once zones are loaded the editor shows them.

## PINNED API (authoritative; the two halves are built in parallel against it)

```cpp
// src/engine/sf2/sf2_to_sampler.h
namespace PatchKnob { namespace engine { namespace sf2 {

struct ImportOptions {
    //! Hard cap on zones actually loaded. 0 = no cap. A 2976-zone concert grand
    //! is legitimate but heavy; the UI may want a cheaper preview import.
    int  maxZones = 0;
    //! Drop zones whose velocity range is fully covered by an earlier zone with
    //! the same key range -- round-robin layers that cost RAM without changing
    //! the map. Off by default: it is a lossy convenience, not a correctness fix.
    bool collapseVelocityLayers = false;
};

struct ImportResult {
    int zonesLoaded    = 0;
    int zonesSkipped   = 0;   //!< over the cap, or no sample
    int samplesInterned= 0;   //!< DISTINCT buffers allocated (not zone count)
    int stereoPairs    = 0;
    std::string warning;      //!< human-readable, empty when nothing to report
};

//! Load one preset of `font` into `sampler` as sampler zones.
//! `font` may come from a headers-only read; this function pulls the PCM it
//! needs itself. Message thread only.
bool importPresetIntoSampler(const std::string& fontPath, SoundFont& font,
                             int presetIndex, IPluginInstance* sampler,
                             const ImportOptions& opts, ImportResult& out,
                             std::string& error);
}}}
```

```cpp
// sdlui/views/sample_slot/sample_browser.h -- replaces the path-only hook
struct DragPayload {
    enum Kind { File, Sf2Preset };
    Kind        kind = File;
    std::string path;          //!< the file, or the .sf2 for Sf2Preset
    int         bank = 0;      //!< Sf2Preset only
    int         program = 0;   //!< Sf2Preset only
    std::string label;         //!< display name ("Overdrive Guitar")
};
std::function<void(const DragPayload&, int x, int y)> on_drag_drop;
```

## Half A — the mapping layer (engine)

New `src/engine/sf2/sf2_to_sampler.{h,cpp}` implementing the above.

Mapping, field by field — the reader has already converted units, so this is
plumbing, not maths:

| SF2 `Zone` | sampler call |
|---|---|
| `loKey/hiKey`, `loVel/hiVel`, `rootKey`, `loopStart/loopEnd`, `loop` | `sampler_load_sample_shared(...)` |
| `ampEnv` (delay/attack/hold/decay/sustain/release) | `sampler_set_zone_env(..., env=0, ...)` |
| `modEnv` + `modEnvToPitchCents` / `modEnvToFilterCents` | `sampler_set_zone_env(..., env=1, ...)`, `sampler_set_zone_modroute` |
| `cutoffHz`, `resonanceDb` | `sampler_set_zone_filter` |
| `coarseTune`, `fineTune`, `scaleTuning` | `sampler_set_zone_tuning` |
| `pan`, `attenuationDb` | `sampler_set_zone_level` |
| `exclusiveClass` | `sampler_set_zone_exclusive` |

Three things that are NOT plumbing and must be right:

1. **Intern each distinct sample exactly once.** Build a
   `map<int sampleIndex, SharedPcm>` and pass the SAME `SharedPcm` to every zone
   that references it. Measured on a real bank, the Concert Grand preset is
   2976 zones over 192 distinct samples: interning is 83 MB, copying is 1285 MB.
   Assert this in a test.
2. **Stereo is two linked mono samples.** SF2 `sampleType` 2 = right, 4 = left,
   with `sampleLink` naming the partner. Interleave the pair into ONE stereo
   buffer and load a single stereo zone; do not load two mono zones. Guard
   against a broken link (partner index out of range, partner not the opposite
   channel, differing lengths) by falling back to mono rather than failing.
3. **Zone addressing.** Zones are addressed `(slot, level)` exactly as
   `sampler_load_sample_ex` does. Pick a scheme, document it in a comment, and
   make sure `audio_app_sampler_get_zone` reads back what you wrote.

Test: `src/engine/sf2/sf2_to_sampler_test.cpp`, headless, in the style of
`sf2_reader_test.cpp` (which BUILDS a soundfont byte by byte — reuse that
approach; do not require a real `.sf2` to be present). Cover: interning (N zones
over M samples yields M allocations), stereo pairing, every parameter arriving on
the zone via the read-back API, the zone cap, and a preset whose zones reference a
missing sample being skipped rather than crashing.

Files you own: `src/engine/sf2/**` (NOT `sf2_reader.*` — leave the reader alone),
and the CMakeLists needed to build them.

## Half B — drag payload + expandable browser node (UI)

1. **Payload.** Replace the path-only `on_drag_drop(path, x, y)`
   (`sdlui/views/sample_slot/sample_browser.h:304`, fired at
   `sample_browser.cpp:836`) with `DragPayload` as pinned. Update every call site.
   A preset is not a file, and the shell needs bank/program to route the drop.
2. **Expandable soundfont node.** In the disk browser, a `.sf2`/`.sf3` gets a `+`
   affordance and expands in place like a folder, listing its presets as
   `[bank:program] Name`. Double-clicking the file expands it too. Use
   `sf2::read(path, font, err, /*loadPcm=*/false)` — 8 ms and 6.4 MB even for an
   800 MB bank, so this is cheap, but cache the parse per path so re-expanding
   costs nothing. Use `sf2::looksLikeSoundFont()` to decide whether to show the
   affordance at all, and degrade quietly on a file that fails to parse.
3. **Drop routing.** In `sdlui/main.cpp`, a `Sf2Preset` payload dropped on the
   sampler window calls `importPresetIntoSampler(...)` and then the existing
   editor-rebuild path at `main.cpp:2616`, so the zones appear in the editor
   ready to edit. Report `ImportResult` on the status line (zones loaded, RAM
   interned, any warning). Import can take a moment for a big patch — do not
   freeze the UI without at least showing progress or a busy state.

Files you own: `sdlui/views/browser/**`, `sdlui/views/sample_slot/**`,
`sdlui/main.cpp`. Do NOT touch `src/engine/**`.

## Constraints (both halves)
* **No git commands that write history** (no add/commit/branch/stash).
* **Cross-platform**: Linux + Windows MinGW. No Linux-only APIs.
* Two-tone: every colour from `ui::theme()`.
* **Do not launch or drive the GUI.** Verify by inspection, by the build, and by
  headless tests.
* Build must pass: `cmake --build /home/lain/vibe/patchknob/build-linux --target PatchKnob -j$(nproc)`
* The arrange selftest must still pass 82/82 (standalone harness from
  `sdlui/views/arrange/CMakeLists.txt`, `--selftest` under `SDL_VIDEODRIVER=dummy`).
* If a pinned signature is genuinely wrong, STOP and report it — the other half
  is compiling against it.
