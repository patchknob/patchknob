//----------------------------------------------------------------------------
//  sdlui/views/tracker/tracker_view.cpp -- SDL2 port of the PatchKnob tracker.
//  See tracker_view.h.  Logic mirrors src/trackeredit.cpp (trackergrid) but
//  draws through the ui:: toolkit and reads/writes the shared engine sequence.
//----------------------------------------------------------------------------
#include "tracker_view.h"

#include "sequence.h"       // engine model  (pulls event.h / globals.h)
#include "audio_app.h"      // header-only bridge to the VST engine
#include "engine/plugin_api.h"
#include "engine/rack/rack_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>       // memcpy (FX blob serialise)

namespace ui {

namespace {

int clamp_int( int v, int lo, int hi )
{
    if ( v < lo ) return lo;
    if ( v > hi ) return hi;
    return v;
}

float clamp_float( float v, float lo, float hi )
{
    if ( v < lo ) return lo;
    if ( v > hi ) return hi;
    return v;
}

std::string clip_chars( const std::string& s, std::size_t n )
{
    if ( s.size() <= n )
        return s;
    return s.substr( 0, n );
}

std::string compact_label( const std::string& name, const char* fallback )
{
    std::string src = name.empty() ? std::string( fallback ) : name;
    std::string out;
    for ( std::size_t i = 0; i < src.size() && out.size() < 4; ++i )
    {
        unsigned char c = (unsigned char) src[i];
        if ( c > ' ' && c != '>' && c != '/' && c != '\\' )
            out.push_back( (char) c );
    }
    if ( out.empty() )
        out = fallback;
    return clip_chars( out, 4 );
}

static const int k_lpb_values[] =
    { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };

int lpb_index( int lpb )
{
    for ( int i = 0; i < (int)( sizeof(k_lpb_values) / sizeof(k_lpb_values[0]) ); ++i )
        if ( k_lpb_values[i] == lpb )
            return i;
    return -1;
}

} // anonymous namespace

//----------------------------------------------------------------------------
//  construction
//----------------------------------------------------------------------------
TrackerView::TrackerView( sequence* seq, int track )
{
    m_seq   = seq;
    m_track = track;

    m_rows_per_beat = 4;
    m_num_tracks    = 1;
    m_fx_cols       = 2;

    m_cursor_row   = 0;
    m_cursor_track = 0;
    m_cursor_col   = 0;
    m_octave       = 4;
    m_edit_step    = 1;
    m_velocity     = 100;
    m_top_row      = 0;
    m_last_progress_row = -1;
    m_last_fire_row     = -1;

    // multi-cell selection + clipboard state (see header).
    m_sel_active     = false;
    m_sel_drag       = false;
    m_sel_anchor_row = 0;
    m_sel_anchor_track = 0;
    m_sel_anchor_col   = 0;
    m_sel_row0       = 0;
    m_sel_row1       = 0;
    m_sel_track      = 0;
    m_sel_col        = 0;
    m_sel_track0     = 0;
    m_sel_track1     = 0;
    m_sel_col0       = 0;
    m_sel_col1       = 0;
    m_copied         = false;
    m_menu_open      = false;
    m_menu_x         = 0;
    m_menu_y         = 0;
    m_menu_w         = 0;
    m_menu_h         = 0;
    m_menu_scroll    = 0;

    m_follow         = false;
    m_vis_rows       = 1;
    for ( int t = 0; t < 8; ++t )
        m_col_muted[t] = false;

    // FX state is PER PATTERN: start from defaults, then adopt this sequence's own
    // saved FX blob if it has one (so a reopened pattern keeps its automation).
    reset_fx_defaults();
    if ( m_seq && !m_seq->get_fx_blob().empty() )
        deserialize_fx( m_seq->get_fx_blob() );
}

//----------------------------------------------------------------------------
//  per-pattern FX state (bugs: FX automation bleeding across tracker patterns)
//----------------------------------------------------------------------------
// Fresh bindings + empty value store, sized for the max note-column count (8) so
// growing the column count never reallocates out from under a live cursor.
void
TrackerView::reset_fx_defaults()
{
    // Reset the column COUNT too, else a fresh/empty pattern inherits the FX-column
    // count from the pattern just viewed (deserialize_fx returns early on an empty
    // blob, so without this the count bleeds and gets committed into the new pattern).
    m_fx_cols = 2;
    m_fx_bind.assign( 8, std::vector<FxBinding>() );
    m_fx_vst.assign( 8, std::vector<std::map<int,int> >() );
    for ( int t = 0; t < 8; ++t )
    {
        m_fx_bind[t].resize( m_fx_cols );
        m_fx_vst[t].resize( m_fx_cols );
        m_fx_bind[t][0].type  = FX_MIDI_CC;
        m_fx_bind[t][0].cc    = 74;                 // filter cutoff
        m_fx_bind[t][0].label = "C74";
        if ( m_fx_cols > 1 )
        {
            m_fx_bind[t][1].type  = FX_MIDI_CC;
            m_fx_bind[t][1].cc    = 7;              // channel volume
            m_fx_bind[t][1].label = "C07";
        }
    }
}

namespace {
// little-endian appenders / cursor-reader over a std::string blob.
void blob_u8 ( std::string& b, unsigned v ) { b.push_back( (char)(v & 0xff) ); }
void blob_i32( std::string& b, int v ) { for ( int i=0;i<4;++i ) b.push_back( (char)((v>>(8*i))&0xff) ); }
void blob_u32( std::string& b, unsigned v ) { for ( int i=0;i<4;++i ) b.push_back( (char)((v>>(8*i))&0xff) ); }
void blob_f32( std::string& b, float f ) { unsigned u; std::memcpy(&u,&f,4); blob_u32(b,u); }
void blob_str( std::string& b, const std::string& s ) { blob_u32(b,(unsigned)s.size()); b += s; }
struct BlobR {
    const unsigned char* p; size_t n, pos; bool ok;
    BlobR( const std::string& s ) : p((const unsigned char*)s.data()), n(s.size()), pos(0), ok(true) {}
    bool need( size_t k ) { if ( pos + k > n ) { ok = false; return false; } return true; }
    unsigned u8 () { if(!need(1)) return 0; return p[pos++]; }
    int      i32() { if(!need(4)) return 0; int v=0; for(int i=0;i<4;++i) v|=(int)p[pos++]<<(8*i); return v; }
    unsigned u32() { if(!need(4)) return 0; unsigned v=0; for(int i=0;i<4;++i) v|=(unsigned)p[pos++]<<(8*i); return v; }
    float    f32() { unsigned u=u32(); float f; std::memcpy(&f,&u,4); return f; }
    std::string str() { unsigned k=u32(); if(!ok||pos+k>n){ok=false;return std::string();} std::string s((const char*)p+pos,k); pos+=k; return s; }
};
} // namespace

// Pack m_fx_cols + every binding + its VST-param value map into an opaque blob.
// Values with row >= the pattern's line count are dropped (they'd otherwise leak
// back when a longer pattern is shown).  MIDI-CC values are NOT stored here --
// they live as real CC events in the sequence.
std::string
TrackerView::serialize_fx() const
{
    std::string b;
    blob_u8( b, (unsigned)m_fx_cols );
    const int maxRow = pattern_lines();
    for ( int t = 0; t < 8; ++t )
    {
        for ( int fi = 0; fi < m_fx_cols; ++fi )
        {
            const FxBinding bd = ( fi < (int)m_fx_bind[t].size() ) ? m_fx_bind[t][fi] : FxBinding();
            blob_i32( b, bd.type );   blob_i32( b, bd.target );
            blob_i32( b, bd.cc );     blob_u32( b, bd.pid );
            blob_i32( b, bd.node );   blob_i32( b, bd.module );
            blob_f32( b, bd.min_value ); blob_f32( b, bd.max_value );
            blob_str( b, bd.label );  blob_str( b, bd.name );
            // value map (rows in range only)
            std::string vv; unsigned cnt = 0;
            if ( fi < (int)m_fx_vst[t].size() )
                for ( std::map<int,int>::const_iterator it = m_fx_vst[t][fi].begin();
                      it != m_fx_vst[t][fi].end(); ++it )
                    if ( it->first >= 0 && it->first < maxRow )
                        { blob_i32( vv, it->first ); blob_i32( vv, it->second ); ++cnt; }
            blob_u32( b, cnt ); b += vv;
        }
    }
    // --- trailer (newer blobs): tracker note-column state.  MIDI events carry no
    // "tracker column", so the note->lane map and the explicit-OFF markers are the
    // ONLY record of which column each note/off sits in.  Persist them here so
    // columns survive save/reload; older blobs simply lack this trailer and fall
    // back to auto-lane packing.
    blob_u32( b, (unsigned)m_note_lanes.size() );
    for ( std::map<NoteKey,int>::const_iterator it = m_note_lanes.begin();
          it != m_note_lanes.end(); ++it )
    {
        blob_i32( b, (int)it->first.ts );
        blob_i32( b, it->first.note );
        blob_i32( b, it->first.occurrence );
        blob_i32( b, it->second );
    }
    blob_u32( b, (unsigned)m_explicit_note_offs.size() );
    for ( std::map<NoteKey,ExplicitNoteOff>::const_iterator it = m_explicit_note_offs.begin();
          it != m_explicit_note_offs.end(); ++it )
    {
        blob_i32( b, (int)it->first.ts );
        blob_i32( b, it->first.note );
        blob_i32( b, it->first.occurrence );
        blob_i32( b, (int)it->second.tick );
        blob_i32( b, it->second.track );
    }
    return b;
}

// Restore FX state from a blob (empty / malformed -> defaults).
void
TrackerView::deserialize_fx( const std::string& blob )
{
    reset_fx_defaults();
    // View-side per-note column state travels in the blob trailer -- start clean so
    // a fresh/empty pattern never inherits the previous pattern's columns.
    m_note_lanes.clear();
    m_explicit_note_offs.clear();
    if ( blob.empty() ) return;
    BlobR r( blob );
    int cols = (int)r.u8();
    if ( !r.ok || cols < 1 || cols > 16 ) return;
    m_fx_cols = cols;
    for ( int t = 0; t < 8; ++t )
    {
        m_fx_bind[t].assign( m_fx_cols, FxBinding() );
        m_fx_vst[t].assign( m_fx_cols, std::map<int,int>() );
        for ( int fi = 0; fi < m_fx_cols && r.ok; ++fi )
        {
            FxBinding& bd = m_fx_bind[t][fi];
            bd.type = r.i32();   bd.target = r.i32();
            bd.cc   = r.i32();   bd.pid    = (unsigned)r.i32();
            bd.node = r.i32();   bd.module = r.i32();
            bd.min_value = r.f32(); bd.max_value = r.f32();
            bd.label = r.str();  bd.name = r.str();
            unsigned cnt = r.u32();
            for ( unsigned k = 0; k < cnt && r.ok; ++k )
            { int row = r.i32(); int val = r.i32(); if ( r.ok ) m_fx_vst[t][fi][row] = val; }
        }
    }
    if ( !r.ok ) { reset_fx_defaults(); return; }   // FX section corrupt -> safe defaults

    // --- optional trailer (newer blobs): note-column lanes + explicit OFF markers.
    // Absent on older blobs (r.pos == r.n here), so columns just fall back to
    // auto-lanes; a truncated trailer stops reading without discarding the FX data.
    if ( r.pos < r.n )
    {
        unsigned lc = r.u32();
        for ( unsigned k = 0; k < lc && r.ok; ++k )
        {
            long ts = (long)r.i32(); int nt = r.i32(); int oc = r.i32(); int ln = r.i32();
            if ( r.ok ) m_note_lanes[ note_key( ts, nt, oc ) ] = ln;
        }
        if ( r.ok && r.pos < r.n )
        {
            unsigned oc2 = r.u32();
            for ( unsigned k = 0; k < oc2 && r.ok; ++k )
            {
                long kts = (long)r.i32(); int knt = r.i32(); int koc = r.i32();
                long otick = (long)r.i32(); int otrk = r.i32();
                if ( r.ok )
                    m_explicit_note_offs[ note_key( kts, knt, koc ) ] =
                        ExplicitNoteOff{ otick, otrk };
            }
        }
    }
}

// Flush the current view FX state back into the sequence it belongs to.
void
TrackerView::commit_fx()
{
    if ( m_seq ) m_seq->set_fx_blob( serialize_fx() );
}

// Load the current sequence's FX blob into the view (defaults if none).  Kept in
// the .cpp because the inline set_sequence in the header only sees a forward
// declaration of `sequence`.
void
TrackerView::load_current_fx()
{
    deserialize_fx( m_seq ? m_seq->get_fx_blob() : std::string() );
    int rows = num_rows();
    if ( m_cursor_row >= rows ) m_cursor_row = rows - 1;
    if ( m_cursor_row < 0 ) m_cursor_row = 0;
    if ( m_top_row >= rows ) m_top_row = rows - 1;
    if ( m_top_row < 0 ) m_top_row = 0;
    sync_lines_edit();
}

//----------------------------------------------------------------------------
//  model <-> grid helpers
//----------------------------------------------------------------------------
int
TrackerView::vst_track( void ) const
{
    return m_track >= 0 ? m_track : ( m_seq ? (int) m_seq->get_midi_bus() : 0 );
}

int
TrackerView::ticks_per_row( void ) const
{
    int lpb = m_rows_per_beat;
    if ( lpb < 1 ) lpb = 1;
    int t = c_ppqn / lpb;
    if ( t < 1 ) t = 1;
    return t;
}

int
TrackerView::num_rows( void ) const
{
    if ( !m_seq )
        return 1;
    int rows = m_seq->get_length() / ticks_per_row();
    if ( rows < 1 ) rows = 1;
    return rows;
}

int
TrackerView::pattern_lines( void ) const
{
    return m_seq ? num_rows() : 1;
}

void
TrackerView::sync_lines_edit( void )
{
    char buf[16];
    snprintf( buf, sizeof(buf), "%d", pattern_lines() );
    m_lines_edit = buf;
}

void
TrackerView::set_pattern_lines( int lines )
{
    if ( !m_seq )
        return;
    if ( lines < 1 ) lines = 1;
    if ( lines > 4096 ) lines = 4096;
    const long new_length = (long) lines * ticks_per_row();
    if ( new_length == m_seq->get_length() )
        return;
    const long old_length = m_seq->get_length();
    m_seq->set_length( new_length, false );
    if ( new_length < old_length )
    {
        for ( std::map< NoteKey, ExplicitNoteOff >::iterator it = m_explicit_note_offs.begin();
              it != m_explicit_note_offs.end(); )
        {
            if ( it->first.ts >= new_length || it->second.tick >= new_length )
                m_explicit_note_offs.erase( it++ );
            else
                ++it;
        }
        for ( std::map< NoteKey, int >::iterator it = m_note_lanes.begin();
              it != m_note_lanes.end(); )
        {
            if ( it->first.ts >= new_length )
                m_note_lanes.erase( it++ );
            else
                ++it;
        }
        for ( int t = 0; t < m_num_tracks; ++t )
            for ( int f = 0; f < (int)m_fx_vst[t].size(); ++f )
                for ( std::map<int,int>::iterator it = m_fx_vst[t][f].begin();
                      it != m_fx_vst[t][f].end(); )
                {
                    if ( it->first >= lines )
                        m_fx_vst[t][f].erase( it++ );
                    else
                        ++it;
                }
    }
    m_seq->set_dirty();
    if ( m_cursor_row >= lines )
        m_cursor_row = lines - 1;
    if ( m_top_row > m_cursor_row )
        m_top_row = m_cursor_row;
    sync_lines_edit();
}

void
TrackerView::begin_lines_edit( App& app )
{
    if ( !m_seq )
        return;
    sync_lines_edit();
    app.begin_text( &m_lines_edit, [&app]() { app.request_redraw(); },
        [this, &app]( bool ok )
        {
            if ( ok )
            {
                char* end = nullptr;
                long value = std::strtol( m_lines_edit.c_str(), &end, 10 );
                if ( end != m_lines_edit.c_str() )
                    set_pattern_lines( (int) value );
            }
            sync_lines_edit();
            ensure_cursor_visible( app );
            app.request_redraw();
        } );
}

long
TrackerView::row_start_tick( int row ) const
{
    return (long) row * ticks_per_row();
}

long
TrackerView::length_measures( void ) const
{
    if ( !m_seq )
        return 1;
    long bpm = m_seq->get_bpm();
    if ( bpm < 1 ) bpm = 4;
    long measures = m_seq->get_length() / ( c_ppqn * bpm );
    if ( measures < 1 ) measures = 1;
    return measures;
}

std::string
TrackerView::note_name( int note )
{
    static const char* names[12] =
        { "C-", "C#", "D-", "D#", "E-", "F-",
          "F#", "G-", "G#", "A-", "A#", "B-" };
    if ( note < 0 || note > 127 )
        return "---";
    int pc  = note % 12;
    int oct = note / 12 - 1;             // MIDI note 60 == C-4
    char buf[8];
    snprintf( buf, sizeof(buf), "%s%d", names[pc], oct );
    return std::string( buf );
}

// gather the note-ons that start in `row`, preserving explicit tracker lanes.
void
TrackerView::collect_row_notes( int row, std::vector<NoteCell>& out )
{
    out.clear();
    if ( !m_seq )
        return;
    NoteCell empty;
    out.assign( m_num_tracks, empty );

    long ts = row_start_tick( row );
    long tf = ts + ticks_per_row();

    long tick_s, tick_f;
    int  note, vel;
    bool selected;

    int order = 0;
    std::map< std::pair<long,int>, int > occurrences;
    m_seq->reset_draw_marker();
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        int occurrence = occurrences[ std::make_pair( tick_s, note ) ]++;
        if ( tick_s >= ts && tick_s < tf )
        {
            NoteCell nc;
            nc.note = note;
            nc.vel  = vel;
            nc.ts   = tick_s;
            nc.order = order;
            nc.occurrence = occurrence;
            nc.has_off = tick_f > tick_s;
            nc.tf   = normalized_note_end( tick_s, tick_f );
            long explicit_off = 0;
            if ( has_explicit_note_off( tick_s, note, occurrence, &explicit_off ) )
            {
                nc.has_off = true;
                nc.tf = explicit_off;
            }
            int lane = assigned_note_lane( tick_s, note, occurrence );
            if ( lane >= 0 && lane < m_num_tracks && out[lane].note < 0 )
            {
                out[lane] = nc;
            }
            else
            {
                for ( int i = 0; i < m_num_tracks; ++i )
                    if ( out[i].note < 0 )
                    {
                        out[i] = nc;
                        break;
                    }
            }
        }
        ++order;
    }
}

// Gather OFF markers placed with backtick.  The map records the selected note
// column, rather than deriving one by sorting all sounding pitches: otherwise
// a still-held duplicate pitch shifts an OFF into another column.
void
TrackerView::collect_row_note_offs( int row, std::vector<NoteCell>& out )
{
    out.clear();
    if ( !m_seq )
        return;

    long rs = row_start_tick( row );
    NoteCell empty;
    out.assign( m_num_tracks, empty );

    for ( std::map< NoteKey, ExplicitNoteOff >::const_iterator it =
              m_explicit_note_offs.begin(); it != m_explicit_note_offs.end(); ++it )
    {
        if ( it->second.tick != rs || it->second.track < 0 ||
             it->second.track >= m_num_tracks )
            continue;

        long tick_s, tick_f;
        int note, vel;
        bool selected;
        std::map< std::pair<long,int>, int > occurrences;
        m_seq->reset_draw_marker();
        while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                            &selected, &vel ) != DRAW_FIN )
        {
            int occurrence = occurrences[ std::make_pair( tick_s, note ) ]++;
            if ( tick_s == it->first.ts && note == it->first.note &&
                 occurrence == it->first.occurrence )
            {
                NoteCell nc;
                nc.note = note;
                nc.vel = vel;
                nc.ts = tick_s;
                nc.tf = rs;
                nc.occurrence = occurrence;
                nc.has_off = true;
                out[it->second.track] = nc;
                break;
            }
        }
    }
}

bool
TrackerView::note_before_row_in_column( int row, int track, NoteCell* out )
{
    if ( !out || track < 0 )
        return false;

    std::vector<NoteCell> nl;
    for ( int r = row - 1; r >= 0; --r )
    {
        collect_row_notes( r, nl );
        if ( track < (int) nl.size() && nl[track].note >= 0 )
        {
            *out = nl[track];
            return true;
        }
    }
    return false;
}

bool
TrackerView::end_unreleased_note_before( int row, int track, long end_tick )
{
    NoteCell previous;
    if ( !note_before_row_in_column( row, track, &previous ) ||
         previous.has_off || previous.ts >= end_tick )
        return false;

    clear_explicit_note_off( previous.ts, previous.note, previous.occurrence );
    remove_specific_note( previous );
    if ( !add_note_pair( previous.ts, end_tick, previous.note, previous.vel ) )
    {
        if ( add_note_on( previous.ts, previous.note, previous.vel ) )
            set_note_lane( previous.ts, previous.note, previous.occurrence, track );
        return false;
    }
    set_explicit_note_off( previous.ts, previous.note, previous.occurrence, end_tick, track );
    return true;
}

bool
TrackerView::note_at_row( int row, int track, int* note, int* vel )
{
    std::vector<NoteCell> nl;
    collect_row_notes( row, nl );
    if ( track < 0 || track >= (int) nl.size() || nl[track].note < 0 )
        return false;
    if ( note ) *note = nl[track].note;
    if ( vel )  *vel  = nl[track].vel;
    return true;
}

void
TrackerView::clear_explicit_note_off( long ts, int note, int occurrence )
{
    m_explicit_note_offs.erase( note_key( ts, note, occurrence ) );
}

TrackerView::NoteKey
TrackerView::note_key( long ts, int note, int occurrence ) const
{
    NoteKey key;
    key.ts = ts;
    key.note = note;
    key.occurrence = occurrence < 0 ? 0 : occurrence;
    return key;
}

int
TrackerView::note_occurrence_count( long ts, int note ) const
{
    if ( !m_seq )
        return 0;
    long tick_s, tick_f;
    int ev_note, vel;
    bool selected;
    int count = 0;
    m_seq->reset_draw_marker();
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &ev_note,
                                        &selected, &vel ) != DRAW_FIN )
        if ( tick_s == ts && ev_note == note )
            ++count;
    return count;
}

int
TrackerView::assigned_note_lane( long ts, int note, int occurrence ) const
{
    std::map< NoteKey, int >::const_iterator it =
        m_note_lanes.find( note_key( ts, note, occurrence ) );
    return it == m_note_lanes.end() ? -1 : it->second;
}

void
TrackerView::set_note_lane( long ts, int note, int occurrence, int track )
{
    if ( track < 0 || track >= m_num_tracks )
        return;
    m_note_lanes[ note_key( ts, note, occurrence ) ] = track;
}

void
TrackerView::clear_note_lane( long ts, int note, int occurrence )
{
    m_note_lanes.erase( note_key( ts, note, occurrence ) );
}

long
TrackerView::normalized_note_end( long tick_s, long tick_f ) const
{
    if ( tick_f > tick_s )
        return tick_f;
    long L = m_seq ? m_seq->get_length() : 0;
    if ( L <= tick_s )
        return tick_s + 1;
    return L;
}

bool
TrackerView::has_explicit_note_off( long ts, int note, int occurrence,
                                    long* off_tick,
                                    int* track ) const
{
    std::map< NoteKey, ExplicitNoteOff >::const_iterator it =
        m_explicit_note_offs.find( note_key( ts, note, occurrence ) );
    if ( it == m_explicit_note_offs.end() )
        return false;
    if ( off_tick )
        *off_tick = it->second.tick;
    if ( track )
        *track = it->second.track;
    return true;
}

void
TrackerView::set_explicit_note_off( long ts, int note, int occurrence,
                                    long off_tick, int track )
{
    ExplicitNoteOff value;
    value.tick = off_tick;
    value.track = clamp_int( track, 0, m_num_tracks - 1 );
    m_explicit_note_offs[ note_key( ts, note, occurrence ) ] = value;
}

bool
TrackerView::add_note_on( long ts, int note, int vel )
{
    if ( !m_seq || note < 0 || note > 127 )
        return false;
    if ( vel < 1 )   vel = 1;
    if ( vel > 127 ) vel = 127;
    long L = m_seq->get_length();
    if ( L < 1 || ts < 0 || ts >= L )
        return false;

    m_seq->add_event( ts, EVENT_NOTE_ON, (unsigned char) note,
                      (unsigned char) vel, false );
    return true;
}

bool
TrackerView::add_note_pair( long ts, long tf, int note, int vel )
{
    if ( !m_seq || note < 0 || note > 127 )
        return false;
    if ( vel < 1 )   vel = 1;
    if ( vel > 127 ) vel = 127;
    long L = m_seq->get_length();
    if ( L < 2 || ts < 0 || ts >= L - 1 )
        return false;
    if ( tf <= ts )
        tf = ts + 1;
    if ( tf >= L )
        tf = L - 1;
    if ( tf <= ts )
        return false;

    m_seq->add_event( ts, EVENT_NOTE_ON,  (unsigned char) note,
                      (unsigned char) vel, false );
    m_seq->add_event( tf, EVENT_NOTE_OFF, (unsigned char) note,
                      (unsigned char) vel, false );
    return true;
}

bool
TrackerView::add_note_cell( long ts, long tf, int note, int vel, bool has_off )
{
    if ( has_off )
        return add_note_pair( ts, tf, note, vel );
    clear_explicit_note_off( ts, note, note_occurrence_count( ts, note ) );
    return add_note_on( ts, note, vel );
}

bool
TrackerView::restore_default_note_off( const NoteCell& nc )
{
    if ( !m_seq )
        return false;

    m_seq->push_undo();
    remove_specific_note( nc );
    bool ok = add_note_on( nc.ts, nc.note, nc.vel );
    clear_explicit_note_off( nc.ts, nc.note, nc.occurrence );
    m_seq->verify_and_link();
    m_seq->set_dirty();
    return ok;
}

bool
TrackerView::clear_note_off_cell( void )
{
    if ( m_cursor_col != 0 )
        return false;
    std::vector<NoteCell> offl;
    collect_row_note_offs( m_cursor_row, offl );
    if ( m_cursor_track < 0 || m_cursor_track >= (int) offl.size() ||
         offl[m_cursor_track].note < 0 )
        return false;
    return restore_default_note_off( offl[m_cursor_track] );
}

// remove exactly the note whose ON is at `ts` with pitch `note`.
void
TrackerView::remove_specific_note( long ts, int note, int occurrence )
{
    if ( m_seq )
        m_seq->remove_note_at( ts, note, occurrence );
}

void
TrackerView::remove_specific_note( const NoteCell& nc )
{
    remove_specific_note( nc.ts, nc.note, nc.occurrence );
}

void
TrackerView::set_note_at_cell( int note )
{
    if ( !m_seq || note < 0 || note > 127 )
        return;

    long ts  = row_start_tick( m_cursor_row );
    const long L  = m_seq->get_length();
    if ( L < 1 || ts < 0 || ts >= L )
        return;

    m_seq->push_undo();

    // if this note-column slot already holds a note, replace just that one.
    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track < (int) nl.size() && nl[m_cursor_track].note >= 0 )
    {
        clear_explicit_note_off( nl[m_cursor_track].ts, nl[m_cursor_track].note,
                                 nl[m_cursor_track].occurrence );
        clear_note_lane( nl[m_cursor_track].ts, nl[m_cursor_track].note,
                         nl[m_cursor_track].occurrence );
        remove_specific_note( nl[m_cursor_track] );
    }
    else
    {
        // Typing a note over a visible OFF replaces that OFF.  Leaving the old
        // MIDI note-off at this row made the newly entered note die instantly.
        std::vector<NoteCell> offl;
        collect_row_note_offs( m_cursor_row, offl );
        if ( m_cursor_track < (int) offl.size() &&
             offl[m_cursor_track].note >= 0 )
        {
            remove_specific_note( offl[m_cursor_track] );
            add_note_on( offl[m_cursor_track].ts,
                         offl[m_cursor_track].note,
                         offl[m_cursor_track].vel );
            clear_explicit_note_off( offl[m_cursor_track].ts,
                                     offl[m_cursor_track].note,
                                     offl[m_cursor_track].occurrence );
        }
    }
    clear_explicit_note_off( ts, note, note_occurrence_count( ts, note ) );
    clear_note_lane( ts, note, note_occurrence_count( ts, note ) );

    end_unreleased_note_before( m_cursor_row, m_cursor_track, ts );

    if ( add_note_on( ts, note, m_velocity ) )
        set_note_lane( ts, note, note_occurrence_count( ts, note ) - 1,
                       m_cursor_track );

    m_seq->verify_and_link();
    m_seq->set_dirty();
}

void
TrackerView::set_velocity_at_cell( int vel )
{
    if ( !m_seq )
        return;
    if ( vel < 0 )   vel = 0;
    if ( vel > 127 ) vel = 127;

    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track >= (int) nl.size() || nl[m_cursor_track].note < 0 )
        return;                             // no note here to re-velocity

    NoteCell nc = nl[m_cursor_track];
    long explicit_off = 0;
    bool was_explicit = has_explicit_note_off( nc.ts, nc.note, nc.occurrence, &explicit_off );

    m_seq->push_undo();
    remove_specific_note( nc );
    bool ok = add_note_cell( nc.ts, nc.tf, nc.note, vel, nc.has_off );
    if ( !ok )
        ok = add_note_cell( nc.ts, nc.tf, nc.note, nc.vel, nc.has_off );
    if ( was_explicit && ok )
        set_explicit_note_off( nc.ts, nc.note, nc.occurrence, explicit_off, m_cursor_track );
    if ( ok )
        set_note_lane( nc.ts, nc.note, nc.occurrence, m_cursor_track );
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

void
TrackerView::clear_note_cell( void )
{
    if ( !m_seq )
        return;
    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track >= (int) nl.size() || nl[m_cursor_track].note < 0 )
    {
        clear_note_off_cell();
        return;
    }

    m_seq->push_undo();
    clear_explicit_note_off( nl[m_cursor_track].ts, nl[m_cursor_track].note,
                             nl[m_cursor_track].occurrence );
    clear_note_lane( nl[m_cursor_track].ts, nl[m_cursor_track].note,
                     nl[m_cursor_track].occurrence );
    remove_specific_note( nl[m_cursor_track] );
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

//----------------------------------------------------------------------------
//  FX-command helpers
//----------------------------------------------------------------------------
int
TrackerView::cur_fx_index( void ) const
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 ) fi = 0;
    if ( fi >= m_fx_cols ) fi = m_fx_cols - 1;
    return fi;
}

// read the value of the CC bound to `cc` that lives in `row`'s tick window.
bool
TrackerView::read_cc_at( int row, int cc, int* val )
{
    long ts = row_start_tick( row );
    long tf = ts + ticks_per_row();

    long          tick;
    unsigned char d0, d1;
    bool          sel;

    m_seq->reset_draw_marker();
    while ( m_seq->get_next_event( EVENT_CONTROL_CHANGE, (unsigned char) cc,
                                   &tick, &d0, &d1, &sel ) )
    {
        if ( d0 == (unsigned char) cc && tick >= ts && tick < tf )
        {
            if ( val ) *val = d1;
            return true;
        }
    }
    return false;
}

void
TrackerView::remove_cc_at( long ts, int cc )
{
    m_seq->unselect();
    int n = m_seq->select_events( ts, ts + ticks_per_row() - 1,
                                  EVENT_CONTROL_CHANGE, (unsigned char) cc,
                                  sequence::e_select );
    if ( n > 0 )
    {
        m_seq->mark_selected();
        m_seq->remove_marked();
        m_seq->verify_and_link();
        m_seq->set_dirty();
    }
}

void
TrackerView::set_cc_at( long ts, int cc, int val )
{
    if ( val < 0 )   val = 0;
    if ( val > 127 ) val = 127;

    remove_cc_at( ts, cc );
    m_seq->add_event( ts, EVENT_CONTROL_CHANGE, (unsigned char) cc,
                      (unsigned char) val, false );
    m_seq->set_dirty();

    // immediate audible feedback: route the CC straight to the track VST now.
    if ( PatchKnob::app::audio_app_running() )
    {
        unsigned char status = 0xB0 | ( m_seq->get_midi_channel() & 0x0F );
        PatchKnob::app::audio_app_route_midi( vst_track(), status,
                                          (unsigned char) cc,
                                          (unsigned char) val );
    }
}

bool
TrackerView::read_fx_value( int row, int track, int fi, int* val )
{
    if ( track < 0 || track >= (int) m_fx_bind.size() ||
         fi < 0 || fi >= (int) m_fx_bind[track].size() )
        return false;

    FxBinding& b = m_fx_bind[track][fi];
    std::map<int,int>& mp = m_fx_vst[track][fi];
    std::map<int,int>::iterator it = mp.find( row );
    if ( it != mp.end() )
    {
        if ( val ) *val = clamp_int( it->second, 0, 0xffff );
        return true;
    }

    if ( b.type == FX_MIDI_CC )
    {
        int cc7 = 0;
        if ( read_cc_at( row, b.cc, &cc7 ) )
        {
            if ( val ) *val = ( cc7 * 65535 + 63 ) / 127;
            return true;
        }
    }

    return false;
}

// Route ONE FX-column value to its target (patch plugin param / rack param /
// track instrument param).  Free + stateless so the engine-side pattern-FX driver
// (play_pattern_fx) and the view both use it identically.
static void route_binding( const FxBinding& binding, int track, int val, int column )
{
    if ( binding.type != FX_VST_PARAM || !PatchKnob::app::audio_app_running() )
        return;
    val = clamp_int( val, 0, 0xffff );
    float norm = (float) val / 65535.0f;
    if ( binding.target == FX_TARGET_PATCH_PLUGIN_PARAM )
    {
        // Per-column: this FX column belongs to note-column `column`, so drive only
        // that column's voices (built-in sampler); other plugins ignore the column.
        PatchKnob::app::audio_app_patch_route_param_column( binding.node, binding.pid, norm, column );
    }
    else if ( binding.target == FX_TARGET_RACK_PARAM )
    {
        rackx::RackEngine* eng = PatchKnob::app::audio_app_rack_engine( binding.node );
        if ( eng )
        {
            float lo = binding.min_value;
            float hi = binding.max_value;
            float value = ( hi != lo ) ? lo + ( hi - lo ) * norm : norm;
            if ( hi > lo ) value = clamp_float( value, lo, hi );
            else if ( lo > hi ) value = clamp_float( value, hi, lo );
            eng->setParam( binding.module, (int) binding.pid, value );
        }
    }
    else
    {
        PatchKnob::app::audio_app_route_param( track, binding.pid, norm );
    }
}

void
TrackerView::route_fx_value( const FxBinding& binding, int track, int val, int column )
{
    route_binding( binding, track, val, column );
}

// Engine-driven pattern-FX playback: fire a sequence's VST-param FX for the rows
// crossed in (lastRow -> curRow] (inclusive, loop-wrapping), reading its committed
// fx_blob.  STATIC so the shell can drive EVERY active pattern's automation, not
// just the one shown in the tracker window -- fixes: FX going silent when the
// window is closed/unfocused, and FX not playing for other patterns on the track.
// (CC-bound FX columns are already real events in the sequence and play themselves.)
void
TrackerView::play_pattern_fx( sequence* s, int lastRow, int curRow, int nrows )
{
    if ( !s || nrows < 1 || !PatchKnob::app::audio_app_running() )
        return;
    const std::string blob = s->get_fx_blob();
    if ( blob.empty() )
        return;
    BlobR r( blob );
    int cols = (int) r.u8();
    if ( !r.ok || cols < 1 || cols > 16 )
        return;
    const int vstTrack = s->get_midi_bus();
    std::vector< std::vector<FxBinding> >          bind( 8 );
    std::vector< std::vector< std::map<int,int> > > vals( 8 );
    for ( int t = 0; t < 8; ++t )
    {
        bind[t].assign( cols, FxBinding() );
        vals[t].assign( cols, std::map<int,int>() );
        for ( int fi = 0; fi < cols && r.ok; ++fi )
        {
            FxBinding& bd = bind[t][fi];
            bd.type = r.i32();   bd.target = r.i32();
            bd.cc   = r.i32();   bd.pid    = (unsigned) r.i32();
            bd.node = r.i32();   bd.module = r.i32();
            bd.min_value = r.f32(); bd.max_value = r.f32();
            bd.label = r.str();  bd.name = r.str();
            unsigned cnt = r.u32();
            for ( unsigned k = 0; k < cnt && r.ok; ++k )
            { int row = r.i32(); int v = r.i32(); if ( r.ok ) vals[t][fi][row] = v; }
        }
    }
    if ( !r.ok )
        return;
    // Tag the instrument's note-columns from the persisted lane trailer so that
    // per-column FX (above) targets the voices those notes trigger.  The FX target
    // node IS the track's instrument; harmless (no-op) for non-sampler plugins.
    {
        int samplerNode = -1;
        for ( int t = 0; t < 8 && samplerNode < 0; ++t )
            for ( int fi = 0; fi < cols; ++fi )
                if ( bind[t][fi].type == FX_VST_PARAM &&
                     bind[t][fi].target == FX_TARGET_PATCH_PLUGIN_PARAM )
                    { samplerNode = bind[t][fi].node; break; }
        if ( samplerNode >= 0 && r.pos < r.n )
        {
            unsigned lc = r.u32();
            for ( unsigned k = 0; k < lc && r.ok; ++k )
            {
                long ts = (long) r.i32(); int nt = r.i32(); int oc = r.i32(); int ln = r.i32();
                (void) ts; (void) oc;
                if ( r.ok )
                    PatchKnob::app::audio_app_patch_sampler_set_note_column( samplerNode, nt, ln );
            }
        }
    }
    auto fireRow = [&]( int row )
    {
        for ( int t = 0; t < 8; ++t )
            for ( int fi = 0; fi < cols; ++fi )
            {
                if ( bind[t][fi].type != FX_VST_PARAM ) continue;
                std::map<int,int>::iterator it = vals[t][fi].find( row );
                if ( it != vals[t][fi].end() ) route_binding( bind[t][fi], vstTrack, it->second, t );
            }
    };
    if ( lastRow < 0 ) { fireRow( ( ( curRow % nrows ) + nrows ) % nrows ); return; }
    int rr = lastRow, guard = 0;
    while ( rr != curRow && ++guard <= nrows ) { rr = ( rr + 1 ) % nrows; fireRow( rr ); }
}

void
TrackerView::set_fx_value_at( int row, int track, int fi, int val, bool push_undo )
{
    if ( !m_seq || track < 0 || track >= (int) m_fx_bind.size() ||
         fi < 0 || fi >= (int) m_fx_bind[track].size() )
        return;

    FxBinding& b = m_fx_bind[track][fi];
    if ( b.type == FX_NONE )
        return;

    val = clamp_int( val, 0, 0xffff );
    long ts = row_start_tick( row );

    if ( b.type == FX_MIDI_CC )
    {
        // MIDI-CC values are stored ONLY as real CC events in the sequence (which
        // are per-pattern + saved).  Do NOT shadow them in m_fx_vst -- a stale
        // shadow made read_fx_value/copy/interpolate return the wrong value.
        if ( push_undo )
            m_seq->push_undo();
        int cc7 = ( val * 127 + 32767 ) / 65535;
        set_cc_at( ts, b.cc, cc7 );
    }
    else if ( b.type == FX_VST_PARAM )
    {
        if ( push_undo )
            m_seq->push_undo();
        m_fx_vst[track][fi][row] = val;   // shadow VST-param values (no host event)
        route_fx_value( b, vst_track(), val, track );   // `track` is the note-column
        m_seq->set_dirty();
    }
}

// Type a hex value into the FX cell under the cursor.  Callers nibble-shift
// the previous value so pressing "1".."F" builds 0x0001..0xFFFF.
void
TrackerView::set_fx_at_cell( int val )
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 || fi >= m_fx_cols )
        return;

    set_fx_value_at( m_cursor_row, m_cursor_track, fi, val & 0xffff, true );
}

void
TrackerView::clear_fx_cell( void )
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 || fi >= m_fx_cols )
        return;

    FxBinding& b = m_fx_bind[m_cursor_track][fi];
    if ( b.type == FX_MIDI_CC )
    {
        m_seq->push_undo();
        remove_cc_at( row_start_tick( m_cursor_row ), b.cc );
        m_fx_vst[m_cursor_track][fi].erase( m_cursor_row );
    }
    else if ( b.type == FX_VST_PARAM )
    {
        m_seq->push_undo();
        m_fx_vst[m_cursor_track][fi].erase( m_cursor_row );
        m_seq->set_dirty();
    }
}

// fire every VST-param FX cell that lives on `row` (from the playhead watcher).
// CC cells are already sequence events, so they play themselves.
void
TrackerView::fire_fx_row( int row )
{
    if ( !PatchKnob::app::audio_app_running() )
        return;

    int track = vst_track();
    for ( int t = 0; t < m_num_tracks; ++t )
    {
        for ( int f = 0; f < m_fx_cols; ++f )
        {
            FxBinding& b = m_fx_bind[t][f];
            if ( b.type != FX_VST_PARAM )
                continue;
            std::map<int,int>& mp = m_fx_vst[t][f];
            std::map<int,int>::iterator it = mp.find( row );
            if ( it != mp.end() )
                route_fx_value( b, track, it->second, t );   // `t` is the note-column
        }
    }
}

//----------------------------------------------------------------------------
//  FX bindings (driven by a toolbar picker in the shell)
//----------------------------------------------------------------------------
void
TrackerView::bind_fx_none( void )
{
    if ( m_fx_cols <= 0 ) return;               // no FX column to bind
    int fi = cur_fx_index();
    m_fx_bind[m_cursor_track][fi] = FxBinding();
    m_fx_vst[m_cursor_track][fi].clear();
}

void
TrackerView::bind_fx_cc( int cc )
{
    if ( m_fx_cols <= 0 ) return;               // no FX column to bind
    int fi = cur_fx_index();
    FxBinding& b = m_fx_bind[m_cursor_track][fi];
    b.type = FX_MIDI_CC;
    b.target = FX_TARGET_TRACK_PARAM;
    b.cc   = cc;
    b.pid  = 0;
    b.node = -1;
    b.module = -1;
    b.min_value = 0.0f;
    b.max_value = 1.0f;
    char lb[8];
    snprintf( lb, sizeof(lb), "C%02d", cc );
    b.label = lb;
    char nm[32];
    snprintf( nm, sizeof(nm), "MIDI CC %d", cc );
    b.name = nm;
    m_fx_vst[m_cursor_track][fi].clear();
}

void
TrackerView::bind_fx_vst( unsigned int pid, const std::string& name )
{
    if ( m_fx_cols <= 0 ) return;               // no FX column to bind
    int fi = cur_fx_index();
    FxBinding b;
    b.type = FX_VST_PARAM;
    b.target = FX_TARGET_TRACK_PARAM;
    b.pid  = pid;
    b.cc   = 0;
    b.label = compact_label( name, "P" );
    b.name = name;
    m_fx_vst[m_cursor_track][fi].clear();
    m_fx_bind[m_cursor_track][fi] = b;
}

void
TrackerView::bind_fx_target( const FxBinding& binding )
{
    if ( m_fx_cols <= 0 ) return;
    int fi = cur_fx_index();
    FxBinding b = binding;
    if ( b.type == FX_VST_PARAM && b.label.empty() )
        b.label = compact_label( b.name, "P" );
    if ( b.type == FX_MIDI_CC )
    {
        char lb[8];
        snprintf( lb, sizeof(lb), "C%02d", clamp_int( b.cc, 0, 127 ) );
        b.cc = clamp_int( b.cc, 0, 127 );
        b.label = lb;
    }
    m_fx_vst[m_cursor_track][fi].clear();
    m_fx_bind[m_cursor_track][fi] = b;
}

std::string
TrackerView::cur_fx_desc( void ) const
{
    if ( m_fx_cols <= 0 ) return std::string( "no fx" );
    int fi = m_cursor_col - 2;
    if ( fi < 0 ) fi = 0;
    if ( fi >= m_fx_cols ) fi = m_fx_cols - 1;
    const FxBinding& b = m_fx_bind[m_cursor_track][fi];
    char buf[32];
    if ( b.type == FX_MIDI_CC )
        snprintf( buf, sizeof(buf), "T%d.F%d CC%d", m_cursor_track, fi, b.cc );
    else if ( b.type == FX_VST_PARAM )
        snprintf( buf, sizeof(buf), "T%d.F%d %s", m_cursor_track, fi,
                  b.name.empty() ? b.label.c_str() : b.name.c_str() );
    else
        snprintf( buf, sizeof(buf), "T%d.F%d --", m_cursor_track, fi );
    return std::string( buf );
}

//----------------------------------------------------------------------------
//  tracker keyboard
//----------------------------------------------------------------------------
int
TrackerView::key_to_pitch( SDL_Keycode k, int* oct_off )
{
    *oct_off = 0;
    switch ( k )
    {
        // lower octave: Z S X D C V G B H N J M
        case SDLK_z: return 0;   // C
        case SDLK_s: return 1;   // C#
        case SDLK_x: return 2;   // D
        case SDLK_d: return 3;   // D#
        case SDLK_c: return 4;   // E
        case SDLK_v: return 5;   // F
        case SDLK_g: return 6;   // F#
        case SDLK_b: return 7;   // G
        case SDLK_h: return 8;   // G#
        case SDLK_n: return 9;   // A
        case SDLK_j: return 10;  // A#
        case SDLK_m: return 11;  // B

        // upper octave: Q 2 W 3 E R 5 T 6 Y 7 U
        case SDLK_q: *oct_off = 1; return 0;
        case SDLK_2: *oct_off = 1; return 1;
        case SDLK_w: *oct_off = 1; return 2;
        case SDLK_3: *oct_off = 1; return 3;
        case SDLK_e: *oct_off = 1; return 4;
        case SDLK_r: *oct_off = 1; return 5;
        case SDLK_5: *oct_off = 1; return 6;
        case SDLK_t: *oct_off = 1; return 7;
        case SDLK_6: *oct_off = 1; return 8;
        case SDLK_y: *oct_off = 1; return 9;
        case SDLK_7: *oct_off = 1; return 10;
        case SDLK_u: *oct_off = 1; return 11;
    }
    return -1;
}

int
TrackerView::key_to_hex( SDL_Keycode k )
{
    if ( k >= SDLK_0 && k <= SDLK_9 ) return (int)( k - SDLK_0 );
    if ( k >= SDLK_a && k <= SDLK_f ) return (int)( k - SDLK_a + 10 );
    return -1;
}

//----------------------------------------------------------------------------
//  geometry / cursor
//----------------------------------------------------------------------------
// column layout within one track, expressed in monospace character cells:
//   [ note(3) ][ sp ][ vel(2) ][ sp ][ fx0(4) ][ sp ][ fx1(4) ][ sp ] ...
void
TrackerView::subcol_geom( int col, int* cx_chars, int* w_chars ) const
{
    if ( col == 0 )      { *cx_chars = 0; *w_chars = 3; }   // note name
    else if ( col == 1 ) { *cx_chars = 4; *w_chars = 2; }   // hex velocity
    else                                                    // FX columns
    {
        int fi = col - 2;
        *cx_chars = 7 + fi * 5;
        *w_chars  = 4;
    }
}

static const int kGutterChars = 4;   // "%3d" row number + 1 space

int
TrackerView::visible_rows( App& app ) const
{
    int ch = app.mono.ch();
    int row_h    = ch + 2;
    int header_h = ch + 4;
    int ctrl_h   = ch + 4;              // the add/remove-column toolbar band
    int r = ( rect.h - header_h - ctrl_h ) / row_h;
    if ( r < 1 ) r = 1;
    return r;
}

// Which header control button (if any) contains the point.  Tested against the
// rects the last draw() cached, so it needs no knowledge of the header layout.
int
TrackerView::hdr_button_at( int px, int py ) const
{
    auto in = [&]( const SDL_Rect& q )
    { return px >= q.x && px < q.x + q.w && py >= q.y && py < q.y + q.h; };
    if ( in( m_btn_note_minus ) ) return HDR_NOTE_MINUS;
    if ( in( m_btn_note_plus  ) ) return HDR_NOTE_PLUS;
    if ( in( m_btn_fx_minus   ) ) return HDR_FX_MINUS;
    if ( in( m_btn_fx_plus    ) ) return HDR_FX_PLUS;
    if ( in( m_btn_lpb_minus  ) ) return HDR_LPB_MINUS;
    if ( in( m_btn_lpb_plus   ) ) return HDR_LPB_PLUS;
    if ( in( m_btn_oct_minus  ) ) return HDR_OCT_MINUS;
    if ( in( m_btn_oct_plus   ) ) return HDR_OCT_PLUS;
    return HDR_NONE;
}

void
TrackerView::ensure_cursor_visible( App& app )
{
    int vis = visible_rows( app );
    m_vis_rows = vis;                   // cache for poll_playhead (no App& there)
    if ( m_cursor_row < m_top_row )
        m_top_row = m_cursor_row;
    else if ( m_cursor_row >= m_top_row + vis )
        m_top_row = m_cursor_row - vis + 1;
    if ( m_top_row < 0 ) m_top_row = 0;
}

void
TrackerView::move_cursor( App& app, int drow, int dcol )
{
    if ( dcol != 0 )
    {
        int total = total_subcols();
        int maxg  = m_num_tracks * total;
        int gc    = m_cursor_track * total + m_cursor_col + dcol;
        while ( gc < 0 )     gc += maxg;
        while ( gc >= maxg ) gc -= maxg;
        m_cursor_track = gc / total;
        m_cursor_col   = gc % total;
    }

    m_cursor_row += drow;
    if ( m_cursor_row < 0 ) m_cursor_row = 0;
    if ( m_cursor_row >= num_rows() ) m_cursor_row = num_rows() - 1;

    ensure_cursor_visible( app );
    app.request_redraw();
}

//----------------------------------------------------------------------------
//  toolbar-equivalent setters
//----------------------------------------------------------------------------
void
TrackerView::set_lines_per_beat( int lpb )
{
    if ( lpb_index( lpb ) < 0 )
        return;
    m_rows_per_beat = lpb;
    m_cursor_row = 0;
    m_top_row = 0;
}

void
TrackerView::set_rows_per_beat( int rpb )
{
    set_lines_per_beat( rpb );
}

void
TrackerView::set_num_note_cols( int n )
{
    if ( n < 1 ) n = 1;
    if ( n > 8 ) n = 8;
    m_num_tracks = n;
    if ( m_cursor_track >= m_num_tracks )
        m_cursor_track = m_num_tracks - 1;
    if ( m_sel_track >= m_num_tracks || m_sel_track1 >= m_num_tracks )
        m_sel_active = false;
}

void
TrackerView::set_octave( int o )
{
    if ( o < 0 ) o = 0;
    if ( o > 8 ) o = 8;
    m_octave = o;
}

void
TrackerView::set_edit_step( int s )
{
    if ( s < 1 ) s = 1;
    if ( s > 8 ) s = 8;
    m_edit_step = s;
}

//----------------------------------------------------------------------------
//  FX-column count  (add / subtract columns)
//----------------------------------------------------------------------------
// Grow the FX-column count and widen EVERY note-column's binding + value store
// so the new indexes are valid before draw()/set_fx_at_cell ever touch them.
void
TrackerView::add_fx_col( void )
{
    if ( m_fx_cols >= 8 )
        return;
    ++m_fx_cols;
    for ( int t = 0; t < 8; ++t )
    {
        m_fx_bind[t].resize( m_fx_cols );   // new FxBinding() defaults to FX_NONE
        m_fx_vst[t].resize( m_fx_cols );
    }
}

// Shrink the FX-column count, dropping the trailing column's data, and pull the
// cursor / any live selection back inside the new column count.
void
TrackerView::remove_fx_col( void )
{
    if ( m_fx_cols <= 0 )
        return;
    --m_fx_cols;
    for ( int t = 0; t < 8; ++t )
    {
        m_fx_bind[t].resize( m_fx_cols );
        m_fx_vst[t].resize( m_fx_cols );
    }
    if ( m_cursor_col >= total_subcols() )
        m_cursor_col = total_subcols() - 1;
    if ( m_sel_col >= total_subcols() || m_sel_col1 >= total_subcols() )
        m_sel_active = false;               // block referenced a dropped column
}

//----------------------------------------------------------------------------
//  selection editing: copy / paste (shared note clipboard) + interpolate
//----------------------------------------------------------------------------
// Copy a rectangular tracker cell block.  This preserves sustaining note-ons
// and 16-bit FX cells, unlike the shared sequence clipboard which only knows
// linked note pairs.
void
TrackerView::copy_selection( void )
{
    if ( !m_seq )
        return;
    int r0, r1, t0, t1, c0, c1;
    selection_region( &r0, &r1, &t0, &t1, &c0, &c1 );

    m_cell_clipboard.clear();
    std::vector<NoteCell> nl, offl;
    for ( int r = r0; r <= r1; ++r )
    {
        collect_row_notes( r, nl );
        collect_row_note_offs( r, offl );
        for ( int t = t0; t <= t1; ++t )
            for ( int c = c0; c <= c1; ++c )
            {
                ClipCell cell;
                cell.dr = r - r0;
                cell.dt = t - t0;
                cell.dc = c - c0;
                if ( c == 0 )
                {
                    if ( t < (int) nl.size() && nl[t].note >= 0 )
                    {
                        cell.kind = CLIP_NOTE;
                        cell.note = nl[t].note;
                        cell.vel  = nl[t].vel;
                        m_cell_clipboard.push_back( cell );
                    }
                    else if ( t < (int) offl.size() && offl[t].note >= 0 )
                    {
                        cell.kind = CLIP_OFF;
                        cell.note = offl[t].note;
                        cell.vel  = offl[t].vel;
                        m_cell_clipboard.push_back( cell );
                    }
                }
                else if ( c == 1 )
                {
                    if ( t < (int) nl.size() && nl[t].note >= 0 )
                    {
                        cell.kind = CLIP_VEL;
                        cell.vel  = nl[t].vel;
                        m_cell_clipboard.push_back( cell );
                    }
                }
                else
                {
                    int val = 0;
                    int fi = c - 2;
                    if ( read_fx_value( r, t, fi, &val ) )
                    {
                        cell.kind = CLIP_FX;
                        cell.value = val;
                        m_cell_clipboard.push_back( cell );
                    }
                }
            }
    }
    m_copied = !m_cell_clipboard.empty();
}

void
TrackerView::cut_selection( void )
{
    if ( !m_seq )
        return;
    copy_selection();
    if ( !m_cell_clipboard.empty() )
        clear_selection_cells( true );
}

// Paste the clipboard so its earliest note lands on the cursor row, keeping the
// original pitches (paste offset note == clipboard's highest note => 0 shift).
// Guarded by the real shared clipboard contents so piano-roll copies can paste
// here too (and an old per-view flag cannot point at an empty buffer).
void
TrackerView::paste_at_cursor( void )
{
    if ( !m_seq )
        return;

    if ( !m_cell_clipboard.empty() )
    {
        m_seq->push_undo();
        int old_row = m_cursor_row;
        int old_track = m_cursor_track;
        int old_col = m_cursor_col;

        for ( std::size_t i = 0; i < m_cell_clipboard.size(); ++i )
        {
            const ClipCell& cell = m_cell_clipboard[i];
            int row = old_row + cell.dr;
            int track = old_track + cell.dt;
            int col = old_col + cell.dc;
            if ( row < 0 || row >= num_rows() ||
                 track < 0 || track >= m_num_tracks ||
                 col < 0 || col >= total_subcols() )
                continue;

            if ( cell.kind == CLIP_NOTE && col == 0 )
            {
                std::vector<NoteCell> nl;
                collect_row_notes( row, nl );
                if ( track < (int) nl.size() && nl[track].note >= 0 )
                {
                    clear_explicit_note_off( nl[track].ts, nl[track].note,
                                             nl[track].occurrence );
                    clear_note_lane( nl[track].ts, nl[track].note,
                                     nl[track].occurrence );
                    remove_specific_note( nl[track] );
                }
                else
                {
                    // Pasting over a visible OFF: drop the lingering note-off partner
                    // and restore its note as sustaining, else the pasted note-on lands
                    // on the same tick as that OFF and verify_and_link kills it instantly
                    // (mirrors set_note_at_cell).
                    std::vector<NoteCell> offl;
                    collect_row_note_offs( row, offl );
                    if ( track < (int) offl.size() && offl[track].note >= 0 )
                    {
                        remove_specific_note( offl[track] );
                        add_note_on( offl[track].ts, offl[track].note, offl[track].vel );
                        clear_explicit_note_off( offl[track].ts, offl[track].note,
                                                 offl[track].occurrence );
                    }
                }
                end_unreleased_note_before( row, track, row_start_tick( row ) );
                if ( add_note_on( row_start_tick( row ), cell.note, cell.vel ) )
                    set_note_lane( row_start_tick( row ), cell.note,
                                   note_occurrence_count( row_start_tick( row ), cell.note ) - 1,
                                   track );
            }
            else if ( cell.kind == CLIP_OFF && col == 0 )
            {
                long rs = row_start_tick( row );
                NoteCell nc;
                bool found = note_before_row_in_column( row, track, &nc );
                if ( !found )
                {
                    std::vector<NoteCell> sl;
                    collect_sounding_notes( row, sl );
                    if ( track < (int) sl.size() && sl[track].note >= 0 )
                    {
                        nc = sl[track];
                        found = true;
                    }
                }
                if ( found && rs > nc.ts )
                {
                    remove_specific_note( nc );
                    if ( add_note_pair( nc.ts, rs, nc.note, nc.vel ) )
                        set_explicit_note_off( nc.ts, nc.note, nc.occurrence, rs, track );
                }
            }
            else if ( cell.kind == CLIP_VEL && col == 1 )
            {
                std::vector<NoteCell> nl;
                collect_row_notes( row, nl );
                if ( track < (int) nl.size() && nl[track].note >= 0 )
                {
                    NoteCell nc = nl[track];
                    long explicit_off = 0;
                    bool was_explicit = has_explicit_note_off( nc.ts, nc.note, nc.occurrence, &explicit_off );
                    remove_specific_note( nc );
                    add_note_cell( nc.ts, nc.tf, nc.note, cell.vel, nc.has_off );
                    if ( was_explicit )
                        set_explicit_note_off( nc.ts, nc.note, nc.occurrence, explicit_off, track );
                    set_note_lane( nc.ts, nc.note, nc.occurrence, track );
                }
            }
            else if ( cell.kind == CLIP_FX && col >= 2 )
            {
                set_fx_value_at( row, track, col - 2, cell.value, false );
            }
        }

        m_cursor_row = old_row;
        m_cursor_track = old_track;
        m_cursor_col = old_col;
        m_seq->verify_and_link();
        m_seq->set_dirty();
        return;
    }

    long cts, ctf;
    int  cnh, cnl;
    m_seq->get_clipboard_box( &cts, &cnh, &ctf, &cnl );
    if ( cts == 0 && ctf == 0 && cnh == 0 && cnl == 0 )
        return;
    m_seq->push_undo();
    m_seq->paste_selected( row_start_tick( m_cursor_row ), cnh );
    m_seq->set_dirty();
}

// Linearly interpolate one FX column's values across the selected row span: the
// first & last selected rows must already hold values; every row in between is
// filled by lerping first->last.  Only FX-value data for the selected column is
// touched (CC events for a MIDI-CC binding, the per-view map for a VST param).
void
TrackerView::interpolate_selection( void )
{
    if ( !m_seq || !m_sel_active || !selection_is_single_column() || m_sel_col < 2 )
        return;                                   // need an FX-column row range
    int fi = m_sel_col - 2;
    if ( fi < 0 || fi >= m_fx_cols )
        return;
    int track = m_sel_track;
    if ( track < 0 || track >= m_num_tracks )
        return;
    int r0 = m_sel_row0, r1 = m_sel_row1;
    if ( r1 - r0 < 2 )
        return;                                   // no rows between the endpoints

    // read the two endpoint values for this binding kind.
    int  v0 = 0, v1 = 0;
    bool h0 = read_fx_value( r0, track, fi, &v0 );
    bool h1 = read_fx_value( r1, track, fi, &v1 );
    if ( !h0 || !h1 )
        return;                                   // both endpoints must hold a value

    int span = r1 - r0;
    m_seq->push_undo();                            // one undo step for the fill

    for ( int r = r0 + 1; r < r1; ++r )
    {
        double f   = double( r - r0 ) / double( span );
        int    val = int( v0 + ( v1 - v0 ) * f + 0.5 );   // rounded lerp
        set_fx_value_at( r, track, fi, val, false );
    }
    m_seq->set_dirty();
}

//----------------------------------------------------------------------------
//  extended editing features
//----------------------------------------------------------------------------
// gather EVERY note-on in the pattern (start tick, end tick, pitch, velocity),
// used by the row insert / delete shifters which rebuild the note list.
void
TrackerView::collect_all_notes( std::vector<NoteCell>& out )
{
    out.clear();
    if ( !m_seq )
        return;
    long tick_s, tick_f;
    int  note, vel;
    bool selected;

    int order = 0;
    std::map< std::pair<long,int>, int > occurrences;
    m_seq->reset_draw_marker();
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        int occurrence = occurrences[ std::make_pair( tick_s, note ) ]++;
        NoteCell nc;
        nc.note = note; nc.vel = vel; nc.ts = tick_s;
        nc.order = order;
        nc.occurrence = occurrence;
        nc.has_off = tick_f > tick_s;
        nc.tf = normalized_note_end( tick_s, tick_f );
        long explicit_off = 0;
        if ( has_explicit_note_off( tick_s, note, occurrence, &explicit_off ) )
        {
            nc.has_off = true;
            nc.tf = explicit_off;
        }
        out.push_back( nc );
        ++order;
    }
}

// gather notes that are SUSTAINING across `row`'s start tick (started earlier,
// end later), preserving tracker lanes.  Pitch-sorted packing made OFF/paste
// operations in column N sometimes shorten column 1's note.
void
TrackerView::collect_sounding_notes( int row, std::vector<NoteCell>& out )
{
    out.clear();
    if ( !m_seq )
        return;
    NoteCell empty;
    out.assign( m_num_tracks, empty );

    long rs = row_start_tick( row );
    long tick_s, tick_f;
    int  note, vel;
    bool selected;

    int order = 0;
    std::map< std::pair<long,int>, int > occurrences;
    m_seq->reset_draw_marker();
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        int occurrence = occurrences[ std::make_pair( tick_s, note ) ]++;
        bool has_off = tick_f > tick_s;
        long nf = normalized_note_end( tick_s, tick_f );
        long explicit_off = 0;
        if ( has_explicit_note_off( tick_s, note, occurrence, &explicit_off ) )
        {
            has_off = true;
            nf = explicit_off;
        }
        if ( tick_s < rs && nf > rs )
        {
            NoteCell nc;
            nc.note = note; nc.vel = vel; nc.ts = tick_s; nc.tf = nf;
            nc.has_off = has_off;
            nc.order = order;
            nc.occurrence = occurrence;
            int lane = assigned_note_lane( tick_s, note, occurrence );
            if ( lane >= 0 && lane < m_num_tracks && out[lane].note < 0 )
            {
                out[lane] = nc;
            }
            else
            {
                for ( int i = 0; i < m_num_tracks; ++i )
                    if ( out[i].note < 0 )
                    {
                        out[i] = nc;
                        break;
                    }
            }
        }
        ++order;
    }
}

// The editing "block": the marked selection's row span + note-column, or -- when
// nothing is marked -- the single cursor cell.
void
TrackerView::block_region( int* r0, int* r1, int* track ) const
{
    if ( m_sel_active )
    {
        *r0 = m_sel_row0; *r1 = m_sel_row1; *track = m_sel_track;
    }
    else
    {
        *r0 = *r1 = m_cursor_row; *track = m_cursor_track;
    }
}

void
TrackerView::selection_region( int* r0, int* r1, int* t0, int* t1,
                               int* c0, int* c1 ) const
{
    if ( m_sel_active )
    {
        *r0 = m_sel_row0;   *r1 = m_sel_row1;
        *t0 = m_sel_track0; *t1 = m_sel_track1;
        *c0 = m_sel_col0;   *c1 = m_sel_col1;
    }
    else
    {
        *r0 = *r1 = m_cursor_row;
        *t0 = *t1 = m_cursor_track;
        *c0 = *c1 = m_cursor_col;
    }

    if ( *r0 < 0 ) *r0 = 0;
    if ( *r1 >= num_rows() ) *r1 = num_rows() - 1;
    if ( *t0 < 0 ) *t0 = 0;
    if ( *t1 >= m_num_tracks ) *t1 = m_num_tracks - 1;
    if ( *c0 < 0 ) *c0 = 0;
    if ( *c1 >= total_subcols() ) *c1 = total_subcols() - 1;
}

bool
TrackerView::selection_is_single_column( void ) const
{
    if ( !m_sel_active )
        return true;
    return m_sel_track0 == m_sel_track1 && m_sel_col0 == m_sel_col1;
}

void
TrackerView::select_all_cells( void )
{
    m_sel_active = true;
    m_sel_drag = false;
    m_sel_row0 = 0;
    m_sel_row1 = num_rows() - 1;
    m_sel_track0 = 0;
    m_sel_track1 = m_num_tracks - 1;
    m_sel_col0 = 0;
    m_sel_col1 = total_subcols() - 1;
    m_sel_track = m_cursor_track;
    m_sel_col = m_cursor_col;
}

void
TrackerView::clear_selection_cells( bool push_undo )
{
    if ( !m_seq )
        return;

    int r0, r1, t0, t1, c0, c1;
    selection_region( &r0, &r1, &t0, &t1, &c0, &c1 );
    if ( r0 > r1 || t0 > t1 || c0 > c1 )
        return;

    if ( push_undo )
        m_seq->push_undo();

    std::vector<NoteCell> to_remove;
    std::map< std::pair<int,int>, bool > restore_off_rows;
    std::vector<NoteCell> nl, offl;

    for ( int r = r0; r <= r1; ++r )
    {
        collect_row_notes( r, nl );
        collect_row_note_offs( r, offl );
        for ( int t = t0; t <= t1; ++t )
        {
            if ( c0 <= 0 && c1 >= 0 )
            {
                if ( t >= 0 && t < (int) nl.size() && nl[t].note >= 0 )
                    to_remove.push_back( nl[t] );
                if ( t >= 0 && t < (int) offl.size() && offl[t].note >= 0 )
                    restore_off_rows[ std::make_pair( r, t ) ] = true;
            }
        }
    }

    // Remove same-(ts,note) duplicates HIGHEST occurrence first: remove_note_at
    // counts occurrences live, so deleting occurrence 0 first would renumber the
    // survivor from 1->0 and the later "occurrence 1" delete would match nothing,
    // leaving a duplicate behind.  Descending order keeps lower indices valid.
    std::stable_sort( to_remove.begin(), to_remove.end(),
        []( const NoteCell& a, const NoteCell& b ) {
            if ( a.ts != b.ts )     return a.ts < b.ts;
            if ( a.note != b.note ) return a.note < b.note;
            return a.occurrence > b.occurrence;
        } );
    for ( size_t i = 0; i < to_remove.size(); ++i )
    {
        clear_explicit_note_off( to_remove[i].ts, to_remove[i].note,
                                 to_remove[i].occurrence );
        clear_note_lane( to_remove[i].ts, to_remove[i].note,
                         to_remove[i].occurrence );
        remove_specific_note( to_remove[i] );
    }

    for ( std::map< std::pair<int,int>, bool >::const_iterator it =
              restore_off_rows.begin(); it != restore_off_rows.end(); ++it )
    {
        collect_row_note_offs( it->first.first, offl );
        int track = it->first.second;
        if ( track >= 0 && track < (int) offl.size() && offl[track].note >= 0 )
        {
            remove_specific_note( offl[track] );
            if ( add_note_on( offl[track].ts, offl[track].note, offl[track].vel ) )
                set_note_lane( offl[track].ts, offl[track].note,
                               note_occurrence_count( offl[track].ts, offl[track].note ) - 1,
                               track );
            clear_explicit_note_off( offl[track].ts, offl[track].note,
                                     offl[track].occurrence );
        }
    }

    if ( c1 >= 1 && c0 <= 1 && !( c0 <= 0 && c1 >= 0 ) )
    {
        std::vector<NoteCell> tgt;
        std::vector<int> vel;
        for ( int r = r0; r <= r1; ++r )
        {
            collect_row_notes( r, nl );
            for ( int t = t0; t <= t1; ++t )
                if ( t >= 0 && t < (int) nl.size() && nl[t].note >= 0 )
                {
                    tgt.push_back( nl[t] );
                    vel.push_back( 1 );
                }
        }
        if ( !tgt.empty() )
            apply_velocities( tgt, vel );
    }

    if ( c1 >= 2 )
    {
        for ( int r = r0; r <= r1; ++r )
            for ( int t = t0; t <= t1; ++t )
                for ( int c = std::max( c0, 2 ); c <= c1; ++c )
                {
                    int fi = c - 2;
                    if ( t < 0 || t >= m_num_tracks || fi < 0 || fi >= m_fx_cols )
                        continue;
                    FxBinding& b = m_fx_bind[t][fi];
                    if ( b.type == FX_MIDI_CC )
                        remove_cc_at( row_start_tick( r ), b.cc );
                    m_fx_vst[t][fi].erase( r );
                }
    }

    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Re-velocity a set of existing notes: remove them all (keeps note/off links
// consistent between removals), then re-add every one with its new velocity and
// re-link once.  Caller must push_undo() first (only when tgt is non-empty).
void
TrackerView::apply_velocities( const std::vector<NoteCell>& tgt,
                               const std::vector<int>& vel )
{
    std::map< NoteKey, ExplicitNoteOff > explicit_offs = m_explicit_note_offs;
    for ( size_t i = 0; i < tgt.size(); ++i )
        remove_specific_note( tgt[i] );

    for ( size_t i = 0; i < tgt.size(); ++i )
    {
        int v = vel[i];
        if ( v < 1 )   v = 1;
        if ( v > 127 ) v = 127;
        if ( !add_note_cell( tgt[i].ts, tgt[i].tf, tgt[i].note, v, tgt[i].has_off ) )
            add_note_cell( tgt[i].ts, tgt[i].tf, tgt[i].note, tgt[i].vel, tgt[i].has_off );
        NoteKey key = note_key( tgt[i].ts, tgt[i].note, tgt[i].occurrence );
        int lane = assigned_note_lane( tgt[i].ts, tgt[i].note, tgt[i].occurrence );
        if ( lane >= 0 )
            set_note_lane( tgt[i].ts, tgt[i].note, tgt[i].occurrence, lane );
        std::map< NoteKey, ExplicitNoteOff >::iterator it = explicit_offs.find( key );
        if ( it != explicit_offs.end() )
            m_explicit_note_offs[key] = it->second;
    }
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Transpose the block's note-column notes (or the cursor note) by delta_note
// semitones.  Rebuild so explicit OFF markers follow the changed pitch.
void
TrackerView::transpose_selection( int delta_note )
{
    int r0, r1, t0, t1, c0, c1;
    selection_region( &r0, &r1, &t0, &t1, &c0, &c1 );   // honor ALL selected columns

    std::vector<NoteCell> tgt, nl;
    for ( int r = r0; r <= r1; ++r )
    {
        collect_row_notes( r, nl );
        for ( int t = t0; t <= t1; ++t )
            if ( t >= 0 && t < (int) nl.size() && nl[t].note >= 0 )
                tgt.push_back( nl[t] );
    }
    if ( tgt.empty() )
        return;
    // remove/re-add highest-occurrence-first so same-pitch duplicates don't strand
    std::stable_sort( tgt.begin(), tgt.end(),
        []( const NoteCell& a, const NoteCell& b ) {
            if ( a.ts != b.ts )     return a.ts < b.ts;
            if ( a.note != b.note ) return a.note < b.note;
            return a.occurrence > b.occurrence;
        } );

    m_seq->push_undo();
    std::map< NoteKey, ExplicitNoteOff > moved_offs = m_explicit_note_offs;
    std::map< NoteKey, int > moved_lanes = m_note_lanes;
    for ( size_t i = 0; i < tgt.size(); ++i )
        remove_specific_note( tgt[i] );

    for ( size_t i = 0; i < tgt.size(); ++i )
    {
        int note = tgt[i].note + delta_note;
        if ( note < 0 )   note = 0;
        if ( note > 127 ) note = 127;
        NoteKey old_key = note_key( tgt[i].ts, tgt[i].note, tgt[i].occurrence );
        std::map< NoteKey, ExplicitNoteOff >::iterator it = moved_offs.find( old_key );
        std::map< NoteKey, int >::iterator lane_it = moved_lanes.find( old_key );
        if ( add_note_cell( tgt[i].ts, tgt[i].tf, note, tgt[i].vel, tgt[i].has_off ) )
        {
            int occurrence = note_occurrence_count( tgt[i].ts, note ) - 1;
            if ( it != moved_offs.end() )
            {
                ExplicitNoteOff off = it->second;
                moved_offs.erase( it );
                moved_offs[ note_key( tgt[i].ts, note, occurrence ) ] = off;
            }
            if ( lane_it != moved_lanes.end() )
            {
                int lane = lane_it->second;
                moved_lanes.erase( lane_it );
                moved_lanes[ note_key( tgt[i].ts, note, occurrence ) ] = lane;
            }
        }
    }
    m_explicit_note_offs.swap( moved_offs );
    m_note_lanes.swap( moved_lanes );
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Note-off: this engine stores offs as linked partners of note-ons. Backtick
// makes the OFF visible and moves that linked partner to the cursor row. If the
// note already ended earlier, the nearest previous note in this visual column is
// extended forward to the cursor row.
void
TrackerView::note_off_at_cell( void )
{
    long rs = row_start_tick( m_cursor_row );
    NoteCell nc;
    // A tracker column refers to its most recent note entry, not the
    // pitch-sorted list of every held voice.  Picking the latter made a later
    // same-pitch note close an older note and caused OFF markers to jump lanes.
    bool found = note_before_row_in_column( m_cursor_row, m_cursor_track, &nc );
    if ( !found )
    {
        std::vector<NoteCell> sl;
        collect_sounding_notes( m_cursor_row, sl );
        if ( m_cursor_track >= 0 && m_cursor_track < (int) sl.size() &&
             sl[m_cursor_track].note >= 0 )
        {
            nc = sl[m_cursor_track];
            found = true;
        }
    }
    if ( !found )
        return;
    if ( rs <= nc.ts )
        return;                         // zero-length result: refuse

    if ( nc.tf == rs )
    {
        m_seq->push_undo();
        set_explicit_note_off( nc.ts, nc.note, nc.occurrence, rs, m_cursor_track );
        m_seq->set_dirty();
        return;
    }

    m_seq->push_undo();
    remove_specific_note( nc );
    if ( add_note_pair( nc.ts, rs, nc.note, nc.vel ) )
        set_explicit_note_off( nc.ts, nc.note, nc.occurrence, rs, m_cursor_track );
    else if ( add_note_on( nc.ts, nc.note, nc.vel ) )
        set_note_lane( nc.ts, nc.note,
                       note_occurrence_count( nc.ts, nc.note ) - 1,
                       m_cursor_track );
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Linear interpolate the VELOCITY of the note-column's notes across the marked
// row span (endpoints must both hold a note; in-between rows that hold a note in
// the same column are ramped).  Mirrors interpolate_selection() for FX values.
void
TrackerView::interpolate_velocity( void )
{
    if ( !m_seq || !m_sel_active )
        return;
    int r0 = m_sel_row0, r1 = m_sel_row1, track = m_sel_track;
    if ( r1 - r0 < 2 || track < 0 || track >= m_num_tracks )
        return;

    int n0, v0, n1, v1;
    if ( !note_at_row( r0, track, &n0, &v0 ) ) return;
    if ( !note_at_row( r1, track, &n1, &v1 ) ) return;

    std::vector<NoteCell> tgt;
    std::vector<int>      nv;
    std::vector<NoteCell> nl;
    int span = r1 - r0;
    for ( int r = r0 + 1; r < r1; ++r )
    {
        collect_row_notes( r, nl );
        if ( track < (int) nl.size() && nl[track].note >= 0 )
        {
            double f   = double( r - r0 ) / double( span );
            int    val = int( v0 + ( v1 - v0 ) * f + 0.5 );
            tgt.push_back( nl[track] );
            nv.push_back( val );
        }
    }
    if ( tgt.empty() )
        return;
    m_seq->push_undo();
    apply_velocities( tgt, nv );
}

// Clear every cell of the cursor row: all note-column notes starting in the row,
// plus each bound FX column's CC event / VST-param value in that row window.
void
TrackerView::clear_row( void )
{
    if ( !m_seq )
        return;
    long ts = row_start_tick( m_cursor_row );

    m_seq->push_undo();

    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    // Delete duplicates highest-occurrence-first (see clear_selection_cells): a
    // live occurrence recount would otherwise strand a same-pitch duplicate.
    std::vector<NoteCell> row_notes;
    for ( size_t i = 0; i < nl.size(); ++i )
        if ( nl[i].note >= 0 ) row_notes.push_back( nl[i] );
    std::stable_sort( row_notes.begin(), row_notes.end(),
        []( const NoteCell& a, const NoteCell& b ) {
            if ( a.note != b.note ) return a.note < b.note;
            return a.occurrence > b.occurrence;
        } );
    for ( size_t i = 0; i < row_notes.size(); ++i )
    {
        clear_explicit_note_off( row_notes[i].ts, row_notes[i].note, row_notes[i].occurrence );
        clear_note_lane( row_notes[i].ts, row_notes[i].note, row_notes[i].occurrence );
        remove_specific_note( row_notes[i] );
    }

    std::vector<NoteCell> offl;
    collect_row_note_offs( m_cursor_row, offl );
    for ( size_t i = 0; i < offl.size(); ++i )
        if ( offl[i].note >= 0 )
        {
            remove_specific_note( offl[i] );
            if ( add_note_on( offl[i].ts, offl[i].note, offl[i].vel ) )
                set_note_lane( offl[i].ts, offl[i].note,
                               note_occurrence_count( offl[i].ts, offl[i].note ) - 1,
                               (int) i );
            clear_explicit_note_off( offl[i].ts, offl[i].note, offl[i].occurrence );
        }

    for ( int t = 0; t < m_num_tracks; ++t )
        for ( int f = 0; f < m_fx_cols; ++f )
        {
            FxBinding& b = m_fx_bind[t][f];
            if ( b.type == FX_MIDI_CC )
                remove_cc_at( ts, b.cc );
            else if ( b.type == FX_VST_PARAM )
                m_fx_vst[t][f].erase( m_cursor_row );
        }
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Wipe the whole pattern: every event (notes AND CC), and every VST-param cell.
void
TrackerView::clear_pattern( void )
{
    if ( !m_seq )
        return;
    m_seq->push_undo();
    m_seq->select_all();
    m_seq->mark_selected();
    m_seq->remove_marked();
    m_seq->verify_and_link();
    m_seq->set_dirty();

    for ( int t = 0; t < 8; ++t )
        for ( int f = 0; f < (int) m_fx_vst[t].size(); ++f )
            m_fx_vst[t][f].clear();
    m_explicit_note_offs.clear();
    m_note_lanes.clear();
}

// Insert a blank row at the cursor: rebuild the note list, shifting every note
// that starts at or after the cursor row DOWN by one row's ticks.  VST-param FX
// shadow rows shift with the notes; notes pushed past pattern length are dropped.
void
TrackerView::insert_row( void )
{
    if ( !m_seq )
        return;
    long rs  = row_start_tick( m_cursor_row );
    long tpr = ticks_per_row();

    std::vector<NoteCell> all;
    collect_all_notes( all );

    m_seq->push_undo();
    std::map<int, std::vector<std::pair<int,int> > > cc_rows;
    for ( int t = 0; t < m_num_tracks; ++t )
        for ( int f = 0; f < m_fx_cols; ++f )
            if ( m_fx_bind[t][f].type == FX_MIDI_CC )
                cc_rows[m_fx_bind[t][f].cc];
    for ( std::map<int, std::vector<std::pair<int,int> > >::iterator cit = cc_rows.begin();
          cit != cc_rows.end(); ++cit )
    {
        for ( int row = 0; row < num_rows(); ++row )
        {
            int value = 0;
            if ( read_cc_at( row, cit->first, &value ) )
                cit->second.push_back( std::make_pair( row, value ) );
        }
    }
    m_seq->unselect();
    m_seq->select_note_events( 0, 127, m_seq->get_length(), 0,
                               sequence::e_select );
    m_seq->mark_selected();
    m_seq->remove_marked();

    std::map< NoteKey, ExplicitNoteOff > new_offs;
    std::map< NoteKey, int > new_lanes;
    long L = m_seq->get_length();
    for ( size_t i = 0; i < all.size(); ++i )
    {
        long nts = all[i].ts, ntf = all[i].tf;
        if ( all[i].ts >= rs )
        {
            nts += tpr;
            if ( all[i].has_off ) ntf += tpr;
        }
        else if ( all[i].has_off && all[i].tf >= rs )
            ntf += tpr;
        if ( all[i].has_off && ntf >= L ) ntf = L - 1;
        if ( nts >= L )
            continue;
        if ( add_note_cell( nts, ntf, all[i].note, all[i].vel, all[i].has_off ) )
        {
            NoteKey key = note_key( all[i].ts, all[i].note, all[i].occurrence );
            NoteKey new_key = note_key( nts, all[i].note,
                                        note_occurrence_count( nts, all[i].note ) - 1 );
            std::map< NoteKey, ExplicitNoteOff >::const_iterator it =
                m_explicit_note_offs.find( key );
            if ( it != m_explicit_note_offs.end() )
            {
                ExplicitNoteOff off = it->second;
                off.tick = ntf;
                new_offs[ new_key ] = off;
            }
            std::map< NoteKey, int >::const_iterator lane_it =
                m_note_lanes.find( key );
            if ( lane_it != m_note_lanes.end() )
                new_lanes[ new_key ] = lane_it->second;
        }
    }
    m_explicit_note_offs.swap( new_offs );
    m_note_lanes.swap( new_lanes );
    for ( int t = 0; t < m_num_tracks; ++t )
        for ( int f = 0; f < m_fx_cols; ++f )
        {
            std::map<int,int> shifted;
            for ( std::map<int,int>::const_iterator it = m_fx_vst[t][f].begin();
                  it != m_fx_vst[t][f].end(); ++it )
            {
                int row = it->first;
                if ( row >= m_cursor_row )
                    ++row;
                if ( row >= 0 && row < num_rows() )
                    shifted[row] = it->second;
            }
            m_fx_vst[t][f].swap( shifted );
        }
    for ( std::map<int, std::vector<std::pair<int,int> > >::iterator cit = cc_rows.begin();
          cit != cc_rows.end(); ++cit )
    {
        for ( int row = 0; row < num_rows(); ++row )
            remove_cc_at( row_start_tick( row ), cit->first );
        for ( size_t i = 0; i < cit->second.size(); ++i )
        {
            int row = cit->second[i].first;
            if ( row >= m_cursor_row )
                ++row;
            if ( row >= 0 && row < num_rows() )
                set_cc_at( row_start_tick( row ), cit->first, cit->second[i].second );
        }
    }
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Delete the cursor row: drop notes starting in it and pull every later note UP
// by one row's ticks (inverse of insert_row).
void
TrackerView::delete_row( void )
{
    if ( !m_seq )
        return;
    long rs  = row_start_tick( m_cursor_row );
    long tpr = ticks_per_row();

    std::vector<NoteCell> all;
    collect_all_notes( all );

    m_seq->push_undo();
    std::map<int, std::vector<std::pair<int,int> > > cc_rows;
    for ( int t = 0; t < m_num_tracks; ++t )
        for ( int f = 0; f < m_fx_cols; ++f )
            if ( m_fx_bind[t][f].type == FX_MIDI_CC )
                cc_rows[m_fx_bind[t][f].cc];
    for ( std::map<int, std::vector<std::pair<int,int> > >::iterator cit = cc_rows.begin();
          cit != cc_rows.end(); ++cit )
    {
        for ( int row = 0; row < num_rows(); ++row )
        {
            int value = 0;
            if ( read_cc_at( row, cit->first, &value ) )
                cit->second.push_back( std::make_pair( row, value ) );
        }
    }
    m_seq->unselect();
    m_seq->select_note_events( 0, 127, m_seq->get_length(), 0,
                               sequence::e_select );
    m_seq->mark_selected();
    m_seq->remove_marked();

    std::map< NoteKey, ExplicitNoteOff > new_offs;
    std::map< NoteKey, int > new_lanes;
    for ( size_t i = 0; i < all.size(); ++i )
    {
        if ( all[i].ts >= rs && all[i].ts < rs + tpr )
            continue;                              // this row's notes: dropped
        long nts = all[i].ts, ntf = all[i].tf;
        if ( all[i].ts >= rs + tpr )
        {
            nts -= tpr;
            if ( all[i].has_off ) ntf -= tpr;
        }
        else if ( all[i].has_off && all[i].tf >= rs + tpr ) ntf -= tpr;
        else if ( all[i].has_off && all[i].tf > rs ) ntf = rs;
        if ( nts < 0 )
            continue;
        if ( add_note_cell( nts, ntf, all[i].note, all[i].vel, all[i].has_off ) )
        {
            NoteKey key = note_key( all[i].ts, all[i].note, all[i].occurrence );
            NoteKey new_key = note_key( nts, all[i].note,
                                        note_occurrence_count( nts, all[i].note ) - 1 );
            std::map< NoteKey, ExplicitNoteOff >::const_iterator it =
                m_explicit_note_offs.find( key );
            if ( it != m_explicit_note_offs.end() )
            {
                ExplicitNoteOff off = it->second;
                off.tick = ntf;
                new_offs[ new_key ] = off;
            }
            std::map< NoteKey, int >::const_iterator lane_it =
                m_note_lanes.find( key );
            if ( lane_it != m_note_lanes.end() )
                new_lanes[ new_key ] = lane_it->second;
        }
    }
    m_explicit_note_offs.swap( new_offs );
    m_note_lanes.swap( new_lanes );
    for ( int t = 0; t < m_num_tracks; ++t )
        for ( int f = 0; f < m_fx_cols; ++f )
        {
            std::map<int,int> shifted;
            for ( std::map<int,int>::const_iterator it = m_fx_vst[t][f].begin();
                  it != m_fx_vst[t][f].end(); ++it )
            {
                int row = it->first;
                if ( row == m_cursor_row )
                    continue;
                if ( row > m_cursor_row )
                    --row;
                if ( row >= 0 && row < num_rows() )
                    shifted[row] = it->second;
            }
            m_fx_vst[t][f].swap( shifted );
        }
    for ( std::map<int, std::vector<std::pair<int,int> > >::iterator cit = cc_rows.begin();
          cit != cc_rows.end(); ++cit )
    {
        for ( int row = 0; row < num_rows(); ++row )
            remove_cc_at( row_start_tick( row ), cit->first );
        for ( size_t i = 0; i < cit->second.size(); ++i )
        {
            int row = cit->second[i].first;
            if ( row == m_cursor_row )
                continue;
            if ( row > m_cursor_row )
                --row;
            if ( row >= 0 && row < num_rows() )
                set_cc_at( row_start_tick( row ), cit->first, cit->second[i].second );
        }
    }
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

// Duplicate the marked block (all note-columns across the row span, or the
// cursor row when nothing is marked) to the rows immediately below it, keeping
// pitches, then move the selection/cursor down so repeated Ctrl+D chains.
void
TrackerView::duplicate_selection( void )
{
    if ( !m_seq )
        return;
    int r0, r1, t0, t1, c0, c1;
    selection_region( &r0, &r1, &t0, &t1, &c0, &c1 );
    const int old_row = m_cursor_row;
    const int old_track = m_cursor_track;
    const int old_col = m_cursor_col;
    const bool old_sel_active = m_sel_active;
    const int old_sel_row0 = m_sel_row0, old_sel_row1 = m_sel_row1;
    const int old_sel_track = m_sel_track, old_sel_track1 = m_sel_track1;
    const int old_sel_col = m_sel_col, old_sel_col1 = m_sel_col1;

    copy_selection();
    if ( m_cell_clipboard.empty() )
        return;

    const int row_delta = r1 - r0 + 1;
    m_cursor_row = std::min( num_rows() - 1, r0 + row_delta );
    m_cursor_track = t0;
    m_cursor_col = c0;
    paste_at_cursor();

    if ( old_sel_active )
    {
        m_sel_row0 = std::min( num_rows() - 1, old_sel_row0 + row_delta );
        m_sel_row1 = std::min( num_rows() - 1, old_sel_row1 + row_delta );
        if ( m_sel_row1 >= num_rows() ) m_sel_row1 = num_rows() - 1;
        m_sel_track = old_sel_track;
        m_sel_track1 = old_sel_track1;
        m_sel_col = old_sel_col;
        m_sel_col1 = old_sel_col1;
    }
    else
    {
        m_sel_active = false;
    }
    m_cursor_row   = std::min( num_rows() - 1, old_row + row_delta );
    m_cursor_track = old_track;
    m_cursor_col   = old_col;
    if ( m_cursor_row >= num_rows() ) m_cursor_row = num_rows() - 1;
}

// Humanize the block's velocities with a DETERMINISTIC xorshift jitter (+/-8) --
// same block always yields the same result (reproducible; not rand()).
void
TrackerView::humanize_selection( void )
{
    int r0, r1, t0, t1, c0, c1;
    selection_region( &r0, &r1, &t0, &t1, &c0, &c1 );   // all selected columns

    std::vector<NoteCell> tgt, nl;
    for ( int r = r0; r <= r1; ++r )
    {
        collect_row_notes( r, nl );
        for ( int t = t0; t <= t1; ++t )
            if ( t >= 0 && t < (int) nl.size() && nl[t].note >= 0 )
                tgt.push_back( nl[t] );
    }
    if ( tgt.empty() )
        return;

    m_seq->push_undo();
    unsigned rng = 0x2545F491u ^ (unsigned)( r0 * 2654435761u )
                               ^ (unsigned)( t0 * 2246822519u );
    std::vector<int> nv;
    nv.reserve( tgt.size() );
    for ( size_t i = 0; i < tgt.size(); ++i )
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        int jitter = (int)( rng % 17 ) - 8;       // -8..+8
        int v = tgt[i].vel + jitter;
        if ( v < 1 )   v = 1;
        if ( v > 127 ) v = 127;
        nv.push_back( v );
    }
    apply_velocities( tgt, nv );
}

// Amplify/scale the block's velocities by a fixed step (clamped 1..127).
void
TrackerView::amplify_selection( int delta )
{
    int r0, r1, t0, t1, c0, c1;
    selection_region( &r0, &r1, &t0, &t1, &c0, &c1 );   // all selected columns

    std::vector<NoteCell> tgt, nl;
    for ( int r = r0; r <= r1; ++r )
    {
        collect_row_notes( r, nl );
        for ( int t = t0; t <= t1; ++t )
            if ( t >= 0 && t < (int) nl.size() && nl[t].note >= 0 )
                tgt.push_back( nl[t] );
    }
    if ( tgt.empty() )
        return;

    m_seq->push_undo();
    std::vector<int> nv;
    nv.reserve( tgt.size() );
    for ( size_t i = 0; i < tgt.size(); ++i )
    {
        int v = tgt[i].vel + delta;
        if ( v < 1 )   v = 1;
        if ( v > 127 ) v = 127;
        nv.push_back( v );
    }
    apply_velocities( tgt, nv );
}

// Toggle the VISUAL mute flag of the cursor's note column.  NOTE: the engine
// plays every event it holds and offers no per-positional-column playback gate,
// so this dims the column in draw() but does NOT silence its notes on playback.
void
TrackerView::toggle_mute_column( void )
{
    if ( m_cursor_track >= 0 && m_cursor_track < 8 )
        m_col_muted[m_cursor_track] = !m_col_muted[m_cursor_track];
}

//----------------------------------------------------------------------------
//  playhead
//----------------------------------------------------------------------------
bool
TrackerView::poll_playhead( void )
{
    if ( !m_seq )
        return false;
    if ( !m_seq->get_playing() )
    {
        m_last_fire_row = -1;
        if ( m_last_progress_row != -1 )
        {
            m_last_progress_row = -1;
            return true;                 // repaint to erase the stale bar
        }
        return false;
    }

    int tpr = ticks_per_row();
    if ( tpr < 1 )
        tpr = 1;
    int nr  = num_rows();
    if ( nr < 1 ) nr = 1;

    long tick = m_seq->get_last_tick();
    int  cur  = ( tick / tpr ) % nr;

    // fire every VST-param FX row crossed since last time (handles loop wrap).
    if ( m_last_fire_row < 0 )
    {
        fire_fx_row( cur );
    }
    else if ( cur != m_last_fire_row )
    {
        int r = m_last_fire_row, guard = 0;
        do {
            r = ( r + 1 ) % nr;
            fire_fx_row( r );
        } while ( r != cur && ++guard < nr );
    }
    m_last_fire_row = cur;

    // follow-playback (Ctrl+F): pin the cursor row to the playhead and scroll
    // using the cached visible-row count (poll_playhead has no App&).
    if ( m_follow && m_cursor_row != cur )
    {
        m_cursor_row = cur;
        int vis = m_vis_rows; if ( vis < 1 ) vis = 1;
        if ( m_cursor_row < m_top_row )
            m_top_row = m_cursor_row;
        else if ( m_cursor_row >= m_top_row + vis )
            m_top_row = m_cursor_row - vis + 1;
        if ( m_top_row < 0 ) m_top_row = 0;
    }

    if ( cur != m_last_progress_row )
    {
        m_last_progress_row = cur;
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------
//  in-canvas menus
//----------------------------------------------------------------------------
void
TrackerView::layout_menu( App& app )
{
    int wmax = 0;
    for ( std::size_t i = 0; i < m_menu_items.size(); ++i )
        wmax = std::max( wmax, app.mono.text_w( m_menu_items[i].label ) );
    m_menu_w = std::max( 170, wmax + 20 );

    const int rowh = app.mono.ch() + 6;
    int total_h = int( m_menu_items.size() ) * rowh + 4;
    int max_h = rect.h - 8;
    if ( max_h < rowh + 4 ) max_h = rowh + 4;
    m_menu_h = std::min( total_h, max_h );

    if ( m_menu_x + m_menu_w > rect.x + rect.w ) m_menu_x = rect.x + rect.w - m_menu_w;
    if ( m_menu_y + m_menu_h > rect.y + rect.h ) m_menu_y = rect.y + rect.h - m_menu_h;
    if ( m_menu_x < rect.x ) m_menu_x = rect.x;
    if ( m_menu_y < rect.y ) m_menu_y = rect.y;

    int visible = ( m_menu_h - 4 ) / rowh;
    int max_scroll = int( m_menu_items.size() ) - visible;
    if ( max_scroll < 0 ) max_scroll = 0;
    if ( m_menu_scroll > max_scroll ) m_menu_scroll = max_scroll;
    if ( m_menu_scroll < 0 ) m_menu_scroll = 0;
}

void
TrackerView::close_menu( void )
{
    m_menu_open = false;
    m_menu_items.clear();
    m_menu_scroll = 0;
}

void
TrackerView::draw_menu( App& app )
{
    const Theme& th = theme();
    const int rowh = app.mono.ch() + 6;

    SDL_Rect frame { m_menu_x, m_menu_y, m_menu_w, m_menu_h };
    fill_rect( app.ren, frame, th.panel );
    frame_rect( app.ren, frame, th.hi );

    int mx, my;
    SDL_GetMouseState( &mx, &my );
    int visible = ( m_menu_h - 4 ) / rowh;
    for ( int vr = 0; vr < visible; ++vr )
    {
        int idx = m_menu_scroll + vr;
        if ( idx < 0 || idx >= (int) m_menu_items.size() )
            break;
        const MenuItem& mi = m_menu_items[idx];
        int ry = m_menu_y + 2 + vr * rowh;
        if ( mi.separator )
        {
            hline( app.ren, m_menu_x + 4, m_menu_x + m_menu_w - 4,
                   ry + rowh/2, th.dim );
            continue;
        }
        SDL_Rect row { m_menu_x + 1, ry, m_menu_w - 2, rowh };
        bool hover = mi.enabled &&
            mx >= row.x && mx < row.x + row.w &&
            my >= row.y && my < row.y + row.h;
        if ( hover )
            fill_rect( app.ren, row, th.accent );
        Color fg = !mi.enabled ? th.dim : ( hover ? th.bg : th.hi );
        app.mono.draw( app.ren, m_menu_x + 10, ry + 3, mi.label, fg );
    }

    if ( int( m_menu_items.size() ) > visible )
    {
        if ( m_menu_scroll > 0 )
            app.mono.draw( app.ren, m_menu_x + m_menu_w - 12, m_menu_y + 4, "^", th.dim );
        if ( m_menu_scroll + visible < int( m_menu_items.size() ) )
            app.mono.draw( app.ren, m_menu_x + m_menu_w - 12,
                           m_menu_y + m_menu_h - rowh + 4, "v", th.dim );
    }
}

void
TrackerView::open_context_menu( App& app, int sx, int sy )
{
    m_menu_items.clear();

    { MenuItem ti; ti.label = "Tracker"; ti.enabled = false; m_menu_items.push_back( ti ); }
    { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
    {
        MenuItem mi; mi.label = "Copy";
        mi.action = [this]() { copy_selection(); };
        m_menu_items.push_back( mi );
    }
    {
        MenuItem mi; mi.label = "Cut";
        mi.action = [this]() { cut_selection(); };
        m_menu_items.push_back( mi );
    }
    {
        MenuItem mi; mi.label = "Paste";
        long cts = 0, ctf = 0;
        int cnh = 0, cnl = 0;
        if ( m_seq )
            m_seq->get_clipboard_box( &cts, &cnh, &ctf, &cnl );
        bool has_seq_clip = !( cts == 0 && ctf == 0 && cnh == 0 && cnl == 0 );
        mi.enabled = !m_cell_clipboard.empty() || has_seq_clip;
        mi.action = [this]() { paste_at_cursor(); };
        m_menu_items.push_back( mi );
    }
    {
        MenuItem mi; mi.label = "Select All";
        mi.action = [this]() { select_all_cells(); };
        m_menu_items.push_back( mi );
    }
    { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
    {
        MenuItem mi; mi.label = "Interpolate FX";
        mi.enabled = m_sel_active && selection_is_single_column() && m_sel_col >= 2;
        mi.action = [this]() { interpolate_selection(); };
        m_menu_items.push_back( mi );
    }
    {
        MenuItem mi; mi.label = "Clear Cell";
        mi.action = [this]() {
            clear_selection_cells( true );
        };
        m_menu_items.push_back( mi );
    }
    {
        MenuItem mi; mi.label = "Clear Row";
        mi.action = [this]() { clear_row(); };
        m_menu_items.push_back( mi );
    }

    m_menu_x = sx; m_menu_y = sy;
    m_menu_scroll = 0;
    m_menu_open = true;
    layout_menu( app );
}

void
TrackerView::open_fx_menu( App& app, int sx, int sy, int track, int fi )
{
    if ( track < 0 || track >= m_num_tracks || fi < 0 || fi >= m_fx_cols )
        return;

    m_cursor_track = track;
    m_cursor_col = 2 + fi;
    m_fxmenu_track = track; m_fxmenu_fi = fi;
    m_menu_x = sx; m_menu_y = sy;
    m_menu_scroll = 0;
    build_fx_menu_root( app );          // top level: None / MIDI CC / instruments
}

// Top level of the FX-target picker: a TREE.  "None", a "MIDI CC >" submenu, an
// optional "Track Instrument >" submenu, then one "<instrument> >" entry per
// patch/rack node -- drilling into a node lists just THAT instrument's params
// (was one giant flat list of every node's every param).
void
TrackerView::build_fx_menu_root( App& app )
{
    m_menu_items.clear();
    { MenuItem ti; ti.label = "FX Target"; ti.enabled = false; m_menu_items.push_back( ti ); }
    { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
    { MenuItem mi; mi.label = "None"; mi.action = [this]() { bind_fx_none(); };
      m_menu_items.push_back( mi ); }

    // MIDI CC submenu
    {
        MenuItem mi; mi.label = "MIDI CC  >";
        mi.submenu = [this]( App& a ) {
            m_menu_items.clear();
            { MenuItem b; b.label = "< Back"; b.submenu = [this]( App& aa ){ build_fx_menu_root( aa ); };
              m_menu_items.push_back( b ); }
            { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
            static const int common_cc[] = { 1, 7, 10, 11, 64, 71, 72, 73, 74, 91, 93 };
            for ( unsigned i = 0; i < sizeof(common_cc)/sizeof(common_cc[0]); ++i ) {
                int cc = common_cc[i]; MenuItem it; char buf[64];
                snprintf( buf, sizeof(buf), "CC %d", cc );
                it.label = buf; it.action = [this, cc]() { bind_fx_cc( cc ); };
                m_menu_items.push_back( it );
            }
            layout_menu( a );
        };
        m_menu_items.push_back( mi );
    }

    // Track instrument submenu (params on the track's own hosted VST)
    if ( PatchKnob::app::audio_app_track_param_count( vst_track() ) > 0 )
    {
        const int tk = vst_track();
        MenuItem mi; mi.label = "Track Instrument  >";
        mi.submenu = [this, tk]( App& a ) {
            m_menu_items.clear();
            { MenuItem b; b.label = "< Back"; b.submenu = [this]( App& aa ){ build_fx_menu_root( aa ); };
              m_menu_items.push_back( b ); }
            { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
            const int cnt = PatchKnob::app::audio_app_track_param_count( tk );
            for ( int i = 0; i < cnt; ++i ) {
                unsigned int pid = 0; char name[128] = { 0 }; float def = 0.f;
                if ( !PatchKnob::app::audio_app_track_param_info( tk, i, &pid, name, sizeof(name), &def ) )
                    continue;
                FxBinding binding; binding.type = FX_VST_PARAM; binding.target = FX_TARGET_TRACK_PARAM;
                binding.pid = pid; binding.label = compact_label( name, "P" );
                binding.name = std::string( "Track > " ) + name;
                MenuItem it; it.label = name; it.action = [this, binding]() { bind_fx_target( binding ); };
                m_menu_items.push_back( it );
            }
            layout_menu( a );
        };
        m_menu_items.push_back( mi );
    }

    // One submenu per distinct patch/rack node (the "instrument" level).
    std::vector<FxBinding> targets = on_list_fx_targets ? on_list_fx_targets()
                                                        : std::vector<FxBinding>();
    std::vector<int> nodes; std::vector<std::string> names;
    for ( std::size_t i = 0; i < targets.size(); ++i ) {
        const FxBinding& b = targets[i];
        if ( std::find( nodes.begin(), nodes.end(), b.node ) != nodes.end() ) continue;
        nodes.push_back( b.node );
        std::string nm = b.name; std::size_t p = nm.find( " > " );
        names.push_back( p != std::string::npos ? nm.substr( 0, p ) : nm );
    }
    if ( !nodes.empty() )
    {
        { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
        { MenuItem hdr; hdr.label = "Instrument"; hdr.enabled = false; m_menu_items.push_back( hdr ); }
        for ( std::size_t i = 0; i < nodes.size(); ++i ) {
            const int nid = nodes[i]; const std::string nm = names[i];
            MenuItem mi; mi.label = nm + "  >";
            mi.submenu = [this, nid, nm]( App& a ){ build_fx_menu_node( a, nid, nm ); };
            m_menu_items.push_back( mi );
        }
    }

    layout_menu( app );
    m_menu_open = true;
}

// One instrument's params (drilled in from the root).
void
TrackerView::build_fx_menu_node( App& app, int nodeId, const std::string& nodeName )
{
    m_menu_items.clear();
    { MenuItem b; b.label = "< Back"; b.submenu = [this]( App& aa ){ build_fx_menu_root( aa ); };
      m_menu_items.push_back( b ); }
    { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
    { MenuItem hdr; hdr.label = nodeName; hdr.enabled = false; m_menu_items.push_back( hdr ); }

    std::vector<FxBinding> targets = on_list_fx_targets ? on_list_fx_targets()
                                                        : std::vector<FxBinding>();
    for ( std::size_t i = 0; i < targets.size(); ++i ) {
        if ( targets[i].node != nodeId ) continue;
        FxBinding binding = targets[i];
        if ( binding.label.empty() ) binding.label = compact_label( binding.name, "P" );
        std::string lab = binding.name; std::size_t p = lab.find( " > " );
        if ( p != std::string::npos ) lab = lab.substr( p + 3 );   // drop the node prefix
        MenuItem it; it.label = lab.empty() ? binding.label : lab;
        it.action = [this, binding]() { bind_fx_target( binding ); };
        m_menu_items.push_back( it );
    }
    layout_menu( app );
    m_menu_open = true;
}

//----------------------------------------------------------------------------
//  drawing
//----------------------------------------------------------------------------
void
TrackerView::draw( App& app )
{
    const Theme& th = theme();

    // No bound sequence -> paint an empty, themed panel instead of dereferencing
    // a null pointer (the shell may mount us before a pattern is chosen).
    if ( !m_seq )
    {
        SDL_RenderSetClipRect( app.ren, &rect );
        fill_rect( app.ren, rect, th.bg );
        frame_rect( app.ren, rect, th.dim );
        SDL_RenderSetClipRect( app.ren, nullptr );
        return;
    }

    const int cw = app.mono.cw();
    const int ch = app.mono.ch();
    const int row_h    = ch + 2;
    const int header_h = ch + 4;
    const int ctrl_h   = ch + 4;        // add/remove-column toolbar band

    // clip so nothing spills outside our widget rect.
    SDL_RenderSetClipRect( app.ren, &rect );

    fill_rect( app.ren, rect, th.bg );

    const int gutter_px = kGutterChars * cw;
    const int track_px  = track_chars() * cw;
    const int grid_w    = gutter_px + track_px * m_num_tracks;

    long bpm = m_seq->get_bpm();
    if ( bpm < 1 ) bpm = 4;

    // ---- control band: NOTE / FX columns + octave ------------------------
    // A full-width toolbar strip above the column labels.  Bracket buttons plus
    // the live counts are cached for on_mouse hit-testing.
    {
        SDL_Rect cbq { rect.x, rect.y, rect.w, ctrl_h };
        fill_rect( app.ren, cbq, th.panel );

        // draw one bracket button; report its rect for hit-testing.
        auto draw_btn = [&]( int col, const char* lbl, int id, bool enabled,
                             SDL_Rect& outr )
        {
            int bx = rect.x + cw/2 + col * cw;
            SDL_Rect br { bx, rect.y + 2, 3 * cw, ch };
            bool pressed = enabled && ( m_hdr_press == id );
            fill_rect ( app.ren, br, pressed ? th.accent : th.panel );
            frame_rect( app.ren, br, enabled ? th.dim : th.panel );
            Color tc = !enabled ? th.dim : ( pressed ? th.bg : th.hi );
            app.mono.draw_fitted( app.ren,
                                  SDL_Rect{ br.x + 1, br.y, br.w - 2, br.h },
                                  lbl, tc, true );
            outr = br;
        };
        auto put = [&]( int col, const char* s, Color c )
        {
            app.mono.draw_fitted( app.ren,
                                  SDL_Rect{ rect.x + cw/2 + col * cw, rect.y + 1,
                                            5 * cw, ch },
                                  s, c, false );
        };

        char nbuf[4], fbuf[4], lbuf[4], obuf[4];
        snprintf( nbuf, sizeof(nbuf), "%d", m_num_tracks );
        snprintf( fbuf, sizeof(fbuf), "%d", m_fx_cols );
        snprintf( lbuf, sizeof(lbuf), "%d", m_rows_per_beat );
        snprintf( obuf, sizeof(obuf), "%d", m_octave );

        put( 0, "NOTE", th.text );
        draw_btn( 5, "[-]", HDR_NOTE_MINUS, m_num_tracks > 1, m_btn_note_minus );
        put( 9, nbuf, th.hi );
        draw_btn( 11, "[+]", HDR_NOTE_PLUS, m_num_tracks < 8, m_btn_note_plus );

        put( 16, "FX", th.text );
        draw_btn( 19, "[-]", HDR_FX_MINUS, m_fx_cols > 0, m_btn_fx_minus );
        put( 23, fbuf, th.hi );
        draw_btn( 25, "[+]", HDR_FX_PLUS, m_fx_cols < 8, m_btn_fx_plus );

        int li = lpb_index( m_rows_per_beat );
        put( 31, "LPB", th.text );
        draw_btn( 35, "[-]", HDR_LPB_MINUS, li > 0, m_btn_lpb_minus );
        put( 39, lbuf, th.hi );
        draw_btn( 42, "[+]", HDR_LPB_PLUS,
                  li >= 0 && li + 1 < (int)( sizeof(k_lpb_values) / sizeof(k_lpb_values[0]) ),
                  m_btn_lpb_plus );

        put( 48, "OCT", th.text );
        draw_btn( 52, "[-]", HDR_OCT_MINUS, m_octave > 0, m_btn_oct_minus );
        put( 56, obuf, th.hi );
        draw_btn( 58, "[+]", HDR_OCT_PLUS, m_octave < 8, m_btn_oct_plus );

        const bool editing_lines = app.editing_text() && app.text_target == &m_lines_edit;
        if ( !editing_lines )
            sync_lines_edit();
        const int edit_w = 7 * cw;
        m_lines_box = SDL_Rect{ rect.x + rect.w - edit_w - 4,
                                rect.y + 2, edit_w, ch };
        int label_x = m_lines_box.x - 6 * cw;
        if ( label_x > rect.x + 61 * cw )
            app.mono.draw_fitted( app.ren,
                                  SDL_Rect{ label_x, rect.y + 1, 6 * cw, ch },
                                  "LINES", th.text, false );
        fill_rect( app.ren, m_lines_box, editing_lines ? th.active : th.bg );
        frame_rect( app.ren, m_lines_box, editing_lines ? th.accent : th.dim );
        std::string shown = clip_chars( m_lines_edit, 6 );
        app.mono.draw_fitted( app.ren,
                              SDL_Rect{ m_lines_box.x + 3, m_lines_box.y,
                                        m_lines_box.w - 6, m_lines_box.h },
                              shown, editing_lines ? th.bg : th.hi, false );

        hline( app.ren, rect.x, rect.x + rect.w, rect.y + ctrl_h - 1, th.dim );
    }

    // ---- header ----------------------------------------------------------
    SDL_Rect hdr { rect.x, rect.y + ctrl_h, grid_w, header_h };
    fill_rect( app.ren, hdr, th.panel );
    int hty = rect.y + ctrl_h + 2;
    for ( int t = 0; t < m_num_tracks; ++t )
    {
        int cx = rect.x + gutter_px + t * track_px;

        char tl[8];
        snprintf( tl, sizeof(tl), "T%d", t );
        app.mono.draw( app.ren, cx, hty, tl,
                       t == m_cursor_track ? th.hi : th.dim );

        // muted note-column marker (visual only -- see toggle_mute_column()).
        if ( m_col_muted[t] )
            app.mono.draw( app.ren, cx + 2*cw, hty, "M", th.accent );

        app.mono.draw( app.ren, cx + 4*cw, hty, "vv", th.dim );

        for ( int f = 0; f < m_fx_cols; ++f )
        {
            int fcx, fcw;
            subcol_geom( 2 + f, &fcx, &fcw );
            FxBinding& b = m_fx_bind[t][f];
            app.mono.draw( app.ren, cx + fcx*cw, hty, clip_chars( b.label, 4 ),
                           b.type == FX_NONE ? th.dim : th.text );
        }
    }
    hline( app.ren, rect.x, rect.x + grid_w,
           rect.y + ctrl_h + header_h - 1, th.dim );

    // ---- rows ------------------------------------------------------------
    int rows = num_rows();
    int vis  = visible_rows( app );
    m_vis_rows = vis;                   // cache for poll_playhead (no App& there)

    NoteCell empty_off;
    empty_off.note = -1;
    empty_off.vel = 0;
    empty_off.ts = 0;
    empty_off.tf = 0;
    empty_off.has_off = false;
    std::vector< std::vector<NoteCell> > visible_notes(
        vis, std::vector<NoteCell>( m_num_tracks, empty_off ) );
    std::vector< std::vector<NoteCell> > visible_offs(
        vis, std::vector<NoteCell>( m_num_tracks, empty_off ) );
    std::vector<int> visible_cc_values( (size_t)vis * 128, -1 );
    bool visible_cc[128] = { false };
    for ( int t = 0; t < m_num_tracks; ++t )
        for ( int f = 0; f < m_fx_cols; ++f )
        {
            const FxBinding& binding = m_fx_bind[t][f];
            if ( binding.type == FX_MIDI_CC && binding.cc >= 0 && binding.cc < 128 )
                visible_cc[binding.cc] = true;
        }

    const int tpr = ticks_per_row();
    m_seq->reset_draw_marker();
    long tick_s, tick_f;
    int note, vel;
    bool selected;
    std::map< std::pair<long,int>, int > draw_occurrences;
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        int occurrence = draw_occurrences[ std::make_pair( tick_s, note ) ]++;
        std::map< NoteKey, ExplicitNoteOff >::const_iterator off =
            m_explicit_note_offs.find( note_key( tick_s, note, occurrence ) );
        int row = (int)( tick_s / tpr );
        int screen_row = row - m_top_row;
        if ( screen_row >= 0 && screen_row < vis )
        {
            NoteCell nc;
            nc.note = note;
            nc.vel = vel;
            nc.ts = tick_s;
            nc.occurrence = occurrence;
            nc.has_off = tick_f > tick_s;
            nc.tf = normalized_note_end( tick_s, tick_f );
            if ( off != m_explicit_note_offs.end() )
            {
                nc.has_off = true;
                nc.tf = off->second.tick;
            }
            int lane = assigned_note_lane( tick_s, note, occurrence );
            if ( lane >= 0 && lane < m_num_tracks &&
                 visible_notes[screen_row][lane].note < 0 )
            {
                visible_notes[screen_row][lane] = nc;
            }
            else
            {
                for ( int i = 0; i < m_num_tracks; ++i )
                    if ( visible_notes[screen_row][i].note < 0 )
                    {
                        visible_notes[screen_row][i] = nc;
                        break;
                    }
            }
        }
        if ( off != m_explicit_note_offs.end() )
        {
            int off_row = (int)( off->second.tick / tpr ) - m_top_row;
            if ( off_row >= 0 && off_row < vis &&
                 off->second.track >= 0 && off->second.track < m_num_tracks )
            {
                NoteCell nc;
                nc.note = note;
                nc.vel = vel;
                nc.ts = tick_s;
                nc.tf = off->second.tick;
                nc.has_off = true;
                visible_offs[off_row][off->second.track] = nc;
            }
        }
    }
    for ( int cc = 0; cc < 128; ++cc )
    {
        if ( !visible_cc[cc] ) continue;
        long tick;
        unsigned char d0, d1;
        bool cc_selected;
        m_seq->reset_draw_marker();
        while ( m_seq->get_next_event( EVENT_CONTROL_CHANGE, (unsigned char)cc,
                                       &tick, &d0, &d1, &cc_selected ) )
        {
            int screen_row = (int)( tick / tpr ) - m_top_row;
            if ( screen_row < 0 || screen_row >= vis ) continue;
            int& value = visible_cc_values[(size_t)screen_row * 128 + cc];
            if ( value < 0 ) value = d1;
        }
    }

    for ( int sr = 0; sr < vis; ++sr )
    {
        int row = m_top_row + sr;
        if ( row >= rows ) break;

        int y = rect.y + ctrl_h + header_h + sr * row_h;
        SDL_Rect rowq { rect.x, y, grid_w, row_h };

        bool is_beat = ( row % m_rows_per_beat ) == 0;
        bool is_bar  = ( row % ( m_rows_per_beat * (int)bpm ) ) == 0;

        // beat / bar shading (panel over bg, brighter on bars).
        if ( is_bar )
            fill_rect( app.ren, rowq, th.panel );
        else if ( is_beat )
        {
            Color c = th.panel; c.a = 150;
            SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_BLEND );
            fill_rect( app.ren, rowq, c );
            SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_NONE );
        }

        // playhead row highlight (from get_last_tick, via poll_playhead).
        if ( row == m_last_progress_row )
        {
            Color a = th.active; a.a = 90;
            SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_BLEND );
            fill_rect( app.ren, rowq, a );
            SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_NONE );
        }

        // cursor row wash.
        if ( row == m_cursor_row )
        {
            Color a = th.accent; a.a = 60;
            SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_BLEND );
            fill_rect( app.ren, rowq, a );
            SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_NONE );
        }

        // multi-cell selection wash: an inclusive row/track/sub-column rectangle.
        if ( m_sel_active &&
             row >= m_sel_row0 && row <= m_sel_row1 )
        {
            for ( int st = m_sel_track0; st <= m_sel_track1; ++st )
                for ( int sc = m_sel_col0; sc <= m_sel_col1; ++sc )
                    if ( st >= 0 && st < m_num_tracks &&
                         sc >= 0 && sc < total_subcols() )
                    {
                        int scx = rect.x + gutter_px + st * track_px;
                        int sx, sw;
                        subcol_geom( sc, &sx, &sw );
                        SDL_Rect selq { scx + sx*cw, y, sw*cw, row_h };
                        Color a = th.sel; a.a = 90;
                        SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_BLEND );
                        fill_rect( app.ren, selq, a );
                        SDL_SetRenderDrawBlendMode( app.ren, SDL_BLENDMODE_NONE );
                    }
        }

        hline( app.ren, rect.x, rect.x + grid_w, y, is_beat ? th.dim : th.panel );

        int ty = y + 1;

        // row number gutter.
        char rn[8];
        snprintf( rn, sizeof(rn), "%3d", row );
        app.mono.draw( app.ren, rect.x + cw/2, ty, rn,
                       is_beat ? th.hi : th.dim );

        const std::vector<NoteCell>& nl = visible_notes[sr];
        const std::vector<NoteCell>& offl = visible_offs[sr];

        for ( int t = 0; t < m_num_tracks; ++t )
        {
            int cx = rect.x + gutter_px + t * track_px;

            bool has = t < (int) nl.size() && nl[t].note >= 0;
            bool has_off = !has && t < (int) offl.size() && offl[t].note >= 0;
            std::string cell_note = has ? note_name( nl[t].note )
                                  : has_off ? "OFF" : "---";
            char cell_vel[8];
            if ( has ) snprintf( cell_vel, sizeof(cell_vel), "%02X", nl[t].vel & 0x7f );
            else       snprintf( cell_vel, sizeof(cell_vel), "--" );

            bool muted = m_col_muted[t];
            app.mono.draw( app.ren, cx, ty, cell_note,
                           ( has || has_off ) ? ( muted ? th.dim : th.text ) : th.dim );
            app.mono.draw( app.ren, cx + 4*cw, ty, cell_vel,
                           has ? ( muted ? th.dim : th.hi ) : th.dim );

            for ( int f = 0; f < m_fx_cols; ++f )
            {
                int fcx, fcw;
                subcol_geom( 2 + f, &fcx, &fcw );

                char fxs[8];
                bool fhas = false;
                int  fval = 0;

                const FxBinding& binding = m_fx_bind[t][f];
                if ( binding.type == FX_MIDI_CC &&
                     binding.cc >= 0 && binding.cc < 128 )
                {
                    int cc7 = visible_cc_values[(size_t)sr * 128 + binding.cc];
                    if ( cc7 >= 0 )
                    {
                        fval = ( cc7 * 65535 + 63 ) / 127;
                        fhas = true;
                    }
                }
                else if ( binding.type == FX_VST_PARAM )
                {
                    std::map<int,int>& values = m_fx_vst[t][f];
                    std::map<int,int>::iterator value = values.find( row );
                    if ( value != values.end() )
                    {
                        fval = clamp_int( value->second, 0, 0xffff );
                        fhas = true;
                    }
                }

                if ( fhas ) snprintf( fxs, sizeof(fxs), "%04X", fval & 0xffff );
                else        snprintf( fxs, sizeof(fxs), "...." );

                app.mono.draw( app.ren, cx + fcx*cw, ty, fxs,
                               fhas ? th.text : th.dim );
            }

            // cursor underline for the active sub-column.
            if ( row == m_cursor_row && t == m_cursor_track )
            {
                int ux, uw;
                subcol_geom( m_cursor_col, &ux, &uw );
                SDL_Rect cur { cx + ux*cw, y + row_h - 2, uw*cw, 2 };
                fill_rect( app.ren, cur, th.sel );
            }
        }
    }

    // ---- vertical rules --------------------------------------------------
    vline( app.ren, rect.x + gutter_px, rect.y, rect.y + rect.h, th.dim );
    for ( int t = 0; t <= m_num_tracks; ++t )
    {
        int x = rect.x + gutter_px + t * track_px;
        vline( app.ren, x, rect.y, rect.y + rect.h, th.panel );
    }

    frame_rect( app.ren, rect, th.dim );
    if ( m_menu_open )
        draw_menu( app );
    SDL_RenderSetClipRect( app.ren, nullptr );
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
bool
TrackerView::on_mouse( App& app, const MouseEv& e )
{
    if ( !m_seq )
        return false;

    if ( m_menu_open )
    {
        if ( e.pressed )
        {
            const int rowh = app.mono.ch() + 6;
            bool inside = e.x >= m_menu_x && e.x < m_menu_x + m_menu_w &&
                          e.y >= m_menu_y && e.y < m_menu_y + m_menu_h;
            if ( inside )
            {
                int vr = ( e.y - ( m_menu_y + 2 ) ) / rowh;
                int idx = m_menu_scroll + vr;
                if ( idx >= 0 && idx < (int) m_menu_items.size() )
                {
                    MenuItem mi = m_menu_items[idx];
                    if ( mi.enabled && !mi.separator )
                    {
                        if ( mi.submenu )              // DRILL DOWN: rebuild in place
                        {
                            mi.submenu( app );
                            m_menu_scroll = 0;
                            app.request_redraw();
                            return true;
                        }
                        close_menu();
                        if ( mi.action ) mi.action();
                        ensure_cursor_visible( app );
                        app.request_redraw();
                        return true;
                    }
                }
            }
            close_menu();
            app.request_redraw();
            return true;
        }
        return true;
    }

    // A button release ends an in-progress selection drag (the marked block
    // stays).  Handle it before the hit-test because a drag can end with the
    // pointer outside our rect (the shell still delivers the captured release).
    if ( !e.pressed )
    {
        m_sel_drag = false;
        if ( m_hdr_press )              // release a held header button
        {
            m_hdr_press = 0;
            app.request_redraw();
        }
        return true;
    }
    if ( e.button != SDL_BUTTON_LEFT && e.button != SDL_BUTTON_RIGHT )
        return true;

    // While a header control button is held, swallow the drag-motion events
    // (delivered as pressed) until the button is released.
    if ( m_hdr_press )
        return true;

    // Header control buttons (add/remove NOTE + FX columns) are hit-tested on a
    // FRESH press BEFORE any cell mapping, so a click on a button never edits a
    // cell.  The button rects were cached by the last draw().
    if ( !m_sel_drag )
    {
        SDL_Point pt { e.x, e.y };
        if ( e.button == SDL_BUTTON_LEFT && SDL_PointInRect( &pt, &m_lines_box ) )
        {
            begin_lines_edit( app );
            m_hdr_press = 0;
            app.request_redraw();
            return true;
        }

        int b = hdr_button_at( e.x, e.y );
        if ( e.button == SDL_BUTTON_LEFT && b != HDR_NONE )
        {
            m_hdr_press = b;
            switch ( b )
            {
                case HDR_NOTE_MINUS:
                    set_num_note_cols( get_num_note_cols() - 1 ); break;
                case HDR_NOTE_PLUS:
                    set_num_note_cols( get_num_note_cols() + 1 ); break;
                case HDR_FX_MINUS: remove_fx_col(); break;
                case HDR_FX_PLUS:  add_fx_col();    break;
                case HDR_LPB_MINUS:
                {
                    int i = lpb_index( m_rows_per_beat );
                    if ( i > 0 ) set_lines_per_beat( k_lpb_values[i - 1] );
                    break;
                }
                case HDR_LPB_PLUS:
                {
                    int i = lpb_index( m_rows_per_beat );
                    int n = (int)( sizeof(k_lpb_values) / sizeof(k_lpb_values[0]) );
                    if ( i >= 0 && i + 1 < n ) set_lines_per_beat( k_lpb_values[i + 1] );
                    break;
                }
                case HDR_OCT_MINUS: set_octave( get_octave() - 1 ); break;
                case HDR_OCT_PLUS:  set_octave( get_octave() + 1 ); break;
                default: break;
            }
            // recompute cached column-x layout on the next draw + repaint.
            ensure_cursor_visible( app );
            app.request_redraw();
            return true;
        }
    }

    // A fresh press must land inside the grid; motion during a drag may stray
    // outside (we still receive it because the shell captures the drag).
    if ( !m_sel_drag && !hit( e.x, e.y ) )
        return false;

    const int cw = app.mono.cw();
    const int ch = app.mono.ch();
    const int row_h    = ch + 2;
    const int header_h = ch + 4;
    const int ctrl_h   = ch + 4;        // add/remove-column toolbar band

    // map the pointer Y to a row, clamped so dragging past either end still
    // selects the first / last row.
    int sr  = ( e.y - rect.y - ctrl_h - header_h ) / row_h;
    int row = ( e.y - rect.y < ctrl_h + header_h ) ? m_top_row : m_top_row + sr;
    if ( row < 0 ) row = 0;
    if ( row >= num_rows() ) row = num_rows() - 1;
    m_cursor_row = row;

    const int gutter_px = kGutterChars * cw;
    const int track_px  = track_chars() * cw;

    auto map_column = [&]( int mx )
    {
        int x = mx - rect.x;
        if ( x < gutter_px )
            return;
        int t = ( x - gutter_px ) / track_px;
        if ( t >= 0 && t < m_num_tracks )
        {
            m_cursor_track = t;
            int rel_chars = ( ( x - gutter_px ) % track_px ) / cw;

            // nearest sub-column by its character midpoint.
            int best = 0, bestd = 1 << 30;
            int total = total_subcols();
            for ( int c = 0; c < total; ++c )
            {
                int sx, sw;
                subcol_geom( c, &sx, &sw );
                int mid = sx + sw / 2;
                int d = rel_chars - mid; if ( d < 0 ) d = -d;
                if ( d < bestd ) { bestd = d; best = c; }
            }
            m_cursor_col = best;
        }
    };

    if ( !m_sel_drag && e.y >= rect.y + ctrl_h &&
         e.y < rect.y + ctrl_h + header_h )
    {
        map_column( e.x );
        if ( e.button == SDL_BUTTON_LEFT && m_cursor_col >= 2 )
            open_fx_menu( app, e.x, e.y, m_cursor_track, m_cursor_col - 2 );
        app.request_redraw();
        return true;
    }

    if ( !m_sel_drag && e.y < rect.y + ctrl_h + header_h )
        return true;

    map_column( e.x );

    if ( !m_sel_drag && e.button == SDL_BUTTON_RIGHT )
    {
        open_context_menu( app, e.x, e.y );
        app.request_redraw();
        return true;
    }

    if ( e.button != SDL_BUTTON_LEFT )
        return true;

    if ( !m_sel_drag )
    {
        // Fresh press: anchor a new selection and clear the old block.  A plain
        // click selects nothing until the pointer is dragged to another row/col.
        m_sel_drag       = true;
        m_sel_anchor_row = m_cursor_row;
        m_sel_anchor_track = m_cursor_track;
        m_sel_anchor_col   = m_cursor_col;
        m_sel_track      = m_cursor_track;
        m_sel_col        = m_cursor_col;
        m_sel_row0 = m_sel_row1 = m_cursor_row;
        m_sel_track0 = m_sel_track1 = m_cursor_track;
        m_sel_col0 = m_sel_col1 = m_cursor_col;
        m_sel_active     = false;
    }
    else
    {
        // Drag: extend an inclusive row/track/sub-column rectangle.
        m_sel_row0   = std::min( m_sel_anchor_row, m_cursor_row );
        m_sel_row1   = std::max( m_sel_anchor_row, m_cursor_row );
        m_sel_track0 = std::min( m_sel_anchor_track, m_cursor_track );
        m_sel_track1 = std::max( m_sel_anchor_track, m_cursor_track );
        m_sel_col0   = std::min( m_sel_anchor_col, m_cursor_col );
        m_sel_col1   = std::max( m_sel_anchor_col, m_cursor_col );
        m_sel_track  = m_sel_anchor_track;
        m_sel_col    = m_sel_anchor_col;
        m_sel_active = ( m_sel_row1 > m_sel_row0 ) ||
                       ( m_sel_track1 > m_sel_track0 ) ||
                       ( m_sel_col1 > m_sel_col0 );
    }

    ensure_cursor_visible( app );
    app.request_redraw();
    return true;
}

bool
TrackerView::on_wheel( App& app, int /*dx*/, int dy )
{
    if ( !m_seq )
        return false;
    if ( m_menu_open )
    {
        m_menu_scroll -= dy;
        layout_menu( app );
        app.request_redraw();
        return true;
    }
    m_top_row -= dy;
    int maxtop = num_rows() - visible_rows( app );
    if ( maxtop < 0 ) maxtop = 0;
    if ( m_top_row > maxtop ) m_top_row = maxtop;
    if ( m_top_row < 0 ) m_top_row = 0;
    app.request_redraw();
    return true;
}

bool
TrackerView::on_key( App& app, SDL_Keycode k )
{
    if ( !m_seq )
        return false;

    if ( m_menu_open )
    {
        if ( k == SDLK_ESCAPE )
        {
            close_menu();
            app.request_redraw();
        }
        return true;
    }

    SDL_Keymod mod = SDL_GetModState();
    bool ctrl  = ( mod & KMOD_CTRL )  != 0;
    bool shift = ( mod & KMOD_SHIFT ) != 0;
    bool alt   = ( mod & KMOD_ALT )   != 0;

    auto extend_selection = [this, &app]( int drow, int dcol )
    {
        if ( !m_sel_active )
        {
            m_sel_anchor_row = m_cursor_row;
            m_sel_anchor_track = m_cursor_track;
            m_sel_anchor_col = m_cursor_col;
            m_sel_track = m_cursor_track;
            m_sel_col = m_cursor_col;
        }
        move_cursor( app, drow, dcol );
        m_sel_row0 = std::min( m_sel_anchor_row, m_cursor_row );
        m_sel_row1 = std::max( m_sel_anchor_row, m_cursor_row );
        m_sel_track0 = std::min( m_sel_anchor_track, m_cursor_track );
        m_sel_track1 = std::max( m_sel_anchor_track, m_cursor_track );
        m_sel_col0 = std::min( m_sel_anchor_col, m_cursor_col );
        m_sel_col1 = std::max( m_sel_anchor_col, m_cursor_col );
        m_sel_active = ( m_sel_row1 > m_sel_row0 ) ||
                       ( m_sel_track1 > m_sel_track0 ) ||
                       ( m_sel_col1 > m_sel_col0 );
    };

    // Ctrl-modified editing commands, checked before note/hex entry so that,
    // e.g., Ctrl+C copies instead of typing a 'C' note.  Unhandled Ctrl combos
    // are passed through (return false) rather than swallowed.
    if ( ctrl )
    {
        switch ( k )
        {
            case SDLK_EQUALS:                       // Ctrl+'='  add FX column
            case SDLK_PLUS:                         // Ctrl+'+'
            case SDLK_KP_PLUS:
                add_fx_col();
                ensure_cursor_visible( app );
                app.request_redraw();
                return true;
            case SDLK_MINUS:                        // Ctrl+'-'  remove FX column
            case SDLK_KP_MINUS:
                remove_fx_col();
                app.request_redraw();
                return true;
            case SDLK_i:                            // Ctrl+I       interp FX span
                if ( shift ) interpolate_velocity();// Ctrl+Shift+I interp vel
                else         interpolate_selection();
                app.request_redraw();
                return true;
            case SDLK_c:                            // Ctrl+C  copy selection notes
                copy_selection();
                app.request_redraw();
                return true;
            case SDLK_x:                            // Ctrl+X  cut selection cells
                cut_selection();
                app.request_redraw();
                return true;
            case SDLK_v:                            // Ctrl+V  paste at cursor
                paste_at_cursor();
                app.request_redraw();
                return true;
            case SDLK_z:                            // Ctrl+Z  undo events
                if ( m_seq )
                {
                    m_seq->pop_undo();
                    m_seq->verify_and_link();
                    load_current_fx();
                    app.request_redraw();
                    return true;
                }
                break;
            case SDLK_a:                            // Ctrl+A  select whole pattern
                select_all_cells();
                app.request_redraw();
                return true;
            case SDLK_UP:                           // Ctrl+Up    +1 semitone
                transpose_selection( shift ? 12 : 1 );   // +Shift => +1 octave
                app.request_redraw();
                return true;
            case SDLK_DOWN:                         // Ctrl+Down  -1 semitone
                transpose_selection( shift ? -12 : -1 ); // +Shift => -1 octave
                app.request_redraw();
                return true;
            case SDLK_d:                            // Ctrl+D  duplicate block below
                duplicate_selection();
                ensure_cursor_visible( app );
                app.request_redraw();
                return true;
            case SDLK_h:                            // Ctrl+H  humanize velocities
                humanize_selection();
                app.request_redraw();
                return true;
            case SDLK_m:                            // Ctrl+M  mute note column
                toggle_mute_column();
                app.request_redraw();
                return true;
            case SDLK_f:                            // Ctrl+F  follow-playback
                m_follow = !m_follow;
                app.request_redraw();
                return true;
            case SDLK_PERIOD:                       // Ctrl+'.'  velocity +8
                amplify_selection( 8 );
                app.request_redraw();
                return true;
            case SDLK_COMMA:                        // Ctrl+','  velocity -8
                amplify_selection( -8 );
                app.request_redraw();
                return true;
            case SDLK_DELETE:                       // Ctrl+Delete  clear whole row
                clear_row();
                app.request_redraw();
                return true;
            case SDLK_BACKSPACE:                    // Ctrl+Shift+Backspace clear all
                if ( shift )
                {
                    clear_pattern();
                    app.request_redraw();
                    return true;
                }
                break;                              // plain Ctrl+Backspace: pass
            case SDLK_HOME:                         // Ctrl+Home  jump first column
                m_cursor_track = 0;
                m_cursor_col   = 0;
                app.request_redraw();
                return true;
            case SDLK_END:                          // Ctrl+End   jump last column
                m_cursor_track = m_num_tracks - 1;
                m_cursor_col   = total_subcols() - 1;
                app.request_redraw();
                return true;
        }
        return false;
    }

    if ( shift )
    {
        switch ( k )
        {
            case SDLK_UP:       extend_selection( -1, 0 ); return true;
            case SDLK_DOWN:     extend_selection(  1, 0 ); return true;
            case SDLK_LEFT:     extend_selection(  0,-1 ); return true;
            case SDLK_RIGHT:    extend_selection(  0, 1 ); return true;
            case SDLK_TAB:      extend_selection(  0,-total_subcols() ); return true;
            case SDLK_PAGEUP:   extend_selection( -visible_rows( app ), 0 ); return true;
            case SDLK_PAGEDOWN: extend_selection(  visible_rows( app ), 0 ); return true;
            default: break;
        }
    }

    if ( alt )
    {
        switch ( k )
        {
            case SDLK_UP:        set_edit_step( m_edit_step + 1 ); app.request_redraw(); return true;
            case SDLK_DOWN:      set_edit_step( m_edit_step - 1 ); app.request_redraw(); return true;
            case SDLK_LEFT:
            {
                int i = lpb_index( m_rows_per_beat );
                if ( i > 0 ) set_lines_per_beat( k_lpb_values[i - 1] );
                ensure_cursor_visible( app );
                app.request_redraw();
                return true;
            }
            case SDLK_RIGHT:
            {
                int i = lpb_index( m_rows_per_beat );
                int n = (int)( sizeof(k_lpb_values) / sizeof(k_lpb_values[0]) );
                if ( i >= 0 && i + 1 < n ) set_lines_per_beat( k_lpb_values[i + 1] );
                ensure_cursor_visible( app );
                app.request_redraw();
                return true;
            }
            default: break;
        }
    }

    switch ( k )
    {
        case SDLK_UP:       move_cursor( app, -1, 0 ); return true;
        case SDLK_DOWN:     move_cursor( app,  1, 0 ); return true;
        case SDLK_LEFT:     move_cursor( app,  0,-1 ); return true;
        case SDLK_RIGHT:    move_cursor( app,  0, 1 ); return true;
        case SDLK_TAB:      move_cursor( app,  0, total_subcols() ); return true;
        case SDLK_PAGEUP:   move_cursor( app, -visible_rows( app ), 0 ); return true;
        case SDLK_PAGEDOWN: move_cursor( app,  visible_rows( app ), 0 ); return true;
        case SDLK_HOME:     m_cursor_row = 0;
                            ensure_cursor_visible( app );
                            app.request_redraw(); return true;
        case SDLK_END:      m_cursor_row = num_rows() - 1;
                            ensure_cursor_visible( app );
                            app.request_redraw(); return true;

        case SDLK_INSERT:                           // Insert  insert blank row
            insert_row();
            ensure_cursor_visible( app );
            app.request_redraw(); return true;

        case SDLK_DELETE:
            if ( shift )                            // Shift+Delete  delete row
            {
                delete_row();
                ensure_cursor_visible( app );
                app.request_redraw();
                return true;
            }
            [[fallthrough]];            // plain Delete: clear the cell
        case SDLK_PERIOD:
        case SDLK_BACKSPACE:
            clear_selection_cells( true );
            move_cursor( app, m_edit_step, 0 );
            return true;

        case SDLK_ESCAPE:
            m_sel_active = false;
            m_sel_drag = false;
            stop_sounding_notes();
            app.request_redraw();
            return true;

        case SDLK_LEFTBRACKET:
            set_octave( m_octave - 1 );
            app.request_redraw();
            return true;

        case SDLK_RIGHTBRACKET:
            set_octave( m_octave + 1 );
            app.request_redraw();
            return true;

        case SDLK_MINUS:
        case SDLK_KP_MINUS:
            set_edit_step( m_edit_step - 1 );
            app.request_redraw();
            return true;

        case SDLK_EQUALS:
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
            set_edit_step( m_edit_step + 1 );
            app.request_redraw();
            return true;

        case SDLK_BACKQUOTE:                        // '`' note column -> OFF
            if ( m_cursor_col == 0 )
            {
                note_off_at_cell();
                move_cursor( app, m_edit_step, 0 );
                return true;
            }
            return false;
    }

    // NOTE column -> tracker keyboard note entry.
    if ( m_cursor_col == 0 )
    {
        int oct_off = 0;
        int pc = key_to_pitch( k, &oct_off );
        if ( pc >= 0 )
        {
            int note = ( m_octave + oct_off + 1 ) * 12 + pc;   // C-4 == MIDI 60
            if ( note >= 0 && note <= 127 )
            {
                set_note_at_cell( note );
                // live preview through the clip's selected instrument; the note
                // sustains until the key is released (on_key_up -> note off).
                if ( m_seq && !m_sounding.count( k ) )
                { m_seq->play_note_on( note, m_velocity ); m_sounding[k] = note; }
                move_cursor( app, m_edit_step, 0 );
            }
            app.request_redraw();
            return true;
        }
        return false;
    }

    // VELOCITY / FX columns -> hex value entry (nibble-accumulate).
    int hv = key_to_hex( k );
    if ( hv >= 0 )
    {
        if ( m_cursor_col == 1 )
        {
            int note, vel;
            if ( note_at_row( m_cursor_row, m_cursor_track, &note, &vel ) )
                set_velocity_at_cell( ( ( vel << 4 ) | hv ) & 0x7f );
        }
        else
        {
            int fi = cur_fx_index();
            int cur = 0;
            read_fx_value( m_cursor_row, m_cursor_track, fi, &cur );
            set_fx_at_cell( ( ( cur << 4 ) | hv ) & 0xffff );
        }
        app.request_redraw();
        return true;
    }

    return false;
}

void
TrackerView::stop_sounding_notes( void )
{
    if ( m_seq )
    {
        for ( std::map<SDL_Keycode,int>::iterator it = m_sounding.begin();
              it != m_sounding.end(); ++it )
            m_seq->play_note_off( it->second );
    }
    m_sounding.clear();
}

// Key RELEASE: silence the preview note this key started ("depress a key on the
// tracker -> the note turns off").
bool
TrackerView::on_key_up( App& app, SDL_Keycode k )
{
    std::map<SDL_Keycode,int>::iterator it = m_sounding.find( k );
    if ( it != m_sounding.end() )
    {
        if ( m_seq ) m_seq->play_note_off( it->second );
        m_sounding.erase( it );
        app.request_redraw();
        return true;
    }
    return false;
}

} // namespace ui
