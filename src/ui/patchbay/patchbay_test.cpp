//----------------------------------------------------------------------------
//
//  patchbay_test  --  standalone driver for the seq24 modular node-editor.
//
//  Populates a PatchCanvas with four dummy nodes wired into today's signal
//  flow  (MIDI in -> instrument -> mixer -> audio out)  plus a couple of
//  connections, mirrors the canvas' request signals to stdout so the engine
//  seam is visible, applies the monochrome runtime palette, and (so the
//  build/test loop stays non-interactive) auto-quits after a few seconds.
//
//  You can drive it by hand while it is up: drag from any OUT dot to a matching
//  IN dot to patch, click a curve to delete it, drag node boxes to move them,
//  right-click the empty canvas for "Add Module", right-click a node for
//  Open Editor / Remove.
//
//  Headless-safe: if no display is available it prints a notice and exits 0 so
//  the build is still validated.
//
//  Build (MSYS2 / mingw64):
//      export PATH="/c/msys64/mingw64/bin:$PATH"
//      cmake -G "MinGW Makefiles" -S . -B build
//      cmake --build build
//      ./build/patchbay_test.exe
//
//-----------------------------------------------------------------------------

#include <gtkmm/main.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>

#include <cstdio>
#include <string>

#include "patch_canvas.h"
#include "../palette.h"

using seq24::patchbay::PatchCanvas;
using seq24::patchbay::Node;
using seq24::patchbay::Port;
using seq24::patchbay::Connection;
using seq24::patchbay::PortRef;
using seq24::patchbay::PortKind;
using seq24::patchbay::NodeId;
using seq24::patchbay::PortId;

namespace {

// --- request-signal loggers (stand in for the engine coordinator) -----------
void on_connect( NodeId sn, PortId sp, NodeId dn, PortId dp )
{
    std::printf( "[patchbay] CONNECT  node %u port %u  ->  node %u port %u\n",
                 sn, sp, dn, dp );
    std::fflush( stdout );
}

void on_disconnect( NodeId sn, PortId sp, NodeId dn, PortId dp )
{
    std::printf( "[patchbay] DISCONNECT node %u port %u -> node %u port %u\n",
                 sn, sp, dn, dp );
    std::fflush( stdout );
}

void on_add( double x, double y )
{
    std::printf( "[patchbay] ADD MODULE at (%.0f, %.0f)  "
                 "(coordinator would open the plugin browser)\n", x, y );
    std::fflush( stdout );
}

void on_add_cat( const std::string& cat, double x, double y )
{
    std::printf( "[patchbay] ADD MODULE category='%s' at (%.0f, %.0f)\n",
                 cat.c_str(), x, y );
    std::fflush( stdout );
}

void on_remove( NodeId n )
{
    std::printf( "[patchbay] REMOVE node %u\n", n );
    std::fflush( stdout );
}

void on_open_editor( NodeId n )
{
    std::printf( "[patchbay] OPEN EDITOR node %u\n", n );
    std::fflush( stdout );
}

// --- demo graph -------------------------------------------------------------
void populate( PatchCanvas& c )
{
    // 1) MIDI input source.
    Node midi_in;
    midi_in.id       = 1;
    midi_in.name     = "MIDI In";
    midi_in.category = "MIDI";
    midi_in.x = 40;  midi_in.y = 70;
    midi_in.outPorts.push_back( Port( 1, PortKind::Midi,  "midi out" ) );

    // 2) Instrument: MIDI in -> stereo audio out.
    Node instr;
    instr.id       = 2;
    instr.name     = "Instrument";
    instr.category = "Instrument";
    instr.x = 250;  instr.y = 60;
    instr.inPorts.push_back(  Port( 1, PortKind::Midi,  "midi in" ) );
    instr.outPorts.push_back( Port( 1, PortKind::Audio, "out L/R" ) );

    // 3) Mixer: two audio ins -> main out.
    Node mixer;
    mixer.id       = 3;
    mixer.name     = "Mixer";
    mixer.category = "Mixer";
    mixer.x = 470;  mixer.y = 120;
    mixer.inPorts.push_back(  Port( 1, PortKind::Audio, "in 1" ) );
    mixer.inPorts.push_back(  Port( 2, PortKind::Audio, "in 2" ) );
    mixer.outPorts.push_back( Port( 1, PortKind::Audio, "main" ) );

    // 4) Audio output sink.
    Node out;
    out.id       = 4;
    out.name     = "Audio Out";
    out.category = "Audio I/O";
    out.x = 690;  out.y = 150;
    out.inPorts.push_back( Port( 1, PortKind::Audio, "in L/R" ) );

    std::vector<Node> nodes;
    nodes.push_back( midi_in );
    nodes.push_back( instr );
    nodes.push_back( mixer );
    nodes.push_back( out );
    c.set_nodes( nodes );

    // A couple of pre-wired connections (drawn straight away).
    std::vector<Connection> conns;
    conns.push_back( Connection( PortRef( 1, 1 ), PortRef( 2, 1 ) ) ); // midi
    conns.push_back( Connection( PortRef( 2, 1 ), PortRef( 3, 1 ) ) ); // instr->mix
    conns.push_back( Connection( PortRef( 3, 1 ), PortRef( 4, 1 ) ) ); // mix->out
    c.set_connections( conns );
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
        std::printf( "patchbay_test: no display available (%s); "
                     "build OK, skipping GUI run.\n", e.what().c_str() );
        return 0;
    }
    catch ( ... )
    {
        std::printf( "patchbay_test: no display available; "
                     "build OK, skipping GUI run.\n" );
        return 0;
    }

    synth::set_palette( synth::PALETTE_ANCIENT );   // strict black & white

    Gtk::Window win;
    win.set_title( "seq24 -- Patchbay / Node Editor (test)" );
    win.set_default_size( 780, 500 );

    PatchCanvas canvas;
    canvas.signal_connect_request().connect(       sigc::ptr_fun( &on_connect ) );
    canvas.signal_disconnect_request().connect(    sigc::ptr_fun( &on_disconnect ) );
    canvas.signal_add_module_request().connect(    sigc::ptr_fun( &on_add ) );
    canvas.signal_add_module_in_category().connect(sigc::ptr_fun( &on_add_cat ) );
    canvas.signal_remove_node().connect(           sigc::ptr_fun( &on_remove ) );
    canvas.signal_open_node_editor().connect(      sigc::ptr_fun( &on_open_editor ) );

    populate( canvas );

    win.add( canvas );
    win.show_all();

    std::printf( "patchbay_test: canvas open with 4 demo nodes + 3 wires; "
                 "auto-quitting in ~6s.\n" );
    std::printf( "  drag OUT->IN to patch, click a curve to delete, drag boxes "
                 "to move, right-click for menus.\n" );
    std::fflush( stdout );

    // Non-interactive: close by itself after ~6 seconds.
    Glib::signal_timeout().connect( sigc::ptr_fun( &quit_soon ), 6000 );

    kit->run( win );

    delete kit;
    return 0;
}
