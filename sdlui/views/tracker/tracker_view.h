//----------------------------------------------------------------------------
//  sdlui/views/tracker/tracker_view.h
//
//  SDL2 port of the seq24 TRACKER (src/trackeredit.cpp).  A single retained
//  ui::Widget that renders ONE sequence as a multi-column vertical tracker:
//
//    * Note-track columns are a polyphonic-stacking view of the shared
//      sequence: per row, note-ons are gathered and sorted by pitch; note
//      column t shows the t-th-lowest note.  Editing writes the same
//      note-on/off pairs the piano roll uses, so notes round-trip both ways.
//    * Each note column carries a hex velocity cell + N FX-command columns.
//    * An FX column bound to a MIDI CC stores a real EVENT_CONTROL_CHANGE
//      event IN the sequence (plays through the normal engine path).
//    * An FX column bound to a VST parameter stores its value in a per-view
//      map and fires it via audio_app_route_param() on entry and, during
//      playback, from poll_playhead() as the playhead crosses the row.
//    * Note entry via the tracker keyboard (Z S X D C.. lower octave,
//      Q 2 W 3.. upper octave).  Delete/./Backspace clears; arrows/Tab move
//      the cursor; hex keys type velocity / FX values (nibble-accumulate).
//    * Playhead-row highlight from sequence::get_last_tick().
//
//  All colour comes from ui::theme() roles (black&white / phosphor-green) --
//  no hardcoded hues -- so it flips with the runtime theme like every view.
//
//  The class is UI-shell agnostic: mount it as a root ui::Widget sized to a
//  SDL_Rect.  It reads the SAME sequence* the piano roll edits.
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_TRACKER_VIEW_H
#define SEQ24_SDLUI_TRACKER_VIEW_H

#include "gui.h"
#include <map>
#include <vector>
#include <string>

class sequence;   // engine model (src/sequence.h)

namespace ui {

// FX-column binding kinds (mirror trackeredit.h's fx_type).
enum FxType { FX_NONE = 0, FX_MIDI_CC, FX_VST_PARAM };

struct FxBinding
{
    int          type  = FX_NONE;   // FxType
    int          cc    = 0;         // controller number      (FX_MIDI_CC)
    unsigned int pid   = 0;         // VST parameter id        (FX_VST_PARAM)
    std::string  label = "--";      // short header label, e.g. "C74" / "P:Cut"
};

class TrackerView : public Widget
{
public:
    // seq   : the sequence this tracker edits (shared with the piano roll).
    // track : the engine track index == sequence's midi bus (VST routing).
    //         Pass -1 to derive it from seq->get_midi_bus() live.
    TrackerView( sequence* seq, int track = -1 );

    // ---- ui::Widget ------------------------------------------------------
    void draw   ( App& app ) override;
    bool on_mouse( App& app, const MouseEv& e ) override;
    bool on_wheel( App& app, int dx, int dy )   override;
    bool on_key  ( App& app, SDL_Keycode k )    override;

    // ---- toolbar-equivalent controls (the shell drives these) ------------
    void set_rows_per_beat( int rpb );     // 4 / 8 / 16
    int  get_rows_per_beat( void ) const { return m_rows_per_beat; }
    void set_num_note_cols( int n );       // 1..8 polyphonic stacking columns
    int  get_num_note_cols( void ) const { return m_num_tracks; }
    void set_octave( int o );              // 0..8 base octave for note entry
    int  get_octave( void ) const { return m_octave; }
    void set_edit_step( int s );           // 1..8 rows advanced after entry
    int  get_edit_step( void ) const { return m_edit_step; }
    void set_velocity( int v ) { m_velocity = v & 0x7f; }

    // Bind the FX column under the cursor to a CC / VST param / nothing.
    void bind_fx_cc  ( int cc );
    void bind_fx_vst ( unsigned int pid, const std::string& name );
    void bind_fx_none( void );
    std::string cur_fx_desc( void ) const;

    // Playhead watcher.  Call once per frame from the shell; returns true when
    // the playhead row changed (so the shell can request_redraw), and fires any
    // due VST-param FX cells as the playhead advances.
    bool poll_playhead( void );

    long length_measures( void ) const;

private:
    struct NoteCell { int note; int vel; long ts; long tf; };

    // --- model <-> grid helpers ------------------------------------------
    int  vst_track( void ) const;
    int  ticks_per_row( void ) const;
    int  num_rows( void ) const;
    long row_start_tick( int row ) const;
    int  total_subcols( void ) const { return 2 + m_fx_cols; }

    static std::string note_name( int note );
    static int  key_to_pitch( SDL_Keycode k, int* oct_off );
    static int  key_to_hex( SDL_Keycode k );

    void collect_row_notes( int row, std::vector<NoteCell>& out );
    bool note_at_row( int row, int track, int* note, int* vel );
    void remove_specific_note( long ts, int note );
    void set_note_at_cell( int note );
    void set_velocity_at_cell( int vel );
    void clear_note_cell( void );

    int  cur_fx_index( void ) const;
    bool read_cc_at( int row, int cc, int* val );
    void remove_cc_at( long ts, int cc );
    void set_cc_at( long ts, int cc, int val );
    void set_fx_at_cell( int val );
    void clear_fx_cell( void );
    void fire_fx_row( int row );

    // --- geometry (character-cell based; pixels derived from mono font) ---
    void subcol_geom( int col, int* cx_chars, int* w_chars ) const;
    int  track_chars( void ) const { return 7 + m_fx_cols * 3; }
    int  visible_rows( App& app ) const;
    void ensure_cursor_visible( App& app );
    void move_cursor( App& app, int drow, int dcol );

    // --- state -----------------------------------------------------------
    sequence* m_seq;
    int  m_track;           // explicit track override (-1 => from seq)

    int  m_rows_per_beat;
    int  m_num_tracks;      // number of note columns (polyphonic stack)
    int  m_fx_cols;         // FX columns per note column

    int  m_cursor_row, m_cursor_track, m_cursor_col;
    int  m_octave, m_edit_step, m_velocity;
    int  m_top_row;
    int  m_last_progress_row;   // last playhead row painted
    int  m_last_fire_row;       // last row whose VST FX cells were fired

    // FX bindings + VST-param value store, [note column][fx column].
    std::vector< std::vector<FxBinding> >       m_fx_bind;
    std::vector< std::vector<std::map<int,int> > > m_fx_vst;
};

} // namespace ui
#endif
