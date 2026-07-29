//----------------------------------------------------------------------------
//  sdlui/views/automation/automation_view.h -- SDL2 automation editor + a small
//  keyfollow (scale-master / scale-follow) panel for the PatchKnob.
//
//  AutomationView is a single retained ui::Widget drawn over:
//      * a `sequence*`  -- only used to read the pattern length (timeline extent
//        of the X axis).  May be null (a default 4-bar span is used).
//      * a track index  -- the PatchKnob output "bus"/track this lane set belongs to
//        (== MixerGraph track == audio_app track == AutomationPlayer track).
//      * an engine::AutomationPlayer* -- the OWNER of the automation data model.
//        The view edits player->track(track).lane(l) breakpoints in place; the
//        shell drives playback with player->advance() (see the hook below).
//
//  It renders one horizontal LANE per AutomationLane on that track: a themed
//  breakpoint envelope (line + draggable points).  A lane targets either a
//  hosted VST parameter (picked from audio_app_track_param_count/info) or a MIDI
//  CC.  Editing:
//      * left-click empty plot     -> add a breakpoint (then drag it)
//      * left-drag a point         -> move it (tick + value)
//      * right-click a point       -> delete it
//      * click a lane gutter       -> select it (interp button acts on it)
//      * the gutter [x]            -> remove the whole lane
//  Toolbar builds the "next lane" target: [PARAM|CC] < name/number > + Lane, and
//  cycles the selected lane's interpolation LIN/STEP/HOLD.
//
//  ------------------------------------------------------------------------
//  PLAYBACK HOOK (shell side, once per processed transport window):
//  ------------------------------------------------------------------------
//      player->advance(fromTick, toTick,
//          // EmitParam -> hosted VST parameter change on this track
//          [](int trk, unsigned id, float v){
//              PatchKnob::app::audio_app_route_param(trk, id, v);
//          },
//          // EmitCC -> a MIDI control-change on this track's output
//          [](int trk, int cc, int v127){
//              PatchKnob::app::audio_app_route_midi(
//                  trk, 0xB0 | (chan & 0x0F), (unsigned char)cc, (unsigned char)v127);
//          });
//  On locate / play-start call player->emitAt(startTick, ...) with the same two
//  callbacks so a breakpoint sitting exactly on the start tick is not skipped.
//
//  Mounting (shell side):
//      automation::AutomationView av(seq, track, player);   // player owns data
//      app.roots.push_back(&av);
//      app.on_layout = [&](ui::App& a){ av.rect = { x, y, w, h }; };
//      // call av.refresh_params() after a track's instrument changes.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_AUTOMATION_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_AUTOMATION_VIEW_H

#include "gui.h"
#include <string>
#include <vector>

class sequence;                                     // PatchKnob engine core
class perform;                                      // PatchKnob engine core
namespace PatchKnob { namespace engine {
    class AutomationPlayer;
    class AutomationLane;
    class AutomationTrack;
}}

namespace automation {

// ===========================================================================
//  AutomationView -- the breakpoint-envelope lane editor.
// ===========================================================================
class AutomationView : public ui::Widget {
public:
    AutomationView(sequence* seq, int track,
                   PatchKnob::engine::AutomationPlayer* player);

    // Rebind to a (new) sequence + track index (keeps the same player).
    void set_target(sequence* seq, int track);
    void set_player(PatchKnob::engine::AutomationPlayer* player) { player_ = player; }

    int  track() const { return track_; }

    // (Re)read the VST-parameter picker list from the track's instrument via
    // audio_app_track_param_*.  Safe to call when no engine/instrument exists
    // (the list is just empty and only CC lanes can be added).
    void refresh_params();

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;

private:
    struct ParamRef { unsigned int id; std::string name; };

    // --- model access --------------------------------------------------------
    PatchKnob::engine::AutomationTrack* atrack() const;   // player_->track(track_) or null
    PatchKnob::engine::AutomationLane*  lane(int idx) const;
    int  lane_count() const;
    long seq_length();                                // >= 1

    // --- layout / mapping ----------------------------------------------------
    void     layout(ui::App& app);
    SDL_Rect gutter_rect(int laneRow) const;
    SDL_Rect plot_rect(int laneRow) const;
    int      x_at_tick(const SDL_Rect& plot, long tick, long len) const;
    long     tick_at_x(const SDL_Rect& plot, int x, long len) const;
    int      y_at_val(const SDL_Rect& plot, float v) const;
    float    val_at_y(const SDL_Rect& plot, int y) const;

    // --- editing -------------------------------------------------------------
    void add_lane_for_picker();
    void cycle_interp();
    int  pick_point(PatchKnob::engine::AutomationLane* L, const SDL_Rect& plot,
                    long len, int px, int py) const;   // nearest bp idx or -1

    // --- drawing helpers -----------------------------------------------------
    void draw_toolbar(ui::App& app);
    void draw_lane(ui::App& app, int row);
    bool draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                  bool active, bool enabled = true) const;
    std::string target_label(const PatchKnob::engine::AutomationLane* L) const;
    std::string picker_label() const;

    // --- model ---------------------------------------------------------------
    sequence*                        seq_    = nullptr;
    int                              track_  = 0;
    PatchKnob::engine::AutomationPlayer* player_ = nullptr;

    // --- toolbar / picker state ---------------------------------------------
    bool                  pickCC_   = false;   // false=VST param, true=MIDI CC
    int                   paramSel_ = 0;       // index into params_
    int                   ccSel_    = 74;      // MIDI CC number 0..127
    std::vector<ParamRef> params_;

    // --- interaction state ---------------------------------------------------
    int  selLane_  = -1;
    bool dragging_ = false;
    int  dragLane_ = -1, dragBp_ = -1;
    int  scrollY_  = 0;

    // --- cached geometry (recomputed each layout()) -------------------------
    SDL_Rect rType_{0,0,0,0}, rPrev_{0,0,0,0}, rName_{0,0,0,0},
             rNext_{0,0,0,0}, rAdd_{0,0,0,0}, rInterp_{0,0,0,0};
    int tbH_ = 26, laneH_ = 64, gutterW_ = 120;
};

// ===========================================================================
//  KeyFollowPanel -- per-track scale-master / scale-follow toggles + master
//  root/scale pickers, wired to perform / sequence scale-follow API.
// ===========================================================================
class KeyFollowPanel : public ui::Widget {
public:
    explicit KeyFollowPanel(perform* perf) : perf_(perf) {}

    void set_perform(perform* perf) { perf_ = perf; }
    void set_tracks(const std::vector<int>& seqs) { seqs_ = seqs; clamp_focus(); }
    void add_track(int seq) { seqs_.push_back(seq); clamp_focus(); }
    void set_focus(int seqNum);                       // seq the pickers act on

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;

private:
    void        clamp_focus();
    sequence*   seq_of(int seqNum) const;             // active seq* or null
    bool        draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                         bool active, bool enabled = true) const;

    perform*         perf_  = nullptr;
    std::vector<int> seqs_;
    int              focus_ = 0;                       // seq number (not row idx)

    SDL_Rect rKeyPrev_{0,0,0,0}, rKeyNext_{0,0,0,0};
    SDL_Rect rScalePrev_{0,0,0,0}, rScaleNext_{0,0,0,0};
    struct RowRect { SDL_Rect row, sm, fm; int seq; };
    std::vector<RowRect> rows_;
    int headH_ = 26, pickH_ = 26, rowH_ = 24;
};

} // namespace automation

#endif // PATCHKNOB_SDLUI_VIEWS_AUTOMATION_VIEW_H
