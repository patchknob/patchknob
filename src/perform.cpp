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
#include <algorithm>
#include <cassert>
#include "perform.h"
#include "midibus.h"
#include "event.h"
#include "audio_app.h"        /* engine-owned tempo: set_bpm funnels into it */
#include "keycodes_compat.h"  /* legacy default keybinding values */
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <vector>

/*  SCHEDULER TRACE (diagnostics only; see sdlui PATCHKNOB_LOOPREPRO).
    Records one row per output_func iteration so a loop misbehaviour can be
    read back as data -- which branch ran, what the horizon was, and what the
    engine's playhead said -- instead of being guessed at from the audio.
    Written ONLY by the output thread; read only after it has stopped.        */
bool                        g_loop_trace_on = false;
std::vector<loop_trace_rec> g_loop_trace;

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

        /*  The four "was active" flags were left UNINITIALISED.  is_dirty_main
            and friends return them (and clear them) for every slot the UI asks
            about, so a fresh perform reported random slots as having just been
            deleted and the pattern grid redrew garbage cells on its first
            frame. */
        m_was_active_main[i]  = false;
        m_was_active_edit[i]  = false;
        m_was_active_perf[i]  = false;
        m_was_active_names[i] = false;
    }

    m_scale_master_seq = -1;

    m_running = false;
    m_looping = false;
    m_inputing = true;
    m_outputing = true;
    /* ctor: no other thread exists yet, but store through the atomic API
       anyway so every access to these members goes one way. */
    m_tick.store( 0, std::memory_order_relaxed );

    /*  ALSO UNINITIALISED, and read before anything ever wrote it:
        reset_sequences() branches on it ("if ( !m_playback_mode ) restore the
        pattern's playing state"), and with the count-in enabled that runs
        BEFORE the first start() -- i.e. before set_playback_mode() has been
        called at all.  On an indeterminate value the count-in either kept or
        silently dropped every pattern's playing state.  A fresh perform is in
        LIVE (pattern) mode until told otherwise. */
    m_playback_mode = false;

    thread_trigger_width_ms = c_thread_trigger_width_ms;

    m_left_tick.store( 0, std::memory_order_relaxed );
    m_right_tick.store( c_ppqn * 16, std::memory_order_relaxed );
    m_starting_tick.store( 0, std::memory_order_relaxed );
    
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

    m_out_thread_launched = false;
    m_in_thread_launched = false;
    
}

void
perform::init( void )
{
    m_master_bus.init( );
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
    /*  The marker PAIR cannot be made atomic without a lock, and the
        scheduler poll reads it lock-free, so decide the whole new geometry in
        locals first and then publish the member that GROWS the window before
        the one that shrinks it.  That way every intermediate the scheduler
        can observe still satisfies left < right -- it never sees a degenerate
        or inverted loop and never pushes one to the engine. */
    /*  Same minimum-window rule as set_right_tick(): pushing the LEFT marker up
        to (or past) the right one used to shove the right marker a whole BAR
        further out, which silently widened a deliberately short loop.  Grow it
        by the minimum window instead, so a short loop stays short. */
    const long minWindow = c_ppqn / 4;          /* a sixteenth */
    const long new_left  = a_tick < 0 ? 0 : a_tick;
    const long cur_right = m_right_tick.load( std::memory_order_relaxed );
    const long new_right = ( cur_right - new_left < minWindow )
                         ? new_left + minWindow
                         : cur_right;

    if ( new_right != cur_right )
        m_right_tick.store( new_right, std::memory_order_relaxed );

    m_left_tick.store( new_left, std::memory_order_relaxed );
    m_starting_tick.store( new_left, std::memory_order_relaxed );
}

long
perform::get_left_tick( void )
{
    return m_left_tick.load( std::memory_order_relaxed );
}


void
perform::set_starting_tick( long a_tick )
{
    m_starting_tick.store( a_tick, std::memory_order_relaxed );
}

long
perform::get_starting_tick( void )
{
    return m_starting_tick.load( std::memory_order_relaxed );
}

void
perform::set_right_tick( long a_tick )
{
    /*  seq24 wrote `if (a_tick >= c_ppqn*4)` here and did NOTHING otherwise, so
        dragging the right loop brace anywhere inside the first bar was silently
        ignored -- the brace snapped back with no feedback and no way to loop a
        half bar from the top of the song.  That was a limit on the marker's
        ABSOLUTE position, which was never the real constraint: the engine
        handles short loops fine (the arrange harness audits a one-beat loop and
        a loop shorter than the scheduler's own lookahead, both clean over 25
        wraps).  What actually has to hold is a minimum WINDOW WIDTH, so the
        scheduler never sees a degenerate or inverted loop.  Clamp to that
        instead of refusing the edit. */
    const long minWindow = c_ppqn / 4;          /* a sixteenth */

    long new_right = a_tick;
    if ( new_right < minWindow )
        new_right = minWindow;                  /* a loop cannot end at/before 0 */

    /* same publication order argument as set_left_tick(): move the marker that
       shrinks the window LAST, so every intermediate the lock-free scheduler
       can observe still satisfies left < right */
    const long cur_left = m_left_tick.load( std::memory_order_relaxed );

    if ( new_right - cur_left < minWindow ){
        long new_left = new_right - minWindow;
        if ( new_left < 0 ) new_left = 0;
        m_left_tick.store( new_left, std::memory_order_relaxed );
        m_starting_tick.store( new_left, std::memory_order_relaxed );
    }

    m_right_tick.store( new_right, std::memory_order_relaxed );
}

long
perform::get_right_tick( void )
{
    return m_right_tick.load( std::memory_order_relaxed );
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

/*  TEMPO HAS EXACTLY ONE HOME: the audio engine's kitchensink tempo map.

    set_bpm is the single write path (UI, file load, hotkeys, MIDI control all
    funnel here) and get_bpm reads that same map back.  There used to be a
    second copy in mastermidibus::m_bpm which was WRITTEN here but READ by the
    output thread's fallback pacing, JACK timebase and project save -- so any
    tempo set through audio_app_set_tempo directly (and any clamp disagreement:
    we clamp 20..500, the engine 20..999) left the two disagreeing about the
    speed of the same song.  The mirror is gone; this is now the only truth. */
void
perform::set_bpm(double a_bpm)
{
    if ( a_bpm < 20.0 )  a_bpm = 20.0;
    if ( a_bpm > 500.0 ) a_bpm = 500.0;

    PatchKnob::app::audio_app_set_tempo( a_bpm );
}

double
perform::get_bpm( )
{
    /* Reads the engine tempo map's derived cache -- a plain atomic load, valid
       (120.0) even before audio_app_init(), so this is safe from the UI, the
       output thread and the JACK timebase callback alike. */
    return PatchKnob::app::audio_app_tempo( );
}

void
perform::delete_sequence( int a_num )
{
    if ( a_num < 0 || a_num >= c_max_sequence )
        return;

    sequence *doomed = m_seqs[a_num];

    /*  PUBLISH "gone", THEN drop the pointer.  Every reader (perform::play,
        set_orig_ticks, off_sequences, reset_sequences, get_max_trigger, the
        UI) grabs m_seqs[i] and tests is_active(i); clearing active first and
        the slot second means a reader that saw active==true necessarily
        loaded the pointer before it was cleared.  Readers now latch the
        pointer and check it (see perform::play) instead of assert()ing it,
        because assert compiles out in Release -- which is exactly where the
        NULL deref happened. */
	set_active(a_num, false);
    m_seqs[a_num] = NULL;

    if ( doomed != NULL ){

        doomed->set_playing( false );

		/* RETIRE, don't free: the output thread / UI may still hold this
		   pointer for an instant (use-after-free -> divide-by-zero crash in
		   get_last_tick).  gc_graveyard() frees it a couple of UI frames later.

		   This used to be skipped entirely when the pattern had its editor
		   open -- but set_active(false) above ran anyway, so the slot was
		   emptied while the sequence itself was never retired and never freed:
		   it leaked for the rest of the session, and the open editor went on
		   editing an orphan that no save would ever see.  Retire it either
		   way; gc_graveyard() holds the free back for as long as something is
		   still editing it. */
		m_seq_graveyard.push_back( doomed );
		m_seq_graveyard_age.push_back( 0 );
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
        /* something still has it OPEN in an editor: it is retired (out of the
           slot, silent, not scheduled) but freeing it would pull the rug from
           under that editor.  Hold the free until the editor lets go. */
        if ( m_seq_graveyard[i] != NULL && m_seq_graveyard[i]->get_editing() )
        {
            ++i;
            continue;
        }

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
    
    m_tick.store( a_tick, std::memory_order_relaxed );
    /* SCALE-MASTER / SCALE-FOLLOW: resolve the active master once per tick into
       plain scalars (each getter takes/releases the master's own mutex, no
       nested locking), then push the snapshot to each follower before its
       play() runs.  See docs/scale-follow.md section 2. */
    bool master_on = false;
    int  master_key = 0, master_scale = c_scale_off;
    int  scale_m = m_scale_master_seq;
    {
        /*  LATCH THE POINTER, THEN TEST is_active().  delete_sequence() clears
            active before it clears the slot, so a pointer read before an
            is_active() that came back true cannot be the NULL it was about to
            become -- and a retired sequence stays alive in the graveyard for a
            couple of UI frames on top of that.  The old code tested is_active()
            and then dereferenced m_seqs[] three times, guarded only by an
            assert(): compiled out in Release, which is where the crash was. */
        sequence *m = ( scale_m >= 0 && scale_m < c_max_sequence )
                      ? m_seqs[scale_m] : NULL;

        if ( m != NULL && is_active(scale_m) &&
             m->get_playing() && m->get_scale_master() ){
            master_on    = true;
            master_key   = m->get_master_key();
            master_scale = m->get_master_scale();
        }
    }

    for (int i=0; i< c_max_sequence; i++ ){

		sequence *s = m_seqs[i];

		if ( s != NULL && is_active(i) ){

				/* a follower snaps only if it is not the master itself and
				   has its follow flag set */
				bool follow = master_on && ( i != m_scale_master_seq )
				              && s->get_follows_master();
				s->set_master_scale_context( follow, master_key, master_scale );


			if ( s->get_queued() &&
				 s->get_queued_tick() <= a_tick ){

				const long qt = s->get_queued_tick();

				s->play( qt - 1, m_playback_mode );

				/*  RELEASE AT THE QUEUED TICK, not at "now".

				    a_tick here is the SCHEDULER HORIZON -- a lookahead
				    (~15 ms) ahead of what the listener is hearing -- so a
				    queued MUTE that went through the plain toggle_playing()
				    handed off_playing_notes() the default tick of -1, i.e.
				    "flush immediately".  The notes were cut up to a whole
				    lookahead early, and because the note-offs jumped the
				    ring ahead of the note-ons that were already queued for
				    the same pattern, a voice could be left hanging.
				    sequence::play_triggered() already releases at the exact
				    boundary tick (cover->m_tick_end, the last tick it
				    played); this is the same boundary -- the last tick the
				    pattern sounded before the queue took effect. */
				s->toggle_playing( qt - 1 );
			}

			s->play( a_tick, m_playback_mode );
		}
    }
	
    /* flush the bus */
    m_master_bus.flush();
}

void 
perform::set_orig_ticks( long a_tick  )
{
    for (int i=0; i< c_max_sequence; i++ ){

	/* latch, then test -- see perform::play() */
	sequence *s = m_seqs[i];

	if ( s != NULL && is_active(i) ){
	    s->set_orig_tick( a_tick );
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
    /* snapshot the marker pair ONCE: it used to be re-read three times, so a
       concurrent drag could hand the loop below a mismatched L/R */
    const long left  = get_left_tick();
    const long right = get_right_tick();

    if ( left < right ){

	long distance = right - left;

	for (int i=0; i< c_max_sequence; i++ ){

	    if ( is_active(i) == true ){
		assert( m_seqs[i] );
		m_seqs[i]->move_triggers( left, distance, a_direction );
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


void
perform::pop_trigger_redo( void )
{
    for (int i=0; i< c_max_sequence; i++ ){

        if ( is_active(i) == true ){
            assert( m_seqs[i] );
            m_seqs[i]->pop_trigger_redo( );
        }
    }
}


/* copies between L and R -> R */
void
perform::copy_triggers( )
{
    /* snapshot the marker pair ONCE -- see move_triggers() */
    const long left  = get_left_tick();
    const long right = get_right_tick();

    if ( left < right ){

	long distance = right - left;

	for (int i=0; i< c_max_sequence; i++ ){

	    if ( is_active(i) == true ){
		assert( m_seqs[i] );
		m_seqs[i]->copy_triggers( left, distance );
	    }
	}
    }
}



/*  JACK IS GONE.

    seq24 could slave its transport to JACK, and the port carried the whole of
    that machinery: init_jack/deinit_jack/start_jack/stop_jack/position_jack,
    the sync/timebase/shutdown callbacks, and an m_jack_running flag that gated
    a dozen branches across this file.  Every line of it lived inside
    #ifdef JACK_SUPPORT, and NOTHING in this tree can define that symbol:
    src/config.h ships it undefined, CMakeLists.txt -- the only build system
    present, since the autotools leftovers have no configure script -- never
    mentions JACK, and no JACK headers are vendored.  So the code could not
    compile even if it were reached, m_jack_running was a constant false, and
    every branch it guarded was unreachable in every build.

    Removed rather than kept: 530-odd lines of a second, dead transport in the
    file that hosts the live scheduler is a standing invitation to reason about
    the wrong one.  perform::start() and stop() were pure pass-throughs behind
    that always-false flag and now say so.  If JACK is ever wanted back it
    belongs on the engine transport in src/audio_app.cpp, not on this legacy
    layer, which no longer owns the clock.  The global_with_jack_* option
    variables are deliberately left alone: optionsfile.cpp still reads and
    writes them, so dropping them would change the on-disk options format for
    no gain.  */
void 
perform::start( bool a_state )
{
    inner_start( a_state );
}



void 
perform::stop( )
{
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

            long long start_tick = a_state ? (long long) get_starting_tick() : 0;

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

		/* latch, then test -- see perform::play() */
		sequence *s = m_seqs[i];

		if ( s != NULL && is_active(i) ){
			s->set_playing( false );

		} 
    }
}




void 
perform::reset_sequences( long release_tick, bool loop_boundary )
{
    for (int i=0; i< c_max_sequence; i++ ){

		/* latch, then test -- see perform::play() */
		sequence *s = m_seqs[i];

		if ( s != NULL && is_active(i) ){

                        bool state = s->get_playing();

			if(loop_boundary)s->queue_loop_note_offs();
			else s->off_playing_notes( release_tick );
			s->set_playing( false );
			s->zero_markers( );

                        if( !m_playback_mode )
                            s->set_playing( state );
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

	/* latch, then test -- see perform::play() */
	sequence *s = m_seqs[i];

	if ( s != NULL && is_active(i) ){

	    t = s->get_max_trigger( );  
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

        if ( PatchKnob::app::audio_app_running() ){

            struct timespec pace;
            pace.tv_sec  = 0;
            pace.tv_nsec = c_thread_trigger_width_ms * 1000000L;

            long long start_tick = 0;

            /* if we are in the performance view, we care
               about starting from the offset */
            if ( m_playback_mode ){

                /* one load, used for both -- the UI can move the marker
                   between two reads */
                const long st = get_starting_tick();
                start_tick = st;
                set_orig_ticks( st );
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
            unsigned long long observed_wrap_generation =
                PatchKnob::app::audio_app_loop_wrap_generation();
            /*  Same treatment for LOCATES -- see the handshake below.  Snapshot
                it here so the locates that happened while the transport was
                stopped (the user parking the playhead before pressing PLAY)
                are not replayed as a discontinuity on the first poll: this
                thread is starting AT that position already. */
            unsigned long long observed_locate_generation =
                PatchKnob::app::audio_app_locate_generation();
            /*  Last transport SAMPLE this thread saw, so a LOCATE (the ruler
                scrub, the rewind button, "return to start") can be noticed --
                see the backward-seek handshake below.  -1 == nothing seen yet. */
            long long prev_transport_sample = -1;
            unsigned long long edit_revision[c_max_sequence] = {};
            for(int i=0;i<c_max_sequence;++i)
                if(is_active(i))edit_revision[i]=m_seqs[i]->edit_revision();

            while ( m_running ){

                /* keep the engine's loop in sync with ours (cheap when idle) */
                bool loop_config_changed=false;
                {
                    const bool want = m_looping && m_playback_mode;
                    const long long ll = get_left_tick(), rr = get_right_tick();
                    if ( want != loop_sent || ( want && ( ll != loop_l || rr != loop_r ) ) ){
                        loop_config_changed=true;
                        PatchKnob::app::audio_app_set_loop_ticks( ll, rr, want ? 1 : 0 );
                        loop_sent = want; loop_l = ll; loop_r = rr;
                    }
                }

                /*  Read the wrap counter on BOTH sides of the transport
                    sample: if a loop wrap lands between the two reads, `s`
                    belongs to a different side of the wrap than the counter
                    does, and the backward-seek test below must not mistake
                    that for a locate. */
                const unsigned long long wrap_generation_pre =
                    PatchKnob::app::audio_app_loop_wrap_generation();
                const long long s = PatchKnob::app::audio_app_transport_sample();
                if(loop_config_changed){
                    // Old-loop tail/head messages and release masks have no
                    // meaning under new marker geometry. Rebuild from the
                    // audible tick instead of carrying the old latch/window.
                    PatchKnob::app::audio_app_invalidate_future_schedule();
                    const long long now=PatchKnob::app::audio_app_sample_to_tick(s);
                    set_orig_ticks((long)now);last_scheduled=now-1;tail_done=false;
                    observed_wrap_generation=
                        PatchKnob::app::audio_app_loop_wrap_generation();
                }

                /* Polling `s < previous_s` can MISS a whole wrap when the audio
                   callback wraps and advances again between scheduler polls.
                   Once missed, tail_done remains true forever on short loops
                   and every later clip pass is silent.  Consume the callback's
                   monotonic generation instead: no wrap can be aliased away. */
                const unsigned long long wrap_generation =
                    PatchKnob::app::audio_app_loop_wrap_generation();
                bool wrapped = ( wrap_generation != wrap_generation_pre );
                if(wrap_generation!=observed_wrap_generation){
                    observed_wrap_generation=wrap_generation;
                    const bool tail_was_queued = tail_done;
                    tail_done=false;
                    wrapped=true;

                    if( !tail_was_queued && m_looping && m_playback_mode ){
                        /*  MISSED TAIL WINDOW.

                            The tail branch below only runs while the horizon
                            hangs past the loop end -- a window ONE LOOKAHEAD
                            wide (~15-40 ms) at the end of each pass.  This
                            thread contends on every sequence's recursive mutex
                            with the GUI (drawing takes the same locks), so a
                            stall covering that whole window is routine on a
                            busy session.  The engine still wraps sample-exactly,
                            but nothing here scheduled the ending pass's tail or
                            the new pass's head: last_scheduled is stranded near
                            the OLD pass's right edge, `horizon_tick >
                            last_scheduled` stays false for almost the entire
                            new pass, and the else-branch emits NOTHING -- one
                            whole silent pass that self-heals just before its
                            end.  Consecutive stalls give consecutive silent
                            passes: the reported "silence for a few loops, then
                            it comes back".  tail_done is true at a wrap exactly
                            when the tail branch ran for the pass that just
                            ended, so its absence is the precise trigger.

                            Recover the way the locate handshake does: rebuild
                            from the AUDIBLE tick.  Rebuilding from the loop's
                            left edge would re-emit ticks the transport has
                            already passed; the drain clamps those "late" only
                            within its wrap margin -- beyond it (tiny loops,
                            late detection) they are misread as NEXT-pass
                            events and wedge the FIFO behind them -- so the
                            audible tick is the only always-safe anchor.  The
                            few ms the stall itself consumed are unrecoverable
                            either way: their samples were rendered while this
                            thread was blocked.

                            Ordering: reset_sequences' queue_loop_note_offs
                            must run BEFORE invalidate_future_schedule, whose
                            clear_loop_boundary_offs() re-arms the audio
                            thread's boundary release from the pending bits --
                            the wrap's own release already fired, empty, while
                            this thread was stalled, and without the re-arm the
                            dying pass's notes would ring for a whole pass.
                            And the transport sample is RE-READ: this
                            iteration's `s` may predate the wrap (the
                            generation above was read after `s`), and a rebuild
                            anchored on a pre-wrap sample would strand
                            last_scheduled all over again.  */
                        reset_sequences( -1, true );
                        PatchKnob::app::audio_app_invalidate_future_schedule();
                        const long long now =
                            PatchKnob::app::audio_app_sample_to_tick(
                                PatchKnob::app::audio_app_transport_sample() );
                        set_orig_ticks( (long) now );
                        last_scheduled = now - 1;
                        /* forget the pre-wrap sample: the backward-seek
                           backstop must not misread this wrap as a locate */
                        prev_transport_sample = -1;
                        /*  This iteration's `s` and the horizon derived from it
                            may still be pre-wrap; acting on them could fire the
                            tail branch against the pass just rebuilt and
                            double-schedule it.  Skip one poll and re-derive
                            everything from fresh reads.  */
                        nanosleep( &pace, NULL );
                        continue;
                    }
                }

                /*  LOCATE HANDSHAKE.

                    Wraps stopped being INFERRED from a sample decrease because
                    that inference aliases away whenever this thread stalls
                    across the event.  Locates were left on the very same
                    inference (below) and inherit that bug, plus two only they
                    can hit: a FORWARD locate never decreases the sample at all,
                    and a locate landing in the same poll as a wrap is dropped
                    by the !wrapped guard -- while prev_transport_sample is
                    updated regardless, so the decrease can never be seen again.
                    Any of the three strands last_scheduled ahead of the horizon,
                    and the else-branch below then schedules NOTHING for the rest
                    of the session.  That is the "silent until Stop+Play" report;
                    Stop+Play cures it only because re-entering this function
                    re-initialises last_scheduled.

                    Consume the published counter instead -- unconditionally,
                    with no direction test and no wrap exclusion.

                    The rebuild WAITS for the queued seek to be applied.
                    audio_app_transport_locate() only queues it, and this thread
                    polls far faster than a block: rebuilding against the
                    pre-seek sample would rewind the cursors to the OLD position
                    and then hand play() a window spanning the entire jump,
                    dumping every event between the two positions into the ring
                    in one call (sequence.cpp's "catch-up burst"). */
                const unsigned long long locate_generation =
                    PatchKnob::app::audio_app_locate_generation();
                if( locate_generation != observed_locate_generation &&
                    PatchKnob::app::audio_app_transport_pending_seek() < 0 ){

                    observed_locate_generation = locate_generation;
                    PatchKnob::app::audio_app_invalidate_future_schedule();
                    /*  Re-read: `s` was sampled before the pending-seek test,
                        so it may still be the pre-seek position. */
                    const long long located =
                        PatchKnob::app::audio_app_transport_sample();
                    const long long now =
                        PatchKnob::app::audio_app_sample_to_tick( located );
                    set_orig_ticks( (long) now );
                    last_scheduled = now - 1;
                    tail_done = false;
                    prev_transport_sample = located;
                }

                /*  BACKWARD SEEK BACKSTOP.

                    Superseded by the locate handshake above, which is direction
                    agnostic and cannot be aliased away; this is kept only to
                    catch a backward jump that reached the transport without
                    going through audio_app_transport_locate() (the count-in and
                    the tempo-map relocate still seek the transport directly).
                    It is safe to run redundantly: it only ever rewinds to the
                    current audible position, which is what the handshake just
                    did.  Do NOT rely on it for ordinary locates.

                    last_scheduled only ever moves FORWARD here, so once the
                    transport jumps backwards -- ruler scrub, rewind, "go to
                    start", a marker jump, anything that calls
                    audio_app_transport_locate() while rolling -- the horizon
                    is behind it and `horizon_tick > last_scheduled` is false
                    for as long as it takes the playhead to grind back to where
                    it already was.  For that whole stretch the sequencer emits
                    NOTHING: seek from bar 5 to bar 1 and four bars of music
                    play silently.

                    Nothing detected it.  The three other ways the schedule can
                    be invalidated -- a loop-marker change, a live edit, the
                    loop wrap -- all rewind last_scheduled and rebuild from the
                    audible tick; a locate needs exactly the same treatment, and
                    the transport going backwards is the observation that says
                    one happened.  A loop WRAP also moves the playhead back, so
                    it is excluded: it has already queued its own tail+head.

                    Note that the transport sample is monotonic while rolling,
                    so any decrease is a real reposition -- there is no jitter
                    threshold to tune. */
                if( !loop_config_changed && !wrapped &&
                    prev_transport_sample >= 0 && s < prev_transport_sample ){

                    PatchKnob::app::audio_app_invalidate_future_schedule();
                    const long long now=PatchKnob::app::audio_app_sample_to_tick(s);
                    set_orig_ticks((long)now);
                    last_scheduled=now-1;
                    tail_done=false;
                }
                prev_transport_sample = s;

                /* the UI playhead reads the SAME clock the audio renders.
                   Stored directly (not through set_tick) so the negative
                   clamp in the setter cannot change what the scheduler
                   publishes. */
                m_tick.store( (long) PatchKnob::app::audio_app_sample_to_tick( s ),
                              std::memory_order_relaxed );

                /* two audio blocks + the configured margin of early feed,
                   floored so a small buffer size can't shrink the total
                   anti-jitter margin below the poll thread's real wake
                   latency (see c_thread_trigger_lookahead_floor_ms). */
                const long long sample_rate =
                    (long long) PatchKnob::app::audio_app_sample_rate();
                const long long lookahead_samples = std::max(
                    2 * (long long) PatchKnob::app::audio_app_buffer_size()
                        + (long long)( 0.001 * c_thread_trigger_lookahead_ms
                                       * sample_rate ),
                    (long long)( 0.001 * c_thread_trigger_lookahead_floor_ms
                                 * sample_rate ) );

                /*  The engine's drain needs this to tell a NEXT-PASS event
                    from a merely late one: the tail branch below queues the
                    next pass's head ONE LOOKAHEAD before the loop end, so on a
                    loop shorter than about twice this value those head events
                    sit less than half a loop behind the playhead and used to be
                    misclassified as late -- drained into the pass still playing
                    and then cut down by the boundary note-offs.  */
                PatchKnob::app::audio_app_set_schedule_lookahead( lookahead_samples );

                long long horizon_tick = PatchKnob::app::audio_app_sample_to_tick(
                        s + lookahead_samples );

                /* Live edits must replace the already-queued lookahead.  Before
                   this handshake, adding a note inside that window could not be
                   seen until the next loop.  Invalidate only future ring data,
                   rewind sequence cursors to the audible position, and rebuild
                   the horizon; sounding notes are intentionally preserved. */
                bool edited=false;
                for(int i=0;i<c_max_sequence;++i)if(is_active(i)){
                    const unsigned long long rev=m_seqs[i]->edit_revision();
                    if(edit_revision[i]!=rev){edit_revision[i]=rev;edited=true;}
                }
                if(edited){
                    PatchKnob::app::audio_app_invalidate_future_schedule();
                    const long long now=PatchKnob::app::audio_app_sample_to_tick(s);
                    set_orig_ticks((long)now);
                    last_scheduled=now-1;
                    tail_done=false;
                }

                int trace_branch = -1;

                if ( m_looping && m_playback_mode &&
                     horizon_tick >= get_right_tick() ){

                    trace_branch = tail_done ? 2 : 1;

                    if ( !tail_done ){

                        /* schedule the tail right up to the loop end... */
                        long long leftover_tick = horizon_tick - get_right_tick();
                        if ( leftover_tick > get_right_tick() - get_left_tick() )
                            leftover_tick = 0;   /* degenerate/moved markers */

                        play( get_right_tick() - 1 );
                        // We are LOOKAHEAD ticks ahead of audible playback.
                        // An untimed reset emits note-offs immediately and
                        // chops the loop tail early.  Timestamp the release on
                        // the last included tick (loop-right is exclusive).
                        reset_sequences( -1, true );
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

                    trace_branch = ( horizon_tick > last_scheduled ) ? 0 : 3;

                    if ( horizon_tick > last_scheduled ){

                        play( (long) horizon_tick );
                        m_master_bus.clock( (long) horizon_tick );
                        last_scheduled = horizon_tick;

                        /* play() published the horizon; snap the display back
                           to the real transport position */
                        m_tick.store( (long) PatchKnob::app::audio_app_sample_to_tick( s ),
                                      std::memory_order_relaxed );
                    }
                }

                if ( g_loop_trace_on && g_loop_trace.size() < g_loop_trace.capacity() ){
                    struct timespec tnow; clock_gettime( CLOCK_MONOTONIC, &tnow );
                    loop_trace_rec r;
                    r.ms             = (long long) tnow.tv_sec * 1000
                                     + tnow.tv_nsec / 1000000;
                    r.transport_tick = PatchKnob::app::audio_app_sample_to_tick( s );
                    r.horizon        = horizon_tick;
                    r.left           = get_left_tick();
                    r.right          = get_right_tick();
                    r.last_scheduled = last_scheduled;
                    r.wrap_gen       = wrap_generation;
                    r.branch         = trace_branch;
                    g_loop_trace.push_back( r );
                }

                nanosleep( &pace, NULL );
            }

            /* leaving the scheduler: the engine loop dies with it */
            if ( loop_sent )
                PatchKnob::app::audio_app_set_loop_ticks( 0, 0, 0 );

            m_tick.store( 0, std::memory_order_relaxed );
            m_master_bus.flush( );
            m_master_bus.stop();

            continue;
        }

        /*  ------- FALLBACK SCHEDULER: no audio engine -------

            Reached only when audio_app_running() is false, i.e. the audio
            device could not be opened.  KEPT DELIBERATELY, and it is not
            unreachable code: sdlui/main.cpp keeps the whole application running
            when audio_app_init() returns false (it only guards the features
            that need the engine), so the transport still starts and this is
            then the ONLY thing advancing musical time.  With hardware MIDI out
            enabled, mastermidibus::play sends straight through RtMidi and does
            not touch the engine ring, so a PatchKnob with a busy or missing
            sound card still drives external gear -- which is exactly the
            machine seq24 was written for.  Deleting it would turn "no audio
            device" into "no sequencer".

            It is a wall-clock integrator paced off CLOCK_MONOTONIC: it
            accumulates delta ticks from elapsed time instead of reading the
            engine's sample position, so its timing is only as good as the
            thread's wakeups.  That is why it is the fallback and not the
            default.  It drives sequence::play() through exactly the same call,
            so everything the clip/loop model does -- per-clip loop windows,
            boundary note-offs -- behaves identically here.  */

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

        bool dumping = false;

        bool init_clock = true;

        for( int i=0; i<100; i++ ){
            stats_all[i] = 0;
            stats_clock[i] = 0;
        }

        /* if we are in the performance view, we care 
           about starting from the offset */
        if ( m_playback_mode ){

            /* one load for all three -- see the engine-paced path above */
            const long st = get_starting_tick();
            current_tick = st;
            clock_tick = st;
            set_orig_ticks( st );

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
            /* bpm -- fractional, in DOUBLE precision end-to-end, read from the
               ONE tempo authority (the engine map) so this fallback pacing
               cannot drift away from the tempo everything else uses */
            double bpm  = get_bpm();

            /* get delta ticks, delta_ticks_f is in 1000th of a tick */
            double delta_tick   =  (double) (bpm * ppqn * (delta_us/60000000.0) );

            //printf ( "delta_tick[%ld.%03ld]\n", delta_tick, delta_tick_f  );
                /* default if jack is not compiled in, or not running */
                /* add delta to current ticks */
                clock_tick     += delta_tick;
                current_tick   += delta_tick;
                total_tick     += delta_tick;
                dumping = true;


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
                m_tick.store( (long) llround( current_tick ),
                              std::memory_order_relaxed );

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

        }


        if ( global_stats ){

            printf ( "\n\n-- trigger width --\n" );
            for ( int i=0; i<100; i++ ){
                printf( "[%3d][%8ld]\n", i * 100, stats_all[i] );
            }
            printf ( "\n\n-- clock width --\n" );
            double bpm  = get_bpm();

            printf ( "optimal : [%d]us\n", (int)((c_ppqn / 24) * 60000000.0 / c_ppqn / bpm ));


            for ( int i=0; i<100; i++ ){
                printf( "[%3d][%8ld]\n", i * 300, stats_clock[i] );
            }


        }

        m_tick.store( 0, std::memory_order_relaxed );
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


/*  LEGACY MIDI INPUT -- currently DORMANT, by construction, not by accident:

      * no legacy input port is ever opened.  mastermidibus::init() only opens
        one when m_init_input[bus] is set, and the sole writer of that is
        set_input(), whose only callers are the config readers -- and nothing
        constructs an optionsfile.  So poll_for_midi() always returns 0.
      * even if an event did arrive, m_dumping_input is false (nothing calls
        set_sequence_input) and every m_midi_cc_* entry is inactive (only the
        config reader activates them), so both branches below are no-ops.

    Live MIDI input reaches the app through the patcher's MidiIn nodes
    (audio_app_patch_set_midi_input) instead.  This loop is kept because it is
    the definition of the legacy MIDI-control feature, but it is NOT a second
    input path -- do not "fix" a routing problem here.  It costs one thread
    waking ~1 kHz (poll_for_midi throttles itself; see midibus.cpp). */
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
                            
                            /* MIDI INPUT thread reading the scheduler's tick */
                            ev.set_timestamp( get_tick() );
                            
                            
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
			/*  m_tick is the AUDIBLE transport tick (output_func snaps it back
			    to the engine position after each play()).  Without it
			    toggle_queued() measured the launch boundary from the
			    sequence's m_last_tick -- the scheduler HORIZON -- and any
			    press inside the lookahead window landed a whole repetition
			    late. */
			m_seqs[a_sequence]->toggle_queued( get_tick() );
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

