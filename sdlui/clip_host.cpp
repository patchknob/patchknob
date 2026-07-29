//----------------------------------------------------------------------------
//  sdlui/clip_host.cpp
//----------------------------------------------------------------------------
#include "clip_host.h"
#include <algorithm>

namespace ui {

static std::string cliphost_fit(const Font& font, std::string text, int maxw) {
    if (maxw <= 0) return "";
    if (font.text_w(text) <= maxw) return text;
    const std::string ell = "...";
    if (font.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && font.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!text.empty() && font.text_w(text + ell) > maxw) text.pop_back();
    return text.empty() ? ell : text + ell;
}

SDL_Rect ClipHost::menu_rect(App& app, int nOpts) const {
    int rowh = app.font.ch() + 6;
    int h = nOpts * rowh + 4;
    if (h > rect.h - bar_h) h = rect.h - bar_h;
    int w = std::min(menu_w, std::max(40, rect.w - 12));
    return SDL_Rect{ rect.x + 6, rect.y + bar_h, w, std::max(0, h) };
}

void ClipHost::draw(App& app) {
    const Theme& t = theme();
    // instrument strip
    SDL_Rect strip{ rect.x, rect.y, rect.w, bar_h };
    fill_rect(app.ren, strip, t.panel);
    frame_rect(app.ren, strip, t.dim);
    std::string cur = current ? current() : std::string("-");
    app.font.draw(app.ren, rect.x + 6, rect.y + (bar_h - app.font.ch()) / 2,
                  cliphost_fit(app.font, "Instrument: " + cur, rect.w - 28), t.text);
    // a little "v" affordance
    app.font.draw(app.ren, rect.x + rect.w - 16, rect.y + (bar_h - app.font.ch()) / 2, "v", t.dim);

    // wrapped editor
    if (view) { view->rect = view_rect(); view->visible = true; view->draw(app); }

    // dropdown list
    if (m_menu && options) {
        std::vector<std::string> opts = options();
        SDL_Rect box = menu_rect(app, (int)opts.size());
        fill_rect(app.ren, box, t.panel);
        frame_rect(app.ren, box, t.dim);
        int rowh = app.font.ch() + 6;
        int y = box.y + 2;
        for (const auto& o : opts) {
            if (y + rowh > box.y + box.h) break;
            app.font.draw(app.ren, box.x + 8, y + 3,
                          cliphost_fit(app.font, o, box.w - 16), t.text);
            y += rowh;
        }
    }
}

bool ClipHost::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) return view && view->hit(e.x, e.y) ? view->on_mouse(app, e) : true;

    if (m_menu && options) {   // a list is open: pick or dismiss
        std::vector<std::string> opts = options();
        SDL_Rect box = menu_rect(app, (int)opts.size());
        if (e.x >= box.x && e.x < box.x + box.w && e.y >= box.y && e.y < box.y + box.h) {
            int rowh = app.font.ch() + 6;
            int idx = (e.y - (box.y + 2)) / rowh;
            if (idx >= 0 && idx < (int)opts.size() && on_select) on_select(idx);
            m_menu = false; app.request_redraw(); return true;
        }
        m_menu = false; app.request_redraw(); // click elsewhere closes; fall through
    }
    // click the strip -> toggle the instrument list
    if (e.y >= rect.y && e.y < rect.y + bar_h) { m_menu = !m_menu; app.request_redraw(); return true; }
    // otherwise the editor gets it
    if (view && view->hit(e.x, e.y)) return view->on_mouse(app, e);
    return true;
}

bool ClipHost::on_wheel(App& app, int dx, int dy) { return view ? view->on_wheel(app, dx, dy) : false; }
bool ClipHost::on_key  (App& app, SDL_Keycode k)  { return view ? view->on_key(app, k)     : false; }
bool ClipHost::on_key_up(App& app, SDL_Keycode k)  { return view ? view->on_key_up(app, k)  : false; }

} // namespace ui
