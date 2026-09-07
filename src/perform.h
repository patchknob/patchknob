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

#ifndef PATCHKNOB_PERFORM
#define PATCHKNOB_PERFORM

class perform;

#include "globals.h"
#include "event.h"
#include "midibus.h"
#include "midifile.h"
#include "sequence.h"
#include <unistd.h>
#include <pthread.h>
#include <vector>
#include <atomic>




/*  SCHEDULER TRACE -- diagnostics for the transport LOOP.
    perform::output_func appends one row per poll when g_loop_trace_on is set,
    so a headless harness can prove WHICH branch ran and with what numbers.
    branch: 0 = normal advance, 1 = loop wrap (tail + head scheduled),
            2 = wrap branch suppressed by the tail_done latch,
            3 = nothing scheduled (horizon still behind last_scheduled).      */
struct loop_trace_rec {
    long long ms;            /* wall ms since the trace was armed            */
    long long transport_tick;
    long long horizon;
    long long left, right;
    long long last_scheduled;
    unsigned long long wrap_gen;
    int  branch;
};
extern bool                        g_loop_trace_on;
extern std::vector<loop_trace_rec> g_loop_trace;

/* class contains sequences that make up a live set */

class midi_control
{
 public:

	bool m_active;
	bool m_inverse_active;
	long m_status;
	long m_data;
	long m_min_value;
	long m_max_value;
};

const int c_status_replace  = 0x01;
const int c_status_snapshot = 0x02;
const int c_status_queue    = 0x04;

const int c_midi_control_bpm_up       = c_seqs_in_set ;
const int c_midi_control_bpm_dn       = c_seqs_in_set + 1;
const int c_midi_control_ss_up        = c_seqs_in_set + 2;
const int c_midi_control_ss_dn        = c_seqs_in_set + 3;
const int c_midi_control_mod_replace  = c_seqs_in_set + 4;
const int c_midi_control_mod_snapshot = c_seqs_in_set + 5;
const int c_midi_control_mod_queue    = c_seqs_in_set + 6;
const int c_midi_controls             = c_seqs_in_set + 7;

class perform
{
 private:

    /* vector of sequences */
    sequence *m_seqs[c_max_sequence];

    /* deleted sequences are RETIRED here instead of freed: the output thread
       (1ms scheduler) and UI views may still hold the pointer for an instant,
       so freeing live caused use-after-free crashes (idiv on garbage m_length
       in get_last_tick).  Freed by gc_graveyard() a couple of UI frames after
       retirement (well past the one-audio-block reference window), or, failing
       that, in the destructor. */
    std::vector<sequence *> m_seq_graveyard;
    std::vector<int>        m_seq_graveyard_age;   // gc_graveyard() passes survived

    bool m_seqs_active[ c_max_sequence ];

    /* SCALE-MASTER / SCALE-FOLLOW: index of the scale master sequence in
       m_seqs[], or -1 == none.  See docs/scale-follow.md. */
    int m_scale_master_seq;

    bool m_was_active_main[ c_max_sequence ];
    bool m_was_active_edit[ c_max_sequence ];
    bool m_was_active_perf[ c_max_sequence ];
    bool m_was_active_names[ c_max_sequence ];
    
    bool m_sequence_state[  c_max_sequence ];

    /*  The LEGACY seq24 MIDI layer.  It is NOT the routing authority -- MIDI
        routing lives in the modular patch graph (src/engine/patch).  What this
        member is still good for:

          * mastermidibus::play(), which every sequence calls and which is THE
            bridge into the audio engine (audio_app_route_midi).  Read the
            contract in midibus.cpp before changing anything about bus indices.
          * enumerating MIDI port names for the config writer.

        Everything else on it is dormant: it opens no hardware port and emits
        no bytes unless set_hw_output(true) is called, which nothing does.  It
        holds NO tempo (the engine tempo map does; see set_bpm/get_bpm) and no
        authoritative bus assignment.  See the map at the top of midibus.h. */
    mastermidibus m_master_bus;

    /* pthread info */
    pthread_t m_out_thread;
    pthread_t m_in_thread;
    bool m_out_thread_launched;
    bool m_in_thread_launched;

    bool m_running;
    bool m_inputing;
    bool m_outputing;
    bool m_looping;

    bool m_playback_mode;

    int thread_trigger_width_ms; 

    /*  TRANSPORT TICK FIELDS -- genuinely SHARED, hence atomic (bug R8).
        They were plain longs, which is undefined behaviour and, in practice,
        torn or indefinitely stale playhead/loop-marker readouts:

          m_tick          written by the SCHEDULER thread (output_func, both
                          the engine-paced and the fallback scheduler, plus
                          perform::play) AND by the MESSAGE/UI thread via
                          set_tick(); read by the UI playhead
                          (ArrangeView::playhead -> get_tick) and by the MIDI
                          INPUT thread (input_func timestamps recorded events
                          with it).  Three threads.
          m_left_tick     the loop markers.  Written by the UI (ruler drags,
          m_right_tick    project load, "set loop to selection"); read every
                          poll by the scheduler, which republishes them to the
                          engine, and read again by the UI to draw them.
          m_starting_tick written by the UI (rewind, punch-in, loop-left drag,
                          project load) and by set_left_tick/set_right_tick;
                          read by inner_start and by both schedulers as the
                          position playback resumes from.

        ORDERING: relaxed, everywhere.  Each of these is a self-contained
        scalar readout -- no other memory is published *through* them, so
        there is nothing for an acquire/release pair to order.  The one place
        an ordering edge genuinely matters (the UI parking the playhead, then
        pressing PLAY) already gets it from m_condition_var's lock/signal in
        inner_start, which the scheduler blocks on before it reads
        m_starting_tick.  Relaxed also keeps the 1ms scheduler poll free of
        barriers: on x86-64 and AArch64 a relaxed load/store compiles to the
        same plain mov/ldr the raw long did.  See BUG R8. */
    std::atomic<long> m_left_tick;
    std::atomic<long> m_right_tick;
    std::atomic<long> m_starting_tick;

    std::atomic<long> m_tick;

    void set_running( bool a_running );
    bool is_running();

    void set_playback_mode( bool a_playback_mode );

    string m_screen_set_notepad[c_max_sets];

    midi_control m_midi_cc_toggle[ c_midi_controls ];
    midi_control m_midi_cc_on[ c_midi_controls ];
    midi_control m_midi_cc_off[ c_midi_controls ];
    
    int m_offset;
    int m_control_status;
    int m_screen_set;

    condition_var m_condition_var;

    std::map<long,long> key_events;

    

    void inner_start( bool a_state );
    void inner_stop();

 public:

    long m_key_bpm_up;
    long m_key_bpm_dn;

    long m_key_replace;
    long m_key_queue;
    long m_key_snapshot_1;
    long m_key_snapshot_2;

    long m_key_screenset_up;
    long m_key_screenset_dn;

    long m_key_start; 
    long m_key_stop;


    perform();
    ~perform();

    void init( void );

    void clear_all( void );
    
    void launch_input_thread( void );
    void launch_output_thread( void );
    
    void add_sequence( sequence *a_seq, int a_perf );
    void delete_sequence( int a_num );
    // Free retired (deleted) sequences that have survived >=2 gc passes, so the
    // graveyard doesn't grow for the whole session.  Call once per UI frame from
    // the MESSAGE thread only.
    void gc_graveyard( void );
    bool is_sequence_in_edit( int a_num );
    
    void clear_sequence_triggers( int a_seq  );


    /* signatures unchanged on purpose -- callers in sdlui/ keep compiling */
    long get_tick( ) { return m_tick.load( std::memory_order_relaxed ); };
    void set_tick( long tick )
    { m_tick.store( tick < 0 ? 0 : tick, std::memory_order_relaxed ); }

    /* Public, READ-ONLY view of the transport run-state (is_running() itself is
       private and is the internal write-side pair of set_running()).  The shell
       needs it because perform -- not the engine transport -- is what PLAY and
       STOP actually drive: the transport bar used to light its PLAY button from
       the engine alone, which rolls during a metronome count-in while perform is
       still stopped, and does not roll at all when there is no audio device.
       Either way the button disagreed with what the button did. */
    bool running( void ) const { return m_running; }

    void set_left_tick( long a_tick );
    long get_left_tick( void );

    void set_starting_tick( long a_tick );
    long get_starting_tick( void );

    void set_right_tick( long a_tick );
    long get_right_tick( void );

    void move_triggers( bool a_direction );
    void copy_triggers(  );
    
    void push_trigger_undo( void );
    void pop_trigger_undo( void );
    void pop_trigger_redo( void );

    void print();

    midi_control *get_midi_control_toggle( unsigned int a_seq );
    midi_control *get_midi_control_on( unsigned int a_seq );
    midi_control *get_midi_control_off( unsigned int a_seq );

    void handle_midi_control( int a_control, bool a_state );

    void set_screen_set_notepad( int a_screen_set, string *a_note );
    string *get_screen_set_notepad( int a_screen_set );

    void set_screenset( int a_ss );
    int get_screenset( void );
    
    void start( bool a_state );
    void stop();


    void off_sequences( void );

    void set_active(int a_sequence, bool a_active);
    void set_was_active( int a_sequence );
    bool is_active(int a_sequence);
    bool is_dirty_main (int a_sequence);
    bool is_dirty_edit (int a_sequence);
    bool is_dirty_perf (int a_sequence);
    bool is_dirty_names (int a_sequence);
        
    void new_sequence( int a_sequence );

    /* plays all notes to Curent tick */
    void play( long a_tick );
    void set_orig_ticks( long a_tick  );

    sequence * get_sequence( int a_sequence );

    void reset_sequences( long release_tick = -1, bool loop_boundary = false );

    /* SCALE-MASTER / SCALE-FOLLOW.
       set_scale_master enforces a single master: it clears the previous
       master's flag, sets the new one (-1 clears).  set_follows_master toggles
       a sequence's follow flag.  See docs/scale-follow.md section 2. */
    void set_scale_master( int a_seq );
    int  get_scale_master( void );
    void set_follows_master( int a_seq, bool a_follow );

    /* Fractional BPM, end to end.  The engine tempo map is the ONE authority:
       set_bpm is the only write path into it and get_bpm reads it straight
       back -- there is no second copy anywhere to fall out of sync. */
    void   set_bpm(double a_bpm);
    double get_bpm( );

    void set_looping( bool a_looping ){ m_looping = a_looping; };
    bool get_looping( void ) const { return m_looping; };
 
    void set_sequence_control_status( int a_status );
    void unset_sequence_control_status( int a_status );

    void sequence_playing_toggle( int a_sequence );
    void sequence_playing_on( int a_sequence );
    void sequence_playing_off( int a_sequence );
    
    void mute_all_tracks( void );

    mastermidibus* get_master_midi_bus( );
    
    void output_func();
    void input_func();
    
    long get_max_trigger( void );

    void set_offset( int a_offset );
    
    void save_playing_state( void );
    void restore_playing_state( void );
    
    
    std::map<long,long> *get_key_events( void ){ return &key_events; };


    friend class midifile;
    friend class optionsfile;


};

/* located in perform.C */
extern void *output_thread_func(void *a_p);
extern void *input_thread_func(void *a_p);





#endif
