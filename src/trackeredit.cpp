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
//  The grid is a Gtk::DrawingArea SUBCLASS (trackergrid) so that on_realize()
//  chains Gtk::DrawingArea::on_realize() FIRST -- the proven seqroll pattern
//  that guarantees the GdkWindow exists before we create the GC / pixmap.
//
//-----------------------------------------------------------------------------

#include "trackeredit.h"
#include "ui/palette.h"
#include "globals.h"

#include <gtkmm/menu.h>
#include <cstdio>
#include <gdk/gdkkeysyms.h>

using namespace Gtk;

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

    m_cursor_row    = 0;
    m_cursor_track  = 0;
    m_cursor_col    = 0;
    m_octave        = 4;
    m_edit_step     = 1;
    m_velocity      = 100;
    m_top_row       = 0;
    m_old_progress_row = -1;

    m_row_h    = 16;
    m_gutter_w = 44;
    m_track_w  = 150;
    m_window_x = m_gutter_w + m_track_w * m_num_tracks + 1;
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
    int r = m_window_y / m_row_h;
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

bool
trackergrid::note_at_row( int a_row, int /*a_track*/, int *a_note, int *a_vel )
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

void
trackergrid::set_note_at_cursor( int a_note )
{
    if ( a_note < 0 || a_note > 127 )
        return;

    long ts = row_start_tick( m_cursor_row );
    long len = ticks_per_row() * m_edit_step;
    if ( len < 1 ) len = ticks_per_row();

    m_seq->push_undo();

    /* clear any existing note that starts on this row */
    clear_cell_at_cursor();

    /* add the events directly so we control the velocity (add_note hardcodes
       100); matches what seqroll/sequence::add_note write. */
    m_seq->add_event( ts,        EVENT_NOTE_ON,  (unsigned char) a_note,
                      (unsigned char) m_velocity, false );
    m_seq->add_event( ts + len,  EVENT_NOTE_OFF, (unsigned char) a_note,
                      (unsigned char) m_velocity, false );

    m_seq->verify_and_link();
    m_seq->set_dirty();

    m_seq->play_note_on( a_note );

    update_pixmap();
    force_draw();
}

void
trackergrid::clear_cell_at_cursor( void )
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

/* ------------------------------------------------------------------------- *
 *  geometry / scrolling
 * ------------------------------------------------------------------------- */

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
trackergrid::draw_grid( void )
{
    if ( !m_pixmap ) return;
    Cairo::RefPtr<Cairo::Context> cr = m_pixmap->create_cairo_context();
    cr->set_line_width( 1.0 );
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

        int y = sr * m_row_h;

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

            cr->move_to( cx + 8, y + m_row_h - 4 );
            cr->show_text( cell_note );

            cr->move_to( cx + 56, y + m_row_h - 4 );
            cr->show_text( cell_vel );

            synth::set_source( cr, synth::cDim );
            cr->move_to( cx + 86, y + m_row_h - 4 );
            cr->show_text( "..." );

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
trackergrid::on_key_press_event( GdkEventKey *e )
{
    unsigned int kv = e->keyval;

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
            move_cursor( m_edit_step, 0 );
            return true;
    }

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

/* ========================================================================= *
 *  trackeredit  (Gtk::Window host)
 * ========================================================================= */

trackeredit::trackeredit( sequence *a_seq, perform *a_perf, int a_pos )
{
    m_seq      = a_seq;
    m_mainperf = a_perf;
    m_pos      = a_pos;

    set_title( std::string("Tracker - ") + m_seq->get_name() );
    set_size_request( 230, 540 );

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

    m_button_octave = manage( new Button( "Oct +" ) );
    m_button_octave->signal_clicked().connect(
        sigc::bind( mem_fun( *this, &trackeredit::change_octave ), 1 ) );
    m_entry_octave = manage( new Entry() );
    m_entry_octave->set_size_request( 28, -1 );
    m_entry_octave->set_editable( false );

    m_button_step = manage( new Button( "Step +" ) );
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
    snprintf( b, sizeof(b), "%d", m_grid->get_octave() );
    m_entry_octave->set_text( b );
    snprintf( b, sizeof(b), "%d", m_grid->get_edit_step() );
    m_entry_step->set_text( b );
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
