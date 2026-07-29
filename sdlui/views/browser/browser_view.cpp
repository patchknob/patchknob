//----------------------------------------------------------------------------
//  sdlui/views/browser/browser_view.cpp -- implementation of BrowserView.
//----------------------------------------------------------------------------
#include "browser_view.h"
#include <cctype>
#include <cstdio>
#include <algorithm>

namespace ui {

// ---- small helpers ---------------------------------------------------------

static std::string lower(const std::string& s) {
    std::string o(s);
    for (size_t i = 0; i < o.size(); ++i)
        o[i] = (char)std::tolower((unsigned char)o[i]);
    return o;
}

std::string BrowserView::fit(const std::string& s, int n) {
    if (n <= 0) return std::string();
    if ((int)s.size() <= n) return s;
    if (n == 1) return std::string("~");
    return s.substr(0, n - 1) + "~";
}

bool BrowserView::matches(const PluginDescriptor& d, const std::string& needle) {
    if (needle.empty()) return true;
    return lower(d.name).find(needle)   != std::string::npos
        || lower(d.vendor).find(needle) != std::string::npos;
}

static std::string format_str(const PatchKnob::engine::PluginDescriptor& d) {
    return d.format == PatchKnob::engine::PluginFormat::VST3 ? "VST3" : "VST2";
}

// ---- controller -> view ----------------------------------------------------

void BrowserView::populate(const std::vector<PluginDescriptor>& list) {
    m_all = list;
    m_scanning = false;
    rebuild_filtered();
}

void BrowserView::set_scanning(bool on) {
    m_scanning = on;
}

// ---- model -----------------------------------------------------------------

void BrowserView::rebuild_filtered() {
    // Remember which descriptor was selected so the highlight survives a filter.
    int prev_desc = (m_sel >= 0 && m_sel < (int)m_filtered.size())
                    ? m_filtered[m_sel] : -1;

    m_filtered.clear();
    for (size_t i = 0; i < m_all.size(); ++i)
        if (matches(m_all[i], m_filter))
            m_filtered.push_back((int)i);

    m_sel = -1;
    if (prev_desc >= 0) {
        for (size_t i = 0; i < m_filtered.size(); ++i)
            if (m_filtered[i] == prev_desc) { m_sel = (int)i; break; }
    }
    if (m_sel < 0 && !m_filtered.empty()) m_sel = 0;
    clamp_scroll();
}

void BrowserView::clamp_scroll() {
    int n = (int)m_filtered.size();
    // Never scroll past the point where the last row sits at the bottom of the
    // list -- otherwise the tail rows leave blank space and the scrollbar thumb
    // runs off the end of its track (thumb_y divides by n - visible).
    int maxscroll = m_visible > 0 ? n - m_visible : n - 1;
    if (maxscroll < 0) maxscroll = 0;
    if (m_scroll > maxscroll) m_scroll = maxscroll;
    if (m_scroll < 0) m_scroll = 0;
}

void BrowserView::ensure_visible(const Layout& L) {
    if (m_sel < 0) return;
    if (m_sel < m_scroll) m_scroll = m_sel;
    else if (L.visible > 0 && m_sel >= m_scroll + L.visible)
        m_scroll = m_sel - L.visible + 1;
    clamp_scroll();
}

void BrowserView::fire_instrument() {
    if (m_scanning) return;
    if (m_sel < 0 || m_sel >= (int)m_filtered.size()) return;
    const PluginDescriptor& d = m_all[m_filtered[m_sel]];
    if (on_load_instrument) on_load_instrument(d);
}

void BrowserView::fire_fx() {
    if (m_scanning) return;
    if (m_sel < 0 || m_sel >= (int)m_filtered.size()) return;
    const PluginDescriptor& d = m_all[m_filtered[m_sel]];
    if (on_add_fx) on_add_fx(d);
}

// ---- geometry --------------------------------------------------------------

BrowserView::Layout BrowserView::compute_layout(App& app) {
    Layout L;
    const int pad  = 6;
    const int fh   = app.font.ch() + 8;      // filter box / button height
    const int rh   = app.mono.ch() + 4;      // list row height
    L.row_h = rh;

    int x = rect.x + pad;
    int y = rect.y + pad;
    int w = rect.w - pad * 2;

    L.filter = { x, y, w, fh };
    y += fh + pad;

    L.header = { x, y, w, rh };
    y += rh;

    int status_h = fh + pad;
    int list_top = y;
    int list_bot = rect.y + rect.h - status_h - pad;
    if (list_bot < list_top) list_bot = list_top;
    L.list = { x, list_top, w, list_bot - list_top };
    L.visible = L.row_h > 0 ? L.list.h / L.row_h : 0;
    m_visible = L.visible;      // cache so clamp_scroll() can bound over-scroll

    // Bottom status bar + two action buttons on the right.
    int by = rect.y + rect.h - status_h;
    int bw_fx    = app.font.text_w("Add FX (F2)") + 16;
    int bw_instr = app.font.text_w("Load Instrument (Enter)") + 16;
    L.btn_fx    = { rect.x + rect.w - pad - bw_fx, by, bw_fx, fh };
    L.btn_instr = { L.btn_fx.x - pad - bw_instr,   by, bw_instr, fh };
    L.statusbar = { x, by, L.btn_instr.x - x - pad, fh };

    // Character-based columns (mono).  NAME flexes; the rest are fixed.
    int cw = app.mono.cw();
    int total_chars = cw > 0 ? (w - 6) / cw : 0;
    L.col_vendor = 20;
    L.col_format = 6;
    L.col_kind   = 7;
    L.col_io     = 7;
    L.col_name   = total_chars - (L.col_vendor + L.col_format + L.col_kind + L.col_io);
    if (L.col_name < 8) {
        // Very narrow: shrink vendor first, then drop to name-only.
        L.col_name = std::max(8, total_chars - (L.col_format + L.col_kind + L.col_io));
        L.col_vendor = std::max(0, total_chars - (L.col_name + L.col_format + L.col_kind + L.col_io));
    }
    return L;
}

// ---- draw ------------------------------------------------------------------

void BrowserView::draw(App& app) {
    if (!visible) return;
    const Theme& t = theme();
    Layout L = compute_layout(app);
    clamp_scroll();

    // background frame
    fill_rect(app.ren, rect, t.bg);
    frame_rect(app.ren, rect, t.dim);

    // ---- filter box --------------------------------------------------------
    fill_rect(app.ren, L.filter, t.panel);
    frame_rect(app.ren, L.filter, t.dim);
    {
        int ty = L.filter.y + (L.filter.h - app.font.ch()) / 2;
        int tx = L.filter.x + 6;
        app.font.draw(app.ren, tx, ty, "Filter: ", t.dim);
        tx += app.font.text_w("Filter: ");
        std::string shown = m_filter + "_";   // static caret
        app.font.draw(app.ren, tx, ty, shown, t.text);
    }

    // ---- column header -----------------------------------------------------
    fill_rect(app.ren, L.header, t.panel);
    {
        int cw = app.mono.cw();
        int hx = L.header.x + 3;
        int hy = L.header.y + (L.header.h - app.mono.ch()) / 2;
        app.mono.draw(app.ren, hx, hy, fit("NAME", L.col_name), t.hi);
        hx += L.col_name * cw;
        app.mono.draw(app.ren, hx, hy, fit("VENDOR", L.col_vendor), t.hi);
        hx += L.col_vendor * cw;
        app.mono.draw(app.ren, hx, hy, fit("FORMAT", L.col_format), t.hi);
        hx += L.col_format * cw;
        app.mono.draw(app.ren, hx, hy, fit("KIND", L.col_kind), t.hi);
        hx += L.col_kind * cw;
        app.mono.draw(app.ren, hx, hy, "I/O", t.hi);
    }
    hline(app.ren, L.header.x, L.header.x + L.header.w,
          L.header.y + L.header.h - 1, t.accent);

    // ---- list --------------------------------------------------------------
    // clip rows to the list rect
    SDL_Rect clip = L.list;
    SDL_RenderSetClipRect(app.ren, &clip);
    if (m_scanning) {
        int ty = L.list.y + (L.row_h - app.mono.ch()) / 2;
        app.mono.draw(app.ren, L.list.x + 4, ty,
                      "scanning plugins (out-of-process, may take minutes)...", t.accent);
    } else if (m_filtered.empty()) {
        int ty = L.list.y + (L.row_h - app.mono.ch()) / 2;
        app.mono.draw(app.ren, L.list.x + 4, ty,
                      m_all.empty() ? "(no plugins scanned)" : "(no matches)", t.dim);
    } else {
        int cw = app.mono.cw();
        for (int r = 0; r < L.visible; ++r) {
            int fi = m_scroll + r;
            if (fi < 0 || fi >= (int)m_filtered.size()) break;
            const PluginDescriptor& d = m_all[m_filtered[fi]];
            SDL_Rect rowq { L.list.x, L.list.y + r * L.row_h, L.list.w, L.row_h };
            bool selected = (fi == m_sel);

            if (selected)
                fill_rect(app.ren, rowq, t.accent);
            else if (fi & 1)
                fill_rect(app.ren, rowq, t.panel);

            Color fg  = selected ? t.bg : t.text;
            Color fgd = selected ? t.bg : t.dim;

            char io[24];
            std::snprintf(io, sizeof io, "%d/%d", d.numAudioIn, d.numAudioOut);

            int tx = rowq.x + 3;
            int ty = rowq.y + (L.row_h - app.mono.ch()) / 2;
            app.mono.draw(app.ren, tx, ty, fit(d.name, L.col_name), fg);
            tx += L.col_name * cw;
            app.mono.draw(app.ren, tx, ty, fit(d.vendor, L.col_vendor), fgd);
            tx += L.col_vendor * cw;
            app.mono.draw(app.ren, tx, ty, fit(format_str(d), L.col_format), fg);
            tx += L.col_format * cw;
            app.mono.draw(app.ren, tx, ty,
                          fit(d.isInstrument ? "INSTR" : "FX", L.col_kind), fg);
            tx += L.col_kind * cw;
            app.mono.draw(app.ren, tx, ty, io, fg);
        }
    }
    SDL_RenderSetClipRect(app.ren, nullptr);

    // scrollbar hint on the right edge of the list
    if (!m_filtered.empty() && (int)m_filtered.size() > L.visible && L.visible > 0) {
        int n = (int)m_filtered.size();
        int track_h = L.list.h;
        int thumb_h = std::max(12, track_h * L.visible / n);
        int thumb_y = L.list.y + (track_h - thumb_h) * m_scroll / std::max(1, n - L.visible);
        SDL_Rect sb { L.list.x + L.list.w - 4, thumb_y, 3, thumb_h };
        fill_rect(app.ren, sb, t.dim);
    }

    // ---- status bar + buttons ---------------------------------------------
    {
        char buf[128];
        if (m_scanning)
            std::snprintf(buf, sizeof buf, "scanning...");
        else if (m_track >= 0)
            std::snprintf(buf, sizeof buf, "track %d  -  %d shown / %d scanned",
                          m_track, (int)m_filtered.size(), (int)m_all.size());
        else
            std::snprintf(buf, sizeof buf, "%d shown / %d scanned",
                          (int)m_filtered.size(), (int)m_all.size());
        app.font.draw_fitted(app.ren, SDL_Rect{ L.statusbar.x, L.statusbar.y,
                                                L.statusbar.w, L.statusbar.h },
                             buf, t.dim, false);
    }
    // buttons (disabled while scanning)
    bool en = !m_scanning && m_sel >= 0;
    Color bfg = en ? t.text : t.dim;
    fill_rect(app.ren, L.btn_instr, t.panel);
    frame_rect(app.ren, L.btn_instr, t.dim);
    app.font.draw_centered(app.ren, L.btn_instr, "Load Instrument (Enter)", bfg);
    fill_rect(app.ren, L.btn_fx, t.panel);
    frame_rect(app.ren, L.btn_fx, t.dim);
    app.font.draw_centered(app.ren, L.btn_fx, "Add FX (F2)", bfg);
}

// ---- input -----------------------------------------------------------------

bool BrowserView::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) return false;
    Layout L = compute_layout(app);

    // buttons
    if (e.x >= L.btn_instr.x && e.x < L.btn_instr.x + L.btn_instr.w &&
        e.y >= L.btn_instr.y && e.y < L.btn_instr.y + L.btn_instr.h) {
        fire_instrument(); app.request_redraw(); return true;
    }
    if (e.x >= L.btn_fx.x && e.x < L.btn_fx.x + L.btn_fx.w &&
        e.y >= L.btn_fx.y && e.y < L.btn_fx.y + L.btn_fx.h) {
        fire_fx(); app.request_redraw(); return true;
    }

    // list rows -> select (double-click = load instrument)
    if (e.x >= L.list.x && e.x < L.list.x + L.list.w &&
        e.y >= L.list.y && e.y < L.list.y + L.list.h && L.row_h > 0) {
        int r  = (e.y - L.list.y) / L.row_h;
        int fi = m_scroll + r;
        if (fi >= 0 && fi < (int)m_filtered.size()) {
            Uint32 now = SDL_GetTicks();
            bool dbl = (fi == m_last_click_row) && (now - m_last_click_ms < 400);
            m_sel = fi;
            m_last_click_row = fi;
            m_last_click_ms = now;
            if (dbl) fire_instrument();
            app.request_redraw();
        }
        return true;
    }
    return true;   // swallow clicks inside the view
}

bool BrowserView::on_wheel(App& app, int, int dy) {
    m_scroll -= dy;          // wheel up (dy>0) scrolls toward the top
    clamp_scroll();
    app.request_redraw();
    return true;
}

bool BrowserView::on_key(App& app, SDL_Keycode k) {
    Layout L = compute_layout(app);
    switch (k) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        fire_instrument();
        app.request_redraw();
        return true;
    case SDLK_F2:
        fire_fx();
        app.request_redraw();
        return true;
    case SDLK_BACKSPACE:
        if (!m_filter.empty()) { m_filter.pop_back(); rebuild_filtered(); app.request_redraw(); }
        return true;
    case SDLK_UP:
        if (m_sel > 0) { --m_sel; ensure_visible(L); app.request_redraw(); }
        return true;
    case SDLK_DOWN:
        if (m_sel + 1 < (int)m_filtered.size()) { ++m_sel; ensure_visible(L); app.request_redraw(); }
        return true;
    case SDLK_PAGEUP:
        m_sel -= L.visible > 0 ? L.visible : 1;
        if (m_sel < 0) m_sel = 0;
        ensure_visible(L); app.request_redraw();
        return true;
    case SDLK_PAGEDOWN:
        m_sel += L.visible > 0 ? L.visible : 1;
        if (m_sel >= (int)m_filtered.size()) m_sel = (int)m_filtered.size() - 1;
        ensure_visible(L); app.request_redraw();
        return true;
    case SDLK_HOME:
        if (!m_filtered.empty()) { m_sel = 0; ensure_visible(L); app.request_redraw(); }
        return true;
    case SDLK_END:
        if (!m_filtered.empty()) { m_sel = (int)m_filtered.size() - 1; ensure_visible(L); app.request_redraw(); }
        return true;
    default:
        // Printable keycodes (ASCII 32..126) type into the filter.  SDLK letter
        // codes are lowercase ASCII already, which suits the case-insensitive
        // filter; digits / space / punctuation map 1:1.
        if (k >= 32 && k <= 126) {
            m_filter.push_back((char)k);
            rebuild_filtered();
            app.request_redraw();
            return true;
        }
        return false;
    }
}

} // namespace ui
