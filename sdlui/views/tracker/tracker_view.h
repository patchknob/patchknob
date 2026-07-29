//----------------------------------------------------------------------------
//  sdlui/views/tracker/tracker_view.h
//
//  SDL2 port of the PatchKnob TRACKER (src/trackeredit.cpp).  A single retained
//  ui::Widget that renders ONE sequence as a multi-column vertical tracker:
//
//    * Note-track columns are a polyphonic-stacking view of the shared
//      sequence: per row, note-ons are gathered and sorted by pitch; note
//      column t shows the t-th-lowest note.  Editing writes NOTE_ON events
//      that sustain until an explicit OFF is entered with backtick.
//    * Each note column carries a hex velocity cell + N FX-command columns.
//    * An FX column bound to a MIDI CC stores a real EVENT_CONTROL_CHANGE
//      event IN the sequence (plays through the normal engine path).
//    * An FX column bound to an instrument/rack parameter stores a 16-bit value
//      in a per-view map and fires it on entry/playback.
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
#ifndef PATCHKNOB_SDLUI_TRACKER_VIEW_H
#define PATCHKNOB_SDLUI_TRACKER_VIEW_H

#include "gui.h"
#include <functional>
#include <map>
#include <vector>
#include <string>

class sequence;   // engine model (src/sequence.h)

namespace ui {

// FX-column binding kinds (mirror trackeredit.h's fx_type).
enum FxType { FX_NONE = 0, FX_MIDI_CC, FX_VST_PARAM };
enum FxTarget { FX_TARGET_TRACK_PARAM = 0, FX_TARGET_PATCH_PLUGIN_PARAM,
                FX_TARGET_RACK_PARAM };

struct FxBinding
{
    int          type  = FX_NONE;   // FxType
    int          target = FX_TARGET_TRACK_PARAM; // FxTarget for FX_VST_PARAM
    int          cc    = 0;         // controller number      (FX_MIDI_CC)
    unsigned int pid   = 0;         // parameter id/index      (FX_VST_PARAM)
    int          node  = -1;        // patch node id           (patch/rack target)
    int          module = -1;       // rack module id          (FX_TARGET_RACK_PARAM)
    float        min_value = 0.0f;  // rack denormalize range
    float        max_value = 1.0f;
    std::string  label = "--";      // 4-char-ish header label, e.g. "C74" / "CUT"
    std::string  name;              // full menu/tooltip label
};

class TrackerView : public Widget
{
public:
    // seq   : the sequence this tracker edits (shared with the piano roll).
    // track : the engine track index == sequence's midi bus (VST routing).
    //         Pass -1 to derive it from seq->get_midi_bus() live.
    TrackerView( sequence* seq, int track = -1 );

    // Rebind to a different sequence/track (used when opening a timeline clip).
    // FX bindings + VST-param values are PER PATTERN: flush the current view's FX
    // state back to the old sequence, then load the new sequence's own state (or
    // reset to defaults if it has none), so automation never bleeds across
    // patterns.
    void set_sequence( sequence* seq, int track = -1 )
    {
        stop_sounding_notes();
        commit_fx();                 // save current FX + note-column state into the
                                     // OLD sequence FIRST (serialize_fx reads the
                                     // lane / OFF maps, so clearing them before this
                                     // would drop every column on a pattern switch)
        m_explicit_note_offs.clear();
        m_note_lanes.clear();
        m_seq = seq;
        m_track = track;
        m_sel_active = false;
        m_sel_drag = false;
        m_menu_open = false;
        m_cell_clipboard.clear();
        load_current_fx();           // load the NEW pattern's FX (defined in .cpp)
    }
    //! Flush the live pattern's FX edits into its sequence (call before save).
    void commit_fx();
    sequence* get_sequence() const { return m_seq; }

    // ---- ui::Widget ------------------------------------------------------
    void draw   ( App& app ) override;
    bool on_mouse( App& app, const MouseEv& e ) override;
    bool on_wheel( App& app, int dx, int dy )   override;
    bool on_key  ( App& app, SDL_Keycode k )    override;
    bool on_key_up( App& app, SDL_Keycode k )   override;   // key release -> note off

    // ---- toolbar-equivalent controls (the shell drives these) ------------
    // LPB (lines per beat) controls the tracker grid resolution.  Valid
    // values divide the fixed engine PPQN (192), keeping every row on a tick.
    void set_lines_per_beat( int lpb );
    int  get_lines_per_beat( void ) const { return m_rows_per_beat; }
    void set_rows_per_beat( int rpb );     // legacy alias for LPB
    int  get_rows_per_beat( void ) const { return m_rows_per_beat; }
    void set_num_note_cols( int n );       // 1..8 polyphonic stacking columns
    int  get_num_note_cols( void ) const { return m_num_tracks; }
    void set_octave( int o );              // 0..8 base octave for note entry
    int  get_octave( void ) const { return m_octave; }
    void set_edit_step( int s );           // 1..8 rows advanced after entry
    int  get_edit_step( void ) const { return m_edit_step; }
    void set_velocity( int v ) { m_velocity = v & 0x7f; }

    // Grow / shrink the per-note-column FX-command count (clamped 0..8) and
    // re-lay-out on the next draw.  Exposed so the shell can add toolbar buttons
    // (also bound to Ctrl+'='/Ctrl+'+' and Ctrl+'-' in on_key).
    void add_fx_col( void );
    void remove_fx_col( void );
    int  get_num_fx_cols( void ) const { return m_fx_cols; }

    // Selection / clipboard editing (also bound to Ctrl+C / Ctrl+V / Ctrl+I).
    // Exposed so the shell can drive them from toolbar buttons.
    void copy_selection( void );        // marked (or cursor-row) cells -> clipboard
    void cut_selection( void );         // copy then clear marked/cursor cells
    void paste_at_cursor( void );       // clipboard -> cursor row/tick
    void interpolate_selection( void ); // lerp an FX column's selected row span

    // Bind the FX column under the cursor to a CC / VST param / nothing.
    void bind_fx_cc  ( int cc );
    void bind_fx_vst ( unsigned int pid, const std::string& name );
    void bind_fx_target( const FxBinding& binding );
    void bind_fx_none( void );
    std::string cur_fx_desc( void ) const;
    std::function<std::vector<FxBinding>()> on_list_fx_targets;

    // Playhead watcher.  Call once per frame from the shell; returns true when
    // the playhead row changed (so the shell can request_redraw), and fires any
    // due VST-param FX cells as the playhead advances.
    bool poll_playhead( void );

    // Engine-driven pattern-FX playback for ANY sequence (not tied to this view's
    // m_seq): fire its VST-param FX automation for the rows crossed in
    // (lastRow -> curRow].  The shell calls this every frame for every ACTIVE
    // pattern so automation plays regardless of which window is focused and for
    // all patterns on a track -- not only the one shown in the tracker.
    static void play_pattern_fx( sequence* s, int lastRow, int curRow, int nrows );

    long length_measures( void ) const;

private:
#ifdef PATCHKNOB_TRACKER_TEST
    friend struct TrackerViewTestAccess;
#endif
    struct NoteCell
    {
        int note = -1;
        int vel = 0;
        long ts = 0;
        long tf = 0;
        bool has_off = false;
        int order = 0;
        int occurrence = 0;
    };
    enum ClipCellKind { CLIP_NOTE = 0, CLIP_OFF, CLIP_VEL, CLIP_FX };
    struct ClipCell
    {
        int dr = 0, dt = 0, dc = 0;
        int kind = CLIP_NOTE;
        int note = -1, vel = 0, value = 0;
    };

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
    void collect_row_note_offs( int row, std::vector<NoteCell>& out );
    bool note_before_row_in_column( int row, int track, NoteCell* out );
    bool end_unreleased_note_before( int row, int track, long end_tick );
    bool note_at_row( int row, int track, int* note, int* vel );
    long normalized_note_end( long tick_s, long tick_f ) const;
    struct NoteKey
    {
        long ts = 0;
        int note = -1;
        int occurrence = 0;
        bool operator<( const NoteKey& rhs ) const
        {
            if ( ts != rhs.ts ) return ts < rhs.ts;
            if ( note != rhs.note ) return note < rhs.note;
            return occurrence < rhs.occurrence;
        }
    };
    NoteKey note_key( long ts, int note, int occurrence = 0 ) const;
    int  note_occurrence_count( long ts, int note ) const;
    bool has_explicit_note_off( long ts, int note, int occurrence,
                                long* off_tick = 0,
                                int* track = 0 ) const;
    int  assigned_note_lane( long ts, int note, int occurrence ) const;
    void set_note_lane( long ts, int note, int occurrence, int track );
    void clear_note_lane( long ts, int note, int occurrence );
    void remove_specific_note( long ts, int note, int occurrence );
    void remove_specific_note( const NoteCell& nc );
    void clear_explicit_note_off( long ts, int note, int occurrence );
    void set_explicit_note_off( long ts, int note, int occurrence,
                                long off_tick, int track );
    bool add_note_on( long ts, int note, int vel );
    bool add_note_pair( long ts, long tf, int note, int vel );
    bool add_note_cell( long ts, long tf, int note, int vel, bool has_off );
    bool restore_default_note_off( const NoteCell& nc );
    bool clear_note_off_cell( void );
    void set_note_at_cell( int note );
    void set_velocity_at_cell( int vel );
    void clear_note_cell( void );

    int  cur_fx_index( void ) const;
    bool read_cc_at( int row, int cc, int* val );
    void remove_cc_at( long ts, int cc );
    void set_cc_at( long ts, int cc, int val );
    bool read_fx_value( int row, int track, int fi, int* val );
    void set_fx_value_at( int row, int track, int fi, int val, bool push_undo );
    void route_fx_value( const FxBinding& binding, int track, int val, int column = -1 );
    void set_fx_at_cell( int val );
    void clear_fx_cell( void );
    void fire_fx_row( int row );

    // --- extended editing features (keyboard-bound; see on_key) -----------
    void collect_all_notes( std::vector<NoteCell>& out );
    void collect_sounding_notes( int row, std::vector<NoteCell>& out );
    void block_region( int* r0, int* r1, int* track ) const;
    void selection_region( int* r0, int* r1, int* t0, int* t1,
                           int* c0, int* c1 ) const;
    bool selection_is_single_column( void ) const;
    void select_all_cells( void );
    void clear_selection_cells( bool push_undo );
    void apply_velocities( const std::vector<NoteCell>& tgt,
                           const std::vector<int>& vel );

    void transpose_selection( int delta_note );   // Ctrl+Up/Down (+/-oct w/Shift)
    void note_off_at_cell( void );                 // backtick on note col only
    void interpolate_velocity( void );             // Ctrl+Shift+I
    void clear_row( void );                        // Ctrl+Delete
    void clear_pattern( void );                    // Ctrl+Shift+Backspace
    void insert_row( void );                       // Insert
    void delete_row( void );                       // Shift+Delete
    void duplicate_selection( void );              // Ctrl+D
    void humanize_selection( void );               // Ctrl+H
    void amplify_selection( int delta );           // Ctrl+'.' / Ctrl+','
    void toggle_mute_column( void );               // Ctrl+M
    void stop_sounding_notes( void );

    // --- geometry (character-cell based; pixels derived from mono font) ---
    void subcol_geom( int col, int* cx_chars, int* w_chars ) const;
    int  track_chars( void ) const { return 7 + m_fx_cols * 5; }
    int  visible_rows( App& app ) const;

    // --- in-canvas menus --------------------------------------------------
    struct MenuItem
    {
        std::string           label;
        bool                  enabled   = true;
        bool                  separator = false;
        std::function<void()> action;
        // If set, clicking DRILLS DOWN: the menu is rebuilt in place (instrument
        // -> params) instead of closing.  Takes App& so it can re-layout.
        std::function<void(App&)> submenu;
    };
    void open_context_menu( App& app, int sx, int sy );
    void open_fx_menu( App& app, int sx, int sy, int track, int fi );
    void build_fx_menu_root( App& app );                       // top: None/CC/instruments
    void build_fx_menu_node( App& app, int nodeId,             // one instrument's params
                             const std::string& nodeName );
    int  m_fxmenu_track = -1, m_fxmenu_fi = -1;                 // FX menu context
    void layout_menu( App& app );
    void draw_menu( App& app );
    void close_menu( void );

    // --- header control buttons (add / remove NOTE and FX columns) --------
    // A one-text-row "toolbar" band drawn above the column-label header hosts
    // four bracket buttons: [-]/[+] for NOTE columns and [-]/[+] for FX columns.
    // Their pixel rects are recomputed every draw() (they move as the column
    // count changes) and hit-tested in on_mouse BEFORE any cell mapping.
    enum HdrButton { HDR_NONE = 0, HDR_NOTE_MINUS, HDR_NOTE_PLUS,
                     HDR_FX_MINUS, HDR_FX_PLUS,
                     HDR_LPB_MINUS, HDR_LPB_PLUS,
                     HDR_OCT_MINUS, HDR_OCT_PLUS };
    int  hdr_button_at( int px, int py ) const;   // -> HdrButton (0 = miss)
    void ensure_cursor_visible( App& app );
    void move_cursor( App& app, int drow, int dcol );
    void set_pattern_lines( int lines );
    int  pattern_lines( void ) const;
    void sync_lines_edit( void );
    void begin_lines_edit( App& app );

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

    // FX bindings + 16-bit FX value store, [note column][fx column].  PER PATTERN:
    // swapped in/out via serialize_fx/deserialize_fx on set_sequence so one
    // pattern's automation never appears in another.  Only VST-param values are
    // shadowed here; MIDI-CC values live in the sequence's own CC events.
    std::vector< std::vector<FxBinding> >       m_fx_bind;
    std::vector< std::vector<std::map<int,int> > > m_fx_vst;
    void        reset_fx_defaults();          // fresh bindings (CC74/CC7) + empty values
    std::string serialize_fx() const;         // pack m_fx_cols + bindings + VST values
    void        deserialize_fx( const std::string& blob );  // unpack (empty -> defaults)
    void        load_current_fx();            // deserialize from m_seq's blob (.cpp)

    // Multi-cell selection (mouse click-drag): an inclusive row/column rectangle
    // spanning note columns and subcolumns. m_sel_track/m_sel_col are kept as the
    // anchor/primary column for legacy column commands.
    bool m_sel_active;
    bool m_sel_drag;
    int  m_sel_anchor_row;      // row where the drag began
    int  m_sel_anchor_track;
    int  m_sel_anchor_col;
    int  m_sel_row0, m_sel_row1; // normalized inclusive row span
    int  m_sel_track;           // note-column the block lives in
    int  m_sel_col;             // sub-column (0 note, 1 vel, 2+ fx) of the block
    int  m_sel_track0, m_sel_track1;
    int  m_sel_col0, m_sel_col1;
    bool m_copied;
    std::vector<ClipCell> m_cell_clipboard;

    // Follow-playback: when true, poll_playhead() keeps the cursor row pinned
    // to the playhead (Ctrl+F).  m_vis_rows caches visible_rows() from the last
    // draw()/ensure_cursor_visible() so poll_playhead (which has no App&) can
    // scroll.  m_col_muted is a per-note-column VISUAL mute (see note in .cpp:
    // the engine plays every event, so this does not suppress audio).
    bool m_follow;
    int  m_vis_rows;
    bool m_col_muted[8];

    // Header-button state: which control button is currently held (HdrButton,
    // 0 = none) for press feedback, plus the last-drawn pixel rect of each so
    // on_mouse can hit-test them without re-deriving the header layout.
    int      m_hdr_press = 0;
    SDL_Rect m_btn_note_minus { 0,0,0,0 };
    SDL_Rect m_btn_note_plus  { 0,0,0,0 };
    SDL_Rect m_btn_fx_minus   { 0,0,0,0 };
    SDL_Rect m_btn_fx_plus    { 0,0,0,0 };
    SDL_Rect m_btn_lpb_minus  { 0,0,0,0 };
    SDL_Rect m_btn_lpb_plus   { 0,0,0,0 };
    SDL_Rect m_btn_oct_minus  { 0,0,0,0 };
    SDL_Rect m_btn_oct_plus   { 0,0,0,0 };
    SDL_Rect m_lines_box      { 0,0,0,0 };
    std::string m_lines_edit;

    bool                  m_menu_open = false;
    int                   m_menu_x = 0, m_menu_y = 0;
    int                   m_menu_w = 0, m_menu_h = 0;
    int                   m_menu_scroll = 0;
    std::vector<MenuItem> m_menu_items;

    // Live keyboard preview: keycode -> the MIDI note it is currently sounding
    // (via m_seq->play_note_on).  Key release (on_key_up) fires play_note_off so
    // "depress a key -> the note turns off".  Routed to the clip's selected
    // instrument through the sequence's MIDI channel.
    std::map<SDL_Keycode,int> m_sounding;

    // Tracker-visible OFF tokens.  The engine stores only MIDI events, which
    // cannot identify a particular overlapping same-pitch note-off.  Keep the
    // column chosen by the user alongside the source note and off tick so the
    // OFF remains in the cell where backtick placed it.
    struct ExplicitNoteOff { long tick; int track; };
    std::map< NoteKey, ExplicitNoteOff > m_explicit_note_offs;

    // Tracker note-column lanes.  MIDI events do not carry a "tracker column",
    // so without this a note inserted in column 3 is redrawn in column 1 because
    // row display was derived from pitch-sorted events.  New tracker edits write
    // their visual lane here; legacy/unmapped notes still pack into free lanes.
    std::map< NoteKey, int > m_note_lanes;
};

} // namespace ui
#endif
