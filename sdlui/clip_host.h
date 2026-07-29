//----------------------------------------------------------------------------
//  sdlui/clip_host.h
//
//  Wraps a clip editor (piano roll / tracker) with a top strip that shows and
//  lets you SELECT the clip's instrument (a named-instrument registry).  Used
//  as a ui::Window's content so the selector rides above the editor.  Clicking
//  the strip drops an in-canvas list of instrument names; picking one fires
//  on_select(index).  Keyboard/mouse/wheel pass through to the wrapped view.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_CLIP_HOST_H
#define PATCHKNOB_SDLUI_CLIP_HOST_H

#include "gui.h"
#include <functional>
#include <string>
#include <vector>

namespace ui {

class ClipHost : public Widget {
public:
    Widget* view = nullptr;                              // the piano/tracker editor
    std::function<std::string()>              current;   // current instrument label
    std::function<std::vector<std::string>()> options;   // selectable instrument names
    std::function<void(int)>                  on_select; // picked instrument index
    int bar_h = 22;

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;
    bool on_key_up(App& app, SDL_Keycode k) override;

private:
    bool     m_menu = false;
    int      menu_w = 220;
    SDL_Rect view_rect() const { return SDL_Rect{ rect.x, rect.y + bar_h, rect.w, rect.h - bar_h }; }
    SDL_Rect menu_rect(App& app, int nOpts) const;
};

} // namespace ui

#endif
