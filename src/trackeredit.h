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
//  A classic vertical TRACKER view/editor for a seq24 sequence, expanded into a
//  full multi-track tracker with note+velocity lanes and FX-command columns.
//
//  It is a sibling editor of seqedit: both edit the SAME sequence* (the same
//  absolute MIDI note-on/off event pairs), so notes round-trip between the
//  piano roll and the tracker.  Rendered entirely in monochrome (black & white)
//  with Cairo, matching the seq24 reskin palette in src/ui/palette.h.
//
//  --- MODEL / LAYOUT --------------------------------------------------------
//
//  N note-track "columns" (1..8) are drawn side by side.  They are a POLYPHONIC
//  STACKING view of the one shared sequence: at each row the note-on events that
//  start in that row are gathered and sorted ascending by pitch; note-column t
//  shows the t-th-lowest note of that row (its chord).  Editing a note-column
//  cell adds / replaces / removes a note in that shared chord, so everything
//  still round-trips through the piano roll (which has no notion of columns).
//
//  Each note-column carries a velocity sub-column (hex) plus FX_COLS FX-command
//  sub-columns to its right.  An FX column is BOUND (via the toolbar picker) to
//  a target -- either a MIDI CC number or a hosted-VST parameter -- and each row
//  cell holds a hex value for that target at that tick:
//
//    * MIDI CC   : the cell is persisted as a real EVENT_CONTROL_CHANGE event in
//                  the sequence, so it plays through the normal engine output
//                  path (sequence::play -> mastermidibus::play ->
//                  audio_app_route_midi) with zero extra plumbing.
//    * VST param : the cell value lives in a per-pattern command map the tracker
//                  owns (VST params are not MIDI events).  It fires immediately
//                  on entry and, during playback, from the redraw timeout that
//                  watches the playhead tick, via audio_app_route_param().
//
//  The grid is a proper Gtk::DrawingArea SUBCLASS (trackergrid) that overrides
//  on_realize()/on_expose_event() and chains the base-class realize FIRST --
//  the proven seqroll/seqkeys pattern that fixes the Windows GTK realize crash.
//  trackeredit (the Gtk::Window) only hosts the toolbar + grid + scrollbar.
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
#include <vector>
#include <map>

class trackeredit;     /* forward */

/*
 *  An FX column is bound to one of these targets.
 */
enum fx_type
{
    FX_NONE = 0,
    FX_MIDI_CC,        /* emits a Control Change (persisted as a seq event)   */
    FX_VST_PARAM       /* drives a hosted-VST parameter via audio_app          */
};

struct fx_binding
{
    int          type;    /* fx_type                                          */
    int          cc;      /* controller number       (FX_MIDI_CC)            */
    unsigned int pid;     /* VST parameter id        (FX_VST_PARAM)         */
    std::string  label;   /* short header label, e.g. "C74" / "P:Cut"        */

    fx_binding() : type(FX_NONE), cc(0), pid(0), label("--") {}
};

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
    int  m_header_h;       /* column-header band height                    */
    int  m_gutter_w;       /* row-number gutter width                      */
    int  m_track_w;        /* per-track column width                       */
    int  m_num_tracks;     /* number of note-track columns (1..8)          */
    int  m_fx_cols;        /* FX sub-columns per note-track (>= 2)         */

    /* model -> grid mapping */
    int  m_rows_per_beat;  /* 4 / 8 / 16                                   */

    /* edit state */
    int  m_cursor_row;
    int  m_cursor_track;
    int  m_cursor_col;     /* 0 note, 1 vel, 2.. = FX column (col-2)       */
    int  m_octave;         /* base octave for keyboard note entry          */
    int  m_edit_step;      /* rows to advance after entering a note        */
    int  m_velocity;       /* default velocity for new notes               */
    int  m_top_row;        /* first visible row (scroll)                   */

    int  m_old_progress_row;

    /* FX bindings + VST value store ------------------------------------- *
     *   m_fx_bind[track][fxcol]              -> binding
     *   m_fx_vst[track][fxcol][row]          -> value 0..255 (VST only)
     * CC values are NOT stored here; they live in the sequence as events.  */
    std::vector< std::vector< fx_binding > >                 m_fx_bind;
    std::vector< std::vector< std::map<int,int> > >          m_fx_vst;

    /* playback-fire bookkeeping for VST-param columns */
    int  m_last_fire_row;

    /* one note cell gathered from the shared sequence */
    struct notecell { int note; int vel; long ts; long tf; };

    /* helpers ------------------------------------------------------------ */
    int  ticks_per_row( void );
    int  num_rows( void );
    int  visible_rows( void );
    long row_start_tick( int a_row );

    void collect_row_notes( int a_row, std::vector<notecell> &a_out );
    bool note_at_row( int a_row, int a_track, int *a_note, int *a_vel );

    void set_note_at_cell( int a_note );
    void set_velocity_at_cell( int a_vel );
    void clear_note_cell( void );
    void remove_specific_note( long a_ts, int a_note );

    /* FX cell helpers */
    int  cur_fx_index( void );                 /* fx column under cursor (>=0) */
    bool read_cc_at( int a_row, int a_cc, int *a_val );
    void set_cc_at( long a_ts, int a_cc, int a_val );
    void remove_cc_at( long a_ts, int a_cc );
    void set_fx_at_cell( int a_val );
    void clear_fx_cell( void );
    void fire_fx_row( int a_row );

    void subcol_geom( int a_col, int *a_x, int *a_w );
    int  total_subcols( void ) { return 2 + m_fx_cols; }

    std::string note_name( int a_note );
    int  key_to_pitch( unsigned int a_keyval, int *a_oct_offset );
    int  key_to_hex( unsigned int a_keyval );

    /* drawing */
    void draw_background( void );
    void draw_header( const Cairo::RefPtr<Cairo::Context> &cr );
    void draw_grid( void );
    void update_pixmap( void );
    void force_draw( void );

    void recalc_track_geometry( void );
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
    void fire_due_fx( void );
    void move_cursor( int a_drow, int a_dcol );

    void set_rows_per_beat( int a_rpb );
    int  get_rows_per_beat( void ) { return m_rows_per_beat; }
    void set_octave( int a_oct )   { m_octave = a_oct; }
    int  get_octave( void )        { return m_octave; }
    void set_edit_step( int a_s )  { m_edit_step = a_s; }
    int  get_edit_step( void )     { return m_edit_step; }

    void set_num_tracks( int a_n );
    int  get_num_tracks( void )    { return m_num_tracks; }

    /* FX binding (driven by the toolbar picker) -- targets the FX column
       currently under the cursor (or FX col 0 if the cursor is on note/vel). */
    int  get_vst_track( void ) { return (int) m_seq->get_midi_bus(); }
    void bind_fx_none( void );
    void bind_fx_cc( int a_cc, const std::string &a_label );
    void bind_fx_vst( unsigned int a_pid, const std::string &a_name );
    std::string cur_fx_desc( void );

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
    Gtk::Button *m_button_tracks;
    Gtk::Entry  *m_entry_tracks;
    Gtk::Button *m_button_octave;
    Gtk::Entry  *m_entry_octave;
    Gtk::Button *m_button_step;
    Gtk::Entry  *m_entry_step;
    Gtk::Button *m_button_fx;
    Gtk::Entry  *m_entry_fx;
    Gtk::Button *m_button_length;
    Gtk::Entry  *m_entry_length;

    Gtk::Menu   *m_menu_rpb;
    Gtk::Menu   *m_menu_fx;

    void set_rows_per_beat( int a_rpb );
    void change_octave( int a_delta );
    void change_step( int a_delta );
    void change_tracks( int a_delta );
    void popup_rpb_menu( void );
    void popup_fx_menu( void );
    void do_bind_cc( int a_cc, std::string a_label );
    void do_bind_vst( unsigned int a_pid, std::string a_name );
    void do_bind_none( void );
    void update_toolbar_entries( void );

    void on_realize();
    bool timeout( void );

 public:

    trackeredit( sequence *a_seq, perform *a_perf, int a_pos );
    ~trackeredit();

    bool on_delete_event( GdkEventAny *a_event );
};

#endif
