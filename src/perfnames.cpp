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
//  You should have received a copy of the GNU General Public License
//  along with seq24; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//-----------------------------------------------------------------------------
//
//  perfnames -- the left "track header" column of the arrangement editor.
//
//  Reworked into an Ardour/Ableton-style track-header strip.  Per active
//  track it shows:
//      * a left status "spine" (bright = audible, dim = muted)
//      * a track-type badge (INS = instrument/MIDI; a clear spot is left for
//        AUD = audio tracks, wired up later by the coordinator)
//      * the track name (double-click to rename) and bus/channel/time-sig info
//      * a small level / VU strip (drawn placeholder; live levels wired later
//        via audio_app_graph())
//      * Mute (M) and Solo (S) toggle buttons
//
//  Strictly black & white / greyscale (src/ui/palette.h, namespace synth),
//  seq24-style GDK drawing.
//
//-----------------------------------------------------------------------------
#include "perfnames.h"
#include "font.h"

#include <gtkmm/dialog.h>
#include <gtkmm/entry.h>
#include <gtkmm/stock.h>

#include <string.h>

/* ---- track-header layout (all within one c_names_y-tall row) ------------- */
static const int cN_spine_w = 4;                       /* left status spine   */
static const int cN_badge_x = cN_spine_w + 4;          /* type badge          */
static const int cN_badge_w = 20;
static const int cN_name_x  = cN_badge_x + cN_badge_w + 4;

static const int cN_btn_w   = 22;                      /* M / S buttons       */
static const int cN_btn_h   = 12;
static const int cN_btn_x   = c_names_x - cN_btn_w - 3;

static const int cN_vu_w    = 10;                      /* level / VU strip    */
static const int cN_vu_x    = cN_btn_x - cN_vu_w - 5;

static const int cN_m_y     = 3;                       /* M button top offset */
static const int cN_s_y     = cN_m_y + cN_btn_h + 2;   /* S button top offset */


perfnames::perfnames( perform *a_perf, Adjustment *a_vadjust ): DrawingArea(), seqmenu(a_perf)
{
    m_mainperf = a_perf;

    add_events( Gdk::BUTTON_PRESS_MASK |
		Gdk::BUTTON_RELEASE_MASK |
		Gdk::SCROLL_MASK );

    /* set default size */
    set_size_request( c_names_x, 100 );

    // in the construor you can only allocate colors,
    // get_window() returns 0 because we have not be realized
    Glib::RefPtr<Gdk::Colormap>  colormap= get_default_colormap();

    /* monochrome palette -- NO hues (src/ui/palette.h) */
    m_black   = synth::gdk_color( synth::cBg );      // window / lane background
    m_white   = synth::gdk_color( synth::cHi );      // near-white text / chrome
    m_grey    = synth::gdk_color( synth::cAccent );  // light grey accents
    m_dk_grey = synth::gdk_color( synth::cDim );     // mid grey lines / muted
    m_panel   = synth::gdk_color( synth::cPanel );   // dark grey (even rows)

    colormap->alloc_color( m_black );
    colormap->alloc_color( m_white );
    colormap->alloc_color( m_grey );
    colormap->alloc_color( m_dk_grey );
    colormap->alloc_color( m_panel );

    m_vadjust = a_vadjust;
    m_vadjust->signal_value_changed().connect( mem_fun( *(this), &perfnames::change_vert ));

    m_sequence_offset = 0;
    m_solo_active = false;

    set_double_buffered( false );

    for( int i=0; i<c_total_seqs; ++i )
    {
        m_sequence_active[i]=false;
        m_solo[i]=false;
        m_mute_snapshot[i]=false;
    }
}




void
perfnames::on_realize()
{
    // we need to do the default realize (chain the base FIRST so the GdkWindow
    // exists before we build any Gdk::GC -- required on the Windows backend)
    Gtk::DrawingArea::on_realize();

    // Now we can allocate any additional resources we need
    m_window = get_window();
    m_gc = Gdk::GC::create( m_window );
    m_window->clear();

    m_pixmap = Gdk::Pixmap::create(m_window,
                                   c_names_x,
                                   c_names_y  * c_total_seqs + 1,
                                   -1);
}


void
perfnames::change_vert( )
{
    if ( m_sequence_offset != (int) m_vadjust->get_value() ){

        m_sequence_offset = (int) m_vadjust->get_value();
        queue_draw();
    }
}

void
perfnames::update_pixmap()
{

}

void
perfnames::draw_area(){

}


void
perfnames::redraw( int sequence )
{
    draw_sequence( sequence);
}


/* draw one small utilitarian push-button (outline + centered label).
   engaged -> filled bright with dark label;  idle -> outline with grey label */
void
perfnames::draw_button( int a_x, int a_y, int a_w, int a_h,
                        const char *a_label, bool a_engaged )
{
    if ( a_engaged )
    {
        m_gc->set_foreground( m_white );
        m_window->draw_rectangle( m_gc, true, a_x, a_y, a_w, a_h );
    }
    else
    {
        m_gc->set_foreground( m_panel );
        m_window->draw_rectangle( m_gc, true, a_x, a_y, a_w, a_h );
    }

    m_gc->set_foreground( a_engaged ? m_white : m_dk_grey );
    m_window->draw_rectangle( m_gc, false, a_x, a_y, a_w, a_h );

    int len = strlen( a_label );
    int tx  = a_x + (a_w - len * 6) / 2;
    int ty  = a_y + (a_h - 8) / 2 - 1;
    char buf[8];
    strncpy( buf, a_label, sizeof(buf)-1 );
    buf[sizeof(buf)-1] = 0;
    p_font_renderer->render_string_on_drawable(m_gc, tx, ty, m_window,
                                               buf, a_engaged ? font::BLACK : font::WHITE );
}


void
perfnames::draw_sequence( int sequence )
{
    if ( sequence >= c_total_seqs )
        return;

    int i = sequence - m_sequence_offset;
    int y0 = c_names_y * i;

    /* row background: subtle alternating grey (DAW lane striping) */
    m_gc->set_foreground( (sequence % 2) == 0 ? m_panel : m_black );
    m_window->draw_rectangle( m_gc, true, 0, y0, c_names_x, c_names_y );

    /* row separator; brighter every screen-set (track group) boundary */
    m_gc->set_foreground( (sequence % c_seqs_in_set) == 0 ? m_grey : m_dk_grey );
    m_window->draw_line( m_gc, 0, y0 + c_names_y - 1, c_names_x, y0 + c_names_y - 1 );

    bool active = m_mainperf->is_active( sequence );

    if ( !active )
    {
        /* empty lane -- leave a faint track-number tab so the grid still reads */
        m_gc->set_foreground( m_dk_grey );
        char num[8];
        sprintf( num, "%d", sequence + 1 );
        p_font_renderer->render_string_on_drawable(m_gc,
                                                   cN_badge_x, y0 + (c_names_y-8)/2,
                                                   m_window, num, font::WHITE );
        return;
    }

    m_sequence_active[sequence] = true;

    /* 'sequence' (the int arg) shadows the class name here, so elaborate it */
    class sequence *seq = m_mainperf->get_sequence( sequence );
    bool muted = seq->get_song_mute();

    /* status spine: bright when audible, dim when muted */
    m_gc->set_foreground( muted ? m_dk_grey : m_white );
    m_window->draw_rectangle( m_gc, true, 0, y0 + 1, cN_spine_w, c_names_y - 2 );

    /* track-type badge -- everything is INSTR/MIDI for now.
       AUDIO tracks would draw "AUD" here (left as a clear hook). */
    m_gc->set_foreground( m_dk_grey );
    m_window->draw_rectangle( m_gc, false, cN_badge_x, y0 + 3, cN_badge_w, c_names_y - 7 );
    m_gc->set_foreground( m_grey );
    p_font_renderer->render_string_on_drawable(m_gc,
                                               cN_badge_x + 1, y0 + (c_names_y-8)/2,
                                               m_window, "INS", font::WHITE );

    /* name (line 1) */
    char name[64];
    sprintf( name, "%02d %-16.16s", sequence + 1, seq->get_name() );
    m_gc->set_foreground( m_white );
    p_font_renderer->render_string_on_drawable(m_gc,
                                               cN_name_x, y0 + 3,
                                               m_window, name, font::WHITE );

    /* bus / channel / time-sig (line 2) */
    char info[48];
    sprintf( info, "b%d ch%d  %ld/%ld",
             seq->get_midi_bus(),
             seq->get_midi_channel() + 1,
             seq->get_bpm(),
             seq->get_bw() );
    m_gc->set_foreground( m_grey );
    p_font_renderer->render_string_on_drawable(m_gc,
                                               cN_name_x, y0 + 15,
                                               m_window, info, font::WHITE );

    /* level / VU strip -- drawn placeholder (coordinator wires live levels
       later via audio_app_graph()).  A few static segments in greyscale. */
    int vu_y = y0 + 3;
    int vu_h = c_names_y - 7;
    m_gc->set_foreground( m_dk_grey );
    m_window->draw_rectangle( m_gc, false, cN_vu_x, vu_y, cN_vu_w, vu_h );
    if ( !muted )
    {
        int seg_gap = 3;
        int filled  = vu_h * 2 / 3;   /* static placeholder level */
        for ( int yy = vu_y + vu_h - 2; yy > vu_y + vu_h - filled; yy -= seg_gap )
        {
            m_gc->set_foreground( (yy < vu_y + vu_h/3) ? m_white : m_grey );
            m_window->draw_line( m_gc, cN_vu_x + 2, yy, cN_vu_x + cN_vu_w - 2, yy );
        }
    }

    /* Mute + Solo toggle buttons */
    draw_button( cN_btn_x, y0 + cN_m_y, cN_btn_w, cN_btn_h, "M", muted );
    draw_button( cN_btn_x, y0 + cN_s_y, cN_btn_w, cN_btn_h, "S", m_solo[sequence] );
}



bool
perfnames::on_expose_event(GdkEventExpose* a_e)
{
    int seqs = (m_window_y / c_names_y) + 1;

    for ( int i=0; i< seqs; i++ ){

	int sequence = i + m_sequence_offset;

        draw_sequence(sequence);

    }
    return true;
}


void
perfnames::convert_y( int a_y, int *a_seq)
{
    *a_seq = a_y / c_names_y;
    *a_seq  += m_sequence_offset;

    if ( *a_seq >= c_total_seqs )
	*a_seq = c_total_seqs - 1;

    if ( *a_seq < 0 )
	*a_seq = 0;
}


/* engage / release solo across all active tracks.  Snapshots the song-mute
   state on entering solo mode and restores it when the last solo is cleared,
   so a user's manual mutes survive a solo pass. */
void
perfnames::apply_solo( void )
{
    bool any = false;
    for ( int t=0; t<c_total_seqs; t++ )
        if ( m_mainperf->is_active(t) && m_solo[t] ) { any = true; break; }

    if ( any )
    {
        if ( !m_solo_active )
        {
            for ( int t=0; t<c_total_seqs; t++ )
                if ( m_mainperf->is_active(t) )
                    m_mute_snapshot[t] = m_mainperf->get_sequence(t)->get_song_mute();
            m_solo_active = true;
        }
        for ( int t=0; t<c_total_seqs; t++ )
            if ( m_mainperf->is_active(t) )
                m_mainperf->get_sequence(t)->set_song_mute( !m_solo[t] );
    }
    else if ( m_solo_active )
    {
        for ( int t=0; t<c_total_seqs; t++ )
            if ( m_mainperf->is_active(t) )
                m_mainperf->get_sequence(t)->set_song_mute( m_mute_snapshot[t] );
        m_solo_active = false;
    }

    queue_draw();
}


/* simple modal rename of a track's pattern name */
void
perfnames::rename_seq( int a_seq )
{
    if ( ! m_mainperf->is_active( a_seq ) )
        return;

    Gtk::Dialog dialog( "Rename Track", true );
    dialog.add_button( Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL );
    dialog.add_button( Gtk::Stock::OK, Gtk::RESPONSE_OK );
    dialog.set_default_response( Gtk::RESPONSE_OK );

    Gtk::Entry entry;
    entry.set_text( m_mainperf->get_sequence( a_seq )->get_name() );
    entry.set_activates_default( true );
    dialog.get_vbox()->pack_start( entry, false, false, 4 );
    dialog.show_all_children();

    if ( dialog.run() == Gtk::RESPONSE_OK )
    {
        std::string text = entry.get_text();
        m_mainperf->get_sequence( a_seq )->set_name( text );
        queue_draw();
    }
}


bool
perfnames::on_button_press_event(GdkEventButton *a_e)
{
    int sequence;

    int x = (int) a_e->x;
    int y = (int) a_e->y;

    convert_y( y, &sequence );

    m_current_seq = sequence;

    int row   = y / c_names_y;
    int ly    = y - row * c_names_y;   /* local y within the row */

    /*      left mouse button     */
    if ( a_e->button == 1 && m_mainperf->is_active( sequence ) )
    {
        /* Mute button */
        if ( x >= cN_btn_x && x <= cN_btn_x + cN_btn_w &&
             ly >= cN_m_y  && ly <= cN_m_y + cN_btn_h )
        {
            bool muted = m_mainperf->get_sequence(sequence)->get_song_mute();
            m_mainperf->get_sequence(sequence)->set_song_mute( !muted );
            queue_draw();
            return true;
        }

        /* Solo button */
        if ( x >= cN_btn_x && x <= cN_btn_x + cN_btn_w &&
             ly >= cN_s_y  && ly <= cN_s_y + cN_btn_h )
        {
            m_solo[sequence] = ! m_solo[sequence];
            apply_solo();
            return true;
        }

        /* double-click the name area -> rename */
        if ( a_e->type == GDK_2BUTTON_PRESS && x >= cN_name_x && x < cN_vu_x )
        {
            rename_seq( sequence );
            return true;
        }

        /* click on the spine still toggles mute (quick DAW gesture) */
        if ( x < cN_name_x && a_e->type == GDK_BUTTON_PRESS )
        {
            bool muted = m_mainperf->get_sequence(sequence)->get_song_mute();
            m_mainperf->get_sequence(sequence)->set_song_mute( !muted );
            queue_draw();
            return true;
        }
    }

    return true;
}


bool
perfnames::on_button_release_event(GdkEventButton* p0)
{
    /*     right mouse button      */
    if ( p0->button == 3 ){
        popup_menu();
    }

    return false;
}

bool
perfnames::on_scroll_event( GdkEventScroll* a_ev )
{
	double val = m_vadjust->get_value();

    if (  a_ev->direction == GDK_SCROLL_UP ){
		val -= m_vadjust->get_step_increment();
    }
    if (  a_ev->direction == GDK_SCROLL_DOWN ){
		val += m_vadjust->get_step_increment();
    }

	m_vadjust->clamp_page( val, val + m_vadjust->get_page_size());
    return true;
}


void
perfnames::on_size_allocate(Gtk::Allocation &a_r )
{
    Gtk::DrawingArea::on_size_allocate( a_r );

    m_window_x = a_r.get_width();
    m_window_y = a_r.get_height();
}



void
perfnames::redraw_dirty_sequences( void )
{
    if ( ! is_realized() )
        return;

    int y_s = 0;
    int y_f = m_window_y / c_names_y;

    for ( int y=y_s; y<=y_f; y++ ){

        int seq = y + m_sequence_offset; // 4am

        if ( seq < c_total_seqs){

                bool dirty = (m_mainperf->is_dirty_names( seq ));

                if (dirty)
                {
                    draw_sequence( seq );
                }
        }
    }
}
