//----------------------------------------------------------------------------
//  audio_app.h  -- seq24 Windows port audio/VST subsystem (the DAW engine glue).
//
//  Owns the AudioEngine + master MixerGraph + PluginHost and bridges the MIDI
//  sequencer to hosted VST instruments:
//
//    perform output thread --(mastermidibus::play)--> audio_app_route_midi()
//        --> lock-free FIFO --> audio thread --> MixerGraph track instrument
//
//  Track model: seq24 output "bus" index N maps 1:1 to MixerGraph track N.
//  Assign a VST instrument to a track with audio_app_set_track_instrument().
//
//  Kept as light C-style entry points (plus a few typed ones) so seq24.cpp /
//  midibus.cpp can call in without pulling engine headers.  PluginDescriptor et
//  al. are only forward-declared here.
//----------------------------------------------------------------------------
#ifndef SEQ24_AUDIO_APP_H
#define SEQ24_AUDIO_APP_H

namespace seq24 { namespace engine {
    struct PluginDescriptor;
    class  IPluginInstance;
    class  MixerGraph;
    class  PluginHost;
    class  AudioEngine;
}}

namespace seq24 { namespace app {

static const int AUDIO_APP_MAX_TRACKS = 32;   // == c_maxBuses

// --- lifecycle (message thread) --------------------------------------------
bool audio_app_init();       // open device, 32-track master graph, start audio
void audio_app_shutdown();
bool audio_app_running();

// --- engine accessors (message thread; UIs use these) ----------------------
seq24::engine::MixerGraph*  audio_app_graph();
seq24::engine::PluginHost*  audio_app_host();
seq24::engine::AudioEngine* audio_app_engine();

// --- per-track plugins (message thread) ------------------------------------
// Instantiate + prepare the descriptor and attach it as track's instrument
// (replacing any existing one) / append it to the track's insert FX chain.
bool audio_app_set_track_instrument(int track, const seq24::engine::PluginDescriptor& desc);
bool audio_app_add_track_fx       (int track, const seq24::engine::PluginDescriptor& desc);
bool audio_app_track_has_instrument(int track);

// --- realtime MIDI routing (sequencer output thread) -----------------------
// Lock-free enqueue of one raw channel message for a track's instrument.
// Drained by the audio thread each block. status already includes the channel.
void audio_app_route_midi(int track, unsigned char status,
                          unsigned char d1, unsigned char d2);

// --- realtime VST parameter routing (sequencer / UI thread) ----------------
// Lock-free enqueue of a normalized (0..1) parameter change for a track's
// instrument, delivered as an engine ParamChange on the next audio block.
// This is what the tracker FX-command columns and automation lanes emit.
void audio_app_route_param(int track, unsigned int paramId, float value);

// --- VST parameter introspection (message thread) --------------------------
// Enumerate a track instrument's parameters so the UI / tracker can map them.
int  audio_app_track_param_count(int track);
bool audio_app_track_param_info(int track, int index, unsigned int* outId,
                                char* nameBuf, int nameBufLen, float* outDefault);

// --- diagnostics ------------------------------------------------------------
// Load a VST from an absolute path onto track 0, fire a note, and measure the
// master peak for ~400ms.  Returns the peak.  Used by the SEQ24_AUDIO_SELFTEST
// startup path to prove the full sequencer->VST->audio chain end to end.
float audio_app_selftest(const char* vst_path);

}} // namespace seq24::app

#endif // SEQ24_AUDIO_APP_H
