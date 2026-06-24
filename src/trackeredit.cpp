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
//  trackeredit.cpp -- vertical tracker view/editor of a seq24 sequence.
//
//  Maps the sequence's absolute MIDI note-on/off pairs onto a vertical grid of
//  time-step rows.  Editing writes the same note-on/off events the piano roll
//  uses, so notes round-trip both ways.  All drawing is monochrome (Cairo),
//  using the greyscale palette in src/ui/palette.h.
//
//-----------------------------------------------------------------------------

#include "trackeredit.h"
#include "ui/palette.h"
#include "globals.h"

#include <gtkmm/menu.h>
#include <gdkmm/general.h>   // Gdk::Cairo::set_source_pixmap if needed
#include <cstdio>
#include <gdk/gdkkeysyms.h>

using namespace Gtk;

/* ------------------------------------------------------------------------- *
 *  construction
 * ------------------------------------------------------------------------- */

trackeredit::trackeredit( sequence *a_seq, perform *a_perf, int a_pos )
{
    m_seq      = a_seq;
    m_mainperf = a_perf;
    m_pos      = a_pos;

    m_rows_per_beat = 4;
    m_num_tracks    = 1;

    m_cursor_row    = 0;
    m_cursor_track  = 0;
    m_cursor_col    = 0;
    m_octave        = 4;
    m_edit_step     = 1;
    m_velocity      = 100;
    m_top_row       = 0;
    m_old_progress_row = -1;

    /* geometry */
    m_row_h    = 16;
    m_gutter_w = 44;
    m_track_w  = 150;
    m_window_x = m_gutter_w + m_track_w * m_num_tracks + 1;
    m_window_y = 480;

    set_title( std::string("Tracker - ") + m_seq->get_name() );
    set_size_request( m_window_x + 18, 540 );

    m_seq->set_editing( true );

    /* ---- toolbar ---- */
    HBox *toolbar = manage( new HBox( false, 2 ) );
    toolbar->set_border_width( 2 );

    m_button_rpb = manage( new Button( "Rows/Beat" ) );
    m_button_rpb->signal_clicked().connect(
        mem_fun( *this, &trackeredit::popup_rpb_menu ) );
    m_entry_rpb = manage( new Entry() );
    m_entry_rpb->set_size_request( 32, -1 );
    m_entry_rpb->set_editable( false );

    m_button_octave = manage( new Button( "Oct -/+" ) );
    m_button_octave->signal_clicked().connect(
        sigc::bind( mem_fun( *this, &trackeredit::change_octave ), 1 ) );
    m_entry_octave = manage( new Entry() );
    m_entry_octave->set_size_request( 28, -1 );
    m_entry_octave->set_editable( false );

    m_button_step = manage( new Button( "Step -/+" ) );
    m_button_step->signal_clicked().connect(
        sigc::bind( mem_fun( *this, &trackeredit::change_step ), 1 ) );
    m_entry_step = manage( new Entry() );
    m_entry_step->set_size_request( 28, -1 );
    m_entry_step->set_editable( false );

    m_button_length = manage( new Button( "Len" ) );
    m_entry_length = manage( new Entry() );
    m_entry_length->set_size_request( 44, -1 );
    m_entry_length->set_editable( false );

    toolbar->pack_start( *m_button_rpb,    false, false );
    toolbar->pack_start( *m_entry_rpb,     false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 4 );
    toolbar->pack_start( *m_button_octave, false, false );
    toolbar->pack_start( *m_entry_octave,  false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 4 );
    toolbar->pack_start( *m_button_step,   false, false );
    toolbar->pack_start( *m_entry_step,    false, false );
    toolbar->pack_start( *manage( new VSeparator() ), false, false, 4 );
    toolbar->pack_start( *m_button_length, false, false );
    toolbar->pack_start( *m_entry_length,  false, false );

    /* ---- grid drawing area + scrollbar ---- */
    m_draw = manage( new DrawingArea() );
    m_draw->set_size_request( m_window_x, m_window_y );
    m_draw->set_double_buffered( false );
    m_draw->add_events( Gdk::EXPOSURE_MASK   |
                        Gdk::BUTTON_PRESS_MASK |
                        Gdk::KEY_PRESS_MASK  |
                        Gdk::FOCUS_CHANGE_MASK |
                        Gdk::SCROLL_MASK );
    m_draw->set_flags( Gtk::CAN_FOCUS );

    m_draw->signal_realize().connect( mem_fun( *this, &trackeredit::on_realize ) );
    m_draw->signal_expose_event().connect( mem_fun( *this, &trackeredit::on_expose ) );
    m_draw->signal_key_press_event().connect( mem_fun( *this, &trackeredit::on_key_press ) );
    m_draw->signal_button_press_event().connect( mem_fun( *this, &trackeredit::on_button_press ) );

    m_vadjust = manage( new Adjustment( 0, 0, 1, 1, 1, 1 ) );
    m_vscroll = manage( new VScrollbar( *m_vadjust ) );
    m_vadjust->signal_value_changed().connect(
        mem_fun( *this, &trackeredit::change_vert ) );

    HBox *gridbox = manage( new HBox( false, 0 ) );
    gridbox->pack_start( *m_draw,    true,  true );
    gridbox->pack_start( *m_vscroll, false, false );

    VBox *vbox = manage( new VBox( false, 0 ) );
    vbox->pack_start( *toolbar, false, false );
    vbox->pack_start( *manage( new HSeparator() ), false, false );
    vbox->pack_start( *gridbox, true,  true );

    add( *vbox );

    update_toolbar_entries();

    show_all();

    Glib::signal_timeout().connect(
        mem_fun( *this, &trackeredit::timeout ), c_redraw_ms );
}

trackeredit::~trackeredit()
{
}

/* ------------------------------------------------------------------------- *
 *  model <-> grid helpers
 * ------------------------------------------------------------------------- */

int
trackeredit::ticks_per_row( void )
{
    int t = c_ppqn / m_rows_per_beat;
    if ( t < 1 ) t = 1;
    return t;
}

int
trackeredit::num_rows( void )
{
    int rows = m_seq->get_length() / ticks_per_row();
    if ( rows < 1 ) rows = 1;
    return rows;
}

int
trackeredit::visible_rows( void )
{
    int r = m_window_y / m_row_h;
    if ( r < 1 ) r = 1;
    return r;
}

long
trackeredit::row_start_tick( int a_row )
{
    return (long) a_row * ticks_per_row();
}

std::string
trackeredit::note_name( int a_note )
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

/* find the note-on whose timestamp falls in this row's tick window */
bool
trackeredit::note_at_row( int a_row, int /*a_track*/, int *a_note, int *a_vel )
{
    long ts = row_start_tick( a_row );
    long tf = ts + ticks_per_row();

    long  tick_s, tick_f;
    int   note, vel;
    bool  selected;

    m_seq->reset_draw_marker();

    bool found = false;
    int  best_note = -1, best_vel = 0;

    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        if ( tick_s >= ts && tick_s < tf )
        {
            /* highest note in the row wins (mono tracker view) */
            if ( !found || note > best_note )
            {
                best_note = note;
                best_vel  = vel;
                found     = true;
            }
        }
    }

    if ( found )
    {
        if ( a_note ) *a_note = best_note;
        if ( a_vel )  *a_vel  = best_vel;
    }
    return found;
}

/* write a note-on/off pair at the cursor row.  Removes any existing note that
   starts in this row first, so a cell holds at most one note. */
void
trackeredit::set_note_at_cursor( int a_note )
{
    if ( a_note < 0 || a_note > 127 )
        return;

    long ts = row_start_tick( m_cursor_row );
    long len = ticks_per_row() * m_edit_step;
    if ( len < 1 ) len = ticks_per_row();

    m_seq->push_undo();

    /* clear any existing note that starts on this row */
    clear_cell_at_cursor();

    /* add_note hardcodes velocity 100; add the events directly so we control
       the velocity.  This matches what seqroll/sequence::add_note do. */
    m_seq->add_event( ts,        EVENT_NOTE_ON,  (unsigned char) a_note,
                      (unsigned char) m_velocity, false );
    m_seq->add_event( ts + len,  EVENT_NOTE_OFF, (unsigned char) a_note,
                      (unsigned char) m_velocity, false );

    m_seq->verify_and_link();
    m_seq->set_dirty();

    /* audible preview */
    m_seq->play_note_on( a_note );

    update_pixmap();
    force_draw();
}

void
trackeredit::clear_cell_at_cursor( void )
{
    long ts = row_start_tick( m_cursor_row );
    long tf = ts + ticks_per_row() - 1;

    m_seq->unselect();
    int n = m_seq->select_note_events( ts, 127, tf, 0,
                                       sequence::e_select );
    if ( n > 0 )
    {
        m_seq->mark_selected();
        m_seq->remove_marked();
        m_seq->verify_and_link();
        m_seq->set_dirty();
    }
}

/* GDK keyval -> pitch class (0..11) with octave offset.  Lower row Z..M is the
   base octave; Q..U is one octave up (classic tracker / DAW layout). */
int
trackeredit::key_to_pitch( unsigned int kv, int *a_oct_offset )
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
        case 'q': case 'Q': *a_oct_offset = 1; return 0;   // C
        case '2':           *a_oct_offset = 1; return 1;   // C#
        case 'w': case 'W': *a_oct_offset = 1; return 2;   // D
        case '3':           *a_oct_offset = 1; return 3;   // D#
        case 'e': case 'E': *a_oct_offset = 1; return 4;   // E
        case 'r': case 'R': *a_oct_offset = 1; return 5;   // F
        case '5':           *a_oct_offset = 1; return 6;   // F#
        case 't': case 'T': *a_oct_offset = 1; return 7;   // G
        case '6':           *a_oct_offset = 1; return 8;   // G#
        case 'y': case 'Y': *a_oct_offset = 1; return 9;   // A
        case '7':           *a_oct_offset = 1; return 10;  // A#
        case 'u': case 'U': *a_oct_offset = 1; return 11;  // B
    }
    return -1;
}

/* ------------------------------------------------------------------------- *
 *  geometry / scrolling
 * ------------------------------------------------------------------------- */

void
trackeredit::update_sizes( void )
{
    int rows = num_rows();
    int vis  = visible_rows();

    m_vadjust->set_lower( 0 );
    m_vadjust->set_upper( rows );
    m_vadjust->set_page_size( vis );
    m_vadjust->set_step_increment( 1 );
    m_vadjust->set_page_increment( vis );

    int maxv = rows - vis;
    if ( maxv < 0 ) maxv = 0;
    if ( m_vadjust->get_value() > maxv )
        m_vadjust->set_value( maxv );

    if ( m_draw->is_realized() )
    {
        m_pixmap = Gdk::Pixmap::create( m_window, m_window_x, m_window_y, -1 );
    }
}

void
trackeredit::change_vert( void )
{
    m_top_row = (int) m_vadjust->get_value();
    update_pixmap();
    force_draw();
}

void
trackeredit::ensure_cursor_visible( void )
{
    int vis = visible_rows();
    if ( m_cursor_row < m_top_row )
        m_top_row = m_cursor_row;
    else if ( m_cursor_row >= m_top_row + vis )
        m_top_row = m_cursor_row - vis + 1;

    if ( m_top_row < 0 ) m_top_row = 0;
    m_vadjust->set_value( m_top_row );
}

/* ------------------------------------------------------------------------- *
 *  drawing
 * ------------------------------------------------------------------------- */

void
trackeredit::draw_background( void )
{
    Cairo::RefPtr<Cairo::Context> cr = m_pixmap->create_cairo_context();
    cr->set_line_width( 1.0 );

    /* clear to pure black */
    synth::set_source( cr, synth::cBg );
    cr->rectangle( 0, 0, m_window_x, m_window_y );
    cr->fill();
}

void
trackeredit::draw_grid( void )
{
    Cairo::RefPtr<Cairo::Context> cr = m_pixmap->create_cairo_context();
    cr->set_line_width( 1.0 );
    cr->select_font_face( "monospace",
                          Cairo::FONT_SLANT_NORMAL,
                          Cairo::FONT_WEIGHT_NORMAL );
    cr->set_font_size( 11.0 );

    int rows = num_rows();
    int vis  = visible_rows();

    for ( int sr = 0; sr < vis; sr++ )
    {
        int row = m_top_row + sr;
        if ( row >= rows ) break;

        int y = sr * m_row_h;

        bool is_beat = ( row % m_rows_per_beat ) == 0;
        bool is_bar  = ( row % ( m_rows_per_beat * m_seq->get_bpm() ) ) == 0;

        /* row background striping: brighter every beat/bar */
        if ( is_bar )
            synth::set_source( cr, synth::cPanel, 1.0 );
        else if ( is_beat )
            synth::set_source( cr, synth::cPanel, 0.6 );
        else
            synth::set_source( cr, synth::cBg );
        cr->rectangle( 0, y, m_window_x, m_row_h );
        cr->fill();

        /* cursor cell highlight */
        if ( row == m_cursor_row )
        {
            synth::set_source( cr, synth::cAccent, 0.30 );
            cr->rectangle( 0, y, m_window_x, m_row_h );
            cr->fill();
        }

        /* horizontal gridline */
        synth::set_source( cr, synth::cDim, is_beat ? 0.9 : 0.4 );
        cr->move_to( 0, y + 0.5 );
        cr->line_to( m_window_x, y + 0.5 );
        cr->stroke();

        /* row-number gutter (decimal) */
        char rn[8];
        snprintf( rn, sizeof(rn), "%3d", row );
        synth::set_source( cr, is_beat ? synth::cHi : synth::cDim );
        cr->move_to( 4, y + m_row_h - 4 );
        cr->show_text( rn );

        /* track columns */
        for ( int t = 0; t < m_num_tracks; t++ )
        {
            int cx = m_gutter_w + t * m_track_w;
            int note, vel;
            bool has = note_at_row( row, t, &note, &vel );

            std::string cell_note = has ? note_name( note ) : "---";
            char cell_vel[8];
            if ( has ) snprintf( cell_vel, sizeof(cell_vel), "%02X", vel & 0x7f );
            else       snprintf( cell_vel, sizeof(cell_vel), "--" );

            synth::set_source( cr, has ? synth::cWhite : synth::cDim );

            /* note name */
            cr->move_to( cx + 8, y + m_row_h - 4 );
            cr->show_text( cell_note );

            /* velocity (hex) */
            cr->move_to( cx + 56, y + m_row_h - 4 );
            cr->show_text( cell_vel );

            /* fx placeholder */
            synth::set_source( cr, synth::cDim );
            cr->move_to( cx + 86, y + m_row_h - 4 );
            cr->show_text( "..." );

            /* cursor sub-cell underline (which column is active) */
            if ( row == m_cursor_row && t == m_cursor_track )
            {
                int ux = cx + 8;
                int uw = 40;
                if ( m_cursor_col == 1 ) { ux = cx + 56; uw = 18; }
                if ( m_cursor_col == 2 ) { ux = cx + 86; uw = 24; }
                synth::set_source( cr, synth::cSel );
                cr->rectangle( ux, y + m_row_h - 2, uw, 1.5 );
                cr->fill();
            }
        }
    }

    /* vertical separators: gutter edge + each track edge */
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
trackeredit::update_pixmap( void )
{
    if ( !m_pixmap )
        return;
    draw_background();
    draw_grid();
}

void
trackeredit::force_draw( void )
{
    if ( !m_pixmap || !m_window )
        return;
    m_window->draw_drawable( m_gc, m_pixmap, 0, 0, 0, 0,
                             m_window_x, m_window_y );
    draw_progress_on_window();
}

/* playhead row highlight, drawn directly on the window over the pixmap */
void
trackeredit::draw_progress_on_window( void )
{
    if ( !m_window || !m_pixmap )
        return;

    long tick = m_seq->get_last_tick();
    int  prow = tick / ticks_per_row();

    /* restore previous progress row from the pixmap */
    if ( m_old_progress_row >= 0 )
    {
        int sr = m_old_progress_row - m_top_row;
        if ( sr >= 0 && sr < visible_rows() )
            m_window->draw_drawable( m_gc, m_pixmap,
                                     0, sr * m_row_h,
                                     0, sr * m_row_h,
                                     m_window_x, m_row_h );
    }

    int sr = prow - m_top_row;
    if ( sr >= 0 && sr < visible_rows() )
    {
        Cairo::RefPtr<Cairo::Context> cr = m_window->create_cairo_context();
        cr->set_line_width( 1.0 );
        synth::set_source( cr, synth::cActive, 0.85 );
        cr->rectangle( 0, sr * m_row_h, m_window_x, 2 );
        cr->fill();
    }

    m_old_progress_row = prow;
}

/* ------------------------------------------------------------------------- *
 *  toolbar actions
 * ------------------------------------------------------------------------- */

void
trackeredit::update_toolbar_entries( void )
{
    char b[16];
    snprintf( b, sizeof(b), "%d", m_rows_per_beat );
    m_entry_rpb->set_text( b );
    snprintf( b, sizeof(b), "%d", m_octave );
    m_entry_octave->set_text( b );
    snprintf( b, sizeof(b), "%d", m_edit_step );
    m_entry_step->set_text( b );
    /* length in measures = length_ticks / (ppqn * beats-per-measure) */
    long bpm = m_seq->get_bpm();
    if ( bpm < 1 ) bpm = 4;
    long measures = m_seq->get_length() / ( c_ppqn * bpm );
    if ( measures < 1 ) measures = 1;
    snprintf( b, sizeof(b), "%ld", measures );
    m_entry_length->set_text( b );
}

void
trackeredit::set_rows_per_beat( int a_rpb )
{
    if ( a_rpb != 4 && a_rpb != 8 && a_rpb != 16 )
        return;
    m_rows_per_beat = a_rpb;
    m_cursor_row = 0;
    m_top_row = 0;
    update_toolbar_entries();
    update_sizes();
    update_pixmap();
    force_draw();
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

void
trackeredit::change_octave( int /*a_delta*/ )
{
    /* cycle 0..8 */
    m_octave++;
    if ( m_octave > 8 ) m_octave = 0;
    update_toolbar_entries();
}

void
trackeredit::change_step( int /*a_delta*/ )
{
    /* cycle 1..8 */
    m_edit_step++;
    if ( m_edit_step > 8 ) m_edit_step = 1;
    update_toolbar_entries();
}

void
trackeredit::move_cursor( int a_drow, int a_dcol )
{
    m_cursor_col += a_dcol;
    if ( m_cursor_col < 0 ) m_cursor_col = 2;
    if ( m_cursor_col > 2 ) m_cursor_col = 0;

    m_cursor_row += a_drow;
    if ( m_cursor_row < 0 ) m_cursor_row = 0;
    if ( m_cursor_row >= num_rows() ) m_cursor_row = num_rows() - 1;

    ensure_cursor_visible();
    update_pixmap();
    force_draw();
}

/* ------------------------------------------------------------------------- *
 *  gtk events
 * ------------------------------------------------------------------------- */

void
trackeredit::on_realize( void )
{
    // The child DrawingArea's GdkWindow may not exist yet when this signal
    // fires (unlike a DrawingArea subclass that chains the default realize
    // first), so guard for NULL.  If it's not ready, the GC/pixmap are created
    // lazily on the first expose, where the window is guaranteed to exist.
    m_window = m_draw->get_window();
    if ( !m_window )
        return;

    if ( !m_gc )
        m_gc = Gdk::GC::create( m_window );
    m_window->clear();

    update_sizes();
    update_pixmap();
    m_draw->grab_focus();
}

bool
trackeredit::on_expose( GdkEventExpose *e )
{
    // Lazy one-time setup: by the time an expose arrives the window exists.
    if ( !m_window )
        m_window = m_draw->get_window();
    if ( !m_window )
        return true;
    if ( !m_gc )
    {
        m_gc = Gdk::GC::create( m_window );
        update_sizes();
        update_pixmap();
    }
    if ( !m_pixmap )
        return true;
    m_window->draw_drawable( m_gc, m_pixmap,
                             e->area.x, e->area.y,
                             e->area.x, e->area.y,
                             e->area.width, e->area.height );
    draw_progress_on_window();
    return true;
}

bool
trackeredit::on_button_press( GdkEventButton *e )
{
    m_draw->grab_focus();

    if ( e->button == 1 )
    {
        int sr  = (int) e->y / m_row_h;
        int row = m_top_row + sr;
        if ( row >= 0 && row < num_rows() )
            m_cursor_row = row;

        int x = (int) e->x;
        if ( x >= m_gutter_w )
        {
            int t = ( x - m_gutter_w ) / m_track_w;
            if ( t >= 0 && t < m_num_tracks )
                m_cursor_track = t;
            int rel = ( x - m_gutter_w ) % m_track_w;
            if ( rel < 52 )      m_cursor_col = 0;
            else if ( rel < 82 ) m_cursor_col = 1;
            else                 m_cursor_col = 2;
        }
        update_pixmap();
        force_draw();
    }
    return true;
}

bool
trackeredit::on_key_press( GdkEventKey *e )
{
    unsigned int kv = e->keyval;

    /* navigation */
    switch ( kv )
    {
        case GDK_Up:        move_cursor( -1, 0 ); return true;
        case GDK_Down:      move_cursor(  1, 0 ); return true;
        case GDK_Left:      move_cursor(  0,-1 ); return true;
        case GDK_Right:     move_cursor(  0, 1 ); return true;
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
            m_seq->push_undo();
            clear_cell_at_cursor();
            update_pixmap();
            force_draw();
            /* auto-advance like a tracker */
            move_cursor( m_edit_step, 0 );
            return true;
    }

    /* note entry via tracker keyboard piano */
    int oct_off = 0;
    int pc = key_to_pitch( kv, &oct_off );
    if ( pc >= 0 )
    {
        int note = ( m_octave + oct_off + 1 ) * 12 + pc;  /* C-4 == MIDI 60 */
        if ( note >= 0 && note <= 127 )
        {
            set_note_at_cursor( note );
            move_cursor( m_edit_step, 0 );
        }
        return true;
    }

    return false;
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
    {
        update_pixmap();
        force_draw();
    }

    draw_progress_on_window();
    return true;
}

bool
trackeredit::on_delete_event( GdkEventAny * /*a_event*/ )
{
    m_seq->set_editing( false );
    delete this;
    return false;
}
