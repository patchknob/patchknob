//----------------------------------------------------------------------------
//  sdlui/views/piano_roll/pianoroll.cpp  -- see pianoroll.h
//
//  Ported 1:1 from the legacy piano-roll pieces:
//     src/seqroll.cpp   (grid, add/move/resize/lasso, snap, playhead)
//     src/seqkeys.cpp   (left keyboard strip)
//     src/seqtime.cpp   (top bar ruler + END marker)
//     src/seqdata.cpp   (bottom velocity lane + ramp drag)
//  All editing goes through the same `sequence` API the engine uses, so the
//  notes drawn here are exactly the events played back.
//----------------------------------------------------------------------------
#include "pianoroll.h"
#include "sequence.h"
#include "event.h"
#include "globals.h"
#include "quantize.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <utility>          // std::pair -- the (status, cc) kind list

namespace ui {

// Full keyboard height mirror: keep a local row height, and reuse the engine's
// ppqn / key count so ticks and notes line up exactly.
static const int K = c_num_keys;   // 128

// ---- data-lane event types (cycled with 'V' / clicking the lane tab) -------
// Each entry names a MIDI status the PatchKnob engine already round-trips.  The
// engine's change_event_data_range() sets d1 for NOTE_ON / CONTROL_CHANGE /
// PITCH_WHEEL and d0 for PROGRAM_CHANGE / CHANNEL_PRESSURE, so the lane reads /
// writes whichever byte carries the value.  Pitch bend is edited coarsely as
// the MSB (d1) only -- a deliberate two-byte simplification for this ASCII UI.
struct DataLane { unsigned char status; unsigned char cc; const char* label; };
static const DataLane g_lanes[] = {
    { EVENT_NOTE_ON,          0, "VEL"  },
    { EVENT_PITCH_WHEEL,      0, "BEND" },
    { EVENT_PROGRAM_CHANGE,   0, "PROG" },
    { EVENT_CHANNEL_PRESSURE, 0, "CHPR" },
    { EVENT_CONTROL_CHANGE,   1, "CC01" },  // mod wheel
    { EVENT_CONTROL_CHANGE,   7, "CC07" },  // volume
    { EVENT_CONTROL_CHANGE,  10, "CC10" },  // pan
    { EVENT_CONTROL_CHANGE,  11, "CC11" },  // expression
    { EVENT_CONTROL_CHANGE,  74, "CC74" },  // brightness
};
static const int G_NLANES = (int)(sizeof(g_lanes) / sizeof(g_lanes[0]));

// pitch-class names for note labels + scale/root readouts (ASCII only)
static const char* const g_pc_names[12] =
    { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

static bool piano_black_key(int note)
{
    const int pc = ((note % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// scale membership masks, one bool per pitch-class relative to the root.
// 0 == off and 4 == chromatic both return null (identity, no snapping).
static const bool* scale_mask(int scale)
{
    static const bool major[12] = {1,0,1,0,1,1,0,1,0,1,0,1};  // 0 2 4 5 7 9 11
    static const bool minor[12] = {1,0,1,1,0,1,0,1,1,0,1,0};  // 0 2 3 5 7 8 10
    static const bool penta[12] = {1,0,1,0,1,0,0,1,0,1,0,0};  // 0 2 4 7 9 (maj pent)
    switch (scale) {
        case 1: return major;
        case 2: return minor;
        case 3: return penta;
        default: return nullptr;
    }
}

// ---- BATCH 1: toolbar cell ids ---------------------------------------------
// Tool cells 0..5 line up with PianoRoll::Tool so (id - TBI_EDIT) is the tool.
enum {
    TBI_EDIT = 0, TBI_DRAW, TBI_ERASE, TBI_SELECT, TBI_ZOOM, TBI_PAN,
    TBI_SNAP, TBI_LEN,
    TBI_ZOUT, TBI_ZIN, TBI_ZRESET,
    TBI_VDN, TBI_VUP,
    TBI_FOLL, TBI_SCALE, TBI_DRUM, TBI_LBL, TBI_GHOST, TBI_OVER
};

// ---- context-menu item ids (kept clear of tick-value dropdown ids) ---------
enum {
    CM_CUT = 1000, CM_COPY, CM_PASTE, CM_DELETE,
    CM_QUANT, CM_LEGATO, CM_HUMAN,
    CM_SELALL, CM_SELNONE, CM_INVERT
};

// snap / note-length dropdown choices (MIDI ticks); ids ARE these tick values.
static const int g_divs[] = {
    4 * c_ppqn, 2 * c_ppqn, c_ppqn, c_ppqn / 2, c_ppqn / 4, c_ppqn / 8, c_ppqn / 16
};
static const int G_NDIVS = (int)(sizeof(g_divs) / sizeof(g_divs[0]));

// "1/16" style label for a tick count (whole note == 4*c_ppqn ticks).
static std::string frac_label(int ticks)
{
    if (ticks <= 0) return "?";
    const int whole = 4 * c_ppqn;
    if (whole % ticks == 0) {
        int d = whole / ticks;
        char b[16]; snprintf(b, sizeof b, "1/%d", d);
        return b;
    }
    char b[16]; snprintf(b, sizeof b, "%dt", ticks);
    return b;
}

// filled rhombus (drum-mode note head) drawn scanline-by-scanline, two-tone.
static void fill_diamond(SDL_Renderer* r, int cx, int cy, int half, Color c)
{
    if (half < 0) return;
    for (int dy = -half; dy <= half; ++dy) {
        int w = half - std::abs(dy);
        hline(r, cx - w, cx + w + 1, cy + dy, c);
    }
}

// ---------------------------------------------------------------------------
PianoRoll::PianoRoll(sequence* seq) : m_seq(seq)
{
    m_snap        = c_ppqn / 4;     // 16th note
    m_note_length = c_ppqn / 4;
}

void PianoRoll::set_sequence(sequence* seq)
{
    stop_preview_notes();
    m_seq = seq;
    m_down = false;
    m_dragging = false;
    m_mode = M_NONE;
    m_popup_open = false;
}
void PianoRoll::set_snap(int t)        { m_snap = t > 0 ? t : 1; }
void PianoRoll::set_note_length(int t) { m_note_length = t > 0 ? t : 1; }
void PianoRoll::set_zoom(int z)        { m_zoom = z < 1 ? 1 : (z > 64 ? 64 : z); }

void PianoRoll::preview_note_on(int note)
{
    if (!m_seq || note < 0 || note >= K) return;
    m_seq->play_note_on(note);
}

void PianoRoll::preview_note_off(int note)
{
    if (!m_seq || note < 0 || note >= K) return;
    m_seq->play_note_off(note);
}

void PianoRoll::stop_preview_notes()
{
    if (m_prev_note >= 0) {
        preview_note_off(m_prev_note);
        m_prev_note = -1;
    }
    if (m_keying_note >= 0) {
        preview_note_off(m_keying_note);
        m_keying_note = -1;
    }
}

void PianoRoll::cancel_interaction(App& app)
{
    stop_preview_notes();
    if(m_seq) m_seq->unpaint_all();
    m_down=false; m_dragging=false; m_mode=M_NONE; m_popup_open=false;
    m_keying_note=-1; m_prev_note=-1;
    m_len_drag_loop_valid=false;   // the END gesture is over; see on_mouse

    app.request_redraw();
}

// ---- geometry -------------------------------------------------------------
void PianoRoll::layout()
{
    const int key_w  = 42;
    const int time_h = 16;
    const int sb_w   = 12;
    const int gap    = 3;
    int data_h = rect.h / 5;                 // ~20% for velocity lane
    if (data_h < 46) data_h = 46;
    if (data_h > 110) data_h = 110;

    // BATCH 1: reserve a top toolbar strip, a horizontal scrollbar under the
    // grid and a full-width status line at the bottom; everything else shifts
    // down by tb_h and the grid loses hb_h + gap + st_h of vertical budget.
    const int top = rect.y + tb_h;
    int grid_h = rect.h - tb_h - time_h - hb_h - gap - data_h - st_h;
    if (grid_h < m_row_h) grid_h = m_row_h;
    int grid_w = rect.w - key_w - sb_w;
    if (grid_w < 1) grid_w = 1;

    m_toolbar = { rect.x, rect.y, rect.w, tb_h };
    m_ruler = { rect.x + key_w, top,          grid_w, time_h };
    m_keys  = { rect.x,         top + time_h, key_w,  grid_h };
    m_grid  = { rect.x + key_w, top + time_h, grid_w, grid_h };
    m_sbar  = { m_grid.x + grid_w, top + time_h, sb_w, grid_h };
    m_hbar  = { m_grid.x, m_grid.y + grid_h, grid_w, hb_h };
    m_data  = { m_grid.x, m_hbar.y + hb_h + gap, grid_w, data_h };
    m_status= { rect.x, m_data.y + data_h, rect.w, st_h };

    // first-time vertical centring on the middle of the keyboard
    if (m_scroll_key < 0) {
        int vis = visible_keys();
        m_scroll_key = 61 - vis / 2;          // note 66 in the middle
    }
    clamp_scroll();
}

int  PianoRoll::visible_keys()  const { return m_grid.h / m_row_h + 1; }
long PianoRoll::visible_ticks() const { return (long)m_grid.w * m_zoom; }
int  PianoRoll::scroll_x()      const { return int(m_scroll_ticks / m_zoom); }

int  PianoRoll::tick_to_x(long t) const { return m_grid.x + int(t / m_zoom) - scroll_x(); }
long PianoRoll::x_to_tick(int x)  const {
    long t = (long)(x - m_grid.x + scroll_x()) * m_zoom;
    return t < 0 ? 0 : t;
}
int  PianoRoll::note_to_y(int n) const { return m_grid.y + (K - 1 - n) * m_row_h - scroll_y(); }
int  PianoRoll::y_to_note(int y) const {
    int yy = y - m_grid.y + scroll_y();
    if (yy < 0) yy = 0;
    int n = (K - 1) - yy / m_row_h;
    return n < 0 ? 0 : (n > K - 1 ? K - 1 : n);
}
long PianoRoll::snap_tick(long t) const {
    if (m_snap <= 0) return t;
    // THE shared quantiser (src/quantize.h), same as record quantise and the
    // offline pattern quantise.  This used to floor with `t - (t % m_snap)`,
    // so dragging a note always dropped it on the line BEHIND the pointer --
    // and, because C++ % keeps the sign, a negative tick snapped the wrong way
    // entirely.  Nearest-line snapping is what the pointer actually indicates.
    return PatchKnob::quantize::snap(t, m_snap);
}

void PianoRoll::clamp_scroll()
{
    int vis = visible_keys();
    int maxk = K - vis; if (maxk < 0) maxk = 0;
    if (m_scroll_key < 0)    m_scroll_key = 0;
    if (m_scroll_key > maxk) m_scroll_key = maxk;

    long len = m_seq ? m_seq->get_length() : (4 * c_ppqn);
    long maxt = len - visible_ticks();
    if (maxt < 0) maxt = 0;
    if (m_scroll_ticks < 0)    m_scroll_ticks = 0;
    if (m_scroll_ticks > maxt) m_scroll_ticks = maxt;
}

void PianoRoll::set_sequence_length_ticks(long ticks)
{
    if (!m_seq) return;
    long step = m_snap > 0 ? m_snap : (c_ppqn / 4);
    if (step < 1) step = 1;
    ticks = snap_tick(ticks);
    if (ticks < step) ticks = step;
    if (ticks == m_seq->get_length()) return;
    m_seq->set_length(ticks, false);
    // sequence::set_length CLAMPS the loop window into the new length and, by
    // design, never restores it when the pattern grows back.  For a live END
    // drag that made a leftward wobble permanently destroy the loop the user
    // had set: drag END in past the loop end, drag it straight back out, and
    // the band was gone.  The gesture remembers the window at press time and
    // re-applies as much of it as the current length allows, so END is a
    // reversible edit for as long as the button is held.
    if (m_len_drag_loop_valid) {
        const long L  = m_seq->get_length();
        long e = m_len_drag_loop_end   < L ? m_len_drag_loop_end   : L;
        long s = m_len_drag_loop_start < e ? m_len_drag_loop_start : e;
        // Direction-dependent order, same reason as in
        // set_sequence_loop_range_ticks(): whichever bound must move out of the
        // way goes first, or the other's clamp truncates the restored window.
        if (s >= m_seq->get_loop_end()) { m_seq->set_loop_end(e);   m_seq->set_loop_start(s); }
        else                            { m_seq->set_loop_start(s); m_seq->set_loop_end(e); }
        if (on_loop_changed) on_loop_changed();
    }
    m_seq->set_dirty();
    m_dirty_flag = true;
    clamp_scroll();
}

void PianoRoll::set_sequence_loop_start_ticks(long ticks)
{
    if (!m_seq) return;
    ticks = snap_tick(ticks);
    if (ticks < 0) ticks = 0;
    // The loop window lives INSIDE the pattern's data: it can never start past
    // the END marker.  Without this clamp a press in the empty ruler beyond the
    // pattern end (easy whenever the pattern is shorter than the widget is
    // wide) drove loop_start into sequence::set_loop_start's own length-1
    // clamp, leaving a 1-tick sliver loop pinned at the end -- invisible in the
    // ruler but enough to wash out the whole tracker grid.
    if (ticks > m_seq->get_length()) ticks = m_seq->get_length();
    // Never let an edge drag COLLAPSE the window.  A zero-width [x,x) reads as
    // "no loop" to sequence::play_span (which needs end > start) while the
    // ruler still shows a stem and the hit test still hands out its edges, so
    // the clip quietly went back to repeating its whole length with a mark on
    // screen claiming otherwise.  Keep at least one snap step of width.
    { const long step = m_snap > 0 ? m_snap : (long)(c_ppqn / 4);
      const long lim  = m_seq->get_loop_end() - (step > 0 ? step : 1);
      if (ticks > lim) ticks = lim > 0 ? lim : 0; }
    if (ticks == m_seq->get_loop_start()) return;
    m_seq->set_loop_start(ticks);   // clamps to [0, length) itself
    m_seq->set_dirty();
    m_dirty_flag = true;
    if (on_loop_changed) on_loop_changed();
}

void PianoRoll::set_sequence_loop_end_ticks(long ticks)
{
    if (!m_seq) return;
    ticks = snap_tick(ticks);
    if (ticks < 0) ticks = 0;
    if (ticks > m_seq->get_length()) ticks = m_seq->get_length();   // never past END
    { const long step = m_snap > 0 ? m_snap : (long)(c_ppqn / 4);   // see above:
      const long lim  = m_seq->get_loop_start() + (step > 0 ? step : 1);
      if (ticks < lim) ticks = lim < m_seq->get_length() ? lim : m_seq->get_length(); }
    if (ticks == m_seq->get_loop_end()) return;
    m_seq->set_loop_end(ticks);   // clamps into [loop_start, length] itself
    m_seq->set_dirty();
    m_dirty_flag = true;
    if (on_loop_changed) on_loop_changed();
}

// Sets the loop window WITHOUT touching the pattern's own length -- the loop
// is a sub-region positionable anywhere inside a longer pattern, with content
// before/after it still present (see sequence::m_loop_start's comment).
void PianoRoll::set_sequence_loop_range_ticks(long start_ticks, long end_ticks)
{
    if (!m_seq) return;
    long step = m_snap > 0 ? m_snap : (c_ppqn / 4);
    if (step < 1) step = 1;
    start_ticks = snap_tick(start_ticks);
    end_ticks   = snap_tick(end_ticks);
    if (start_ticks < 0) start_ticks = 0;
    // The window is a sub-region of the DATA, so the END marker bounds it at
    // both ends.  Clamp the whole span here (keeping its width where there is
    // room) rather than letting sequence's per-bound clamps collapse a
    // dragged-past-the-end region into a 1-tick sliver at the pattern end.
    const long L = m_seq->get_length();
    if (start_ticks > L - step) start_ticks = L - step > 0 ? L - step : 0;
    if (end_ticks < start_ticks + step) end_ticks = start_ticks + step;
    if (end_ticks > L) {
        end_ticks = L;
        if (start_ticks > end_ticks - step)
            start_ticks = end_ticks - step > 0 ? end_ticks - step : 0;
    }
    //  ORDER MATTERS, AND IT DEPENDS ON THE DIRECTION.  Each setter clamps
    //  against the CURRENT value of the other (set_loop_start against loop_end,
    //  set_loop_end against loop_start), so the bound that has to move OUT OF
    //  THE WAY must be written first or the stale one truncates the new window.
    //  This wrote the end unconditionally first, which is right only when the
    //  new region lies to the RIGHT: dragging a fresh region entirely to the
    //  LEFT of the existing one had set_loop_end() clamped up to the old
    //  loop_start, and the set_loop_start() that followed could not pull it back
    //  -- the user let go and got [new_start, OLD_start), a window they never
    //  drew.  Same shape as TrackerView::set_loop_from_selection(), so the two
    //  editors agree.
    if (start_ticks >= m_seq->get_loop_end()) {
        m_seq->set_loop_end(end_ticks);
        m_seq->set_loop_start(start_ticks);
    } else {
        m_seq->set_loop_start(start_ticks);
        m_seq->set_loop_end(end_ticks);
    }
    m_seq->set_dirty();
    m_dirty_flag = true;
    if (on_loop_changed) on_loop_changed();
}

// ---- snap-to-scale (K) ----------------------------------------------------
bool PianoRoll::note_in_scale(int note) const
{
    const bool* m = scale_mask(m_scale);
    if (!m) return true;                                   // off / chromatic
    int root = ((m_scale_root % 12) + 12) % 12;
    int pc   = (((note - root) % 12) + 12) % 12;
    return m[pc];
}

// nearest in-scale pitch to `note`; ties resolve downward.  Identity when the
// scale is off or chromatic (mask == null).  Result clamped to 0..127.
int PianoRoll::snap_note_to_scale(int note) const
{
    const bool* m = scale_mask(m_scale);
    if (!m) return note;
    int root = ((m_scale_root % 12) + 12) % 12;
    for (int dist = 0; dist <= 6; ++dist) {
        int down = note - dist, up = note + dist;
        if (down >= 0  && m[(((down - root) % 12) + 12) % 12]) return down;
        if (up   <= 127 && m[(((up   - root) % 12) + 12) % 12]) return up;
    }
    return note;
}

// ---- data-lane type tab (V / click) ---------------------------------------
SDL_Rect PianoRoll::data_header_rect() const
{
    return SDL_Rect{ m_data.x + 1, m_data.y + 1, 40, 13 };
}

void PianoRoll::cycle_data_type(int d)
{
    m_data_type = (((m_data_type + d) % G_NLANES) + G_NLANES) % G_NLANES;
}

// ===========================================================================
//  BATCH 1 : TOOL MODEL + TOOLBAR + CHROME
// ===========================================================================
void PianoRoll::set_tool(int tool)
{
    if (tool < T_EDIT) tool = T_EDIT;
    if (tool > T_PAN)  tool = T_PAN;
    m_tool = tool;
}

// Recompute the toolbar cell rects + labels left->right.  Both draw_toolbar()
// and toolbar_hit() call this so the painted cells and the hit map never drift.
void PianoRoll::build_toolbar(App& app)
{
    m_tb_cells.clear();
    int x = m_toolbar.x + 2;
    const int y = m_toolbar.y + 1;
    const int h = m_toolbar.h - 2;

    auto add = [&](const std::string& lbl, int id) {
        int w = app.mono.text_w(lbl) + 6;
        m_tb_cells.push_back(TbCell{ SDL_Rect{ x, y, w, h }, id, lbl });
        x += w + 1;
    };
    auto sep = [&]() { x += 7; };

    add("Edit", TBI_EDIT); add("Draw", TBI_DRAW);  add("Ers", TBI_ERASE);
    add("Sel",  TBI_SELECT); add("Zoom", TBI_ZOOM); add("Pan", TBI_PAN);
    sep();
    add("Snap:" + frac_label(m_snap) + " v", TBI_SNAP);
    add("Len:"  + frac_label(m_note_length) + " v", TBI_LEN);
    sep();
    add("Z-", TBI_ZOUT); add("Z+", TBI_ZIN); add("Z0", TBI_ZRESET);
    add("V-", TBI_VDN);  add("V+", TBI_VUP);
    sep();
    add("Foll", TBI_FOLL); add("Scale", TBI_SCALE); add("Drum", TBI_DRUM);
    add("Lbl", TBI_LBL);   add("Ghost", TBI_GHOST); add("Over", TBI_OVER);
}

void PianoRoll::draw_toolbar(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    build_toolbar(app);

    fill_rect(r, m_toolbar, t.panel);
    frame_rect(r, m_toolbar, t.dim);

    for (const TbCell& c : m_tb_cells) {
        // a cell reads "on" when it is the active tool or an engaged toggle
        bool on = false;
        switch (c.id) {
            case TBI_EDIT: case TBI_DRAW: case TBI_ERASE:
            case TBI_SELECT: case TBI_ZOOM: case TBI_PAN:
                on = (m_tool == c.id - TBI_EDIT); break;
            case TBI_FOLL:  on = m_follow;      break;
            case TBI_SCALE: on = (m_scale > 0); break;
            case TBI_DRUM:  on = m_drum_mode;   break;
            case TBI_LBL:   on = m_show_labels; break;
            case TBI_GHOST: on = m_show_ghost;  break;
            case TBI_OVER:  on = m_show_over;   break;
            default: break;
        }
        fill_rect(r, c.r, on ? t.accent : t.panel);
        frame_rect(r, c.r, t.dim);
        app.mono.draw(r, c.r.x + 3, c.r.y + (c.r.h - app.mono.ch()) / 2,
                      c.label, on ? t.bg : t.text);
    }
}

int PianoRoll::toolbar_hit(int x, int y)
{
    SDL_Point p{ x, y };
    for (const TbCell& c : m_tb_cells)
        if (SDL_PointInRect(&p, &c.r)) return c.id;
    return -1;
}

// ---- horizontal scrollbar --------------------------------------------------
void PianoRoll::draw_hbar(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    fill_rect(r, m_hbar, t.dim);                        // track

    long len = m_seq ? m_seq->get_length() : (long)(4 * c_ppqn);
    if (len < 1) len = 1;
    long vis = visible_ticks(); if (vis > len) vis = len;

    int tw = m_hbar.w;
    int thumb_w = (int)((double)vis / len * tw);
    if (thumb_w < 8) thumb_w = 8;
    int thumb_x = m_hbar.x + (int)((double)m_scroll_ticks / len * tw);
    if (thumb_x + thumb_w > m_hbar.x + tw) thumb_x = m_hbar.x + tw - thumb_w;
    if (thumb_x < m_hbar.x) thumb_x = m_hbar.x;

    fill_rect(r, SDL_Rect{ thumb_x, m_hbar.y + 1, thumb_w, m_hbar.h - 2 }, t.hi);
    frame_rect(r, m_hbar, t.dim);
}

void PianoRoll::hbar_set(int x)
{
    long len = m_seq ? m_seq->get_length() : (long)(4 * c_ppqn);
    long maxt = len - visible_ticks(); if (maxt < 0) maxt = 0;
    double frac = (double)(x - m_hbar.x) / (m_hbar.w > 0 ? m_hbar.w : 1);
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    m_scroll_ticks = (long)(frac * maxt);
    clamp_scroll();
}

// ---- status / coordinate readout -------------------------------------------
std::string PianoRoll::note_name(int n) const
{
    if (n < 0) n = 0; if (n > 127) n = 127;
    char b[8];
    snprintf(b, sizeof b, "%s%d", g_pc_names[((n % 12) + 12) % 12], n / 12 - 1);
    return b;
}

std::string PianoRoll::bbt(long tick) const
{
    int bw  = (m_seq && m_seq->get_bw()  > 0) ? (int)m_seq->get_bw()  : 4;
    int bpm = (m_seq && m_seq->get_bpm() > 0) ? (int)m_seq->get_bpm() : 4;
    int tpb = (4 * c_ppqn) / (bw > 0 ? bw : 4);         // ticks / beat
    if (tpb < 1) tpb = c_ppqn;
    long tpbar = (long)tpb * bpm;                        // ticks / measure
    if (tick < 0) tick = 0;
    long bar    = tpbar > 0 ? tick / tpbar : 0;
    long within = tpbar > 0 ? tick % tpbar : tick;
    long beat   = within / tpb;
    long sub    = within % tpb;
    char b[32];
    snprintf(b, sizeof b, "%03ld:%ld:%03ld", bar + 1, beat + 1, sub);
    return b;
}

void PianoRoll::draw_status_bar(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    fill_rect(r, m_status, t.panel);
    hline(r, m_status.x, m_status.x + m_status.w, m_status.y, t.dim);

    static const char* const toolnm[] =
        { "EDIT", "DRAW", "ERASE", "SELECT", "ZOOM", "PAN" };
    long tick = x_to_tick(m_last_mx);
    int  note = y_to_note(m_last_my);
    int  sel  = m_seq ? m_seq->get_num_selected_notes() : 0;

    // LOOP READOUT.  The grey band is the only place the loop window is shown,
    // and it scrolls/zooms out of sight with the pattern -- so a clip could be
    // repeating a sub-region with nothing on screen saying so.  The status line
    // never scrolls, so state it here in bar.beat, with the enable flag.
    char loopbuf[48] = "";
    if (m_seq) {
        const long ls = m_seq->get_loop_start(), le = m_seq->get_loop_end();
        if (ls > 0 || le < m_seq->get_length()) {
            const int bw  = m_seq->get_bw()  > 0 ? (int)m_seq->get_bw()  : 4;
            const int bpm = m_seq->get_bpm() > 0 ? (int)m_seq->get_bpm() : 4;
            const long tpb   = (4 * c_ppqn) / (bw > 0 ? bw : 4);
            const long tpbar = (tpb > 0 ? tpb : c_ppqn) * bpm;
            auto bb = [&](long tk, char* o, size_t n) {
                if (tk < 0) tk = 0;
                snprintf(o, n, "%ld.%ld", tpbar > 0 ? tk / tpbar + 1 : 1,
                         tpb > 0 ? (tpbar > 0 ? (tk % tpbar) : tk) / tpb + 1 : 1);
            };
            char a[16], b2[16];
            bb(ls, a, sizeof a); bb(le, b2, sizeof b2);
            snprintf(loopbuf, sizeof loopbuf, "  loop %s-%s%s", a, b2,
                     m_seq->get_loop_enabled() ? "" : " (1-shot)");
        }
    }

    char buf[256];
    snprintf(buf, sizeof buf,
             "%s  pitch %s (%d)  snap %s  sel %d  tool %s%s  %s",
             bbt(tick).c_str(), note_name(note).c_str(), note,
             frac_label(m_snap).c_str(), sel, toolnm[m_tool], loopbuf,
             m_dirty_flag ? "[*]" : "");
    app.mono.draw(r, m_status.x + 3,
                  m_status.y + (m_status.h - app.mono.ch()) / 2, buf, t.text);
}

bool PianoRoll::note_overlaps(long ts, long tf, int note) const
{
    if (!m_seq || note < 0 || note >= K) return false;
    long a, b; int n, v; bool s;
    m_seq->reset_draw_marker();
    draw_type dt;
    while ((dt = m_seq->get_next_note_event(&a, &b, &n, &s, &v)) != DRAW_FIN) {
        if (dt == DRAW_NORMAL_LINKED && n == note && a < tf && b > ts)
            return true;
    }
    return false;
}

bool PianoRoll::insert_note(long tick, long len, int note, bool paint)
{
    if (!m_seq || note < 0 || note >= K) return false;
    long L = m_seq->get_length();
    if (L < 2 || tick < 0 || tick >= L - 1) return false;
    if (len < 1) len = 1;
    long tf = tick + len;
    if (tf >= L) tf = L - 1;
    if (tf <= tick) return false;
    if (note_overlaps(tick, tf, note)) return false;
    m_seq->add_note(tick, tf - tick, note, paint);
    return true;
}

// ---- draw tool : paint a run of snapped notes ------------------------------
// Adds one note per snap cell across [t0,t1] that is not already occupied.
// Note-off ticks are clamped strictly below the sequence length so the
// engine's verify_and_link() never discards the pair.
void PianoRoll::paint_run(long t0, long t1, int note)
{
    if (!m_seq) return;
    if (note < 0 || note > 127) return;
    if (t1 < t0) std::swap(t0, t1);
    if (t0 < 0) t0 = 0;
    long step = m_snap > 0 ? m_snap : (c_ppqn / 4);
    long L = m_seq->get_length(); if (L < 2) return;

    for (long tk = t0; tk <= t1; tk += step)
        insert_note(tk, m_note_length, note, true);
    m_seq->set_dirty();
}

// ---- erase tool : delete the note under the pointer ------------------------
void PianoRoll::erase_at(int x, int y)
{
    if (!m_seq) return;
    long ts, tf; int note; bool edge;
    if (!find_note_at(x, y, &ts, &tf, &note, &edge)) return;
    // mark_selected() marks EVERY selected event, not just the one just picked,
    // so erasing with a selection live deleted the whole selection along with
    // the note under the pointer.  e_select_one adds to the selection; it does
    // not replace it.
    m_seq->unselect();
    m_seq->select_note_events(ts, note, tf, note, sequence::e_select_one);
    m_seq->mark_selected();
    m_seq->remove_marked();
    m_seq->set_dirty();
}

// ---- zoom tool -------------------------------------------------------------
void PianoRoll::zoom_to_box(long ts, long tf, int nh, int nl)
{
    if (tf < ts) std::swap(ts, tf);
    if (nh < nl) std::swap(nh, nl);
    long span = tf - ts;
    if (span < (long)(m_snap > 0 ? m_snap : 1)) span = m_snap > 0 ? m_snap : 1;

    int z = (int)(span / (m_grid.w > 0 ? m_grid.w : 1));
    if (z < 1) z = 1; if (z > 64) z = 64;
    m_zoom = z;
    m_scroll_ticks = ts;

    int nspan = nh - nl + 1; if (nspan < 1) nspan = 1;
    int rh = m_grid.h / nspan;
    if (rh < 5)  rh = 5;
    if (rh > 24) rh = 24;
    m_row_h = rh;
    m_scroll_key = (K - 1) - nh;                        // top selected note at top
    clamp_scroll();
}

void PianoRoll::zoom_step(int dir, int ax, int ay)
{
    (void)ay;
    long anchor_tick = x_to_tick(ax);
    int newz = dir > 0 ? m_zoom / 2 : m_zoom * 2;       // dir>0 == zoom in
    if (newz < 1)  newz = 1;
    if (newz > 64) newz = 64;
    m_zoom = newz;
    m_scroll_ticks = anchor_tick - (long)(ax - m_grid.x) * m_zoom;
    clamp_scroll();
}

// ---- in-widget popup (context menu + snap/length dropdowns) ----------------
void PianoRoll::open_context_menu(int x, int y)
{
    m_popup_items.clear();
    bool hassel  = m_seq && m_seq->get_num_selected_notes() > 0;
    bool hasclip = clipboard_has_notes();
    m_popup_items.push_back(PopItem{ "Cut",         CM_CUT,    hassel  });
    m_popup_items.push_back(PopItem{ "Copy",        CM_COPY,   hassel  });
    m_popup_items.push_back(PopItem{ "Paste",       CM_PASTE,  hasclip });
    m_popup_items.push_back(PopItem{ "Delete",      CM_DELETE, hassel  });
    m_popup_items.push_back(PopItem{ "Quantize",    CM_QUANT,  hassel  });
    m_popup_items.push_back(PopItem{ "Legato",      CM_LEGATO, hassel  });
    m_popup_items.push_back(PopItem{ "Humanize",    CM_HUMAN,  hassel  });
    m_popup_items.push_back(PopItem{ "Select All",  CM_SELALL, true    });
    m_popup_items.push_back(PopItem{ "Select None", CM_SELNONE,hassel  });
    m_popup_items.push_back(PopItem{ "Invert",      CM_INVERT, hassel  });
    m_popup_kind = 0;
    layout_popup(x, y);
    m_popup_open = true;
}

void PianoRoll::open_dropdown(int kind, int x, int y)
{
    m_popup_items.clear();
    for (int i = 0; i < G_NDIVS; ++i)
        m_popup_items.push_back(PopItem{ frac_label(g_divs[i]), g_divs[i], true });
    m_popup_kind = kind;
    layout_popup(x, y);
    m_popup_open = true;
}

void PianoRoll::layout_popup(int x, int y)
{
    int rowh = m_chh + 4;
    int maxc = 1;
    for (const PopItem& it : m_popup_items)
        if ((int)it.label.size() > maxc) maxc = (int)it.label.size();
    int w = maxc * m_cw + 12;
    int h = (int)m_popup_items.size() * rowh + 2;

    // Cap to the widget FIRST.  Previously an over-tall popup (the 10-row
    // context menu in a short docked editor, or a long division list) was only
    // pushed up, then snapped to rect.y -- so it still ran off the bottom, its
    // lower rows drawn over whatever sat underneath and still clickable.
    if (w > rect.w) w = rect.w;
    if (h > rect.h) h = rect.h;
    if (x + w > rect.x + rect.w) x = rect.x + rect.w - w;
    if (y + h > rect.y + rect.h) y = rect.y + rect.h - h;
    if (x < rect.x) x = rect.x;
    if (y < rect.y) y = rect.y;

    m_popup = SDL_Rect{ x, y, w, h };
    m_popup_rowh = rowh;
}

// Rows the (possibly capped) popup can actually show.
int PianoRoll::popup_rows() const
{
    if (m_popup_rowh <= 0) return 0;
    const int rows = (m_popup.h - 2) / m_popup_rowh;
    return std::min(std::max(0, rows), (int)m_popup_items.size());
}

int PianoRoll::popup_hit(int y)
{
    if (m_popup_rowh <= 0) return -1;
    int idx = (y - (m_popup.y + 1)) / m_popup_rowh;
    // Only DRAWN rows are pickable -- a click below the last visible row used to
    // resolve to an item that was never on screen.
    if (idx < 0 || idx >= popup_rows()) return -1;
    return idx;
}

void PianoRoll::draw_popup(App& app)
{
    if (!m_popup_open) return;
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    fill_rect(r, m_popup, t.panel);
    frame_rect(r, m_popup, t.dim);

    const int rows = popup_rows();
    for (int i = 0; i < rows; ++i) {
        const PopItem& it = m_popup_items[i];
        SDL_Rect row{ m_popup.x + 1, m_popup.y + 1 + i * m_popup_rowh,
                      m_popup.w - 2, m_popup_rowh };
        bool hov = it.enabled &&
                   m_last_mx >= m_popup.x && m_last_mx < m_popup.x + m_popup.w &&
                   m_last_my >= row.y && m_last_my < row.y + m_popup_rowh;
        if (hov) fill_rect(r, row, t.active);
        Color tc = it.enabled ? (hov ? t.bg : t.text) : t.dim;
        app.mono.draw(r, row.x + 4, row.y + 2, it.label, tc);
    }
}

// ===========================================================================
//  DRAW
// ===========================================================================
void PianoRoll::draw(App& app)
{
    if (!visible) return;
    layout();
    const Theme& t = theme();
    m_cw  = app.mono.cw();       // cache metrics for app-less helpers (popups)
    m_chh = app.mono.ch();

    // follow-playhead ([Foll] toggle): page the view so the playhead stays
    // visible during playback, mirroring the Arrange view's m_follow logic.
    // The toggle button existed but nothing ever consulted it, so playback
    // would happily scroll the playhead right off the edge of the grid.
    if (m_follow && m_seq && m_seq->get_length() > 0) {
        long span = visible_ticks();
        if (span < 1) span = 1;
        const long ls = m_seq->get_loop_start(), le = m_seq->get_loop_end();
        const bool looping = m_seq->get_loop_enabled() && le > ls &&
                             (ls > 0 || le < m_seq->get_length());
        if (looping && (le - ls) <= span) {
            // A LOOPED CLIP'S PLAYHEAD NEVER LEAVES ITS WINDOW.  Paging on the
            // playhead the way the general case does made a short window shove
            // the grid sideways on every repetition -- the bar being edited slid
            // out from under the pointer several times a bar and follow-mode was
            // unusable with a loop set.  When the whole window fits on screen
            // there is nothing to follow: park the view on it once and hold.
            if (ls < m_scroll_ticks || le > m_scroll_ticks + span) {
                m_scroll_ticks = ls - (span - (le - ls)) / 2;   // centre it
                if (m_scroll_ticks < 0) m_scroll_ticks = 0;
                clamp_scroll();
            }
        } else {
            long ph = m_seq->get_last_tick();
            if (ph < m_scroll_ticks || ph > m_scroll_ticks + span) {
                m_scroll_ticks = ph - span / 4;
                if (m_scroll_ticks < 0) m_scroll_ticks = 0;
                clamp_scroll();
            }
        }
    }

    // whole widget backdrop
    fill_rect(app.ren, rect, t.bg);

    draw_grid(app);
    draw_notes(app);
    draw_overlay(app);
    draw_playhead(app);
    draw_keys(app);
    draw_ruler(app);
    draw_data(app);
    draw_status(app);

    frame_rect(app.ren, m_grid, t.dim);

    // BATCH 1 chrome (drawn over the regions; popup drawn last so it overlays)
    draw_toolbar(app);
    draw_hbar(app);
    draw_status_bar(app);
    draw_popup(app);
}

// ---- note grid: striping + beat/bar lines ---------------------------------
void PianoRoll::draw_grid(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;

    ui::ScopedClip clipScope(r,m_grid);
    fill_rect(r, m_grid, t.bg);

    // horizontal: row striping by black/white key + octave separators
    int vis = m_grid.h / m_row_h + 2;
    for (int i = 0; i < vis; ++i) {
        int note = (K - 1) - (m_scroll_key + i);
        if (note < 0) break;
        int key = ((note % 12) + 12) % 12;
        bool is_black = piano_black_key(note);
        int y = m_grid.y + i * m_row_h;

        if (is_black)
            fill_rect(r, SDL_Rect{ m_grid.x, y, m_grid.w, m_row_h }, t.panel);

        // snap-to-scale: tint rows that fall outside the current scale so the
        // in-scale degrees read as clear lanes (mirrors Qtractor's shading).
        if (m_scale > 0 && m_scale != 4 && !note_in_scale(note))
            fill_rect(r, SDL_Rect{ m_grid.x, y, m_grid.w, m_row_h }, t.scale);

        // divider: brighter under each C (octave boundary), else faint
        hline(r, m_grid.x, m_grid.x + m_grid.w, y, key == 0 ? t.dim : t.panel);
    }

    // vertical: bar / beat / sub-beat lines
    // get_bw() is nominally always > 0 (project_io clamps it on load), but
    // dividing by it unguarded -- unlike bbt() below, which already guards
    // this same division -- is a live SIGFPE waiting for a stray 0 to reach
    // here, so clamp it the same way.
    int bw = m_seq ? (int)m_seq->get_bw() : 4; if (bw <= 0) bw = 4;
    int tpb  = (4 * c_ppqn) / bw;                                           // ticks/beat
    int tpbar= (m_seq ? (int)m_seq->get_bpm() : 4) * (4 * c_ppqn) / bw;     // ticks/measure
    int step = tpb / 4;                       // 16th grid
    if (step < 1) step = 1;
    bool draw_sub = (step / m_zoom) >= 4;     // hide sub lines when too dense

    long start = m_scroll_ticks - (m_scroll_ticks % step);
    long end   = m_scroll_ticks + visible_ticks();
    for (long tk = start; tk <= end; tk += step) {
        int x = tick_to_x(tk);
        if (x < m_grid.x || x > m_grid.x + m_grid.w) continue;
        bool bar  = (tk % tpbar) == 0;
        bool beat = (tk % tpb)   == 0;
        if (!beat && !draw_sub) continue;
        Color c = bar ? t.accent : (beat ? t.dim : t.panel);
        vline(r, x, m_grid.y, m_grid.y + m_grid.h, c);
    }

}

// ---- notes ----------------------------------------------------------------
void PianoRoll::draw_notes(App& app)
{
    if (!m_seq) return;
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    ui::ScopedClip clipScope(r,m_grid);

    long ts, tf; int note, vel; bool sel;
    m_seq->reset_draw_marker();
    draw_type dt;
    while ((dt = m_seq->get_next_note_event(&ts, &tf, &note, &sel, &vel)) != DRAW_FIN) {

        int x = tick_to_x(ts);
        int y = note_to_y(note);
        int h = m_row_h - 2;
        if (h < 2) h = 2;
        if (y + m_row_h < m_grid.y || y > m_grid.y + m_grid.h) continue;   // off-screen
        Color body = sel ? t.notesel : t.note;

        // --- drum mode: fixed-size diamond at the start tick (ignore length) --
        if (m_drum_mode) {
            int half = m_row_h / 2; if (half < 3) half = 3; if (half > 8) half = 8;
            int cx = x, cy = y + m_row_h / 2;
            if (cx + half < m_grid.x || cx - half > m_grid.x + m_grid.w) continue;
            fill_diamond(r, cx, cy, half, vel >= 64 ? body : t.dim);  // shade by velocity
            fill_diamond(r, cx, cy, 1, t.hi);                         // centre pip
            continue;
        }

        // --- bar mode --------------------------------------------------------
        // A note that WRAPS the pattern end has its off at a LOWER tick than its
        // on, so (tf - ts) came out negative and was clamped to a 1 px sliver --
        // a wrapped note was effectively invisible.  Draw the two segments it
        // really is: on..pattern end, and pattern start..off.
        long barEnd  = tf;
        long wrapEnd = -1;
        if (dt == DRAW_NORMAL_LINKED && tf < ts) {
            barEnd  = m_seq->get_length();
            wrapEnd = tf;
        }
        int w;
        if (dt == DRAW_NORMAL_LINKED) w = int((barEnd - ts) / m_zoom);
        else                          w = 8 / m_zoom;                 // unlinked stub
        if (w < 1) w = 1;
        if (wrapEnd > 0) {
            const int hx = tick_to_x(0);
            int hw = int(wrapEnd / m_zoom); if (hw < 1) hw = 1;
            SDL_Rect hb{ hx, y + 1, hw, h };
            fill_rect(r, hb, t.dim);
            int hlit = h * vel / 127; if (hlit < 1) hlit = 1; if (hlit > h) hlit = h;
            fill_rect(r, SDL_Rect{ hx, y + 1 + (h - hlit), hw, hlit }, body);
            frame_rect(r, hb, t.hi);
        }
        if (x + w < m_grid.x || x > m_grid.x + m_grid.w) continue;

        SDL_Rect nb{ x, y + 1, w, h };
        // velocity shading (two-tone): whole bar dim, then a bottom-anchored
        // slice in the bright note colour proportional to velocity, so louder
        // notes read brighter/fuller.
        fill_rect(r, nb, t.dim);
        int litH = h * vel / 127; if (litH < 1) litH = 1; if (litH > h) litH = h;
        fill_rect(r, SDL_Rect{ x, y + 1 + (h - litH), w, litH }, body);
        frame_rect(r, nb, t.hi);

        // right-edge resize hint
        if (w >= 6)
            fill_rect(r, SDL_Rect{ x + w - 3, y + 2, 2, h - 2 }, t.hi);

        // note-name label (B) on bars wide/tall enough to hold it
        if (m_show_labels && h >= app.mono.ch()) {
            char nm[8];
            int oct = note / 12 - 1;
            snprintf(nm, sizeof nm, "%s%d", g_pc_names[((note % 12) + 12) % 12], oct);
            if (w >= app.mono.text_w(nm) + 3)
                app.mono.draw(r, x + 2, y + 1 + (h - app.mono.ch()) / 2, nm, t.bg);
        }
    }
}

// ---- overlay: lasso box / move / grow preview -----------------------------
void PianoRoll::draw_overlay(App& app)
{
    if (m_mode == M_NONE || !m_dragging) return;
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    ui::ScopedClip clipScope(r,m_grid);

    if (m_mode == M_SELECT || m_mode == M_ADDPEND || m_mode == M_ZOOM) {
        int x0 = std::min(m_drop_x, m_cur_x), x1 = std::max(m_drop_x, m_cur_x);
        int y0 = std::min(m_drop_y, m_cur_y), y1 = std::max(m_drop_y, m_cur_y);
        frame_rect(r, SDL_Rect{ x0, y0, x1 - x0, y1 - y0 },
                   m_mode == M_ZOOM ? t.hi : t.accent);
    }
    else if (m_mode == M_MOVE) {
        long dt = snap_tick(x_to_tick(m_cur_x)) - snap_tick(x_to_tick(m_drop_x));
        int  dn = y_to_note(m_cur_y) - y_to_note(m_drop_y);
        int x = tick_to_x(m_sel_ts + dt);
        int y = note_to_y(m_sel_nh + dn);
        int w = int((m_sel_tf - m_sel_ts) / m_zoom); if (w < 1) w = 1;
        int h = (m_sel_nh - m_sel_nl + 1) * m_row_h;
        frame_rect(r, SDL_Rect{ x, y, w, h }, t.hi);
    }
    else if (m_mode == M_GROW) {
        long newend = snap_tick(x_to_tick(m_cur_x));
        int x = tick_to_x(m_sel_ts);
        int y = note_to_y(m_sel_nh);
        int w = int((newend - m_sel_ts) / m_zoom); if (w < 1) w = 1;
        int h = (m_sel_nh - m_sel_nl + 1) * m_row_h;
        frame_rect(r, SDL_Rect{ x, y, w, h }, t.hi);
    }

}

// ---- playhead -------------------------------------------------------------
void PianoRoll::draw_playhead(App& app)
{
    if (!m_seq) return;
    // get_last_tick() does `tick % m_length`; a zero-length sequence would raise
    // SIGFPE inside the engine, so never call it in that degenerate state.
    if (m_seq->get_length() <= 0) return;
    long tk = m_seq->get_last_tick();
    int x = tick_to_x(tk);
    if (x < m_grid.x || x > m_grid.x + m_grid.w) return;
    const Theme& t = theme();
    vline(app.ren, x,     m_grid.y, m_grid.y + m_grid.h, t.active);
    vline(app.ren, x + 1, m_grid.y, m_grid.y + m_grid.h, t.active);
}

// ---- left keyboard strip --------------------------------------------------
void PianoRoll::draw_keys(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    ui::ScopedClip clipScope(r,m_keys);
    fill_rect(r, m_keys, t.bg);

    int vis = m_grid.h / m_row_h + 2;
    for (int i = 0; i < vis; ++i) {
        int note = (K - 1) - (m_scroll_key + i);
        if (note < 0) break;
        int key = ((note % 12) + 12) % 12;
        bool is_black = piano_black_key(note);
        bool is_root  = (key == 0);
        int y = m_keys.y + i * m_row_h;

        const Color ivory{ 245, 245, 240, 255 };
        const Color ebony{ 10, 10, 12, 255 };
        const Color ink{ 18, 18, 20, 255 };
        const int kx = m_keys.x + 1;
        const int whiteW = m_keys.w - 3;
        const int kw = is_black ? std::max(12, whiteW * 2 / 3) : whiteW;
        SDL_Rect keyRect{ kx, y + 1, kw, std::max(1, m_row_h - 1) };
        fill_rect(r, keyRect, is_black ? ebony : ivory);
        frame_rect(r, keyRect, ink);
        hline(r, m_keys.x, m_keys.x + m_keys.w, y + m_row_h, t.dim);

        // State uses outlines, so C stays white and sharps stay black.
        if (is_root)
            frame_rect(r, SDL_Rect{keyRect.x+1,keyRect.y+1,
                                   std::max(1,keyRect.w-2),std::max(1,keyRect.h-2)}, t.accent);
        if (note == m_keying_note)
            frame_rect(r, keyRect, t.active);

        if (m_row_h >= 8) {
            char lbl[8];
            int oct = (note / 12) - 1;
            snprintf(lbl, sizeof lbl, "%s%d", g_pc_names[key], oct);
            app.mono.draw(r, keyRect.x + 2, y + (m_row_h - app.mono.ch()) / 2,
                          lbl, is_black ? ivory : ink);
        }
    }
    vline(r, m_keys.x + m_keys.w - 1, m_keys.y, m_keys.y + m_keys.h, t.dim);
}

// ---- top ruler ------------------------------------------------------------
void PianoRoll::draw_ruler(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    ui::ScopedClip clipScope(r,m_ruler);
    fill_rect(r, m_ruler, t.panel);
    hline(r, m_ruler.x, m_ruler.x + m_ruler.w, m_ruler.y + m_ruler.h - 1, t.dim);

    int rbw = m_seq ? (int)m_seq->get_bw() : 4; if (rbw <= 0) rbw = 4;
    int tpbar = (m_seq ? (int)m_seq->get_bpm() : 4) * (4 * c_ppqn) / rbw;
    if (tpbar < 1) tpbar = 4 * c_ppqn;
    // draw a label at most every ~48px: coarsen the measure step if needed
    int measures_per_step = 1;
    while ((tpbar * measures_per_step) / m_zoom < 48) measures_per_step *= 2;
    int step = tpbar * measures_per_step;

    long start = m_scroll_ticks - (m_scroll_ticks % step);
    long end   = m_scroll_ticks + visible_ticks();
    for (long tk = start; tk <= end; tk += step) {
        int x = tick_to_x(tk);
        if (x < m_ruler.x - 1 || x > m_ruler.x + m_ruler.w) continue;
        vline(r, x, m_ruler.y, m_ruler.y + m_ruler.h, t.accent);
        char bar[8];
        snprintf(bar, sizeof bar, "%ld", (tk / tpbar) + 1);
        app.mono.draw(r, x + 2, m_ruler.y + 1, bar, t.text);
    }

    // Ableton-style loop brace: a shaded band over the [loop_start, loop_end)
    // window, independent of the pattern's own length -- content before/after
    // the loop is still there, just not part of it.  No separate tab/text
    // widgets -- the ruler itself IS the control (see on_mouse: drag empty
    // ruler to select a new region, drag the band to move it, drag right at
    // an edge to resize just that edge).  NOT the song/transport loop
    // (perform's left/right tick, shown in the Arrange view) -- separate,
    // global range.  Default state (loop_end == length, loop_start == 0)
    // means "no loop set": draw nothing so a fresh pattern looks unchanged.
    if (m_seq) {
        const long ls = m_seq->get_loop_start(), le = m_seq->get_loop_end();
        const bool loopSet = ls > 0 || le < m_seq->get_length();
        const bool loopOn  = m_seq->get_loop_enabled();
        const int lx = tick_to_x(ls), ex = tick_to_x(le);
        if (loopSet && ex > lx) {
            // A band with looping OFF is inert -- draw it faint so it does not
            // claim to be doing something it is not.
            Color band = loopOn ? t.accent : t.dim; band.a = loopOn ? 48 : 24;
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            fill_rect(r, SDL_Rect{lx, m_ruler.y, ex-lx, m_ruler.h}, band);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
            // Bright double-line edges hint the grabbable resize handles.
            const Color edge = loopOn ? t.hi : t.dim;
            vline(r, lx,     m_ruler.y, m_ruler.y + m_ruler.h, edge);
            vline(r, lx + 1, m_ruler.y, m_ruler.y + m_ruler.h, edge);
            vline(r, ex - 1, m_ruler.y, m_ruler.y + m_ruler.h, edge);
            vline(r, ex,     m_ruler.y, m_ruler.y + m_ruler.h, edge);
        }
        else if (loopSet) {
            // Sub-pixel window: ex == lx at this zoom, so the block above drew
            // NOTHING even though the loop is live and the hit test below still
            // treats it as grabbable.  A pattern that audibly repeated a sliver
            // had no mark on the ruler to explain why.  Always leave a stem.
            const Color edge = loopOn ? t.hi : t.dim;
            vline(r, lx,     m_ruler.y, m_ruler.y + m_ruler.h, edge);
            vline(r, lx + 1, m_ruler.y, m_ruler.y + m_ruler.h, edge);
        }


        // END MARKER -- where the pattern's DATA ends (sequence::get_length()),
        // which is a different thing from the loop window drawn above: the loop
        // is a sub-region inside the pattern, the END is the pattern itself.
        // Drag it to lengthen or shorten the sequence.  It was missing
        // entirely: set_sequence_length_ticks() existed but nothing drew a
        // handle or called it, so the only length you could get was whatever
        // the pattern was created with.
        const int endx = tick_to_x(m_seq->get_length());
        if (endx >= m_ruler.x - 2 && endx <= m_ruler.x + m_ruler.w + 2) {
            vline(r, endx,     m_ruler.y, m_ruler.y + m_ruler.h, t.note);
            vline(r, endx + 1, m_ruler.y, m_ruler.y + m_ruler.h, t.note);
            // Flag pointing back over the pattern, so it reads as "ends here".
            // Geometry comes from end_marker_rect() so the hit-test grabs
            // exactly what is painted (its top strip is the flag).
            const SDL_Rect g = end_marker_rect();
            const int fh = m_ruler.h / 2 > 5 ? 5 : m_ruler.h / 2;
            SDL_Rect flag{ g.x, m_ruler.y, g.w - 2, fh };
            if (flag.x < m_ruler.x) { flag.w -= (m_ruler.x - flag.x); flag.x = m_ruler.x; }
            if (flag.w > 0) fill_rect(r, flag, t.note);
        }

        // LOOP / 1-SHOT chip, pinned to the RIGHT of the ruler.  It used to be
        // plain "1-SHOT" text at m_ruler.x + 3, i.e. painted straight over bar
        // 1's number, and it was display-only.  Here it is a real button:
        // clicking it toggles THIS pattern's sequence::m_loop_enabled, the same
        // thing Shift+L does, so the per-clip one-shot state is visible and
        // reachable without knowing the shortcut.
        {
            const char* lbl = loopOn ? "LOOP" : "1-SHOT";
            const int cw = app.mono.text_w(lbl);
            SDL_Rect chip{ m_ruler.x + m_ruler.w - cw - 8, m_ruler.y + 1,
                           cw + 6, m_ruler.h - 2 };
            if (chip.h < 4) chip.h = 4;
            m_loopchip = chip;
            fill_rect(r, chip, loopOn ? t.accent : t.panel);
            frame_rect(r, chip, loopOn ? t.hi : t.dim);
            app.mono.draw(r, chip.x + 3, chip.y + 1, lbl, loopOn ? t.bg : t.note);
        }

        // OFF-SCREEN LOOP.  The band is the only thing in the whole editor that
        // says "this pattern repeats a sub-region", and it is painted in pattern
        // coordinates -- so scrolling right, or zooming in on a later bar, hid
        // it completely and the ruler then looked exactly like a pattern with no
        // loop at all, while playback kept repeating the window.  Point at it
        // from the ruler edge instead: an arrow towards the side the window sits
        // on, in the same colour the band would have had.  Drawn LAST so the
        // LOOP chip cannot paint over it (the chip owns the right end, so the
        // right-hand arrow parks just inside it).
        if (loopSet && (ex <= m_ruler.x || lx >= m_ruler.x + m_ruler.w)) {
            const Color mark = loopOn ? t.hi : t.dim;
            const int ah = m_ruler.h >= 10 ? 4 : 2;             // arrow half-height
            const int ay = m_ruler.y + m_ruler.h / 2;
            const bool left = (ex <= m_ruler.x);
            const int  x0 = left ? m_ruler.x
                                 : (m_loopchip.w > 0 ? m_loopchip.x - 2
                                                     : m_ruler.x + m_ruler.w - 1);
            for (int i = 0; i < ah; ++i)
                fill_rect(r, SDL_Rect{ left ? x0 + i : x0 - i, ay - i, 1, 2*i + 1 },
                          mark);
        }
    } else {
        m_loopchip = SDL_Rect{ 0, 0, 0, 0 };
    }
}

// Shared END-marker geometry (see the declaration).  The flag is drawn back
// over the pattern from the stem, so the grab area is that flag plus a couple
// of pixels either side of the stem itself.
SDL_Rect PianoRoll::end_marker_rect() const
{
    const int endx = m_seq ? tick_to_x(m_seq->get_length()) : m_ruler.x;
    const int fh = m_ruler.h / 2 > 5 ? 5 : m_ruler.h / 2;
    const int fw = 3 * fh;
    return SDL_Rect{ endx - fw, m_ruler.y, fw + 4, m_ruler.h };
}

// ---- bottom velocity lane -------------------------------------------------
void PianoRoll::draw_data(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    ui::ScopedClip clipScope(r,m_data);
    fill_rect(r, m_data, t.panel);
    hline(r, m_data.x, m_data.x + m_data.w, m_data.y, t.dim);

    // reference lines at 25/50/75/100 %
    for (int v = 1; v <= 4; ++v) {
        int y = m_data.y + m_data.h - (m_data.h * v) / 4;
        hline(r, m_data.x, m_data.x + m_data.w, y, t.dim);
    }

    if (m_seq) {
        const DataLane& L = g_lanes[m_data_type];
        if (L.status == EVENT_NOTE_ON) {
            // velocity lane -- one mark per note, keyed on its start tick
            long ts, tf; int note, vel; bool sel;
            m_seq->reset_draw_marker();
            draw_type dt;
            while ((dt = m_seq->get_next_note_event(&ts, &tf, &note, &sel, &vel)) != DRAW_FIN) {
                int x = tick_to_x(ts);
                if (x < m_data.x - 4 || x > m_data.x + m_data.w) continue;
                int barH = (vel * m_data.h) / 127;
                if (barH < 1) barH = 1;
                int y = m_data.y + m_data.h - barH;
                fill_rect(r, SDL_Rect{ x, y, 3, barH }, sel ? t.notesel : t.note);
                fill_rect(r, SDL_Rect{ x - 1, y, 5, 2 }, t.hi);
            }
        } else {
            // CC / pitch-bend / program / channel-pressure lane
            long tick; unsigned char d0, d1; bool sel;
            m_seq->reset_draw_marker();
            while (m_seq->get_next_event(L.status, L.cc, &tick, &d0, &d1, &sel)) {
                int val = (L.status == EVENT_PROGRAM_CHANGE ||
                           L.status == EVENT_CHANNEL_PRESSURE) ? d0 : d1;
                int x = tick_to_x(tick);
                if (x < m_data.x - 4 || x > m_data.x + m_data.w) continue;
                int barH = (val * m_data.h) / 127;
                if (barH < 1) barH = 1;
                int y = m_data.y + m_data.h - barH;
                fill_rect(r, SDL_Rect{ x, y, 3, barH }, sel ? t.notesel : t.note);
                fill_rect(r, SDL_Rect{ x - 1, y, 5, 2 }, t.hi);
            }
        }
    }

    // clickable type tab: current lane label (V or click cycles it)
    SDL_Rect hb = data_header_rect();
    fill_rect(r, hb, t.accent);
    frame_rect(r, hb, t.dim);
    app.mono.draw(r, hb.x + 2, hb.y + (hb.h - app.mono.ch()) / 2,
                  g_lanes[m_data_type].label, t.bg);

}

// ---- corner status: scale + toggle flags ----------------------------------
void PianoRoll::draw_status(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    SDL_Rect box{ rect.x, m_ruler.y, m_keys.w, m_grid.y - m_ruler.y };
    if (box.h < 1 || box.w < 1) return;
    fill_rect(r, box, t.panel);

    // scale readout: root + abbreviation, shown only when a scale is engaged
    static const char* const abbr[] = { "", "Maj", "min", "Pen", "Chr" };
    if (m_scale > 0) {
        char s[12];
        snprintf(s, sizeof s, "%s%s",
                 g_pc_names[((m_scale_root % 12) + 12) % 12], abbr[m_scale]);
        app.mono.draw(r, box.x + 1, box.y + 1, s, t.text);
    }

    // toggle flags (drum / labels) on a second mini-row when there's headroom
    char f[4]; int fi = 0;
    if (m_drum_mode)   f[fi++] = 'D';
    if (m_show_labels) f[fi++] = 'B';
    f[fi] = 0;
    if (fi && box.h >= 2 * app.mono.ch() + 1)
        app.mono.draw(r, box.x + 1, box.y + 1 + app.mono.ch(), f, t.accent);
}

// ===========================================================================
//  HIT TEST
// ===========================================================================
bool PianoRoll::find_note_at(int x, int y, long* ts, long* tf, int* note, bool* edge)
{
    if (!m_seq) return false;
    long a, b; int n, v; bool s;
    bool found = false;
    long best_ts = 0, best_tf = 0; int best_n = 0, best_edge_x = 0;

    m_seq->reset_draw_marker();
    draw_type dt;
    while ((dt = m_seq->get_next_note_event(&a, &b, &n, &s, &v)) != DRAW_FIN) {
        if (dt != DRAW_NORMAL_LINKED) continue;
        int x0 = tick_to_x(a);
        int y0 = note_to_y(n);
        int y1 = y0 + m_row_h;
        if (m_drum_mode) {
            // drum mode: a fixed box around the diamond at the start tick
            int half = m_row_h / 2; if (half < 3) half = 3; if (half > 8) half = 8;
            if (x >= x0 - half - 1 && x <= x0 + half + 1 && y >= y0 && y <= y1) {
                found = true; best_ts = a; best_tf = b; best_n = n; best_edge_x = x0;
            }
            continue;
        }
        int w  = int((b - a) / m_zoom); if (w < 1) w = 1;
        int x1 = x0 + w;
        if (x >= x0 - 1 && x <= x1 + 2 && y >= y0 && y <= y1) {
            // topmost/last wins; remember the note under cursor
            found = true; best_ts = a; best_tf = b; best_n = n; best_edge_x = x1;
        }
    }
    if (!found) return false;
    *ts = best_ts; *tf = best_tf; *note = best_n;
    if (m_drum_mode) { *edge = false; return true; }   // no edge-resize on diamonds
    int w = int((best_tf - best_ts) / m_zoom);
    int handle = w / 3; if (handle > 6) handle = 6; if (handle < 3) handle = 3;
    *edge = (x >= best_edge_x - handle);
    return true;
}

void PianoRoll::capture_selbox()
{
    if (!m_seq) return;
    m_seq->get_selected_box(&m_sel_ts, &m_sel_nh, &m_sel_tf, &m_sel_nl);
}

// ===========================================================================
//  MOUSE
// ===========================================================================
bool PianoRoll::on_mouse(App& app, const MouseEv& e)
{
    layout();
    m_last_mx = e.x; m_last_my = e.y;

    // ---- release --------------------------------------------------------
    if (!e.pressed) {
        m_cur_x = e.x; m_cur_y = e.y;
        SDL_Keymod mod = SDL_GetModState();
        if (m_prev_note >= 0) { preview_note_off(m_prev_note); m_prev_note = -1; }

        switch (m_mode) {
        case M_KEYS:
            if (m_preview && m_keying_note >= 0) preview_note_off(m_keying_note);
            m_keying_note = -1;
            break;

        case M_ADDPEND: {                       // click / draw-paint release
            if (m_tool == T_DRAW) {             // draw tool: run already painted
                if (m_seq) { m_seq->unpaint_all(); m_seq->set_dirty(); }
                m_dirty_flag = true;
                break;
            }
            int an = snap_note_to_scale(m_drop_note);   // K : lock to scale
            if (m_seq) {
                m_seq->push_undo();
                bool added = insert_note(m_drop_tick, m_note_length, an, true);
                m_seq->unpaint_all();
                if (added) {
                    m_seq->set_dirty();
                    m_dirty_flag = true;
                }
            }
            break; }

        case M_SELECT: {                        // lasso
            if (m_seq) {
                long ts = x_to_tick(std::min(m_drop_x, m_cur_x));
                long tf = x_to_tick(std::max(m_drop_x, m_cur_x));
                int  nh = y_to_note(std::min(m_drop_y, m_cur_y));
                int  nl = y_to_note(std::max(m_drop_y, m_cur_y));
                // Ctrl or Shift keeps the existing selection (additive marquee)
                if (!(mod & (KMOD_CTRL | KMOD_SHIFT))) m_seq->unselect();
                m_seq->select_note_events(ts, nh, tf, nl, sequence::e_select);
                m_seq->set_dirty();
            }
            break; }

        case M_ZOOM: {                          // zoom tool: region or click
            if (m_dragging) {
                long ts = x_to_tick(std::min(m_drop_x, m_cur_x));
                long tf = x_to_tick(std::max(m_drop_x, m_cur_x));
                int  nh = y_to_note(std::min(m_drop_y, m_cur_y));
                int  nl = y_to_note(std::max(m_drop_y, m_cur_y));
                zoom_to_box(ts, tf, nh, nl);
            } else {
                zoom_step((mod & KMOD_CTRL) ? -1 : +1, m_cur_x, m_cur_y);
            }
            break; }

        case M_MOVE: {
            long dt = snap_tick(x_to_tick(m_cur_x)) - snap_tick(x_to_tick(m_drop_x));
            int  dn = y_to_note(m_cur_y) - y_to_note(m_drop_y);
            if (m_seq && (dt != 0 || dn != 0))
                move_selection(dt, dn);
            break; }

        case M_GROW: {
            long newend = snap_tick(x_to_tick(m_cur_x));
            long delta  = newend - m_sel_tf;
            if (m_seq && delta != 0)
                grow_selection(delta, (mod & KMOD_SHIFT) != 0);
            break; }

        case M_LOOP_EDGE_L:
            set_sequence_loop_start_ticks(x_to_tick(m_cur_x));
            break;
        case M_LOOP_EDGE_R:
            set_sequence_loop_end_ticks(x_to_tick(m_cur_x));
            break;
        case M_LOOP_MOVE: {
            const long delta = x_to_tick(m_cur_x) - m_loop_drag_press_tick;
            long newStart = m_loop_drag_anchor_start + delta;
            if (newStart < 0) newStart = 0;
            set_sequence_loop_range_ticks(newStart, newStart + m_loop_drag_anchor_len);
            break; }
        case M_LOOP_SELECT: {
            // A press that never moved is a click, and a click must leave the
            // existing window alone (see the press handler).
            if (!m_dragging) break;
            const long cur = snap_tick(x_to_tick(m_cur_x));
            const long lo  = std::min(cur, m_loop_drag_press_tick);
            const long hi  = std::max(cur, m_loop_drag_press_tick);
            set_sequence_loop_range_ticks(lo, hi);
            break; }

        default: break;
        }

        // The remembered loop window is only good for the END gesture that just
        // ended, so it is dropped HERE, on the button release.  It used to be
        // cleared at the bottom of the motion handler instead -- which runs on
        // every motion event, not once per gesture, so only the FIRST pixel of
        // an END drag was reversible: drag END left past the loop end and the
        // second motion event let sequence::set_length() clamp the window away
        // for good, exactly the destruction the anchor exists to prevent.
        m_len_drag_loop_valid = false;

        m_down = false; m_dragging = false; m_mode = M_NONE;
        app.request_redraw();
        return true;
    }

    // ---- motion (button held, toolkit re-sends pressed=true) ------------
    if (m_down) {
        m_cur_x = e.x; m_cur_y = e.y;
        if (std::abs(e.x - m_drop_x) > 3 || std::abs(e.y - m_drop_y) > 3)
            m_dragging = true;

        switch (m_mode) {
        case M_KEYS: {
            int n = y_to_note(e.y);
            if (n != m_keying_note) {
                if (m_preview && m_seq) {
                    if (m_keying_note >= 0) preview_note_off(m_keying_note);
                    preview_note_on(n);
                }
                m_keying_note = n;
            }
            break; }
        case M_DATA: apply_data_drag(app); break;
        case M_SBAR: {
            int vis = visible_keys();
            m_scroll_key = (e.y - m_grid.y) * K / (m_grid.h ? m_grid.h : 1) - vis / 2;
            clamp_scroll();
            break; }
        case M_HBAR: hbar_set(e.x); break;
        case M_LENGTH_EDGE: set_sequence_length_ticks(x_to_tick(e.x)); break;
        case M_LOOP_EDGE_L: set_sequence_loop_start_ticks(x_to_tick(e.x)); break;
        case M_LOOP_EDGE_R: set_sequence_loop_end_ticks(x_to_tick(e.x)); break;
        case M_LOOP_MOVE: {
            const long delta = x_to_tick(e.x) - m_loop_drag_press_tick;
            long newStart = m_loop_drag_anchor_start + delta;
            if (newStart < 0) newStart = 0;
            set_sequence_loop_range_ticks(newStart, newStart + m_loop_drag_anchor_len);
            break; }
        case M_LOOP_SELECT: {
            if (!m_dragging) break;      // inside the click slop: not an edit yet
            const long cur = snap_tick(x_to_tick(e.x));
            const long lo  = std::min(cur, m_loop_drag_press_tick);
            const long hi  = std::max(cur, m_loop_drag_press_tick);
            set_sequence_loop_range_ticks(lo, hi);
            break; }
        case M_ERASE: erase_at(e.x, e.y); m_dirty_flag = true; break;
        case M_PAN: {                            // hand-drag scrolls both axes
            m_scroll_ticks += (long)(m_drop_x - e.x) * m_zoom;
            int dky = (e.y - m_drop_y) / (m_row_h ? m_row_h : 1);
            m_scroll_key += dky;
            m_drop_x = e.x;                       // re-anchor horizontally
            if (dky != 0) m_drop_y = e.y;         // re-anchor once a row moved
            clamp_scroll();
            break; }
        case M_ADDPEND:
            if (m_tool == T_DRAW)
                paint_run(m_drop_tick, snap_tick(x_to_tick(e.x)), m_drop_note);
            else if (m_dragging)
                m_mode = M_SELECT;
            break;
        default: break;   // M_ZOOM: overlay only, applied on release
        }
        app.request_redraw();
        return true;
    }

    // ---- genuine press --------------------------------------------------
    m_down = true; m_dragging = false;
    m_drop_x = m_cur_x = e.x; m_drop_y = m_cur_y = e.y;
    SDL_Keymod mod = SDL_GetModState();
    SDL_Point pt{ e.x, e.y };

    // an open popup eats the next press: dispatch a hit, then close either way
    if (m_popup_open) {
        if (SDL_PointInRect(&pt, &m_popup)) {
            int idx = popup_hit(e.y);
            if (idx >= 0 && m_popup_items[idx].enabled) {
                int id = m_popup_items[idx].id;
                if (m_popup_kind == 0 && m_seq) {          // context menu
                    switch (id) {
                    case CM_CUT:    cut_selection();   break;
                    case CM_COPY:   copy_selection();  break;
                    case CM_PASTE:  paste_clipboard(); break;
                    case CM_DELETE: delete_selection(); break;
                    case CM_QUANT:
                        if (m_snap > 0 && m_seq->get_num_selected_notes() > 0) {
                            m_seq->push_undo();
                            // Nearest grid line, matching record quantize and
                            // note insertion.  `leftward=true` was the obsolete
                            // floor-only mode and pulled early notes a full step
                            // backwards.
                            m_seq->quanize_events(EVENT_NOTE_ON, 0, m_snap, 1, true, false);
                            m_seq->set_dirty();
                        }
                        break;
                    case CM_LEGATO: legato_selection();   break;
                    case CM_HUMAN:  humanize_selection(); break;
                    case CM_SELALL: m_seq->select_all(); m_seq->set_dirty(); break;
                    case CM_SELNONE:m_seq->unselect();   m_seq->set_dirty(); break;
                    case CM_INVERT: invert_selection();  break;
                    }
                    m_dirty_flag = true;
                } else if (m_popup_kind == 1) {            // snap dropdown
                    m_snap = id;
                } else if (m_popup_kind == 2) {            // length dropdown
                    m_note_length = id;
                }
            }
        }
        m_popup_open = false;
        m_down = false; m_mode = M_NONE;
        app.request_redraw();
        return true;
    }

    // toolbar strip: mutate view-state only, short-circuit (no sequence edit)
    if (SDL_PointInRect(&pt, &m_toolbar)) {
        build_toolbar(app);
        int id = toolbar_hit(e.x, e.y);
        switch (id) {
        case TBI_EDIT: case TBI_DRAW: case TBI_ERASE:
        case TBI_SELECT: case TBI_ZOOM: case TBI_PAN:
            set_tool(id - TBI_EDIT); break;
        case TBI_SNAP: open_dropdown(1, e.x, m_toolbar.y + m_toolbar.h); break;
        case TBI_LEN:  open_dropdown(2, e.x, m_toolbar.y + m_toolbar.h); break;
        case TBI_ZOUT: zoom_step(-1, m_grid.x + m_grid.w / 2, m_grid.y); break;
        case TBI_ZIN:  zoom_step(+1, m_grid.x + m_grid.w / 2, m_grid.y); break;
        case TBI_ZRESET: m_zoom = 6; clamp_scroll(); break;
        case TBI_VDN: m_row_h = m_row_h > 5  ? m_row_h - 1 : 5;  clamp_scroll(); break;
        case TBI_VUP: m_row_h = m_row_h < 24 ? m_row_h + 1 : 24; clamp_scroll(); break;
        case TBI_FOLL:  m_follow      = !m_follow;      break;
        case TBI_SCALE: m_scale       = (m_scale + 1) % 5; break;
        case TBI_DRUM:  m_drum_mode   = !m_drum_mode;   break;
        case TBI_LBL:   m_show_labels = !m_show_labels; break;
        case TBI_GHOST: m_show_ghost  = !m_show_ghost;  break;
        case TBI_OVER:  m_show_over    = !m_show_over;  break;
        default: break;
        }
        m_down = false; m_mode = M_NONE;
        app.request_redraw();
        return true;
    }

    // right-click on the grid opens the in-widget context menu
    if (e.button == SDL_BUTTON_RIGHT) {
        if (SDL_PointInRect(&pt, &m_grid)) open_context_menu(e.x, e.y);
        m_down = false; m_mode = M_NONE;
        app.request_redraw();
        return true;
    }

    // Ableton-style loop brace on the ruler.  No draggable tab widgets: the
    // ruler bar itself is the control.
    //   - within ~6px of an edge  -> resize just that edge
    //   - inside the shaded band  -> move the whole region (span preserved)
    //   - anywhere else in the ruler (empty grey area) -> marquee-select a
    //     brand new region from this press point
    if (m_seq && SDL_PointInRect(&pt, &m_ruler)) {
        // The LOOP / 1-SHOT chip sits on top of the ruler and is checked first,
        // or the brace logic below would swallow the click and start a marquee
        // under the button.
        if (SDL_PointInRect(&pt, &m_loopchip)) {
            m_seq->set_loop_enabled(!m_seq->get_loop_enabled());
            m_dirty_flag = true;
            m_down = false; m_mode = M_NONE;
            if (on_loop_changed) on_loop_changed();
            app.request_redraw();
            return true;
        }
        const long ls = m_seq->get_loop_start(), le = m_seq->get_loop_end();
        // Nothing is drawn for the default "no loop set" state (see
        // draw_ruler), so there is nothing to grab an edge/middle of either --
        // any press there always starts a fresh marquee-select.
        const bool loopSet = ls > 0 || le < m_seq->get_length();
        const int  lx = tick_to_x(ls), ex = tick_to_x(le);
        const int  endx = tick_to_x(m_seq->get_length());
        const int  kEdge = 6;
        // END vs the loop's right edge COINCIDE whenever the loop runs to the
        // end of the data (loop_end == length, which includes every pattern
        // whose loop start alone was moved).  The loop-edge test used to run
        // first and win outright, so the END marker -- drawn on top, with a
        // flag you can see -- was simply not grabbable on those patterns and
        // the pattern length could never be changed.  Resolve by DISTANCE, with
        // END taking an exact tie: it is the marker painted over the other.
        const SDL_Rect eg = end_marker_rect();
        const int dEnd = SDL_PointInRect(&pt, &eg) ? 0 : std::abs(e.x - endx);
        const int dL   = std::abs(e.x - lx), dR = std::abs(e.x - ex);
        const bool endWins = dEnd <= kEdge && dEnd <= dR;
        if (!endWins && loopSet && dL <= kEdge && dL <= dR) {
            m_mode = M_LOOP_EDGE_L;
            set_sequence_loop_start_ticks(x_to_tick(e.x));
        } else if (!endWins && loopSet && dR <= kEdge) {
            m_mode = M_LOOP_EDGE_R;
            set_sequence_loop_end_ticks(x_to_tick(e.x));
        } else if (dEnd <= kEdge) {
            // GRAB ONLY -- do not resize on the press.  A bare click near END
            // used to snap the pattern length to the nearest snap line, and
            // sequence::set_length then clamped the loop window to the new
            // (shorter) end for good.  Length now changes on MOTION only, and
            // the window is remembered so the drag stays reversible.
            m_mode = M_LENGTH_EDGE;
            m_len_drag_loop_start = ls;
            m_len_drag_loop_end   = le;
            m_len_drag_loop_valid = true;
        } else if (loopSet && ex > lx && e.x > lx && e.x < ex) {
            m_mode = M_LOOP_MOVE;
            m_loop_drag_anchor_start = ls;
            m_loop_drag_anchor_len   = le - ls;
            m_loop_drag_press_tick   = x_to_tick(e.x);
        } else {
            // GRAB ONLY -- the new region is committed on MOTION, exactly like
            // the END marker two branches up.  This used to call
            // set_sequence_loop_range_ticks(t, t) on the press itself, which
            // collapsed whatever window existed into a single snap step under
            // the pointer.  One stray click on the ruler -- at a bar number, in
            // the empty stretch past the pattern, or anywhere at all while the
            // band was scrolled or zoomed off screen and so invisible -- threw
            // the user's loop away silently, and nothing (no undo, no re-drag)
            // brought it back.  A click is not an edit; only a drag is.
            m_mode = M_LOOP_SELECT;
            m_loop_drag_press_tick = snap_tick(x_to_tick(e.x));
        }
        app.request_redraw();
        return true;
    }

    // horizontal scrollbar
    if (SDL_PointInRect(&pt, &m_hbar)) {
        m_mode = M_HBAR;
        hbar_set(e.x);
        app.request_redraw();
        return true;
    }

    if (SDL_PointInRect(&pt, &m_keys)) {
        m_mode = M_KEYS;
        m_keying_note = y_to_note(e.y);
        if (m_preview) preview_note_on(m_keying_note);
        app.request_redraw();
        return true;
    }
    if (SDL_PointInRect(&pt, &m_sbar)) {
        m_mode = M_SBAR;
        int vis = visible_keys();
        m_scroll_key = (e.y - m_grid.y) * K / (m_grid.h ? m_grid.h : 1) - vis / 2;
        clamp_scroll();
        app.request_redraw();
        return true;
    }
    if (SDL_PointInRect(&pt, &m_data)) {
        SDL_Rect hb = data_header_rect();
        if (SDL_PointInRect(&pt, &hb)) {          // click the tab -> cycle lane type
            cycle_data_type(+1);
            m_down = false; m_mode = M_NONE;
            app.request_redraw();
            return true;
        }
        m_mode = M_DATA;
        if (m_seq) m_seq->push_undo();
        apply_data_drag(app);
        app.request_redraw();
        return true;
    }
    if (SDL_PointInRect(&pt, &m_grid) && m_seq) {
        // --- tool gating: the active tool decides which Mode a press enters --
        if (m_tool == T_DRAW) {                 // paint notes at the snap grid
            m_mode = M_ADDPEND;
            m_drop_tick = snap_tick(x_to_tick(e.x));
            m_drop_note = y_to_note(e.y);
            m_seq->push_undo();
            paint_run(m_drop_tick, m_drop_tick, m_drop_note);   // first cell
            m_dirty_flag = true;
            app.request_redraw();
            return true;
        }
        if (m_tool == T_ERASE) {                // delete notes under the pointer
            m_mode = M_ERASE;
            m_seq->push_undo();
            erase_at(e.x, e.y);
            m_dirty_flag = true;
            app.request_redraw();
            return true;
        }
        if (m_tool == T_SELECT) {               // marquee only (never move/grow)
            m_mode = M_SELECT;
            app.request_redraw();
            return true;
        }
        if (m_tool == T_ZOOM) {                 // rubber-band / click zoom
            m_mode = M_ZOOM;
            app.request_redraw();
            return true;
        }
        if (m_tool == T_PAN) {                  // hand-drag scroll
            m_mode = M_PAN;
            app.request_redraw();
            return true;
        }

        // --- T_EDIT: original select / move / grow / add behaviour ----------
        long ts, tf; int note; bool edge;
        if (find_note_at(e.x, e.y, &ts, &tf, &note, &edge)) {
            bool already = m_seq->select_note_events(ts, note, ts, note,
                                                     sequence::e_is_selected);
            if (!already) {
                if (!(mod & KMOD_CTRL)) m_seq->unselect();
                m_seq->select_note_events(ts, note, ts, note, sequence::e_select_one);
            }
            capture_selbox();
            m_mode = edge ? M_GROW : M_MOVE;
            m_seq->set_dirty();
            if (m_prev_note < 0) { preview_note_on(note); m_prev_note = note; }  // preview
        } else {
            // empty: pending add / lasso -- decided on drag vs release
            m_mode = M_ADDPEND;
            m_drop_tick = snap_tick(x_to_tick(e.x));
            m_drop_note = y_to_note(e.y);
            if (m_prev_note < 0) { preview_note_on(m_drop_note); m_prev_note = m_drop_note; }  // preview
        }
        app.request_redraw();
        return true;
    }

    m_mode = M_NONE;
    m_down = false;
    return true;
}

// The data lane edits WHAT YOU DRAG OVER.
//
// sequence::change_event_data_range() has a seq24 rule baked in: if any event
// of the same status/CC is selected anywhere in the pattern, it edits ONLY
// selected events.  So once a few notes were selected in the grid, the velocity
// lane silently refused to touch anything else -- drag across the whole lane and
// only the selected handful moved, with no indication why.  The view cannot
// unmark or re-select individual events through the public API, so instead of
// fighting the filter it aims it: the span under the pointer BECOMES the
// selection, which makes the filter a no-op and shows on screen (the lane and
// the grid both draw selected events highlighted).
void PianoRoll::retarget_data_selection(long ts, long tf,
                                       unsigned char status, unsigned char cc)
{
    if (!m_seq) return;
    m_seq->unselect();
    if (status == EVENT_NOTE_ON)
        // note pairs, so both halves get selected -- select_events() would
        // select the note-on alone and leave a half-selected note behind.
        m_seq->select_note_events(ts, 127, tf, 0, sequence::e_select);
    else
        m_seq->select_events(ts, tf, status, cc, sequence::e_select);
}

// Grow a degenerate (click, not drag) tick span out to the drawn bar's width so
// a plain click lands on the mark under the pointer.  m_zoom is ticks/pixel.
void PianoRoll::widen_data_span(long& ts, long& tf) const
{
    const long pad = (long)(m_zoom > 0 ? m_zoom : 1) * 4;   // 4 px each side
    if (tf - ts < 2 * pad) {
        long mid = (ts + tf) / 2;
        ts = mid - pad;
        tf = mid + pad;
    }
    if (ts < 0) ts = 0;
    if (tf < ts) tf = ts;
}

void PianoRoll::apply_data_drag(App& app)
{
    (void)app;
    if (!m_seq) return;
    const DataLane& L = g_lanes[m_data_type];

    auto val_of = [&](int y) {
        int v = (m_data.y + m_data.h - y) * 127 / (m_data.h ? m_data.h : 1);
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    };

    // Velocity: ramp every note across the drag span (the original behaviour).
    if (L.status == EVENT_NOTE_ON) {
        int x0 = m_drop_x, y0 = m_drop_y, x1 = m_cur_x, y1 = m_cur_y;
        if (x1 < x0) { std::swap(x0, x1); std::swap(y0, y1); }
        long ts = x_to_tick(x0), tf = x_to_tick(x1);
        //  A press with no movement is a real edit, but ts == tf here and
        //  change_event_data_range() only touches events whose timestamp lands
        //  exactly inside [ts,tf] -- a tick a note-on essentially never sits on.
        //  So a single click on a velocity bar did nothing at all.  Give the hit
        //  window the width of the bar draw_data() actually paints (3 px body,
        //  5 px cap starting one pixel left) so clicking a bar sets that note.
        widen_data_span(ts, tf);
        retarget_data_selection(ts, tf, EVENT_NOTE_ON, 0);
        m_seq->change_event_data_range(ts, tf, EVENT_NOTE_ON, 0, val_of(y0), val_of(y1));
        m_seq->set_dirty();
        return;
    }

    // Other event types are a single value per column: set the event in the
    // snap cell under the cursor, creating one if none exists there yet (the
    // same way the tracker paints CC / bend / program / pressure events).
    long col = snap_tick(x_to_tick(m_cur_x));
    if (col < 0) col = 0;
    int  val = val_of(m_cur_y);
    long cf  = col + (m_snap > 0 ? m_snap - 1 : 0);

    bool exists = false;
    long et; unsigned char d0, d1; bool es;
    m_seq->reset_draw_marker();
    while (m_seq->get_next_event(L.status, L.cc, &et, &d0, &d1, &es)) {
        if (et >= col && et <= cf) { exists = true; break; }
    }

    if (exists) {
        retarget_data_selection(col, cf, L.status, L.cc);
        m_seq->change_event_data_range(col, cf, L.status, L.cc, val, val);
    } else {
        unsigned char b0, b1;
        if      (L.status == EVENT_CONTROL_CHANGE)   { b0 = L.cc;                b1 = (unsigned char)val; }
        else if (L.status == EVENT_PROGRAM_CHANGE)   { b0 = (unsigned char)val;  b1 = 0; }
        else if (L.status == EVENT_CHANNEL_PRESSURE) { b0 = (unsigned char)val;  b1 = 0; }
        else /* EVENT_PITCH_WHEEL */                 { b0 = 0;                   b1 = (unsigned char)val; }
        m_seq->add_event(col, L.status, b0, b1);
    }
    m_seq->set_dirty();
}

// ===========================================================================
//  WHEEL  (horizontal zoom;  Ctrl = vertical zoom;  Shift = vscroll;  dx = hscroll)
// ===========================================================================
bool PianoRoll::on_wheel(App& app, int dx, int dy)
{
    // A popup is modal: scrolling or zooming the grid underneath it moved the
    // content out from under a menu whose position was fixed when it opened.
    if (m_popup_open) { app.request_redraw(); return true; }
    layout();
    SDL_Keymod mod = SDL_GetModState();

    if (mod & KMOD_CTRL) {                         // vertical zoom @ cursor
        int anchor_y = (m_last_my >= m_grid.y && m_last_my <= m_grid.y + m_grid.h)
                       ? m_last_my : m_grid.y;
        int old_h = m_row_h;
        int newh  = m_row_h + dy;
        if (newh < 5)  newh = 5;
        if (newh > 24) newh = 24;
        if (newh != old_h) {
            int rows_old = (anchor_y - m_grid.y) / old_h;   // keep the note under
            m_row_h = newh;                                 // the cursor stationary
            int rows_new = (anchor_y - m_grid.y) / newh;
            m_scroll_key += rows_old - rows_new;
            clamp_scroll();
        }
    }
    else if (mod & KMOD_SHIFT) {                   // vertical scroll
        m_scroll_key -= dy * 3;
        clamp_scroll();
    }
    else if (dx != 0) {                            // horizontal scroll
        m_scroll_ticks += (long)dx * (c_ppqn / 2);
        clamp_scroll();
    }
    else {                                        // horizontal zoom @ cursor
        int anchor_x = (m_last_mx >= m_grid.x && m_last_mx <= m_grid.x + m_grid.w)
                       ? m_last_mx : m_grid.x;
        long anchor_tick = x_to_tick(anchor_x);
        int newz = m_zoom - dy;
        if (newz < 1) newz = 1;
        if (newz > 64) newz = 64;
        m_zoom = newz;
        // keep the tick under the cursor stationary
        m_scroll_ticks = anchor_tick - (long)(anchor_x - m_grid.x) * m_zoom;
        clamp_scroll();
    }
    app.request_redraw();
    return true;
}

// ===========================================================================
//  CLIPBOARD / EDIT OPS
// ===========================================================================
// True when the sequence's (static, shared) note clipboard holds something to
// paste.  get_clipboard_box() zeroes every field for an empty clipboard, so a
// non-zero span means real content -- and it keeps us from calling
// paste_selected() on an empty clipboard, which dereferences begin() == end().
bool PianoRoll::clipboard_has_notes()
{
    if (!m_seq) return false;
    long ts, tf; int nh, nl;
    m_seq->get_clipboard_box(&ts, &nh, &tf, &nl);
    return !(ts == 0 && tf == 0 && nh == 0 && nl == 0);
}

// Ctrl+C : copy the current selection into the sequence clipboard.  Guarded on a
// non-empty selection -- copy_selected() dereferences the first clipboard event
// unconditionally, so copying an empty selection would crash the engine.
void PianoRoll::copy_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    m_seq->copy_selected();
}

bool PianoRoll::delete_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return false;
    m_seq->push_undo();
    m_seq->mark_selected();
    m_seq->remove_marked();
    m_seq->verify_and_link();
    m_seq->set_dirty();
    return true;
}

// Ctrl+X : cut = copy the selection, then delete it (mark + remove, like Delete).
void PianoRoll::cut_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    m_seq->copy_selected();
    delete_selection();
}

// Ctrl+V : paste the clipboard.  paste_selected() takes an offset, so we drop it
// at the snapped tick / note under the mouse (falling back to the top-left of the
// visible grid when the pointer is off-grid).  Clearing the selection first leaves
// ONLY the freshly pasted notes selected -- the clipboard events keep their
// selected flag through the merge -- so the paste can be dragged immediately.
void PianoRoll::paste_clipboard()
{
    if (!m_seq || !clipboard_has_notes()) return;

    long tick; int note;
    SDL_Point pt{ m_last_mx, m_last_my };
    if (SDL_PointInRect(&pt, &m_grid)) {          // paste under the pointer
        tick = snap_tick(x_to_tick(m_last_mx));
        note = y_to_note(m_last_my);
    } else {                                      // off-grid: view top-left
        tick = snap_tick(m_scroll_ticks);
        note = y_to_note(m_grid.y);
    }

    m_seq->push_undo();
    m_seq->unselect();                            // so only the pasted notes stay selected
    m_seq->paste_selected(tick, note);            // top note lands at 'note', block starts at 'tick'
    m_seq->set_dirty();
}

// ===========================================================================
//  SELECTED-NOTE EDIT OPS
// ===========================================================================
// Snapshot every linked, currently-selected note as a plain record so a
// transform can rebuild it.  Reads engine ticks directly -- independent of
// zoom / scroll -- exactly like draw_notes / find_note_at walk the events.
void PianoRoll::collect_selected(std::vector<NoteRec>& out) const
{
    if (!m_seq) return;
    long ts, tf; int note, vel; bool sel;
    m_seq->reset_draw_marker();
    draw_type dt;
    while ((dt = m_seq->get_next_note_event(&ts, &tf, &note, &sel, &vel)) != DRAW_FIN) {
        if (dt == DRAW_NORMAL_LINKED && sel)
            out.push_back(NoteRec{ ts, tf, note, vel });
    }
}

// Snapshot every SELECTED event that is not half of a note pair -- CC,
// pitch-bend, aftertouch, channel pressure, program change.
//
// The rebuild transforms below all do mark_selected() / remove_marked() /
// add_notes().  mark_selected() marks EVERY selected event, but only notes are
// ever re-added, so anything else in the selection was deleted and never came
// back: press A (select all) then any transform and the pattern's entire
// controller, bend and program-change data vanished, with nothing on screen
// saying so.  (The engine hit the same bug in transpose_notes() and
// quanize_events(); both now mark only what they put back.)  The view cannot
// unmark events through the public API, so it carries them across instead.
//
// Two passes, because get_next_event() filters by status: first enumerate the
// distinct (status, cc) kinds actually present, then walk each kind.  Every
// event a sequence can hold is a 3-byte channel message -- no SysEx ever
// reaches m_list_event -- so status/d0/d1 is a lossless snapshot.
void PianoRoll::collect_selected_non_notes(std::vector<RawEvent>& out) const
{
    out.clear();
    if (!m_seq) return;

    std::vector<std::pair<unsigned char, unsigned char>> kinds;
    unsigned char st = 0, cc = 0;
    m_seq->reset_draw_marker();
    while (m_seq->get_next_event(&st, &cc)) {
        if (st == EVENT_NOTE_ON || st == EVENT_NOTE_OFF) continue;
        unsigned char key = (st == EVENT_CONTROL_CHANGE) ? cc : 0;
        bool seen = false;
        for (const auto& k : kinds)
            if (k.first == st && k.second == key) { seen = true; break; }
        if (!seen) kinds.push_back({ st, key });
    }

    for (const auto& k : kinds) {
        long tick = 0; unsigned char d0 = 0, d1 = 0; bool sel = false;
        m_seq->reset_draw_marker();
        while (m_seq->get_next_event(k.first, k.second, &tick, &d0, &d1, &sel))
            if (sel) out.push_back(RawEvent{ tick, k.first, d0, d1 });
    }
    m_seq->reset_draw_marker();
}

// Put a snapshot back.  add_event() re-sorts, and the sort is stable, so events
// keep their relative order at equal timestamps.
void PianoRoll::readd_events(const std::vector<RawEvent>& evs, bool select)
{
    if (!m_seq || evs.empty()) return;
    for (const RawEvent& r : evs) {
        event e;
        e.set_status((char)r.status);
        e.set_data((char)r.d0, (char)r.d1);
        e.set_timestamp(r.tick);
        if (select) e.select();
        m_seq->add_event(&e);
    }
    m_seq->set_dirty();
}

// The rebuild round-trip every transform shares: delete the selection, put the
// transformed notes back, and restore everything in the selection that was not
// a note.  Restore first so a transform that ends up adding no notes at all
// (every pitch clamped out of range) still cannot eat the controller lanes.
void PianoRoll::replace_selected_notes(const std::vector<NoteRec>& out)
{
    if (!m_seq) return;
    std::vector<RawEvent> keep;
    collect_selected_non_notes(keep);
    m_seq->mark_selected();
    m_seq->remove_marked();
    readd_events(keep, true);
    add_notes(out, true);
}

// Re-add note-on/off pairs with an explicit velocity.  add_note() hardcodes
// velocity 100, so -- like move_selected_notes -- we build the events directly.
// add_event(event*) does not relink, so verify_and_link() once at the end.
void PianoRoll::add_notes(const std::vector<NoteRec>& notes, bool select)
{
    if (!m_seq) return;
    bool any = false;
    long L = m_seq->get_length();
    if (L < 2) return;
    for (const NoteRec& n : notes) {
        int note = n.note;
        if (note < 0 || note > 127) continue;      // drop out-of-range pitches
        int vel = n.vel; if (vel < 1) vel = 1; if (vel > 127) vel = 127;
        long ts = n.ts < 0 ? 0 : n.ts;
        if (ts >= L - 1) continue;
        long tf = n.tf; if (tf <= ts) tf = ts + 1; // never zero-length
        if (tf >= L) tf = L - 1;
        if (tf <= ts) continue;

        event e;
        e.set_status(EVENT_NOTE_ON);
        e.set_data((char)note, (char)vel);
        e.set_timestamp(ts);
        if (select) e.select();
        m_seq->add_event(&e);                      // copies e by value

        e.set_status(EVENT_NOTE_OFF);              // reuse: same note/vel/select
        e.set_timestamp(tf);
        m_seq->add_event(&e);
        any = true;
    }
    if (!any) return;
    m_seq->verify_and_link();
    m_seq->set_dirty();
}

bool PianoRoll::move_selection(long dt, int dn)
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return false;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return false;

    long ts0, tf0; int nh, nl;
    m_seq->get_selected_box(&ts0, &nh, &tf0, &nl);
    long L = m_seq->get_length();
    if (L < 2) return false;
    if (ts0 + dt < 0) dt = -ts0;
    if (tf0 + dt >= L) dt = (L - 1) - tf0;
    if (nl + dn < 0) dn = -nl;
    if (nh + dn > 127) dn = 127 - nh;
    if (dt == 0 && dn == 0) return false;

    std::vector<NoteRec> out;
    out.reserve(notes.size());
    for (const NoteRec& n : notes)
        out.push_back(NoteRec{ n.ts + dt, n.tf + dt, n.note + dn, n.vel });

    m_seq->push_undo();
    replace_selected_notes(out);
    return true;
}

void PianoRoll::grow_selection(long delta, bool stretch)
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    long L = m_seq->get_length();
    if (L < 2) return;

    std::vector<NoteRec> out;
    out.reserve(notes.size());

    if (stretch) {
        long ts0, tf0; int nh, nl;
        m_seq->get_selected_box(&ts0, &nh, &tf0, &nl);
        long old_len = tf0 - ts0;
        long new_len = old_len + delta;
        if (old_len <= 0 || new_len <= 0) return;
        double ratio = double(new_len) / double(old_len);
        for (const NoteRec& n : notes) {
            long ns = ts0 + long((n.ts - ts0) * ratio);
            long nf = ts0 + long((n.tf - ts0) * ratio);
            if (nf <= ns) nf = ns + 1;
            out.push_back(NoteRec{ ns, nf, n.note, n.vel });
        }
    } else {
        for (const NoteRec& n : notes) {
            long len = (n.tf - n.ts) + delta;
            if (len < 1) len = 1;
            out.push_back(NoteRec{ n.ts, n.ts + len, n.note, n.vel });
        }
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// Ctrl+D : clone the selection in place, shifted right by its own span.  Leaves
// ONLY the clones selected (drag / duplicate again).  Rebuilds notes directly,
// so it never disturbs the shared copy/paste clipboard.
void PianoRoll::duplicate_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    long ts0, tf0; int nh, nl;
    m_seq->get_selected_box(&ts0, &nh, &tf0, &nl);
    long span = tf0 - ts0;
    if (span <= 0) span = m_note_length;

    std::vector<NoteRec> copies;
    for (const NoteRec& n : notes)
        copies.push_back(NoteRec{ n.ts + span, n.tf + span, n.note, n.vel });

    m_seq->push_undo();
    m_seq->unselect();                             // originals drop out of the selection
    add_notes(copies, true);
}

// L : stretch each selected note so it ends at the next selected note's start.
// Notes sharing a start (a chord) all extend to the next distinct start.
void PianoRoll::legato_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        long next = -1;
        for (const NoteRec& o : notes)
            if (o.ts > n.ts && (next < 0 || o.ts < next)) next = o.ts;
        long tf = (next < 0) ? n.tf : next;        // last note keeps its end
        if (tf <= n.ts) tf = n.tf;                 // safety
        out.push_back(NoteRec{ n.ts, tf, n.note, n.vel });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// H : deterministic velocity + small timing jitter.  A member counter seeds an
// xorshift state so repeated presses differ, yet the result is reproducible and
// never touches rand().
void PianoRoll::humanize_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    m_humanize_ctr++;
    m_rng_state ^= 0x9E3779B9u * m_humanize_ctr;   // stir the counter into the state
    if (m_rng_state == 0) m_rng_state = 0x2545F491u;

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        int  dv = (int)(next_rand() % 17) - 8;     // velocity  +/- 8
        long dt = (long)(next_rand() % 7) - 3;     // timing    +/- 3 ticks
        long ts = n.ts + dt; if (ts < 0) ts = 0;
        out.push_back(NoteRec{ ts, n.tf + dt, n.note, n.vel + dv });
    }

    m_seq->push_undo();
    replace_selected_notes(out);                          // clamps velocity 1..127, length >= 1
}

// '=' : force every selected note to the current note-length (uniform).
void PianoRoll::set_uniform_length()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes)
        out.push_back(NoteRec{ n.ts, n.ts + m_note_length, n.note, n.vel });

    m_seq->push_undo();
    replace_selected_notes(out);
}

// '.' / ',' : shift selected velocities by +/- a step, clamped 1..127.  Rebuilt
// because increment_selected() wraps at 127/0 (m_data[1] & 0x7F) instead.
void PianoRoll::change_velocity(int d)
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        int v = n.vel + d;
        if (v < 1) v = 1; if (v > 127) v = 127;
        out.push_back(NoteRec{ n.ts, n.tf, n.note, v });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// R : reverse the selection in time -- mirror each note inside the selection's
// [minStart,maxEnd] span, preserving length, pitch and velocity.
void PianoRoll::reverse_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    long ts0, tf0; int nh, nl;
    m_seq->get_selected_box(&ts0, &nh, &tf0, &nl);

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        long len = n.tf - n.ts;
        long ns  = ts0 + (tf0 - n.tf);             // note end mirrors to the new start
        out.push_back(NoteRec{ ns, ns + len, n.note, n.vel });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// I : melodic inversion -- reflect each pitch about the selection's average.
void PianoRoll::invert_selection()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    long sum = 0;
    for (const NoteRec& n : notes) sum += n.note;
    int avg = (int)((sum + (long)notes.size() / 2) / (long)notes.size());

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        int p = 2 * avg - n.note;
        if (p < 0) p = 0; if (p > 127) p = 127;
        out.push_back(NoteRec{ n.ts, n.tf, p, n.vel });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// N : normalize -- scale every selected velocity so the loudest hits 127.
void PianoRoll::normalize_velocities()
{
    if (!m_seq || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    int maxv = 1;
    for (const NoteRec& n : notes) if (n.vel > maxv) maxv = n.vel;
    if (maxv >= 127) return;                       // already peaked -- no-op

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        int v = n.vel * 127 / maxv;
        if (v < 1) v = 1; if (v > 127) v = 127;
        out.push_back(NoteRec{ n.ts, n.tf, n.note, v });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// Ctrl+[ / Ctrl+] : resize -- scale each selected note's length by mul/div
// about its own start (pitch, start and velocity unchanged).
void PianoRoll::resize_selection(int mul, int div)
{
    if (!m_seq || div <= 0 || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        long len = (n.tf - n.ts) * mul / div;
        if (len < 1) len = 1;
        out.push_back(NoteRec{ n.ts, n.ts + len, n.note, n.vel });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// { / } : rescale -- time-stretch the selection about its start tick, scaling
// both each note's offset-from-start and its length by mul/div.
void PianoRoll::rescale_selection(int mul, int div)
{
    if (!m_seq || div <= 0 || m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    long ts0, tf0; int nh, nl;
    m_seq->get_selected_box(&ts0, &nh, &tf0, &nl);

    std::vector<NoteRec> out;
    for (const NoteRec& n : notes) {
        long ns  = ts0 + (n.ts - ts0) * mul / div;
        long len = (n.tf - n.ts) * mul / div;
        if (ns < 0)  ns = 0;
        if (len < 1) len = 1;
        out.push_back(NoteRec{ ns, ns + len, n.note, n.vel });
    }

    m_seq->push_undo();
    replace_selected_notes(out);
}

// K : re-snap every selected pitch onto the current scale.  Does NOT push undo
// -- callers (transpose) fold it into their own undo frame.
void PianoRoll::snap_selection_to_scale()
{
    if (!m_seq || m_scale <= 0 || m_scale == 4) return;   // off / chromatic
    if (m_seq->get_num_selected_notes() <= 0) return;
    std::vector<NoteRec> notes;
    collect_selected(notes);
    if (notes.empty()) return;

    bool changed = false;
    for (NoteRec& n : notes) {
        int s = snap_note_to_scale(n.note);
        if (s != n.note) { n.note = s; changed = true; }
    }
    if (!changed) return;

    replace_selected_notes(notes);
}

// xorshift32 -- deterministic PRNG for humanize (never rand()).
unsigned PianoRoll::next_rand()
{
    unsigned x = m_rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    if (x == 0) x = 0x2545F491u;                   // xorshift must never latch to 0
    m_rng_state = x;
    return x;
}

// ===========================================================================
//  KEYBOARD
// ===========================================================================
bool PianoRoll::on_key(App& app, SDL_Keycode k)
{
    // Keys used to reach the grid straight through an open popup -- Delete
    // erased notes, the tool keys switched tools -- and nothing dismissed it.
    if (m_popup_open) {
        if (k == SDLK_ESCAPE) m_popup_open = false;
        app.request_redraw();
        return true;
    }
    if (!m_seq) return false;                      // guard: no model bound
    layout();                                      // keep m_grid / scroll current for paste
    const bool ctrl  = (SDL_GetModState() & KMOD_CTRL)  != 0;
    const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
    bool handled = false;

    switch (k) {
    case SDLK_1: set_tool(T_EDIT);   handled = true; break;   // tool accelerators
    case SDLK_2: set_tool(T_DRAW);   handled = true; break;
    case SDLK_3: set_tool(T_ERASE);  handled = true; break;
    case SDLK_4: set_tool(T_SELECT); handled = true; break;
    case SDLK_5: set_tool(T_ZOOM);   handled = true; break;
    case SDLK_6: set_tool(T_PAN);    handled = true; break;

    case SDLK_DELETE:
    case SDLK_BACKSPACE:                           // remove selected notes
        handled = delete_selection();
        break;
    case SDLK_a:                                   // A / Ctrl+A : select all
        m_seq->select_all();
        m_seq->set_dirty();
        handled = true;
        break;
    case SDLK_c:                                   // Ctrl+C : copy selection
        if (ctrl) { copy_selection();  handled = true; }
        break;
    case SDLK_x:                                   // Ctrl+X : cut selection
        if (ctrl) { cut_selection();   handled = true; }
        break;
    case SDLK_v:                                   // Ctrl+V paste / V cycle lane type
        if (ctrl) paste_clipboard();
        else      cycle_data_type(+1);
        handled = true;
        break;
    case SDLK_u:                                   // undo
        m_seq->pop_undo();
        handled = true;
        break;
    case SDLK_z:                                   // Ctrl+Z undo, Ctrl+Shift+Z redo
        if (ctrl) { if (shift) m_seq->pop_redo(); else m_seq->pop_undo(); handled = true; }
        break;
    case SDLK_y:                                   // Ctrl+Y : redo
        if (ctrl) { m_seq->pop_redo(); handled = true; }
        break;

    case SDLK_q:                                   // Q : quantize starts to snap (keep length)
        if (m_snap > 0 && m_seq->get_num_selected_notes() > 0) {
            m_seq->push_undo();
            m_seq->quanize_events(EVENT_NOTE_ON, 0, m_snap, 1, true, false);
            m_seq->set_dirty();
        }
        handled = true;
        break;

    case SDLK_UP:                                  // transpose up (Shift = octave)
        if (m_seq->get_num_selected_notes() > 0) {
            if (move_selection(0, shift ? 12 : 1)) {
                snap_selection_to_scale();         // K : keep pitches in scale
                m_seq->set_dirty();
            }
        }
        handled = true;
        break;
    case SDLK_DOWN:                                // transpose down (Shift = octave)
        if (m_seq->get_num_selected_notes() > 0) {
            if (move_selection(0, shift ? -12 : -1)) {
                snap_selection_to_scale();
                m_seq->set_dirty();
            }
        }
        handled = true;
        break;
    case SDLK_PAGEUP:                              // transpose up one octave
        if (m_seq->get_num_selected_notes() > 0) {
            if (move_selection(0, 12)) {
                snap_selection_to_scale();
                m_seq->set_dirty();
            }
        }
        handled = true;
        break;
    case SDLK_PAGEDOWN:                            // transpose down one octave
        if (m_seq->get_num_selected_notes() > 0) {
            if (move_selection(0, -12)) {
                snap_selection_to_scale();
                m_seq->set_dirty();
            }
        }
        handled = true;
        break;

    case SDLK_LEFT: {                              // nudge earlier: one snap, Ctrl=one beat
        if (m_seq->get_num_selected_notes() > 0) {
            long ts0, tf0; int nh, nl;
            m_seq->get_selected_box(&ts0, &nh, &tf0, &nl);
            long step = ctrl ? (long)((4 * c_ppqn) /
                        (m_seq->get_bw() > 0 ? (int)m_seq->get_bw() : 4)) : (long)m_snap;
            long d = -step;
            if (ts0 + d < 0) d = -ts0;             // keep earliest note at/after 0
            if (d != 0)
                move_selection(d, 0);
        }
        handled = true;
        break; }
    case SDLK_RIGHT:                               // nudge later: one snap, Ctrl=one beat
        if (m_seq->get_num_selected_notes() > 0) {
            long step = ctrl ? (long)((4 * c_ppqn) /
                        (m_seq->get_bw() > 0 ? (int)m_seq->get_bw() : 4)) : (long)m_snap;
            move_selection(step, 0);
        }
        handled = true;
        break;

    case SDLK_d:                                   // Ctrl+D duplicate / D drum mode
        if (ctrl) duplicate_selection();
        else      m_drum_mode = !m_drum_mode;
        handled = true;
        break;
    case SDLK_b:                                   // B : toggle note-name labels
        m_show_labels = !m_show_labels;
        handled = true;
        break;
    case SDLK_k:                                   // K cycle scale / Shift+K cycle root
        if (shift) m_scale_root = (m_scale_root + 1) % 12;
        else       m_scale = (m_scale + 1) % 5;
        handled = true;
        break;
    case SDLK_n:                                   // N : normalize velocities
        normalize_velocities();
        handled = true;
        break;

    case SDLK_l:
        // Ctrl+Shift+L : clear the loop WINDOW -- put it back to spanning the
        // whole pattern, which is the "no loop set" default (see
        // sequence::m_loop_start).  There was no way to get rid of a window
        // once one existed: a click in the ruler used to collapse it to a snap
        // step rather than clear it (and that click is now a no-op, precisely
        // because collapsing was never what anyone meant), and dragging a fresh
        // region can only ever make another window.
        if (m_seq && ctrl && shift) {
            set_sequence_loop_range_ticks(0, m_seq->get_length());
            handled = true;
            break;
        }
        // Ctrl+L : loop the SELECTION -- the same gesture (and the same key) as
        // TrackerView's "Loop Selection", so the two editors of one pattern do
        // not need two different vocabularies for one window.
        if (m_seq && ctrl) {
            std::vector<NoteRec> sel;
            collect_selected(sel);
            if (!sel.empty()) {
                long lo = sel[0].ts, hi = sel[0].tf;
                for (const NoteRec& n : sel) {
                    if (n.ts < lo) lo = n.ts;
                    if (n.tf > hi) hi = n.tf;
                }
                set_sequence_loop_range_ticks(lo, hi);
            }
            handled = true;
            break;
        }
        // Shift+L : loop on/off for THIS clip.  Off == one-shot: the data
        // plays once and dragging the clip longer in Arrange just moves its
        // end point instead of repeating the pattern.
        if (m_seq && (SDL_GetModState() & KMOD_SHIFT)) {
            m_seq->set_loop_enabled(!m_seq->get_loop_enabled());
            m_dirty_flag = true;
            if (on_loop_changed) on_loop_changed();
            handled = true;
            break;
        }
        // L : legato
        legato_selection();
        handled = true;
        break;
    case SDLK_h:                                   // H : humanize
        humanize_selection();
        handled = true;
        break;

    case SDLK_RIGHTBRACKET:                        // ] snap+ / Ctrl+] resize x2 / } rescale x2
        if (ctrl)       resize_selection(2, 1);    // scale note lengths up
        else if (shift) rescale_selection(2, 1);   // time-stretch selection out
        else if (m_seq->get_num_selected_notes() > 0) {
            grow_selection((long)m_snap, false);
        }
        handled = true;
        break;
    case SDLK_LEFTBRACKET:                         // [ snap- / Ctrl+[ resize x.5 / { rescale x.5
        if (ctrl)       resize_selection(1, 2);    // scale note lengths down
        else if (shift) rescale_selection(1, 2);   // time-compress selection
        else if (m_seq->get_num_selected_notes() > 0) {
            grow_selection(-(long)m_snap, false);
        }
        handled = true;
        break;

    case SDLK_EQUALS:                              // = : uniform note length
        set_uniform_length();
        handled = true;
        break;

    case SDLK_PERIOD:                              // . : velocity up
        change_velocity(8);
        handled = true;
        break;
    case SDLK_COMMA:                               // , : velocity down
        change_velocity(-8);
        handled = true;
        break;

    case SDLK_r:                                   // R : reverse in time
        reverse_selection();
        handled = true;
        break;
    case SDLK_i:                                   // I : invert pitch
        invert_selection();
        handled = true;
        break;

    case SDLK_g: {                                 // G : cycle snap size (1/1..1/32)
        static const int divs[] = { 4 * c_ppqn, 2 * c_ppqn, c_ppqn,
                                    c_ppqn / 2, c_ppqn / 4, c_ppqn / 8 };
        const int N = (int)(sizeof(divs) / sizeof(divs[0]));
        int idx = 4;                               // default to 1/16 if off-grid
        for (int j = 0; j < N; ++j) if (divs[j] == m_snap) { idx = j; break; }
        m_snap = divs[(idx + 1) % N];
        if (m_snap < 1) m_snap = 1;
        handled = true;
        break; }

    case SDLK_ESCAPE:                              // clear selection
        m_seq->unselect();
        m_seq->set_dirty();
        handled = true;
        break;

    default: break;
    }

    if (handled) {
        // light the status [*] for edits (not tool switches / undo / deselect)
        if (k != SDLK_1 && k != SDLK_2 && k != SDLK_3 && k != SDLK_4 &&
            k != SDLK_5 && k != SDLK_6 && k != SDLK_u && k != SDLK_ESCAPE)
            m_dirty_flag = true;
        app.request_redraw();
    }
    return handled;
}

} // namespace ui
