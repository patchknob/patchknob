//----------------------------------------------------------------------------
//  sdlui/views/master_mixer/master_mixer_view.h
//
//  THE MIXER.  A horizontal row of Ardour-style channel strips over the engine's
//  MasterMixerNode: `bus_count` track strips, then one AUX-RETURN strip per
//  engine aux bus (audio_app.h "AUX SENDS / AUX BUSES") plus a slim "+ AUX"
//  add-bus column, then the MASTER strip.
//
//  Ported from Ardour (gtk2_ardour/), strip laid out in the same order as
//  MixerStrip::global_vpacker (mixer_strip.cc:335-350):
//
//      1. NAME header        number + name button      (mixer_strip.cc name_button)
//      2. INPUT button       what feeds the strip      (mixer_strip.cc input_button)
//      3. PROCESSOR BOX      the insert FX chain       (processor_box.cc)
//                            add / delete / reorder by drag / bypass /
//                            pre- vs post-fader placement, with the FADER shown
//                            as its own entry exactly like Ardour's Amp
//                            (ProcessorBox::setup_entry_positions)
//      4. PAN                                          (mixer_strip.cc panners)
//      5. MUTE / SOLO        Ardour state semantics    (route_ui.cc
//                            mute_active_state / solo_active_state): explicit
//                            self-mute vs implicit muted-by-others-soloing,
//                            plus solo isolate and solo safe
//      6. GAIN               fader on Ardour's gain->position taper
//                            (pbd/control_math.h gain_to_position) + a stereo
//                            meter, a numeric dB entry (GainMeterBase::
//                            gain_activated) and a click-to-reset peak readout
//                            (GainMeterBase::update_meters)
//      7. OUTPUT button      the strip's direct out    (mixer_strip.cc)
//      8. COMMENTS button                              (mixer_strip.cc)
//
//  Every meter is drawn through sdlui/meter.h (ui::meter) -- this view never
//  rasterises a meter itself.
//
//  MODEL BINDING.  The strip reads and writes the engine directly through
//  src/audio_app.h (insert chains, routing, solo/mute resolution), and the
//  std::function hooks below OVERRIDE that wherever a shell wants to supply its
//  own values.  Any hook may be null.  `get_gain`/`set_gain` carry a linear GAIN
//  COEFFICIENT (0..kMaxGain, unity = 1.0), not a fader fraction -- the Ardour
//  taper lives inside the view.
//
//  Mounting (shell side):
//      mixerui::MasterMixerView mv;
//      mv.get_bus_count = []{ return ...; };
//      app.roots.push_back(&mv);
//      app.on_layout = [&](ui::App& a){ mv.rect = { x, y, w, h }; };
//      // drive periodic redraws so the meters stay live.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_MASTER_MIXER_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_MASTER_MIXER_VIEW_H

#include "gui.h"

#include "meter.h"

#include <functional>
#include <string>
#include <vector>

namespace mixerui {

//! Ardour's default Config->get_max_gain(): +6 dB at the top of the fader.
static const float kMaxGain = 2.0f;

class MasterMixerView : public ui::Widget {
public:
    // ---- configuration -----------------------------------------------------
    int bus_count = 8;      // number of track strips (the master is an extra strip)
    //! Aux-send knobs per TRACK strip.  Only ever DRAWN when the host has
    //! bound both get_send and set_send (see live_aux_count): the knobs used to
    //! render, highlight and drag with both hooks unbound, so they looked like
    //! working sends while routing precisely nothing.  An inert control is
    //! worse than an absent one, so an unwired host still gets no knobs and no
    //! reserved space at all.  When get_aux_count is bound this is SYNCED from
    //! it every frame, so the send section (and the aux-return strips) appears
    //! exactly when buses exist and vanishes when the last one is removed.
    int aux_count = 0;
    bool show_inserts = true;
    ui::meter::Type meter_type = ui::meter::PPM;

    //! Live model count, so draw/hit-test follow structural changes made while
    //! the mixer window is already open.
    std::function<int()> get_bus_count;
    //! The mixer node the strips address.  Defaults to the master mixer node.
    std::function<int()> get_node;
    //! Live AUX BUS count.  Return the number of buses to show, 0 for "none
    //! right now", or NEGATIVE for "aux buses are not available on this node".
    //! The distinction matters because one view object serves both the master
    //! mixer and plain mixer-node windows, while the whole aux API
    //! (audio_app_master_aux_* / audio_app_master_send_*) exists only for the
    //! master -- a shell showing another node answers -1 and gets no aux
    //! strips, no send knobs and no add-bus column, instead of pointing that
    //! node's strips at the master's buses.
    std::function<int()> get_aux_count;
    //! Fired when a processor entry asks to be edited (double-click / "Edit"),
    //! with the patch node id -- the shell owns plugin editor windows.
    std::function<void(int)> on_edit_insert;

    // ---- per-strip readers / setters (all optional overrides) --------------
    //  idx: 0..bus_count-1 == tracks, idx == master_index() == the MASTER
    //  strip.  These hooks are NEVER called with an aux strip's index -- the
    //  aux strips read and write the audio_app aux API inside the view, so a
    //  shell that tests `i < bus_count ? track : master` stays correct.
    std::function<std::string(int)>    get_label;   // "1".."8", "MASTER"
    std::function<float(int)>          get_gain;    // GAIN COEFFICIENT (0..kMaxGain)
    std::function<void(int,float)>     set_gain;
    std::function<float(int)>          get_level;   // live peak amplitude (meter)
    std::function<float(int,int)>      get_level_stereo; // strip, 0=L/1=R
    std::function<bool(int)>           get_mute;
    std::function<bool(int)>           get_solo;
    std::function<void(int)>           toggle_mute;
    std::function<void(int)>           toggle_solo;
    std::function<float(int)>          get_pan;     // -1..1
    std::function<void(int,float)>     set_pan;
    std::function<float(int,int)>      get_send;    // (strip, aux) -> 0..1
    std::function<void(int,int,float)> set_send;

    // ---- ui::Widget --------------------------------------------------------
    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;
    void cancel_interaction(ui::App& app) override;

    // ---- fixed metrics (logical px) ----------------------------------------
    static const int STRIP_W  = 92;   // a track strip column
    static const int MASTER_W = 104;  // the master strip is wider
    static const int GAP      = 4;
    static const int PAD      = 6;
    static const int AUX_ADD_W = 22;  // the slim "+ AUX" add-bus column

    // ---- Ardour gain law (pbd/control_math.h + ardour/dB.h) -----------------
    static double gain_to_position(double g);
    static double position_to_gain(double pos);
    static float  gain_to_slider(float gain);   // coefficient -> 0..1 fader pos
    static float  slider_to_gain(float pos);
    static float  coefficient_to_dB(float c);
    static float  dB_to_coefficient(float db);

private:
    // Every sub-rect of one strip, computed once so draw + hit-test agree.
    struct Sub {
        SDL_Rect area{0,0,0,0};
        SDL_Rect header{0,0,0,0};
        SDL_Rect input{0,0,0,0};
        SDL_Rect procs{0,0,0,0};      // the processor box
        SDL_Rect sends{0,0,0,0};      // bounding box of the aux knob grid
        SDL_Rect pan{0,0,0,0};
        SDL_Rect pan_bar{0,0,0,0};
        SDL_Rect mute{0,0,0,0};
        SDL_Rect solo{0,0,0,0};
        SDL_Rect iso{0,0,0,0};        // solo-isolate LED
        SDL_Rect fader{0,0,0,0};      // fader track (handle lives here)
        SDL_Rect meter{0,0,0,0};      // stereo meter column pair
        SDL_Rect scale{0,0,0,0};      // dB ruler gutter
        SDL_Rect db{0,0,0,0};         // numeric gain entry
        SDL_Rect peak{0,0,0,0};       // peak-hold readout
        SDL_Rect output{0,0,0,0};
        SDL_Rect comment{0,0,0,0};
        int knob_r        = 8;
        int knobs_per_row = 1;
        int send_cell_w   = 1;
        int send_cell_h   = 1;
        int proc_row_h    = 12;
        int proc_rows     = 0;        // rows that fit in `procs`
    };

    //! One processor-box display row.  The FADER is a row like Ardour's Amp
    //! entry, so the pre/post split is visible and draggable-across.
    struct ProcRow {
        int  slot = -1;               // insert slot, -1 for the fader row
        bool fader = false;
        bool pre = true;
    };

    struct StripState {
        ui::meter::State meterL, meterR;
        float maxPeakDb = -318.f;
        bool  selfMute = false, selfSolo = false, soloIso = false, soloSafe = false;
        bool  seeded = false;
        std::string name;             // local override ("" = use get_label)
        std::string comment;
        int   procScroll = 0;
        int   selProc = -1;
    };

    // ---- popup menus (Ardour's context menus, drawn in-view) ---------------
    enum MenuKind { M_NONE = 0, M_PROC, M_PLUGIN, M_INPUT, M_OUTPUT, M_STRIP, M_METER, M_SEND };
    struct MenuItem {
        std::string label;
        int  id = 0;
        bool separator = false;
        bool enabled = true;
        bool check = false;
        bool checked = false;
    };
    struct Menu {
        MenuKind kind = M_NONE;
        std::vector<MenuItem> items;
        SDL_Rect rect{0,0,0,0};
        int scroll = 0;
        int hover = -1;
        int strip = -1;
        int slot  = -1;
        bool pre  = true;             // placement the pending action should use
    };

    // geometry / model
    void     sync_model(ui::App& app);
    int      node() const;
    //! Strip order: TRACKS, then one strip per AUX BUS, then the MASTER --
    //! consoles put the returns between the channels and the mix bus, and the
    //! master keeps its far-right seat.  track_of() is -1 for aux strips AND
    //! the master: neither is addressable through the per-track APIs.
    int      track_of(int strip) const { return strip < bus_count ? strip : -1; }
    int      master_index() const { return (bus_count < 0 ? 0 : bus_count) + live_aux_count(); }
    bool     is_master(int idx) const { return idx == master_index(); }
    bool     is_aux(int idx) const { return idx >= bus_count && idx < master_index(); }
    int      aux_of(int idx) const { return is_aux(idx) ? idx - bus_count : -1; }
    //! What the insert API's `track` argument must be for this strip: the
    //! track index, -1 for the MASTER, or the aux STRIP CODE (-2 - aux,
    //! audio_app_master_aux_strip) -- so a bus's processor box goes through
    //! the exact same insert API and UI code as every other strip.
    int      chain_id(int idx) const;
    //! Aux UI (send knobs, aux strips, add-bus column) is only offered when
    //! the shell wired ALL of it: the count feed and both level hooks.
    bool     aux_ui() const { return m_aux_supported && get_send && set_send; }
    SDL_Rect add_bus_rect() const;
    int      strip_count() const { return (bus_count < 0 ? 0 : bus_count) + live_aux_count() + 1; }
    int      strip_x(int idx) const;
    SDL_Rect strip_area(int idx) const;
    Sub      layout(ui::App& app, int idx, const SDL_Rect& area) const;
    bool     send_pos(const Sub& L, int aux, int& cx, int& cy, int& r) const;
    //! aux_count, but 0 unless the host actually wired the send hooks.  Every
    //! layout / draw / hit-test path goes through this, so an unwired host gets
    //! no send knobs and no space reserved for them.
    int      live_aux_count() const {
        return (get_send && set_send && aux_count > 0) ? aux_count : 0;
    }
    int      content_w() const;
    void     clamp_scroll();
    StripState& state(int idx);

    // processor box
    std::vector<ProcRow> proc_rows(int idx) const;
    int      proc_row_at(ui::App& app, int idx, int y) const;
    void     add_plugin(int idx, int pluginIndex, bool pre);

    // solo / mute (route_ui.cc semantics)
    bool     any_solo() const;
    bool     muted_by_others(int idx) const;
    void     apply_mutes();
    void     seed_strip(int idx);

    // drawing
    void     draw_strip(ui::App& app, int idx, const SDL_Rect& area);
    void     draw_procs(ui::App& app, int idx, const Sub& L);
    void     draw_gain(ui::App& app, int idx, const Sub& L, bool master);
    void     draw_button(ui::App& app, const SDL_Rect& q, const std::string& text,
                         bool active, bool implicit, bool hot);
    void     draw_menu(ui::App& app);

    // input
    void     begin_press(ui::App& app, int idx, const ui::MouseEv& e);
    void     apply_send(ui::App& app, const ui::MouseEv& e);
    void     apply_pan(ui::App& app, const ui::MouseEv& e);
    void     apply_fader(ui::App& app, const ui::MouseEv& e);
    void     begin_gain_entry(ui::App& app, int idx);
    void     begin_rename(ui::App& app, int idx);
    //! The processor selection is SINGLE and view-wide: setting it on one strip
    //! clears it everywhere else, so Delete can only ever act on the row that
    //! is actually drawn selected.
    void     select_proc(int idx, int slot);
    void     clear_proc_selection();
    void     begin_comment(ui::App& app, int idx);
    void     open_menu(ui::App& app, MenuKind kind, int idx, int slot, const SDL_Rect& anchor);
    void     menu_pick(ui::App& app, int id);
    bool     menu_mouse(ui::App& app, const ui::MouseEv& e);

    std::vector<StripState> m_strips;
    //! get_aux_count() >= 0 at the last sync -- i.e. this node HAS an aux API.
    bool   m_aux_supported = false;
    //! Structure at the last sync, so a bus/aux add/remove can reset the strip
    //! states whose indices just shifted (see sync_model).
    int    m_last_bus = -1;
    int    m_last_aux = -1;
    Menu   m_menu;
    int    m_scroll_x = 0;
    unsigned m_lastTicks = 0;
    // One meter dt per FRAME, not per strip -- see MasterMixerView::draw().
    double   m_meterDt = 1.0 / 60.0;

    // in-view text editing (gain entry / rename / comment)
    enum EditKind { E_NONE = 0, E_GAIN, E_NAME, E_COMMENT };
    //! Strip that owns the single processor selection, -1 for none.
    int         m_sel_strip = -1;
    //! Header double-click detection (rename is a double-click, not a click).
    int         m_hdr_click_idx = -1;
    unsigned    m_hdr_click_ms  = 0;
    int         m_edit_kind = E_NONE;
    int         m_edit_strip = -1;
    std::string m_edit_buf;

    // clipboard for processor copy/paste (Ardour ProcessorBox cut/copy/paste)
    bool        m_clip_valid = false;
    int         m_clip_plugin = -1;      // index into the cached plugin inventory
    bool        m_clip_pre = true;

    // active drag gesture
    enum Drag { D_NONE = 0, D_FADER, D_SEND, D_PAN, D_PROC, D_CONSUME };
    int   m_drag       = D_NONE;
    int   m_drag_strip = -1;
    int   m_drag_aux   = -1;
    int   m_drag_slot  = -1;
    int   m_drag_row   = -1;
    float m_drag_start = 0.f;
    int   m_drag_y0    = 0;
    int   m_drag_x0    = 0;
    bool  m_drag_moved = false;
};

} // namespace mixerui

#endif // PATCHKNOB_SDLUI_VIEWS_MASTER_MIXER_VIEW_H
