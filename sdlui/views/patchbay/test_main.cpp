//----------------------------------------------------------------------------
//  sdlui/views/patchbay/test_main.cpp
//
//  Standalone harness for the SDL patchbay node editor.  Builds a sample
//  model, mounts PatchView as a root widget, wires the callbacks to stdout,
//  and runs the app loop.  ESC / q quits, 'm' toggles Light<->Midnight.
//----------------------------------------------------------------------------
#include "gui.h"
#include "patch_view.h"

#include <cstdio>

using namespace PatchKnob::patchbay;

// tiny transparent root that just eats keys (theme toggle / quit)
class KeyRoot : public ui::Widget
{
public:
    void draw( ui::App& ) override {}                 // paints nothing
    bool on_key( ui::App& app, SDL_Keycode k ) override
    {
        if ( k == SDLK_m )
        {
            ui::set_mode( ui::mode() == ui::Mode::Midnight
                          ? ui::Mode::Light : ui::Mode::Midnight );
            app.request_redraw();
            return true;
        }
        if ( k == SDLK_q ) { app.running = false; return true; }
        return false;
    }
};

static Port audio( PortId id, const char* nm ) { return Port( id, PortKind::Audio, nm ); }
static Port midi ( PortId id, const char* nm ) { return Port( id, PortKind::Midi,  nm ); }

int main( int, char** )
{
    ui::set_mode( ui::Mode::Midnight );

    ui::App app;
    app.w = 1200; app.h = 720;
    if ( !app.init( "PatchKnob -- Patchbay (SDL)" ) )
        return 1;

    // ---- sample model ------------------------------------------------------
    std::vector<Node> nodes;

    Node midiIn; midiIn.id = 1; midiIn.name = "MIDI In"; midiIn.category = "MIDI";
    midiIn.x = 40;  midiIn.y = 90;
    midiIn.outPorts.push_back( midi( 1, "out" ) );
    nodes.push_back( midiIn );

    Node synth; synth.id = 2; synth.name = "Poly Synth"; synth.category = "Instrument";
    synth.x = 250; synth.y = 70;
    synth.inPorts.push_back( midi( 1, "midi" ) );
    synth.outPorts.push_back( audio( 1, "L" ) );
    synth.outPorts.push_back( audio( 2, "R" ) );
    nodes.push_back( synth );

    Node filt; filt.id = 3; filt.name = "Filter"; filt.category = "Effect";
    filt.x = 480; filt.y = 90;
    filt.inPorts.push_back( audio( 1, "in L" ) );
    filt.inPorts.push_back( audio( 2, "in R" ) );
    filt.outPorts.push_back( audio( 3, "L" ) );
    filt.outPorts.push_back( audio( 4, "R" ) );
    nodes.push_back( filt );

    Node verb; verb.id = 4; verb.name = "Reverb"; verb.category = "Effect";
    verb.x = 710; verb.y = 90;
    verb.inPorts.push_back( audio( 1, "in L" ) );
    verb.inPorts.push_back( audio( 2, "in R" ) );
    verb.outPorts.push_back( audio( 3, "L" ) );
    verb.outPorts.push_back( audio( 4, "R" ) );
    nodes.push_back( verb );

    Node out; out.id = 5; out.name = "Audio Out"; out.category = "Audio I/O";
    out.x = 950; out.y = 110;
    out.inPorts.push_back( audio( 1, "L" ) );
    out.inPorts.push_back( audio( 2, "R" ) );
    nodes.push_back( out );

    std::vector<Connection> conns;
    conns.push_back( Connection( PortRef(1,1), PortRef(2,1) ) ); // midi -> synth
    conns.push_back( Connection( PortRef(2,1), PortRef(3,1) ) ); // synth L -> filt L
    conns.push_back( Connection( PortRef(2,2), PortRef(3,2) ) ); // synth R -> filt R
    conns.push_back( Connection( PortRef(3,3), PortRef(4,1) ) ); // filt L -> verb L
    conns.push_back( Connection( PortRef(3,4), PortRef(4,2) ) ); // filt R -> verb R
    conns.push_back( Connection( PortRef(4,3), PortRef(5,1) ) ); // verb L -> out L
    conns.push_back( Connection( PortRef(4,4), PortRef(5,2) ) ); // verb R -> out R

    // ---- mount the view ----------------------------------------------------
    PatchView pv;
    pv.set_nodes( nodes );
    pv.set_connections( conns );

    pv.on_connect = []( NodeId fn, PortId fp, NodeId tn, PortId tp ) {
        printf( "[connect]     node %u port %u  ->  node %u port %u\n",
                fn, fp, tn, tp ); fflush( stdout );
    };
    pv.on_disconnect = []( NodeId fn, PortId fp, NodeId tn, PortId tp ) {
        printf( "[disconnect]  node %u port %u  ->  node %u port %u\n",
                fn, fp, tn, tp ); fflush( stdout );
    };
    pv.on_add_module = [&pv]( double x, double y, const std::string& cat ) {
        printf( "[add_module]  (%.0f, %.0f)  category='%s'\n",
                x, y, cat.c_str() ); fflush( stdout );
        // demo: actually drop a placeholder node so the menu is visibly live
        Node n; n.name = cat.empty() ? "Module" : cat;
        n.category = cat; n.x = x; n.y = y;
        n.inPorts.push_back( Port( 1, PortKind::Audio, "in" ) );
        n.outPorts.push_back( Port( 1, PortKind::Audio, "out" ) );
        pv.add_node( n );
    };
    pv.on_remove_node = []( NodeId id ) {
        printf( "[remove_node] node %u\n", id ); fflush( stdout );
    };
    pv.on_open_editor = []( NodeId id ) {
        printf( "[open_editor] node %u\n", id ); fflush( stdout );
    };

    KeyRoot keys;

    app.on_layout = [&]( ui::App& a ) {
        pv.rect = { 0, 0, a.w, a.h };     // fill the window; track resizes
    };
    app.roots.push_back( &keys );          // keys first so PatchView paints over
    app.roots.push_back( &pv );

    app.run();
    app.shutdown();
    return 0;
}
