# Task — per-zone sampler parameters + full editor GUI

## Why
The built-in sampler's zone model is close to SoundFont 2's, which is what makes it
the right engine for soundfont patches — but three things are **instrument-global**
that SF2 defines **per zone**, and a fourth is missing entirely:

| SF2 (per zone) | PatchKnob today |
|---|---|
| `volEnv` (delay/attack/hold/decay/sustain/release) | `envs_[0]` — one amp envelope per INSTRUMENT (`samplerinstrument.cpp:466,648`) |
| `modEnv` + `modEnvToPitch` / `modEnvToFilterFc` | `envs_[1..3]` — global |
| `initialFilterFc` / `initialFilterQ` | global params only |
| `coarseTune` / `fineTune` / `scaleTuning` | only `rootKey` per zone |
| `pan`, `initialAttenuation` | global |
| `exclusiveClass` (drum choke group) | no equivalent |

The practical consequence: **a drum kit cannot be expressed.** Kick, snare and closed
hat each carry their own decay and release; with one shared amp envelope they all get
whichever imported last — a choked kick or a ringing hat. Same for a piano whose low
zones need a longer release than its top octave.

These are wanted for the sampler in its own right. SF2 import is simply the thing
that will consume them, so **match SF2 semantics where a choice exists** — that is
what makes our format a superset of SF2 rather than merely similar to it.

## Scope

### 1. Engine — `src/engine/sampler/{sampler_instrument.h,samplerinstrument.cpp}`
Add to the per-zone model (and to `SamplerZoneInfo` so the editor can read it back):
* **Amp envelope**, per zone: delay, attack, hold, decay, sustain (level), release.
* **Mod envelope**, per zone: same six stages + `modEnvToPitch` (cents) and
  `modEnvToFilterFc` (cents).
* **Filter**, per zone: low-pass cutoff (Hz) + resonance (dB). Cutoff 0 = no filter.
* **Tuning**, per zone: `coarseTune` (semitones), `fineTune` (cents),
  `scaleTuning` (cents per key; 100 = normal, 0 = fixed pitch — drum zones use this).
* **Level/position**, per zone: `pan` (-1..+1), `attenuation` (dB, positive = quieter).
* **`exclusiveClass`** (int, 0 = none): a note-on in a zone with a non-zero class
  immediately cuts every other sounding voice of the same class **on the same
  instrument**. This is how closed hi-hats choke open ones.

Rules:
* A zone with no envelope of its own **falls back to the instrument-global envelope**,
  so every existing project and the current editor behave exactly as they do now.
* Per-zone envelopes must interact correctly with the existing per-column voice
  allocation — releasing a voice in column 2 must not touch column 1's.
* Extend the state blob to **v5**; v3 and v4 blobs must still load (see the existing
  version handling at `samplerinstrument.cpp:384`).
* `process()` stays realtime-safe: no allocation, no locking, no file I/O.

### 2. C API — `src/audio_app.{h,cpp}`
Extend the `audio_app_sampler_*` surface so the shell can set and read the new
per-zone fields (`main.cpp:2616` already walks `zone_count`/`get_zone` to rebuild
editor state after a load — that path must carry the new fields too). Keep existing
signatures source-compatible; add new entry points rather than changing old ones.

### 3. GUI — `sdlui/views/sampler_editor/sampler_editor_view.cpp` — **all of it editable**
Every field above gets a real control, per selected zone:
* a **zone list / keyboard map** showing key and velocity ranges, with the selected
  zone's parameters in an inspector panel;
* an **envelope editor** per zone for both amp and mod envelopes (the view already
  has envelope editing for the global ones — extend it to a per-zone target with a
  clear "using instrument default / override" state, since falling back is the
  default and must stay visible);
* **filter** cutoff + resonance, **tuning** coarse/fine/scale, **pan**,
  **attenuation**, **exclusive class**, root key, key range, velocity range, loop
  mode and loop points;
* modulation destinations for the mod envelope (to pitch, to cutoff).

Two-tone rules apply: every colour from `ui::theme()`, so it flips with Light/Midnight.

## Constraints
* **Never run a git command that writes history** (no add/commit/branch/stash).
* **Cross-platform**: the Linux build and the Windows MinGW build. No Linux-only APIs.
* **Do not launch or drive the GUI.** Verify by inspection, by the build, and by
  headless tests. `src/engine/sampler/sampler_declick_test.cpp` is the precedent for
  an engine-level test; add cases for envelope fallback, per-zone release, exclusive-
  class choke, and a v4 blob still loading.
* **Files you own**: `src/engine/sampler/**`, `sdlui/views/sampler_editor/**`,
  `src/audio_app.{h,cpp}`, `sdlui/main.cpp`, and the CMakeLists needed to build them.
  **Do NOT touch** `src/engine/pk/**` or `src/engine/sf2/**` — the plugin format and
  the SoundFont reader are being written in parallel and will consume your model.
* Build must pass: `cmake --build /home/lain/vibe/patchknob/build-linux --target PatchKnob -j$(nproc)`
* The arrange selftest must still pass 82/82 (standalone harness built from
  `sdlui/views/arrange/CMakeLists.txt`, run with `--selftest` under `SDL_VIDEODRIVER=dummy`).
* If you hit an account session limit, stop at a building state and say where you got to.

---

## PINNED API CONTRACT (do not change unilaterally)

The backend and the GUI are being built **in parallel** against this contract. It is
authoritative for both. If you believe a signature is wrong, **stop and report it** —
do not "fix" it on your side, because the other agent is compiling against it.

```c
// --- src/engine/sampler/sampler_instrument.h -----------------------------
//! One envelope stage set.  Times in SECONDS, sustain is a LEVEL 0..1.
//! `enabled == 0` means this zone has no envelope of its own and falls back to
//! the instrument-global envelope -- the default, and what every pre-v5 project
//! restores to.
struct SamplerZoneEnv {
    float delay = 0.f, attack = 0.f, hold = 0.f, decay = 0.f;
    float sustain = 1.f, release = 0.f;
    int   enabled = 0;
};

// SamplerZoneInfo gains (appended after the existing fields so positional
// aggregate init of older code keeps compiling):
//     SamplerZoneEnv ampEnv, modEnv;
//     float cutoffHz = 0.f;         // 0 = no filter
//     float resonanceDb = 0.f;
//     int   coarseTune = 0;         // semitones
//     int   fineTune = 0;           // cents
//     int   scaleTuning = 100;      // cents per key; 0 = fixed pitch (drums)
//     float pan = 0.f;              // -1..+1
//     float attenuationDb = 0.f;    // positive = quieter
//     int   exclusiveClass = 0;     // 0 = none; non-zero cuts same-class voices
//     float modEnvToPitchCents = 0.f, modEnvToFilterCents = 0.f;

// --- src/audio_app.h ------------------------------------------------------
// `env`: 0 = amplitude, 1 = modulation.  Passing a null / disabled env clears
// the override so the zone falls back to the instrument-global envelope.
void audio_app_sampler_set_zone_env     (int node,int slot,int level,int env,
                                         const PatchKnob::engine::SamplerZoneEnv* e);
bool audio_app_sampler_get_zone_env     (int node,int slot,int level,int env,
                                         PatchKnob::engine::SamplerZoneEnv* out);
void audio_app_sampler_set_zone_filter  (int node,int slot,int level,
                                         float cutoffHz,float resonanceDb);
void audio_app_sampler_set_zone_tuning  (int node,int slot,int level,
                                         int coarse,int fine,int scaleTuning);
void audio_app_sampler_set_zone_level   (int node,int slot,int level,
                                         float pan,float attenuationDb);
void audio_app_sampler_set_zone_exclusive(int node,int slot,int level,int exclusiveClass);
void audio_app_sampler_set_zone_modroute(int node,int slot,int level,
                                         float toPitchCents,float toFilterCents);
```

`slot`/`level` address a zone exactly as `sampler_load_sample_ex` already does.
Every setter is message-thread only; the engine takes its own lock.
