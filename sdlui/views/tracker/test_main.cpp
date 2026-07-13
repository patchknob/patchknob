//----------------------------------------------------------------------------
//  sdlui/views/tracker/test_main.cpp
//
//  Self-contained harness for TrackerView: builds a sample sequence (notes +
//  CC events), mounts the tracker as a root ui::Widget, and drives a simulated
//  playhead so the row highlight moves.  A tiny toolbar exercises the shell-
//  facing API (rows/beat, note columns, octave, step, FX bind, theme).
//
//    export PATH="/c/msys64/mingw64/bin:$PATH"
//    cmake -S sdlui/views/tracker -B sdlui/views/tracker/build -G "MinGW Makefiles"
//    cmake --build sdlui/views/tracker/build -j
//    ./sdlui/views/tracker/build/tracker_test
//----------------------------------------------------------------------------
#include "gui.h"
#include "tracker_view.h"

#include "sequence.h"
#include "event.h"

#include <atomic>
#include <thread>
#include <cstdio>

using namespace ui;

// add one note-on / note-off pair straight into the sequence (piano-roll form).
static void add_note( sequence& s, int row, int rpb, int note, int vel, int step )
{
    int tpr = c_ppqn / rpb;
    long ts = (long) row * tpr;
    long tf = ts + (long) tpr * step;
    s.add_event( ts, EVENT_NOTE_ON,  (unsigned char) note, (unsigned char) vel, false );
    s.add_event( tf, EVENT_NOTE_OFF, (unsigned char) note, (unsigned char) vel, false );
}

static void add_cc( sequence& s, int row, int rpb, int cc, int val )
{
    int tpr = c_ppqn / rpb;
    long ts = (long) row * tpr;
    s.add_event( ts, EVENT_CONTROL_CHANGE, (unsigned char) cc, (unsigned char) val, false );
}

int main( int argc, char** argv )
{
    (void) argc; (void) argv;

    App app;
    app.w = 720; app.h = 620;
    if ( !app.init( "seq24 / SDL tracker" ) ) { app.shutdown(); return 1; }

    // ---- sample model: one sequence shared with (a hypothetical) piano roll --
    sequence seq;
    seq.set_name( "SDL Tracker Demo" );
    seq.set_midi_bus( 0 );           // == VST track 0
    seq.set_midi_channel( 0 );
    seq.set_length( 2 * 4 * c_ppqn, false );   // 2 measures @ 4/4 -> 32 rows @rpb4

    const int rpb = 4;
    // a little chord/melody so multiple note columns light up.
    add_note( seq, 0,  rpb, 60, 100, 4 );   // C-4
    add_note( seq, 0,  rpb, 64,  80, 4 );   // E-4  (2nd note column)
    add_note( seq, 0,  rpb, 67,  70, 4 );   // G-4  (3rd note column)
    add_note( seq, 4,  rpb, 62,  96, 4 );   // D-4
    add_note( seq, 4,  rpb, 65,  76, 4 );   // F-4
    add_note( seq, 8,  rpb, 65, 110, 4 );   // F-4
    add_note( seq, 8,  rpb, 69,  88, 4 );   // A-4
    add_note( seq, 12, rpb, 67, 104, 4 );   // G-4
    add_note( seq, 16, rpb, 60, 100, 8 );   // C-4 (longer)
    add_note( seq, 16, rpb, 63,  90, 8 );   // D#4
    add_note( seq, 24, rpb, 72, 120, 4 );   // C-5
    // FX (CC) values so the default FX columns (CC74 / CC07) show content.
    add_cc( seq, 0,  rpb, 74, 0x30 );
    add_cc( seq, 8,  rpb, 74, 0x70 );
    add_cc( seq, 16, rpb, 74, 0x50 );
    add_cc( seq, 0,  rpb, 7,  0x64 );
    add_cc( seq, 16, rpb, 7,  0x40 );
    seq.verify_and_link();

    // ---- the view under test ------------------------------------------------
    TrackerView tracker( &seq, /*track=*/0 );
    tracker.set_num_note_cols( 3 );
    tracker.set_octave( 4 );
    tracker.set_edit_step( 1 );

    // ---- tiny toolbar exercising the shell-facing API -----------------------
    Panel bar; static Color barbg; barbg = theme().panel; bar.bg = &barbg;

    Label title; title.text = "TRACKER";

    static int rpb_i = 4;
    Button bRpb; bRpb.text = "RPB 4";
    bRpb.clicked = [&]{ rpb_i = rpb_i==4?8:(rpb_i==8?16:4);
                        tracker.set_rows_per_beat( rpb_i );
                        char b[16]; snprintf(b,sizeof(b),"RPB %d",rpb_i); bRpb.text=b;
                        app.request_redraw(); };

    Button bCols; bCols.text = "Cols 3";
    bCols.clicked = [&]{ int n = tracker.get_num_note_cols()%8 + 1;
                         tracker.set_num_note_cols( n );
                         char b[16]; snprintf(b,sizeof(b),"Cols %d",n); bCols.text=b;
                         app.request_redraw(); };

    Button bOct; bOct.text = "Oct 4";
    bOct.clicked = [&]{ int o = (tracker.get_octave()+1)%9;
                        tracker.set_octave( o );
                        char b[16]; snprintf(b,sizeof(b),"Oct %d",o); bOct.text=b;
                        app.request_redraw(); };

    Button bStep; bStep.text = "Step 1";
    bStep.clicked = [&]{ int s = tracker.get_edit_step()%8 + 1;
                         tracker.set_edit_step( s );
                         char b[16]; snprintf(b,sizeof(b),"Step %d",s); bStep.text=b;
                         app.request_redraw(); };

    Button bFx; bFx.text = "FX CC10";
    bFx.clicked = [&]{ tracker.bind_fx_cc( 10 ); app.request_redraw(); };

    Button bTheme; bTheme.text = "Midnight";
    bTheme.clicked = [&]{
        bool toMid = ( mode()==Mode::Light );
        set_mode( toMid ? Mode::Midnight : Mode::Light );
        bTheme.text = toMid ? "Light" : "Midnight";
        barbg = theme().panel;
        app.request_redraw();
    };

    bar.children = { &title, &bRpb, &bCols, &bOct, &bStep, &bFx, &bTheme };

    // roots: toolbar first, tracker LAST so it receives keyboard focus.
    app.roots = { &bar, &tracker };

    app.on_layout = [&]( App& a ){
        bar.rect     = { 0, 0, a.w, 30 };
        title.rect   = { 8, 0, 90, 30 };
        bRpb.rect    = { 100, 4, 64, 22 };
        bCols.rect   = { 168, 4, 68, 22 };
        bOct.rect    = { 240, 4, 60, 22 };
        bStep.rect   = { 304, 4, 64, 22 };
        bFx.rect     = { 372, 4, 72, 22 };
        bTheme.rect  = { a.w-96, 4, 88, 22 };
        tracker.rect = { 6, 36, a.w-12, a.h-42 };
        tracker.poll_playhead();     // advance playhead-row state before draw
    };

    // ---- simulated playhead: advance last_tick + request redraw -------------
    seq.set_playing( true );
    std::atomic<bool> alive{ true };
    std::thread ticker( [&]{
        long tick = 0;
        long len  = seq.get_length();
        if ( len < 1 ) len = 1;
        while ( alive.load() && app.running )
        {
            tick = ( tick + 24 ) % len;   // ~half a row per step @rpb4
            seq.set_orig_tick( tick );
            app.request_redraw();
            SDL_Delay( 45 );
        }
    } );

    app.run();

    alive.store( false );
    ticker.join();
    app.shutdown();
    return 0;
}
