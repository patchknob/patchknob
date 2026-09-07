//----------------------------------------------------------------------------
//  audio_app.h  -- PatchKnob audio/VST subsystem (the DAW engine glue).
//
//  Owns the AudioEngine + master MixerGraph + PluginHost and bridges the MIDI
//  sequencer to hosted VST instruments:
//
//    perform output thread --(mastermidibus::play)--> audio_app_route_midi()
//        --> lock-free FIFO --> audio thread --> MixerGraph track instrument
//
//  Track model: PatchKnob output "bus" index N maps 1:1 to MixerGraph track N.
//  Assign a VST instrument to a track with audio_app_set_track_instrument().
//
//  Kept as light C-style entry points (plus a few typed ones) so PatchKnob.cpp /
//  midibus.cpp can call in without pulling engine headers.  PluginDescriptor et
//  al. are only forward-declared here.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_AUDIO_APP_H
#define PATCHKNOB_AUDIO_APP_H

#include <functional>

namespace PatchKnob { namespace engine {
    struct PluginDescriptor;
    class  IPluginInstance;
    class  MixerGraph;
    class  PluginHost;
    class  AudioEngine;
    struct AudioClip;
    struct WarpMarker;
    struct SamplerZoneInfo;
    struct SamplerZoneEnv;
    namespace patch { class PatchGraph; }
}}

// The Rack modular engine lives in the GLOBAL ::rackx namespace (engine/rack).
namespace rackx { class RackEngine; }

namespace PatchKnob { namespace app {

static const int AUDIO_APP_MAX_TRACKS = 32;   // == c_maxBuses

// --- lifecycle (message thread) --------------------------------------------
bool audio_app_init();       // open device, 32-track master graph, start audio
void audio_app_shutdown();
bool audio_app_running();

// --- audio backend (host API) selection (message thread) -------------------
// ASIO / WASAPI / WDM-KS / DirectSound / MME (Windows), ALSA / JACK / Pulse
// (Linux; PipeWire via its compat layers).
int  audio_app_hostapi_count();
bool audio_app_hostapi_info(int idx, int* outIndex, char* nameBuf,
                            int nameBufLen, int* outIsCurrent);
bool audio_app_set_hostapi(int paHostApiIndex);

// --- audio OUTPUT device (sound driver) selection (message thread) ---------
int  audio_app_device_count();
bool audio_app_device_info(int idx, unsigned* outId, char* nameBuf,
                           int nameBufLen, int* outIsCurrent);
bool audio_app_set_device(unsigned deviceId);

// --- audio INPUT device selection --------------------------------------------
// The engine opens the stream DUPLEX when a capture device exists; input
// arrives on the audio thread and is served to the patcher's Audio In node
// (AudioDeviceInNode), from where the modular graph routes it anywhere --
// including into a track strip's inlet, which is what input recording taps.
int  audio_app_input_device_count();
bool audio_app_input_device_info(int idx, unsigned* outId, char* nameBuf,
                                 int nameBufLen, int* outIsCurrent);
void audio_app_set_input_device(unsigned deviceId);

// --- live MIDI input for the modular MIDI-In node (message thread) ----------
int  audio_app_midi_input_count();
bool audio_app_midi_input_info(int idx, char* nameBuf, int nameBufLen, int* outIsCurrent);
bool audio_app_patch_set_midi_input(int idx);   // idx < 0 closes

// --- hardware MIDI OUTPUT device enumeration (for MIDI-out port nodes) -------
int  audio_app_midi_output_count();
bool audio_app_midi_output_info(int idx, char* nameBuf, int nameBufLen, int* outIsCurrent);

// --- PipeWire-style patchable MIDI ports (message thread) -------------------
// Each becomes a normal patch node you wire freely.  hwDevice < 0 => a VIRTUAL
// routing point; >= 0 binds the node to that hardware MIDI in / out device.
// MIDI-In nodes emit their device's incoming events; MIDI-Out nodes send their
// patched-in events to the device (drained by a background thread).
int  audio_app_patch_add_midi_in(int hwDevice);
int  audio_app_patch_add_midi_out(int hwDevice);
bool audio_app_patch_is_hw_midi_in(int node);                       // hardware-bound MIDI-in?
void audio_app_patch_midiin_set_out_channel(int node, int ch);     // re-stamp channel (-1 passthrough)
bool audio_app_patch_set_virtual_midi_ports(int node, int ports);
int  audio_app_virtual_midi_node();
int  audio_app_default_hw_midi_in_node();
void audio_app_virtual_midi_set_ports(int inputs, int outputs);
int  audio_app_virtual_midi_inputs();
int  audio_app_virtual_midi_outputs();
void audio_app_virtual_midi_set_route(int output, int input);
int  audio_app_virtual_midi_route(int output);
int  audio_app_add_midi_track_node();
void audio_app_midi_track_monitor(int node,bool on);
void audio_app_midi_track_capture(int node,bool on);
void audio_app_midi_track_set_routing(int node,int input,int output);
int  audio_app_midi_track_input(int node);
int  audio_app_midi_track_output(int node);
int  audio_app_midi_track_drain(int node,long* ticks,unsigned char* status,
                                unsigned char* d1,unsigned char* d2,int cap);

// --- buffer size / latency (message thread) --------------------------------
unsigned audio_app_buffer_size();
bool     audio_app_set_buffer_size(unsigned frames);

// --- engine accessors (message thread; UIs use these) ----------------------
PatchKnob::engine::MixerGraph*  audio_app_graph();
PatchKnob::engine::PluginHost*  audio_app_host();
PatchKnob::engine::AudioEngine* audio_app_engine();
PatchKnob::engine::patch::PatchGraph* audio_app_patch_graph();

// --- per-track plugins (message thread) ------------------------------------
// Instantiate + prepare the descriptor and attach it as track's instrument
// (replacing any existing one) / append it to the track's insert FX chain.
bool audio_app_set_track_instrument(int track, const PatchKnob::engine::PluginDescriptor& desc);
bool audio_app_add_track_fx       (int track, const PatchKnob::engine::PluginDescriptor& desc);
bool audio_app_track_has_instrument(int track);

// --- realtime MIDI routing (sequencer output thread) -----------------------
// Lock-free enqueue of one raw channel message for a track's instrument.
// Drained by the audio thread each block. status already includes the channel.
// `tick` is the event's ABSOLUTE musical due-time in c_ppqn (192) sequencer
// ticks; the audio thread converts it to a sample offset within the block it
// falls in (holding back future events).  tick = -1 means "due now" (UI
// preview clicks) and lands at the start of the next block.
// `column` is the tracker note-column (VOICE) the event belongs to, or -1 when
// untagged (live MIDI in, piano-roll edits, UI preview, held-note releases).
// It MUST ride the event: a note-on and its note-off identify a voice by
// (column, pitch), and the column cannot be recovered downstream -- the sampler
// previously guessed it from a pitch-keyed table, so one pitch played from two
// columns collapsed onto a single voice and the notes cut each other off.
void audio_app_route_midi(int track, unsigned char status,
                          unsigned char d1, unsigned char d2,
                          long long tick = -1, int column = -1);

// --- per-callback block publication (message/MIDI threads) ------------------
// Snapshot of the most recent audio block: its start sample position, the
// QueryPerformanceCounter value when the callback began, and QPC ticks per
// audio sample.  Lets MIDI I/O threads convert sample<->wall time (timestamped
// hardware MIDI output pacing, input stamping).  False until audio runs.
bool audio_app_block_pub(long long* blockSample, long long* blockQpc,
                         double* qpcPerSample);

// --- realtime VST parameter routing (sequencer / UI thread) ----------------
// Lock-free enqueue of a normalized (0..1) parameter change for a track's
// instrument, delivered as an engine ParamChange on the next audio block.
// This is what the tracker FX-command columns and automation lanes emit.
void audio_app_route_param(int track, unsigned int paramId, float value);
// Schedule a parameter change on the transport timeline.  The audio callback
// converts tick to a sample offset, so automation is not quantized to either
// the UI refresh or the start of an audio block.
void audio_app_route_param_at_tick(int track, unsigned int paramId, float value,
                                   long long tick);

// --- MIDI panic (any thread) ------------------------------------------------
// Release every note the engine has delivered and is still holding, then discard
// whatever is queued ahead of the playhead.  The audio thread does the work on
// its next block, emitting the note-offs (plus CC123/CC120 per touched channel)
// on the very paths the note-ons went out on -- so soft instruments AND external
// hardware wired downstream both go quiet.  Called automatically on every
// transport locate and stop; call it directly for an explicit panic button.
void audio_app_midi_panic();

// --- VST parameter introspection (message thread) --------------------------
// Enumerate a track instrument's parameters so the UI / tracker can map them.
int  audio_app_track_param_count(int track);
bool audio_app_track_param_info(int track, int index, unsigned int* outId,
                                char* nameBuf, int nameBufLen, float* outDefault);

// --- modular patch graph (message thread) ----------------------------------
// An alternate render core: a PatchGraph the modular patchbay UI drives.  While
// audio_app_set_modular(true), the audio thread renders the patch graph instead
// of the fixed MixerGraph.  Node ids are the engine PatchGraph NodeIds (as int).
// The graph always has a default audio-output node and a MIDI-input node
// (audio_app_patch_out_node / _midi_in_node) to patch to/from.
int  audio_app_patch_add_plugin(const PatchKnob::engine::PluginDescriptor& desc);
int  audio_app_patch_add_empty_plugin();              // empty plugin slot (Choose Plugin fills)
// Native keyzone SAMPLER instrument node. audio_app_sampler_load loads a sample
// (interleaved float -1..1) into a node's wavetable slot as a keyrange level.
int  audio_app_patch_add_sampler();
bool audio_app_sampler_load(int node, int slot, int level,
                            const float* interleaved, int numFrames, int stereo,
                            int rootMidiNote, int sampleRate,
                            int loopStart, int loopEnd, int loop,
                            int loKey, int hiKey, const char* name);
bool audio_app_sampler_load_ex(int node, int slot, int level,
                               const float* interleaved, int numFrames, int stereo,
                               int rootMidiNote, int sampleRate,
                               int loopStart, int loopEnd, int loop,
                               int loKey, int hiKey, int loVel, int hiVel,
                               int noteOffLayer, int keyToPitch, int velToVol,
                               int overlapMode, const char* name);
void audio_app_sampler_clear(int node, int slot);   // drop all levels in a slot
// Push a modulation envelope (env 0=amp,1=pitch,2=cutoff,3=resonance,4=pan) as
// dense linear points (Buzz 0..65535 x/y axes; flags carries EIF_SUSTAIN).
void audio_app_sampler_set_env(int node, int env, const unsigned short* xs,
                               const unsigned short* ys, const int* flags, int count);
// Read back a restored sampler's zones (so the editor can re-show them after a
// project load without pushing empty state over the restored samples).
int  audio_app_sampler_zone_count(int node);
bool audio_app_sampler_get_zone(int node, int index, PatchKnob::engine::SamplerZoneInfo& out);
bool audio_app_sampler_get_zone_meta(int node, int index, PatchKnob::engine::SamplerZoneInfo& out);
bool audio_app_sampler_has_envelopes(int node);
// --- per-zone parameters (SoundFont 2 generator parity) ---------------------
// `slot`/`level` address a zone exactly as audio_app_sampler_load_ex() does.
// Every one of these is message-thread only; the engine takes its own lock, and
// mutates the zone in place so sounding voices are never disturbed.  The new
// fields also ride the audio_app_sampler_get_zone()/_meta read-back, so the
// editor rebuilds them after a project load along with the key ranges.
//
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
int  audio_app_patch_add_builtin(const char* kind);   // includes "mono2stereo"

// --- patchable mixer node (N stereo channels; patch mixers for submixes) ----
int   audio_app_patch_add_mixer(int channels);
int   audio_app_mixer_channels(int node);
void  audio_app_mixer_set_channels(int node, int n);
float audio_app_mixer_gain(int node, int ch);
void  audio_app_mixer_set_gain(int node, int ch, float g);
bool  audio_app_mixer_mute(int node, int ch);
void  audio_app_mixer_set_mute(int node, int ch, bool mute);
float audio_app_mixer_vu(int node, int ch);
float audio_app_mixer_vu_left(int node, int ch);
float audio_app_mixer_vu_right(int node, int ch);
float audio_app_mixer_pan(int node, int ch);
void  audio_app_mixer_set_pan(int node, int ch, float p);
float audio_app_mixer_master_gain(int node);
void  audio_app_mixer_set_master_gain(int node, float g);
bool  audio_app_mixer_master_mute(int node);
void  audio_app_mixer_set_master_mute(int node, bool mute);
float audio_app_mixer_master_vu(int node);
float audio_app_mixer_master_vu_left(int node);
//! Latch every mixer meter for this UI frame.  The audio thread accumulates
//! peaks continuously; this moves them into the readable values and resets the
//! accumulators.  Call it ONCE per frame, before anything draws a meter --
//! without it the meters read stale, and calling it per-reader would let one
//! view steal another's peak.
void audio_app_meters_latch();
float audio_app_mixer_master_vu_right(int node);

// --- Pure Data node (embedded libpd; SDL Pd editor drives it) ---------------
int  audio_app_patch_add_pd();
bool audio_app_pd_load(int node, const char* path);       // open a .pd patch
bool audio_app_pd_path(int node, char* buf, int buflen);  // current patch path
int  audio_app_pd_io_sig(int node);                       // adc~/dac~ sig: inCh*1000+outCh (-1 if not Pd)
void audio_app_pd_send_float(int node, const char* recv, float v);  // live -> GUI atom receive symbol
void audio_app_pd_send_bang (int node, const char* recv);           // live bang -> receive symbol
bool audio_app_pd_set_text(int node, const char* text);   // load patch from in-memory text (reloads)
void audio_app_pd_store_text(int node, const char* text); // store text WITHOUT reloading libpd
const char* audio_app_pd_get_text(int node);              // the node's in-memory patch text
void audio_app_pd_set_dsp(int node, int on);              // DSP switch (compute audio) on/off
int  audio_app_pd_dsp(int node);                          // current DSP state (1/0)
void audio_app_pd_subscribe(int node, const char* sendSym);   // bind a GUI atom's send symbol
void audio_app_pd_clear_gui_binds(int node);                  // unbind all GUI subscriptions
int  audio_app_pd_poll_gui(int node, char* recv, int recvcap, float* val, int* isBang); // drain 1 feedback

// --- VCV-Rack-style modular node (SDL rack editor drives its engine) --------
int  audio_app_patch_add_rack();
::rackx::RackEngine* audio_app_rack_engine(int node);     // the node's live patch
void audio_app_rack_set_poly(int node, int voices);       // 1..16 MIDI voices
int  audio_app_rack_poly(int node);

// --- embedded Csound (.csd) node --------------------------------------------
int  audio_app_patch_add_csound();                        // new CsoundNode -> node id
bool audio_app_patch_is_csound(int node);
const char* audio_app_patch_csound_text(int node);        // current CSD text (copy it)
void audio_app_patch_csound_set_text(int node, const char* text);  // editor buffer (no compile)
bool audio_app_patch_csound_recompile(int node);          // compile stored CSD; republish ports
const char* audio_app_patch_csound_error(int node);       // last compile error ("" if ok)

// DSP load 0..1 (fraction of the audio block's real-time budget consumed).
float audio_app_cpu_load();

// --- device-loss detection + recovery (message thread) ----------------------
// The engine flags an uninvited stream finish (device unplugged / lost).  The
// UI polls audio_app_device_lost() on its idle tick and calls
// audio_app_try_recover() to reopen on the default device.
bool audio_app_device_lost();
bool audio_app_try_recover();

// --- master mixer: singleton hub with dynamic per-track PORTS -----------------
int  audio_app_master_mixer_node();          // the master MasterMixerNode id
int  audio_app_master_add_track(int isMidi); // add a track's inlet+outlet pair; index
void audio_app_master_remove_track(int idx); // remove a track's ports
// Auto-wire an instrument node into master-mixer track `track`'s audio inlet
// (and MidiIn->instrument); makes the per-track mixer live. Returns inlet or -1.
int  audio_app_master_connect_instrument(int track, int instrNode);
// ---- virtual MIDI as the inter-track routing fabric ------------------------
//! Is anything patched INTO virtual MIDI input `input`?  A track armed on an
//! unpatched input records nothing -- there is no hidden hardware side channel.
bool audio_app_virtual_midi_input_is_patched(int input);
//! Which virtual input currently feeds `destNode` (via a routed virtual out),
//! or -1.  This is what the track's MIDI-In readout reflects.
//! Wire virtual `input` -> `destNode`'s first MIDI in, allocating (or reusing) a
//! virtual output and routing it.  Pass input < 0 to unwire.  Returns the
//! virtual output used, or -1.  The cable is a real graph edge, so it shows up
//! in the patcher and can be edited or removed there.

// --- master-mixer INSERT FX chains (Ardour's processor box) -----------------
// Each master-mixer strip owns an ORDERED list of insert processors wired in
// series in front of its channel inlet.  `track >= 0` addresses a track strip;
// `track < 0` addresses the MASTER strip, whose chain sits between the mix bus
// and the audio device.
//
// Like Ardour's processor box the FADER is part of the ordering: entries marked
// pre-fader run before the strip gain, entries marked post-fader after it.  A
// strip with post-fader entries grows a TrackFaderNode (the Amp) and its mixer
// channel switches to unity, so the gain is applied exactly once.
//
// Bypass ("active = 0") is the graph's own passthrough -- the entry keeps its
// place in the list and in the chain but processes nothing, matching Ardour's
// Processor::enable(false).
int  audio_app_master_insert_count(int track);
bool audio_app_master_insert_info(int track, int slot, int* outNode,
                                  char* nameBuf, int nameBufLen,
                                  int* outActive, int* outPreFader);
int  audio_app_master_insert_node_at(int track, int slot);
// Instantiate `desc` and append it to the strip's chain.  Returns the slot.
int  audio_app_master_insert_add(int track, const PatchKnob::engine::PluginDescriptor& desc,
                                 int preFader);
// Adopt a patch node that already exists (e.g. one dropped in the patcher).
int  audio_app_master_insert_add_node(int track, int node, int preFader);
bool audio_app_master_insert_remove(int track, int slot);      // removes the node too
// Reorder; preFader < 0 keeps the entry's current placement, otherwise it moves
// across the fader.  The list is renormalised so pre entries precede post ones.
bool audio_app_master_insert_move(int track, int from, int to, int preFader);
bool audio_app_master_insert_set_active(int track, int slot, int active);
bool audio_app_master_insert_set_prefader(int track, int slot, int pre);
bool audio_app_master_insert_clear(int track, int which);      // -1 all, 0 pre, 1 post
void audio_app_master_insert_refresh(int track);               // re-wire the chain
int  audio_app_master_insert_fader_node(int track);            // Amp node id or -1
float audio_app_master_strip_peak(int track, int channel);     // 0=L, 1=R

// Generic per-node bypass (Ardour Processor::enable): the graph substitutes a
// passthrough for the node's process().
bool audio_app_patch_set_node_bypass(int node, int bypass);
bool audio_app_patch_node_bypass(int node);
// Display name of a patch node (hosted plugin name, else the node type).
int  audio_app_patch_node_name(int node, char* buf, int buflen);
// Enumerate live patch node ids.  Returns the TOTAL count; fills up to `cap`.
int  audio_app_patch_node_ids(int* buf, int cap);

// --- strip input / output routing (Ardour's MixerStrip in/out buttons) ------
// The node feeding a strip's chain head, and the node its DIRECT OUT (the
// mixer's per-track outlet) feeds; -1 when nothing is connected.
int  audio_app_master_strip_input(int track);
int  audio_app_master_strip_output(int track);
bool audio_app_master_strip_set_output(int track, int destNode);  // destNode < 0 = none

bool audio_app_master_route_audio_input(int track, int sourceNode);
//! Drop whatever feeds a track's audio inlet (the plain "Audio input" choice).
//! Routing with sourceNode < 0 cannot do this: the node lookup fails and the
//! router returns before it reaches the disconnect.
bool audio_app_master_clear_audio_input(int track);
int  audio_app_master_track_count();
bool audio_app_master_track_is_midi(int idx);

// --- AUX SENDS / AUX BUSES --------------------------------------------------
//
// An aux bus is a real destination in the graph: every track can send a copy of
// its signal to it (pre- or post-fader), the bus runs its own insert chain --
// the ordinary audio_app_master_insert_* API, addressed by the bus's node id --
// and its output returns into the master mix at the bus's return gain.  This is
// the standard reverb/delay send topology; before it existed the mixer's send
// knobs rendered and dragged but had nothing to route to, so they were hidden.
//
// Send levels are NORMALISED 0..1 to match the strip UI; the engine maps that to
// its own taper.  A level of 0 is silence, not merely quiet.
int   audio_app_master_aux_count();
//! Create a bus.  Returns its index, or -1 if the engine is down / at capacity.
int   audio_app_master_aux_add(const char* name);
bool  audio_app_master_aux_remove(int aux);
//! Writes the name; returns its length (0 and an empty buf for a bad index).
int   audio_app_master_aux_name(int aux, char* buf, int buflen);
bool  audio_app_master_aux_set_name(int aux, const char* name);
//! The patch node that IS this bus -- wire it by hand in the patcher, or drop a
//! reverb in front of it with audio_app_master_insert_add_node().  -1 when
//! there is no such bus.
int   audio_app_master_aux_node(int aux);
//! THE STRIP CODE FOR THE INSERT API.  The audio_app_master_insert_* calls
//! address a strip by an int: `track >= 0` is a track strip, -1 is the MASTER
//! strip, and an AUX BUS is -2 - aux.  So a reverb on bus 0 is
//!
//!     audio_app_master_insert_add(audio_app_master_aux_strip(0), desc, 0);
//!
//! and audio_app_master_insert_count/_info/_remove/_move/_set_active/_clear/
//! _refresh all work on a bus exactly as they do on a track.  An aux strip's
//! inserts are ALWAYS pre-fader (its "fader" is the return gain, which sits at
//! the very end of the bus), the mirror image of the master strip's always
//! being post-fader -- so the preFader argument is ignored for a bus and
//! _set_prefader returns false, just as it does for the master strip.
inline int audio_app_master_aux_strip(int aux) { return -2 - aux; }
float audio_app_master_aux_return_gain(int aux);            // linear, 1.0 = unity
bool  audio_app_master_aux_set_return_gain(int aux, float g);
bool  audio_app_master_aux_mute(int aux);
bool  audio_app_master_aux_set_mute(int aux, int mute);
float audio_app_master_aux_peak(int aux, int channel);      // 0 = L, 1 = R
// NOTE FOR THE UI: a bus's meter is published with its own ballistics (instant
// attack, 21 ms hold, 24 dB/s fall) and is NOT part of MixerNode::meterLatch(),
// so read it whenever you draw -- there is nothing to latch and no peak to
// steal from another view.  audio_app_master_strip_peak() accepts an aux strip
// code too, so a bus strip can use the same meter call a track strip does.

// Per-track sends.  `track` indexes the master-mixer tracks exactly as the rest
// of the audio_app_master_* API does.
float audio_app_master_send_level(int track, int aux);      // 0..1
bool  audio_app_master_set_send_level(int track, int aux, float level);
//! Pre-fader sends ignore the strip fader and mute (a headphone/monitor feed);
//! post-fader sends follow them, which is what a reverb send normally wants.
//! Post-fader is the default.
bool  audio_app_master_send_prefader(int track, int aux);
bool  audio_app_master_set_send_prefader(int track, int aux, int pre);
//! An individually disabled send keeps its level so it can be toggled back.
bool  audio_app_master_send_enabled(int track, int aux);
bool  audio_app_master_set_send_enabled(int track, int aux, int on);

// --- SOLO -------------------------------------------------------------------
// Real solo state, in the engine.  It used to live in a function-local
// `static bool g_busSolo[]` in main.cpp that recompute_mutes() folded into the
// mixer node's MUTE, so it could never round-trip through a project file and
// nothing below the UI could tell "the user muted this" from "the user soloed
// something else".  Now that aux buses exist that distinction is load-bearing:
// a PRE-fader send has to ignore solo exactly as it ignores mute, which is only
// decidable where the send matrix is.
//
// Setting solo here is enough -- the engine applies it (and pushes it onto a
// strip's lifted Amp when it has post-fader inserts).  Folding solo into mute
// upstream as well still behaves identically while nothing is soloed, so the
// switch-over can be done whenever the mixer view is ready.  Solo state is
// saved with the project (v18+).
bool audio_app_master_solo(int track);
bool audio_app_master_set_solo(int track, int on);
int  audio_app_master_solo_count();      // 0 == nothing soloed anywhere
//! mute + solo resolved: what the fader stage actually does with this strip.
bool audio_app_master_audible(int track);

// --- live-record node (arrange: audio + MIDI ins -> piano-roll clips) --------
// --- transport live MIDI record (into a new timeline clip) ------------------
// Arm captures every LIVE event (hardware input + on-screen preview, not the
// sequencer's own output) timestamped to the transport.  The shell drains on
// stop and builds a sequence + trigger at the record position.  `ticks` are
// returned RELATIVE to where recording began.
// Arm at the one explicit timeline origin chosen by the arrange transport.
void      audio_app_record_arm_at_tick(bool on, long long startTick);
bool      audio_app_record_armed();
long long audio_app_record_start_tick();
// Replace the VST hosted by an existing plugin node (keeps ports + wiring).
bool audio_app_patch_set_node_plugin(int node, const PatchKnob::engine::PluginDescriptor& desc);
bool audio_app_patch_connect(int fromNode, int fromPort, int toNode, int toPort);
bool audio_app_patch_disconnect(int fromNode, int fromPort, int toNode, int toPort);
void audio_app_patch_remove(int node);
int  audio_app_patch_out_node();      // the AudioDeviceOut node id
int  audio_app_patch_midi_in_node();  // the MidiIn node id
// The plugin instance a node hosts (for its native editor GUI); null if none.
PatchKnob::engine::IPluginInstance* audio_app_patch_node_instance(int node);
bool audio_app_patch_route_param(int node, unsigned int paramId, float value);
// Per-column FX: route a param to one tracker note-column's voices (built-in
// sampler); column < 0 or non-sampler nodes fall back to the global param.
bool audio_app_patch_route_param_column(int node, unsigned int paramId, float value, int column);
// Tag a built-in sampler note's tracker column so the voice it next triggers is
// bound to that column (no-op for non-sampler nodes).
void audio_app_patch_sampler_set_note_column(int node, int note, int column);
// Route a clip to one instrument node by MIDI channel (-1 omni, 0..15).
void audio_app_patch_set_node_channel(int node, int ch);
int  audio_app_patch_node_channel(int node);
bool audio_app_patch_node_is_instrument(int node);
// Resolve a node's first port by kind/direction (kind: 0 audio, 1 MIDI;
// direction: 0 input, 1 output). Returns the real PortId, not its array index.
int  audio_app_patch_first_port(int node, int kind, int direction);
// Project I/O helpers.  Reset keeps the built-in Audio Out, MIDI In and master
// mixer nodes, then removes all user patch nodes/wires and master tracks.
void audio_app_project_reset_patch();
// Own and schedule an in-memory audio clip on a mixer track. Used by the
// project container to restore embedded audio without relying on source files.
bool audio_app_project_add_audio_clip(int track, const PatchKnob::engine::AudioClip& clip,
                                      long long startSample, float gain);
// As above but restores the FULL non-destructive region (source offset, length,
// mute, loop, fades) -- used by project load so trims/slips/fades survive a
// round-trip.  length<=0 == to source end.
// `loopLength` is the loop PERIOD in source frames; 0 == "everything from
// sourceOffset to the end of the source", which is the pre-v17 behaviour and
// what a project saved before the trimmed-loop fix restores to.
// The four `fade*Shape/Slope` values are ScheduledClip::kFadeShape*/kFadeSlope*
// (ch.32); `xfadeLink` is the edit-time crossfade link on the fade-in side.
// Defaults reproduce the pre-v19 fade rendering exactly (Standard/Equal Gain).
bool audio_app_project_add_audio_region(int track, const PatchKnob::engine::AudioClip& clip,
                                        long long startSample, long long sourceOffset,
                                        long long length, float gain, int muted, int loop,
                                        long long fadeIn, long long fadeOut,
                                        float fadeInK, float fadeOutK,
                                        long long loopLength = 0,
                                        int fadeInShape = 0, int fadeOutShape = 0,
                                        int fadeInSlope = 0, int fadeOutSlope = 0,
                                        int xfadeLink = 0);
// Ardour RecNonLayered-style playlist partition: remove [start,end) from every
// scheduled region on the track, trimming or splitting regions non-destructively.
bool audio_app_project_partition_track(int track, long long startSample, long long endSample);

// --- punch recording (PT ch.27) ---------------------------------------------
// OWN a clip on `track` WITHOUT scheduling it: the whole-file parent of a
// QuickPunch/TrackPunch pass, which every punch region then references via
// _add_region_shared.  Returns the stored (engine-owned) pointer, or null.
//! CONTRACT: at least one region referencing the returned clip must be added
//! before the next partition/remove on any track -- those run an orphan-clip
//! GC that frees sources nothing references.
const PatchKnob::engine::AudioClip* audio_app_project_own_clip(
    int track, const PatchKnob::engine::AudioClip& clip);
// Schedule a region over an ALREADY-OWNED source (from _own_clip or any stored
// project clip) without copying the audio again -- N punches from one pass
// share one parent buffer.  Fades are linear (tension 0).
bool audio_app_project_add_region_shared(
    int track, const PatchKnob::engine::AudioClip* source,
    long long startSample, long long sourceOffset, long long length, float gain,
    long long fadeInFrames, long long fadeOutFrames);
// QuickPunch/TrackPunch pre/post crossfades (PT p636): give the region that
// ENDS exactly at `inSample` a linear fade-OUT of `fadeFrames` (the
// pre-crossfade, up to but not into the punched clip) and the region that
// STARTS exactly at `outSample` a linear fade-IN (the post-crossfade).  The
// punch region itself (starting at inSample) is left alone.
bool audio_app_project_set_boundary_fades(int track, long long inSample,
                                          long long outSample, long long fadeFrames);
// TrackInput monitor switching for a punch: mute every currently-unmuted
// scheduled region on `track` (on=1) and restore exactly those regions later
// (on=0).  MONITORING ONLY -- the capture tap reads the strip INLET, which
// clip playback never enters, so this changes what is HEARD while punched in
// (input instead of the old material), never what is recorded.  Region
// identity is held by regionId, so it survives ordinary edits but NOT a
// partition (which rebuilds the schedule); release the hold before
// partitioning.  Returns the number of regions toggled.
int  audio_app_project_hold_mute(int track, int on);
// DestructivePunch eligibility (PT p644): the track must hold ONE contiguous
// scheduled audio region starting at timeline sample 0 / source frame 0, at
// least `minFrames` long, with unity region gain.  On failure writes a short
// human-readable reason (with its remedy) into reasonBuf.
bool audio_app_project_dp_eligible(int track, long long minFrames,
                                   char* reasonBuf, int reasonCap);
// Prepare DPE Tracks (PT p644): render everything scheduled on `track` over
// [0, lengthFrames) -- region gain and fade envelopes included -- into ONE new
// contiguous clip named `name`, and replace the track's schedule with that
// single region at 0 with unity gain.  An empty track renders silence (a valid
// dubber target).  Returns the stored clip via _project_clip_on_track.
bool audio_app_project_consolidate_track(int track, long long lengthFrames,
                                         const char* name);
// DestructivePunch write (PT p642): overwrite `frames` samples of the audio
// UNDER timeline sample `destSample` on `track` with `take` (read from
// `takeOffset`), applying a linear crossfade of `xfadeFrames` at the in and
// out points.  Destructive by design: the stored source buffer is edited in
// place, no new clips are created.  The audio thread may be playing the same
// buffer; single float loads/stores tear at worst for one sample, which is the
// same benign race every destructive editor here already accepts.
bool audio_app_project_destructive_punch(int track, long long destSample,
                                         const PatchKnob::engine::AudioClip& take,
                                         long long takeOffset, long long frames,
                                         long long xfadeFrames);
bool audio_app_project_remove_audio_clip(int track, const PatchKnob::engine::AudioClip* clip);
void audio_app_project_clear_audio_clips();
// The source AudioClip of the first project clip scheduled on `track` (null if
// none) -- used to re-show a reloaded freeze's waveform.  Owned by the engine.
const PatchKnob::engine::AudioClip* audio_app_project_clip_on_track(int track);
const PatchKnob::engine::AudioClip* audio_app_project_last_clip_on_track(int track);
bool audio_app_project_find_region(int track, const PatchKnob::engine::AudioClip* clip,
                                   long long* startSample, long long* sourceOffset,
                                   long long* length, float* gain = nullptr,
                                   int* muted = nullptr, int* loop = nullptr,
                                   // Loop PERIOD in source frames; 0 == the
                                   // whole source from sourceOffset on.
                                   long long* loopLength = nullptr,
                                   // Fade envelope + shapes (ch.32), so a
                                   // reloaded project can re-show its fades.
                                   long long* fadeIn = nullptr, long long* fadeOut = nullptr,
                                   float* fadeInK = nullptr, float* fadeOutK = nullptr,
                                   int* fadeInShape = nullptr, int* fadeOutShape = nullptr,
                                   int* fadeInSlope = nullptr, int* fadeOutSlope = nullptr,
                                   int* xfadeLink = nullptr);
bool audio_app_project_set_region(int track, const PatchKnob::engine::AudioClip* clip,
                                  long long startSample, long long sourceOffset,
                                  long long length);
bool audio_app_project_set_fades(int track, const PatchKnob::engine::AudioClip* clip,
                                 long long fadeInFrames, long long fadeOutFrames,
                                 float fadeInTension, float fadeOutTension);
// Fade SHAPES/SLOPES (ScheduledClip::kFadeShape*/kFadeSlope*) + the edit-time
// crossfade link (link < 0 leaves it untouched) of a scheduled region (ch.32).
bool audio_app_project_set_fade_shapes(int track, const PatchKnob::engine::AudioClip* clip,
                                       int inShape, int outShape,
                                       int inSlope, int outSlope, int link = -1);
// AutoFades (PT p752): real-time fade length applied at every free-standing
// clip boundary during playback, on EVERY audio clip player (project + freeze
// + future ones).  0 (the default) = off.  Applied inside the players, so it
// is baked into offline bounce/consolidate renders that run through them.
void   audio_app_set_auto_fade_ms(double ms);
double audio_app_auto_fade_ms();
bool audio_app_project_set_gain(int track, const PatchKnob::engine::AudioClip* clip, float gain);
bool audio_app_project_set_muted(int track, const PatchKnob::engine::AudioClip* clip, bool muted);
bool audio_app_project_set_loop(int track, const PatchKnob::engine::AudioClip* clip, bool loop);

// --- STABLE REGION IDENTITY --------------------------------------------------
// The pointer-based accessors above resolve (track, clip) to the FIRST region
// scheduled over that source -- which is ambiguous the moment a track holds
// two slices of one shared clip (exactly what a split produces).  Every
// scheduled region carries a per-track stable id (>0, assigned at add time,
// preserved across ordinary edits AND across a track partition for the
// surviving pieces); these accessors address one region unambiguously.
// 0 == "no such region".  Ids are unique per track, not globally.
int  audio_app_project_region_count(int track);
//! Region id at editable index `index` on `track` (0 if out of range).
unsigned long long audio_app_project_region_id_at(int track, int index);
//! Id of the most recently ADDED region on `track` (0 if none) -- capture it
//! right after audio_app_project_add_audio_clip/_add_audio_region/
//! _add_region_shared to get the new region's handle.
unsigned long long audio_app_project_last_region_id(int track);
//! Resolve a region by source clip + exact timeline start (for re-binding a
//! view to a reloaded schedule).  clip == nullptr matches any source.
unsigned long long audio_app_project_region_id_find(int track,
                                                    const PatchKnob::engine::AudioClip* clip,
                                                    long long startSample);
//! The source clip a region plays (nullptr if the id is unknown).
const PatchKnob::engine::AudioClip* audio_app_project_region_clip(int track,
                                                                  unsigned long long id);
// By-id twins of the pointer-based region accessors above.
bool audio_app_project_find_region_by_id(int track, unsigned long long id,
                                   long long* startSample, long long* sourceOffset,
                                   long long* length, float* gain = nullptr,
                                   int* muted = nullptr, int* loop = nullptr,
                                   long long* loopLength = nullptr,
                                   long long* fadeIn = nullptr, long long* fadeOut = nullptr,
                                   float* fadeInK = nullptr, float* fadeOutK = nullptr,
                                   int* fadeInShape = nullptr, int* fadeOutShape = nullptr,
                                   int* fadeInSlope = nullptr, int* fadeOutSlope = nullptr,
                                   int* xfadeLink = nullptr);
bool audio_app_project_set_region_by_id(int track, unsigned long long id,
                                        long long startSample, long long sourceOffset,
                                        long long length);
bool audio_app_project_set_fades_by_id(int track, unsigned long long id,
                                       long long fadeInFrames, long long fadeOutFrames,
                                       float fadeInTension, float fadeOutTension);
bool audio_app_project_set_fade_shapes_by_id(int track, unsigned long long id,
                                             int inShape, int outShape,
                                             int inSlope, int outSlope, int link = -1);
bool audio_app_project_set_gain_by_id(int track, unsigned long long id, float gain);
bool audio_app_project_set_muted_by_id(int track, unsigned long long id, bool muted);
bool audio_app_project_set_loop_by_id(int track, unsigned long long id, bool loop);
bool audio_app_project_set_loop_length_by_id(int track, unsigned long long id,
                                             long long frames);
//! Remove exactly ONE region (the source clip is freed only when nothing else
//! references it -- slices sharing the source are untouched).
bool audio_app_project_remove_region_by_id(int track, unsigned long long id);
// Loop PERIOD (source-iteration length) of an already-looping region, in
// source frames.  setClipLoop() captures the current trimmed span as the
// period; this publishes an explicit one (the arrange view's Loop Trim).
bool audio_app_project_set_loop_length(int track,
                                       const PatchKnob::engine::AudioClip* clip,
                                       long long frames);
// Stop + unschedule the project clips on `track` (e.g. unfreezing a RELOADED
// freeze, whose audio is a project clip, not a live freeze entry).
void audio_app_project_clear_track(int track);
void audio_app_set_modular(bool on);
bool audio_app_modular();
// Multi-core (level-parallel) plugin processing in the modular graph; off by default.
void audio_app_set_multithreaded(bool on);
bool audio_app_multithreaded();
// Reflect transport run-state into the modular render context (host-sync).
void audio_app_patch_set_playing(bool on);

// --- kitchensink transport / musical clock (Ardour temporal tempo map) -------
void      audio_app_set_tempo(double bpm);            // 20..999
double    audio_app_tempo();
void      audio_app_metronome_enable(bool on);
bool      audio_app_metronome_enabled();
void      audio_app_metronome_start_countin();
bool      audio_app_metronome_countin_finished();
long long audio_app_metronome_countin_origin_tick();
void      audio_app_transport_locate(long long sample);
void      audio_app_transport_locate_sync(long long sample);
//! Wait (bounded) for any ALREADY-QUEUED seek to be applied by the audio thread
//! and for the blocks that began before it to retire.  This is
//! audio_app_transport_locate_sync() without the locate: a punch-in has to let
//! somebody else's seek land before it opens capture, and issuing a locate of
//! its own to achieve that would bump the schedule epoch and throw away MIDI
//! that the sequencer had already queued.
void      audio_app_transport_settle();
long long audio_app_transport_sample();
//! Where the playhead is ABOUT to be: the queued seek if a locate has not been
//! applied yet, otherwise the live position.  Anything that CAPTURES a position
//! to act on (record origin, punch-out, count-in return) must use this --
//! audio_app_transport_locate() only queues the seek, so the plain accessor
//! keeps reporting the old spot until the audio thread runs a block.
long long audio_app_transport_effective_sample();
bool      audio_app_transport_rolling();
double    audio_app_transport_beats();
void      audio_app_transport_bbt(int* bar, int* beat, int* tick);   // 1-based bar/beat
long long audio_app_beats_to_sample(double beats);
double    audio_app_sample_to_beats(long long sample);
// Integer 192-PPQN sequencer-tick conversions against the ACTIVE tempo map
// (RCU snapshot; symmetric rounding so round-trips are exact).  These are what
// the output thread's lookahead scheduler and the event drain both use, so the
// sequencer and the engine can never disagree on where a tick falls.
long long audio_app_tick_to_sample(long long tick);
long long audio_app_sample_to_tick(long long sample);
long long audio_app_sample_to_tick_ceil(long long sample);
double    audio_app_sample_rate();

// Headless diagnostic: stop the live stream, roll from sample 0, single-thread
// pump `blocks` modular render blocks, and return the peak |master out|.  Used by
// freeze/playback probes to confirm audio actually reaches the master.  Restores
// the prior stream/transport state.  Returns 0 if the engine isn't running.
float     audio_app_probe_render_peak(int blocks);

// Engine-side sample-accurate LOOP (the audio callback cycle-splits at the
// loop-end frame; nobody seeks the transport for looping anymore).  The
// sequencer publishes its loop markers in TICKS; on = 0 disables.
void audio_app_set_loop_ticks(long long leftTick, long long rightTick, int on);
// Discard scheduled-ahead messages after a live sequence edit without seeking
// the transport or releasing notes that are already sounding.
void audio_app_invalidate_future_schedule();
void audio_app_queue_loop_note_off(int track, int channel, int note);
unsigned long long audio_app_loop_wrap_generation();
// Monotonic locate counter, the counterpart of the wrap generation above.  The
// scheduler consumes it to learn that the transport was repositioned, instead
// of inferring it from a sample decrease (which cannot see a FORWARD locate and
// aliases away under a scheduler stall).
unsigned long long audio_app_locate_generation();
// Publish the scheduler's lookahead (samples) so the engine's drain can tell a
// next-pass event from a late one even when the lookahead exceeds half the
// loop -- which is every loop shorter than roughly twice the lookahead.
void audio_app_set_schedule_lookahead( long long samples );
// >= 0 while a queued seek has not yet been applied by the audio thread.  A
// locate rebuild must WAIT for this to clear: rebuilding against the pre-seek
// position sets the sequence cursors to the old spot and then feeds play() a
// window spanning the whole jump.
long long audio_app_transport_pending_seek();
// Read back exactly what the AUDIO thread uses to decide a wrap.  The scheduler
// publishes ticks; these are the derived SAMPLES the cycle-split compares the
// playhead against.  Diagnostics only (the loop regression harness) -- if these
// disagree with tick_to_sample(left/right) the two sides of the loop have
// drifted apart, which is what "the clip plays at the wrong place" looks like.
void audio_app_loop_debug(long long* startSample, long long* endSample,
                          long long* leftTick, long long* rightTick, int* on);
// Harness aid: keep rendering exactly as normal (transport, MIDI, graph, meters
// all live) but zero the device buffers on the way out, so a many-minute
// headless loop test does not blast the user's speakers.
//! DISPATCH CENSUS (diagnostic).  Every fate an outgoing MIDI event can meet
//! between the sequencer's ring and an instrument.  The sequencer's own emit
//! tap can show a PERFECT pass while the audio drops out, so these are what
//! localise the loss: enqueued vs delivered, and why the difference.
void audio_app_midi_census(unsigned long long* enqueued,
                           unsigned long long* delivered,
                           unsigned long long* ringFull,
                           unsigned long long* staleEpoch,
                           unsigned long long* unreachable,
                           unsigned long long* nodeFull);

void audio_app_set_silent_output(bool silent);

// --- bounce / render to disk (message thread) ------------------------------
// Realtime master capture: begin() allocates a buffer for `seconds`; the audio
// thread taps every master block; end_wav() writes a 16-bit stereo WAV + frees.
// Drive the sequencer (start/stop) around these from the caller.
bool audio_app_capture_begin(double seconds);
bool audio_app_capture_end_wav(const char* path);
void audio_app_capture_cancel();
// Realtime capture of one master-mixer track's INPUT into an AudioClip: the
// raw PRE-FADER strip inlet, i.e. whatever the patcher routes into that track
// (an Audio In device node, an instrument, a rack).  The engine keeps
// MixerNode::kMaxCaptureTaps (8) independent tap slots, so up to 8 tracks can
// record simultaneously -- what the punch modes' multi-track passes use.  A
// track armed on an unpatched inlet records silence: there is no hidden
// hardware side channel (the same contract the virtual-MIDI side states).
bool audio_app_track_capture_begin(int track, double seconds);
bool audio_app_track_capture_end_for(int track, PatchKnob::engine::AudioClip& out);
// Legacy single-capture entries: operate on the first (typically only) open
// capture.  The normal record path opens exactly one, so these stay valid.
bool audio_app_track_capture_end(PatchKnob::engine::AudioClip& out);
bool audio_app_track_capture_preview(PatchKnob::engine::AudioClip& out, int maxFrames);
// Frames a running capture has written so far (0 if none is open).  The punch
// recorder timestamps punch in/out points with this, so the marks are in the
// same clock as the buffer they cut.
long long audio_app_track_capture_frames_for(int track);
long long audio_app_track_capture_frames();
// Copy [startFrame, startFrame+frames) of a RUNNING capture into `out`
// (planar stereo).  Only already-written frames are readable (the audio thread
// appends strictly forward), which is what lets DestructivePunch commit each
// punch at punch-out without stopping the pass.
bool audio_app_track_capture_read_for(int track, long long startFrame,
                                      long long frames, PatchKnob::engine::AudioClip& out);
bool audio_app_track_capture_read(long long startFrame, long long frames,
                                  PatchKnob::engine::AudioClip& out);
// Is anything patched INTO master-mixer track `track`'s audio inlet in the
// modular graph?  The audio twin of audio_app_virtual_midi_input_is_patched:
// arming a track whose inlet is unpatched records silence, and the UI should
// say so rather than pretending some other signal exists.
bool audio_app_track_input_patched(int track);

// One-shot UI sample audition, independent of song transport. Replaces any
// previous audition and mixes directly into the master output.
void audio_app_preview_clip(const PatchKnob::engine::AudioClip& clip, float gain = 1.0f);

// --- freeze: render ONE track's output offline to an in-memory AudioClip -----
// Solos `track` (master gain neutralised), stops the live stream, seeks to 0,
// runs `scheduleMidi` (which should audio_app_route_midi() the pattern's events
// at ticks starting from 0), then pumps the fixed-graph render for `seconds`
// while tapping the master into `outClip` (planar stereo, engine rate), and
// restores everything.  Faster than realtime; the caller owns the resulting
// AudioClip.  Returns false if the engine/graph is unavailable or nothing was
// captured.  scheduleMidi may be empty (e.g. freezing an existing audio clip,
// which just needs the transport rolling).
bool audio_app_freeze_render(int track, double seconds,
                             const std::function<void()>& scheduleMidi,
                             PatchKnob::engine::AudioClip& outClip);
// As above, but render at the source timeline position.  MIDI scheduled by the
// callback uses absolute project ticks, so tempo-map changes remain aligned.
//! Latency compensation for the offline bounce, in samples.  An instrument's
//! MIDI->audio pipeline delay is NOT part of the music: a plugin with lookahead,
//! a Csound control period or a long modular chain all emit their first sample
//! some frames after the note that caused it, so a bounce captured on the
//! timeline lands late by exactly that much and drifts out of phase with the
//! tracks around it.
//!
//! Node::latencySamples() exists for PDC but almost nothing implements it, and
//! plugins routinely under-report, so this is MEASURED rather than asked for:
//! the freeze notes where its first event was scheduled, finds the first sample
//! the instrument actually produced, and the difference is the delay.
//!
//! Set <= 0 to disable (capture exactly what came out, warts and all).  The
//! default cap keeps a pathological measurement from shifting a whole bar.
void audio_app_freeze_set_latency_compensation(bool on, int maxSamples);
bool audio_app_freeze_latency_compensation(int* outMaxSamples);
//! Delay measured by the most recent bounce, in samples (-1 if none / unknown).
int  audio_app_freeze_last_measured_latency();

bool audio_app_freeze_render_at(int track, long long startSample, double seconds,
                                const std::function<void()>& scheduleMidi,
                                const std::function<void(long long,long long)>& advanceAutomation,
                                PatchKnob::engine::AudioClip& outClip);

// Frozen-clip playback lifecycle.  attach() copies the clip in and schedules it
// at `startSample` on `track`'s AudioClipPlayer (track < 0 = a shared hidden
// freeze track, for CLIP freeze; track >= 0 = that engine track's own player,
// for a TRACK-freeze audio lane), returning a stable id (>=1) or -1.  clip()
// returns the owned copy (for the lane waveform); detach(id) removes exactly
// that clip (unfreeze).
int  audio_app_freeze_attach(int track, const PatchKnob::engine::AudioClip& clip,
                             long long startSample, float gain);
int  audio_app_freeze_attach_shared(int track, const PatchKnob::engine::AudioClip* source,
                                    long long startSample, float gain);
const PatchKnob::engine::AudioClip* audio_app_freeze_clip(int id);
// Diagnostic: render a frozen entry through its real playback path and return
// the best correlation lag versus its stored source (samples; 0 is exact).
int audio_app_freeze_alignment_lag(int id, int searchSamples = 2048);
void audio_app_freeze_detach(int id);
//! Release detached freeze audio that the UI may still have been pointing at.
//! Detach cannot free the clip outright: the RCU grace inside it only protects
//! the AUDIO thread, while the arrange and sample-editor views hold plain
//! `const AudioClip*` for drawing.  Freeing there left those views reading
//! released memory, which drew as garbage.  Detached clips are parked instead,
//! and reclaimed here -- call it at a point where the views have provably
//! dropped their pointers (project clear / reload).
void audio_app_freeze_gc();
// Reschedule a frozen clip to a new timeline start (samples) so moving the
// arrange block moves where the audio plays.  Preserves offset/length/fades.
bool audio_app_freeze_move(int id, long long newStartSample);
// Full non-destructive REGION update: timeline position + start-offset into the
// source + length (samples; length<=0 == to source end).  The trim/slip/move
// primitive: the arrange view drives this from a clip's (start,length,offset).
bool audio_app_freeze_set_region(int id, long long positionSample,
                                 long long sourceOffsetSample, long long lengthSample);
// Read a freeze region's current placement (for undo snapshots).  Any out ptr
// may be null.  Returns false if `id` is unknown.
bool  audio_app_freeze_get_region(int id, long long* startSample,
                                  long long* sourceOffset, long long* length,
                                  float* gain, int* muted, int* loop);
// Region GAIN (negative = phase invert), MUTE, and LOOP (wrap source to fill).
bool  audio_app_freeze_set_gain(int id, float gain);
bool  audio_app_freeze_set_muted(int id, bool muted);
bool  audio_app_freeze_set_loop(int id, bool loop);
//! Loop PERIOD in source frames for a looping freeze region (see the project
//! twin above).
bool  audio_app_freeze_set_loop_length(int id, long long frames);
// NORMALIZE the region to targetDb (0 = peak just under 0 dBFS); returns applied
// gain.  REVERSE reverses the region's source window in place.
float audio_app_freeze_normalize(int id, float targetDb);
bool  audio_app_freeze_reverse(int id);
// Replace a region's clip with a new (e.g. time-stretched) clip; the region
// re-spans the new clip at the same timeline start + gain.
bool  audio_app_freeze_replace_clip(int id, const PatchKnob::engine::AudioClip& newClip);
// Publish a LIVE warp map so the region time-stretches in REALTIME during
// playback (hear warp-marker edits as you drag).  count<2 clears warp (raw).
bool  audio_app_freeze_set_warp(int id, const PatchKnob::engine::WarpMarker* markers, int count);
// Set fade-in/out lengths (frames) + curve tensions (-1..1) on a frozen clip.
bool audio_app_freeze_set_fades(int id, long long fadeInFrames, long long fadeOutFrames,
                                float fadeInTension, float fadeOutTension);
// Fade shapes/slopes + crossfade link on a frozen clip (see the project twin).
bool audio_app_freeze_set_fade_shapes(int id, int inShape, int outShape,
                                      int inSlope, int outSlope, int link = -1);

// Disable a track's whole signal chain (instrument + FX emit nothing, run no
// DSP -> frees CPU).  Used by track-freeze to idle the frozen source.
void audio_app_set_track_disabled(int track, bool disabled);

// Headless proof of the freeze render+capture path (sine -> Out, frozen
// offline).  Returns the captured peak (>0 == capture works).  Env-gated via
// PATCHKNOB_FREEZE_SELFTEST at startup.
float audio_app_freeze_selftest();
// Realistic freeze proof: a MIDI-CV->VCO->VCA voice + ADSR plays 16 notes and is
// frozen offline.  Returns the captured peak (logs peak + note-burst count).
float audio_app_freeze_voice_selftest();
// Enumerate a patch node's ports so the UI can wire them.
int  audio_app_patch_node_port_count(int node);
bool audio_app_patch_node_port(int node, int index, int* outKind /*0=audio,1=midi*/,
                               int* outDir /*0=in,1=out*/, int* outChannels);
// Human-readable label for a node port (e.g. "Instrument 1"); "" if none.  The
// returned pointer is owned by the engine node -- copy it before the next graph
// edit.
const char* audio_app_patch_node_port_name(int node, int index);

// --- diagnostics ------------------------------------------------------------
// Load a VST from an absolute path onto track 0, fire a note, and measure the
// master peak for ~400ms.  Returns the peak.  Used by the PATCHKNOB_AUDIO_SELFTEST
// startup path to prove the full sequencer->VST->audio chain end to end.
float audio_app_selftest(const char* vst_path);

// Headless proof of the MODULAR path: build MidiIn -> PluginNode(vst) ->
// AudioOut, switch to modular, fire a note, return the master peak.
float audio_app_patch_selftest(const char* vst_path);

// Headless CLOCK acceptance test: schedules 32nd notes at 500 BPM as ticked
// events through the REAL path (tick ring -> tempo map -> holdback drain ->
// MidiIn node -> graph -> MidiOut node) while pumping render blocks directly
// (stream stopped).  Returns the max |inter-onset - ideal| in samples
// (ideal = 720 at 48kHz), or -1 on setup failure.  Steady == 0.
double audio_app_clock_selftest();

// Headless LOOP acceptance test: one looped bar of 32nds at 500 BPM through a
// faithful scheduler simulation + the engine's cycle-split loop.  Returns the
// max |due - expected| in samples (0 == every pass sample-exact), or huge on
// missing onsets (the loop-tail displacement bug).
double audio_app_loop_selftest();

}} // namespace PatchKnob::app

#endif // PATCHKNOB_AUDIO_APP_H
