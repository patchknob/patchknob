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
//  trackeredit.h
//
//  A classic vertical TRACKER view/editor for a seq24 sequence.  It is a
//  sibling editor of seqedit: both edit the SAME sequence* (the same absolute
//  MIDI note-on/off event pairs), so notes round-trip between the piano roll
//  and the tracker.  Rendered entirely in monochrome (black & white) with
//  Cairo, matching the seq24 reskin palette in src/ui/palette.h.
//
//  The grid is a proper Gtk::DrawingArea SUBCLASS (trackergrid) that overrides
//  on_realize()/on_expose_event() and chains the base-class realize FIRST --
//  the proven seqroll/seqkeys pattern.  trackeredit (the Gtk::Window) only
//  hosts the toolbar + grid + scrollbar.
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_TRACKEREDIT
#define SEQ24_TRACKEREDIT

#include "sequence.h"
#include "perform.h"

#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/table.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/scrollbar.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/entry.h>
#include <gtkmm/separator.h>
#include <gtkmm/menu.h>

#include <string>

class trackeredit;     /* forward */

/*
 *  trackergrid : the drawing surface for the tracker.  A real DrawingArea
 *  subclass (like seqroll), so on_realize() chains Gtk::DrawingArea::on_realize()
 *  first and the GdkWindow / GC / pixmap are always valid afterwards.
 */
class trackergrid : public Gtk::DrawingArea
{
 private:

    sequence   *m_seq;
    perform    *m_mainperf;
    int         m_pos;

    Gtk::Adjustment *m_vadjust;

    Glib::RefPtr<Gdk::Window>   m_window;
    Glib::RefPtr<Gdk::GC>       m_gc;
    Glib::RefPtr<Gdk::Pixmap>   m_pixmap;

    /* geometry (pixels) */
    int  m_window_x;
    int  m_window_y;
    int  m_row_h;          /* row height                                   */
    int  m_gutter_w;       /* row-number gutter width                      */
    int  m_track_w;        /* per-track column width                       */
    int  m_num_tracks;     /* number of note-track columns (1 for mono)    */

    /* model -> grid mapping */
    int  m_rows_per_beat;  /* 4 / 8 / 16                                   */

    /* edit state */
    int  m_cursor_row;
    int  m_cursor_track;
    int  m_cursor_col;     /* 0 = note, 1 = velocity, 2 = fx               */
    int  m_octave;         /* base octave for keyboard note entry          */
    int  m_edit_step;      /* rows to advance after entering a note        */
    int  m_velocity;       /* default velocity for new notes               */
    int  m_top_row;        /* first visible row (scroll)                   */

    int  m_old_progress_row;

    /* helpers ------------------------------------------------------------ */
    int  ticks_per_row( void );
    int  num_rows( void );
    int  visible_rows( void );
    long row_start_tick( int a_row );
    bool note_at_row( int a_row, int a_track, int *a_note, int *a_vel );
    void set_note_at_cursor( int a_note );
    void clear_cell_at_cursor( void );
    std::string note_name( int a_note );
    int  key_to_pitch( unsigned int a_keyval, int *a_oct_offset );

    /* drawing */
    void draw_background( void );
    void draw_grid( void );
    void update_pixmap( void );
    void force_draw( void );

    void ensure_cursor_visible( void );

    /* DrawingArea overrides (seqroll pattern) */
    void on_realize();
    bool on_expose_event( GdkEventExpose *a_e );
    bool on_key_press_event( GdkEventKey *a_e );
    bool on_button_press_event( GdkEventButton *a_e );
    void on_size_allocate( Gtk::Allocation &a_r );

    void change_vert( void );

 public:

    trackergrid( sequence *a_seq, perform *a_perf, int a_pos,
                 Gtk::Adjustment *a_vadjust );
    ~trackergrid();

    /* called by trackeredit (window) */
    void update_sizes( void );
    void redraw( void );
    void draw_progress_on_window( void );
    void move_cursor( int a_drow, int a_dcol );

    void set_rows_per_beat( int a_rpb );
    int  get_rows_per_beat( void ) { return m_rows_per_beat; }
    void set_octave( int a_oct )   { m_octave = a_oct; }
    int  get_octave( void )        { return m_octave; }
    void set_edit_step( int a_s )  { m_edit_step = a_s; }
    int  get_edit_step( void )     { return m_edit_step; }

    long length_measures( void );
};

/*
 *  trackeredit : the top-level editor window.  Hosts a trackergrid plus a
 *  monochrome toolbar and a vertical scrollbar.
 */
class trackeredit : public Gtk::Window
{
 private:

    sequence   *m_seq;
    perform    *m_mainperf;
    int         m_pos;

    trackergrid     *m_grid;
    Gtk::VScrollbar *m_vscroll;
    Gtk::Adjustment *m_vadjust;

    /* toolbar widgets */
    Gtk::Button *m_button_rpb;
    Gtk::Entry  *m_entry_rpb;
    Gtk::Button *m_button_octave;
    Gtk::Entry  *m_entry_octave;
    Gtk::Button *m_button_step;
    Gtk::Entry  *m_entry_step;
    Gtk::Button *m_button_length;
    Gtk::Entry  *m_entry_length;

    Gtk::Menu   *m_menu_rpb;

    void set_rows_per_beat( int a_rpb );
    void change_octave( int a_delta );
    void change_step( int a_delta );
    void popup_rpb_menu( void );
    void update_toolbar_entries( void );

    void on_realize();
    bool timeout( void );

 public:

    trackeredit( sequence *a_seq, perform *a_perf, int a_pos );
    ~trackeredit();

    bool on_delete_event( GdkEventAny *a_event );
};

#endif
