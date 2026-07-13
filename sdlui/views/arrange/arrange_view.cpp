//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_view.cpp  -- implementation.  See arrange_view.h.
//
//  Ports src/perfroll.cpp + src/perfnames.cpp + src/perftime.cpp onto the SDL2
//  toolkit.  All GDK pixmap/Gdk::GC drawing becomes ui:: draw helpers; the
//  colour roles map 1:1 (m_black->bg, m_panel->panel, m_grey->accent,
//  m_lt_grey/m_dk_grey->dim, m_white->hi, m_note->note).
//----------------------------------------------------------------------------
#include "arrange_view.h"
#include "perform.h"
#include "sequence.h"
#include "globals.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

using namespace ui;

namespace arrange {

//----------------------------------------------------------------------------
//  small drawing helpers
//----------------------------------------------------------------------------

// Filled rounded rectangle (row-span scanline with circular corners).
static void fill_round(SDL_Renderer* r, SDL_Rect q, int rad, Color c)
{
    if (q.w <= 0 || q.h <= 0) return;
    if (rad * 2 > q.w) rad = q.w / 2;
    if (rad * 2 > q.h) rad = q.h / 2;
    if (rad < 1) { fill_rect(r, q, c); return; }
    set_color(r, c);
    for (int yy = 0; yy < q.h; ++yy) {
        int dx = 0;
        int dy = -1;
        if (yy < rad)              dy = rad - 1 - yy;
        else if (yy >= q.h - rad)  dy = yy - (q.h - rad);
        if (dy >= 0)
            dx = rad - (int)floor(sqrt((double)(rad * rad - dy * dy)));
        SDL_Rect ln{ q.x + dx, q.y + yy, q.w - 2 * dx, 1 };
        SDL_RenderFillRect(r, &ln);
    }
}

// Small utilitarian push-button (ports perfnames::draw_button).
static void draw_button(App& app, SDL_Rect q, const char* label, bool engaged)
{
    const Theme& t = theme();
    fill_rect(app.ren, q, engaged ? t.accent : t.panel);
    frame_rect(app.ren, q, engaged ? t.accent : t.dim);
    app.mono.draw_centered(app.ren, q, label, engaged ? t.bg : t.text);
}

//----------------------------------------------------------------------------
//  construction
//----------------------------------------------------------------------------
ArrangeView::ArrangeView(perform* p)
    : m_perf(p),
      m_scale_x(c_perf_scale_x),          // 32 ticks / pixel
      m_scroll_ticks(0),
      m_v_offset(0),
      m_snap(c_ppqn),                      // 1 beat
      m_measure_len(c_ppqn * 4),           // 4/4 bar
      m_beat_len(c_ppqn)
{
    m_solo.assign(c_max_sequence, 0);
    m_mute_snapshot.assign(c_max_sequence, 0);
}

//----------------------------------------------------------------------------
//  coordinate helpers
//----------------------------------------------------------------------------
std::vector<int> ArrangeView::active_list() const
{
    std::vector<int> v;
    for (int i = 0; i < c_max_sequence; ++i)
        if (m_perf->is_active(i)) v.push_back(i);
    return v;
}

int ArrangeView::tick_to_x(long tick) const
{
    return canvas_x() + (int)((double)(tick - m_scroll_ticks) / m_scale_x);
}

long ArrangeView::x_to_tick(int px) const
{
    long t = m_scroll_ticks + (long)((double)(px - canvas_x()) * m_scale_x);
    return t < 0 ? 0 : t;
}

long ArrangeView::snap(long tick) const
{
    if (m_snap <= 0) return tick;
    return tick - (tick % m_snap);
}

int ArrangeView::row_at(int py) const
{
    if (py < canvas_y()) return -1;
    return (py - canvas_y()) / row_h;
}

long ArrangeView::playhead() const
{
    return playhead_tick >= 0 ? playhead_tick : m_perf->get_tick();
}

//----------------------------------------------------------------------------
//  draw
//----------------------------------------------------------------------------
void ArrangeView::draw(App& app)
{
    if (!visible) return;
    const Theme& t = theme();
    std::vector<int> act = active_list();

    fill_rect(app.ren, rect, t.bg);

    draw_canvas (app, act);   // lanes + grid + clips + playhead
    draw_headers(app, act);   // left track-header column
    draw_ruler  (app, act);   // top time ruler + L/R markers

    // top-left corner box (over the header/ruler junction)
    SDL_Rect corner{ rect.x, rect.y, header_w, ruler_h };
    fill_rect(app.ren, corner, t.panel);
    hline(app.ren, corner.x, corner.x + corner.w, corner.y + corner.h - 1, t.accent);
    vline(app.ren, corner.x + corner.w - 1, corner.y, corner.y + corner.h, t.accent);
    app.mono.draw(app.ren, corner.x + 6, corner.y + (ruler_h - app.mono.ch()) / 2,
                  "ARRANGE", t.text);

    frame_rect(app.ren, rect, t.dim);
}

//----------------------------------------------------------------------------
//  canvas : lanes, bar/beat grid, clip blocks, playhead
//----------------------------------------------------------------------------
void ArrangeView::draw_canvas(App& app, const std::vector<int>& act)
{
    const Theme& t = theme();
    SDL_Rect cv{ canvas_x(), canvas_y(), canvas_w(), canvas_h() };
    fill_rect(app.ren, cv, t.bg);

    int rows_visible = canvas_h() / row_h + 1;

    // lane stripes + bottom separators
    for (int r = 0; r < rows_visible; ++r) {
        int idx = m_v_offset + r;
        int y = canvas_y() + r * row_h;
        if (y >= canvas_y() + canvas_h()) break;
        Color lane = (r % 2 == 0) ? t.panel : t.bg;
        fill_rect(app.ren, SDL_Rect{ cv.x, y, cv.w, row_h }, lane);
        hline(app.ren, cv.x, cv.x + cv.w, y + row_h - 1, t.dim);
        (void)idx;
    }

    // bar / beat grid (ports perfroll::draw_background_on grid section)
    int beat_px = (int)(m_beat_len / m_scale_x);
    bool draw_beats = beat_px >= 6;
    long first = m_scroll_ticks - (m_scroll_ticks % m_beat_len);
    for (long tick = first; ; tick += m_beat_len) {
        int x = tick_to_x(tick);
        if (x > cv.x + cv.w) break;
        if (x < cv.x) continue;
        bool measure = (tick % m_measure_len) == 0;
        if (!measure && !draw_beats) continue;
        vline(app.ren, x, cv.y, cv.y + cv.h, measure ? t.accent : t.dim);
    }

    // clip blocks per visible active track
    for (int r = 0; r < rows_visible; ++r) {
        int idx = m_v_offset + r;
        if (idx < 0 || idx >= (int)act.size()) continue;
        int y = canvas_y() + r * row_h;
        if (y >= canvas_y() + canvas_h()) break;
        draw_clips(app, act[idx], y);
    }

    // playhead (ports perfroll::draw_progress)
    int px = tick_to_x(playhead());
    if (px >= cv.x && px <= cv.x + cv.w)
        vline(app.ren, px, cv.y, cv.y + cv.h, t.hi);
}

//----------------------------------------------------------------------------
//  one track's clip blocks (ports perfroll::draw_sequence_on)
//----------------------------------------------------------------------------
void ArrangeView::draw_clips(App& app, int seq, int lane_y)
{
    if (!m_perf->is_active(seq)) return;
    const Theme& t = theme();
    sequence* s = m_perf->get_sequence(seq);
    s->reset_draw_trigger_marker();

    int cvx = canvas_x();
    int cvr = canvas_x() + canvas_w();

    long seq_len = s->get_length();
    int  length_w = (int)(seq_len / m_scale_x);

    long tick_on, tick_off, offset;
    bool selected;
    while (s->get_next_trigger(&tick_on, &tick_off, &selected, &offset)) {
        if (tick_off <= 0) continue;

        int x_on  = tick_to_x(tick_on);
        int x_off = tick_to_x(tick_off);
        if (x_off < cvx || x_on > cvr) continue;          // off-screen

        int y = lane_y + 3;
        int h = row_h - 6;
        int full_x = x_on;

        // clamp the drawn body to the canvas
        int bx = x_on  < cvx ? cvx : x_on;
        int br = x_off > cvr ? cvr : x_off;
        int bw = br - bx + 1;
        if (bw < 2) bw = 2;

        // body: rounded, light grey (near-white when selected)
        fill_round(app.ren, SDL_Rect{ bx, y, bw, h }, 4, selected ? t.notesel : t.note);

        // ---- tiny note preview tiled across the clip (perfroll port) -------
        int lowest  = s->get_lowest_note_event();
        int highest = s->get_highest_note_event();
        if (highest >= lowest && length_w > 1) {
            int height = highest - lowest + 2;
            long first_marker =
                tick_on - (tick_on % seq_len) + (offset % seq_len) - seq_len;
            set_color(app.ren, t.dim);
            for (long marker = first_marker; marker < tick_off; marker += seq_len) {
                int marker_x = tick_to_x(marker);
                long tick_s, tick_f; int note, vel; bool nsel; draw_type dt;
                s->reset_draw_marker();
                while ((dt = s->get_next_note_event(&tick_s, &tick_f, &note,
                                                    &nsel, &vel)) != DRAW_FIN) {
                    int note_y = ((h - 8) - ((h - 8) * (note - lowest)) / height) + 4;
                    int ns_x = (int)(((long)tick_s * length_w) / seq_len) + marker_x;
                    int nf_x = (int)(((long)tick_f * length_w) / seq_len) + marker_x;
                    if (dt == DRAW_NOTE_ON || dt == DRAW_NOTE_OFF) nf_x = ns_x + 1;
                    if (nf_x <= ns_x) nf_x = ns_x + 1;
                    if (ns_x < bx) ns_x = bx;
                    if (nf_x > bx + bw) nf_x = bx + bw;
                    if (nf_x >= bx && ns_x <= bx + bw)
                        SDL_RenderDrawLine(app.ren, ns_x, y + note_y, nf_x, y + note_y);
                }
            }
        }

        // outline + resize handles
        frame_rect(app.ren, SDL_Rect{ bx, y, bw, h }, t.accent);
        if (x_on >= cvx) {
            fill_rect(app.ren, SDL_Rect{ full_x + 1, y + 1, 3, 3 }, t.accent);
            fill_rect(app.ren, SDL_Rect{ full_x + 1, y + h - 4, 3, 3 }, t.accent);
        }

        // title bar with pattern name (drawn last so it stays legible)
        const char* nm = s->get_name();
        if (nm && bw > 12) {
            int tb = app.mono.ch() + 1;
            if (tb > h - 2) tb = h - 2;
            fill_rect(app.ren, SDL_Rect{ bx + 1, y + 1, bw - 2, tb }, t.accent);
            int maxc = (bw - 4) / (app.mono.cw() ? app.mono.cw() : 6);
            if (maxc > 0) {
                char lbl[48];
                std::snprintf(lbl, sizeof(lbl), "%s", nm);
                if ((int)std::strlen(lbl) > maxc) lbl[maxc] = 0;
                app.mono.draw(app.ren, bx + 2, y + 1, lbl, t.bg);
            }
        }
    }
}

//----------------------------------------------------------------------------
//  left track-header column (ports perfnames::draw_sequence)
//----------------------------------------------------------------------------
void ArrangeView::draw_headers(App& app, const std::vector<int>& act)
{
    const Theme& t = theme();
    const int spine_w = 4;
    const int badge_x = spine_w + 6;
    const int badge_w = 22;
    const int name_x  = badge_x + badge_w + 6;
    const int btn_w   = 22;
    const int btn_h   = 12;
    const int btn_x   = header_w - btn_w - 4;
    const int vu_w    = 10;
    const int vu_x    = btn_x - vu_w - 6;

    int rows_visible = canvas_h() / row_h + 1;

    for (int r = 0; r < rows_visible; ++r) {
        int idx = m_v_offset + r;
        int y0 = canvas_y() + r * row_h;
        if (y0 >= canvas_y() + canvas_h()) break;

        SDL_Rect hdr{ rect.x, y0, header_w, row_h };
        Color bg = (r % 2 == 0) ? t.panel : t.bg;
        fill_rect(app.ren, hdr, bg);
        hline(app.ren, hdr.x, hdr.x + hdr.w, y0 + row_h - 1, t.dim);

        if (idx < 0 || idx >= (int)act.size()) continue;
        int seq = act[idx];
        sequence* s = m_perf->get_sequence(seq);
        bool muted = s->get_song_mute();

        // status spine
        fill_rect(app.ren, SDL_Rect{ hdr.x, y0 + 1, spine_w, row_h - 2 },
                  muted ? t.dim : t.hi);

        // INS type badge
        SDL_Rect badge{ hdr.x + badge_x, y0 + 4, badge_w, row_h - 8 };
        frame_rect(app.ren, badge, t.dim);
        app.mono.draw_centered(app.ren, badge, "INS", t.accent);

        // name (line 1)
        char name[64];
        std::snprintf(name, sizeof(name), "%02d %.16s", seq + 1, s->get_name());
        app.mono.draw(app.ren, hdr.x + name_x, y0 + 5, name, t.text);

        // bus / channel / time-sig (line 2)
        char info[48];
        std::snprintf(info, sizeof(info), "b%d ch%d  %ld/%ld",
                      s->get_midi_bus(), s->get_midi_channel() + 1,
                      s->get_bpm(), s->get_bw());
        app.mono.draw(app.ren, hdr.x + name_x, y0 + 6 + app.mono.ch(), info, t.dim);

        // level / VU strip placeholder (pseudo level per track)
        int vu_y = y0 + 4, vu_h = row_h - 8;
        frame_rect(app.ren, SDL_Rect{ hdr.x + vu_x, vu_y, vu_w, vu_h }, t.dim);
        if (!muted) {
            int level = 30 + (seq * 37) % 60;            // 30..90 %
            int filled = vu_h * level / 100;
            for (int yy = vu_y + vu_h - 2; yy > vu_y + vu_h - filled; yy -= 3) {
                Color seg = (yy < vu_y + vu_h / 3) ? t.hi : t.accent;
                hline(app.ren, hdr.x + vu_x + 2, hdr.x + vu_x + vu_w - 2, yy, seg);
            }
        }

        // Mute / Solo toggle buttons
        draw_button(app, SDL_Rect{ hdr.x + btn_x, y0 + 4, btn_w, btn_h }, "M", muted);
        draw_button(app, SDL_Rect{ hdr.x + btn_x, y0 + 4 + btn_h + 2, btn_w, btn_h },
                    "S", m_solo[seq] != 0);
    }
}

//----------------------------------------------------------------------------
//  top time ruler (ports perftime::on_expose_event)
//----------------------------------------------------------------------------
void ArrangeView::draw_ruler(App& app, const std::vector<int>& act)
{
    (void)act;
    const Theme& t = theme();
    SDL_Rect rl{ canvas_x(), rect.y, canvas_w(), ruler_h };
    fill_rect(app.ren, rl, t.panel);
    hline(app.ren, rl.x, rl.x + rl.w, rl.y + rl.h - 1, t.accent);

    long first = m_scroll_ticks - (m_scroll_ticks % m_measure_len);
    int bar = (int)(first / m_measure_len);
    for (long tick = first; ; tick += m_measure_len, ++bar) {
        int x = tick_to_x(tick);
        if (x > rl.x + rl.w) break;
        if (x >= rl.x) {
            vline(app.ren, x, rl.y, rl.y + rl.h, t.accent);
            char b[8]; std::snprintf(b, sizeof(b), "%d", bar + 1);
            app.mono.draw(app.ren, x + 2, rl.y + 1, b, t.text);
        }
    }

    // L / R loop markers
    long left  = m_perf->get_left_tick();
    long right = m_perf->get_right_tick();
    int lx = tick_to_x(left);
    int rx = tick_to_x(right);
    if (lx >= rl.x && lx <= rl.x + rl.w) {
        fill_rect(app.ren, SDL_Rect{ lx, rl.y + rl.h - 10, 8, 10 }, t.accent);
        app.mono.draw(app.ren, lx + 1, rl.y + rl.h - 10, "L", t.bg);
    }
    if (rx >= rl.x && rx <= rl.x + rl.w) {
        fill_rect(app.ren, SDL_Rect{ rx - 7, rl.y + rl.h - 10, 8, 10 }, t.accent);
        app.mono.draw(app.ren, rx - 6, rl.y + rl.h - 10, "R", t.bg);
    }

    // playhead nub on the ruler
    int px = tick_to_x(playhead());
    if (px >= rl.x && px <= rl.x + rl.w) {
        for (int i = 0; i < 5; ++i)
            hline(app.ren, px - (4 - i), px + (4 - i), rl.y + 2 + i, t.hi);
    }
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
bool ArrangeView::on_mouse(App& app, const MouseEv& e)
{
    // release ---------------------------------------------------------------
    if (!e.pressed) {
        m_mouse_down = m_moving = m_growing = m_adding = false;
        return true;
    }

    // drag (continue whatever mode the press established) --------------------
    if (m_mouse_down) {
        if (m_moving || m_growing || m_adding) drag_canvas(app, e);
        return true;
    }

    // fresh press -----------------------------------------------------------
    m_mouse_down = true;

    const int spine_end = 4 + 6;                 // spine + gap
    const int btn_w = 22, btn_h = 12;
    const int btn_x = header_w - btn_w - 4;

    // (a) header column ------------------------------------------------------
    if (e.x < rect.x + header_w && e.y >= canvas_y()) {
        int r = row_at(e.y);
        std::vector<int> act = active_list();
        int idx = m_v_offset + r;
        if (idx < 0 || idx >= (int)act.size()) return true;
        int seq = act[idx];
        sequence* s = m_perf->get_sequence(seq);
        int lx = e.x - rect.x;                    // x within header
        int ly = e.y - (canvas_y() + r * row_h);  // y within row

        // Mute button
        if (lx >= btn_x && lx <= btn_x + btn_w && ly >= 4 && ly <= 4 + btn_h) {
            s->set_song_mute(!s->get_song_mute());
            app.request_redraw(); return true;
        }
        // Solo button
        if (lx >= btn_x && lx <= btn_x + btn_w &&
            ly >= 4 + btn_h + 2 && ly <= 4 + 2 * btn_h + 2) {
            m_solo[seq] = !m_solo[seq];
            apply_solo(); app.request_redraw(); return true;
        }
        // spine / left area -> quick mute toggle
        if (lx < spine_end) {
            s->set_song_mute(!s->get_song_mute());
            app.request_redraw(); return true;
        }
        return true;
    }

    // (b) ruler --------------------------------------------------------------
    if (e.y < canvas_y() && e.x >= rect.x + header_w) {
        long tick = snap(x_to_tick(e.x));
        if (e.button == SDL_BUTTON_LEFT)  m_perf->set_left_tick(tick);
        if (e.button == SDL_BUTTON_RIGHT) m_perf->set_right_tick(tick + m_snap);
        app.request_redraw();
        return true;
    }

    // (c) canvas -------------------------------------------------------------
    if (e.x >= rect.x + header_w && e.y >= canvas_y()) {
        int r = row_at(e.y);
        std::vector<int> act = active_list();
        int idx = m_v_offset + r;
        if (idx < 0 || idx >= (int)act.size()) return true;
        press_canvas(app, e, act[idx]);
    }
    return true;
}

void ArrangeView::press_canvas(App& app, const MouseEv& e, int seq)
{
    if (!m_perf->is_active(seq)) return;
    sequence* s = m_perf->get_sequence(seq);

    // clear any prior selection on the previously-touched track
    if (m_drop_seq >= 0 && m_drop_seq != seq && m_perf->is_active(m_drop_seq))
        m_perf->get_sequence(m_drop_seq)->unselect_triggers();

    long tick = x_to_tick(e.x);
    m_drop_seq  = seq;
    m_drop_tick = tick;

    // right-click deletes a clip under the cursor
    if (e.button == SDL_BUTTON_RIGHT) {
        if (s->get_trigger_state(tick)) {
            m_perf->push_trigger_undo();
            s->del_trigger(tick);
            app.request_redraw();
        }
        return;
    }
    // middle-click splits a clip
    if (e.button == SDL_BUTTON_MIDDLE) {
        if (s->get_trigger_state(tick)) {
            m_perf->push_trigger_undo();
            s->split_trigger(tick);
            app.request_redraw();
        }
        return;
    }
    if (e.button != SDL_BUTTON_LEFT) return;

    long seq_len = s->get_length();
    bool state = s->get_trigger_state(tick);

    if (state) {
        // select + decide move vs. resize by proximity to the clip edges
        m_perf->push_trigger_undo();
        s->select_trigger(tick);
        long start = s->get_selected_trigger_start_tick();
        long end   = s->get_selected_trigger_end_tick();
        int xs = tick_to_x(start), xe = tick_to_x(end);
        const int handle = 6;
        if (e.x - xs <= handle) {
            m_growing = true; m_grow_dir = true;  m_drop_offset = tick - start;
        } else if (xe - e.x <= handle) {
            m_growing = true; m_grow_dir = false; m_drop_offset = tick - end;
        } else {
            m_moving = true; m_drop_offset = tick - start;
        }
    } else {
        // empty lane: place a fresh clip (snapped to pattern length), then grow
        m_perf->push_trigger_undo();
        long t = tick - (tick % seq_len);
        s->add_trigger(t, seq_len);
        m_adding = true;
        m_drop_tick = t;                          // a tick inside the new clip
    }
    app.request_redraw();
}

void ArrangeView::drag_canvas(App& app, const MouseEv& e)
{
    if (m_drop_seq < 0 || !m_perf->is_active(m_drop_seq)) return;
    sequence* s = m_perf->get_sequence(m_drop_seq);
    long tick = x_to_tick(e.x);

    if (m_adding) {
        long seq_len = s->get_length();
        s->grow_trigger(m_drop_tick, tick, seq_len);
    } else if (m_moving) {
        long t = snap(tick - m_drop_offset);
        s->move_selected_triggers_to(t, true);
    } else if (m_growing) {
        long t = snap(tick - m_drop_offset);
        if (m_grow_dir) s->move_selected_triggers_to(t, false, 0);
        else            s->move_selected_triggers_to(t - 1, false, 1);
    }
    app.request_redraw();
}

bool ArrangeView::on_wheel(App& app, int dx, int dy)
{
    if (dy != 0) {
        // vertical wheel = horizontal zoom (ticks per pixel)
        double f = (dy > 0) ? (1.0 / 1.2) : 1.2;
        double ns = m_scale_x * f;
        if (ns < 2.0)   ns = 2.0;
        if (ns > 512.0) ns = 512.0;
        m_scale_x = ns;
    }
    if (dx != 0) {
        m_scroll_ticks += (long)(dx * 8 * m_scale_x);
        if (m_scroll_ticks < 0) m_scroll_ticks = 0;
    }
    app.request_redraw();
    return true;
}

bool ArrangeView::on_key(App& app, SDL_Keycode k)
{
    long page = (long)(canvas_w() * m_scale_x / 4);
    switch (k) {
    case SDLK_LEFT:  m_scroll_ticks -= page; if (m_scroll_ticks < 0) m_scroll_ticks = 0; break;
    case SDLK_RIGHT: m_scroll_ticks += page; break;
    case SDLK_UP:    if (m_v_offset > 0) --m_v_offset; break;
    case SDLK_DOWN:  ++m_v_offset; break;
    case SDLK_HOME:  m_scroll_ticks = 0; m_v_offset = 0; break;
    case SDLK_EQUALS:
    case SDLK_PLUS:  m_scale_x = std::max(2.0,  m_scale_x / 1.2); break;
    case SDLK_MINUS: m_scale_x = std::min(512.0, m_scale_x * 1.2); break;
    case SDLK_t:     set_mode(mode() == Mode::Light ? Mode::Midnight : Mode::Light); break;
    case SDLK_DELETE:
        if (m_drop_seq >= 0 && m_perf->is_active(m_drop_seq)) {
            m_perf->push_trigger_undo();
            m_perf->get_sequence(m_drop_seq)->del_selected_trigger();
        }
        break;
    default: return false;
    }
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  solo (ports perfnames::apply_solo)
//----------------------------------------------------------------------------
void ArrangeView::apply_solo()
{
    bool any = false;
    for (int i = 0; i < c_max_sequence; ++i)
        if (m_perf->is_active(i) && m_solo[i]) { any = true; break; }

    if (any) {
        if (!m_solo_active) {
            for (int i = 0; i < c_max_sequence; ++i)
                if (m_perf->is_active(i))
                    m_mute_snapshot[i] = m_perf->get_sequence(i)->get_song_mute();
            m_solo_active = true;
        }
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i))
                m_perf->get_sequence(i)->set_song_mute(!m_solo[i]);
    } else if (m_solo_active) {
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i))
                m_perf->get_sequence(i)->set_song_mute(m_mute_snapshot[i] != 0);
        m_solo_active = false;
    }
}

} // namespace arrange
