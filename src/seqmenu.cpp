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

#include "seqmenu.h"
#include "seqedit.h"
#include "trackeredit.h"
#include "font.h"

#include <gtkmm/dialog.h>
#include <gtkmm/stock.h>
#include <gtkmm/label.h>

/* clip-type chooser result */
enum clip_view_e { CLIP_PIANO_ROLL = 0, CLIP_TRACKER = 1 };

/* Small monochrome dialog asking which editor view to open for a pattern.
   Returns CLIP_PIANO_ROLL or CLIP_TRACKER (defaults to piano roll on cancel). */
static int
choose_clip_view( const char *a_name )
{
    Gtk::Dialog dialog( "Open Clip As", true );    /* modal */
    dialog.set_size_request( 260, -1 );

    /* black & white look */
    Gdk::Color black; black.set_rgb_p( 0.0, 0.0, 0.0 );
    Gdk::Color white; white.set_rgb_p( 0.93, 0.93, 0.93 );
    dialog.modify_bg( Gtk::STATE_NORMAL, black );

    Gtk::Label *lbl = Gtk::manage(
        new Gtk::Label( std::string("Choose an editor for \"") +
                        ( a_name ? a_name : "" ) + "\":" ) );
    lbl->modify_fg( Gtk::STATE_NORMAL, white );
    lbl->set_padding( 8, 8 );
    dialog.get_vbox()->pack_start( *lbl, false, false );
    lbl->show();

    /* response ids: 1 = piano roll, 2 = tracker */
    dialog.add_button( "Piano Roll", 1 );
    dialog.add_button( "Tracker",    2 );

    int resp = dialog.run();
    if ( resp == 2 )
        return CLIP_TRACKER;
    return CLIP_PIANO_ROLL;
}


// Constructor

seqmenu::seqmenu( perform *a_p  )
{    
    using namespace Menu_Helpers;

    m_mainperf = a_p;
    m_menu = NULL;

} 


    void
seqmenu::popup_menu( void )
{

    using namespace Menu_Helpers;

    if ( m_menu != NULL )
        delete m_menu;

    m_menu = manage( new Menu());

    if ( m_mainperf->is_active( m_current_seq )) {
        m_menu->items().push_back(MenuElem("Edit", mem_fun(*this,&seqmenu::seq_edit)));
    } else {
        m_menu->items().push_back(MenuElem("New", mem_fun(*this,&seqmenu::seq_edit)));
    }



    m_menu->items().push_back(SeparatorElem());

    if ( m_mainperf->is_active( m_current_seq )) {
        m_menu->items().push_back(MenuElem("Cut", mem_fun(*this,&seqmenu::seq_cut)));
        m_menu->items().push_back(MenuElem("Copy", mem_fun(*this,&seqmenu::seq_copy)));
    } else {
        m_menu->items().push_back(MenuElem("Paste", mem_fun(*this,&seqmenu::seq_paste)));
    }

    m_menu->items().push_back(SeparatorElem());
    
    Menu *menu_song = manage( new Menu() );
    m_menu->items().push_back( MenuElem( "Song", *menu_song) );
    
    if ( m_mainperf->is_active( m_current_seq ))
    {
        menu_song->items().push_back(MenuElem("Clear Song Data", mem_fun(*this,&seqmenu::seq_clear_perf)));
    }
    
    menu_song->items().push_back(MenuElem("Mute All Tracks", mem_fun(*this,&seqmenu::mute_all_tracks)));
    
    if ( m_mainperf->is_active( m_current_seq )) {
        m_menu->items().push_back(SeparatorElem());
        Menu *menu_buses = manage( new Menu() );

        m_menu->items().push_back( MenuElem( "Midi Bus", *menu_buses) );

        /* midi buses */
        mastermidibus *masterbus = m_mainperf->get_master_midi_bus();
        for ( int i=0; i< masterbus->get_num_out_buses(); i++ ){

            Menu *menu_channels = manage( new Menu() );

            menu_buses->items().push_back(MenuElem( masterbus->get_midi_out_bus_name(i),
                        *menu_channels ));
            char b[4];

            /* midi channel menu */
            for( int j=0; j<16; j++ ){
                sprintf( b, "%d", j+1 );
                std::string name = string(b);
                int instrument = global_user_midi_bus_definitions[i].instrument[j]; 
                if ( instrument >= 0 && instrument < c_maxBuses )
                {
                    name = name + (string(" (") + 
                            global_user_instrument_definitions[instrument].instrument + 
                            string(")") );
                }

                menu_channels->items().push_back(MenuElem(name, 
                            sigc::bind(mem_fun(*this,&seqmenu::set_bus_and_midi_channel), 
                                i, j )));
            }
        }        
    }

    /* SCALE-MASTER / SCALE-FOLLOW -- see docs/scale-follow.md section 5 */
    if ( m_mainperf->is_active( m_current_seq )) {

        m_menu->items().push_back(SeparatorElem());

        Menu *menu_scale = manage( new Menu() );
        m_menu->items().push_back( MenuElem( "Scale Follow", *menu_scale ) );

        sequence *s = m_mainperf->get_sequence( m_current_seq );

        CheckMenuItem *master_item =
            manage( new CheckMenuItem("Set as scale master") );
        master_item->set_active( s->get_scale_master() );
        master_item->signal_activate().connect(
            mem_fun(*this,&seqmenu::seq_set_scale_master) );
        menu_scale->items().push_back( *master_item );

        CheckMenuItem *follow_item =
            manage( new CheckMenuItem("Follow scale master") );
        follow_item->set_active( s->get_follows_master() );
        follow_item->signal_activate().connect(
            mem_fun(*this,&seqmenu::seq_toggle_follows_master) );
        menu_scale->items().push_back( *follow_item );

        /* master root key picker */
        Menu *menu_mkey = manage( new Menu() );
        menu_scale->items().push_back( MenuElem( "Master Root", *menu_mkey ) );
        for ( int k=0; k<12; k++ ){
            menu_mkey->items().push_back(MenuElem( c_key_text[k],
                sigc::bind(mem_fun(*this,&seqmenu::seq_set_master_key), k )));
        }

        /* master scale picker */
        Menu *menu_mscale = manage( new Menu() );
        menu_scale->items().push_back( MenuElem( "Master Scale", *menu_mscale ) );
        for ( int sc=0; sc<c_scale_size; sc++ ){
            menu_mscale->items().push_back(MenuElem( c_scales_text[sc],
                sigc::bind(mem_fun(*this,&seqmenu::seq_set_master_scale), sc )));
        }
    }

    m_menu->popup(0,0);

}

/* SCALE-MASTER / SCALE-FOLLOW menu handlers */

void
seqmenu::seq_set_scale_master( void )
{
    if ( ! m_mainperf->is_active( m_current_seq ) )
        return;

    /* toggle: if this seq is already the master, clear; otherwise designate it
       (perform enforces a single master). */
    if ( m_mainperf->get_scale_master() == m_current_seq )
        m_mainperf->set_scale_master( -1 );
    else
        m_mainperf->set_scale_master( m_current_seq );

    redraw( m_current_seq );
}

void
seqmenu::seq_toggle_follows_master( void )
{
    if ( ! m_mainperf->is_active( m_current_seq ) )
        return;

    sequence *s = m_mainperf->get_sequence( m_current_seq );
    m_mainperf->set_follows_master( m_current_seq, ! s->get_follows_master() );
    redraw( m_current_seq );
}

void
seqmenu::seq_set_master_key( int a_key )
{
    if ( m_mainperf->is_active( m_current_seq ) )
        m_mainperf->get_sequence( m_current_seq )->set_master_key( a_key );
}

void
seqmenu::seq_set_master_scale( int a_scale )
{
    if ( m_mainperf->is_active( m_current_seq ) )
        m_mainperf->get_sequence( m_current_seq )->set_master_scale( a_scale );
}

    void
seqmenu::set_bus_and_midi_channel( int a_bus, int a_ch )
{
    if ( m_mainperf->is_active( m_current_seq )) {
        m_mainperf->get_sequence( m_current_seq )->set_midi_bus( a_bus );
        m_mainperf->get_sequence( m_current_seq )->set_midi_channel( a_ch );
        m_mainperf->get_sequence( m_current_seq )->set_dirty();
    }
}

void
seqmenu::mute_all_tracks( void )
{
    m_mainperf->mute_all_tracks();
}


// Opens the chosen editor (piano roll or tracker) for the current seq.
void
seqmenu::open_clip_editor()
{
    sequence *s = m_mainperf->get_sequence( m_current_seq );

    /* present the B/W clip-type chooser */
    int view = choose_clip_view( s->get_name() );

    if ( view == CLIP_TRACKER )
        new trackeredit( s, m_mainperf, m_current_seq );
    else
        new seqedit( s, m_mainperf, m_current_seq );
}

// Menu callback, Lanches Editor Window
void
seqmenu::seq_edit(){

    if ( m_mainperf->is_active( m_current_seq )) {
        if ( !m_mainperf->get_sequence( m_current_seq )->get_editing())
        {
            open_clip_editor();
        }
        else {
            m_mainperf->get_sequence( m_current_seq )->set_raise(true);
        }
    }
    else {
        this->seq_new();
        open_clip_editor();
    }
}

// Makes a New sequence 
void 
seqmenu::seq_new(){

    if ( ! m_mainperf->is_active( m_current_seq )){

        m_mainperf->new_sequence( m_current_seq );
        m_mainperf->get_sequence( m_current_seq )->set_dirty();

    }
}

// Copies selected to clipboard sequence */
void 
seqmenu::seq_copy(){

    if ( m_mainperf->is_active( m_current_seq ))
        m_clipboard = *(m_mainperf->get_sequence( m_current_seq ));
}

// Deletes and Copies to Clipboard */
void 
seqmenu::seq_cut(){

    if ( m_mainperf->is_active( m_current_seq ) &&
            !m_mainperf->is_sequence_in_edit( m_current_seq ) ){

        m_clipboard = *(m_mainperf->get_sequence( m_current_seq ));
        m_mainperf->delete_sequence( m_current_seq );
        redraw( m_current_seq );

    }
}

// Puts clipboard into location
void 
seqmenu::seq_paste(){

    if ( ! m_mainperf->is_active( m_current_seq )){

        m_mainperf->new_sequence( m_current_seq  );
        *(m_mainperf->get_sequence( m_current_seq )) = m_clipboard;

        m_mainperf->get_sequence( m_current_seq )->set_dirty();

    }
}


void 
seqmenu::seq_clear_perf(){

    if ( m_mainperf->is_active( m_current_seq )){

        m_mainperf->push_trigger_undo();
        
        m_mainperf->clear_sequence_triggers( m_current_seq  );
        m_mainperf->get_sequence( m_current_seq )->set_dirty();

    }
}



