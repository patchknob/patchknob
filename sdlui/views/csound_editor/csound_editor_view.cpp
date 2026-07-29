//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/csound_editor_view.cpp
//----------------------------------------------------------------------------
#include "csound_editor_view.h"

#include <algorithm>

namespace ui {

void CsoundEditorView::setText(const std::string& s) {
    lines_.clear();
    std::string cur;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') { lines_.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    lines_.push_back(cur);
    if (lines_.empty()) lines_.push_back(std::string());
    cr_ = std::min(cr_, (int)lines_.size() - 1);
    cc_ = std::min(cc_, (int)lines_[cr_].size());
    top_ = 0;
}

std::string CsoundEditorView::text() const {
    std::string s;
    for (size_t i = 0; i < lines_.size(); ++i) { if (i) s.push_back('\n'); s += lines_[i]; }
    return s;
}

void CsoundEditorView::insert(const char* utf8) {
    if (!utf8 || !*utf8) return;
    std::string& ln = lines_[cr_];
    std::string add(utf8);
    if (cc_ < 0) cc_ = 0;
    if (cc_ > (int)ln.size()) cc_ = (int)ln.size();
    ln.insert((size_t)cc_, add);
    cc_ += (int)add.size();
}

void CsoundEditorView::ensureVisible(int rows) {
    if (rows < 1) rows = 1;
    if (cr_ < top_) top_ = cr_;
    else if (cr_ >= top_ + rows) top_ = cr_ - rows + 1;
    if (top_ < 0) top_ = 0;
}

bool CsoundEditorView::on_key(App& app, SDL_Keycode k) {
    (void)app;
    const bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    std::string& ln = lines_[cr_];
    if (ctrl && (k == SDLK_e)) { if (on_recompile) on_recompile(); return true; }
    switch (k) {
        case SDLK_RETURN: case SDLK_KP_ENTER: {
            std::string tail = ln.substr((size_t)std::min(cc_, (int)ln.size()));
            ln.erase((size_t)std::min(cc_, (int)ln.size()));
            lines_.insert(lines_.begin() + cr_ + 1, tail);
            cr_++; cc_ = 0; return true;
        }
        case SDLK_BACKSPACE:
            if (cc_ > 0) { ln.erase((size_t)(cc_ - 1), 1); cc_--; }
            else if (cr_ > 0) { cc_ = (int)lines_[cr_ - 1].size();
                                lines_[cr_ - 1] += ln; lines_.erase(lines_.begin() + cr_); cr_--; }
            return true;
        case SDLK_DELETE:
            if (cc_ < (int)ln.size()) ln.erase((size_t)cc_, 1);
            else if (cr_ + 1 < (int)lines_.size()) { ln += lines_[cr_ + 1]; lines_.erase(lines_.begin() + cr_ + 1); }
            return true;
        case SDLK_TAB: ln.insert((size_t)std::min(cc_, (int)ln.size()), "  "); cc_ += 2; return true;
        case SDLK_LEFT:
            if (cc_ > 0) cc_--; else if (cr_ > 0) { cr_--; cc_ = (int)lines_[cr_].size(); } return true;
        case SDLK_RIGHT:
            if (cc_ < (int)ln.size()) cc_++; else if (cr_ + 1 < (int)lines_.size()) { cr_++; cc_ = 0; } return true;
        case SDLK_UP:    if (cr_ > 0) { cr_--; cc_ = std::min(cc_, (int)lines_[cr_].size()); } return true;
        case SDLK_DOWN:  if (cr_ + 1 < (int)lines_.size()) { cr_++; cc_ = std::min(cc_, (int)lines_[cr_].size()); } return true;
        case SDLK_HOME:  cc_ = 0; return true;
        case SDLK_END:   cc_ = (int)ln.size(); return true;
        case SDLK_PAGEUP:   cr_ = std::max(0, cr_ - 20); cc_ = std::min(cc_, (int)lines_[cr_].size()); return true;
        case SDLK_PAGEDOWN: cr_ = std::min((int)lines_.size() - 1, cr_ + 20); cc_ = std::min(cc_, (int)lines_[cr_].size()); return true;
        default: break;
    }
    // swallow plain character keys so they don't trigger app shortcuts (the actual
    // glyphs arrive via the text-input sink); let unhandled combos pass through.
    if (!ctrl && k >= 0x20 && k < 0x7f) return true;
    return false;
}

bool CsoundEditorView::on_wheel(App& app, int, int dy) {
    (void)app; top_ = std::max(0, top_ - dy * 3); return true;
}

bool CsoundEditorView::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed || !hit(e.x, e.y)) return false;
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
    const int rowH = app.mono.ch() + 2;
    const int gutterW = 5 * cw;
    int row = top_ + (e.y - (rect.y + 2)) / std::max(1, rowH);
    row = std::max(0, std::min((int)lines_.size() - 1, row));
    int col = (e.x - (rect.x + gutterW)) / std::max(1, cw);
    cr_ = row;
    cc_ = std::max(0, std::min((int)lines_[cr_].size(), col));
    return true;
}

void CsoundEditorView::draw(App& app) {
    SDL_Renderer* r = app.ren;
    const Theme& t = theme();
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
    const int ch = app.mono.ch();
    const int rowH = ch + 2;
    SDL_Rect area = rect;
    fill_rect(r, area, t.bg);

    const int statusH = ch + 8;
    const int gutterW = 5 * cw;
    const int textTop = area.y + 3;
    const int textAreaH = area.h - statusH - 6;
    const int visRows = std::max(1, textAreaH / rowH);
    ensureVisible(visRows);

    // gutter
    SDL_Rect gutter{ area.x, area.y, gutterW - 2, area.h - statusH };
    fill_rect(r, gutter, t.panel);

    for (int i = 0; i < visRows; ++i) {
        const int ln = top_ + i;
        if (ln >= (int)lines_.size()) break;
        const int y = textTop + i * rowH;
        char num[8]; std::snprintf(num, sizeof(num), "%4d", ln + 1);
        app.mono.draw(r, area.x + 2, y, num, t.dim);
        app.mono.draw(r, area.x + gutterW, y, lines_[(size_t)ln], t.text);
    }

    // caret (simple blink)
    blink_ = (blink_ + 1) & 63;
    if (cr_ >= top_ && cr_ < top_ + visRows && blink_ < 40) {
        const int cx = area.x + gutterW + cc_ * cw;
        const int cy = textTop + (cr_ - top_) * rowH;
        vline(r, cx, cy, cy + ch, t.accent);
    }
    app.request_redraw();   // keep the caret blinking

    // status bar
    SDL_Rect status{ area.x, area.y + area.h - statusH, area.w, statusH };
    fill_rect(r, status, t.panel);
    frame_rect(r, status, t.dim);
    const std::string msg = status_.empty() ? std::string("Ctrl+E to compile  |  ins/outs update from nchnls / nchnls_i")
                                             : status_;
    app.mono.draw(r, status.x + 6, status.y + 4, msg, statusError_ ? t.hi : t.dim);
}

} // namespace ui
