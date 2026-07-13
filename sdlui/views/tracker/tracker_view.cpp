//----------------------------------------------------------------------------
//  sdlui/views/tracker/tracker_view.cpp -- SDL2 port of the seq24 tracker.
//  See tracker_view.h.  Logic mirrors src/trackeredit.cpp (trackergrid) but
//  draws through the ui:: toolkit and reads/writes the shared engine sequence.
//----------------------------------------------------------------------------
#include "tracker_view.h"

#include "sequence.h"       // engine model  (pulls event.h / globals.h)
#include "audio_app.h"      // header-only bridge to the VST engine

#include <algorithm>
#include <cstdio>

namespace ui {

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

    // Allocate binding + value stores for the max note-column count (8) so
    // growing the column count never reallocates out from under a live cursor.
    m_fx_bind.resize( 8 );
    m_fx_vst.resize( 8 );
    for ( int t = 0; t < 8; ++t )
    {
        m_fx_bind[t].resize( m_fx_cols );
        m_fx_vst[t].resize( m_fx_cols );
        // sensible musical defaults so a fresh pattern already does something.
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

//----------------------------------------------------------------------------
//  model <-> grid helpers
//----------------------------------------------------------------------------
int
TrackerView::vst_track( void ) const
{
    return m_track >= 0 ? m_track : (int) m_seq->get_midi_bus();
}

int
TrackerView::ticks_per_row( void ) const
{
    int t = c_ppqn / m_rows_per_beat;
    if ( t < 1 ) t = 1;
    return t;
}

int
TrackerView::num_rows( void ) const
{
    int rows = m_seq->get_length() / ticks_per_row();
    if ( rows < 1 ) rows = 1;
    return rows;
}

long
TrackerView::row_start_tick( int row ) const
{
    return (long) row * ticks_per_row();
}

long
TrackerView::length_measures( void ) const
{
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

// gather the note-ons that start in `row`, sorted ascending by pitch.
void
TrackerView::collect_row_notes( int row, std::vector<NoteCell>& out )
{
    out.clear();

    long ts = row_start_tick( row );
    long tf = ts + ticks_per_row();

    long tick_s, tick_f;
    int  note, vel;
    bool selected;

    m_seq->reset_draw_marker();
    while ( m_seq->get_next_note_event( &tick_s, &tick_f, &note,
                                        &selected, &vel ) != DRAW_FIN )
    {
        if ( tick_s >= ts && tick_s < tf )
        {
            NoteCell nc;
            nc.note = note;
            nc.vel  = vel;
            nc.ts   = tick_s;
            nc.tf   = tick_f;
            out.push_back( nc );
        }
    }

    std::sort( out.begin(), out.end(),
               []( const NoteCell& l, const NoteCell& r )
               { return l.note < r.note; } );
}

bool
TrackerView::note_at_row( int row, int track, int* note, int* vel )
{
    std::vector<NoteCell> nl;
    collect_row_notes( row, nl );
    if ( track < 0 || track >= (int) nl.size() )
        return false;
    if ( note ) *note = nl[track].note;
    if ( vel )  *vel  = nl[track].vel;
    return true;
}

// remove exactly the note whose ON is at `ts` with pitch `note`.
void
TrackerView::remove_specific_note( long ts, int note )
{
    m_seq->unselect();
    int n = m_seq->select_note_events( ts, note, ts, note,
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
TrackerView::set_note_at_cell( int note )
{
    if ( note < 0 || note > 127 )
        return;

    long ts  = row_start_tick( m_cursor_row );
    long len = ticks_per_row() * m_edit_step;
    if ( len < 1 ) len = ticks_per_row();

    m_seq->push_undo();

    // if this note-column slot already holds a note, replace just that one.
    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track < (int) nl.size() )
        remove_specific_note( nl[m_cursor_track].ts, nl[m_cursor_track].note );

    m_seq->add_event( ts,       EVENT_NOTE_ON,  (unsigned char) note,
                      (unsigned char) m_velocity, false );
    m_seq->add_event( ts + len, EVENT_NOTE_OFF, (unsigned char) note,
                      (unsigned char) m_velocity, false );

    m_seq->verify_and_link();
    m_seq->set_dirty();

    // audible preview through the engine (guarded: no masterbus in headless).
    if ( seq24::app::audio_app_running() )
    {
        unsigned char status = 0x90 | ( m_seq->get_midi_channel() & 0x0F );
        seq24::app::audio_app_route_midi( vst_track(), status,
                                          (unsigned char) note,
                                          (unsigned char) m_velocity );
    }
}

void
TrackerView::set_velocity_at_cell( int vel )
{
    if ( vel < 0 )   vel = 0;
    if ( vel > 127 ) vel = 127;

    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track >= (int) nl.size() )
        return;                             // no note here to re-velocity

    NoteCell nc = nl[m_cursor_track];

    m_seq->push_undo();
    remove_specific_note( nc.ts, nc.note );
    m_seq->add_event( nc.ts, EVENT_NOTE_ON,  (unsigned char) nc.note,
                      (unsigned char) vel, false );
    m_seq->add_event( nc.tf, EVENT_NOTE_OFF, (unsigned char) nc.note,
                      (unsigned char) vel, false );
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

void
TrackerView::clear_note_cell( void )
{
    std::vector<NoteCell> nl;
    collect_row_notes( m_cursor_row, nl );
    if ( m_cursor_track >= (int) nl.size() )
        return;

    m_seq->push_undo();
    remove_specific_note( nl[m_cursor_track].ts, nl[m_cursor_track].note );
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
    if ( seq24::app::audio_app_running() )
    {
        unsigned char status = 0xB0 | ( m_seq->get_midi_channel() & 0x0F );
        seq24::app::audio_app_route_midi( vst_track(), status,
                                          (unsigned char) cc,
                                          (unsigned char) val );
    }
}

// Type a hex value into the FX cell under the cursor.  Callers nibble-shift
// the previous value so pressing "7" then "F" builds 0x7F.
void
TrackerView::set_fx_at_cell( int val )
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 || fi >= m_fx_cols )
        return;

    FxBinding& b = m_fx_bind[m_cursor_track][fi];
    long ts = row_start_tick( m_cursor_row );

    if ( b.type == FX_MIDI_CC )
    {
        m_seq->push_undo();
        set_cc_at( ts, b.cc, val & 0x7f );
    }
    else if ( b.type == FX_VST_PARAM )
    {
        m_fx_vst[m_cursor_track][fi][m_cursor_row] = val & 0xff;
        if ( seq24::app::audio_app_running() )
            seq24::app::audio_app_route_param( vst_track(), b.pid,
                                               ( val & 0xff ) / 255.0f );
    }
    // else: unbound column -> nothing to do
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
    }
    else if ( b.type == FX_VST_PARAM )
    {
        m_fx_vst[m_cursor_track][fi].erase( m_cursor_row );
    }
}

// fire every VST-param FX cell that lives on `row` (from the playhead watcher).
// CC cells are already sequence events, so they play themselves.
void
TrackerView::fire_fx_row( int row )
{
    if ( !seq24::app::audio_app_running() )
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
                seq24::app::audio_app_route_param( track, b.pid,
                                                   ( it->second & 0xff ) / 255.0f );
        }
    }
}

//----------------------------------------------------------------------------
//  FX bindings (driven by a toolbar picker in the shell)
//----------------------------------------------------------------------------
void
TrackerView::bind_fx_none( void )
{
    int fi = cur_fx_index();
    m_fx_bind[m_cursor_track][fi] = FxBinding();
    m_fx_vst[m_cursor_track][fi].clear();
}

void
TrackerView::bind_fx_cc( int cc )
{
    int fi = cur_fx_index();
    FxBinding& b = m_fx_bind[m_cursor_track][fi];
    b.type = FX_MIDI_CC;
    b.cc   = cc;
    b.pid  = 0;
    char lb[8];
    snprintf( lb, sizeof(lb), "C%02d", cc );
    b.label = lb;
    m_fx_vst[m_cursor_track][fi].clear();
}

void
TrackerView::bind_fx_vst( unsigned int pid, const std::string& name )
{
    int fi = cur_fx_index();
    FxBinding& b = m_fx_bind[m_cursor_track][fi];
    b.type = FX_VST_PARAM;
    b.pid  = pid;
    b.cc   = 0;
    b.label = std::string( "P:" ) + name.substr( 0, 3 );
}

std::string
TrackerView::cur_fx_desc( void ) const
{
    int fi = m_cursor_col - 2;
    if ( fi < 0 ) fi = 0;
    if ( fi >= m_fx_cols ) fi = m_fx_cols - 1;
    const FxBinding& b = m_fx_bind[m_cursor_track][fi];
    char buf[32];
    if ( b.type == FX_MIDI_CC )
        snprintf( buf, sizeof(buf), "T%d.F%d CC%d", m_cursor_track, fi, b.cc );
    else if ( b.type == FX_VST_PARAM )
        snprintf( buf, sizeof(buf), "T%d.F%d P%u", m_cursor_track, fi, b.pid );
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
//   [ note(3) ][ sp ][ vel(2) ][ sp ][ fx0(2) ][ sp ][ fx1(2) ][ sp ] ...
void
TrackerView::subcol_geom( int col, int* cx_chars, int* w_chars ) const
{
    if ( col == 0 )      { *cx_chars = 0; *w_chars = 3; }   // note name
    else if ( col == 1 ) { *cx_chars = 4; *w_chars = 2; }   // hex velocity
    else                                                    // FX columns
    {
        int fi = col - 2;
        *cx_chars = 7 + fi * 3;
        *w_chars  = 2;
    }
}

static const int kGutterChars = 4;   // "%3d" row number + 1 space

int
TrackerView::visible_rows( App& app ) const
{
    int ch = app.mono.ch();
    int row_h    = ch + 2;
    int header_h = ch + 4;
    int r = ( rect.h - header_h ) / row_h;
    if ( r < 1 ) r = 1;
    return r;
}

void
TrackerView::ensure_cursor_visible( App& app )
{
    int vis = visible_rows( app );
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
TrackerView::set_rows_per_beat( int rpb )
{
    if ( rpb != 4 && rpb != 8 && rpb != 16 )
        return;
    m_rows_per_beat = rpb;
    m_cursor_row = 0;
    m_top_row = 0;
}

void
TrackerView::set_num_note_cols( int n )
{
    if ( n < 1 ) n = 1;
    if ( n > 8 ) n = 8;
    m_num_tracks = n;
    if ( m_cursor_track >= m_num_tracks )
        m_cursor_track = m_num_tracks - 1;
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
//  playhead
//----------------------------------------------------------------------------
bool
TrackerView::poll_playhead( void )
{
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

    if ( cur != m_last_progress_row )
    {
        m_last_progress_row = cur;
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------
//  drawing
//----------------------------------------------------------------------------
void
TrackerView::draw( App& app )
{
    const Theme& th = theme();
    const int cw = app.mono.cw();
    const int ch = app.mono.ch();
    const int row_h    = ch + 2;
    const int header_h = ch + 4;

    // clip so nothing spills outside our widget rect.
    SDL_RenderSetClipRect( app.ren, &rect );

    fill_rect( app.ren, rect, th.bg );

    const int gutter_px = kGutterChars * cw;
    const int track_px  = track_chars() * cw;
    const int grid_w    = gutter_px + track_px * m_num_tracks;

    long bpm = m_seq->get_bpm();
    if ( bpm < 1 ) bpm = 4;

    // ---- header ----------------------------------------------------------
    SDL_Rect hdr { rect.x, rect.y, grid_w, header_h };
    fill_rect( app.ren, hdr, th.panel );
    int hty = rect.y + 2;
    for ( int t = 0; t < m_num_tracks; ++t )
    {
        int cx = rect.x + gutter_px + t * track_px;

        char tl[8];
        snprintf( tl, sizeof(tl), "T%d", t );
        app.mono.draw( app.ren, cx, hty, tl,
                       t == m_cursor_track ? th.hi : th.dim );

        app.mono.draw( app.ren, cx + 4*cw, hty, "vv", th.dim );

        for ( int f = 0; f < m_fx_cols; ++f )
        {
            int fcx, fcw;
            subcol_geom( 2 + f, &fcx, &fcw );
            FxBinding& b = m_fx_bind[t][f];
            app.mono.draw( app.ren, cx + fcx*cw, hty, b.label,
                           b.type == FX_NONE ? th.dim : th.text );
        }
    }
    hline( app.ren, rect.x, rect.x + grid_w, rect.y + header_h - 1, th.dim );

    // ---- rows ------------------------------------------------------------
    int rows = num_rows();
    int vis  = visible_rows( app );

    std::vector<NoteCell> nl;
    for ( int sr = 0; sr < vis; ++sr )
    {
        int row = m_top_row + sr;
        if ( row >= rows ) break;

        int y = rect.y + header_h + sr * row_h;
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

        hline( app.ren, rect.x, rect.x + grid_w, y, is_beat ? th.dim : th.panel );

        int ty = y + 1;

        // row number gutter.
        char rn[8];
        snprintf( rn, sizeof(rn), "%3d", row );
        app.mono.draw( app.ren, rect.x + cw/2, ty, rn,
                       is_beat ? th.hi : th.dim );

        // gather this row's chord once, reused for every note column.
        collect_row_notes( row, nl );

        for ( int t = 0; t < m_num_tracks; ++t )
        {
            int cx = rect.x + gutter_px + t * track_px;

            bool has = t < (int) nl.size();
            std::string cell_note = has ? note_name( nl[t].note ) : "---";
            char cell_vel[8];
            if ( has ) snprintf( cell_vel, sizeof(cell_vel), "%02X", nl[t].vel & 0x7f );
            else       snprintf( cell_vel, sizeof(cell_vel), "--" );

            app.mono.draw( app.ren, cx, ty, cell_note, has ? th.text : th.dim );
            app.mono.draw( app.ren, cx + 4*cw, ty, cell_vel, has ? th.hi : th.dim );

            for ( int f = 0; f < m_fx_cols; ++f )
            {
                int fcx, fcw;
                subcol_geom( 2 + f, &fcx, &fcw );
                FxBinding& b = m_fx_bind[t][f];

                char fxs[8];
                bool fhas = false;
                int  fval = 0;

                if ( b.type == FX_MIDI_CC )
                    fhas = read_cc_at( row, b.cc, &fval );
                else if ( b.type == FX_VST_PARAM )
                {
                    std::map<int,int>& mp = m_fx_vst[t][f];
                    std::map<int,int>::iterator it = mp.find( row );
                    if ( it != mp.end() ) { fhas = true; fval = it->second; }
                }

                if ( fhas ) snprintf( fxs, sizeof(fxs), "%02X", fval & 0xff );
                else        snprintf( fxs, sizeof(fxs), ".." );

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
    SDL_RenderSetClipRect( app.ren, nullptr );
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
bool
TrackerView::on_mouse( App& app, const MouseEv& e )
{
    // only consume events that land inside our rect (don't swallow the shell's
    // toolbar clicks / releases that happen outside the grid).
    if ( !hit( e.x, e.y ) )
        return false;
    if ( !e.pressed || e.button != SDL_BUTTON_LEFT )
        return true;

    const int cw = app.mono.cw();
    const int ch = app.mono.ch();
    const int row_h    = ch + 2;
    const int header_h = ch + 4;

    int sr  = ( e.y - rect.y - header_h ) / row_h;
    int row = m_top_row + sr;
    if ( e.y - rect.y >= header_h && row >= 0 && row < num_rows() )
        m_cursor_row = row;

    const int gutter_px = kGutterChars * cw;
    const int track_px  = track_chars() * cw;
    int x = e.x - rect.x;
    if ( x >= gutter_px )
    {
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
    }
    app.request_redraw();
    return true;
}

bool
TrackerView::on_wheel( App& app, int /*dx*/, int dy )
{
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
    switch ( k )
    {
        case SDLK_UP:       move_cursor( app, -1, 0 ); return true;
        case SDLK_DOWN:     move_cursor( app,  1, 0 ); return true;
        case SDLK_LEFT:     move_cursor( app,  0,-1 ); return true;
        case SDLK_RIGHT:    move_cursor( app,  0, 1 ); return true;
        case SDLK_TAB:      move_cursor( app,  0, total_subcols() ); return true;
        case SDLK_PAGEUP:   move_cursor( app, -m_rows_per_beat, 0 ); return true;
        case SDLK_PAGEDOWN: move_cursor( app,  m_rows_per_beat, 0 ); return true;
        case SDLK_HOME:     m_cursor_row = 0;
                            ensure_cursor_visible( app );
                            app.request_redraw(); return true;
        case SDLK_END:      m_cursor_row = num_rows() - 1;
                            ensure_cursor_visible( app );
                            app.request_redraw(); return true;

        case SDLK_DELETE:
        case SDLK_PERIOD:
        case SDLK_BACKSPACE:
            if ( m_cursor_col <= 1 )
                clear_note_cell();          // velocity follows the note
            else
                clear_fx_cell();
            move_cursor( app, m_edit_step, 0 );
            return true;
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
            FxBinding& b = m_fx_bind[m_cursor_track][fi];
            int cur = 0;
            if ( b.type == FX_MIDI_CC )
                read_cc_at( m_cursor_row, b.cc, &cur );
            else if ( b.type == FX_VST_PARAM )
            {
                std::map<int,int>& mp = m_fx_vst[m_cursor_track][fi];
                std::map<int,int>::iterator it = mp.find( m_cursor_row );
                if ( it != mp.end() ) cur = it->second;
            }
            set_fx_at_cell( ( ( cur << 4 ) | hv ) & 0xff );
        }
        app.request_redraw();
        return true;
    }

    return false;
}

} // namespace ui
