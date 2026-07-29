//----------------------------------------------------------------------------
//  sdlui/views/master_mixer/master_mixer_view.cpp -- see master_mixer_view.h.
//
//  A horizontal row of channel strips over callback-supplied state.  Strictly
//  two-tone via ui::theme(); all text is ASCII through the monospace atlas.
//----------------------------------------------------------------------------
#include "master_mixer_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using ui::App;
using ui::Color;
using ui::MouseEv;
using ui::Theme;
using ui::theme;

namespace {

const double PI = 3.14159265358979323846;

inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline int   imax(int a, int b) { return a > b ? a : b; }

inline bool in_rect(const SDL_Rect& q, int x, int y) {
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

// squared-distance point test (generous hit radius for the small knobs)
inline bool near_pt(int x, int y, int cx, int cy, int rad) {
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

// clip `s` to at most `cells` monospace glyphs (no ellipsis -- terse strip font)
std::string clip_cells(const std::string& s, int cells) {
    if (cells <= 0) return std::string();
    if ((int)s.size() <= cells) return s;
    return s.substr(0, (size_t)cells);
}

// ---- filled disc (rack-editor style) ---------------------------------------
void fill_disc(SDL_Renderer* r, int cx, int cy, int rad, Color c) {
    if (rad < 1) rad = 1;
    ui::set_color(r, c);
    for (int dy = -rad; dy <= rad; ++dy) {
        int dx = int(std::floor(std::sqrt(double(rad * rad - dy * dy))));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// ---- ~2px circle outline (midpoint), drawn at rad and rad-1 ----------------
void circle_ring(SDL_Renderer* r, int cx, int cy, int rad, Color c) {
    ui::set_color(r, c);
    for (int rr = rad; rr >= rad - 1 && rr > 0; --rr) {
        int x = rr, y = 0, err = 1 - rr;
        while (x >= y) {
            SDL_RenderDrawPoint(r, cx + x, cy + y);
            SDL_RenderDrawPoint(r, cx + y, cy + x);
            SDL_RenderDrawPoint(r, cx - y, cy + x);
            SDL_RenderDrawPoint(r, cx - x, cy + y);
            SDL_RenderDrawPoint(r, cx - x, cy - y);
            SDL_RenderDrawPoint(r, cx - y, cy - x);
            SDL_RenderDrawPoint(r, cx + y, cy - x);
            SDL_RenderDrawPoint(r, cx + x, cy - y);
            ++y;
            if (err < 0) err += 2 * y + 1;
            else { --x; err += 2 * (y - x) + 1; }
        }
    }
}

// knob metrics shared by layout + drawing
const int KNOB_R       = 9;    // aux-send knob radius
const int PAN_BAR_H    = 9;    // pan slider bar height
const int KNOB_DRAG_PX = 110;  // pointer travel (px) for a full 0..1 knob sweep
const int WHEEL_STEP   = 24;   // px scrolled per wheel notch

} // anonymous namespace

namespace mixerui {

// ============================================================================
//  Owned faders -- one per bus + the master.  Rebuilt only when the count
//  changes; each fader's meter (level) + gain (changed) callbacks capture the
//  strip index, and value is re-synced from get_gain() every draw.
// ============================================================================
void MasterMixerView::ensure_faders() {
    int want = (bus_count < 0 ? 0 : bus_count) + 1;   // +1 for the master strip
    if ((int)m_faders.size() == want) return;

    m_faders.clear();
    m_faders.resize((size_t)want);
    for (int i = 0; i < want; ++i) {
        ui::Fader& f = m_faders[(size_t)i];
        f.level   = [this, i]() -> float {
            float v = get_level ? get_level(i) : 0.f;
            return v < 0.f ? 0.f : v;                 // Fader clamps the top end
        };
        f.changed = [this, i](float v) {
            if (set_gain) set_gain(i, clamp01(v));
        };
    }
}

// ============================================================================
//  Geometry
// ============================================================================
int MasterMixerView::strip_x(int idx) const {
    return rect.x + PAD - m_scroll_x + idx * (STRIP_W + GAP);
}

SDL_Rect MasterMixerView::strip_area(int idx) const {
    SDL_Rect a;
    a.x = strip_x(idx);
    a.y = rect.y + PAD;
    a.w = (idx == bus_count) ? MASTER_W : STRIP_W;
    a.h = imax(40, rect.h - 2 * PAD);
    return a;
}

int MasterMixerView::content_w() const {
    int bc = bus_count < 0 ? 0 : bus_count;
    return 2 * PAD + bc * (STRIP_W + GAP) + MASTER_W;
}

void MasterMixerView::clamp_scroll() {
    int maxsx = content_w() - rect.w;
    if (maxsx < 0) maxsx = 0;
    if (m_scroll_x > maxsx) m_scroll_x = maxsx;
    if (m_scroll_x < 0)     m_scroll_x = 0;
}

// One place computes every sub-rect of a strip so draw() and hit-testing agree.
MasterMixerView::Sub MasterMixerView::layout(App& app, int idx, const SDL_Rect& area) const {
    Sub L;
    L.area = area;

    const int ip = 5;
    int ix = area.x + ip;
    int iw = area.w - 2 * ip; if (iw < 8) iw = 8;
    const int fch = app.font.ch();
    const int mch = app.mono.ch();

    // 1. name header (spans the full strip width)
    int hh = fch + 6;
    L.header = SDL_Rect{ area.x + 1, area.y + 1, area.w - 2, hh };
    int y = area.y + 1 + hh + 4;

    // 2. FX insert slot
    int fxh = mch + 6;
    L.fx = SDL_Rect{ ix, y, iw, fxh };
    y += fxh + 4;

    // 3. aux-send knob grid
    L.knob_r = KNOB_R;
    int aux = aux_count < 0 ? 0 : aux_count;
    int cell = 2 * L.knob_r + 6;
    L.knobs_per_row = iw / (cell > 0 ? cell : 1);
    if (L.knobs_per_row < 1)   L.knobs_per_row = 1;
    if (aux > 0 && L.knobs_per_row > aux) L.knobs_per_row = aux;
    int rows = aux > 0 ? (aux + L.knobs_per_row - 1) / L.knobs_per_row : 0;
    L.send_cell_w = iw / L.knobs_per_row;
    L.send_cell_h = 2 * L.knob_r + 2 + mch;
    int sendsH = rows * L.send_cell_h;
    L.sends = SDL_Rect{ ix, y, iw, sendsH };
    y += sendsH + (rows > 0 ? 4 : 0);

    // 4. pan (label + readout line, then the slider bar)
    int panH = mch + 2 + PAN_BAR_H;
    L.pan     = SDL_Rect{ ix, y, iw, panH };
    L.pan_bar = SDL_Rect{ ix, y + mch + 2, iw, PAN_BAR_H };
    y += panH + 5;

    // 6 (bottom-anchored). mute / solo buttons, and 5b the dB readout above them
    int bottom = area.y + area.h - 4;
    int btnH = mch + 6;
    int dbH  = mch + 2;
    int halfW = (iw - 3) / 2;
    L.mute = SDL_Rect{ ix, bottom - btnH, halfW, btnH };
    L.solo = SDL_Rect{ ix + halfW + 3, bottom - btnH, iw - halfW - 3, btnH };
    int dbY = bottom - btnH - 2 - dbH;
    L.db = SDL_Rect{ ix, dbY, iw, dbH };

    // 5. the big fader fills whatever vertical space is left in the middle
    int faderTop = y;
    int faderBot = dbY - 3;
    int faderH = faderBot - faderTop; if (faderH < 24) faderH = 24;
    L.fader = SDL_Rect{ ix, faderTop, iw, faderH };

    return L;
}

bool MasterMixerView::send_pos(const Sub& L, int aux, int& cx, int& cy, int& r) const {
    int aux_n = aux_count < 0 ? 0 : aux_count;
    if (aux < 0 || aux >= aux_n || L.knobs_per_row <= 0) return false;
    int row = aux / L.knobs_per_row;
    int col = aux % L.knobs_per_row;
    cx = L.sends.x + col * L.send_cell_w + L.send_cell_w / 2;
    cy = L.sends.y + row * L.send_cell_h + L.knob_r + 1;
    r  = L.knob_r;
    return true;
}

// ============================================================================
//  Drawing
// ============================================================================
void MasterMixerView::draw(App& app) {
    if (!visible) return;
    ensure_faders();
    clamp_scroll();

    const Theme& t = theme();
    SDL_Renderer* r = app.ren;

    ui::fill_rect(r, rect, t.bg);

    SDL_Rect clip = rect;
    SDL_RenderSetClipRect(r, &clip);

    int total = (bus_count < 0 ? 0 : bus_count) + 1;
    for (int i = 0; i < total; ++i) {
        SDL_Rect a = strip_area(i);
        if (a.x + a.w < rect.x || a.x > rect.x + rect.w) continue;   // cull off-screen
        draw_strip(app, i, a);
    }

    SDL_RenderSetClipRect(r, nullptr);
    ui::frame_rect(r, rect, t.dim);

    app.request_redraw();   // keep the fader level meters live
}

void MasterMixerView::draw_strip(App& app, int idx, const SDL_Rect& area) {
    const Theme& t = theme();
    SDL_Renderer* r = app.ren;
    const bool master = (idx == bus_count);
    const int mcw = app.mono.cw() > 0 ? app.mono.cw() : 8;

    Sub L = layout(app, idx, area);

    // strip body -- master gets an accent frame so it reads as separate
    ui::fill_rect(r, area, t.panel);
    ui::frame_rect(r, area, master ? t.accent : t.dim);

    // 1. NAME header (accent bg for the master)
    ui::fill_rect(r, L.header, master ? t.accent : t.keybg);
    {
        std::string nm = get_label ? get_label(idx)
                                   : (master ? std::string("MASTER")
                                             : std::to_string(idx + 1));
        nm = clip_cells(nm, (L.header.w - 4) / mcw);
        app.font.draw_centered(r, L.header, nm, master ? t.bg : t.text);
    }

    // 2. FX INSERT slot
    {
        std::string ins = get_insert ? get_insert(idx) : std::string();
        bool empty = ins.empty();
        ui::fill_rect(r, L.fx, t.keybg);
        ui::frame_rect(r, L.fx, t.dim);
        std::string s = empty ? std::string("[+FX]") : ins;
        s = clip_cells(s, (L.fx.w - 4) / mcw);
        app.mono.draw_centered(r, L.fx, s, empty ? t.dim : t.accent);
    }

    // 3. AUX SEND knobs
    {
        int aux_n = aux_count < 0 ? 0 : aux_count;
        for (int a = 0; a < aux_n; ++a) {
            int cx, cy, rr;
            if (!send_pos(L, a, cx, cy, rr)) continue;
            float val = get_send ? clamp01(get_send(idx, a)) : 0.f;

            fill_disc(r, cx, cy, rr, t.keybg);
            circle_ring(r, cx, cy, rr, t.dim);

            // indicator line: -135deg (min) .. +135deg (max), 0 at 12 o'clock
            double ang = (-135.0 + val * 270.0) * PI / 180.0;
            int ex = cx + (int)std::lround(std::sin(ang) * (rr - 2));
            int ey = cy - (int)std::lround(std::cos(ang) * (rr - 2));
            ui::set_color(r, t.accent);
            SDL_RenderDrawLine(r, cx, cy, ex, ey);
            SDL_RenderDrawLine(r, cx + 1, cy, ex + 1, ey);
            SDL_RenderDrawLine(r, cx, cy + 1, ex, ey + 1);

            char lb[8]; std::snprintf(lb, sizeof(lb), "A%d", a + 1);
            int lw = app.mono.text_w(lb);
            app.mono.draw(r, cx - lw / 2, cy + rr + 1, lb, t.dim);
        }
    }

    // 4. PAN (label + C/L/R readout, then the slider bar)
    {
        float pv = get_pan ? clampf(get_pan(idx), -1.f, 1.f) : 0.f;
        char rd[8];
        if (std::fabs(pv) < 0.06f)   std::snprintf(rd, sizeof(rd), "C");
        else if (pv < 0.f)           std::snprintf(rd, sizeof(rd), "L%d", (int)std::lround(-pv * 100));
        else                         std::snprintf(rd, sizeof(rd), "R%d", (int)std::lround(pv * 100));

        app.mono.draw(r, L.pan.x, L.pan.y, "PAN", t.dim);
        int rw = app.mono.text_w(rd);
        app.mono.draw(r, L.pan.x + L.pan.w - rw, L.pan.y, rd, t.text);

        ui::fill_rect(r, L.pan_bar, t.keybg);
        ui::frame_rect(r, L.pan_bar, t.dim);
        int cx = L.pan_bar.x + L.pan_bar.w / 2;                       // centre detent
        ui::vline(r, cx, L.pan_bar.y + 2, L.pan_bar.y + L.pan_bar.h - 3, t.dim);
        float norm = pv * 0.5f + 0.5f;                               // -1..1 -> 0..1
        int kx = L.pan_bar.x + 2 + (int)(clamp01(norm) * (L.pan_bar.w - 6));
        SDL_Rect knob{ kx, L.pan_bar.y + 2, 4, L.pan_bar.h - 4 };
        ui::fill_rect(r, knob, t.accent);
    }

    // 5. the big ui::Fader (metered dB backdrop) -- we OWN it; sync value + rect
    {
        ui::Fader& f = m_faders[(size_t)idx];
        f.rect = L.fader;
        if (!f.m_drag && get_gain) f.value = clamp01(get_gain(idx));
        f.draw(app);

        // tiny dB readout (from get_gain, 20*log10) just above the buttons
        float v = get_gain ? get_gain(idx) : 0.f;
        char buf[16];
        if (v <= 1e-4f) std::snprintf(buf, sizeof(buf), "-inf");
        else            std::snprintf(buf, sizeof(buf), "%+.1f", 20.0f * std::log10(v));
        app.mono.draw_centered(r, L.db, buf, master ? t.accent : t.text);
    }

    // 6. MUTE / SOLO (lit accent / note when engaged)
    {
        bool mute = get_mute ? get_mute(idx) : false;
        bool solo = get_solo ? get_solo(idx) : false;

        ui::fill_rect(r, L.mute, mute ? t.accent : t.keybg);
        ui::frame_rect(r, L.mute, t.dim);
        app.mono.draw_centered(r, L.mute, "M", mute ? t.bg : t.dim);

        ui::fill_rect(r, L.solo, solo ? t.note : t.keybg);
        ui::frame_rect(r, L.solo, t.dim);
        app.mono.draw_centered(r, L.solo, "S", solo ? t.bg : t.dim);
    }
}

// ============================================================================
//  Input
// ============================================================================
bool MasterMixerView::on_mouse(App& app, const MouseEv& e) {
    ensure_faders();

    // ---- continue an in-flight drag (motion has e.pressed == true) ---------
    if (m_drag == D_FADER && m_drag_strip >= 0 && m_drag_strip < (int)m_faders.size()) {
        SDL_Rect a = strip_area(m_drag_strip);
        Sub L = layout(app, m_drag_strip, a);
        ui::Fader& f = m_faders[(size_t)m_drag_strip];
        f.rect = L.fader;
        f.on_mouse(app, e);                         // absolute vertical mapping
        if (!e.pressed) { m_drag = D_NONE; m_drag_strip = -1; }
        app.request_redraw();
        return true;
    }
    if (m_drag == D_SEND) {
        apply_send(app, e);
        if (!e.pressed) { m_drag = D_NONE; m_drag_strip = -1; m_drag_aux = -1; }
        app.request_redraw();
        return true;
    }
    if (m_drag == D_PAN) {
        apply_pan(app, e);
        if (!e.pressed) { m_drag = D_NONE; m_drag_strip = -1; }
        app.request_redraw();
        return true;
    }
    if (m_drag == D_CONSUME) {
        if (!e.pressed) m_drag = D_NONE;            // swallow motion until release
        return true;
    }

    if (!e.pressed) return true;                    // stray release: nothing armed

    // ---- fresh press: find the strip under the cursor, dispatch to a control
    int total = (bus_count < 0 ? 0 : bus_count) + 1;
    for (int i = 0; i < total; ++i) {
        SDL_Rect a = strip_area(i);
        if (in_rect(a, e.x, e.y)) { begin_press(app, i, e); return true; }
    }
    m_drag = D_CONSUME;                             // clicked the gutter: swallow
    return true;
}

void MasterMixerView::begin_press(App& app, int idx, const MouseEv& e) {
    Sub L = layout(app, idx, strip_area(idx));

    // FX insert slot
    if (in_rect(L.fx, e.x, e.y)) {
        if (on_insert) on_insert(idx);
        m_drag = D_CONSUME;
        app.request_redraw();
        return;
    }
    // aux-send knobs (relative vertical drag)
    {
        int aux_n = aux_count < 0 ? 0 : aux_count;
        for (int a = 0; a < aux_n; ++a) {
            int cx, cy, rr;
            if (send_pos(L, a, cx, cy, rr) && near_pt(e.x, e.y, cx, cy, rr + 3)) {
                m_drag = D_SEND; m_drag_strip = idx; m_drag_aux = a;
                m_drag_start = get_send ? clamp01(get_send(idx, a)) : 0.f;
                m_drag_y0 = e.y;
                app.request_redraw();
                return;
            }
        }
    }
    // pan bar (absolute horizontal position)
    if (in_rect(L.pan_bar, e.x, e.y)) {
        m_drag = D_PAN; m_drag_strip = idx;
        apply_pan(app, e);
        return;
    }
    // fader (forward to the owned widget)
    if (in_rect(L.fader, e.x, e.y)) {
        m_drag = D_FADER; m_drag_strip = idx;
        ui::Fader& f = m_faders[(size_t)idx];
        f.rect = L.fader;
        f.on_mouse(app, e);
        app.request_redraw();
        return;
    }
    // mute / solo
    if (in_rect(L.mute, e.x, e.y)) {
        if (toggle_mute) toggle_mute(idx);
        m_drag = D_CONSUME; app.request_redraw(); return;
    }
    if (in_rect(L.solo, e.x, e.y)) {
        if (toggle_solo) toggle_solo(idx);
        m_drag = D_CONSUME; app.request_redraw(); return;
    }
    // header / empty strip space: swallow the gesture
    m_drag = D_CONSUME;
}

void MasterMixerView::apply_send(App& app, const MouseEv& e) {
    if (m_drag_strip < 0 || m_drag_aux < 0) return;
    // relative drag: up = increase, over KNOB_DRAG_PX for the full 0..1 sweep
    float nv = m_drag_start + float(m_drag_y0 - e.y) / float(KNOB_DRAG_PX);
    nv = clamp01(nv);
    if (set_send) set_send(m_drag_strip, m_drag_aux, nv);
    app.request_redraw();
}

void MasterMixerView::apply_pan(App& app, const MouseEv& e) {
    if (m_drag_strip < 0) return;
    Sub L = layout(app, m_drag_strip, strip_area(m_drag_strip));
    const SDL_Rect& bar = L.pan_bar;
    int travel = bar.w - 6 > 0 ? bar.w - 6 : 1;
    float n = float(e.x - (bar.x + 2)) / float(travel);
    float v = clamp01(n) * 2.0f - 1.0f;             // 0..1 -> -1..1
    if (std::fabs(v) < 0.06f) v = 0.0f;             // snap to centre
    if (set_pan) set_pan(m_drag_strip, v);
    app.request_redraw();
}

bool MasterMixerView::on_wheel(App& app, int dx, int dy) {
    if (content_w() <= rect.w) return false;        // nothing to scroll
    int d = dx != 0 ? dx : dy;
    m_scroll_x -= d * WHEEL_STEP;
    clamp_scroll();
    app.request_redraw();
    return true;
}

bool MasterMixerView::on_key(App& app, SDL_Keycode k) {
    int before = m_scroll_x;
    switch (k) {
        case SDLK_LEFT:  m_scroll_x -= WHEEL_STEP; break;
        case SDLK_RIGHT: m_scroll_x += WHEEL_STEP; break;
        case SDLK_HOME:  m_scroll_x = 0; break;
        case SDLK_END:   m_scroll_x = content_w(); break;   // clamped below
        default: return false;
    }
    clamp_scroll();
    if (m_scroll_x != before) app.request_redraw();
    return true;
}

} // namespace mixerui
