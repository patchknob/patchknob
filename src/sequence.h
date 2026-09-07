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

class sequence;

#ifndef PATCHKNOB_SEQUENCE
#define PATCHKNOB_SEQUENCE

#include "event.h"
#include "midibus.h"
#include "globals.h"
#include "mutex.h"

#include <string>
#include <list>
#include <stack>
#include <atomic>

enum draw_type
{

    DRAW_FIN = 0,
    DRAW_NORMAL_LINKED,
    DRAW_NOTE_ON,
    DRAW_NOTE_OFF
};

/* used in playback */
class trigger
{
public:
    
    long m_tick_start;
    long m_tick_end;
    
    bool m_selected;
    
    long m_offset;
    
    trigger (){
        
        m_tick_start = 0;
        m_tick_end = 0;
        m_offset = 0;
        m_selected = false;
    };
    
    bool operator< (const trigger& rhs) const {
        
        if (m_tick_start < rhs.m_tick_start)
            return true;
        
        return false;
    };
};

/*  DIAGNOSTIC EMIT TAP (loop regression harness).

    Every MIDI byte a sequence produces -- pattern notes, retrigger releases,
    trigger-end releases, panic offs and loop-boundary offs -- passes through
    exactly one of the three call sites that invoke this hook.  A headless
    harness can therefore count precisely what the sequencer emitted per loop
    pass without touching the audio thread or the ring.

    NULL in every normal build/run; set only by PATCHKNOB_LOOPREPRO.  `kind`
    names the emit path so a missing note-off can be attributed to the pattern
    (0), a release (1), a panic off (2) or the loop boundary (3).             */
class sequence;
typedef void (*seq_emit_tap_t)( sequence *a_seq, int a_bus, int a_channel,
                                unsigned char a_status, unsigned char a_note,
                                unsigned char a_vel, long a_tick, int kind );
extern seq_emit_tap_t g_seq_emit_tap;

class sequence
{

  private:

    /* holds the events */
    list < event > m_list_event;
    static list < event > m_list_clipboard;

    list < trigger > m_list_trigger;
    /*  seq24's trigger clipboard held exactly ONE trigger, because the song
        editor there could only ever have one selected.  The arrange view
        selects a whole rubber-band of clips, so the clipboard is the SELECTION
        -- kept in list order, with each clip's own start/end/offset, so a
        paste reproduces the group's internal spacing. */
    list < trigger > m_trigger_clipboard;

    stack < list < event > >m_list_undo;
    /* Note edits were undoable but not REDOable: pop_undo discarded the state
       it replaced.  It is pushed here instead. */
    stack < list < event > >m_list_redo;
    stack < list < trigger > >m_list_trigger_undo;
    /* Trigger edits were undoable but not REDOable: pop_trigger_undo
       discarded the state it replaced.  It is pushed here instead. */
    stack < list < trigger > >m_list_trigger_redo;

    /* markers */
    list < event >::iterator m_iterator_play;
    list < event >::iterator m_iterator_draw;

    list < trigger >::iterator m_iterator_draw_trigger;

    /* contains the proper midi channel */
    char m_midi_channel;
    char m_bus;


    /* song playback mode mute */
    bool m_song_mute;

    /* PER-PATTERN tracker FX state (bindings + VST-param values), an opaque blob
       the TrackerView serialises into/out of.  Lives here (not in the reused
       view) so each pattern keeps its own FX automation instead of inheriting the
       last-edited pattern's. */
    std::string m_fx_blob;

    /* Persistent arrange-lane role.  This belongs to the sequence/region
       identity, not to its current mixer routing. */
    int m_track_kind; /* 0 instrument, 1 audio, 2 automation */
    int m_arrange_lane_id;

    /* outputs to sequence to this Bus on midichannel */
    mastermidibus *m_masterbus;

    /* map for noteon, used when muting, to shut off current
       messages */
    int m_playing_notes[c_midi_notes];

    /*  STRANDED-NOTE MARK, indexed by the SOUNDING (post scale-snap) pitch.
        Set when a note-on is emitted whose note-off the pattern will never
        play -- it lies outside the loop window, or is HIDDEN past the end
        marker.  Such a note used to be held until the whole clip stopped;
        cut_stranded_notes() releases it at the repetition boundary instead.
        A note whose off merely lies at a LOWER tick than its on WRAPS the
        pattern end (verify_and_link's second pass pairs those) and IS
        released -- by the next repetition -- so it is never marked. */
    bool m_loop_cut[c_midi_notes];

    /* Pitch currently sounding in each tracker column, or -1.  A column is one
       voice: striking it again releases whatever it was holding, whatever the
       pitch.  This is what makes "column == voice" true for EVERY instrument,
       rather than only for the sampler (which tags its own voices by column). */
    static const int c_max_columns = 32;
    int m_column_note[c_max_columns];

    /* SCALE-MASTER / SCALE-FOLLOW -- see docs/scale-follow.md */

    /* this sequence carries the master scale */
    bool m_is_scale_master;
    /* this sequence snaps its emitted notes to the master scale */
    bool m_follows_master;
    /* master scale this sequence carries (index into c_scales_* tables) */
    int  m_master_scale;
    /* master root pitch-class 0..11 (C..B) */
    int  m_master_key;

    /* per-tick master snapshot pushed down by perform::play, read by
       put_event_on_bus on the same (output) thread */
    bool m_have_master;
    int  m_follow_key;
    int  m_follow_scale;

    /* latch: on a follower's note-on we record the snapped pitch here,
       indexed by the ORIGINAL pitch; on the matching note-off we reuse it so
       the off always matches the on even if the master scale changed.  -1 ==
       not currently latched. */
    int  m_note_remap[c_midi_notes];

    /* states */
    bool m_was_playing;
    bool m_playing;
    bool m_recording;
    bool m_thru;
    bool m_queued;

    bool m_trigger_copied;

    /* flag indicates that contents has changed from
       a recording */
    bool m_dirty_main;
    bool m_dirty_edit;
    bool m_dirty_perf;
    std::atomic<unsigned long long> m_edit_revision{1};
    bool m_dirty_names;

    /* anything editing currently ? */
    bool m_editing;
    bool m_raise;
    
    /* named sequence */
    string m_name;

    /* where were we */
    long m_last_tick;

    /*  ABSOLUTE tick at which the currently playing clip's content grid
        begins -- trigger_start minus the trigger's content offset -- or -1 in
        live mode, where there is no clip and the grid is the song's.

        m_last_tick is a SONG tick, and turning it into the PATTERN position
        every editor draws its playhead at needs to know where the clip's grid
        starts.  seq24 needed no such thing: a pattern always repeated at
        m_length from song tick 0, so `m_last_tick % m_length` was the answer.
        Neither half of that survives -- a clip repeats its own window, and
        play_span anchors the repetitions on the clip's own start tick -- so
        play_span records the anchor it used and get_last_tick() inverts it.
        Written by the output thread, read by the drawing threads, exactly like
        m_last_tick and m_trigger_offset beside it. */
    long m_play_anchor;
    long m_queued_tick;

    long m_trigger_offset;

    /* length of sequence in pulses
       should be powers of two in bars */
    long m_length;

    /* Pattern-local loop window [m_loop_start, m_loop_end), independent of
       m_length (the pattern's actual total size/note-storage bound) -- the
       loop is a sub-region you can position ANYWHERE inside a longer
       pattern, with content before/after it still present.  Defaults to
       spanning the whole pattern (loop_end == length), which reads as "no
       loop set"; callers that draw/tint a loop UI should treat that default
       state as "nothing to highlight", not as a zero-width or degenerate
       region.  Purely a piano-roll editing/marker concept -- NOT the
       song/transport loop (perform::get_left_tick/get_right_tick), which is
       a separate, global playback range unrelated to any one sequence.
       Both clamped into [0, m_length] by set_length() so neither can point
       past the pattern. */
    long m_loop_start;
    long m_loop_end;

    /*  LOOPING IS OPTIONAL.  With it OFF the clip is a ONE-SHOT: its data plays
        once from where the clip starts and then stops, and dragging the clip
        longer in the arrange view just moves its end point.  With it ON the
        clip repeats [m_loop_start, m_loop_end) for as long as the clip is,
        so dragging it longer repeats the selected bars.
        Defaults to ON with the window spanning the whole pattern, which is
        exactly the historical behaviour (repeat at m_length), so existing
        projects load unchanged. */
    bool m_loop_enabled;

    /* these are just for the editor to mark things
       in correct time */
    //long m_length_measures;
    long m_time_beats_per_measure;
    long m_time_beat_width;

    /* locking */
    PatchKnobMutex m_mutex;

    /* used to idenfity which events are ours in the out queue */
    //unsigned char m_tag;

    /* takes an event this sequence is holding and places it on our midibus.
       a_tick = the event's ABSOLUTE due-time in sequencer ticks (-1 = "now"),
       carried to the audio engine for sample-accurate delivery */
    void put_event_on_bus (event * a_e, long a_tick = -1,
                           bool a_stranded = false);

    /*  Release, at an ABSOLUTE tick, every note this sequence started that its
        own data will never release (see m_loop_cut).  Called at each loop-window
        repetition boundary and at the end of the pattern's data, so a
        repetition always releases what it started -- the MIDI hard-cut every
        DAW performs at a clip/loop boundary.  Unlike off_playing_notes() this
        leaves legitimately sounding notes alone. */
    void cut_stranded_notes (long a_tick);

    /* resetes the location counters */
    void reset_loop (void);

    void remove_all (void);


    /*  Does this pattern carry a loop window of its own, as opposed to the
        "spans the whole pattern" default?

        The test was written out by hand at four sites (play_span,
        repeat_period, get_last_tick, adjust_trigger_offsets_to_legnth) and
        they MUST agree: the phase the trigger machinery stores and the phase
        playback uses are computed from it, and a partial edit desyncs them.
        Hence one definition.

        `end != length`, not `end < length`: a window is allowed to reach PAST
        the end marker (see set_loop_end), which is how an odd-length loop is
        laid over a shorter phrase -- a 3-beat window on a 2-beat pattern.
        With `<` that case tested as "no window at all" and the clip repeated
        at m_length, so the feature could be stored and drawn but never
        played. */
    bool loop_window_set (void)
    {
        return ( m_loop_start > 0 || m_loop_end != m_length )
               && m_loop_end > m_loop_start;
    }

    /* sets m_trigger_offset and wraps it to the repetition period */
    void set_trigger_offset (long a_trigger_offset);
    void split_trigger( trigger &trig, long a_split_tick);
    void adjust_trigger_offsets_to_legnth( long a_new_len );
    long adjust_offset( long a_offset );

  public:

      sequence ();
     ~sequence ();

    /*  The pattern mutex (RECURSIVE; every public method takes it).  Public so
        a caller can pin the sequence across a multi-call read the way the GUI
        does implicitly while drawing -- and so the loop-wrap regression
        harness can induce exactly that contention against the output thread
        (the missed-tail-window stall, see perform::output_func).  */
    void lock ();
    void unlock ();


    void push_undo (void);
    void pop_undo (void);
    void pop_redo (void);

    void push_trigger_undo (void);
    void pop_trigger_undo (void);
    void pop_trigger_redo (void);

    //
    //  Gets and Sets
    //

    /* name */
    void set_name (string a_name);
    void set_name (char *a_name);

    void set_measures (long a_length_measures);
    long get_measures (void);

    void set_bpm (long a_beats_per_measure);
    long get_bpm (void);

    void set_bw (long a_beat_width);
    long get_bw (void);

    void set_song_mute (bool a_mute);
    bool get_song_mute (void);

    /* per-pattern tracker FX blob (opaque; owned by TrackerView's format) */
    void set_fx_blob (const std::string& a_blob) { m_fx_blob = a_blob; }
    const std::string& get_fx_blob (void) const { return m_fx_blob; }
    void set_track_kind (int k) { m_track_kind = k < 0 ? 0 : (k > 2 ? 2 : k); }
    int  get_track_kind (void) const { return m_track_kind; }
    void set_arrange_lane_id(int id){m_arrange_lane_id=id;}
    int  get_arrange_lane_id() const{return m_arrange_lane_id;}

    /* returns string of name */
    const char *get_name (void);

    void set_editing (bool a_edit)
    {
	m_editing = a_edit;
    };
    bool get_editing (void)
    {
	return m_editing;
    };
    void set_raise (bool a_edit)
    {
	m_raise = a_edit;
    };
    bool get_raise (void)
    {
	return m_raise;
    };


    /* length in ticks */
    void set_length (long a_len, bool a_adjust_triggers = true);
    long get_length ();

    /* list sizes -- diagnostics / regression harnesses only */
    int  event_count   () { return (int) m_list_event.size();   }
    int  trigger_count () { return (int) m_list_trigger.size(); }

    /* returns last tick played..  used by 
       editors idle function */
    long get_last_tick ();

    /*  The PATTERN position a given SONG tick maps to for this sequence --
        the inverse of what play_span() does when it lays the clip's
        repetitions out.  get_last_tick() (the drawn playhead) and
        stream_event() (where a live-recorded note lands) are the same
        question asked about two different ticks, and they were answering it
        two different ways: seq24's `tick % m_length`, which is only right
        when a pattern repeats at its full length from song tick zero. */
    long pattern_position (long a_song_tick);

    /* sets state.  when playing,
       and sequencer is running, notes
       get dumped to the alsa buffers */
    void set_playing (bool, long a_tick = -1);

    /* Emit every event due in [a_start,a_end] at pattern phase a_trigger_offset.
       Split out of play() so a scheduling window spanning SEVERAL triggers can
       be played as one segment per trigger instead of collapsing to a single
       trigger state. */
    /* a_trigger_start is the covering clip's START tick, needed to anchor a
       ONE-SHOT (loop disabled) pass.  -1 == live mode / unknown. */
    void play_span (long a_start, long a_end, long a_trigger_offset,
                    long a_trigger_start = -1);
    /* Walk [a_start,a_end] trigger by trigger, playing each covered segment and
       releasing at each trigger's own end tick. */
    void play_triggered (long a_start, long a_end);
    bool get_playing ();
    void toggle_playing ();
    /*  Same, but naming the tick the release belongs on.  perform::play() runs
        a LOOKAHEAD ahead of audible time, so a queued mute that toggles with
        no tick releases its notes "now" -- up to a lookahead too early, and
        ahead of note-ons already queued for the same pattern (hung voice).
        play_triggered() already releases at the exact boundary tick; this lets
        the queue path be symmetric. */
    void toggle_playing (long a_tick);

    /* SCALE-MASTER / SCALE-FOLLOW accessors */
    void set_scale_master (bool a_v);
    bool get_scale_master (void);
    void set_follows_master (bool a_v);
    bool get_follows_master (void);

    void set_master_scale (int a_scale);
    int  get_master_scale (void);
    void set_master_key (int a_key);
    int  get_master_key (void);

    /* called by perform::play once per tick to push the resolved master
       context down to this follower before play() runs */
    void set_master_scale_context (bool a_on, int a_key, int a_scale);

    /*  a_now_tick is the AUDIBLE transport tick the queue press happened at,
        and the launch boundary is computed from it.  It used to be computed
        from m_last_tick, which perform's scheduler has already advanced to the
        lookahead HORIZON -- ~15 ms of music into the future.  Whenever the
        audible tick and the horizon straddled a repetition boundary the "next
        boundary" came out one whole repetition later than the one the user was
        aiming at, so the pattern launched a full bar late.  (seq24 had the same
        expression but only ~1 ms of lookahead to be wrong by.)

        -1 keeps the old behaviour for callers with no transport tick to hand. */
    void toggle_queued (long a_now_tick = -1);
    void off_queued (void);
    bool get_queued (void);
    long get_queued_tick (void);

    void set_recording (bool);
    bool get_recording ();

    void set_thru (bool);
    bool get_thru ();

    /* singals that a redraw is needed from recording */
    /* resets flag on call */
    bool is_dirty_main ();
    bool is_dirty_edit ();
    bool is_dirty_perf ();
    bool is_dirty_names ();
    

    void set_dirty_mp();
    void set_dirty();
    unsigned long long edit_revision() const
        { return m_edit_revision.load(std::memory_order_acquire); }

    /* midi channel */
    unsigned char get_midi_channel ();
    void set_midi_channel (unsigned char a_ch);

    /* dumps contents to stdout */
    void print ();
    void print_triggers();

    /* dumps notes from tick and prebuffers to
       ahead.  Called by sequencer thread - performance */
    void play (long a_tick, bool a_playback_mode);
    void set_orig_tick (long a_tick);

    //
    //  Selection and Manipulation
    //

    /* adds event to internal list */
    void add_event (const event * a_e);

    void add_trigger (long a_tick, long a_length, long a_offset = 0, bool a_adjust_offset = true);
    void split_trigger( long a_tick );
    void grow_trigger (long a_tick_from, long a_tick_to, long a_length);
    void del_trigger (long a_tick );
    bool get_trigger_state (long a_tick);
    bool select_trigger(long a_tick);
    bool unselect_triggers (void);

    void del_selected_trigger( void );
    void cut_selected_trigger( void );
    void copy_selected_trigger( void );
    void paste_trigger( void );
    
    void move_selected_triggers_to(long a_tick, bool a_adjust_offset, int a_which=2);
    long get_selected_trigger_start_tick( void );
    long get_selected_trigger_end_tick( void );

    long get_max_trigger (void);

    void move_triggers (long a_start_tick, long a_distance, bool a_direction);
    void copy_triggers (long a_start_tick, long a_distance);
    void clear_triggers (void);
    void clear_events (void);
    void discard_edit_history (void);


    long get_trigger_offset (void);

    /* Pattern-local loop window shown in the piano roll (distinct from the
       song/transport loop and from m_length -- see m_loop_start's comment).
       Setters clamp into [0, m_length] with start <= end. */
    long get_loop_start (void) const { return m_loop_start; }
    void set_loop_start (long a_tick);
    long get_loop_end (void) const { return m_loop_end; }
    void set_loop_end (long a_tick);
    bool get_loop_enabled (void) const { return m_loop_enabled; }
    void set_loop_enabled (bool a_on);

    /* The tick span ONE repetition of this clip covers: the loop window's
       period when the pattern has its own window, else the whole pattern.
       seq24 had no such notion -- a pattern always repeated at m_length -- so
       every trigger-offset computation folded by m_length.  Mirrors exactly
       the `loop_set`/`period` pair play_span() derives, so the phase the
       trigger machinery stores and the phase playback uses agree.  Never 0.
       PUBLIC: the arrange view folds a split's right-half offset with it, and
       must use the same period the engine does. */
    long repeat_period (void);

    /* sets the midibus to dump to */
    void set_midi_bus (char a_mb);
    char get_midi_bus (void);

    void set_master_midi_bus (mastermidibus * a_mmb);

    /* Tag the note-on at `a_tick` with pitch `a_note` (the `a_occurrence`-th
       such note at that tick) with the tracker COLUMN it lives in, so playback
       can treat that column as one voice.  -1 clears the tag. */
    void set_event_column (long a_tick, int a_note, int a_occurrence, int a_column);
    /* Drop every column tag (before re-stamping the whole pattern). */
    void clear_event_columns (void);

    /* Make every COLUMN monophonic in the DATA: within a column, a note is cut
       (or given a note-off it never had) `a_min_gap` ticks before the next note
       starts.  A gap of one max-LPB step means the release always lands on its
       own step, so a retrigger never depends on which of two events at the same
       tick happens to be emitted first. */
    void enforce_column_gaps (long a_min_gap);

    /* One-pass snapshot of every event INCLUDING its tracker column, for the
       project writer.  get_next_event() cannot report the column, so saving
       through it silently dropped the per-note voice assignment. */
    struct EventSnapshot {
        long tick;
        unsigned char status, d0, d1;
        int  column;                 /* -1 when untagged */
    };
    void snapshot_events (std::vector<EventSnapshot>& out);

    enum select_action_e
    {
        e_select,
        e_select_one,
        e_is_selected,
        e_would_select
    };
    
    /* select note events in range, returns number
       selected */
    int select_note_events (long a_tick_s, int a_note_h,
			    long a_tick_f, int a_note_l, select_action_e a_action );

    /* select events in range, returns number
       selected */
    int select_events (long a_tick_s, long a_tick_f,
		       unsigned char a_status, unsigned char a_cc, select_action_e a_action);

    int get_num_selected_notes ();
    int get_num_selected_events (unsigned char a_status, unsigned char a_cc);

    void select_all (void);

    void copy_selected (void);
    void paste_selected (long a_tick, int a_note);

    /* returns the 'box' of selected items */
    void get_selected_box (long *a_tick_s, int *a_note_h,
			   long *a_tick_f, int *a_note_l);

    /* returns the 'box' of selected items */
    void get_clipboard_box (long *a_tick_s, int *a_note_h,
			    long *a_tick_f, int *a_note_l);

    /* removes and adds readds selected in position */
    void move_selected_notes (long a_delta_tick, int a_delta_note);

    /* adds a single note on / note off pair */
    void add_note (long a_tick, long a_length, int a_note, bool a_paint = false);
    void add_note_velocity(long a_tick, long a_length, int a_note,
                           int velocity, bool a_paint = false);

    void add_event (long a_tick,
		    unsigned char a_status,
		    unsigned char a_d0, unsigned char a_d1, bool a_paint = false);

    void stream_event (event * a_ev);

    /* changes velocities in a ramping way from vel_s to vel_f  */
    void change_event_data_range (long a_tick_s, long a_tick_f,
				  unsigned char a_status,
				  unsigned char a_cc,
				  int a_d_s, int a_d_f);
				  //unsigned char a_d_s, unsigned char a_d_f);

    /* moves note off event */
    void increment_selected (unsigned char a_status, unsigned char a_control);
    void decrement_selected (unsigned char a_status, unsigned char a_control);

    /* moves note off event */
    void grow_selected (long a_delta_tick);
    void stretch_selected(long a_delta_tick);
    
    /* deletes events */
    void remove_marked();
    /* remove one note-on at exactly tick/note, plus its linked partner */
    bool remove_note_at (long a_tick, int a_note);
    bool remove_note_at (long a_tick, int a_note, int a_occurrence);
    void mark_selected();
    void unpaint_all();
    
    /* unselects every event */
    void unselect ();

    /* verfies state, all noteons have an off,
       links noteoffs with their ons */
    /* a_prune deletes every event that falls outside [0, m_length).

       It DEFAULTS OFF, and nothing in the engine turns it on.  seq24 defaulted
       it on because there an event past the end marker was meaningless; here it
       is HIDDEN but alive -- set_length() shrinks a pattern without destroying
       what falls off the end (see set_length and the hidden-event branch in
       play_span) precisely so that growing it again brings the notes back.
       Every ordinary edit relinks, so a pruning default meant transpose,
       quantise, move, grow, paste, undo/redo and even duplicating a pattern all
       silently deleted the hidden tail: an eight-event pattern shrunk to one bar
       came back as two events after any of them.

       Pass true only from code that genuinely wants events outside the pattern
       destroyed -- and note that no caller currently does. */
    void verify_and_link (bool a_prune = false);
    void link_new ();

    /* resets everything to zero, used when
       sequencer stops */
    void zero_markers (void);

    /* flushes a note to the midibus to preview its 
       sound, used by the virtual paino */
    void play_note_on (int a_note);
    void play_note_on (int a_note, int a_velocity);   // audition at a given velocity
    void play_note_off (int a_note);

    /* send a note off for all active notes */
    /* Panic-off every sounding note.  a_tick is the ABSOLUTE tick the offs are
       due at; -1 means "immediately" (block start).  Without it a trigger
       ending mid-note released at the start of whichever audio block was
       rendering, not at the trigger's own tick. */
    void off_playing_notes (long a_tick = -1);
    void queue_loop_note_offs();

    //
    // Drawing functions
    //

    /* resets draw marker so calls to getNextnoteEvent
       will start from the first */
    void reset_draw_marker (void);
    void reset_draw_trigger_marker (void);

    /* each call seqdata( sequence *a_seq, int a_scale );fills the passed refrences with a 
       events elements, and returns true.  When it 
       has no more events, returns a false */
    draw_type get_next_note_event (long *a_tick_s,
				   long *a_tick_f,
				   int *a_note,
				   bool * a_selected, int *a_velocity);

    int get_lowest_note_event ();
    int get_highest_note_event ();

    bool get_next_event (unsigned char a_status,
			 unsigned char a_cc,
			 long *a_tick,
			 unsigned char *a_D0,
			 unsigned char *a_D1, bool * a_selected);

    bool get_next_event (unsigned char *a_status, unsigned char *a_cc);

    bool get_next_trigger (long *a_tick_on,
			   long *a_tick_off,
			   bool * a_selected, long *a_tick_offset);

    sequence & operator= (const sequence & a_rhs);

    void fill_list (list < char >*a_list, int a_pos);

    void select_events (unsigned char a_status, unsigned char a_cc,
			bool a_inverse = false);
    void quanize_events (unsigned char a_status, unsigned char a_cc,
			 long a_snap_tick, int a_divide, bool a_linked =
			 false, bool a_left = false);
    void transpose_notes (int a_steps, int a_scale);
};

#endif
