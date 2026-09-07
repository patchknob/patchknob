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
#include "sequence.h"
#include <algorithm>
#include "quantize.h"
#include "audio_app.h"
// #include "seqedit.h"  (GUI; not used by engine)
#include <stdlib.h>
#include <vector>
    
list < event > sequence::m_list_clipboard;

/* diagnostic emit tap -- see sequence.h.  NULL unless a harness installs one. */
seq_emit_tap_t g_seq_emit_tap = NULL;

sequence::sequence( )
{
 
    m_editing       = false;
    m_raise         = false;
    m_playing       = false;
    m_was_playing   = false;
    m_recording     = false;
    m_thru          = false;
    m_queued        = false;

    m_trigger_copied = false;
        
    m_time_beats_per_measure = 4;
    m_time_beat_width = 4;

    //m_tag           = 0;

    m_name          = c_dummy;
    m_bus           = 0;
    m_length        = 4 * c_ppqn;
    m_loop_start    = 0;
    m_loop_end      = m_length;   // default: loop spans the whole pattern ("no loop set")
    m_loop_enabled  = true;       // repeat-at-length: the historical behaviour
    m_midi_channel  = 0;
  
    /* no notes are playing */
    for (int i=0; i< c_midi_notes; i++ )
        m_playing_notes[i] = 0;
    for (int i=0; i< c_midi_notes; i++ )
        m_loop_cut[i] = false;
    for ( int i = 0; i < c_max_columns; i++ )
        m_column_note[i] = -1;

    /* SCALE-MASTER / SCALE-FOLLOW */
    m_is_scale_master = false;
    m_follows_master  = false;
    m_master_scale    = c_scale_off;
    m_master_key      = 0;
    m_have_master     = false;
    m_follow_key      = 0;
    m_follow_scale    = c_scale_off;
    for (int i=0; i< c_midi_notes; i++ )
        m_note_remap[i] = -1;

    m_last_tick = 0;
    m_play_anchor = -1;
    m_track_kind = 0;
    m_arrange_lane_id = -1;
    /* Was left UNINITIALISED.  perform::play() reads it every tick
       (get_queued_tick() <= a_tick) and only the get_queued() guard kept the
       garbage from being used; a pattern that had never been queued could still
       hand a junk tick to anything that asked. */
    m_queued_tick = 0;

    m_masterbus = NULL;
    m_dirty_main = true;
    m_dirty_edit = true;
    m_dirty_perf = true;
    m_dirty_names = true;
    
    m_song_mute = false;

    m_trigger_offset = 0;

    /* THE FOUR MARKER ITERATORS WERE NEVER INITIALISED.  A default-constructed
       list iterator is singular (libstdc++ value-initialises the node pointer to
       NULL), so `m_iterator_draw != m_list_event.end()` is TRUE on a fresh
       sequence and the very next line dereferences NULL.  Every current caller
       happens to call reset_draw_marker() / reset_draw_trigger_marker() first,
       which is the only reason this has not crashed -- one caller that forgets
       is a null dereference, not a wrong drawing.  Point them all at the (empty)
       lists so a walk without a reset is merely empty. */
    m_iterator_play         = m_list_event.begin();
    m_iterator_draw         = m_list_event.begin();
    m_iterator_draw_trigger = m_list_trigger.begin();
}

void 
sequence::push_undo( void )
{
    lock();
    /* a fresh edit forks the history: anything redoable is now unreachable */
    while ( !m_list_redo.empty() ) m_list_redo.pop();
    m_list_undo.push( m_list_event );
    unlock();
}


void 
sequence::pop_undo( void )
{
    lock();

    if (m_list_undo.size() > 0 ){

        /* keep what we are undoing so it can be redone */
        m_list_redo.push( m_list_event );
        m_list_event = m_list_undo.top();
        m_list_undo.pop();
        verify_and_link();
        unselect();
    }

    unlock();
}


void
sequence::pop_redo( void )
{
    lock();

    if (m_list_redo.size() > 0 ){

        m_list_undo.push( m_list_event );
        m_list_event = m_list_redo.top();
        m_list_redo.pop();
        verify_and_link();
        unselect();
    }

    unlock();
}


void 
sequence::push_trigger_undo( void )
{
    lock();
    /* a fresh edit forks the history: anything redoable is now unreachable */
    while ( !m_list_trigger_redo.empty() ) m_list_trigger_redo.pop();
    m_list_trigger_undo.push( m_list_trigger );

    list<trigger>::iterator i;
    
    for ( i  = m_list_trigger_undo.top().begin();
          i != m_list_trigger_undo.top().end(); i++ ){
	  (*i).m_selected = false;
    }

    
    unlock();
}


void 
sequence::pop_trigger_undo( void )
{
    lock();

    if (m_list_trigger_undo.size() > 0 ){

        /* keep what we are undoing so it can be redone */
        m_list_trigger_redo.push( m_list_trigger );
        m_list_trigger = m_list_trigger_undo.top();
        m_list_trigger_undo.pop();

        /* the whole list was replaced -- every node the draw/play markers
           pointed at is gone */
        m_iterator_draw_trigger = m_list_trigger.begin();
    }

    unlock();
}


void
sequence::pop_trigger_redo( void )
{
    lock();

    if (m_list_trigger_redo.size() > 0 ){

        m_list_trigger_undo.push( m_list_trigger );
        m_list_trigger = m_list_trigger_redo.top();
        m_list_trigger_redo.pop();

        m_iterator_draw_trigger = m_list_trigger.begin();
    }

    unlock();
}



void
sequence::enforce_column_gaps( long a_min_gap )
{
    if ( a_min_gap < 1 )
        a_min_gap = 1;

    lock();

    /* 1. every tagged note-on, per column, in time order */
    struct On { long tick; int note; int col; };
    std::vector<On> ons;
    for ( list<event>::iterator i = m_list_event.begin();
          i != m_list_event.end(); i++ )
    {
        if ( !(*i).is_note_on() || !(*i).has_column() )
            continue;
        On o; o.tick = (*i).get_timestamp();
        o.note = (int) (*i).get_note();
        o.col  = (int) (*i).get_column();
        ons.push_back( o );
    }
    if ( ons.size() < 2 ) { unlock(); return; }

    std::sort( ons.begin(), ons.end(),
               []( const On& a, const On& b ) {
                   if ( a.col != b.col ) return a.col < b.col;
                   return a.tick < b.tick;
               } );

    /* 2. for each neighbouring pair IN THE SAME COLUMN, decide where the first
          note has to end.  Collect the work first: moving or inserting events
          while walking the list would invalidate the iteration. */
    struct Fix { long onTick; int note; long endTick; };
    std::vector<Fix> fixes;
    for ( size_t k = 0; k + 1 < ons.size(); ++k )
    {
        if ( ons[k].col != ons[k + 1].col )
            continue;
        long want = ons[k + 1].tick - a_min_gap;
        if ( want <= ons[k].tick )
            want = ons[k].tick + 1;          /* never a zero-length note */
        Fix f; f.onTick = ons[k].tick; f.note = ons[k].note; f.endTick = want;
        fixes.push_back( f );
    }

    /* 3. apply: move an existing off that lands too late, or add the off the
          note never had. */
    bool dirty = false;
    for ( size_t k = 0; k < fixes.size(); ++k )
    {
        const Fix& f = fixes[k];
        list<event>::iterator off = m_list_event.end();
        for ( list<event>::iterator i = m_list_event.begin();
              i != m_list_event.end(); i++ )
        {
            if ( !(*i).is_note_off() ) continue;
            if ( (int) (*i).get_note() != f.note ) continue;
            if ( (*i).get_timestamp() <= f.onTick ) continue;
            if ( off == m_list_event.end() ||
                 (*i).get_timestamp() < (*off).get_timestamp() )
                off = i;                     /* earliest off after this on */
        }

        if ( off != m_list_event.end() )
        {
            if ( (*off).get_timestamp() > f.endTick )
            { (*off).set_timestamp( f.endTick ); dirty = true; }
        }
        else
        {
            event e;
            e.set_status( EVENT_NOTE_OFF );
            e.set_data( (char) f.note, 0 );
            e.set_timestamp( f.endTick );
            e.set_column( -1 );
            m_list_event.push_back( e );
            dirty = true;
        }
    }

    if ( dirty )
    {
        m_list_event.sort();
        verify_and_link();
    }
    unlock();
}

void
sequence::snapshot_events( std::vector<EventSnapshot>& out )
{
    lock();
    out.clear();
    for ( list<event>::iterator i = m_list_event.begin();
          i != m_list_event.end(); i++ )
    {
        EventSnapshot e;
        e.tick   = (*i).get_timestamp();
        e.status = (*i).get_status();
        unsigned char d0 = 0, d1 = 0;
        (*i).get_data( &d0, &d1 );
        e.d0 = d0; e.d1 = d1;
        e.column = (*i).has_column() ? (int) (*i).get_column() : -1;
        out.push_back( e );
    }
    unlock();
}

void
sequence::clear_event_columns( void )
{
    lock();
    for ( list<event>::iterator i = m_list_event.begin();
          i != m_list_event.end(); i++ )
        (*i).set_column( -1 );
    unlock();
}

void
sequence::set_event_column( long a_tick, int a_note, int a_occurrence, int a_column )
{
    lock();
    int seen = 0;
    for ( list<event>::iterator i = m_list_event.begin();
          i != m_list_event.end(); i++ )
    {
        if ( !(*i).is_note_on() ) continue;
        if ( (*i).get_timestamp() != a_tick ) continue;
        if ( (int) (*i).get_note() != a_note ) continue;
        if ( seen++ != a_occurrence ) continue;
        (*i).set_column( a_column );
        /* Deliberately NOT tagging the paired note-off through get_linked():
           verify_and_link() has not necessarily run since the last edit, so
           that pointer can dangle.  The emit funnel frees a column by PITCH
           instead (see put_event_on_bus), which needs no link at all. */
        break;
    }
    unlock();
}

void 
sequence::set_master_midi_bus( mastermidibus *a_mmb )
{
    lock();

    m_masterbus = a_mmb;

    unlock();
}


void
sequence::set_song_mute( bool a_mute )
{
    m_song_mute = a_mute;
}

bool
sequence::get_song_mute( void )
{
    return m_song_mute;
}

void 
sequence::set_bpm( long a_beats_per_measure )
{
    lock();
    m_time_beats_per_measure = a_beats_per_measure;
    set_dirty_mp();
    unlock();
}

long 
sequence::get_bpm( void )
{
    return m_time_beats_per_measure;
}

void 
sequence::set_bw( long a_beat_width )
{
    lock();
    m_time_beat_width = a_beat_width;
    set_dirty_mp();
    unlock();
}

long 
sequence::get_bw( void )
{
    return m_time_beat_width;
}


sequence::~sequence()
{
 
}

/* adds event in sorted manner */
void 
sequence::add_event( const event *a_e )
{
    lock();

    // push_BACK (not front) so that among equal-key events (same timestamp AND
    // same rank, e.g. two note-ons at one tick) the NEWEST sorts LAST after the
    // stable sort.  The tracker numbers "occurrences" front-to-back, so the newest
    // duplicate must be the highest occurrence for its lane/velocity/off bookkeeping
    // to land on the right note (else two same-pitch notes swap columns/velocities).
    m_list_event.push_back( *a_e );
    m_list_event.sort( );

    reset_draw_marker();

    set_dirty();

    unlock();
}

void 
sequence::set_orig_tick( long a_tick )
{
    lock();
    m_last_tick = a_tick;
    unlock();
}


void 
sequence::toggle_queued( long a_now_tick )
{
    lock();
    
    set_dirty_mp();
    
    m_queued = !m_queued;

    /*  MEASURE FROM AUDIBLE TIME, NOT FROM THE SCHEDULER'S HORIZON.

        m_last_tick is where perform's output thread has already scheduled up
        to -- one full lookahead (~15 ms of music) past what the listener is
        hearing.  Computing "the next repetition boundary" from it therefore
        SKIPPED the boundary the user was aiming at whenever the press landed
        inside that window: the pattern launched a whole repetition late.
        seq24 had the identical expression, but only ~1 ms of lookahead to be
        wrong by, so it never showed.  perform::sequence_playing_toggle passes
        the transport tick it publishes to the UI, which is audible time.  */
    const long now = ( a_now_tick >= 0 ) ? a_now_tick : m_last_tick;

    /*  "Take effect at the end of the pattern".  seq24 meant the next m_length
        boundary measured from song tick zero; the end of a repetition is now
        the end of the clip's LOOP WINDOW, measured on the clip's own grid (see
        play_span's anchor).  With no window set and a clip on the beat this is
        the same tick it always was. */
    if ( m_length > 0 ){

        const long period = repeat_period();
        const long anchor = ( m_play_anchor >= 0 ) ? m_play_anchor : 0;

        long rel = now - anchor;
        long k   = rel / period;
        if ( rel < 0 && k * period != rel ) --k;      /* C truncates toward 0 */

        m_queued_tick = anchor + ( k + 1 ) * period;
    }
    else
        m_queued_tick = now;
    
    unlock();
}

void
sequence::off_queued( void )
{
    
    lock();
    
    set_dirty_mp();
    
    m_queued = false;
    
    unlock();
}

bool 
sequence::get_queued( void )
{
    return m_queued;
}

long 
sequence::get_queued_tick( void )
{
    return m_queued_tick;
}


/* tick comes in as global tick */
    void 
sequence::play( long a_tick, bool a_playback_mode )
{

    /* HOLDS m_mutex FOR THE WHOLE BODY -- every exit below must unlock().  This
       used to leak the lock on the normal path: the mutex is RECURSIVE, so the
       output thread that calls play() every few ms simply re-entered it and kept
       running, while the count climbed one per block.  Nothing looked wrong from
       the audio side; the GUI thread froze the first time it drew an arrange
       clip (sequence::reset_draw_trigger_marker), which is why "the DAW hangs on
       play with anything in the patcher" only ever reproduced with a window up. */
    lock();

    //printf( "a_tick[%ld] a_playback[%d]\n", a_tick, a_playback_mode );

    /* m_length is the divisor for the pattern phase AND the increment that
       terminates the event walk below.  At zero the first divides by zero and
       the second never terminates, so refuse to run rather than hang the
       output thread on a corrupt pattern. */
    if ( m_length <= 0 ){
        m_last_tick = a_tick + 1;
        unlock();
        return;
    }

    const long window_start = m_last_tick;
    const long window_end   = a_tick;

    if ( getenv("PATCHKNOB_TRACELOOP") ) {
        static long lastAt = -99999;
        if ( a_tick - lastAt > (long) c_ppqn ) {
            lastAt = a_tick;
            fprintf(stderr, "[loop] play() seq='%s'@%p tick=%ld mode=%d "
                    "playing=%d songmute=%d triggers=%zu len=%ld\n",
                    m_name.c_str(), (void*)this, a_tick, (int)a_playback_mode,
                    (int)m_playing, (int)m_song_mute,
                    m_list_trigger.size(), m_length);
        }
    }

    /* CATCH-UP BURST DIAGNOSTIC.
       The scheduler normally advances this in small steps (one lookahead
       horizon, a few ms of music).  If the window ever opens WIDE, the event
       walk runs over the pattern repeatedly and fires every event in the span
       into a single block.  That is heard as a spray of notes with no relation
       to the music.  A wide window means the scheduler thread lost time (an OS
       stall, a long UI/disk operation) and jumped, or m_last_tick was left
       stale by a locate/loop edit.

       This does not change what plays -- it names the cause on stderr the first
       few times it happens, so the burst can be attributed instead of guessed
       at.  A quarter note of slip is already far more than the horizon. */
    if ( m_playing && window_end > window_start ){

        const long span = window_end - window_start;

        if ( span > (long) c_ppqn ){

            static int s_burstWarn = 0;

            if ( s_burstWarn < 8 ){
                fprintf( stderr,
                    "[seq] catch-up burst: %s window %ld ticks "
                    "(%.2f beats, %ld pattern passes) start=%ld end=%ld len=%ld%s\n",
                    m_name.c_str(), span, (double) span / (double) c_ppqn,
                    span / m_length, window_start, window_end, m_length,
                    ++s_burstWarn == 8 ? "  (further warnings suppressed)" : "" );
            }
        }
    }

    if ( m_song_mute )
    {
        set_playing( false, window_start );
    }
    else if ( a_playback_mode )
    {
        /* SEGMENTED trigger playback.

           The old code scanned the trigger list and kept only the LAST state
           change it saw in the window, then played ONE span with ONE offset.
           A window covering the end of one clip and the start of the next
           collapsed to a single trigger, so the second clip was not scheduled
           until a later pass -- and a window spanning a whole short clip could
           miss it entirely.  Walk the window trigger by trigger instead, so
           every clip boundary inside it is honoured at its own tick and with
           its own pattern offset. */
        play_triggered( window_start, window_end );
    }
    else
    {
        /* live mode: no triggers, the pattern runs while it is switched on */
        if ( m_playing )
            play_span( window_start, window_end, 0 );
    }

    /* update for next frame */
    m_last_tick = window_end + 1;
    m_was_playing = m_playing;

    unlock();
}







/*  Will this repetition's own data ever release this note-on?

    Only if its linked note-off is inside what the repetition actually plays:
    [a_lo, a_hi), i.e. the loop window bounded by the end marker.  An off that
    sits at a LOWER tick than its on is NOT out of range -- that note WRAPS the
    pattern end (verify_and_link's second pass pairs the recorded held-key
    case) and is released by the NEXT repetition, so it must be left alone.  An
    off outside the range, or a note-on with no off at all, is stranded: the
    boundary has to cut it. */
static bool
event_is_stranded( event &a_e, long a_lo, long a_hi )
{
    if ( ! a_e.is_note_on() )
        return false;

    if ( ! a_e.is_linked() )
        return true;                     /* no note-off anywhere */

    const long off = a_e.get_linked()->get_timestamp();

    return ( off < a_lo || off >= a_hi );
}


/*  One trigger-state segment: every event whose pattern position falls in
    [a_start,a_end] is emitted, carrying its ABSOLUTE tick so the audio thread
    places it at the exact sample.  a_trigger_offset is the covering trigger's
    offset (0 in live mode).  */
void
sequence::play_span( long a_start, long a_end, long a_trigger_offset,
                     long a_trigger_start )
{
    if ( m_length <= 0 || a_end < a_start )
        return;

    set_trigger_offset( a_trigger_offset );

    /*  ONE-SHOT.  Looping is optional: with it off the clip's data plays once
        from where the clip starts and then stops, so dragging the clip longer
        in the arrange view just moves its end point instead of repeating the
        pattern.  There is no wrap here at all -- the pattern is walked once,
        anchored to the clip's start tick, and m_length simply bounds how far
        the data extends (the END marker).

        Live mode (no covering trigger, a_trigger_start < 0) has no clip to
        anchor to, so it keeps the repeating behaviour. */
    if ( !m_loop_enabled && a_trigger_start >= 0 ){

        const long off = m_trigger_offset;

        /*  Where this clip's content grid starts, in song ticks: pattern
            position p sounds at a_trigger_start + (p - off).  get_last_tick()
            inverts this for the editors' playhead. */
        m_play_anchor = a_trigger_start - off;

        for ( list<event>::iterator e  = m_list_event.begin();
                                    e != m_list_event.end(); e++ ){

            const long ts = (*e).get_timestamp();

            /* bounded by the END marker, and nothing before the clip's own
               starting content position */
            if ( ts < off || ts >= m_length )
                continue;

            const long due = a_trigger_start + ( ts - off );

            if ( due > a_end )
                break;                       /* list is time-ordered */
            if ( due >= a_start )
                put_event_on_bus( &(*e), due );
        }

        /*  THE DATA ENDS AT THE END MARKER, AND SO MUST THE SOUND.  A one-shot
            has no next repetition, so a note whose note-off lies at or past
            m_length -- HIDDEN by a set_length() shrink, or simply never
            recorded -- had nothing left that could release it and was held
            until the trigger itself ended.  Release everything still sounding
            at the tick the data runs out.  Every event emitted above has
            ts < m_length, so this tick is strictly after all of them. */
        if ( m_length > off ){

            const long data_end = a_trigger_start + ( m_length - off );

            if ( data_end >= a_start && data_end <= a_end )
                off_playing_notes( data_end );
        }
        return;
    }

    /*  PATTERN-LOCAL LOOP.  [m_loop_start, m_loop_end) is THIS sequence's own
        loop window -- the one the piano roll's ruler sets.  It is per sequence,
        so every clip loops its own window, with its own length, independently
        of every other clip on this track or any other.  It is NOT the song
        loop (perform's left/right tick), which is one global transport range.

        When a loop is engaged the pattern repeats that WINDOW instead of its
        whole length: a 4-step loop over a 1-bar pattern plays those 4 steps
        over and over, rather than the bar. Events outside the window are
        skipped exactly the way out-of-range events already are.

        DEFAULT IS BIT-IDENTICAL.  With no loop set (start 0, end == length)
        `period` is m_length and `loop_start` is 0, so every expression below
        reduces to what it was before.  */
    const bool loop_set = loop_window_set();
    const long loop_start = loop_set ? m_loop_start : 0;
    const long loop_end   = loop_set ? m_loop_end   : m_length;
    const long period     = loop_end - loop_start;
    if ( period <= 0 )
        return;

    if ( getenv("PATCHKNOB_TRACELOOP") ) {
        /* Print on CHANGE, not first-N: a fixed cap silences the trace during
           the first drag step and hides everything that follows. */
        static long lp_s = -9, lp_e = -9, lp_l = -9; static int lp_en = -9;
        static const void* lp_p = 0;
        const bool changed = ( lp_s != m_loop_start || lp_e != m_loop_end ||
                               lp_l != m_length || lp_en != (int)m_loop_enabled ||
                               lp_p != (const void*)this );
        if ( changed ) {
            lp_s = m_loop_start; lp_e = m_loop_end; lp_l = m_length;
            lp_en = (int)m_loop_enabled; lp_p = (const void*)this;
            fprintf(stderr,
                "[loop] play_span seq='%s'@%p len=%ld loop=[%ld,%ld) set=%d "
                "period=%ld span=[%ld,%ld] trigoff=%ld enabled=%d\n",
                m_name.c_str(), (void*)this, m_length, m_loop_start, m_loop_end,
                (int)loop_set, loop_end - loop_start, a_start, a_end,
                a_trigger_offset, (int)m_loop_enabled);
        }
    }

    /* Content offset folds into the window, not the whole pattern. */
    const long trig_off = loop_set ? ( m_trigger_offset % period ) : m_trigger_offset;

    /*  PHASE ANCHORS AT THE CLIP, NOT AT SONG TICK ZERO.
        This used to be `offset_base = (a_start / period) * period`, i.e. a
        repetition grid laid out from tick 0 of the SONG.  That is wrong for any
        period that does not divide the bar: the same clip started on a
        different step depending on where it sat in the arrangement, and moving
        it changed which step you heard first.  Every DAW with a per-clip loop
        (Reaper item-loop, Live arrangement clips) lays repetitions out from the
        clip's own left edge:

            phase(g) = loop_start + ((g - clip_start + content_offset) mod period)

        `anchor` is the absolute tick where this clip's repetition grid begins.
        Live mode has no covering clip (a_trigger_start < 0) and keeps the
        song-zero grid it always had.

        The walk below is now in ABSOLUTE ticks.  The old code offset both the
        window and every event by +period-trig_off and undid it at the emit,
        which only worked because the grid was global. */
    const long anchor = ( a_trigger_start >= 0 ) ? ( a_trigger_start - trig_off )
                                                 : 0;
    /* remembered for get_last_tick(): the drawn playhead has to fold by the
       SAME grid that sounds, or it drifts on any clip whose start tick is not
       a whole number of repetitions from song zero */
    m_play_anchor = ( a_trigger_start >= 0 ) ? anchor : -1;
    /* floor-divide: a_start can precede the anchor, and C truncates toward
       zero, which would place the grid one repetition late there. */
    long rel = a_start - anchor;
    long k   = rel / period;
    if ( rel < 0 && k * period != rel ) --k;
    long offset_base = anchor + k * period;

    /*  Where this repetition's DATA stops.  Events at or past m_length are
        hidden (see set_length), so once the walk is past that point nothing
        left in the repetition can release a note it started -- which matters
        whenever a shrink left the loop window reaching beyond the data, the
        standard way to hang an odd-length loop over a shorter phrase.  With no
        such overhang this is just loop_end and the two cuts coincide. */
    const long data_end = ( m_length < loop_end ) ? m_length : loop_end;

    /*  END OF ONE REPETITION.  Release what this repetition started and cannot
        release itself (see cut_stranded_notes), then move the grid on.  The
        data end comes first because it is at or before the boundary, and both
        are emitted before the next repetition's events at the same tick, so a
        cut note is off before it is struck again. */
    auto wrap_repetition = [&]( void ){

        if ( data_end > loop_start ){
            const long cut = offset_base + ( data_end - loop_start );
            if ( cut >= a_start && cut <= a_end && cut != offset_base + period )
                cut_stranded_notes( cut );
        }

        offset_base += period;

        if ( offset_base >= a_start && offset_base <= a_end )
            cut_stranded_notes( offset_base );
    };

    /*  A window that OPENS exactly on a boundary reaches it by construction --
        offset_base is floor-aligned to a_start -- not by wrapping, so the
        release above would never fire for it.  The previous window stopped one
        tick short of this boundary, so nothing has cut it yet. */
    if ( offset_base == a_start )
        cut_stranded_notes( a_start );

    list<event>::iterator e = m_list_event.begin();

    while ( e != m_list_event.end() ){

        /* An event outside [0, m_length) is HIDDEN (see set_length): it is
           kept so a later length increase restores it, but it must never sound
           and -- because the list is time-ordered -- must not end the walk
           either, or everything after it would stop playing. */
        /* Outside the window -> not part of this repetition.  At or past
           m_length -> HIDDEN (kept only so a re-grow can restore it), and a
           window that reaches past the data must not resurrect those. */
        if ( (*e).get_timestamp() >= loop_end || (*e).get_timestamp() < loop_start ||
             (*e).get_timestamp() >= m_length || (*e).get_timestamp() < 0 ){
            e++;
            if ( e == m_list_event.end() ){
                e = m_list_event.begin();
                wrap_repetition();
                if ( offset_base > a_end )
                    break;                       /* see the note below */
            }
            continue;
        }

        /* window-local position, so the loop's first step lands on each
           repetition boundary */
        const long due = (*e).get_timestamp() - loop_start + offset_base;

        if ( due >= a_start && due <= a_end ){

            /* already an absolute song tick: the grid is anchored, not skewed */
            put_event_on_bus( &(*e), due,
                              event_is_stranded( *e, loop_start, data_end ) );
            if ( getenv("PATCHKNOB_TRACELOOP") ) {
                static int emitted = 0; static long lastReport = 0;
                ++emitted;
                if ( a_start - lastReport > 4 * (long) c_ppqn ) {
                    lastReport = a_start;
                    fprintf(stderr, "[loop] emitted=%d events so far "
                            "(loop=[%ld,%ld) period=%ld)\n",
                            emitted, m_loop_start, m_loop_end,
                            m_loop_end - m_loop_start);
                }
            }
        }
        else if ( due > a_end ){
            break;
        }

        e++;

        /* wrapped the pattern: advance a repetition and keep going */
        if ( e == m_list_event.end() ){
            e = m_list_event.begin();
            wrap_repetition();

            /*  HARD TERMINATION.  The only other way out of this walk is the
                `due > a_end` break, and a HIDDEN event never reaches
                it -- it `continue`s.  So a pattern in which EVERY event is
                hidden spun here forever, on the output thread, holding
                m_mutex: the whole DAW froze.  That is not hypothetical since
                set_length() stopped pruning; shorten a clip below its first
                note (all events now >= m_length) and press play.

                Every in-range event has a timestamp >= 0, so its due tick is
                >= offset_base.  Once offset_base itself is past the end of the
                window, no event in any later repetition can be due, and
                offset_base grows by m_length (> 0, checked above) every wrap --
                so this always terminates.  (`period` is > 0, checked above,
                whether or not a pattern-local loop is engaged.)  */
            if ( offset_base > a_end )
                break;
        }
    }
}


/*  Play [a_start,a_end] trigger by trigger.  Each covered stretch is played
    with that trigger's own offset, and the sequence is released AT the tick a
    trigger ends rather than at the next block boundary.  */
void
sequence::play_triggered( long a_start, long a_end )
{
    if ( a_end < a_start )
        return;

    if ( m_list_trigger.size() == 0 ){
        if ( m_playing )
            set_playing( false, a_start );
        return;
    }

    long cursor = a_start;

    while ( cursor <= a_end ){

        /* the trigger covering `cursor`, else the next one starting after it
           (the list is kept sorted by start tick) */
        trigger *cover = NULL;
        trigger *next  = NULL;

        for ( list<trigger>::iterator i  = m_list_trigger.begin();
                                      i != m_list_trigger.end(); i++ ){

            if ( (*i).m_tick_start <= cursor && cursor <= (*i).m_tick_end ){
                cover = &(*i);
                break;
            }
            if ( (*i).m_tick_start > cursor ){
                next = &(*i);
                break;
            }
        }

        if ( cover != NULL ){

            const long seg_end = ( a_end < cover->m_tick_end )
                                 ? a_end : cover->m_tick_end;

            if ( !m_playing )
                set_playing( true );

            play_span( cursor, seg_end, cover->m_offset, cover->m_tick_start );

            /* the trigger ENDS inside this window: release its held notes at
               that exact tick before whatever follows */
            if ( seg_end == cover->m_tick_end )
                set_playing( false, cover->m_tick_end );

            cursor = seg_end + 1;
        }
        else {

            /* a gap: nothing sounds here */
            if ( m_playing )
                set_playing( false, cursor );

            if ( next == NULL || next->m_tick_start > a_end )
                break;

            cursor = next->m_tick_start;
        }
    }
}


void 
sequence::zero_markers( void )
{
    lock();

    m_last_tick = 0;
    m_play_anchor = -1;          /* no clip is driving the playhead any more */

    /* SCALE-FOLLOW: clear any stale snap latches on transport reset */
    for ( int x=0; x< c_midi_notes; x++ )
        m_note_remap[x] = -1;

    //m_masterbus->flush( );

    unlock();
}


/* verfies state, all noteons have an off,
   links noteoffs with their ons */
void 
sequence::verify_and_link( bool a_prune )
{
    
    list<event>::iterator i;
    list<event>::iterator on;
    list<event>::iterator off;

    lock();

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	(*i).clear_link();
    (*i).unmark();
    }

    on = m_list_event.begin();
	    
    /* pair ons and offs */
    while ( on != m_list_event.end() ){

	/* check for a note on, then look for its
	   note off */
	if ( (*on).is_note_on() ){

	    /* get next possible off node */
	    off = on; off++;

	    while ( off != m_list_event.end() ){
		
		/* is a off event, == notes, and isnt
		   markeded  */
		if ( (*off).is_note_off()                  &&
		     (*off).get_note() == (*on).get_note() && 
		     ! (*off).is_marked()                  ){

		    /* link + mark */
		    (*on).link( &(*off) );
		    (*off).link( &(*on) );
		    (*on).mark(  );
		    (*off).mark( );

		    break;
		}
		off++;
	    }
	}
	on++;
    }

    /*  SECOND PASS: notes that WRAP THE PATTERN END.

        Recording appends modulo the pattern length (sequence::stream_event), so
        a key held across the loop point lands its note-ON near the end of the
        pattern and its note-OFF near the START -- BEFORE the on in the list.
        The forward pass above can never pair those two: it only ever looks
        further down a time-ordered list.  The result was an unlinked note-on
        (drawn as a stub, no length, no partner to move or delete with it) plus
        an orphan note-off sitting at the top of the pattern.

        Both editors already draw this case -- see the "note that WRAPS the
        pattern end has its off at a LOWER tick than its on" handling in
        pianoroll.cpp and arrange_view.cpp -- so the pairing is what was
        missing, not the presentation.

        Only LEFTOVERS are considered: everything the forward pass paired is
        marked, so a well-formed pattern reaches here with nothing to do.  This
        deliberately does NOT live in link_new(): that runs after every single
        recorded event, where an orphan off from an earlier pass would be
        snatched by the next note-on to arrive, stranding the on's real off.

        Guarded by an O(n) scan for an unclaimed off, because the pass itself is
        O(n) per unpaired note-on: a pattern of bare one-shot note-ons (a drum
        part imported without offs) has an unpaired on for EVERY event, and
        add_note_velocity calls this once per recorded note.  With nothing for
        them to pair with there is no reason to look. */
    bool have_orphan_off = false;
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	if ( (*i).is_note_off() && ! (*i).is_marked() ){
	    have_orphan_off = true;
	    break;
	}
    }

    for ( on = m_list_event.begin(); have_orphan_off && on != m_list_event.end(); on++ ){

	if ( ! (*on).is_note_on() || (*on).is_marked() )
	    continue;

	/* the EARLIEST unclaimed off of this pitch, which must lie before the
	   on -- anything after it would have been taken by the forward pass */
	for ( off = m_list_event.begin(); off != on; off++ ){

	    if ( (*off).is_note_off()                  &&
		 (*off).get_note() == (*on).get_note() &&
		 ! (*off).is_marked()                  ){

		(*on).link( &(*off) );
		(*off).link( &(*on) );
		(*on).mark(  );
		(*off).mark( );

		break;
	    }
	}
    }

    /* unmark all */
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	(*i).unmark();
    }

    /* kill those not in range */
    if ( a_prune )
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	
	/* if our current time stamp is greater then the length */

	if ( (*i).get_timestamp() >= m_length ||
	     (*i).get_timestamp() < 0            ){
	    
	    /* we have to prune it */
	    (*i).mark();
	    if ( (*i).is_linked() )
		(*i).get_linked()->mark();
	}
    }

    remove_marked( );
    unlock();
}
    




void 
sequence::link_new( )
{
    list<event>::iterator on;
    list<event>::iterator off;

    lock();

    on = m_list_event.begin();

    /* pair ons and offs */
    while ( on != m_list_event.end()){

	/* check for a note on, then look for its
	   note off */
	if ( (*on).is_note_on() &&
	     ! (*on).is_linked() ){
	    
	    /* get next element */
	    off = on; off++;
	    
	    while ( off != m_list_event.end()){

		/* is a off event, == notes, and isnt
		   selected  */
		if ( (*off).is_note_off()                    &&
		     (*off).get_note() == (*on).get_note() && 
		     ! (*off).is_linked()                    ){
		    
		    /* link */
		    (*on).link( &(*off) );
		    (*off).link( &(*on) );
		    
		    break;
		}
		off++;
	    }
	}    
	on++;
    }
    unlock();
}




void 
sequence::remove_marked( )
{
    list<event>::iterator i, t;

    lock();

    i = m_list_event.begin();
    while( i != m_list_event.end() ){
	
	if ((*i).is_marked()){
	    
	    /* if its a note off, and that note is currently
	       playing, send a note off */
	    if ( (*i).is_note_off()  &&
		 m_playing_notes[ (*i).get_note()] > 0 ){
		
                m_masterbus->play( m_bus, &(*i), m_midi_channel );
                m_playing_notes[(*i).get_note()]--;
	    }
	   
	    /* The partner of an erased event keeps a RAW pointer to it.  When
	       only one half of a pair is marked (a selection that caught the
	       note-on but not its off), that pointer was left dangling and the
	       next get_linked() dereference read freed memory. */
	    if ( (*i).is_linked() ){
		event *partner = (*i).get_linked();
		if ( partner != NULL )
		    partner->clear_link();
	    }

	    t = i; t++;
	    m_list_event.erase(i);
	    i = t;
	}
	else {

	    i++;
	}
    }

    reset_draw_marker();

    unlock();
}

/* Remove exactly one note-on at a precise time and pitch.  The selection API
   intentionally includes a small timing tolerance for mouse editing, which is
   wrong for tracker cells: adjacent high-LPB rows can otherwise be deleted. */
bool
sequence::remove_note_at( long a_tick, int a_note )
{
    return remove_note_at( a_tick, a_note, 0 );
}

bool
sequence::remove_note_at( long a_tick, int a_note, int a_occurrence )
{
    lock();

    int occurrence = 0;
    for ( list<event>::iterator i = m_list_event.begin();
          i != m_list_event.end(); ++i )
    {
        if ( (*i).is_note_on() && (*i).get_timestamp() == a_tick &&
             (*i).get_note() == a_note )
        {
            if ( occurrence++ != a_occurrence )
                continue;
            (*i).mark();
            if ( (*i).is_linked() )
                (*i).get_linked()->mark();
            remove_marked();
            set_dirty();
            unlock();
            return true;
        }
    }

    unlock();
    return false;
}



void 
sequence::mark_selected( )
{
    list<event>::iterator i, t;

    lock();

    i = m_list_event.begin();
    while( i != m_list_event.end() ){
	
	if ((*i).is_selected()){

        (*i).mark();
    }
    ++i;
    }
    reset_draw_marker();

    unlock();
}

void 
sequence::unpaint_all( )
{
    list<event>::iterator i;

    lock();

    i = m_list_event.begin();
    while( i != m_list_event.end() ){
        (*i).unpaint();
        i++;
    }
    unlock();
}


/* returns the 'box' of the selected items */
void 
sequence::get_selected_box( long *a_tick_s, int *a_note_h, 
			    long *a_tick_f, int *a_note_l )
{

    list<event>::iterator i;

    *a_tick_s = c_maxbeats * c_ppqn;
    *a_tick_f = 0;

    *a_note_h = 0;
    *a_note_l = 128;

    long time;
    int note;

    lock();

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	if( (*i).is_selected() ){
	    
	    time = (*i).get_timestamp();
	    
	    if ( time < *a_tick_s ) *a_tick_s = time;
	    if ( time > *a_tick_f ) *a_tick_f = time;
	    
	    note = (*i).get_note();

	    if ( note < *a_note_l ) *a_note_l = note;
	    if ( note > *a_note_h ) *a_note_h = note;
	}
    }
    
    unlock();
}

void 
sequence::get_clipboard_box( long *a_tick_s, int *a_note_h, 
			     long *a_tick_f, int *a_note_l )
{

    list<event>::iterator i;
    
    *a_tick_s = c_maxbeats * c_ppqn;
    *a_tick_f = 0;
    
    *a_note_h = 0;
    *a_note_l = 128;
    
    long time;
    int note;
    
    lock();

    if ( m_list_clipboard.size() == 0 ) {
	*a_tick_s = *a_tick_f = *a_note_h = *a_note_l = 0; 
    }

    for ( i = m_list_clipboard.begin(); i != m_list_clipboard.end(); i++ ){
	
	time = (*i).get_timestamp();
	
	if ( time < *a_tick_s ) *a_tick_s = time;
	if ( time > *a_tick_f ) *a_tick_f = time;
	
	note = (*i).get_note();
	
	if ( note < *a_note_l ) *a_note_l = note;
	if ( note > *a_note_h ) *a_note_h = note;
    }
   
    unlock();
}



int 
sequence::get_num_selected_notes( )
{
    int ret = 0;

    list<event>::iterator i;

    lock();

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        if( (*i).is_note_on()                &&
            (*i).is_selected() ){
            ret++;
        }
    }

    unlock();

    return ret;

}


int 
sequence::get_num_selected_events( unsigned char a_status, 
                                   unsigned char a_cc )
{
    int ret = 0;
    list<event>::iterator i;

    lock();

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        if( (*i).get_status()    == a_status ){

            unsigned char d0,d1;
            (*i).get_data( &d0, &d1 );

            if ( (a_status == EVENT_CONTROL_CHANGE && d0 == a_cc )
                 || (a_status != EVENT_CONTROL_CHANGE) ){

                if ( (*i).is_selected( ))
                    ret++;
            }
        }
    }
    
    unlock();
    
    return ret;
}


/* selects events in range..  tick start, note high, tick end
   note low */
    int
sequence::select_note_events( long a_tick_s, int a_note_h,
        long a_tick_f, int a_note_l, select_action_e a_action)
{
    int ret=0;
    list<event>::iterator i;

    lock();

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        if( (*i).is_note_off()                &&
                (*i).get_timestamp() >= a_tick_s &&
                (*i).get_note()      <= a_note_h &&
                (*i).get_note()      >= a_note_l ){


            if ( (*i).is_linked() ){

                event *ev = (*i).get_linked();

                if ( ev->get_timestamp() <= a_tick_f ){

                    if ( a_action == e_select ||
                         a_action == e_select_one )
                    {
                        (*i).select( );
                        ev->select( );
                        ret++;
                        if ( a_action == e_select_one )
                            break;
                    }
                    if ( a_action == e_is_selected )
                    {
                        if ( (*i).is_selected())
                        {
                            ret = 1;
                            break;
                        }
                    }
                    if ( a_action == e_would_select )
                    {
                        ret = 1;
                        break;
                    }
                }
            }
        }

        if ( ! (*i).is_linked() &&
                ( (*i).is_note_on() ||
                  (*i).is_note_off() ) &&
                (*i).get_timestamp()  >= a_tick_s - 16 &&
                (*i).get_timestamp()  <= a_tick_f && 
                (*i).get_note()       <= a_note_h &&
                (*i).get_note()       >= a_note_l ) {

            if ( a_action == e_select ||
                 a_action == e_select_one )
            {
                (*i).select( );
                ret++;
                if ( a_action == e_select_one )
                    break;
            }
            if ( a_action == e_is_selected )
            {
                if ( (*i).is_selected())
                {
                    ret = 1;
                    break;
                }
            }
            if ( a_action == e_would_select )
            {
                ret = 1;
                break;
            }
        }
    }

    unlock();

    return ret;
}

/* select events in range, returns number
   selected */
int 
sequence::select_events( long a_tick_s, long a_tick_f, 
			 unsigned char a_status, 
			 unsigned char a_cc, select_action_e a_action)
{
    int ret=0;
    list<event>::iterator i;

    lock();

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        if( (*i).get_status()    == a_status && 
                (*i).get_timestamp() >= a_tick_s &&
                (*i).get_timestamp() <= a_tick_f ){

            unsigned char d0,d1;
            (*i).get_data( &d0, &d1 );

            if ( (a_status == EVENT_CONTROL_CHANGE &&
                        d0 == a_cc )
                    || (a_status != EVENT_CONTROL_CHANGE) ){


                if ( a_action == e_select ||
                     a_action == e_select_one )
                {
                    (*i).select( );
                    ret++;
                    if ( a_action == e_select_one )
                        break;
                }
                if ( a_action == e_is_selected )
                {
                    if ( (*i).is_selected())
                    {
                        ret = 1;
                        break;
                    }
                }
                if ( a_action == e_would_select )
                {
                    ret = 1;
                    break;
                }
            }
        }
    }
        unlock();

        return ret;
}



void
sequence::select_all( void )
{
    lock();

    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ )
	(*i).select( );

    unlock();
}


/* unselects every event */
void 
sequence::unselect( void )
{
    lock();

    list<event>::iterator i;
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ )
	(*i).unselect();

    unlock();
}


/* removes and adds readds selected in position */
void 
sequence::move_selected_notes( long a_delta_tick, int a_delta_note )
{
    event e;

    lock();

    mark_selected();

    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        /* is it being moved ? */
        if ( (*i).is_marked() ){

            /* copy event */
            e  = (*i);
            e.unmark();

            if ( (e.get_timestamp() + a_delta_tick) >= 0   &&
                 (e.get_note() + a_delta_note)      >= 0   &&
                 (e.get_note() + a_delta_note)      <  c_num_keys ){

                e.set_timestamp( e.get_timestamp() + a_delta_tick );
                e.set_note( e.get_note() + a_delta_note );
                e.select();

                add_event( &e );
            }
        }
    }

    remove_marked();
    verify_and_link();

    unlock();
}

    
/* stretch */
void 
sequence::stretch_selected( long a_delta_tick )
{
    event *e, new_e;

    lock();

    list<event>::iterator i;

    int old_len = 0, new_len = 0;
    int first_ev = 0x7fffffff;
    int last_ev = 0x00000000;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
        if ( (*i).is_selected() )
        {
            e = &(*i);

            if (e->get_timestamp() < first_ev) {
                first_ev = e->get_timestamp();
            }
            if (e->get_timestamp() > last_ev) {
                last_ev = e->get_timestamp();
            }
        }
    }

    old_len = last_ev - first_ev;

    /*  DIVIDE BY ZERO.  old_len is the span of the selection, and it is zero
        whenever the selection is a SINGLE event -- or several events all on the
        same tick, which is what a chord is.  float/0.0f is +inf,
        (ts - first_ev) is then 0 * inf == NaN, and long(NaN) is undefined
        behaviour: every selected event came out with an arbitrary timestamp (in
        practice LONG_MIN, which verify_and_link's prune then swept away).
        Stretching a single note deleted it.  With NO selection, first_ev and
        last_ev keep their sentinels and old_len is hugely negative -- which the
        new_len test below happened to catch, which is why only the
        single-selection case ever showed up. */
    if ( old_len <= 0 ){
        unlock();
        return;
    }

    new_len = old_len + a_delta_tick;
    float ratio = float(new_len)/float(old_len);

    if( new_len > 1) {

        mark_selected();

        for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
            if ( (*i).is_marked() ){

                e = &(*i);

                /* copy & scale event */
                new_e = *e;
                new_e.set_timestamp( long((e->get_timestamp() - first_ev) * ratio) + first_ev );
               
                new_e.unmark();
                
                add_event( &new_e );
            }
        }

        remove_marked();
        verify_and_link();
    }

    unlock();

#if 0
    event *on, *off, new_on, new_off;

    lock();

    list<event>::iterator i;

    int old_len = 0, new_len = 0;
    int first_ev = 0x7fffffff;
    int last_ev = 0x00000000;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
        if ( (*i).is_selected() &&
                (*i).is_note_on() &&
                (*i).is_linked() ){

            on = &(*i);
            off = (*i).get_linked();

            if (on->get_timestamp() < first_ev) {
                first_ev = on->get_timestamp();
            }
            if (off->get_timestamp() > last_ev) {
                last_ev = off->get_timestamp();
            }
        }
    }

    old_len = last_ev - first_ev;
    new_len = old_len + a_delta_tick;
    float ratio = float(new_len)/float(old_len);

    if( new_len > 1) {

        mark_selected();
        
        for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
            if ( (*i).is_marked() &&
                    (*i).is_note_on() &&
                    (*i).is_linked() ){

                on = &(*i);
                off = (*i).get_linked();

                /* copy & scale event */
                new_on = *on;
                new_on.set_timestamp( long((on->get_timestamp() - first_ev) * ratio) + first_ev );
                new_off = *off;
                new_off.set_timestamp( long((off->get_timestamp() - first_ev) * ratio) + first_ev );
               
                new_on.unmark();
                new_off.unmark();
                
                add_event( &new_on );
                add_event( &new_off );
            }
        }

        remove_marked();
        verify_and_link();
    }

    unlock();

#endif
}


/* moves note off event */
void 
sequence::grow_selected( long a_delta_tick )
{
    event *on, *off, e;

    lock();

    list<event>::iterator i;

    mark_selected();
    
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        if ( (*i).is_marked() &&
                (*i).is_note_on() &&
                (*i).is_linked() ){

            on = &(*i);
            off = (*i).get_linked();

            long length = 
                off->get_timestamp() - 
                on->get_timestamp() +
                a_delta_tick;
            on->unmark();

            if ( length < 1 )
                length = 1;

            /* copy event */
            e  = *off;
            e.unmark();

            e.set_timestamp( on->get_timestamp() + length );
            add_event( &e );
        }
    }

    remove_marked();
    verify_and_link();
    
    unlock();
}


void 
sequence::increment_selected( unsigned char a_status, unsigned char a_control )
{
    lock();

    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	
        if ( (*i).is_selected() &&
             (*i).get_status() == a_status ){
			
            if ( a_status == EVENT_NOTE_ON || 
                 a_status == EVENT_NOTE_OFF || 
                 a_status == EVENT_AFTERTOUCH ||
                 a_status == EVENT_CONTROL_CHANGE || 
                 a_status == EVENT_PITCH_WHEEL ){
				
                (*i).increment_data2();
            }
			
            if ( a_status == EVENT_PROGRAM_CHANGE ||
                 a_status == EVENT_CHANNEL_PRESSURE ){
				
                (*i).increment_data1();
            }
        }
    }
        
    unlock();
}


void 
sequence::decrement_selected(unsigned char a_status, unsigned char a_control )
{
    lock();

    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	
        if ( (*i).is_selected() &&
             (*i).get_status() == a_status ){
			
            if ( a_status == EVENT_NOTE_ON || 
                 a_status == EVENT_NOTE_OFF || 
                 a_status == EVENT_AFTERTOUCH ||
                 a_status == EVENT_CONTROL_CHANGE || 
                 a_status == EVENT_PITCH_WHEEL ){
				
                (*i).decrement_data2();
            }
			
            if ( a_status == EVENT_PROGRAM_CHANGE ||
                 a_status == EVENT_CHANNEL_PRESSURE ){
				
                (*i).decrement_data1();
            }
			
        }
    }
        
    unlock();
}




void
sequence::copy_selected( void )
{
    list<event>::iterator i;

    lock();

    m_list_clipboard.clear( );

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	if ( (*i).is_selected() ){
	    m_list_clipboard.push_back( (*i) );
	}
    }

    if ( m_list_clipboard.empty() )
    {
        unlock();
        return;
    }

    long first_tick = (*m_list_clipboard.begin()).get_timestamp();
    
    for ( i = m_list_clipboard.begin(); i != m_list_clipboard.end(); i++ ){

	(*i).set_timestamp((*i).get_timestamp() - first_tick );
    }

    unlock();
}

void 
sequence::paste_selected( long a_tick, int a_note )
{
    lock();

    if ( m_list_clipboard.empty() || m_length <= 0 )
    {
        unlock();
        return;
    }

    list<event> clipboard = m_list_clipboard;

    bool has_notes = false;
    int highest_note = 0;
    for ( list<event>::iterator i = clipboard.begin(); i != clipboard.end(); i++ )
    {
        if ( (*i).is_note_on() || (*i).is_note_off() )
        {
            has_notes = true;
            if ( (*i).get_note() > highest_note )
                highest_note = (*i).get_note();
        }
    }

    list<event> pasted;

    if ( has_notes )
    {
        std::vector<event> evs( clipboard.begin(), clipboard.end() );
        std::vector<bool>  used( evs.size(), false );
        int delta_note = a_note - highest_note;

        for ( size_t i = 0; i < evs.size(); ++i )
        {
            if ( !evs[i].is_note_on() )
                continue;

            int new_note = (int) evs[i].get_note() + delta_note;
            if ( new_note < 0 || new_note >= c_num_keys )
                continue;

            long ts = evs[i].get_timestamp() + a_tick;
            if ( ts < 0 || ts >= m_length - 1 )
                continue;

            size_t off_index = evs.size();
            for ( size_t j = i + 1; j < evs.size(); ++j )
            {
                if ( !used[j] && evs[j].is_note_off() &&
                     evs[j].get_note() == evs[i].get_note() )
                {
                    off_index = j;
                    break;
                }
            }
            if ( off_index == evs.size() )
                continue;

            long tf = evs[off_index].get_timestamp() + a_tick;
            if ( tf <= ts )
                tf = ts + 1;
            if ( tf >= m_length )
                tf = m_length - 1;
            if ( tf <= ts )
                continue;

            event on = evs[i];
            event off = evs[off_index];
            on.clear_link();
            off.clear_link();
            on.unmark();
            off.unmark();
            on.select();
            off.select();
            on.set_note( (char) new_note );
            off.set_note( (char) new_note );
            on.set_timestamp( ts );
            off.set_timestamp( tf );
            pasted.push_back( on );
            pasted.push_back( off );
            used[off_index] = true;
        }

        for ( size_t i = 0; i < evs.size(); ++i )
        {
            if ( evs[i].is_note_on() || evs[i].is_note_off() )
                continue;
            long ts = evs[i].get_timestamp() + a_tick;
            if ( ts < 0 || ts >= m_length )
                continue;
            event e = evs[i];
            e.clear_link();
            e.unmark();
            e.select();
            e.set_timestamp( ts );
            pasted.push_back( e );
        }
    }
    else
    {
        for ( list<event>::iterator i = clipboard.begin(); i != clipboard.end(); i++ )
        {
            long ts = (*i).get_timestamp() + a_tick;
            if ( ts < 0 || ts >= m_length )
                continue;
            event e = *i;
            e.clear_link();
            e.unmark();
            e.select();
            e.set_timestamp( ts );
            pasted.push_back( e );
        }
    }

    pasted.sort();
    m_list_event.merge( pasted );
    m_list_event.sort();

    verify_and_link();

    reset_draw_marker();
    
    unlock();

}


/* change */
void 
sequence::change_event_data_range( long a_tick_s, long a_tick_f,
				   unsigned char a_status,
				   unsigned char a_cc,
				   int a_data_s,
				   int a_data_f )
{
    lock();

    unsigned char d0, d1;
    list<event>::iterator i;

    /* change only selected events, if any */
    bool have_selection = false;
    if( get_num_selected_events(a_status, a_cc) )
        have_selection = true;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        /* initially false */
        bool set = false;	
        (*i).get_data( &d0, &d1 );

        /* correct status and not CC */
        if ( a_status != EVENT_CONTROL_CHANGE &&
                (*i).get_status() == a_status )
            set = true;

        /* correct status and correct cc */
        if ( a_status == EVENT_CONTROL_CHANGE &&
                (*i).get_status() == a_status &&
                d0 == a_cc )
            set = true;

        /* in range? */
        if ( !((*i).get_timestamp() >= a_tick_s &&
                    (*i).get_timestamp() <= a_tick_f ))
            set = false;

        /* in selection? */
        if ( have_selection && (!(*i).is_selected()) )
            set = false;

        if ( set ){

            float weight;

            /* no divide by 0 */
            if( a_tick_f == a_tick_s )
                a_tick_f = a_tick_s + 1;

            /* ratio of v1 to v2 */
            /*
               weight =
               (float)( (*i).get_timestamp() - a_tick_s ) /
               (float)( a_tick_f - a_tick_s );

               int newdata = (int)
               ((weight         * (float) a_data_f ) +
               ((1.0f - weight) * (float) a_data_s ));
               */

            int tick = (*i).get_timestamp();

            //printf("ticks: %d %d %d\n", a_tick_s, tick, a_tick_f);
            //printf("datas: %d %d\n", a_data_s, a_data_f);

            int newdata = ((tick-a_tick_s)*a_data_f + (a_tick_f-tick)*a_data_s)
                /(a_tick_f - a_tick_s);

            if ( newdata < 0 ) newdata = 0;
            if ( newdata > 127 ) newdata = 127;

            if ( a_status == EVENT_NOTE_ON )
                d1 = newdata;

            if ( a_status == EVENT_NOTE_OFF )
                d1 = newdata;

            if ( a_status == EVENT_AFTERTOUCH )
                d1 = newdata;

            if ( a_status == EVENT_CONTROL_CHANGE )
                d1 = newdata;

            if ( a_status == EVENT_PROGRAM_CHANGE )
                d0 = newdata; /* d0 == new patch */

            if ( a_status == EVENT_CHANNEL_PRESSURE )
                d0 = newdata; /* d0 == pressure */

            if ( a_status == EVENT_PITCH_WHEEL )
                d1 = newdata;

            (*i).set_data( d0, d1 );
        }	    
    }

    unlock();
}




void 
sequence::add_note( long a_tick, long a_length, int a_note, bool a_paint)
{
    add_note_velocity(a_tick,a_length,a_note,100,a_paint);
}

void
sequence::add_note_velocity(long a_tick,long a_length,int a_note,
                            int velocity,bool a_paint)
{

    lock();

    event e;
    
    if ( a_length < 1 )
        a_length = 1;
    long off_tick = a_tick + a_length;
    if ( off_tick >= m_length )
        off_tick = m_length - 1;

    if ( m_length > 1 &&
         a_tick >= 0 &&
         a_tick < m_length - 1 &&
         off_tick > a_tick &&
         a_note >= 0 &&
         a_note < c_num_keys ){

        /* if we care about the painted, run though 
         * our events, delete the painted ones that
         * overlap the one we want to add */
        if ( a_paint )
        {
            list<event>::iterator i,t;
            for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

                if ( (*i).is_painted() &&
                     (*i).is_note_on() &&
                     (*i).get_timestamp() == a_tick )
                {
                    (*i).mark();

                    if ( (*i).is_linked())
                    {
                        (*i).get_linked()->mark();
                    }

                    set_dirty();
                }
            }

            remove_marked();
        }

        /*  OVERLAP WITH AN EXISTING NOTE OF THE SAME PITCH.

            This is the one entry point that can be handed a note landing on top
            of one that is already in the pattern: LOOP OVERDUB records into a
            pattern that already has events, and nothing upstream compares the
            take against what is already there (the take is only de-overlapped
            against ITSELF -- see the same-pitch clamp in commit_recording).

            Two overlapping same-pitch notes are not a chord, they are one voice
            struck twice, and verify_and_link() pairs each on with the NEXT off
            of that pitch.  So on@100,on@150,off@160,off@200 links as 100->160
            and 150->200: BOTH recorded lengths are wrong, the first note now
            ends inside the second, and the pattern that gets saved is not the
            one that was played.

            The piano roll already refuses to create this (PianoRoll::insert_note
            calls note_overlaps first), so enforcing it here changes nothing for
            editing -- it only repairs the record path.  The newly recorded note
            wins the ticks it occupies; the older note is trimmed to end where it
            starts, and dropped outright if that leaves it nothing.  A note-off
            sorts BEFORE a note-on at the same tick (event::get_rank), so
            trimming to exactly a_tick relinks correctly. */
        {
            list<event>::iterator i;
            for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

                if ( ! (*i).is_note_on()          ) continue;
                if ( (int) (*i).get_note() != a_note ) continue;
                if ( ! (*i).is_linked()           ) continue;

                event *old_off = (*i).get_linked();
                if ( old_off == NULL ) continue;

                const long o_on  = (*i).get_timestamp();
                const long o_off = old_off->get_timestamp();

                /* a wrapped pair (off before on) is left alone: it is not an
                   overlap in the ordinary sense and trimming it would need a
                   rule this function has no way to pick */
                if ( o_off <= o_on ) continue;

                if ( o_off <= a_tick || o_on >= off_tick ) continue;   /* clear */

                if ( o_on < a_tick ){
                    old_off->set_timestamp( a_tick );        /* trim it back */
                }
                else {
                    (*i).mark();                             /* fully covered */
                    old_off->mark();
                }
            }
            remove_marked();
        }

        if ( a_paint )
            e.paint();

        e.set_status( EVENT_NOTE_ON );
        if (velocity < 1) velocity = 1;
        if (velocity > 127) velocity = 127;
        e.set_data( a_note, velocity );
        e.set_timestamp( a_tick );

        add_event( &e );

        e.set_status( EVENT_NOTE_OFF );
        e.set_data( a_note, 0 );
        e.set_timestamp( off_tick );

        add_event( &e );
    }

    /*  NO PRUNE.  Everything this function adds is already clamped inside
        [0, m_length), so pruning can only ever destroy something ELSE: the
        events a set_length() shrink deliberately HID (see set_length and the
        hidden-event branch in play_span) so that growing the pattern again
        brings them back.  Recording one note into a shortened pattern used to
        delete every note past the end, permanently. */
    verify_and_link( false );
    unlock();
}


void 
sequence::add_event( long a_tick, 
		     unsigned char a_status,
		     unsigned char a_d0,
		     unsigned char a_d1,
             bool a_paint)
{
    lock();

    if ( a_tick >= 0 ){

        event e;

        /* if we care about the painted, run though 
         * our events, delete the painted ones that
         * overlap the one we want to add */
        if ( a_paint )
        {
            list<event>::iterator i,t;
            for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

                if ( (*i).is_painted() &&
                     (*i).get_timestamp() == a_tick )
                {
                    (*i).mark();

                    if ( (*i).is_linked())
                    {
                        (*i).get_linked()->mark();
                    }

                    set_dirty();
                }
            }

            remove_marked();
        }                        
        

        if ( a_paint )
            e.paint();
 
        
        e.set_status( a_status );
        e.set_data( a_d0, a_d1 );
        e.set_timestamp( a_tick );

        add_event( &e );
    }
    verify_and_link();

    unlock();
}


void 
sequence::stream_event(  event *a_ev  )
{
    lock();

    /* m_length is the DIVISOR for the wrap below.  A pattern with zero length
       (freshly created, or loaded from a corrupt file) divided by zero on the
       first live note.  Same class of bug as the guard in play(): refuse rather
       than fault on the MIDI input thread. */
    if ( m_length <= 0 ){ unlock(); return; }

    /* LOOP RECORDING APPENDS INTO THIS PATTERN.  Wrapping the live tick into
       [0, m_length) is what makes a second pass over the loop land in the SAME
       clip at its own position instead of producing a clip per pass -- the
       overdub behaviour a drum machine has.

       Wrap a COPY, not the caller's event.  This used to mutate *a_ev in place
       BEFORE the thru echo below, so the note sent to the instrument carried the
       wrapped pattern-relative tick instead of its real one, and any caller that
       looked at its own event afterwards saw it silently rewritten. */
    event ev = *a_ev;

    /*  seq24 folded the live tick by m_length, because that was the only
        repetition a pattern had.  A clip that loops its own window replays
        [loop_start, loop_end) instead, so a note folded by the full length
        lands wherever the pattern happens to be long -- outside the window, on
        material that is never played.  The note was recorded and simply never
        heard again.  Put it where the clip IS: the same mapping the playhead
        and play_span use. */
    ev.set_timestamp( pattern_position( ev.get_timestamp() ) );

    /* The copy inherits the caller's editing flags, and the caller is OUTSIDE
       this class -- perform::input_func reuses ONE event object for every
       message that arrives.  A stale link flag is the dangerous one: link_new()
       and verify_and_link() both skip a note-on that already claims to be
       linked, so such an event would never be paired with its note-off and the
       note would have no length for the rest of the pattern's life.  A stale
       selected flag would silently enrol the recorded note in whatever the user
       has selected in the editor. */
    ev.clear_link();
    ev.unmark();
    ev.unselect();

    if ( m_recording ){

        add_event( &ev );
        set_dirty();
        /* Only the record path changes the list, so only it needs to re-link.
           link_new() rescans to pair note-ons with their offs; running it on
           every event even when nothing was added made a pure MIDI-thru pass
           O(n) per note for no reason. */
        link_new();
    }

    if ( m_thru )
    {
        put_event_on_bus( a_ev );
    }

    unlock();
} 


void
sequence::set_dirty_mp()
{
    //printf( "set_dirtymp\n" );
    m_dirty_names =  m_dirty_main =  m_dirty_perf = true; 
}


void
sequence::set_dirty()
{
    //printf( "set_dirty\n" );
    m_dirty_names = m_dirty_main =  m_dirty_perf = m_dirty_edit = true;
    m_edit_revision.fetch_add(1, std::memory_order_release);
}


bool
sequence::is_dirty_names( )
{
    lock();

    bool ret = m_dirty_names;
    m_dirty_names = false;
    
    unlock();

    return ret;
}

bool
sequence::is_dirty_main( )
{
    lock();

    bool ret = m_dirty_main;
    m_dirty_main = false;

    unlock();

    return ret;
}


bool
sequence::is_dirty_perf( )
{
    lock();

    bool ret = m_dirty_perf;
    m_dirty_perf = false;

    unlock();

    return ret;
}


bool
sequence::is_dirty_edit( )
{
    lock();

    bool ret = m_dirty_edit;
    m_dirty_edit = false;

    unlock();

    return ret;
}


/* plays a note from the paino roll */
void
sequence::play_note_on( int a_note )
{
    play_note_on( a_note, 127 );
}

/* audition a note at a specific velocity (tracker step-entry preview) */
void
sequence::play_note_on( int a_note, int a_velocity )
{
    if ( a_velocity < 1 )   a_velocity = 1;
    if ( a_velocity > 127 ) a_velocity = 127;
    lock();

    event e;

    e.set_status( EVENT_NOTE_ON );
    e.set_data( a_note, a_velocity );
    m_masterbus->play( m_bus, &e, m_midi_channel );

    m_masterbus->flush();

    unlock();
}


/* plays a note from the paino roll */
void 
sequence::play_note_off( int a_note )
{
    lock();

    event e;

    e.set_status( EVENT_NOTE_OFF );
    e.set_data( a_note, 127 );
    m_masterbus->play( m_bus, &e, m_midi_channel ); 

    m_masterbus->flush();

    unlock();
}


/*  THE TRIGGER DRAW MARKER AND ERASURE.

    m_iterator_draw_trigger is a member iterator that survives between
    get_next_trigger() calls, and the GUI leaves it parked mid-list every time a
    walk stops early (trigger_offset_at, region_for and clip_spans all `break`
    or `return` on the first hit).  Nothing below is allowed to erase or replace
    a node while that parked iterator still points at it, so every mutator that
    can do so re-parks it at begin() -- exactly what remove_marked() has always
    done for the event list.  Without this the marker is a dangling pointer
    until the next reset, and the only thing keeping it from being dereferenced
    is that every caller today happens to reset first.

    m_list_event's equivalent is handled inside remove_marked/add_event.  */
void
sequence::clear_triggers( void )
{
    lock();
    m_list_trigger.clear();
    m_iterator_draw_trigger = m_list_trigger.begin();
    unlock();
}



/* adds trigger, a_state = true, range is on.
   a_state = false, range is off


   is      ie     
   <      ><        ><        >
   es             ee
   <               >
   XX
            
   es ee
   <   >
   <>
               
   es    ee
   <      >
   <    >
                
   es     ee
   <       >
   <    >            
*/ 
void 
sequence::add_trigger( long a_tick, long a_length, long a_offset, bool a_adjust_offset )
{
    lock();

    trigger e;
    
    if ( a_adjust_offset )
        e.m_offset = adjust_offset(a_offset);
    else
        e.m_offset = a_offset;
    
    e.m_selected = false;

    e.m_tick_start  = a_tick;
    e.m_tick_end    = a_tick + a_length - 1;

    list<trigger>::iterator i = m_list_trigger.begin();

    while ( i != m_list_trigger.end() ){

        // Is it inside the new one ? erase
        if ((*i).m_tick_start >= e.m_tick_start &&
            (*i).m_tick_end   <= e.m_tick_end  )
        {
            //printf ( "erase start[%d] end[%d]\n", (*i).m_tick_start, (*i).m_tick_end );
            m_list_trigger.erase(i);
            i = m_list_trigger.begin();
            continue;
        }
        // Is the e's end inside  ?
        else if ( (*i).m_tick_end   >= e.m_tick_end &&
                  (*i).m_tick_start <= e.m_tick_end )
        {
            (*i).m_tick_start = e.m_tick_end + 1;
            //printf ( "mvstart start[%d] end[%d]\n", (*i).m_tick_start, (*i).m_tick_end );
        }
        // Is the last start inside the new end ?
        else if ((*i).m_tick_end   >= e.m_tick_start &&
                 (*i).m_tick_start <= e.m_tick_start )
        {
            (*i).m_tick_end = e.m_tick_start - 1;
            //printf ( "mvend start[%d] end[%d]\n", (*i).m_tick_start, (*i).m_tick_end );
        }

        ++i;
    }

    /*  Trimming above can INVERT a trigger.  The "e's end is inside" branch
        moves an existing start to e.m_tick_end + 1, and when that trigger ended
        exactly where the new one does the result is start == end + 1: a trigger
        occupying no ticks at all.  It is unreachable in playback, draws as a
        zero-width clip, and every later edit has to step over it.  Drop
        anything that no longer spans a tick.  */
    {
        list<trigger>::iterator k = m_list_trigger.begin();
        while ( k != m_list_trigger.end() ){
            if ( (*k).m_tick_start > (*k).m_tick_end )
                k = m_list_trigger.erase( k );
            else
                ++k;
        }
    }

    /*  A zero/negative-length request would push exactly such a trigger.  */
    if ( e.m_tick_end < e.m_tick_start ){
        m_iterator_draw_trigger = m_list_trigger.begin();   /* erases above */
        unlock();
        return;
    }

    m_list_trigger.push_front( e );
    m_list_trigger.sort();

    m_iterator_draw_trigger = m_list_trigger.begin();

    unlock();
}

void
sequence::grow_trigger (long a_tick_from, long a_tick_to, long a_length)
{
    lock();

    list<trigger>::iterator i = m_list_trigger.begin();
    
    while ( i != m_list_trigger.end() ){
        
        // Find our pair
        if ((*i).m_tick_start <= a_tick_from &&
            (*i).m_tick_end   >= a_tick_from  )
        {
            long start = (*i).m_tick_start;
            long end   = (*i).m_tick_end;
            
            if ( a_tick_to < start )
            {
                start = a_tick_to;
            }
            
            /*  seq24 grew a trigger in whole PATTERNS: callers pass
                get_length() as a_length and the clip's end was snapped to
                a_tick_to + m_length - 1, so dragging a clip out always added a
                full pattern past the mouse.

                With looping ON the unit of repetition is the clip's own loop
                window, so grow by that instead -- dragging longer repeats the
                selected bars.  (No window set => period == m_length ==
                a_length, i.e. bit-identical to before.)

                With looping OFF the clip is a ONE-SHOT: dragging it longer
                just moves its END POINT, so the end follows the drag tick and
                nothing is quantised to the pattern at all.  */
            long unit = a_length;

            if ( m_loop_enabled ){
                const long period = repeat_period();
                if ( period < unit )
                    unit = period;
            }
            else {
                unit = 1;
            }

            if ( (a_tick_to + unit - 1) > end )
            {
                end = (a_tick_to + unit - 1);
            }
            
            /*  NO OFFSET ADJUST.  This offset is the one the clip ALREADY has,
                so there is nothing to canonicalise -- and re-folding it is
                actively wrong for a one-shot, whose offset is a trim-in point
                into its data and is deliberately allowed to sit PAST the end
                marker (split_trigger puts it there for the half of a one-shot
                clip that falls beyond the pattern's data: silent by design).
                adjust_offset()'s `% m_length` wrapped that back to the top, so
                merely dragging such a clip's right edge made it start playing
                the pattern again from the beginning. */
            add_trigger( start, end - start + 1, (*i).m_offset, false );
            break;
        }
        ++i;
    }
    
    unlock();
}


void 
sequence::del_trigger( long a_tick )
{
    lock();

    list<trigger>::iterator i = m_list_trigger.begin();

    while ( i != m_list_trigger.end() ){
        if ((*i).m_tick_start <= a_tick &&
            (*i).m_tick_end   >= a_tick ){

            m_list_trigger.erase(i);
            m_iterator_draw_trigger = m_list_trigger.begin();
            break;
        }
        ++i;
    }

    unlock();
}

void
sequence::set_trigger_offset( long a_trigger_offset )
{
    lock();


    if ( m_length <= 0 ){          /* would divide by zero */
        m_trigger_offset = 0;
        unlock();
        return;
    }

    /*  Was folded by m_length (seq24: one pattern == one repetition).  With a
        loop window shorter than the pattern the phase lives modulo the WINDOW,
        and reducing by m_length first destroys it whenever the window's period
        does not divide m_length: (x % len) % period != x % period.  play_span
        then folded the already-mangled value again.  Fold once, by the period
        that is actually repeating.  */
    m_trigger_offset = adjust_offset( a_trigger_offset );
    
    unlock();
}


long
sequence::get_trigger_offset( void )
{
    return m_trigger_offset;
}

void
sequence::split_trigger( trigger &trig, long a_split_tick)
{
    lock();

    long new_tick_end   = trig.m_tick_end;
    long new_tick_start = a_split_tick;

    /*  THE RIGHT HALF MUST CARRY ON WHERE THE LEFT ONE STOPPED.

        seq24 handed the right half the LEFT half's offset, which was harmless
        there only because a trigger's offset was a phase into a grid laid out
        from song tick 0 -- the cut point's own position supplied the rest.  The
        grid is anchored on each clip's own start tick now (see play_span), so
        copying the offset restarts the content AT THE CUT: a split one-shot
        replayed its pattern from the top, and a split looping clip whose window
        does not divide the cut distance came back in on the wrong step.

        The content position at the cut is offset + (cut - start), which is what
        the right half must start from.  Folded by the repetition only when the
        clip LOOPS: a one-shot's offset is a trim-in point into its data, so a
        cut past the end of that data must stay past it and go silent rather
        than wrap around to the beginning. */
    const long cut_offset = trig.m_offset + ( a_split_tick - trig.m_tick_start );
    const long right_offset = m_loop_enabled ? adjust_offset( cut_offset )
                                             : cut_offset;

    trig.m_tick_end = a_split_tick - 1;

    /*  The right half spans [split, end] inclusive, i.e. `length + 1` ticks.
        The guard used to be `length > 1`, which silently threw the right half
        away whenever it came out one or two ticks long -- so splitting close to
        a clip's end deleted its tail instead of splitting it.  */
    long length = new_tick_end - new_tick_start;
    if ( length >= 0 )
        add_trigger( new_tick_start, length + 1, right_offset, false );

    unlock();
}

#if 0
/*
  |...|...|...|...|...|...|...

  0123456789abcdef0123456789abcdef
  [      ][      ][      ][      ][      ][

  [  ][      ][  ][][][][][      ]  [  ][  ]
  0   4       4   0 7 4 2 0         6   2  
  0   4       4   0 1 4 6 0         2   6 inverse offset

  [              ][              ][              ]
  [  ][      ][  ][][][][][      ]  [  ][  ]
  0   c       4   0 f c a 8         e   a
  0   4       c   0 1 4 6 8         2   6  inverse offset

  [                              ][
  [  ][      ][  ][][][][][      ]  [  ][  ]
  k   g f c a 8
  0   4       c   g h k m n       inverse offset

  0123456789abcdefghijklmonpq
  ponmlkjihgfedcba9876543210
  0fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210

*/

#endif


void
sequence::adjust_trigger_offsets_to_legnth( long a_new_len )
{
    lock();

    /*  Keep every clip playing the SAME pattern content at its own start tick
        after the pattern's length changed.

        The event a clip emits at song tick G sits at pattern position
        (G - offset) mod period (see play_span), so preserving the content at
        the clip's start means preserving (start - offset) mod period.

        seq24 did this with a chain of flips through m_length and C's `%`,
        which truncates toward zero: `local_offset %= m_length` and
        `new_offset % a_new_len` both go NEGATIVE for perfectly ordinary
        inputs, and the final `a_new_len - x` then produced an offset EQUAL TO
        or LARGER THAN the new length -- an out-of-range phase that was also
        the wrong one (offset 9216 on a 12288-tick pattern shrunk to 6144 came
        out 9216 again instead of 3072), and that value got written to the
        project file.  Do the arithmetic once, with a floor-mod.

        Two clips are left alone entirely:
          - a ONE-SHOT's offset is a content trim-in, not a phase; re-phasing
            it silently re-trims the clip on any length edit.
          - a clip with its OWN loop window repeats at the window's period,
            which a pattern-length change does not alter, so its phase is
            already correct.  */
    if ( a_new_len <= 0 || m_length <= 0 || !m_loop_enabled ){
        unlock();
        return;
    }

    const bool loop_set = loop_window_set();
    if ( loop_set ){
        unlock();
        return;
    }

    /* m_length is still the OLD length here: set_length() calls us before it
       assigns the new one. */
    const long old_len = m_length;

    list<trigger>::iterator i = m_list_trigger.begin();
    
    while ( i != m_list_trigger.end() ){

        long content = ( i->m_tick_start - i->m_offset ) % old_len;
        if ( content < 0 ) content += old_len;

        /* the pattern shrank past the position this clip was playing */
        if ( content >= a_new_len ) content %= a_new_len;

        long off = ( i->m_tick_start - content ) % a_new_len;
        if ( off < 0 ) off += a_new_len;

        i->m_offset = off;
        
        ++i;
    }
    
    unlock();
    
}

#if 0    

... a
[      ][      ]
...
... a
...

         
    
5   7    play
3        offset
8   10   play

    

X...X...X...X...X...X...X...X...X...X...
L       R    
[        ] [     ]  []  orig
[                    ]
        
<<
    [     ]    [  ][ ]  [] split on the R marker, shift first
    [     ]        [     ]   
    delete middle
    [     ][ ]  []         move ticks
    [     ][     ]

    L       R        
    [     ][ ] [     ]  [] split on L
    [     ][             ]
        
    [     ]        [ ] [     ]  [] increase all after L
    [     ]        [             ]

        
#endif

    void
    sequence::copy_triggers( long a_start_tick, 
                             long a_distance  )
{

    long from_start_tick = a_start_tick + a_distance;
    long from_end_tick = from_start_tick + a_distance - 1;

    lock();

    move_triggers( a_start_tick, 
		   a_distance, 
		   true );
    
    list<trigger>::iterator i = m_list_trigger.begin();
    while(  i != m_list_trigger.end() ){


        
	if ( (*i).m_tick_start >= from_start_tick &&
             (*i).m_tick_start <= from_end_tick )
        {
            trigger e;
            e.m_offset = (*i).m_offset;
            e.m_selected = false;

            e.m_tick_start  = (*i).m_tick_start - a_distance;
        
            if ((*i).m_tick_end   <= from_end_tick )
            {
                e.m_tick_end  = (*i).m_tick_end - a_distance;
            }

            if ((*i).m_tick_end   > from_end_tick )
            {
                e.m_tick_end = from_start_tick -1;
            }

            /*  MOVING A CLIP DOES NOT CHANGE WHAT IT PLAYS.  m_offset is the
                content position at the clip's own start, and play_span now
                anchors the repetition grid on that start rather than on song
                tick 0 -- so the phase already travels with the clip.  seq24
                re-phased here because its grid WAS global; doing it on top of
                an anchored grid double-counts the move and slides the content
                by the drag distance every time the clip is nudged.  */

            

            m_list_trigger.push_front( e );
        }

        ++i;
    }

    m_list_trigger.sort();

    m_iterator_draw_trigger = m_list_trigger.begin();

    unlock();

}


void
sequence::split_trigger( long a_tick )
{
    lock();
    for (list<trigger>::iterator i=m_list_trigger.begin();i!=m_list_trigger.end();++i) {
        // Cut at the requested edit point. The old implementation ignored
        // a_tick and always split the containing trigger at its midpoint.
        if(i->m_tick_start<a_tick&&a_tick<=i->m_tick_end) {
            trigger right=*i;
            right.m_tick_start=a_tick;
            /*  The right half continues the content, it does not restart it --
                see the long note in split_trigger(trigger&,long).  Copying the
                left half's offset (which is all `right=*i` does) made the tail
                of a split clip replay the pattern from the top. */
            {
                const long cut_offset = i->m_offset + ( a_tick - i->m_tick_start );
                right.m_offset = m_loop_enabled ? adjust_offset( cut_offset )
                                                : cut_offset;
            }
            right.m_selected=true;
            i->m_tick_end=a_tick-1;
            i->m_selected=false;
            m_list_trigger.push_back(right);
            m_list_trigger.sort();
            m_iterator_draw_trigger = m_list_trigger.begin();
            break;
        }
    }
    unlock();
}

void 
sequence::move_triggers( long a_start_tick, 
			 long a_distance, 
			 bool a_direction )
{

    long a_end_tick = a_start_tick + a_distance;
    //printf( "move_triggers() a_start_tick[%d] a_distance[%d] a_direction[%d]\n",
    //        a_start_tick, a_distance, a_direction );
    
    lock();
    
    list<trigger>::iterator i = m_list_trigger.begin();
    while(  i != m_list_trigger.end() ){

        /*  A TRIGGER THAT STRADDLES L.

            seq24 tested this TWICE, back to back, under two different comments
            ("trigger greater than L and R", then "triggers on L") with the
            IDENTICAL condition -- the first was meant to catch a trigger
            spanning BOTH markers and never did.  Going forward the two did the
            same split, so the second was a no-op; going back the first split at
            R and the second then trimmed the left piece to L-1, which is what
            actually implemented "delete the time between L and R".

            Merged into one branch that says what it does.  The behaviour is the
            same except in the degenerate case seq24 got wrong: with a distance
            of ONE tick, split_trigger() left the left piece ending at R-1 == L,
            which failed the second test (`end > L`) so the trim never ran -- the
            left piece kept a tick that had just been deleted, and the shifted
            right piece then OVERLAPPED it.  The trim is unconditional now.

            Splitting is likewise conditional: seq24 split at R even when the
            trigger ended before R, where split_trigger() only EXTENDED it to
            R-1 (a right half of negative length is dropped) and the trim
            happened to undo the damage.  Split only when there really is
            something at or past R to keep. */
        if ( (*i).m_tick_start < a_start_tick &&
             (*i).m_tick_end   > a_start_tick )
        {
            if ( a_direction )                    /* forward: insert time at L */
            {
                split_trigger( *i, a_start_tick );
            }
            else                                  /* back: delete [L,R) */
            {
                /* whatever lies at or past R survives the deletion; it is
                   shifted back by the second pass below */
                if ( (*i).m_tick_end >= a_end_tick )
                    split_trigger( *i, a_end_tick );

                (*i).m_tick_end = a_start_tick - 1;
            }
        }

        // In betweens
        if ( (*i).m_tick_start >= a_start_tick &&
             (*i).m_tick_end <= a_end_tick &&
             !a_direction )
        {
            /*  ERASE THEN RESTART.  This used to erase, reset i to begin(), and
                then FALL THROUGH into the "triggers on R" test below -- which
                dereferences i.  When that erase emptied the list (drag a
                selection back over every trigger of a pattern), begin() ==
                end() and the very next line read, and could WRITE
                (m_tick_start = a_end_tick), through the list's sentinel node.
                It also ran ++i at the bottom, so the element that had just been
                shuffled into first place was skipped and never trimmed. */
            i = m_list_trigger.erase(i);
            continue;
        }

        // triggers on R
	if ( (*i).m_tick_start < a_end_tick &&
             (*i).m_tick_end > a_end_tick )
        {
            if ( !a_direction ) // forward
            {
                (*i).m_tick_start = a_end_tick;
            }
        }

        ++i;
    }


    i = m_list_trigger.begin();
    while(  i != m_list_trigger.end() ){

        /*  seq24 re-phased EVERY trigger it moved by folding the distance
            through m_length.  Two things were wrong for this model:

              - a ONE-SHOT clip has no phase.  Its offset is where its data is
                trimmed in, so adding the move distance to it re-trimmed the
                clip: drag a one-shot right by a bar and it played a bar later
                INTO its data -- usually silence.  Moving must move it, and
                nothing else.

              - a looping clip's phase lives modulo its own loop window, so
                folding by m_length shifted the phase of any window whose
                period does not divide the pattern (the polyrhythmic case).
                adjust_offset() now folds by the repetition.  */
        const long delta = a_direction ? a_distance : -a_distance;
        const bool moved = a_direction ? ( (*i).m_tick_start >= a_start_tick )
                                       : ( (*i).m_tick_start >= a_end_tick );

        if ( moved ){

            (*i).m_tick_start += delta;
            (*i).m_tick_end   += delta;
            /* offset deliberately untouched -- see the note in copy_triggers */
        }

        ++i;
    }

    m_iterator_draw_trigger = m_list_trigger.begin();   /* nodes were erased */

    unlock();

}

long
sequence::get_selected_trigger_start_tick( void )
{
    long ret = -1;
    lock();

    list<trigger>::iterator i = m_list_trigger.begin();
    
    while(  i != m_list_trigger.end() ){
        
	if ( i->m_selected ){

            ret = i->m_tick_start;
        }

        ++i;
    }
    
    unlock();

    return ret;
}

long
sequence::get_selected_trigger_end_tick( void )
{
    long ret = -1;
    lock();

    list<trigger>::iterator i = m_list_trigger.begin();
    
    while(  i != m_list_trigger.end() ){
        
	if ( i->m_selected ){

            ret = i->m_tick_end;
        }

        ++i;
    }
    
    unlock();

    return ret;
}


/*  MOVE / TRIM THE WHOLE SELECTION.

    seq24's song editor could only ever have one trigger selected, so this
    found the first selected clip, moved it, and `break`ed.  The arrange view
    rubber-bands a group of clips (and Ctrl+A selects the lot), so dragging a
    multi-clip selection moved exactly one of them -- and then clamped that one
    flush against the neighbour it was supposed to be travelling WITH, because
    the neighbour counted as an obstacle.  Same single-selection assumption
    del_selected_trigger() and copy_selected_trigger() were already fixed for.

    The group moves RIGIDLY: one delta, measured from the group's outer edge
    (its earliest start, or its latest end when the right edge is being
    dragged), applied to every selected clip.  With one clip selected that is
    exactly what it always did.

    Clamping is per clip and intersected, so the group stops as soon as ANY of
    its members would hit something: a clip is bounded by the neighbour on that
    side only when that neighbour is NOT itself moving, and a trim may never
    pull a clip inside out. */
void
sequence::move_selected_triggers_to( long a_tick, bool a_adjust_offset, int a_which )
{

    lock();

    const long min_clip = c_ppqn / 8;    /* smallest clip a trim may leave */

    /* the group's outer edges: what the drag's delta is measured from */
    long group_start = 0, group_end = 0;
    bool any = false;

    for ( list<trigger>::iterator i  = m_list_trigger.begin();
                                  i != m_list_trigger.end(); ++i ){

        if ( ! i->m_selected )
            continue;

        if ( ! any ){
            group_start = i->m_tick_start;
            group_end   = i->m_tick_end;
            any = true;
        }
        else {
            if ( i->m_tick_start < group_start ) group_start = i->m_tick_start;
            if ( i->m_tick_end   > group_end   ) group_end   = i->m_tick_end;
        }
    }

    if ( ! any ){
        unlock();
        return;
    }

    // min_tick][0                1][max_tick
    //                   2

    // if we are moving the 0, use first as offset
    // if we are moving the 1, use the last as the offset
    // if we are moving both (2), use first as offset

    long a_delta_tick = ( a_which == 1 ) ? a_tick - group_end
                                         : a_tick - group_start;

    /*  Intersect every selected clip's allowed delta.  A neighbour that is
        ALSO selected moves by the same delta, so it is never an obstacle --
        that is the part the old code got wrong even for the one clip it did
        move. */
    long lo = a_delta_tick, hi = a_delta_tick;
    bool bounded = false;

    for ( list<trigger>::iterator i  = m_list_trigger.begin();
                                  i != m_list_trigger.end(); ++i ){

        if ( ! i->m_selected )
            continue;

        list<trigger>::iterator prev = i;
        list<trigger>::iterator next = i;
        ++next;

        const bool has_prev = ( i != m_list_trigger.begin() );
        if ( has_prev ) --prev;
        const bool has_next = ( next != m_list_trigger.end() );

        long c_lo = -0x7fffffffL, c_hi = 0x7fffffffL;

        if ( a_which == 0 || a_which == 2 ){
            /* the start edge moves: not past song zero, and not into the
               clip in front unless that one is moving too */
            c_lo = -i->m_tick_start;
            if ( has_prev && ! prev->m_selected )
                c_lo = std::max( c_lo, prev->m_tick_end + 1 - i->m_tick_start );
        }

        if ( a_which == 1 || a_which == 2 ){
            /* the end edge moves: not into the clip behind unless that one
               is moving too */
            if ( has_next && ! next->m_selected )
                c_hi = std::min( c_hi, next->m_tick_start - 1 - i->m_tick_end );
        }

        if ( a_which == 0 )                     /* left trim: not past the end */
            c_hi = std::min( c_hi, ( i->m_tick_end - min_clip ) - i->m_tick_start );

        if ( a_which == 1 )                     /* right trim: not past the start */
            c_lo = std::max( c_lo, ( i->m_tick_start + min_clip ) - i->m_tick_end );

        if ( ! bounded ){ lo = c_lo; hi = c_hi; bounded = true; }
        else            { lo = std::max( lo, c_lo ); hi = std::min( hi, c_hi ); }
    }

    if ( bounded ){
        if ( a_delta_tick < lo ) a_delta_tick = lo;
        if ( a_delta_tick > hi ) a_delta_tick = hi;
        if ( lo > hi )           a_delta_tick = 0;   /* nowhere legal to go */
    }

    for ( list<trigger>::iterator i  = m_list_trigger.begin();
                                  i != m_list_trigger.end(); ++i ){

        if ( ! i->m_selected )
            continue;

        if ( a_which == 0 || a_which == 2 )
            i->m_tick_start += a_delta_tick;

        if ( a_which == 1 || a_which == 2 )
            i->m_tick_end   += a_delta_tick;

        /*  The arrange view's clip DRAG.  Offset left alone for the same
            reason as move_triggers(): the grid is anchored on the clip, so
            its content travels with it without help.  */
        (void) a_adjust_offset;
    }

    unlock();
}


long
sequence::get_max_trigger( void )
{
    lock();

    long ret;

    if ( m_list_trigger.size() > 0 )
	ret = m_list_trigger.back().m_tick_end;
    else
	ret = 0;

    unlock();

    return ret;
}

long
sequence::repeat_period( void )
{
    /* same test play_span() uses, so stored phase and played phase agree */
    const bool loop_set = loop_window_set();
    long p = loop_set ? ( m_loop_end - m_loop_start ) : m_length;

    if ( p <= 0 )
        p = 1;                    /* never hand a zero modulus to % */

    return p;
}


long
sequence::adjust_offset( long a_offset )
{    
    /*  seq24 folded every trigger offset by m_length because a pattern there
        ALWAYS repeated at its full length.  A clip that loops its own window
        repeats at the WINDOW's period, and folding by m_length is only
        phase-preserving when that period divides m_length -- which is exactly
        what a polyrhythmic window (a 3-step loop in a 4-step pattern) does not
        do.  Fold by the repetition the clip actually plays.

        A ONE-SHOT HAS NO REPETITION, so there is no phase to fold it into:
        its offset is a content trim-IN into the data, and a value at or past
        the END MARKER is LEGITIMATE and means "this clip's content has run
        out" -- silence.  split_trigger() and the arrange view's split both
        deliberately leave a one-shot's offset unfolded for exactly that
        reason, and grow_trigger() refuses to re-canonicalise it.  This
        function was the one site that still folded it, and because play_span()
        re-derives the offset through set_trigger_offset() on EVERY scheduling
        window, the fold was applied at playback time no matter who had been
        careful earlier: cut a one-shot that had been dragged out past its data
        and the tail replayed the pattern from the top, sounding notes the
        clip's earlier half had already played.  Clamp at zero and leave the
        top alone -- play_span's one-shot branch already emits nothing once the
        offset passes m_length, which is the intended silence.  */
    if ( ! m_loop_enabled )
        return ( a_offset < 0 ) ? 0 : a_offset;

    const long mod = repeat_period();

    a_offset %= mod;
    
    if ( a_offset < 0 )
        a_offset += mod;

    return a_offset;
}

bool 
sequence::get_trigger_state( long a_tick )
{
    lock();

    bool ret = false;
    list<trigger>::iterator i;

    for ( i = m_list_trigger.begin(); i != m_list_trigger.end(); i++ ){

	if ( (*i).m_tick_start <= a_tick &&
             (*i).m_tick_end >= a_tick){
	    ret = true;
            break;
        }
    }

    unlock();

    return ret;
}



bool 
sequence::select_trigger( long a_tick )
{
    lock();

    bool ret = false;
    list<trigger>::iterator i;
    
    for ( i = m_list_trigger.begin(); i != m_list_trigger.end(); i++ ){

	if ( (*i).m_tick_start <= a_tick &&
             (*i).m_tick_end   >= a_tick){
            
            (*i).m_selected = true;
	    ret = true;
        }        
    }
    
    unlock();
    
    return ret;
}


bool 
sequence::unselect_triggers( void )
{
    lock();

    bool ret = false;
    list<trigger>::iterator i;

    for ( i = m_list_trigger.begin(); i != m_list_trigger.end(); i++ ){
        (*i).m_selected = false;
    }

    unlock();

    return ret;
}



/*  DELETE THE SELECTION, not "the first thing in it".

    seq24's song editor could only ever have one trigger selected, so this
    stopped at the first `break`.  The arrange view rubber-bands a whole range
    of clips, and deleting them one call at a time is not merely slow -- it is
    wrong under undo, which snapshots the trigger list per call, and the view
    had to paper over it with a loop that re-asked "is anything still
    selected?" after every call (arrange_view.cpp).  Erase them all in one
    pass, in one undo step. */
void
sequence::del_selected_trigger( void )
{
    lock();

    list<trigger>::iterator i = m_list_trigger.begin();

    while ( i != m_list_trigger.end() ){

        if ( i->m_selected )
            i = m_list_trigger.erase( i );
        else
            ++i;
    }

    m_iterator_draw_trigger = m_list_trigger.begin();   /* nodes were erased */

    unlock();
}


void
sequence::cut_selected_trigger( void )
{
    copy_selected_trigger();
    del_selected_trigger();
}


/*  COPY THE SELECTION.  Same seq24 single-selection assumption as
    del_selected_trigger(): everything after the first selected clip was
    silently dropped, so copying a rubber-banded group and pasting it gave back
    one clip. */
void
sequence::copy_selected_trigger( void )
{
    lock();

    m_trigger_clipboard.clear();

    for ( list<trigger>::iterator i  = m_list_trigger.begin();
                                  i != m_list_trigger.end(); i++ ){

        if ( i->m_selected )
            m_trigger_clipboard.push_back( *i );
    }

    m_trigger_copied = ! m_trigger_clipboard.empty();

    unlock();
}


void
sequence::paste_trigger( void )
{
    lock();     /* m_trigger_clipboard is shared with the copy path */

    if ( m_trigger_copied && ! m_trigger_clipboard.empty() ){

        /*  The whole COPIED GROUP is pasted immediately after itself, keeping
            the spacing (and the gaps) between its clips: the group's span is
            the distance from its first start to its last end, and every clip
            moves by that.  A single-clip selection reduces to what this always
            did -- paste at the copy's end + 1.  */
        long group_start = m_trigger_clipboard.front().m_tick_start;
        long group_end   = m_trigger_clipboard.front().m_tick_end;

        for ( list<trigger>::iterator c  = m_trigger_clipboard.begin();
                                      c != m_trigger_clipboard.end(); c++ ){
            if ( c->m_tick_start < group_start ) group_start = c->m_tick_start;
            if ( c->m_tick_end   > group_end   ) group_end   = c->m_tick_end;
        }

        const long delta = group_end + 1 - group_start;

        for ( list<trigger>::iterator c  = m_trigger_clipboard.begin();
                                      c != m_trigger_clipboard.end(); c++ ){

            const long length = c->m_tick_end - c->m_tick_start + 1;

            /*  seq24 advanced the pasted clip's offset by the clip's length, so
                the copy continues the phase of a pattern that repeats forever.
                A ONE-SHOT does not repeat: its offset is the trim-in point of
                its data, so advancing it made the pasted clip play LATER
                content than the clip that was copied (a one-bar one-shot pasted
                from a four-bar pattern played bar 2 -- or nothing at all).
                Paste the same data.  Same rule for BOTH modes now: the paste
                starts its own repetition grid at its own start tick, so it
                plays exactly what was copied. */
            add_trigger( c->m_tick_start + delta, length, c->m_offset, false );

            /* chain: pasting again lands after THIS paste */
            c->m_tick_start += delta;
            c->m_tick_end   += delta;
        }
    }

    unlock();
}


/* this refreshes the play marker to the LastTick */
void 
sequence::reset_draw_marker( void )
{
    lock();
    
    m_iterator_draw = m_list_event.begin();
    
    unlock();
}

void
sequence::reset_draw_trigger_marker( void )
{
    lock();

    m_iterator_draw_trigger = m_list_trigger.begin();

    unlock();
}


int
sequence::get_lowest_note_event( void )
{
    lock();

    int ret = 127;
    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	if ( (*i).is_note_on() || (*i).is_note_off() )
	    if ( (*i).get_note() < ret )
		ret = (*i).get_note();
    }

    unlock();

    return ret;
}



int
sequence::get_highest_note_event( void )
{
    lock();

    int ret = 0;
    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	if ( (*i).is_note_on() || (*i).is_note_off() )
	    if ( (*i).get_note() > ret )
		ret = (*i).get_note();
    }

    unlock();

    return ret;
}

/*  THE THREE get_next_* WALKS RUN ON THE GUI THREAD AND WERE UNLOCKED.

    Everything else that touches m_list_event / m_list_trigger takes m_mutex;
    these read them raw, while sequence::stream_event() (MIDI input thread) can
    be inserting into the SAME list and re-sorting it.  That is a torn read at
    best and a walk off a relinked node at worst.  The mutex is recursive and
    these are leaves, so taking it is free of deadlock risk.

    Honest about what this does NOT buy: the marker iterator lives BETWEEN
    calls, so locking each call makes the call atomic, not the walk.  The
    guarantee that the marker still points at a live node comes from the
    mutators re-parking it (see remove_marked and clear_triggers).  */
draw_type
sequence::get_next_note_event( long *a_tick_s,
			       long *a_tick_f,
			       int  *a_note,
			       bool *a_selected,
			       int  *a_velocity  )
{

    draw_type ret = DRAW_FIN;
    *a_tick_f = 0;

    lock();

    while (  m_iterator_draw  != m_list_event.end() )
    {
	*a_tick_s   = (*m_iterator_draw).get_timestamp();
	*a_note     = (*m_iterator_draw).get_note();
	*a_selected = (*m_iterator_draw).is_selected();
	*a_velocity = (*m_iterator_draw).get_note_velocity();

	/* note on, so its linked */
	if( (*m_iterator_draw).is_note_on() &&
	    (*m_iterator_draw).is_linked() ){

	    *a_tick_f   = (*m_iterator_draw).get_linked()->get_timestamp();

	    ret = DRAW_NORMAL_LINKED;
	    m_iterator_draw++;
	    unlock();
	    return ret;
	}

	else if( (*m_iterator_draw).is_note_on() &&
		 (! (*m_iterator_draw).is_linked()) ){

	    ret = DRAW_NOTE_ON;
	    m_iterator_draw++;
	    unlock();
	    return ret;
	}

	else if( (*m_iterator_draw).is_note_off() &&
		 (! (*m_iterator_draw).is_linked()) ){

	    ret = DRAW_NOTE_OFF;
	    m_iterator_draw++;
	    unlock();
	    return ret;
	}

	/* keep going until we hit null or find a NoteOn */
	m_iterator_draw++;
    }

    unlock();
    return DRAW_FIN;
}


bool
sequence::get_next_event( unsigned char *a_status,
                          unsigned char *a_cc)
{
    unsigned char j;

    lock();

    while (  m_iterator_draw  != m_list_event.end() ){

        *a_status = (*m_iterator_draw).get_status();
        (*m_iterator_draw).get_data( a_cc, &j );

        /* we have a good one */
        /* update and return */
        m_iterator_draw++;
        unlock();
        return true;
    }

    unlock();
    return false;
}


bool 
sequence::get_next_event( unsigned char a_status,
			  unsigned char a_cc,
			  long *a_tick,
			  unsigned char *a_D0,
			  unsigned char *a_D1,
			  bool *a_selected )
{
    lock();

    while (  m_iterator_draw  != m_list_event.end() ){
	
	/* note on, so its linked */
	if( (*m_iterator_draw).get_status() == a_status ){
	    
	    (*m_iterator_draw).get_data( a_D0, a_D1 );
	    *a_tick   = (*m_iterator_draw).get_timestamp();
	    *a_selected = (*m_iterator_draw).is_selected();
	    
	    /* either we have a control chage with the right CC
	       or its a different type of event */
	    if ( (a_status == EVENT_CONTROL_CHANGE &&
		  *a_D0 == a_cc )
		 || (a_status != EVENT_CONTROL_CHANGE) ){

		/* we have a good one */
		/* update and return */
		m_iterator_draw++;
		unlock();
		return true;
	    }
	}
	/* keep going until we hit null or find a NoteOn */
	m_iterator_draw++;
    }

    unlock();
    return false;
}

bool 
sequence::get_next_trigger( long *a_tick_on, long *a_tick_off, bool *a_selected, long *a_offset )
{
    lock();

    while (  m_iterator_draw_trigger  != m_list_trigger.end() ){

	*a_tick_on  = (*m_iterator_draw_trigger).m_tick_start;
        *a_selected = (*m_iterator_draw_trigger).m_selected;
        *a_offset =   (*m_iterator_draw_trigger).m_offset;
	*a_tick_off = (*m_iterator_draw_trigger).m_tick_end;
	m_iterator_draw_trigger++;

	unlock();
	return true;
    }

    unlock();
    return false;
}


void 
sequence::remove_all( void )
{
    lock();

    m_list_event.clear();

    /* the draw/play markers pointed into what was just freed */
    m_iterator_play = m_list_event.begin();
    m_iterator_draw = m_list_event.begin();

    unlock();

}

void sequence::clear_events(void)
{
    lock();
    m_list_event.clear();
    m_iterator_play=m_list_event.begin();
    m_iterator_draw=m_list_event.begin();
    while(!m_list_undo.empty())m_list_undo.pop();
    unlock();
}

void sequence::discard_edit_history(void)
{
    lock();
    while(!m_list_undo.empty())m_list_undo.pop();
    while(!m_list_trigger_undo.empty())m_list_trigger_undo.pop();
    while(!m_list_redo.empty())m_list_redo.pop();
    while(!m_list_trigger_redo.empty())m_list_trigger_redo.pop();
    m_iterator_play=m_list_event.begin();
    m_iterator_draw=m_list_event.begin();
    m_iterator_draw_trigger=m_list_trigger.begin();
    unlock();
}

sequence& 
sequence::operator= (const sequence& a_rhs)
{
    lock();

    /* dont copy to self */
    if (this != &a_rhs){
	
	m_list_event   = a_rhs.m_list_event;
	m_list_trigger   = a_rhs.m_list_trigger;

	m_midi_channel = a_rhs.m_midi_channel;
	m_masterbus    = a_rhs.m_masterbus;
	m_bus          = a_rhs.m_bus;
	m_name         = a_rhs.m_name;
	m_length       = a_rhs.m_length;
	m_loop_start   = a_rhs.m_loop_start;
	m_loop_end     = a_rhs.m_loop_end;
	m_loop_enabled = a_rhs.m_loop_enabled;

        /* The tracker automation values AND their selected parameter bindings
           are pattern data.  Omitting this opaque payload made operator= look
           like a successful pattern clone while producing a notes-only copy. */
        m_fx_blob      = a_rhs.m_fx_blob;
        m_track_kind   = a_rhs.m_track_kind;
        m_arrange_lane_id = a_rhs.m_arrange_lane_id;

	m_time_beats_per_measure = a_rhs.m_time_beats_per_measure;
	m_time_beat_width = a_rhs.m_time_beat_width;

        /* SCALE-MASTER / SCALE-FOLLOW settings are PATTERN DATA (docs/
           scale-follow.md) -- persisted per sequence, not runtime state.
           Leaving them out meant every duplicated or pasted clip silently lost
           its scale-follow role: a follower stopped snapping to the master
           scale, and a copy of the scale master no longer carried the scale.
           The copy played different notes than the clip it came from, which is
           exactly the "duplicated clips are mangled" symptom. */
        m_is_scale_master = a_rhs.m_is_scale_master;
        m_follows_master  = a_rhs.m_follows_master;
        m_master_scale    = a_rhs.m_master_scale;
        m_master_key      = a_rhs.m_master_key;

	m_playing      = false;

	/* no notes are playing */
	for (int i=0; i< c_midi_notes; i++ )
	    m_playing_notes[i] = 0;

        /* The remaining scale fields are the opposite: a per-tick snapshot the
           output thread pushes down (m_have_master/m_follow_key/m_follow_scale)
           and the note-on->note-off pitch latch (m_note_remap).  Those are
           RUNTIME, so the copy starts clean rather than inheriting a latch that
           would make its first note-off snap to a pitch it never played. */
        m_have_master  = false;
        m_follow_key   = 0;
        m_follow_scale = 0;
        for (int i = 0; i < c_midi_notes; i++ )
            m_note_remap[i] = -1;
    for ( int i = 0; i < c_max_columns; i++ )
        m_column_note[i] = -1;

	/* reset */
	zero_markers( );

	/* BOTH lists were replaced above: every node the four markers pointed
	   at belonged to the old contents and is gone.  zero_markers() only
	   resets the tick counter, not these. */
	m_iterator_play         = m_list_event.begin();
	m_iterator_draw         = m_list_event.begin();
	m_iterator_draw_trigger = m_list_trigger.begin();
    }

    verify_and_link();

    unlock();

    return *this;

}



void 
sequence::lock( )
{
    m_mutex.lock();
}


void 
sequence::unlock( )
{   
    m_mutex.unlock();
}



const char* 
sequence::get_name()
{
    return m_name.c_str();
}

long
sequence::get_last_tick( )
{
    /* self-defense: never divide by a zero/garbage length (a freed or
       half-constructed sequence read from another thread crashed here) */
    if ( m_length <= 0 )
        return 0;

    /*  The playhead the piano roll / tracker draws is a PATTERN position, and
        seq24 derived it modulo m_length because that was the only repetition
        there was.  A clip looping a short window replays that window while
        this swept the whole pattern, so the drawn playhead ran off past the
        loop's grey band and bore no relation to what was sounding.  Fold by
        the repetition and report it inside the window.  */
    return pattern_position( m_last_tick );
}


/*  See the header: the inverse of play_span()'s repetition layout. */
long
sequence::pattern_position( long a_song_tick )
{
    if ( m_length <= 0 )
        return 0;

    const long period = repeat_period();
    const bool loop_set = loop_window_set();

    const long anchor = m_play_anchor;

    /*  A ONE-SHOT clip walks its data ONCE, from the clip's start tick to the
        END MARKER -- play_span's one-shot branch does not fold by anything.
        Folding here anyway pinned the drawn playhead inside a loop window the
        clip is not playing, so both editors drew a playhead cycling over a few
        steps while the clip ran straight through the pattern.  A loop window
        may perfectly well be SET on a one-shot (it is just not being used),
        which is why m_loop_enabled -- not `loop_set` -- decides this. */
    if ( anchor >= 0 && !m_loop_enabled ){

        long p = a_song_tick - anchor;

        if ( p < 0 )        p = 0;
        if ( p > m_length ) p = m_length;   /* parked on the end marker */

        return p;
    }

    /*  Fold by the repetition, and by the grid that actually sounds: the
        clip's own left edge (see m_play_anchor), not song tick zero.  Those
        agree only when the clip starts a whole number of repetitions from
        zero, which is exactly what a polyrhythmic window does not do.  Live
        mode has no clip and keeps the song-zero grid. */
    long p = ( anchor >= 0 ) ? ( a_song_tick - anchor )
                             : ( a_song_tick - m_trigger_offset );
    p %= period;
    if ( p < 0 )
        p += period;

    return ( loop_set ? m_loop_start : 0 ) + p;
}

void
sequence::set_midi_bus( char  a_mb )
{
    lock();
    
    /* off notes except initial */
    off_playing_notes( );
    
    this->m_bus = a_mb;
    set_dirty();

    unlock();
}

char
sequence::get_midi_bus(  )
{
    return this->m_bus;
}



void 
sequence::set_length( long a_len, bool a_adjust_triggers )
{
    lock();

    bool was_playing = get_playing();

    /* turn everything off */
    set_playing( false );

    if ( a_len < (c_ppqn / 4) )
        a_len = (c_ppqn /4);

    if ( a_adjust_triggers )
        adjust_trigger_offsets_to_legnth( a_len  );

    /*  A window that spanned the WHOLE old pattern is the "no loop set"
        default (see m_loop_start's comment), so it has to keep spanning the
        whole pattern when the length changes -- otherwise merely setting a
        pattern's length turned the default into a real, shorter loop window
        pinned at the OLD length: a fresh sequence (ctor length 4 beats) grown
        to four bars silently looped its first beat, and adjust_trigger_offsets
        _to_legnth() below then treated it as a user window and skipped it.
        An explicitly positioned window is left alone and only clamped. */
    const bool spanned_whole = ( m_loop_start == 0 && m_loop_end == m_length );

    m_length = a_len;

    if ( spanned_whole )
        m_loop_end = m_length;

    /*  HIDE THE WINDOW, DO NOT DESTROY IT -- exactly the policy the note list
        three lines below already follows.  Clamping the bounds down on a shrink
        made every length change eat the user's loop: the tracker's LINES box, a
        project load that sets length before loop bounds, a paste, or dragging
        the END marker in and back out all silently reset a carefully placed
        window, and growing the pattern again could not restore it.
        play_span refuses to sound any event at or past m_length, so a window
        reaching beyond the data is silent there rather than wrong -- which is
        also what lets a 3-bar loop sit over a 2-bar phrase, the standard way to
        build an odd-length loop.  Only the degenerate ordering is repaired. */
    if ( m_loop_start > m_loop_end ) m_loop_start = m_loop_end;
    if ( m_loop_start < 0 ) m_loop_start = 0;

    /* Only re-link.  Pruning here is what made shortening a pattern destroy
       everything past the new end; play_span already refuses to emit an event
       that lies outside the pattern, so the hidden notes are silent until the
       length grows back.

       This used to prune whenever the pattern was NOT shrinking, which threw
       away the hidden notes on a PARTIAL re-grow: shorten a four-bar pattern to
       one bar (bars 2-4 hidden, by design), then grow it to two -- "not
       shrinking", so everything past bar 2 was deleted and the last two bars
       could never come back.  A length change alone must never delete an
       event, in either direction. */
    verify_and_link( false );

    reset_draw_marker();
    
    /* start up and refresh */
    if ( was_playing )
	set_playing( true );
    
    unlock();
}


long
sequence::get_length( )
{
    return m_length;
}


void
sequence::set_loop_start( long a_tick )
{
    lock();
    /*  NOT clamped against m_length -- see set_loop_end() below.  The window is
        stored as the user placed it; set_length() already hides rather than
        destroys the part that reaches past the data, and clamping HERE but not
        there meant a window survived a shrink yet was silently truncated the
        next time it was written back (a project save/load round trip, an undo
        that restores the whole project file).  A window that starts past the
        end marker is simply silent, exactly like one that ends past it. */
    if ( a_tick < 0 ) a_tick = 0;
    if ( a_tick > m_loop_end ) a_tick = m_loop_end;
    m_loop_start = a_tick;
    if ( getenv("PATCHKNOB_TRACELOOP") )
        fprintf(stderr, "[loop] set_loop_start seq='%s'@%p -> [%ld,%ld) len=%ld\n",
                m_name.c_str(), (void*)this, m_loop_start, m_loop_end, m_length);
    unlock();
}


void
sequence::set_loop_enabled( bool a_on )
{
    lock();
    if ( m_loop_enabled != a_on ){
        m_loop_enabled = a_on;
        set_dirty();
    }
    unlock();
}


void
sequence::set_loop_end( long a_tick )
{
    lock();
    /*  A LOOP WINDOW MAY REACH PAST THE END MARKER.

        This used to clamp to m_length, which flatly contradicted set_length()
        sixty lines up: that HIDES a window overhanging the data rather than
        destroying it, because a window longer than the phrase under it is the
        standard way to build an odd-length loop (a 3-beat window over a 2-beat
        phrase).  play_span() already refuses to sound anything at or past
        m_length, so the overhang is SILENT there, not wrong.

        Clamping here made the setter the one place that could not express the
        state the rest of the class preserves: restoring a saved project had to
        grow the pattern, place the window and shrink it back to dodge this
        line, and any code path that copied a window between sequences (clip
        duplicate) truncated it.  Order is still enforced -- the window may not
        end before it starts. */
    if ( a_tick < 0 ) a_tick = 0;
    if ( a_tick < m_loop_start ) a_tick = m_loop_start;
    m_loop_end = a_tick;
    if ( getenv("PATCHKNOB_TRACELOOP") )
        fprintf(stderr, "[loop] set_loop_end seq='%s'@%p -> [%ld,%ld) len=%ld\n",
                m_name.c_str(), (void*)this, m_loop_start, m_loop_end, m_length);
    unlock();
}



void 
sequence::set_playing( bool a_p, long a_tick )
{
    lock();

    if ( a_p != get_playing() )
    {
    
        if (a_p){
	
            /* turn on */
            m_playing = true;

        } else {

            /* turn off */
            m_playing = false;
            off_playing_notes( a_tick );

        } 

        // NOT set_dirty().  This is TRANSPORT state, not an edit, and
        // set_dirty() bumps m_edit_revision -- which perform::output_func's
        // live-edit handshake watches.  Every clip start/end and every loop
        // wrap therefore looked like a user edit, firing
        // audio_app_invalidate_future_schedule(): that bumps g_schedEpoch
        // (discarding legitimately queued future ring events) and, before it
        // was made to flush, destroyed the pending loop-boundary note-offs
        // ~14 ms before the audio thread could emit them.  set_dirty_mp()
        // marks the same display flags without claiming the data changed.
        set_dirty_mp();
    }
    
    m_queued = false;

    unlock();
}


void 
sequence::toggle_playing()
{
    set_playing( ! get_playing() );
}

/*  Same toggle, but naming the tick a resulting release belongs on -- see the
    header.  set_playing() ignores the tick when it is turning something ON. */
void 
sequence::toggle_playing( long a_tick )
{
    set_playing( ! get_playing(), a_tick );
}

bool
sequence::get_playing( )
{
    return m_playing;
}


/* SCALE-MASTER / SCALE-FOLLOW accessors -- see docs/scale-follow.md */

void
sequence::set_scale_master( bool a_v )
{
    lock();
    m_is_scale_master = a_v;
    unlock();
}

bool
sequence::get_scale_master( void )
{
    lock();
    bool r = m_is_scale_master;
    unlock();
    return r;
}

void
sequence::set_follows_master( bool a_v )
{
    lock();
    m_follows_master = a_v;
    unlock();
}

bool
sequence::get_follows_master( void )
{
    lock();
    bool r = m_follows_master;
    unlock();
    return r;
}

void
sequence::set_master_scale( int a_scale )
{
    lock();
    m_master_scale = a_scale;
    unlock();
}

int
sequence::get_master_scale( void )
{
    lock();
    int r = m_master_scale;
    unlock();
    return r;
}

void
sequence::set_master_key( int a_key )
{
    lock();
    m_master_key = a_key;
    unlock();
}

int
sequence::get_master_key( void )
{
    lock();
    int r = m_master_key;
    unlock();
    return r;
}

/* Push the per-tick resolved master context into this follower.  Called by
   perform::play on the output thread just before this sequence's play(). */
void
sequence::set_master_scale_context( bool a_on, int a_key, int a_scale )
{
    lock();
    m_have_master  = a_on;
    m_follow_key   = a_key;
    m_follow_scale = a_scale;
    unlock();
}



void 
sequence::set_recording( bool a_r )
{
    lock();
    m_recording = a_r;
    unlock();
}


bool 
sequence::get_recording( )
{
    return m_recording;
}



void 
sequence::set_thru( bool a_r )
{
    lock();
    m_thru = a_r;
    unlock();
}


bool 
sequence::get_thru( )
{
    return m_thru;
}


/* sets sequence name */
/*  LOCKED.  m_name is a std::string and it is not only GUI data: the output
    thread reads it (sequence::play's catch-up-burst diagnostic does
    m_name.c_str()).  commit_recording renames a pattern from the GUI thread the
    instant a take lands, so an unlocked reassignment there can free the buffer
    another thread is printing from. */
void
sequence::set_name( char *a_name )
{
    lock();
    m_name = a_name;
    set_dirty_mp();
    unlock();
}

void
sequence::set_name( string a_name )
{
    lock();
    m_name = a_name;
    set_dirty_mp();
    unlock();
}

void 
sequence::set_midi_channel( unsigned char a_ch )
{
    lock();
    off_playing_notes( );
    m_midi_channel = a_ch;
    set_dirty();
    unlock();
}

unsigned char 
sequence::get_midi_channel( )
{
    return m_midi_channel;
}


void 
sequence::print()
{
    printf("[%s]\n", m_name.c_str()  );

    for( list<event>::iterator i = m_list_event.begin(); i != m_list_event.end(); i++ )
	(*i).print();
    printf("events[%zd]\n\n",m_list_event.size());

}


void 
sequence::print_triggers()
{
    printf("[%s]\n", m_name.c_str()  );

    for( list<trigger>::iterator i = m_list_trigger.begin();
         i != m_list_trigger.end(); i++ ){

        /*long d= c_ppqn / 8;*/
        
        printf ("  tick_start[%ld] tick_end[%ld] off[%ld]\n", (*i).m_tick_start, (*i).m_tick_end, (*i).m_offset );

    }
}


void
sequence::put_event_on_bus( event *a_e, long a_tick, bool a_stranded )
{
    lock();

    /* SCALE-MASTER / SCALE-FOLLOW
       Snap is applied here, the single emit funnel, to a TEMPORARY copy of the
       event -- the stored *a_e is never mutated.  Note-on latches its snapped
       pitch in m_note_remap[orig]; note-off reuses that latch so the off always
       matches the on (stuck-note safety), and m_playing_notes[] is indexed by
       the snapped pitch.  See docs/scale-follow.md sections 1 and 4. */

    bool is_on  = a_e->is_note_on();
    bool is_off = a_e->is_note_off();

    unsigned char orig = a_e->get_note();
    unsigned char note = orig;     /* the pitch we will actually account/emit */

    event tmp;                     /* local copy used only when we snap */
    event *out = a_e;

    if ( ( is_on || is_off ) && m_follows_master && m_have_master ){

        int snapped;

        if ( is_on ){
            snapped = snap_to_scale( orig, m_follow_key, m_follow_scale );
            m_note_remap[orig] = snapped;        /* latch for the matching off */
        }
        else {
            /* note-off: reuse the latched mapping if present */
            if ( m_note_remap[orig] >= 0 ){
                snapped = m_note_remap[orig];
                m_note_remap[orig] = -1;         /* consume the latch */
            }
            else {
                snapped = snap_to_scale( orig, m_follow_key, m_follow_scale );
            }
        }

        if ( snapped < 0 )   snapped = 0;
        if ( snapped > 127 ) snapped = 127;   /* a MIDI data byte is 7 bits */
        note = (unsigned char) snapped;

        if ( note != orig ){
            tmp = *a_e;
            tmp.set_note( note );                /* mutate the COPY only */
            out = &tmp;
        }
    }

    bool skip = false;

    if ( is_on ){

        /* MONOPHONIC RETRIGGER -- a tracker note column is ONE voice.
           This pitch is already sounding, i.e. the pattern holds a second
           note-on with no intervening off: a column re-struck without an OFF
           (backtick), or overlapping same-pitch notes produced by record
           quantise pulling two starts onto the same grid line.

           A bare second note-on strands the first.  verify_and_link() pairs
           each on with the NEXT off of that pitch, so with on,on,off,off the
           FIRST off closes the note the synth is holding and the trailing off
           is then discarded by the m_playing_notes guard below -- the voice
           hangs until the next panic.  Release the sounding voice first and
           then retrigger: off THEN on, in that order, on the same tick. */
        auto release = [&]( int pitch ){
            if ( pitch < 0 || pitch > 255 ) return;
            while ( m_playing_notes[pitch] > 0 ){
                event off = *out;
                off.set_status( EVENT_NOTE_OFF );
                off.set_note( (char) pitch );
                off.set_note_velocity( 0 );
                if ( g_seq_emit_tap )
                    g_seq_emit_tap( this, (int) m_bus, (int) m_midi_channel,
                                    EVENT_NOTE_OFF, (unsigned char) pitch, 0,
                                    a_tick, 1 );
                m_masterbus->play( m_bus, &off, m_midi_channel, a_tick );
                m_playing_notes[pitch]--;
            }
            m_loop_cut[pitch] = false;
        };

        /* A COLUMN is one voice: whatever it was holding is released first,
           even at a different pitch.  This is the tracker rule, and doing it
           here means every instrument gets it -- not just the sampler, which
           has its own per-column voice allocator. */
        const int col = out->has_column() ? (int) out->get_column() : -1;
        if ( col >= 0 && col < c_max_columns ){
            if ( m_column_note[col] >= 0 && m_column_note[col] != (int) note )
                release( m_column_note[col] );
            m_column_note[col] = (int) note;
        }

        /* Same pitch struck again with no intervening off (untagged notes, or
           the same column re-struck at the same pitch). */
        release( note );

        m_playing_notes[note]++;

        /*  Indexed by the SOUNDING pitch, which is why the mark is taken here
            and not at the call site: scale-follow may have snapped it. */
        m_loop_cut[note] = a_stranded;
    }
    if ( is_off ){

        /* Free whichever column was holding this pitch.  Matching by pitch
           rather than by the off's own column tag means an untagged off (an
           imported file, an edit made before the grid re-stamped) still
           releases the voice, instead of leaving it marked busy forever. */
        for ( int c = 0; c < c_max_columns; c++ )
            if ( m_column_note[c] == (int) note ) m_column_note[c] = -1;

        /* This used to DROP the off when the counter was already zero.  The
           counter only tracks what THIS sequence started, so a note the synth
           holds for any other reason -- started before a locate, or by live
           input on the same channel -- was left sounding forever.  A redundant
           note-off is harmless to a synth; a missing one hangs a voice. */
        if ( m_playing_notes[note] > 0 )
            m_playing_notes[note]--;

        if ( m_playing_notes[note] == 0 )
            m_loop_cut[note] = false;
    }

    if ( !skip ){
        if ( g_seq_emit_tap ){
            unsigned char t0, t1;
            out->get_data( &t0, &t1 );
            g_seq_emit_tap( this, (int) m_bus, (int) m_midi_channel,
                            out->get_status(), t0, t1, a_tick, 0 );
        }
        m_masterbus->play( m_bus, out,  m_midi_channel, a_tick );
    }

    m_masterbus->flush();

    unlock();
}


void 
sequence::off_playing_notes( long a_tick )
{
    lock();


    event e;
    
    for ( int x=0; x< c_midi_notes; x++ ){
	
        while( m_playing_notes[x] > 0 ){
			
            e.set_status( EVENT_NOTE_OFF );
            e.set_data( x, 0 );

            if ( g_seq_emit_tap )
                g_seq_emit_tap( this, (int) m_bus, (int) m_midi_channel,
                                EVENT_NOTE_OFF, (unsigned char) x, 0,
                                a_tick, 2 );
            m_masterbus->play( m_bus, &e, m_midi_channel, a_tick );

            m_playing_notes[x]--;
        }
    }

    /* SCALE-FOLLOW: nothing is sounding any more, drop all snap latches */
    for ( int x=0; x< c_midi_notes; x++ ){
        m_note_remap[x] = -1;
        m_loop_cut[x]   = false;
    }

    /* A COLUMN is a voice, and none of them hold anything now.  This was
       left stale, so after a stop or a mute every column still claimed the
       pitch it had been holding, and the next note on that column opened
       with a spurious release of a note that was not sounding. */
    for ( int x=0; x< c_max_columns; x++ )
        m_column_note[x] = -1;

    m_masterbus->flush();


    unlock();
}

/*  HARD CUT AT A LOOP / CLIP BOUNDARY.

    seq24 had exactly one release path -- off_playing_notes(), fired when the
    pattern was switched off -- because a pattern there always repeated at its
    full length and verify_and_link() guaranteed every note-on inside it had a
    note-off inside it too.  Neither holds any more: a clip repeats its own
    window [m_loop_start, m_loop_end), and set_length() HIDES events past the
    end marker instead of deleting them.  Either way a note-on can be played
    while its note-off is skipped, and the voice was then held until the
    trigger ended -- the whole clip, for one note struck every repetition.

    This releases those notes AT the boundary tick (so the audio engine places
    the off at the exact sample, ahead of the next repetition's note-ons at the
    same tick) and leaves everything else sounding: a note that WRAPS the
    pattern end is released by the next repetition's own data and must not be
    cut here.  Unlike perform::reset_sequences' queue_loop_note_offs(), which
    fires at the SONG loop boundary with no tick to aim at and therefore has to
    ask the audio thread to flush "as soon as possible", this path is scheduled
    like any other event. */
void
sequence::cut_stranded_notes( long a_tick )
{
    lock();

    event e;
    bool any = false;

    for ( int x = 0; x < c_midi_notes; x++ ){

        if ( ! m_loop_cut[x] )
            continue;

        m_loop_cut[x] = false;

        while ( m_playing_notes[x] > 0 ){

            e.set_status( EVENT_NOTE_OFF );
            e.set_data( x, 0 );

            if ( g_seq_emit_tap )
                g_seq_emit_tap( this, (int) m_bus, (int) m_midi_channel,
                                EVENT_NOTE_OFF, (unsigned char) x, 0,
                                a_tick, 4 );
            m_masterbus->play( m_bus, &e, m_midi_channel, a_tick );

            m_playing_notes[x]--;
            any = true;
        }

        /* the voice is free again: a column that was holding this pitch is
           no longer busy, and its snap latch is spent */
        for ( int c = 0; c < c_max_columns; c++ )
            if ( m_column_note[c] == x ) m_column_note[c] = -1;
        for ( int n = 0; n < c_midi_notes; n++ )
            if ( m_note_remap[n] == x ) m_note_remap[n] = -1;
    }

    if ( any )
        m_masterbus->flush();

    unlock();
}


void sequence::queue_loop_note_offs()
{
    lock();
    for(int note=0;note<c_midi_notes;++note){
        if(m_playing_notes[note]>0){
            if ( g_seq_emit_tap )
                g_seq_emit_tap( this, (int) m_bus, (int) m_midi_channel,
                                EVENT_NOTE_OFF, (unsigned char) note, 0,
                                -1, 3 );
            PatchKnob::app::audio_app_queue_loop_note_off(
                (int)m_bus,(int)m_midi_channel,note);
        }
        m_playing_notes[note]=0;
        m_note_remap[note]=-1;
        m_loop_cut[note]=false;
    }
    for(int c=0;c<c_max_columns;++c)m_column_note[c]=-1;
    unlock();
}










/* change */
void 
sequence::select_events( unsigned char a_status, unsigned char a_cc, bool a_inverse )
{
    lock();

    unsigned char d0, d1;
    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	/* initially false */
	bool set = false;	
	(*i).get_data( &d0, &d1 );
	
	/* correct status and not CC */
	if ( a_status != EVENT_CONTROL_CHANGE &&
	     (*i).get_status() == a_status )
	    set = true;
	
	/* correct status and correct cc */
	if ( a_status == EVENT_CONTROL_CHANGE &&
	     (*i).get_status() == a_status &&
	     d0 == a_cc )
	    set = true;
	
        if ( set ){

            if ( a_inverse ){
                if ( !(*i).is_selected( ) )
                    (*i).select( );
                else
                    (*i).unselect( );
                    
            }
            else 
                (*i).select( );
	}	    
    }

    unlock();
}

void
sequence::transpose_notes( int a_steps, int a_scale )
{
    event e;

    list<event> transposed_events;

    lock();

    /*  MARK ONLY WHAT WILL BE PUT BACK.

        This used to mark_selected() -- EVERY selected event -- and then only
        re-add the note-ons and note-offs, so remove_marked() below silently
        deleted every other selected event.  "Select all" then "transpose" wiped
        the pattern's controller, pitch-bend and program-change events, with no
        way back short of undo.  Nothing about a transpose should touch them. */
    list<event>::iterator i;
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	if ( (*i).is_selected() &&
	     ( (*i).get_status() == EVENT_NOTE_ON ||
	       (*i).get_status() == EVENT_NOTE_OFF ) )
	    (*i).mark();
	else
	    (*i).unmark();
    }

    const int *transpose_table = NULL;

    if ( a_steps < 0 ){
        transpose_table = &c_scales_transpose_dn[a_scale][0];
        a_steps *= -1;
    }
    else {
        transpose_table = &c_scales_transpose_up[a_scale][0];
    }

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){
	
	/* is it being moved ? */
	if ( ((*i).get_status() ==  EVENT_NOTE_ON ||
              (*i).get_status() ==  EVENT_NOTE_OFF) &&
             (*i).is_marked() ){

            e = (*i);
            e.unmark();

            int  note = e.get_note();

            /*  transpose_table is indexed by note % 12, and C++ keeps the sign
                of the dividend: at note 0 the "step down to the nearest scale
                tone" below produced -1, and -1 % 12 is -1 -- a read one int
                BEFORE the table.  The same underflow can happen inside the
                loop with the DOWN table, whose entries are negative.  Keep the
                index in range at every step. */
            bool off_scale = false;
            if ( note > 0 && transpose_table[note % 12] == 0 ){
                off_scale = true;
                note -= 1;
            }

            for( int x=0; x<a_steps; ++x ){
                if ( note < 0 || note > 127 )
                    break;
                note += transpose_table[note % 12];
            }

            if ( off_scale )
                note += 1;

            if ( note < 0 || note > 127 ){
                /*  Transposing off the end of the MIDI range.  set_note() masks
                    with 0x7F, so this used to WRAP -- a bass note pushed below
                    zero came back as a shriek near the top of the keyboard.
                    Leave the event exactly where it is instead: unmark it so
                    remove_marked() does not delete an event we are not
                    replacing. */
                (*i).unmark();
                continue;
            }

            e.set_note( note );

            transposed_events.push_front(e);

	}
    }

    remove_marked();
    transposed_events.sort();
    m_list_event.merge( transposed_events);
    
   
    verify_and_link();

    unlock();

    
}



// NOT DELETING THE ENDS, NOT SELECTED.
void
sequence::quanize_events( unsigned char a_status, unsigned char a_cc,
                          long a_snap_tick,  int a_divide, bool a_linked,
                          bool a_left )
{
    event e,f;

    lock();

    unsigned char d0, d1;
    list<event>::iterator i;

    list<event> quantized_events;

    /*  MARK ONLY WHAT WILL BE PUT BACK.

        Everything marked here is deleted by remove_marked() below and expected
        to have been replaced by a moved copy.  mark_selected() marked EVERY
        selected event, but only events matching a_status (plus, when a_linked,
        their note-off partners) are ever copied -- so quantising a selection
        that contained anything else destroyed it.  "Select all" then "Quantize"
        in the piano roll is the reachable case: it deleted every CC,
        pitch-bend, aftertouch and program-change event in the pattern while
        appearing to only nudge the notes.  */
    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	(*i).unmark();

	if ( ! (*i).is_selected() )
	    continue;

	(*i).get_data( &d0, &d1 );

	const bool matches =
	    ( a_status != EVENT_CONTROL_CHANGE && (*i).get_status() == a_status ) ||
	    ( a_status == EVENT_CONTROL_CHANGE && (*i).get_status() == a_status &&
	      d0 == a_cc );

	if ( matches )
	    (*i).mark();
    }

    /*  a matched note's partner is replaced through the a_linked branch below,
        so it has to be marked too -- in a SECOND pass, because the partner can
        sit either side of its note-on in the list */
    if ( a_linked ){
	for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	    if ( (*i).is_marked() && (*i).is_linked() ){

		event *partner = (*i).get_linked();
		if ( partner != NULL )
		    partner->mark();
	    }
	}
    }

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

        /* initially false */
	bool set = false;	
	(*i).get_data( &d0, &d1 );
	
	/* correct status and not CC */
	if ( a_status != EVENT_CONTROL_CHANGE &&
	     (*i).get_status() == a_status )
	    set = true;
	
	/* correct status and correct cc */
	if ( a_status == EVENT_CONTROL_CHANGE &&
	     (*i).get_status() == a_status &&
	     d0 == a_cc )
	    set = true;

        if( !(*i).is_marked() )
            set = false;
	
        if ( set ){

            /* copy event */
	    e = (*i);
            (*i).select();
            e.unmark();

            /* THE shared quantiser (src/quantize.h) -- the same code the record
               quantiser and the piano-roll grid snap run through, so all three
               agree.  a_divide stays an integer strength divisor for the
               callers: 1 == full snap, 2 == halfway, and so on. */
            long timestamp = e.get_timestamp();
            PatchKnob::quantize::Params qp;
            qp.grid     = a_snap_tick;
            qp.strength = ( a_divide > 1 ) ? 1.0f / (float) a_divide : 1.0f;
            qp.leftward = a_left;
            long timestamp_delta =
                PatchKnob::quantize::apply( timestamp, qp ) - timestamp;

            e.set_timestamp( timestamp + timestamp_delta );
            quantized_events.push_front(e);
            
            if ( (*i).is_linked() && a_linked ){
                
                f = *(*i).get_linked();
                f.unmark();
                (*i).get_linked()->select();
                
                f.set_timestamp( f.get_timestamp() + timestamp_delta );
                quantized_events.push_front(f);
            }
        }

    }

    remove_marked();
    quantized_events.sort();
    m_list_event.merge(quantized_events);
    verify_and_link();

    unlock();

}






void 
addListVar( list<char> *a_list, long a_var )
{
    long buffer;
    buffer = a_var & 0x7F;

    /* we shift it right 7, if there is 
       still set bits, encode into buffer
       in reverse order */
    while ( ( a_var >>= 7) ){
	buffer <<= 8;
	buffer |= ((a_var & 0x7F) | 0x80);
    }

    while (true){
	
	a_list->push_front( (char) buffer & 0xFF );

	if (buffer & 0x80)
	    buffer >>= 8;
	else
	    break;
    }
}

void
addLongList( list<char> *a_list, long a_x )
{
    a_list->push_front(  (a_x & 0xFF000000) >> 24 );
    a_list->push_front(  (a_x & 0x00FF0000) >> 16 );
    a_list->push_front(  (a_x & 0x0000FF00) >> 8  );
    a_list->push_front(  (a_x & 0x000000FF)       );
}
 

void 
sequence::fill_list( list<char> *a_list, int a_pos )
{

    lock();

    /* clear list */
    *a_list = list<char>();
    
    /* sequence number */
    addListVar( a_list, 0 );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x00 );
    a_list->push_front( 0x02 );
    a_list->push_front( (a_pos & 0xFF00) >> 8 );
    a_list->push_front( (a_pos & 0x00FF)      );
            
    /* name */
    addListVar( a_list, 0 );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x03 );

    int length =  m_name.length();
    if ( length > 0x7F ) length = 0x7f;
    a_list->push_front( length );

    for ( int i=0; i< length; i++ )
	a_list->push_front( m_name.c_str()[i] );	
 
    long timestamp = 0, delta_time = 0, prev_timestamp = 0;
    list<event>::iterator i;

    for ( i = m_list_event.begin(); i != m_list_event.end(); i++ ){

	event e = (*i);
	timestamp = e.get_timestamp();
	delta_time = timestamp - prev_timestamp;
	prev_timestamp = timestamp;

	/* encode delta_time */
	addListVar( a_list, delta_time );

	/* now that the timestamp is encoded, do the status and
	   data */

	a_list->push_front( e.m_status | m_midi_channel );

	switch( e.m_status & 0xF0 ){

            case 0x80:	  
            case 0x90:
            case 0xA0:
            case 0xB0:
            case 0xE0: 
	    
                a_list->push_front(  e.m_data[0] );
                a_list->push_front(  e.m_data[1] );

                //printf ( "- d[%2X %2X]\n" , e.m_data[0], e.m_data[1] ); 

                break;
	    
            case 0xC0:
            case 0xD0:
	    
                a_list->push_front(  e.m_data[0] );

                //printf ( "- d[%2X]\n" , e.m_data[0] ); 

                break;
	    
            default: 
                break;
	}
    }

    int num_triggers = m_list_trigger.size();
    list<trigger>::iterator t = m_list_trigger.begin();
    list<trigger>::iterator p;

    addListVar( a_list, 0 );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x7F );
    addListVar( a_list, (num_triggers * 3 * 4) + 4);
    addLongList( a_list, c_triggers_new );

    //printf( "num_triggers[%d]\n", num_triggers );

    for ( int i=0; i<num_triggers; i++ ){

        p = t;
        //printf( "> start[%d] end[%d] offset[%d]\n",
        //        (*t).m_tick_start, (*t).m_tick_end, (*t).m_offset );
        
	addLongList( a_list, (*t).m_tick_start );
        addLongList( a_list, (*t).m_tick_end );
        addLongList( a_list, (*t).m_offset ); 
	t++;
    }

    /* bus */
    addListVar( a_list, 0 );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x7F );
    a_list->push_front( 0x05 );
    addLongList( a_list, c_midibus );
    a_list->push_front( m_bus  );

    /* timesig */
    addListVar( a_list, 0 );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x7F );
    a_list->push_front( 0x06 );
    addLongList( a_list, c_timesig );
    a_list->push_front( m_time_beats_per_measure  );
    a_list->push_front( m_time_beat_width  );

    /* channel */
    addListVar( a_list, 0 );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x7F );
    a_list->push_front( 0x05 );
    addLongList( a_list, c_midich );
    a_list->push_front( m_midi_channel );

    delta_time = m_length - prev_timestamp;
 
    /* meta track end */
    addListVar( a_list, delta_time );
    a_list->push_front( 0xFF );
    a_list->push_front( 0x2F );
    a_list->push_front( 0x00 );

    unlock();
}








//     list<char> triggers;

//     /* triggers */

//     list<trigger>::iterator t;
//     for ( t = m_list_trigger.begin(); t != m_list_trigger.end(); t++ ){

// 	addLongList( &triggers, (*t).m_tick );
// 	printf ( "[%ld]\n", (*t).m_tick );
//     }    

//     addListVar( a_list, 0 );
//     a_list->push_front( 0xFF );
//     a_list->push_front( 0x7F );

//     a_list->push_front( 0x05 );
//     addLongList( a_list, c_triggersmidibus );
