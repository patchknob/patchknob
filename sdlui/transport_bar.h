//----------------------------------------------------------------------------
//  sdlui/transport_bar.h -- Ardour-style transport bar widget for the SDL2
//  two-tone DAW UI.  Vector (filled-shape) icon buttons, a BBT clock readout,
//  and a click-to-edit tempo field.  Two layouts:
//
//    * COMPACT -- a thin horizontal icon strip + inline BBT + tempo, meant to
//      be mounted inline in a toolbar.
//    * FULL    -- a larger panel with a big prominent BBT clock, tempo + BPM
//      label + a "4/4" meter, meant to be hosted in a ui::Window.
//
//  All text is ASCII; every color comes from theme() (strictly two-tone).  The
//  shell wires the action callbacks to the engine transport and the live-state
//  readers to the engine clock; every callback is guarded for null.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_TRANSPORT_BAR_H
#define PATCHKNOB_SDLUI_TRANSPORT_BAR_H

#include "gui.h"

namespace ui {

class TransportBar : public Widget {
public:
    bool compact = false;          // true = small icon strip; false = large panel

    // ACTION callbacks (the shell wires these to the engine transport):
    std::function<void()> on_to_start, on_rewind, on_play, on_stop,
                          on_rec, on_ffwd, on_to_end, on_loop;
    std::function<void()> on_mode;                     // toggle SONG <-> LIVE
    std::function<void(double)> on_tempo;              // committed new BPM value

    // LIVE STATE readers, polled every draw() (any may be null -> false/0):
    std::function<bool()>   is_rolling, is_rec, is_loop;
    std::function<bool()>   is_song;   // true = SONG (timeline/trigger-gated),
                                       // false = LIVE (armed patterns loop)
    std::function<double()> get_tempo;                 // current BPM
    std::function<void(int&,int&,int&)> get_bbt;       // fills bar,beat,tick (1-based)

    // Preferred height for the compact strip (the shell sizes rect from this).
    int preferred_compact_h() const { return 28; }

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_key(App& app, SDL_Keycode k) override;     // used only during tempo edit

private:
    // Computed button + readout rects, shared by draw() and on_mouse() so clicks
    // land exactly on what is drawn.  Buttons dropped (compact, tight) have w==0.
    struct Rects {
        SDL_Rect btn[8];      // 8 transport buttons (see .cpp for order)
        int      nbtn;        // number of button slots laid out
        SDL_Rect bbt;         // BBT clock readout
        SDL_Rect tempo;       // tempo number field (clickable / editable)
        SDL_Rect meter;       // "4/4" meter label (full mode only)
        SDL_Rect mode;        // SONG/LIVE toggle chip
        bool     has_meter;
    };
    Rects layout(App& app) const;

    void draw_button(App& app, int i, const SDL_Rect& b,
                     bool rolling, bool rec, bool loop);
    void draw_bbt  (App& app, const SDL_Rect& r);
    void draw_tempo(App& app, const SDL_Rect& r);

    void fire_button(int i);
    void begin_tempo_edit(App& app);
    void apply_tempo();

    std::string m_tempo_edit;      // text buffer while editing tempo
    int  m_press_btn  = -1;        // button index currently pressed (for feedback)
    bool m_mouse_down = false;     // down-edge tracking (motion re-sends pressed)
};

} // namespace ui
#endif
