//----------------------------------------------------------------------------
//  sdlui/views/piano_roll/pianoroll.h
//
//  SDL2 piano-roll editor (seqroll + seqkeys + seqtime +
//  seqdata folded into one retained ui::Widget).  Edits a live PatchKnob
//  `sequence*` in place -- it round-trips the exact NOTE_ON/NOTE_OFF events the
//  engine plays.
//
//  Regions inside the widget rect:
//     +------+---------------------------+--+
//     | (--) | time ruler  (bars/END)    |  |   <- ruler
//     +------+---------------------------+sb|
//     | keys | note grid  (playhead)     |ba|   <- keys + grid + vscroll
//     |strip |                           |r |
//     +------+---------------------------+--+
//     | (--) | velocity lane             |  |   <- data
//     +------+---------------------------+--+
//
//  Interactions (all use the LEFT button, matching the SDL toolkit's input):
//     * click empty grid  -> add note at snap
//     * drag empty grid    -> lasso select
//     * click a note       -> select it (Ctrl adds); drag -> move (multi-drag)
//     * drag a note's right edge -> resize (Shift = stretch)
//     * drag velocity lane -> ramp note velocities
//     * click keyboard strip -> optional MIDI preview
//     * wheel              -> horizontal zoom (anchored at cursor)
//     * Shift+wheel        -> vertical scroll   (Ctrl+wheel = vertical zoom)
//     * vertical scrollbar -> vertical scroll
//     * Delete / Backspace -> remove selected;  A = select all; U = undo
//     * Ctrl+C / Ctrl+X / Ctrl+V -> copy / cut / paste selected notes
//     * Q quantize; Up/Down = transpose semitone (Shift/PageUp/PageDn = octave)
//     * Left/Right = nudge one snap; Ctrl+D duplicate; L legato; H humanize
//     * ] / [ = lengthen / shorten; '=' uniform length; '.' / ',' velocity +/-
//     * R reverse in time; I invert pitch; G cycle snap; Esc deselect; Ctrl+Z undo
//
//  Qtractor-inspired additions:
//     * Ctrl+wheel         -> vertical zoom (row height, anchored at cursor)
//     * V / click data tab -> cycle data-lane event type (Vel/Bend/Prog/ChPr/CCx)
//     * D                  -> drum mode (notes drawn as diamonds at start tick)
//     * K / Shift+K        -> cycle snap-to-scale / cycle scale root
//     * N                  -> normalize selected velocities (loudest -> 127)
//     * Ctrl+[ / Ctrl+]    -> resize: scale selected note lengths x0.5 / x2
//     * { / } (Shift+[ /]) -> rescale: time-stretch selection x0.5 / x2
//     * Ctrl+Left / Right  -> timeshift selection by one beat
//     * B                  -> toggle note-name labels on wide bars
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PIANOROLL_H
#define PATCHKNOB_SDLUI_PIANOROLL_H

#include "gui.h"
#include <vector>

class sequence;   // PatchKnob engine model (src/sequence.h)

namespace ui {

class PianoRoll : public Widget {
public:
    explicit PianoRoll(sequence* seq = nullptr);

    // model -------------------------------------------------------------------
    void set_sequence(sequence* seq);
    sequence* get_sequence() const { return m_seq; }

    // editor parameters -------------------------------------------------------
    void set_snap(int ticks);          // grid snap in MIDI ticks (c_ppqn based)
    void set_note_length(int ticks);   // length of a freshly painted note
    void set_zoom(int ticks_per_px);   // horizontal zoom (1 = most zoomed in)
    void set_preview(bool on) { m_preview = on; }   // emit MIDI from key strip

    // ui::Widget --------------------------------------------------------------
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;

private:
    // --- geometry (recomputed from rect every draw / event) -----------------
    void layout();
    SDL_Rect m_ruler{}, m_keys{}, m_grid{}, m_data{}, m_sbar{};
    // BATCH 1 chrome rects (toolbar strip / horizontal scrollbar / status line)
    SDL_Rect m_toolbar{}, m_hbar{}, m_status{};
    const int tb_h = 20;    // toolbar strip height
    const int hb_h = 12;    // horizontal scrollbar height
    const int st_h = 14;    // status line height

    // --- coordinate helpers --------------------------------------------------
    int  scroll_x() const;
    int  scroll_y() const { return m_scroll_key * m_row_h; }
    int  tick_to_x(long t) const;
    long x_to_tick(int x) const;
    int  note_to_y(int n) const;
    int  y_to_note(int y) const;
    long snap_tick(long t) const;
    int  visible_keys() const;
    long visible_ticks() const;
    void clamp_scroll();
    void set_sequence_length_ticks(long ticks);

    // --- drawing pieces ------------------------------------------------------
    void draw_grid(App& app);
    void draw_keys(App& app);
    void draw_ruler(App& app);
    void draw_data(App& app);
    void draw_notes(App& app);
    void draw_overlay(App& app);
    void draw_playhead(App& app);

    // --- BATCH 1: toolbar / tools / chrome ----------------------------------
    enum Tool { T_EDIT, T_DRAW, T_ERASE, T_SELECT, T_ZOOM, T_PAN };
    void set_tool(int tool);                 // 1..6 keys / toolbar cells

    struct TbCell { SDL_Rect r; int id; std::string label; };
    std::vector<TbCell> m_tb_cells;
    void build_toolbar(App& app);            // recompute cell rects + labels
    void draw_toolbar(App& app);             // paint the strip
    int  toolbar_hit(int x, int y);          // -> cell id (TBI_*), or -1

    void draw_hbar(App& app);                // horizontal scrollbar
    void hbar_set(int x);                    // scroll from pointer x

    void draw_status_bar(App& app);          // bottom coordinate readout
    std::string note_name(int n) const;      // "C#4" style pitch name
    std::string bbt(long tick) const;        // "bar:beat:tick" readout

    bool insert_note(long tick, long len, int note, bool paint);
    bool note_overlaps(long ts, long tf, int note) const;
    void paint_run(long t0, long t1, int note);   // draw-tool note run
    void erase_at(int x, int y);                  // erase-tool delete under pointer
    void zoom_to_box(long ts, long tf, int nh, int nl);  // zoom-tool region
    void zoom_step(int dir, int ax, int ay);             // zoom-tool click

    // in-widget popup (context menu + snap/length dropdowns) ------------------
    struct PopItem { std::string label; int id; bool enabled; };
    std::vector<PopItem> m_popup_items;
    SDL_Rect m_popup{};
    bool m_popup_open = false;
    int  m_popup_kind = 0;      // 0 = context menu, 1 = snap dropdown, 2 = length
    int  m_popup_rowh = 14;
    void open_context_menu(int x, int y);
    void open_dropdown(int kind, int x, int y);
    void layout_popup(int x, int y);
    void draw_popup(App& app);
    int  popup_hit(int y);

    // --- hit test ------------------------------------------------------------
    bool find_note_at(int x, int y, long* ts, long* tf, int* note, bool* edge);
    void capture_selbox();
    void apply_data_drag(App& app);

    // --- Qtractor-inspired additions -----------------------------------------
    void draw_status(App& app);                 // corner: scale + toggle flags
    SDL_Rect data_header_rect() const;          // clickable data-lane type tab
    void cycle_data_type(int d);                // V / tab click: change lane type
    bool note_in_scale(int note) const;         // K : is pitch in current scale?
    int  snap_note_to_scale(int note) const;    // K : nearest in-scale pitch
    void snap_selection_to_scale();             // re-snap selected pitches (no undo)
    void normalize_velocities();                // N : loudest selected -> 127
    void resize_selection(int mul, int div);    // Ctrl+[ / Ctrl+] : scale lengths
    void rescale_selection(int mul, int div);   // { / } : time-stretch about start

    // --- clipboard / edit ops ------------------------------------------------
    void copy_selection();       // Ctrl+C : selection -> sequence clipboard
    void cut_selection();        // Ctrl+X : copy then delete the selection
    void paste_clipboard();      // Ctrl+V : paste clipboard at the cursor
    bool clipboard_has_notes();  // guard: is the shared note clipboard non-empty?
    bool delete_selection();     // Delete/context delete with empty-buffer guard

    // --- selected-note edit ops ----------------------------------------------
    // Each reads the selection, pushes an undo frame, mutates, and leaves the
    // result selected.  Rebuild-style ops go through add_notes() because the
    // engine's add_note() hardcodes velocity 100.
    struct NoteRec { long ts, tf; int note, vel; };            // one linked note
    void collect_selected(std::vector<NoteRec>& out) const;    // selected+linked notes
    void add_notes(const std::vector<NoteRec>& notes, bool select);
    bool move_selection(long dt, int dn);       // drag/arrows with range clamping
    void grow_selection(long delta, bool stretch);
    void duplicate_selection();   // Ctrl+D : clone shifted right by selection span
    void legato_selection();      // L      : extend each note to the next note's start
    void humanize_selection();    // H      : deterministic velocity / timing jitter
    void set_uniform_length();    // '='    : every note -> current note length
    void change_velocity(int d);  // '.'/',': offset velocities (clamp 1..127)
    void reverse_selection();     // R      : mirror note starts in [minStart,maxEnd]
    void invert_selection();      // I      : melodic inversion about average pitch
    unsigned next_rand();         // xorshift32 (NOT rand()) used by humanize
    void preview_note_on(int note);
    void preview_note_off(int note);
    void stop_preview_notes();

    // --- model ---------------------------------------------------------------
    sequence* m_seq = nullptr;

    // --- view state ----------------------------------------------------------
    int  m_zoom        = 6;    // ticks per pixel (horizontal)
    int  m_snap        = 48;   // c_ppqn/4  (16th)
    int  m_note_length = 48;
    int  m_row_h       = 9;    // pixels per key row
    long m_scroll_ticks = 0;
    int  m_scroll_key   = -1;  // -1 == not yet centred
    bool m_preview      = false;

    // Qtractor-inspired view state
    bool m_drum_mode   = false;  // D : diamonds at start tick (ignore length)
    bool m_show_labels = false;  // B : note-name labels on wide bars
    int  m_scale       = 0;      // K : 0 off,1 maj,2 min,3 penta,4 chromatic
    int  m_scale_root  = 0;      // Shift+K : 0..11 (C..B)
    int  m_data_type   = 0;      // V : index into the data-lane event-type table

    // BATCH 1 : edit-tool model + toolbar toggles
    int  m_tool        = T_EDIT; // active toolbar tool (1..6)
    bool m_follow      = false;  // [Foll] : follow playhead (toggle state)
    bool m_show_ghost  = false;  // [Ghost] : show ghost notes (toggle state)
    bool m_show_over   = false;  // [Over] : overview strip (toggle state)
    bool m_dirty_flag  = false;  // status-bar [*] : an edit happened this session
    int  m_cw          = 8;      // cached mono cell width (from last draw)
    int  m_chh         = 14;     // cached mono cell height (from last draw)

    // deterministic humanize RNG: a member counter seeds an evolving xorshift
    // state, so repeated presses vary without ever calling rand()
    unsigned m_rng_state   = 0x2545F491u;
    unsigned m_humanize_ctr = 0;

    // --- interaction ---------------------------------------------------------
    enum Mode { M_NONE, M_ADDPEND, M_SELECT, M_MOVE, M_GROW, M_DATA, M_SBAR, M_KEYS,
                M_PAN, M_HBAR, M_ERASE, M_ZOOM, M_END };
    int  m_mode = M_NONE;
    bool m_down = false, m_dragging = false;
    int  m_drop_x = 0, m_drop_y = 0, m_cur_x = 0, m_cur_y = 0;
    int  m_last_mx = 0, m_last_my = 0;
    long m_drop_tick = 0;  int m_drop_note = 0;
    long m_sel_ts = 0, m_sel_tf = 0;  int m_sel_nh = 0, m_sel_nl = 0;
    int  m_keying_note = -1;
    int  m_prev_note   = -1;   // grid interaction preview note (sounding until release)
};

} // namespace ui
#endif
