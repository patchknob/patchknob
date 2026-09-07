//----------------------------------------------------------------------------
//  sdlui/views/automation/automation_view.h -- SDL2 automation CLIP editor + a
//  small keyfollow (scale-master / scale-follow) panel for the PatchKnob.
//
//  AutomationView is a single retained ui::Widget drawn over:
//      * a `sequence*`  -- the pattern length (timeline extent of the X axis),
//        its time signature, and THE CLIP'S LOOP WINDOW: [loop_start,loop_end)
//        plus loop_enabled are owned there and merely mirrored onto the region
//        this view plays through (see loop_length() in the .cpp -- there is one
//        loop per clip, not one per view).  May be null (a default 4-bar span
//        at 4/4 is used).
//      * a track index  -- the PatchKnob output "bus"/track this lane set belongs to
//        (== MixerGraph track == audio_app track == AutomationPlayer track).
//      * an engine::AutomationPlayer* -- the OWNER of the automation data model.
//        The view edits player->track(track).lane(l) breakpoints in place; the
//        shell drives playback with player->advance() (see the hook below).
//
//  ------------------------------------------------------------------------
//  LAYOUT (geometry is computed ONCE per frame in layout() and shared by the
//  painter and every hit test -- a draw/click mismatch in the picker popup was
//  a real bug here, so nothing recomputes a rect on its own):
//
//      +-----------------------------------------------------------+
//      | toolbar                                                   |  tbH_
//      +----------+------------------------------------------------+
//      | (corner) | bar/beat RULER  (clip-local or song bars)       |  rulerH_
//      +----------+------------------------------------------------+
//      | gutter 0 | plot 0                                          |
//      | gutter 1 | plot 1                                          |  lanes
//      +----------+------------------------------------------------+
//      | horizontal scrollbar + clip overview                      |  sbH_
//      +-----------------------------------------------------------+
//
//  ------------------------------------------------------------------------
//  SELECTION MODEL
//  ------------------------------------------------------------------------
//  A range selection is {set of lanes} x {tick range}:
//      * drag in the RULER  -> the range across ALL lanes
//      * drag in a LANE     -> that lane only
//      * Shift              -> extend the tick range
//      * Ctrl               -> add / remove a lane from the set
//  Processes (automation/automation_ops.h) run over exactly that rectangle.
//  A separate POINT selection (marquee / box select) exists for breakpoint
//  editing: nudging, numeric entry, copy / paste and drag-all.
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
//      av.on_playhead = [&]{ return transport_tick(); };   // optional
//      // call av.refresh_params() after a track's instrument changes.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_AUTOMATION_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_AUTOMATION_VIEW_H

#include "gui.h"
#include "engine/automation/automation_lane.h"
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
    void set_region(sequence* seq, int regionId, int destinationTrack);

    int  track() const { return track_; }
    int  region_id() const { return regionId_; }

    struct TargetChoice {
        PatchKnob::engine::LaneTarget target;
        std::string node, device, parameter;
    };
    std::function<std::vector<TargetChoice>()> on_list_targets;

    //! Absolute project tick of the transport, for the playhead + follow mode.
    //! Optional: without it the view falls back to the sequence's own tick.
    std::function<int64_t()> on_playhead;

    // (Re)read the VST-parameter picker list from the track's instrument via
    // audio_app_track_param_*.  Safe to call when no engine/instrument exists
    // (the list is just empty and only CC lanes can be added).
    void refresh_params();

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;
    void cancel_interaction(ui::App& app) override;

private:
    struct ParamRef { unsigned int id; std::string name; };

    // -----------------------------------------------------------------------
    //  per-lane VIEW state.  The engine model (AutomationTrack) carries no
    //  name / colour / mute / zoom, and this view does not own that header, so
    //  the presentation state lives here, parallel to the lane vector and
    //  permuted with it by every reorder / duplicate / remove.
    // -----------------------------------------------------------------------
    struct LaneUI {
        std::string name;              // user rename ("" -> derived target label)
        int         colour  = 0;       // index into the theme-role lane palette
        bool        mute    = false;   // lane silenced (drawn struck through)
        bool        solo    = false;   // only soloed lanes read as live
        bool        bypass  = false;   // curve kept but not driven
        float       vzoom   = 1.0f;    // value-axis magnification (1..16)
        float       vcenter = 0.5f;    // value at the vertical centre of the plot
        int         height  = 0;       // 0 -> the shared default row height
    };

    //! {set of lanes} x {tick range}.  `lanes` is parallel to the lane vector.
    struct Selection {
        bool              active = false;
        long              t0 = 0, t1 = 0;      // t0 <= t1, half-open in spirit
        std::vector<char> lanes;
        bool has_lane(int i) const {
            return i >= 0 && i < (int)lanes.size() && lanes[(size_t)i] != 0;
        }
        int  lane_count() const {
            int n = 0; for (char c : lanes) if (c) ++n; return n;
        }
        void clear() { active = false; t0 = t1 = 0; lanes.assign(lanes.size(), 0); }
    };

    //! A breakpoint identified by (lane, tick) -- indices shift under editing,
    //! ticks do not, so the point selection survives an insert or a sort.
    struct PointRef { int lane; int64_t tick; };

    //! One undo step: the whole lane set plus its presentation state.
    struct Snapshot {
        std::vector<PatchKnob::engine::AutomationLane> lanes;
        std::vector<LaneUI>                           ui;
    };

    //! Adaptive ruler / grid density for the current zoom (see metric()).
    struct Metric { long barStep = 0, labelStep = 0, beatStep = 0, subStep = 0; };

    //! Every interaction that owns the pointer until release.  A press is a
    //! press only while this is None -- the toolkit re-delivers held motion as
    //! another pressed event, so without the latch a click would repeat.
    enum class Drag {
        None, Consumed, Point, Points, Curve, Loop, HScroll,
        RulerRange, LaneRange, Marquee, LaneReorder, LaneResize, VPan, PromptField
    };

    //! Which context menu is open.  All of them share one geometry + hit test.
    enum MenuKind { MenuNone, MenuToolbar, MenuGutter, MenuPlot, MenuRuler, MenuOps };

    struct MenuRow {
        std::string label;
        int  id        = -1;
        bool enabled   = true;
        bool header    = false;   // group caption, not clickable
        bool separator = false;
        bool check     = false;   // draws a tick box
        bool on        = false;
    };

    //! Op parameters, held as doubles and marshalled into autoops::Params at
    //! the single call site so the view never depends on the engine's field
    //! types.  Names match automation_ops.h's Params one for one.
    struct OpParams {
        double amount = 0.5, pivot = 0.5, grid = 48.0, steps = 8.0;
        double freqHz = 1.0, syncCycles = 1.0, shape = 0.0, phase = 0.0;
        double lo = 0.0, hi = 1.0, seed = 1.0;
    };

    //! One adjustable row in the op parameter prompt.  Draggable readout with
    //! click-to-type; geometry comes from prompt_box() so draw and hit agree.
    struct PromptField {
        const char* label = "";
        double*     value = nullptr;
        double      lo = 0.0, hi = 1.0, step = 0.01;
        bool        integer = false;
        bool        ticks   = false;   // format as bars.beats as well
    };

    // --- model access --------------------------------------------------------
    PatchKnob::engine::AutomationTrack* atrack() const;   // player_->track(track_) or null
    PatchKnob::engine::AutomationLane*  lane(int idx) const;
    int  lane_count() const;
    long seq_length();                                // >= 1
    long measure_len() const;                         // ticks per bar
    long beat_len() const;                            // ticks per beat
    long clip_origin() const;                         // song tick of clip start
    long playhead_local() const;                      // clip-local transport tick
    void sync_lane_ui();                              // keep laneUI_ parallel

    // --- layout / mapping ----------------------------------------------------
    void     layout(ui::App& app);
    int      lane_h(int row) const;
    int      lanes_top() const;
    int      lanes_bottom() const;
    int      content_h() const;
    SDL_Rect ruler_rect() const;
    SDL_Rect scrollbar_rect() const;
    SDL_Rect gutter_rect(int laneRow) const;
    SDL_Rect plot_rect(int laneRow) const;
    //! Every clickable box inside one lane gutter, computed ONCE and used by
    //! both the painter and the hit test.
    struct GutterGeo {
        SDL_Rect handle{0,0,0,0}, name{0,0,0,0}, mute{0,0,0,0}, solo{0,0,0,0},
                 byp{0,0,0,0}, colour{0,0,0,0}, vzUp{0,0,0,0}, vzDn{0,0,0,0},
                 del{0,0,0,0}, resize{0,0,0,0};
    };
    GutterGeo gutter_geo(ui::App& app, int laneRow) const;
    int      row_at_y(int y) const;                   // lane row under y, or -1
    int      x_at_tick(const SDL_Rect& plot, long tick, long len) const;
    long     tick_at_x(const SDL_Rect& plot, int x, long len) const;
    int      y_at_val(const SDL_Rect& plot, int row, float v) const;
    float    val_at_y(const SDL_Rect& plot, int row, int y) const;
    long     visible_ticks(long len) const;
    long     max_view_start(long len) const;
    void     clamp_view(long len);
    // --- the CLIP's loop window (owned by `sequence`, mirrored by the region;
    //     see the long comment over loop_length() in the .cpp) ---------------
    bool     clip_loop_on() const;      // sequence::get_loop_enabled()
    long     clip_loop_start() const;   // window start, clip-local ticks
    long     clip_loop_end() const;     // window end == where playback wraps
    int64_t  loop_length() const;       // == clip_loop_end(), the wrap point
    void     set_loop_length(int64_t ticks);       // moves the window's END
    void     set_loop_start_ticks(long ticks);     // moves the window's START
    void     sync_region_loop();        // push the window onto Region::loopLength
    //! One-shot latch for sync_region_loop()'s legacy-endpoint migration;
    //! cleared by set_target()/set_region() so each bind migrates once.
    bool     loopAdopted_ = false;
    Metric   metric(ui::App& app, const SDL_Rect& plot, long len) const;
    std::string bbt(long localTick) const;            // bar.beat.tick readout
    std::string bars_len(long ticks) const;           // a DURATION in bars.beats

    // --- zoom / scroll -------------------------------------------------------
    void zoom_to_fit();
    void zoom_to_selection();
    void zoom_about(double factor, int anchorX);
    void scroll_by(long ticks);

    // --- editing -------------------------------------------------------------
    void add_lane_for_picker();
    void apply_picker_to_selected();
    void sync_picker_to_lane(int row);
    void refresh_targets_preserving_selection();
    void cycle_interp();
    long snap_tick(long tick) const;
    int  pick_point(PatchKnob::engine::AutomationLane* L, const SDL_Rect& plot,
                    int row, long len, int px, int py) const;   // nearest bp or -1
    void reorder_lane(int from, int to);
    void duplicate_lane(int row);
    void remove_lane(int row);
    void begin_rename(ui::App& app, int row);
    void begin_point_entry(ui::App& app);

    // --- selection -----------------------------------------------------------
    void select_range(long t0, long t1, bool allLanes, int oneLane);
    void select_all_lanes();
    bool point_selected(int lane, int64_t tick) const;
    void toggle_point(int lane, int64_t tick, bool additive);
    void clear_points();
    //! How a finished marquee combines with the existing point selection.
    //! The marquee used to ALWAYS add: it read the live Ctrl state on release,
    //! and Ctrl is what starts a marquee in the first place, so `additive` was
    //! true every single time -- there was no way to replace or subtract.  The
    //! mode is latched at the press instead (see MarqueeMode / marqueeMode_).
    enum MarqueeMode { MarqueeReplace = 0, MarqueeAdd, MarqueeSubtract };
    void points_in_box(const SDL_Rect& box, int mode);
    void nudge_points(long dTick, float dVal);
    void delete_points();
    //! Clear every breakpoint inside the range selection (the primary model).
    //! Plain Delete used to fall through to remove_lane() whenever selPts_ was
    //! empty -- and a range selection never fills selPts_ -- so "select bars
    //! 5..9 and press Delete" destroyed the whole lane, its target binding and
    //! its colour instead of clearing those bars.
    void delete_range();
    void copy_points();
    void paste_points();

    // --- undo ----------------------------------------------------------------
    Snapshot capture() const;
    void     restore(const Snapshot& s);
    void     push_undo();
    void     undo();
    void     redo();

    // --- processes -----------------------------------------------------------
    void run_op(ui::App& app, int opIndex);
    int  prompt_fields(int opIndex, PromptField* out);   // count (max 6)
    void open_prompt(ui::App& app, int opIndex);
    SDL_Rect prompt_box(ui::App& app, int& rowh, int& n);
    void draw_prompt(ui::App& app);
    bool prompt_click(ui::App& app, int x, int y);

    // --- menus ---------------------------------------------------------------
    void open_menu(ui::App& app, MenuKind kind, int x, int y, int row, long tick);
    void build_menu(MenuKind kind, int row);
    SDL_Rect menu_box(ui::App& app, int& rowh, int& rows) const;
    int  menu_row_at(ui::App& app, int x, int y) const;
    void draw_menu(ui::App& app);
    bool menu_click(ui::App& app, int x, int y);
    void do_menu(ui::App& app, int id);
    void close_menus();
    bool any_popup_open() const;

    // --- drawing helpers -----------------------------------------------------
    void draw_toolbar(ui::App& app);
    void draw_ruler(ui::App& app);
    void draw_scrollbar(ui::App& app);
    void draw_lane(ui::App& app, int row);
    void draw_lane_gutter(ui::App& app, int row);
    void draw_empty_state(ui::App& app);
    void draw_cursor_readout(ui::App& app);
    void draw_status(ui::App& app);
    bool draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                  bool active, bool enabled = true) const;
    ui::Color lane_colour(int row) const;
    std::string lane_name(int row) const;
    std::string target_label(const PatchKnob::engine::AutomationLane* L) const;
    std::string picker_label() const;
    std::string picker_part(int level) const;
    void open_picker_level(int level);
    void draw_picker_popup(ui::App& app);
    // ONE definition of the popup's row geometry, shared by the painter and the
    // hit-test so a click always lands on the row under the cursor.
    static constexpr int kPopupMaxRows = 18;
    int  popup_row_h(ui::App& app) const;
    int  popup_rows_shown(ui::App& app) const;
    int  popup_row_at(ui::App& app, int y) const;
    int  popMx_ = -1, popMy_ = -1;      // polled pointer, for hover feedback

    // --- model ---------------------------------------------------------------
    sequence*                        seq_    = nullptr;
    int                              track_  = 0;
    int                              regionId_ = -1;
    PatchKnob::engine::AutomationPlayer* player_ = nullptr;

    // --- toolbar / picker state ---------------------------------------------
    bool                  pickCC_   = false;   // false=VST param, true=MIDI CC
    int                   paramSel_ = 0;       // index into params_
    int                   ccSel_    = 74;      // MIDI CC number 0..127
    std::vector<ParamRef> params_;
    std::vector<TargetChoice> targets_;
    int targetSel_ = 0;
    int popupLevel_ = -1;
    int popupScroll_ = 0;
    int paramPageStart_ = -1;
    SDL_Rect popupRect_{0,0,0,0};
    std::vector<std::string> popupItems_;
    long snapTicks_ = 48;             // default 1/16 at 192 PPQN
    bool snapOn_    = true;
    bool localBars_ = true;           // ruler counts the CLIP's bars, not the song's
    bool follow_    = true;           // keep the playhead in view during playback

    // --- interaction state ---------------------------------------------------
    int   selLane_  = -1;
    Drag  drag_     = Drag::None;
    int   dragLane_ = -1, dragBp_ = -1;
    int   dragLoopEdge_ = 1;          // Drag::Loop: 0 = window start, 1 = end
    int   curveLane_ = -1, curveSeg_ = -1;
    int   curveStartY_ = 0;
    float curveStart_ = 0.f;
    int   dragStartX_ = 0, dragStartY_ = 0;
    long  dragAnchorTick_ = 0;
    long  dragLastTick_ = 0;
    float dragLastVal_ = 0.f;
    int   reorderFrom_ = -1, reorderTo_ = -1;
    int   resizeRow_ = -1, resizeStartH_ = 0;
    int   scrollGrab_ = 0;
    SDL_Rect marquee_{0,0,0,0};
    int   marqueeMode_ = MarqueeReplace;   // latched at the press, not read on release
    int   scrollY_  = 0;
    double zoomX_ = 1.0;
    long  viewStartTick_ = 0;
    int   mx_ = -1, my_ = -1;         // polled pointer (hover + readout)

    // --- selection -----------------------------------------------------------
    Selection             sel_;
    std::vector<PointRef> selPts_;
    std::vector<PatchKnob::engine::Breakpoint> clip_;   // copy buffer
    long                  clipSpan_ = 0;

    // --- undo ----------------------------------------------------------------
    std::vector<Snapshot> undo_, redo_;
    static constexpr int  kUndoDepth = 64;

    // --- per-lane presentation ----------------------------------------------
    std::vector<LaneUI>   laneUI_;

    // --- menus / prompt ------------------------------------------------------
    MenuKind             menuKind_ = MenuNone;
    std::vector<MenuRow> menuRows_;
    int                  menuX_ = 0, menuY_ = 0, menuScroll_ = 0;
    int                  menuRow_ = -1;
    long                 menuTick_ = 0;
    bool                 promptOpen_ = false;
    int                  promptOp_   = -1;
    int                  promptField_ = -1;
    int                  promptDragX_ = 0;
    double               promptDragStart_ = 0.0;
    SDL_Rect             promptRect_{0,0,0,0};
    SDL_Rect             promptApply_{0,0,0,0}, promptCancel_{0,0,0,0};
    OpParams             opp_;

    // --- inline text entry ---------------------------------------------------
    enum EditWhat { EditNone, EditLaneName, EditPointValue, EditPointTick, EditPromptField };
    EditWhat    editWhat_ = EditNone;
    int         editRow_  = -1;
    int         editIdx_  = -1;
    std::string editBuf_;

    // --- cached geometry (recomputed once per layout()) ----------------------
    SDL_Rect rAdd_{0,0,0,0}, rRemove_{0,0,0,0}, rDup_{0,0,0,0}, rInterp_{0,0,0,0},
             rSnap_{0,0,0,0}, rSnapOn_{0,0,0,0}, rFit_{0,0,0,0}, rFitSel_{0,0,0,0},
             rFollow_{0,0,0,0}, rBars_{0,0,0,0}, rFx_{0,0,0,0}, rUndo_{0,0,0,0},
             rRedo_{0,0,0,0};
    SDL_Rect rPrev_{0,0,0,0}, rNext_{0,0,0,0}, rName_{0,0,0,0},
             rDevice_{0,0,0,0}, rParam_{0,0,0,0}, rType_{0,0,0,0};
    SDL_Rect scrollTrack_{0,0,0,0}, scrollThumb_{0,0,0,0};
    std::vector<int> laneTop_;        // y of each lane row (pre-scroll), size n+1
    int tbH_ = 26, laneH_ = 64, gutterW_ = 150, rulerH_ = 22, sbH_ = 16;
};

// ===========================================================================
//  KeyFollowPanel -- per-track scale-master / scale-follow toggles + master
//  root/scale pickers, wired to perform / sequence scale-follow API.
// ===========================================================================
class KeyFollowPanel : public ui::Widget {
public:
    explicit KeyFollowPanel(perform* perf) : perf_(perf) {}

    void set_tracks(const std::vector<int>& seqs) { seqs_ = seqs; clamp_focus(); }
    void add_track(int seq) { seqs_.push_back(seq); clamp_focus(); }

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
    //! Seq number the Key/Scl pickers write to: the scale MASTER when there is
    //! one (the only key/scale the engine reads), else the focused row.
    //! Resolved in draw(), used by on_mouse -- both must agree.
    int              pickSeq_ = 0;

    SDL_Rect rKeyPrev_{0,0,0,0}, rKeyNext_{0,0,0,0};
    SDL_Rect rScalePrev_{0,0,0,0}, rScaleNext_{0,0,0,0};
    struct RowRect { SDL_Rect row, sm, fm; int seq; };
    std::vector<RowRect> rows_;
    int headH_ = 26, pickH_ = 26, rowH_ = 24;
};

} // namespace automation

#endif // PATCHKNOB_SDLUI_VIEWS_AUTOMATION_VIEW_H
