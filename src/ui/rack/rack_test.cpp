//----------------------------------------------------------------------------
//
//  rack_test  --  standalone driver for the seq24 VST "rack" plugin browser.
//
//  Feeds the PluginBrowser a small HARD-CODED list of PluginDescriptors (so it
//  renders instantly -- the real out-of-process scan is minutes long and is
//  exercised by the engine-coupled controller in src/rackapp.cpp, not here).
//  Wires the browser's assign signals to stdout so the engine-facing API is
//  visible, applies the monochrome look, and auto-quits after a few seconds so
//  the build/test loop stays non-interactive.
//
//  Headless-safe: if no display is available it prints a notice and exits 0.
//
//-----------------------------------------------------------------------------

#include <gtkmm/main.h>
#include <glibmm/main.h>

#include <cstdio>
#include <string>
#include <vector>

#include "plugin_browser.h"

using seq24::rack::PluginBrowser;
using seq24::engine::PluginDescriptor;
using seq24::engine::PluginFormat;

namespace {

PluginDescriptor mk( PluginFormat fmt, const char* name, const char* vendor,
                     bool instr, int in, int out )
{
    PluginDescriptor d;
    d.format       = fmt;
    d.name         = name;
    d.vendor       = vendor;
    d.path         = std::string( "C:/VST/" ) + name;
    d.uid          = "";
    d.isInstrument = instr;
    d.numAudioIn   = in;
    d.numAudioOut  = out;
    return d;
}

void on_load_instrument( const PluginDescriptor& d )
{
    std::printf( "[rack] LOAD INSTRUMENT: %s (%s)\n",
                 d.name.c_str(), d.vendor.c_str() );
    std::fflush( stdout );
}

void on_add_fx( const PluginDescriptor& d )
{
    std::printf( "[rack] ADD FX: %s (%s)\n",
                 d.name.c_str(), d.vendor.c_str() );
    std::fflush( stdout );
}

bool quit_soon()
{
    Gtk::Main::quit();
    return false;
}

} // namespace

int main( int argc, char** argv )
{
    Gtk::Main* kit = 0;
    try
    {
        kit = new Gtk::Main( argc, argv );
    }
    catch ( const Glib::Error& e )
    {
        std::printf( "rack_test: no display available (%s); "
                     "build OK, skipping GUI run.\n", e.what().c_str() );
        return 0;
    }
    catch ( ... )
    {
        std::printf( "rack_test: no display available; "
                     "build OK, skipping GUI run.\n" );
        return 0;
    }

    PluginBrowser browser( 3 /* pretend we are assigning to track 3 */ );

    browser.signal_load_instrument().connect( sigc::ptr_fun( &on_load_instrument ) );
    browser.signal_add_fx().connect(          sigc::ptr_fun( &on_add_fx ) );

    // A small, representative hard-coded catalogue.
    std::vector<PluginDescriptor> plugins;
    plugins.push_back( mk( PluginFormat::VST2, "Surge XT",       "Surge Synth Team", true,  0, 2 ) );
    plugins.push_back( mk( PluginFormat::VST3, "Vital",          "Matt Tytel",       true,  0, 2 ) );
    plugins.push_back( mk( PluginFormat::VST2, "Dexed",          "Digital Suburban", true,  0, 2 ) );
    plugins.push_back( mk( PluginFormat::VST3, "TAL-NoiseMaker", "TAL Software",     true,  0, 2 ) );
    plugins.push_back( mk( PluginFormat::VST2, "TDR Nova",       "Tokyo Dawn Labs",  false, 2, 2 ) );
    plugins.push_back( mk( PluginFormat::VST3, "Valhalla Room",  "Valhalla DSP",     false, 2, 2 ) );
    plugins.push_back( mk( PluginFormat::VST2, "OTT",            "Xfer Records",     false, 2, 2 ) );
    plugins.push_back( mk( PluginFormat::VST3, "Pro-Q 3",        "FabFilter",        false, 2, 2 ) );
    plugins.push_back( mk( PluginFormat::VST2, "Ozone Imager",   "iZotope",          false, 2, 2 ) );

    browser.populate( plugins );

    std::printf( "rack_test: browser open with %d demo plugins; "
                 "auto-quitting in ~3s.\n", (int) plugins.size() );
    std::fflush( stdout );

    // Non-interactive: close by itself after ~3 seconds.
    Glib::signal_timeout().connect( sigc::ptr_fun( &quit_soon ), 3000 );

    browser.show_all();
    kit->run( browser );

    delete kit;
    return 0;
}
