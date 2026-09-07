//----------------------------------------------------------------------------
//  PatchKnob -- native keyzone sampler instrument (IPluginInstance).
//
//  A self-contained native engine exposed as an ordinary IPluginInstance, so it drops into a
//  PluginNode exactly where a VST instrument would, its params surface in the
//  tracker FX picker, and MIDI notes trigger sample playback.
//
//  This header exposes ONLY the app's own types (IPluginInstance) -- none of the
//  Buzz SDK's global typedefs (byte/word/PI/...) leak into the app.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_BUZZ_SAMPLER_INSTRUMENT_H
#define PATCHKNOB_ENGINE_BUZZ_SAMPLER_INSTRUMENT_H

#include "../plugin_api.h"
#include <memory>
#include <vector>
#include <string>

namespace PatchKnob { namespace engine {

//! An immutable block of interleaved PCM shared by any number of zones.
//!
//! Multisampled instruments reuse one recording across velocity layers and
//! round-robins, so zones sharing a sample is the NORMAL case rather than an
//! edge case.  Measured on a real SoundFont bank, a 2976-zone Concert Grand
//! draws on 192 distinct samples (~85 MB); giving every zone its own copy costs
//! ~1.3 GB.  Sharing one CONST buffer collapses that back to the 85 MB -- and
//! const is load-bearing: no zone can mutate audio another zone is reading.
using SharedPcm = std::shared_ptr<const std::vector<float>>;

//! One envelope stage set.  Times in SECONDS, sustain is a LEVEL 0..1.
//! `enabled == 0` means this zone has no envelope of its own and falls back to
//! the instrument-global envelope -- the default, and what every pre-v5 project
//! restores to.
//!
//! Stage semantics are SoundFont 2's volEnv/modEnv: delay (silence), attack
//! (0 -> 1), hold (at 1), decay (1 -> sustain), then hold at sustain until
//! note-off, then release (from wherever the level is -> 0 over `release`).
struct SamplerZoneEnv {
    float delay = 0.f, attack = 0.f, hold = 0.f, decay = 0.f;
    float sustain = 1.f, release = 0.f;
    int   enabled = 0;
};

//! A sampler zone read back from a loaded/restored instrument (for the editor to
//! re-show its zones after a project load without wiping the restored audio).
//!
//! The per-zone SF2-parity fields are APPENDED after the original ones so any
//! positional aggregate initialisation of the old layout keeps compiling.
struct SamplerZoneInfo {
    int slot = 1, level = 0, rootKey = 60, loKey = 0, hiKey = 127, loVel = 0, hiVel = 127;
    bool noteOffLayer = false, keyToPitch = true, velToVol = true;
    int overlapMode = 0, sampleRate = 48000, loopStart = 0, loopEnd = 0;
    bool loop = false, stereo = false;
    int numFrames = 0;
    std::string name;
    std::vector<float> pcm;   // interleaved -1..1
    // ---- per-zone parameters (SF2 generator parity) ------------------------
    SamplerZoneEnv ampEnv, modEnv;
    float cutoffHz = 0.f;         // 0 = no filter
    float resonanceDb = 0.f;
    int   coarseTune = 0;         // semitones
    int   fineTune = 0;           // cents
    int   scaleTuning = 100;      // cents per key; 0 = fixed pitch (drums)
    float pan = 0.f;              // -1..+1
    float attenuationDb = 0.f;    // positive = quieter
    int   exclusiveClass = 0;     // 0 = none; non-zero cuts same-class voices
    float modEnvToPitchCents = 0.f, modEnvToFilterCents = 0.f;
};

//! Create the native keyzone sampler. Caller owns it (delete via release()+delete
//! like any IPluginInstance).  Returns null if the machine isn't registered.
IPluginInstance* create_sampler_instrument();

//! True if `inst` is one of our sampler instruments (safe to pass to the calls below).
bool sampler_is_sampler(IPluginInstance* inst);

//! Per-column routing (built-in sampler only; not part of the frozen plugin API).
//! Tell the sampler which tracker note-column a MIDI note belongs to (call just
//! before that note's note-on) so the voice it triggers is tagged with the column.
void sampler_set_note_column(IPluginInstance* inst, int note, int column);
//! Apply a tracker FX-column value to ONE note-column's voices (column<0 = global).
//! Only volume/pan/pitch are per-column; other params fall back to global.
void sampler_set_column_param(IPluginInstance* inst, int column, int paramId, float value);

//! Load a sample into a sampler's wavetable slot (1-based) as a keyrange level.
//! `interleaved` is -1..1 float (L,R,... if stereo).  rootMidiNote/loKey/hiKey are
//! MIDI notes.  Replaces whatever was in that (slot,level).
void sampler_load_sample(IPluginInstance* inst, int slot, int level,
                         const float* interleaved, int numFrames, bool stereo,
                         int rootMidiNote, int sampleRate,
                         int loopStart, int loopEnd, bool loop,
                         int loKey, int hiKey, const char* name);
void sampler_load_sample_ex(IPluginInstance* inst, int slot, int level,
                            const float* interleaved, int numFrames, bool stereo,
                            int rootMidiNote, int sampleRate,
                            int loopStart, int loopEnd, bool loop,
                            int loKey, int hiKey, int loVel, int hiVel,
                            bool noteOffLayer, bool keyToPitch, bool velToVol,
                            int overlapMode, const char* name);

//! Load a zone that SHARES an already-interned PCM buffer.
//!
//! This is the entry point an importer wants: intern each DISTINCT sample once
//! and hand the same SharedPcm to every zone that draws on it, and a preset with
//! 2976 zones over 192 samples holds 192 allocations instead of 2976 copies.
//! The two calls above are unchanged -- they copy their input into a fresh
//! shared buffer, so every existing caller keeps working untouched.
//!
//! `pcm` must be `numFrames * (stereo ? 2 : 1)` floats; the zone is dropped if
//! it is not.  The buffer is const and never written, so sharing is safe with no
//! copy-on-write dance.
void sampler_load_sample_shared(IPluginInstance* inst, int slot, int level,
                                SharedPcm pcm,
                                int numFrames, bool stereo, int rootMidiNote,
                                int sampleRate, int loopStart, int loopEnd, bool loop,
                                int loKey, int hiKey, int loVel, int hiVel,
                                bool noteOffLayer, bool keyToPitch, bool velToVol,
                                int overlapMode, const char* name);

//! The PCM buffer a zone is reading from, by zone index.  Two zones sharing a
//! sample return the SAME pointer -- which is how a caller (or a test) proves
//! that sharing actually happened rather than merely being asked for.
const float* sampler_zone_pcm_ptr(IPluginInstance* inst, int index);

//! Clear a wavetable slot.
void sampler_clear_slot(IPluginInstance* inst, int slot);

//! Remove EVERY zone under one lock.  What a full-patch replace (SF2 import)
//! needs: clearing N slots one sampler_clear_slot() at a time is O(N^2) in
//! zone moves (measured ~458 ms per re-import of a 2976-zone preset); this is
//! one lock, one voice kill, one clear (measured well under a millisecond).
void sampler_clear_all_zones(IPluginInstance* inst);

//! Set a modulation envelope (env 0=amp/vel, 1=pitch, 2=cutoff, 3=resonance,
//! 4=pan).  The editor passes dense linear points approximating its curves:
//! x/y are on the Buzz 0..65535 axes, flags carries EIF_SUSTAIN on the hold point.
void sampler_set_envelope(IPluginInstance* inst, int env,
                          const unsigned short* xs, const unsigned short* ys,
                          const int* flags, int count);

//! ---- per-zone parameters (SF2 generator parity) --------------------------
//! Every one of these addresses a zone by (slot, level) exactly as
//! sampler_load_sample_ex() does, and is a no-op if no such zone exists.
//! Message-thread only; the instrument takes its own lock.
//!
//! `env`: 0 = amplitude, 1 = modulation.  A null or disabled `e` CLEARS the
//! override so the zone falls back to the instrument-global envelope.
void sampler_set_zone_env(IPluginInstance* inst, int slot, int level, int env,
                          const SamplerZoneEnv* e);
bool sampler_get_zone_env(IPluginInstance* inst, int slot, int level, int env,
                          SamplerZoneEnv* out);
void sampler_set_zone_filter(IPluginInstance* inst, int slot, int level,
                             float cutoffHz, float resonanceDb);
void sampler_set_zone_tuning(IPluginInstance* inst, int slot, int level,
                             int coarse, int fine, int scaleTuning);
void sampler_set_zone_level(IPluginInstance* inst, int slot, int level,
                            float pan, float attenuationDb);
void sampler_set_zone_exclusive(IPluginInstance* inst, int slot, int level,
                                int exclusiveClass);
void sampler_set_zone_modroute(IPluginInstance* inst, int slot, int level,
                               float toPitchCents, float toFilterCents);

//! Read back a restored instrument's zones + whether it carries any envelope, so
//! the shell can rebuild its editor state after a load instead of pushing empty
//! state over the restored samples/envelopes.
int  sampler_zone_count(IPluginInstance* inst);
bool sampler_get_zone(IPluginInstance* inst, int index, SamplerZoneInfo& out);
// Metadata-only variant: never copies the potentially multi-gigabyte PCM.
bool sampler_get_zone_meta(IPluginInstance* inst, int index, SamplerZoneInfo& out);
bool sampler_has_envelopes(IPluginInstance* inst);

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_BUZZ_SAMPLER_INSTRUMENT_H
