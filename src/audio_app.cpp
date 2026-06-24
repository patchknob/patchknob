//----------------------------------------------------------------------------
//  audio_app.cpp -- owns the hand-rolled audio engine, master mixer graph, and
//  plugin host for the seq24 Windows port, and wires them together:
//
//      AudioEngine (RtAudio/WASAPI)  --render callback-->  MixerGraph
//      MixerGraph                     --hosts-->           Track[] (VST insts + FX)
//      PluginHost                     --scans/instantiates-> VST2 / VST3 plugins
//
//  Step 4b scope: open the device and run a (silent) master graph so the audio
//  thread is live.  The sequencer->VST MIDI bridge and per-track instruments are
//  layered on in the following integration step.
//----------------------------------------------------------------------------
#include "audio_app.h"

#include "engine/audio/audio_engine.h"
#include "engine/graph/mixer_graph.h"
#include "engine/host/plugin_host.h"

#include <cstdio>

using namespace seq24::engine;

namespace {
    AudioEngine* g_engine = nullptr;
    MixerGraph*  g_graph  = nullptr;
    PluginHost*  g_host   = nullptr;
    bool         g_running = false;
}

namespace seq24 { namespace app {

bool audio_app_init()
{
    if ( g_running ) return true;

    g_engine = new AudioEngine();
    g_graph  = new MixerGraph();
    g_host   = new PluginHost();

    if ( !g_engine->open( 48000, 512, 2 ) )
    {
        fprintf( stderr, "[audio] could not open output device: %s\n",
                 g_engine->lastError().c_str() );
        return false;   // app continues as a pure-MIDI sequencer
    }

    const double sr    = (double) g_engine->sampleRate();
    const int    block = g_engine->blockSize();

    g_graph->prepare( sr, block );

    // The master mixer graph is the single audio producer.
    g_engine->setRenderCallback(
        []( float** out, int numChannels, int nframes, double sampleRate )
        {
            g_graph->render( out, numChannels, nframes, sampleRate );
        } );

    if ( !g_engine->start() )
    {
        fprintf( stderr, "[audio] could not start stream: %s\n",
                 g_engine->lastError().c_str() );
        return false;
    }

    g_running = true;
    fprintf( stderr, "[audio] engine running: %.0f Hz, %d-frame blocks\n", sr, block );
    return true;
}

void audio_app_shutdown()
{
    if ( g_engine )
    {
        g_engine->stop();
        g_engine->close();
    }
    delete g_engine; g_engine = nullptr;
    delete g_graph;  g_graph  = nullptr;
    delete g_host;   g_host   = nullptr;
    g_running = false;
}

bool audio_app_running() { return g_running; }

}} // namespace seq24::app
