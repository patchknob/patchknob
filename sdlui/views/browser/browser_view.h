//----------------------------------------------------------------------------
//  sdlui/views/browser/browser_view.h -- SDL2 port of the VST plugin browser.
//
//  A scrollable, filterable list of scanned PluginDescriptors rendered in the
//  lean grayscale/green toolkit (gui.h).  Columns NAME | VENDOR | FORMAT | INSTR/FX |
//  I/O, a type-to-filter box, and two actions -- "load as instrument" (double
//  click / Enter) and "add as FX" (F2 / button).
//
//  PURE UI: depends only on the header-only engine contract
//  (PatchKnob::engine::PluginDescriptor) + the toolkit.  It never calls audio_app;
//  the shell wires the callbacks below to audio_app_set_track_instrument /
//  audio_app_add_track_fx.
//
//  Mounting (shell):
//      BrowserView browser;
//      browser.populate(descs);
//      browser.set_track(cur_track);
//      browser.on_load_instrument = [](const PluginDescriptor& d){ ... };
//      browser.on_add_fx          = [](const PluginDescriptor& d){ ... };
//      app.roots.push_back(&browser);
//      app.on_layout = [&](ui::App& a){ browser.rect = {0,0,a.w,a.h}; };
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_BROWSER_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_BROWSER_VIEW_H

#include "gui.h"
#include "engine/plugin_api.h"

#include <string>
#include <vector>
#include <functional>

namespace ui {

class BrowserView : public Widget {
public:
    typedef PatchKnob::engine::PluginDescriptor PluginDescriptor;

    BrowserView() {}

    // ---- controller -> view -------------------------------------------------
    //! Replace the descriptor set and (re)apply the current filter.
    void populate(const std::vector<PluginDescriptor>& list);
    //! Show / clear the "scanning..." placeholder.
    void set_scanning(bool on);
    //! Which mixer track the actions target (display only).
    void set_track(int track) { m_track = track; }
    int  track() const { return m_track; }

    // ---- view -> controller (set these callbacks) ---------------------------
    std::function<void(const PluginDescriptor&)> on_load_instrument;
    std::function<void(const PluginDescriptor&)> on_add_fx;

    // ---- Widget -------------------------------------------------------------
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;

private:
    // geometry (recomputed each draw / mouse from `rect`)
    struct Layout {
        SDL_Rect filter;        // filter text box
        SDL_Rect header;        // column header strip
        SDL_Rect list;          // scrollable rows area
        SDL_Rect statusbar;     // bottom status + buttons
        SDL_Rect btn_instr;     // "load instrument" button
        SDL_Rect btn_fx;        // "add fx" button
        int      row_h = 0;     // per-row height
        int      visible = 0;   // rows that fit in `list`
        int      col_name = 0;  // char columns
        int      col_vendor = 0;
        int      col_format = 0;
        int      col_kind = 0;
        int      col_io = 0;
    };
    Layout compute_layout(App& app);

    void rebuild_filtered();
    void clamp_scroll();
    void ensure_visible(const Layout& L);
    void fire_instrument();
    void fire_fx();
    static std::string fit(const std::string& s, int n);
    static bool matches(const PluginDescriptor& d, const std::string& needle);

    std::vector<PluginDescriptor> m_all;      // full scanned set
    std::vector<int>              m_filtered; // indices into m_all after filter
    std::string                   m_filter;   // live filter text (lowercased typing)
    int   m_sel     = -1;     // selected row (index into m_filtered)
    int   m_scroll  = 0;      // first visible row (index into m_filtered)
    int   m_visible = 0;      // rows that fit in the list (from last layout)
    int   m_track   = -1;
    bool  m_scanning = false;

    // double-click detection
    Uint32 m_last_click_ms  = 0;
    int    m_last_click_row = -1;
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_VIEWS_BROWSER_VIEW_H
