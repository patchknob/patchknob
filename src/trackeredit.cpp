//----------------------------------------------------------------------------
//
//  This file is part of seq24.
//
//  seq24 is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  seq24 is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//-----------------------------------------------------------------------------
//
//  trackeredit.cpp -- full multi-track vertical tracker editor of a sequence.
//
//  See trackeredit.h for the model/layout notes.  In short:
//
//    * Note-track columns are a polyphonic-stacking view of the shared
//      sequence: per row, note-ons are gathered and sorted by pitch; column t
//      shows the t-th-lowest note.  Editing writes the same note-on/off pairs
//      the piano roll uses, so notes round-trip both ways.
//    * FX columns bound to a MIDI CC are stored as real EVENT_CONTROL_CHANGE
//      events IN the sequence, so they play through the normal engine path.
//    * FX columns bound to a VST parameter are stored in a per-pattern map the
//      tracker owns and fired via audio_app_route_param() on entry and, during
//      playback, from the redraw timeout that watches the playhead tick.
//
//  All drawing is monochrome (Cairo) using the greyscale palette in
//  src/ui/palette.h.  The grid is a Gtk::DrawingArea SUBCLASS (trackergrid) so
//  on_realize() chains Gtk::DrawingArea::on_realize() FIRST -- the proven
//  seqroll pattern that guarantees the GdkWindow exists before we build the GC.
//
//-----------------------------------------------------------------------------

#include "trackeredit.h"
#include "ui/palette.h"
#include "globals.h"
#include "audio_app.h"          /* header-only bridge to the VST engine       */

#include <gtkmm/menu.h>
#include <cstdio>
#include <algorithm>
#include <gdk/gdkkeysyms.h>

using namespace Gtk;

/* per-track pixel layout (offsets within one track column) */
static const int kNoteX = 6,  kNoteW = 40;
static const int kVelX  = 50, kVelW  = 22;
static const int kFxX0  = 78, kFxW   = 22, kFxGap = 6;

/* ========================================================================= *
 *  trackergrid  (DrawingArea subclass)
 * ========================================================================= */

trackergrid::trackergrid( sequence *a_seq, perform *a_perf, int a_pos,
                          Adjustment *a_vadjust )
: DrawingArea()
{
    m_seq      = a_seq;
    m_mainperf = a_perf;
    m_pos      = a_pos;
    m_vadjust  = a_vadjust;

    m_rows_per_beat = 4;
    m_num_tracks    = 1;
    m_fx_cols       = 2;

    m_cursor_row    = 0;
    m_cursor_track  = 0;
    m_cursor_col    = 0;
    m_octave        = 4;
    m_edit_step     = 1;
    m_velocity      = 100;
    m_top_row       = 0;
    m_old_progress_row = -1;
    m_last_fire_row = -1;

    m_row_h    = 16;
    m_header_h = 16;
    m_gutter_w = 44;

    /* allocate binding + value stores for the max track count so growing the
       track count never reallocates out from under a live cursor. */
    m_fx_bind.resize( 8 );
    m_fx_vst.resize( 8 );
    for ( int t = 0; t < 8; t++ )
    {
        m_fx_bind[t].resize( m_fx_cols );
        m_fx_vst[t].resize( m_fx_cols );
        /* give columns a sensible default so a fresh pattern already does
           something musical when the user types values in. */
        m_fx_bind[t][0].type  = FX_MIDI_CC;
        m_fx_bind[t][0].cc    = 74;                 /* filter cutoff */
        m_fx_bind[t][0].label = "C74";
        if ( m_fx_cols > 1 )
        {
            m_fx_bind[t][1].type  = FX_MIDI_CC;
            m_fx_bind[t][1].cc    = 7;               /* channel volume */
            m_fx_bind[t][1].label = "C07";
        }
    }

    recalc_track_geometry();
    m_window_y = 480;

    add_events( Gdk::EXPOSURE_MASK      |
                Gdk::BUTTON_PRESS_MASK  |
                Gdk::KEY_PRESS_MASK     |
                Gdk::FOCUS_CHANGE_MASK  |
                Gdk::SCROLL_MASK );

    set_size_request( m_window_x, m_window_y );
    set_double_buffered( false );
}

trackergrid::~trackergrid()
{
}

void
trackergrid::recalc_track_geometry( void )
{
    m_track_w  = kFxX0 + m_fx_cols * ( kFxW + kFxGap ) + 4;
    m_window_x = m_gutter_w + m_track_w * m_num_tracks + 1;
}

/* ------------------------------------------------------------------------- *
 *  model <-> grid helpers
 * ------------------------------------------------------------------------- */

int
trackergrid::ticks_per_row( void )
{
    int t = c_ppqn / m_rows_per_beat;
    if ( t < 1 ) t = 1;
    return t;
}

int
trackergrid::num_rows( void )
{
    int rows = m_seq->get_length() / ticks_per_row();
    if ( rows < 1 ) rows = 1;
    return rows;
}

int
trackergrid::visible_rows( void )
{
    int r = ( m_window_y - m_header_h ) / m_row_h;
    if ( r < 1 ) r = 1;
    return r;
}

long
trackergrid::row_start_tick( int a_row )
{
    return (long) a_row * ticks_per_row();
}

long
trackergrid::length_measures( void )
{
    long bpm = m_seq->get_bpm();
    if ( bpm < 1 ) bpm = 4;
    long measures = m_seq->get_length() / ( c_ppqn * bpm );
    if ( measures < 1 ) measures = 1;
    return measures;
}

std::string
trackergrid::note_name( int a_note )
{
    static const char *names[12] =
        { "C-", "C#", "D-", "D#", "E-", "F-",
          "F#", "G-", "G#", "A-", "A#", "B-" };

    if ( a_note < 0 || a_note > 127 )
        return "---";

    int pc  = a_note % 12;
    int oct = a_note / 12 - 1;       /* MIDI note 60 == C-4 */

    char buf[8];
    snprintf( buf, sizeof(buf), "%s%d", names[pc], oct );
    return std::string( buf );
}

/* gather the note-ons that start in a_row, sorted ascending by pitch. */
void
trackergrid::collect_row_notes( int a_row, std::vector<notecell> &a_out )
{
    a_out.clear();

    long ts = row_start_tick( a_row );
    long tf = ts + ticks_per_row();

    long  tick_s, tick_f;
    int   note, vel;
    bool  selected;

    m_seq->reset_draw_marker();
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        if ( tick_s >= ts && tick_s < tf )
        {
            notecell nc;
            nc.note = note;
            nc.vel  = vel;
            nc.ts   = tick_s;
            nc.tf   = tick_f;
            a_out.push_back( nc );
        }
    }

    std::sort( a_out.begin(), a_out.end(),
               []( const notecell &l, const notecell &r )
               { return l.note < r.note; } );
}

bool
trackergrid::note_at_row( int a_row, int a_track, int *a_note, int *a_vel )
{
    std::vector<notecell> nl;
    collect_row_notes( a_row, nl );
    if ( a_track < 0 || a_track >= (int) nl.size() )
        return false;
    if ( a_note ) *a_note = nl[a_track].note;
    if ( a_vel )  *a_vel  = nl[a_track].vel;
    return true;
}

/* remove exactly the note whose ON is at a_ts with pitch a_note. */
void
trackergrid::remove_specific_note( long a_ts, int a_note )
{
    m_seq->unselect();
    int n = m_seq->select_note_events( a_ts, a_note, a_ts, a_note,
                                       sequence::e_select );
    if ( n > 0 )
    {
        m_seq->mark_selected();
        m_seq->remove_marked();
        m_seq->verify_and_link();
        m_seq->set_dirty();
    }
}

void
trackergrid::set_note_at_cell( int a_note )
{
    if ( a_note < 0 || a_note > 127 )
        return;

    long ts  = row_start_tick( m_cursor_row );
    long len = ticks_per_row() * m_edit_step;
    if ( len < 1 ) len = ticks_per_row();

    m_seq->push_undo();

    /* if this note-column slot already holds a note, replace just that one */
    std::vector<notecell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track < (int) nl.size() )
        remove_specific_note( nl[m_cursor_track].ts, nl[m_cursor_track].note );

    m_seq->add_event( ts,       EVENT_NOTE_ON,  (unsigned char) a_note,
                      (unsigned char) m_velocity, false );
    m_seq->add_event( ts + len, EVENT_NOTE_OFF, (unsigned char) a_note,
                      (unsigned char) m_velocity, false );

    m_seq->verify_and_link();
    m_seq->set_dirty();

    m_seq->play_note_on( a_note );

    update_pixmap();
    force_draw();
}

void
trackergrid::set_velocity_at_cell( int a_vel )
{
    if ( a_vel < 0 )   a_vel = 0;
    if ( a_vel > 127 ) a_vel = 127;

    std::vector<notecell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track >= (int) nl.size() )
        return;                                  /* no note here to re-velocity */

    notecell nc = nl[m_cursor_track];

    m_seq->push_undo();
    remove_specific_note( nc.ts, nc.note );
    m_seq->add_event( nc.ts, EVENT_NOTE_ON,  (unsigned char) nc.note,
                      (unsigned char) a_vel, false );
    m_seq->add_event( nc.tf, EVENT_NOTE_OFF, (unsigned char) nc.note,
                      (unsigned char) a_vel, false );
    m_seq->verify_and_link();
    m_seq->set_dirty();

    update_pixmap();
    force_draw();
}

void
trackergrid::clear_note_cell( void )
{
    std::vector<notecell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track >= (int) nl.size() )
        return;

    m_seq->push_undo();
    remove_specific_note( nl[m_cursor_track].ts, nl[m_cursor_track].note );
    update_pixmap();
    force_draw();
}

/* ------------------------------------------------------------------------- *
 *  FX-command helpers
 * ------------------------------------------------------------------------- */

int
trackergrid::cur_fx_index( void )
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 ) fi = 0;
    if ( fi >= m_fx_cols ) fi = m_fx_cols - 1;
    return fi;
}

/* read the value of the CC bound to a_cc that lives in a_row's tick window. */
bool
trackergrid::read_cc_at( int a_row, int a_cc, int *a_val )
{
    long ts = row_start_tick( a_row );
    long tf = ts + ticks_per_row();

    long          tick;
    unsigned char d0, d1;
    bool          sel;

    m_seq->reset_draw_marker();
    while ( m_seq->get_next_event( EVENT_CONTROL_CHANGE, (unsigned char) a_cc,
                                   &tick, &d0, &d1, &sel ) )
    {
        if ( d0 == (unsigned char) a_cc && tick >= ts && tick < tf )
        {
            if ( a_val ) *a_val = d1;
            return true;
        }
    }
    return false;
}

void
trackergrid::remove_cc_at( long a_ts, int a_cc )
{
    m_seq->unselect();
    int n = m_seq->select_events( a_ts, a_ts + ticks_per_row() - 1,
                                  EVENT_CONTROL_CHANGE, (unsigned char) a_cc,
                                  sequence::e_select );
    if ( n > 0 )
    {
        m_seq->mark_selected();
        m_seq->remove_marked();
        m_seq->verify_and_link();
        m_seq->set_dirty();
    }
}

void
trackergrid::set_cc_at( long a_ts, int a_cc, int a_val )
{
    if ( a_val < 0 )   a_val = 0;
    if ( a_val > 127 ) a_val = 127;

    remove_cc_at( a_ts, a_cc );
    m_seq->add_event( a_ts, EVENT_CONTROL_CHANGE, (unsigned char) a_cc,
                      (unsigned char) a_val, false );
    m_seq->set_dirty();

    /* immediate audible feedback: route the CC straight to the track VST now */
    if ( seq24::app::audio_app_running() )
    {
        int track = (int) m_seq->get_midi_bus();
        unsigned char status = 0xB0 | ( m_seq->get_midi_channel() & 0x0F );
        seq24::app::audio_app_route_midi( track, status,
                                          (unsigned char) a_cc,
                                          (unsigned char) a_val );
    }
}

/* Type a hex value into the FX cell under the cursor.  Shift-accumulates the
   nibble: pressing "7" then "F" builds 0x7F. */
void
trackergrid::set_fx_at_cell( int a_val )
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 || fi >= m_fx_cols )
        return;

    fx_binding &b = m_fx_bind[m_cursor_track][fi];
    long ts = row_start_tick( m_cursor_row );

    if ( b.type == FX_MIDI_CC )
    {
        m_seq->push_undo();
        set_cc_at( ts, b.cc, a_val & 0x7f );
    }
    else if ( b.type == FX_VST_PARAM )
    {
        m_fx_vst[m_cursor_track][fi][m_cursor_row] = a_val & 0xff;
        if ( seq24::app::audio_app_running() )
            seq24::app::audio_app_route_param( get_vst_track(), b.pid,
                                               ( a_val & 0xff ) / 255.0f );
    }
    else
        return;                              /* unbound column: nothing to do */

    update_pixmap();
    force_draw();
}

void
trackergrid::clear_fx_cell( void )
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 || fi >= m_fx_cols )
        return;

    fx_binding &b = m_fx_bind[m_cursor_track][fi];

    if ( b.type == FX_MIDI_CC )
    {
        m_seq->push_undo();
        remove_cc_at( row_start_tick( m_cursor_row ), b.cc );
    }
    else if ( b.type == FX_VST_PARAM )
    {
        m_fx_vst[m_cursor_track][fi].erase( m_cursor_row );
    }
    update_pixmap();
    force_draw();
}

/* fire every VST-param FX cell that lives on a_row (called from the playhead
   watcher).  CC cells are already sequence events, so they play themselves. */
void
trackergrid::fire_fx_row( int a_row )
{
    if ( !seq24::app::audio_app_running() )
        return;

    int track = get_vst_track();
    for ( int t = 0; t < m_num_tracks; t++ )
    {
        for ( int f = 0; f < m_fx_cols; f++ )
        {
            fx_binding &b = m_fx_bind[t][f];
            if ( b.type != FX_VST_PARAM )
                continue;
            std::map<int,int> &mp = m_fx_vst[t][f];
            std::map<int,int>::iterator it = mp.find( a_row );
            if ( it != mp.end() )
                seq24::app::audio_app_route_param( track, b.pid,
                                                   ( it->second & 0xff ) / 255.0f );
        }
    }
}

void
trackergrid::fire_due_fx( void )
{
    if ( !m_seq->get_playing() )
    {
        m_last_fire_row = -1;
        return;
    }

    int tpr = ticks_per_row();
    int nr  = num_rows();
    if ( nr < 1 ) nr = 1;

    long tick = m_seq->get_last_tick();
    int  cur  = ( tick / tpr ) % nr;

    if ( m_last_fire_row < 0 )
    {
        fire_fx_row( cur );
        m_last_fire_row = cur;
        return;
    }

    if ( cur == m_last_fire_row )
        return;

    /* fire every row crossed since last time (handles wrap at loop end) */
    int r     = m_last_fire_row;
    int guard = 0;
    do {
        r = ( r + 1 ) % nr;
        fire_fx_row( r );
    } while ( r != cur && ++guard < nr );

    m_last_fire_row = cur;
}

/* ------------------------------------------------------------------------- *
 *  FX bindings (driven by the toolbar picker)
 * ------------------------------------------------------------------------- */

void
trackergrid::bind_fx_none( void )
{
    int fi = cur_fx_index();
    m_fx_bind[m_cursor_track][fi] = fx_binding();
    m_fx_vst[m_cursor_track][fi].clear();
    update_pixmap();
    force_draw();
}

void
trackergrid::bind_fx_cc( int a_cc, const std::string &a_label )
{
    int fi = cur_fx_index();
    fx_binding &b = m_fx_bind[m_cursor_track][fi];
    b.type  = FX_MIDI_CC;
    b.cc    = a_cc;
    b.pid   = 0;
    char lb[8];
    snprintf( lb, sizeof(lb), "C%02d", a_cc );
    b.label = lb;
    (void) a_label;
    m_fx_vst[m_cursor_track][fi].clear();
    update_pixmap();
    force_draw();
}

void
trackergrid::bind_fx_vst( unsigned int a_pid, const std::string &a_name )
{
    int fi = cur_fx_index();
    fx_binding &b = m_fx_bind[m_cursor_track][fi];
    b.type  = FX_VST_PARAM;
    b.pid   = a_pid;
    b.cc    = 0;
    std::string sh = a_name.substr( 0, 3 );
    b.label = std::string( "P:" ) + sh;
    update_pixmap();
    force_draw();
}

std::string
trackergrid::cur_fx_desc( void )
{
    int fi = cur_fx_index();
    fx_binding &b = m_fx_bind[m_cursor_track][fi];
    char buf[32];
    if ( b.type == FX_MIDI_CC )
        snprintf( buf, sizeof(buf), "T%d.F%d CC%d", m_cursor_track, fi, b.cc );
    else if ( b.type == FX_VST_PARAM )
        snprintf( buf, sizeof(buf), "T%d.F%d P%u", m_cursor_track, fi, b.pid );
    else
        snprintf( buf, sizeof(buf), "T%d.F%d --", m_cursor_track, fi );
    return std::string( buf );
}

int
trackergrid::key_to_pitch( unsigned int kv, int *a_oct_offset )
{
    *a_oct_offset = 0;
    switch ( kv )
    {
        /* lower octave: Z S X D C V G B H N J M */
        case 'z': case 'Z': return 0;   // C
        case 's': case 'S': return 1;   // C#
        case 'x': case 'X': return 2;   // D
        case 'd': case 'D': return 3;   // D#
        case 'c': case 'C': return 4;   // E
        case 'v': case 'V': return 5;   // F
        case 'g': case 'G': return 6;   // F#
        case 'b': case 'B': return 7;   // G
        case 'h': case 'H': return 8;   // G#
        case 'n': case 'N': return 9;   // A
        case 'j': case 'J': return 10;  // A#
        case 'm': case 'M': return 11;  // B

        /* upper octave: Q 2 W 3 E R 5 T 6 Y 7 U */
        case 'q': case 'Q': *a_oct_offset = 1; return 0;
        case '2':           *a_oct_offset = 1; return 1;
        case 'w': case 'W': *a_oct_offset = 1; return 2;
        case '3':           *a_oct_offset = 1; return 3;
        case 'e': case 'E': *a_oct_offset = 1; return 4;
        case 'r': case 'R': *a_oct_offset = 1; return 5;
        case '5':           *a_oct_offset = 1; return 6;
        case 't': case 'T': *a_oct_offset = 1; return 7;
        case '6':           *a_oct_offset = 1; return 8;
        case 'y': case 'Y': *a_oct_offset = 1; return 9;
        case '7':           *a_oct_offset = 1; return 10;
        case 'u': case 'U': *a_oct_offset = 1; return 11;
    }
    return -1;
}

int
trackergrid::key_to_hex( unsigned int kv )
{
    if ( kv >= '0' && kv <= '9' ) return (int)( kv - '0' );
    if ( kv >= 'a' && kv <= 'f' ) return (int)( kv - 'a' + 10 );
    if ( kv >= 'A' && kv <= 'F' ) return (int)( kv - 'A' + 10 );
    return -1;
}

/* ------------------------------------------------------------------------- *
 *  geometry / scrolling
 * ------------------------------------------------------------------------- */

void
trackergrid::subcol_geom( int a_col, int *a_x, int *a_w )
{
    if ( a_col == 0 )      { *a_x = kNoteX; *a_w = kNoteW; }
    else if ( a_col == 1 ) { *a_x = kVelX;  *a_w = kVelW;  }
    else
    {
        int fi = a_col - 2;
        *a_x = kFxX0 + fi * ( kFxW + kFxGap );
        *a_w = kFxW;
    }
}

void
trackergrid::update_sizes( void )
{
    int rows = num_rows();
    int vis  = visible_rows();

    if ( m_vadjust )
    {
        m_vadjust->set_lower( 0 );
        m_vadjust->set_upper( rows );
        m_vadjust->set_page_size( vis );
        m_vadjust->set_step_increment( 1 );
        m_vadjust->set_page_increment( vis );

        int maxv = rows - vis;
        if ( maxv < 0 ) maxv = 0;
        if ( m_vadjust->get_value() > maxv )
            m_vadjust->set_value( maxv );
    }

    if ( is_realized() )
        m_pixmap = Gdk::Pixmap::create( m_window, m_window_x, m_window_y, -1 );
}

void
trackergrid::set_num_tracks( int a_n )
{
    if ( a_n < 1 ) a_n = 1;
    if ( a_n > 8 ) a_n = 8;
    m_num_tracks = a_n;

    if ( m_cursor_track >= m_num_tracks )
        m_cursor_track = m_num_tracks - 1;

    recalc_track_geometry();
    set_size_request( m_window_x, m_window_y );
    update_sizes();
    update_pixmap();
    force_draw();
}

void
trackergrid::change_vert( void )
{
    if ( !m_vadjust )
        return;
    m_top_row = (int) m_vadjust->get_value();
    update_pixmap();
    force_draw();
}

void
trackergrid::ensure_cursor_visible( void )
{
    int vis = visible_rows();
    if ( m_cursor_row < m_top_row )
        m_top_row = m_cursor_row;
    else if ( m_cursor_row >= m_top_row + vis )
        m_top_row = m_cursor_row - vis + 1;

    if ( m_top_row < 0 ) m_top_row = 0;
    if ( m_vadjust )
        m_vadjust->set_value( m_top_row );
}

/* ------------------------------------------------------------------------- *
 *  drawing
 * ------------------------------------------------------------------------- */

void
trackergrid::draw_background( void )
{
    if ( !m_pixmap ) return;
    Cairo::RefPtr<Cairo::Context> cr = m_pixmap->create_cairo_context();
    cr->set_line_width( 1.0 );
    synth::set_source( cr, synth::cBg );
    cr->rectangle( 0, 0, m_window_x, m_window_y );
    cr->fill();
}

void
trackergrid::draw_header( const Cairo::RefPtr<Cairo::Context> &cr )
{
    cr->select_font_face( "monospace",
                          Cairo::FONT_SLANT_NORMAL,
                          Cairo::FONT_WEIGHT_NORMAL );
    cr->set_font_size( 10.0 );

    synth::set_source( cr, synth::cPanel, 1.0 );
    cr->rectangle( 0, 0, m_window_x, m_header_h );
    cr->fill();

    int ty = m_header_h - 4;

    for ( int t = 0; t < m_num_tracks; t++ )
    {
        int cx = m_gutter_w + t * m_track_w;

        synth::set_source( cr, t == m_cursor_track ? synth::cHi : synth::cDim );
        char th[8];
        snprintf( th, sizeof(th), "T%d", t );
        cr->move_to( cx + kNoteX, ty );
        cr->show_text( th );

        synth::set_source( cr, synth::cDim );
        cr->move_to( cx + kVelX, ty );
        cr->show_text( "vv" );

        for ( int f = 0; f < m_fx_cols; f++ )
        {
            int fx, fw;
            subcol_geom( 2 + f, &fx, &fw );
            fx_binding &b = m_fx_bind[t][f];
            synth::set_source( cr, b.type == FX_NONE ? synth::cDim : synth::cWhite );
            cr->move_to( cx + fx, ty );
            cr->show_text( b.label.c_str() );
        }
    }

    synth::set_source( cr, synth::cDim, 0.9 );
    cr->move_to( 0, m_header_h - 0.5 );
    cr->line_to( m_window_x, m_header_h - 0.5 );
    cr->stroke();
}

void
trackergrid::draw_grid( void )
{
    if ( !m_pixmap ) return;
    Cairo::RefPtr<Cairo::Context> cr = m_pixmap->create_cairo_context();
    cr->set_line_width( 1.0 );

    draw_header( cr );

    cr->select_font_face( "monospace",
                          Cairo::FONT_SLANT_NORMAL,
                          Cairo::FONT_WEIGHT_NORMAL );
    cr->set_font_size( 11.0 );

    int rows = num_rows();
    int vis  = visible_rows();
    long bpm = m_seq->get_bpm();
    if ( bpm < 1 ) bpm = 4;

    for ( int sr = 0; sr < vis; sr++ )
    {
        int row = m_top_row + sr;
        if ( row >= rows ) break;

        int y = m_header_h + sr * m_row_h;

        bool is_beat = ( row % m_rows_per_beat ) == 0;
        bool is_bar  = ( row % ( m_rows_per_beat * bpm ) ) == 0;

        if ( is_bar )
            synth::set_source( cr, synth::cPanel, 1.0 );
        else if ( is_beat )
            synth::set_source( cr, synth::cPanel, 0.6 );
        else
            synth::set_source( cr, synth::cBg );
        cr->rectangle( 0, y, m_window_x, m_row_h );
        cr->fill();

        if ( row == m_cursor_row )
        {
            synth::set_source( cr, synth::cAccent, 0.30 );
            cr->rectangle( 0, y, m_window_x, m_row_h );
            cr->fill();
        }

        synth::set_source( cr, synth::cDim, is_beat ? 0.9 : 0.4 );
        cr->move_to( 0, y + 0.5 );
        cr->line_to( m_window_x, y + 0.5 );
        cr->stroke();

        char rn[8];
        snprintf( rn, sizeof(rn), "%3d", row );
        synth::set_source( cr, is_beat ? synth::cHi : synth::cDim );
        cr->move_to( 4, y + m_row_h - 4 );
        cr->show_text( rn );

        /* gather this row's chord once, reuse for every note-column */
        std::vector<notecell> nl;
        collect_row_notes( row, nl );

        for ( int t = 0; t < m_num_tracks; t++ )
        {
            int cx = m_gutter_w + t * m_track_w;

            bool has = t < (int) nl.size();
            std::string cell_note = has ? note_name( nl[t].note ) : "---";
            char cell_vel[8];
            if ( has ) snprintf( cell_vel, sizeof(cell_vel), "%02X", nl[t].vel & 0x7f );
            else       snprintf( cell_vel, sizeof(cell_vel), "--" );

            synth::set_source( cr, has ? synth::cWhite : synth::cDim );
            cr->move_to( cx + kNoteX, y + m_row_h - 4 );
            cr->show_text( cell_note );

            synth::set_source( cr, has ? synth::cHi : synth::cDim );
            cr->move_to( cx + kVelX, y + m_row_h - 4 );
            cr->show_text( cell_vel );

            /* FX cells */
            for ( int f = 0; f < m_fx_cols; f++ )
            {
                int fx, fw;
                subcol_geom( 2 + f, &fx, &fw );
                fx_binding &b = m_fx_bind[t][f];

                char fxs[8];
                bool fhas = false;
                int  fval = 0;

                if ( b.type == FX_MIDI_CC )
                    fhas = read_cc_at( row, b.cc, &fval );
                else if ( b.type == FX_VST_PARAM )
                {
                    std::map<int,int> &mp = m_fx_vst[t][f];
                    std::map<int,int>::iterator it = mp.find( row );
                    if ( it != mp.end() ) { fhas = true; fval = it->second; }
                }

                if ( fhas ) snprintf( fxs, sizeof(fxs), "%02X", fval & 0xff );
                else        snprintf( fxs, sizeof(fxs), ".." );

                synth::set_source( cr, fhas ? synth::cWhite : synth::cDim );
                cr->move_to( cx + fx, y + m_row_h - 4 );
                cr->show_text( fxs );
            }

            /* cursor underline for the active sub-column */
            if ( row == m_cursor_row && t == m_cursor_track )
            {
                int ux, uw;
                subcol_geom( m_cursor_col, &ux, &uw );
                synth::set_source( cr, synth::cSel );
                cr->rectangle( cx + ux, y + m_row_h - 2, uw, 1.5 );
                cr->fill();
            }
        }
    }

    /* vertical rules: gutter + one per track boundary */
    synth::set_source( cr, synth::cDim, 0.8 );
    cr->move_to( m_gutter_w + 0.5, 0 );
    cr->line_to( m_gutter_w + 0.5, m_window_y );
    cr->stroke();
    for ( int t = 0; t <= m_num_tracks; t++ )
    {
        int x = m_gutter_w + t * m_track_w;
        synth::set_source( cr, synth::cDim, 0.5 );
        cr->move_to( x + 0.5, 0 );
        cr->line_to( x + 0.5, m_window_y );
        cr->stroke();
    }
}

void
trackergrid::update_pixmap( void )
{
    if ( !m_pixmap )
        return;
    draw_background();
    draw_grid();
}

void
trackergrid::force_draw( void )
{
    if ( !m_pixmap || !m_window )
        return;
    m_window->draw_drawable( m_gc, m_pixmap, 0, 0, 0, 0,
                             m_window_x, m_window_y );
    draw_progress_on_window();
}

void
trackergrid::redraw( void )
{
    update_pixmap();
    force_draw();
}

void
trackergrid::draw_progress_on_window( void )
{
    if ( !m_window || !m_pixmap )
        return;

    long tick = m_seq->get_last_tick();
    int  prow = tick / ticks_per_row();

    if ( m_old_progress_row >= 0 )
    {
        int sr = m_old_progress_row - m_top_row;
        if ( sr >= 0 && sr < visible_rows() )
        {
            int y = m_header_h + sr * m_row_h;
            m_window->draw_drawable( m_gc, m_pixmap, 0, y, 0, y,
                                     m_window_x, m_row_h );
        }
    }

    int sr = prow - m_top_row;
    if ( sr >= 0 && sr < visible_rows() )
    {
        int y = m_header_h + sr * m_row_h;
        Cairo::RefPtr<Cairo::Context> cr = m_window->create_cairo_context();
        cr->set_line_width( 1.0 );
        synth::set_source( cr, synth::cActive, 0.85 );
        cr->rectangle( 0, y, m_window_x, 2 );
        cr->fill();
    }

    m_old_progress_row = prow;
}

/* ------------------------------------------------------------------------- *
 *  cursor / rows-per-beat
 * ------------------------------------------------------------------------- */

void
trackergrid::set_rows_per_beat( int a_rpb )
{
    if ( a_rpb != 4 && a_rpb != 8 && a_rpb != 16 )
        return;
    m_rows_per_beat = a_rpb;
    m_cursor_row = 0;
    m_top_row = 0;
    update_sizes();
    update_pixmap();
    force_draw();
}

void
trackergrid::move_cursor( int a_drow, int a_dcol )
{
    if ( a_dcol != 0 )
    {
        int total = total_subcols();
        int maxg  = m_num_tracks * total;
        int gc    = m_cursor_track * total + m_cursor_col + a_dcol;
        if ( gc < 0 )     gc += maxg;
        if ( gc >= maxg ) gc -= maxg;
        m_cursor_track = gc / total;
        m_cursor_col   = gc % total;
    }

    m_cursor_row += a_drow;
    if ( m_cursor_row < 0 ) m_cursor_row = 0;
    if ( m_cursor_row >= num_rows() ) m_cursor_row = num_rows() - 1;

    ensure_cursor_visible();
    update_pixmap();
    force_draw();
}

/* ------------------------------------------------------------------------- *
 *  DrawingArea overrides -- mirror seqroll exactly
 * ------------------------------------------------------------------------- */

void
trackergrid::on_realize()
{
    /* chain the default realize FIRST -- this creates the GdkWindow */
    Gtk::DrawingArea::on_realize();

    set_flags( Gtk::CAN_FOCUS );

    m_window = get_window();
    m_gc = Gdk::GC::create( m_window );
    m_window->clear();

    if ( m_vadjust )
        m_vadjust->signal_value_changed().connect(
            mem_fun( *this, &trackergrid::change_vert ) );

    update_sizes();
    update_pixmap();
    grab_focus();
}

void
trackergrid::on_size_allocate( Gtk::Allocation &a_r )
{
    Gtk::DrawingArea::on_size_allocate( a_r );

    m_window_x = a_r.get_width();
    m_window_y = a_r.get_height();
    if ( m_window_x < 1 ) m_window_x = 1;
    if ( m_window_y < 1 ) m_window_y = 1;

    if ( is_realized() )
    {
        update_sizes();
        update_pixmap();
        force_draw();
    }
}

bool
trackergrid::on_expose_event( GdkEventExpose *e )
{
    if ( !m_pixmap || !m_window )
        return true;
    m_window->draw_drawable( m_gc, m_pixmap,
                             e->area.x, e->area.y,
                             e->area.x, e->area.y,
                             e->area.width, e->area.height );
    draw_progress_on_window();
    return true;
}

bool
trackergrid::on_button_press_event( GdkEventButton *e )
{
    grab_focus();

    if ( e->button == 1 )
    {
        int sr  = ( (int) e->y - m_header_h ) / m_row_h;
        int row = m_top_row + sr;
        if ( (int) e->y >= m_header_h && row >= 0 && row < num_rows() )
            m_cursor_row = row;

        int x = (int) e->x;
        if ( x >= m_gutter_w )
        {
            int t = ( x - m_gutter_w ) / m_track_w;
            if ( t >= 0 && t < m_num_tracks )
            {
                m_cursor_track = t;
                int rel = ( x - m_gutter_w ) % m_track_w;

                /* nearest sub-column by its x offset */
                int best = 0;
                int bestd = 1 << 30;
                int total = total_subcols();
                for ( int c = 0; c < total; c++ )
                {
                    int sx, sw;
                    subcol_geom( c, &sx, &sw );
                    int cxm = sx + sw / 2;
                    int d = rel - cxm; if ( d < 0 ) d = -d;
                    if ( d < bestd ) { bestd = d; best = c; }
                }
                m_cursor_col = best;
            }
        }
        update_pixmap();
        force_draw();
    }
    return true;
}

bool
trackergrid::on_key_press_event( GdkEventKey *e )
{
    unsigned int kv = e->keyval;

    switch ( kv )
    {
        case GDK_Up:        move_cursor( -1, 0 ); return true;
        case GDK_Down:      move_cursor(  1, 0 ); return true;
        case GDK_Left:      move_cursor(  0,-1 ); return true;
        case GDK_Right:     move_cursor(  0, 1 ); return true;
        case GDK_Tab:       move_cursor(  0, total_subcols() ); return true;
        case GDK_ISO_Left_Tab: move_cursor( 0, -total_subcols() ); return true;
        case GDK_Page_Up:   move_cursor( -m_rows_per_beat, 0 ); return true;
        case GDK_Page_Down: move_cursor(  m_rows_per_beat, 0 ); return true;
        case GDK_Home:      m_cursor_row = 0;
                            ensure_cursor_visible(); update_pixmap();
                            force_draw(); return true;
        case GDK_End:       m_cursor_row = num_rows() - 1;
                            ensure_cursor_visible(); update_pixmap();
                            force_draw(); return true;

        case GDK_Delete:
        case GDK_period:
        case GDK_BackSpace:
            if ( m_cursor_col == 0 )
                clear_note_cell();
            else if ( m_cursor_col == 1 )
                clear_note_cell();          /* velocity follows the note */
            else
                clear_fx_cell();
            move_cursor( m_edit_step, 0 );
            return true;
    }

    /* NOTE column -> tracker keyboard note entry */
    if ( m_cursor_col == 0 )
    {
        int oct_off = 0;
        int pc = key_to_pitch( kv, &oct_off );
        if ( pc >= 0 )
        {
            int note = ( m_octave + oct_off + 1 ) * 12 + pc;  /* C-4 == MIDI 60 */
            if ( note >= 0 && note <= 127 )
            {
                set_note_at_cell( note );
                move_cursor( m_edit_step, 0 );
            }
            return true;
        }
        return false;
    }

    /* VELOCITY / FX columns -> hex value entry (shift-accumulate) */
    int hv = key_to_hex( kv );
    if ( hv >= 0 )
    {
        if ( m_cursor_col == 1 )
        {
            int note, vel;
            if ( note_at_row( m_cursor_row, m_cursor_track, &note, &vel ) )
                set_velocity_at_cell( ( ( vel << 4 ) | hv ) & 0x7f );
        }
        else
        {
            int fi = cur_fx_index();
            fx_binding &b = m_fx_bind[m_cursor_track][fi];
            int cur = 0;
            if ( b.type == FX_MIDI_CC )
                read_cc_at( m_cursor_row, b.cc, &cur );
            else if ( b.type == FX_VST_PARAM )
            {
                std::map<int,int> &mp = m_fx_vst[m_cursor_track][fi];
                std::map<int,int>::iterator it = mp.find( m_cursor_row );
                if ( it != mp.end() ) cur = it->second;
            }
            set_fx_at_cell( ( ( cur << 4 ) | hv ) & 0xff );
        }
        return true;
    }

    return false;
}

/* ========================================================================= *
 *  trackeredit  (Gtk::Window host)
 * ========================================================================= */

trackeredit::trackeredit( sequence *a_seq, perform *a_perf, int a_pos )
{
    m_seq      = a_seq;
    m_mainperf = a_perf;
    m_pos      = a_pos;

    set_title( std::string("Tracker - ") + m_seq->get_name() );
    set_default_size( 720, 560 );
    set_size_request( 360, 400 );

    m_seq->set_editing( true );

    /* ---- toolbar ---- */
    HBox *toolbar = manage( new HBox( false, 2 ) );
    toolbar->set_border_width( 2 );

    m_button_rpb = manage( new Button( "Rows/Beat" ) );
    m_button_rpb->signal_clicked().connect(
        mem_fun( *this, &trackeredit::popup_rpb_menu ) );
    m_entry_rpb = manage( new Entry() );
    m_entry_rpb->set_size_request( 30, -1 );
    m_entry_rpb->set_editable( false );

    m_button_tracks = manage( new Button( "Trk +" ) );
    m_button_tracks->signal_clicked().connect(
        sigc::bind( mem_fun( *this, &trackeredit::change_tracks ), 1 ) );
    m_entry_tracks = manage( new Entry() );
    m_entry_tracks->set_size_request( 26, -1 );
    m_entry_tracks->set_editable( false );

    m_button_octave = manage( new Button( "Oct +" ) );
    m_button_octave->signal_clicked().connect(
        sigc::bind( mem_fun( *this, &trackeredit::change_octave ), 1 ) );
    m_entry_octave = manage( new Entry() );
    m_entry_octave->set_size_request( 26, -1 );
    m_entry_octave->set_editable( false );

    m_button_step = manage( new Button( "Step +" ) );
    m_button_step->signal_clicked().connect(
        sigc::bind( mem_fun( *this, &trackeredit::change_step ), 1 ) );
    m_entry_step = manage( new Entry() );
    m_entry_step->set_size_request( 26, -1 );
    m_entry_step->set_editable( false );

    m_button_fx = manage( new Button( "FX Bind" ) );
    m_button_fx->signal_clicked().connect(
        mem_fun( *this, &trackeredit::popup_fx_menu ) );
    m_entry_fx = manage( new Entry() );
    m_entry_fx->set_size_request( 96, -1 );
    m_entry_fx->set_editable( false );

    m_button_length = manage( new Button( "Len" ) );
    m_entry_length = manage( new Entry() );
    m_entry_length->set_size_request( 40, -1 );
    m_entry_length->set_editable( false );

    toolbar->pack_start( *m_button_rpb,    false, false );
    toolbar->pack_start( *m_entry_rpb,     false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 3 );
    toolbar->pack_start( *m_button_tracks, false, false );
    toolbar->pack_start( *m_entry_tracks,  false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 3 );
    toolbar->pack_start( *m_button_octave, false, false );
    toolbar->pack_start( *m_entry_octave,  false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 3 );
    toolbar->pack_start( *m_button_step,   false, false );
    toolbar->pack_start( *m_entry_step,    false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 3 );
    toolbar->pack_start( *m_button_fx,     false, false );
    toolbar->pack_start( *m_entry_fx,      false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 3 );
    toolbar->pack_start( *m_button_length, false, false );
    toolbar->pack_start( *m_entry_length,  false, false );

    /* ---- grid + scrollbar ---- */
    m_vadjust = manage( new Adjustment( 0, 0, 1, 1, 1, 1 ) );
    m_grid    = manage( new trackergrid( m_seq, m_mainperf, m_pos, m_vadjust ) );
    m_vscroll = manage( new VScrollbar( *m_vadjust ) );

    HBox *gridbox = manage( new HBox( false, 0 ) );
    gridbox->pack_start( *m_grid,    true,  true );
    gridbox->pack_start( *m_vscroll, false, false );

    VBox *vbox = manage( new VBox( false, 0 ) );
    vbox->pack_start( *toolbar, false, false );
    vbox->pack_start( *manage( new HSeparator() ), false, false );
    vbox->pack_start( *gridbox, true,  true );

    add( *vbox );

    update_toolbar_entries();

    show_all();
}

trackeredit::~trackeredit()
{
}

void
trackeredit::update_toolbar_entries( void )
{
    char b[16];
    snprintf( b, sizeof(b), "%d", m_grid->get_rows_per_beat() );
    m_entry_rpb->set_text( b );
    snprintf( b, sizeof(b), "%d", m_grid->get_num_tracks() );
    m_entry_tracks->set_text( b );
    snprintf( b, sizeof(b), "%d", m_grid->get_octave() );
    m_entry_octave->set_text( b );
    snprintf( b, sizeof(b), "%d", m_grid->get_edit_step() );
    m_entry_step->set_text( b );
    m_entry_fx->set_text( m_grid->cur_fx_desc() );
    snprintf( b, sizeof(b), "%ld", m_grid->length_measures() );
    m_entry_length->set_text( b );
}

void
trackeredit::set_rows_per_beat( int a_rpb )
{
    m_grid->set_rows_per_beat( a_rpb );
    update_toolbar_entries();
}

void
trackeredit::popup_rpb_menu( void )
{
    using namespace Menu_Helpers;
    m_menu_rpb = manage( new Menu() );
    m_menu_rpb->items().push_back(
        MenuElem( "4",  sigc::bind( mem_fun( *this, &trackeredit::set_rows_per_beat ), 4 ) ) );
    m_menu_rpb->items().push_back(
        MenuElem( "8",  sigc::bind( mem_fun( *this, &trackeredit::set_rows_per_beat ), 8 ) ) );
    m_menu_rpb->items().push_back(
        MenuElem( "16", sigc::bind( mem_fun( *this, &trackeredit::set_rows_per_beat ), 16 ) ) );
    m_menu_rpb->popup( 0, 0 );
}

/* FX-column target picker: bind the FX column under the grid cursor to a MIDI
   CC or to a hosted-VST parameter enumerated from the track instrument. */
void
trackeredit::popup_fx_menu( void )
{
    using namespace Menu_Helpers;
    m_menu_fx = manage( new Menu() );

    m_menu_fx->items().push_back(
        MenuElem( "Unbind (none)",
                  mem_fun( *this, &trackeredit::do_bind_none ) ) );

    /* --- MIDI CC submenu (a handful of the usual suspects) --- */
    Menu *cc = manage( new Menu() );
    struct { int n; const char *nm; } ccs[] = {
        {  1, "01 Mod Wheel" }, {  7, "07 Volume" }, { 10, "10 Pan" },
        { 11, "11 Expression" }, { 71, "71 Resonance" }, { 74, "74 Cutoff" },
        { 91, "91 Reverb" }, { 93, "93 Chorus" }
    };
    for ( unsigned i = 0; i < sizeof(ccs)/sizeof(ccs[0]); i++ )
        cc->items().push_back(
            MenuElem( ccs[i].nm,
                      sigc::bind( mem_fun( *this, &trackeredit::do_bind_cc ),
                                  ccs[i].n, std::string( ccs[i].nm ) ) ) );
    m_menu_fx->items().push_back( MenuElem( "MIDI CC", *cc ) );

    /* --- VST parameter submenu (enumerated from the track instrument) --- */
    Menu *vst = manage( new Menu() );
    int track = m_grid->get_vst_track();
    int pcount = 0;
    if ( seq24::app::audio_app_running() )
        pcount = seq24::app::audio_app_track_param_count( track );

    if ( pcount <= 0 )
    {
        vst->items().push_back( MenuElem( "(no VST params)" ) );
    }
    else
    {
        int shown = pcount > 48 ? 48 : pcount;   /* keep the menu sane */
        for ( int i = 0; i < shown; i++ )
        {
            unsigned int pid = 0;
            float def = 0.0f;
            char nm[64];
            nm[0] = 0;
            if ( seq24::app::audio_app_track_param_info( track, i, &pid,
                                                         nm, sizeof(nm), &def ) )
            {
                std::string label = nm[0] ? nm : "param";
                char item[96];
                snprintf( item, sizeof(item), "%d: %s", i, label.c_str() );
                vst->items().push_back(
                    MenuElem( item,
                              sigc::bind( mem_fun( *this, &trackeredit::do_bind_vst ),
                                          pid, label ) ) );
            }
        }
    }
    m_menu_fx->items().push_back( MenuElem( "VST Param", *vst ) );

    m_menu_fx->popup( 0, 0 );
}

void
trackeredit::do_bind_cc( int a_cc, std::string a_label )
{
    m_grid->bind_fx_cc( a_cc, a_label );
    update_toolbar_entries();
}

void
trackeredit::do_bind_vst( unsigned int a_pid, std::string a_name )
{
    m_grid->bind_fx_vst( a_pid, a_name );
    update_toolbar_entries();
}

void
trackeredit::do_bind_none( void )
{
    m_grid->bind_fx_none();
    update_toolbar_entries();
}

void
trackeredit::change_octave( int /*a_delta*/ )
{
    int o = m_grid->get_octave() + 1;
    if ( o > 8 ) o = 0;
    m_grid->set_octave( o );
    update_toolbar_entries();
}

void
trackeredit::change_step( int /*a_delta*/ )
{
    int s = m_grid->get_edit_step() + 1;
    if ( s > 8 ) s = 1;
    m_grid->set_edit_step( s );
    update_toolbar_entries();
}

void
trackeredit::change_tracks( int /*a_delta*/ )
{
    int n = m_grid->get_num_tracks() + 1;
    if ( n > 8 ) n = 1;
    m_grid->set_num_tracks( n );
    update_toolbar_entries();
}

void
trackeredit::on_realize()
{
    Gtk::Window::on_realize();
    Glib::signal_timeout().connect(
        mem_fun( *this, &trackeredit::timeout ), c_redraw_ms );
}

bool
trackeredit::timeout( void )
{
    if ( m_seq->get_raise() )
    {
        m_seq->set_raise( false );
        raise();
    }

    if ( m_seq->is_dirty_edit() )
        m_grid->redraw();

    /* fire any due VST-param FX cells as the playhead advances, then paint */
    m_grid->fire_due_fx();
    m_grid->draw_progress_on_window();
    return true;
}

bool
trackeredit::on_delete_event( GdkEventAny * /*a_event*/ )
{
    m_seq->set_editing( false );
    delete this;
    return false;
}
