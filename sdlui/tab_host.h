//----------------------------------------------------------------------------
//  sdlui/tab_host.h -- a tiny tabbed container widget.
//
//  Holds N (label, view) tabs, draws a tab strip along the top, and delegates
//  drawing + input to the ACTIVE tab's view (laid out in the area beneath the
//  strip).  Two-tone, matching the rest of the SDL toolkit.  Used to dock the
//  modular patchbay and the sample editor into one bottom window.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_TAB_HOST_H
#define PATCHKNOB_SDLUI_TAB_HOST_H

#include "gui.h"
#include <string>
#include <vector>

namespace ui {

class TabHost : public Widget {
public:
    struct Tab { std::string label; Widget* view = nullptr; };
    std::vector<Tab> tabs;
    int  active = 0;
    int  tab_h  = 20;
    std::function<void(int)> on_change;   // fired when the active tab changes

    void add(const std::string& label, Widget* view) { tabs.push_back({ label, view }); }

    SDL_Rect body() const {
        return SDL_Rect{ rect.x, rect.y + tab_h, rect.w, rect.h - tab_h };
    }

    void draw(App& app) override {
        const Theme& t = theme();
        // tab strip
        SDL_Rect strip{ rect.x, rect.y, rect.w, tab_h };
        fill_rect(app.ren, strip, t.panel);
        hline(app.ren, strip.x, strip.x + strip.w, strip.y + strip.h - 1, t.dim);
        int x = rect.x + 2;
        for (int i = 0; i < (int)tabs.size(); ++i) {
            const int cw = app.mono.cw() ? app.mono.cw() : 6;
            const int tw = (int)tabs[i].label.size() * cw + 16;
            SDL_Rect tr{ x, rect.y + 1, tw, tab_h - 1 };
            const bool on = (i == active);
            fill_rect(app.ren, tr, on ? t.bg : t.panel);
            if (on) { hline(app.ren, tr.x, tr.x + tr.w, tr.y, t.accent);
                      vline(app.ren, tr.x, tr.y, tr.y + tr.h, t.dim);
                      vline(app.ren, tr.x + tr.w, tr.y, tr.y + tr.h, t.dim); }
            app.mono.draw(app.ren, tr.x + 8, tr.y + (tr.h - app.mono.ch()) / 2,
                          tabs[i].label.c_str(), on ? t.text : t.dim);
            x += tw + 2;
        }
        // active view
        if (active >= 0 && active < (int)tabs.size() && tabs[active].view) {
            tabs[active].view->rect = body();
            tabs[active].view->draw(app);
        }
    }

    bool on_mouse(App& app, const MouseEv& e) override {
        // tab strip hit-test on press
        if (e.pressed && e.y >= rect.y && e.y < rect.y + tab_h) {
            int x = rect.x + 2;
            for (int i = 0; i < (int)tabs.size(); ++i) {
                const int cw = app.mono.cw() ? app.mono.cw() : 6;
                const int tw = (int)tabs[i].label.size() * cw + 16;
                if (e.x >= x && e.x < x + tw) {
                    if (active != i) { active = i; if (on_change) on_change(i); app.request_redraw(); }
                    return true;
                }
                x += tw + 2;
            }
            return true;
        }
        if (active >= 0 && active < (int)tabs.size() && tabs[active].view) {
            tabs[active].view->rect = body();
            return tabs[active].view->on_mouse(app, e);
        }
        return false;
    }

    bool on_wheel(App& app, int dx, int dy) override {
        if (active >= 0 && active < (int)tabs.size() && tabs[active].view)
            return tabs[active].view->on_wheel(app, dx, dy);
        return false;
    }
    bool on_key(App& app, SDL_Keycode k) override {
        if (active >= 0 && active < (int)tabs.size() && tabs[active].view)
            return tabs[active].view->on_key(app, k);
        return false;
    }
    bool on_key_up(App& app, SDL_Keycode k) override {
        if (active >= 0 && active < (int)tabs.size() && tabs[active].view)
            return tabs[active].view->on_key_up(app, k);
        return false;
    }
    // Docked views (e.g. the Sample Editor) implement their own local undo;
    // without this forward gui.cpp's per-view Ctrl+Z route can never reach them
    // and the global project undo fires instead.
    bool on_undo(App& app, bool redo) override {
        if (active >= 0 && active < (int)tabs.size() && tabs[active].view)
            return tabs[active].view->on_undo(app, redo);
        return false;
    }
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_TAB_HOST_H
