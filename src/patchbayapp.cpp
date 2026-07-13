//----------------------------------------------------------------------------
//
//  patchbayapp.cpp  --  seq24 Windows port, modular patchbay coordinator (impl).
//
//  Controller for the header-only node-editor canvas (src/ui/patchbay/).  See
//  patchbayapp.h.  It:
//    * opens a heap Gtk::Window holding a PatchCanvas (self-deletes on close);
//    * seeds it with a representative default patch that mirrors today's signal
//      flow (MIDI in -> instrument -> mixer -> audio out);
//    * wires the canvas' request signals to the existing coordinators:
//        - "Add Module"  -> seq24::rack::show_plugin_browser(track)
//        - "Open Editor" -> seq24::rack::open_track_editor(track)
//      and leaves the connect / disconnect / remove requests as the documented
//      seam for the engine graph binding (built in parallel) to mutate the real
//      PatchGraph.
//
//  Deliberately depends ONLY on rackapp.h, the runtime palette and the
//  header-only UI, so it compiles as one translation unit into seq24 with no
//  new link dependencies.
//
//-----------------------------------------------------------------------------

#include "patchbayapp.h"

#include "rackapp.h"                    // seq24::rack::show_plugin_browser / editor
#include "ui/patchbay/patch_canvas.h"  // header-only node-editor canvas
#include "ui/palette.h"

#include <glibmm/main.h>
#include <gtkmm/window.h>
#include <sigc++/sigc++.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace seq24 {
namespace patchbay {

namespace {

using seq24::patchbay::PatchCanvas;
using seq24::patchbay::Node;
using seq24::patchbay::Port;
using seq24::patchbay::Connection;
using seq24::patchbay::PortRef;
using seq24::patchbay::PortKind;
using seq24::patchbay::NodeId;
using seq24::patchbay::PortId;

// ---------------------------------------------------------------------------
//  One open patchbay window + its wiring.
//
//  Derives sigc::trackable so that when we delete the session (on window close)
//  every connected signal auto-disconnects -- no dangling slot touching a freed
//  canvas.  The window is a heap Gtk::Window that we own and free here.
// ---------------------------------------------------------------------------
struct PatchbaySession : public sigc::trackable
{
    Gtk::Window* window;
    PatchCanvas* canvas;

    // Maps a canvas node -> the mixer track it stands for, so "Open Editor" /
    // "Add Module" can reuse the existing per-track rack coordinator.  The
    // engine graph binding replaces this with a real NodeId<->engine map.
    std::map< NodeId, int > nodeTrack;

    PatchbaySession()
        : window( new Gtk::Window() )
        , canvas( Gtk::manage( new PatchCanvas() ) )   // owned by the window
    {
        window->set_title( "seq24 -- Patchbay / Node Editor" );
        window->set_default_size( 820, 520 );
        window->add( *canvas );

        canvas->signal_add_module_request().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_add_module ) );
        canvas->signal_add_module_in_category().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_add_module_cat ) );
        canvas->signal_open_node_editor().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_open_editor ) );
        canvas->signal_connect_request().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_connect ) );
        canvas->signal_disconnect_request().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_disconnect ) );
        canvas->signal_remove_node().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_remove ) );

        window->signal_hide().connect(
            sigc::mem_fun( *this, &PatchbaySession::on_hide ) );

        seed_default_patch();
    }

    // A representative default graph reproducing today's fixed signal chain.
    void seed_default_patch()
    {
        std::vector<Node> nodes;

        Node midi_in;
        midi_in.id = 1; midi_in.name = "MIDI In"; midi_in.category = "MIDI";
        midi_in.x = 40; midi_in.y = 80;
        midi_in.outPorts.push_back( Port( 1, PortKind::Midi, "midi out" ) );
        nodes.push_back( midi_in );

        Node instr;
        instr.id = 2; instr.name = "Track 1 Instrument";
        instr.category = "Instrument"; instr.x = 260; instr.y = 70;
        instr.inPorts.push_back(  Port( 1, PortKind::Midi,  "midi in" ) );
        instr.outPorts.push_back( Port( 1, PortKind::Audio, "out L/R" ) );
        nodes.push_back( instr );
        nodeTrack[ 2 ] = 0;   // instrument node stands for mixer track 0

        Node mixer;
        mixer.id = 3; mixer.name = "Mixer"; mixer.category = "Mixer";
        mixer.x = 500; mixer.y = 130;
        mixer.inPorts.push_back(  Port( 1, PortKind::Audio, "in 1" ) );
        mixer.inPorts.push_back(  Port( 2, PortKind::Audio, "in 2" ) );
        mixer.outPorts.push_back( Port( 1, PortKind::Audio, "main" ) );
        nodes.push_back( mixer );

        Node out;
        out.id = 4; out.name = "Audio Out"; out.category = "Audio I/O";
        out.x = 720; out.y = 160;
        out.inPorts.push_back( Port( 1, PortKind::Audio, "in L/R" ) );
        nodes.push_back( out );

        canvas->set_nodes( nodes );

        std::vector<Connection> conns;
        conns.push_back( Connection( PortRef( 1, 1 ), PortRef( 2, 1 ) ) );
        conns.push_back( Connection( PortRef( 2, 1 ), PortRef( 3, 1 ) ) );
        conns.push_back( Connection( PortRef( 3, 1 ), PortRef( 4, 1 ) ) );
        canvas->set_connections( conns );
    }

    int track_of( NodeId n ) const
    {
        std::map< NodeId, int >::const_iterator it = nodeTrack.find( n );
        return ( it != nodeTrack.end() ) ? it->second : 0;
    }

    // --- canvas request signals -> engine / rack coordinators ---------------
    void on_add_module( double /*x*/, double /*y*/ )
    {
        // The plugin browser assigns to a mixer track; a real graph binding
        // would create a fresh node at (x,y) and route the chosen plugin to it.
        seq24::rack::show_plugin_browser( 0 );
    }

    void on_add_module_cat( const std::string& /*cat*/, double x, double y )
    {
        on_add_module( x, y );
    }

    void on_open_editor( NodeId n )
    {
        seq24::rack::open_track_editor( track_of( n ) );
    }

    // Seam for the parallel engine graph binding: translate to PatchGraph edits.
    void on_connect( NodeId sn, PortId sp, NodeId dn, PortId dp )
    {
        std::printf( "[patchbay] connect req %u:%u -> %u:%u\n", sn, sp, dn, dp );
        std::fflush( stdout );
    }
    void on_disconnect( NodeId sn, PortId sp, NodeId dn, PortId dp )
    {
        std::printf( "[patchbay] disconnect req %u:%u -> %u:%u\n", sn, sp, dn, dp );
        std::fflush( stdout );
    }
    void on_remove( NodeId n )
    {
        std::printf( "[patchbay] remove req node %u\n", n );
        std::fflush( stdout );
    }

    void show() { window->show_all(); }

    void on_hide()
    {
        // Defer teardown off the signal emission; deleting the session drops the
        // trackable and auto-disconnects every wired slot.
        Glib::signal_idle().connect(
            sigc::bind( sigc::ptr_fun( &PatchbaySession::deferred_delete ), this ) );
    }

    static bool deferred_delete( PatchbaySession* self )
    {
        delete self->window;   // destroys the managed canvas too
        delete self;
        return false;          // one-shot
    }
};

} // namespace

// ---------------------------------------------------------------------------
//  public entry point
// ---------------------------------------------------------------------------
void show_patchbay()
{
    PatchbaySession* s = new PatchbaySession();
    s->show();
}

} // namespace patchbay
} // namespace seq24
