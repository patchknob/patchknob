//----------------------------------------------------------------------------
//  sdlui/views/piano_roll/pianoroll.h
//
//  SDL2 port of the seq24 GTK piano-roll editor (seqroll + seqkeys + seqtime +
//  seqdata folded into one retained ui::Widget).  Edits a live seq24
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
//     * Shift+wheel        -> vertical scroll   (Ctrl+wheel = horizontal scroll)
//     * vertical scrollbar -> vertical scroll
//     * Delete / Backspace -> remove selected;  A = select all; U = undo
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_PIANOROLL_H
#define SEQ24_SDLUI_PIANOROLL_H

#include "gui.h"

class sequence;   // seq24 engine model (src/sequence.h)

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

    // --- drawing pieces ------------------------------------------------------
    void draw_grid(App& app);
    void draw_keys(App& app);
    void draw_ruler(App& app);
    void draw_data(App& app);
    void draw_notes(App& app);
    void draw_overlay(App& app);
    void draw_playhead(App& app);

    // --- hit test ------------------------------------------------------------
    bool find_note_at(int x, int y, long* ts, long* tf, int* note, bool* edge);
    void capture_selbox();
    void apply_data_drag(App& app);

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

    // --- interaction ---------------------------------------------------------
    enum Mode { M_NONE, M_ADDPEND, M_SELECT, M_MOVE, M_GROW, M_DATA, M_SBAR, M_KEYS };
    int  m_mode = M_NONE;
    bool m_down = false, m_dragging = false;
    int  m_drop_x = 0, m_drop_y = 0, m_cur_x = 0, m_cur_y = 0;
    int  m_last_mx = 0, m_last_my = 0;
    long m_drop_tick = 0;  int m_drop_note = 0;
    long m_sel_ts = 0, m_sel_tf = 0;  int m_sel_nh = 0, m_sel_nl = 0;
    int  m_keying_note = -1;
};

} // namespace ui
#endif
