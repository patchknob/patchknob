//----------------------------------------------------------------------------
//
//  mixer_test - standalone driver for the seq24 mixer UI.
//
//  Opens a MixerWindow with ~6 dummy channels + a master, attaches a Glib
//  timer that animates the VU meters with moving sine / noise levels, and
//  shows a couple of dummy insert-FX names per channel.  Also wires up the
//  per-strip sigc signals to stdout so you can see the engine-facing API fire.
//
//  Headless-safe: if Gtk::Main cannot connect to a display it prints a notice
//  and exits 0 (so the build/test still "passes" on a headless box).
//
//-----------------------------------------------------------------------------

#include <gtkmm/main.h>
#include <glibmm/main.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>

#include "mixer_window.h"

using namespace seq24::mixer;

namespace {

MixerWindow* g_win = 0;
double       g_phase = 0.0;

float frand()
{
    return (float) std::rand() / (float) RAND_MAX;
}

// Animate the meters.  Returns true to keep the timer alive.
bool on_tick()
{
    if ( !g_win )
        return false;

    g_phase += 0.08;

    int n = g_win->channel_count();
    float master_pkL = 0.0f, master_pkR = 0.0f;
    float master_rmL = 0.0f, master_rmR = 0.0f;

    for ( int i = 0; i < n; ++i )
    {
        // Per-channel sine with a phase offset, plus a little noise.
        double ph = g_phase + i * 0.9;
        float base = 0.5f + 0.45f * (float) std::sin( ph );
        float rms  = base * ( 0.6f + 0.15f * frand() );
        float peak = rms + 0.15f + 0.2f * frand();

        float pkL = peak * ( 0.85f + 0.3f * frand() );
        float pkR = peak * ( 0.85f + 0.3f * frand() );
        float rmL = rms  * ( 0.9f  + 0.1f * frand() );
        float rmR = rms  * ( 0.9f  + 0.1f * frand() );

        g_win->set_channel_levels( i, pkL, pkR, rmL, rmR );

        if ( pkL > master_pkL ) master_pkL = pkL;
        if ( pkR > master_pkR ) master_pkR = pkR;
        master_rmL += rmL * 0.4f;
        master_rmR += rmR * 0.4f;
    }

    g_win->set_master_levels( master_pkL, master_pkR,
                              std::min(1.0f, master_rmL),
                              std::min(1.0f, master_rmR) );
    return true;
}

// Demo: print signal activity so the engine-facing API is visible.
void on_gain( int ch, double g )
{
    std::printf( "[ch %d] gain -> %.3f\n", ch, g ); std::fflush(stdout);
}
void on_pan( int ch, double p )
{
    std::printf( "[ch %d] pan  -> %.3f\n", ch, p ); std::fflush(stdout);
}
void on_mute( int ch, bool m )
{
    std::printf( "[ch %d] mute -> %d\n", ch, (int)m ); std::fflush(stdout);
}
void on_solo( int ch, bool s )
{
    std::printf( "[ch %d] solo -> %d\n", ch, (int)s ); std::fflush(stdout);
}
void on_add_fx( int ch )
{
    std::printf( "[ch %d] add FX requested\n", ch ); std::fflush(stdout);
}
void on_remove_fx( int ch, int idx )
{
    std::printf( "[ch %d] remove FX idx %d\n", ch, idx ); std::fflush(stdout);
}

} // namespace

int main( int argc, char** argv )
{
    // Gtk::Main throws if there is no display.  Catch it so a headless build
    // still exits cleanly.
    Gtk::Main* kit = 0;
    try
    {
        kit = new Gtk::Main( argc, argv );
    }
    catch ( const Glib::Error& e )
    {
        std::printf( "mixer_test: no display available (%s); "
                     "build OK, skipping GUI run.\n", e.what().c_str() );
        return 0;
    }
    catch ( ... )
    {
        std::printf( "mixer_test: no display available; "
                     "build OK, skipping GUI run.\n" );
        return 0;
    }

    MixerWindow win;
    g_win = &win;

    const char* names[] = {
        "Drums", "Bass", "Lead Synth", "Pad", "Vocals", "FX Bus"
    };
    const char* fx_a[] = {
        "EQ-3 Band", "Compressor", "Reverb", "Chorus", "Delay", "Saturator"
    };
    const char* fx_b[] = {
        "Limiter", "Stereo Width", "Gate", "Phaser", "Tape", "BitCrush"
    };

    for ( int i = 0; i < 6; ++i )
    {
        int idx = win.add_channel( names[i] );

        std::vector<std::string> fx;
        fx.push_back( fx_a[i] );
        fx.push_back( fx_b[i] );
        win.set_channel_fx( idx, fx );

        // Wire signals (engine would do this).  bind() captures the index.
        ChannelStrip* s = win.channel( idx );
        s->signal_gain_changed().connect( sigc::bind<0>(
            sigc::ptr_fun( &on_gain ), idx ) );
        s->signal_pan_changed().connect( sigc::bind<0>(
            sigc::ptr_fun( &on_pan ), idx ) );
        s->signal_mute_toggled().connect( sigc::bind<0>(
            sigc::ptr_fun( &on_mute ), idx ) );
        s->signal_solo_toggled().connect( sigc::bind<0>(
            sigc::ptr_fun( &on_solo ), idx ) );
        s->signal_add_fx().connect( sigc::bind(
            sigc::ptr_fun( &on_add_fx ), idx ) );
        s->signal_remove_fx().connect( sigc::bind<0>(
            sigc::ptr_fun( &on_remove_fx ), idx ) );
    }

    // Master master chain demo.
    std::vector<std::string> mfx;
    mfx.push_back( "Bus Comp" );
    mfx.push_back( "Brickwall" );
    win.master()->set_fx_list( mfx );

    // ~30 Hz animation timer.
    Glib::signal_timeout().connect( sigc::ptr_fun( &on_tick ), 33 );

    win.show_all();
    kit->run( win );

    delete kit;
    return 0;
}
