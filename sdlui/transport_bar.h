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
    std::function<void()> on_metronome;
    std::function<void(bool)> on_count_in;
    std::function<void()> on_mode;                     // toggle SONG <-> LIVE
    std::function<void(double)> on_tempo;              // committed new BPM value
    std::function<void(int)> on_record_quantize;       // ticks; 0 = off
    // Q-Range as a PERCENT of the largest possible deviation (half a grid step).
    // 0 = snap everything (rigid); 100 = never snap.  Expressed relative to the
    // grid rather than in absolute ticks so it keeps its meaning when the grid
    // changes -- a fixed tick count >= half the grid would silently disable
    // quantise altogether.
    std::function<void(int)> on_record_qrange;         // percent 0..100
    std::function<void(int)> on_record_swing;          // percent 50..75 (50=straight)

    // Punch record modes (Pro Tools ch.27): 0 = Normal, 1 = QuickPunch,
    // 2 = TrackPunch, 3 = DestructivePunch.  The shell owns the mode; the bar
    // shows the badge ("P" / "T" / "DP") in the Record button, cycles modes on
    // Ctrl+click, and offers a right-click pop-up listing all four.
    std::function<int()>     get_record_mode;
    std::function<void(int)> on_record_mode;

    // LIVE STATE readers, polled every draw() (any may be null -> false/0):
    std::function<bool()>   is_rolling, is_rec, is_loop;
    //! Punch-status inputs for the Record button's four-state display (PT
    //! p639/643).  `punch_track_count` = how many tracks are punch-enabled for
    //! the current mode; `is_rec_ready` = the transport is record-armed but not
    //! yet capturing (Record Ready -- the flashing states).  `is_rec` stays
    //! "actually recording" (solid).
    std::function<int()>    punch_track_count;
    std::function<bool()>   is_rec_ready;
    //! TRUE from the moment REC / PLAY is PRESSED until the engine actually arms
    //! or rolls -- i.e. for the whole count-in pre-roll.
    //!
    //! Without these the buttons were lit purely from engine state, so pressing
    //! REC with count-in enabled produced NO visual change for a whole bar: the
    //! press started the pre-roll but `is_rec` (g_recArmed) only becomes true
    //! when the count-in FINISHES.  The button felt like a dead momentary key
    //! and people pressed it again -- which used to restart the count-in and
    //! move the punch point.  Latching the visual on the press is what makes it
    //! read as a SWITCH.
    std::function<bool()>   is_rec_pending, is_play_pending;
    std::function<bool()>   is_metronome, is_count_in;
    std::function<bool()>   is_song;   // true = SONG (timeline/trigger-gated),
                                       // false = LIVE (armed patterns loop)
    std::function<double()> get_tempo;                 // current BPM
    std::function<int()>    get_record_quantize;
    std::function<int()>    get_record_qrange;         // percent 0..100
    std::function<int()>    get_record_swing;          // percent 50..75
    //! Screen recorder: toggle, and live state for the chip (recording? how
    //! long? is it even usable -- ffmpeg may be missing).
    std::function<void()>   on_screen_record;
    std::function<bool()>   is_screen_recording;
    std::function<bool()>   screen_record_available;
    std::function<double()> screen_record_elapsed;
    std::function<void(int&,int&,int&)> get_bbt;       // fills bar,beat,tick (1-based)

    // Preferred height for the compact strip (the shell sizes rect from this).

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_key(App& app, SDL_Keycode k) override;     // used only during tempo edit
    //! Drop every in-flight interaction.  The shell calls this when the window
    //! loses focus / the pointer is taken away mid-gesture.  Without it a button
    //! that was pressed when you alt-tabbed stayed drawn in its PRESSED shade
    //! forever, and the count-in popup stayed open with no way to dismiss it.
    void cancel_interaction(App& app) override;

private:
    // Computed button + readout rects, shared by draw() and on_mouse() so clicks
    // land exactly on what is drawn.  Buttons dropped (compact, tight) have w==0.
    struct Rects {
        SDL_Rect btn[9];      // 8 transport buttons + metronome
        int      nbtn;        // number of button slots laid out
        SDL_Rect bbt;         // BBT clock readout
        SDL_Rect tempo;       // tempo number field (clickable / editable)
        SDL_Rect meter;       // "4/4" meter label (full mode only)
        SDL_Rect mode;        // SONG/LIVE toggle chip
        SDL_Rect recquant;    // record quantize selector (full mode only)
        SDL_Rect qrange;      // record quantize Q-Range  (full mode only)
        SDL_Rect qswing;      // record quantize swing    (full mode only)
        SDL_Rect screenrec;   // screen recorder arm / disarm
        bool     has_meter;
    };
    Rects layout(App& app) const;

    //! Everything the button painter needs, read ONCE per frame.  It used to be
    //! three loose bools that grew every time a button gained a state; polling
    //! the callbacks per button also meant two buttons could disagree inside one
    //! frame if the engine changed between them.
    struct BtnState {
        bool rolling = false, rec = false, loop = false;
        bool rec_pending = false, play_pending = false;
        bool rec_ready = false;        // record-armed, not yet capturing
        int  rec_mode  = 0;            // 0 Normal / 1 QP / 2 TP / 3 DP
        int  punch_n   = 0;            // punch-enabled track count
    };
    void draw_button(App& app, int i, const SDL_Rect& b, const BtnState& st);
    void draw_bbt  (App& app, const SDL_Rect& r);
    void draw_tempo(App& app, const SDL_Rect& r);

    void fire_button(int i);
    void begin_tempo_edit(App& app);
    void apply_tempo();

    //! The strip's pop-up rows.  kind 0 = count-in toggle (metronome anchor);
    //! kind 1 = record-mode picker (Record anchor) -- one horizontal row of
    //! "[x] Mode" segments, because the compact strip has no vertical room for
    //! a stacked menu and clicks outside the widget rect route elsewhere.
    //! Segment rects are computed by click_menu_segments() so draw() and
    //! on_mouse() always agree on what is where.
    int click_menu_segments(App& app, std::string* labels, SDL_Rect* segs,
                            int cap) const;

    std::string m_tempo_edit;      // text buffer while editing tempo
    int  m_press_btn  = -1;        // button index currently pressed (for feedback)
    bool m_mouse_down = false;     // down-edge tracking (motion re-sends pressed)
    bool m_click_menu = false;
    int  m_click_menu_kind = 0;    // 0 = count-in, 1 = record-mode picker
    SDL_Rect m_click_menu_rect{0,0,0,0};
};

} // namespace ui
#endif
