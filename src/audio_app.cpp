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
#include "engine/rack/rack_factory.h"
#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/audio_clip_player.h"
#include "engine/sampler/sampler_instrument.h" // native keyzone sampler
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
#include <fstream>
#include <sstream>
#include <filesystem>
#include <set>
#include <map>
#include <queue>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>    // SetThreadPriority + waitable timers for the MIDI drain
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

using namespace PatchKnob::engine;

extern const unsigned char g_click_wav[];
extern const std::size_t g_click_wav_size;

namespace {
// DERIVED, never a literal.  This was a hardcoded 192 sitting alongside
// globals.h's c_ppqn with nothing tying them together, so raising one silently
// left the other behind -- and this copy is the one the tick<->sample math on
// the audio thread runs on.
// Taken from the tempo map's own header rather than restated, so the sequencer
// tick unit has exactly ONE definition.  globals.h is deliberately NOT included
// here: it carries `using namespace std;`, which collides with the Windows SDK's
// `byte` via RtMidi.  The c_ppqn == seq_ppqn equality is asserted in midibus.cpp,
// where both names are already in scope.
constexpr long long kSequencerPpqn = ::kitchensink::seq_ppqn;

AudioEngine* g_engine  = nullptr;
MixerGraph*  g_graph   = nullptr;
PluginHost*  g_host    = nullptr;
bool         g_running = false;
double       g_sr      = 48000.0;
int          g_block   = 512;
static unsigned g_inputDev = 0;

struct AudioPreferences {
    std::string backendName, outputName, inputName;
    int backendId=-1;
    unsigned outputId=0, inputId=0, buffer=512, sampleRate=48000, channels=2;
};
AudioPreferences g_audioPrefs;
std::string g_audioPrefsPath;

static std::string audio_preferences_path()
{
    if(!g_audioPrefsPath.empty()) return g_audioPrefsPath;
#ifdef _WIN32
    const char* base=std::getenv("APPDATA");
    std::string dir=(base&&*base)?std::string(base)+"\\PatchKnob":".";
    std::error_code ec; std::filesystem::create_directories(dir,ec);
    return dir+"\\audio_preferences.txt";
#else
    const char* base=std::getenv("HOME");
    std::string dir=(base&&*base)?std::string(base)+"/.config/patchknob":".";
    std::error_code ec; std::filesystem::create_directories(dir,ec);
    return dir+"/audio_preferences.txt";
#endif
}

static void load_audio_preferences()
{
    g_audioPrefsPath=audio_preferences_path();
    std::ifstream f(g_audioPrefsPath);
    std::string line;
    while(std::getline(f,line)) {
        const size_t eq=line.find('='); if(eq==std::string::npos) continue;
        const std::string k=line.substr(0,eq),v=line.substr(eq+1);
        try {
            if(k=="backend_name") g_audioPrefs.backendName=v;
            else if(k=="backend_id") g_audioPrefs.backendId=std::stoi(v);
            else if(k=="output_name") g_audioPrefs.outputName=v;
            else if(k=="output_id") g_audioPrefs.outputId=(unsigned)std::stoul(v);
            else if(k=="input_name") g_audioPrefs.inputName=v;
            else if(k=="input_id") g_audioPrefs.inputId=(unsigned)std::stoul(v);
            else if(k=="buffer_frames") g_audioPrefs.buffer=(unsigned)std::stoul(v);
            else if(k=="sample_rate") g_audioPrefs.sampleRate=(unsigned)std::stoul(v);
            else if(k=="channels") g_audioPrefs.channels=(unsigned)std::stoul(v);
        } catch(...) {}
    }
}

static void save_audio_preferences()
{
    if(!g_engine) return;
    g_audioPrefsPath=audio_preferences_path();
    g_audioPrefs.backendId=g_engine->selectedHostApi();
    g_audioPrefs.outputId=g_engine->selectedDeviceId();
    g_audioPrefs.inputId=g_inputDev;
    g_audioPrefs.buffer=g_engine->blockSize();
    g_audioPrefs.sampleRate=g_engine->sampleRate();
    g_audioPrefs.channels=g_engine->numChannels();
    for(const auto& h:g_engine->enumerateHostApis())
        if(h.index==g_audioPrefs.backendId) g_audioPrefs.backendName=h.name;
    for(const auto& d:g_engine->enumerateOutputDevices())
        if(d.id==g_audioPrefs.outputId) g_audioPrefs.outputName=d.name;
    for(const auto& d:g_engine->enumerateInputDevices())
        if(d.id==g_audioPrefs.inputId) g_audioPrefs.inputName=d.name;
    std::ofstream f(g_audioPrefsPath,std::ios::trunc);
    if(!f) return;
    f<<"# PatchKnob audio preferences\n"
     <<"backend_name="<<g_audioPrefs.backendName<<"\nbackend_id="<<g_audioPrefs.backendId
     <<"\noutput_name="<<g_audioPrefs.outputName<<"\noutput_id="<<g_audioPrefs.outputId
     <<"\ninput_name="<<g_audioPrefs.inputName<<"\ninput_id="<<g_audioPrefs.inputId
     <<"\nbuffer_frames="<<g_audioPrefs.buffer<<"\nsample_rate="<<g_audioPrefs.sampleRate
     <<"\nchannels="<<g_audioPrefs.channels<<"\n";
}

std::vector<IPluginInstance*> g_owned;   // instruments/fx we must release
struct ProjectAudioClipEntry
{
    int track = -1;
    std::shared_ptr<AudioClip> clip;
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
patch::NodeId       g_defaultHwMidiInId = 0;
patch::NodeId       g_virtualMidiId = 0;
patch::NodeId       g_masterMixerId = 0;    // 8-bus + master summing mixer -> Out
int                 g_nextMasterCh  = 0;    // next free master-mixer channel (0..7)
patch::MidiInNode*  g_midiInNode    = nullptr;
patch::MidiInNode*  g_defaultHwMidiInNode = nullptr;
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
std::atomic<bool>      g_metronomeEnabled{false};
std::atomic<bool>      g_countinActive{false};
std::atomic<bool>      g_countinFinished{false};
long long              g_countinOrigin = 0;
long long              g_countinEnd = 0;
std::vector<float>     g_clickMono;
double                 g_clickSourceRate = 48000.0;
double                 g_clickPos = 0.0;
double                 g_clickStep = 1.0;
bool                   g_clickActive = false;
bool                   g_clickAccent = false;
std::atomic<long long> g_lastClickDue{-1};
std::atomic<bool>      g_clickReset{false};
std::mutex             g_tmapEditMx;           // serializes map WRITERS (message threads)
struct RetiredMap { const kitchensink::TempoMap* map; unsigned gen; };
std::vector<RetiredMap> g_tmapRetired;         // guarded by g_tmapEditMx

static unsigned rd16(const unsigned char* p) { return unsigned(p[0]) | (unsigned(p[1]) << 8); }
static unsigned rd32(const unsigned char* p) { return rd16(p) | (rd16(p + 2) << 16); }

static void load_embedded_click()
{
    const unsigned char* p=g_click_wav; const size_t n=g_click_wav_size;
    if(n<44 || std::memcmp(p,"RIFF",4) || std::memcmp(p+8,"WAVE",4)) return;
    unsigned channels=0,bits=0,rate=48000; const unsigned char* data=nullptr; size_t bytes=0;
    for(size_t at=12;at+8<=n;) {
        const unsigned sz=rd32(p+at+4); const unsigned char* q=p+at+8;
        if(at+8+sz>n) break;
        if(!std::memcmp(p+at,"fmt ",4) && sz>=16) {
            channels=rd16(q+2); rate=rd32(q+4); bits=rd16(q+14);
        } else if(!std::memcmp(p+at,"data",4)) { data=q; bytes=sz; }
        at += 8 + sz + (sz & 1u);
    }
    if(!data || !channels || bits!=16) return;
    const size_t frames=bytes/(channels*2u); g_clickMono.resize(frames);
    for(size_t i=0;i<frames;++i) {
        float sum=0.f;
        for(unsigned c=0;c<channels;++c) {
            const unsigned char* s=data+(i*channels+c)*2u;
            sum += (float)(int16_t)rd16(s) / 32768.f;
        }
        g_clickMono[i]=sum/(float)channels;
    }
    g_clickSourceRate=(double)rate;
}

static void mix_metronome(float** out, int channels, int nframes,
                          long long blockStart, const kitchensink::TempoMap* map)
{
    if(g_clickReset.exchange(false,std::memory_order_acq_rel)) {
        g_clickActive=false;
        g_clickPos=0.0;
    }
    const bool countin=g_countinActive.load(std::memory_order_relaxed);
    const bool running=g_transport.rolling();
    if((!countin && (!running || !g_metronomeEnabled.load(std::memory_order_relaxed))) ||
       g_clickMono.empty()) {
        // A stopped transport stays on one beat forever.  Never repeatedly
        // retrigger that boundary, which turns a click sample into a buzz.
        if(!running) g_clickActive=false;
        return;
    }
    const long long blockEnd=blockStart+nframes;
    long long tick=map->sample_to_tick(blockStart);
    const long long beatTicks=kSequencerPpqn;
    long long beat=(tick/beatTicks)*beatTicks;
    if(map->tick_to_sample(beat)<blockStart) beat+=beatTicks;
    for(;map->tick_to_sample(beat)<blockEnd;beat+=beatTicks) {
        const long long due=map->tick_to_sample(beat);
        const long long beatNo=beat/beatTicks;
        const bool accent=(beatNo%4)==0;
        // If a click boundary occurs within this block, render up to it first,
        // then restart the embedded sample exactly on the boundary.
        for(int i=0;i<nframes;++i) {
            if(blockStart+i==due &&
               due!=g_lastClickDue.load(std::memory_order_relaxed)) {
                g_lastClickDue.store(due,std::memory_order_relaxed);
                g_clickPos=0.0;
                // Keep the source envelope at normal duration.  The accent's
                // octave component is layered below instead of doubling this
                // step (2x playback made beat one sound cut in half).
                g_clickStep=g_clickSourceRate/g_sr;
                g_clickAccent=accent;
                g_clickActive=true;
            }
            if(!g_clickActive) continue;
            const size_t si=(size_t)g_clickPos;
            if(si>=g_clickMono.size()) { g_clickActive=false; continue; }
            const size_t sj=std::min(si+1,g_clickMono.size()-1);
            const float f=(float)(g_clickPos-(double)si);
            const float base=g_clickMono[si]+(g_clickMono[sj]-g_clickMono[si])*f;
            float v=base;
            if(g_clickAccent) {
                const double octavePos=g_clickPos*2.0;
                const size_t oi=(size_t)octavePos;
                if(oi<g_clickMono.size()) {
                    const size_t oj=std::min(oi+1,g_clickMono.size()-1);
                    const float of=(float)(octavePos-(double)oi);
                    const float octave=g_clickMono[oi]+(g_clickMono[oj]-g_clickMono[oi])*of;
                    v=base*0.35f+octave*0.75f;
                } else v=base*0.35f;
            }
            v*=0.7f;
            for(int c=0;c<channels;++c) out[c][i]+=v;
            g_clickPos+=g_clickStep;
        }
        // Only one beat normally falls in a block; avoid rendering the block
        // twice at extreme tempos.
        return;
    }
    for(int i=0;i<nframes && g_clickActive;++i) {
        const size_t si=(size_t)g_clickPos;
        if(si>=g_clickMono.size()) { g_clickActive=false; break; }
        const size_t sj=std::min(si+1,g_clickMono.size()-1);
        const float f=(float)(g_clickPos-(double)si);
        const float base=g_clickMono[si]+(g_clickMono[sj]-g_clickMono[si])*f;
        float v=base;
        if(g_clickAccent) {
            const double octavePos=g_clickPos*2.0;
            const size_t oi=(size_t)octavePos;
            if(oi<g_clickMono.size()) {
                const size_t oj=std::min(oi+1,g_clickMono.size()-1);
                const float of=(float)(octavePos-(double)oi);
                const float octave=g_clickMono[oi]+(g_clickMono[oj]-g_clickMono[oi])*of;
                v=base*0.35f+octave*0.75f;
            } else v=base*0.35f;
        }
        v*=0.7f;
        for(int c=0;c<channels;++c) out[c][i]+=v;
        g_clickPos+=g_clickStep;
    }
}

static void finish_countin_if_due()
{
    if(g_countinActive.load(std::memory_order_relaxed) &&
       g_transport.sample() >= g_countinEnd) {
        g_countinActive.store(false,std::memory_order_release);
        g_countinFinished.store(true,std::memory_order_release);
    }
}

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
// Events/params rejected because their track index was outside
// [0, AUDIO_APP_MAX_TRACKS).  Formerly these WRAPPED into an 8-bit field (track
// -1 became 255 == the "no such plug" sentinel) and vanished without a trace.
std::atomic<unsigned long long> g_midiBadTrackCount{ 0 };

//  DISPATCH CENSUS.  The sequencer's emit tap shows a PERFECT pass while the
//  audio drops out, so the loss is downstream of it: somewhere between the ring
//  and the instrument.  These count every fate an event can meet on that
//  journey, so a bad pass can be attributed instead of guessed at.
std::atomic<unsigned long long> g_mcEnq{ 0 };        // accepted into the ring
std::atomic<unsigned long long> g_mcRingFull{ 0 };   // refused: ring full / headroom
std::atomic<unsigned long long> g_mcDelivered{ 0 };  // pushed to an instrument
std::atomic<unsigned long long> g_mcStaleEpoch{ 0 }; // discarded: pre-locate
std::atomic<unsigned long long> g_mcUnreachDrop{ 0 };// dropped: moment unreachable
std::atomic<unsigned long long> g_mcNodeFull{ 0 };   // MidiInNode ring refused it
std::atomic<unsigned long long> g_mcNoNode{ 0 };     // NO MidiIn node: silently eaten
//  The three EARLY-EXIT paths.  These do not drop an event -- they stop the
//  drain, leaving everything behind them in the ring for a later block.  If one
//  of them fires every block for a whole pass, that pass is silent even though
//  nothing was "dropped" and every drop counter reads zero.
std::atomic<unsigned long long> g_mcBrkEpoch{ 0 };   // "newer epoch: next block"
std::atomic<unsigned long long> g_mcBrkFuture{ 0 };  // due >= blockEnd
std::atomic<unsigned long long> g_mcBrkWrapped{ 0 };
std::atomic<unsigned long long> g_mcPreWrap{ 0 };    // retired: belonged to the old pass
//  Where the ring stood when the transport last wrapped.  Entries older than
//  this were scheduled for the pass that just ended.
std::atomic<unsigned>           g_ringWrapHead{ 0 };
std::atomic<bool>               g_ringWrapValid{ false };
std::atomic<bool>               g_midiBadTrackWarned{ false };

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
// Per-track INPUT capture slots (the punch modes record several tracks at
// once).  Each mirrors one MixerNode tap: the buffer we own, its capacity in
// frames, and the mixer track it is recording (-1 = slot free).  Message
// thread only.
struct TrackCapSlot { float* buf = nullptr; size_t cap = 0; int track = -1; };
TrackCapSlot          g_trackCaps[8];   // == MixerNode::kMaxCaptureTaps

// --- sample audition (browser click-to-preview) -----------------------------
// RCU, not a mutex.  render_preview_clip() runs on EVERY audio callback, and it
// used to take g_previewMutex there while audio_app_preview_clip() held the same
// mutex on the UI thread deep-copying an entire AudioClip -- so auditioning a
// long sample during playback blocked the audio thread on its next callback and
// dropped out the whole mix.  It also destroyed the clip ON the audio thread
// when playback ran off the end (a free() in the callback).
//
// Now the UI builds the copy off to the side, publishes the pointer with one
// release store, and only then waits the standard two-block grace before freeing
// the clip it displaced -- the identical pattern capture_tap()/g_capBuf use.
// The audio thread does one acquire load and never allocates, frees or blocks.
std::atomic<AudioClip*>    g_previewClip{nullptr};
std::atomic<float>         g_previewGain{1.0f};
// Bumped by the message thread before each publish; the audio thread restarts
// its read cursor whenever it sees a value it has not played yet.  (An epoch
// rather than pointer identity: a fresh clip can land on a recycled address.)
std::atomic<unsigned>      g_previewEpoch{0};
long long                  g_previewPos = 0;          // AUDIO THREAD ONLY
unsigned                   g_previewSeen = 0;         // AUDIO THREAD ONLY

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
    const AudioClip* clip = g_previewClip.load( std::memory_order_acquire );
    const unsigned epoch = g_previewEpoch.load( std::memory_order_relaxed );
    // A freshly published clip starts from its head.  The epoch was stored
    // before the release-exchange that published `clip`, so the acquire above
    // guarantees this read belongs to that same publish.
    if ( epoch != g_previewSeen ) { g_previewSeen = epoch; g_previewPos = 0; }
    if ( !clip || clip->empty() ) return;

    const long long frames = clip->numFrames();
    if ( g_previewPos >= frames )
        return;                       // finished; the message thread reclaims it

    const float* srcL = clip->ch[0].empty() ? nullptr : clip->ch[0].data();
    const float* srcR = clip->ch[1].empty() ? srcL : clip->ch[1].data();
    if ( !srcL ) return;
    const float previewGain = g_previewGain.load( std::memory_order_relaxed );

    float* dstL = out[0];
    float* dstR = ( numChannels > 1 && out[1] ) ? out[1] : out[0];
    int n = (int)std::min<long long>( nframes, frames - g_previewPos );
    const size_t start = (size_t)g_previewPos;
    for ( int i = 0; i < n; ++i )
    {
        float l = srcL[start + (size_t)i] * previewGain;
        float r = srcR[start + (size_t)i] * previewGain;
        dstL[i] += l;
        dstR[i] += r;
    }
    g_previewPos += n;
    // No reset()/free() here: destroying the clip on the audio thread was a
    // deallocation inside the callback.  The pointer simply stays published,
    // g_previewPos parks at the end, and the next audition (or shutdown)
    // reclaims it on the message thread.
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
                  unsigned char track, status, d1, d2;
                  // Tracker note-column (VOICE) the event belongs to, -1 = untagged.
                  // Carried per-event because a column CANNOT be recovered later:
                  // the sampler used to look it up in a 128-entry pitch-keyed table,
                  // so the same pitch played from two columns collapsed onto one.
                  signed char column; };
// `track` is 8 bits and downstream 0xFF is MidiInNode's "no such plug" sentinel,
// so every valid track index must fit strictly below it (audio_app_route_midi
// rejects anything outside [0, AUDIO_APP_MAX_TRACKS) rather than wrapping).
static_assert( PatchKnob::app::AUDIO_APP_MAX_TRACKS > 0 &&
               PatchKnob::app::AUDIO_APP_MAX_TRACKS < 255,
               "RouteMsg::track must hold every track index below the 0xFF sentinel" );
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
std::atomic<bool>      g_recArm{false};
std::atomic<long long> g_recStartTick{0};

// --- schedule epoch: bumped on every transport discontinuity (locate) --------
// Producers stamp each TICKED message; the drains discard messages from an
// older epoch, so events scheduled ahead of a seek can never fire displaced at
// the new position.  tick<0 ("now") messages are epoch-exempt.  The MidiIn
// node's already-pushed lookahead is flushed by the audio thread on the first
// block after the bump (g_flushMidiIn).
std::atomic<unsigned>  g_schedEpoch{0};
std::atomic<bool>      g_flushMidiIn{false};

// --- held-note tracker (AUDIO THREAD ONLY) ----------------------------------
// Every note the two ring drains below actually deliver is folded in here, so a
// transport discontinuity can RELEASE what is sounding instead of stranding it.
// MidiInNode::flush() (and the epoch bump) only discard QUEUED events -- the
// note-ons already handed to the graph stay ON forever, on soft instruments and
// on external hardware alike (hardware sees exactly the stream the graph emits,
// so its note-offs have to travel the same path).
//
// Written and read only from audio_render_segment().  That is a single logical
// thread: normally the stream callback, and for the offline pumps (freeze /
// clock selftest) the message thread AFTER g_engine->stop() has joined the
// stream -- never both at once -- so no synchronization is needed.
struct HeldNotes {
    unsigned       note[PatchKnob::app::AUDIO_APP_MAX_TRACKS][16][4]; // 128-note bitmap
    unsigned short chMask[PatchKnob::app::AUDIO_APP_MAX_TRACKS];      // channels with a note on
    unsigned       trackMask;                                         // tracks with a note on
};
HeldNotes s_held{};
static_assert( PatchKnob::app::AUDIO_APP_MAX_TRACKS <= 32,
               "HeldNotes::trackMask is a 32-bit track bitmap" );

// Audio thread: fold one delivered channel message into the tracker.
inline void held_note_track( int track, unsigned char status,
                             unsigned char d1, unsigned char d2 )
{
    if ( track < 0 || track >= PatchKnob::app::AUDIO_APP_MAX_TRACKS ) return;
    const unsigned hi = (unsigned)( status & 0xF0 );
    if ( hi != 0x90u && hi != 0x80u ) return;
    const int      ch   = status & 0x0F;
    const int      n    = d1 & 0x7F;
    const unsigned bit  = 1u << ( n & 31 );
    unsigned&      word = s_held.note[track][ch][n >> 5];
    if ( hi == 0x90u && d2 != 0 ) {                 // note ON (vel > 0)
        word |= bit;
        s_held.chMask[track] |= (unsigned short)( 1u << ch );
        s_held.trackMask     |= ( 1u << track );
    }
    else word &= ~bit;                              // note OFF / vel-0 note-on
}

// Audio thread: emit a release for every note the tracker says is sounding, then
// forget them all.  `emit(track, status, d1, d2)` is the per-render-mode sink
// (the patch MidiIn node in modular mode, the per-track stage in fixed-graph
// mode) so the offs go out on exactly the path the ons took.  Each active
// (track, channel) pair also gets CC123 (all notes off) + CC120 (all sound off)
// -- the same belt-and-braces the offline freeze drain uses -- for instruments
// that swallow individual note-offs.  Bounded work, no allocation, no locks;
// runs only on a discontinuity.
template <class Emit>
inline void held_notes_release_all( Emit emit )
{
    const unsigned tracks = s_held.trackMask;
    s_held.trackMask = 0;
    if ( !tracks ) return;
    for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t )
    {
        if ( !( tracks & ( 1u << t ) ) ) continue;
        const unsigned chans = s_held.chMask[t];
        s_held.chMask[t] = 0;
        for ( int ch = 0; ch < 16; ++ch )
        {
            if ( !( chans & ( 1u << ch ) ) ) continue;
            for ( int w = 0; w < 4; ++w )
            {
                const unsigned bits = s_held.note[t][ch][w];
                s_held.note[t][ch][w] = 0;
                if ( !bits ) continue;
                for ( int b = 0; b < 32; ++b )
                    if ( bits & ( 1u << b ) )
                        emit( t, (unsigned char)( 0x80 | ch ),
                              (unsigned char)( w * 32 + b ), (unsigned char)0 );
            }
            emit( t, (unsigned char)( 0xB0 | ch ), (unsigned char)123, (unsigned char)0 );
            emit( t, (unsigned char)( 0xB0 | ch ), (unsigned char)120, (unsigned char)0 );
        }
    }
}

// --- engine-side loop (sample-accurate, applied by cycle-splitting) ----------
// The scheduler publishes the loop as TICKS; samples are (re)derived from the
// ACTIVE tempo map here and whenever the map is republished, so a tempo change
// moves the loop points with the music.
const int               kMaxDeviceCh = 16;   // cycle-split pointer scratch bound
std::atomic<bool>       g_loopOn{false};
std::atomic<long long>  g_loopStartSample{0};
std::atomic<long long>  g_loopEndSample{0};
std::atomic<unsigned>   g_loopConfigGeneration{0};
std::atomic<bool>       g_loopBoundaryRelease{false};
std::atomic<unsigned long long> g_loopWrapGeneration{0};
// Monotonic count of transport LOCATES, the exact counterpart of
// g_loopWrapGeneration above.  Wraps were moved onto a generation counter
// because polling for a position decrease aliases away whenever the scheduler
// stalls across the event; locates were left on that same broken inference and
// so inherit the identical bug -- plus two only they can hit: a FORWARD locate
// never decreases the sample at all, and a locate landing in the same poll as a
// wrap was deliberately suppressed.  A missed locate strands the scheduler's
// last_scheduled watermark ahead of the horizon, and it emits nothing at all
// until the transport is stopped and restarted.
std::atomic<unsigned long long> g_locateGeneration{0};
// The scheduler's current lookahead, in samples, published every poll.  The
// drain needs it to tell a NEXT-PASS event from a merely LATE one on short
// loops -- see the wrap discriminator in audio_render_segment().
std::atomic<long long> g_schedLookahead{0};
std::atomic<unsigned>   g_loopOff[PatchKnob::app::AUDIO_APP_MAX_TRACKS][16][4];
long long               g_loopLeftTick  = 0;   // message thread only
long long               g_loopRightTick = 0;
// Harness-only output mute (see audio_app_set_silent_output).  Everything is
// still rendered; only the bytes handed to the device are zeroed.
std::atomic<bool>       g_silentOut{false};

template<class Emit> void emit_loop_boundary_offs(Emit emit)
{
    for(int t=0;t<PatchKnob::app::AUDIO_APP_MAX_TRACKS;++t)for(int ch=0;ch<16;++ch)
        for(int w=0;w<4;++w){
            const unsigned bits=g_loopOff[t][ch][w].exchange(0,std::memory_order_acq_rel);
            for(int b=0;b<32;++b)if(bits&(1u<<b)){
                const unsigned char st=(unsigned char)(0x80|ch);
                const unsigned char note=(unsigned char)(w*32+b);
                emit(t,st,note,(unsigned char)0);
                held_note_track(t,st,note,0);
            }
        }
}

// FLUSH, never discard.  These bits are the ONLY remaining record that those
// notes are sounding: sequence::queue_loop_note_offs() zeroes m_playing_notes[]
// in the same pass that sets them (src/sequence.cpp), and this path explicitly
// does not release held notes.  Zeroing them therefore did not "cancel a
// pending release" -- it destroyed the note-off outright and hung the note
// until the next transport Stop ran the panic path.
//
// Arming g_loopBoundaryRelease instead makes the audio thread emit whatever is
// pending on its next segment.  A note released a few milliseconds early beats
// one that never releases at all, and emit_loop_boundary_offs() exchanges each
// word to zero, so anything already consumed is not emitted twice.
void clear_loop_boundary_offs()
{
    bool pending = false;
    for(int t=0;t<PatchKnob::app::AUDIO_APP_MAX_TRACKS && !pending;++t)
        for(int ch=0;ch<16 && !pending;++ch)
            for(int w=0;w<4;++w)
                if(g_loopOff[t][ch][w].load(std::memory_order_acquire)){ pending=true; break; }
    // Nothing outstanding: clear the arm flag as before so a stale request
    // from a wrap that never happened does not linger.
    g_loopBoundaryRelease.store(pending,std::memory_order_release);
}

bool loop_config_snapshot(long long& start,long long& end)
{
    for(int tries=0;tries<8;++tries){
        const unsigned a=g_loopConfigGeneration.load(std::memory_order_acquire);
        if(a&1u)continue;
        const bool on=g_loopOn.load(std::memory_order_relaxed);
        start=g_loopStartSample.load(std::memory_order_relaxed);
        end=g_loopEndSample.load(std::memory_order_relaxed);
        const unsigned b=g_loopConfigGeneration.load(std::memory_order_acquire);
        if(a==b)return on&&end>start;
    }
    return false;
}

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
// Renders ONE contiguous segment of the timeline (a whole device block, or a
// slice of one when the block is cycle-split at the loop boundary -- see the
// audio_render wrapper below).
void audio_render_segment( const float* const* in, int numInputChannels,
                           float** out, int numChannels, int nframes, double /*sr*/ )
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
    long long loopStartSnapshot=0,loopEndSnapshot=0;
    const bool loopSnapshot=loop_config_snapshot(loopStartSnapshot,loopEndSnapshot);
    const bool      loopHold = loopSnapshot && g_transport.rolling();
    // Discriminate a NEXT-PASS event (queued before the engine has wrapped, so
    // it sits nearly a whole loop behind the playhead) from a merely LATE one
    // (a few blocks behind).
    //
    // Half the loop is the correct midpoint only while the lookahead is small.
    // The tail branch queues the next pass's head ONE LOOKAHEAD BEFORE the loop
    // end, so those head events sit (loopLen - lookahead) behind the playhead.
    // Once the lookahead exceeds half the loop -- any loop shorter than about
    // twice the lookahead, i.e. a few tens of ms -- that distance drops BELOW
    // half, so the head events were misread as "late", drained into the pass
    // that was still playing (clamped to offset 0) and then cut down by the
    // boundary note-offs.  The next pass played nothing.
    //
    // Take the midpoint between "a couple of blocks late" and where the head
    // events actually sit.  With a small lookahead this reduces to loopLen/2,
    // which is exactly the previous behaviour.
    const long long loopLen = loopHold
        ? ( loopEndSnapshot - loopStartSnapshot ) : 0;
    const long long wrapMark = loopHold
        ? std::max( 2LL * (long long) nframes, ( loopLen - g_schedLookahead.load(
                        std::memory_order_relaxed ) ) / 2 )
        : 0;
    auto is_wrapped_future = [&]( long long due ) {
        return loopHold && due < blockStart && ( blockStart - due ) > wrapMark;
    };

    //  LEFTOVERS FROM THE PASS WE JUST LEFT.
    //
    //  The scheduler runs a lookahead, so in the last few ms of a pass it
    //  enqueues events for ticks near the LOOP END.  The transport then wraps
    //  to the start, and those entries are suddenly a whole pass in the future
    //  -- they are NOT `is_unreachable` (their due is below loopEnd) and NOT
    //  `is_wrapped_future` (their due is ahead of us, not behind), so they hit
    //  the plain `due >= blockEnd` hold-back and STOP THE DRAIN.  The ring is
    //  FIFO, so every event of the new pass sits behind them and cannot be
    //  delivered until the playhead reaches the old due time near the end of
    //  the new pass.  That is one silent pass followed by a flush -- measured
    //  on the user's project as enq=160 deliv=5 with every drop counter at
    //  zero, because nothing was dropped: the queue was simply blocked.
    //
    //  perform re-emits the new pass's content after a wrap, so those
    //  leftovers are duplicates. Retire them at the wrap: a release still goes
    //  out (its note-on sounded before the wrap and must not hang), anything
    //  else is dropped.  `g_ringWrapHead` marks where the ring stood when the
    //  wrap happened, so only entries enqueued BEFORE it are affected.
    const bool wrapMarkValid = g_ringWrapValid.load( std::memory_order_relaxed );
    const unsigned wrapHead  = g_ringWrapHead.load( std::memory_order_relaxed );
    auto is_pre_wrap_leftover = [&]( unsigned idx, long long due ) {
        return loopHold && wrapMarkValid && due >= blockEnd &&
               (int)( wrapHead - idx ) > 0;      // enqueued before the wrap
    };

    // SAFETY VALVE for the two holdbacks below.
    //
    // Both are FIFO `break`s: they stop the drain WITHOUT advancing the tail,
    // so a single entry that never becomes due blocks every entry behind it,
    // for every track, permanently.  Nothing recovers that except a
    // schedule-epoch bump -- i.e. Stop then Play -- which is exactly the
    // reported "you have to stop and start the song again".  MidiInNode has
    // carried a valve for this same failure for a while ("a backlog must never
    // wedge the queue permanently"); this ring never got one.
    //
    // Anything further out than a whole loop plus a generous lookahead margin
    // cannot be reached by normal transport motion, so drop it rather than
    // hold it.  Legitimately-future and genuinely-wrapped entries are well
    // inside the margin and still hold the line, preserving FIFO order.
    const long long unreachableSpan =
        ( loopHold ? ( loopEndSnapshot - loopStartSnapshot ) : 0 )
        + (long long) ( ( g_sr > 0.0 ? g_sr : 48000.0 ) * 4.0 );
    auto is_unreachable = [&]( long long due ) {
        // While looping the transport provably visits only [loopStart,
        // loopEnd): every segment renders [blockStart, blockEnd) with
        // blockEnd <= loopEnd, so a due AT or PAST the loop end can never
        // come due, no matter how long playback runs.  Such an entry used to
        // sit at the FIFO head forever ("due >= blockEnd: hold back"),
        // silencing every event queued behind it -- on every track -- until a
        // Stop/locate bumped the epoch.  The distance margin below could not
        // catch it: a due in [loopEnd, loopEnd + margin) sat exactly inside
        // its blind spot, wedging the ring for good.
        if ( loopHold && due >= loopEndSnapshot ) return true;
        return ( due >= blockEnd ) ? ( due - blockEnd )   > unreachableSpan
                                   : ( blockStart - due ) > unreachableSpan;
    };

    // A locate/stop happened: the MidiIn node's scheduled-ahead lookahead
    // references the OLD position -- flush it before this block delivers
    // anything.  flush() only DISCARDS the queue, so on its own it strands every
    // note that was sounding (stuck-on notes on soft instruments AND on external
    // hardware).  The per-mode release pass below therefore sends the matching
    // note-offs FIRST, ahead of anything this block drains.
    const bool discontinuity =
        g_flushMidiIn.exchange( false, std::memory_order_acq_rel );
    const bool loopBoundary =
        g_loopBoundaryRelease.exchange(false,std::memory_order_acq_rel);
    if ( loopBoundary ) {
        //  Everything already in the ring was scheduled for the pass that just
        //  ended; mark the boundary so the drain can retire the stragglers.
        g_ringWrapHead.store( g_head.load( std::memory_order_acquire ),
                              std::memory_order_relaxed );
        g_ringWrapValid.store( true, std::memory_order_relaxed );
    }
    if ( discontinuity && g_midiInNode ) g_midiInNode->flush();

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

    const bool modularNow = g_modular.load( std::memory_order_relaxed ) && g_patch;

    // --- modular mode: render the patch graph instead of the fixed mixer ---
    if ( modularNow )
    {
        // Transport discontinuity: release everything still sounding BEFORE this
        // block's events, on the same plugs the note-ons went out on, so a
        // locate/stop can never leave a note hanging (bug B3).
        if ( loopBoundary && g_midiInNode )
            emit_loop_boundary_offs(
                [&](int trk,unsigned char st,unsigned char d1,unsigned char d2){
                    MidiEvent e;e.sampleOffset=0;e.status=st;e.data1=d1;e.data2=d2;
                    g_midiInNode->push(e,blockStart,trk);
                });
        else if(loopBoundary)
            emit_loop_boundary_offs([](int,unsigned char,unsigned char,unsigned char){});
        if ( discontinuity && g_midiInNode )
            held_notes_release_all(
                [&]( int trk, unsigned char st, unsigned char d1, unsigned char d2 ) {
                    MidiEvent e; e.sampleOffset = 0;
                    e.status = st; e.data1 = d1; e.data2 = d2;
                    g_midiInNode->push( e, blockStart, trk );
                } );
        else if ( discontinuity )
            held_notes_release_all( []( int, unsigned char, unsigned char,
                                        unsigned char ) {} );   // no sink: just forget

        // HOLDBACK drain of the MIDI ring into the patch's MidiIn node.  FIFO
        // order == tick order (single monotonic producer, see RouteMsg), so we
        // stop at the first event due beyond this block and leave it queued.
        //
        // SINGLE-PRODUCER INVARIANT (bug B1): MidiInNode's ring is strict SPSC.
        // THIS is the only place allowed to push into g_midiInNode -- everything
        // else (GUI preview, selftests, the sequencer) must go through
        // audio_app_route_midi(), whose g_midi_producer spinlock serializes the
        // producers into g_ring, which this audio thread alone forwards.
        unsigned tail = g_tail.load( std::memory_order_relaxed );
        const unsigned head = g_head.load( std::memory_order_acquire );
        while ( tail != head )
        {
            const RouteMsg& m = g_ring[tail & RING_MASK];
            // ticked events from BEFORE a locate reference the old position:
            // discard (epoch mismatch).  tick<0 previews are epoch-exempt.
            // Wrap-safe age test.  `!=` also destroyed messages from a NEWER
            // epoch -- i.e. exactly what a post-locate rebuild enqueues while
            // this block still holds the epoch it latched at its start.  Those
            // are not stale, they belong to the next block: hold them.
            if ( m.tick >= 0 && (int)( m.epoch - epoch ) < 0 ) {
                g_mcStaleEpoch.fetch_add( 1, std::memory_order_relaxed );
                ++tail; continue; }
            if ( m.tick >= 0 && m.epoch != epoch ) {
                g_mcBrkEpoch.fetch_add( 1, std::memory_order_relaxed );
                break; }   // newer: next block
            const long long due = ( m.tick < 0 ) ? blockStart
                                                 : map->tick_to_sample( m.tick );
            // Valve first: an entry that can never come due must not wedge
            // the FIFO behind it (see is_unreachable).  A RELEASE is still
            // let out at "now" -- its note-on may well have sounded (before a
            // wrap), and a note-off a few ms early beats one that never
            // arrives.  Non-releases whose moment is unreachable are dropped.
            if ( m.tick >= 0 && is_unreachable( due ) )
            {
                if ( midi_is_critical( m.status, m.d2 ) && g_midiInNode )
                {
                    MidiEvent e; e.sampleOffset = 0; e.status = m.status;
                    e.data1 = m.d1; e.data2 = m.d2; e.column = m.column;
                    if ( !g_midiInNode->push( e, blockStart, (int) m.track ) ) {
                        g_mcNodeFull.fetch_add( 1, std::memory_order_relaxed );
                        break;                     // full: retry next block
                    }
                    g_mcDelivered.fetch_add( 1, std::memory_order_relaxed );
                    held_note_track( (int) m.track, m.status, m.d1, m.d2 );
                }
                else g_mcUnreachDrop.fetch_add( 1, std::memory_order_relaxed );
                ++tail; continue;
            }
            if ( m.tick >= 0 && is_pre_wrap_leftover( tail, due ) ) {
                if ( midi_is_critical( m.status, m.d2 ) && g_midiInNode ) {
                    MidiEvent e; e.sampleOffset = 0; e.status = m.status;
                    e.data1 = m.d1; e.data2 = m.d2; e.column = m.column;
                    if ( !g_midiInNode->push( e, blockStart, (int) m.track ) )
                        break;                       // full: retry next block
                    held_note_track( (int) m.track, m.status, m.d1, m.d2 );
                }
                g_mcPreWrap.fetch_add( 1, std::memory_order_relaxed );
                ++tail; continue;
            }
            if ( due >= blockEnd ) {
                g_mcBrkFuture.fetch_add( 1, std::memory_order_relaxed );
                break; }                             // future block: hold back
            if ( m.tick >= 0 && is_wrapped_future( due ) ) {
                g_mcBrkWrapped.fetch_add( 1, std::memory_order_relaxed );
                break; }                             // next pass
            if ( g_midiInNode ) {
                MidiEvent e; e.sampleOffset = 0; e.status = m.status;
                e.data1 = m.d1; e.data2 = m.d2; e.column = m.column;
                // route to this track's "Instrument out" plug (track == bus).
                // m.track is validated at enqueue (audio_app_route_midi), so it
                // is always a real plug index here, never the 0xFF "drop it"
                // sentinel a wrapped negative track used to produce.
                //
                // push() can fail when the ring's headroom is tight (see
                // MidiInNode::push).  Unlike the epoch/future-block checks
                // above, this is NOT a reason to skip the event -- do not
                // advance tail, so the same event is retried next block
                // instead of being silently dropped.
                if ( !g_midiInNode->push( e, due, (int) m.track ) ) {
                    g_mcNodeFull.fetch_add( 1, std::memory_order_relaxed );
                    break;
                }
                g_mcDelivered.fetch_add( 1, std::memory_order_relaxed );
                held_note_track( (int) m.track, m.status, m.d1, m.d2 );
            }
            else g_mcNoNode.fetch_add( 1, std::memory_order_relaxed );
            ++tail;
        }
        if ( g_ringWrapValid.load( std::memory_order_relaxed ) &&
             (int)( g_ringWrapHead.load( std::memory_order_relaxed ) - tail ) <= 0 )
            g_ringWrapValid.store( false, std::memory_order_relaxed );
        g_tail.store( tail, std::memory_order_release );

        // Publish this block's device capture so AudioDeviceInNode has something
        // to read.  PatchGraph::stageDeviceInput() existed but nothing ever
        // called it, so an "Audio In" node in the patcher emitted pure silence
        // no matter how it was wired.
        RenderContext ctx;
        ctx.tempoBpm            = map->tempo_at_sample( blockStart );  // map-derived
        ctx.playPositionSamples = blockStart;               // block-START position
        ctx.isPlaying           = g_patchPlaying.load( std::memory_order_relaxed );
        g_patchPlayPos          = blockStart;               // keep legacy mirror in sync
        g_patch->stageDeviceInput( in, numInputChannels, nframes );
        g_patch->process( out, numChannels, nframes, ctx );
        render_preview_clip( out, numChannels, nframes );
        // Frozen/project audio lives on fixed-graph tracks that this modular path
        // does not render -- sound them here so freeze playback is audible.
        render_modular_clip_players( out, numChannels, nframes, blockStart );
        mix_metronome(out,numChannels,nframes,blockStart,map);
        capture_tap( out, numChannels, nframes );
        g_transport.advance( nframes );   // advance at END; the seek was applied at callback start
        finish_countin_if_due();
        return;
    }

    const int NT = PatchKnob::app::AUDIO_APP_MAX_TRACKS;
    for ( int t = 0; t < NT; ++t ) { s_counts[t] = 0; s_pcounts[t] = 0; }

    if(loopBoundary)
        emit_loop_boundary_offs(
            [&](int trk,unsigned char st,unsigned char d1,unsigned char d2){
                if(trk<0||trk>=NT||s_counts[trk]>=MAX_EV)return;
                MidiEvent& e=s_trackMidi[trk][s_counts[trk]++];
                e.sampleOffset=0;e.status=st;e.data1=d1;e.data2=d2;
                // column/captureSample are NOT set by the assignments above and
                // this array is REUSED every block, so without these two lines
                // the release inherits the previous event's voice tag.
                // SamplerInstrument::noteOff skips any voice whose column does
                // not match a tagged off, so a mistagged release is silently
                // dropped and the note hangs -- and the CC123/CC120 fallback
                // does not save it, because the sampler handles neither.
                e.column=-1;e.captureSample=-1;
            });

    // Same discontinuity contract as the modular path (bug B3): stage the
    // releases for every sounding note before anything this block drains, so a
    // locate/stop cannot strand a note on a fixed-graph instrument either.
    if ( discontinuity )
        held_notes_release_all(
            [&]( int trk, unsigned char st, unsigned char d1, unsigned char d2 ) {
                if ( trk < 0 || trk >= NT || s_counts[trk] >= MAX_EV ) return;
                MidiEvent& e = s_trackMidi[trk][ s_counts[trk]++ ];
                e.sampleOffset = 0; e.status = st; e.data1 = d1; e.data2 = d2;
                // column/captureSample are NOT set by the assignments above and
                // this array is REUSED every block, so without these two lines
                // the release inherits the previous event's voice tag.
                // SamplerInstrument::noteOff skips any voice whose column does
                // not match a tagged off, so a mistagged release is silently
                // dropped and the note hangs -- and the CC123/CC120 fallback
                // does not save it, because the sampler handles neither.
                e.column = -1; e.captureSample = -1;
            } );

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
        // See the modular drain: discard only strictly-older epochs.
        if ( m.tick >= 0 && (int)( m.epoch - epoch ) < 0 ) { ++tail; continue; }
        if ( m.tick >= 0 && m.epoch != epoch ) break;                  // newer: next block
        const long long due = ( m.tick < 0 ) ? blockStart
                                             : map->tick_to_sample( m.tick );
        // Valve first: an entry that can never come due must not wedge the
        // FIFO behind it (see is_unreachable).  Same release contract as the
        // modular drain above: a critical (note-off) still goes out at "now",
        // only non-releases are dropped.
        if ( m.tick >= 0 && is_unreachable( due ) )
        {
            const int vt = m.track;
            if ( midi_is_critical( m.status, m.d2 ) && vt >= 0 && vt < NT )
            {
                held_note_track( vt, m.status, m.d1, m.d2 );
                MidiEvent& e = ( s_counts[vt] < MAX_EV )
                    ? s_trackMidi[vt][ s_counts[vt]++ ]
                    : s_trackMidi[vt][ MAX_EV - 1 ];   // never lose a note-off
                e.sampleOffset = 0; e.status = m.status;
                e.data1 = m.d1; e.data2 = m.d2; e.column = m.column;
                e.captureSample = -1;
            }
            ++tail; continue;
        }
        if ( due >= blockEnd ) break;                // future block: hold back
        if ( m.tick >= 0 && is_wrapped_future( due ) ) break;         // next pass
        ++tail;
        int t = m.track;
        if ( t >= 0 && t < NT )
        {
            held_note_track( t, m.status, m.d1, m.d2 );
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
                e.column = m.column;
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
                e.column = m.column;
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
        // See the modular drain: discard only strictly-older epochs.
        if ( m.tick >= 0 && (int)( m.epoch - epoch ) < 0 ) { ++ptail; continue; }
        if ( m.tick >= 0 && m.epoch != epoch ) break;                  // newer: next block
        const long long due = ( m.tick < 0 ) ? blockStart
                                             : map->tick_to_sample( m.tick );
        // Valve first: an entry that can never come due must not wedge the
        // FIFO behind it (see is_unreachable).
        if ( m.tick >= 0 && is_unreachable( due ) ) { ++ptail; continue; }
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
    mix_metronome(out,numChannels,nframes,blockStart,map);
    capture_tap( out, numChannels, nframes );
    g_transport.advance( nframes );   // advance at END; the seek was applied at callback start
    finish_countin_if_due();
}

// The device callback: renders whole blocks, CYCLE-SPLITTING at the loop-end
// boundary so the playhead jumps to loopStart at the EXACT frame (not at a
// block edge, not ~a-lookahead early).  Every sub-segment goes through the
// full segment body above, so event dues, holdback, offsets and transport
// bookkeeping stay consistent inside the split.
void audio_render_device_inner( const float* const* in, int numInputChannels,
                                float** out, int numChannels, int nframes, double sr )
{
    // Consume any queued seek HERE, before blockStart or the split loop's
    // `pos` is read, so the very next window rendered starts AT the seek
    // target.  Applying it at the END of a block (which is what
    // process(nframes) did) left the playhead at S and then immediately
    // advanced it, so the window [S, S+nframes) was never rendered at all --
    // every locate silently skipped its first buffer, taking clip attacks and
    // any tick-0 event at a loop start with it.
    //
    // Exactly ONCE per device callback, and never inside the split loop: a
    // locate arriving mid-callback must not move the playhead between
    // sub-segments (the mid-block shear the queued-seek design exists to
    // prevent).
    g_transport.apply_pending_seek();

    long long lstart=0,lend=0;
    if ( !loop_config_snapshot(lstart,lend) || !g_transport.rolling() )
    {
        audio_render_segment( in, numInputChannels, out, numChannels, nframes, sr );
        return;
    }

    if ( lend <= lstart )                       // degenerate loop: ignore
    {
        audio_render_segment( in, numInputChannels, out, numChannels, nframes, sr );
        return;
    }

    float* seg[kMaxDeviceCh];
    const float* inSeg[kMaxDeviceCh];
    int done = 0, splits = 0;
    while ( done < nframes )
    {
        const long long pos  = g_transport.sample();
        long long       room = lend - pos;      // frames until the wrap frame
        if ( room <= 0 )
        {
            // at/past the boundary (or markers just moved): wrap NOW.
            if ( ++splits > 4 ) { room = nframes - done; }   // runaway guard
            else
            {
                // Relocate on the AUDIO thread without touching the seek
                // mailbox.  request_seek() here used to overwrite a locate the
                // UI had just queued -- the user's "return to beginning" was
                // silently swallowed by a wrap landing in the same block, and
                // conversely a wrap could be consumed as the user's seek.  The
                // mailbox now has exactly one writer (the UI), so a queued
                // locate survives the wrap and wins at the top of the next
                // callback, which is the correct precedence.
                g_transport.locate_now( lstart );
                g_loopBoundaryRelease.store(true,std::memory_order_release);
                g_loopWrapGeneration.fetch_add(1,std::memory_order_release);
                continue;
            }
        }
        int n = nframes - done;
        if ( (long long) n > room ) n = (int) room;
        for ( int c = 0; c < numChannels && c < kMaxDeviceCh; ++c )
            seg[c] = out[c] + done;
        for ( int c = 0; c < numInputChannels && c < kMaxDeviceCh; ++c )
            inSeg[c] = in && in[c] ? in[c] + done : nullptr;
        audio_render_segment( in ? inSeg : nullptr,
                              std::min( numInputChannels, kMaxDeviceCh ),
                              seg, numChannels, n, sr );
        done += n;
    }
}

void audio_render_device( const float* const* in, int numInputChannels,
                          float** out, int numChannels, int nframes, double sr )
{
    audio_render_device_inner( in, numInputChannels, out, numChannels, nframes, sr );
    // harness mute: render everything, hand the device silence
    if ( g_silentOut.load( std::memory_order_relaxed ) )
        for ( int c = 0; c < numChannels; ++c )
            if ( out[c] ) memset( out[c], 0, sizeof(float) * (size_t) nframes );
}

// Offline rendering has no capture device but intentionally shares the exact
// realtime render path.
// `in` / `inCh` are the device CAPTURE buffers for this block (nullptr / 0 when
// the stream is output-only, and for every offline/bounce caller).
void audio_render( const float* const* in, int inCh,
                   float** out, int numChannels, int nframes, double sr )
{
    audio_render_device( nullptr, 0, out, numChannels, nframes, sr );
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
    load_audio_preferences();

    g_graph->setTrackCount( AUDIO_APP_MAX_TRACKS );   // before prepare()

    // Prefer WASAPI shared over an MME default (nerf 12): MME suggests ~90ms
    // of latency and fires callbacks in timer-driven bursts, both fatal to
    // sample-accurate delivery.  Only overrides the DEFAULT choice -- explicit
    // host-API selection via audio_app_set_hostapi() behaves exactly as before.
    bool loadedBackend=false;
    {
        auto apis = g_engine->enumerateHostApis();
        int wasapi = -1; bool curIsMme = false;
        for ( size_t i = 0; i < apis.size(); ++i )
        {
            if((!g_audioPrefs.backendName.empty() &&
                apis[i].name==g_audioPrefs.backendName) ||
               (g_audioPrefs.backendName.empty() &&
                apis[i].index==g_audioPrefs.backendId)) {
                g_engine->selectHostApi(apis[i].index);
                loadedBackend=true;
            }
            if ( apis[i].name.find( "WASAPI" ) != std::string::npos ) wasapi = apis[i].index;
            if ( apis[i].isCurrent && apis[i].name.find( "MME" ) != std::string::npos )
                curIsMme = true;
        }
        if ( !loadedBackend && curIsMme && wasapi >= 0 )
        {
            fprintf( stderr, "[audio] default host API is MME; preferring WASAPI\n" );
            g_engine->selectHostApi( wasapi );
        }
    }
    {
        auto devs=g_engine->enumerateOutputDevices();
        for(const auto& d:devs)
            if((!g_audioPrefs.outputName.empty() && d.name==g_audioPrefs.outputName) ||
               (g_audioPrefs.outputName.empty() && d.id==g_audioPrefs.outputId)) {
                g_engine->selectDevice(d.id); break;
            }
        auto indevs=g_engine->enumerateInputDevices();
        for(const auto& d:indevs)
            if((!g_audioPrefs.inputName.empty() && d.name==g_audioPrefs.inputName) ||
               (g_audioPrefs.inputName.empty() && d.id==g_audioPrefs.inputId)) {
                g_inputDev=d.id;
                g_engine->selectInputDevice(d.id);
                g_engine->setInputChannels(d.outputChannels);
                break;
            }
        g_engine->setBufferSize(g_audioPrefs.buffer);
    }

    if ( !g_engine->open( g_audioPrefs.sampleRate, g_audioPrefs.buffer, g_audioPrefs.channels ) )
    {
        // the WASAPI preference may not open on every setup: fall back to the
        // PortAudio default backend before giving up.
        fprintf( stderr, "[audio] open failed (%s); retrying on default backend\n",
                 g_engine->lastError().c_str() );
        g_engine->stop(); g_engine->close();
        g_engine->selectHostApi( -1 );
        if ( !g_engine->open( g_audioPrefs.sampleRate, g_audioPrefs.buffer, g_audioPrefs.channels ) )
        {
            fprintf( stderr, "[audio] could not open output device: %s\n",
                     g_engine->lastError().c_str() );
            delete g_engine; g_engine=nullptr;
            delete g_graph;  g_graph=nullptr;
            delete g_host;   g_host=nullptr;
            return false;
        }
    }

    g_sr    = (double) g_engine->sampleRate();
    g_block = g_engine->blockSize();
    load_embedded_click();
    save_audio_preferences();

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
    g_defaultHwMidiInId = g_patch->addNode( std::make_unique<patch::MidiInNode>() );
    g_defaultHwMidiInNode = static_cast<patch::MidiInNode*>(
        g_patch->node( g_defaultHwMidiInId ) );
    g_virtualMidiId = g_patch->addNode( std::make_unique<patch::VirtualMidiPortsNode>() );
    g_patch->setDeviceOutNode( g_patchOutId );   // designate the master sink
    // Master mixer: 8 bus channels summed to the device output (+ a MIDI clock
    // out).  Tracks wire to its channels (port ch+1); its output (port 0) feeds
    // the master out.  Singleton -- created here, never added/removed by the UI.
    g_masterMixerId = g_patch->addNode( std::make_unique<patch::MasterMixerNode>() );
    g_patch->connect( patch::Connection{ patch::PortRef{ g_masterMixerId, 0 },
                                         patch::PortRef{ g_patchOutId, 0 } } );
    g_patch->compileAndPublish();

    g_engine->setRenderCallback( &audio_render_device );

    if ( !g_engine->start() )
    {
        fprintf( stderr, "[audio] could not start stream: %s\n",
                 g_engine->lastError().c_str() );
        audio_app_shutdown();
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
    // The audition clip is RCU-published and deliberately never freed on the
    // audio thread; the stream is stopped by now, so reclaim it here.
    if ( AudioClip* prev = g_previewClip.exchange( nullptr, std::memory_order_acq_rel ) )
        delete prev;
    if(float* capture=g_capBuf.exchange(nullptr,std::memory_order_acq_rel))
        free(capture);
    g_capturing.store(false,std::memory_order_release);
    g_capPos.store(0); g_capCap=0;
    for(auto& tc:g_trackCaps) if(tc.buf) {
        if(g_patch)
            if(auto* mm=dynamic_cast<patch::MasterMixerNode*>(g_patch->node(g_masterMixerId)))
                mm->endTrackCaptureFor(tc.track);
        // No keep-alive grace needed here (unlike audio_app_track_capture_begin):
        // the stream was stopped and closed at the top of this function, so no
        // audio block can still be holding the pointer.
        free(tc.buf); tc = TrackCapSlot{};
    }
    delete g_patch; g_patch=nullptr; g_midiInNode=nullptr;
    g_defaultHwMidiInNode=nullptr;
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
    if ( gain < 0.f ) gain = 0.f;
    if ( gain > 4.f ) gain = 4.f;
    // Deep-copy OFF the audio thread's path: nothing here is shared with it
    // until the publishing exchange below, so a 30-second sample no longer costs
    // the callback a single cycle (it used to block on the mutex this held).
    AudioClip* fresh = clip.empty() ? nullptr : new AudioClip( clip );
    g_previewGain.store( gain, std::memory_order_relaxed );
    g_previewEpoch.fetch_add( 1, std::memory_order_relaxed );
    AudioClip* old = g_previewClip.exchange( fresh, std::memory_order_acq_rel );
    if ( old )
    {
        // A block that loaded `old` before the exchange is still reading it for
        // the rest of that block; wait the standard two completed blocks before
        // freeing, exactly like the bounce-capture buffer.
        freeze_rcu_grace();
        delete old;
    }
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
    g_engine->setRenderCallback( &audio_render_device );
    return g_engine->start();
}

// Re-open + re-prepare after a backend/device/buffer change.  Robust: if the
// requested config can't open (WASAPI/WDM-KS often reject a default device or
// the 48k/512 format), fall back to the host-API default, then to the global
// default -- so a bad choice never leaves the DAW silent.  Caller has already
// stopped+closed the engine and applied its selection.
bool reopen_engine()
{
    if ( g_engine->open( g_audioPrefs.sampleRate, g_audioPrefs.buffer,
                         g_audioPrefs.channels ) && start_opened() ) {
        save_audio_preferences(); return true;
    }

    fprintf( stderr, "[audio] open failed (%s); trying host-API default device\n",
             g_engine->lastError().c_str() );
    g_engine->stop(); g_engine->close();
    g_engine->selectDevice( 0 );                 // host-API default output
    if ( g_engine->open( g_audioPrefs.sampleRate, g_audioPrefs.buffer,
                         g_audioPrefs.channels ) && start_opened() ) {
        save_audio_preferences(); return true;
    }

    fprintf( stderr, "[audio] still failing (%s); reverting to default backend\n",
             g_engine->lastError().c_str() );
    g_engine->stop(); g_engine->close();
    g_engine->selectHostApi( -1 );               // PortAudio default host API
    g_engine->selectDevice( 0 );
    g_engine->setBufferSize( 512 );
    if ( g_engine->open( g_audioPrefs.sampleRate, 512, g_audioPrefs.channels ) &&
         start_opened() ) {
        save_audio_preferences(); return true;
    }

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
    g_audioPrefs.outputId=deviceId;
    g_audioPrefs.outputName.clear();
    return reopen_engine();
}

// --- input device -----------------------------------------------------------
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

void audio_app_set_input_device( unsigned deviceId )
{
    if ( !g_engine ) return;
    g_inputDev = deviceId;
    g_audioPrefs.inputId=deviceId;
    g_audioPrefs.inputName.clear();
    g_engine->stop();
    g_engine->close();
    g_engine->selectInputDevice( deviceId );
    unsigned available = 0;
    for ( const auto& d : g_engine->enumerateInputDevices() )
        if ( d.id == deviceId || ( deviceId == 0 && d.isDefault ) ) {
            available = d.outputChannels;
            break;
        }
    g_engine->setInputChannels( available ? available : 2 );
    if ( g_running ) reopen_engine();
    else save_audio_preferences();
}

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
static long long hw_in_arrival_sample()
{
    // Live input while the transport is STOPPED: blockStart (g_transport.sample())
    // is frozen, so a wall-clock lookahead diverges past the block end and the
    // MidiIn node would hold the event back forever -- i.e. keyboard input would be
    // silent whenever you're not playing.  Deliver "now" (offset 0) when not
    // rolling so live hardware input always sounds.
    if ( !g_transport.rolling() ) return -1;
    // Absolute lookahead timestamps beyond loopEnd are unreachable: transport
    // wraps before it can ever visit them, leaving a queued key release behind
    // the boundary forever.  Live input is already consumed on the audio thread,
    // so while looping deliver it in the current block.  This also keeps ring
    // ordering monotonic across the backward transport jump.
    if(g_loopOn.load(std::memory_order_relaxed))return -1;
    long long ps = 0, pq = 0; double qps = 0.0;
    if ( !audio_app_block_pub( &ps, &pq, &qps ) || qps <= 0.0 ) return -1;
    const double dSamples = (double)( qpc_now() - pq ) / qps;
    return ps+(long long)llround(dSamples);
}

static void patch_midi_cb( double, std::vector<unsigned char>* msg, void* ud )
{
    if ( !msg || msg->empty() ) return;
    MidiEvent e{};
    e.sampleOffset = 0;
    e.status = (*msg)[0];
    e.data1  = msg->size() > 1 ? (*msg)[1] : 0;
    e.data2  = msg->size() > 2 ? (*msg)[2] : 0;
    auto* node=static_cast<patch::MidiInNode*>(ud);
    const long long arrival=hw_in_arrival_sample();
    e.captureSample=arrival;
    if(node) node->push(e,arrival<0?-1:arrival+(long long)g_block,0);
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
    const long long arrival=hw_in_arrival_sample();
    e.captureSample=arrival;
    node->push(e,arrival<0?-1:arrival+(long long)g_block);
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
    if(g_midiInDev) {
        try {
            if(g_midiInDev->isPortOpen()) {
                g_midiInDev->cancelCallback();
                g_midiInDev->closePort();
            }
        } catch(...) {}
        delete g_midiInDev; g_midiInDev=nullptr; g_midiInPort=-1;
    }
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
        g_midiInDev->setCallback( &patch_midi_cb, g_defaultHwMidiInNode );
        g_midiInDev->openPort( (unsigned) idx );
        g_midiInPort = idx;
        // This device owns the dedicated Hardware MIDI In graph node. Selecting
        // it enables graph rendering, but does not connect it to any track.
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
    g_audioPrefs.backendId=paHostApiIndex;
    g_audioPrefs.backendName.clear();
    g_audioPrefs.outputId=0;
    g_audioPrefs.outputName.clear();
    return reopen_engine();
}

// --- buffer size (latency) --------------------------------------------------
unsigned audio_app_buffer_size() { return g_engine ? g_engine->blockSize() : 0; }

bool audio_app_set_buffer_size( unsigned frames )
{
    if ( !g_engine || !g_running ) return false;
    g_engine->stop(); g_engine->close();
    g_engine->setBufferSize( frames );
    g_audioPrefs.buffer=frames;
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

// Defined further down with the rest of the freeze/bounce plumbing; declared
// here because the router below is the thing that reports the first note.
void audio_app_freeze_note_first_event( long long sample );

// Defined with the freeze state further down; route_midi is its only observer.
void audio_app_freeze_note_first_event( long long sample );

void audio_app_route_midi( int track, unsigned char status,
                           unsigned char d1, unsigned char d2, long long tick,
                           int column )
{
    // Bounds-check the track FIRST (bug B2).  RouteMsg::track is 8 bits and the
    // drains treat 0xFF as "no such plug", so `(unsigned char) track` silently
    // wrapped an out-of-range index into a value that either addressed the WRONG
    // track or (for -1 -> 255) made the event -- note-offs included -- disappear
    // in the drain.  Refuse it loudly instead: an event with no valid
    // destination has none, and pretending otherwise only hides the caller's bug.
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS )
    {
        g_midiBadTrackCount.fetch_add( 1, std::memory_order_relaxed );
        if ( !g_midiBadTrackWarned.exchange( true, std::memory_order_relaxed ) )
            fprintf( stderr, "[midi] route_midi: track %d out of range [0,%d) -- "
                             "event %02X %02X %02X rejected (further occurrences "
                             "counted only)\n",
                     track, AUDIO_APP_MAX_TRACKS, status, d1, d2 );
        return;
    }

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
    if ( !admit ) g_mcRingFull.fetch_add( 1, std::memory_order_relaxed );
    if ( admit )
    {
        g_mcEnq.fetch_add( 1, std::memory_order_relaxed );
        RouteMsg& m = g_ring[head & RING_MASK];
        // A bounce measures its own instrument latency against the first note it
        // scheduled (see audio_app_freeze_render_at).  Note-ons only: a CC or a
        // note-off makes no sound to measure from.
        if ( tick >= 0 && ( status & 0xF0 ) == 0x90 && d2 > 0 )
        {
            const kitchensink::TempoMap* tm =
                g_tmapActive.load( std::memory_order_acquire );
            audio_app_freeze_note_first_event(
                tm ? tm->tick_to_sample( tick ) : (long long) tick );
        }
        m.tick   = tick;
        m.epoch  = g_schedEpoch.load( std::memory_order_acquire );
        m.track  = (unsigned char) track;
        m.status = status;
        m.d1     = d1;
        m.d2     = d2;
        m.column = ( column < 0 || column > 126 ) ? (signed char) -1
                                                 : (signed char) column;
        g_head.store( head + 1, std::memory_order_release );
    }
    else
        g_midiDropCount.fetch_add( 1, std::memory_order_relaxed );
    g_midi_producer.clear( std::memory_order_release );
}

void audio_app_route_param_at_tick( int track, unsigned int paramId, float value,
                                    long long tick )
{
    // Same 8-bit wrap as audio_app_route_midi (bug B2): an out-of-range track
    // either vanished or, worse, landed on a DIFFERENT track's instrument.
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS )
    {
        g_midiBadTrackCount.fetch_add( 1, std::memory_order_relaxed );
        return;
    }
    while ( g_param_producer.test_and_set( std::memory_order_acquire ) ) { }
    unsigned head = g_phead.load( std::memory_order_relaxed );
    unsigned tail = g_ptail.load( std::memory_order_acquire );
    if ( head - tail < RING_SIZE )
    {
        ParamMsg& m = g_pring[head & RING_MASK];
        m.tick  = tick;
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

void audio_app_route_param( int track, unsigned int paramId, float value )
{
    audio_app_route_param_at_tick( track, paramId, value, -1 );
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

// --- Native keyzone SAMPLER instrument node ---------------------------------
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
bool audio_app_sampler_get_zone_meta(int node,int index,PatchKnob::engine::SamplerZoneInfo& out)
{
    if(!g_patch)return false;
    patch::PluginNode* pn=dynamic_cast<patch::PluginNode*>(g_patch->node((patch::NodeId)node));
    return pn&&PatchKnob::engine::sampler_get_zone_meta(pn->instance(),index,out);
}
bool audio_app_sampler_has_envelopes( int node )
{
    if ( !g_patch ) return false;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    return pn && PatchKnob::engine::sampler_has_envelopes( pn->instance() );
}

// --- per-zone parameters (SoundFont 2 generator parity) ---------------------
// One resolver for all of them: every setter is a no-op on a node that is not a
// sampler, exactly like the loaders above.
static PatchKnob::engine::IPluginInstance* audio_app_sampler_inst( int node )
{
    if ( !g_patch ) return nullptr;
    patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( g_patch->node( (patch::NodeId)node ) );
    if ( !pn ) return nullptr;
    IPluginInstance* inst = pn->instance();
    return PatchKnob::engine::sampler_is_sampler( inst ) ? inst : nullptr;
}

void audio_app_sampler_set_zone_env( int node, int slot, int level, int env,
                                     const PatchKnob::engine::SamplerZoneEnv* e )
{
    if ( IPluginInstance* inst = audio_app_sampler_inst( node ) )
        PatchKnob::engine::sampler_set_zone_env( inst, slot, level, env, e );
}
bool audio_app_sampler_get_zone_env( int node, int slot, int level, int env,
                                     PatchKnob::engine::SamplerZoneEnv* out )
{
    IPluginInstance* inst = audio_app_sampler_inst( node );
    return inst && PatchKnob::engine::sampler_get_zone_env( inst, slot, level, env, out );
}
void audio_app_sampler_set_zone_filter( int node, int slot, int level,
                                        float cutoffHz, float resonanceDb )
{
    if ( IPluginInstance* inst = audio_app_sampler_inst( node ) )
        PatchKnob::engine::sampler_set_zone_filter( inst, slot, level, cutoffHz, resonanceDb );
}
void audio_app_sampler_set_zone_tuning( int node, int slot, int level,
                                        int coarse, int fine, int scaleTuning )
{
    if ( IPluginInstance* inst = audio_app_sampler_inst( node ) )
        PatchKnob::engine::sampler_set_zone_tuning( inst, slot, level, coarse, fine, scaleTuning );
}
void audio_app_sampler_set_zone_level( int node, int slot, int level,
                                       float pan, float attenuationDb )
{
    if ( IPluginInstance* inst = audio_app_sampler_inst( node ) )
        PatchKnob::engine::sampler_set_zone_level( inst, slot, level, pan, attenuationDb );
}
void audio_app_sampler_set_zone_exclusive( int node, int slot, int level, int exclusiveClass )
{
    if ( IPluginInstance* inst = audio_app_sampler_inst( node ) )
        PatchKnob::engine::sampler_set_zone_exclusive( inst, slot, level, exclusiveClass );
}
void audio_app_sampler_set_zone_modroute( int node, int slot, int level,
                                          float toPitchCents, float toFilterCents )
{
    if ( IPluginInstance* inst = audio_app_sampler_inst( node ) )
        PatchKnob::engine::sampler_set_zone_modroute( inst, slot, level, toPitchCents, toFilterCents );
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
// Defined with the master-mixer insert chains further down (same TU).  A strip
// whose insert chain owns an external fader carries its gain on a TrackFaderNode,
// so EVERY path that writes a master-mixer gain/mute has to push it there too --
// otherwise loading a project or moving an automation lane would silently leave
// the audible gain at whatever the Amp last held.
void master_chain_sync_fader( int track );
void master_chain_rebuild( int track );
void master_chain_track_removed( int track );
void master_chain_forget_sources( int track );
void master_chain_reset_all();
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
void  audio_app_mixer_set_gain( int node, int ch, float g ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setGain(ch, g);
                                                             if ( (patch::NodeId)node == g_masterMixerId ) master_chain_sync_fader(ch); }
bool  audio_app_mixer_mute( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->mute(ch) : false; }
void  audio_app_mixer_set_mute( int node, int ch, bool mu ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setMute(ch, mu);
                                                              if ( (patch::NodeId)node == g_masterMixerId ) master_chain_sync_fader(ch); }
float audio_app_mixer_vu( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->vu(ch) : 0.f; }
float audio_app_mixer_vu_left( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->vuLeft(ch) : 0.f; }
float audio_app_mixer_vu_right( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->vuRight(ch) : 0.f; }
float audio_app_mixer_pan( int node, int ch ) { patch::MixerNode* m = mixer_node(node); return m ? m->pan(ch) : 0.f; }
void  audio_app_mixer_set_pan( int node, int ch, float p ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setPan(ch, p); }
float audio_app_mixer_master_gain( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterGain() : 1.f; }
void  audio_app_mixer_set_master_gain( int node, float g ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setMasterGain(g);
                                                            if ( (patch::NodeId)node == g_masterMixerId ) master_chain_sync_fader(-1); }
bool  audio_app_mixer_master_mute( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterMute() : false; }
void  audio_app_mixer_set_master_mute( int node, bool mu ) { if ( patch::MixerNode* m = mixer_node(node) ) m->setMasterMute(mu); }
float audio_app_mixer_master_vu( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterVu() : 0.f; }
void audio_app_meters_latch()
{
    if ( !g_patch ) return;
    // Every mixer in the graph, not just the master: the arrange lane VUs read
    // per-track channels off the master mixer, and a patch can hold more.
    for ( patch::NodeId id : g_patch->nodeIds() )
        if ( patch::MixerNode* m = dynamic_cast<patch::MixerNode*>( g_patch->node( id ) ) )
            m->meterLatch();
}

float audio_app_mixer_master_vu_left( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterVuLeft() : 0.f; }
float audio_app_mixer_master_vu_right( int node ) { patch::MixerNode* m = mixer_node(node); return m ? m->masterVuRight() : 0.f; }

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
// Live control from the editor: push a value/bang to a GUI atom's receive symbol
// in the running patch (queued, delivered on the audio thread).  Drives toggles /
// sliders / bangs without a file reload -- e.g. a toggle starts a [metro] live.
void audio_app_pd_send_float( int node, const char* recv, float v )
{
    patch::PdNode* p = pd_node( node );
    if ( p && recv ) p->sendFloat( recv, v );
}
void audio_app_pd_send_bang( int node, const char* recv )
{
    patch::PdNode* p = pd_node( node );
    if ( p && recv ) p->sendBang( recv );
}
// In-memory patch text (source of truth, saved with the project -- no disk .pd).
// set_text reloads libpd (structural change); store_text only updates the stored
// text (a GUI value edit whose live value already went out via send_float/bang).
// Live GUI feedback: bind a GUI atom's SEND symbol so the editor can reflect
// messages that reach it, and poll the queued updates (one per call, 0 when empty).
void audio_app_pd_subscribe( int node, const char* sendSym )
{
    patch::PdNode* p = pd_node( node );
    if ( p && sendSym ) p->subscribeGui( sendSym );
}
void audio_app_pd_clear_gui_binds( int node )
{
    patch::PdNode* p = pd_node( node );
    if ( p ) p->clearGuiBinds();
}
int audio_app_pd_poll_gui( int node, char* recv, int recvcap, float* val, int* isBang )
{
    patch::PdNode* p = pd_node( node );
    if ( !p || !recv || recvcap <= 0 ) return 0;
    patch::PdNode::GuiFb fb;
    if ( p->drainGui( &fb, 1 ) != 1 ) return 0;
    int k = (int) fb.recv.size(); if ( k > recvcap - 1 ) k = recvcap - 1;
    memcpy( recv, fb.recv.data(), (size_t) k ); recv[k] = 0;
    if ( val )    *val    = fb.val;
    if ( isBang ) *isBang = fb.bang ? 1 : 0;
    return 1;
}
bool audio_app_pd_set_text( int node, const char* text )
{
    patch::PdNode* p = pd_node( node );
    if ( !p || !text ) return false;
    bool ok = p->loadPatchText( text );
    if ( g_patch ) g_patch->compileAndPublish();   // adc~/dac~ counts may have changed
    return ok;
}
void audio_app_pd_store_text( int node, const char* text )
{
    patch::PdNode* p = pd_node( node );
    if ( p && text ) p->storePatchText( text );
}
const char* audio_app_pd_get_text( int node )
{
    patch::PdNode* p = pd_node( node );
    return p ? p->patchText().c_str() : "";
}
void audio_app_pd_set_dsp( int node, int on )
{
    patch::PdNode* p = pd_node( node );
    if ( p ) p->setDsp( on != 0 );
}
int audio_app_pd_dsp( int node )
{
    patch::PdNode* p = pd_node( node );
    return ( p && p->dsp() ) ? 1 : 0;
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
    // Project restore can create a RackNode before the rack editor has ever
    // opened. Register scripting types here so PKPd/PKCsound never depend on
    // UI initialization order and silently disappear during restore.
    ::rackx::registerRackExtModules();
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
    if ( !g_engine ) return false;
    // AudioEngine::tryRecover() reopens on the default device and starts the
    // stream -- and that is ALL it does.  AudioEngine::open() sets sampleRate_/
    // blockSize_ to whatever the new stream actually negotiates, so after a
    // recovery g_sr/g_block still described the DEAD device: neither graph was
    // re-prepare()d at the new rate, the patch plan was never republished, and
    // the pools stayed sized for the old block.  Every other reconfiguration
    // path in this file goes through reopen_engine() -> start_opened(), which
    // does all of that; use it here as well.  reopen_engine() also keeps the
    // full fallback chain (current selection, then host-API default, then the
    // global default at 512), which is strictly better than tryRecover()'s
    // single shot at device 0.
    g_engine->stop();
    g_engine->close();
    return reopen_engine();
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
    master_chain_track_removed( idx );   // drop this strip's processor box + shift
    if ( patch::MasterMixerNode* mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) ) )
        { const int removed = idx;
          mm->removeTrack( removed );
          // MasterMixerNode's per-track port ids are POSITIONAL (inlet = 2+2t),
          // so compacting its track list RENAMES the ports of every track above
          // `removed` -- while g_patch->conns_ still holds the OLD ids.  Left
          // alone, every surviving strip's fader/pan/mute/meter/record drove the
          // wrong track, the deleted track kept sounding through whichever strip
          // inherited its port id, and the top track fell silent (its cables
          // pointed past the end of the shrunken port list).  Rewrite the
          // connections in the SAME edit, before the recompile below reads them.
          g_patch->remapNodePorts( g_masterMixerId,
              [removed]( patch::PortId pid ) {
                  return patch::MasterMixerNode::portAfterTrackRemoval( (int)pid, removed );
              } );
          // The sequencer MidiIn node's "Instrument out" plugs are positional in
          // exactly the same way (plug index == track index -- see
          // audio_app_master_connect_instrument), so its cables need the same
          // rename or every surviving track's instrument keeps listening on the
          // plug the sequencer no longer pushes its notes to.
          if ( g_midiInNode ) {
              g_patch->remapNodePorts( g_patchMidiInId,
                  [removed]( patch::PortId pid ) {
                      const int plug = (int)pid;
                      if ( plug <  removed ) return plug;
                      if ( plug == removed ) return -1;
                      return plug - 1;
                  } );
              g_midiInNode->setTrackPorts( mm->trackCount() );
          }
          g_patch->pruneDanglingConnections();   // anything left pointing past the shrink
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

    int audioOut = -1;
    for ( int i = 0; i < inst->numPorts(); ++i )
    {
        const patch::PortDesc pd = inst->port( i );
        if ( pd.dir == patch::PortDir::Out && pd.kind == patch::PortKind::Audio && audioOut < 0 ) audioOut = (int)pd.id;
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
    //
    // Bug B2 (related): this used to fall back to midiPlug = -1 whenever the node
    // had not grown that far, which quietly wired NO MIDI at all -- the track's
    // note-offs then had nowhere to go and every note it played hung.  The plug
    // count is message-thread state we are allowed to grow right here (exactly
    // what audio_app_master_add_track does), so grow it and always wire a real
    // plug.  compileAndPublish() below picks up the new port count.
    if ( audioOut >= 0 && !has( (patch::NodeId)instrNode, audioOut, g_masterMixerId, inlet ) )
        g_patch->connect( patch::Connection{ patch::PortRef{ (patch::NodeId)instrNode, (patch::PortId)audioOut },
                                             patch::PortRef{ g_masterMixerId, (patch::PortId)inlet } } );

    /*  ...and the MIDI, which the comment above has described for a long time
        without any code actually doing it.  Only the AUDIO connection was ever
        made, so an instrument routed through here was wired to be HEARD but
        never to be PLAYED: it received no notes at all.

        That is why the shipped voice-freeze proof captured 132000 frames of
        silence with 0 of its 16 note bursts, while driving the same voice
        directly through the engine gave a full-level signal -- the notes had
        nowhere to go.  It breaks live modular playback for the same reason, not
        just the offline bounce.

        Plug index == track ("Instrument out" N), so each track drives its own
        instrument with no channel filtering.  The plug count is message-thread
        state, so grow it here rather than falling back to "no MIDI"; the
        compileAndPublish() below picks up the new port count.  */
    int midiIn = -1;
    for ( int i = 0; i < inst->numPorts(); ++i )
    {
        const patch::PortDesc pd = inst->port( i );
        if ( pd.dir == patch::PortDir::In && pd.kind == patch::PortKind::Midi && midiIn < 0 )
            midiIn = (int)pd.id;
    }
    if ( midiIn >= 0 && g_midiInNode )
    {
        if ( g_midiInNode->trackPorts() <= track )
            g_midiInNode->setTrackPorts( track + 1 );
        if ( !has( g_patchMidiInId, track, (patch::NodeId)instrNode, midiIn ) )
            g_patch->connect( patch::Connection{
                patch::PortRef{ g_patchMidiInId, (patch::PortId)track },
                patch::PortRef{ (patch::NodeId)instrNode, (patch::PortId)midiIn } } );
    }

    g_patch->compileAndPublish();
    // The instrument was wired straight onto the inlet; if this strip has an
    // insert chain the chain now has to be re-inserted between them (and the
    // instrument recorded as the chain's new head source).
    master_chain_rebuild( track );
    g_modular.store( true, std::memory_order_release );
    return audioOut >= 0 ? inlet : -1;
}

// Disconnect whatever currently feeds a track's audio inlet.  Selecting the
// plain "Audio input" entry has to be able to UNDO an instrument route; routing
// with sourceNode < 0 could not do it, because the node lookup failed and the
// function returned before reaching the disconnect.
bool audio_app_master_clear_audio_input( int track )
{
    if ( !g_patch || track < 0 ) return false;
    patch::MasterMixerNode* mm =
        dynamic_cast<patch::MasterMixerNode*>( g_patch->node(g_masterMixerId) );
    if ( !mm || track >= mm->trackCount() || mm->trackIsMidi(track) ) return false;
    const int inlet = 2 + 2 * track;
    bool any = false;
    const std::vector<patch::Connection> conns = g_patch->connections();
    for ( const patch::Connection& c : conns )
        if ( c.to.node == g_masterMixerId && (int)c.to.port == inlet )
            { g_patch->disconnect(c); any = true; }
    // Forget the insert chain's remembered head source as well, otherwise the
    // next rebuild would helpfully re-attach the input the user just removed.
    // The processors themselves stay -- Ardour disconnecting an input does not
    // empty the processor box.
    master_chain_forget_sources( track );
    master_chain_rebuild( track );
    if ( any ) g_patch->compileAndPublish();
    return any;
}

bool audio_app_master_route_audio_input( int track, int sourceNode )
{
    if ( !g_patch || track < 0 ) return false;
    patch::MasterMixerNode* mm =
        dynamic_cast<patch::MasterMixerNode*>( g_patch->node(g_masterMixerId) );
    patch::Node* source = g_patch->node((patch::NodeId)sourceNode);
    if ( !mm || !source || track >= mm->trackCount() || mm->trackIsMidi(track) )
        return false;

    int audioOut = -1;
    for ( int i = 0; i < source->numPorts(); ++i ) {
        const patch::PortDesc pd = source->port(i);
        if ( pd.dir == patch::PortDir::Out && pd.kind == patch::PortKind::Audio ) {
            audioOut = (int)pd.id;
            break;
        }
    }
    if ( audioOut < 0 ) return false;

    const int inlet = 2 + 2 * track;
    const std::vector<patch::Connection> conns = g_patch->connections();
    for ( const patch::Connection& c : conns )
        if ( c.to.node == g_masterMixerId && (int)c.to.port == inlet )
            g_patch->disconnect(c);

    const bool ok = g_patch->connect(patch::Connection{
        patch::PortRef{(patch::NodeId)sourceNode, (patch::PortId)audioOut},
        patch::PortRef{g_masterMixerId, (patch::PortId)inlet}
    });
    g_patch->compileAndPublish();
    master_chain_rebuild( track );      // re-insert this strip's processor box
    if ( ok ) g_modular.store(true, std::memory_order_release);
    return ok;
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

// ===========================================================================
//  MASTER-MIXER INSERT CHAINS -- Ardour's PROCESSOR BOX for this engine.
//
//  gtk2_ardour/processor_box.cc drives Route::add_processor_by_index /
//  ::remove_processor / ::reorder_processors over a single ordered processor
//  list in which the FADER (an Amp) is itself an entry: whatever sits above it
//  is pre-fader, below is post-fader (ProcessorBox::setup_entry_positions).
//
//  Here the strip is a MasterMixerNode channel, so the equivalent list is an
//  ordered run of patch nodes wired in series in front of that channel's audio
//  inlet:
//
//      source(s) -> [pre inserts] -> (TrackFaderNode) -> [post inserts] -> inlet
//
//  The TrackFaderNode only materialises when at least one insert is placed
//  post-fader; the mixer channel is then switched to unity (setExternalFader)
//  so the gain is applied exactly once, in the right place.  Bypass is the
//  graph's own generic passthrough (Node::setBypass), the direct analogue of
//  Ardour's Processor::enable(false): the entry stays in the list and in the
//  signal path but does nothing.
//
//  The MASTER strip (track < 0) chains between the mix bus output and the audio
//  device sink.  Its fader is the summing stage's own master gain, which cannot
//  be lifted out of the node, so master inserts are always post-fader.
// ===========================================================================
namespace {

struct MasterInsertSlot {
    patch::NodeId node   = 0;
    bool          active = true;    // Ardour Processor::enabled()
    bool          pre    = true;    // above the fader entry
};
struct MasterStripChain {
    std::vector<MasterInsertSlot> slots;
    patch::NodeId                 fader = 0;     // TrackFaderNode, 0 = none
    std::vector<patch::PortRef>   sources;       // last known chain head feed(s)
};

MasterStripChain g_masterChain[PatchKnob::app::AUDIO_APP_MAX_TRACKS];
MasterStripChain g_masterBusChain;               // the MASTER strip (track < 0)

// ---- AUX BUSES -----------------------------------------------------------
// Both of these are keyed by the mixer's stable SLOT, never by the bus INDEX,
// for the same reason the ports are: deleting bus 0 of three renumbers the
// indices and must not drag another bus's node or processor box with it.
constexpr int kAuxSlots = patch::MasterMixerNode::kMaxAuxBuses;
patch::NodeId    g_auxNode[kAuxSlots]  = { 0 };   // the AuxBusNode per slot
MasterStripChain g_auxChain[kAuxSlots];           // its insert chain per slot

patch::MasterMixerNode* master_mixer_node()
{
    return g_patch ? dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) )
                   : nullptr;
}

//! Bus INDEX -> stable slot, or -1.  Everything app-side that stores per-bus
//! state goes through this.
int aux_slot_of( int aux )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return mm ? mm->auxSlot( aux ) : -1;
}

patch::AuxBusNode* aux_bus_node( int aux )
{
    const int slot = aux_slot_of( aux );
    if ( slot < 0 || !g_patch ) return nullptr;
    return dynamic_cast<patch::AuxBusNode*>( g_patch->node( g_auxNode[slot] ) );
}

MasterStripChain* master_chain( int track )
{
    // track >= 0 : a track strip.  -1 : the MASTER strip.  <= -2 : aux bus
    // (-2 - INDEX), which is what audio_app_master_aux_strip() hands out so the
    // ordinary audio_app_master_insert_* API can put a reverb on a bus.
    if ( track <= -2 ) {
        const int slot = aux_slot_of( -2 - track );
        return ( slot >= 0 && slot < kAuxSlots ) ? &g_auxChain[slot] : nullptr;
    }
    if ( track < 0 ) return &g_masterBusChain;
    if ( track >= PatchKnob::app::AUDIO_APP_MAX_TRACKS ) return nullptr;
    return &g_masterChain[track];
}

// First audio port of a node in the requested direction (-1 if it has none).
int node_audio_port( patch::NodeId id, patch::PortDir dir )
{
    patch::Node* n = g_patch ? g_patch->node( id ) : nullptr;
    if ( !n ) return -1;
    for ( int i = 0; i < n->numPorts(); ++i ) {
        const patch::PortDesc p = n->port( i );
        if ( p.kind == patch::PortKind::Audio && p.dir == dir ) return (int)p.id;
    }
    return -1;
}

// Where a strip's chain terminates: the mixer channel inlet, or (master strip)
// the device sink's audio input.
bool master_chain_sink( int track, patch::NodeId& node, int& port )
{
    if ( track <= -2 ) {                       // aux strip: the bus node's input
        const int slot = aux_slot_of( -2 - track );
        if ( slot < 0 || slot >= kAuxSlots || !g_auxNode[slot] ) return false;
        node = g_auxNode[slot];
        port = node_audio_port( node, patch::PortDir::In );
        return port >= 0;
    }
    if ( track < 0 ) {
        node = g_patchOutId;
        port = node_audio_port( g_patchOutId, patch::PortDir::In );
        return port >= 0;
    }
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !mm || track >= mm->trackCount() || mm->trackIsMidi( track ) ) return false;
    node = g_masterMixerId;
    port = 2 + 2 * track;
    return true;
}

// Push the strip's gain/mute onto its Amp node, so the fader stays live no
// matter which path set the value (UI, project load, automation).
void master_chain_sync_fader( int track )
{
    MasterStripChain* ch = master_chain( track );
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !ch || !ch->fader || !mm || !g_patch ) return;
    if ( auto* f = dynamic_cast<patch::TrackFaderNode*>( g_patch->node( ch->fader ) ) ) {
        f->setGain( track < 0 ? mm->masterGain() : mm->gain( track ) );
        // channelSilenced() == mute OR "something else is soloed": with the
        // fader lifted out of the node, the Amp has to carry BOTH or a soloed-
        // out strip with post-fader inserts would keep sounding.
        f->setMute( track >= 0 && mm->channelSilenced( track ) );
    }
}

// Re-wire one strip's chain from scratch.  Idempotent: it re-derives the head
// source(s) from the live graph (so a cable the user drew in the patcher is
// picked up as a source rather than clobbered), tears down every edge that
// belongs to the chain, and rebuilds the series in list order.
void master_chain_rebuild( int track )
{
    if ( !g_patch ) return;
    MasterStripChain* ch = master_chain( track );
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !ch || !mm ) return;

    patch::NodeId sinkNode = 0; int sinkPort = -1;
    if ( !master_chain_sink( track, sinkNode, sinkPort ) ) return;

    auto owned = [&]( patch::NodeId n ) {
        if ( n != 0 && n == ch->fader ) return true;
        for ( size_t i = 0; i < ch->slots.size(); ++i ) if ( ch->slots[i].node == n ) return true;
        return false;
    };

    const std::vector<patch::Connection> conns = g_patch->connections();

    // 1. head source(s).  For a track strip anything feeding the inlet that is
    //    not part of the chain is a source; the master strip is always fed by
    //    the mix bus itself.
    std::vector<patch::PortRef> sources;
    if ( track <= -2 ) {
        // An aux strip is fed by the mixer's SEND port for that bus, and by
        // nothing else -- the whole point of the bus is that the sends are the
        // only way in.
        const int slot = aux_slot_of( -2 - track );
        if ( slot < 0 ) return;
        sources.push_back( patch::PortRef{
            g_masterMixerId, patch::MasterMixerNode::auxSendPort( slot ) } );
    } else if ( track < 0 ) {
        sources.push_back( patch::PortRef{ g_masterMixerId, (patch::PortId)0 } );
    } else {
        for ( size_t i = 0; i < conns.size(); ++i )
            if ( conns[i].to.node == sinkNode && (int)conns[i].to.port == sinkPort &&
                 !owned( conns[i].from.node ) )
                sources.push_back( conns[i].from );
    }
    if ( !sources.empty() ) ch->sources = sources;
    else                    sources = ch->sources;

    // 2. tear down every edge the chain is responsible for.
    for ( size_t i = 0; i < conns.size(); ++i ) {
        const patch::Connection& c = conns[i];
        bool drop = owned( c.from.node ) || owned( c.to.node );
        if ( c.to.node == sinkNode && (int)c.to.port == sinkPort ) {
            // The MASTER strip shares the device sink with anything else the
            // user patched there -- only our own feed may be removed.  A track
            // strip and an aux strip both own their sink outright.
            if ( track == -1 ) drop = drop || ( c.from.node == g_masterMixerId );
            else               drop = true;
        }
        if ( drop ) g_patch->disconnect( c );
    }

    // 3. decide whether the Amp is needed (a post-fader insert exists) and
    //    build the ordered element list.  A bypassed entry stays in the path:
    //    the graph's generic passthrough is the bypass (Ardour keeps the
    //    processor in the list too), so ordering never shifts under the user.
    // Only a TRACK strip can grow an Amp.  The master strip's fader is inside
    // the summing stage; an AUX strip's "fader" is the return gain at the END of
    // the bus node, so everything wired in front of the node is already
    // pre-fader and there is nothing to lift out.
    bool needFader = false;
    if ( track >= 0 )
        for ( size_t i = 0; i < ch->slots.size(); ++i )
            if ( !ch->slots[i].pre ) { needFader = true; break; }

    if ( needFader && ch->fader == 0 ) {
        ch->fader = g_patch->addNode( std::make_unique<patch::TrackFaderNode>(
                        track < 0 ? mm->masterGain() : mm->gain( track ) ) );
    } else if ( !needFader && ch->fader != 0 ) {
        g_patch->disconnectAll( ch->fader );
        g_patch->removeNode( ch->fader );
        ch->fader = 0;
    }
    mm->setExternalFader( track, needFader );
    master_chain_sync_fader( track );

    std::vector<patch::NodeId> series;
    for ( size_t i = 0; i < ch->slots.size(); ++i ) if ( ch->slots[i].pre ) series.push_back( ch->slots[i].node );
    if ( needFader ) series.push_back( ch->fader );
    for ( size_t i = 0; i < ch->slots.size(); ++i ) if ( !ch->slots[i].pre ) series.push_back( ch->slots[i].node );

    // 4. wire source(s) -> series -> sink.  A node that turns out to have no
    //    usable audio in/out (an empty plugin slot, say) is skipped rather than
    //    breaking the strip's audio.
    std::vector<patch::PortRef> cursor = sources;
    for ( size_t i = 0; i < series.size(); ++i ) {
        const int in  = node_audio_port( series[i], patch::PortDir::In );
        const int out = node_audio_port( series[i], patch::PortDir::Out );
        if ( in < 0 || out < 0 ) continue;
        for ( size_t s = 0; s < cursor.size(); ++s )
            g_patch->connect( patch::Connection{ cursor[s],
                              patch::PortRef{ series[i], (patch::PortId)in } } );
        cursor.clear();
        cursor.push_back( patch::PortRef{ series[i], (patch::PortId)out } );
    }
    for ( size_t s = 0; s < cursor.size(); ++s )
        g_patch->connect( patch::Connection{ cursor[s],
                          patch::PortRef{ sinkNode, (patch::PortId)sinkPort } } );

    g_patch->compileAndPublish();
    if ( !ch->slots.empty() ) g_modular.store( true, std::memory_order_release );
}

// Keep pre-fader entries ahead of post-fader ones so the display order and the
// signal order are the same list (Ardour's processor box invariant).
void master_chain_normalise( MasterStripChain& ch )
{
    std::stable_sort( ch.slots.begin(), ch.slots.end(),
                      []( const MasterInsertSlot& a, const MasterInsertSlot& b ) {
                          return (a.pre ? 0 : 1) < (b.pre ? 0 : 1); } );
}

void master_chain_forget_sources( int track )
{
    if ( MasterStripChain* ch = master_chain( track ) ) ch->sources.clear();
}

// MasterMixerNode::removeTrack COMPACTS the channel indices, so the per-track
// chains have to shift in lockstep or a surviving strip inherits the removed
// strip's processor box.
void master_chain_track_removed( int track )
{
    if ( track < 0 || track >= PatchKnob::app::AUDIO_APP_MAX_TRACKS ) return;
    MasterStripChain& dead = g_masterChain[track];
    std::vector<patch::NodeId> nodes;
    for ( size_t i = 0; i < dead.slots.size(); ++i ) nodes.push_back( dead.slots[i].node );
    if ( dead.fader ) nodes.push_back( dead.fader );
    dead.slots.clear(); dead.sources.clear(); dead.fader = 0;
    for ( size_t i = 0; i < nodes.size(); ++i ) {
        if ( g_patch ) g_patch->disconnectAll( nodes[i] );
        audio_app_patch_remove( (int)nodes[i] );
    }
    for ( int t = track; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS - 1; ++t )
        g_masterChain[t] = g_masterChain[t + 1];
    g_masterChain[PatchKnob::app::AUDIO_APP_MAX_TRACKS - 1] = MasterStripChain();
    if ( patch::MasterMixerNode* mm = master_mixer_node() )
        for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t )
            mm->setExternalFader( t, g_masterChain[t].fader != 0 );
}

// Project load / new song: the whole patch is torn down, so drop every chain
// (the nodes themselves are removed by audio_app_project_reset_patch).
void master_chain_reset_all()
{
    for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t )
        g_masterChain[t] = MasterStripChain();
    g_masterBusChain = MasterStripChain();
    for ( int a = 0; a < kAuxSlots; ++a ) { g_auxChain[a] = MasterStripChain(); g_auxNode[a] = 0; }
    if ( patch::MasterMixerNode* mm = master_mixer_node() )
        for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t )
            mm->setExternalFader( t, false );
}

} // namespace

int audio_app_master_insert_count( int track )
{
    MasterStripChain* ch = master_chain( track );
    return ch ? (int)ch->slots.size() : 0;
}

bool audio_app_master_insert_info( int track, int slot, int* outNode,
                                   char* nameBuf, int nameBufLen,
                                   int* outActive, int* outPreFader )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || slot < 0 || slot >= (int)ch->slots.size() ) return false;
    const MasterInsertSlot& s = ch->slots[(size_t)slot];
    if ( outNode )     *outNode = (int)s.node;
    if ( outActive )   *outActive = s.active ? 1 : 0;
    if ( outPreFader ) *outPreFader = s.pre ? 1 : 0;
    if ( nameBuf && nameBufLen > 0 ) {
        std::string nm;
        if ( g_patch ) {
            if ( patch::Node* n = g_patch->node( s.node ) ) {
                if ( patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n ) ) {
                    if ( IPluginInstance* inst = pn->instance() ) nm = inst->descriptor().name;
                    else nm = "<empty>";
                } else {
                    nm = n->typeName() ? n->typeName() : "";
                }
            }
        }
        if ( nm.empty() ) nm = "<gone>";
        std::snprintf( nameBuf, (size_t)nameBufLen, "%s", nm.c_str() );
    }
    return true;
}

int audio_app_master_insert_node_at( int track, int slot )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || slot < 0 || slot >= (int)ch->slots.size() ) return -1;
    return (int)ch->slots[(size_t)slot].node;
}

int audio_app_master_insert_add_node( int track, int node, int preFader )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || !g_patch || node < 0 ) return -1;
    if ( !g_patch->node( (patch::NodeId)node ) ) return -1;
    patch::NodeId sinkNode = 0; int sinkPort = -1;
    if ( !master_chain_sink( track, sinkNode, sinkPort ) ) return -1;
    for ( size_t i = 0; i < ch->slots.size(); ++i )
        if ( ch->slots[i].node == (patch::NodeId)node ) return (int)i;

    MasterInsertSlot s;
    s.node = (patch::NodeId)node;
    s.active = true;
    // master strip: always POST-fader (the fader is inside the sum).
    // aux strip:    always PRE-fader  (the fader is the bus's return gain).
    s.pre = ( track == -1 ) ? false : ( track <= -2 ? true : ( preFader != 0 ) );
    ch->slots.push_back( s );
    master_chain_normalise( *ch );
    master_chain_rebuild( track );
    for ( size_t i = 0; i < ch->slots.size(); ++i )
        if ( ch->slots[i].node == (patch::NodeId)node ) return (int)i;
    return -1;
}

int audio_app_master_insert_add( int track, const PluginDescriptor& desc, int preFader )
{
    const int node = audio_app_patch_add_plugin( desc );
    if ( node < 0 ) return -1;
    const int slot = audio_app_master_insert_add_node( track, node, preFader );
    if ( slot < 0 ) { audio_app_patch_remove( node ); return -1; }
    return slot;
}

bool audio_app_master_insert_remove( int track, int slot )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || slot < 0 || slot >= (int)ch->slots.size() ) return false;
    const patch::NodeId dead = ch->slots[(size_t)slot].node;
    ch->slots.erase( ch->slots.begin() + slot );
    if ( g_patch ) g_patch->disconnectAll( dead );
    master_chain_rebuild( track );
    audio_app_patch_remove( (int)dead );
    return true;
}

bool audio_app_master_insert_move( int track, int from, int to, int preFader )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || from < 0 || from >= (int)ch->slots.size() ) return false;
    if ( to < 0 ) to = 0;
    if ( to > (int)ch->slots.size() - 1 ) to = (int)ch->slots.size() - 1;
    MasterInsertSlot s = ch->slots[(size_t)from];
    if ( preFader >= 0 )
        s.pre = ( track == -1 ) ? false : ( track <= -2 ? true : ( preFader != 0 ) );
    ch->slots.erase( ch->slots.begin() + from );
    ch->slots.insert( ch->slots.begin() + to, s );
    master_chain_normalise( *ch );
    master_chain_rebuild( track );
    return true;
}

bool audio_app_master_insert_set_active( int track, int slot, int active )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || slot < 0 || slot >= (int)ch->slots.size() || !g_patch ) return false;
    ch->slots[(size_t)slot].active = active != 0;
    if ( patch::Node* n = g_patch->node( ch->slots[(size_t)slot].node ) )
        n->setBypass( active == 0 );          // graph-level passthrough == Ardour deactivate
    return true;
}

bool audio_app_master_insert_set_prefader( int track, int slot, int pre )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch || slot < 0 || slot >= (int)ch->slots.size() ) return false;
    // Neither the master strip nor an aux strip has an Amp to move across.
    if ( track < 0 ) return false;
    if ( ch->slots[(size_t)slot].pre == ( pre != 0 ) ) return true;
    ch->slots[(size_t)slot].pre = ( pre != 0 );
    master_chain_normalise( *ch );
    master_chain_rebuild( track );
    return true;
}

bool audio_app_master_insert_clear( int track, int which )
{
    MasterStripChain* ch = master_chain( track );
    if ( !ch ) return false;
    std::vector<patch::NodeId> dead;
    for ( size_t i = 0; i < ch->slots.size(); ) {
        const bool match = ( which < 0 ) || ( which == 0 && ch->slots[i].pre )
                                         || ( which == 1 && !ch->slots[i].pre );
        if ( match ) { dead.push_back( ch->slots[i].node );
                       ch->slots.erase( ch->slots.begin() + (long)i ); }
        else ++i;
    }
    if ( dead.empty() ) return false;
    if ( g_patch ) for ( size_t i = 0; i < dead.size(); ++i ) g_patch->disconnectAll( dead[i] );
    master_chain_rebuild( track );
    for ( size_t i = 0; i < dead.size(); ++i ) audio_app_patch_remove( (int)dead[i] );
    return true;
}

void audio_app_master_insert_refresh( int track ) { master_chain_rebuild( track ); }

int audio_app_master_insert_fader_node( int track )
{
    MasterStripChain* ch = master_chain( track );
    return ( ch && ch->fader ) ? (int)ch->fader : -1;
}

float audio_app_master_strip_peak( int track, int channel )
{
    // Metering follows the fader: with an external Amp the chain tail is what
    // the mixer channel receives, and the channel's own VU is already
    // post-everything -- so the channel VU is right in both configurations.
    // An AUX strip meters its bus's own return (see audio_app_master_aux_peak).
    if ( track <= -2 ) return audio_app_master_aux_peak( -2 - track, channel );
    // The master strip meters the mix bus output (post master gain).
    if ( track < 0 )
        return channel == 0 ? audio_app_mixer_master_vu_left( (int)g_masterMixerId )
                            : audio_app_mixer_master_vu_right( (int)g_masterMixerId );
    return channel == 0 ? audio_app_mixer_vu_left( (int)g_masterMixerId, track )
                        : audio_app_mixer_vu_right( (int)g_masterMixerId, track );
}

// ===========================================================================
//  AUX SENDS / AUX BUSES
//
//  TOPOLOGY (one bus, standard send/return):
//
//     track inlet --(send level, pre/post fader)--> MasterMixerNode
//        "AUX k SEND" out --> [bus insert chain: a reverb, ...] --> AuxBusNode
//        --> MasterMixerNode "AUX k RETURN" in --> the mix, ahead of the master
//        fader --> [master inserts] --> device
//
//  Three things make that work, and each is documented where it lives:
//
//   * the SEND MATRIX is in MasterMixerNode, because that is the only place
//     that has both the track's signal and its fader/mute/solo state -- which
//     is what "pre- or post-fader" is defined against;
//   * the BUS is a real AuxBusNode, so it is patchable in the modular view and
//     an ordinary insert chain wires in front of it (see below: an aux strip is
//     addressed in the audio_app_master_insert_* API as -2 - aux, which is what
//     audio_app_master_aux_strip() returns);
//   * the RETURN leg closes a loop back into the node that feeds the bus. That
//     is a cycle at node granularity and connect() rejects those, so the mixer's
//     return ports are FEEDBACK ports (Node::portIsFeedback): the edge carries
//     signal but no ordering, and the return arrives one block (~2.7 ms at
//     128/48k) late. That is the deliberate price for a send/return loop in a
//     topologically sorted graph, and it is inaudible on a reverb/delay send.
//
//  The aux state is keyed by the mixer's stable SLOT, never by the bus index
//  the API speaks in -- see the port-id note in patch_nodes.h.
// ===========================================================================
namespace {

//! Wire (or re-wire) bus `aux`: mixer send -> insert chain -> bus -> mixer
//! return.  master_chain_rebuild() owns the send-side half (it is the bus's
//! processor box); the return leg is drawn here.
void aux_wire( int aux )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    const int slot = aux_slot_of( aux );
    if ( !g_patch || !mm || slot < 0 || !g_auxNode[slot] ) return;
    const int busOut = node_audio_port( g_auxNode[slot], patch::PortDir::Out );
    if ( busOut >= 0 )
        g_patch->connect( patch::Connection{
            patch::PortRef{ g_auxNode[slot], (patch::PortId)busOut },
            patch::PortRef{ g_masterMixerId,
                            patch::MasterMixerNode::auxReturnPort( slot ) } } );
    master_chain_rebuild( audio_app_master_aux_strip( aux ) );   // compiles
}

} // namespace

int audio_app_master_aux_count()
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return mm ? mm->auxCount() : 0;
}

int audio_app_master_aux_add( const char* name )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !g_patch || !mm ) return -1;
    const int aux = mm->addAux();
    if ( aux < 0 ) return -1;                       // at capacity
    const int slot = mm->auxSlot( aux );
    if ( slot < 0 || slot >= kAuxSlots ) { mm->removeAux( aux ); return -1; }

    char fallback[32];
    if ( !name || !*name ) { std::snprintf( fallback, sizeof fallback, "AUX %d", slot + 1 );
                             name = fallback; }
    const patch::NodeId bus =
        g_patch->addNode( std::make_unique<patch::AuxBusNode>( name ) );
    if ( !bus ) { mm->removeAux( aux ); return -1; }   // graph at capacity
    g_auxNode[slot]  = bus;
    g_auxChain[slot] = MasterStripChain();
    aux_wire( aux );
    // A send bus only exists in the modular graph, so make sure that is the
    // path being rendered -- exactly what adding an insert already does.
    g_modular.store( true, std::memory_order_release );
    return aux;
}

bool audio_app_master_aux_remove( int aux )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    const int slot = aux_slot_of( aux );
    if ( !g_patch || !mm || slot < 0 || slot >= kAuxSlots ) return false;

    // The bus's processor box goes with it (the nodes belong to the strip).
    std::vector<patch::NodeId> dead;
    for ( size_t i = 0; i < g_auxChain[slot].slots.size(); ++i )
        dead.push_back( g_auxChain[slot].slots[i].node );
    if ( g_auxChain[slot].fader ) dead.push_back( g_auxChain[slot].fader );
    if ( g_auxNode[slot] ) dead.push_back( g_auxNode[slot] );
    g_auxChain[slot] = MasterStripChain();
    g_auxNode[slot]  = 0;
    for ( size_t i = 0; i < dead.size(); ++i ) {
        g_patch->disconnectAll( dead[i] );
        audio_app_patch_remove( (int)dead[i] );
    }
    // Only the INDEX list compacts -- every surviving bus keeps its ports, so
    // nothing else has to be re-wired.  The deleted bus's own cables are gone
    // with its node; prune anything still aimed at the retired ports.
    mm->removeAux( aux );
    g_patch->pruneDanglingConnections();
    g_patch->compileAndPublish();
    return true;
}

int audio_app_master_aux_name( int aux, char* buf, int buflen )
{
    if ( buf && buflen > 0 ) buf[0] = 0;
    patch::AuxBusNode* bus = aux_bus_node( aux );
    if ( !bus ) return 0;
    const std::string& nm = bus->name();
    if ( buf && buflen > 0 ) std::snprintf( buf, (size_t)buflen, "%s", nm.c_str() );
    return (int)nm.size();
}

bool audio_app_master_aux_set_name( int aux, const char* name )
{
    patch::AuxBusNode* bus = aux_bus_node( aux );
    if ( !bus ) return false;
    bus->setName( name ? name : "" );
    return true;
}

int audio_app_master_aux_node( int aux )
{
    const int slot = aux_slot_of( aux );
    if ( slot < 0 || slot >= kAuxSlots || !g_auxNode[slot] ) return -1;
    return (int)g_auxNode[slot];
}

float audio_app_master_aux_return_gain( int aux )
{
    patch::AuxBusNode* bus = aux_bus_node( aux );
    return bus ? bus->returnGain() : 1.0f;
}

bool audio_app_master_aux_set_return_gain( int aux, float g )
{
    patch::AuxBusNode* bus = aux_bus_node( aux );
    if ( !bus ) return false;
    bus->setReturnGain( g );
    return true;
}

bool audio_app_master_aux_mute( int aux )
{
    patch::AuxBusNode* bus = aux_bus_node( aux );
    return bus && bus->mute();
}

bool audio_app_master_aux_set_mute( int aux, int mute )
{
    patch::AuxBusNode* bus = aux_bus_node( aux );
    if ( !bus ) return false;
    bus->setMute( mute != 0 );
    return true;
}

float audio_app_master_aux_peak( int aux, int channel )
{
    patch::AuxBusNode* bus = aux_bus_node( aux );
    if ( !bus ) return 0.f;
    return channel == 0 ? bus->peakLeft() : bus->peakRight();
}

// ---- per-track sends -------------------------------------------------------
namespace {
//! A send only exists for a real AUDIO track: a MIDI strip has no signal to
//! copy, and letting the UI set one would silently do nothing.
bool send_addressable( patch::MasterMixerNode* mm, int track, int aux )
{
    return mm && track >= 0 && track < mm->trackCount() && !mm->trackIsMidi( track ) &&
           aux >= 0 && aux < mm->auxCount();
}
} // namespace

float audio_app_master_send_level( int track, int aux )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return send_addressable( mm, track, aux ) ? mm->sendLevel( track, aux ) : 0.f;
}

bool audio_app_master_set_send_level( int track, int aux, float level )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !send_addressable( mm, track, aux ) ) return false;
    mm->setSendLevel( track, aux, level );
    return true;
}

bool audio_app_master_send_prefader( int track, int aux )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return send_addressable( mm, track, aux ) && mm->sendPreFader( track, aux );
}

bool audio_app_master_set_send_prefader( int track, int aux, int pre )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !send_addressable( mm, track, aux ) ) return false;
    mm->setSendPreFader( track, aux, pre != 0 );
    return true;
}

bool audio_app_master_send_enabled( int track, int aux )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return send_addressable( mm, track, aux ) && mm->sendEnabled( track, aux );
}

bool audio_app_master_set_send_enabled( int track, int aux, int on )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !send_addressable( mm, track, aux ) ) return false;
    mm->setSendEnabled( track, aux, on != 0 );
    return true;
}

// ---- SOLO (real engine state; see patch_nodes.h) ---------------------------
bool audio_app_master_solo( int track )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return mm && track >= 0 && track < mm->trackCount() && mm->solo( track );
}

bool audio_app_master_set_solo( int track, int on )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !mm || track < 0 || track >= mm->trackCount() ) return false;
    mm->setSolo( track, on != 0 );
    // A strip whose fader was lifted into a TrackFaderNode applies mute/solo
    // upstream, so push the new state onto every Amp.
    for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t )
        master_chain_sync_fader( t );
    return true;
}

int audio_app_master_solo_count()
{
    patch::MasterMixerNode* mm = master_mixer_node();
    return mm ? mm->soloCount() : 0;
}

bool audio_app_master_audible( int track )
{
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !mm || track < 0 || track >= mm->trackCount() ) return false;
    return !mm->channelSilenced( track );
}


bool audio_app_patch_set_node_bypass( int node, int bypass )
{
    if ( !g_patch ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( !n ) return false;
    n->setBypass( bypass != 0 );
    return true;
}

bool audio_app_patch_node_bypass( int node )
{
    if ( !g_patch ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    return n ? n->bypass() : false;
}

// ---- strip input / output routing (Ardour MixerStrip input/output buttons) --
int audio_app_master_strip_input( int track )
{
    if ( !g_patch ) return -1;
    MasterStripChain* ch = master_chain( track );
    if ( ch && !ch->sources.empty() ) return (int)ch->sources.front().node;
    patch::NodeId sinkNode = 0; int sinkPort = -1;
    if ( !master_chain_sink( track, sinkNode, sinkPort ) ) return -1;
    const std::vector<patch::Connection> conns = g_patch->connections();
    for ( size_t i = 0; i < conns.size(); ++i )
        if ( conns[i].to.node == sinkNode && (int)conns[i].to.port == sinkPort )
            return (int)conns[i].from.node;
    return -1;
}

int audio_app_master_strip_output( int track )
{
    if ( !g_patch || track < 0 ) return -1;
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !mm || track >= mm->trackCount() || mm->trackIsMidi( track ) ) return -1;
    const int outlet = 3 + 2 * track;
    const std::vector<patch::Connection> conns = g_patch->connections();
    for ( size_t i = 0; i < conns.size(); ++i )
        if ( conns[i].from.node == g_masterMixerId && (int)conns[i].from.port == outlet )
            return (int)conns[i].to.node;
    return -1;
}

bool audio_app_master_strip_set_output( int track, int destNode )
{
    if ( !g_patch || track < 0 ) return false;
    patch::MasterMixerNode* mm = master_mixer_node();
    if ( !mm || track >= mm->trackCount() || mm->trackIsMidi( track ) ) return false;
    const int outlet = 3 + 2 * track;
    const std::vector<patch::Connection> conns = g_patch->connections();
    for ( size_t i = 0; i < conns.size(); ++i )
        if ( conns[i].from.node == g_masterMixerId && (int)conns[i].from.port == outlet )
            g_patch->disconnect( conns[i] );
    bool ok = true;
    if ( destNode >= 0 ) {
        const int in = node_audio_port( (patch::NodeId)destNode, patch::PortDir::In );
        ok = in >= 0 && g_patch->connect( patch::Connection{
                 patch::PortRef{ g_masterMixerId, (patch::PortId)outlet },
                 patch::PortRef{ (patch::NodeId)destNode, (patch::PortId)in } } );
    }
    g_patch->compileAndPublish();
    return ok;
}

int audio_app_patch_node_ids( int* buf, int cap )
{
    if ( !g_patch ) return 0;
    const std::vector<patch::NodeId> ids = g_patch->nodeIds();
    const int n = (int)ids.size();
    if ( buf && cap > 0 )
        for ( int i = 0; i < n && i < cap; ++i ) buf[i] = (int)ids[(size_t)i];
    return n;
}

int audio_app_patch_node_name( int node, char* buf, int buflen )
{
    if ( !g_patch || !buf || buflen <= 0 ) return 0;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( !n ) { buf[0] = 0; return 0; }
    std::string nm;
    if ( patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n ) ) {
        if ( IPluginInstance* inst = pn->instance() ) nm = inst->descriptor().name;
    }
    if ( nm.empty() ) nm = n->typeName() ? n->typeName() : "";
    std::snprintf( buf, (size_t)buflen, "%s", nm.c_str() );
    return (int)nm.size();
}

static std::map<const AudioClip*, std::weak_ptr<AudioClip>>& project_clip_cache();

void audio_app_project_clear_audio_clips()
{
    // Drop the source-identity cache at the restore boundary.  Keying on the
    // caller's ADDRESS is only valid within one restore: a later load can put a
    // different source clip at the same address while the previous one is still
    // referenced, and reusing that entry would attach the OLD project's audio to
    // the NEW project's regions.  The weak_ptr guards lifetime, not identity --
    // this is what guards identity.
    project_clip_cache().clear();
    bool changed = false;
    for ( int t = 0; t < AUDIO_APP_MAX_TRACKS; ++t )
        if ( g_projectAudioPlayers[t] )
        {
            g_projectAudioPlayers[t]->clearWarp( nullptr );
            g_projectAudioPlayers[t]->clearClips();
            changed = true;
        }
    if ( g_graph )
        for ( int t = 0; t < AUDIO_APP_MAX_TRACKS; ++t )
            if ( g_projectAudioPlayers[t] )
                if ( Track* trk = g_graph->track( t ) )
                    if ( trk->instrument() == g_projectAudioPlayers[t] )
                        trk->setInstrument( nullptr );
    if ( changed ) freeze_rcu_grace();
    for ( int t = 0; t < AUDIO_APP_MAX_TRACKS; ++t ) {
        AudioClipPlayer* p=g_projectAudioPlayers[t];
        if(!p)continue;
        auto owned=std::find(g_owned.begin(),g_owned.end(),p);
        if(owned!=g_owned.end())g_owned.erase(owned);
        p->release();delete p;g_projectAudioPlayers[t]=nullptr;
    }
    g_projectAudioClips.clear();
}

// Defined with the AutoFades preference below; every AudioClipPlayer creation
// site calls it so a new player starts with the current AutoFade length.
static void applyAutoFadeTo( AudioClipPlayer* p );


//! Store ONE copy per distinct source clip.
//!
//! Project load calls the add-region entry points once per REGION, and each call
//! used to `new AudioClip(clip)` -- so ten slices of one ten-minute file became
//! ten 230 MB copies (2.3 GB, and seconds of memcpy on the message thread)
//! restoring a project whose file on disk holds the audio exactly once.  The
//! caller hands us the same source object for every region cut from it, so the
//! address identifies the source for the duration of one restore.
static std::map<const AudioClip*, std::weak_ptr<AudioClip>>& project_clip_cache()
{
    static std::map<const AudioClip*, std::weak_ptr<AudioClip>> s_seen;
    return s_seen;
}

static std::shared_ptr<AudioClip> project_store_clip( const AudioClip& clip )
{
    auto& s_seen = project_clip_cache();
    auto it = s_seen.find( &clip );
    if ( it != s_seen.end() ) {
        if ( std::shared_ptr<AudioClip> alive = it->second.lock() ) return alive;
        s_seen.erase( it );
    }
    // A cache miss COPIES the clip into engine-owned storage.  This used to
    // recurse into itself (the miss path called project_store_clip again with
    // the same key), which was an unconditional stack overflow on the very
    // first clip stored -- every add_audio_clip/add_audio_region call crashed.
    std::shared_ptr<AudioClip> stored = std::make_shared<AudioClip>( clip );
    s_seen[&clip] = stored;
    return stored;
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
        applyAutoFadeTo( player );
        g_projectAudioPlayers[track] = player;
        trk->setInstrument( player );
    }

    std::shared_ptr<AudioClip> stored = project_store_clip( clip );
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
                                         float fadeInK, float fadeOutK,
                                         long long loopLength,
                                         int fadeInShape, int fadeOutShape,
                                         int fadeInSlope, int fadeOutSlope,
                                         int xfadeLink )
{
    if ( !audio_app_project_add_audio_clip( track, clip, startSample, gain ) ) return false;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player ) return false;
    const int idx = player->clipCount() - 1;           // the clip just appended
    if ( idx < 0 ) return false;
    // LOOP FIRST: setClipRegion used to clamp `length` into the source whenever
    // the region was not looping, so a looped region longer than its source was
    // truncated before its loop flag was ever set.
    player->setClipLoop( idx, loop != 0 );
    // setClipLoop() captures the region's CURRENT trimmed span as the period,
    // so the saved one has to be put back after it, and only when there is one
    // (0 == the whole source, the pre-v17 default).
    if ( loop != 0 && loopLength > 0 ) player->setClipLoopLength( idx, loopLength );
    player->setClipRegion( idx, startSample, sourceOffset, length );
    player->setClipMuted( idx, muted != 0 );
    if ( fadeIn > 0 || fadeOut > 0 )
        player->setClipFades( idx, fadeIn, fadeOut, fadeInK, fadeOutK );
    if ( fadeInShape || fadeOutShape || fadeInSlope || fadeOutSlope || xfadeLink )
        player->setClipFadeShapes( idx, fadeInShape, fadeOutShape,
                                   fadeInSlope, fadeOutSlope, xfadeLink );
    return true;
}

namespace {
void projectCollectOrphanClips()
{
    std::set<const AudioClip*> live;
    for(int t=0;t<AUDIO_APP_MAX_TRACKS;++t)
        if(AudioClipPlayer* p=g_projectAudioPlayers[t])
            for(int i=0;i<p->clipCount();++i)
                if(const AudioClip* c=p->clipAt(i).clip)live.insert(c);
    g_projectAudioClips.erase(
        std::remove_if(g_projectAudioClips.begin(),g_projectAudioClips.end(),
            [&](const ProjectAudioClipEntry& e){return !e.clip||!live.count(e.clip.get());}),
        g_projectAudioClips.end());
}
}

bool audio_app_project_remove_audio_clip(int track,const AudioClip* clip)
{
    if(track<0||track>=AUDIO_APP_MAX_TRACKS||!clip)return false;
    AudioClipPlayer* p=g_projectAudioPlayers[track];if(!p)return false;
    bool removed=false;
    // One call removes one REGION. Several non-destructive slices deliberately
    // share the same source pointer and must not be erased as a group.
    for(int i=0;i<p->clipCount();++i)
        if(p->clipAt(i).clip==clip){p->removeClip(i);removed=true;break;}
    if(!removed)return false;
    // removeClip publishes a new immutable schedule. Wait until no render block
    // can still hold the old snapshot before freeing its source buffer.
    freeze_rcu_grace();
    projectCollectOrphanClips();
    if(p->clipCount()==0) {
        if(g_graph)if(Track* trk=g_graph->track(track))
            if(trk->instrument()==p)trk->setInstrument(nullptr);
        auto owned=std::find(g_owned.begin(),g_owned.end(),p);
        if(owned!=g_owned.end())g_owned.erase(owned);
        p->release();delete p;g_projectAudioPlayers[track]=nullptr;
    }
    return true;
}

bool audio_app_project_partition_track( int track, long long cutStart, long long cutEnd )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || cutEnd <= cutStart )
        return false;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player ) return true;

    std::vector<ScheduledClip> rebuilt;
    rebuilt.reserve( (size_t)player->clipCount() + 1 );
    for ( int i = 0; i < player->clipCount(); ++i )
    {
        const ScheduledClip sc = player->clipAt( i );
        const int64_t length = sc.regionLength();
        const int64_t begin = sc.startSample;
        const int64_t end = begin + length;
        if ( length <= 0 || end <= cutStart || begin >= cutEnd )
        {
            rebuilt.push_back( sc );
            continue;
        }
        // Preserve the left child {P,S,B}.  It keeps the parent's regionId --
        // the caller's stable handle keeps addressing the piece that stayed
        // where the region used to start.
        if ( begin < cutStart )
        {
            ScheduledClip left = sc;
            left.length = cutStart - begin;
            left.fadeOutFrames = 0;
            rebuilt.push_back( left );
        }
        // Preserve the right child {P+B,S+B,L-B}; this is Ardour's
        // Playlist partition law and leaves the source itself untouched.
        if ( end > cutEnd )
        {
            ScheduledClip right = sc;
            const int64_t consumed = cutEnd - begin;
            right.startSample = cutEnd;
            right.sourceOffset += consumed;
            right.length = end - cutEnd;
            right.fadeInFrames = 0;
            // A region split in two cannot give ONE id to both children: the
            // right child is a NEW region and gets a fresh id in setSchedule.
            if ( begin < cutStart ) right.regionId = 0;
            rebuilt.push_back( right );
        }
    }

    // One atomic republish that PRESERVES the surviving regions' stable ids
    // (the old clear-and-re-add path minted new ids for everything, orphaning
    // every handle a caller held across the partition).
    if ( !player->setSchedule( rebuilt ) ) return false;
    freeze_rcu_grace();
    projectCollectOrphanClips();
    return true;
}

// --- punch recording (PT ch.27) ---------------------------------------------

const AudioClip* audio_app_project_own_clip( int track, const AudioClip& clip )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || clip.empty() ) return nullptr;
    ProjectAudioClipEntry entry;
    entry.track = track;
    entry.clip = std::shared_ptr<AudioClip>( new AudioClip( clip ) );
    const AudioClip* raw = entry.clip.get();
    g_projectAudioClips.push_back( std::move( entry ) );
    return raw;
}

bool audio_app_project_add_region_shared( int track, const AudioClip* source,
                                          long long startSample, long long sourceOffset,
                                          long long length, float gain,
                                          long long fadeInFrames, long long fadeOutFrames )
{
    if ( !g_graph || track < 0 || track >= AUDIO_APP_MAX_TRACKS || !source )
        return false;
    // The source must be one the project store owns: a raw pointer from
    // anywhere else would dangle the moment its real owner freed it.
    bool owned = false;
    for ( const auto& e : g_projectAudioClips )
        if ( e.clip.get() == source ) { owned = true; break; }
    if ( !owned ) return false;
    Track* trk = g_graph->track( track );
    if ( !trk ) return false;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player )
    {
        player = new AudioClipPlayer();
        if ( !player->prepare( g_sr, g_block ) ) { delete player; return false; }
        player->setActive( true );
        g_owned.push_back( player );
        applyAutoFadeTo( player );
        g_projectAudioPlayers[track] = player;
        trk->setInstrument( player );
    }
    if ( !player->addClip( source, startSample, gain ) ) return false;
    const int idx = player->clipCount() - 1;
    player->setClipRegion( idx, startSample, sourceOffset, length );
    if ( fadeInFrames > 0 || fadeOutFrames > 0 )
        player->setClipFades( idx, fadeInFrames, fadeOutFrames, 0.f, 0.f );
    return true;
}

bool audio_app_project_set_boundary_fades( int track, long long inSample,
                                           long long outSample, long long fadeFrames )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || fadeFrames <= 0 ) return false;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player ) return false;
    bool touched = false;
    for ( int i = 0; i < player->clipCount(); ++i )
    {
        const ScheduledClip sc = player->clipAt( i );
        const int64_t end = sc.startSample + sc.regionLength();
        // Pre-crossfade: the OUTGOING material fades out up to (not into) the
        // punch boundary.  Post-crossfade: the returning material fades back in
        // after the punch.  Exact-sample matches only -- partition cut there.
        if ( end == inSample )
        {
            player->setClipFades( i, sc.fadeInFrames, fadeFrames,
                                  sc.fadeInTension, 0.f );
            touched = true;
        }
        if ( sc.startSample == outSample )
        {
            player->setClipFades( i, fadeFrames, sc.fadeOutFrames,
                                  0.f, sc.fadeOutTension );
            touched = true;
        }
    }
    return touched;
}

int audio_app_project_hold_mute( int track, int on )
{
    // regionIds this hold muted, per track, so OFF restores exactly the regions
    // ON silenced -- a region the user had muted before the punch stays muted.
    static std::map<int, std::vector<uint64_t>> s_punchHoldMuted;
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return 0;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player ) { if ( !on ) s_punchHoldMuted.erase( track ); return 0; }
    int n = 0;
    if ( on )
    {
        auto& held = s_punchHoldMuted[track];
        for ( int i = 0; i < player->clipCount(); ++i )
        {
            const ScheduledClip sc = player->clipAt( i );
            if ( sc.muted ) continue;
            player->setClipMuted( i, true );
            held.push_back( sc.regionId );
            ++n;
        }
    }
    else
    {
        auto it = s_punchHoldMuted.find( track );
        if ( it == s_punchHoldMuted.end() ) return 0;
        for ( uint64_t id : it->second )
        {
            const int i = player->regionIndex( id );
            if ( i >= 0 ) { player->setClipMuted( i, false ); ++n; }
        }
        s_punchHoldMuted.erase( it );
    }
    return n;
}

bool audio_app_project_dp_eligible( int track, long long minFrames,
                                    char* reasonBuf, int reasonCap )
{
    auto say = [&]( const char* msg ) {
        if ( reasonBuf && reasonCap > 0 ) snprintf( reasonBuf, (size_t)reasonCap, "%s", msg );
        return false;
    };
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS )
        return say( "no such track" );
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player || player->clipCount() == 0 )
        return say( "track has no audio - use Prepare DPE Tracks to render a file" );
    if ( player->clipCount() != 1 )
        return say( "track audio is not one contiguous file - use Prepare DPE Tracks (or Consolidate)" );
    const ScheduledClip sc = player->clipAt( 0 );
    if ( sc.startSample != 0 || sc.effectiveSourceOffset() != 0 )
        return say( "track audio does not start at the session start - move it to bar 1 or use Prepare DPE Tracks" );
    if ( sc.regionLength() < minFrames )
        return say( "track file is shorter than the DestructivePunch File Length preference - lower the preference or use Prepare DPE Tracks" );
    if ( sc.gain != 1.0f )
        return say( "clip gain is not 0 dB - use Prepare DPE Tracks to render it in" );
    if ( sc.loop )
        return say( "track region is looped - use Prepare DPE Tracks to render it out" );
    return true;
}

bool audio_app_project_consolidate_track( int track, long long lengthFrames,
                                          const char* name )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || lengthFrames <= 0 )
        return false;
    if ( !g_graph || !g_graph->track( track ) ) return false;
    AudioClip flat;
    flat.name = name ? name : "DPE";
    flat.sampleRate = flat.sourceSampleRate = g_sr;
    // A DP file is minutes of stereo floats held in RAM; a failed allocation
    // must come back as "no", not as a crash mid-command.
    try { flat.resize( lengthFrames ); }
    catch ( const std::bad_alloc& ) { return false; }
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( player )
    {
        // Render every scheduled region (gain + fade envelope + loop wrap,
        // exactly the fields the player's own render honours) into the flat
        // buffer.  Overlapping regions sum, as they do at playback.
        for ( int i = 0; i < player->clipCount(); ++i )
        {
            const ScheduledClip sc = player->clipAt( i );
            if ( sc.muted || !sc.clip ) continue;
            const int64_t regLen = sc.regionLength();
            const int64_t off = sc.effectiveSourceOffset();
            const int64_t n = sc.clip->safeFrames();
            int64_t loopSpan = n - off;
            if ( sc.loop && sc.loopLength > 0 && sc.loopLength < loopSpan )
                loopSpan = sc.loopLength;
            if ( loopSpan <= 0 ) continue;
            for ( int64_t ri = 0; ri < regLen; ++ri )
            {
                const int64_t pos = sc.startSample + ri;
                if ( pos < 0 || pos >= lengthFrames ) continue;
                int64_t src = sc.loop ? off + ( ri % loopSpan ) : off + ri;
                if ( src < 0 || src >= n ) continue;
                const float g = sc.gain * sc.fadeGain( ri, regLen );
                flat.ch[0][(size_t)pos] += sc.clip->ch[0][(size_t)src] * g;
                flat.ch[1][(size_t)pos] += sc.clip->ch[1][(size_t)src] * g;
            }
        }
        player->clearClips();
        freeze_rcu_grace();
        projectCollectOrphanClips();
    }
    // One contiguous region at sample 0, unity gain: exactly the shape
    // audio_app_project_dp_eligible demands.
    return audio_app_project_add_audio_clip( track, flat, 0, 1.f );
}

bool audio_app_project_destructive_punch( int track, long long destSample,
                                          const AudioClip& take,
                                          long long takeOffset, long long frames,
                                          long long xfadeFrames )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || frames <= 0 ) return false;
    AudioClipPlayer* player = g_projectAudioPlayers[track];
    if ( !player ) return false;
    // The region under the punch; DP eligibility guarantees there is exactly
    // one, but find it honestly rather than assuming index 0.
    for ( int i = 0; i < player->clipCount(); ++i )
    {
        const ScheduledClip sc = player->clipAt( i );
        const int64_t regLen = sc.regionLength();
        if ( !sc.clip || destSample < sc.startSample ||
             destSample >= sc.startSample + regLen )
            continue;
        // Mutable access to the stored source via the owning shared_ptr.
        AudioClip* dst = nullptr;
        for ( auto& e : g_projectAudioClips )
            if ( e.clip.get() == sc.clip ) { dst = e.clip.get(); break; }
        if ( !dst ) return false;
        const int64_t srcBase = sc.effectiveSourceOffset() + ( destSample - sc.startSample );
        const int64_t nDst = dst->safeFrames();
        const int64_t nTake = take.safeFrames();
        int64_t n = frames;
        if ( srcBase + n > nDst )  n = nDst - srcBase;         // stop at file end
        if ( takeOffset + n > nTake ) n = nTake - takeOffset;  // and at take end
        if ( n <= 0 ) return false;
        int64_t xf = xfadeFrames < 0 ? 0 : xfadeFrames;
        if ( xf * 2 > n ) xf = n / 2;                          // tiny punches
        for ( int64_t k = 0; k < n; ++k )
        {
            // Linear ramps at both ends; the shorter of the two wins so a
            // punch a hair over 2*xf long still reaches unity in the middle.
            float w = 1.f;
            if ( xf > 0 )
            {
                const float wi = (float)( k + 1 ) / (float)( xf + 1 );
                const float wo = (float)( n - k ) / (float)( xf + 1 );
                w = wi < 1.f ? wi : 1.f;
                if ( wo < w ) w = wo;
            }
            for ( int c = 0; c < 2; ++c )
            {
                float& d = dst->ch[c][(size_t)( srcBase + k )];
                d = d * ( 1.f - w ) + take.ch[c][(size_t)( takeOffset + k )] * w;
            }
        }
        return true;
    }
    return false;
}

const AudioClip* audio_app_project_clip_on_track( int track )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return nullptr;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p || p->clipCount() < 1 ) return nullptr;
    return p->clipAt( 0 ).clip;
}

const AudioClip* audio_app_project_last_clip_on_track( int track )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return nullptr;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p || p->clipCount() < 1 ) return nullptr;
    return p->clipAt( p->clipCount() - 1 ).clip;
}

namespace {
int projectRegionIndex( int track, const AudioClip* clip )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || !clip ) return -1;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p ) return -1;
    for ( int i = 0; i < p->clipCount(); ++i )
        if ( p->clipAt( i ).clip == clip ) return i;
    return -1;
}
}

bool audio_app_project_find_region( int track, const AudioClip* clip,
                                    long long* startSample, long long* sourceOffset,
                                    long long* length, float* gain, int* muted, int* loop,
                                    long long* loopLength,
                                    long long* fadeIn, long long* fadeOut,
                                    float* fadeInK, float* fadeOutK,
                                    int* fadeInShape, int* fadeOutShape,
                                    int* fadeInSlope, int* fadeOutSlope,
                                    int* xfadeLink )
{
    const int i = projectRegionIndex( track, clip );
    AudioClipPlayer* p = (track >= 0 && track < AUDIO_APP_MAX_TRACKS)
                       ? g_projectAudioPlayers[track] : nullptr;
    if ( i < 0 || !p ) return false;
    const ScheduledClip* sc = p->scheduled( i );
    if ( !sc ) return false;
    if ( startSample )  *startSample = sc->startSample;
    if ( sourceOffset ) *sourceOffset = sc->sourceOffset;
    if ( length )       *length = sc->regionLength();
    if ( gain )         *gain = sc->gain;
    if ( muted )        *muted = sc->muted ? 1 : 0;
    if ( loop )         *loop = sc->loop ? 1 : 0;
    if ( loopLength )   *loopLength = sc->loopLength;
    if ( fadeIn )       *fadeIn  = sc->fadeInFrames;
    if ( fadeOut )      *fadeOut = sc->fadeOutFrames;
    if ( fadeInK )      *fadeInK  = sc->fadeInTension;
    if ( fadeOutK )     *fadeOutK = sc->fadeOutTension;
    if ( fadeInShape )  *fadeInShape  = sc->fadeInShape;
    if ( fadeOutShape ) *fadeOutShape = sc->fadeOutShape;
    if ( fadeInSlope )  *fadeInSlope  = sc->fadeInSlope;
    if ( fadeOutSlope ) *fadeOutSlope = sc->fadeOutSlope;
    if ( xfadeLink )    *xfadeLink = sc->xfadeLink;
    return true;
}

bool audio_app_project_set_region( int track, const AudioClip* clip,
                                   long long startSample, long long sourceOffset,
                                   long long length )
{
    const int i = projectRegionIndex( track, clip );
    return i >= 0 && g_projectAudioPlayers[track]->setClipRegion(
        i, startSample, sourceOffset, length );
}

bool audio_app_project_set_fades( int track, const AudioClip* clip,
                                  long long inFrames, long long outFrames,
                                  float inK, float outK )
{
    const int i = projectRegionIndex( track, clip );
    return i >= 0 && g_projectAudioPlayers[track]->setClipFades(i,inFrames,outFrames,inK,outK);
}

bool audio_app_project_set_fade_shapes( int track, const AudioClip* clip,
                                        int inShape, int outShape,
                                        int inSlope, int outSlope, int link )
{
    const int i = projectRegionIndex( track, clip );
    return i >= 0 && g_projectAudioPlayers[track]->setClipFadeShapes(
        i, inShape, outShape, inSlope, outSlope, link );
}

// ---- AutoFades (PT p752) ---------------------------------------------------
// One global preference (0..10 ms), pushed into every audio clip player.  All
// players ever created are in g_owned, so one walk reaches project + freeze
// players alike; applyAutoFadeTo() also runs at each creation site so a player
// born after the preference was set starts with it.
static double g_autoFadeMs = 0.0;

static void applyAutoFadeTo( AudioClipPlayer* p )
{
    if ( p ) p->setAutoFadeFrames( (int64_t)( g_autoFadeMs * 0.001 * g_sr + 0.5 ) );
}

void audio_app_set_auto_fade_ms( double ms )
{
    if ( ms < 0.0 )  ms = 0.0;
    if ( ms > 10.0 ) ms = 10.0;
    g_autoFadeMs = ms;
    for ( IPluginInstance* inst : g_owned )
        applyAutoFadeTo( dynamic_cast<AudioClipPlayer*>( inst ) );
}

double audio_app_auto_fade_ms() { return g_autoFadeMs; }

bool audio_app_project_set_gain( int track, const AudioClip* clip, float gain )
{ const int i=projectRegionIndex(track,clip); return i>=0&&g_projectAudioPlayers[track]->setClipGain(i,gain); }
bool audio_app_project_set_muted( int track, const AudioClip* clip, bool muted )
{ const int i=projectRegionIndex(track,clip); return i>=0&&g_projectAudioPlayers[track]->setClipMuted(i,muted); }
bool audio_app_project_set_loop( int track, const AudioClip* clip, bool loop )
{ const int i=projectRegionIndex(track,clip); return i>=0&&g_projectAudioPlayers[track]->setClipLoop(i,loop); }

bool audio_app_project_set_loop_length( int track, const AudioClip* clip,
                                        long long frames )
{
    const int i = projectRegionIndex( track, clip );
    return i >= 0 && g_projectAudioPlayers[track]->setClipLoopLength( i, frames );
}

// ---- STABLE REGION IDENTITY -------------------------------------------------
// projectRegionIndex() above matches by clip POINTER and returns the first hit,
// so two slices of one shared source were indistinguishable -- an edit aimed at
// the second region landed on (or silently missed) the first.  These entry
// points address a region by the per-track stable id the player assigned when
// the region was scheduled.
namespace {
int projectRegionIndexById( int track, unsigned long long id )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS || id == 0 ) return -1;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    return p ? p->regionIndex( (uint64_t)id ) : -1;
}
}

int audio_app_project_region_count( int track )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return 0;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    return p ? p->clipCount() : 0;
}

unsigned long long audio_app_project_region_id_at( int track, int index )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return 0;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p || index < 0 || index >= p->clipCount() ) return 0;
    return (unsigned long long) p->clipAt( index ).regionId;
}

unsigned long long audio_app_project_last_region_id( int track )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return 0;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p || p->clipCount() < 1 ) return 0;
    return (unsigned long long) p->clipAt( p->clipCount() - 1 ).regionId;
}

unsigned long long audio_app_project_region_id_find( int track, const AudioClip* clip,
                                                     long long startSample )
{
    if ( track < 0 || track >= AUDIO_APP_MAX_TRACKS ) return 0;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    if ( !p ) return 0;
    for ( int i = 0; i < p->clipCount(); ++i )
    {
        const ScheduledClip sc = p->clipAt( i );
        if ( ( !clip || sc.clip == clip ) && sc.startSample == startSample )
            return (unsigned long long) sc.regionId;
    }
    return 0;
}

const AudioClip* audio_app_project_region_clip( int track, unsigned long long id )
{
    const int i = projectRegionIndexById( track, id );
    if ( i < 0 ) return nullptr;
    return g_projectAudioPlayers[track]->clipAt( i ).clip;
}

bool audio_app_project_find_region_by_id( int track, unsigned long long id,
                                          long long* startSample, long long* sourceOffset,
                                          long long* length, float* gain, int* muted, int* loop,
                                          long long* loopLength,
                                          long long* fadeIn, long long* fadeOut,
                                          float* fadeInK, float* fadeOutK,
                                          int* fadeInShape, int* fadeOutShape,
                                          int* fadeInSlope, int* fadeOutSlope,
                                          int* xfadeLink )
{
    const int i = projectRegionIndexById( track, id );
    if ( i < 0 ) return false;
    const ScheduledClip* sc = g_projectAudioPlayers[track]->scheduled( i );
    if ( !sc ) return false;
    if ( startSample )  *startSample = sc->startSample;
    if ( sourceOffset ) *sourceOffset = sc->sourceOffset;
    if ( length )       *length = sc->regionLength();
    if ( gain )         *gain = sc->gain;
    if ( muted )        *muted = sc->muted ? 1 : 0;
    if ( loop )         *loop = sc->loop ? 1 : 0;
    if ( loopLength )   *loopLength = sc->loopLength;
    if ( fadeIn )       *fadeIn  = sc->fadeInFrames;
    if ( fadeOut )      *fadeOut = sc->fadeOutFrames;
    if ( fadeInK )      *fadeInK  = sc->fadeInTension;
    if ( fadeOutK )     *fadeOutK = sc->fadeOutTension;
    if ( fadeInShape )  *fadeInShape  = sc->fadeInShape;
    if ( fadeOutShape ) *fadeOutShape = sc->fadeOutShape;
    if ( fadeInSlope )  *fadeInSlope  = sc->fadeInSlope;
    if ( fadeOutSlope ) *fadeOutSlope = sc->fadeOutSlope;
    if ( xfadeLink )    *xfadeLink = sc->xfadeLink;
    return true;
}

bool audio_app_project_set_region_by_id( int track, unsigned long long id,
                                         long long startSample, long long sourceOffset,
                                         long long length )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipRegion(
        i, startSample, sourceOffset, length );
}

bool audio_app_project_set_fades_by_id( int track, unsigned long long id,
                                        long long inFrames, long long outFrames,
                                        float inK, float outK )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipFades( i, inFrames, outFrames,
                                                                 inK, outK );
}

bool audio_app_project_set_fade_shapes_by_id( int track, unsigned long long id,
                                              int inShape, int outShape,
                                              int inSlope, int outSlope, int link )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipFadeShapes(
        i, inShape, outShape, inSlope, outSlope, link );
}

bool audio_app_project_set_gain_by_id( int track, unsigned long long id, float gain )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipGain( i, gain );
}

bool audio_app_project_set_muted_by_id( int track, unsigned long long id, bool muted )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipMuted( i, muted );
}

bool audio_app_project_set_loop_by_id( int track, unsigned long long id, bool loop )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipLoop( i, loop );
}

bool audio_app_project_set_loop_length_by_id( int track, unsigned long long id,
                                              long long frames )
{
    const int i = projectRegionIndexById( track, id );
    return i >= 0 && g_projectAudioPlayers[track]->setClipLoopLength( i, frames );
}

bool audio_app_project_remove_region_by_id( int track, unsigned long long id )
{
    const int i = projectRegionIndexById( track, id );
    if ( i < 0 ) return false;
    AudioClipPlayer* p = g_projectAudioPlayers[track];
    p->removeClip( i );
    // Same teardown sequence as audio_app_project_remove_audio_clip: wait out
    // any render block still on the old snapshot, then free sources nothing
    // references, then retire an empty player.
    freeze_rcu_grace();
    projectCollectOrphanClips();
    if ( p->clipCount() == 0 )
    {
        if ( g_graph )
            if ( Track* trk = g_graph->track( track ) )
                if ( trk->instrument() == p ) trk->setInstrument( nullptr );
        auto owned = std::find( g_owned.begin(), g_owned.end(), p );
        if ( owned != g_owned.end() ) g_owned.erase( owned );
        p->release(); delete p; g_projectAudioPlayers[track] = nullptr;
    }
    return true;
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

    master_chain_reset_all();       // every processor box goes with the project

    std::vector<patch::NodeId> ids = g_patch->nodeIds();
    for ( size_t i = 0; i < ids.size(); ++i )
        if ( ids[i] != g_patchOutId && ids[i] != g_patchMidiInId &&
             ids[i] != g_defaultHwMidiInId && ids[i] != g_virtualMidiId &&
             ids[i] != g_masterMixerId )
            audio_app_patch_remove( (int) ids[i] );

    patch::MasterMixerNode* master =
        dynamic_cast<patch::MasterMixerNode*>( g_patch->node( g_masterMixerId ) );
    if ( master )
    {
        while ( master->trackCount() > 0 ) master->removeTrack( master->trackCount() - 1 );
        // The AuxBusNodes were swept away with the rest of the user patch above;
        // drop the mixer's send/return PORTS with them or the next project
        // starts life with phantom buses wired to nothing.
        while ( master->auxCount() > 0 ) master->removeAux( 0 );
        for ( int t = 0; t < PatchKnob::app::AUDIO_APP_MAX_TRACKS; ++t )
            master->setSolo( t, false );
        master->setMasterGain( 1.0f );
        master->setBypass(false);
    }

    // Singleton nodes survive project replacement, so reset their mutable
    // project state explicitly instead of leaking it into the next song.
    if(g_midiInNode){g_midiInNode->flush();g_midiInNode->setTrackPorts(1);
                     g_midiInNode->setOutChannel(-1);g_midiInNode->setBypass(false);}
    if(auto* hw=dynamic_cast<patch::MidiInNode*>(g_patch->node(g_defaultHwMidiInId)))
        {hw->flush();hw->setTrackPorts(1);hw->setOutChannel(-1);hw->setBypass(false);}
    if(auto* vm=dynamic_cast<patch::VirtualMidiPortsNode*>(g_patch->node(g_virtualMidiId)))
        {vm->setPorts(1,1);vm->setRoute(0,-1);vm->setBypass(false);}
    if(auto* out=g_patch->node(g_patchOutId))out->setBypass(false);

    std::vector<patch::Connection> conns = g_patch->connections();
    for ( size_t i = 0; i < conns.size(); ++i )
        g_patch->disconnect( conns[i] );
    g_patch->connect( patch::Connection{ patch::PortRef{g_masterMixerId,0},
                                         patch::PortRef{g_patchOutId,0} } );
    g_patch->compileAndPublish();
    g_modular.store( false );
}

// --- transport live MIDI record (into a timeline clip; message thread) -------
void audio_app_record_arm_at_tick( bool on, long long startTick )
{
    if ( on )
    {
        g_recStartTick.store( std::max<long long>( 0, startTick ),
                              std::memory_order_relaxed );
    }
    g_recArm.store( on, std::memory_order_release );
}
bool      audio_app_record_armed()      { return g_recArm.load( std::memory_order_acquire ); }
long long audio_app_record_start_tick() { return g_recStartTick.load( std::memory_order_relaxed ); }

// Drain captured live events; `ticks` come back RELATIVE to the record start
// (clamped to >= 0).  Returns the count.
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
    else if ( k == "mono2stereo" ) n = std::make_unique<patch::MonoToStereoNode>();
    else if ( k == "sum"    ) n = std::make_unique<patch::SumNode>();
    else if ( k == "in"     ) {
        const unsigned granted = g_engine ? g_engine->inputChannels() : 0;
        n = std::make_unique<patch::AudioDeviceInNode>(
            (int)std::max( 1u, std::min( granted, (unsigned)patch::kMaxPortsPerNode ) ) );
    }
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

bool audio_app_patch_set_virtual_midi_ports( int node, int ports )
{
    if ( !g_patch ) return false;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( auto* in = dynamic_cast<patch::MidiInNode*>( n ) ) in->setTrackPorts( ports );
    else if ( auto* out = dynamic_cast<patch::MidiOutNode*>( n ) ) out->setPorts( ports );
    else return false;
    g_patch->pruneDanglingConnections();
    g_patch->compileAndPublish();
    return true;
}

namespace {
patch::VirtualMidiPortsNode* virtual_midi_node() {
    return g_patch ? dynamic_cast<patch::VirtualMidiPortsNode*>(g_patch->node(g_virtualMidiId)) : nullptr;
}
}
int audio_app_virtual_midi_node() { return (int)g_virtualMidiId; }

// Virtual MIDI ports are patch metadata. They never form an implicit bus;
// the UI resolves explicit endpoint cables to isolated per-track graph edges.

bool audio_app_virtual_midi_input_is_patched( int input )
{
    if ( !g_patch || input < 0 ) return false;
    const std::vector<patch::Connection> conns = g_patch->connections();
    for ( const patch::Connection& c : conns )
        if ( c.to.node == g_virtualMidiId && (int) c.to.port == input ) return true;
    return false;
}

void audio_app_virtual_midi_set_ports(int inputs, int outputs) {
    if (auto* n = virtual_midi_node()) {
        n->setPorts(inputs, outputs);
        g_patch->pruneDanglingConnections();
        g_patch->compileAndPublish();
    }
}
int audio_app_virtual_midi_inputs() { auto* n=virtual_midi_node(); return n?n->inputCount():0; }
int audio_app_virtual_midi_outputs(){ auto* n=virtual_midi_node(); return n?n->outputCount():0; }
void audio_app_virtual_midi_set_route(int output,int input) {
    if(auto* n=virtual_midi_node())n->setRoute(output,input);
}
int audio_app_virtual_midi_route(int output) {
    if(auto* n=virtual_midi_node()) return n->route(output);
    return -1;
}
int audio_app_add_midi_track_node(){if(!g_patch)return -1;
    const auto id=g_patch->addNode(std::make_unique<patch::MidiTrackNode>());
    g_patch->compileAndPublish();return (int)id;}
static patch::MidiTrackNode* midi_track_node(int id){return g_patch?
    dynamic_cast<patch::MidiTrackNode*>(g_patch->node((patch::NodeId)id)):nullptr;}
void audio_app_midi_track_monitor(int node,bool on){if(auto* n=midi_track_node(node))n->setMonitor(on);}
void audio_app_midi_track_capture(int node,bool on){
    if(auto* n=midi_track_node(node)){
        n->setCapture(on);
        // On stop, wait until every audio block that could have observed the old
        // armed flag has completed. The following drain then includes the tail.
        if(!on)freeze_rcu_grace();
    }
}
void audio_app_midi_track_set_routing(int node,int input,int output){
    if(auto* n=midi_track_node(node))n->setRouting(input,output);}
int audio_app_midi_track_input(int node){auto* n=midi_track_node(node);return n?n->input():-1;}
int audio_app_midi_track_output(int node){auto* n=midi_track_node(node);return n?n->output():-1;}
int audio_app_midi_track_drain(int node,long* ticks,unsigned char* status,
                               unsigned char* d1,unsigned char* d2,int cap){
    auto* n=midi_track_node(node);if(!n||cap<=0)return 0;
    static thread_local std::vector<patch::MidiTrackNode::Ev> ev;
    if((int)ev.size()<cap)ev.resize((size_t)cap);
    const int count=n->drain(ev.data(),cap);
    for(int i=0;i<count;++i){
        ticks[i]=(long)audio_app_sample_to_tick(ev[i].sample);
        status[i]=ev[i].status;d1[i]=ev[i].d1;d2[i]=ev[i].d2;
    }
    return count;}

bool audio_app_patch_connect( int fn, int fp, int tn, int tp )
{
    if ( !g_patch ) return false;
    patch::Connection c{ patch::PortRef{ (patch::NodeId)fn, (patch::PortId)fp },
                         patch::PortRef{ (patch::NodeId)tn, (patch::PortId)tp } };
    bool ok = g_patch->connect( c );
    if ( ok ) {
        g_patch->compileAndPublish();
        g_modular.store(true, std::memory_order_release);
    }
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
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( !n ) return false;
    if ( patch::PluginNode* pn = dynamic_cast<patch::PluginNode*>( n ) )
        return pn->instance() && pn->instance()->descriptor().isInstrument;
    bool midiIn = false, audioOut = false;
    for ( int i = 0; i < n->numPorts(); ++i ) {
        const patch::PortDesc p = n->port( i );
        midiIn   |= p.kind == patch::PortKind::Midi  && p.dir == patch::PortDir::In;
        audioOut |= p.kind == patch::PortKind::Audio && p.dir == patch::PortDir::Out;
    }
    return midiIn && audioOut; // Csound, Pd and Rack instruments
}

int audio_app_patch_first_port( int node, int kind, int direction )
{
    if ( !g_patch ) return -1;
    patch::Node* n = g_patch->node( (patch::NodeId)node );
    if ( !n ) return -1;
    const patch::PortKind wantKind = kind ? patch::PortKind::Midi : patch::PortKind::Audio;
    const patch::PortDir wantDir = direction ? patch::PortDir::Out : patch::PortDir::In;
    for ( int i = 0; i < n->numPorts(); ++i ) {
        const patch::PortDesc p = n->port( i );
        if ( p.kind == wantKind && p.dir == wantDir ) return (int)p.id;
    }
    return -1;
}

int  audio_app_patch_out_node()     { return (int) g_patchOutId; }
int  audio_app_patch_midi_in_node() { return (int) g_patchMidiInId; }
int  audio_app_default_hw_midi_in_node() { return (int) g_defaultHwMidiInId; }
void audio_app_set_modular( bool on ) { g_modular.store( on ); }
bool audio_app_modular()            { return g_modular.load(); }

// Multi-core plugin processing in the modular graph (level-parallel; off by default).
void audio_app_set_multithreaded( bool on ) { if ( g_patch ) g_patch->setMultiThreaded( on ); }
bool audio_app_multithreaded()      { return g_patch ? g_patch->multiThreaded() : false; }

// Report transport run-state into the modular render context so host-synced
// plugin elements (arps / tempo-locked LFOs) advance during playback.  Also
// rolls / holds the kitchensink musical clock.
// Ask the audio thread to release every note it has delivered and is still
// holding, then drop whatever is queued ahead of it.  Safe from any thread: it
// only raises the flag audio_render_segment already honors, and the release
// itself happens on the audio thread through the normal per-track paths (so
// external hardware wired downstream sees the note-offs too).
void audio_app_midi_panic()
{
    // A panic is a scheduling discontinuity, and the documented contract in
    // audio_app.h is "release every held note, THEN discard whatever is queued
    // ahead of the playhead".  Raising g_flushMidiIn alone only flushes the
    // MidiIn node's own lookahead: everything already handed to the app-level
    // schedulers stayed live and kept firing note-ons a moment later, so the
    // panic went quiet and then started playing again.  Bumping the epoch is
    // what makes the drains drop those stale messages -- exactly what
    // audio_app_transport_locate() does for the same reason.
    g_schedEpoch.fetch_add( 1, std::memory_order_release );
    g_flushMidiIn.store( true, std::memory_order_release );
}

void audio_app_patch_set_playing( bool on )
{
    g_patchPlaying.store( on );
    if ( on ) {
        g_lastClickDue=-1;
        g_transport.start();
    }
    else {
        g_transport.stop();
        // Bug B3: STOP is a discontinuity too.  Every stop path in the app lands
        // here, and without this a note that was sounding when you hit stop was
        // simply abandoned (nothing else releases it -- the scheduler's own
        // note-offs only cover notes IT believes are playing).
        audio_app_midi_panic();
        g_clickReset.store(true,std::memory_order_release);
        g_countinActive.store(false, std::memory_order_release);
        g_countinFinished.store(false, std::memory_order_release);
    }
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
            g_loopConfigGeneration.fetch_add(1,std::memory_order_acq_rel);
            g_loopStartSample.store( nm->tick_to_sample( g_loopLeftTick ),
                                     std::memory_order_relaxed );
            g_loopEndSample.store( nm->tick_to_sample( g_loopRightTick ),
                                   std::memory_order_relaxed );
            g_loopConfigGeneration.fetch_add(1,std::memory_order_release);
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
    g_loopConfigGeneration.fetch_add(1,std::memory_order_acq_rel);
    if ( on && rightTick > leftTick )
    {
        g_loopStartSample.store( map->tick_to_sample( leftTick ),
                                 std::memory_order_relaxed );
        g_loopEndSample.store( map->tick_to_sample( rightTick ),
                               std::memory_order_relaxed );
        g_loopOn.store( true, std::memory_order_release );
    }
    else {
        g_loopOn.store( false, std::memory_order_release );
        clear_loop_boundary_offs();
    }
    g_loopConfigGeneration.fetch_add(1,std::memory_order_release);
}

void audio_app_invalidate_future_schedule()
{
    // Ring entries carry the epoch captured by their producer.  Advancing it
    // makes only already-queued future entries stale; unlike locate(), this
    // deliberately does not request a MIDI-node flush or release held notes.
    g_schedEpoch.fetch_add( 1, std::memory_order_acq_rel );
    clear_loop_boundary_offs();
}

void audio_app_queue_loop_note_off(int track,int channel,int note)
{
    if(track<0||track>=AUDIO_APP_MAX_TRACKS||note<0||note>127)return;
    channel&=15;
    g_loopOff[track][channel][note>>5].fetch_or(1u<<(note&31),std::memory_order_release);
}

unsigned long long audio_app_loop_wrap_generation()
{
    return g_loopWrapGeneration.load(std::memory_order_acquire);
}

unsigned long long audio_app_locate_generation()
{
    return g_locateGeneration.load(std::memory_order_acquire);
}

void audio_app_set_schedule_lookahead( long long samples )
{
    if ( samples < 0 ) samples = 0;
    g_schedLookahead.store( samples, std::memory_order_relaxed );
}

long long audio_app_transport_pending_seek()
{
    return g_transport.pending_seek();
}

void audio_app_loop_debug( long long* startSample, long long* endSample,
                           long long* leftTick, long long* rightTick, int* on )
{
    if ( startSample ) *startSample = g_loopStartSample.load( std::memory_order_relaxed );
    if ( endSample )   *endSample   = g_loopEndSample.load( std::memory_order_relaxed );
    if ( leftTick )    *leftTick    = g_loopLeftTick;
    if ( rightTick )   *rightTick   = g_loopRightTick;
    if ( on )          *on          = g_loopOn.load( std::memory_order_relaxed ) ? 1 : 0;
}

void audio_app_midi_census( unsigned long long* enq, unsigned long long* delivered,
                            unsigned long long* ringFull, unsigned long long* staleEpoch,
                            unsigned long long* unreachable, unsigned long long* nodeFull )
{
    if ( enq )         *enq         = g_mcEnq.load( std::memory_order_relaxed );
    if ( delivered )   *delivered   = g_mcDelivered.load( std::memory_order_relaxed );
    if ( ringFull )    *ringFull    = g_mcRingFull.load( std::memory_order_relaxed );
    if ( staleEpoch )  *staleEpoch  = g_mcStaleEpoch.load( std::memory_order_relaxed );
    if ( unreachable ) *unreachable = g_mcUnreachDrop.load( std::memory_order_relaxed );
    if ( staleEpoch )  *staleEpoch  = g_mcBrkEpoch.load( std::memory_order_relaxed );
    if ( ringFull )    *ringFull    = g_mcBrkFuture.load( std::memory_order_relaxed );
    if ( nodeFull )    *nodeFull    = g_mcBrkWrapped.load( std::memory_order_relaxed );
    if ( nodeFull )    *nodeFull    = g_mcNodeFull.load( std::memory_order_relaxed )
                                    + g_mcNoNode.load( std::memory_order_relaxed );
}

void audio_app_set_silent_output( bool silent )
{
    g_silentOut.store( silent, std::memory_order_relaxed );
}
double    audio_app_tempo()                 { return g_tempoBpm.load(); }
void audio_app_metronome_enable(bool on) { g_metronomeEnabled.store(on); }
bool audio_app_metronome_enabled() { return g_metronomeEnabled.load(); }
void audio_app_metronome_start_countin()
{
    const kitchensink::TempoMap* map=g_tmapActive.load(std::memory_order_acquire);
    // Capture where the playhead is ABOUT to be, not where it still reads.
    // audio_app_transport_locate() only QUEUES a seek (the audio thread applies
    // it at the next block start), so "rewind to start, then hit record with
    // count-in" used to latch the pre-rewind position here -- and the count-in
    // then handed the transport back to that stale spot, so recording began
    // wherever you had been instead of at the playhead.
    g_countinOrigin=g_transport.effective_sample();
    g_transport.stop();
    // R9: route the jump through the LOCATE CONTRACT, not a bare seek.  A raw
    // locate moves the playhead without bumping the schedule epoch or asking
    // the audio thread to flush MidiIn, so events scheduled for the
    // pre-count-in position survived and fired against the count-in, and
    // anything sounding was never released.
    audio_app_transport_locate(0); // beat one is the accented click
    g_lastClickDue=-1;
    g_countinEnd=map->tick_to_sample(4LL*kSequencerPpqn);
    g_countinFinished.store(false,std::memory_order_release);
    g_countinActive.store(true,std::memory_order_release);
    g_transport.start();
}
bool audio_app_metronome_countin_finished()
{
    if(!g_countinFinished.exchange(false,std::memory_order_acq_rel)) return false;
    g_transport.stop();
    // R9: same contract on the way back out of the count-in.
    audio_app_transport_locate(g_countinOrigin);
    return true;
}
long long audio_app_metronome_countin_origin_tick()
{
    return audio_app_sample_to_tick( g_countinOrigin );
}
void      audio_app_transport_locate( long long sample )
{
    if ( sample < 0 ) sample = 0;
    // A locate is a transport DISCONTINUITY: everything scheduled ahead of it
    // references the old position.  Bump the epoch so the drains discard those
    // messages, and have the audio thread flush the MidiIn node's lookahead.
    g_schedEpoch.fetch_add( 1, std::memory_order_release );
    g_flushMidiIn.store( true, std::memory_order_release );
    // R10: a locate is a discontinuity for the loop-boundary release mask too.
    // audio_app_invalidate_future_schedule() has always flushed it; locate did
    // not, so note-offs queued for notes that were sounding BEFORE the jump
    // survived it and fired at the next wrap, against whatever was playing
    // then.  Flushing (never discarding) is the same contract used there.
    clear_loop_boundary_offs();
    // PUBLISH the discontinuity rather than leaving the scheduler to infer it
    // from a position decrease -- see g_locateGeneration.  Bumped last, so a
    // scheduler that observes the new generation is guaranteed to also see the
    // epoch bump and the flush request that go with it.
    g_locateGeneration.fetch_add( 1, std::memory_order_acq_rel );
    // Race-free seek (nerf 21 glue): the audio thread applies it at block
    // start, so a locate can never shear a block across two positions.  With
    // no audio thread to apply it, seek immediately.
    if ( g_running ) g_transport.request_seek( sample );
    // No audio thread exists to consume the mailbox, and locate() is itself
    // just request_seek(), so the old call queued a seek that was NEVER
    // applied.  Store it directly -- with no renderer running there is nothing
    // to race.
    else             g_transport.locate_now( sample );
}
void audio_app_transport_settle()
{
    if(!g_running)return;
    // A punch-in must not open capture while callbacks still publish the old
    // timeline position. Waiting two completed callbacks both applies the
    // pending seek and retires every block that began before it.
    freeze_rcu_grace();
    for(int i=0;i<100&&g_transport.pending_seek()>=0;++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void audio_app_transport_locate_sync(long long sample)
{
    if(sample<0)sample=0;
    audio_app_transport_locate(sample);
    audio_app_transport_settle();
}
long long audio_app_transport_sample()      { return g_transport.sample(); }
long long audio_app_transport_effective_sample() { return g_transport.effective_sample(); }
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
long long audio_app_sample_to_tick_ceil( long long sample )
{
    long long tick = audio_app_sample_to_tick( sample );
    if ( audio_app_tick_to_sample( tick ) < sample ) ++tick;
    return tick;
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
        audio_render( nullptr, 0, o, 2, g_block, g_sr );
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

// ---- per-track INPUT capture ------------------------------------------------
// What these record is the track's INPUT in the modular graph: the raw,
// PRE-FADER inlet of the master-mixer strip -- i.e. whatever the patcher has
// routed into that strip (an Audio In device node, an instrument, a rack...).
// The tap sits in MixerNode::process / MasterMixerNode::process, one slot per
// track, so several tracks can record simultaneously (punch modes).  A track
// whose inlet has nothing patched into it records silence -- there is no
// hidden hardware side channel, exactly the MIDI-side contract.

static TrackCapSlot* track_cap_for( int track )
{
    for ( auto& tc : g_trackCaps ) if ( tc.buf && tc.track == track ) return &tc;
    return nullptr;
}
static TrackCapSlot* track_cap_first()
{
    for ( auto& tc : g_trackCaps ) if ( tc.buf ) return &tc;
    return nullptr;
}
static patch::MasterMixerNode* track_cap_node()
{
    return g_patch ? dynamic_cast<patch::MasterMixerNode*>(
                         g_patch->node( g_masterMixerId ) ) : nullptr;
}

bool audio_app_track_capture_begin( int track, double seconds )
{
    if ( !g_patch || seconds <= 0.0 ) return false;
    patch::MasterMixerNode* mm = track_cap_node();
    if ( !mm || track < 0 || track >= mm->trackCount() ) return false;
    // A capture left over on THIS track from a take that never reached end()
    // used to make every later take fail here -- audio recording silently died
    // for the rest of the session.  Reclaim it instead of refusing.
    if ( TrackCapSlot* prev = track_cap_for( track ) ) {
        mm->endTrackCaptureFor( track );
        // endTrackCaptureFor() only exchanges the pointer out; a block that
        // already loaded it (patch_nodes.cpp capture tap) is still writing into
        // it for the rest of THIS block.  Honour the same keep-alive-one-block
        // contract audio_app_track_capture_end_for() does -- freeing here
        // immediately was a use-after-free on the audio thread.
        freeze_rcu_grace();
        free( prev->buf );
        *prev = TrackCapSlot{};
    }
    TrackCapSlot* slot = nullptr;
    for ( auto& tc : g_trackCaps ) if ( !tc.buf ) { slot = &tc; break; }
    if ( !slot ) return false;              // all 8 simultaneous taps in use
    const size_t frames = (size_t)( seconds * g_sr ) + (size_t)g_block;
    float* buf = (float*)calloc( frames * 2, sizeof(float) );
    if ( !buf ) return false;
    if ( !mm->beginTrackCapture( track, buf, frames ) ) { free( buf ); return false; }
    slot->buf = buf;
    slot->cap = frames;
    slot->track = track;
    return true;
}

bool audio_app_track_capture_end_for( int track, AudioClip& out )
{
    TrackCapSlot* tc = track_cap_for( track );
    patch::MasterMixerNode* mm = track_cap_node();
    if ( !tc || !mm ) return false;
    const size_t frames = std::min( mm->trackCaptureFramesFor( track ), tc->cap );
    float* buf = mm->endTrackCaptureFor( track );
    const unsigned gen0 = g_blockGen.load( std::memory_order_acquire );
    for ( int i = 0; i < 250; ++i ) {
        if ( g_blockGen.load( std::memory_order_acquire ) >= gen0 + 2 ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds(1) );
    }
    // endTrackCaptureFor() hands the buffer over exactly once.  Returning early
    // here without releasing the slot left it dangling and non-empty, which
    // then blocked every later capture_begin -- the take is lost either way, so
    // free it and reset rather than wedging the recorder.
    if ( !buf ) {
        free( tc->buf );
        *tc = TrackCapSlot{};
        return false;
    }
    out.name = "Recorded audio";
    out.sampleRate = out.sourceSampleRate = g_sr;
    out.resize( (int64_t)frames );
    for ( size_t i = 0; i < frames; ++i ) {
        out.ch[0][i] = buf[i * 2];
        out.ch[1][i] = buf[i * 2 + 1];
    }
    free( buf );
    *tc = TrackCapSlot{};
    return frames > 0;
}

bool audio_app_track_capture_end( AudioClip& out )
{
    // Legacy single-capture entry (the normal record path opens exactly one).
    TrackCapSlot* tc = track_cap_first();
    return tc && audio_app_track_capture_end_for( tc->track, out );
}

bool audio_app_track_capture_preview( AudioClip& out, int maxFrames )
{
    TrackCapSlot* tc = track_cap_first();
    patch::MasterMixerNode* mm = track_cap_node();
    if ( !tc || !mm || maxFrames <= 0 ) return false;
    const size_t frames = std::min( mm->trackCaptureFramesFor( tc->track ), tc->cap );
    if ( frames == 0 ) return false;
    const size_t take = std::min( frames, (size_t)maxFrames );
    out.name = "Recording";
    out.sampleRate = out.sourceSampleRate = g_sr;
    out.resize( (int64_t)take );
    // Uniformly sample the entire recording into a bounded display buffer.
    // Copying the latest N raw frames made the waveform slide and forced the UI
    // to copy hundreds of thousands of floats every repaint.
    for ( size_t i = 0; i < take; ++i ) {
        const size_t src = take > 1 ? ( i * ( frames - 1 ) ) / ( take - 1 ) : 0;
        out.ch[0][i] = tc->buf[src * 2];
        out.ch[1][i] = tc->buf[src * 2 + 1];
    }
    return true;
}

long long audio_app_track_capture_frames_for( int track )
{
    TrackCapSlot* tc = track_cap_for( track );
    patch::MasterMixerNode* mm = track_cap_node();
    if ( !tc || !mm ) return 0;
    return (long long)std::min( mm->trackCaptureFramesFor( track ), tc->cap );
}

long long audio_app_track_capture_frames()
{
    TrackCapSlot* tc = track_cap_first();
    return tc ? audio_app_track_capture_frames_for( tc->track ) : 0;
}

bool audio_app_track_capture_read_for( int track, long long startFrame,
                                       long long frames, AudioClip& out )
{
    TrackCapSlot* tc = track_cap_for( track );
    if ( !tc || frames <= 0 || startFrame < 0 ) return false;
    // Only frames the audio thread has ALREADY written are readable; it appends
    // strictly forward, so everything before `avail` is stable history.
    const long long avail = audio_app_track_capture_frames_for( track );
    if ( startFrame >= avail ) return false;
    if ( startFrame + frames > avail ) frames = avail - startFrame;
    out.name = "Punch";
    out.sampleRate = out.sourceSampleRate = g_sr;
    out.resize( frames );
    for ( long long i = 0; i < frames; ++i )
    {
        out.ch[0][(size_t)i] = tc->buf[( startFrame + i ) * 2];
        out.ch[1][(size_t)i] = tc->buf[( startFrame + i ) * 2 + 1];
    }
    return frames > 0;
}

bool audio_app_track_capture_read( long long startFrame, long long frames,
                                   AudioClip& out )
{
    TrackCapSlot* tc = track_cap_first();
    return tc && audio_app_track_capture_read_for( tc->track, startFrame, frames, out );
}

bool audio_app_track_input_patched( int track )
{
    if ( !g_patch || track < 0 ) return false;
    // The strip's audio inlet PortId -- the same math
    // audio_app_master_connect_instrument uses to wire instruments in.
    const int inlet = 2 + 2 * track;
    for ( const patch::Connection& c : g_patch->connections() )
        if ( c.to.node == g_masterMixerId && (int)c.to.port == inlet ) return true;
    return false;
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

void audio_app_capture_cancel()
{
    g_capturing.store(false,std::memory_order_release);
    float* buf=g_capBuf.exchange(nullptr,std::memory_order_acq_rel);
    if(!buf)return;
    freeze_rcu_grace();
    free(buf);
    g_capCap=0;g_capPos.store(0,std::memory_order_relaxed);
}

// Render one track's WHOLE signal chain offline into an in-memory AudioClip
// (freeze).  Renders in the CURRENT engine mode so it captures the real audible
// output -- the fixed-graph track (instrument + insert FX) OR the modular patch
// graph (patched instruments + effects).  The target track is isolated so the
// master tap == just that track: fixed graph solos it (master gain neutralised);
// modular mutes every OTHER master-mixer channel.  The caller's scheduleMidi()
// enqueues the pattern's events (ticks from 0) AFTER the seek so they land
// in-window.
namespace {
// Latency compensation state for the offline bounce (message thread only).
bool g_freezePdcOn      = true;
int  g_freezePdcMax     = 8192;      // ~170 ms at 48k: past this it is not latency
int  g_freezePdcLast    = -1;        // what the last bounce measured
// Where the bounce's first scheduled event landed, in samples, or -1 when the
// render scheduled no MIDI at all (a pure audio track).
long long g_freezeFirstEventSample = -1;
}

void audio_app_freeze_set_latency_compensation( bool on, int maxSamples )
{
    g_freezePdcOn = on;
    if ( maxSamples > 0 ) g_freezePdcMax = maxSamples;
}

bool audio_app_freeze_latency_compensation( int* outMaxSamples )
{
    if ( outMaxSamples ) *outMaxSamples = g_freezePdcMax;
    return g_freezePdcOn;
}

int audio_app_freeze_last_measured_latency() { return g_freezePdcLast; }

// audio_app_route_midi records the earliest ticked event of a bounce so the
// capture below knows what the instrument was answering.
void audio_app_freeze_note_first_event( long long sample )
{
    if ( g_freezeFirstEventSample < 0 || sample < g_freezeFirstEventSample )
        g_freezeFirstEventSample = sample;
}

bool audio_app_freeze_render_at( int track, long long startSample, double seconds,
                                 const std::function<void()>& scheduleMidi,
                                 const std::function<void(long long,long long)>& advanceAutomation,
                                 PatchKnob::engine::AudioClip& outClip )
{
    if ( !g_engine || seconds <= 0.0 ) return false;
    if ( g_capturing.load() || g_capBuf.load() ) return false;   // a capture is busy

    const bool wasRunning = g_engine->isRunning();
    // effective_sample(), not sample(): locate() only QUEUES a seek, so sample()
    // still reports the pre-seek position and the freeze would restore the user
    // to where the playhead used to be rather than where it is.
    const long long savedTransportSample = g_transport.effective_sample();
    if ( wasRunning ) g_engine->stop();          // single-threaded block pump

    // Offline freeze must be a sealed transport session.  In particular, a raw
    // Transport::locate() does not invalidate the scheduled MIDI ring, so notes
    // queued by normal playback can sit in front of the freeze events (or fire
    // in the render).  Looping can also wrap the manually-pumped transport and
    // capture arbitrary/repeated portions of the song.  Preserve the user
    // switches, but suppress both for the duration of the bounce.
    const bool savedLoop = g_loopOn.exchange( false, std::memory_order_acq_rel );
    const bool savedMetronome =
        g_metronomeEnabled.exchange( false, std::memory_order_acq_rel );
    g_countinActive.store( false, std::memory_order_release );
    g_countinFinished.store( false, std::memory_order_release );
    g_clickReset.store( true, std::memory_order_release );

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
    std::vector<patch::Connection> freezeDisconnected;

    auto restoreAndReturn = [&]( bool ok ) -> bool {
        g_transport.stop();
        if ( modular ) audio_app_patch_set_playing( false );
        // Invalidate any freeze-only events which might remain beyond the
        // requested window, then restore the user's play position.
        g_schedEpoch.fetch_add( 1, std::memory_order_acq_rel );
        g_flushMidiIn.store( true, std::memory_order_release );
        g_transport.locate( savedTransportSample );
        g_loopOn.store( savedLoop, std::memory_order_release );
        g_metronomeEnabled.store( savedMetronome, std::memory_order_release );
        if ( modular ) {
            // The offline bounce temporarily removes non-target feeds at the
            // master boundary.  Restore the exact graph before the device is
            // restarted; otherwise a failed/aborted freeze can leave live
            // routing silently disconnected.
            if ( g_patch && !freezeDisconnected.empty() ) {
                for ( const patch::Connection& c : freezeDisconnected )
                    g_patch->connect( c );
                g_patch->compileAndPublish();
                freezeDisconnected.clear();
            }
            for ( int c = 0; c < (int)savedMmMute.size() && mm; ++c )
                mm->setMute( c, savedMmMute[(size_t)c] != 0 );
        }
        else if ( g_graph ) {
            for ( int i = 0; i < (int)savedSolo.size(); ++i )
                if ( Track* t = g_graph->track(i) )
                    t->setSolo( savedSolo[(size_t)i] != 0 );
            if ( target ) target->setMute( savedTargetMute );
            g_graph->setMasterGain( oldMaster );
        }
        if ( wasRunning ) g_engine->start();
        return ok;
    };

    if ( modular )
    {
        if ( !g_patch ) return restoreAndReturn( false );
        mm = dynamic_cast<patch::MasterMixerNode*>( g_patch->node(g_masterMixerId) );
        if ( !mm || track < 0 || track >= mm->trackCount() )
            return restoreAndReturn( false );

        // A modular track can fan out through several effects and arrive on
        // several mixer strips.  Keeping only `track` loses those branches;
        // capturing the unmodified master records every other track too.  Trace
        // forward from this track's dedicated MIDI outlet and retain every
        // master inlet reached by that route.  The selected strip is retained
        // as a fallback for audio-only/direct routes.
        const std::vector<patch::Connection> conns = g_patch->connections();
        std::unordered_set<patch::NodeId> reachable;
        std::queue<patch::NodeId> pending;
        for ( const patch::Connection& c : conns )
            if ( c.from.node == g_patchMidiInId && (int)c.from.port == track &&
                 c.to.node != g_masterMixerId && reachable.insert(c.to.node).second )
                pending.push(c.to.node);
        while ( !pending.empty() ) {
            const patch::NodeId n = pending.front(); pending.pop();
            for ( const patch::Connection& c : conns )
                if ( c.from.node == n && c.to.node != g_masterMixerId &&
                     reachable.insert(c.to.node).second )
                    pending.push(c.to.node);
        }

        std::set<int> keptChannels;
        keptChannels.insert(track);
        for ( const patch::Connection& c : conns )
            if ( c.to.node == g_masterMixerId && c.to.port >= 2 &&
                 ((c.to.port - 2) & 1u) == 0u && reachable.count(c.from.node) )
                keptChannels.insert((int)(c.to.port - 2) / 2);

        // Disconnecting at the sink is stronger than mixer mute: strips with an
        // external TrackFaderNode deliberately ignore MasterMixerNode::mute.
        // The engine is stopped here, so publish one isolated immutable render
        // plan and restore the original connections in restoreAndReturn().
        for ( const patch::Connection& c : conns ) {
            bool remove = false;
            if ( c.to.node == g_masterMixerId && c.to.port >= 2 &&
                 ((c.to.port - 2) & 1u) == 0u ) {
                const int channel = (int)(c.to.port - 2) / 2;
                remove = !keptChannels.count(channel);
            }
            // User patch cables may bypass the mixer and feed Audio Out
            // directly.  Keep a direct feed only when it belongs to the traced
            // route (the master mixer's own output is always retained).
            else if ( c.to.node == g_patchOutId && c.from.node != g_masterMixerId )
                remove = !reachable.count(c.from.node);
            if ( remove && g_patch->disconnect(c) )
                freezeDisconnected.push_back(c);
        }
        if ( !g_patch->compileAndPublish() )
            return restoreAndReturn( false );
    }
    else
    {
        if ( !g_graph ) return restoreAndReturn( false );
        const int NT = g_graph->trackCount();
        if ( track < 0 || track >= NT ) return restoreAndReturn( false );
        target = g_graph->track( track );
        if ( !target ) return restoreAndReturn( false );
        savedSolo.resize( (size_t)NT );
        for ( int i = 0; i < NT; ++i ) {
            Track* t = g_graph->track( i );
            savedSolo[(size_t)i] = (t && t->solo()) ? 1 : 0;
            if ( t ) t->setSolo( i == track );
        }
        savedTargetMute = target->mute();
        target->setMute( false );
        oldMaster = g_graph->masterGain();
        g_graph->setMasterGain( 1.0f );
    }

    // COLD-START WARM-UP.
    //
    // A note landing on the very first rendered sample is missed: an envelope's
    // gate input has never been sampled LOW, so the Schmitt trigger sees no
    // rising edge and never opens.  A track freeze starts at tick 0, so this ate
    // whatever was on beat 1 -- most visibly a drum hit, which is all transient
    // and simply vanished.
    //
    // Render a block with the transport PARKED and nothing scheduled: no music
    // time passes and no event can fire, but every gate, filter and envelope in
    // the chain samples an idle input first.  Then start for real.
    {
        std::vector<float> wL( (size_t)g_block, 0.f ), wR( (size_t)g_block, 0.f );
        float* w[2] = { wL.data(), wR.data() };
        g_transport.stop();
        // Pin the position BEFORE the warm-up renders: audio_render() runs
        // Transport::process(), which would otherwise consume whatever seek the
        // playhead had left pending and warm up at the wrong place.
        g_transport.locate( std::max<long long>( 0, startSample ) );
        g_transport.process( 0 );                     // apply it NOW, no time passes
        audio_render( nullptr, 0, w, 2, g_block, g_sr );          // uncaptured, time parked
        g_transport.locate( std::max<long long>( 0, startSample ) );
        g_transport.process( 0 );
    }

    if ( !audio_app_capture_begin( seconds ) ) return restoreAndReturn( false );

    // Roll at the actual source position.  The callback schedules absolute
    // project ticks, preserving tempo-map phase for nonzero freeze spans.
    // New epoch makes every pre-freeze scheduled event stale.  The first
    // pumped block also flushes MidiIn's own lookahead before accepting the
    // newly scheduled freeze events below.
    g_schedEpoch.fetch_add( 1, std::memory_order_acq_rel );
    g_flushMidiIn.store( true, std::memory_order_release );
    // locate() is a QUEUED seek applied at the next block, so the bounce used to
    // inherit wherever the playhead was sitting: every note scheduled before that
    // point was already in the past when the render began.  Freezing a 4-bar kick
    // loop with the playhead at bar 5 captured 2 of its 16 hits.  process(0)
    // applies the seek immediately without advancing time, so a freeze always
    // starts exactly where it was told to, independent of the playhead.
    g_transport.locate( std::max<long long>( 0, startSample ) );
    g_transport.process( 0 );

    g_transport.start();
    if ( modular ) audio_app_patch_set_playing( true );
    g_freezeFirstEventSample = -1;      // observed while scheduling, below
    g_freezePdcLast = -1;
    if ( scheduleMidi ) scheduleMidi();

    const long long requestedFrames =
        std::max<long long>( 1, (long long)llround( seconds * (double)g_sr ) );
    // Pump past the requested span by the compensation ceiling, so shifting the
    // capture earlier still has real audio to put at the end instead of running
    // off the buffer and truncating the freeze.
    const long long pdcRoom = ( g_freezePdcOn && g_freezePdcMax > 0 )
                            ? (long long) g_freezePdcMax : 0;
    const long long endSample = requestedFrames + pdcRoom;
    {
        std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
        float* out[2] = { L.data(), R.data() };
        long long pumped = 0;
        while ( pumped < endSample )
        {
            // UI playback normally advances automation.  Offline rendering has
            // no UI loop, so drive it explicitly for this exact audio block.
            // Parameters are queued before audio_render() and therefore become
            // effective at the first sample of the corresponding block.
            if ( advanceAutomation ) {
                const long long bs = g_transport.sample();
                const long long be = bs + (long long)g_block;
                advanceAutomation( audio_app_sample_to_tick( bs ),
                                   audio_app_sample_to_tick_ceil( be ) );
            }
            audio_render( nullptr, 0, out, 2, g_block, g_sr );   // taps master -> g_capBuf
            pumped += (long long)g_block;
        }

        // Leave the source instrument in a known silent state.  The captured
        // span may end while notes/releases are still active (especially with
        // tracker note-ons or a release tail), and merely disabling the source
        // track preserves those voices inside rack/Csound/Pd/plugin state.
        // CC123 + CC120 cover standards-compliant instruments; RackEngine also
        // handles these explicitly. One uncaptured drain block applies them.
        for (int ch=0;ch<16;++ch) {
            audio_app_route_midi(track,(unsigned char)(0xB0|ch),123,0,-1);
            audio_app_route_midi(track,(unsigned char)(0xB0|ch),120,0,-1);
        }
        audio_render(nullptr,0,out,2,g_block,g_sr);
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

        // ---- latency compensation ------------------------------------------
        // The instrument answers its first note some frames late.  Find the
        // first sample it actually produced and compare with where that note was
        // scheduled; the difference is pipeline delay, and dropping it from the
        // head puts the captured audio back on the timeline it came from.
        //
        // Only EXACT zeros count as "not yet started": a slow attack still emits
        // tiny non-zero values immediately, so this measures buffering, not
        // musical shape.  A measurement outside [0, max] is not latency (a rest,
        // a silent instrument, a mis-scheduled note) and is ignored.
        // MEASURE ONLY -- never trim on this number.
        //
        // The first-non-zero-sample test cannot distinguish a pipeline delay
        // from a soft attack: an envelope at t=0 multiplies to EXACTLY 0.0, so a
        // synth with a slow attack emits exact zeros for its opening samples and
        // trimming them cuts the front off the note.  Real compensation needs a
        // latency the instrument REPORTS (Node::latencySamples) or a dedicated
        // calibration pass against a known-instant signal -- not an inference
        // drawn from musical content.  The number is still worth printing.
        const size_t skip = 0;
        if ( g_freezePdcOn && g_freezeFirstEventSample >= 0 )
        {
            const long long evRel = g_freezeFirstEventSample - startSample;
            if ( evRel >= 0 && (size_t)evRel < frames )
            {
                size_t firstAudio = frames;
                for ( size_t i = (size_t)evRel; i < frames; ++i )
                    if ( buf[i * 2 + 0] != 0.f || buf[i * 2 + 1] != 0.f ) { firstAudio = i; break; }
                if ( firstAudio < frames )
                    g_freezePdcLast = (int)( (long long)firstAudio - evRel );
            }
        }

        const size_t avail = ( frames > skip ) ? frames - skip : 0;
        const size_t keptFrames = std::min<size_t>( avail, (size_t)requestedFrames );
        outClip.resize( (int64_t)keptFrames );
        for ( size_t i = 0; i < keptFrames; ++i )
        {
            const size_t si = i + skip;
            float l = buf[si * 2 + 0], r = buf[si * 2 + 1];
            if ( l != l ) l = 0.f;
            if ( r != r ) r = 0.f;
            outClip.ch[0][i] = l;
            outClip.ch[1][i] = r;
        }
        free( buf );
        // A partial buffer is never a valid freeze.  Previously any non-empty
        // capture was accepted, producing half-tracks or random short regions
        // when a tap stopped early.
        ok = keptFrames >= (size_t)requestedFrames;
    }
    g_capCap = 0;
    g_capPos.store( 0 );

    return restoreAndReturn( ok );
}

bool audio_app_freeze_render( int track, double seconds,
                              const std::function<void()>& scheduleMidi,
                              PatchKnob::engine::AudioClip& outClip )
{
    return audio_app_freeze_render_at( track, 0, seconds, scheduleMidi, {}, outClip );
}

// --- frozen-clip playback -----------------------------------------------------
// Each frozen clip is owned here and scheduled on an AudioClipPlayer.  A CLIP
// freeze uses a shared hidden freeze track (track < 0); a TRACK freeze passes an
// explicit engine track (the new audio lane's track), so its player is that
// track's own instrument.  Attach returns a stable id; detach removes exactly
// that clip from its player, so unfreeze is surgical.
namespace {
struct FreezeEntry { int id; AudioClipPlayer* player; std::shared_ptr<AudioClip> clip; uint64_t regionId=0; };
std::vector<FreezeEntry> g_freezeEntries;

// Detached-but-not-yet-freed freeze audio.  See audio_app_freeze_gc(): the UI
// keeps raw AudioClip pointers for drawing, so a detach must not free the
// samples out from under a view that has not been told to drop them.  Parked
// clips are unscheduled and inaudible -- they only keep the memory readable.
std::vector<std::shared_ptr<AudioClip>> g_freezeGraveyard;
size_t g_freezeGraveyardFrames = 0;

// Hard ceiling so a long session cannot grow this without bound.  Anything past
// it is many redraws old, so no view can still be pointing at it.
const size_t kFreezeGraveyardMaxFrames = 48u * 1000u * 1000u;   // ~380 MB stereo

void freeze_graveyard_park( std::shared_ptr<AudioClip> clip )
{
    if ( !clip ) return;
    g_freezeGraveyardFrames += (size_t) clip->numFrames();
    g_freezeGraveyard.push_back( std::move( clip ) );
    while ( g_freezeGraveyard.size() > 1 &&
            g_freezeGraveyardFrames > kFreezeGraveyardMaxFrames )
    {
        const size_t n = (size_t) g_freezeGraveyard.front()->numFrames();
        g_freezeGraveyardFrames = ( g_freezeGraveyardFrames > n )
                                ? g_freezeGraveyardFrames - n : 0;
        g_freezeGraveyard.erase( g_freezeGraveyard.begin() );
    }
}
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
            applyAutoFadeTo( g_freezePlayer );
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
        applyAutoFadeTo( p );
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
    std::shared_ptr<AudioClip> stored = project_store_clip( clip );
    AudioClip* raw = stored.get();
    const uint64_t regionId=player->addRegion(raw,startSample,gain); if(!regionId)return -1;
    const int id = g_freezeNextId++;
    g_freezeEntries.push_back( FreezeEntry{ id, player, std::move(stored), regionId } );
    return id;
}

int audio_app_freeze_attach_shared(int track,const AudioClip* source,long long startSample,float gain)
{
    if(!source||source->empty())return -1;
    std::shared_ptr<AudioClip> owner;
    for(const auto& e:g_projectAudioClips)if(e.clip.get()==source){owner=e.clip;break;}
    if(!owner)for(const auto& e:g_freezeEntries)if(e.clip.get()==source){owner=e.clip;break;}
    if(!owner)return -1;
    AudioClipPlayer* player=freezePlayerForTrack(track);if(!player)return -1;
    const uint64_t regionId=player->addRegion(source,startSample,gain);if(!regionId)return -1;
    const int id=g_freezeNextId++;
    g_freezeEntries.push_back(FreezeEntry{id,player,std::move(owner),regionId});
    return id;
}

const AudioClip* audio_app_freeze_clip( int id )
{
    for ( const FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) return e.clip.get();
    return nullptr;
}

int audio_app_freeze_alignment_lag( int id, int searchSamples )
{
    FreezeEntry* fe=nullptr;
    for (FreezeEntry& e:g_freezeEntries) if(e.id==id){fe=&e;break;}
    if(!fe||!fe->clip||fe->clip->empty()||!g_engine)return 0x7fffffff;
    int idx=-1;
    for(int i=0;i<fe->player->clipCount();++i)
        if(fe->player->clipAt(i).clip==fe->clip.get()){idx=i;break;}
    if(idx<0)return 0x7fffffff;
    const ScheduledClip sc=fe->player->clipAt(idx);
    const int n=(int)std::min<long long>(fe->clip->numFrames(),(long long)g_sr*2);
    if(n<64)return 0;
    const bool wasRunning=g_engine->isRunning(); if(wasRunning)g_engine->stop();
    const long long saved=g_transport.sample();
    g_transport.stop(); g_transport.locate(sc.startSample); g_transport.start();
    const bool modular=g_modular.load(std::memory_order_relaxed);
    if(modular)audio_app_patch_set_playing(true);
    std::vector<float> got((size_t)n+g_block,0.f),tmpR((size_t)g_block,0.f);
    std::vector<float> tmpL((size_t)g_block,0.f); int pos=0;
    while(pos<n){float* o[2]={tmpL.data(),tmpR.data()};audio_render(nullptr,0,o,2,g_block,g_sr);
        int take=std::min(g_block,n-pos);std::copy(tmpL.begin(),tmpL.begin()+take,got.begin()+pos);pos+=take;}
    g_transport.stop();if(modular)audio_app_patch_set_playing(false);g_transport.locate(saved);
    if(wasRunning)g_engine->start();
    const float* src=fe->clip->ch[0].data();
    searchSamples=std::max(0,std::min(searchSamples,n/4));
    int bestLag=0;double best=-2.0;
    const int stride=8;
    for(int lag=-searchSamples;lag<=searchSamples;++lag){double dot=0,a2=0,b2=0;
        const int a0=std::max(0,-lag),a1=std::min(n,n-lag);
        for(int i=a0;i<a1;i+=stride){double a=src[i],b=got[(size_t)(i+lag)];dot+=a*b;a2+=a*a;b2+=b*b;}
        double c=(a2>0&&b2>0)?dot/std::sqrt(a2*b2):-2.0;if(c>best){best=c;bestLag=lag;}}
    return bestLag;
}

void audio_app_freeze_detach( int id )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.player )
        {
            const AudioClip* target = e.clip.get();
            const int i=e.player->regionIndex(e.regionId);
            if(i>=0)e.player->removeClip(i);
            // A shared source may still have live regions; only clear its warp
            // when this was the last scheduled reference on the player.
            bool stillUsed=false;for(int k=0;k<e.player->clipCount();++k)if(e.player->clipAt(k).clip==target){stillUsed=true;break;}
            if(!stillUsed)e.player->clearWarp(target);
            break;
        }
    // The clip pointer is now unscheduled and its warp entry gone, but a live
    // audio block may still be reading the PREVIOUS snapshot -- wait it out before
    // the erase frees the AudioClip, else the audio thread reads freed samples.
    freeze_rcu_grace();

    // Park the samples rather than freeing them: the grace above only covers
    // the audio thread, and the UI still holds a raw pointer to this clip until
    // it is told to forget it.  audio_app_freeze_gc() reclaims them later.
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) freeze_graveyard_park( std::move( e.clip ) );

    g_freezeEntries.erase(
        std::remove_if( g_freezeEntries.begin(), g_freezeEntries.end(),
                        [id]( const FreezeEntry& e ){ return e.id == id; } ),
        g_freezeEntries.end() );
}

void audio_app_freeze_gc()
{
    g_freezeGraveyard.clear();
    g_freezeGraveyardFrames = 0;
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
    return e.player->regionIndex(e.regionId);
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

// Loop PERIOD (source-iteration length) for a looping freeze region.
bool audio_app_freeze_set_loop_length( int id, long long frames )
{
    for ( FreezeEntry& e : g_freezeEntries )
        if ( e.id == id ) { const int i = freezeRegionIndex( e ); return i >= 0 && e.player->setClipLoopLength( i, frames ); }
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
            const ScheduledClip* live = e.player->scheduled( i );
            if ( !live ) return false;
            const ScheduledClip sc = *live;
            AudioClip* c = e.clip.get();
            const int64_t off = sc.sourceOffset;
            const int64_t len = sc.regionLength();
            // Stop publishing this source before changing its sample vectors.
            // The previous code reversed in-place while audio/UI readers could
            // still scan it, producing the draw_waveform access violation.
            e.player->removeClip( i );
            e.player->clearWarp( c );
            freeze_rcu_grace();
            for ( int ch = 0; ch < 2; ++ch )
            {
                int64_t a = off, b = off + len - 1;
                while ( a < b && b < c->numFrames() )
                { std::swap( c->ch[(size_t)ch][(size_t)a], c->ch[(size_t)ch][(size_t)b] ); ++a; --b; }
            }
            if ( !e.player->addClip( c, sc.startSample, sc.gain ) ) return false;
            const int ni=e.player->clipCount()-1;
            if ( !e.player->setClipRegion( ni,sc.startSample,sc.sourceOffset,sc.length ) ) return false;
            e.player->setClipMuted(ni,sc.muted);
            e.player->setClipLoop(ni,sc.loop);
            e.player->setClipFades(ni,sc.fadeInFrames,sc.fadeOutFrames,
                                   sc.fadeInTension,sc.fadeOutTension);
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

bool audio_app_freeze_set_fade_shapes( int id, int inShape, int outShape,
                                       int inSlope, int outSlope, int link )
{
    for ( const FreezeEntry& e : g_freezeEntries )
        if ( e.id == id && e.player )
        {
            const AudioClip* target = e.clip.get();
            for ( int i = 0; i < e.player->clipCount(); ++i )
                if ( e.player->clipAt( i ).clip == target )
                    return e.player->setClipFadeShapes( i, inShape, outShape,
                                                        inSlope, outSlope, link );
        }
    return false;
}

// Headless proof of the freeze render/capture path: a continuous sine -> Out in
// modular mode, frozen offline; a non-zero peak proves the pump+capture work
// with the stream stopped (isolating render bugs from MIDI/instrument routing).
float audio_app_freeze_selftest()
{
    if ( !g_patch ) return -1.f;
    const int selfTrack = audio_app_master_add_track( 0 );
    const int sine = audio_app_patch_add_builtin( "sine" );
    if ( sine < 0 || selfTrack < 0 ) {
        if ( sine >= 0 ) audio_app_patch_remove( sine );
        if ( selfTrack >= 0 ) audio_app_master_remove_track( selfTrack );
        return -1.f;
    }
    audio_app_master_connect_instrument( selfTrack, sine );
    const bool wasModular = g_modular.load();
    audio_app_set_modular( true );

    PatchKnob::engine::AudioClip clip;
    const bool ok = audio_app_freeze_render( selfTrack, 0.2, std::function<void()>(), clip );
    float peak = 0.f;
    for ( int c = 0; c < 2; ++c )
        for ( float v : clip.ch[(size_t)c] ) { const float a = v < 0.f ? -v : v; if ( a > peak ) peak = a; }
    fprintf( stderr, "[freeze-selftest] ok=%d frames=%lld peak=%.4f (%s)\n",
             (int)ok, (long long)clip.numFrames(), peak,
             peak > 0.0001f ? "CAPTURE OK" : "CAPTURE SILENT" );
    fflush( stderr );

    audio_app_patch_remove( sine );
    audio_app_master_remove_track( selfTrack );

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
    const long   kSpacing = (long)( kSequencerPpqn / 4 );  // 16ths, whatever the PPQN
    const long   kGate    = kSpacing * 5 / 8;              // same 30/48 proportion
    const double seconds = (double)( kNotes * kSpacing + kSequencerPpqn / 2 )
                           / (double)kSequencerPpqn * 60.0 / 120.0 + 0.5;

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
            audio_render( nullptr, 0, o, 2, g_block, g_sr );
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
    // Bug B1: this used to push STRAIGHT into g_midiInNode from the message
    // thread while the stream callback was pushing into the same ring -- two
    // producers doing a read-modify-write on a strict SPSC tail_, i.e. torn /
    // duplicated / lost events and a scrambled per-event target plug.  Go through
    // the producer-serialized route ring instead (tick -1 == "due now", plug 0 ==
    // track 0, which is the plug wired to the plugin above); the audio thread
    // stays the node's one and only producer.  Longer wait to survive heavy
    // async plugin init.
    audio_app_route_midi( 0, 0x90, 60, 110, -1 );
    std::this_thread::sleep_for( std::chrono::milliseconds( 800 ) );
    float peak = g_engine->masterPeak();
    audio_app_route_midi( 0, 0x80, 60, 0, -1 );
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
    // samples per sequencer tick.  This was hard-coded 192.0 from the seq24
    // era; kitchensink::seq_ppqn is 768, so every "ideal" below was 4x too
    // wide and the selftest reported a 540-sample deviation on a stream that
    // was in fact sample-exact.
    const double spt    = (double)g_sr * 60.0 / ( 500.0 * (double)kSequencerPpqn );
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
            audio_render( nullptr, 0, out, 2, g_block, g_sr );
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

    // Analyze: ideal inter-onset = tick_to_sample(24) exactly (180 @48k/500).
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
// One 4/4 bar (768 ticks) looped with 32nds on the grid, driven by a faithful
// simulation of the output thread's lookahead scheduler (including its wrap
// branch that enqueues the NEXT pass's events before the engine wraps, its
// published schedule lookahead, and the loop-boundary note-off mask that
// reset_sequences() arms for notes held across the wrap).
//
// PER-PASS verification across MANY wraps: the reported loop bug is
// INTERMITTENT ("silence for a few loops, then it comes back"), so an
// aggregate onset count can pass while whole passes are silent.  Every
// interior pass must deliver its complete onset set at the exact per-tick
// sample dues, plus the sustained pad's note-on and its boundary release.
//
// Env knobs (all optional; defaults are the CI acceptance run):
//   PATCHKNOB_LOOP_PASSES       wraps to run (default 24, min 4)
//   PATCHKNOB_LOOP_BPM          tempo (default 500; e.g. 490 makes the loop
//                               length a NON-multiple of the block size, so
//                               every wrap is a mid-block cycle split)
//   PATCHKNOB_LOOP_STALL_TAIL   N>0: every Nth pass, the sim scheduler stalls
//                               across the whole tail window (misses its wrap
//                               branch) exactly like a stalled output thread
//   PATCHKNOB_LOOP_STALL_SPLIT  N>0: every Nth pass, the tail enqueue
//                               STRADDLES the wrap (second half of the tail +
//                               the next pass's head land in the ring just
//                               after the engine has wrapped)
//   PATCHKNOB_LOOP_STRAY        default 1: on pass 2 enqueue one stray CC at
//                               tick == loop-right (a due the looping
//                               transport can never reach).  This is the
//                               REGRESSION for the ring-wedge bug: with the
//                               unreachable-due valve missing, that single
//                               entry blocks the FIFO forever and every pass
//                               from 3 on is TOTALLY silent.  0 disables.
//   PATCHKNOB_LOOP_STALL_RECOVER 1: with STALL_TAIL, the sim scheduler also
//                               mirrors the PROPOSED perform fix (rebuild
//                               from the audible tick when a wrap arrives
//                               without its tail branch having run) and the
//                               acceptance tolerates the few ms lost to the
//                               stall itself instead of a whole silent pass
double audio_app_loop_selftest()
{
    if ( !g_patch || !g_engine ) return -1.0;

    const bool wasRunning = g_engine->isRunning();
    if ( wasRunning ) g_engine->stop();
    const bool  wasModular = g_modular.load();
    const double oldTempo  = audio_app_tempo();

    auto envn = []( const char* k, long d ) -> long {
        const char* v = std::getenv( k ); return v ? strtol( v, nullptr, 10 ) : d; };
    const int  kPasses    = (int) std::max( 4L, envn( "PATCHKNOB_LOOP_PASSES", 24 ) );
    const double bpm      = std::max( 30.0, (double) envn( "PATCHKNOB_LOOP_BPM", 500 ) );
    const long stallTail  = envn( "PATCHKNOB_LOOP_STALL_TAIL",  0 );
    const long stallSplit = envn( "PATCHKNOB_LOOP_STALL_SPLIT", 0 );
    const bool strayTick  = envn( "PATCHKNOB_LOOP_STRAY", 1 ) != 0;
    const bool stallRecover = envn( "PATCHKNOB_LOOP_STALL_RECOVER", 0 ) != 0;

    audio_app_set_tempo( bpm );

    int outId = audio_app_patch_add_midi_out( -1 );
    if ( outId < 0 ) return -1.0;
    patch::MidiOutNode* mo =
        dynamic_cast<patch::MidiOutNode*>( g_patch->node( (patch::NodeId)outId ) );
    if ( !mo ) { audio_app_patch_remove( outId ); return -1.0; }
    audio_app_patch_connect( (int)g_patchMidiInId, 0, outId, 0 );
    audio_app_set_modular( true );

    const long long kLoopTicks = 4LL * kSequencerPpqn;     // one 4/4 bar
    const int       kGridStep  = 24;                       // 32nds
    const int       kPerPass   = (int)( kLoopTicks / kGridStep );
    const unsigned char kGridNote = 60, kPadNote = 72;
    audio_app_set_loop_ticks( 0, kLoopTicks, 1 );
    audio_app_transport_locate( 0 );
    audio_app_patch_set_playing( true );

    const long long loopLenSamples = audio_app_tick_to_sample( kLoopTicks );
    // expected absolute due of each grid onset, via the SAME tempo map the
    // drain uses (a flat ideal-IOI grid diverges from per-tick rounding)
    std::vector<long long> wantDue( (size_t)kPerPass );
    for ( int k = 0; k < kPerPass; ++k )
        wantDue[(size_t)k] = audio_app_tick_to_sample( (long long)k * kGridStep );

    // scheduler sim state (mirrors perform's audio-paced loop, including the
    // published lookahead the drain's wrap discriminator depends on)
    const long long lookahead = 2 * (long long) g_block
                              + (long long)( 0.005 * g_sr );
    audio_app_set_schedule_lookahead( lookahead );

    long long lastSched = -1;
    bool      tailDone  = false;
    bool      padOn     = false;          // sim's m_playing_notes for the pad
    // STALL_SPLIT carry-over: the part of the tail enqueue that lands after
    // the wrap.  from/to in ticks; head leftover recorded alongside.
    bool      splitPending = false;
    long long splitFrom = 0, splitTo = 0, splitLeftover = 0;
    unsigned long long splitGen = 0;   // finish only after the engine wraps

    auto enqueue_range = [&]( long long from, long long to ){   // ticks (from,to]
        // MONOTONIC by tick -- the ring's FIFO==due-order contract.
        const long long t0 = ( from < 0 ) ? 0 : ( from / kGridStep + 1 ) * kGridStep;
        for ( long long t = t0; t <= to; t += kGridStep )
        {
            if ( t == 0 )                // sustained pad: on at the pass head,
            {                            // released only by the boundary mask
                if ( padOn )             // boundary mask was missed (stall):
                    audio_app_route_midi( 0, 0x80, kPadNote, 0, 0 );
                    // ^ the tracker retrigger rule (put_event_on_bus):
                    //   release-then-strike on the same tick
                audio_app_route_midi( 0, 0x90, kPadNote, 100, 0 );
                padOn = true;
            }
            audio_app_route_midi( 0, 0x90, kGridNote, 100, t );
            audio_app_route_midi( 0, 0x80, kGridNote, 0,   t + kGridStep / 2 );
        }
    };
    auto queue_boundary_offs = [&](){    // == reset_sequences' queue_loop_note_offs
        if ( padOn )
        {
            audio_app_queue_loop_note_off( 0, 0, kPadNote );
            padOn = false;
        }
    };

    // per-pass delivery records (index == engine wrap generation delta)
    struct PassRec {
        std::vector<long long> gridOnDue;
        int gridOffs = 0, padOns = 0, padOffs = 0, strays = 0;
    };
    std::vector<PassRec> rec( (size_t)kPasses + 2 );

    std::vector<float> L( (size_t)g_block ), R( (size_t)g_block );
    float* out[2] = { L.data(), R.data() };

    const unsigned long long gen0 = audio_app_loop_wrap_generation();
    unsigned long long genSeen = gen0;
    bool strayDone = false;

    const long loopBlocks = (long)( loopLenSamples / g_block ) + 2;
    const long guardMax   = ( (long)kPasses + 2 ) * loopBlocks + 4000;
    for ( long guard = 0; guard < guardMax; ++guard )
    {
        const unsigned long long genNowPre = audio_app_loop_wrap_generation();
        const int passIdx = (int)( genNowPre - gen0 );
        if ( passIdx >= kPasses ) break;
        if ( genNowPre != genSeen )
        {
            genSeen = genNowPre;
            const bool tailWasQueued = tailDone || splitPending;
            tailDone = false;
            if ( stallRecover && !tailWasQueued )
            {
                // PROPOSED perform::output_func fix, mirrored here: the engine
                // wrapped but the scheduler never ran its tail branch (a stall
                // ate the whole lookahead window), so last_scheduled still
                // points near the END of the OLD pass and nothing would be
                // emitted until the horizon crawls past it again -- an entire
                // silent pass.  Rebuild from the loop start exactly like the
                // locate handshake rebuilds from the audible tick.
                audio_app_invalidate_future_schedule();
                lastSched = -1;
            }
        }

        const long long pos = g_transport.sample();

        // -- simulated output-thread scheduler ------------------------------
        const bool tailStalled = stallTail > 0 &&
            ( passIdx % stallTail ) == ( stallTail - 1 ) &&
            pos >= loopLenSamples - ( lookahead + 3LL * g_block );
        if ( splitPending )
        {
            if ( genNowPre != splitGen )
            {
                // the enqueue that straddled the wrap finishes now, AFTER the
                // engine has wrapped: stale tail first (FIFO), then the head
                enqueue_range( splitFrom, splitTo );
                queue_boundary_offs();
                enqueue_range( -1, splitLeftover );
                lastSched    = splitLeftover;
                splitPending = false;
            }
            // else: mid-enqueue stall continues until the engine wraps
        }
        else if ( !tailStalled )
        {
            const long long horizon = audio_app_sample_to_tick( pos + lookahead );
            if ( strayTick && !strayDone && passIdx == 2 )
            {
                audio_app_route_midi( 0, 0xB0, 1, 64, kLoopTicks ); // due == lend
                strayDone = true;
            }
            if ( horizon >= kLoopTicks )
            {
                if ( !tailDone )
                {
                    long long leftover = horizon - kLoopTicks;
                    if ( leftover > kLoopTicks ) leftover = 0;
                    const bool split = stallSplit > 0 &&
                        ( passIdx % stallSplit ) == ( stallSplit - 1 );
                    if ( split )
                    {
                        const long long mid = ( lastSched + kLoopTicks - 1 ) / 2;
                        enqueue_range( lastSched, mid );
                        splitPending  = true;   // rest lands after the wrap
                        splitFrom     = mid;
                        splitTo       = kLoopTicks - 1;
                        splitLeftover = leftover;
                        splitGen      = genNowPre;
                    }
                    else
                    {
                        enqueue_range( lastSched, kLoopTicks - 1 );
                        queue_boundary_offs();
                        enqueue_range( -1, leftover );      // next pass head
                        lastSched = leftover;
                    }
                    tailDone = true;
                }
            }
            else
            {
                tailDone = false;
                if ( horizon > lastSched )
                { enqueue_range( lastSched, horizon ); lastSched = horizon; }
            }
        }

        // -- one device block (the engine cycle-splits at the wrap inside) --
        audio_render( nullptr, 0, out, 2, g_block, g_sr );

        // -- collect deliveries, attributing the wrap block's pre-wrap
        //    sub-segment (dues near the loop end) to the pass that ended
        const unsigned long long genNowPost = audio_app_loop_wrap_generation();
        patch::TimedMidi tm;
        while ( mo->pop( tm ) )
        {
            int p = (int)( genNowPost - gen0 );
            if ( genNowPost != genNowPre && tm.dueSample * 2 > loopLenSamples )
                p = (int)( genNowPre - gen0 );
            if ( p < 0 || p >= (int)rec.size() ) continue;
            PassRec& r = rec[(size_t)p];
            const unsigned hi = tm.ev.status & 0xF0u;
            const bool on  = hi == 0x90u && tm.ev.data2 != 0;
            const bool off = hi == 0x80u || ( hi == 0x90u && tm.ev.data2 == 0 );
            if      ( on  && tm.ev.data1 == kGridNote ) r.gridOnDue.push_back( tm.dueSample );
            else if ( off && tm.ev.data1 == kGridNote ) r.gridOffs++;
            else if ( on  && tm.ev.data1 == kPadNote )  r.padOns++;
            else if ( off && tm.ev.data1 == kPadNote )  r.padOffs++;
            else if ( hi == 0xB0u && tm.ev.data1 == 1 ) r.strays++;
        }
    }
    const int passesRun = (int)( audio_app_loop_wrap_generation() - gen0 );

    // restore
    audio_app_patch_set_playing( false );
    audio_app_set_loop_ticks( 0, 0, 0 );
    audio_app_set_schedule_lookahead( 0 );
    audio_app_patch_disconnect( (int)g_patchMidiInId, 0, outId, 0 );
    audio_app_patch_remove( outId );
    audio_app_set_modular( wasModular );
    audio_app_set_tempo( oldTempo );
    audio_app_transport_locate( 0 );
    if ( wasRunning ) g_engine->start();

    // -- verify EVERY interior pass individually ----------------------------
    // Pass 0 spins up (locate + first lookahead) and the final pass is cut by
    // the run ending, so passes 1 .. passesRun-1 must each be complete.
    double maxDev = 0.0;
    int badPasses = 0, totalOnsets = 0;
    for ( int p = 1; p < passesRun && p < (int)rec.size(); ++p )
    {
        PassRec& r = rec[(size_t)p];
        totalOnsets += (int)r.gridOnDue.size();
        if ( std::getenv( "PATCHKNOB_LOOP_DUMP" ) && p <= 2 )
        {
            fprintf( stderr, "[loop-selftest] pass %d dues:", p );
            for ( size_t i = 0; i < r.gridOnDue.size() && i < 200; ++i )
                fprintf( stderr, " %lld", r.gridOnDue[i] );
            fprintf( stderr, "\n" );
        }
        std::sort( r.gridOnDue.begin(), r.gridOnDue.end() );
        // Stall-recovery is allowed to clamp the few ms the stall itself ate
        // to "now" (a block or two late); everything else must be EXACT.
        const double tol = stallRecover ? 4.0 * (double)g_block : 0.5;
        std::string missing;
        int nMissing = 0;
        int extra = (int)r.gridOnDue.size();
        size_t j = 0;
        for ( int k = 0; k < kPerPass; ++k )
        {
            while ( j < r.gridOnDue.size() &&
                    (double)r.gridOnDue[j] < (double)wantDue[(size_t)k] - tol )
                ++j;
            bool found = false;
            if ( j < r.gridOnDue.size() &&
                 fabs( (double)( r.gridOnDue[j] - wantDue[(size_t)k] ) ) <= tol )
            { found = true; ++j; --extra; }
            if ( !found )
            {
                ++nMissing;
                if ( missing.size() < 96 )
                {
                    missing += ' ';
                    missing += std::to_string( (long long)k * kGridStep );
                }
            }
        }
        // A stalled pass genuinely loses the ticks the stall window covered
        // (they were never scheduled and their moment passed); recovery is
        // judged on the PASS AFTER the stall not going silent.
        const int missAllow = stallRecover
            ? (int)( (double)( lookahead + 3LL * g_block )
                     / ( (double)loopLenSamples / (double)kPerPass ) ) + 2
            : 0;
        const bool bad = nMissing > missAllow || r.padOns != 1 || r.padOffs < 1;
        if ( bad )
        {
            ++badPasses;
            fprintf( stderr, "[loop-selftest]   pass %2d BAD: ons=%d/%d offs=%d "
                     "pad on=%d off=%d extra=%d missing-ticks:%s\n",
                     p, (int)r.gridOnDue.size(), kPerPass, r.gridOffs,
                     r.padOns, r.padOffs, extra,
                     missing.empty() ? " (none)" : missing.c_str() );
        }
        for ( size_t i = 0; i < r.gridOnDue.size(); ++i )
        {
            // nearest expected due, for the deviation report
            double best = 1e18;
            for ( int k = 0; k < kPerPass; ++k )
                best = std::min( best, fabs( (double)( r.gridOnDue[i] - wantDue[(size_t)k] ) ) );
            if ( best > maxDev && best < (double)loopLenSamples ) maxDev = best;
        }
    }
    const int interior = std::max( 0, std::min( passesRun - 1, (int)rec.size() - 1 ) - 1 + 1 );
    fprintf( stderr, "[loop-selftest] %d onsets over %d interior passes of %d "
             "(loop %lld samples, block %d, bpm %.0f), bad passes %d, "
             "max due deviation %.1f\n",
             totalOnsets, interior, passesRun, loopLenSamples, g_block, bpm,
             badPasses, maxDev );
    fflush( stderr );
    if ( passesRun < kPasses || badPasses ) return 1e9;
    // recovery mode tolerates the stall's own clamp-to-now by design, so the
    // pass/fail verdict is the per-pass audit, not the raw deviation
    if ( stallRecover ) return 0.0;
    return maxDev;
}

}} // namespace PatchKnob::app
