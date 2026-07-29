//----------------------------------------------------------------------------
//
//  This file is part of PatchKnob.
//
//  PatchKnob is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  PatchKnob is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with PatchKnob; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//-----------------------------------------------------------------------------
#include <cassert>
#include "perform.h"
#include "midibus.h"
#include "event.h"
#include "audio_app.h"        /* engine-owned tempo: set_bpm funnels into it */
#include "keycodes_compat.h"  /* legacy default keybinding values */
#include <stdio.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN   /* keep rpcndr.h's 'byte' away from std::byte */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>          /* SetThreadPriority for the rt threads */
#endif

perform::perform()
{
    for (int i=0; i< c_max_sequence; i++ ){

		m_seqs[i] = NULL;
        m_seqs_active[i] = false;
		
		
    }

    m_scale_master_seq = -1;

    m_running = false;
    m_looping = false;
    m_inputing = true;
    m_outputing = true;
    m_tick = 0;

    thread_trigger_width_ms = c_thread_trigger_width_ms;

    m_left_tick = 0;
    m_right_tick = c_ppqn * 16;
    m_starting_tick = 0;
    
    midi_control zero = {false,false,0,0,0};

    for ( int i=0; i<c_midi_controls; i++ ){

		m_midi_cc_toggle[i] = zero;
		m_midi_cc_on[i] = zero;
		m_midi_cc_off[i] = zero;
    }

    key_events[ PATCHKNOB_KEY_1 ] = 0;
    key_events[ PATCHKNOB_KEY_Q ] = 1;
    key_events[ PATCHKNOB_KEY_A ] = 2;
    key_events[ PATCHKNOB_KEY_Z ] = 3;
    key_events[ PATCHKNOB_KEY_2 ] = 4;
    key_events[ PATCHKNOB_KEY_W ] = 5;
    key_events[ PATCHKNOB_KEY_S ] = 6;
    key_events[ PATCHKNOB_KEY_X ] = 7;
    key_events[ PATCHKNOB_KEY_3 ] = 8;
    key_events[ PATCHKNOB_KEY_E ] = 9;
    key_events[ PATCHKNOB_KEY_D ] = 10;
    key_events[ PATCHKNOB_KEY_C ] = 11;
    key_events[ PATCHKNOB_KEY_4 ] = 12;
    key_events[ PATCHKNOB_KEY_R ] = 13;
    key_events[ PATCHKNOB_KEY_F ] = 14;
    key_events[ PATCHKNOB_KEY_V ] = 15;
    key_events[ PATCHKNOB_KEY_5 ] = 16;
    key_events[ PATCHKNOB_KEY_T ] = 17;
    key_events[ PATCHKNOB_KEY_G ] = 18;
    key_events[ PATCHKNOB_KEY_B ] = 19;
    key_events[ PATCHKNOB_KEY_6 ] = 20;
    key_events[ PATCHKNOB_KEY_Y ] = 21;
    key_events[ PATCHKNOB_KEY_H ] = 22;
    key_events[ PATCHKNOB_KEY_N ] = 23;
    key_events[ PATCHKNOB_KEY_7 ] = 24;
    key_events[ PATCHKNOB_KEY_U ] = 25;
    key_events[ PATCHKNOB_KEY_J ] = 26;
    key_events[ PATCHKNOB_KEY_M ] = 27;
    key_events[ PATCHKNOB_KEY_8 ] = 28;
    key_events[ PATCHKNOB_KEY_I ] = 29;
    key_events[ PATCHKNOB_KEY_K ] = 30;
    key_events[ PATCHKNOB_KEY_COMMA ] = 31;

    
    m_key_bpm_up = PATCHKNOB_KEY_APOSTROPHE;
    m_key_bpm_dn = PATCHKNOB_KEY_SEMICOLON;

    m_key_replace = PATCHKNOB_KEY_CONTROL_LEFT;
    m_key_queue = PATCHKNOB_KEY_CONTROL_RIGHT;
    m_key_snapshot_1 = PATCHKNOB_KEY_ALT_LEFT;
    m_key_snapshot_2 = PATCHKNOB_KEY_ALT_RIGHT;
    
    m_key_screenset_up = PATCHKNOB_KEY_BRACKET_RIGHT;
    m_key_screenset_dn = PATCHKNOB_KEY_BRACKET_LEFT;

    m_key_start  = PATCHKNOB_KEY_SPACE;
    m_key_stop   = PATCHKNOB_KEY_ESCAPE;
    
    m_offset = 0;
    m_control_status = 0;
    m_screen_set = 0;

    m_jack_running = false;
    m_jack_master = false;

    m_out_thread_launched = false;
    m_in_thread_launched = false;
    
}

void
perform::init( void )
{
    m_master_bus.init( );
}

void
perform::init_jack( void )
{

#ifdef JACK_SUPPORT

    if ( global_with_jack_transport  && !m_jack_running){

        m_jack_running = true;
        m_jack_master = true;

        //printf ( "init_jack() m_jack_running[%d]\n", m_jack_running );
        
        do {

            char client_name[100];
            sprintf( client_name, "PatchKnob (%d)", getpid());
            
            /* become a new client of the JACK server */
            if (( m_jack_client = jack_client_new(client_name)) == 0) {
                printf( "JACK server is not running.\n[JACK sync disabled]\n");
                m_jack_running = false;
                break;
            }
            if (jack_activate(m_jack_client)) {
                printf("Cannot register as JACK client\n");
                m_jack_running = false;
                break;
            }
            
            jack_on_shutdown( m_jack_client, jack_shutdown,(void *) this );
            jack_set_sync_callback(m_jack_client, jack_sync_callback, (void *) this );

            
            
            bool cond = global_with_jack_master_cond; /* true if we want to fail if there is already a master */
            if ( global_with_jack_master && 
                 jack_set_timebase_callback(m_jack_client, cond, jack_timebase_callback, this) == 0){
                
                printf("[JACK transport master]\n");
                m_jack_master = true;
            }
            else {
                printf("[JACK transport slave]\n");
                m_jack_master = false;
             
            }
            
        } while (0);
        
    } 

 
    
#endif
}








void
perform::deinit_jack( void )
{

#ifdef JACK_SUPPORT

    if ( m_jack_running){

         //printf ( "deinit_jack() m_jack_running[%d]\n", m_jack_running );
        
        m_jack_running = false;
        m_jack_master = false;

        if ( jack_release_timebase(m_jack_client)){
            printf("Cannot release Timebase.\n");
        }
        
        if (jack_client_close(m_jack_client)) {
            printf("Cannot close JACK client.\n");
        }
            

    }

    if ( !m_jack_running ){
        printf( "[JACK sync disabled]\n");
    }
    
#endif
}




void
perform::clear_all( void )
{

    reset_sequences();

    for (int i=0; i< c_max_sequence; i++ ){

        if ( is_active(i) )
            delete_sequence( i );
    }
    
    string e( "" );
    
    for (int i=0; i<c_max_sets; i++ ){
        set_screen_set_notepad( i, &e );
    }

}



void
perform::mute_all_tracks( void )
{
    for (int i=0; i< c_max_sequence; i++ )
    {    
        if ( is_active(i) )
            m_seqs[i]->set_song_mute( true );
              
    }
}




perform::~perform()
{
    m_inputing = false;
    m_outputing = false;
    m_running = false;

    m_condition_var.signal();

    if (m_out_thread_launched )
        pthread_join( m_out_thread, NULL );
    
    if (m_in_thread_launched )
        pthread_join( m_in_thread, NULL );

    for (int i=0; i< c_max_sequence; i++ ){
	if ( is_active(i) ){
	    delete m_seqs[i];
	}
    }

    /* threads are joined: retired sequences are finally safe to free */
    for ( size_t i = 0; i < m_seq_graveyard.size(); i++ )
        delete m_seq_graveyard[i];
    m_seq_graveyard.clear();
}

void 
perform::set_left_tick( long a_tick )
{
    m_left_tick = a_tick;
    m_starting_tick = a_tick;
 
    if ( m_left_tick >= m_right_tick )
	m_right_tick = m_left_tick + c_ppqn * 4;
    
}

long 
perform::get_left_tick( void )
{ 
    return m_left_tick; 
}


void 
perform::set_starting_tick( long a_tick )
{
    m_starting_tick = a_tick;
}

long 
perform::get_starting_tick( void )
{ 
    return m_starting_tick; 
}

void 
perform::set_right_tick( long a_tick ) 
{
    if ( a_tick >= c_ppqn * 4 ){
	
	m_right_tick = a_tick; 
	
	if ( m_right_tick <= m_left_tick ){
	    m_left_tick = m_right_tick - c_ppqn * 4;
            m_starting_tick = m_left_tick;
        }
    }
}

long 
perform::get_right_tick( void )
{ 
    return m_right_tick; 
}


void 
perform::add_sequence( sequence *a_seq, int a_perf )
{
    /* check for perferred */
    if ( a_perf < c_max_sequence &&  
	 is_active(a_perf) == false &&
	 a_perf >= 0 ){
		    
      m_seqs[a_perf] = a_seq;
      set_active(a_perf, true);
	    //a_seq->set_tag( a_perf );

    } else {

	for (int i=a_perf; i< c_max_sequence; i++ ){
	    
	    if ( is_active(i) == false ){

	      m_seqs[i] = a_seq;
	      set_active(i,true);

		//a_seq->set_tag( i  );
		break;
	    }
	}
    }
}


void
perform::set_active( int a_sequence, bool a_active )
{
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
        return;

    //printf ("set_active %d\n", a_active );
    
    if ( m_seqs_active[ a_sequence ] == true && a_active == false )
    {
        set_was_active(a_sequence);
    }
    
    m_seqs_active[ a_sequence ] = a_active;

    
}
    

void 
perform::set_was_active( int a_sequence )
{
    
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
        return;
    
        //printf( "was_active true\n" );
        
        m_was_active_main[ a_sequence ] = true;
        m_was_active_edit[ a_sequence ] = true;
        m_was_active_perf[ a_sequence ] = true;
        m_was_active_names[ a_sequence ] = true;
}
 


bool 
perform::is_active( int a_sequence )
{
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
	    return false;
    
    return m_seqs_active[ a_sequence ];
}


bool 
perform::is_dirty_main (int a_sequence)
{
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
	   return false;

    if ( is_active(a_sequence) )
    {
        return m_seqs[a_sequence]->is_dirty_main();
    }

    bool was_active = m_was_active_main[ a_sequence ];
    m_was_active_main[ a_sequence ] = false;
    
    return was_active;
}


bool 
perform::is_dirty_edit (int a_sequence)
{
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
	    return false;

    if ( is_active(a_sequence) )
    {
        return m_seqs[a_sequence]->is_dirty_edit();
    }
    
    bool was_active = m_was_active_edit[ a_sequence ];
    m_was_active_edit[ a_sequence ] = false;
    
    return was_active;
}


bool 
perform::is_dirty_perf (int a_sequence)
{
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
	return false;

    if ( is_active(a_sequence) )
    {
        return m_seqs[a_sequence]->is_dirty_perf();
    }

    bool was_active = m_was_active_perf[ a_sequence ];
    m_was_active_perf[ a_sequence ] = false;
    
    return was_active;
}

bool 
perform::is_dirty_names (int a_sequence)
{
    if ( a_sequence < 0 || a_sequence >= c_max_sequence )
	return false;

    if ( is_active(a_sequence) )
    {
        return m_seqs[a_sequence]->is_dirty_names();
    } 
 
    bool was_active = m_was_active_names[ a_sequence ];
    m_was_active_names[ a_sequence ] = false;
    
    return was_active;
}

sequence*
perform::get_sequence( int a_sequence )
{
    return m_seqs[a_sequence];
}


/* SCALE-MASTER / SCALE-FOLLOW -- see docs/scale-follow.md section 2.

   Enforces a single master: clears the previous master's flag before setting
   the new one.  Passing -1 (or an inactive sequence) clears the master. */
void
perform::set_scale_master( int a_seq )
{
    /* clear the previous master's flag */
    if ( m_scale_master_seq >= 0 && is_active( m_scale_master_seq ) )
        m_seqs[ m_scale_master_seq ]->set_scale_master( false );

    if ( a_seq >= 0 && a_seq < c_max_sequence && is_active( a_seq ) ){
        m_seqs[ a_seq ]->set_scale_master( true );
        m_scale_master_seq = a_seq;
    }
    else {
        m_scale_master_seq = -1;
    }
}

int
perform::get_scale_master( void )
{
    return m_scale_master_seq;
}

void
perform::set_follows_master( int a_seq, bool a_follow )
{
    if ( a_seq >= 0 && a_seq < c_max_sequence && is_active( a_seq ) )
        m_seqs[ a_seq ]->set_follows_master( a_follow );
}

mastermidibus* 
perform::get_master_midi_bus( )
{
    return &m_master_bus; 
}





void 
perform::set_running( bool a_running )
{
    m_running = a_running;
}

bool 
perform::is_running( void )
{
    return m_running;
}

void
perform::set_bpm(double a_bpm)
{
    if ( a_bpm < 20.0 )  a_bpm = 20.0;
    if ( a_bpm > 500.0 ) a_bpm = 500.0;

    if ( ! (m_jack_running && m_running )){
        m_master_bus.set_bpm( a_bpm );
    }

    /* the AUDIO ENGINE owns tempo: every bpm write (UI, file load, hotkeys,
       midi control) funnels through here into the kitchensink tempo map, so
       the sequencer pace, plugin VstTimeInfo and transport clock can never
       disagree.  fractional BPM survives end-to-end. */
    PatchKnob::app::audio_app_set_tempo( a_bpm );
}

double
perform::get_bpm( )
{
    return  m_master_bus.get_bpm( );
}

void
perform::delete_sequence( int a_num )
{
	set_active(a_num, false);

    if ( m_seqs[a_num] != NULL &&
         !m_seqs[a_num]->get_editing() ){

		m_seqs[a_num]->set_playing( false );
		/* RETIRE, don't free: the output thread / UI may still hold this
		   pointer for an instant (use-after-free -> divide-by-zero crash in
		   get_last_tick).  gc_graveyard() frees it a couple of UI frames later. */
		m_seq_graveyard.push_back( m_seqs[a_num] );
		m_seq_graveyard_age.push_back( 0 );
		m_seqs[a_num] = NULL;
    }

}

void
perform::gc_graveyard( void )
{
    /* Message thread only.  A retired sequence was set_active(false) before it
       landed here, so the output thread stops referencing it within one audio
       block; surviving >=2 gc passes (>=2 UI frames, ~tens of ms) is well past
       that window, so freeing is safe -- and the graveyard stays bounded. */
    for ( size_t i = 0; i < m_seq_graveyard.size(); )
    {
        if ( ++m_seq_graveyard_age[i] >= 2 )
        {
            delete m_seq_graveyard[i];
            m_seq_graveyard.erase( m_seq_graveyard.begin() + (long)i );
            m_seq_graveyard_age.erase( m_seq_graveyard_age.begin() + (long)i );
        }
        else ++i;
    }
}

bool 
perform::is_sequence_in_edit( int a_num )
{
	return ( m_seqs[a_num] != NULL &&
			 m_seqs[a_num]->get_editing());

}

void 
perform::new_sequence( int a_sequence )
{

    m_seqs[ a_sequence ] = new sequence();
    m_seqs[ a_sequence ]->set_master_midi_bus( &m_master_bus );
    set_active(a_sequence, true);

}

midi_control *
perform::get_midi_control_toggle( unsigned int a_seq )
{
	if ( a_seq >= (unsigned int) c_midi_controls )
		return NULL;
	return &m_midi_cc_toggle[a_seq];

}

midi_control *
perform::get_midi_control_on( unsigned int a_seq )
{
	if ( a_seq >= (unsigned int) c_midi_controls )
		return NULL;
	return &m_midi_cc_on[a_seq];
}

midi_control *
perform::get_midi_control_off( unsigned int a_seq )
{
	if ( a_seq >= (unsigned int) c_midi_controls )
		return NULL;
	return &m_midi_cc_off[a_seq];
}





void 
perform::print()
{
    //   for( int i=0; i<m_numSeq; i++ ){
	
	//printf("Sequence %d\n", i);
	//m_seqs[i]->print();
    // }

    //  m_master_bus.print();
}

void 
perform::set_screen_set_notepad( int a_screen_set, string *a_notepad )
{
    if ( a_screen_set < c_max_sets )
	m_screen_set_notepad[a_screen_set] = *a_notepad;
}


string *
perform::get_screen_set_notepad( int a_screen_set )
{
     return &m_screen_set_notepad[a_screen_set];
}


void
perform::set_screenset( int a_ss )
{
    m_screen_set = a_ss;

    if ( m_screen_set < 0 ) 
	m_screen_set = c_max_sets - 1;
    
    if ( m_screen_set >= c_max_sets )
	m_screen_set = 0;
}

int
perform::get_screenset( void )
{
    return m_screen_set;
}

void 
perform::set_offset( int a_offset ) 
{ 
	m_offset = a_offset  * c_mainwnd_rows * c_mainwnd_cols; 
}


void 
perform::play( long a_tick )
{

    /* just run down the list of sequences and have them dump */

    //printf( "play [%d]\n", a_tick );
    
    m_tick = a_tick;	
    /* SCALE-MASTER / SCALE-FOLLOW: resolve the active master once per tick into
       plain scalars (each getter takes/releases the master's own mutex, no
       nested locking), then push the snapshot to each follower before its
       play() runs.  See docs/scale-follow.md section 2. */
    bool master_on = false;
    int  master_key = 0, master_scale = c_scale_off;
    int  scale_m = m_scale_master_seq;
    if ( scale_m >= 0 && is_active(scale_m) && m_seqs[scale_m]->get_playing()
         && m_seqs[scale_m]->get_scale_master() ){
        master_on    = true;
        master_key   = m_seqs[scale_m]->get_master_key();
        master_scale = m_seqs[scale_m]->get_master_scale();
    }

    for (int i=0; i< c_max_sequence; i++ ){
		
		if ( is_active(i) ){
			assert( m_seqs[i] );

				/* a follower snaps only if it is not the master itself and
				   has its follow flag set */
				bool follow = master_on && ( i != m_scale_master_seq )
				              && m_seqs[i]->get_follows_master();
				m_seqs[i]->set_master_scale_context( follow, master_key, master_scale );
			
			
			if ( m_seqs[i]->get_queued() &&
				 m_seqs[i]->get_queued_tick() <= a_tick ){
				
				m_seqs[i]->play( m_seqs[i]->get_queued_tick() - 1, m_playback_mode );
				m_seqs[i]->toggle_playing();
			}
			
			m_seqs[i]->play( a_tick, m_playback_mode );
		} 
    }
	
    /* flush the bus */
    m_master_bus.flush();
}

void 
perform::set_orig_ticks( long a_tick  )
{
    for (int i=0; i< c_max_sequence; i++ ){
	
	if ( is_active(i) == true ){
	    assert( m_seqs[i] );
	    m_seqs[i]->set_orig_tick( a_tick );
	} 
    }
}

void 
perform::clear_sequence_triggers( int a_seq  )
{
	if ( is_active(a_seq) == true ){
		assert( m_seqs[a_seq] );
		m_seqs[a_seq]->clear_triggers( );
	} 
}

void
perform::move_triggers( bool a_direction )
{
    if ( m_left_tick < m_right_tick ){

	long distance = m_right_tick - m_left_tick;

	for (int i=0; i< c_max_sequence; i++ ){

	    if ( is_active(i) == true ){
		assert( m_seqs[i] );
		m_seqs[i]->move_triggers( m_left_tick, distance, a_direction );
	    } 
	}
    }
}

void
perform::push_trigger_undo( void )
{
    for (int i=0; i< c_max_sequence; i++ ){
        
        if ( is_active(i) == true ){
            assert( m_seqs[i] );
            m_seqs[i]->push_trigger_undo( );
        } 
    }
}

void
perform::pop_trigger_undo( void )
{
    for (int i=0; i< c_max_sequence; i++ ){
        
        if ( is_active(i) == true ){
            assert( m_seqs[i] );
            m_seqs[i]->pop_trigger_undo( );
        } 
    }
}


/* copies between L and R -> R */
void
perform::copy_triggers( )
{
    if ( m_left_tick < m_right_tick ){

	long distance = m_right_tick - m_left_tick;

	for (int i=0; i< c_max_sequence; i++ ){

	    if ( is_active(i) == true ){
		assert( m_seqs[i] );
		m_seqs[i]->copy_triggers( m_left_tick, distance );
	    } 
	}
    }
}



void 
perform::start_jack(  )
{
    //printf( "perform::start_jack()\n" );
#ifdef JACK_SUPPORT
    if ( m_jack_running)
        jack_transport_start (m_jack_client );
#endif
}


void 
perform::stop_jack(  )
{
    //printf( "perform::stop_jack()\n" );
#ifdef JACK_SUPPORT
    if( m_jack_running )
        jack_transport_stop (m_jack_client);
#endif
}


void
perform::position_jack( bool a_state )
{
    
    //printf( "perform::position_jack()\n" );

    
#ifdef JACK_SUPPORT
    
    if ( m_jack_running ){
        jack_transport_locate( m_jack_client, 0 );
    }
    return;
    
  

    

    jack_nframes_t rate = jack_get_sample_rate( m_jack_client ) ;

    long current_tick = 0;

    if ( a_state ){
        current_tick = m_left_tick;
    }

    jack_position_t pos;

    pos.valid = JackPositionBBT;
    pos.beats_per_bar = 4;
    pos.beat_type = 4;
    pos.ticks_per_beat = c_ppqn * 10;
    pos.beats_per_minute =  m_master_bus.get_bpm();
    
    /* Compute BBT info from frame number.  This is relatively
     * simple here, but would become complex if we supported tempo
     * or time signature changes at specific locations in the
     * transport timeline. */
    
    current_tick *= 10;
    
    pos.bar  = (int32_t) (current_tick / (long) pos.ticks_per_beat / pos.beats_per_bar);
    pos.beat = (int32_t) ((current_tick / (long) pos.ticks_per_beat) % 4);
    pos.tick = (int32_t) (current_tick % (c_ppqn * 10));

    pos.bar_start_tick = pos.bar * pos.beats_per_bar * pos.ticks_per_beat;
    pos.frame_rate = rate;
    pos.frame = (jack_nframes_t) ( (current_tick * rate * 60.0)
        / (pos.ticks_per_beat * pos.beats_per_minute) );

    /*
    ticks * 10 = jack ticks;
    jack ticks / ticks per beat = num beats;
    num beats / beats per minute = num minutes
        num minutes * 60 = num seconds
        num secords * frame_rate  = frame */

    
    pos.bar++;
    pos.beat++;

    //printf( "position bbb[%d:%d:%4d]\n", pos.bar, pos.beat, pos.tick );
    
    jack_transport_reposition( m_jack_client, &pos );

    
    
#endif
    
}   

void 
perform::start( bool a_state )
{

    if(  m_jack_running ){
        return;
    }

    inner_start( a_state );
}



void 
perform::stop( )
{
    if(  m_jack_running ){
        return;
    }

    inner_stop();
}


void
perform::inner_start( bool a_state )
{
    m_condition_var.lock();
    
    if ( ! is_running() ){

        set_playback_mode( a_state );

         if ( a_state )
            off_sequences( );

        /* drive the ENGINE transport too (every start path lands here: UI,
           MIDI control, JACK sync): position it at our start tick and raise
           the patch run-state, so host-synced plugins and the sample clock
           the output thread paces off can never diverge from us. */
        if ( PatchKnob::app::audio_app_running() ){

            long long start_tick = a_state ? (long long) m_starting_tick : 0;

            PatchKnob::app::audio_app_transport_locate(
                    PatchKnob::app::audio_app_tick_to_sample( start_tick ) );
            PatchKnob::app::audio_app_patch_set_playing( true );
        }

        set_running( true );
		
       

        m_condition_var.signal();
        
    }

    m_condition_var.unlock();
}



void
perform::inner_stop( )
{
    set_running( false );

    /* drop the engine transport run-state with us (all stop paths) */
    if ( PatchKnob::app::audio_app_running() )
        PatchKnob::app::audio_app_patch_set_playing( false );

    //off_sequences();
    reset_sequences(  );
}



void 
perform::off_sequences( void )
{
    for (int i=0; i< c_max_sequence; i++ ){
		
		if ( is_active(i) == true ){
			assert( m_seqs[i] );
			m_seqs[i]->set_playing( false );
			
		} 
    }
}




void 
perform::reset_sequences( void )
{
    for (int i=0; i< c_max_sequence; i++ ){
		
		if ( is_active(i) == true ){
			assert( m_seqs[i] );

                        bool state = m_seqs[i]->get_playing();
                        
			m_seqs[i]->off_playing_notes( );
			m_seqs[i]->set_playing( false );
			m_seqs[i]->zero_markers( );

                        if( !m_playback_mode )
                            m_seqs[i]->set_playing( state );
		} 
    }
    /* flush the bus */
    m_master_bus.flush();
}

void 
perform::launch_output_thread( void )
{
    int err;
    err = pthread_create( &m_out_thread, 
			  NULL, 
			  output_thread_func,
			  this );
    m_out_thread_launched= true;
}



void
perform::set_playback_mode( bool a_playback_mode )
{
    m_playback_mode = a_playback_mode;
}

void 
perform::launch_input_thread()
{
    int err;
    err = pthread_create( &m_in_thread, 
			  NULL, 
			  input_thread_func,
			  this );
     m_in_thread_launched= true;

}


long 
perform::get_max_trigger( void )
{
    long ret = 0, t;

    for (int i=0; i< c_max_sequence; i++ ){
	
	if ( is_active(i) == true ){
	    assert( m_seqs[i] );
	    
	    t = m_seqs[i]->get_max_trigger( );  
	    if ( t > ret )
		ret = t;
	} 
    }

    return ret;
}



void*
output_thread_func(void *a_pef )
{
    /* set our performance */
    perform *p = (perform *) a_pef;
    assert(p);

    /* the scheduling feed must not lose the CPU to UI repaints or plugin
       scans; on failure just log and keep going -- never kill the thread */
#ifdef _WIN32
    if ( !SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL ))
        printf( "output_thread_func: SetThreadPriority(TIME_CRITICAL) failed\n" );
#endif

    p->output_func();

    return 0;
}



#ifdef JACK_SUPPORT


int jack_sync_callback(jack_transport_state_t state, 
					   jack_position_t *pos, void *arg)
{
  //printf( "jack_sync_callback() " );

  perform *p = (perform *) arg;

  p->m_jack_frame_current = jack_get_current_transport_frame( p->m_jack_client );

  p->m_jack_tick =
      p->m_jack_frame_current *
      p->m_jack_pos.ticks_per_beat *
      p->m_jack_pos.beats_per_minute / (p->m_jack_pos.frame_rate * 60.0);
  
  p->m_jack_frame_last = p->m_jack_frame_current;

  p->m_jack_transport_state_last =
      p->m_jack_transport_state =
      state;
  
  
  
  switch ( state ){
      
      case JackTransportStopped:
          
          
          //printf( "[JackTransportStopped]\n" );
          break;
          
          
      case JackTransportRolling:
          
          //printf( "[JackTransportRolling]\n" );
          break;
          
          
          
      case JackTransportStarting:

          //printf( "[JackTransportStarting]\n" );
          p->inner_start( global_jack_start_mode );
          break;

      case JackTransportLooping:

          //printf( "[JackTransportLooping]" );
          break;

  }

  //printf( "starting frame[%d] tick[%8.2f]\n", p->m_jack_frame_current, p->m_jack_tick );
  
  print_jack_pos( pos );

  return true;

}



#endif


    void
perform::output_func(void)
{


    while(m_outputing){ 

        //printf ("waiting for signal\n");

        m_condition_var.lock();

        while ( !m_running ){

            m_condition_var.wait();

            /* if stopping, then kill thread */
            if ( !m_outputing )
                break;
        }

        m_condition_var.unlock();

        //printf( "signaled [%d]\n", m_playback_mode );

        /*******************************************************************

          AUDIO-TRANSPORT-PACED LOOKAHEAD SCHEDULER

          When the audio engine runs (and we are not slaved to JACK), the
          engine's sample clock is the only time authority: each iteration
          we read the transport sample position, convert position+lookahead
          to a musical tick through the shared tempo map, and play() every
          event up to that horizon.  Events leave play() carrying their
          ABSOLUTE tick, so the audio thread places each one at its exact
          sample offset -- wall-clock wake jitter only affects how early
          the ring is fed, never audible timing.

         *******************************************************************/

        if ( PatchKnob::app::audio_app_running() && !m_jack_running ){

            struct timespec pace;
            pace.tv_sec  = 0;
            pace.tv_nsec = c_thread_trigger_width_ms * 1000000L;

            long long start_tick = 0;

            /* if we are in the performance view, we care
               about starting from the offset */
            if ( m_playback_mode ){

                start_tick = m_starting_tick;
                set_orig_ticks( m_starting_tick );
            }

            /* inner_start already located the engine transport onto
               start_tick; the seek applies at a block boundary, so wait
               (bounded) until the transport reads near our start before
               computing the first horizon off a stale position */
            for ( int w = 0; w < 200 && m_running; w++ ){

                long long cur = PatchKnob::app::audio_app_sample_to_tick(
                        PatchKnob::app::audio_app_transport_sample() );

                if ( cur >= start_tick - (c_ppqn / 4) &&
                     cur <= start_tick + (c_ppqn / 4) )
                    break;

                nanosleep( &pace, NULL );
            }

            m_master_bus.init_clock( (long) start_tick );

            long long last_scheduled = start_tick - 1;

            /* the ENGINE owns looping now: publish the loop markers as ticks
               and the audio callback cycle-splits at the loop-end FRAME.  We
               never seek the transport for a wrap -- we only wrap the tick
               window we schedule.  (The old horizon-triggered seek jumped
               ~a-lookahead EARLY, so the loop tail's events were left for the
               next pass -- "clips playing outside their positions".) */
            bool      loop_sent  = false;
            long long loop_l = -1, loop_r = -1;
            /* the wrap branch schedules tail+head ONCE per pass; it must not
               re-fire while the horizon still hangs past the loop end waiting
               for the engine's sample-exact wrap to move the playhead back */
            bool      tail_done = false;

            while ( m_running ){

                /* keep the engine's loop in sync with ours (cheap when idle) */
                {
                    const bool want = m_looping && m_playback_mode;
                    const long long ll = get_left_tick(), rr = get_right_tick();
                    if ( want != loop_sent || ( want && ( ll != loop_l || rr != loop_r ) ) ){
                        PatchKnob::app::audio_app_set_loop_ticks( ll, rr, want ? 1 : 0 );
                        loop_sent = want; loop_l = ll; loop_r = rr;
                    }
                }

                const long long s = PatchKnob::app::audio_app_transport_sample();

                /* the UI playhead reads the SAME clock the audio renders */
                m_tick = (long) PatchKnob::app::audio_app_sample_to_tick( s );

                /* two audio blocks + the configured margin of early feed */
                const long long lookahead_samples =
                    2 * (long long) PatchKnob::app::audio_app_buffer_size()
                    + (long long)( 0.001 * c_thread_trigger_lookahead_ms
                                   * PatchKnob::app::audio_app_sample_rate() );

                long long horizon_tick = PatchKnob::app::audio_app_sample_to_tick(
                        s + lookahead_samples );

                if ( m_looping && m_playback_mode &&
                     horizon_tick >= get_right_tick() ){

                    if ( !tail_done ){

                        /* schedule the tail right up to the loop end... */
                        long long leftover_tick = horizon_tick - get_right_tick();
                        if ( leftover_tick > get_right_tick() - get_left_tick() )
                            leftover_tick = 0;   /* degenerate/moved markers */

                        play( get_right_tick() - 1 );
                        reset_sequences();
                        set_orig_ticks( get_left_tick() );

                        /* ...then continue from the loop start.  The transport
                           itself wraps sample-exactly inside the audio
                           callback; our tick window simply follows the music. */
                        last_scheduled = get_left_tick() + leftover_tick;
                        play( (long) last_scheduled );
                        m_master_bus.clock( (long) last_scheduled );
                        tail_done = true;
                    }
                    /* else: tail + head are queued; wait for the engine wrap
                       to pull the playhead (and thus the horizon) back */
                }
                else {

                    tail_done = false;    /* horizon back inside the loop */

                    if ( horizon_tick > last_scheduled ){

                        play( (long) horizon_tick );
                        m_master_bus.clock( (long) horizon_tick );
                        last_scheduled = horizon_tick;

                        /* play() published the horizon; snap the display back
                           to the real transport position */
                        m_tick = (long) PatchKnob::app::audio_app_sample_to_tick( s );
                    }
                }

                nanosleep( &pace, NULL );
            }

            /* leaving the scheduler: the engine loop dies with it */
            if ( loop_sent )
                PatchKnob::app::audio_app_set_loop_ticks( 0, 0, 0 );

            m_tick = 0;
            m_master_bus.flush( );
            m_master_bus.stop();

            continue;
        }

        /* ------- fallback: no audio engine (or JACK slave/master) -------
           legacy wall-clock integration, paced off CLOCK_MONOTONIC */

        /* begning time */
        struct timespec last;
        /* current time */
        struct timespec current;

        struct timespec stats_loop_start;
        struct timespec stats_loop_finish;


        /* difference between last and current */
        struct timespec delta;

        /* tick and tick fraction */
        double current_tick   = 0.0;
        double total_tick   = 0.0;
        double clock_tick = 0.0;

        long stats_total_tick = 0;

        long stats_loop_index = 0;
        long stats_min = 0x7FFFFFFF;
        long stats_max = 0;
        long stats_avg = 0;
        long stats_last_clock_us = 0;
        long stats_clock_width_us = 0;

        long stats_all[100];
        long stats_clock[100];

        bool jack_stopped = false;
        bool dumping = false;

        bool init_clock = true;

        double jack_ticks_converted = 0.0;
        double jack_ticks_converted_last = 0.0;
        double jack_ticks_delta = 0.0;

        for( int i=0; i<100; i++ ){
            stats_all[i] = 0;
            stats_clock[i] = 0;
        }

        /* if we are in the performance view, we care 
           about starting from the offset */
        if ( m_playback_mode && !m_jack_running){

            current_tick = m_starting_tick;
            clock_tick = m_starting_tick;
            set_orig_ticks( m_starting_tick ); 

        }


        int ppqn = m_master_bus.get_ppqn();
        /* get start time position */
        clock_gettime(CLOCK_MONOTONIC, &last);

        if ( global_stats )
            stats_last_clock_us= (last.tv_sec * 1000000) + (last.tv_nsec / 1000);




        while( m_running ){

            /************************************

              Get delta time ( current - last )
              Get delta ticks from time
              Add to current_ticks
              Compute prebuffer ticks
              play from current tick to prebuffer

             **************************************/

            if ( global_stats ){
                clock_gettime(CLOCK_MONOTONIC, &stats_loop_start);
            }


            /* delta time */
            clock_gettime(CLOCK_MONOTONIC, &current);
            delta.tv_sec  =  (current.tv_sec  - last.tv_sec  );
            delta.tv_nsec =  (current.tv_nsec - last.tv_nsec );
            long delta_us = (delta.tv_sec * 1000000) + (delta.tv_nsec / 1000);


            /* delta time to ticks */
            /* bpm -- fractional, in DOUBLE precision end-to-end */
            double bpm  = m_master_bus.get_bpm();

            /* get delta ticks, delta_ticks_f is in 1000th of a tick */
            double delta_tick   =  (double) (bpm * ppqn * (delta_us/60000000.0) );

            //printf ( "delta_tick[%ld.%03ld]\n", delta_tick, delta_tick_f  );
#ifdef JACK_SUPPORT

            // no init until we get a good lock
            
            if ( m_jack_running ){
               
                init_clock = false;

                m_jack_transport_state = jack_transport_query( m_jack_client, &m_jack_pos );
                m_jack_frame_current =  jack_get_current_transport_frame( m_jack_client );
 
                if ( m_jack_transport_state_last  ==  JackTransportStarting &&
                     m_jack_transport_state       == JackTransportRolling ){

                    m_jack_frame_last = m_jack_frame_current;


                    printf ("[Start Playback]\n" );
                    dumping = true;
                    m_jack_tick =
                        m_jack_pos.frame *
                        m_jack_pos.ticks_per_beat *
                        m_jack_pos.beats_per_minute / (m_jack_pos.frame_rate * 60.0);


                    /* convert ticks */
                    jack_ticks_converted =
                        m_jack_tick * ((double) c_ppqn /
                                (m_jack_pos.ticks_per_beat *
                                 m_jack_pos.beat_type / 4.0  ));

                    set_orig_ticks( (long) jack_ticks_converted );
                    current_tick = clock_tick = total_tick = jack_ticks_converted_last = jack_ticks_converted;
                    init_clock = true;

                    if ( m_looping && m_playback_mode ){

                        //printf( "left[%lf] right[%lf]\n", (double) get_left_tick(), (double) get_right_tick() );
                        
                        if ( current_tick >= get_right_tick() ){

                            while ( current_tick >= get_right_tick() ){

                                double size = get_right_tick() - get_left_tick();
                                current_tick = current_tick - size;
                                
                                //printf( "> current_tick[%lf]\n", current_tick );
                            }        
                            reset_sequences();
                            set_orig_ticks( (long)current_tick );
                        }
                    }
                }

                if ( m_jack_transport_state_last  ==  JackTransportRolling &&
                        m_jack_transport_state  == JackTransportStopped ){

                    m_jack_transport_state_last = JackTransportStopped;
                    //printf ("[Stop Playback]\n" );
                    jack_stopped = true;
                }

                //-----  Jack transport is Rolling Now ---------

                /* transport is in a sane state if dumping == true */
                if ( dumping )
                {
                    m_jack_frame_current =  jack_get_current_transport_frame( m_jack_client );

                    //printf( " frame[%7d]", m_jack_pos.frame );
                    //printf( " current_transport_frame[%7d]", m_jack_frame_current );

                    // if we are moving ahead
                    if ( (m_jack_frame_current > m_jack_frame_last)){


                        m_jack_tick +=
                            (m_jack_frame_current - m_jack_frame_last)  *
                            m_jack_pos.ticks_per_beat *
                            m_jack_pos.beats_per_minute / (m_jack_pos.frame_rate * 60.0);


                        //printf ( "m_jack_tick += (m_jack_frame_current[%lf] - m_jack_frame_last[%lf]) *\n",
                        //        (double) m_jack_frame_current, (double) m_jack_frame_last );
                        //printf(  "m_jack_pos.ticks_per_beat[%lf] * m_jack_pos.beats_per_minute[%lf] / \n(m_jack_pos.frame_rate[%lf] * 60.0\n", (double) m_jack_pos.ticks_per_beat, (double) m_jack_pos.beats_per_minute, (double) m_jack_pos.frame_rate);
           
                        
                        m_jack_frame_last = m_jack_frame_current;
                    }

                    /* convert ticks */
                    jack_ticks_converted =
                        m_jack_tick * ((double) c_ppqn /
                                (m_jack_pos.ticks_per_beat * m_jack_pos.beat_type / 4.0  ));

                    //printf ( "jack_ticks_conv[%lf] = \n",  jack_ticks_converted ); 
                    //printf ( "    m_jack_tick[%lf] * ((double) c_ppqn[%lf] / \n", m_jack_tick, (double) c_ppqn );
                    //printf ( "   (m_jack_pos.ticks_per_beat[%lf] * m_jack_pos.beat_type[%lf] / 4.0  )\n",
                    //        m_jack_pos.ticks_per_beat, m_jack_pos.beat_type );
                            
                          

          
                    jack_ticks_delta = jack_ticks_converted - jack_ticks_converted_last;

                    clock_tick     += jack_ticks_delta;
                    current_tick   += jack_ticks_delta;
                    total_tick     += jack_ticks_delta;

                    m_jack_transport_state_last = m_jack_transport_state;
                    jack_ticks_converted_last = jack_ticks_converted;

                    /* printf( "current_tick[%lf] delta[%lf]\n", current_tick, jack_ticks_delta ); */


                    long ptick, pbeat, pbar;

                    pbar  = (long) ((long) m_jack_tick / (m_jack_pos.ticks_per_beat *  m_jack_pos.beats_per_bar ));

                    pbeat = (long) ((long) m_jack_tick % (long) (m_jack_pos.ticks_per_beat *  m_jack_pos.beats_per_bar ));
                    pbeat = pbeat / (long) m_jack_pos.ticks_per_beat;

                    ptick = (long) m_jack_tick % (long) m_jack_pos.ticks_per_beat;


                    //printf( " bbb [%2d:%2d:%4d]", pbar+1, pbeat+1, ptick );
                    //printf( " bbb [%2d:%2d:%4d]", m_jack_pos.bar, m_jack_pos.beat, m_jack_pos.tick );

                    /*double jack_tick = (m_jack_pos.bar-1) * (m_jack_pos.ticks_per_beat *  m_jack_pos.beats_per_bar ) +
                        (m_jack_pos.beat-1) * m_jack_pos.ticks_per_beat + m_jack_pos.tick;*/

                    //printf( " jtick[%8.3f]", m_jack_tick );
                    //printf( " mtick[%8.3f]", jack_tick );

                    //printf( " delta[%8.3f]", m_jack_tick - jack_tick );

                    //printf( "\n");

                } /* end if dumping / sane state */

            } /* if jack running */
            else 
            {
#endif
                /* default if jack is not compiled in, or not running */
                /* add delta to current ticks */
                clock_tick     += delta_tick;
                current_tick   += delta_tick;
                total_tick     += delta_tick;
                dumping = true;

#ifdef JACK_SUPPORT
            }
#endif

            /* init_clock will be true when we run for the first time, or
             * as soon as jack gets a good lock on playback */
            
            if( init_clock ) 
            {
                m_master_bus.init_clock( (long)clock_tick );
                init_clock = false;
            }
            
            if ( dumping )
            {
                if ( m_looping && m_playback_mode )
                {
                    if ( current_tick >= get_right_tick() )
                    {
                        double leftover_tick = current_tick - (get_right_tick());

                        play( get_right_tick() - 1 );
                        reset_sequences();

                        set_orig_ticks( get_left_tick() );
                        current_tick = (double) get_left_tick() + leftover_tick;
                    }
                }

                /* play -- round the double tick window to nearest instead
                   of truncating a whole tick away */
                play( (long) llround( current_tick ) );
                //printf( "play[%d]\n", current_tick );

                /* publish the live transport position so the UI playhead
                   (perform::get_tick) tracks playback in real time. */
                m_tick = (long) llround( current_tick );

                /* midi clock */
                m_master_bus.clock( (long) llround( clock_tick ) );


                if ( global_stats ){	  

                    while ( stats_total_tick <= total_tick ){

                        /* was there a tick ? */
                        if ( stats_total_tick % (c_ppqn / 24) == 0 ){

                            long current_us = (current.tv_sec * 1000000) + (current.tv_nsec / 1000);
                            stats_clock_width_us = current_us - stats_last_clock_us;
                            stats_last_clock_us = current_us;

                            int index = stats_clock_width_us / 300;
                            if ( index >= 100 ) index = 99; 
                            stats_clock[index]++;

                        }
                        stats_total_tick++;
                    }
                }
            }

            /***********************************

              Figure out how much time 
              we need to sleep, and do it

             ************************************/

            /* set last */
            last = current;

            clock_gettime(CLOCK_MONOTONIC, &current);
            delta.tv_sec  =  (current.tv_sec  - last.tv_sec  );
            delta.tv_nsec =  (current.tv_nsec - last.tv_nsec );
            long elapsed_us = (delta.tv_sec * 1000000) + (delta.tv_nsec / 1000);
            //printf( "elapsed_us[%ld]\n", elapsed_us );

            /* now, we want to trigger every c_thread_trigger_width_ms,
               and it took us delta_us to play() */ 

            delta_us = (c_thread_trigger_width_ms * 1000) - elapsed_us;
            //printf( "sleeping_us[%ld]\n", delta_us );


            /* check midi clock adjustment: wake exactly on the next midi
               clock boundary if it lands sooner than the trigger width --
               use the FRACTIONAL distance to the boundary, and never
               lengthen the pacing sleep */

            double frac_tick = fmod( total_tick, c_ppqn / 24.0 );
            double next_clock_delta_us =
                ((c_ppqn / 24.0) - frac_tick) * 60000000.0 / c_ppqn / bpm;

            if ( next_clock_delta_us < (double) delta_us ){
                delta_us = (long)next_clock_delta_us;
            }


            if ( delta_us > 0.0 ){

                delta.tv_sec =  (delta_us / 1000000);
                delta.tv_nsec = (delta_us % 1000000) * 1000;

                //printf("sleeping() ");
                nanosleep( &delta, NULL );

            } 

            else {

                if ( global_stats )
                    printf ("underrun\n" );
            }

            if ( global_stats ){	  
                clock_gettime(CLOCK_MONOTONIC, &stats_loop_finish);
            }

            if ( global_stats ){

                delta.tv_sec  =  (stats_loop_finish.tv_sec  - stats_loop_start.tv_sec  );
                delta.tv_nsec =  (stats_loop_finish.tv_nsec - stats_loop_start.tv_nsec );
                long delta_us = (delta.tv_sec * 1000000) + (delta.tv_nsec / 1000);

                int index = delta_us / 100;
                if ( index >= 100  ) index = 99;

                stats_all[index]++;

                if ( delta_us > stats_max )
                    stats_max = delta_us;
                if ( delta_us < stats_min )
                    stats_min = delta_us;

                stats_avg += delta_us;

                stats_loop_index++;	  

                if ( stats_loop_index > 200 ){

                    stats_loop_index = 0;
                    stats_avg /= 200;

                    printf( "stats_avg[%ld]us stats_min[%ld]us stats_max[%ld]us\n", stats_avg, stats_min, stats_max );

                    stats_min = 0x7FFFFFFF;
                    stats_max = 0;
                    stats_avg = 0;

                }

            }

            if (jack_stopped )
                inner_stop();
        }


        if ( global_stats ){

            printf ( "\n\n-- trigger width --\n" );
            for ( int i=0; i<100; i++ ){
                printf( "[%3d][%8ld]\n", i * 100, stats_all[i] );
            }
            printf ( "\n\n-- clock width --\n" );
            double bpm  = m_master_bus.get_bpm();

            printf ( "optimal : [%d]us\n", (int)((c_ppqn / 24) * 60000000.0 / c_ppqn / bpm ));


            for ( int i=0; i<100; i++ ){
                printf( "[%3d][%8ld]\n", i * 300, stats_clock[i] );
            }


        }

        m_tick = 0;
        m_master_bus.flush( );
        m_master_bus.stop();

    }

    pthread_exit(0);
}




void*
input_thread_func(void *a_pef )
{

    /* set our performance */
    perform *p = (perform *) a_pef;
    assert(p);

    /* same treatment as the output thread: keep MIDI input responsive, and
       never kill the thread if the priority bump is refused */
#ifdef _WIN32
    if ( !SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL ))
        printf( "input_thread_func: SetThreadPriority(TIME_CRITICAL) failed\n" );
#endif

    p->input_func();

    return 0;
}

void
perform::handle_midi_control( int a_control, bool a_state )
{

    switch( a_control ){

        case c_midi_control_bpm_up:
            //printf ( "bpm up\n" );
            set_bpm( get_bpm() + 1 );   
            break;
            
        case c_midi_control_bpm_dn:
            //printf ( "bpm dn\n" );
            set_bpm( get_bpm() - 1 );
            break;
            
        case c_midi_control_ss_up:
            //printf ( "ss up\n" );
            set_screenset( get_screenset() + 1 );  
            break;
            
        case c_midi_control_ss_dn:
            //printf ( "ss dn\n" );
            set_screenset( get_screenset() - 1 );
            break;
            
        case c_midi_control_mod_replace:

            //printf ( "replace\n" );

            
            if ( a_state )
                set_sequence_control_status( c_status_replace );
            else
                unset_sequence_control_status( c_status_replace );
            
            break;
            
        case c_midi_control_mod_snapshot:

            //printf ( "snapshot\n" );

            if ( a_state )
                set_sequence_control_status( c_status_snapshot );
            else
                unset_sequence_control_status( c_status_snapshot );
            
            break;
            
        case c_midi_control_mod_queue:

            //printf ( "queue\n" );

            
            if ( a_state )
                set_sequence_control_status( c_status_queue );
            else
                unset_sequence_control_status( c_status_queue );
            break;
            
        default:
            break;
            
    }
}


void 
perform::input_func( void ){

    event ev;
    
    while( m_inputing ){
        
        if ( m_master_bus.poll_for_midi() > 0 ){
            
            do {
                
                if ( m_master_bus.get_midi_event( &ev ) ){
                    
                    /* filter system wide messages */
                    if ( ev.get_status() <= EVENT_SYSEX ){  
                        
                        if( global_showmidi) 
                            ev.print();
                        
                        /* is there a sequence set ? */
                        if ( m_master_bus.is_dumping( )) {
                            
                            ev.set_timestamp( m_tick );
                            
                            
                            /* dump to it */
                            (m_master_bus.get_sequence( ))->stream_event( &ev );
                            
                        }
                        
                        /* use it to control our sequencer */
                        else {

                            for ( int i=0; i<c_midi_controls; i++ ){
                                
                                unsigned char data[2] = {0,0};
                                unsigned char status = ev.get_status();
                                
                                ev.get_data( &data[0], &data[1] );
                                
                                if(  get_midi_control_toggle(i)->m_active &&
                                     status  == get_midi_control_toggle(i)->m_status &&
                                     data[0] == get_midi_control_toggle(i)->m_data ){
                                    
                                    if ( data[1] >= get_midi_control_toggle(i)->m_min_value &&
                                         data[1] <= get_midi_control_toggle(i)->m_max_value ){

                                        if ( i <  c_seqs_in_set )
                                            sequence_playing_toggle( i + m_offset );
                                    }
                                }
                                
                                if ( get_midi_control_on(i)->m_active && 
                                     status  == get_midi_control_on(i)->m_status &&
                                     data[0] == get_midi_control_on(i)->m_data ){
                                    
                                    if ( data[1] >= get_midi_control_on(i)->m_min_value &&
                                         data[1] <= get_midi_control_on(i)->m_max_value ){

                                        if ( i <  c_seqs_in_set )
                                            sequence_playing_on( i  + m_offset);
                                        else
                                            handle_midi_control( i, true );
                                        
                                    } else if (  get_midi_control_on(i)->m_inverse_active ){
                                        
                                        if ( i <  c_seqs_in_set )
                                            sequence_playing_off(  i + m_offset );
                                        else
                                            handle_midi_control( i, false );
                                    }
                                    
                                }
                                
                                if ( get_midi_control_off(i)->m_active &&
                                     status  == get_midi_control_off(i)->m_status &&
                                     data[0] == get_midi_control_off(i)->m_data ){
                                    
                                    if ( data[1] >= get_midi_control_off(i)->m_min_value &&
                                         data[1] <= get_midi_control_off(i)->m_max_value ){

                                        if ( i <  c_seqs_in_set )
                                            sequence_playing_off(  i + m_offset );
                                        else
                                            handle_midi_control( i, false );
                                        
                                    } else if ( get_midi_control_off(i)->m_inverse_active ){
                                        
                                        if ( i <  c_seqs_in_set )
                                            sequence_playing_on(  i + m_offset );
                                        else
                                            handle_midi_control( i, true );
                                        
                                    }
                                    
                                }
                                
                            }
                        }
                        
                    }
                    
                    if ( ev.get_status() == EVENT_SYSEX ){  
                        
                        if( global_showmidi ) 
                            ev.print();
                        
                        if( global_pass_sysex )
                            m_master_bus.sysex( &ev );   
                    }
                }
                
            } while ( m_master_bus.is_more_input( ));
        }
    }
    pthread_exit(0);
	
}



void 
perform::save_playing_state( void )
{
	for( int i=0; i<c_total_seqs; i++ ){
		
		if ( is_active(i) == true ){
			assert( m_seqs[i] );
			m_sequence_state[i] = m_seqs[i]->get_playing();
		}
		else
			m_sequence_state[i] = false;
	} 
}

void 
perform::restore_playing_state( void )
{
	for( int i=0; i<c_total_seqs; i++ ){
		
		if ( is_active(i) == true ){
			assert( m_seqs[i] );
			m_seqs[i]->set_playing( m_sequence_state[i] );
		}
	} 
}


void 
perform::set_sequence_control_status( int a_status )
{
	if ( a_status & c_status_snapshot ){
		save_playing_state(  );
	}

	m_control_status |= a_status;
}


void 
perform::unset_sequence_control_status( int a_status )
{
	if ( a_status & c_status_snapshot ){
		restore_playing_state(  );
	}

	m_control_status &= (~a_status);
}



void
perform::sequence_playing_toggle( int a_sequence )
{
    if ( is_active(a_sequence) == true ){
		assert( m_seqs[a_sequence] );

		if ( m_control_status & c_status_queue ){
			m_seqs[a_sequence]->toggle_queued();
		}
		else {

			if (  m_control_status & c_status_replace ){
				unset_sequence_control_status( c_status_replace );	
				off_sequences( );
			}
			
			m_seqs[a_sequence]->toggle_playing();

		}
    } 
}


void
perform::sequence_playing_on( int a_sequence )
{
    if ( is_active(a_sequence) == true ){
		assert( m_seqs[a_sequence] );
		m_seqs[a_sequence]->set_playing(true);
    } 
}


void
perform::sequence_playing_off( int a_sequence )
{
    if ( is_active(a_sequence) == true ){
		assert( m_seqs[a_sequence] );
		m_seqs[a_sequence]->set_playing(false);
    } 
}

#ifdef JACK_SUPPORT
void jack_timebase_callback(jack_transport_state_t state, jack_nframes_t nframes, 
	      jack_position_t *pos, int new_pos, void *arg)
{

    static double jack_tick;
    static jack_nframes_t last_frame;
    static jack_nframes_t current_frame;
    static jack_transport_state_t state_current;
    static jack_transport_state_t state_last;

    state_current = state;

    perform *p = (perform *) arg;
    current_frame = jack_get_current_transport_frame( p->m_jack_client );

    //printf( "jack_timebase_callback() [%d] [%d] [%d]", state, new_pos, current_frame);
    
    pos->valid = JackPositionBBT;
    pos->beats_per_bar = 4;
    pos->beat_type = 4;
    pos->ticks_per_beat = c_ppqn * 10;    
    pos->beats_per_minute = p->get_bpm();
    
    
    /* Compute BBT info from frame number.  This is relatively
     * simple here, but would become complex if we supported tempo
     * or time signature changes at specific locations in the
     * transport timeline. */

    // if we are in a new position
    if (  state_last    ==  JackTransportStarting &&
          state_current ==  JackTransportRolling ){

        //printf ( "Starting [%d] [%d]\n", last_frame, current_frame );
        
        jack_tick = 0.0;
        last_frame = current_frame;
    }

    if ( current_frame > last_frame ){

        double jack_delta_tick =
            (current_frame - last_frame) *
            pos->ticks_per_beat *
            pos->beats_per_minute / (pos->frame_rate * 60.0);
        
        jack_tick += jack_delta_tick;

        last_frame = current_frame;
    }
    
    long ptick = 0, pbeat = 0, pbar = 0;
    
    pbar  = (long) ((long) jack_tick / (pos->ticks_per_beat *  pos->beats_per_bar ));
    
    pbeat = (long) ((long) jack_tick % (long) (pos->ticks_per_beat *  pos->beats_per_bar ));
    pbeat = pbeat / (long) pos->ticks_per_beat;
    
    ptick = (long) jack_tick % (long) pos->ticks_per_beat;
    
    pos->bar = pbar + 1;
    pos->beat = pbeat + 1;
    pos->tick = ptick;;
    pos->bar_start_tick = pos->bar * pos->beats_per_bar *
        pos->ticks_per_beat;

    //printf( " bbb [%2d:%2d:%4d]\n", pos->bar, pos->beat, pos->tick );

    state_last = state_current;
 
}



void jack_shutdown(void *arg)
{
    perform *p = (perform *) arg;
    p->m_jack_running = false;
    
	printf("JACK shut down.\nJACK sync Disabled.\n");
}


 
void print_jack_pos( jack_position_t* jack_pos ){

    return;
  printf( "print_jack_pos()\n" );
  printf( "    bar  [%d]\n", jack_pos->bar  );
  printf( "    beat [%d]\n", jack_pos->beat );		
  printf( "    tick [%d]\n", jack_pos->tick );
  printf( "    bar_start_tick   [%lf]\n", jack_pos->bar_start_tick );
  printf( "    beats_per_bar    [%f]\n", jack_pos->beats_per_bar );
  printf( "    beat_type        [%f]\n", jack_pos->beat_type );
  printf( "    ticks_per_beat   [%lf]\n", jack_pos->ticks_per_beat );
  printf( "    beats_per_minute [%lf]\n", jack_pos->beats_per_minute );
  printf( "    frame_time       [%lf]\n", jack_pos->frame_time );
  printf( "    next_time        [%lf]\n", jack_pos->next_time );
}













#if 0

int main ( void )
{

  jack_client_t *client;

  /* become a new client of the JACK server */
  if ((client = jack_client_new("transport tester")) == 0) {
	fprintf(stderr, "jack server not running?\n");
	return 1;
  }

  jack_on_shutdown(client, jack_shutdown, 0);
  jack_set_sync_callback(client, jack_sync_callback, NULL);

  if (jack_activate(client)) {
	fprintf(stderr, "cannot activate client");
	return 1;
  }

  bool cond = false; /* true if we want to fail if there is already a master */
  if (jack_set_timebase_callback(client, cond, timebase, NULL) != 0){
	printf("Unable to take over timebase or there is already a master.\n");
	exit(1);
  }

  jack_position_t pos;

  pos.valid = JackPositionBBT;

  pos.bar = 0;
  pos.beat = 0;
  pos.tick = 0;

  pos.beats_per_bar = time_beats_per_bar;
  pos.beat_type = time_beat_type;
  pos.ticks_per_beat = time_ticks_per_beat;
  pos.beats_per_minute = time_beats_per_minute;
  pos.bar_start_tick = 0.0; 


  //jack_transport_reposition( client, &pos );

  jack_transport_start (client);

  //void jack_transport_stop (jack_client_t *client);

  int bob;
  scanf ("%d", &bob);

 
  jack_transport_stop (client);
  jack_release_timebase(client);
  jack_client_close(client);

  return 0;
}

#endif





#endif 
