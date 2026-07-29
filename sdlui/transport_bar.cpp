//----------------------------------------------------------------------------
//  sdlui/transport_bar.cpp -- implementation of the Ardour-style transport bar.
//
//  Icons are drawn from filled SDL primitives (triangles via horizontal
//  scanlines, squares via fill_rect, discs via scanline circles) rather than
//  font glyphs, because the UI font is an ASCII-only monospace atlas that has
//  no play/stop/loop symbols.  Icon size scales with the button so both the
//  compact strip and the full panel read correctly.
//----------------------------------------------------------------------------
#include "gui.h"
#include "transport_bar.h"

namespace ui {

// ---- small local helpers ---------------------------------------------------
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static bool pt_in(const SDL_Rect& r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Integer square root (for scanline discs) -- avoids pulling in <cmath>.
static int isqrt(int v) {
    if (v <= 0) return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

// Zero-padded decimal, e.g. zpad(3,3) -> "003".
static std::string zpad(int v, int width) {
    if (v < 0) v = 0;
    std::string s = std::to_string(v);
    while ((int)s.size() < width) s.insert(s.begin(), '0');
    return s;
}

// One-decimal fixed format, e.g. 120.0 -> "120.0".
static std::string fmt1(double v) {
    if (v < 0) v = 0;
    long long whole = (long long)v;
    int frac = (int)((v - (double)whole) * 10.0 + 0.5);
    if (frac >= 10) { whole += 1; frac = 0; }
    return std::to_string(whole) + "." + std::to_string(frac);
}

// ---- filled vector primitives ----------------------------------------------
// Right-pointing triangle filling box `b` (vertical base on left, apex right).
static void tri_right(SDL_Renderer* r, SDL_Rect b, Color c) {
    if (b.w <= 0 || b.h <= 0) return;
    int cy = b.y + b.h / 2, den = b.h > 0 ? b.h : 1;
    for (int y = b.y; y <= b.y + b.h; ++y) {
        int d = y - cy; if (d < 0) d = -d;
        int num = b.h - 2 * d; if (num < 0) num = 0;
        int xr = b.x + (b.w * num) / den;
        hline(r, b.x, xr, y, c);
    }
}
// Left-pointing triangle filling box `b` (apex left, vertical base on right).
static void tri_left(SDL_Renderer* r, SDL_Rect b, Color c) {
    if (b.w <= 0 || b.h <= 0) return;
    int cy = b.y + b.h / 2, den = b.h > 0 ? b.h : 1;
    for (int y = b.y; y <= b.y + b.h; ++y) {
        int d = y - cy; if (d < 0) d = -d;
        int num = b.h - 2 * d; if (num < 0) num = 0;
        int w = (b.w * num) / den;
        hline(r, b.x + b.w - w, b.x + b.w, y, c);
    }
}
// Filled disc, center (cx,cy) radius rr, via horizontal scanlines.
static void disc(SDL_Renderer* r, int cx, int cy, int rr, Color c) {
    if (rr <= 0) { hline(r, cx, cx, cy, c); return; }
    for (int dy = -rr; dy <= rr; ++dy) {
        int dx = isqrt(rr * rr - dy * dy);
        hline(r, cx - dx, cx + dx, cy + dy, c);
    }
}

// ---- transport icons (drawn in color `c` inside icon box `b`) --------------
// which: 0 to-start, 1 rewind, 2 play, 3 stop, 5 ffwd, 6 to-end, 7 loop.
// (4 record is drawn directly in draw_button because its ring needs the fill.)
static void draw_icon(SDL_Renderer* r, int which, SDL_Rect b, Color c) {
    switch (which) {
    case 0: {                               // |<  to-start : bar + left triangle
        int bw = imax(2, b.w / 5);
        fill_rect(r, SDL_Rect{ b.x, b.y, bw, b.h }, c);
        tri_left(r, SDL_Rect{ b.x + bw + 1, b.y, b.w - bw - 1, b.h }, c);
    } break;
    case 1: {                               // << rewind : two left triangles
        int hw = (b.w - 2) / 2; if (hw < 2) hw = b.w / 2;
        tri_left(r, SDL_Rect{ b.x, b.y, hw, b.h }, c);
        tri_left(r, SDL_Rect{ b.x + hw + 2, b.y, hw, b.h }, c);
    } break;
    case 2:                                 // >  play : right triangle
        tri_right(r, b, c);
        break;
    case 3: {                               // []  stop : filled square
        int s = imin(b.w, b.h);
        fill_rect(r, SDL_Rect{ b.x + (b.w - s) / 2, b.y + (b.h - s) / 2, s, s }, c);
    } break;
    case 5: {                               // >> ffwd : two right triangles
        int hw = (b.w - 2) / 2; if (hw < 2) hw = b.w / 2;
        tri_right(r, SDL_Rect{ b.x, b.y, hw, b.h }, c);
        tri_right(r, SDL_Rect{ b.x + hw + 2, b.y, hw, b.h }, c);
    } break;
    case 6: {                               // >| to-end : right triangle + bar
        int bw = imax(2, b.w / 5);
        fill_rect(r, SDL_Rect{ b.x + b.w - bw, b.y, bw, b.h }, c);
        tri_right(r, SDL_Rect{ b.x, b.y, b.w - bw - 1, b.h }, c);
    } break;
    case 7: {                               // loop : two hooked arrows (repeat)
        int t  = imax(2, b.h / 6);
        int ah = imax(4, b.h / 3);
        int cy = b.y + b.h / 2;
        // top hook: left vertical + top bar + right-pointing arrowhead
        fill_rect(r, SDL_Rect{ b.x, b.y, t, cy - b.y }, c);
        fill_rect(r, SDL_Rect{ b.x, b.y, b.w - ah, t }, c);
        tri_right(r, SDL_Rect{ b.x + b.w - ah, b.y + t / 2 - ah / 2, ah, ah }, c);
        // bottom hook: right vertical + bottom bar + left-pointing arrowhead
        fill_rect(r, SDL_Rect{ b.x + b.w - t, cy, t, b.y + b.h - cy }, c);
        fill_rect(r, SDL_Rect{ b.x + ah, b.y + b.h - t, b.w - ah, t }, c);
        tri_left(r, SDL_Rect{ b.x, b.y + b.h - t / 2 - ah / 2, ah, ah }, c);
    } break;
    default: break;
    }
}

// ---- layout ----------------------------------------------------------------
TransportBar::Rects TransportBar::layout(App& app) const {
    Rects L;
    for (int i = 0; i < 8; ++i) L.btn[i] = SDL_Rect{ 0, 0, 0, 0 };
    L.bbt = L.tempo = L.meter = L.mode = SDL_Rect{ 0, 0, 0, 0 };
    L.nbtn = 0; L.has_meter = false;
    const int N = 8;

    if (compact) {
        const Font& mf = app.mono;         // small inline readout
        const int pad = 3, gap = 2;
        int bh = rect.h - 6; if (bh < 12) bh = 12;
        int bw = bh;                       // square icon buttons
        int th = mf.ch();

        int bbtw = mf.text_w("000|00|000");
        int tw   = mf.text_w("000.0");
        int mw   = mf.text_w("SONG") + 4;   // SONG/LIVE mode chip
        int readoutW = bbtw + gap + tw + gap + mw + gap;

        // How many buttons fit while leaving room for the readouts?  Buttons are
        // dropped from the tail (loop first, then to-end), matching the spec.
        int availBtn = rect.w - 2 * pad - readoutW;
        int n = (bw + gap > 0) ? (availBtn + gap) / (bw + gap) : 0;
        if (n > N) n = N; if (n < 0) n = 0;

        int by = rect.y + (rect.h - bh) / 2;
        int x  = rect.x + pad;
        for (int i = 0; i < n; ++i) { L.btn[i] = SDL_Rect{ x, by, bw, bh }; x += bw + gap; }
        L.nbtn = n;

        int ry = rect.y + (rect.h - th) / 2;
        L.bbt   = SDL_Rect{ x, ry, bbtw, th };  x += bbtw + gap;
        L.tempo = SDL_Rect{ x, ry, tw,   th };  x += tw + gap;
        L.mode  = SDL_Rect{ x, ry, mw,   th };
    } else {
        const Font& cf = app.font;         // large / prominent
        const int pad = 6, gap = 4;
        SDL_Rect in{ rect.x + pad, rect.y + pad, rect.w - 2 * pad, rect.h - 2 * pad };

        int bh = in.h * 42 / 100; if (bh < 24) bh = 24; if (bh > 46) bh = 46;
        int bw = bh;
        int totalW = N * bw + (N - 1) * gap;
        if (totalW > in.w) {               // shrink to fit width if needed
            bw = (in.w - (N - 1) * gap) / N; if (bw < 10) bw = 10;
            totalW = N * bw + (N - 1) * gap;
        }
        int bx = in.x + (in.w - totalW) / 2; if (bx < in.x) bx = in.x;
        int by = in.y;
        for (int i = 0; i < N; ++i) { L.btn[i] = SDL_Rect{ bx, by, bw, bh }; bx += bw + gap; }
        L.nbtn = N;

        // Clock row (below the button row) holds the big BBT clock (centered),
        // the meter on the left and the tempo + BPM label on the right.
        int cy0 = by + bh + gap;
        int crh = in.y + in.h - cy0;
        if (crh < cf.ch() + 6) crh = cf.ch() + 6;

        int cbw = cf.text_w("000|00|000") + 16;
        int cbh = cf.ch() + 8; if (cbh > crh) cbh = crh;
        int cbx = in.x + (in.w - cbw) / 2; if (cbx < in.x) cbx = in.x;
        int cby = cy0 + (crh - cbh) / 2;
        L.bbt = SDL_Rect{ cbx, cby, cbw, cbh };

        int tfw  = cf.text_w("000.0") + 8;
        int tfh  = cf.ch() + 6; if (tfh > crh) tfh = crh;
        int bpmw = cf.text_w("BPM");
        int tfx  = in.x + in.w - tfw - bpmw - 6; if (tfx < in.x) tfx = in.x;
        int tfy  = cy0 + (crh - tfh) / 2;
        L.tempo = SDL_Rect{ tfx, tfy, tfw, tfh };

        L.meter = SDL_Rect{ in.x, cby + (cbh - cf.ch()) / 2, cf.text_w("4/4"), cf.ch() };
        L.has_meter = true;

        // SONG/LIVE chip right after the meter on the clock row.
        L.mode = SDL_Rect{ L.meter.x + L.meter.w + 10, L.meter.y,
                           cf.text_w("SONG") + 6, cf.ch() };
    }
    return L;
}

// ---- drawing ---------------------------------------------------------------
void TransportBar::draw_button(App& app, int i, const SDL_Rect& b,
                               bool rolling, bool rec, bool loop) {
    if (b.w <= 0 || b.h <= 0) return;
    const Theme& t = theme();

    bool  lit  = false;
    Color fill = t.panel;
    Color icon = t.text;
    if      (i == 2 && rolling) { lit = true; fill = t.accent; icon = t.bg; }  // playing
    else if (i == 4 && rec)     { lit = true; fill = t.note;   icon = t.bg; }  // rec armed
    else if (i == 7 && loop)    { lit = true; fill = t.accent; icon = t.bg; }  // loop on
    if (!lit && m_press_btn == i) { fill = t.dim; icon = t.bg; }               // pressed

    fill_rect (app.ren, b, fill);
    frame_rect(app.ren, b, lit ? t.accent : t.dim);

    int mx = imax(2, b.w / 5), my = imax(2, b.h / 5);
    SDL_Rect ic{ b.x + mx, b.y + my, b.w - 2 * mx, b.h - 2 * my };
    if (ic.w < 3 || ic.h < 3) ic = SDL_Rect{ b.x + 2, b.y + 2, b.w - 4, b.h - 4 };

    if (i == 4) {                          // record : filled disc / dim ring
        int cx = ic.x + ic.w / 2, cy = ic.y + ic.h / 2;
        int rr = imin(ic.w, ic.h) / 2;
        if (rec) {
            disc(app.ren, cx, cy, rr, icon);
        } else {
            disc(app.ren, cx, cy, rr, t.dim);
            disc(app.ren, cx, cy, imax(1, rr - imax(2, rr / 3)), fill);
        }
        return;
    }
    draw_icon(app.ren, i, ic, icon);
}

void TransportBar::draw_bbt(App& app, const SDL_Rect& r) {
    if (r.w <= 0) return;
    const Theme& t = theme();
    int bar = 1, beat = 1, tick = 0;
    if (get_bbt) get_bbt(bar, beat, tick);
    std::string s = zpad(bar, 3) + "|" + zpad(beat, 2) + "|" + zpad(tick, 3);

    if (compact) {
        app.mono.draw(app.ren, r.x, r.y, s, t.text);
    } else {
        // Recessed, prominent clock display: keybg well + accent frame + digits.
        fill_rect (app.ren, r, t.keybg);
        frame_rect(app.ren, r, t.accent);
        app.font.draw_centered(app.ren, r, s, t.accent);
    }
}

void TransportBar::draw_tempo(App& app, const SDL_Rect& r) {
    if (r.w <= 0) return;
    const Theme& t = theme();
    bool editing = app.editing_text() && app.text_target == &m_tempo_edit;

    std::string s;
    if (editing) s = m_tempo_edit;
    else { double bpm = get_tempo ? get_tempo() : 0.0; s = fmt1(bpm); }

    if (compact) {
        if (editing) {
            fill_rect(app.ren, r, t.accent);
            app.mono.draw(app.ren, r.x, r.y, s, t.bg);
            int cx = r.x + app.mono.text_w(s);
            vline(app.ren, cx, r.y, r.y + app.mono.ch(), t.bg);   // caret
        } else {
            app.mono.draw(app.ren, r.x, r.y, s, t.text);
        }
    } else {
        if (editing) {
            fill_rect(app.ren, r, t.accent);
            SDL_Rect tx{ r.x + 3, r.y + 1, r.w - 6, r.h - 2 };
            app.font.draw_fitted(app.ren, tx, s, t.bg, false);
            int cx = r.x + 3 + std::min(app.font.text_w(s), r.w - 6);
            vline(app.ren, cx, r.y + 2, r.y + r.h - 2, t.bg);     // caret
        } else {
            frame_rect(app.ren, r, t.dim);
            app.font.draw_fitted(app.ren, SDL_Rect{ r.x + 3, r.y + 1, r.w - 6, r.h - 2 },
                                 s, t.text, false);
        }
        app.font.draw(app.ren, r.x + r.w + 4, r.y + (r.h - app.font.ch()) / 2, "BPM", t.dim);
    }
}

void TransportBar::draw(App& app) {
    if (!visible) return;
    const Theme& t = theme();

    if (compact) {
        fill_rect(app.ren, rect, t.panel);
    } else {
        fill_rect (app.ren, rect, t.panel);
        frame_rect(app.ren, rect, t.dim);
    }

    bool rolling = is_rolling && is_rolling();
    bool rec     = is_rec     && is_rec();
    bool loop    = is_loop    && is_loop();

    Rects L = layout(app);
    for (int i = 0; i < L.nbtn; ++i) draw_button(app, i, L.btn[i], rolling, rec, loop);
    draw_bbt  (app, L.bbt);
    draw_tempo(app, L.tempo);
    if (L.has_meter && L.meter.w > 0)
        app.font.draw(app.ren, L.meter.x, L.meter.y, "4/4", t.dim);

    // SONG/LIVE mode chip: SONG = timeline (trigger) gated playback; LIVE =
    // armed patterns loop freely.  Bright when SONG, dim when LIVE.
    if (L.mode.w > 0) {
        bool song = is_song && is_song();
        const Font& f = compact ? app.mono : app.font;
        f.draw(app.ren, L.mode.x + 2, L.mode.y, song ? "SONG" : "LIVE",
               song ? t.text : t.dim);
    }
}

// ---- input -----------------------------------------------------------------
void TransportBar::fire_button(int i) {
    switch (i) {
    case 0: if (on_to_start) on_to_start(); break;
    case 1: if (on_rewind)   on_rewind();   break;
    case 2: if (on_play)     on_play();     break;
    case 3: if (on_stop)     on_stop();     break;
    case 4: if (on_rec)      on_rec();      break;
    case 5: if (on_ffwd)     on_ffwd();     break;
    case 6: if (on_to_end)   on_to_end();   break;
    case 7: if (on_loop)     on_loop();     break;
    default: break;
    }
}

void TransportBar::apply_tempo() {
    if (m_tempo_edit.empty()) return;
    double v = 0.0; bool ok = true;
    try { v = std::stod(m_tempo_edit); } catch (...) { ok = false; }
    if (!ok) return;
    if (v < 20.0)  v = 20.0;
    if (v > 999.0) v = 999.0;
    if (on_tempo) on_tempo(v);
}

void TransportBar::begin_tempo_edit(App& app) {
    double cur = get_tempo ? get_tempo() : 0.0;
    m_tempo_edit = fmt1(cur);
    app.begin_text(&m_tempo_edit,
        [this] {                            // keep only digits and a single '.'
            std::string o; bool dot = false;
            for (char ch : m_tempo_edit) {
                if (ch >= '0' && ch <= '9') o += ch;
                else if (ch == '.' && !dot) { o += ch; dot = true; }
            }
            if (o.size() > 7) o.resize(7);
            m_tempo_edit = o;
        },
        [this](bool okk) { if (okk) apply_tempo(); });
}

bool TransportBar::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) {                       // release: end the press, clear feedback
        bool was = m_mouse_down;
        m_mouse_down = false; m_press_btn = -1;
        app.request_redraw();
        return was;
    }
    if (m_mouse_down) return true;          // ignore drag re-sends of the press
    m_mouse_down = true;
    if (e.button != SDL_BUTTON_LEFT) return true;

    Rects L = layout(app);

    // If we are editing the tempo, a click inside the field keeps editing; a
    // click anywhere else commits first, then falls through to handle the click.
    if (app.editing_text() && app.text_target == &m_tempo_edit) {
        if (pt_in(L.tempo, e.x, e.y)) return true;
        apply_tempo();
        app.end_text();
    }

    for (int i = 0; i < L.nbtn; ++i) {
        if (pt_in(L.btn[i], e.x, e.y)) {
            m_press_btn = i;
            fire_button(i);
            app.request_redraw();
            return true;
        }
    }
    if (pt_in(L.tempo, e.x, e.y)) {
        begin_tempo_edit(app);
        return true;
    }
    if (L.mode.w > 0 && pt_in(L.mode, e.x, e.y)) {
        if (on_mode) on_mode();
        app.request_redraw();
        return true;
    }
    return true;                            // consume clicks anywhere in our rect
}

bool TransportBar::on_key(App& app, SDL_Keycode k) {
    (void)app; (void)k;
    // begin_text() already routes typing / Enter / Esc while the tempo field is
    // active, so no extra key handling is needed here.
    return false;
}

} // namespace ui
