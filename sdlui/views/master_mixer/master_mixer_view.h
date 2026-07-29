//----------------------------------------------------------------------------
//  sdlui/views/master_mixer/master_mixer_view.h
//
//  MASTER MIXER -- a single retained ui::Widget that draws a horizontal row of
//  channel strips: `bus_count` bus strips (index 0..bus_count-1) followed by one
//  MASTER strip on the right (index == bus_count).  Every value the strips show
//  or change flows through the std::function callbacks below; the shell wires
//  them to the engine.  Any callback may be null -- the view treats a missing
//  reader as 0 / false / "" and a missing setter as a no-op, so it is safe to
//  mount before the model exists.
//
//  Each strip, top to bottom:
//      1. NAME header       (get_label; accent header for the MASTER strip)
//      2. FX INSERT slot     (get_insert / on_insert -- "[+FX]" when empty)
//      3. AUX SEND knobs     (aux_count discs "A1".."A2"; get_send / set_send)
//      4. PAN                (get_pan / set_pan; "PAN" + C/L/R readout)
//      5. big ui::Fader      (metered dB backdrop via .level; .value = gain,
//                             .changed = set_gain) + a tiny dB readout
//      6. MUTE / SOLO        (get_mute / get_solo, toggle_mute / toggle_solo)
//
//  The view OWNS one ui::Fader per strip (bus_count + 1 of them, rebuilt when
//  bus_count changes); layout() positions each fader inside its strip so drawing
//  and hit-testing agree to the pixel.  Everything is two-tone via ui::theme()
//  and ASCII-only through the fixed monospace atlas.
//
//  Mounting (shell side):
//      mixerui::MasterMixerView mv;
//      mv.bus_count = 8;
//      mv.get_label = [](int i){ ... };   mv.get_gain = ...;  // wire callbacks
//      app.roots.push_back(&mv);
//      app.on_layout = [&](ui::App& a){ mv.rect = { x, y, w, h }; };
//      // drive periodic redraws so the fader level meters stay live.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_MASTER_MIXER_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_MASTER_MIXER_VIEW_H

#include "gui.h"
#include <functional>
#include <string>
#include <vector>

namespace mixerui {

class MasterMixerView : public ui::Widget {
public:
    // ---- configuration -----------------------------------------------------
    int bus_count = 8;      // number of bus strips (the master is an extra strip)
    int aux_count = 2;      // aux-send knobs per strip

    // ---- per-strip readers / setters ---------------------------------------
    //  idx: 0..bus_count-1 == buses, idx == bus_count == the MASTER strip.
    std::function<std::string(int)>    get_label;   // "1".."8", "MASTER"
    std::function<float(int)>          get_gain;    // 0..1 fader value
    std::function<void(int,float)>     set_gain;
    std::function<float(int)>          get_level;   // live peak amplitude (meter)
    std::function<bool(int)>           get_mute;
    std::function<bool(int)>           get_solo;
    std::function<void(int)>           toggle_mute;
    std::function<void(int)>           toggle_solo;
    std::function<float(int)>          get_pan;     // -1..1
    std::function<void(int,float)>     set_pan;
    std::function<std::string(int)>    get_insert;  // plugin name or "" (empty)
    std::function<void(int)>           on_insert;   // clicked the FX insert slot
    std::function<float(int,int)>      get_send;    // (strip, aux) -> 0..1
    std::function<void(int,int,float)> set_send;

    // ---- ui::Widget --------------------------------------------------------
    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;

    // ---- fixed metrics (logical px) ----------------------------------------
    static const int STRIP_W  = 64;   // a bus strip column
    static const int MASTER_W = 84;   // the master strip is wider
    static const int GAP      = 4;    // gap between strips
    static const int PAD      = 6;    // outer padding inside rect

private:
    // Every sub-rect of one strip, computed once so draw + hit-test agree.
    struct Sub {
        SDL_Rect area{0,0,0,0};
        SDL_Rect header{0,0,0,0};
        SDL_Rect fx{0,0,0,0};
        SDL_Rect sends{0,0,0,0};      // bounding box of the aux knob grid
        SDL_Rect pan{0,0,0,0};        // pan label line + bar
        SDL_Rect pan_bar{0,0,0,0};    // the draggable pan slider bar
        SDL_Rect fader{0,0,0,0};
        SDL_Rect db{0,0,0,0};
        SDL_Rect mute{0,0,0,0};
        SDL_Rect solo{0,0,0,0};
        int knob_r        = 9;
        int knobs_per_row = 1;
        int send_cell_w   = 1;
        int send_cell_h   = 1;
    };

    void     ensure_faders();
    int      strip_x(int idx) const;
    SDL_Rect strip_area(int idx) const;
    Sub      layout(ui::App& app, int idx, const SDL_Rect& area) const;
    bool     send_pos(const Sub& L, int aux, int& cx, int& cy, int& r) const;
    int      content_w() const;
    void     clamp_scroll();
    void     draw_strip(ui::App& app, int idx, const SDL_Rect& area);
    void     begin_press(ui::App& app, int idx, const ui::MouseEv& e);
    void     apply_send(ui::App& app, const ui::MouseEv& e);
    void     apply_pan(ui::App& app, const ui::MouseEv& e);

    // Owned fader widgets: one per bus + one for the master (index bus_count).
    std::vector<ui::Fader> m_faders;

    int m_scroll_x = 0;                 // horizontal scroll offset (px)

    // active drag gesture (a fresh press has m_drag == D_NONE)
    enum Drag { D_NONE = 0, D_FADER, D_SEND, D_PAN, D_CONSUME };
    int   m_drag       = D_NONE;
    int   m_drag_strip = -1;
    int   m_drag_aux   = -1;
    float m_drag_start = 0.f;           // value at grab (relative knob drag)
    int   m_drag_y0    = 0;             // pointer y at grab
};

} // namespace mixerui

#endif // PATCHKNOB_SDLUI_VIEWS_MASTER_MIXER_VIEW_H
