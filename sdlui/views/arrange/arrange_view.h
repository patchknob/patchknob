//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_view.h
//
//  ArrangeView -- the seq24 ARRANGEMENT (song) editor ported to the SDL2
//  grayscale/green toolkit (sdlui/gui.h).  One self-contained ui::Widget that
//  draws, over a live `perform*`:
//
//    * a LEFT track-header column (one row per ACTIVE sequence): status spine,
//      INS/AUD type badge, track name + bus/ch/time-sig, a level/VU strip and
//      Mute / Solo toggle buttons                       (ports src/perfnames.cpp)
//    * a TOP time ruler with bar numbers + L / R loop markers
//                                                        (ports src/perftime.cpp)
//    * the RIGHT arrangement canvas: alternating-stripe track lanes, a bar/beat
//      grid, rounded themed clip blocks (trigger regions) carrying the pattern
//      name + a tiny note preview, and a moving playhead
//                                                        (ports src/perfroll.cpp)
//
//  Interactions mirror perfroll: left-drag on empty lane places+grows a clip
//  trigger (snapped to the pattern length); left-drag on a clip moves it, or
//  resizes it when grabbed near an edge; right-click deletes; middle-click
//  splits.  The ruler sets the L/R loop ticks.  The header M/S buttons toggle
//  song-mute / solo.  Wheel = horizontal ZOOM (vertical wheel), horizontal
//  wheel / arrow keys pan.
//
//  Strictly two-tone: every colour comes from ui::theme() so it flips with the
//  Light / Midnight mode at runtime.
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_ARRANGE_VIEW_H
#define SEQ24_SDLUI_ARRANGE_VIEW_H

#include "gui.h"
#include <vector>

class perform;   // seq24 engine (gtkmm-free core)

namespace arrange {

class ArrangeView : public ui::Widget {
public:
    explicit ArrangeView(perform* p);

    // ui::Widget -------------------------------------------------------------
    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;

    // Optional externally-driven playhead (in MIDI ticks).  When < 0 the view
    // reads perform::get_tick() instead.  The shell can set this to animate a
    // playhead when the transport isn't actually running.
    long playhead_tick = -1;

    // Layout knobs (pixels).  The shell just sizes `rect`; these partition it.
    int header_w = 6 * 30;   // left track-header column width  (== c_names_x)
    int ruler_h  = 20;       // top time-ruler height
    int row_h    = 40;       // per-track lane / header row height

private:
    perform* m_perf;

    // horizontal mapping: ticks-per-pixel (zoom) + left-edge scroll (ticks)
    double m_scale_x;        // ticks per pixel  (default c_perf_scale_x == 32)
    long   m_scroll_ticks;   // tick at the canvas left edge
    int    m_v_offset;       // index (into the active-track list) of top row

    // grid / snap (ticks)
    long   m_snap;
    long   m_measure_len;
    long   m_beat_len;

    // interaction state (mirrors perfroll drag machine)
    bool   m_mouse_down = false;
    bool   m_moving     = false;
    bool   m_growing    = false;
    bool   m_grow_dir   = false;   // true = drag start edge, false = end edge
    bool   m_adding     = false;   // placing+growing a fresh clip
    int    m_drop_seq   = -1;      // absolute sequence index under the press
    long   m_drop_tick  = 0;
    long   m_drop_offset = 0;      // click-tick - selected-trigger-edge

    // solo bookkeeping (ports perfnames::apply_solo)
    std::vector<char> m_solo;
    std::vector<char> m_mute_snapshot;
    bool   m_solo_active = false;

    // ---- helpers -----------------------------------------------------------
    std::vector<int> active_list() const;            // active sequence indices
    int  canvas_x() const { return rect.x + header_w; }
    int  canvas_y() const { return rect.y + ruler_h; }
    int  canvas_w() const { return rect.w - header_w; }
    int  canvas_h() const { return rect.h - ruler_h; }

    int  tick_to_x(long tick) const;
    long x_to_tick(int px) const;
    long snap(long tick) const;
    int  row_at(int py) const;                       // on-screen row or -1
    long playhead() const;

    void draw_ruler   (ui::App& app, const std::vector<int>& act);
    void draw_canvas  (ui::App& app, const std::vector<int>& act);
    void draw_clips   (ui::App& app, int seq, int lane_y);
    void draw_headers (ui::App& app, const std::vector<int>& act);

    void press_canvas (ui::App& app, const ui::MouseEv& e, int seq);
    void drag_canvas  (ui::App& app, const ui::MouseEv& e);
    void apply_solo   ();
};

} // namespace arrange
#endif
