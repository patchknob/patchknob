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

/*
 *  trackeredit : a tracker-grid window bound to one sequence*.
 *
 *  Rows are time steps; columns are note tracks.  A row spans
 *  ( c_ppqn / m_rows_per_beat ) ticks.  The number of rows is
 *  get_length() / ticks-per-row.
 */
class trackeredit : public Gtk::Window
{
 private:

    sequence   *m_seq;
    perform    *m_mainperf;
    int         m_pos;

    /* drawing surface (back-buffer pixmap pattern, like seqroll) */
    Gtk::DrawingArea           *m_draw;
    Glib::RefPtr<Gdk::Window>   m_window;
    Glib::RefPtr<Gdk::GC>       m_gc;
    Glib::RefPtr<Gdk::Pixmap>   m_pixmap;
    Gtk::VScrollbar            *m_vscroll;
    Gtk::Adjustment            *m_vadjust;

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

    /* tick range covered by a row */
    long row_start_tick( int a_row );

    /* find the note-on event (note, velocity) that begins on a row, or -1 */
    bool note_at_row( int a_row, int a_track, int *a_note, int *a_vel );

    /* write / clear a cell */
    void set_note_at_cursor( int a_note );
    void clear_cell_at_cursor( void );

    /* note name string e.g. "C-4", "---", "===" */
    std::string note_name( int a_note );

    /* keymap: piano-style note from a GDK keyval, -1 if none.  Returns the
       0-based pitch class (0..11 = C..B) plus an octave offset via *a_oct. */
    int key_to_pitch( unsigned int a_keyval, int *a_oct_offset );

    /* drawing */
    void update_sizes( void );
    void draw_background( void );
    void draw_grid( void );
    void update_pixmap( void );
    void force_draw( void );
    void draw_progress_on_window( void );

    /* toolbar */
    void set_rows_per_beat( int a_rpb );
    void change_octave( int a_delta );
    void change_step( int a_delta );
    void popup_rpb_menu( void );
    void update_toolbar_entries( void );

    void move_cursor( int a_drow, int a_dcol );
    void ensure_cursor_visible( void );

    /* events */
    void on_realize( void );
    bool on_expose( GdkEventExpose *a_e );
    bool on_key_press( GdkEventKey *a_e );
    bool on_button_press( GdkEventButton *a_e );
    void change_vert( void );
    bool timeout( void );

 public:

    trackeredit( sequence *a_seq, perform *a_perf, int a_pos );
    ~trackeredit();

    bool on_delete_event( GdkEventAny *a_event );
};

#endif
