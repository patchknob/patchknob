//----------------------------------------------------------------------------
//
//  This file is part of seq24.
//
//  seq24 is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free So%ftware Foundation; either version 2 of the License, or
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
#include "event.h"
#include "seqroll.h"


seqroll::seqroll(perform *a_perf,
                 sequence *a_seq, 
                 int a_zoom, 
                 int a_snap, 
                 seqdata *a_seqdata_wid, 
                 seqevent *a_seqevent_wid,
                 seqkeys *a_seqkeys_wid,
                 int a_pos, 
                 Adjustment *a_hadjust,
                 Adjustment *a_vadjust )
: DrawingArea() 
{    
    using namespace Menu_Helpers;

    Glib::RefPtr<Gdk::Colormap> colormap = get_default_colormap();

    /* synthwave palette (1111 reskin); names kept for the legacy GDK
       on-window drawing paths (selection / paste / grow rects, playhead). */
    m_black   = synth::gdk_color( synth::cBg );      // background
    m_white   = synth::gdk_color( synth::cHi );      // light chrome
    m_grey    = synth::gdk_color( synth::cAccent );  // selection chrome (violet)
    m_dk_grey = synth::gdk_color( synth::cDim );     // muted lines
    m_red     = synth::gdk_color( synth::cActive );  // playhead (green)

    colormap->alloc_color( m_black );
    colormap->alloc_color( m_white );
    colormap->alloc_color( m_grey );
    colormap->alloc_color( m_dk_grey );
    colormap->alloc_color( m_red );

    m_perform = a_perf;
    m_seq =   a_seq;
    m_zoom = a_zoom;
    m_snap =  a_snap;
    m_seqdata_wid = a_seqdata_wid;
    m_seqevent_wid = a_seqevent_wid;
    m_seqkeys_wid = a_seqkeys_wid;
    m_pos = a_pos;

    m_clipboard = new sequence( );

    add_events( Gdk::BUTTON_PRESS_MASK | 
		Gdk::BUTTON_RELEASE_MASK |
		Gdk::POINTER_MOTION_MASK |
		Gdk::KEY_PRESS_MASK |
		Gdk::KEY_RELEASE_MASK |
		Gdk::FOCUS_CHANGE_MASK |
		Gdk::ENTER_NOTIFY_MASK |
		Gdk::LEAVE_NOTIFY_MASK |
		Gdk::SCROLL_MASK);

    m_selecting = false;
    m_moving    = false;
	m_moving_init = false;
    m_growing   = false;
    m_adding    = false;
    m_painting  = false;
    m_paste     = false;

    m_old_progress_x = 0;

    m_scale = 0;
    m_key = 0;

    m_vadjust = a_vadjust;
    m_hadjust = a_hadjust;

    m_window_x = 10;
    m_window_y = 10;

    m_scroll_offset_ticks = 0;
    m_scroll_offset_key = 0;

    m_scroll_offset_x = 0;
    m_scroll_offset_y = 0;

    m_background_sequence = 0;
    m_drawing_background_seq = false;

    set_double_buffered( false );
}


void
seqroll::set_background_sequence( bool a_state, int a_seq )
{
    m_drawing_background_seq = a_state;
    m_background_sequence = a_seq;

    update_pixmap();
    queue_draw();
}



seqroll::~seqroll( )
{
	delete m_clipboard;
}

/* popup menu calls this */
void 
seqroll::set_adding( bool a_adding )
{
    if ( a_adding ){

	get_window()->set_cursor(  Gdk::Cursor( Gdk::PENCIL ));
	m_adding = true;
    
    } else {

	get_window()->set_cursor( Gdk::Cursor( Gdk::LEFT_PTR ));
	m_adding = false;
    }
}




void 
seqroll::on_realize()
{
    // we need to do the default realize
    Gtk::DrawingArea::on_realize();
    
    set_flags( Gtk::CAN_FOCUS );

    // Now we can allocate any additional resources we need
    m_window = get_window();
    m_gc = Gdk::GC::create( m_window );
    m_window->clear();

    m_hadjust->signal_value_changed().connect( mem_fun( *this, &seqroll::change_horz ));
    m_vadjust->signal_value_changed().connect( mem_fun( *this, &seqroll::change_vert ));

    update_sizes();
}

/*

use m_zoom and

i % m_seq->get_bpm() == 0

 int numberLines = 128 / m_seq->get_bw() / m_zoom; 
    int distance = c_ppqn / 32;
*/

void 
seqroll::update_sizes()
{
    /* set default size */


    m_hadjust->set_lower( 0 );
    m_hadjust->set_upper( m_seq->get_length() );
    m_hadjust->set_page_size( m_window_x * m_zoom );
    m_hadjust->set_step_increment( c_ppqn / 4 );
    m_hadjust->set_page_increment( c_ppqn / 4 );

    int h_max_value = ( m_seq->get_length() - (m_window_x * m_zoom));
    
    if ( m_hadjust->get_value() > h_max_value ){
        m_hadjust->set_value( h_max_value );
    }


    
    m_vadjust->set_lower( 0 );
    m_vadjust->set_upper( c_num_keys ); 
    m_vadjust->set_page_size( m_window_y / c_key_y );
    m_vadjust->set_step_increment( 12 );
    m_vadjust->set_page_increment( 12 );

    int v_max_value = c_num_keys - (m_window_y / c_key_y);

    if ( m_vadjust->get_value() > v_max_value ){
        m_vadjust->set_value(v_max_value);
    }
    
    /* create pixmaps with window dimentions */
    if( is_realized() ) {
        
        m_pixmap = Gdk::Pixmap::create( m_window,
                                        m_window_x,
                                        m_window_y,
                                        -1);
        change_vert();
    }
}


void
seqroll::change_horz( )
{
    m_scroll_offset_ticks = (int) m_hadjust->get_value();
    m_scroll_offset_x = m_scroll_offset_ticks / m_zoom;

    update_pixmap();
    force_draw();    
}

void
seqroll::change_vert( )
{
    m_scroll_offset_key = (int) m_vadjust->get_value();
    m_scroll_offset_y = m_scroll_offset_key * c_key_y;

    update_pixmap();
    force_draw();
    
}

/* basically resets the whole widget as if it was realized again */
void 
seqroll::reset()
{
    m_scroll_offset_ticks = (int) m_hadjust->get_value();
    m_scroll_offset_x = m_scroll_offset_ticks / m_zoom;
    
    update_sizes();
    update_pixmap();
    queue_draw();
}

void
seqroll::redraw()
{
    m_scroll_offset_ticks = (int) m_hadjust->get_value();
    m_scroll_offset_x = m_scroll_offset_ticks / m_zoom;
    
    update_pixmap();
    queue_draw();
}



/* updates background -- Cairo synthwave grid (1111 reskin) */
void
seqroll::draw_background()
{
    Cairo::RefPtr<Cairo::Context> cr = m_pixmap->create_cairo_context();
    cr->set_line_width( 1.0 );

    /* clear background to near-black */
    synth::set_source( cr, synth::cBg );
    cr->rectangle( 0, 0, m_window_x, m_window_y );
    cr->fill();

    /* ---- horizontal row striping + octave separators --------------------- *
       Rows are chromatic keys (c_key_y px each).  We stripe by note-in-octave
       parity for the 1111 "alternating degree" feel, tint out-of-scale rows,
       and draw a brighter separator line at each octave boundary (note C).    */
    int rows = (m_window_y / c_key_y) + 1;
    for ( int i = 0; i < rows; i++ )
    {
        /* the actual MIDI note for this screen row */
        int note = (c_num_keys - 1) - ( i + m_scroll_offset_key );
        int key  = ((note % 12) + 12) % 12;

        double y = i * c_key_y;

        /* base stripe by octave parity (subtle) */
        bool even_oct = (((note / 12)) % 2) == 0;
        synth::set_source( cr, even_oct ? synth::cPanel : synth::cBg );
        cr->rectangle( 0, y + 1, m_window_x, c_key_y - 1 );
        cr->fill();

        /* shade out-of-scale rows darker, matching seq24 scale overlay */
        if ( m_scale != c_scale_off )
        {
            if ( !c_scales_policy[m_scale][ ((c_num_keys - i)
                                             - m_scroll_offset_key
                                             - 1 + ( 12 - m_key )) % 12] )
            {
                synth::set_source( cr, synth::cBg, 0.55 );
                cr->rectangle( 0, y + 1, m_window_x, c_key_y - 1 );
                cr->fill();
            }
        }

        /* octave separator: brighter line below each C */
        if ( key == 0 )
        {
            synth::set_source( cr, synth::cDim, 0.55 );
            cr->move_to( 0, y + 0.5 );
            cr->line_to( m_window_x, y + 0.5 );
            cr->stroke();
        }
        else
        {
            /* faint divider between every row */
            synth::set_source( cr, synth::cDim, 0.12 );
            cr->move_to( 0, y + 0.5 );
            cr->line_to( m_window_x, y + 0.5 );
            cr->stroke();
        }
    }

    /* ---- vertical beat lines with graded alpha / thickness --------------- */
    int ticks_per_measure =  m_seq->get_bpm() * (4 * c_ppqn) / m_seq->get_bw();
    int ticks_per_beat    =  (4 * c_ppqn) / m_seq->get_bw();
    int ticks_per_half    =  ticks_per_beat / 2;
    int ticks_per_step    =  6 * m_zoom;
    if ( ticks_per_step < 1 ) ticks_per_step = 1;

    int start_tick = m_scroll_offset_ticks - (m_scroll_offset_ticks % ticks_per_step );
    int end_tick   = (m_window_x * m_zoom) + m_scroll_offset_ticks;

    for ( int i = start_tick; i < end_tick; i += ticks_per_step )
    {
        double x = (double)(i / m_zoom) - m_scroll_offset_x + 0.5;

        bool is_bar  = (i % ticks_per_measure) == 0;
        bool is_beat = (i % ticks_per_beat)    == 0;
        bool is_half = (i % ticks_per_half)    == 0;

        double alpha = is_bar  ? 0.90
                     : is_beat ? 0.50
                     : is_half ? 0.22
                     :           0.10;
        double thick = is_bar ? 1.6 : (is_beat ? 1.0 : 0.75);

        synth::set_source( cr, is_bar ? synth::cAccent : synth::cDim, alpha );
        cr->set_line_width( thick );
        cr->move_to( x, 0 );
        cr->line_to( x, m_window_y );
        cr->stroke();
    }
    cr->set_line_width( 1.0 );
}

/* sets zoom, resets */
void 
seqroll::set_zoom( int a_zoom )
{
    if ( m_zoom != a_zoom ){

        m_zoom = a_zoom;
        reset();
    }
}

/* simply ses the snap member */
void 
seqroll::set_snap( int a_snap )
{
    m_snap = a_snap;
    reset();
}


void 
seqroll::set_note_length( int a_note_length )
{
	m_note_length = a_note_length;
}

/* sets the music scale */
void 
seqroll::set_scale( int a_scale )
{
  if ( m_scale != a_scale ){
    m_scale = a_scale;
    reset();
  }

}

/* sets the key */
void 
seqroll::set_key( int a_key )
{
  if ( m_key != a_key ){
    m_key = a_key;
    reset();
  }
}



/* draws background pixmap on main pixmap,
   then puts the events on */
void 
seqroll::update_pixmap()
{
    draw_background();
    draw_events_on_pixmap();
}


void 
seqroll::draw_progress_on_window()
{	
    m_window->draw_drawable(m_gc, 
                            m_pixmap, 
                            m_old_progress_x,
                            0,
                            m_old_progress_x,
                            0,
                            1,
                            m_window_y );
	
	m_old_progress_x = (m_seq->get_last_tick() / m_zoom) - m_scroll_offset_x;

	if ( m_old_progress_x != 0 ){

	    /* 1111 playhead: 2px green glowing line */
	    Cairo::RefPtr<Cairo::Context> cr = m_window->create_cairo_context();
	    synth::set_source( cr, synth::cActive, 0.85 );
	    cr->set_line_width( 2.0 );
	    cr->move_to( m_old_progress_x + 0.5, 0 );
	    cr->line_to( m_old_progress_x + 0.5, m_window_y );
	    cr->stroke();
	}
}



void seqroll::draw_events_on( Glib::RefPtr<Gdk::Drawable> a_draw )
{
    long tick_s;
    long tick_f;
    int note;

    int note_x;
    int note_width;
    int note_y;
    int note_height;

    bool selected;

    int velocity;

    draw_type dt;

    int start_tick = m_scroll_offset_ticks ;
    int end_tick = (m_window_x * m_zoom) + m_scroll_offset_ticks;

    Cairo::RefPtr<Cairo::Context> cr = a_draw->create_cairo_context();
    cr->set_line_width( 1.0 );

    sequence *seq = NULL;
    for( int method=0; method<2; ++method ){

        if ( method == 0 && m_drawing_background_seq  ){

            if ( m_perform->is_active( m_background_sequence )){
                seq =m_perform->get_sequence( m_background_sequence );
            }
            else{
                method++;
            }
        }
        else if ( method == 0 ){
            method++;
        }

        if ( method==1){
            seq = m_seq;
        }

        seq->reset_draw_marker();

        while ( (dt = seq->get_next_note_event( &tick_s, &tick_f, &note,
                                                &selected, &velocity )) != DRAW_FIN ){

            if ( (tick_s >= start_tick && tick_s <= end_tick) ||
                 ((dt == DRAW_NORMAL_LINKED) &&
                  (tick_f >= start_tick && tick_f <= end_tick))){

                /* turn into screen corrids */
                note_x = tick_s / m_zoom;
                note_y = c_rollarea_y -(note * c_key_y) - c_key_y - 1 + 2;
                note_height = c_key_y - 3;

                if ( dt == DRAW_NORMAL_LINKED ){
                    note_width = (tick_f - tick_s) / m_zoom;
                    if ( note_width < 1 ) note_width = 1;
                }
                else {
                    note_width = 8 / m_zoom;
                    if ( note_width < 1 ) note_width = 1;
                }

                note_x -= m_scroll_offset_x;
                note_y -= m_scroll_offset_y;

                double nx = note_x;
                double ny = note_y;
                double nw = note_width;
                double nh = note_height;

                /* body fill: background seq = dim, selected = cyan, else violet */
                unsigned int body = (method == 0) ? synth::cDim
                                  : (selected     ? synth::cNoteSel
                                                  : synth::cNote);

                double radius = (nh >= 5.0 && nw >= 5.0) ? 2.0 : 0.0;
                synth::rounded_rect( cr, nx, ny, nw, nh, radius );
                synth::set_source( cr, body, (method == 0) ? 0.55 : 1.0 );
                cr->fill();

                if ( method == 1 && nw > 2.0 ){

                    /* 1px translucent white outline */
                    synth::rounded_rect( cr, nx + 0.5, ny + 0.5,
                                         nw - 1.0, nh - 1.0, radius );
                    synth::set_source( cr, synth::cWhite, 0.25 );
                    cr->set_line_width( 1.0 );
                    cr->stroke();

                    /* right-edge resize handle hint */
                    if ( nw >= 6.0 ){
                        synth::set_source( cr, synth::cWhite, 0.45 );
                        cr->rectangle( nx + nw - 3.0, ny + 1.0,
                                       2.0, nh - 2.0 );
                        cr->fill();
                    }
                }
            }
        }
    }
}



/* fills main pixmap with events */
void 
seqroll::draw_events_on_pixmap()
{
    draw_events_on( m_pixmap ); 
}


int 
seqroll::idle_redraw()
{
    draw_events_on( m_window );
    draw_events_on( m_pixmap );
  
    return true;
}



void 
seqroll::draw_selection_on_window()
{
    int x,y,w,h;

    
    if ( m_selecting  ||  m_moving || m_paste ||  m_growing ){
        
        m_gc->set_line_attributes( 1,
                                   Gdk::LINE_SOLID,
                                   Gdk::CAP_NOT_LAST,
                                   Gdk::JOIN_MITER );
        
        /* replace old */
        m_window->draw_drawable(m_gc, 
                                m_pixmap, 
                                m_old.x,
                                m_old.y,
                                m_old.x,
                                m_old.y,
                                m_old.width + 1,
                                m_old.height + 1 );
    }
    
    if ( m_selecting ){
        
        xy_to_rect ( m_drop_x,
                     m_drop_y,
                     m_current_x,
                     m_current_y,
                     &x, &y,
                     &w, &h );
        
        x -= m_scroll_offset_x;
        y -= m_scroll_offset_y;
        
        m_old.x = x;
        m_old.y = y;
        m_old.width = w;
        m_old.height = h + c_key_y;
        
        m_gc->set_foreground(m_grey);
        m_window->draw_rectangle(m_gc,false,
                                 x,
                                 y,
                                 w,
                                 h + c_key_y );
    }
    
    if ( m_moving || m_paste ){
        
        int delta_x = m_current_x - m_drop_x;
        int delta_y = m_current_y - m_drop_y;
        
        x = m_selected.x + delta_x;
        y = m_selected.y + delta_y;

        x -= m_scroll_offset_x;
        y -= m_scroll_offset_y;
        
        m_gc->set_foreground(m_white);
        m_window->draw_rectangle(m_gc,false,
                                 x,
                                 y,
                                 m_selected.width,
                                 m_selected.height );
        m_old.x = x;
        m_old.y = y;
        m_old.width = m_selected.width;
        m_old.height = m_selected.height;
    }
    
    if ( m_growing ){
        
        int delta_x = m_current_x - m_drop_x;
        int width = delta_x + m_selected.width;
        
        if ( width < 1 ) 
            width = 1;

        x = m_selected.x;
        y = m_selected.y;

        x -= m_scroll_offset_x;
        y -= m_scroll_offset_y;
        
        m_gc->set_foreground(m_white);
        m_window->draw_rectangle(m_gc,false,
                                 x,
                                 y,
                                 width,
                                 m_selected.height );

        m_old.x = x;
        m_old.y = y;
        m_old.width = width;
        m_old.height = m_selected.height;
        
    }
    
}

bool
seqroll::on_expose_event(GdkEventExpose* e)
{
  
    m_window->draw_drawable(m_gc, 
                            m_pixmap, 
                            e->area.x,
                            e->area.y,
                            e->area.x,
                            e->area.y,
                            e->area.width,
                            e->area.height );

    draw_selection_on_window();
    return true;
}


void
seqroll::force_draw(void )
{
    m_window->draw_drawable(m_gc, 
                            m_pixmap, 
                            0,
                            0,
                            0,
                            0,
                            m_window_x,
                            m_window_y );

    draw_selection_on_window();
}



/* takes screen corrdinates, give us notes and ticks */
void 
seqroll::convert_xy( int a_x, int a_y, long *a_tick, int *a_note)
{
    *a_tick = a_x * m_zoom; 
    *a_note = (c_rollarea_y - a_y - 2) / c_key_y; 
}


/* notes and ticks to screen corridinates */
void 
seqroll::convert_tn( long a_ticks, int a_note, int *a_x, int *a_y)
{
    *a_x = a_ticks /  m_zoom;
    *a_y = c_rollarea_y - ((a_note + 1) * c_key_y) - 1;
}



/* checks mins / maxes..  the fills in x,y
   and width and height */
void 
seqroll::xy_to_rect( int a_x1,  int a_y1,
		     int a_x2,  int a_y2,
		     int *a_x,  int *a_y,
		     int *a_w,  int *a_h )
{
    if ( a_x1 < a_x2 ){
	*a_x = a_x1; 
	*a_w = a_x2 - a_x1;
    } else {
	*a_x = a_x2; 
	*a_w = a_x1 - a_x2;
    }
    
    if ( a_y1 < a_y2 ){
	*a_y = a_y1; 
	*a_h = a_y2 - a_y1;
    } else {
	*a_y = a_y2; 
	*a_h = a_y1 - a_y2;
    }
}

void
seqroll::convert_tn_box_to_rect( long a_tick_s, long a_tick_f,
				 int a_note_h, int a_note_l,
				 int *a_x, int *a_y, 
				 int *a_w, int *a_h )
{
    int x1, y1, x2, y2;

    /* convert box to X,Y values */
    convert_tn( a_tick_s, a_note_h, &x1, &y1 );
    convert_tn( a_tick_f, a_note_l, &x2, &y2 );

    xy_to_rect( x1, y1, x2, y2, a_x, a_y, a_w, a_h );

    *a_h += c_key_y;
}



void
seqroll::start_paste( )
{
     long tick_s;
     long tick_f;
     int note_h;
     int note_l;

     snap_x( &m_current_x );
     snap_y( &m_current_x );

     m_drop_x = m_current_x;
     m_drop_y = m_current_y;

     m_paste = true;

     /* get the box that selected elements are in */
     m_seq->get_clipboard_box( &tick_s, &note_h, 
			       &tick_f, &note_l );

     convert_tn_box_to_rect( tick_s, tick_f, note_h, note_l,
			     &m_selected.x,
			     &m_selected.y,
			     &m_selected.width,
			     &m_selected.height );

     /* adjust for clipboard being shifted to tick 0 */
     m_selected.x += m_drop_x;
     m_selected.y += (m_drop_y - m_selected.y);
}
	

bool
seqroll::on_button_press_event(GdkEventButton* a_ev)
{
    int numsel;
	
    long tick_s;
    long tick_f;
    int note_h;
    int note_l;
	
    int norm_x, norm_y, snapped_x, snapped_y;
	
    grab_focus(  );

    bool needs_update = false;
	
    snapped_x = norm_x = (int) (a_ev->x + m_scroll_offset_x );
    snapped_y = norm_y = (int) (a_ev->y + m_scroll_offset_y );
	
    snap_x( &snapped_x );
    snap_y( &snapped_y );
	
    /* y is always snapped */
    m_current_y = m_drop_y = snapped_y;

    /* reset box that holds dirty redraw spot */
    m_old.x = 0;
    m_old.y = 0;
    m_old.width = 0;
    m_old.height = 0;

    if ( m_paste ){

        convert_xy( snapped_x, snapped_y, &tick_s, &note_h );
        m_paste = false;
        m_seq->push_undo();
        m_seq->paste_selected( tick_s, note_h );

        needs_update = true;

    } else { 

        /*  left mouse button     */
        if ( a_ev->button == 1 ||
             a_ev->button == 2 )
        { 

            /* selection, normal x */
            m_current_x = m_drop_x = norm_x;

            /* turn x,y in to tick/note */
            convert_xy( m_drop_x, m_drop_y, &tick_s, &note_h );

            if ( m_adding )
            {
                /* start the paint job */
                m_painting = true;

                /* adding, snapped x */
                m_current_x = m_drop_x = snapped_x;
                convert_xy( m_drop_x, m_drop_y, &tick_s, &note_h );

                // test if a note is already there
                // fake select, if so, no add
                if ( ! m_seq->select_note_events( tick_s, note_h, 
                                                  tick_s, note_h,
                                                  sequence::e_would_select ))
                {
                
                    /* add note, length = little less than snap */
                    m_seq->push_undo();
                    m_seq->add_note( tick_s, m_note_length - 2, note_h, true );

                    needs_update = true;
                }

            }
            else /* selecting */
            {

               
                if ( !m_seq->select_note_events( tick_s, note_h,
                                                tick_s, note_h, 
                                                sequence::e_is_selected ))
                {
                    if ( ! (a_ev->state & GDK_CONTROL_MASK) )
                    {
                        m_seq->unselect();	    
                    }
 

                    /* on direct click select only one event */
                    numsel = m_seq->select_note_events( tick_s,note_h,tick_s,note_h, 
                                                        sequence::e_select_one );

                    /* none selected, start selection box */
                    if ( numsel == 0 )
                    {
                        if ( a_ev->button == 1 )
                            m_selecting = true;
                    }
                    else
                    {
                        needs_update = true;
                    }
                }
 
                
                if ( m_seq->select_note_events( tick_s, note_h,
                                                tick_s, note_h, 
                                                sequence::e_is_selected ))
                {
                    if ( a_ev->button == 1 )
                    {
                        m_moving_init = true;
                        needs_update = true;


                        /* get the box that selected elements are in */
                        m_seq->get_selected_box( &tick_s, &note_h, 
                                &tick_f, &note_l );


                        convert_tn_box_to_rect( tick_s, tick_f, note_h, note_l,
                                &m_selected.x,
                                &m_selected.y,
                                &m_selected.width,
                                &m_selected.height );

                        /* save offset that we get from the snap above */
                        int adjusted_selected_x = m_selected.x;
                        snap_x( &adjusted_selected_x );
                        m_move_snap_offset_x = ( m_selected.x - adjusted_selected_x);

                        /* align selection for drawing */
                        snap_x( &m_selected.x );

                        m_current_x = m_drop_x = snapped_x;
                    }

                    /* middle mouse button  */
                    if ( a_ev->button == 2 ){	

                        /* moving, normal x */
                        //m_current_x = m_drop_x = norm_x;
                        //convert_xy( m_drop_x, m_drop_y, &tick_s, &note_h );

                        m_growing = true;

                        /* get the box that selected elements are in */
                        m_seq->get_selected_box( &tick_s, &note_h, 
                                    &tick_f, &note_l );

                        convert_tn_box_to_rect( tick_s, tick_f, note_h, note_l,
                                &m_selected.x,
                                &m_selected.y,
                                &m_selected.width,
                                &m_selected.height );	
                        
                    }
                }
           }
       }

        /*     right mouse button      */
        if ( a_ev->button == 3 ){
            set_adding( true );
        }

   }

    /* if they clicked, something changed */
    if ( needs_update ){
        redraw();
        m_seq->set_dirty();
    }
    return true;

}


    bool
seqroll::on_button_release_event(GdkEventButton* a_ev)
{
    long tick_s;
    long tick_f;
    int note_h;
    int note_l;
    int x,y,w,h;
    int numsel;

    bool needs_update = false;

    m_current_x = (int) (a_ev->x + m_scroll_offset_x );
    m_current_y = (int) (a_ev->y + m_scroll_offset_y );

    snap_y ( &m_current_y );

    if ( m_moving )
        snap_x( &m_current_x );

    int delta_x = m_current_x - m_drop_x;
    int delta_y = m_current_y - m_drop_y;

    long delta_tick;
    int delta_note;

    if ( a_ev->button == 1 ){

        if ( m_selecting ){

            xy_to_rect ( m_drop_x,
                    m_drop_y,
                    m_current_x,
                    m_current_y,
                    &x, &y,
                    &w, &h );

            convert_xy( x,     y, &tick_s, &note_h );
            convert_xy( x+w, y+h, &tick_f, &note_l );

            numsel = m_seq->select_note_events( tick_s, note_h, tick_f, note_l, sequence::e_select );

            needs_update = true;
        }

        if (  m_moving  ){

            /* adjust for snap */
            delta_x -= m_move_snap_offset_x;

            /* convert deltas into screen corridinates */
            convert_xy( delta_x, delta_y, &delta_tick, &delta_note );

            /* since delta_note was from delta_y, it will be filpped
               ( delta_y[0] = note[127], etc.,so we have to adjust */
            delta_note = delta_note - (c_num_keys-1);

            m_seq->push_undo();
            m_seq->move_selected_notes( delta_tick, delta_note );
            needs_update = true;
        }

   }

    if ( a_ev->button == 2 ){	

        if ( m_growing ){

            /* convert deltas into screen corridinates */
            convert_xy( delta_x, delta_y, &delta_tick, &delta_note );
            m_seq->push_undo();

            if ( a_ev->state & GDK_SHIFT_MASK )
            {
                m_seq->stretch_selected( delta_tick );
            }
            else
            {
                m_seq->grow_selected( delta_tick );
            }

            needs_update = true;
        }
    }

    if ( a_ev->button == 3 ){	
        set_adding( false );
    }

    /* turn off */
    m_selecting = false;
    m_moving = false;
    m_growing = false;
    m_paste = false;
    m_moving_init = false;
    m_painting = false;

    m_seq->unpaint_all();
 
    /* if they clicked, something changed */
    if (  needs_update ){

        redraw();
        m_seq->set_dirty();
    }
    return true;
}

 

bool
seqroll::on_motion_notify_event(GdkEventMotion* a_ev)
{
    m_current_x = (int) (a_ev->x  + m_scroll_offset_x );
    m_current_y = (int) (a_ev->y  + m_scroll_offset_y );

    int note;
    long tick;

    if ( m_moving_init ){
        m_moving_init = false;
        m_moving = true;
    }
		 
    
    snap_y( &m_current_y );
    convert_xy( 0, m_current_y, &tick, &note );
	
    m_seqkeys_wid->set_hint_key( note );
	
    if ( m_selecting || m_moving || m_growing || m_paste ){
        
        if ( m_moving || m_paste ){
            snap_x( &m_current_x );
        }
        
        draw_selection_on_window();
        return true;
	
    }

    if ( m_painting )
    {
        snap_x( &m_current_x );
        convert_xy( m_current_x, m_current_y, &tick, &note );

        m_seq->add_note( tick, m_note_length - 2, note, true );
    }
    
    return false;
}



/* performs a 'snap' on y */
void 
seqroll::snap_y( int *a_y )
{
    *a_y = *a_y - (*a_y % c_key_y);
}

/* performs a 'snap' on x */
void 
seqroll::snap_x( int *a_x )
{
    //snap = number pulses to snap to
    //m_zoom = number of pulses per pixel
    //so snap / m_zoom  = number pixels to snap to
    int mod = (m_snap / m_zoom);
    if ( mod <= 0 )
        mod = 1;
    
    *a_x = *a_x - (*a_x % mod );
}




bool
seqroll::on_enter_notify_event(GdkEventCrossing* a_p0)
{
  m_seqkeys_wid->set_hint_state( true );
  return false;
}



bool
seqroll::on_leave_notify_event(GdkEventCrossing* a_p0)
{
  m_seqkeys_wid->set_hint_state( false );
  return false;
}



bool
seqroll::on_focus_in_event(GdkEventFocus*)
{
    set_flags(Gtk::HAS_FOCUS);

    //m_seq->clear_clipboard();

    return false;
}


bool
seqroll::on_focus_out_event(GdkEventFocus*)
{
    unset_flags(Gtk::HAS_FOCUS);
    
    return false;
}

bool 
seqroll::on_key_press_event(GdkEventKey* a_p0)
{
    bool ret = false;

    if ( a_p0->type == GDK_KEY_PRESS ){

        if ( a_p0->keyval ==  GDK_Delete ){

            m_seq->push_undo();
            m_seq->mark_selected();
            m_seq->remove_marked();
            ret = true;
        }

        if ( a_p0->state & GDK_CONTROL_MASK ){

            /* cut */
            if ( a_p0->keyval == GDK_x || a_p0->keyval == GDK_X ){

                m_seq->push_undo();
                m_seq->copy_selected();
                m_seq->mark_selected();
                m_seq->remove_marked();

                ret = true;
            }
            /* copy */
            if ( a_p0->keyval == GDK_c || a_p0->keyval == GDK_C ){

                m_seq->copy_selected();
                ret = true;
            }

            /* paste */
            if ( a_p0->keyval == GDK_v || a_p0->keyval == GDK_V ){

                start_paste();
                ret = true;
            }

            /* Undo */
            if ( a_p0->keyval == GDK_z || a_p0->keyval == GDK_Z ){

                m_seq->pop_undo();
                ret = true;
            }
        }
    }

    if ( ret == true ){
        redraw();
        m_seq->set_dirty();
        return true;
    }

    return false;
}




void 
seqroll::set_data_type( unsigned char a_status, unsigned char a_control = 0 )
{
    m_status = a_status;
    m_cc = a_control;
}



void
seqroll::on_size_allocate(Gtk::Allocation& a_r )
{
    Gtk::DrawingArea::on_size_allocate( a_r );

    m_window_x = a_r.get_width();
    m_window_y = a_r.get_height();

    update_sizes(); 
 
}

bool
seqroll::on_scroll_event( GdkEventScroll* a_ev )
{
	double val = m_vadjust->get_value();

	if ( a_ev->direction == GDK_SCROLL_UP ){
		val -= m_vadjust->get_step_increment()/6;
	} else if (  a_ev->direction == GDK_SCROLL_DOWN ){
		val += m_vadjust->get_step_increment()/6;
	} else {
		return true;
	}

	m_vadjust->clamp_page( val, val + m_vadjust->get_page_size() );
    return true;

}

