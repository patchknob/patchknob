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

// --- audio INPUT device selection (stored; capture path is future work) ----
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
void audio_app_route_midi(int track, unsigned char status,
                          unsigned char d1, unsigned char d2,
                          long long tick = -1);

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
// Buzz/Unwieldy SAMPLER instrument node.  audio_app_sampler_load loads a sample
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
bool audio_app_sampler_has_envelopes(int node);
int  audio_app_patch_add_builtin(const char* kind);   // "sine" "gain" "sum" "in" "out" "midiin"

// --- patchable mixer node (N stereo channels; patch mixers for submixes) ----
int   audio_app_patch_add_mixer(int channels);
int   audio_app_mixer_channels(int node);
void  audio_app_mixer_set_channels(int node, int n);
float audio_app_mixer_gain(int node, int ch);
void  audio_app_mixer_set_gain(int node, int ch, float g);
bool  audio_app_mixer_mute(int node, int ch);
void  audio_app_mixer_set_mute(int node, int ch, bool mute);
float audio_app_mixer_vu(int node, int ch);
float audio_app_mixer_pan(int node, int ch);
void  audio_app_mixer_set_pan(int node, int ch, float p);
float audio_app_mixer_master_gain(int node);
void  audio_app_mixer_set_master_gain(int node, float g);
float audio_app_mixer_master_vu(int node);

// --- Pure Data node (embedded libpd; SDL Pd editor drives it) ---------------
int  audio_app_patch_add_pd();
bool audio_app_pd_load(int node, const char* path);       // open a .pd patch
bool audio_app_pd_path(int node, char* buf, int buflen);  // current patch path
int  audio_app_pd_io_sig(int node);                       // adc~/dac~ sig: inCh*1000+outCh (-1 if not Pd)

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
int  audio_app_master_track_count();
bool audio_app_master_track_is_midi(int idx);

// --- live-record node (arrange: audio + MIDI ins -> piano-roll clips) --------
int  audio_app_patch_add_record();
void audio_app_record_set(int node, bool on);
bool audio_app_record_active(int node);
// Drain captured MIDI (message thread) into parallel arrays; returns count.
int  audio_app_record_drain(int node, long* ticks, unsigned char* status,
                            unsigned char* d1, unsigned char* d2, int cap);

// --- transport live MIDI record (into a new timeline clip) ------------------
// Arm captures every LIVE event (hardware input + on-screen preview, not the
// sequencer's own output) timestamped to the transport.  The shell drains on
// stop and builds a sequence + trigger at the record position.  `ticks` are
// returned RELATIVE to where recording began.
void      audio_app_record_arm(bool on);
bool      audio_app_record_armed();
long long audio_app_record_start_tick();
int       audio_app_record_drain_live(long* ticks, unsigned char* status,
                                      unsigned char* d1, unsigned char* d2, int cap);
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
bool audio_app_project_add_audio_region(int track, const PatchKnob::engine::AudioClip& clip,
                                        long long startSample, long long sourceOffset,
                                        long long length, float gain, int muted, int loop,
                                        long long fadeIn, long long fadeOut,
                                        float fadeInK, float fadeOutK);
void audio_app_project_clear_audio_clips();
// The source AudioClip of the first project clip scheduled on `track` (null if
// none) -- used to re-show a reloaded freeze's waveform.  Owned by the engine.
const PatchKnob::engine::AudioClip* audio_app_project_clip_on_track(int track);
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
void      audio_app_transport_locate(long long sample);
long long audio_app_transport_sample();
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

// --- bounce / render to disk (message thread) ------------------------------
// Realtime master capture: begin() allocates a buffer for `seconds`; the audio
// thread taps every master block; end_wav() writes a 16-bit stereo WAV + frees.
// Drive the sequencer (start/stop) around these from the caller.
bool audio_app_capture_begin(double seconds);
bool audio_app_capture_end_wav(const char* path);

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

// Frozen-clip playback lifecycle.  attach() copies the clip in and schedules it
// at `startSample` on `track`'s AudioClipPlayer (track < 0 = a shared hidden
// freeze track, for CLIP freeze; track >= 0 = that engine track's own player,
// for a TRACK-freeze audio lane), returning a stable id (>=1) or -1.  clip()
// returns the owned copy (for the lane waveform); detach(id) removes exactly
// that clip (unfreeze).
int  audio_app_freeze_attach(int track, const PatchKnob::engine::AudioClip& clip,
                             long long startSample, float gain);
const PatchKnob::engine::AudioClip* audio_app_freeze_clip(int id);
void audio_app_freeze_detach(int id);
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
