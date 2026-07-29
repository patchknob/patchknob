//----------------------------------------------------------------------------
//  PatchKnob -- Sampler instrument (Buzz/Unwieldy-backed IPluginInstance).
//
//  Bridges the ported Buzz machine (fuzzpilz Unwieldy, driven by BuzzMachineHost)
//  into the app's engine as an ordinary IPluginInstance -- so it drops into a
//  PluginNode exactly where a VST instrument would, its params surface in the
//  tracker FX picker, and MIDI notes trigger sample playback.
//
//  This header exposes ONLY the app's own types (IPluginInstance) -- none of the
//  Buzz SDK's global typedefs (byte/word/PI/...) leak into the app.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_BUZZ_SAMPLER_INSTRUMENT_H
#define PATCHKNOB_ENGINE_BUZZ_SAMPLER_INSTRUMENT_H

#include "../plugin_api.h"
#include <vector>
#include <string>

namespace PatchKnob { namespace engine {

//! A sampler zone read back from a loaded/restored instrument (for the editor to
//! re-show its zones after a project load without wiping the restored audio).
struct SamplerZoneInfo {
    int slot = 1, level = 0, rootKey = 60, loKey = 0, hiKey = 127, loVel = 0, hiVel = 127;
    bool noteOffLayer = false, keyToPitch = true, velToVol = true;
    int overlapMode = 0, sampleRate = 48000, loopStart = 0, loopEnd = 0;
    bool loop = false, stereo = false;
    int numFrames = 0;
    std::string name;
    std::vector<float> pcm;   // interleaved -1..1
};

//! Create the Unwieldy-backed sampler.  Caller owns it (delete via release()+delete
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

//! Clear a wavetable slot.
void sampler_clear_slot(IPluginInstance* inst, int slot);

//! Set a modulation envelope (env 0=amp/vel, 1=pitch, 2=cutoff, 3=resonance,
//! 4=pan).  The editor passes dense linear points approximating its curves:
//! x/y are on the Buzz 0..65535 axes, flags carries EIF_SUSTAIN on the hold point.
void sampler_set_envelope(IPluginInstance* inst, int env,
                          const unsigned short* xs, const unsigned short* ys,
                          const int* flags, int count);

//! Read back a restored instrument's zones + whether it carries any envelope, so
//! the shell can rebuild its editor state after a load instead of pushing empty
//! state over the restored samples/envelopes.
int  sampler_zone_count(IPluginInstance* inst);
bool sampler_get_zone(IPluginInstance* inst, int index, SamplerZoneInfo& out);
bool sampler_has_envelopes(IPluginInstance* inst);

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_BUZZ_SAMPLER_INSTRUMENT_H
