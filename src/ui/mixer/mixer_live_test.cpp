//----------------------------------------------------------------------------
//
//  mixer_live_test - drives the LIVE mixer binding end to end.
//
//  Builds a real engine::MixerGraph (6 tracks, prepared at 48 kHz / 512), then
//  hands it to seq24::mixer::show_mixer() -- the actual deliverable path, which
//  builds a MixerWindow + MixerController with a ~30 Hz meter timer that reads
//  each Track's VU and drives the strips.
//
//  A separate "fake render loop" timer simulates the audio thread: every tick
//  it generates a varying stereo block per track, pushes it into that Track's
//  stereo VU meters (and the summed result into the master VU).  The result is
//  that the strips show MOVING VU meters fed by the real engine metering path.
//
//  A monitor timer prints the master + a couple of channel levels read back
//  from the graph so movement is verifiable on stdout even without eyeballing
//  the window.  The run self-terminates after N seconds (default 6, override
//  with argv[1] or $SEQ24_MIXER_TEST_SECS) so it is safe to run unattended.
//
//  Headless-safe: if there is no display it still runs the non-GUI VU
//  self-check and exits 0.
//
//-----------------------------------------------------------------------------

#include <gtkmm/main.h>
#include <glibmm/main.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mixerapp.h"                    // seq24::mixer::show_mixer
#include "engine/graph/mixer_graph.h"    // seq24::engine::MixerGraph
#include "engine/graph/track.h"          // seq24::engine::Track

using seq24::engine::MixerGraph;
using seq24::engine::Track;

namespace {

MixerGraph* g_graph        = 0;
double      g_phase        = 0.0;
int         g_render_ticks = 0;
const int   g_frames       = 512;

std::vector<float> g_bufL;
std::vector<float> g_bufR;
std::vector<float> g_mL;
std::vector<float> g_mR;

std::chrono::steady_clock::time_point g_start;

double elapsed_s()
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - g_start ).count();
}

// Fake audio-thread render: synthesize a varying stereo block per track and
// push it through the real engine VU meters (Track + master).
bool on_fake_render()
{
    if ( !g_graph )
        return false;

    const int N  = g_frames;
    const int nt = g_graph->trackCount();
    g_phase += 0.05;

    g_mL.assign( (size_t) N, 0.0f );
    g_mR.assign( (size_t) N, 0.0f );

    for ( int i = 0; i < nt; ++i )
    {
        Track* t = g_graph->track( i );
        if ( !t )
            continue;

        // Slowly-varying per-track amplitude, phase-offset per track.
        double a = 0.5 + 0.45 * std::sin( g_phase + i * 0.9 );
        if ( a < 0.0 ) a = 0.0;
        if ( a > 1.0 ) a = 1.0;
        const float ampL = (float) a;
        const float ampR = (float) ( a * 0.85 + 0.08 );

        for ( int s = 0; s < N; ++s )
        {
            const float ph = (float) s * 0.19f;
            g_bufL[s] = ampL * std::sin( ph );
            g_bufR[s] = ampR * std::sin( ph * 1.01f );
            g_mL[s]  += g_bufL[s];
            g_mR[s]  += g_bufR[s];
        }

        t->vuLeft().push ( g_bufL.data(), N );
        t->vuRight().push( g_bufR.data(), N );
    }

    if ( nt > 0 )
    {
        const float inv = 1.0f / (float) nt;
        for ( int s = 0; s < N; ++s ) { g_mL[s] *= inv; g_mR[s] *= inv; }
    }
    g_graph->masterVuLeft().push ( g_mL.data(), N );
    g_graph->masterVuRight().push( g_mR.data(), N );

    ++g_render_ticks;
    return true;
}

// Print levels read back from the graph so movement is visible on stdout.
bool on_monitor()
{
    if ( !g_graph )
        return false;

    Track* t0 = g_graph->track( 0 );
    Track* t3 = g_graph->track( 3 );
    std::printf(
        "[live] t=%4.1fs  master pk=%.3f rms=%.3f | ch0 pk=%.3f rms=%.3f | "
        "ch3 pk=%.3f  (render ticks=%d)\n",
        elapsed_s(),
        g_graph->masterVuLeft().peak(), g_graph->masterVuLeft().rms(),
        t0 ? t0->vuLeft().peak() : 0.0f, t0 ? t0->vuLeft().rms() : 0.0f,
        t3 ? t3->vuLeft().peak() : 0.0f,
        g_render_ticks );
    std::fflush( stdout );
    return true;
}

bool on_quit()
{
    std::printf( "[live] time up (%.1fs, %d render ticks); quitting.\n",
                 elapsed_s(), g_render_ticks );
    std::fflush( stdout );
    Gtk::Main::quit();
    return false;
}

} // namespace

int main( int argc, char** argv )
{
    // How long to run.
    double secs = 6.0;
    if ( argc > 1 )              secs = std::atof( argv[1] );
    else if ( const char* e = std::getenv( "SEQ24_MIXER_TEST_SECS" ) )
        secs = std::atof( e );
    if ( secs <= 0.0 ) secs = 6.0;

    // --- Build a real, prepared MixerGraph ---------------------------------
    MixerGraph graph;
    graph.setTrackCount( 6 );
    const bool prepared = graph.prepare( 48000.0, g_frames );
    g_graph = &graph;

    g_bufL.assign( (size_t) g_frames, 0.0f );
    g_bufR.assign( (size_t) g_frames, 0.0f );

    std::printf( "mixer_live_test: MixerGraph tracks=%d prepared=%d\n",
                 graph.trackCount(), (int) prepared );

    // --- Non-GUI VU self-check (works even headless) -----------------------
    {
        std::vector<float> loud( (size_t) g_frames, 0.0f );
        for ( int s = 0; s < g_frames; ++s )
            loud[s] = 0.8f * std::sin( (float) s * 0.2f );
        graph.track( 0 )->vuLeft().push( loud.data(), g_frames );
        const float pk = graph.track( 0 )->vuLeft().peak();
        std::printf( "[selftest] pushed a block into track0: VU peak=%.3f -> %s\n",
                     pk, ( pk > 0.1f ) ? "PASS" : "FAIL" );
        graph.track( 0 )->vuLeft().reset();
    }

    // --- GUI (headless-safe) -----------------------------------------------
    Gtk::Main* kit = 0;
    try
    {
        kit = new Gtk::Main( argc, argv );
    }
    catch ( const Glib::Error& e )
    {
        std::printf( "mixer_live_test: no display (%s); build OK, skipping GUI.\n",
                     e.what().c_str() );
        return 0;
    }
    catch ( ... )
    {
        std::printf( "mixer_live_test: no display; build OK, skipping GUI.\n" );
        return 0;
    }

    g_start = std::chrono::steady_clock::now();

    // The deliverable: build + show the live mixer bound to our graph.
    seq24::mixer::show_mixer( &graph );

    // Fake audio-thread render (~50 Hz) feeding the engine VU meters.
    Glib::signal_timeout().connect( sigc::ptr_fun( &on_fake_render ), 20 );
    // Level monitor to stdout (~2 Hz).
    Glib::signal_timeout().connect( sigc::ptr_fun( &on_monitor ), 500 );
    // Self-terminate.
    Glib::signal_timeout().connect( sigc::ptr_fun( &on_quit ),
                                    (unsigned) ( secs * 1000.0 ) );

    std::printf( "mixer_live_test: running live mixer for %.1fs...\n", secs );
    std::fflush( stdout );

    kit->run();

    delete kit;
    std::printf( "mixer_live_test: done (%d render ticks).\n", g_render_ticks );
    return 0;
}
