//----------------------------------------------------------------------------
//  audio_app.cpp -- DAW engine glue: AudioEngine + MixerGraph + PluginHost,
//  and the lock-free sequencer->VST MIDI bridge.  See audio_app.h.
//----------------------------------------------------------------------------
#include "audio_app.h"

#include "engine/audio/audio_engine.h"
#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"
#include "engine/host/plugin_host.h"
#include "engine/plugin_api.h"
#include "engine/patch/patch_graph.h"
#include "engine/patch/patch_nodes.h"
#include "engine/patch/pd_node.h"
#include "engine/patch/csound_node.h"
#include "engine/rack/rack_node.h"
#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/audio_clip_player.h"
#include "engine/buzz/sampler_instrument.h"   // Buzz/Unwieldy sampler instrument
#include "engine/kitchensink/kitchensink.h"
#include "RtMidi.h"
#include <memory>

#include <atomic>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>    // SetThreadPriority + waitable timers for the MIDI drain
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

using namespace PatchKnob::engine;

namespace {

AudioEngine* g_engine  = nullptr;
MixerGraph*  g_graph   = nullptr;
PluginHost*  g_host    = nullptr;
bool         g_running = false;
double       g_sr      = 48000.0;
int          g_block   = 512;

std::vector<IPluginInstance*> g_owned;   // instruments/fx we must release
struct ProjectAudioClipEntry
{
    int track = -1;
    std::unique_ptr<AudioClip> clip;
};
std::vector<ProjectAudioClipEntry> g_projectAudioClips;
AudioClipPlayer* g_projectAudioPlayers[PatchKnob::app::AUDIO_APP_MAX_TRACKS] = {};
// Shared hidden freeze track (CLIP freeze).  File-scope so the modular render
// path (audio_render_segment, below) can sound it too -- frozen/project audio
// lives on FIXED-graph tracks, which are NOT processed in modular mode.
AudioClipPlayer* g_freezePlayer = nullptr;

// --- modular patch graph (alternate render core) ---------------------------
namespace patch = PatchKnob::engine::patch;
patch::PatchGraph*  g_patch        = nullptr;
patch::NodeId       g_patchOutId    = 0;
patch::NodeId       g_patchMidiInId = 0;
patch::NodeId       g_masterMixerId = 0;    // 8-bus + master summing mixer -> Out
int                 g_nextMasterCh  = 0;    // next free master-mixer channel (0..7)
patch::MidiInNode*  g_midiInNode    = nullptr;
std::atomic<bool>   g_modular{false};
// Modular host-transport state.  playPos advances every block (audio thread
// only); playing follows the real transport so host-synced plugins (arps/LFOs)
// behave -- notably a "playing but time frozen" context makes some synths
// (e.g. Zebra2's synced elements) produce nothing.
long long           g_patchPlayPos = 0;
std::atomic<bool>   g_patchPlaying{false};

// --- kitchensink: real musical clock (tempo map ported from Ardour temporal).
// TIME AUTHORITY (RCU-published): g_tmapActive points at the map every reader
// (audio drain, output thread, UI BBT) load-acquires per call.  Live edits
// build a NEW map off-thread (copy_with_tempo_at re-anchors at the current
// beat), publish it with release, and retire the old map only after two more
// audio blocks have completed (the g_blockGen grace pattern below) -- so the
// audio thread's tick->sample conversions are lock-free and never race an
// edit.  g_tempoBpm is a DERIVED cache (UI display); it is written only from
// audio_app_set_tempo after the map update -- never independently.
kitchensink::TempoMap  g_tmap;                 // the original static map (never freed)
kitchensink::Transport g_transport{ g_tmap };
std::atomic<const kitchensink::TempoMap*> g_tmapActive{ &g_tmap };
std::atomic<double>    g_tempoBpm{ 120.0 };
std::mutex             g_tmapEditMx;           // serializes map WRITERS (message threads)
struct RetiredMap { const kitchensink::TempoMap* map; unsigned gen; };
std::vector<RetiredMap> g_tmapRetired;         // guarded by g_tmapEditMx

// --- QPC wall clock (hardware MIDI I/O timestamping) ------------------------
#ifdef _WIN32
inline long long qpc_now()
{
    LARGE_INTEGER t; QueryPerformanceCounter( &t );
    return (long long) t.QuadPart;
}
inline double qpc_freq_init()
{
    LARGE_INTEGER f; QueryPerformanceFrequency( &f );
    return (double) f.QuadPart;
}
double g_qpcFreq = qpc_freq_init();            // constant after boot (QPF contract)
#else
inline long long qpc_now()
{
    return (long long) std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch() ).count();
}
double g_qpcFreq = 1e9;
#endif

// --- per-block publication: {block-start sample, QPC at callback entry, QPC
// ticks per sample} + a seqlock generation so MIDI I/O threads get a
// consistent snapshot (audio_app_block_pub).  Written ONLY by audio_render.
std::atomic<long long> g_blockPubSample{ 0 };
std::atomic<long long> g_blockPubQpc{ 0 };
std::atomic<double>    g_qpcPerSample{ 0.0 };
std::atomic<unsigned>  g_blockPubGen{ 0 };     // odd = write in progress; 0 = never

// --- diagnostics: events rejected by the overflow drop policy (note-offs and
// realtime-critical messages are always admitted; see audio_app_route_midi).
std::atomic<unsigned long long> g_midiDropCount{ 0 };
std::atomic<unsigned long long> g_paramDropCount{ 0 };

// Critical messages must survive ring/stage overflow: note-offs (0x80 or 0x90
// with velocity 0) end sound; 0xFA/0xFB/0xFC keep slaved devices coherent.
inline bool midi_is_critical( unsigned char status, unsigned char d2 )
{
    const unsigned char hi = (unsigned char)( status & 0xF0 );
    if ( hi == 0x80 ) return true;
    if ( hi == 0x90 && d2 == 0 ) return true;
    return status >= 0xF8;      // system realtime (clock/start/continue/stop)
}

// --- DSP load meter: fraction of each block's real-time budget the render
// callback consumed (0..1+), exponentially smoothed.  Read by the UI CPU meter.
std::atomic<double> g_cpuLoad{0.0};
struct LoadTimer {
    std::chrono::steady_clock::time_point t0;
    int n;
    explicit LoadTimer(int nframes)
        : t0(std::chrono::steady_clock::now()), n(nframes) {}
    ~LoadTimer() {
        double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0).count();
        double avail = (g_sr > 0.0) ? (double)n / g_sr : 0.0;
        double load  = (avail > 0.0) ? elapsed / avail : 0.0;
        double prev  = g_cpuLoad.load(std::memory_order_relaxed);
        g_cpuLoad.store(prev + 0.10 * (load - prev), std::memory_order_relaxed);
    }
};

// --- offline/realtime bounce capture (message thread sets up; audio thread
// appends interleaved stereo into a preallocated buffer, lock-free via an
// atomic write cursor).  Drained to WAV on the message thread afterwards.
// The buffer pointer is atomic and freed only after a block-generation grace
// period, so an in-flight tap can never write into freed memory.
std::atomic<bool>   g_capturing{false};
std::atomic<float*> g_capBuf{nullptr};
size_t              g_capCap = 0;                 // capacity in FRAMES
std::atomic<size_t> g_capPos{0};                  // frames written
std::atomic<unsigned> g_blockGen{0};              // completed audio blocks (all paths)

std::mutex                 g_previewMutex;
std::unique_ptr<AudioClip> g_previewClip;
long long                  g_previewPos = 0;
float                      g_previewGain = 1.0f;

inline void capture_tap( float** out, int numChannels, int nframes )
{
    // snapshot the buffer pointer ONCE -- the message thread may null it and
    // then waits two block generations before freeing, so this pointer stays
    // valid for the remainder of this block even mid-teardown.
    float* buf = g_capBuf.load( std::memory_order_acquire );
    if ( !buf || !g_capturing.load( std::memory_order_relaxed ) ) return;
    size_t pos = g_capPos.load( std::memory_order_relaxed );
    const float* L = out[0];
    const float* R = ( numChannels > 1 && out[1] ) ? out[1] : out[0];
    for ( int i = 0; i < nframes && pos < g_capCap; ++i, ++pos )
    {
        buf[pos * 2 + 0] = L[i];
        buf[pos * 2 + 1] = R[i];
    }
    g_capPos.store( pos, std::memory_order_relaxed );
}

inline void render_preview_clip( float** out, int numChannels, int nframes )
{
    if ( numChannels < 1 || nframes < 1 ) return;
    std::lock_guard<std::mutex> lock( g_previewMutex );
    if ( !g_previewClip || g_previewClip->empty() )
        return;

    const long long frames = g_previewClip->numFrames();
    if ( g_previewPos >= frames )
    {
        g_previewClip.reset();
        g_previewPos = 0;
        return;
    }

    const float* srcL = g_previewClip->ch[0].empty() ? nullptr : g_previewClip->ch[0].data();
    const float* srcR = g_previewClip->ch[1].empty() ? srcL : g_previewClip->ch[1].data();
    if ( !srcL )
    {
        g_previewClip.reset();
        g_previewPos = 0;
        return;
    }

    float* dstL = out[0];
    float* dstR = ( numChannels > 1 && out[1] ) ? out[1] : out[0];
    int n = (int)std::min<long long>( nframes, frames - g_previewPos );
    const size_t start = (size_t)g_previewPos;
    for ( int i = 0; i < n; ++i )
    {
        float l = srcL[start + (size_t)i] * g_previewGain;
        float r = srcR[start + (size_t)i] * g_previewGain;
        dstL[i] += l;
        dstR[i] += r;
    }
    g_previewPos += n;
    if ( g_previewPos >= frames )
    {
        g_previewClip.reset();
        g_previewPos = 0;
    }
}

// Render the frozen/project audio-clip players and SUM them into `out`.  These
// players live on FIXED-graph tracks (as Track instruments), but the fixed graph
// is NOT processed in modular mode -- so without this, frozen/project audio is
// silent whenever any modular instrument exists (g_modular==true).  Called AFTER
// capture_tap so a fresh freeze never captures already-frozen audio.
inline void render_modular_clip_players( float** out, int numChannels,
                                         int nframes, long long blockStart )
{
    if ( numChannels < 1 || nframes < 1 ) return;
    static thread_local std::vector<float> sL, sR;
    if ( (int)sL.size() < nframes ) { sL.assign( (size_t)nframes, 0.f ); sR.assign( (size_t)nframes, 0.f ); }
    const bool playing = g_transport.rolling();
    float* oL = out[0];
    float* oR = ( numChannels > 1 && out[1] ) ? out[1] : out[0];
    // The frozen/project audio track index == its master-mixer CHANNEL index, so
    // route each player THROUGH its mixer strip: honour gain/mute/pan and feed
    // the channel VU, exactly as if the audio flowed through the mixer node.
    patch::MixerNode* mm = g_patch ? dynamic_cast<patch::MixerNode*>( g_patch->node( g_masterMixerId ) ) : nullptr;
    auto renderOne = [&]( AudioClipPlayer* p, int mixCh ) {
        if ( !p ) return;
        std::fill( sL.begin(), sL.begin() + nframes, 0.f );
        std::fill( sR.begin(), sR.begin() + nframes, 0.f );
        float* aout[2] = { sL.data(), sR.data() };
        ProcessBlock blk{};
        blk.audioIn = nullptr; blk.audioOut = aout; blk.nframes = nframes;
        blk.midiIn = nullptr; blk.numMidiIn = 0;
        blk.paramIn = nullptr; blk.numParamIn = 0;
        blk.tempoBpm = 120.0; blk.playPositionSamples = blockStart; blk.isPlaying = playing;
        blk.numAudioIn = 0; blk.numAudioOut = 2;
        p->process( blk );
        float g = 1.f, panL = 1.f, panR = 1.f;
        if ( mm && mixCh >= 0 && mixCh < mm->channels() ) {
            if ( mm->mute( mixCh ) ) return;                  // channel muted -> silent
            g = mm->gain( mixCh );
            const float pan = mm->pan( mixCh );               // -1 L .. +1 R
            panL = ( pan <= 0.f ) ? 1.f : ( 1.f - pan );
            panR = ( pan >= 0.f ) ? 1.f : ( 1.f + pan );
        }
        float vpk = 0.f;
        for ( int i = 0; i < nframes; ++i ) {
            const float l = sL[i] * g * panL, r = sR[i] * g * panR;
            oL[i] += l; if ( numChannels > 1 && out[1] ) oR[i] += r;
            const float a = std::fabs( l ) > std::fabs( r ) ? std::fabs( l ) : std::fabs( r );
            if ( a > vpk ) vpk = a;
        }
        if ( mm && mixCh >= 0 && mixCh < mm->channels() ) mm->setVu( mixCh, vpk );
    };
    for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t ) renderOne( g_projectAudioPlayers[t], t );
    renderOne( g_freezePlayer, -1 );   // shared clip-freeze track: no mixer channel
}

// Bumps the block-generation counter on EVERY exit from audio_render (RAII, so
// early returns count too).  Teardown paths wait for +2 generations to know no
// callback that predates a pointer swap can still be running.
struct BlockGenBump {
    ~BlockGenBump() { g_blockGen.fetch_add( 1, std::memory_order_release ); }
};

// RCU grace: block until two audio blocks complete so no audio-thread reader
// still holds a schedule snapshot / pointer that references data we are about to
// free, mutate, or shift.  When the stream is stopped (!g_running) no callback
// runs and no reader exists, so it returns immediately; when running it resolves
// in ~2 blocks (a few ms).  Mirrors the capture-buffer grace in capture_end_wav.
inline void freeze_rcu_grace()
{
    if ( !g_running ) return;
    const unsigned gen0 = g_blockGen.load( std::memory_order_acquire );
    for ( int i = 0; i < 250; ++i )
    {
        if ( g_blockGen.load( std::memory_order_acquire ) >= gen0 + 2 ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

// --- lock-free SPSC MIDI ring (producer = sequencer thread, consumer = audio) --
// `tick` is the event's ABSOLUTE due time in c_ppqn (192) sequencer ticks; the
// audio thread converts it at delivery via g_tmapActive so a tempo edit between
// enqueue and drain is resolved by the single time authority.  tick < 0 means
// "due now" (UI preview clicks) and lands at block start.  ORDERING CONTRACT:
// the ring is a FIFO fed by ONE monotonically-scheduling producer (the output
// thread's lookahead scheduler), so FIFO order == tick order and the drain may
// stop at the first future event (holdback).
struct RouteMsg { long long tick; unsigned epoch;
                  unsigned char track, status, d1, d2; };
const unsigned RING_SIZE = 1u << 14;      // 16384
const unsigned RING_MASK = RING_SIZE - 1;
RouteMsg               g_ring[RING_SIZE];
std::atomic<unsigned>  g_head{0};         // written by producer
std::atomic<unsigned>  g_tail{0};         // written by consumer

// --- lock-free SPSC parameter ring (same tick semantics / FIFO contract) ----
struct ParamMsg { long long tick; unsigned epoch;
                  unsigned char track; unsigned int id; float value; };
ParamMsg               g_pring[RING_SIZE];
std::atomic<unsigned>  g_phead{0};
std::atomic<unsigned>  g_ptail{0};

// --- live MIDI record capture (transport-timestamped) -----------------------
// While armed + rolling, every LIVE event (hardware input + on-screen preview,
// NOT the sequencer's own output) is captured on the audio thread with its
// absolute transport tick, into a lock-free SPSC ring the message thread drains
// to build a timeline clip.  The sequencer stream is excluded so overdub records
// only what you actually play.
struct RecEv { long long tick; unsigned char status, d1, d2; };
const unsigned         REC_RING = 1u << 13;      // 8192
RecEv                  g_recRing[REC_RING];
std::atomic<unsigned>  g_recHead{0};             // audio thread (producer)
std::atomic<unsigned>  g_recTail{0};             // message thread (consumer)
std::atomic<bool>      g_recArm{false};
std::atomic<long long> g_recStartTick{0};

inline void rec_capture( long long tick, unsigned char s,
                         unsigned char d1, unsigned char d2 )
{
    if ( !g_recArm.load( std::memory_order_relaxed ) ) return;
    const unsigned h = g_recHead.load( std::memory_order_relaxed );
    if ( h - g_recTail.load( std::memory_order_acquire ) >= REC_RING ) return;  // full: drop
    g_recRing[h % REC_RING] = RecEv{ tick, s, d1, d2 };
    g_recHead.store( h + 1, std::memory_order_release );
}

// --- schedule epoch: bumped on every transport discontinuity (locate) --------
// Producers stamp each TICKED message; the drains discard messages from an
// older epoch, so events scheduled ahead of a seek can never fire displaced at
// the new position.  tick<0 ("now") messages are epoch-exempt.  The MidiIn
// node's already-pushed lookahead is flushed by the audio thread on the first
// block after the bump (g_flushMidiIn).
std::atomic<unsigned>  g_schedEpoch{0};
std::atomic<bool>      g_flushMidiIn{false};

// --- engine-side loop (sample-accurate, applied by cycle-splitting) ----------
// The scheduler publishes the loop as TICKS; samples are (re)derived from the
// ACTIVE tempo map here and whenever the map is republished, so a tempo change
// moves the loop points with the music.
const int               kMaxDeviceCh = 16;   // cycle-split pointer scratch bound
std::atomic<bool>       g_loopOn{false};
std::atomic<long long>  g_loopStartSample{0};
std::atomic<long long>  g_loopEndSample{0};
long long               g_loopLeftTick  = 0;   // message thread only
long long               g_loopRightTick = 0;

// Producer-side spinlocks.  audio_app_route_midi / _param can be called from
// BOTH the sequencer output thread AND the GUI thread (piano-roll/tracker
// preview + automation), so the enqueue (a non-atomic head RMW) must be
// serialized between producers.  The audio consumer (audio_render) NEVER takes
// these -- it only acquire-loads head and release-stores tail -- so the audio
// thread stays lock-free.  Held for a few instructions only.
std::atomic_flag g_midi_producer = ATOMIC_FLAG_INIT;
std::atomic_flag g_param_producer = ATOMIC_FLAG_INIT;

// --- per-block staging (audio thread only) ---------------------------------
const int MAX_EV = 256;
const int MAX_PC = 256;
MidiEvent        s_trackMidi [PatchKnob::app::AUDIO_APP_MAX_TRACKS][MAX_EV];
int              s_counts    [PatchKnob::app::AUDIO_APP_MAX_TRACKS];
ParamChange      s_trackParam[PatchKnob::app::AUDIO_APP_MAX_TRACKS][MAX_PC];
int              s_pcounts   [PatchKnob::app::AUDIO_APP_MAX_TRACKS];
TrackBlockInput  s_inputs    [PatchKnob::app::AUDIO_APP_MAX_TRACKS];

// The single audio render callback: drain queued MIDI into per-track buffers,
// then let the graph render every track's instrument + FX + mix.
//
// TIMING DISCIPLINE: the block-START sample is captured FIRST and used for all
// offset math (nerf 17); g_transport.process() advances the playhead at the
// very END of the callback (it also applies any pending seek so the NEXT
// block starts at the seek target).  The drain converts each event's tick to
// its absolute due sample via the active tempo map and HOLDS BACK events due
// in a future block, so a 720-sample 32nd grid lands at exact sample offsets
// instead of snapping to the 512-sample block grid (nerfs 1/2/4).
// Live-input staging ring (SPSC: RtMidi callback thread -> audio thread).  The
// default MidiIn node's own ring must keep a SINGLE producer (this audio-thread
// drain), so hardware events are staged here (patch_midi_cb) and forwarded at
// block start -- BEFORE the sequencer drain, since live dues are the soonest.
struct LiveMidi { long long due; MidiEvent ev; };
const unsigned LIVE_RING_SIZE = 256;
LiveMidi              g_liveRing[LIVE_RING_SIZE];
std::atomic<unsigned> g_liveHead{0};   // producer (RtMidi thread)
std::atomic<unsigned> g_liveTail{0};   // consumer (audio thread)

inline void drain_live_midi()
{
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    unsigned t = g_liveTail.load( std::memory_order_relaxed );
    const unsigned h = g_liveHead.load( std::memory_order_acquire );
    while ( t != h )
    {
        const LiveMidi& lm = g_liveRing[t % LIVE_RING_SIZE]; ++t;
        // BROADCAST live/hardware keyboard input to every "Instrument out" plug so it
        // reaches ALL wired instruments (each channel-filters downstream) -- per-track
        // plugs would otherwise pin live input to track 0 only.
        if ( g_midiInNode ) g_midiInNode->push( lm.ev, lm.due, -1 );
        // hardware live input is real performance -> capture for recording.
        rec_capture( map->sample_to_tick( lm.due >= 0 ? lm.due : g_transport.sample() ),
                     lm.ev.status, lm.ev.data1, lm.ev.data2 );
    }
    g_liveTail.store( t, std::memory_order_release );
}

// Renders ONE contiguous segment of the timeline (a whole device block, or a
// slice of one when the block is cycle-split at the loop boundary -- see the
// audio_render wrapper below).
void audio_render_segment( float** out, int numChannels, int nframes, double /*sr*/ )
{
    LoadTimer    _lt( nframes ); // measure DSP load for the CPU meter (all exits)
    BlockGenBump _bg;            // count this block on all exits (teardown grace)

    const kitchensink::TempoMap* map =
        g_tmapActive.load( std::memory_order_acquire );        // RCU snapshot
    const long long blockStart = g_transport.sample();          // BEFORE advancing
    const long long blockEnd   = blockStart + (long long) nframes;
    const unsigned  epoch      = g_schedEpoch.load( std::memory_order_acquire );

    // Loop-aware holdback: while looping, the scheduler legitimately enqueues
    // NEXT-pass events (dues near loopStart) before the engine wraps.  A due
    // far behind the playhead is that wrapped future -- hold it (and, FIFO,
    // everything after it) for the post-wrap blocks.  Genuinely-late events
    // (small lag) still clamp to "now" via the half-loop margin.
    const bool      loopHold = g_loopOn.load( std::memory_order_relaxed )
                               && g_transport.rolling();
    const long long loopHalf = loopHold
        ? ( g_loopEndSample.load( std::memory_order_relaxed )
            - g_loopStartSample.load( std::memory_order_relaxed ) ) / 2 : 0;
    auto is_wrapped_future = [&]( long long due ) {
        return loopHold && due < blockStart && ( blockStart - due ) > loopHalf;
    };

    // A locate happened: the MidiIn node's scheduled-ahead lookahead references
    // the OLD position -- flush it before this block delivers anything.
    if ( g_flushMidiIn.exchange( false, std::memory_order_acq_rel ) && g_midiInNode )
        g_midiInNode->flush();

    // publish {blockStart, QPC now, QPC/sample} for the MIDI I/O threads.
    // Seqlock: gen goes odd, data stores, gen goes even (audio thread is the
    // only writer, so plain increments suffice).
    {
        const long long nowQpc = qpc_now();
        const unsigned g = g_blockPubGen.load( std::memory_order_relaxed );
        g_blockPubGen.store( g + 1, std::memory_order_relaxed );
        std::atomic_thread_fence( std::memory_order_release );
        g_blockPubSample.store( blockStart, std::memory_order_relaxed );
        g_blockPubQpc.store( nowQpc, std::memory_order_relaxed );
        g_qpcPerSample.store( ( g_sr > 0.0 ) ? g_qpcFreq / g_sr : 0.0,
                              std::memory_order_relaxed );
        g_blockPubGen.store( g + 2, std::memory_order_release );
    }

    // --- modular mode: render the patch graph instead of the fixed mixer ---
    if ( g_modular.load( std::memory_order_relaxed ) && g_patch )
    {
        // Live hardware input first (its dues are the soonest), keeping this
        // thread the MidiIn node ring's ONLY producer.
        drain_live_midi();
        // HOLDBACK drain of the MIDI ring into the patch's MidiIn node.  FIFO
        // order == tick order (single monotonic producer, see RouteMsg), so we
        // stop at the first event due beyond this block and leave it queued.
        unsigned tail = g_tail.load( std::memory_order_relaxed );
        const unsigned head = g_head.load( std::memory_order_acquire );
        while ( tail != head )
        {
            const RouteMsg& m = g_ring[tail & RING_MASK];
            // ticked events from BEFORE a locate reference the old position:
            // discard (epoch mismatch).  tick<0 previews are epoch-exempt.
            if ( m.tick >= 0 && m.epoch != epoch ) { ++tail; continue; }
            const long long due = ( m.tick < 0 ) ? blockStart
                                                 : map->tick_to_sample( m.tick );
            if ( due >= blockEnd ) break;            // future block: hold back
            if ( m.tick >= 0 && is_wrapped_future( due ) ) break;   // next pass
            if ( g_midiInNode ) {
                MidiEvent e; e.sampleOffset = 0; e.status = m.status;
                e.data1 = m.d1; e.data2 = m.d2;
                // route to this track's "Instrument out" plug (track == bus); the
                // node clamps an out-of-range track to plug 0.
                g_midiInNode->push( e, due, (int) m.track );
            }
            if ( m.tick < 0 )                        // live preview -> record it
                rec_capture( map->sample_to_tick( blockStart ), m.status, m.d1, m.d2 );
            ++tail;
        }
        g_tail.store( tail, std::memory_order_release );

        RenderContext ctx;
        ctx.tempoBpm            = map->tempo_at_sample( blockStart );  // map-derived
        ctx.playPositionSamples = blockStart;               // block-START position
        ctx.isPlaying           = g_patchPlaying.load( std::memory_order_relaxed );
        g_patchPlayPos          = blockStart;               // keep legacy mirror in sync
        g_patch->process( out, numChannels, nframes, ctx );
        render_preview_clip( out, numChannels, nframes );
        capture_tap( out, numChannels, nframes );
        // Frozen/project audio lives on fixed-graph tracks that this modular path
        // does not render -- sound them here so freeze playback is audible.
        render_modular_clip_players( out, numChannels, nframes, blockStart );
        g_transport.process( nframes );   // advance at END (+ pending seek for next block)
        return;
    }

    const int NT = PatchKnob::app::AUDIO_APP_MAX_TRACKS;
    for ( int t = 0; t < NT; ++t ) { s_counts[t] = 0; s_pcounts[t] = 0; }

    // fixed-graph transport parity (nerf 18): tempo/position/run-state every
    // block, from the same map + block-start sample as the event drain.
    g_graph->setTransport( map->tempo_at_sample( blockStart ), blockStart,
                           g_transport.rolling() );

    // drain MIDI ring (HOLDBACK; same contract as the modular drain above)
    unsigned tail = g_tail.load( std::memory_order_relaxed );
    const unsigned head = g_head.load( std::memory_order_acquire );
    while ( tail != head )
    {
        const RouteMsg& m = g_ring[tail & RING_MASK];
        if ( m.tick >= 0 && m.epoch != epoch ) { ++tail; continue; }   // stale (pre-locate)
        const long long due = ( m.tick < 0 ) ? blockStart
                                             : map->tick_to_sample( m.tick );
        if ( due >= blockEnd ) break;                // future block: hold back
        if ( m.tick >= 0 && is_wrapped_future( due ) ) break;         // next pass
        if ( m.tick < 0 )                            // live preview -> record it
            rec_capture( map->sample_to_tick( blockStart ), m.status, m.d1, m.d2 );
        ++tail;
        int t = m.track;
        if ( t >= 0 && t < NT )
        {
            long long rel = due - blockStart;        // late events clamp to 0 (never early)
            if ( rel < 0 ) rel = 0;
            if ( rel > (long long) nframes - 1 ) rel = nframes - 1;
            if ( s_counts[t] < MAX_EV )
            {
                MidiEvent& e = s_trackMidi[t][ s_counts[t]++ ];
                e.sampleOffset = (int) rel;
                e.status = m.status;
                e.data1  = m.d1;
                e.data2  = m.d2;
            }
            else if ( midi_is_critical( m.status, m.d2 ) )
            {
                // stage full: never lose a note-off -- overwrite the newest
                // staged event (a note-on is the acceptable casualty).
                MidiEvent& e = s_trackMidi[t][MAX_EV - 1];
                e.sampleOffset = (int) rel;
                e.status = m.status;
                e.data1  = m.d1;
                e.data2  = m.d2;
                g_midiDropCount.fetch_add( 1, std::memory_order_relaxed );
            }
            else
                g_midiDropCount.fetch_add( 1, std::memory_order_relaxed );
        }
    }
    g_tail.store( tail, std::memory_order_release );

    // per-track insertion sort by offset: FIFO drain is already tick-ordered,
    // but preview events (tick=-1 -> offset 0) may interleave; plugins expect
    // non-decreasing offsets.  Allocation-free; n is tiny in practice.
    for ( int t = 0; t < NT; ++t )
        for ( int i = 1; i < s_counts[t]; ++i )
        {
            MidiEvent e = s_trackMidi[t][i];
            int j = i - 1;
            while ( j >= 0 && s_trackMidi[t][j].sampleOffset > e.sampleOffset )
            { s_trackMidi[t][j + 1] = s_trackMidi[t][j]; --j; }
            s_trackMidi[t][j + 1] = e;
        }

    // drain parameter ring (same holdback + offset stamping)
    unsigned ptail = g_ptail.load( std::memory_order_relaxed );
    const unsigned phead = g_phead.load( std::memory_order_acquire );
    while ( ptail != phead )
    {
        const ParamMsg& m = g_pring[ptail & RING_MASK];
        if ( m.tick >= 0 && m.epoch != epoch ) { ++ptail; continue; }  // stale (pre-locate)
        const long long due = ( m.tick < 0 ) ? blockStart
                                             : map->tick_to_sample( m.tick );
        if ( due >= blockEnd ) break;                // future block: hold back
        if ( m.tick >= 0 && is_wrapped_future( due ) ) break;         // next pass
        ++ptail;
        int t = m.track;
        if ( t >= 0 && t < NT && s_pcounts[t] < MAX_PC )
        {
            long long rel = due - blockStart;
            if ( rel < 0 ) rel = 0;
            if ( rel > (long long) nframes - 1 ) rel = nframes - 1;
            ParamChange& p = s_trackParam[t][ s_pcounts[t]++ ];
            p.id           = m.id;
            p.sampleOffset = (int) rel;
            p.value        = m.value;
        }
    }
    g_ptail.store( ptail, std::memory_order_release );

    for ( int t = 0; t < NT; ++t )
    {
        s_inputs[t].midi   = s_trackMidi[t];
        s_inputs[t].nMidi  = s_counts[t];
        s_inputs[t].autom  = s_trackParam[t];
        s_inputs[t].nAutom = s_pcounts[t];
    }

    g_graph->renderBlock( out, numChannels, nframes, s_inputs, NT );
    render_preview_clip( out, numChannels, nframes );
    capture_tap( out, numChannels, nframes );
    g_transport.process( nframes );   // advance at END (+ pending seek for next block)
}

// The device callback: renders whole blocks, CYCLE-SPLITTING at the loop-end
// boundary so the playhead jumps to loopStart at the EXACT frame (not at a
// block edge, not ~a-lookahead early).  Every sub-segment goes through the
// full segment body above, so event dues, holdback, offsets and transport
// bookkeeping stay consistent inside the split.
void audio_render( float** out, int numChannels, int nframes, double sr )
{
    if ( !g_loopOn.load( std::memory_order_relaxed ) || !g_transport.rolling() )
    {
        audio_render_segment( out, numChannels, nframes, sr );
        return;
    }

    const long long lstart = g_loopStartSample.load( std::memory_order_relaxed );
    const long long lend   = g_loopEndSample.load( std::memory_order_relaxed );
    if ( lend <= lstart )                       // degenerate loop: ignore
    {
        audio_render_segment( out, numChannels, nframes, sr );
        return;
    }

    float* seg[kMaxDeviceCh];
    int done = 0, splits = 0;
    while ( done < nframes )
    {
        const long long pos  = g_transport.sample();
        long long       room = lend - pos;      // frames until the wrap frame
        if ( room <= 0 )
        {
            // at/past the boundary (or markers just moved): wrap NOW.  The
            // pending seek applies at the start of the next process() call.
            if ( ++splits > 4 ) { room = nframes - done; }   // runaway guard
            else
            {
                g_transport.request_seek( lstart );
                g_transport.process( 0 );        // apply immediately (audio thread)
                continue;
            }
        }
        int n = nframes - done;
        if ( (long long) n > room ) n = (int) room;
        for ( int c = 0; c < numChannels && c < kMaxDeviceCh; ++c )
            seg[c] = out[c] + done;
        audio_render_segment( seg, numChannels, n, sr );
        done += n;
    }
}

PluginDescriptor make_descriptor( const char* path )
{
    PluginDescriptor d;
    std::string p( path ? path : "" );
    bool is_vst3 = ( p.size() >= 5 &&
                     p.compare( p.size() - 5, 5, ".vst3" ) == 0 );
    d.format       = is_vst3 ? PluginFormat::VST3 : PluginFormat::VST2;
    d.path         = p;
    d.uid          = "";
    d.name         = p;
    d.isInstrument = true;
    d.numAudioIn   = 0;
    d.numAudioOut  = 2;
    return d;
}

} // anonymous namespace

namespace PatchKnob { namespace app {

bool audio_app_init()
{
    if ( g_running ) return true;

    g_engine = new AudioEngine();
    g_graph  = new MixerGraph();
    g_host   = new PluginHost();

    g_graph->setTrackCount( AUDIO_APP_MAX_TRACKS );   // before prepare()

    // Prefer WASAPI shared over an MME default (nerf 12): MME suggests ~90ms
    // of latency and fires callbacks in timer-driven bursts, both fatal to
    // sample-accurate delivery.  Only overrides the DEFAULT choice -- explicit
    // host-API selection via audio_app_set_hostapi() behaves exactly as before.
    {
        auto apis = g_engine->enumerateHostApis();
        int wasapi = -1; bool curIsMme = false;
        for ( size_t i = 0; i < apis.size(); ++i )
        {
            if ( apis[i].name.find( "WASAPI" ) != std::string::npos ) wasapi = apis[i].index;
            if ( apis[i].isCurrent && apis[i].name.find( "MME" ) != std::string::npos )
                curIsMme = true;
        }
        if ( curIsMme && wasapi >= 0 )
        {
            fprintf( stderr, "[audio] default host API is MME; preferring WASAPI\n" );
            g_engine->selectHostApi( wasapi );
        }
    }

    if ( !g_engine->open( 48000, 512, 2 ) )
    {
        // the WASAPI preference may not open on every setup: fall back to the
        // PortAudio default backend before giving up.
        fprintf( stderr, "[audio] open failed (%s); retrying on default backend\n",
                 g_engine->lastError().c_str() );
        g_engine->stop(); g_engine->close();
        g_engine->selectHostApi( -1 );
        if ( !g_engine->open( 48000, 512, 2 ) )
        {
            fprintf( stderr, "[audio] could not open output device: %s\n",
                     g_engine->lastError().c_str() );
            return false;
        }
    }

    g_sr    = (double) g_engine->sampleRate();
    g_block = g_engine->blockSize();

    g_graph->prepare( g_sr, g_block );

    // kitchensink musical clock: default 120 BPM 4/4 at the device sample rate.
    g_tmap.set_sample_rate( g_sr );
    g_tmap.set_meter( 4, 4 );
    g_tmap.set_tempo( 120.0 );
    g_transport.set_sample_rate( g_sr );
    g_tempoBpm.store( 120.0 );

    // fixed-graph transport: seeded here, refreshed EVERY block by audio_render
    // from the active tempo map (nerf 18 -- no more frozen 120/0/playing).
    g_graph->setTransport( g_tmap.tempo_at_sample( 0 ), g_transport.sample(),
                           g_transport.rolling() );

    // modular patch graph (alternate render core): start with an audio-output
    // node + a MIDI-input node the patchbay UI can wire to.
    g_patch = new patch::PatchGraph();
    g_patch->prepare( g_sr, g_block );
    g_patchOutId    = g_patch->addNode( std::make_unique<patch::AudioDeviceOutNode>( 2 ) );
    g_patchMidiInId = g_patch->addNode( std::make_unique<patch::MidiInNode>() );
    g_midiInNode    = static_cast<patch::MidiInNode*>( g_patch->node( g_patchMidiInId ) );
    g_patch->setDeviceOutNode( g_patchOutId );   // designate the master sink
    // Master mixer: 8 bus channels summed to the device output (+ a MIDI clock
    // out).  Tracks wire to its channels (port ch+1); its output (port 0) feeds
    // the master out.  Singleton -- created here, never added/removed by the UI.
    g_masterMixerId = g_patch->addNode( std::make_unique<patch::MasterMixerNode>() );
    g_patch->connect( patch::Connection{ patch::PortRef{ g_masterMixerId, 0 },
                                         patch::PortRef{ g_patchOutId, 0 } } );
    g_patch->compileAndPublish();

    g_engine->setRenderCallback( &audio_render );

    if ( !g_engine->start() )
    {
        fprintf( stderr, "[audio] could not start stream: %s\n",
                 g_engine->lastError().c_str() );
        return false;
    }

    g_running = true;
    fprintf( stderr, "[audio] engine running: %.0f Hz, %d-frame blocks, %d tracks\n",
             g_sr, g_block, AUDIO_APP_MAX_TRACKS );
    return true;
}

static void midi_ports_shutdown();   // defined below (with the MIDI-port machinery)

void audio_app_shutdown()
{
    midi_ports_shutdown();   // stop the MIDI-out drain thread + close all hardware ports
    if ( g_engine ) { g_engine->stop(); g_engine->close(); }
    // RCU tempo maps: the stream is stopped, so no reader can be in flight.
    // Point everything back at the original static map and free every copy.
    {
        std::lock_guard<std::mutex> lk( g_tmapEditMx );
        const kitchensink::TempoMap* act = g_tmapActive.exchange( &g_tmap );
        g_transport.set_map( g_tmap );
        if ( act && act != &g_tmap ) delete act;
        for ( size_t i = 0; i < g_tmapRetired.size(); ++i )
            if ( g_tmapRetired[i].map != &g_tmap ) delete g_tmapRetired[i].map;
        g_tmapRetired.clear();
    }
    audio_app_project_clear_audio_clips();
    for ( IPluginInstance* inst : g_owned )
    {
        if ( inst ) { inst->setActive( false ); inst->release(); delete inst; }
    }
    g_owned.clear();
    delete g_engine; g_engine = nullptr;
    delete g_graph;  g_graph  = nullptr;
    delete g_host;   g_host   = nullptr;
    g_running = false;
}

bool audio_app_running() { return g_running; }

void audio_app_preview_clip( const AudioClip& clip, float gain )
{
    std::lock_guard<std::mutex> lock( g_previewMutex );
    if ( clip.empty() )
    {
        g_previewClip.reset();
        g_previewPos = 0;
        return;
    }
    if ( gain < 0.f ) gain = 0.f;
    if ( gain > 4.f ) gain = 4.f;
    g_previewClip.reset( new AudioClip( clip ) );
    g_previewPos = 0;
    g_previewGain = gain;
}

// --- audio device (sound driver) selection ---------------------------------
int audio_app_device_count()
{
    if ( !g_engine ) return 0;
    return (int) g_engine->enumerateOutputDevices().size();
}

bool audio_app_device_info( int idx, unsigned* outId, char* nameBuf,
                            int nameBufLen, int* outIsCurrent )
{
    if ( !g_engine ) return false;
    auto devs = g_engine->enumerateOutputDevices();
    if ( idx < 0 || idx >= (int) devs.size() ) return false;
    if ( outId ) *outId = devs[idx].id;
    if ( nameBuf && nameBufLen > 0 ) {
        const std::string& n = devs[idx].name;
        int k = (int) n.size(); if ( k > nameBufLen-1 ) k = nameBufLen-1;
        memcpy( nameBuf, n.data(), k ); nameBuf[k] = 0;
    }
    if ( outIsCurrent ) *outIsCurrent = ( devs[idx].id == g_engine->selectedDeviceId() ) ? 1 : 0;
    return true;
}

namespace {
// Bring the freshly-opened stream fully live (re-prepare graphs, start).
bool start_opened()
{
    g_sr    = (double) g_engine->sampleRate();
    g_block = g_engine->blockSize();
    g_graph->prepare( g_sr, g_block );
    // transport is refreshed per block by audio_render; seed from the live state
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    g_graph->setTransport( map->tempo_at_sample( g_transport.sample() ),
                           g_transport.sample(), g_transport.rolling() );
    g_patch->prepare( g_sr, g_block );
    g_patch->compileAndPublish();
    g_engine->setRenderCallback( &audio_render );
    return g_engine->start();
}

// Re-open + re-prepare after a backend/device/buffer change.  Robust: if the
// requested config can't open (WASAPI/WDM-KS often reject a default device or
// the 48k/512 format), fall back to the host-API default, then to the global
// default -- so a bad choice never leaves the DAW silent.  Caller has already
// stopped+closed the engine and applied its selection.
bool reopen_engine()
{
    if ( g_engine->open( 48000, 512, 2 ) && start_opened() ) return true;

    fprintf( stderr, "[audio] open failed (%s); trying host-API default device\n",
             g_engine->lastError().c_str() );
    g_engine->stop(); g_engine->close();
    g_engine->selectDevice( 0 );                 // host-API default output
    if ( g_engine->open( 48000, 512, 2 ) && start_opened() ) return true;

    fprintf( stderr, "[audio] still failing (%s); reverting to default backend\n",
             g_engine->lastError().c_str() );
    g_engine->stop(); g_engine->close();
    g_engine->selectHostApi( -1 );               // PortAudio default host API
    g_engine->selectDevice( 0 );
    g_engine->setBufferSize( 512 );
    if ( g_engine->open( 48000, 512, 2 ) && start_opened() ) return true;

    fprintf( stderr, "[audio] could not reopen ANY device: %s\n",
             g_engine->lastError().c_str() );
    return false;
}
} // namespace

// Re-open the stream on a different output device.
bool audio_app_set_device( unsigned deviceId )
{
    if ( !g_engine || !g_running ) return false;
    g_engine->stop(); g_engine->close();
    g_engine->selectDevice( deviceId );
    return reopen_engine();
}

// --- input device (for a future capture/record path) -----------------------
// Enumerated + selectable now; the render engine is output-only so the choice
// is stored for when recording lands.
static unsigned g_inputDev = 0;

int audio_app_input_device_count()
{
    return g_engine ? (int) g_engine->enumerateInputDevices().size() : 0;
}

bool audio_app_input_device_info( int idx, unsigned* outId, char* nameBuf,
                                  int nameBufLen, int* outIsCurrent )
{
    if ( !g_engine ) return false;
    auto devs = g_engine->enumerateInputDevices();
    if ( idx < 0 || idx >= (int) devs.size() ) return false;
    if ( outId ) *outId = devs[idx].id;
    if ( nameBuf && nameBufLen > 0 ) {
        const std::string& n = devs[idx].name;
        int k = (int) n.size(); if ( k > nameBufLen-1 ) k = nameBufLen-1;
        memcpy( nameBuf, n.data(), k ); nameBuf[k] = 0;
    }
    if ( outIsCurrent ) *outIsCurrent = ( devs[idx].id == g_inputDev ) ? 1 : 0;
    return true;
}

void audio_app_set_input_device( unsigned deviceId ) { g_inputDev = deviceId; }

// --- live MIDI input for the modular MIDI-In node ---------------------------
// A hardware MIDI keyboard selected on the patcher's MIDI-In node.  Its events
// are routed through the same lock-free ring the sequencer uses (spinlock-safe
// multi-producer), so they reach the patch MidiIn node on the audio thread.
static RtMidiIn* g_midiInDev  = nullptr;
static int       g_midiInPort = -1;

// Stamp "now" (this RtMidi callback) into the transport sample domain via the
// audio thread's block publication, plus one block of safety so the event can
// never be already late when the audio thread picks it up (nerf 16).  Falls
// back to -1 ("deliver at block start") until the first block has published.
static long long hw_in_due_sample()
{
    // Live input while the transport is STOPPED: blockStart (g_transport.sample())
    // is frozen, so a wall-clock lookahead diverges past the block end and the
    // MidiIn node would hold the event back forever -- i.e. keyboard input would be
    // silent whenever you're not playing.  Deliver "now" (offset 0) when not
    // rolling so live hardware input always sounds.
    if ( !g_transport.rolling() ) return -1;
    long long ps = 0, pq = 0; double qps = 0.0;
    if ( !audio_app_block_pub( &ps, &pq, &qps ) || qps <= 0.0 ) return -1;
    const double dSamples = (double)( qpc_now() - pq ) / qps;
    return ps + (long long) llround( dSamples ) + (long long) g_block;
}

static void patch_midi_cb( double, std::vector<unsigned char>* msg, void* )
{
    if ( !msg || msg->empty() ) return;
    MidiEvent e{};
    e.sampleOffset = 0;
    e.status = (*msg)[0];
    e.data1  = msg->size() > 1 ? (*msg)[1] : 0;
    e.data2  = msg->size() > 2 ? (*msg)[2] : 0;
    // Stage into the live ring (anonymous-namespace, SPSC with THIS thread as
    // the only producer); the audio thread forwards at block start so the
    // MidiIn node ring keeps its single producer.
    const unsigned h = g_liveHead.load( std::memory_order_relaxed );
    if ( h - g_liveTail.load( std::memory_order_acquire ) >= LIVE_RING_SIZE ) return;  // full: drop
    g_liveRing[h % LIVE_RING_SIZE] = LiveMidi{ hw_in_due_sample(), e };
    g_liveHead.store( h + 1, std::memory_order_release );
}

static bool ensure_midi_in()
{
    if ( g_midiInDev ) return true;
    try { g_midiInDev = new RtMidiIn(); } catch ( ... ) { g_midiInDev = nullptr; }
    return g_midiInDev != nullptr;
}

// ---------------------------------------------------------------------------
// PipeWire-style patchable MIDI ports.
//
// Every MIDI-In / MIDI-Out node in the patcher can be either VIRTUAL (a pure
// routing point / merge) or bound to a real hardware port.  A hardware MIDI-In
// node owns an RtMidiIn whose callback pushes straight into that node's
// lock-free ring; a hardware MIDI-Out node owns an RtMidiOut a background drain
// thread feeds from that node's ring.  You wire them freely in the graph.
// ---------------------------------------------------------------------------
struct HwMidiIn  { patch::NodeId node; patch::MidiInNode*  ring; RtMidiIn*  dev; };
struct HwMidiOut { patch::NodeId node; patch::MidiOutNode* ring; RtMidiOut* dev; };
static std::vector<HwMidiIn>  g_hwMidiIns;
static std::vector<HwMidiOut> g_hwMidiOuts;
static std::mutex             g_hwMidiMx;         // guards both vectors
static std::thread            g_midiOutThread;
static std::atomic<bool>      g_midiOutRun{ false };
static std::atomic<unsigned>  g_drainGen{ 0 };           // completed drain passes (removal grace)
static RtMidiOut*             g_midiOutEnum = nullptr;   // enumeration-only handle

// Byte length of a raw MIDI status (so we send the right count to WinMM).
static int midi_msg_len( unsigned char status )
{
    unsigned char hi = status & 0xF0;
    if ( status >= 0xF8 )                 return 1;   // system realtime (clock/start/stop)
    if ( status == 0xF1 || status == 0xF3 ) return 2;
    if ( status == 0xF2 )                 return 3;   // song position
    if ( hi == 0xC0 || hi == 0xD0 )       return 2;   // program change / channel pressure
    return 3;                                          // note/cc/pitch-bend
}

// RtMidi input thread -> push directly into the bound node's ring (lock-free),
// stamped with its QPC arrival time mapped into the sample domain (nerf 16) so
// live input no longer snaps to the block grid.
static void hw_midi_in_cb( double, std::vector<unsigned char>* msg, void* ud )
{
    auto* node = static_cast<patch::MidiInNode*>( ud );
    if ( !node || !msg || msg->empty() ) return;
    MidiEvent e{};
    e.sampleOffset = 0;
    e.status = (*msg)[0];
    e.data1  = msg->size() > 1 ? (*msg)[1] : 0;
    e.data2  = msg->size() > 2 ? (*msg)[2] : 0;
    node->push( e, hw_in_due_sample() );
}

// Drains every hardware MIDI-out node's ring to its RtMidiOut, honoring each
// event's timestamp (nerf 8): dueSample is converted to a QPC deadline via the
// audio thread's block publication, long waits ride a high-resolution waitable
// timer and the sub-millisecond remainder is QPC-spun, so wire timing tracks
// the audio clock within ~0.1ms instead of bursting per block.  The port
// registry is SNAPSHOT under the mutex and sends happen OUTSIDE it (nerf 26),
// so a UI-thread rescan can no longer stall the sender mid-stream.
static void midi_out_drain_loop()
{
#ifdef _WIN32
    // MIDI timing thread: outrank normal threads so a busy UI can't starve the
    // clock.  (timeBeginPeriod(1) at app start makes short waits real.)
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
    HANDLE timer = CreateWaitableTimerExW( nullptr, nullptr,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS );
    if ( !timer ) timer = CreateWaitableTimerW( nullptr, TRUE, nullptr );
#endif
    // registry snapshot (refreshed every pass; bounded, no allocation in the loop)
    const int MAX_PORTS = 64;
    struct Snap { patch::NodeId node; patch::MidiOutNode* ring; RtMidiOut* dev; };
    Snap snap[MAX_PORTS]; int nSnap = 0;
    // pending sends ordered by QPC deadline.  Entries hold the NODE id, not the
    // device pointer -- the device is re-resolved against the fresh snapshot at
    // send time, so a removed port is dropped instead of dereferenced.
    struct PendingHw { long long dueQpc; patch::NodeId node; MidiEvent ev; };
    std::vector<PendingHw> pending;
    pending.reserve( 1024 );          // one-time; the loop itself never allocates past this
    const size_t MAX_PENDING = 4096;

    patch::TimedMidi tm;
    while ( g_midiOutRun.load( std::memory_order_relaxed ) )
    {
        // 1) snapshot the port registry under the mutex (no I/O while held)
        {
            std::lock_guard<std::mutex> lk( g_hwMidiMx );
            nSnap = 0;
            for ( auto& h : g_hwMidiOuts )
                if ( h.ring && h.dev && nSnap < MAX_PORTS )
                    { snap[nSnap].node = h.node; snap[nSnap].ring = h.ring;
                      snap[nSnap].dev = h.dev; ++nSnap; }
        }

        // 2) pop everything available, stamping QPC deadlines from the block pub
        long long pubS = 0, pubQ = 0; double qps = 0.0;
        const bool havePub = audio_app_block_pub( &pubS, &pubQ, &qps );
        for ( int i = 0; i < nSnap; ++i )
        {
            while ( pending.size() < MAX_PENDING && snap[i].ring->pop( tm ) )
            {
                long long dueQ = 0;                       // 0 == "send now"
                if ( havePub && tm.dueSample >= 0 )
                    dueQ = pubQ + (long long) llround(
                               (double)( tm.dueSample - pubS ) * qps );
                PendingHw p; p.dueQpc = dueQ; p.node = snap[i].node; p.ev = tm.ev;
                pending.push_back( p );
            }
        }
        if ( pending.size() > 1 )
            std::stable_sort( pending.begin(), pending.end(),
                []( const PendingHw& a, const PendingHw& b )
                { return a.dueQpc < b.dueQpc; } );

        // 3) send everything due, OUTSIDE the mutex
        size_t sent = 0;
        long long now = qpc_now();
        while ( sent < pending.size() && pending[sent].dueQpc <= now )
        {
            const PendingHw& p = pending[sent];
            RtMidiOut* dev = nullptr;
            for ( int i = 0; i < nSnap; ++i )
                if ( snap[i].node == p.node ) { dev = snap[i].dev; break; }
            if ( dev ) {
                unsigned char m[3] = { p.ev.status, p.ev.data1, p.ev.data2 };
                try { dev->sendMessage( m, (size_t) midi_msg_len( p.ev.status ) ); }
                catch ( ... ) {}
            }
            ++sent;
            now = qpc_now();
        }
        if ( sent ) pending.erase( pending.begin(), pending.begin() + (long)sent );

        g_drainGen.fetch_add( 1, std::memory_order_release );  // pass complete (removal grace)

        // 4) pace toward the next deadline, but never sleep more than ~1ms so
        //    freshly queued events are noticed promptly.  Far deadlines: 1ms
        //    high-res timer ticks; the last <=1.5ms is QPC-spun to the sample.
        double waitMs = 1.0;
        if ( !pending.empty() )
        {
            const long long due = pending.front().dueQpc;
            const double dtMs = (double)( due - qpc_now() ) * 1000.0 / g_qpcFreq;
            if ( dtMs <= 1.5 )
            {
                while ( g_midiOutRun.load( std::memory_order_relaxed ) &&
                        qpc_now() < due ) { /* sub-ms QPC spin */ }
                waitMs = 0.0;                             // due now: loop sends it
            }
        }
        if ( waitMs > 0.0 )
        {
#ifdef _WIN32
            bool waited = false;
            if ( timer )
            {
                LARGE_INTEGER rel;
                rel.QuadPart = -(LONGLONG)( waitMs * 10000.0 );   // 100ns units, relative
                if ( SetWaitableTimer( timer, &rel, 0, nullptr, nullptr, FALSE ) )
                    { WaitForSingleObject( timer, 2 ); waited = true; }
            }
            if ( !waited )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
#else
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
#endif
        }
    }
#ifdef _WIN32
    if ( timer ) CloseHandle( timer );
#endif
}

static void midi_out_thread_start()
{
    if ( !g_midiOutRun.exchange( true ) )
        g_midiOutThread = std::thread( midi_out_drain_loop );
}

static void midi_ports_shutdown()
{
    if ( g_midiOutRun.exchange( false ) && g_midiOutThread.joinable() )
        g_midiOutThread.join();
    std::lock_guard<std::mutex> lk( g_hwMidiMx );
    for ( auto& h : g_hwMidiIns )  { try { if (h.dev){ h.dev->cancelCallback(); h.dev->closePort(); } } catch(...){} delete h.dev; }
    for ( auto& h : g_hwMidiOuts ) { try { if (h.dev) h.dev->closePort(); } catch(...){} delete h.dev; }
    g_hwMidiIns.clear();
    g_hwMidiOuts.clear();
    delete g_midiOutEnum; g_midiOutEnum = nullptr;
}

int audio_app_midi_input_count()
{
    if ( !ensure_midi_in() ) return 0;
    try { return (int) g_midiInDev->getPortCount(); } catch ( ... ) { return 0; }
}

bool audio_app_midi_input_info( int idx, char* nameBuf, int nameBufLen, int* outIsCurrent )
{
    if ( !ensure_midi_in() ) return false;
    try {
        if ( idx < 0 || idx >= (int) g_midiInDev->getPortCount() ) return false;
        std::string n = g_midiInDev->getPortName( idx );
        if ( nameBuf && nameBufLen > 0 ) {
            int k = (int) n.size(); if ( k > nameBufLen-1 ) k = nameBufLen-1;
            memcpy( nameBuf, n.data(), k ); nameBuf[k] = 0;
        }
        if ( outIsCurrent ) *outIsCurrent = ( idx == g_midiInPort ) ? 1 : 0;
        return true;
    } catch ( ... ) { return false; }
}

bool audio_app_patch_set_midi_input( int idx )
{
    if ( !ensure_midi_in() ) return false;
    try {
        if ( g_midiInDev->isPortOpen() ) { g_midiInDev->cancelCallback(); g_midiInDev->closePort(); }
        g_midiInPort = -1;
        if ( idx < 0 ) return true;                  // "None" -> just close
        if ( idx >= (int) g_midiInDev->getPortCount() ) return false;
        // callback BEFORE openPort (see add_midi_in) so no live input is dropped.
        g_midiInDev->ignoreTypes( false, false, false );
        g_midiInDev->setCallback( &patch_midi_cb );
        g_midiInDev->openPort( (unsigned) idx );
        g_midiInPort = idx;
        // Live input only flows through the patch graph (drain_live_midi runs in the
        // modular render path) -- selecting a hardware input implies playing through
        // it, so enable modular or the keyboard is silent.
        g_modular.store( true, std::memory_order_release );
        return true;
    } catch ( ... ) { return false; }
}

// --- hardware MIDI OUTPUT device enumeration (for MIDI-out port nodes) -------
static bool ensure_midi_out_enum()
{
    if ( g_midiOutEnum ) return true;
    try { g_midiOutEnum = new RtMidiOut(); } catch ( ... ) { g_midiOutEnum = nullptr; }
    return g_midiOutEnum != nullptr;
}

int audio_app_midi_output_count()
{
    if ( !ensure_midi_out_enum() ) return 0;
    try { return (int) g_midiOutEnum->getPortCount(); } catch ( ... ) { return 0; }
}

bool audio_app_midi_output_info( int idx, char* nameBuf, int nameBufLen, int* outIsCurrent )
{
    if ( !ensure_midi_out_enum() ) return false;
    try {
        if ( idx < 0 || idx >= (int) g_midiOutEnum->getPortCount() ) return false;
        std::string n = g_midiOutEnum->getPortName( idx );
        if ( nameBuf && nameBufLen > 0 ) {
            int k = (int) n.size(); if ( k > nameBufLen-1 ) k = nameBufLen-1;
            memcpy( nameBuf, n.data(), k ); nameBuf[k] = 0;
        }
        if ( outIsCurrent ) *outIsCurrent = 0;
        return true;
    } catch ( ... ) { return false; }
}

// --- audio backend (host API) selection ------------------------------------
int audio_app_hostapi_count()
{
    return g_engine ? (int) g_engine->enumerateHostApis().size() : 0;
}

bool audio_app_hostapi_info( int idx, int* outIndex, char* nameBuf, int nameBufLen,
                             int* outIsCurrent )
{
    if ( !g_engine ) return false;
    auto apis = g_engine->enumerateHostApis();
    if ( idx < 0 || idx >= (int) apis.size() ) return false;
    if ( outIndex ) *outIndex = apis[idx].index;
    if ( nameBuf && nameBufLen > 0 ) {
        const std::string& n = apis[idx].name;
        int k = (int) n.size(); if ( k > nameBufLen-1 ) k = nameBufLen-1;
        memcpy( nameBuf, n.data(), k ); nameBuf[k] = 0;
    }
    if ( outIsCurrent ) *outIsCurrent = apis[idx].isCurrent ? 1 : 0;
    return true;
}

bool audio_app_set_hostapi( int paHostApiIndex )
{
    if ( !g_engine || !g_running ) return false;
    g_engine->stop(); g_engine->close();
    g_engine->selectHostApi( paHostApiIndex );   // also resets device to default
    return reopen_engine();
}

// --- buffer size (latency) --------------------------------------------------
unsigned audio_app_buffer_size() { return g_engine ? g_engine->blockSize() : 0; }

bool audio_app_set_buffer_size( unsigned frames )
{
    if ( !g_engine || !g_running ) return false;
    g_engine->stop(); g_engine->close();
    g_engine->setBufferSize( frames );
    return reopen_engine();
}

MixerGraph*  audio_app_graph()  { return g_graph;  }
PluginHost*  audio_app_host()   { return g_host;   }
AudioEngine* audio_app_engine() { return g_engine; }
patch::PatchGraph* audio_app_patch_graph() { return g_patch; }

bool audio_app_set_track_instrument( int track, const PluginDescriptor& desc )
{
    if ( !g_host || !g_graph ) return false;
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return false;

    IPluginInstance* inst = g_host->instantiate( desc );
    if ( !inst ) return false;

    inst->prepare( g_sr, g_block );
    inst->setActive( true );

    Track* trk = g_graph->track( track );
    if ( !trk ) { inst->release(); delete inst; return false; }

    trk->setInstrument( inst );   // NB: swap-while-running race, acceptable v1
    g_owned.push_back( inst );
    return true;
}

bool audio_app_add_track_fx( int track, const PluginDescriptor& desc )
{
    if ( !g_host || !g_graph ) return false;
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return false;

    IPluginInstance* inst = g_host->instantiate( desc );
    if ( !inst ) return false;

    inst->prepare( g_sr, g_block );
    inst->setActive( true );

    Track* trk = g_graph->track( track );
    if ( !trk || !trk->addFx( inst ) ) { inst->release(); delete inst; return false; }

    g_owned.push_back( inst );
    return true;
}

bool audio_app_track_has_instrument( int track )
{
    if ( !g_graph || track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return false;
    Track* trk = g_graph->track( track );
    return trk && trk->instrument() != nullptr;
}

void audio_app_route_midi( int track, unsigned char status,
                           unsigned char d1, unsigned char d2, long long tick )
{
    // serialize producers (sequencer thread + GUI preview); audio thread never
    // takes this lock.
    while ( g_midi_producer.test_and_set( std::memory_order_acquire ) ) { }
    unsigned head = g_head.load( std::memory_order_relaxed );
    unsigned tail = g_tail.load( std::memory_order_acquire );
    const unsigned used = head - tail;
    // Drop policy (nerf 27): reserve headroom so a backlog flush can never
    // discard note-offs -- once < 64 slots remain only critical messages
    // (note-offs / system realtime) are admitted; the rest are counted drops.
    bool admit;
    if ( used >= RING_SIZE )               admit = false;   // full (never blocks audio)
    else if ( RING_SIZE - used < 64 )      admit = midi_is_critical( status, d2 );
    else                                   admit = true;
    if ( admit )
    {
        RouteMsg& m = g_ring[head & RING_MASK];
        m.tick   = tick;
        m.epoch  = g_schedEpoch.load( std::memory_order_acquire );
        m.track  = (unsigned char) track;
        m.status = status;
        m.d1     = d1;
        m.d2     = d2;
        g_head.store( head + 1, std::memory_order_release );
    }
    else
        g_midiDropCount.fetch_add( 1, std::memory_order_relaxed );
    g_midi_producer.clear( std::memory_order_release );
}

void audio_app_route_param( int track, unsigned int paramId, float value )
{
    while ( g_param_producer.test_and_set( std::memory_order_acquire ) ) { }
    unsigned head = g_phead.load( std::memory_order_relaxed );
    unsigned tail = g_ptail.load( std::memory_order_acquire );
    if ( head - tail < RING_SIZE )
    {
        ParamMsg& m = g_pring[head & RING_MASK];
        m.tick  = -1;              // no tick source yet: due at block start
        m.epoch = g_schedEpoch.load( std::memory_order_acquire );
        m.track = (unsigned char) track;
        m.id    = paramId;
        m.value = value;
        g_phead.store( head + 1, std::memory_order_release );
    }
    else
        g_paramDropCount.fetch_add( 1, std::memory_order_relaxed );
    g_param_producer.clear( std::memory_order_release );
}

// Consistent snapshot of the audio thread's per-block publication (seqlock
// read side).  False until the first block has been rendered.
bool audio_app_block_pub( long long* blockSample, long long* blockQpc,
                          double* qpcPerSample )
{
    for ( int tries = 0; tries < 64; ++tries )
    {
        const unsigned g1 = g_blockPubGen.load( std::memory_order_acquire );
        if ( g1 == 0u ) return false;                 // never published
        if ( g1 & 1u ) continue;                      // write in progress
        const long long s  = g_blockPubSample.load( std::memory_order_relaxed );
        const long long q  = g_blockPubQpc.load( std::memory_order_relaxed );
        const double    ps = g_qpcPerSample.load( std::memory_order_relaxed );
        std::atomic_thread_fence( std::memory_order_acquire );
        if ( g_blockPubGen.load( std::memory_order_relaxed ) != g1 ) continue;
        if ( ps <= 0.0 ) return false;
        if ( blockSample )  *blockSample  = s;
        if ( blockQpc )     *blockQpc     = q;
        if ( qpcPerSample ) *qpcPerSample = ps;
        return true;
    }
    return false;
}

int audio_app_track_param_count( int track )
{
    if ( !g_graph || track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return 0;
    Track* trk = g_graph->track( track );
    if ( !trk || !trk->instrument() ) return 0;
    return trk->instrument()->paramCount();
}

bool audio_app_track_param_info( int track, int index, unsigned int* outId,
                                 char* nameBuf, int nameBufLen, float* outDefault )
{
    if ( !g_graph || track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return false;
    Track* trk = g_graph->track( track );
    if ( !trk || !trk->instrument() ) return false;
    if ( index < 0 || index >= trk->instrument()->paramCount() ) return false;

    ParamInfo pi = trk->instrument()->paramInfo( index );
    if ( outId )      *outId = pi.id;
    if ( outDefault ) *outDefault = pi.defaultValue;
    if ( nameBuf && nameBufLen > 0 )
    {
        int n = (int) pi.name.size();
        if ( n > nameBufLen - 1 ) n = nameBufLen - 1;
        memcpy( nameBuf, pi.name.c_str(), n );
        nameBuf[n] = '\0';
    }
    return true;
}

float audio_app_selftest( const char* vst_path )
{
    if ( !g_running )
    {
        fprintf( stderr, "[selftest] audio not running\n" );
        return -1.f;
    }
    PluginDescriptor d = make_descriptor( vst_path );
    fprintf( stderr, "[selftest] loading %s (%s) on track 0...\n",
             vst_path, d.format == PluginFormat::VST3 ? "VST3" : "VST2" );

    if ( !audio_app_set_track_instrument( 0, d ) )
    {
        fprintf( stderr, "[selftest] FAILED to instantiate plugin\n" );
        return -1.f;
    }
    fprintf( stderr, "[selftest] instrument loaded; firing C4...\n" );

    audio_app_route_midi( 0, 0x90, 60, 110 );          // note on C4
    std::this_thread::sleep_for( std::chrono::milliseconds( 350 ) );
    float peak = g_engine->masterPeak();
    audio_app_route_midi( 0, 0x80, 60, 0 );            // note off
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    fprintf( stderr, "[selftest] master peak after note = %.4f  (%s)\n",
             peak, peak > 0.0001f ? "AUDIBLE - chain works!"
                                  : "silent (plugin default patch may be silent)" );
    fflush( stderr );
    return peak;
}

// ===========================================================================
//  modular patch graph
// ===========================================================================
int audio_app_patch_add_plugin( const PluginDescriptor& desc )
{
    if ( !g_patch || !g_host ) return -1;
    IPluginInstance* inst = g_host->instantiate( desc );
    if ( !inst ) return -1;
    inst->prepare( g_sr, g_block );
    // NB: activate AFTER addNode.  addNode re-prepares the node when the graph
    // is already prepared (PluginNode::prepare -> inst->prepare), and a VST3
    // re-prepare deactivates an active instance -- so activating first would
    // leave the plugin inactive (its process() early-returns to silence).
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::PluginNode>( inst ) );
    inst->setActive( true );
    g_patch->compileAndPublish();
    g_owned.push_back( inst );
    return (int) id;
}

// --- Buzz/Unwieldy SAMPLER instrument node ----------------------------------
int audio_app_patch_add_sampler()
{
    if ( !g_patch ) return -1;
    IPluginInstance* inst = PatchKnob::engine::create_sampler_instrument();
    if ( !inst ) return -1;
    inst->prepare( g_sr, g_block );
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::PluginNode>( inst ) );
    inst->setActive( true );
    g_patch->compileAndPublish();
    g_owned.push_back( inst );
    return (int) id;
}

// Load a sample from disk into a sampler node's wavetable slot (keyrange level).
bool audio_app_sampler_load( int node, int slot, int level,
                             const float* interleaved, int numFrames, int stereo,
                             int rootMidiNote, int sampleRate,
                             int loopStart, int loopEnd, int loop,
                             int loKey, int hiKey, const char* name )
{
    return audio_app_sampler_load_ex( node, slot, level, interleaved, numFrames, stereo,
                                      rootMidiNote, sampleRate, loopStart, loopEnd, loop,
                                      loKey, hiKey, 0, 127, 0, 1, 1, 0, name );
}

bool audio_app_sampler_load_ex( int node, int slot, int level,
                                const float* interleaved, int numFrames, int stereo,
                                int rootMidiNote, int sampleRate,
                                int loopStart, int loopEnd, int loop,
                                int loKey, int hiKey, int loVel, int hiVel,
                                int noteOffLayer, int keyToPitch, int velToVol,
                                int overlapMode, const char* name )
{
    if ( !g_patch ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    if ( !pn ) return false;
    IPluginInstance* inst = pn->instance();
    if ( !PatchKnob::engine::sampler_is_sampler( inst ) ) return false;
    PatchKnob::engine::sampler_load_sample_ex( inst, slot, level, interleaved, numFrames, stereo != 0,
                                           rootMidiNote, sampleRate, loopStart, loopEnd, loop != 0,
                                           loKey, hiKey, loVel, hiVel, noteOffLayer != 0,
                                           keyToPitch != 0, velToVol != 0, overlapMode, name );
    return true;
}

void audio_app_sampler_clear( int node, int slot )
{
    if ( !g_patch ) return;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    if ( !pn ) return;
    PatchKnob::engine::sampler_clear_slot( pn->instance(), slot );
}

void audio_app_sampler_set_env( int node, int env, const unsigned short* xs,
                                const unsigned short* ys, const int* flags, int count )
{
    if ( !g_patch ) return;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    if ( !pn ) return;
    PatchKnob::engine::sampler_set_envelope( pn->instance(), env, xs, ys, flags, count );
}
int audio_app_sampler_zone_count( int node )
{
    if ( !g_patch ) return 0;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    return pn ? PatchKnob::engine::sampler_zone_count( pn->instance() ) : 0;
}
bool audio_app_sampler_get_zone( int node, int index, PatchKnob::engine::SamplerZoneInfo& out )
{
    if ( !g_patch ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    return pn && PatchKnob::engine::sampler_get_zone( pn->instance(), index, out );
}
bool audio_app_sampler_has_envelopes( int node )
{
    if ( !g_patch ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    return pn && PatchKnob::engine::sampler_has_envelopes( pn->instance() );
}
int audio_app_patch_add_mixer( int channels )
{
    if ( !g_patch ) return -1;
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::MixerNode>( channels ) );
    g_patch->compileAndPublish();
    return (int) id;
}

namespace {
patch::MixerNode* mixer_node( int node )
{
    if ( !g_patch ) return nullptr;
    return dynamic_cast<patch::MixerNode*>( g_patch->node( (patch::NodeId) node ) );
}
}

int  audio_app_mixer_channels( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->channels() : 0; }
void audio_app_mixer_set_channels( int node, int n )
{
    if ( patch::MixerNode* m = mixer_node(node) ) {
        m->setChannels(n);
        if (g_patch) {
            g_patch->pruneDanglingConnections();   // drop wires to removed channel ports
            g_patch->compileAndPublish();
        }
    }
}
float audio_app_mixer_gain( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->gain(ch) : 0.f; }
void  audio_app_mixer_set_gain( int node, int ch, float g ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setGain(ch, g); }
bool  audio_app_mixer_mute( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->mute(ch) : false; }
void  audio_app_mixer_set_mute( int node, int ch, bool mu ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setMute(ch, mu); }
float audio_app_mixer_vu( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->vu(ch) : 0.f; }
float audio_app_mixer_pan( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->pan(ch) : 0.f; }
void  audio_app_mixer_set_pan( int node, int ch, float p ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setPan(ch, p); }
float audio_app_mixer_master_gain( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterGain() : 1.f; }
void  audio_app_mixer_set_master_gain( int node, float g ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setMasterGain(g); }
float audio_app_mixer_master_vu( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterVu() : 0.f; }

// --- Pure Data node (embedded libpd patch) ----------------------------------
int audio_app_patch_add_pd()
{
    if ( !g_patch ) return -1;
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::PdNode>() );
    g_patch->compileAndPublish();
    return (int) id;
}
namespace { patch::PdNode* pd_node( int node ) {
    if ( !g_patch ) return nullptr;
    return dynamic_cast<patch::PdNode*>( g_patch->node( (patch::NodeId) node ) );
} }
bool audio_app_pd_load( int node, const char* path )
{
    patch::PdNode* p = pd_node( node );
    if ( !p || !path ) return false;
    bool ok = p->loadPatch( path );
    // adc~/dac~ counts (hence numPorts) may have changed -> republish the graph so
    // the new audio in/out ports take effect and the UI can re-mirror them.
    if ( g_patch ) g_patch->compileAndPublish();
    return ok;
}
// adc~/dac~ channel signature (inCh*1000 + outCh) so the shell can detect a port
// change after a patch (re)load and re-mirror the node, or -1 if not a Pd node.
int audio_app_pd_io_sig( int node )
{
    patch::PdNode* p = pd_node( node );
    return p ? p->audioInChannels() * 1000 + p->audioOutChannels() : -1;
}
bool audio_app_pd_path( int node, char* buf, int buflen )
{
    patch::PdNode* p = pd_node( node );
    if ( !p || !buf || buflen <= 0 ) return false;
    const std::string& s = p->patchPath();
    int k = (int) s.size(); if ( k > buflen-1 ) k = buflen-1;
    memcpy( buf, s.data(), k ); buf[k] = 0;
    return !s.empty();
}

// --- VCV-Rack-style modular node --------------------------------------------
int audio_app_patch_add_rack()
{
    if ( !g_patch ) return -1;
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::RackNode>() );
    g_patch->compileAndPublish();
    return (int) id;
}

// --- embedded Csound (.csd) node --------------------------------------------
int audio_app_patch_add_csound()
{
    if ( !g_patch ) return -1;
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::CsoundNode>() );
    g_patch->compileAndPublish();
    return (int) id;
}
namespace { patch::CsoundNode* csound_node( int node ) {
    if ( !g_patch ) return nullptr;
    return dynamic_cast<patch::CsoundNode*>( g_patch->node( (patch::NodeId) node ) );
} }
bool audio_app_patch_is_csound( int node ) { return csound_node( node ) != nullptr; }
const char* audio_app_patch_csound_text( int node )
{
    patch::CsoundNode* n = csound_node( node );
    return n ? n->csdText().c_str() : "";
}
void audio_app_patch_csound_set_text( int node, const char* text )
{
    if ( patch::CsoundNode* n = csound_node( node ) ) n->setCsdText( text ? text : "" );
}
// Recompile the node's stored CSD.  Its audio in/out channel counts (nchnls /
// nchnls_i) may change, so republish the graph -> the UI remirrors the ports.
bool audio_app_patch_csound_recompile( int node )
{
    patch::CsoundNode* n = csound_node( node );
    if ( !n ) return false;
    bool ok = n->recompile();
    if ( g_patch ) g_patch->compileAndPublish();
    return ok;
}
const char* audio_app_patch_csound_error( int node )
{
    patch::CsoundNode* n = csound_node( node );
    return n ? n->lastError().c_str() : "";
}
::rackx::RackEngine* audio_app_rack_engine( int node )
{
    if ( !g_patch ) return nullptr;
    patch::RackNode* r = dynamic_cast<patch::RackNode*>( g_patch->node( (patch::NodeId) node ) );
    return r ? r->engine() : nullptr;
}
void audio_app_rack_set_poly( int node, int voices )
{
    if ( ::rackx::RackEngine* e = audio_app_rack_engine( node ) ) e->setPolyphony( voices );
}
int audio_app_rack_poly( int node )
{
    ::rackx::RackEngine* e = audio_app_rack_engine( node );
    return e ? e->polyphony() : 1;
}

// DSP load 0..1 (fraction of the audio block budget used); >1 means overrun.
float audio_app_cpu_load()
{
    return (float) g_cpuLoad.load( std::memory_order_relaxed );
}

// --- device-loss detection + recovery (message thread; UI polls on idle) ----
bool audio_app_device_lost()
{
    return g_engine && g_engine->deviceLost();
}

bool audio_app_try_recover()
{
    return g_engine && g_engine->tryRecover();
}

// --- master mixer: dynamic per-track PORTS (no separate nodes) ---------------
int audio_app_master_mixer_node() { return (int) g_masterMixerId; }

// Adding a track just grows the master mixer's ports: an instrument track adds a
// MIDI inlet+outlet, an audio track an AUDIO inlet+outlet.  Returns track index.
int audio_app_master_add_track( int isMidi )
{
    if ( !g_patch ) return -1;
    patch::MasterMixerNode* mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) );
    if ( !mm ) return -1;
    int idx = mm->addTrack( isMidi != 0 );
    // Grow the sequencer MidiIn node's "Instrument out" plugs in lockstep so each
    // track has its own MIDI-out plug on the patcher (mirrors the mixer).
    if ( g_midiInNode ) g_midiInNode->setTrackPorts( mm->trackCount() );
    g_patch->compileAndPublish();
    return idx;
}
void audio_app_master_remove_track( int idx )
{
    if ( !g_patch ) return;
    if ( patch::MasterMixerNode* mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) ) )
        { mm->removeTrack( idx );
          if ( g_midiInNode ) g_midiInNode->setTrackPorts( mm->trackCount() );
          g_patch->compileAndPublish(); }
    // removeTrack() compacts the mixer channels (indices above idx drop by one).
    // render_modular_clip_players routes g_projectAudioPlayers[t] -> mixer channel
    // t, so the player array MUST shift in lockstep -- otherwise a removed track's
    // player keeps sounding and survivors route through the wrong strip.
    if ( idx >= 0 && idx < PatchKnob::app::AUDIO_APP_MAX_TRACKS )
    {
        if ( AudioClipPlayer* dead = g_projectAudioPlayers[idx] )
        {
            dead->clearClips();                          // stop any audio it still holds
            if ( g_graph )
                for ( int i = 0; i < g_graph->trackCount(); ++i )
                    if ( Track* tk = g_graph->track( i ) )
                        if ( tk->instrument() == dead ) tk->setInstrument( nullptr );
        }
        freeze_rcu_grace();                              // drain readers before the shift
        for ( int t = idx; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS - 1; ++t )
            g_projectAudioPlayers[t] = g_projectAudioPlayers[t + 1];
        g_projectAudioPlayers[PatchKnob::app::AUDIO_APP_MAX_TRACKS - 1] = nullptr;
    }
}

// Wire an instrument node into its per-track master-mixer AUDIO inlet (so the
// mixer's gain/pan/mute/VU are actually in the signal path) and feed it MIDI
// from the shared MidiIn node.  The master mixer already sums to Out, so the
// instrument stays audible AND the mixer becomes live.  Returns the inlet port,
// or -1.  Enables modular render (the mixer routing lives in the patch graph).
int audio_app_master_connect_instrument( int track, int instrNode )
{
    if ( !g_patch || track < 0 ) return -1;
    patch::MasterMixerNode* mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) );
    if ( !mm || track >= mm->trackCount() ) return -1;
    patch::Node* inst = g_patch->node( (patch::NodeId)instrNode );
    if ( !inst ) return -1;

    int audioOut = -1, midiIn = -1;
    for ( int i = 0; i < inst->numPorts(); ++i )
    {
        const patch::PortDesc pd = inst->port( i );
        if ( pd.dir == patch::PortDir::Out && pd.kind == patch::PortKind::Audio && audioOut < 0 ) audioOut = (int)pd.id;
        if ( pd.dir == patch::PortDir::In  && pd.kind == patch::PortKind::Midi  && midiIn   < 0 ) midiIn   = (int)pd.id;
    }
    const int inlet = 2 + 2 * track;   // MasterMixerNode audio inlet for track t

    // Dedup: skip a connection the graph already has (repeated wiring must not
    // double the signal).
    const std::vector<patch::Connection> existing = g_patch->connections();
    auto has = [&]( patch::NodeId fn, int fp, patch::NodeId tn, int tp ) {
        for ( const patch::Connection& c : existing )
            if ( c.from.node == fn && (int)c.from.port == fp &&
                 c.to.node == tn && (int)c.to.port == tp ) return true;
        return false;
    };
    // Feed the instrument from ITS track's "Instrument out" plug (plug index ==
    // track), so each track drives its own instrument with no channel filtering.
    const int midiPlug = ( g_midiInNode && track < g_midiInNode->trackPorts() ) ? track : 0;
    if ( midiIn >= 0 && !has( g_patchMidiInId, midiPlug, (patch::NodeId)instrNode, midiIn ) )
        g_patch->connect( patch::Connection{ patch::PortRef{ g_patchMidiInId, (patch::PortId)midiPlug },
                                             patch::PortRef{ (patch::NodeId)instrNode, (patch::PortId)midiIn } } );
    if ( audioOut >= 0 && !has( (patch::NodeId)instrNode, audioOut, g_masterMixerId, inlet ) )
        g_patch->connect( patch::Connection{ patch::PortRef{ (patch::NodeId)instrNode, (patch::PortId)audioOut },
                                             patch::PortRef{ g_masterMixerId, (patch::PortId)inlet } } );
    g_patch->compileAndPublish();
    g_modular.store( true, std::memory_order_release );
    return audioOut >= 0 ? inlet : -1;
}
int audio_app_master_track_count()
{
    if ( !g_patch ) return 0;
    patch::MasterMixerNode* mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) );
    return mm ? mm->trackCount() : 0;
}
bool audio_app_master_track_is_midi( int idx )
{
    if ( !g_patch ) return false;
    patch::MasterMixerNode* mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) );
    return mm ? mm->trackIsMidi( idx ) : false;
}

void audio_app_project_clear_audio_clips()
{
    bool changed = false;
    for ( int t = 0; t < AUDIO_APP_MAX_TRACKS; ++t )
        if ( g_projectAudioPlayers[t] )
        {
            g_projectAudioPlayers[t]->clearWarp( nullptr );
            g_projectAudioPlayers[t]->clearClips();
            changed = true;
        }
    if ( changed ) freeze_rcu_grace();
    if ( g_graph )
        for ( int t = 0; t < AUDIO_APP_MAX_TRACKS; ++t )
            if ( g_projectAudioPlayers[t] )
                if ( Track* trk = g_graph->track( t ) )
                    if ( trk->instrument() == g_projectAudioPlayers[t] )
                        trk->setInstrument( nullptr );
    for ( int t = 0; t < AUDIO_APP_MAX_TRACKS; ++t )
        g_projectAudioPlayers[t] = nullptr;
    g_projectAudioClips.clear();
}

bool audio_app_project_add_audio_clip( int track, const AudioClip& clip,
                                       long long startSample, float gain )
{
    if ( !g_graph || track < 0 || track >= AUDIO_APP_MAX_TRACKS || clip.empty() )
        return false;
    Track* trk = g_graph->track( track );
    if ( !trk ) return false;

    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player )
    {
        player = new AudioClipPlayer();
        if ( !player->prepare( g_sr, g_block ) ) { delete player; return false; }
        player->setActive( true );
        g_owned.push_back( player );
        g_projectAudioPlayers[track] = player;
        trk->setInstrument( player );
    }

    std::unique_ptr<AudioClip> stored( new AudioClip( clip ) );
    const AudioClip* raw = stored.get();
    if ( !player->addClip( raw, startSample, gain ) ) return false;
    ProjectAudioClipEntry entry;
    entry.track = track;
    entry.clip = std::move( stored );
    g_projectAudioClips.push_back( std::move( entry ) );
    return true;
}

bool audio_app_project_add_audio_region( int track, const AudioClip& clip,
                                         long long startSample, long long sourceOffset,
                                         long long length, float gain, int muted, int loop,
                                         long long fadeIn, long long fadeOut,
                                         float fadeInK, float fadeOutK )
{
    if ( !audio_app_project_add_audio_clip( track, clip, startSample, gain ) ) return false;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player ) return false;
    const int idx = player->clipCount() - 1;           // the clip just appended
    if ( idx < 0 ) return false;
    player->setClipRegion( idx, startSample, sourceOffset, length );
    player->setClipMuted( idx, muted != 0 );
    player->setClipLoop( idx, loop != 0 );
    if ( fadeIn > 0 || fadeOut > 0 )
        player->setClipFades( idx, fadeIn, fadeOut, fadeInK, fadeOutK );
    return true;
}

const AudioClip* audio_app_project_clip_on_track( int track )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return nullptr;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p || p->clipCount() < 1 ) return nullptr;
    return p->clipAt( 0 ).clip;
}

void audio_app_project_clear_track( int track )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return;
    if ( AudioClipPlayer* p = g_projectAudioPlayers[track] )
    {
        p->clearClips();       // republishes an empty schedule (audio stops)
        freeze_rcu_grace();    // let a live block drain before the caller moves on
        g_projectAudioClips.erase(
            std::remove_if( g_projectAudioClips.begin(), g_projectAudioClips.end(),
                            [track]( const ProjectAudioClipEntry& e ){ return e.track == track; } ),
            g_projectAudioClips.end() );
    }
}

void audio_app_project_reset_patch()
{
    if ( !g_patch ) return;

    std::vector<patch::NodeId> ids = g_patch->nodeIds();
    for ( size_t i = 0; i < ids.size(); ++i )
        if ( ids[i] != g_patchOutId && ids[i] != g_patchMidiInId &&
             ids[i] != g_masterMixerId )
            audio_app_patch_remove( (int) ids[i] );

    patch::MasterMixerNode* master =
        dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) );
    if ( master )
    {
        while ( master->trackCount() > 0 ) master->removeTrack( master->trackCount() - 1 );
        master->setMasterGain( 1.0f );
    }

    std::vector<patch::Connection> conns = g_patch->connections();
    for ( size_t i = 0; i < conns.size(); ++i )
        g_patch->disconnect( conns[i] );
    g_patch->compileAndPublish();
    g_modular.store( false );
}

// --- live-record node (audio + MIDI ins -> piano-roll clips) ----------------
int audio_app_patch_add_record()
{
    if ( !g_patch ) return -1;
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::RecordNode>() );
    g_patch->compileAndPublish();
    return (int) id;
}

namespace {
patch::RecordNode* record_node( int node )
{
    if ( !g_patch ) return nullptr;
    return dynamic_cast<patch::RecordNode*>( g_patch->node( (patch::NodeId) node ) );
}
}

void audio_app_record_set( int node, bool on ) { if ( patch::RecordNode* r = record_node(node) ) r->setRecording(on); }
bool audio_app_record_active( int node ) { patch::RecordNode* r = record_node(node); return r ? r->recording() : false; }

int audio_app_record_drain( int node, long* ticks, unsigned char* status,
                            unsigned char* d1, unsigned char* d2, int cap )
{
    patch::RecordNode* r = record_node(node);
    if ( !r || cap <= 0 ) return 0;
    static thread_local std::vector<patch::RecordNode::Ev> buf;
    if ( (int) buf.size() < cap ) buf.resize(cap);
    int n = r->drain( buf.data(), cap );
    for ( int i = 0; i < n; ++i ) {
        if ( ticks )  ticks[i]  = buf[i].tick;
        if ( status ) status[i] = buf[i].status;
        if ( d1 )     d1[i]     = buf[i].d1;
        if ( d2 )     d2[i]     = buf[i].d2;
    }
    return n;
}

// --- transport live MIDI record (into a timeline clip; message thread) -------
void audio_app_record_arm( bool on )
{
    if ( on )
    {
        g_recStartTick.store( audio_app_sample_to_tick( g_transport.sample() ),
                              std::memory_order_relaxed );
        g_recHead.store( g_recTail.load( std::memory_order_relaxed ),
                         std::memory_order_relaxed );   // clear stale captures
    }
    g_recArm.store( on, std::memory_order_release );
}
bool      audio_app_record_armed()      { return g_recArm.load( std::memory_order_acquire ); }
long long audio_app_record_start_tick() { return g_recStartTick.load( std::memory_order_relaxed ); }

// Drain captured live events; `ticks` come back RELATIVE to the record start
// (clamped to >= 0).  Returns the count.
int audio_app_record_drain_live( long* ticks, unsigned char* status,
                                 unsigned char* d1, unsigned char* d2, int cap )
{
    if ( cap <= 0 ) return 0;
    const long long start = g_recStartTick.load( std::memory_order_relaxed );
    unsigned t = g_recTail.load( std::memory_order_relaxed );
    const unsigned h = g_recHead.load( std::memory_order_acquire );
    int n = 0;
    while ( t != h && n < cap )
    {
        const RecEv& e = g_recRing[t % REC_RING]; ++t;
        long rel = (long)( e.tick - start ); if ( rel < 0 ) rel = 0;
        if ( ticks )  ticks[n]  = rel;
        if ( status ) status[n] = e.status;
        if ( d1 )     d1[n]     = e.d1;
        if ( d2 )     d2[n]     = e.d2;
        ++n;
    }
    g_recTail.store( t, std::memory_order_release );
    return n;
}

// Add an EMPTY plugin node (no plugin yet) with the uniform in/out/midi layout.
// Used for "Add Effect": the user then right-clicks -> Choose Plugin to fill it.
int audio_app_patch_add_empty_plugin()
{
    if ( !g_patch ) return -1;
    patch::NodeId id = g_patch->addNode( std::make_unique<patch::PluginNode>( nullptr ) );
    g_patch->compileAndPublish();
    return (int) id;
}

// Replace the VST hosted by an existing plugin node (right-click "Choose
// Instrument" in the patcher).  Keeps the node's ports + connections.
bool audio_app_patch_set_node_plugin( int node, const PluginDescriptor& desc )
{
    if ( !g_patch || !g_host ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId) node );
    if ( !n ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n );
    if ( !pn ) return false;

    IPluginInstance* inst = g_host->instantiate( desc );
    if ( !inst ) return false;
    inst->prepare( g_sr, g_block );
    inst->setActive( true );

    // swapInstance() is RT-safe: it publishes the new pointer atomically and
    // drains any in-flight process() before returning the old instance, so no
    // render pause / sleep handoff is needed.
    IPluginInstance* old = pn->swapInstance( inst );

    if ( old ) old->setActive( false );   // deactivated; freed with the rest at shutdown
    g_owned.push_back( inst );
    return true;
}

int audio_app_patch_add_builtin( const char* kind )
{
    if ( !g_patch ) return -1;
    std::string k( kind ? kind : "" );
    std::unique_ptr<patch::Node> n;
    if      ( k == "sine"   ) n = std::make_unique<patch::SineSourceNode>( 440.f, 0.5f );
    else if ( k == "gain"   ) n = std::make_unique<patch::GainNode>( 1.0f );
    else if ( k == "sum"    ) n = std::make_unique<patch::SumNode>();
    else if ( k == "in"     ) n = std::make_unique<patch::AudioDeviceInNode>( 2 );
    else if ( k == "out"    ) n = std::make_unique<patch::AudioDeviceOutNode>( 2 );
    else if ( k == "midiin" ) n = std::make_unique<patch::MidiInNode>();
    else return -1;
    patch::NodeId id = g_patch->addNode( std::move( n ) );
    g_patch->compileAndPublish();
    return (int) id;
}

// --- patchable MIDI ports (virtual or hardware-bound) -----------------------
// hwDevice < 0 => VIRTUAL routing point.  hwDevice >= 0 => bind to that
// hardware MIDI in / out device.  Either way you get a normal patch node.
int audio_app_patch_add_midi_in( int hwDevice )
{
    if ( !g_patch ) return -1;
    auto  n   = std::make_unique<patch::MidiInNode>();
    auto* raw = n.get();
    patch::NodeId id = g_patch->addNode( std::move( n ) );
    g_patch->compileAndPublish();
    if ( hwDevice >= 0 ) {
        RtMidiIn* dev = nullptr;
        try {
            dev = new RtMidiIn();
            if ( hwDevice < (int) dev->getPortCount() ) {
                // RtMidi requires the callback be installed BEFORE opening the port
                // (messages between open and setCallback are otherwise queued, not
                // delivered) -- set it first so live input is never dropped.
                dev->ignoreTypes( false, false, false );
                dev->setCallback( &hw_midi_in_cb, raw );
                dev->openPort( (unsigned) hwDevice );
                { std::lock_guard<std::mutex> lk( g_hwMidiMx );
                  g_hwMidiIns.push_back( HwMidiIn{ id, raw, dev } ); }
                dev = nullptr;                                 // owned by the registry now
                // Hardware MIDI I/O only moves through the patch graph, which the
                // audio callback renders ONLY in modular mode -- so binding a
                // hardware port must enable it or the node stays dead.
                g_modular.store( true, std::memory_order_release );
            }
        } catch ( ... ) {}
        delete dev;                                            // only if we failed to bind
    }
    return (int) id;
}

int audio_app_patch_add_midi_out( int hwDevice )
{
    if ( !g_patch ) return -1;
    auto  n   = std::make_unique<patch::MidiOutNode>();
    auto* raw = n.get();
    patch::NodeId id = g_patch->addNode( std::move( n ) );
    g_patch->compileAndPublish();
    if ( hwDevice >= 0 ) {
        RtMidiOut* dev = nullptr;
        try {
            dev = new RtMidiOut();
            if ( hwDevice < (int) dev->getPortCount() ) {
                dev->openPort( (unsigned) hwDevice );
                { std::lock_guard<std::mutex> lk( g_hwMidiMx );
                  g_hwMidiOuts.push_back( HwMidiOut{ id, raw, dev } ); }
                dev = nullptr;                                 // owned by the registry now
                midi_out_thread_start();
                // See add_midi_in: the MidiOut node's ring is only filled by the
                // graph render, which runs in modular mode -- enable it on bind.
                g_modular.store( true, std::memory_order_release );
            }
        } catch ( ... ) {}
        delete dev;
    }
    return (int) id;
}

// True if `node` is a hardware-bound MIDI-IN node (in the hw registry).
bool audio_app_patch_is_hw_midi_in( int node )
{
    std::lock_guard<std::mutex> lk( g_hwMidiMx );
    for ( const auto& h : g_hwMidiIns ) if ( (int) h.node == node ) return true;
    return false;
}
// Force a MidiIn node's channel-voice output onto `ch` (0..15), or -1 passthrough.
void audio_app_patch_midiin_set_out_channel( int node, int ch )
{
    if ( !g_patch ) return;
    if ( auto* mi = dynamic_cast<patch::MidiInNode*>( g_patch->node( (patch::NodeId) node ) ) )
        mi->setOutChannel( ch );
}

bool audio_app_patch_connect( int fn, int fp, int tn, int tp )
{
    if ( !g_patch ) return false;
    patch::Connection c{ patch::PortRef{ (patch::NodeId)fn, (patch::PortId)fp },
                         patch::PortRef{ (patch::NodeId)tn, (patch::PortId)tp } };
    bool ok = g_patch->connect( c );
    if ( ok ) g_patch->compileAndPublish();
    return ok;
}

bool audio_app_patch_disconnect( int fn, int fp, int tn, int tp )
{
    if ( !g_patch ) return false;
    patch::Connection c{ patch::PortRef{ (patch::NodeId)fn, (patch::PortId)fp },
                         patch::PortRef{ (patch::NodeId)tn, (patch::PortId)tp } };
    bool ok = g_patch->disconnect( c );
    if ( ok ) g_patch->compileAndPublish();
    return ok;
}

void audio_app_patch_remove( int node )
{
    if ( !g_patch ) return;
    patch::NodeId id = (patch::NodeId) node;
    // A hardware-bound MIDI port must have its device torn down BEFORE the node
    // is retired -- the RtMidi input callback still references the node's ring.
    RtMidiIn*  inDev  = nullptr;
    RtMidiOut* outDev = nullptr;
    {
        std::lock_guard<std::mutex> lk( g_hwMidiMx );
        for ( size_t i = 0; i < g_hwMidiIns.size(); ++i )
            if ( g_hwMidiIns[i].node == id ) {
                inDev = g_hwMidiIns[i].dev;
                g_hwMidiIns.erase( g_hwMidiIns.begin() + i );
                break;
            }
        for ( size_t i = 0; i < g_hwMidiOuts.size(); ++i )
            if ( g_hwMidiOuts[i].node == id ) {
                outDev = g_hwMidiOuts[i].dev;
                g_hwMidiOuts.erase( g_hwMidiOuts.begin() + i );
                break;
            }
    }
    if ( inDev )
    {
        try { inDev->cancelCallback(); inDev->closePort(); } catch ( ... ) {}
        delete inDev;
    }
    if ( outDev )
    {
        // The drain thread sends OUTSIDE g_hwMidiMx from a snapshot taken at
        // the top of each pass: wait for it to complete a pass that can no
        // longer see this port before closing the device (message thread; may
        // sleep).  Timeout guards a stalled/stopped drain thread.
        if ( g_midiOutRun.load( std::memory_order_acquire ) )
        {
            const unsigned gen0 = g_drainGen.load( std::memory_order_acquire );
            for ( int i = 0; i < 200 &&
                  g_drainGen.load( std::memory_order_acquire ) - gen0 < 2; ++i )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        try { outDev->closePort(); } catch ( ... ) {}
        delete outDev;
    }
    g_patch->removeNode( id );
    g_patch->compileAndPublish();
}

// Expose a plugin node's hosted instance (for opening its native editor GUI).
// Returns nullptr for non-plugin nodes / missing nodes.
IPluginInstance* audio_app_patch_node_instance( int node )
{
    if ( !g_patch ) return nullptr;
    patch::Node* n = g_patch->node( (patch::NodeId) node );
    if ( !n ) return nullptr;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n );
    return pn ? pn->instance() : nullptr;
}

bool audio_app_patch_route_param( int node, unsigned int paramId, float value )
{
    if ( !g_patch ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId) node );
    if ( !n ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n );
    if ( !pn ) return false;
    pn->setParamNormalized( paramId, value );
    return true;
}

// Per-column FX: route a param to ONE tracker note-column's voices on the target
// node.  For the built-in sampler this drives only that column's voices; for any
// other node (or column < 0) it falls back to the normal global param.
bool audio_app_patch_route_param_column( int node, unsigned int paramId, float value, int column )
{
    if ( !g_patch ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId) node );
    if ( !n ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n );
    if ( !pn ) return false;
    PatchKnob::engine::IPluginInstance* inst = pn->instance();
    if ( column >= 0 && inst && PatchKnob::engine::sampler_is_sampler( inst ) ) {
        PatchKnob::engine::sampler_set_column_param( inst, column, (int) paramId, value );
        return true;
    }
    pn->setParamNormalized( paramId, value );
    return true;
}

// Tag a built-in sampler note's tracker column (so the voice it triggers next is
// bound to that column).  No-op for non-sampler nodes.
void audio_app_patch_sampler_set_note_column( int node, int note, int column )
{
    if ( !g_patch ) return;
    patch::Node* n = g_patch->node( (patch::NodeId) node );
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n );
    if ( pn && pn->instance() )
        PatchKnob::engine::sampler_set_note_column( pn->instance(), note, column );
}

// Set a plugin node's MIDI channel filter (-1 omni, 0..15) so a clip on that
// channel plays ONLY this instrument.
void audio_app_patch_set_node_channel( int node, int ch )
{
    if ( !g_patch ) return;
    // Polymorphic: PluginNode / PdNode / RackNode all honor the channel filter,
    // so a clip can target any of them as its instrument.
    if ( patch::Node* n = g_patch->node( (patch::NodeId)node ) )
        n->setMidiChannel( ch );
}
int audio_app_patch_node_channel( int node )
{
    if ( !g_patch ) return -1;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    return n ? n->midiChannel() : -1;
}
// True if the node hosts an INSTRUMENT plugin (for the clip instrument list).
bool audio_app_patch_node_is_instrument( int node )
{
    if ( !g_patch ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    return pn && pn->instance() && pn->instance()->descriptor().isInstrument;
}

int  audio_app_patch_out_node()     { return (int) g_patchOutId; }
int  audio_app_patch_midi_in_node() { return (int) g_patchMidiInId; }
void audio_app_set_modular( bool on ) { g_modular.store( on ); }
bool audio_app_modular()            { return g_modular.load(); }

// Multi-core plugin processing in the modular graph (level-parallel; off by default).
void audio_app_set_multithreaded( bool on ) { if ( g_patch ) g_patch->setMultiThreaded( on ); }
bool audio_app_multithreaded()      { return g_patch ? g_patch->multiThreaded() : false; }

// Report transport run-state into the modular render context so host-synced
// plugin elements (arps / tempo-locked LFOs) advance during playback.  Also
// rolls / holds the kitchensink musical clock.
void audio_app_patch_set_playing( bool on )
{
    g_patchPlaying.store( on );
    if ( on ) g_transport.start(); else g_transport.stop();
}

// --- kitchensink transport / musical clock (UI transport bar drives these) ---
// Tempo edits are RCU-published (nerfs 19/20/22): every reader load-acquires
// g_tmapActive per call, so the audio thread's conversions never race an edit,
// and a live edit re-anchors at the CURRENT beat so the musical position never
// teleports.  The old map is retired only after two more audio blocks have
// completed (g_blockGen grace, same pattern as the capture buffer).
namespace {
// Free retired maps whose grace period has elapsed.  Caller holds g_tmapEditMx.
void tmap_gc_locked()
{
    const unsigned gen = g_blockGen.load( std::memory_order_acquire );
    for ( size_t i = 0; i < g_tmapRetired.size(); )
    {
        if ( gen - g_tmapRetired[i].gen >= 2 )
        {
            if ( g_tmapRetired[i].map != &g_tmap ) delete g_tmapRetired[i].map;
            g_tmapRetired.erase( g_tmapRetired.begin() + (long)i );
        }
        else ++i;
    }
}
} // namespace

void audio_app_set_tempo( double bpm )
{
    if ( bpm < 20.0 )  bpm = 20.0;
    if ( bpm > 999.0 ) bpm = 999.0;
    std::lock_guard<std::mutex> lk( g_tmapEditMx );   // serialize writers (UI + MIDI ctl)
    const kitchensink::TempoMap* old = g_tmapActive.load( std::memory_order_acquire );
    if ( g_running )
    {
        // Audio callbacks read the map every block, so ALWAYS swap via RCU.
        // WHOLE-map replace (anchor 0): this DAW has one global tempo, so a
        // tempo change must retune the ENTIRE timeline -- anchoring at the
        // playhead would leave everything before it at the old tempo ("reset
        // won't take" when replaying from the top).  The playhead's MUSICAL
        // position is preserved instead: capture the current tick under the
        // old map and seek to that tick's sample under the new one.
        const long long curTick = old->sample_to_tick( g_transport.sample() );
        kitchensink::TempoMap* nm = old->copy_with_tempo_at( 0.0, bpm );
        if ( !nm ) return;
        g_tmapActive.store( nm, std::memory_order_release );
        g_transport.set_map( *nm );
        // tick-invariant relocate: same musical spot, new sample position.
        // NOT a schedule discontinuity -- ring ticks stay valid (dues are
        // recomputed against the new map at drain time), so no epoch bump.
        g_transport.request_seek( nm->tick_to_sample( curTick ) );
        // the loop points are musical too: rederive their samples.
        if ( g_loopRightTick > g_loopLeftTick )
        {
            g_loopStartSample.store( nm->tick_to_sample( g_loopLeftTick ),
                                     std::memory_order_relaxed );
            g_loopEndSample.store( nm->tick_to_sample( g_loopRightTick ),
                                   std::memory_order_relaxed );
        }
        tmap_gc_locked();
        if ( old != &g_tmap )
            g_tmapRetired.push_back( RetiredMap{ old,
                g_blockGen.load( std::memory_order_acquire ) } );
    }
    else
    {
        // no audio thread: mutate in place as before.
        const_cast<kitchensink::TempoMap*>( old )->set_tempo( bpm );
    }
    g_tempoBpm.store( bpm );          // DERIVED display cache; single write path
}

// --- engine-side loop points (published by the sequencer as TICKS) -----------
// The audio callback cycle-splits at the loop-end SAMPLE, so wraps land on the
// exact frame.  Samples rederive from the active map here and on tempo edits.
void audio_app_set_loop_ticks( long long leftTick, long long rightTick, int on )
{
    std::lock_guard<std::mutex> lk( g_tmapEditMx );
    g_loopLeftTick  = leftTick;
    g_loopRightTick = rightTick;
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    if ( on && rightTick > leftTick )
    {
        g_loopStartSample.store( map->tick_to_sample( leftTick ),
                                 std::memory_order_relaxed );
        g_loopEndSample.store( map->tick_to_sample( rightTick ),
                               std::memory_order_relaxed );
        g_loopOn.store( true, std::memory_order_release );
    }
    else
        g_loopOn.store( false, std::memory_order_release );
}
double    audio_app_tempo()                 { return g_tempoBpm.load(); }
void      audio_app_transport_locate( long long sample )
{
    if ( sample < 0 ) sample = 0;
    // A locate is a transport DISCONTINUITY: everything scheduled ahead of it
    // references the old position.  Bump the epoch so the drains discard those
    // messages, and have the audio thread flush the MidiIn node's lookahead.
    g_schedEpoch.fetch_add( 1, std::memory_order_release );
    g_flushMidiIn.store( true, std::memory_order_release );
    // Race-free seek (nerf 21 glue): the audio thread applies it at block
    // start, so a locate can never shear a block across two positions.  With
    // no audio thread to apply it, seek immediately.
    if ( g_running ) g_transport.request_seek( sample );
    else             g_transport.locate( sample );
}
long long audio_app_transport_sample()      { return g_transport.sample(); }
bool      audio_app_transport_rolling()     { return g_transport.rolling(); }
double    audio_app_transport_beats()       { return g_transport.beats(); }
void      audio_app_transport_bbt( int* bar, int* beat, int* tick )
{
    kitchensink::BBT b = g_transport.bbt();
    if ( bar )  *bar  = b.bars;
    if ( beat ) *beat = b.beats;
    if ( tick ) *tick = b.ticks;
}
long long audio_app_beats_to_sample( double beats )
{
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    return map->beats_to_sample( beats < 0 ? 0 : beats );
}
double    audio_app_sample_to_beats( long long s )
{
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    return map->sample_to_beats( s < 0 ? 0 : s );
}

// Integer 192-PPQN sequencer-tick conversions against the ACTIVE map -- what
// the output thread's lookahead scheduler and locate use, so the sequencer and
// the engine can never disagree on where a tick falls.
long long audio_app_tick_to_sample( long long tick )
{
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    return map->tick_to_sample( tick < 0 ? 0 : tick );
}
long long audio_app_sample_to_tick( long long sample )
{
    const kitchensink::TempoMap* map = g_tmapActive.load( std::memory_order_acquire );
    return map->sample_to_tick( sample < 0 ? 0 : sample );
}
double audio_app_sample_rate() { return g_sr; }

float audio_app_probe_render_peak( int blocks )
{
    if ( !g_engine || blocks < 1 ) return 0.f;
    const bool wasRun = g_engine->isRunning();
    if ( wasRun ) g_engine->stop();
    const bool wasModular = g_modular.load( std::memory_order_relaxed );
    g_transport.stop(); g_transport.locate( 0 ); g_transport.start();
    if ( wasModular ) audio_app_patch_set_playing( true );
    float pk = 0.f;
    std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
    float* o[2] = { L.data(), R.data() };
    for ( int b = 0; b < blocks; ++b ) {
        audio_render( o, 2, g_block, g_sr );
        for ( int i = 0; i < g_block; ++i ) { const float a = std::fabs( L[i] ); if ( a > pk ) pk = a; }
    }
    g_transport.stop(); if ( wasModular ) audio_app_patch_set_playing( false );
    g_transport.locate( 0 );
    if ( wasRun ) g_engine->start();
    return pk;
}

// --- bounce / render to disk -----------------------------------------------
// Realtime capture: begin allocates a buffer for `seconds`, the audio thread
// taps every master block into it, end() writes a 16-bit stereo WAV and frees.
bool audio_app_capture_begin( double seconds )
{
    if ( g_capturing.load() || g_capBuf.load() || seconds <= 0.0 ) return false;
    const size_t frames = (size_t)( seconds * g_sr ) + (size_t)g_block;
    float* buf = (float*) calloc( frames * 2, sizeof( float ) );
    if ( !buf ) return false;
    g_capCap = frames;                                    // published before buf
    g_capPos.store( 0 );
    g_capBuf.store( buf, std::memory_order_release );
    g_capturing.store( true, std::memory_order_release );
    return true;
}

bool audio_app_capture_end_wav( const char* path )
{
    g_capturing.store( false, std::memory_order_release );
    float* buf = g_capBuf.exchange( nullptr, std::memory_order_acq_rel );
    if ( !buf ) return false;
    // Grace period: a tap that snapshotted `buf` before the exchange may still
    // be writing this block.  Wait for TWO completed audio blocks (or a 250ms
    // timeout when the stream is stopped and no blocks are coming).
    const unsigned gen0 = g_blockGen.load( std::memory_order_acquire );
    for ( int i = 0; i < 250; ++i )
    {
        if ( g_blockGen.load( std::memory_order_acquire ) >= gen0 + 2 ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    const size_t frames = g_capPos.load();
    bool ok = false;
    FILE* f = fopen( path ? path : "render.wav", "wb" );
    if ( f )
    {
        const unsigned rate   = (unsigned) g_sr;
        const unsigned ch     = 2;
        const unsigned bits   = 16;
        const unsigned byteRate   = rate * ch * ( bits / 8 );
        const unsigned blockAlign = ch * ( bits / 8 );
        const unsigned dataBytes  = (unsigned)( frames * blockAlign );
        auto w32 = [&]( unsigned v ){ unsigned char b[4]={(unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16),(unsigned char)(v>>24)}; fwrite(b,1,4,f); };
        auto w16 = [&]( unsigned v ){ unsigned char b[2]={(unsigned char)v,(unsigned char)(v>>8)}; fwrite(b,1,2,f); };
        fwrite( "RIFF", 1, 4, f ); w32( 36 + dataBytes ); fwrite( "WAVE", 1, 4, f );
        fwrite( "fmt ", 1, 4, f ); w32( 16 ); w16( 1 ); w16( ch );
        w32( rate ); w32( byteRate ); w16( blockAlign ); w16( bits );
        fwrite( "data", 1, 4, f ); w32( dataBytes );
        for ( size_t i = 0; i < frames * 2; ++i )
        {
            float s = buf[i];
            if ( s !=  s  ) s =  0.f;    // NaN -> silence, never into the file
            if ( s >  1.f ) s =  1.f;
            if ( s < -1.f ) s = -1.f;
            w16( (unsigned)(int)( s * 32767.f ) & 0xFFFF );
        }
        fclose( f );
        ok = true;
    }
    free( buf );
    g_capCap = 0; g_capPos.store( 0 );
    return ok;
}

// Render one track's WHOLE signal chain offline into an in-memory AudioClip
// (freeze).  Renders in the CURRENT engine mode so it captures the real audible
// output -- the fixed-graph track (instrument + insert FX) OR the modular patch
// graph (patched instruments + effects).  The target track is isolated so the
// master tap == just that track: fixed graph solos it (master gain neutralised);
// modular mutes every OTHER master-mixer channel.  The caller's scheduleMidi()
// enqueues the pattern's events (ticks from 0) AFTER the seek so they land
// in-window.
bool audio_app_freeze_render( int track, double seconds,
                              const std::function<void()>& scheduleMidi,
                              PatchKnob::engine::AudioClip& outClip )
{
    if ( !g_engine || seconds <= 0.0 ) return false;
    if ( g_capturing.load() || g_capBuf.load() ) return false;   // a capture is busy

    const bool wasRunning = g_engine->isRunning();
    if ( wasRunning ) g_engine->stop();          // single-threaded block pump

    const bool modular = g_modular.load( std::memory_order_relaxed );

    // ---- isolate the target track (mode-specific) --------------------------
    // fixed-graph state
    std::vector<char> savedSolo;
    float             oldMaster      = 1.0f;
    bool              savedTargetMute = false;
    Track*            target         = nullptr;
    // modular master-mixer state
    patch::MasterMixerNode* mm = nullptr;
    std::vector<char>       savedMmMute;

    auto restoreAndReturn = [&]( bool ok ) -> bool {
        g_transport.stop();
        if ( modular ) audio_app_patch_set_playing( false );
        g_transport.locate( 0 );
        if ( modular ) { for ( int c = 0; c < (int)savedMmMute.size() && mm; ++c ) mm->setMute( c, savedMmMute[(size_t)c] != 0 ); }
        else if ( g_graph ) {
            for ( int i = 0; i < (int)savedSolo.size(); ++i ) g_graph->track( i )->setSolo( savedSolo[(size_t)i] != 0 );
            if ( target ) target->setMute( savedTargetMute );
            g_graph->setMasterGain( oldMaster );
        }
        if ( wasRunning ) g_engine->start();
        return ok;
    };

    if ( modular )
    {
        if ( !g_patch ) { if ( wasRunning ) g_engine->start(); return false; }
        // NO per-channel muting: capture the whole patch output with only the
        // target's MIDI scheduled.  Robust to incomplete master-mixer routing
        // (isolation comes from scheduling just this pattern's notes; a proper
        // per-track mixer tap can tighten this once the mixer routing is fixed).
        (void)mm; (void)savedMmMute; (void)track;
    }
    else
    {
        if ( !g_graph ) { if ( wasRunning ) g_engine->start(); return false; }
        const int NT = g_graph->trackCount();
        if ( track < 0 || track >= NT ) { if ( wasRunning ) g_engine->start(); return false; }
        savedSolo.resize( (size_t)NT );
        for ( int i = 0; i < NT; ++i ) { Track* t = g_graph->track( i ); savedSolo[(size_t)i] = t->solo() ? 1 : 0; t->setSolo( i == track ); }
        target = g_graph->track( track );
        savedTargetMute = target->mute();
        target->setMute( false );
        oldMaster = g_graph->masterGain();
        g_graph->setMasterGain( 1.0f );
    }

    if ( !audio_app_capture_begin( seconds ) ) return restoreAndReturn( false );

    // Rewind, roll, let the caller schedule the pattern's MIDI in-window.
    g_transport.locate( 0 );
    g_transport.start();
    if ( modular ) audio_app_patch_set_playing( true );
    if ( scheduleMidi ) scheduleMidi();

    const long long endSample = (long long)( seconds * (double)g_sr ) + (long long)g_block;
    {
        std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
        float* out[2] = { L.data(), R.data() };
        long long pumped = 0;
        while ( pumped < endSample )
        {
            audio_render( out, 2, g_block, g_sr );   // taps master -> g_capBuf
            pumped += (long long)g_block;
        }
    }

    // Read the captured interleaved master into the clip at full float precision.
    g_capturing.store( false, std::memory_order_release );
    float* buf = g_capBuf.exchange( nullptr, std::memory_order_acq_rel );
    const size_t frames = g_capPos.load();
    bool ok = false;
    if ( buf )
    {
        outClip.sampleRate       = (double)g_sr;
        outClip.sourceSampleRate = (double)g_sr;
        outClip.resize( (int64_t)frames );
        for ( size_t i = 0; i < frames; ++i )
        {
            float l = buf[i * 2 + 0], r = buf[i * 2 + 1];
            if ( l != l ) l = 0.f;
            if ( r != r ) r = 0.f;
            outClip.ch[0][i] = l;
            outClip.ch[1][i] = r;
        }
        free( buf );
        ok = frames > 0;
    }
    g_capCap = 0;
    g_capPos.store( 0 );

    return restoreAndReturn( ok );
}

// --- frozen-clip playback -----------------------------------------------------
// Each frozen clip is owned here and scheduled on an AudioClipPlayer.  A CLIP
// freeze uses a shared hidden freeze track (track < 0); a TRACK freeze passes an
// explicit engine track (the new audio lane's track), so its player is that
// track's own instrument.  Attach returns a stable id; detach removes exactly
// that clip from its player, so unfreeze is surgical.
namespace {
struct FreezeEntry { int id; AudioClipPlayer* player; std::unique_ptr<AudioClip> clip; };
std::vector<FreezeEntry> g_freezeEntries;
// g_freezePlayer is declared at file scope (top) so the modular render can sound it.
int                      g_freezeTrack  = -1;
int                      g_freezeNextId = 1;

// A fixed-graph track with no instrument/FX we can borrow for frozen audio
// (scanned high-to-low so it avoids the low-index sequence tracks).
int findFreeGraphTrack()
{
    if ( !g_graph ) return -1;
    if ( g_freezeTrack >= 0 ) return g_freezeTrack;
    for ( int i = g_graph->trackCount() - 1; i >= 0; --i )
    {
        Track* t = g_graph->track( i );
        if ( t && !t->instrument() && t->fxCount() == 0 ) return i;
    }
    return -1;
}

// The AudioClipPlayer to schedule a frozen clip on.  track < 0 -> the shared
// hidden freeze track; track >= 0 -> that engine track's own player (created as
// its instrument, like a normal audio track).
AudioClipPlayer* freezePlayerForTrack( int track )
{
    if ( !g_graph ) return nullptr;
    if ( track < 0 )
    {
        if ( !g_freezePlayer )
        {
            const int tr = findFreeGraphTrack();
            if ( tr < 0 ) return nullptr;
            g_freezeTrack = tr;
            g_freezePlayer = new AudioClipPlayer();
            if ( !g_freezePlayer->prepare( g_sr, g_block ) ) { delete g_freezePlayer; g_freezePlayer = nullptr; return nullptr; }
            g_freezePlayer->setActive( true );
            g_owned.push_back( g_freezePlayer );
            g_graph->track( tr )->setInstrument( g_freezePlayer );
        }
        return g_freezePlayer;
    }
    if ( track >= AUDIO_APP_MAX_TRACKS ) return nullptr;
    Track* trk = g_graph->track( track );
    if ( !trk ) return nullptr;
    if ( !g_projectAudioPlayers[track] )
    {
        AudioClipPlayer* p = new AudioClipPlayer();
        if ( !p->prepare( g_sr, g_block ) ) { delete p; return nullptr; }
        p->setActive( true );
        g_owned.push_back( p );
        g_projectAudioPlayers[track] = p;
        trk->setInstrument( p );
    }
    return g_projectAudioPlayers[track];
}
} // namespace

int audio_app_freeze_attach( int track, const AudioClip& clip, long long startSample, float gain )
{
    if ( clip.empty() ) return -1;
    AudioClipPlayer* player = freezePlayerForTrack( track );
    if ( !player ) return -1;
    std::unique_ptr<AudioClip> stored( new AudioClip( clip ) );
    AudioClip* raw = stored.get();
    if ( !player->addClip( raw, startSample, gain ) ) return -1;
    const int id = g_freezeNextId++;
    g_freezeEntries.push_back( FreezeEntry{ id, player, std::move( stored ) } );
    return id;
}

const AudioClip* audio_app_freeze_clip( int id )
{
    for ( const FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) return e.clip.get();
    return nullptr;
}

void audio_app_freeze_detach( int id )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.player )
        {
            const AudioClip* target = e.clip.get();
            for ( int i = 0; i < e.player->clipCount(); ++i )
                if ( e.player->clipAt( i ).clip == target ) { e.player->removeClip( i ); break; }
            e.player->clearWarp( target );      // drop the per-clip stretcher (was leaking)
            break;
        }
    // The clip pointer is now unscheduled and its warp entry gone, but a live
    // audio block may still be reading the PREVIOUS snapshot -- wait it out before
    // the erase frees the AudioClip, else the audio thread reads freed samples.
    freeze_rcu_grace();
    g_freezeEntries.erase(
        std::remove_if( g_freezeEntries.begin(), g_freezeEntries.end(),
                        [id]( const FreezeEntry& e ){ return e.id == id; } ),
        g_freezeEntries.end() );
}

void audio_app_set_track_disabled( int track, bool disabled )
{
    if ( !g_graph || track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return;
    if ( Track* t = g_graph->track( track ) ) t->setDisabled( disabled );
}

// Find the editable index of a freeze entry's region on its player.
namespace {
int freezeRegionIndex( const FreezeEntry& e )
{
    if ( !e.player ) return -1;
    const AudioClip* target = e.clip.get();
    for ( int i = 0; i < e.player->clipCount(); ++i )
        if ( e.player->clipAt( i ).clip == target ) return i;
    return -1;
}
} // namespace

// Move a frozen region to a new timeline start (samples), preserving its
// source-offset + length + fades.  Returns false if the id/clip is unknown.
bool audio_app_freeze_move( int id, long long newStartSample )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id )
        {
            const int i = freezeRegionIndex( e );
            if ( i < 0 ) return false;
            const ScheduledClip sc = e.player->clipAt( i );
            return e.player->setClipRegion( i, newStartSample, sc.sourceOffset, sc.length );
        }
    return false;
}

// Full non-destructive REGION update: timeline position, start-offset into the
// source, and length (all in samples; length<=0 == to source end).  This is the
// trim/slip/move primitive the arrange view drives from a clip's trigger.
bool audio_app_freeze_set_region( int id, long long positionSample,
                                  long long sourceOffsetSample, long long lengthSample )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id )
        {
            const int i = freezeRegionIndex( e );
            if ( i < 0 ) return false;
            return e.player->setClipRegion( i, positionSample, sourceOffsetSample, lengthSample );
        }
    return false;
}

bool audio_app_freeze_get_region( int id, long long* startSample,
                                  long long* sourceOffset, long long* length,
                                  float* gain, int* muted, int* loop )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id )
        {
            const int i = freezeRegionIndex( e );
            if ( i < 0 ) return false;
            const ScheduledClip* sc = e.player->scheduled( i );
            if ( !sc ) return false;
            if ( startSample )  *startSample  = sc->startSample;
            if ( sourceOffset ) *sourceOffset = sc->sourceOffset;
            if ( length )       *length       = sc->length;
            if ( gain )         *gain         = sc->gain;
            if ( muted )        *muted        = sc->muted ? 1 : 0;
            if ( loop )         *loop         = sc->loop  ? 1 : 0;
            return true;
        }
    return false;
}

// Region GAIN (Ardour scale_amplitude; negative = phase invert, unclamped).
bool audio_app_freeze_set_gain( int id, float gain )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) { const int i = freezeRegionIndex( e ); return i >= 0 && e.player->setClipGain( i, gain ); }
    return false;
}

// Region MUTE.
bool audio_app_freeze_set_muted( int id, bool muted )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) { const int i = freezeRegionIndex( e ); return i >= 0 && e.player->setClipMuted( i, muted ); }
    return false;
}

// Region LOOP (wrap the source to fill an extended length).
bool audio_app_freeze_set_loop( int id, bool loop )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) { const int i = freezeRegionIndex( e ); return i >= 0 && e.player->setClipLoop( i, loop ); }
    return false;
}

// NORMALIZE (Ardour): gain = target / peak, target = dB_to_coefficient(targetDb)
// (clamped just under unity at 0 dB), peak = max |sample| over the REGION window
// of the RAW source.  Returns the applied gain (0 if silent / not found).
float audio_app_freeze_normalize( int id, float targetDb )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.clip )
        {
            const int i = freezeRegionIndex( e );
            if ( i < 0 ) return 0.f;
            const ScheduledClip* sc = e.player->scheduled( i );
            if ( !sc ) return 0.f;
            const AudioClip* c = e.clip.get();
            const int64_t off = sc->sourceOffset;
            const int64_t len = sc->regionLength();
            float peak = 0.f;
            for ( int ch = 0; ch < 2; ++ch )
                for ( int64_t k = off; k < off + len && k < c->numFrames(); ++k )
                { const float a = std::fabs( c->ch[(size_t)ch][(size_t)k] ); if ( a > peak ) peak = a; }
            if ( peak < 1e-7f ) return 0.f;
            float target = std::pow( 10.f, targetDb * 0.05f );
            if ( target >= 1.f ) target = 1.f - 1e-6f;         // avoid appearing clipped at 0 dBFS
            const float g = target / peak;
            e.player->setClipGain( i, g );
            return g;
        }
    return 0.f;
}

// Publish a LIVE warp map to a region's player so it time-stretches in realtime
// during playback (you hear warp-marker edits live).  Empty/1 marker = raw.
bool audio_app_freeze_set_warp( int id, const PatchKnob::engine::WarpMarker* markers, int count )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.player && e.clip )
        {
            std::vector<PatchKnob::engine::WarpMarker> m;
            if ( markers && count > 0 ) m.assign( markers, markers + count );
            e.player->setWarp( e.clip.get(), m );
            return true;
        }
    return false;
}

// Replace a region's source clip with `newClip` (e.g. the warped/stretched
// result) and re-place a FULL region at the same timeline start + gain.  The
// region length follows the new clip length.  Stop the transport first.
bool audio_app_freeze_replace_clip( int id, const AudioClip& newClip )
{
    if ( newClip.empty() ) return false;
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.player && e.clip )
        {
            const int i = freezeRegionIndex( e );
            int64_t startSample = 0; float gain = 1.f;
            if ( i >= 0 ) { const ScheduledClip* sc = e.player->scheduled( i );
                            if ( sc ) { startSample = sc->startSample; gain = sc->gain; }
                            e.player->removeClip( i ); }
            e.player->clearWarp( e.clip.get() );            // data is changing: drop stale stretcher
            freeze_rcu_grace();                             // drain readers before freeing old buffers
            *e.clip = newClip;                              // swap owned data (pointer stable)
            return e.player->addClip( e.clip.get(), startSample, gain );  // full region
        }
    return false;
}

// REVERSE the region's source window in place (Ardour makes a new reversed
// source; here we reverse this region's own clip copy -- freeze/copy/split each
// hold a distinct copy, so neighbours are unaffected).
bool audio_app_freeze_reverse( int id )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.clip )
        {
            const int i = freezeRegionIndex( e );
            if ( i < 0 ) return false;
            const ScheduledClip* sc = e.player->scheduled( i );
            if ( !sc ) return false;
            AudioClip* c = e.clip.get();
            const int64_t off = sc->sourceOffset;
            const int64_t len = sc->regionLength();
            for ( int ch = 0; ch < 2; ++ch )
            {
                int64_t a = off, b = off + len - 1;
                while ( a < b && b < c->numFrames() )
                { std::swap( c->ch[(size_t)ch][(size_t)a], c->ch[(size_t)ch][(size_t)b] ); ++a; --b; }
            }
            return true;
        }
    return false;
}

bool audio_app_freeze_set_fades( int id, long long fadeInFrames, long long fadeOutFrames,
                                 float fadeInTension, float fadeOutTension )
{
    for ( const FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.player )
        {
            const AudioClip* target = e.clip.get();
            for ( int i = 0; i < e.player->clipCount(); ++i )
                if ( e.player->clipAt( i ).clip == target )
                    return e.player->setClipFades( i, (int64_t)fadeInFrames, (int64_t)fadeOutFrames,
                                                   fadeInTension, fadeOutTension );
        }
    return false;
}

// Headless proof of the freeze render/capture path: a continuous sine -> Out in
// modular mode, frozen offline; a non-zero peak proves the pump+capture work
// with the stream stopped (isolating render bugs from MIDI/instrument routing).
float audio_app_freeze_selftest()
{
    if ( !g_patch ) return -1.f;
    const int sine = audio_app_patch_add_builtin( "sine" );
    if ( sine < 0 ) return -1.f;
    audio_app_patch_connect( sine, 0, (int)g_patchOutId, 0 );
    const bool wasModular = g_modular.load();
    audio_app_set_modular( true );

    PatchKnob::engine::AudioClip clip;
    const bool ok = audio_app_freeze_render( 0, 0.2, std::function<void()>(), clip );
    float peak = 0.f;
    for ( int c = 0; c < 2; ++c )
        for ( float v : clip.ch[(size_t)c] ) { const float a = v < 0.f ? -v : v; if ( a > peak ) peak = a; }
    fprintf( stderr, "[freeze-selftest] ok=%d frames=%lld peak=%.4f (%s)\n",
             (int)ok, (long long)clip.numFrames(), peak,
             peak > 0.0001f ? "CAPTURE OK" : "CAPTURE SILENT" );
    fflush( stderr );

    audio_app_patch_disconnect( sine, 0, (int)g_patchOutId, 0 );
    audio_app_patch_remove( sine );

    // Mixer-routing check: sine -> its master-mixer AUDIO inlet -> master -> Out.
    // Proves audio_app_master_connect_instrument wires audio into the mixer (the
    // "it's not connecting" complaint).
    {
        const int track = audio_app_master_add_track( 0 );
        const int sine2 = audio_app_patch_add_builtin( "sine" );
        const int inlet = ( track >= 0 && sine2 >= 0 )
                          ? audio_app_master_connect_instrument( track, sine2 ) : -1;
        audio_app_set_modular( true );
        PatchKnob::engine::AudioClip mclip;
        const bool mok = ( track >= 0 ) && audio_app_freeze_render( track, 0.1, std::function<void()>(), mclip );
        float mpeak = 0.f;
        for ( int c = 0; c < 2; ++c )
            for ( float v : mclip.ch[(size_t)c] ) { const float a = v < 0.f ? -v : v; if ( a > mpeak ) mpeak = a; }
        fprintf( stderr, "[mixer-selftest] connect inlet=%d render_ok=%d peak=%.4f (%s)\n",
                 inlet, (int)mok, mpeak, ( inlet >= 2 && mpeak > 0.0001f ) ? "MIXER ROUTE OK" : "MIXER ROUTE BROKEN" );
        fflush( stderr );
        if ( sine2 >= 0 ) audio_app_patch_remove( sine2 );
        if ( track >= 0 ) audio_app_master_remove_track( track );
    }

    audio_app_set_modular( wasModular );
    return peak;
}

// Realistic freeze proof: a MIDI-CV -> VCO -> VCA voice with an ADSR envelope,
// built in a Rack node, plays 16 notes and is frozen offline.  Verifies the
// whole synth-voice path renders and freezes: returns the captured peak, and
// logs peak + how many distinct note bursts were captured (expect ~16).
float audio_app_freeze_voice_selftest()
{
    if ( !g_patch ) return -1.f;
    const int rackNode = audio_app_patch_add_rack();
    ::rackx::RackEngine* re = rackNode >= 0 ? audio_app_rack_engine( rackNode ) : nullptr;
    if ( !re ) { if ( rackNode >= 0 ) audio_app_patch_remove( rackNode ); return -1.f; }

    // Build the full voice the user asked for: MIDI-CV(pitch,gate) -> VCO -> VCA,
    // with an ADSR envelope shaping the VCA (gate -> ADSR -> VCA CV).
    audio_app_rack_set_poly( rackNode, 1 );             // allocate a MIDI voice
    const int outId  = re->ensureDefaultIO();
    const int midiId = re->addModule( "MIDI-CV", 0.f, 0.f );
    const int vco    = re->addModule( "VCO", 0.f, 0.f );
    const int adsr   = re->addModule( "ADSR", 0.f, 0.f );
    const int vca    = re->addModule( "VCA", 0.f, 0.f );
    int cables = 0;
    // NOTE: the "ADSR" is the real Fundamental ADSR -- its inputs are
    // ATTACK=0,DECAY=1,SUSTAIN=2,RELEASE=3,GATE=4,RETRIG=5, env output=0.  The
    // gate MUST go to input 4 (feeding port 0 lands on the attack-time CV and the
    // envelope never opens).
    cables += re->addCable( midiId, 0, vco,  0 ) > 0;   // pitch -> VCO v/oct
    cables += re->addCable( midiId, 1, adsr, 4 ) > 0;   // gate  -> ADSR GATE_INPUT (port 4)
    cables += re->addCable( adsr,   0, vca,  1 ) > 0;   // env   -> VCA CV
    cables += re->addCable( vco,    1, vca,  0 ) > 0;   // saw   -> VCA in
    cables += re->addCable( vca,    0, outId, 0 ) > 0;  // VCA   -> out L
    cables += re->addCable( vca,    0, outId, 1 ) > 0;  // VCA   -> out R
    // Pure gate-shaped envelope: instant attack, no decay, full sustain, instant
    // release (ATT=0, DEC=0, SUS=1, REL=0) -- the env just tracks the gate.
    re->setParam( adsr, 0, 0.0f ); re->setParam( adsr, 1, 0.0f );
    re->setParam( adsr, 2, 1.0f ); re->setParam( adsr, 3, 0.0f );

    // DIAGNOSTIC: drive the engine DIRECTLY (bypass the graph/freeze path) with
    // the SAME gate pattern, so we can tell "ADSR voice doesn't sound at all"
    // from "it sounds direct but the freeze/graph path drops it".  Feed the gate
    // low first (idle), THEN the note, so the ADSR's Schmitt gate sees a genuine
    // rising edge (a note landing on the very first sample is a cold-start miss).
    // Probe the port voltages so we see exactly where the signal dies.
    {
        std::vector<float> dL( 512 ), dR( 512 );
        re->process( 512, nullptr, nullptr, dL.data(), dR.data(), nullptr, 0, 120.f, true ); // idle: gate low
        PatchKnob::engine::MidiEvent on; on.sampleOffset = 0; on.status = 0x90; on.data1 = 60; on.data2 = 100;
        re->process( 512, nullptr, nullptr, dL.data(), dR.data(), &on, 1, 120.f, true );
        float dpk = 0.f;
        for ( int b = 0; b < 8; ++b ) {
            re->process( 512, nullptr, nullptr, dL.data(), dR.data(), nullptr, 0, 120.f, true );
            for ( float v : dL ) { const float a = std::fabs( v ); if ( a > dpk ) dpk = a; }
        }
        // Read the actual port voltages while the gate is held high.
        auto outV = [&]( int mid, int port ) -> float {
            ::rackx::RackModule* rm = re->moduleById( mid );
            if ( !rm || !rm->mod ) return -999.f;
            auto& p = rm->mod->outputs;
            return ( port >= 0 && port < (int)p.size() ) ? p[(size_t)port].getVoltage() : -999.f;
        };
        auto inV = [&]( int mid, int port ) -> float {
            ::rackx::RackModule* rm = re->moduleById( mid );
            if ( !rm || !rm->mod ) return -999.f;
            auto& p = rm->mod->inputs;
            return ( port >= 0 && port < (int)p.size() ) ? p[(size_t)port].getVoltage() : -999.f;
        };
        const float gateOut = outV( midiId, 1 );   // MidiCV GATE output
        const float adsrIn  = inV(  adsr,   4 );   // ADSR GATE input (port 4)
        const float envOut  = outV( adsr,   0 );   // ADSR ENV output
        const float vcaCv   = inV(  vca,    1 );   // VCA CV input
        const float vcaOut  = outV( vca,    0 );   // VCA output
        PatchKnob::engine::MidiEvent off; off.sampleOffset = 0; off.status = 0x80; off.data1 = 60; off.data2 = 0;
        re->process( 512, nullptr, nullptr, dL.data(), dR.data(), &off, 1, 120.f, true );
        fprintf( stderr, "[voice-direct] peak=%.4f | gateOut=%.2f adsrIn=%.2f envOut=%.2f vcaCv=%.2f vcaOut=%.2f (%s)\n",
                 dpk, gateOut, adsrIn, envOut, vcaCv, vcaOut,
                 dpk > 0.001f ? "ENGINE OK" : "ENGINE SILENT" );
        fflush( stderr );
    }

    // Wire it the REAL way: MidiIn -> voice + voice audio -> its master-mixer
    // inlet -> master -> Out (NOT straight to Out, whose input is already taken
    // by the master mixer -- audio inputs are fan-in-capped at one).
    const int voiceTrack = audio_app_master_add_track( 0 );
    const int inlet = ( voiceTrack >= 0 )
                      ? audio_app_master_connect_instrument( voiceTrack, rackNode ) : -1;
    audio_app_set_modular( true );

    const double oldTempo = audio_app_tempo();
    audio_app_set_tempo( 120.0 );
    const int    kNotes  = 16;
    const long   kSpacing = 48;               // 16th notes @ 192 PPQN
    const long   kGate    = 30;
    const double seconds = (double)( kNotes * kSpacing + 96 ) / 192.0 * 60.0 / 120.0 + 0.5;

    PatchKnob::engine::AudioClip clip;
    auto sched = [&]() {
        for ( int k = 0; k < kNotes; ++k ) {
            audio_app_route_midi( 0, 0x90, (unsigned char)( 48 + k ), 100, (long long)k * kSpacing );
            audio_app_route_midi( 0, 0x80, (unsigned char)( 48 + k ), 0,   (long long)k * kSpacing + kGate );
        }
    };
    const bool ok = audio_app_freeze_render( 0, seconds, sched, clip );

    // Analyse: peak + count note bursts (amplitude rising through a threshold
    // after quiet) to confirm the notes actually rendered.
    float peak = 0.f;
    int   bursts = 0;
    bool  above = false;
    const int64_t nf = clip.numFrames();
    float envf = 0.f;
    for ( int64_t i = 0; i < nf; ++i )
    {
        const float a = std::fabs( clip.ch[0][(size_t)i] );
        if ( a > peak ) peak = a;
        envf += ( a - envf ) * 0.002f;                 // ~cheap RMS follower
        if ( !above && envf > 0.03f ) { above = true; ++bursts; }
        else if ( above && envf < 0.01f ) above = false;
    }
    fprintf( stderr, "[freeze-voice] cables=%d/6 inlet=%d ok=%d frames=%lld peak=%.4f bursts=%d (%s)\n",
             cables, inlet, (int)ok, (long long)nf, peak, bursts,
             ( peak > 0.001f && bursts >= 8 ) ? "VOICE FREEZE OK" : "VOICE FREEZE BROKEN" );
    fflush( stderr );

    // PLAYBACK CHECK (the "track freeze -> no audio" bug): the frozen clip must
    // be AUDIBLE in MODULAR mode.  Attach it to a fresh track's player, roll the
    // transport, render modular blocks and read the master out DIRECTLY (the
    // capture tap sits upstream of the clip players by design, so we can't use
    // it here) -- non-silence proves render_modular_clip_players sounds it.
    if ( ok && !clip.empty() )
    {
        const int playTrack = audio_app_master_add_track( 0 );
        const int fid = ( playTrack >= 0 )
                        ? audio_app_freeze_attach( playTrack, clip, 0, 1.0f ) : -1;
        audio_app_set_modular( true );
        const bool wasRun = g_engine && g_engine->isRunning();
        if ( wasRun ) g_engine->stop();
        g_transport.stop(); g_transport.locate( 0 ); g_transport.start();
        audio_app_patch_set_playing( true );
        float ppk = 0.f;
        std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
        float* o[2] = { L.data(), R.data() };
        for ( int b = 0; b < 32; ++b ) {
            audio_render( o, 2, g_block, g_sr );
            for ( int i = 0; i < g_block; ++i ) { const float a = std::fabs( L[i] ); if ( a > ppk ) ppk = a; }
        }
        g_transport.stop(); audio_app_patch_set_playing( false ); g_transport.locate( 0 );
        if ( wasRun && g_engine ) g_engine->start();
        if ( fid >= 0 ) audio_app_freeze_detach( fid );
        if ( playTrack >= 0 ) audio_app_master_remove_track( playTrack );
        fprintf( stderr, "[freeze-playback] modular peak=%.4f (%s)\n",
                 ppk, ppk > 0.001f ? "PLAYS IN MODULAR OK" : "SILENT IN MODULAR" );
        fflush( stderr );
    }

    audio_app_set_tempo( oldTempo );
    audio_app_patch_remove( rackNode );
    if ( voiceTrack >= 0 ) audio_app_master_remove_track( voiceTrack );
    return peak;
}

int audio_app_patch_node_port_count( int node )
{
    if ( !g_patch ) return 0;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    return n ? n->numPorts() : 0;
}

bool audio_app_patch_node_port( int node, int index, int* k, int* d, int* ch )
{
    if ( !g_patch ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( !n || index < 0 || index >= n->numPorts() ) return false;
    patch::PortDesc pd = n->port( index );
    if ( k )  *k  = ( pd.kind == patch::PortKind::Midi ) ? 1 : 0;
    if ( d )  *d  = ( pd.dir  == patch::PortDir::Out  ) ? 1 : 0;
    if ( ch ) *ch = pd.channels;
    return true;
}

const char* audio_app_patch_node_port_name( int node, int index )
{
    if ( !g_patch ) return "";
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( !n || index < 0 || index >= n->numPorts() ) return "";
    const char* nm = n->port( index ).name;
    return nm ? nm : "";
}

float audio_app_patch_selftest( const char* vst_path )
{
    if ( !g_running || !g_patch ) return -1.f;

    // isolate the graph render: SineSource -> Out (no plugin / no MIDI).
    {
        int sine = audio_app_patch_add_builtin( "sine" );
        bool sc = audio_app_patch_connect( sine, 0, (int)g_patchOutId, 0 );
        audio_app_set_modular( true );
        std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
        float sp = g_engine->masterPeak();
        fprintf( stderr, "[patchtest] SINE->Out connect=%d peak=%.4f (%s)\n",
                 (int)sc, sp, sp > 0.0001f ? "device-out OK" : "device-out SILENT" );
        fflush( stderr );
        audio_app_patch_disconnect( sine, 0, (int)g_patchOutId, 0 );
        audio_app_patch_remove( sine );
        audio_app_set_modular( false );
    }

    // MULTI-THREAD smoke test: a pure-audio chain sine->gain->gain->gain->Out
    // (>= 4 nodes, no MIDI) is exactly what the parallel path activates on.
    {
        audio_app_set_multithreaded( true );
        int s  = audio_app_patch_add_builtin( "sine" );
        int g1 = audio_app_patch_add_builtin( "gain" );
        int g2 = audio_app_patch_add_builtin( "gain" );
        int g3 = audio_app_patch_add_builtin( "gain" );
        audio_app_patch_connect( s, 0, g1, 0 );
        audio_app_patch_connect( g1, 1, g2, 0 );
        audio_app_patch_connect( g2, 1, g3, 0 );
        audio_app_patch_connect( g3, 1, (int)g_patchOutId, 0 );
        audio_app_set_modular( true );
        std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
        float mp = g_engine->masterPeak();
        fprintf( stderr, "[patchtest] MT chain (sine->3xgain->Out) peak=%.4f (%s)\n",
                 mp, mp > 0.0001f ? "multi-thread OK" : "MT SILENT" );
        fflush( stderr );
        audio_app_set_modular( false );
        audio_app_patch_disconnect( g3, 1, (int)g_patchOutId, 0 );
        audio_app_patch_remove( s ); audio_app_patch_remove( g1 );
        audio_app_patch_remove( g2 ); audio_app_patch_remove( g3 );
        audio_app_set_multithreaded( false );
    }

    // PURE DATA smoke test: a Pd node running test_sine.pd (osc~ 220 -> dac~)
    // wired straight to Out.  Proves the embedded libpd path makes sound.
    {
        int pd = audio_app_patch_add_pd();
        bool loaded = audio_app_pd_load( pd, "test_sine.pd" );
        bool pc = audio_app_patch_connect( pd, 2, (int)g_patchOutId, 0 );  // Pd audio-out -> Out
        audio_app_set_modular( true );
        std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
        float pp = g_engine->masterPeak();
        fprintf( stderr, "[patchtest] Pd (osc~220->dac~) loaded=%d connect=%d peak=%.4f (%s)\n",
                 (int)loaded, (int)pc, pp, pp > 0.0001f ? "libpd OK" : "libpd SILENT" );
        fflush( stderr );
        audio_app_set_modular( false );
        audio_app_patch_disconnect( pd, 2, (int)g_patchOutId, 0 );
        audio_app_patch_remove( pd );
    }

    PluginDescriptor d = make_descriptor( vst_path );
    int pnode = audio_app_patch_add_plugin( d );
    if ( pnode < 0 ) { fprintf( stderr, "[patchtest] plugin load failed\n" ); return -1.f; }

    // find the plugin node's first MIDI-in and first audio-out ports
    int midiInPort = -1, audioOutPort = -1;
    int nports = audio_app_patch_node_port_count( pnode );
    for ( int i = 0; i < nports; ++i ) {
        int kk, dd, cc;
        if ( audio_app_patch_node_port( pnode, i, &kk, &dd, &cc ) ) {
            if ( kk == 1 && dd == 0 && midiInPort   < 0 ) midiInPort   = i;   // midi in
            if ( kk == 0 && dd == 1 && audioOutPort < 0 ) audioOutPort = i;   // audio out
        }
    }
    bool cMidi = ( midiInPort   >= 0 ) && audio_app_patch_connect( (int)g_patchMidiInId, 0, pnode, midiInPort );
    bool cAud  = ( audioOutPort >= 0 ) && audio_app_patch_connect( pnode, audioOutPort, (int)g_patchOutId, 0 );
    fprintf( stderr, "[patchtest] pnode=%d ports=%d midiInPort=%d audioOutPort=%d "
                     "connectMidi=%d connectAudio=%d outNode=%d\n",
             pnode, nports, midiInPort, audioOutPort, (int)cMidi, (int)cAud, (int)g_patchOutId );
    fflush( stderr );

    audio_app_set_modular( true );
    // push the note-on directly into the MidiIn node (single-producer message
    // thread) + a longer wait to survive heavy async plugin init.
    if ( g_midiInNode ) {
        MidiEvent on; on.sampleOffset=0; on.status=0x90; on.data1=60; on.data2=110;
        g_midiInNode->push( on );
    }
    std::this_thread::sleep_for( std::chrono::milliseconds( 800 ) );
    float peak = g_engine->masterPeak();
    if ( g_midiInNode ) {
        MidiEvent off; off.sampleOffset=0; off.status=0x80; off.data1=60; off.data2=0;
        g_midiInNode->push( off );
    }
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    fprintf( stderr, "[patchtest] MidiIn->%s->Out modular peak=%.4f (%s)\n",
             vst_path, peak, peak > 0.0001f ? "AUDIBLE - modular works!" : "silent" );
    fflush( stderr );
    return peak;
}

// --- clock acceptance test ---------------------------------------------------
// 32nd notes at 500 BPM (one per 24 ticks = 720 samples at 48kHz) scheduled as
// TICKED events into the real ring, rendered by pumping audio_render directly
// with the stream stopped.  Onsets are read back from a MidiOut node's timed
// ring (dueSample), so this exercises the whole chain: tick ring -> tempo map
// -> holdback drain -> MidiIn -> graph -> MidiOut.
double audio_app_clock_selftest()
{
    if ( !g_patch || !g_engine ) return -1.0;

    const bool wasRunning = g_engine->isRunning();
    if ( wasRunning ) g_engine->stop();          // we pump blocks ourselves

    const bool  wasModular = g_modular.load();
    const double oldTempo  = audio_app_tempo();

    audio_app_set_tempo( 500.0 );

    // MidiIn (default node) -> a scratch MidiOut node.
    int outId = audio_app_patch_add_midi_out( -1 );
    if ( outId < 0 ) return -1.0;
    patch::MidiOutNode* mo =
        dynamic_cast<patch::MidiOutNode*>( g_patch->node( (patch::NodeId)outId ) );
    if ( !mo ) { audio_app_patch_remove( outId ); return -1.0; }
    audio_app_patch_connect( (int)g_patchMidiInId, 0, outId, 0 );
    audio_app_set_modular( true );

    // Rewind + roll.
    g_transport.request_seek( 0 );
    audio_app_patch_set_playing( true );

    // Schedule 512 32nd notes (note-on vel 100 + note-off 12 ticks later).
    const int    kNotes = 512;
    const double spt    = (double)g_sr * 60.0 / ( 500.0 * 192.0 ); // samples per 192-PPQN tick
    for ( int k = 0; k < kNotes; ++k )
    {
        audio_app_route_midi( 0, 0x90, 60, 100, (long long)k * 24 );
        audio_app_route_midi( 0, 0x80, 60, 0,   (long long)k * 24 + 12 );
    }

    // Pump blocks and collect note-on onsets from the MidiOut timed ring.
    std::vector<long long> onsets;
    onsets.reserve( kNotes );
    {
        std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
        float* out[2] = { L.data(), R.data() };
        const long long endSample =
            (long long)llround( (double)( (long long)kNotes * 24 + 48 ) * spt );
        long long pumped = 0;
        while ( pumped < endSample )
        {
            audio_render( out, 2, g_block, g_sr );
            pumped += g_block;
            patch::TimedMidi tm;
            while ( mo->pop( tm ) )
                if ( ( tm.ev.status & 0xF0 ) == 0x90 && tm.ev.data2 > 0 )
                    onsets.push_back( tm.dueSample );
        }
    }

    // Restore everything.
    audio_app_patch_set_playing( false );
    audio_app_patch_disconnect( (int)g_patchMidiInId, 0, outId, 0 );
    audio_app_patch_remove( outId );
    audio_app_set_modular( wasModular );
    audio_app_set_tempo( oldTempo );
    g_transport.request_seek( 0 );
    if ( wasRunning ) g_engine->start();

    // Analyze: ideal inter-onset = tick_to_sample(24) exactly (720 @48k/500).
    const kitchensink::TempoMap* map = &g_tmap;
    const long long ideal = (long long)llround( 24.0 * spt );
    double maxDev = 0.0; (void)map;
    if ( (int)onsets.size() != kNotes )
    {
        fprintf( stderr, "[clock-selftest] FAIL: %d/%d onsets delivered\n",
                 (int)onsets.size(), kNotes );
        return 1e9;
    }
    for ( size_t i = 1; i < onsets.size(); ++i )
    {
        const double dev = fabs( (double)( onsets[i] - onsets[i-1] - ideal ) );
        if ( dev > maxDev ) maxDev = dev;
    }
    const long long drift = onsets.back() - onsets.front()
                          - (long long)( kNotes - 1 ) * ideal;
    fprintf( stderr, "[clock-selftest] %d onsets, ideal IOI %lld samples, "
             "max deviation %.1f samples, end-to-end drift %lld samples\n",
             (int)onsets.size(), ideal, maxDev, drift );
    fflush( stderr );
    return maxDev;
}

// --- loop acceptance test ----------------------------------------------------
// One 4/4 bar (768 ticks) looped at 500 BPM with 32nds on the grid, driven by a
// faithful simulation of the output thread's lookahead scheduler (including its
// wrap branch that enqueues the NEXT pass's events before the engine wraps).
// Verifies the engine-side cycle-split loop end to end: every pass must deliver
// all 32 onsets at EXACTLY due = k*720 -- the loop-tail-displacement bug shows
// up here as missing/extra onsets or dues shifted by a pass.
double audio_app_loop_selftest()
{
    if ( !g_patch || !g_engine ) return -1.0;

    const bool wasRunning = g_engine->isRunning();
    if ( wasRunning ) g_engine->stop();
    const bool  wasModular = g_modular.load();
    const double oldTempo  = audio_app_tempo();

    audio_app_set_tempo( 500.0 );

    int outId = audio_app_patch_add_midi_out( -1 );
    if ( outId < 0 ) return -1.0;
    patch::MidiOutNode* mo =
        dynamic_cast<patch::MidiOutNode*>( g_patch->node( (patch::NodeId)outId ) );
    if ( !mo ) { audio_app_patch_remove( outId ); return -1.0; }
    audio_app_patch_connect( (int)g_patchMidiInId, 0, outId, 0 );
    audio_app_set_modular( true );

    const long long kLoopTicks = 768;                      // one 4/4 bar
    const int       kPasses    = 4;
    audio_app_set_loop_ticks( 0, kLoopTicks, 1 );
    audio_app_transport_locate( 0 );
    audio_app_patch_set_playing( true );

    const double spt = (double)g_sr * 60.0 / ( 500.0 * 192.0 );
    const long long loopLenSamples = (long long) llround( (double)kLoopTicks * spt );

    // scheduler sim state (mirrors perform's audio-paced loop)
    long long lastSched = -1;
    int       passes    = 0;
    bool      tailDone  = false;
    auto enqueue_range = [&]( long long from, long long to ){   // ticks (from,to]
        // MONOTONIC by tick -- the ring's FIFO==due-order contract.
        const long long t0 = ( from < 0 ) ? 0 : ( from / 24 + 1 ) * 24;
        for ( long long t = t0; t <= to; t += 24 )
        {
            audio_app_route_midi( 0, 0x90, 60, 100, t );
            audio_app_route_midi( 0, 0x80, 60, 0,   t + 12 );
        }
    };

    std::vector<long long> dues;
    std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
    float* out[2] = { L.data(), R.data() };
    long long prevPos = -1;
    const long long lookahead = 2 * (long long) g_block
                              + (long long)( 0.005 * g_sr );

    for ( int guard = 0; guard < 4000 && passes < kPasses; ++guard )
    {
        const long long pos = g_transport.sample();
        if ( pos < prevPos ) { ++passes; }               // engine wrapped
        prevPos = pos;
        const long long horizon = audio_app_sample_to_tick( pos + lookahead );
        if ( horizon >= kLoopTicks )
        {
            if ( !tailDone )
            {
                long long leftover = horizon - kLoopTicks;
                if ( leftover > kLoopTicks ) leftover = 0;
                enqueue_range( lastSched, kLoopTicks - 1 );
                enqueue_range( -1, leftover );           // next pass head
                lastSched = leftover;
                tailDone  = true;
            }
        }
        else
        {
            tailDone = false;
            if ( horizon > lastSched )
            { enqueue_range( lastSched, horizon ); lastSched = horizon; }
        }
        audio_render( out, 2, g_block, g_sr );
        patch::TimedMidi tm;
        while ( mo->pop( tm ) )
            if ( ( tm.ev.status & 0xF0 ) == 0x90 && tm.ev.data2 > 0 )
                dues.push_back( tm.dueSample );
    }

    // restore
    audio_app_patch_set_playing( false );
    audio_app_set_loop_ticks( 0, 0, 0 );
    audio_app_patch_disconnect( (int)g_patchMidiInId, 0, outId, 0 );
    audio_app_patch_remove( outId );
    audio_app_set_modular( wasModular );
    audio_app_set_tempo( oldTempo );
    audio_app_transport_locate( 0 );
    if ( wasRunning ) g_engine->start();

    // verify: kPasses complete passes of 32 onsets, each at exactly k*720.
    const long long ideal32 = (long long) llround( 24.0 * spt );   // 720
    const int perPass = (int)( kLoopTicks / 24 );                  // 32
    double maxDev = 0.0; int bad = 0;
    const int expect = perPass * kPasses;
    for ( int i = 0; i < (int)dues.size() && i < expect; ++i )
    {
        const long long want = (long long)( i % perPass ) * ideal32;
        const double dev = fabs( (double)( dues[i] - want ) );
        if ( dev > maxDev ) maxDev = dev;
        if ( dev > 0.5 && ++bad <= 4 )
            fprintf( stderr, "[loop-selftest]   mismatch i=%d due=%lld want=%lld\n",
                     i, dues[i], want );
    }
    const bool countOk = ( (int)dues.size() >= expect );
    fprintf( stderr, "[loop-selftest] %d onsets over %d passes (want >=%d), "
             "loop %lld samples, max due deviation %.1f, misplaced %d\n",
             (int)dues.size(), passes, expect, loopLenSamples, maxDev, bad );
    fflush( stderr );
    if ( !countOk ) return 1e9;
    return maxDev;
}

}} // namespace PatchKnob::app
