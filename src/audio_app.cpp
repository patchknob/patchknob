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

#include <atomic>
#include <vector>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

using namespace seq24::engine;

namespace {

AudioEngine* g_engine  = nullptr;
MixerGraph*  g_graph   = nullptr;
PluginHost*  g_host    = nullptr;
bool         g_running = false;
double       g_sr      = 48000.0;
int          g_block   = 512;

std::vector<IPluginInstance*> g_owned;   // instruments/fx we must release

// --- lock-free SPSC MIDI ring (producer = sequencer thread, consumer = audio) --
struct RouteMsg { unsigned char track, status, d1, d2; };
const unsigned RING_SIZE = 1u << 14;      // 16384
const unsigned RING_MASK = RING_SIZE - 1;
RouteMsg               g_ring[RING_SIZE];
std::atomic<unsigned>  g_head{0};         // written by producer
std::atomic<unsigned>  g_tail{0};         // written by consumer

// --- per-block staging (audio thread only) ---------------------------------
const int MAX_EV = 256;
MidiEvent        s_trackMidi[seq24::app::AUDIO_APP_MAX_TRACKS][MAX_EV];
int              s_counts   [seq24::app::AUDIO_APP_MAX_TRACKS];
TrackBlockInput  s_inputs   [seq24::app::AUDIO_APP_MAX_TRACKS];

// The single audio render callback: drain queued MIDI into per-track buffers,
// then let the graph render every track's instrument + FX + mix.
void audio_render( float** out, int numChannels, int nframes, double /*sr*/ )
{
    const int NT = seq24::app::AUDIO_APP_MAX_TRACKS;
    for ( int t = 0; t < NT; ++t ) s_counts[t] = 0;

    unsigned tail = g_tail.load( std::memory_order_relaxed );
    const unsigned head = g_head.load( std::memory_order_acquire );
    while ( tail != head )
    {
        const RouteMsg& m = g_ring[tail & RING_MASK];
        ++tail;
        int t = m.track;
        if ( t >= 0 && t < NT && s_counts[t] < MAX_EV )
        {
            MidiEvent& e = s_trackMidi[t][ s_counts[t]++ ];
            e.sampleOffset = 0;          // block-granular for now
            e.status = m.status;
            e.data1  = m.d1;
            e.data2  = m.d2;
        }
    }
    g_tail.store( tail, std::memory_order_release );

    for ( int t = 0; t < NT; ++t )
    {
        s_inputs[t].midi   = s_trackMidi[t];
        s_inputs[t].nMidi  = s_counts[t];
        s_inputs[t].autom  = nullptr;
        s_inputs[t].nAutom = 0;
    }

    g_graph->renderBlock( out, numChannels, nframes, s_inputs, NT );
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

namespace seq24 { namespace app {

bool audio_app_init()
{
    if ( g_running ) return true;

    g_engine = new AudioEngine();
    g_graph  = new MixerGraph();
    g_host   = new PluginHost();

    g_graph->setTrackCount( AUDIO_APP_MAX_TRACKS );   // before prepare()

    if ( !g_engine->open( 48000, 512, 2 ) )
    {
        fprintf( stderr, "[audio] could not open output device: %s\n",
                 g_engine->lastError().c_str() );
        return false;
    }

    g_sr    = (double) g_engine->sampleRate();
    g_block = g_engine->blockSize();

    g_graph->prepare( g_sr, g_block );
    g_graph->setTransport( 120.0, 0, true );

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

void audio_app_shutdown()
{
    if ( g_engine ) { g_engine->stop(); g_engine->close(); }
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

MixerGraph*  audio_app_graph()  { return g_graph;  }
PluginHost*  audio_app_host()   { return g_host;   }
AudioEngine* audio_app_engine() { return g_engine; }

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
                           unsigned char d1, unsigned char d2 )
{
    unsigned head = g_head.load( std::memory_order_relaxed );
    unsigned tail = g_tail.load( std::memory_order_acquire );
    if ( head - tail >= RING_SIZE ) return;   // full: drop (never blocks audio)
    RouteMsg& m = g_ring[head & RING_MASK];
    m.track  = (unsigned char) track;
    m.status = status;
    m.d1     = d1;
    m.d2     = d2;
    g_head.store( head + 1, std::memory_order_release );
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

}} // namespace seq24::app
