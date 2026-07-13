//----------------------------------------------------------------------------
//  sdlui/views/piano_roll/pianoroll.cpp  -- see pianoroll.h
//
//  Ported 1:1 from the GTK originals:
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

#include <algorithm>
#include <cstdio>
#include <cmath>

namespace ui {

// GTK constant for a full keyboard height mirror -- we keep our own row height
// but reuse the engine's ppqn / key count so ticks & notes line up exactly.
static const int K = c_num_keys;   // 128

// ---------------------------------------------------------------------------
PianoRoll::PianoRoll(sequence* seq) : m_seq(seq)
{
    m_snap        = c_ppqn / 4;     // 16th note
    m_note_length = c_ppqn / 4;
}

void PianoRoll::set_sequence(sequence* seq) { m_seq = seq; }
void PianoRoll::set_snap(int t)        { m_snap = t > 0 ? t : 1; }
void PianoRoll::set_note_length(int t) { m_note_length = t > 0 ? t : 1; }
void PianoRoll::set_zoom(int z)        { m_zoom = z < 1 ? 1 : (z > 64 ? 64 : z); }

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

    int grid_h = rect.h - time_h - data_h - gap;
    if (grid_h < m_row_h) grid_h = m_row_h;
    int grid_w = rect.w - key_w - sb_w;
    if (grid_w < 1) grid_w = 1;

    m_ruler = { rect.x + key_w, rect.y,          grid_w, time_h };
    m_keys  = { rect.x,         rect.y + time_h, key_w,  grid_h };
    m_grid  = { rect.x + key_w, rect.y + time_h, grid_w, grid_h };
    m_sbar  = { m_grid.x + grid_w, rect.y + time_h, sb_w, grid_h };
    m_data  = { m_grid.x, m_grid.y + grid_h + gap, grid_w, data_h };

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
    return t - (t % m_snap);
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

// ===========================================================================
//  DRAW
// ===========================================================================
void PianoRoll::draw(App& app)
{
    if (!visible) return;
    layout();
    const Theme& t = theme();

    // whole widget backdrop
    fill_rect(app.ren, rect, t.bg);

    draw_grid(app);
    draw_notes(app);
    draw_overlay(app);
    draw_playhead(app);
    draw_keys(app);
    draw_ruler(app);
    draw_data(app);

    frame_rect(app.ren, m_grid, t.dim);
}

// ---- note grid: striping + beat/bar lines ---------------------------------
void PianoRoll::draw_grid(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;

    SDL_RenderSetClipRect(r, &m_grid);
    fill_rect(r, m_grid, t.bg);

    // horizontal: row striping by black/white key + octave separators
    int vis = m_grid.h / m_row_h + 2;
    for (int i = 0; i < vis; ++i) {
        int note = (K - 1) - (m_scroll_key + i);
        if (note < 0) break;
        int key = ((note % 12) + 12) % 12;
        bool is_black = (key == 1 || key == 3 || key == 6 || key == 8 || key == 10);
        int y = m_grid.y + i * m_row_h;

        if (is_black)
            fill_rect(r, SDL_Rect{ m_grid.x, y, m_grid.w, m_row_h }, t.panel);

        // divider: brighter under each C (octave boundary), else faint
        hline(r, m_grid.x, m_grid.x + m_grid.w, y, key == 0 ? t.dim : t.panel);
    }

    // vertical: bar / beat / sub-beat lines
    int tpb  = (4 * c_ppqn) / (m_seq ? (int)m_seq->get_bw()  : 4);          // ticks/beat
    int tpbar= (m_seq ? (int)m_seq->get_bpm() : 4) * (4 * c_ppqn) /
               (m_seq ? (int)m_seq->get_bw() : 4);                          // ticks/measure
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

    SDL_RenderSetClipRect(r, nullptr);
}

// ---- notes ----------------------------------------------------------------
void PianoRoll::draw_notes(App& app)
{
    if (!m_seq) return;
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    SDL_RenderSetClipRect(r, &m_grid);

    long ts, tf; int note, vel; bool sel;
    m_seq->reset_draw_marker();
    draw_type dt;
    while ((dt = m_seq->get_next_note_event(&ts, &tf, &note, &sel, &vel)) != DRAW_FIN) {

        int x = tick_to_x(ts);
        int w;
        if (dt == DRAW_NORMAL_LINKED) {
            w = int((tf - ts) / m_zoom);
        } else {
            w = 8 / m_zoom;                 // unlinked stub
        }
        if (w < 1) w = 1;
        int y = note_to_y(note);
        int h = m_row_h - 2;
        if (h < 2) h = 2;

        if (x + w < m_grid.x || x > m_grid.x + m_grid.w) continue;   // off-screen
        if (y + h < m_grid.y || y > m_grid.y + m_grid.h) continue;

        SDL_Rect nb{ x, y + 1, w, h };
        fill_rect(r, nb, sel ? t.notesel : t.note);
        frame_rect(r, nb, t.hi);

        // right-edge resize hint
        if (w >= 6) {
            fill_rect(r, SDL_Rect{ x + w - 3, y + 2, 2, h - 2 }, t.hi);
        }
    }
    SDL_RenderSetClipRect(r, nullptr);
}

// ---- overlay: lasso box / move / grow preview -----------------------------
void PianoRoll::draw_overlay(App& app)
{
    if (m_mode == M_NONE || !m_dragging) return;
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    SDL_RenderSetClipRect(r, &m_grid);

    if (m_mode == M_SELECT || m_mode == M_ADDPEND) {
        int x0 = std::min(m_drop_x, m_cur_x), x1 = std::max(m_drop_x, m_cur_x);
        int y0 = std::min(m_drop_y, m_cur_y), y1 = std::max(m_drop_y, m_cur_y);
        frame_rect(r, SDL_Rect{ x0, y0, x1 - x0, y1 - y0 }, t.accent);
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

    SDL_RenderSetClipRect(r, nullptr);
}

// ---- playhead -------------------------------------------------------------
void PianoRoll::draw_playhead(App& app)
{
    if (!m_seq) return;
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
    SDL_RenderSetClipRect(r, &m_keys);
    fill_rect(r, m_keys, t.bg);

    int vis = m_grid.h / m_row_h + 2;
    for (int i = 0; i < vis; ++i) {
        int note = (K - 1) - (m_scroll_key + i);
        if (note < 0) break;
        int key = ((note % 12) + 12) % 12;
        bool is_black = (key == 1 || key == 3 || key == 6 || key == 8 || key == 10);
        bool is_root  = (key == 0);
        int y = m_keys.y + i * m_row_h;

        // white keys use the bright 'hi' role, black keys the dark 'keybg'
        // role, root C the accent -- 1:1 with GTK seqkeys (cHi/cBlk/cAccent).
        Color face = is_root ? t.accent : (is_black ? t.keybg : t.hi);
        int kx = m_keys.x + 10, kw = m_keys.w - 12;
        fill_rect(r, SDL_Rect{ kx, y + 1, kw, m_row_h - 1 }, face);
        hline(r, m_keys.x, m_keys.x + m_keys.w, y + m_row_h, t.dim);

        // hover hint (from grid / keys motion)
        if (note == m_keying_note)
            fill_rect(r, SDL_Rect{ kx, y + 1, kw, m_row_h - 1 }, t.active);

        // octave label on each C
        if (is_root && m_row_h >= 8) {
            char lbl[8];
            int oct = (note / 12) - 1;
            snprintf(lbl, sizeof lbl, "C%d", oct);
            app.mono.draw(r, m_keys.x + 1, y + (m_row_h - app.mono.ch()) / 2, lbl, t.bg);
        }
    }
    vline(r, m_keys.x + m_keys.w - 1, m_keys.y, m_keys.y + m_keys.h, t.dim);
    SDL_RenderSetClipRect(r, nullptr);
}

// ---- top ruler ------------------------------------------------------------
void PianoRoll::draw_ruler(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    SDL_RenderSetClipRect(r, &m_ruler);
    fill_rect(r, m_ruler, t.panel);
    hline(r, m_ruler.x, m_ruler.x + m_ruler.w, m_ruler.y + m_ruler.h - 1, t.dim);

    int tpbar = (m_seq ? (int)m_seq->get_bpm() : 4) * (4 * c_ppqn) /
                (m_seq ? (int)m_seq->get_bw() : 4);
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

    // END marker at sequence length
    if (m_seq) {
        int ex = tick_to_x(m_seq->get_length());
        if (ex >= m_ruler.x - 20 && ex <= m_ruler.x + m_ruler.w) {
            SDL_Rect eb{ ex, m_ruler.y + m_ruler.h / 2, 22, m_ruler.h / 2 };
            fill_rect(r, eb, t.active);
            app.mono.draw(r, ex + 1, m_ruler.y + m_ruler.h / 2, "END", t.bg);
        }
    }
    SDL_RenderSetClipRect(r, nullptr);
}

// ---- bottom velocity lane -------------------------------------------------
void PianoRoll::draw_data(App& app)
{
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    SDL_RenderSetClipRect(r, &m_data);
    fill_rect(r, m_data, t.panel);
    hline(r, m_data.x, m_data.x + m_data.w, m_data.y, t.dim);

    // reference lines at 25/50/75/100 %
    for (int v = 1; v <= 4; ++v) {
        int y = m_data.y + m_data.h - (m_data.h * v) / 4;
        hline(r, m_data.x, m_data.x + m_data.w, y, t.dim);
    }

    if (m_seq) {
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
    }
    SDL_RenderSetClipRect(r, nullptr);
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
        int w  = int((b - a) / m_zoom); if (w < 1) w = 1;
        int x1 = x0 + w;
        int y0 = note_to_y(n);
        int y1 = y0 + m_row_h;
        if (x >= x0 - 1 && x <= x1 + 2 && y >= y0 && y <= y1) {
            // topmost/last wins; remember the note under cursor
            found = true; best_ts = a; best_tf = b; best_n = n; best_edge_x = x1;
        }
    }
    if (!found) return false;
    *ts = best_ts; *tf = best_tf; *note = best_n;
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

        switch (m_mode) {
        case M_KEYS:
            if (m_preview && m_seq && m_keying_note >= 0) m_seq->play_note_off(m_keying_note);
            m_keying_note = -1;
            break;

        case M_ADDPEND:                         // click with no drag -> add note
            if (m_seq &&
                !m_seq->select_note_events(m_drop_tick, m_drop_note,
                                           m_drop_tick, m_drop_note,
                                           sequence::e_would_select)) {
                m_seq->push_undo();
                m_seq->add_note(m_drop_tick, m_note_length - 2, m_drop_note, true);
                m_seq->unpaint_all();
                m_seq->set_dirty();
            }
            break;

        case M_SELECT: {                        // lasso
            if (m_seq) {
                long ts = x_to_tick(std::min(m_drop_x, m_cur_x));
                long tf = x_to_tick(std::max(m_drop_x, m_cur_x));
                int  nh = y_to_note(std::min(m_drop_y, m_cur_y));
                int  nl = y_to_note(std::max(m_drop_y, m_cur_y));
                if (!(mod & KMOD_CTRL)) m_seq->unselect();
                m_seq->select_note_events(ts, nh, tf, nl, sequence::e_select);
                m_seq->set_dirty();
            }
            break; }

        case M_MOVE: {
            long dt = snap_tick(x_to_tick(m_cur_x)) - snap_tick(x_to_tick(m_drop_x));
            int  dn = y_to_note(m_cur_y) - y_to_note(m_drop_y);
            if (m_seq && (dt != 0 || dn != 0)) {
                m_seq->push_undo();
                m_seq->move_selected_notes(dt, dn);
                m_seq->set_dirty();
            }
            break; }

        case M_GROW: {
            long newend = snap_tick(x_to_tick(m_cur_x));
            long delta  = newend - m_sel_tf;
            if (m_seq && delta != 0) {
                m_seq->push_undo();
                if (mod & KMOD_SHIFT) m_seq->stretch_selected(delta);
                else                  m_seq->grow_selected(delta);
                m_seq->set_dirty();
            }
            break; }

        default: break;
        }

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
                    if (m_keying_note >= 0) m_seq->play_note_off(m_keying_note);
                    m_seq->play_note_on(n);
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
        case M_ADDPEND: if (m_dragging) m_mode = M_SELECT; break;
        default: break;
        }
        app.request_redraw();
        return true;
    }

    // ---- genuine press --------------------------------------------------
    m_down = true; m_dragging = false;
    m_drop_x = m_cur_x = e.x; m_drop_y = m_cur_y = e.y;
    SDL_Keymod mod = SDL_GetModState();
    SDL_Point pt{ e.x, e.y };

    if (SDL_PointInRect(&pt, &m_keys)) {
        m_mode = M_KEYS;
        m_keying_note = y_to_note(e.y);
        if (m_preview && m_seq) m_seq->play_note_on(m_keying_note);
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
        m_mode = M_DATA;
        if (m_seq) m_seq->push_undo();
        apply_data_drag(app);
        app.request_redraw();
        return true;
    }
    if (SDL_PointInRect(&pt, &m_grid) && m_seq) {
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
        } else {
            // empty: pending add / lasso -- decided on drag vs release
            m_mode = M_ADDPEND;
            m_drop_tick = snap_tick(x_to_tick(e.x));
            m_drop_note = y_to_note(e.y);
        }
        app.request_redraw();
        return true;
    }

    m_mode = M_NONE;
    m_down = false;
    return true;
}

void PianoRoll::apply_data_drag(App& app)
{
    (void)app;
    if (!m_seq) return;
    int x0 = m_drop_x, y0 = m_drop_y, x1 = m_cur_x, y1 = m_cur_y;
    if (x1 < x0) { std::swap(x0, x1); std::swap(y0, y1); }
    long ts = x_to_tick(x0), tf = x_to_tick(x1);
    auto vel_of = [&](int y) {
        int v = (m_data.y + m_data.h - y) * 127 / (m_data.h ? m_data.h : 1);
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    };
    m_seq->change_event_data_range(ts, tf, EVENT_NOTE_ON, 0, vel_of(y0), vel_of(y1));
    m_seq->set_dirty();
}

// ===========================================================================
//  WHEEL  (horizontal zoom;  Shift = vscroll;  Ctrl = hscroll)
// ===========================================================================
bool PianoRoll::on_wheel(App& app, int dx, int dy)
{
    layout();
    SDL_Keymod mod = SDL_GetModState();

    if (mod & KMOD_SHIFT) {                       // vertical scroll
        m_scroll_key -= dy * 3;
        clamp_scroll();
    }
    else if ((mod & KMOD_CTRL) || dx != 0) {      // horizontal scroll
        long d = (dx != 0 ? dx : dy);
        m_scroll_ticks += d * (c_ppqn / 2);
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
//  KEYBOARD
// ===========================================================================
bool PianoRoll::on_key(App& app, SDL_Keycode k)
{
    if (!m_seq) return false;
    bool handled = false;
    switch (k) {
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        m_seq->push_undo();
        m_seq->mark_selected();
        m_seq->remove_marked();
        m_seq->set_dirty();
        handled = true;
        break;
    case SDLK_a:                          // select all
        m_seq->select_all();
        handled = true;
        break;
    case SDLK_u:                          // undo
        m_seq->pop_undo();
        handled = true;
        break;
    default: break;
    }
    if (handled) app.request_redraw();
    return handled;
}

} // namespace ui
