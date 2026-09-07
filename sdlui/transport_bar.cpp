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
#include <cstdio>
#include <cmath>          // std::sin -- SDL2's headers pulled this in transitively
#include "globals.h"

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

// ---- record identity -------------------------------------------------------
// The ONE deliberate exception to the strictly two-tone rule, and the reason is
// safety, not decoration: `t.note` (what REC used to light with) is 0x383838 in
// Light and 0x34E066 in Midnight -- indistinguishable from `t.accent` (0x404040
// / 0x3BFF74), which PLAY and LOOP already use.  A lit REC therefore looked
// exactly like a lit PLAY, and you could not tell at a glance whether the next
// take was going to be captured or thrown away.  Record red reads as RECORD in
// both themes and is the only hue in the app, so it can never be confused with
// anything else.
static const Color k_rec_red      { 0xE0, 0x2B, 0x2B, 0xFF };
static const Color k_rec_red_dim  { 0x70, 0x18, 0x18, 0xFF };

// 0..1 triangle-free sine pulse on a `periodMs` cycle.  One clock for every
// pulsing element on the bar, so the REC button and the screen-recorder chip
// breathe together instead of beating against each other.
static float pulse_phase(unsigned periodMs) {
    if (periodMs == 0) periodMs = 1;
    const float ph = float(SDL_GetTicks() % periodMs) / float(periodMs);
    return 0.55f + 0.45f * std::sin(ph * 6.2831853f);
}

// Soft glow of `rings` alpha-blended frames radiating out of `box`.
//
// SDL blending is enabled ONLY for the duration of this call and put back
// exactly as it was found: this codebase draws opaque by default, and a leaked
// SDL_BLENDMODE_BLEND silently corrupts every fill drawn after it (colours
// composite against whatever they land on instead of replacing it).  That is
// why the screen-recorder chip saved/restored it, and why this shared helper
// exists -- so the second pulsing element could not get it wrong.
static void pulse_halo(SDL_Renderer* r, const SDL_Rect& box, Color c,
                       float amount, int rings = 4, Uint8 peak = 150) {
    if (box.w <= 0 || box.h <= 0 || rings <= 0) return;
    if (amount <= 0.f) return;
    SDL_BlendMode prev = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(r, &prev);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = rings; i >= 1; --i) {
        SDL_Rect h{ box.x - i, box.y - i, box.w + 2 * i, box.h + 2 * i };
        const float fall = amount * (1.f - (float)(i - 1) / (float)rings);
        ui::frame_rect(r, h, Color{ c.r, c.g, c.b, (Uint8)((float)peak * fall) });
    }
    SDL_SetRenderDrawBlendMode(r, prev);
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
    case 8: {                               // mechanical metronome
        const int cx=b.x+b.w/2;
        for(int y=0;y<b.h;++y) {
            const int half=2+(y*(b.w/2-2))/imax(1,b.h);
            hline(r,cx-half,cx+half,b.y+y,c);
        }
        fill_rect(r,SDL_Rect{b.x,b.y+b.h-2,b.w,2},c);
        hline(r,cx, b.x+b.w-2, b.y+2, c);
        disc(r,b.x+b.w-2,b.y+2,1,c);
    } break;
    default: break;
    }
}

// ---- layout ----------------------------------------------------------------
TransportBar::Rects TransportBar::layout(App& app) const {
    Rects L;
    for (int i = 0; i < 9; ++i) L.btn[i] = SDL_Rect{ 0, 0, 0, 0 };
    L.bbt = L.tempo = L.meter = L.mode = L.recquant = L.qrange = L.qswing
          = L.screenrec = SDL_Rect{ 0, 0, 0, 0 };
    L.nbtn = 0; L.has_meter = false;
    const int N = 9;

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
        // The screen-recorder chip sits at the far right, after the transport
        // buttons and every readout.  Its width is reserved BEFORE the buttons
        // are counted, so a narrow strip drops a button rather than sliding the
        // chip off the end -- it used to be laid out first, at the left.
        const int recw = mf.text_w("REC 00:00") + 8;
        int availBtn = rect.w - 2 * pad - readoutW - (recw + gap);
        int n = (bw + gap > 0) ? (availBtn + gap) / (bw + gap) : 0;
        if (n > N) n = N; if (n < 0) n = 0;

        int by = rect.y + (rect.h - bh) / 2;
        int x  = rect.x + pad;
        for (int i = 0; i < n; ++i) { L.btn[i] = SDL_Rect{ x, by, bw, bh }; x += bw + gap; }
        L.nbtn = n;

        int ry = rect.y + (rect.h - th) / 2;
        L.bbt   = SDL_Rect{ x, ry, bbtw, th };  x += bbtw + gap;
        L.tempo = SDL_Rect{ x, ry, tw,   th };  x += tw + gap;
        L.mode  = SDL_Rect{ x, ry, mw,   th };  x += mw + gap;

        // Record-quantise chips on the compact strip too.  They used to exist
        // ONLY on the full bar, which lives in a window that is hidden by
        // default -- so the quantise settings were effectively unreachable from
        // the transport you actually look at.  Appended tail-first and only
        // while they fit, matching how this strip already drops buttons when
        // the window is narrow.
        const int rqw = mf.text_w("RQ 1/32") + 6;
        const int qrw = mf.text_w("QR100")   + 6;
        const int sww = mf.text_w("SW100")   + 6;
        L.screenrec = SDL_Rect{ rect.x + rect.w - pad - recw, by, recw, bh };
        const int right = L.screenrec.x - gap;
        if (x + rqw <= right) { L.recquant = SDL_Rect{ x, ry, rqw, th }; x += rqw + gap; }
        if (x + qrw <= right) { L.qrange   = SDL_Rect{ x, ry, qrw, th }; x += qrw + gap; }
        if (x + sww <= right) { L.qswing   = SDL_Rect{ x, ry, sww, th }; x += sww + gap; }
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
        L.recquant = SDL_Rect{ L.mode.x + L.mode.w + 8, L.mode.y,
                               cf.text_w("RQ 1/32") + 8, cf.ch() + 2 };
        L.qrange = SDL_Rect{ L.recquant.x + L.recquant.w + 4, L.recquant.y,
                             cf.text_w("QR100") + 8, L.recquant.h };
        L.qswing = SDL_Rect{ L.qrange.x + L.qrange.w + 4, L.recquant.y,
                             cf.text_w("SW100") + 8, L.recquant.h };

        // The chip existed ONLY on the compact strip, so whenever the large
        // transport panel was the one on screen the recorder had no control at
        // all.  Right-aligned on the button row; if the buttons fill that row it
        // falls back to the clock row, clamped clear of the tempo field.
        {
            const int recw = app.mono.text_w("REC 00:00") + 10;
            int rh = app.mono.ch() + 8; if (rh > bh) rh = bh;
            int rx = in.x + in.w - recw;
            int ry = by + (bh - rh) / 2;
            if (rx < bx + gap) {
                rx = L.qswing.x + L.qswing.w + 8;
                ry = L.qswing.y;
                rh = L.qswing.h;
                if (rx + recw > L.tempo.x - 6)
                    rx = std::max(in.x, L.tempo.x - 6 - recw);
            }
            L.screenrec = SDL_Rect{ rx, ry, recw, rh };
        }
    }
    return L;
}

// ---- drawing ---------------------------------------------------------------
void TransportBar::draw_button(App& app, int i, const SDL_Rect& b,
                               const BtnState& st) {
    if (b.w <= 0 || b.h <= 0) return;
    const Theme& t = theme();

    bool  lit  = false;
    Color fill = t.panel;
    Color icon = t.text;
    Color edge = t.dim;
    // PLAY lights while ROLLING and also while a count-in pre-roll is counting
    // us in.  The pre-roll is transport time you asked for, so the button that
    // asked for it has to look engaged for the whole of it.
    if      (i == 2 && (st.rolling || st.play_pending))
                                { lit = true; fill = t.accent; icon = t.bg; edge = t.accent; }
    else if (i == 4 && st.rec)  { lit = true; fill = k_rec_red; icon = t.bg; edge = k_rec_red; }
    else if (i == 4 && st.rec_pending) { edge = k_rec_red; }   // armed-in-waiting
    // Punch modes, Record Ready: the button FLASHES (drawn at the disc below);
    // the frame goes record-red so the armed state reads even between blinks.
    else if (i == 4 && st.rec_ready)   { edge = k_rec_red; }
    // Punch mode enabled + at least one punch-enabled track, transport not
    // armed: PT lights the Record button solid blue.  Here that maps to the
    // engaged/secondary role (accent fill), same as a lit PLAY/LOOP -- the
    // disc + mode badge keep it unmistakably the record button.
    else if (i == 4 && st.rec_mode != 0 && st.punch_n > 0)
                                { lit = true; fill = t.accent; icon = t.bg; edge = t.accent; }
    else if (i == 7 && st.loop) { lit = true; fill = t.accent; icon = t.bg; edge = t.accent; }
    else if (i == 8 && is_metronome && is_metronome())
                                { lit = true; fill = t.accent; icon = t.bg; edge = t.accent; }
    if (!lit && m_press_btn == i) { fill = t.dim; icon = t.bg; }               // pressed

    fill_rect (app.ren, b, fill);
    frame_rect(app.ren, b, edge);

    int mx = imax(2, b.w / 5), my = imax(2, b.h / 5);
    SDL_Rect ic{ b.x + mx, b.y + my, b.w - 2 * mx, b.h - 2 * my };
    if (ic.w < 3 || ic.h < 3) ic = SDL_Rect{ b.x + 2, b.y + 2, b.w - 4, b.h - 4 };

    if (i == 4) {                          // record : filled disc / dim ring
        int cx = ic.x + ic.w / 2, cy = ic.y + ic.h / 2;
        int rr = imin(ic.w, ic.h) / 2;
        // Whether a SOLID disc was painted, remembered so the punch-mode badge
        // below can pick a contrasting ink.
        bool discSolid = false;
        if (st.rec) {
            // RECORDING: solid red field, disc punched out of it.  Steady shape
            // -- "this is on" -- as distinct from the flashing states below.
            disc(app.ren, cx, cy, rr, icon);
            discSolid = true;
        } else if (st.rec_ready) {
            // PUNCH MODES, RECORD READY (PT p639/643): the button flashes.  With
            // punch-enabled tracks it alternates secondary/record ("blue and
            // red"); with none it alternates dim/record ("gray and red").  A
            // hard square wave, not the count-in's sine -- these are different
            // states and must read differently.
            const bool ph = (SDL_GetTicks() / 350u) & 1u;
            disc(app.ren, cx, cy, rr, ph ? k_rec_red : (st.punch_n > 0 ? t.accent : t.dim));
            discSolid = true;
        } else if (st.rec_pending) {
            // COUNT-IN RUNNING: the press has been accepted but the engine is
            // not capturing yet.  Blink the disc between full and dark red so
            // the state reads as "about to record", never as "nothing happened"
            // -- the old button sat completely unchanged for the whole pre-roll
            // bar, which is exactly why it felt like a dead momentary key.
            const float p = pulse_phase(600);
            const Color d{ (Uint8)(k_rec_red_dim.r + (k_rec_red.r - k_rec_red_dim.r) * p),
                           (Uint8)(k_rec_red_dim.g + (k_rec_red.g - k_rec_red_dim.g) * p),
                           (Uint8)(k_rec_red_dim.b + (k_rec_red.b - k_rec_red_dim.b) * p),
                           255 };
            disc(app.ren, cx, cy, rr, d);
            discSolid = true;
        } else if (st.rec_mode != 0 && st.punch_n > 0) {
            // Punch-enabled, not armed: solid disc on the accent field ("solid
            // blue" in PT).  The steady fill + badge is what separates it from
            // every flashing state.
            disc(app.ren, cx, cy, rr, icon);
            discSolid = true;
        } else {
            disc(app.ren, cx, cy, rr, t.dim);
            disc(app.ren, cx, cy, imax(1, rr - imax(2, rr / 3)), fill);
        }
        // Punch-mode badge (PT p639/643): "P" / "T" / "DP" drawn IN the Record
        // button whenever the mode is on, over every state above.  Ink is the
        // button fill when a solid disc was painted (punch-through), otherwise
        // plain text on the recessed centre.
        if (st.rec_mode != 0) {
            static const char* kBadge[4] = { "", "P", "T", "DP" };
            const char* badge = kBadge[st.rec_mode & 3];
            int bw2 = app.mono.text_w(badge);
            if (bw2 > b.w - 2 && badge[1]) { badge = "D"; bw2 = app.mono.text_w(badge); }
            if (bw2 <= b.w - 2) {
                const Color ink = discSolid ? fill : t.text;
                app.mono.draw(app.ren, b.x + (b.w - bw2) / 2,
                              b.y + (b.h - app.mono.ch()) / 2, badge, ink);
            }
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

    // Poll every live-state reader ONCE per frame so no two buttons can be
    // painted from different snapshots of the engine.
    BtnState st;
    st.rolling      = is_rolling      && is_rolling();
    st.rec          = is_rec          && is_rec();
    st.loop         = is_loop         && is_loop();
    st.rec_pending  = is_rec_pending  && is_rec_pending();
    st.play_pending = is_play_pending && is_play_pending();
    st.rec_ready    = is_rec_ready    && is_rec_ready();
    st.rec_mode     = get_record_mode ? get_record_mode() : 0;
    st.punch_n      = punch_track_count ? punch_track_count() : 0;
    // Once the engine really is armed the pre-roll is over; never show both,
    // and actual recording outranks every flashing armed state.
    if (st.rec) { st.rec_pending = false; st.rec_ready = false; }

    Rects L = layout(app);
    for (int i = 0; i < L.nbtn; ++i) draw_button(app, i, L.btn[i], st);
    // The record glow is painted AFTER the whole button row: a halo bleeds into
    // its neighbours' cells, and drawing it inside the loop meant the next
    // button's opaque fill immediately erased the half that fell on it.
    if (L.nbtn > 4 && L.btn[4].w > 0 && (st.rec || st.rec_pending || st.rec_ready)) {
        // Recording pulses gently (it is a steady state you must not miss);
        // pending/armed pulses hard and fast (it is a countdown / hot trigger).
        pulse_halo(app.ren, L.btn[4], k_rec_red,
                   st.rec ? 0.35f + 0.35f * pulse_phase(1400) : pulse_phase(600),
                   3, 190);
    }
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
    if (L.screenrec.w > 0) {
        const bool avail = !screen_record_available || screen_record_available();
        const bool on    = is_screen_recording && is_screen_recording();
        char lab[24];
        if (on) {
            const double el = screen_record_elapsed ? screen_record_elapsed() : 0.0;
            std::snprintf(lab, sizeof lab, "REC %d:%02d",
                          (int)(el / 60.0), (int)el % 60);
        } else {
            std::snprintf(lab, sizeof lab, "REC");
        }
        // Recording reads as a pulsing halo around a lit chip.  Drawn with real
        // alpha rather than colours mixed against t.bg, so it composites over
        // whatever is actually behind the chip.  Now shares pulse_halo() with
        // the REC button, which is what guarantees the blend mode is restored.
        if (on) pulse_halo(app.ren, L.screenrec, t.accent, pulse_phase(1100));
        ui::fill_rect(app.ren, L.screenrec, on ? t.accent : t.panel);
        ui::frame_rect(app.ren, L.screenrec, on ? t.accent : (avail ? t.dim : t.panel));
        app.mono.draw(app.ren, L.screenrec.x + 4,
                      L.screenrec.y + (L.screenrec.h - app.mono.ch()) / 2, lab,
                      on ? t.bg : (avail ? t.text : t.dim));
    }
    if (L.recquant.w > 0) {
        // Compact strip uses the small mono readout font; the full bar the large
        // one.  Layout above measured with the same font, so these agree.
        const Font& qf = compact ? app.mono : app.font;
        const int ticks = get_record_quantize ? get_record_quantize() : 0;
        std::string label = "RQ OFF";
        if (ticks > 0) label = "RQ 1/" + std::to_string((4 * c_ppqn) / ticks);
        frame_rect(app.ren, L.recquant, t.dim);
        qf.draw_centered(app.ren, L.recquant, label, ticks ? t.accent : t.dim);

        // Q-Range: how much of a note's deviation is treated as FEEL and left
        // alone.  0 nails every note to the grid; higher values preserve more
        // of the performance.  Greyed out when record quantise is off.
        if (L.qrange.w > 0) {
            const int qr = get_record_qrange ? get_record_qrange() : 0;
            frame_rect(app.ren, L.qrange, t.dim);
            qf.draw_centered(app.ren, L.qrange, "QR" + std::to_string(qr),
                                   (ticks && qr) ? t.accent : t.dim);
        }
        // Swing: 50 == straight; higher delays every other grid line.
        if (L.qswing.w > 0) {
            const int sw = get_record_swing ? get_record_swing() : 50;
            frame_rect(app.ren, L.qswing, t.dim);
            qf.draw_centered(app.ren, L.qswing, "SW" + std::to_string(sw),
                                   (ticks && sw != 50) ? t.accent : t.dim);
        }
    }
    if(m_click_menu) {
        fill_rect(app.ren,m_click_menu_rect,t.panel);
        frame_rect(app.ren,m_click_menu_rect,t.accent);
        std::string labels[8]; SDL_Rect segs[8];
        const int n=click_menu_segments(app,labels,segs,8);
        const int mode=get_record_mode?get_record_mode():0;
        for(int i=0;i<n;++i) {
            // The active record mode reads accent so the checked row also pops
            // at a glance; the count-in row keeps plain text.
            const bool active=m_click_menu_kind==1&&i<4&&i==mode;
            app.mono.draw(app.ren,segs[i].x,
                          segs[i].y+(segs[i].h-app.mono.ch())/2,
                          labels[i],active?t.accent:t.text);
        }
    }
}

// One horizontal row of "[x] Label" segments inside m_click_menu_rect, shared
// by draw() and on_mouse() so clicks always land on what is drawn.  Segments
// that do not fit the rect are dropped from the tail (the strip is only as
// wide as the window).
int TransportBar::click_menu_segments(App& app, std::string* labels,
                                      SDL_Rect* segs, int cap) const {
    const bool cnt = is_count_in && is_count_in();
    std::string want[8]; int n = 0;
    if (m_click_menu_kind == 0) {
        want[n++] = cnt ? "[x] Count in (4 beats)" : "[ ] Count in (4 beats)";
    } else {
        // Record modes, PT ch.27.  The checked row is the active mode.
        static const char* kMode[4] = { "Normal", "QuickPunch", "TrackPunch",
                                        "DestructivePunch" };
        const int mode = get_record_mode ? get_record_mode() : 0;
        for (int i = 0; i < 4; ++i)
            want[n++] = std::string(i == mode ? "[x] " : "[ ] ") + kMode[i];
        want[n++] = cnt ? "[x] Count-in" : "[ ] Count-in";
    }
    if (n > cap) n = cap;
    int x = m_click_menu_rect.x + 6;
    const int gap = app.mono.text_w("  ");
    int out = 0;
    for (int i = 0; i < n; ++i) {
        const int w = app.mono.text_w(want[i]);
        if (x + w > m_click_menu_rect.x + m_click_menu_rect.w - 4) break;
        labels[out] = want[i];
        segs[out] = SDL_Rect{ x, m_click_menu_rect.y, w, m_click_menu_rect.h };
        x += w + gap;
        ++out;
    }
    return out;
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
    case 8: if (on_metronome) on_metronome(); break;
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
    Rects L = layout(app);
    if(m_click_menu && e.button==SDL_BUTTON_LEFT) {
        std::string labels[8]; SDL_Rect segs[8];
        const int n=click_menu_segments(app,labels,segs,8);
        for(int i=0;i<n;++i) {
            if(!pt_in(segs[i],e.x,e.y)) continue;
            if(m_click_menu_kind==1&&i<4) {
                if(on_record_mode) on_record_mode(i);      // pick a record mode
            } else if(on_count_in) {
                on_count_in(!(is_count_in&&is_count_in()));
            }
            break;
        }
        m_click_menu=false; app.request_redraw(); return true;
    }
    if(e.button==SDL_BUTTON_RIGHT) {
        // Count-in hangs off the METRONOME button, but the compact strip drops
        // buttons from the tail when the window is narrow -- and the metronome
        // is the LAST one, so on any small window the count-in toggle became
        // completely unreachable.  The REC button opens the RECORD-MODE picker
        // instead (PT p638/643: right-click the Record button to choose
        // Normal / QuickPunch / TrackPunch / DestructivePunch), which also
        // carries a count-in segment so the old gesture keeps working.
        const int anchor = (L.nbtn>8 && pt_in(L.btn[8],e.x,e.y)) ? 8
                         : (L.nbtn>4 && pt_in(L.btn[4],e.x,e.y)) ? 4 : -1;
        if(anchor>=0) {
            m_click_menu=true;
            m_click_menu_kind = (anchor==4) ? 1 : 0;
            // Keep the popup in the transport's own top-level rectangle.  The
            // compact bar is below later roots, so an outside popup would be
            // painted over and would also fail root hit-testing.  That is also
            // why the record-mode picker is one horizontal ROW of segments
            // rather than a stacked menu: the strip has no vertical room.
            //
            // Width from the FONT, not a hard-coded 156: at any fractional UI
            // scale the atlas cell grows and the label ran out of its own box.
            const int mw=std::min(
                (m_click_menu_kind==1
                     ? app.mono.text_w("[x] Normal  [ ] QuickPunch  [ ] TrackPunch"
                                       "  [ ] DestructivePunch  [ ] Count-in")
                     : app.mono.text_w("[x] Count in (4 beats)"))+14,
                                  std::max(40,rect.w-8));
            int mx=L.btn[anchor].x+L.btn[anchor].w+3;
            if(mx+mw>rect.x+rect.w) mx=L.btn[anchor].x-mw-3;
            // The flipped-left position had no lower clamp: on a narrow bar it
            // went past the left edge and the popup was drawn (and hit-tested)
            // outside the widget.
            if(mx<rect.x+4) mx=rect.x+4;
            if(mx+mw>rect.x+rect.w) mx=rect.x+rect.w-mw;
            m_click_menu_rect={mx,rect.y+2,mw,rect.h-4};
            app.request_redraw();
        } else {
            // A right-click anywhere ELSE used to leave the popup up forever --
            // only a left-click dismissed it.
            if(m_click_menu) { m_click_menu=false; app.request_redraw(); }
        }
        return true;
    }
    if (e.button != SDL_BUTTON_LEFT) return true;

    // If we are editing the tempo, a click inside the field keeps editing; a
    // click anywhere else commits first, then falls through to handle the click.
    if (app.editing_text() && app.text_target == &m_tempo_edit) {
        if (pt_in(L.tempo, e.x, e.y)) return true;
        apply_tempo();
        app.end_text();
    }

    // The chip was drawn but never hit-tested, so clicking it did nothing at
    // all -- the recorder could not be started from the transport.
    if (L.screenrec.w > 0 && pt_in(L.screenrec, e.x, e.y)) {
        if (on_screen_record) on_screen_record();
        app.request_redraw();
        return true;
    }
    // Ctrl+click the Record button cycles through the record modes (PT p638,
    // p643: "Control-click (Mac) or Start-click (Windows) the Record button to
    // cycle through available Record modes").  Checked BEFORE the plain button
    // dispatch so a modified click can never fire a stray record toggle.
    if (L.nbtn > 4 && pt_in(L.btn[4], e.x, e.y) &&
        (SDL_GetModState() & KMOD_CTRL)) {
        if (on_record_mode)
            on_record_mode(((get_record_mode ? get_record_mode() : 0) + 1) & 3);
        app.request_redraw();
        return true;
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
    if (pt_in(L.recquant, e.x, e.y)) {
        static const int q[] = {
            0, 4*c_ppqn, 2*c_ppqn, c_ppqn, c_ppqn/2, c_ppqn/4, c_ppqn/8
        };
        const int cur = get_record_quantize ? get_record_quantize() : 0;
        int next = q[0];
        for (int i = 0; i < (int)(sizeof q / sizeof q[0]); ++i)
            if (q[i] == cur) { next = q[(i + 1) % (int)(sizeof q / sizeof q[0])]; break; }
        if (on_record_quantize) on_record_quantize(next);
        app.request_redraw();
        return true;
    }
    // Q-Range / swing cycle forward on left click, backward on right click, so
    // a value overshot by one step does not need a full trip round the list.
    if (pt_in(L.qrange, e.x, e.y)) {
        static const int qr[] = { 0, 25, 50, 75 };
        const int n = (int)(sizeof qr / sizeof qr[0]);
        const int cur = get_record_qrange ? get_record_qrange() : 0;
        int idx = 0;
        for (int i = 0; i < n; ++i) if (qr[i] == cur) { idx = i; break; }
        idx = (e.button == SDL_BUTTON_RIGHT) ? (idx + n - 1) % n : (idx + 1) % n;
        if (on_record_qrange) on_record_qrange(qr[idx]);
        app.request_redraw();
        return true;
    }
    if (pt_in(L.qswing, e.x, e.y)) {
        static const int sw[] = { 50, 54, 58, 62, 66, 70 };
        const int n = (int)(sizeof sw / sizeof sw[0]);
        const int cur = get_record_swing ? get_record_swing() : 50;
        int idx = 0;
        for (int i = 0; i < n; ++i) if (sw[i] == cur) { idx = i; break; }
        idx = (e.button == SDL_BUTTON_RIGHT) ? (idx + n - 1) % n : (idx + 1) % n;
        if (on_record_swing) on_record_swing(sw[idx]);
        app.request_redraw();
        return true;
    }
    return true;                            // consume clicks anywhere in our rect
}

void TransportBar::cancel_interaction(App& app) {
    // The shell calls this when focus is taken away mid-gesture.  Everything
    // here is PURELY visual/interaction state: a button left in m_press_btn
    // stayed drawn in its pressed shade with no pointer anywhere near it, and
    // the count-in popup stayed open (it can only be dismissed by a click, and
    // clicks were going to another window by then).
    if (m_press_btn >= 0 || m_mouse_down || m_click_menu) app.request_redraw();
    m_press_btn  = -1;
    m_mouse_down = false;
    m_click_menu = false;
    // ...and the tempo field, which is NOT purely visual: begin_tempo_edit()
    // points app.text_target at m_tempo_edit, and the only things that cleared
    // it were Enter/Esc or a click back inside the transport bar.  Click the BPM
    // then click into the arrange view and every keystroke in the whole app
    // still went into the tempo field -- Space stopped playing, typing did
    // nothing visible, the app looked frozen.  end_text_if() is a no-op unless
    // OUR field is the one live, so this can never cancel another view's edit.
    // The value is committed rather than dropped: the user typed it, and losing
    // focus is not "cancel".
    if (app.text_target == &m_tempo_edit) {
        apply_tempo();
        app.end_text_if(&m_tempo_edit);
        app.request_redraw();
    }
}

bool TransportBar::on_key(App& app, SDL_Keycode k) {
    (void)app; (void)k;
    // begin_text() already routes typing / Enter / Esc while the tempo field is
    // active, so no extra key handling is needed here.
    return false;
}

} // namespace ui
