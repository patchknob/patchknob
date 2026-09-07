//----------------------------------------------------------------------------
//  sdlui/views/automation/automation_view.cpp -- see automation_view.h.
//----------------------------------------------------------------------------
#include "automation_view.h"

#include "sequence.h"          // sequence + c_ppqn + c_scale_* + c_key_text
#include "perform.h"           // scale-master / scale-follow API
#include "audio_app.h"         // VST param introspection
#include "automation/automation_player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

//----------------------------------------------------------------------------
//  PROCESS ENGINE BINDING
//
//  The range processes live in engine/automation/automation_ops.h, owned by the
//  engine module.  This view drives them and nothing else: the whole dependency
//  is one include, one type alias, and one call site (run_op()), so an
//  engine-side change is a one-function fix here rather than a sweep.
//----------------------------------------------------------------------------
#include "automation/automation_ops.h"

namespace ao = PatchKnob::engine::autoops;

//! The op enum's type, without this file having to name how the engine spells
//! it -- the view addresses ops by index and converts through kOpEnum[].
using AoOp = decltype(ao::Quantise);


using namespace ui;
using PatchKnob::engine::AutomationPlayer;
using PatchKnob::engine::AutomationTrack;
using PatchKnob::engine::AutomationLane;
using PatchKnob::engine::Breakpoint;
using PatchKnob::engine::LaneTarget;
using PatchKnob::engine::LaneTargetKind;
using PatchKnob::engine::Interpolation;

namespace automation {
static constexpr int kParameterPageSize = 16;

// ---------------------------------------------------------------------------
//  the op table -- ONE ordering, used by the menu, the parameter prompt and
//  apply_ops().  The view addresses ops by index so the header never has to
//  name the engine's enum type.
// ---------------------------------------------------------------------------
enum OpIndex {
    OP_QUANTISE = 0, OP_HUMANISE, OP_STRETCH, OP_REVERSE, OP_SHIFTTIME,
    OP_DUPRANGE, OP_INSERTTIME, OP_DELETETIME,
    OP_SCALEVAL, OP_OFFSETVAL, OP_INVERTVAL, OP_NORMALISE, OP_CLAMPVAL,
    OP_SMOOTH, OP_THIN, OP_DENSIFY,
    OP_LFOFILL, OP_RAMPFILL, OP_SCURVEFILL, OP_BENDCURVE, OP_STEPQUANT,
    OP_RANDWALK, OP_NOISEFILL,
    OP_COPYSHAPE, OP_MIRROR, OP_PHASEOFF, OP_AVERAGE,
    OP_COUNT
};

static const AoOp kOpEnum[OP_COUNT] = {
    ao::Quantise, ao::Humanise, ao::Stretch, ao::Reverse, ao::ShiftTime,
    ao::DuplicateRange, ao::InsertTime, ao::DeleteTime,
    ao::ScaleValue, ao::OffsetValue, ao::InvertValue, ao::Normalise, ao::ClampValue,
    ao::Smooth, ao::Thin, ao::Densify,
    ao::LfoFill, ao::RampFill, ao::SCurveFill, ao::BendCurve, ao::StepQuantise,
    ao::RandomWalk, ao::NoiseFill,
    ao::CopyShape, ao::MirrorLanes, ao::PhaseOffsetLanes, ao::AverageLanes
};

// Group boundaries (first index of each group) + captions.
static const int         kGroupFirst[5] = { OP_QUANTISE, OP_SCALEVAL, OP_LFOFILL,
                                            OP_COPYSHAPE, OP_COUNT };
static const char* const kGroupName[4]  = { "TIME", "VALUE", "SHAPE", "MULTI-LANE" };

static bool op_is_multi(int i) {
    return (i >= 0 && i < OP_COUNT) ? ao::opIsMultiLane(kOpEnum[i]) : false;
}
static std::string op_label(int i) {
    if (i < 0 || i >= OP_COUNT) return std::string();
    const char* n = ao::opName(kOpEnum[i]);
    return n ? std::string(n) : std::string("op");
}

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------
static inline int   clampi(int v, int lo, int hi) { return v<lo?lo:(v>hi?hi:v); }
static inline float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v);}
static inline long  clampl(long v, long lo, long hi){ return v<lo?lo:(v>hi?hi:v); }
static inline double clampd(double v,double lo,double hi){return v<lo?lo:(v>hi?hi:v);}

//! Measured ellipsis.  A byte count times a nominal advance overflows every box
//! at any non-1.0 UI scale, so nothing in this file counts characters.
static std::string fit_text(const ui::Font& font, std::string s, int maxw) {
    if (maxw <= 0) return std::string();
    if (font.text_w(s) <= maxw) return s;
    const std::string ell = "...";
    if (font.text_w(ell) > maxw) {
        std::string d = ell;
        while (!d.empty() && font.text_w(d) > maxw) d.pop_back();
        return d;
    }
    while (!s.empty() && font.text_w(s + ell) > maxw) s.pop_back();
    return s.empty() ? ell : s + ell;
}

//! Alpha fill that always hands the renderer back in BLENDMODE_NONE.
static void fill_alpha(SDL_Renderer* r, const SDL_Rect& q, Color c, Uint8 a) {
    if (q.w <= 0 || q.h <= 0) return;
    c.a = a;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    fill_rect(r, q, c);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

//! Filled rounded rectangle (scanline with circular corners) -- toolbar chips
//! and the scrollbar thumb.
static void fill_round(SDL_Renderer* r, SDL_Rect q, int rad, Color c) {
    if (q.w <= 0 || q.h <= 0) return;
    if (rad * 2 > q.w) rad = q.w / 2;
    if (rad * 2 > q.h) rad = q.h / 2;
    if (rad < 1) { fill_rect(r, q, c); return; }
    set_color(r, c);
    for (int yy = 0; yy < q.h; ++yy) {
        int dx = 0, dy = -1;
        if (yy < rad)             dy = rad - 1 - yy;
        else if (yy >= q.h - rad) dy = yy - (q.h - rad);
        if (dy >= 0) dx = rad - (int)std::floor(std::sqrt((double)(rad*rad - dy*dy)));
        SDL_Rect ln{ q.x + dx, q.y + yy, q.w - 2*dx, 1 };
        SDL_RenderFillRect(r, &ln);
    }
}

static SDL_Rect clamp_popup(SDL_Rect box, const SDL_Rect& bounds) {
    if (box.w > bounds.w) box.w = bounds.w;
    if (box.h > bounds.h) box.h = bounds.h;
    if (box.x + box.w > bounds.x + bounds.w) box.x = bounds.x + bounds.w - box.w;
    if (box.y + box.h > bounds.y + bounds.h) box.y = bounds.y + bounds.h - box.h;
    if (box.x < bounds.x) box.x = bounds.x;
    if (box.y < bounds.y) box.y = bounds.y;
    return box;
}

static bool in_rect(const SDL_Rect& r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

//  Smallest power-of-two multiplier n making n units at least minPx wide.  The
//  ruler and the grid are both thinned with this, so a division either has room
//  for itself (and its label) or is not drawn at all.
static long pow2_stride(double unitPx, double minPx) {
    if (unitPx <= 0.0) return 1L << 28;
    long n = 1;
    while ((double)n * unitPx < minPx && n < (1L << 27)) n <<= 1;
    return n;
}

static const char* interp_name(Interpolation i) {
    switch (i) {
        case Interpolation::Step: return "STEP";
        case Interpolation::Hold: return "HOLD";
        case Interpolation::Linear: default: return "LIN";
    }
}

//  Per-lane colour.  The themes are strictly two-tone, so a "lane colour" is a
//  choice of THEME ROLE -- every entry below is legible against keybg in BOTH
//  Light and Midnight (t.scale / t.chord are deliberately absent: they are
//  near-white on Light and near-black on Midnight, i.e. invisible in a plot).
static const int kLaneTones = 6;
static Color lane_tone(int i) {
    const Theme& t = theme();
    switch (((i % kLaneTones) + kLaneTones) % kLaneTones) {
        case 1:  return t.note;
        case 2:  return t.hi;
        case 3:  return t.notesel;
        case 4:  return t.active;
        case 5:  return t.dim;
        default: return t.accent;
    }
}
static const char* lane_tone_name(int i) {
    static const char* n[kLaneTones] = { "Accent", "Note", "Bright", "Pale",
                                         "Active", "Muted" };
    return n[((i % kLaneTones) + kLaneTones) % kLaneTones];
}

// ===========================================================================
//  AutomationView -- construction / rebinding
// ===========================================================================
AutomationView::AutomationView(sequence* seq, int track, AutomationPlayer* player)
    : seq_(seq), track_(track), player_(player) {
    refresh_params();
}

void AutomationView::set_target(sequence* seq, int track) {
    seq_ = seq; track_ = track; regionId_ = -1;
    loopAdopted_ = false;   // re-read the clip's loop window on the new bind
    selLane_ = -1; drag_ = Drag::None; dragLane_ = dragBp_ = -1; scrollY_ = 0;
    zoomX_ = 1.0; viewStartTick_ = 0;
    sel_.lanes.clear(); sel_.clear(); selPts_.clear();
    laneUI_.clear(); undo_.clear(); redo_.clear(); close_menus();
    refresh_params();
}

void AutomationView::set_region(sequence* seq, int regionId, int destinationTrack) {
    seq_ = seq; regionId_ = regionId; track_ = destinationTrack;
    loopAdopted_ = false;   // re-read the clip's loop window on the new bind
    selLane_ = -1; drag_ = Drag::None;
    dragLane_ = dragBp_ = curveLane_ = curveSeg_ = -1; scrollY_ = 0;
    zoomX_ = 1.0; viewStartTick_ = 0;
    sel_.lanes.clear(); sel_.clear(); selPts_.clear();
    laneUI_.clear(); undo_.clear(); redo_.clear(); close_menus();
    refresh_params();
}

void AutomationView::refresh_params() {
    params_.clear();
    const int n = PatchKnob::app::audio_app_track_param_count(track_);
    for (int i = 0; i < n; ++i) {
        unsigned int id = 0; float def = 0.0f; char nm[64] = {0};
        if (PatchKnob::app::audio_app_track_param_info(track_, i, &id, nm, (int)sizeof(nm), &def))
            params_.push_back(ParamRef{ id, std::string(nm) });
    }
    if (paramSel_ >= (int)params_.size()) paramSel_ = 0;
    targets_ = on_list_targets ? on_list_targets() : std::vector<TargetChoice>();
    if (targetSel_ >= (int)targets_.size()) targetSel_ = 0;
    sync_lane_ui();
}

void AutomationView::cancel_interaction(ui::App& app) {
    drag_ = Drag::None; dragLane_ = dragBp_ = -1;
    reorderFrom_ = reorderTo_ = -1; resizeRow_ = -1; promptField_ = -1;
    close_menus();
    app.request_redraw();
}

// --- model access ----------------------------------------------------------
AutomationTrack* AutomationView::atrack() const {
    if (!player_) return nullptr;
    if (regionId_ >= 0) {
        AutomationPlayer::Region* r = player_->findRegion(regionId_);
        return r ? &r->automation : nullptr;
    }
    if (track_ < 0 || track_ >= player_->trackCount()) return nullptr;
    return &player_->track(track_);
}
AutomationLane* AutomationView::lane(int idx) const {
    AutomationTrack* at = atrack();
    if (!at || idx < 0 || idx >= at->laneCount()) return nullptr;
    return &at->lane(idx);
}
int AutomationView::lane_count() const {
    AutomationTrack* at = atrack();
    return at ? at->laneCount() : 0;
}
long AutomationView::seq_length() {
    if (player_ && regionId_ >= 0)
        if (const AutomationPlayer::Region* r = player_->findRegion(regionId_))
            return (long)std::max<int64_t>(1, r->length);
    long L = seq_ ? seq_->get_length() : 0;
    if (L < 1) L = 4L * 4 * c_ppqn;     // default 4 bars @ 4/4
    return L;
}
long AutomationView::beat_len() const {
    long bw = seq_ ? seq_->get_bw() : 4;
    if (bw < 1) bw = 4;
    return std::max<long>(1, (long)c_ppqn * 4 / bw);
}
long AutomationView::measure_len() const {
    long bpm = seq_ ? seq_->get_bpm() : 4;
    if (bpm < 1) bpm = 4;
    return std::max<long>(1, beat_len() * bpm);
}
long AutomationView::clip_origin() const {
    if (player_ && regionId_ >= 0)
        if (const AutomationPlayer::Region* r = player_->findRegion(regionId_))
            return (long)r->position;
    return 0;
}
long AutomationView::playhead_local() const {
    int64_t abs = 0;
    if (on_playhead)   abs = on_playhead();
    else if (seq_)     abs = (int64_t)seq_->get_last_tick();
    else               return -1;
    int64_t local = abs - (int64_t)clip_origin();
    if (local < 0) return (long)local;
    // Fold the way the clip itself repeats (sequence::play_span): into the loop
    // WINDOW when the clip loops, and not at all when it is one-shot.  Folding
    // at the pattern length regardless put the automation playhead somewhere the
    // notes were not -- on a looped clip it ran past the window and swept the
    // whole clip, and on a one-shot clip it wrapped round and round while the
    // material played once.
    const int64_t ls = clip_loop_start(), le = clip_loop_end();
    if (!clip_loop_on() || le <= ls) return (long)local;
    const int64_t period = le - ls;
    return (long)(ls + (((local - ls) % period) + period) % period);
}

// ===========================================================================
//  THE CLIP'S LOOP WINDOW -- one feature that used to be stored in two places.
//
//  `sequence` owns the real one: [get_loop_start(), get_loop_end()) plus
//  get_loop_enabled().  It is the grey band on the piano roll's ruler, and
//  sequence::play_span() repeats exactly that window -- or, with looping off,
//  plays the pattern ONCE from the clip's start.
//
//  AutomationPlayer::Region carries `loopLength` and folds every arranged tick
//  with a plain `local % loopLength` (automation_player.cpp:26,51).  That is
//  the SAME feature, not an independent one: the region IS the clip (Region::id
//  is the sequence number -- see main.cpp's on_open_editor / on_create_pattern),
//  so a curve that wraps somewhere other than where the notes wrap is simply
//  wrong.  loopLength was seeded once from sequence::get_length() and never
//  revisited, so putting a 1-bar loop on a 4-bar clip left the notes repeating
//  bar 1 while the automation kept sweeping all four -- they drifted apart on
//  every repetition -- and a one-shot clip's automation still wrapped forever.
//
//  The sequence is the authority.  The view reads the window from it, writes
//  edits back into it, and keeps the region's loopLength in step
//  (sync_region_loop(), called once per frame from draw()).
//
//  REMAINING ENGINE GAP -- do NOT "fix" it by reviving a second loop here:
//  Region has no `loopStart`, so its fold can only ever start at 0.  A window
//  that starts at 0 (the default, and what the end handle alone produces) is
//  exact.  A window dragged to start LATER wraps at the right point but replays
//  the curve from tick 0 instead of from loop_start.  That needs a
//  Region::loopStart in src/engine/automation/automation_player.{h,cpp} (fold
//  `loopStart + (local - loopStart) % (loopEnd - loopStart)`), plus main.cpp
//  pushing the window onto the region when the piano roll's on_loop_changed
//  fires -- this view can only keep it true while it is open.
// ===========================================================================
bool AutomationView::clip_loop_on() const {
    return seq_ ? seq_->get_loop_enabled() : true;
}

//! True only for a window the user actually placed: sequence treats
//! [0, length) as "no loop set" (see sequence::m_loop_start's comment).
static inline bool seq_window_set(sequence* s) {
    if (!s) return false;
    const long ls = s->get_loop_start(), le = s->get_loop_end();
    return le > ls && (ls > 0 || le < s->get_length());
}

long AutomationView::clip_loop_start() const {
    return seq_window_set(seq_) ? seq_->get_loop_start() : 0;
}

long AutomationView::clip_loop_end() const {
    if (seq_window_set(seq_)) return seq_->get_loop_end();
    if (seq_ && seq_->get_length() > 0) return seq_->get_length();
    if (player_ && regionId_ >= 0)
        if (const AutomationPlayer::Region* r = player_->findRegion(regionId_))
            return (long)std::max<int64_t>(1, r->loopLength);
    return 4L*4*c_ppqn;
}

int64_t AutomationView::loop_length() const {
    return std::max<int64_t>(1, clip_loop_end());
}

//! Mirror the window onto the region the engine actually plays.  With looping
//! OFF the fold has to become a no-op rather than a wrap, so the modulus is
//! pushed past the end of the arranged clip -- that is how a one-shot curve is
//! expressed in a model whose only control is a wrap point.
void AutomationView::sync_region_loop() {
    if (!player_ || regionId_ < 0 || !seq_) return;
    AutomationPlayer::Region* r = player_->findRegion(regionId_);
    if (!r) return;

    //  MIGRATION, once per bind.  Before the window moved into `sequence`, this
    //  view's "clip end" handle wrote loopLength and nothing else, and projects
    //  saved that value (project_io.cpp writes/reads Region::loopLength).  Just
    //  overwriting it from the sequence would throw away an endpoint the user
    //  had deliberately set and saved, so adopt it INTO the sequence instead --
    //  the one place the loop lives now -- and let everything below flow from
    //  there.  Guarded by a once-per-bind flag rather than by the state itself:
    //  otherwise clearing the window (Ctrl+Shift+L in the piano roll) would be
    //  undone by the next frame re-adopting the stale region value.
    if (!loopAdopted_) {
        loopAdopted_ = true;
        const long L = seq_->get_length();
        if (!seq_window_set(seq_) && seq_->get_loop_start() == 0 &&
            r->loopLength >= 1 && r->loopLength < L) {
            seq_->set_loop_end((long)r->loopLength);
            seq_->set_dirty();
        }
    }
    const int64_t wrap = std::max<int64_t>(1, clip_loop_end());
    const int64_t want = clip_loop_on()
        ? wrap
        : std::max<int64_t>(wrap, r->source + std::max<int64_t>(1, r->length));
    if (r->loopLength != want) r->loopLength = want;
}

//! Move the window's END (the wrap point).  Writes through to the sequence,
//! which is where the loop lives; the region follows via sync_region_loop().
void AutomationView::set_loop_length(int64_t ticks) {
    long t = (long)std::max<int64_t>(1, ticks);
    if (seq_) {
        if (t > seq_->get_length()) {
            //  The window may not reach past the END marker -- sequence clamps
            //  it there -- but an AUTOMATION clip's pattern length is pure
            //  bookkeeping (it holds no notes) while its curve is drawn across
            //  the whole arranged span, which can be longer after a resize in
            //  the arrange view.  Move the END for those rather than refuse a
            //  drag the user can see room for.  A clip that holds NOTES keeps
            //  its length: silently growing a pattern is not something this
            //  view should do behind the piano roll's back, so there the handle
            //  simply stops at the data end.
            if (seq_->get_track_kind() == 2) seq_->set_length(t, false);
            else                             t = seq_->get_length();
        }
        if (t <= seq_->get_loop_start())
            seq_->set_loop_start(std::max<long>(0, t - 1));
        seq_->set_loop_end(t);
        seq_->set_dirty();
    } else if (player_ && regionId_ >= 0) {
        if (AutomationPlayer::Region* r = player_->findRegion(regionId_))
            r->loopLength = t;                     // no sequence: legacy path
    }
    sync_region_loop();
}

//! Move the window's START.  Clamped inside the pattern and never past the end.
void AutomationView::set_loop_start_ticks(long ticks) {
    if (!seq_) return;
    long t = ticks < 0 ? 0 : ticks;
    if (t > seq_->get_length()) t = seq_->get_length();
    if (t >= seq_->get_loop_end())
        t = std::max<long>(0, seq_->get_loop_end() - 1);
    seq_->set_loop_start(t);
    seq_->set_dirty();
    sync_region_loop();
}

//! laneUI_ and the selection's lane set are parallel to the lane vector; every
//! path that changes the lane count comes through here.
void AutomationView::sync_lane_ui() {
    const int n = lane_count();
    while ((int)laneUI_.size() < n) {
        LaneUI u;
        u.colour = (int)laneUI_.size() % kLaneTones;
        laneUI_.push_back(u);
    }
    if ((int)laneUI_.size() > n) laneUI_.resize((size_t)n);
    if ((int)sel_.lanes.size() != n) sel_.lanes.resize((size_t)n, 0);
}

// --- layout / mapping ------------------------------------------------------
void AutomationView::layout(ui::App& app) {
    sync_lane_ui();
    const int ch = app.font.ch(), cw = app.font.cw();
    tbH_     = ch + 12;
    rulerH_  = ch + 10;
    sbH_     = std::max(14, ch + 2);
    gutterW_ = std::max(cw * 22, 168);
    if (gutterW_ > rect.w / 2) gutterW_ = std::max(60, rect.w / 2);

    // toolbar: measured widths so a fractional UI scale never clips a label.
    const int y = rect.y + 5, h = tbH_ - 10;
    int x = rect.x + 5;
    auto seg = [&](SDL_Rect& r, const std::string& label) {
        const int w = app.font.text_w(label) + 12;
        r = (x + w < rect.x + rect.w - 4) ? SDL_Rect{ x, y, w, h } : SDL_Rect{0,0,0,0};
        if (r.w) x += w + 3;
    };
    char snapBuf[32];
    const long denom = snapTicks_ > 0 ? (4L*c_ppqn)/snapTicks_ : 0;
    std::snprintf(snapBuf, sizeof(snapBuf), "1/%ld", denom > 0 ? denom : 1);
    AutomationLane* selL = lane(selLane_);

    seg(rAdd_,    "+");
    seg(rRemove_, "-");
    seg(rDup_,    "DUP");
    seg(rInterp_, selL ? interp_name(selL->interpolation()) : "LIN");
    seg(rSnapOn_, "SNAP");
    seg(rSnap_,   snapBuf);
    seg(rFit_,    "FIT");
    seg(rFitSel_, "FIT SEL");
    seg(rFollow_, "FOLLOW");
    seg(rFx_,     "PROCESS");
    seg(rUndo_,   "UNDO");
    seg(rRedo_,   "REDO");

    // lane rows: default height fits every lane when there are few, then a
    // fixed row with vertical scroll.  Per-lane overrides win.
    const int avail = std::max(40, lanes_bottom() - lanes_top());
    const int n = lane_count();
    laneH_ = (n <= 0) ? 72 : clampi(avail / n, 44, 140);

    laneTop_.assign((size_t)n + 1, 0);
    for (int i = 0; i < n; ++i) laneTop_[(size_t)i+1] = laneTop_[(size_t)i] + lane_h(i);

    const int viewH = std::max(1, lanes_bottom() - lanes_top());
    scrollY_ = clampi(scrollY_, 0, std::max(0, content_h() - viewH));
    clamp_view(seq_length());
}

int AutomationView::lane_h(int row) const {
    if (row >= 0 && row < (int)laneUI_.size() && laneUI_[(size_t)row].height > 0)
        return laneUI_[(size_t)row].height;
    return laneH_;
}
int AutomationView::lanes_top()    const { return rect.y + tbH_ + 2 + rulerH_; }
int AutomationView::lanes_bottom() const { return rect.y + rect.h - sbH_ - 1; }
int AutomationView::content_h()    const {
    return laneTop_.empty() ? 0 : laneTop_.back();
}

SDL_Rect AutomationView::ruler_rect() const {
    return SDL_Rect{ rect.x + gutterW_ + 2, rect.y + tbH_ + 2,
                     std::max(16, rect.w - gutterW_ - 6), rulerH_ };
}
SDL_Rect AutomationView::scrollbar_rect() const {
    return SDL_Rect{ rect.x + 1, rect.y + rect.h - sbH_, rect.w - 2, sbH_ - 1 };
}
SDL_Rect AutomationView::gutter_rect(int row) const {
    if (row < 0 || row + 1 >= (int)laneTop_.size()) return SDL_Rect{0,0,0,0};
    const int top = lanes_top() + laneTop_[(size_t)row] - scrollY_;
    return SDL_Rect{ rect.x, top, gutterW_, lane_h(row) - 2 };
}
SDL_Rect AutomationView::plot_rect(int row) const {
    if (row < 0 || row + 1 >= (int)laneTop_.size()) return SDL_Rect{0,0,0,0};
    const int top = lanes_top() + laneTop_[(size_t)row] - scrollY_;
    const SDL_Rect rl = ruler_rect();
    return SDL_Rect{ rl.x, top + 2, rl.w, lane_h(row) - 6 };
}
int AutomationView::row_at_y(int y) const {
    // laneTop_ is built by layout(); a hit test that arrives before the first
    // paint (or right after a lane was added) must not index past it.
    const int n = std::min(lane_count(), (int)laneTop_.size() - 1);
    if (n <= 0) return -1;
    const int rel = y - lanes_top() + scrollY_;
    if (rel < 0) return -1;
    for (int i = 0; i < n; ++i)
        if (rel >= laneTop_[(size_t)i] && rel < laneTop_[(size_t)i+1]) return i;
    return -1;
}

int AutomationView::x_at_tick(const SDL_Rect& p, long tick, long len) const {
    if (len < 1) len = 1;
    const long span = visible_ticks(len);
    const double f = (double)(tick - viewStartTick_) / (double)span;
    return p.x + (int)std::lround(f * (p.w - 1));
}
long AutomationView::tick_at_x(const SDL_Rect& p, int x, long len) const {
    if (p.w <= 1) return 0;
    const double f = (double)(x - p.x) / (double)(p.w - 1);
    return clampl(viewStartTick_ + (long)std::lround(f * visible_ticks(len)), 0, len);
}
long AutomationView::visible_ticks(long len) const {
    return std::max<long>(1, (long)std::lround((double)std::max<long>(1,len) / zoomX_));
}
long AutomationView::max_view_start(long len) const {
    return std::max<long>(0, len - visible_ticks(len));
}
void AutomationView::clamp_view(long len) {
    if (zoomX_ < 1.0) zoomX_ = 1.0;
    if (zoomX_ > 4096.0) zoomX_ = 4096.0;
    viewStartTick_ = clampl(viewStartTick_, 0, max_view_start(len));
}

//! Value axis with per-lane magnification: vcenter sits at the middle of the
//! plot and vzoom stretches around it.
int AutomationView::y_at_val(const SDL_Rect& p, int row, float v) const {
    float z = 1.f, c = 0.5f;
    if (row >= 0 && row < (int)laneUI_.size()) {
        z = laneUI_[(size_t)row].vzoom; c = laneUI_[(size_t)row].vcenter;
    }
    const float u = clampf(0.5f + (v - c) * z, -4.f, 5.f);
    return p.y + (int)std::lround((1.0f - u) * (p.h - 1));
}
float AutomationView::val_at_y(const SDL_Rect& p, int row, int y) const {
    if (p.h <= 1) return 0.0f;
    float z = 1.f, c = 0.5f;
    if (row >= 0 && row < (int)laneUI_.size()) {
        z = laneUI_[(size_t)row].vzoom; c = laneUI_[(size_t)row].vcenter;
    }
    if (z < 0.01f) z = 1.f;
    const float u = 1.0f - (float)(y - p.y) / (float)(p.h - 1);
    return clampf(c + (u - 0.5f) / z, 0.0f, 1.0f);
}

//  Bar lines / bar NUMBERS / beats / snap subdivisions for the current zoom.
//  Bar numbers use their OWN (coarser or equal) stride, computed from how wide
//  the widest visible number actually is, so zooming out drops them 1 -> 2 -> 4
//  -> 8 instead of overprinting, and zooming in brings them back.
AutomationView::Metric AutomationView::metric(ui::App& app, const SDL_Rect& p,
                                              long len) const {
    Metric m;
    const long bar  = std::max<long>(1, measure_len());
    const long beat = std::max<long>(1, beat_len());
    const long span = visible_ticks(len);
    const double pxPerTick = (double)std::max(1, p.w - 1) / (double)std::max<long>(1, span);
    const double barPx = (double)bar * pxPerTick;

    const long lastBar = (viewStartTick_ + span) / bar + 2 + clip_origin() / bar;
    int digits = 1;
    for (long v = lastBar; v >= 10; v /= 10) ++digits;
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
    const double labelPx = (double)digits * cw + (double)cw * 3.0;

    // bar * stride must stay inside a long (32-bit under LLP64): unclamped an
    // extreme zoom-out overflows it, and barStep is both a divisor and a loop
    // increment, so the grid would divide by zero and never terminate.
    const long maxN   = std::max<long>(1, 0x3FFFFFFFL / bar);
    const long barN   = std::min(maxN, pow2_stride(barPx, 7.0));
    const long labelN = std::min(maxN, std::max(barN, pow2_stride(barPx, labelPx)));
    m.barStep   = bar * barN;
    m.labelStep = bar * labelN;
    if (m.barStep   < 1)         m.barStep   = bar;
    if (m.labelStep < m.barStep) m.labelStep = m.barStep;

    // Beats, then subdivisions, only once EVERY bar is already drawn.
    m.beatStep = (barN == 1 && (double)beat * pxPerTick >= 6.0) ? beat : 0;
    const long sub = snapTicks_ > 0 ? snapTicks_ : beat;
    m.subStep = (barN == 1 && sub < beat && (double)sub * pxPerTick >= 6.0) ? sub : 0;
    return m;
}

//! "bar.beat.tick" in the numbering the ruler is currently labelled with.
std::string AutomationView::bbt(long localTick) const {
    long t = localTick + (localBars_ ? 0 : clip_origin());
    if (t < 0) t = 0;
    const long bar  = t / measure_len() + 1;
    const long beat = (t % measure_len()) / beat_len() + 1;
    const long sub  = t % beat_len();
    char b[48];
    std::snprintf(b, sizeof(b), "%ld.%ld.%03ld", bar, beat, sub);
    return b;
}
//! A DURATION in bars.beats, so a range readout says "2.0" not "1536 ticks".
std::string AutomationView::bars_len(long ticks) const {
    if (ticks < 0) ticks = 0;
    char b[48];
    std::snprintf(b, sizeof(b), "%ld.%ld", ticks / measure_len(),
                  (ticks % measure_len()) / beat_len());
    return b;
}

long AutomationView::snap_tick(long tick) const {
    if (!snapOn_ || snapTicks_ <= 1) return tick;
    return ((tick + snapTicks_ / 2) / snapTicks_) * snapTicks_;
}

// --- zoom / scroll ---------------------------------------------------------
void AutomationView::zoom_to_fit() {
    zoomX_ = 1.0; viewStartTick_ = 0;
}
void AutomationView::zoom_to_selection() {
    const long len = seq_length();
    long a = sel_.t0, b = sel_.t1;
    if (!sel_.active || b <= a) { zoom_to_fit(); return; }
    const long pad = std::max<long>(beat_len() / 2, (b - a) / 20);
    a = std::max<long>(0, a - pad); b = std::min(len, b + pad);
    if (b <= a) { zoom_to_fit(); return; }
    zoomX_ = clampd((double)len / (double)(b - a), 1.0, 4096.0);
    viewStartTick_ = a;
    clamp_view(len);
}
void AutomationView::zoom_about(double factor, int anchorX) {
    const long len = seq_length();
    const SDL_Rect rl = ruler_rect();
    const long anchor = tick_at_x(rl, clampi(anchorX, rl.x, rl.x + rl.w - 1), len);
    const double frac = clampd((double)(anchorX - rl.x) / (double)std::max(1, rl.w - 1), 0.0, 1.0);
    zoomX_ = clampd(zoomX_ * factor, 1.0, 4096.0);
    viewStartTick_ = anchor - (long)std::lround(frac * (double)visible_ticks(len));
    clamp_view(len);
}
void AutomationView::scroll_by(long ticks) {
    viewStartTick_ += ticks;
    clamp_view(seq_length());
}

// ---------------------------------------------------------------------------
//  undo / redo -- the whole lane set plus its presentation state.  Everything
//  destructive calls push_undo() BEFORE it edits.
// ---------------------------------------------------------------------------
AutomationView::Snapshot AutomationView::capture() const {
    Snapshot s;
    if (AutomationTrack* at = atrack()) {
        s.lanes.reserve((size_t)at->laneCount());
        for (int i = 0; i < at->laneCount(); ++i) s.lanes.push_back(at->lane(i));
    }
    s.ui = laneUI_;
    return s;
}
void AutomationView::restore(const Snapshot& s) {
    AutomationTrack* at = atrack();
    if (!at) return;
    at->clear();
    for (const AutomationLane& l : s.lanes) {
        AutomationLane& nl = at->addLane(l.target(), l.interpolation());
        nl = l;
    }
    laneUI_ = s.ui;
    sync_lane_ui();
    selLane_ = clampi(selLane_, -1, lane_count() - 1);
    selPts_.clear();
}
void AutomationView::push_undo() {
    if (!atrack()) return;
    sync_lane_ui();
    undo_.push_back(capture());
    if ((int)undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
    redo_.clear();
}
void AutomationView::undo() {
    if (undo_.empty()) return;
    redo_.push_back(capture());
    restore(undo_.back());
    undo_.pop_back();
}
void AutomationView::redo() {
    if (redo_.empty()) return;
    undo_.push_back(capture());
    restore(redo_.back());
    redo_.pop_back();
}

// ---------------------------------------------------------------------------
//  lane editing
// ---------------------------------------------------------------------------
void AutomationView::add_lane_for_picker() {
    AutomationTrack* at = atrack();
    if (!at) return;
    LaneTarget tgt;
    if (pickCC_) { tgt.kind = LaneTargetKind::MidiCC; tgt.id = (unsigned)ccSel_; }
    else {
        if (!targets_.empty())
            tgt = targets_[(size_t)clampi(targetSel_,0,(int)targets_.size()-1)].target;
        else {
            if (params_.empty()) return;
            tgt.kind = LaneTargetKind::VstParam; tgt.id = params_[(size_t)paramSel_].id;
        }
    }
    push_undo();
    at->laneForTarget(tgt, Interpolation::Linear);
    for (int i = 0; i < at->laneCount(); ++i)
        if (at->lane(i).target() == tgt) { selLane_ = i; break; }
    sync_lane_ui();
}

//! A genuine model reorder (playback order and serialisation follow), done
//! through AutomationTrack's public API since this view does not own it.
void AutomationView::reorder_lane(int from, int to) {
    AutomationTrack* at = atrack();
    if (!at) return;
    const int n = at->laneCount();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    sync_lane_ui();
    std::vector<AutomationLane> tmp;
    tmp.reserve((size_t)n);
    for (int i = 0; i < n; ++i) tmp.push_back(at->lane(i));
    AutomationLane moved = tmp[(size_t)from];
    tmp.erase(tmp.begin() + from);
    tmp.insert(tmp.begin() + to, moved);
    LaneUI mui = laneUI_[(size_t)from];
    laneUI_.erase(laneUI_.begin() + from);
    laneUI_.insert(laneUI_.begin() + to, mui);
    at->clear();
    for (const AutomationLane& l : tmp) {
        AutomationLane& nl = at->addLane(l.target(), l.interpolation());
        nl = l;
    }
    // Point + range selections are keyed by lane index; move them with the row.
    for (PointRef& pr : selPts_) {
        if (pr.lane == from) pr.lane = to;
        else if (from < to && pr.lane > from && pr.lane <= to) --pr.lane;
        else if (from > to && pr.lane >= to && pr.lane < from) ++pr.lane;
    }
    if ((int)sel_.lanes.size() == n) {
        char f = sel_.lanes[(size_t)from];
        sel_.lanes.erase(sel_.lanes.begin() + from);
        sel_.lanes.insert(sel_.lanes.begin() + to, f);
    }
    if (selLane_ == from) selLane_ = to;
}

void AutomationView::duplicate_lane(int row) {
    AutomationTrack* at = atrack();
    AutomationLane* L = lane(row);
    if (!at || !L) return;
    push_undo();
    sync_lane_ui();
    const AutomationLane copy = *L;
    LaneUI u = laneUI_[(size_t)row];
    u.name = lane_name(row) + " copy";
    AutomationLane& nl = at->addLane(copy.target(), copy.interpolation());
    nl = copy;
    laneUI_.push_back(u);
    sel_.lanes.push_back(0);
    reorder_lane(at->laneCount() - 1, row + 1);
    selLane_ = row + 1;
}

void AutomationView::remove_lane(int row) {
    AutomationTrack* at = atrack();
    if (!at || row < 0 || row >= at->laneCount()) return;
    push_undo();
    at->removeLane(row);
    sync_lane_ui();
    if ((int)laneUI_.size() > row) { /* sync_lane_ui truncated the tail */ }
    selPts_.erase(std::remove_if(selPts_.begin(), selPts_.end(),
                                 [&](const PointRef& p){ return p.lane == row; }),
                  selPts_.end());
    for (PointRef& p : selPts_) if (p.lane > row) --p.lane;
    if (selLane_ == row) selLane_ = -1;
    else if (selLane_ > row) --selLane_;
}

void AutomationView::sync_picker_to_lane(int row) {
    AutomationLane* L = lane(row);
    if (!L) return;
    pickCC_ = L->target().kind == LaneTargetKind::MidiCC;
    if (pickCC_) ccSel_ = clampi((int)L->target().id, 0, 127);
    else for (int i = 0; i < (int)targets_.size(); ++i)
        if (targets_[(size_t)i].target == L->target()) { targetSel_ = i; break; }
}

void AutomationView::refresh_targets_preserving_selection() {
    if (!on_list_targets) return;
    LaneTarget keep;
    bool have = false;
    if (AutomationLane* L = lane(selLane_)) { keep = L->target(); have = true; }
    else if (!targets_.empty()) {
        keep = targets_[(size_t)clampi(targetSel_,0,(int)targets_.size()-1)].target;
        have = true;
    }
    std::vector<TargetChoice> fresh = on_list_targets();
    targets_.swap(fresh); targetSel_ = 0;
    if (have) for (int i = 0; i < (int)targets_.size(); ++i)
        if (targets_[(size_t)i].target == keep) { targetSel_ = i; break; }
}

void AutomationView::apply_picker_to_selected() {
    AutomationLane* L = lane(selLane_);
    if (!L) { add_lane_for_picker(); return; }
    LaneTarget tgt;
    if (pickCC_) { tgt.kind = LaneTargetKind::MidiCC; tgt.id = (unsigned)ccSel_; }
    else if (!targets_.empty())
        tgt = targets_[(size_t)clampi(targetSel_,0,(int)targets_.size()-1)].target;
    else if (!params_.empty()) {
        tgt.kind = LaneTargetKind::VstParam; tgt.id = params_[(size_t)paramSel_].id;
    } else return;
    push_undo();
    L->setTarget(tgt);
}

void AutomationView::cycle_interp() {
    AutomationLane* L = lane(selLane_);
    if (!L) return;
    push_undo();
    switch (L->interpolation()) {
        case Interpolation::Linear: L->setInterpolation(Interpolation::Step); break;
        case Interpolation::Step:   L->setInterpolation(Interpolation::Hold); break;
        case Interpolation::Hold: default: L->setInterpolation(Interpolation::Linear); break;
    }
}

// nearest breakpoint index within a pick radius scaled off the font, or -1.
int AutomationView::pick_point(AutomationLane* L, const SDL_Rect& p, int row,
                               long len, int px, int py) const {
    if (!L) return -1;
    const int R = 8;
    int best = -1; double bestD = (double)(R*R) + 1.0;
    const std::vector<Breakpoint>& bps = L->breakpoints();
    for (int i = 0; i < (int)bps.size(); ++i) {
        const int bx = x_at_tick(p, (long)bps[(size_t)i].tick, len);
        const int by = y_at_val(p, row, bps[(size_t)i].value);
        const double d = (double)(bx-px)*(bx-px) + (double)(by-py)*(by-py);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
//  selection
// ---------------------------------------------------------------------------
void AutomationView::select_range(long t0, long t1, bool allLanes, int oneLane) {
    sync_lane_ui();
    if (t1 < t0) std::swap(t0, t1);
    sel_.active = true;
    sel_.t0 = t0; sel_.t1 = t1;
    if (allLanes) sel_.lanes.assign(sel_.lanes.size(), 1);
    else if (oneLane >= 0) {
        sel_.lanes.assign(sel_.lanes.size(), 0);
        if (oneLane < (int)sel_.lanes.size()) sel_.lanes[(size_t)oneLane] = 1;
    }
}
void AutomationView::select_all_lanes() {
    sync_lane_ui();
    sel_.active = true;
    sel_.t0 = 0; sel_.t1 = seq_length();
    sel_.lanes.assign(sel_.lanes.size(), 1);
}

bool AutomationView::point_selected(int lane, int64_t tick) const {
    for (const PointRef& p : selPts_)
        if (p.lane == lane && p.tick == tick) return true;
    return false;
}
void AutomationView::toggle_point(int lane, int64_t tick, bool additive) {
    if (!additive) { selPts_.clear(); selPts_.push_back(PointRef{ lane, tick }); return; }
    for (size_t i = 0; i < selPts_.size(); ++i)
        if (selPts_[i].lane == lane && selPts_[i].tick == tick) {
            selPts_.erase(selPts_.begin() + (long)i); return;
        }
    selPts_.push_back(PointRef{ lane, tick });
}
void AutomationView::clear_points() { selPts_.clear(); }

void AutomationView::points_in_box(const SDL_Rect& box, int mode) {
    if (mode == MarqueeReplace) selPts_.clear();
    const long len = seq_length();
    for (int row = 0; row < lane_count(); ++row) {
        AutomationLane* L = lane(row);
        if (!L) continue;
        const SDL_Rect p = plot_rect(row);
        if (p.y + p.h < box.y || p.y > box.y + box.h) continue;
        const std::vector<Breakpoint>& bps = L->breakpoints();
        for (const Breakpoint& b : bps) {
            const int bx = x_at_tick(p, (long)b.tick, len);
            const int by = y_at_val(p, row, b.value);
            if (!in_rect(box, bx, by)) continue;
            if (mode == MarqueeSubtract) {
                for (size_t i = 0; i < selPts_.size(); ++i)
                    if (selPts_[i].lane == row && selPts_[i].tick == (int64_t)b.tick) {
                        selPts_.erase(selPts_.begin() + (long)i);
                        break;
                    }
            } else if (!point_selected(row, b.tick)) {
                selPts_.push_back(PointRef{ row, (int64_t)b.tick });
            }
        }
    }
}

void AutomationView::nudge_points(long dTick, float dVal) {
    if (selPts_.empty()) return;
    // A pointer drag already pushed one entry at the press; a keyboard nudge
    // is its own edit.  Otherwise a single drag would fill the whole history.
    if (drag_ == Drag::None) push_undo();
    // Move right-to-left when shifting forward so points never step onto each
    // other mid-pass (add() overwrites a colliding tick).
    std::sort(selPts_.begin(), selPts_.end(), [&](const PointRef& a, const PointRef& b) {
        return dTick >= 0 ? (a.tick > b.tick) : (a.tick < b.tick);
    });
    const long len = seq_length();
    for (PointRef& pr : selPts_) {
        AutomationLane* L = lane(pr.lane);
        if (!L) continue;
        const std::vector<Breakpoint>& bps = L->breakpoints();
        int idx = -1;
        for (int i = 0; i < (int)bps.size(); ++i)
            if (bps[(size_t)i].tick == pr.tick) { idx = i; break; }
        if (idx < 0) continue;
        const long nt = clampl((long)pr.tick + dTick, 0, len);
        const float nv = clampf(bps[(size_t)idx].value + dVal, 0.f, 1.f);
        // move() is erase + add(), and add() resets curve to 0 -- keep the
        // segment shape attached to the point that owns it.
        const float curve = bps[(size_t)idx].curve;
        const int ni = L->move(idx, nt, nv);
        if (ni >= 0) { L->setCurveAfter(ni, curve); pr.tick = L->at(ni).tick; }
    }
}

void AutomationView::delete_points() {
    if (selPts_.empty()) return;
    push_undo();
    for (const PointRef& pr : selPts_)
        if (AutomationLane* L = lane(pr.lane)) L->removeAtTick(pr.tick);
    selPts_.clear();
}

//! Clear the range selection: drop every breakpoint inside [t0,t1] on each
//! selected lane.  The lanes themselves -- target binding, colour, height,
//! interpolation -- are untouched; deleting a LANE is a separate, deliberate
//! act (Shift+Delete, or "Delete lane" in the gutter / lane menu).
void AutomationView::delete_range() {
    if (!sel_.active) return;
    std::vector<int> rows;
    for (int i = 0; i < lane_count(); ++i) if (sel_.has_lane(i)) rows.push_back(i);
    if (rows.empty() && selLane_ >= 0) rows.push_back(selLane_);
    if (rows.empty()) return;

    long t0 = sel_.t0, t1 = sel_.t1;
    if (t1 < t0) std::swap(t0, t1);

    // Nothing inside the range?  Do not burn an undo slot on a no-op.
    bool any = false;
    for (int row : rows) {
        AutomationLane* L = lane(row);
        if (!L) continue;
        for (const Breakpoint& b : L->breakpoints())
            if ((long)b.tick >= t0 && (long)b.tick <= t1) { any = true; break; }
        if (any) break;
    }
    if (!any) return;

    push_undo();
    for (int row : rows) {
        AutomationLane* L = lane(row);
        if (!L) continue;
        // Collect first: removeAtTick() invalidates the breakpoint vector.
        std::vector<int64_t> kill;
        for (const Breakpoint& b : L->breakpoints())
            if ((long)b.tick >= t0 && (long)b.tick <= t1) kill.push_back(b.tick);
        for (int64_t tk : kill) L->removeAtTick(tk);
    }
    selPts_.clear();
}

//! Copy is lane-relative: the buffer stores ticks measured from the earliest
//! selected point, so it can be pasted into ANY lane at any position.
void AutomationView::copy_points() {
    clip_.clear(); clipSpan_ = 0;
    std::vector<Breakpoint> got;
    if (!selPts_.empty()) {
        for (const PointRef& pr : selPts_) {
            AutomationLane* L = lane(pr.lane);
            if (!L) continue;
            for (const Breakpoint& b : L->breakpoints())
                if (b.tick == pr.tick) { got.push_back(b); break; }
        }
    } else if (sel_.active) {
        for (int row = 0; row < lane_count(); ++row) {
            if (!sel_.has_lane(row)) continue;
            AutomationLane* L = lane(row);
            if (!L) continue;
            for (const Breakpoint& b : L->breakpoints())
                if (b.tick >= sel_.t0 && b.tick <= sel_.t1) got.push_back(b);
            break;                      // one lane's worth: paste is per-lane
        }
    }
    if (got.empty()) return;
    std::sort(got.begin(), got.end(),
              [](const Breakpoint& a, const Breakpoint& b){ return a.tick < b.tick; });
    const int64_t base = got.front().tick;
    for (Breakpoint b : got) { b.tick -= base; clip_.push_back(b); }
    clipSpan_ = (long)(clip_.back().tick - clip_.front().tick);
}

//! Paste lands at the range-selection start (else the playhead, else 0) into
//! every selected lane -- that is what makes it a cross-lane paste.
void AutomationView::paste_points() {
    if (clip_.empty()) return;
    long at = sel_.active ? sel_.t0 : playhead_local();
    if (at < 0) at = 0;
    at = snap_tick(at);
    std::vector<int> rows;
    for (int i = 0; i < lane_count(); ++i) if (sel_.has_lane(i)) rows.push_back(i);
    if (rows.empty() && selLane_ >= 0) rows.push_back(selLane_);
    if (rows.empty()) return;
    push_undo();
    selPts_.clear();
    const long len = seq_length();
    for (int row : rows) {
        AutomationLane* L = lane(row);
        if (!L) continue;
        for (const Breakpoint& b : clip_) {
            const long t = clampl(at + (long)b.tick, 0, len);
            const int i = L->add(t, b.value);
            L->setCurveAfter(i, b.curve);
            selPts_.push_back(PointRef{ row, (int64_t)t });
        }
    }
}

// ---------------------------------------------------------------------------
//  menu action ids.  One flat space shared by every context menu; ops live
//  above kOpBase so the whole process list is one contiguous block.
// ---------------------------------------------------------------------------
enum MenuId {
    MI_NONE = 0,
    MI_ADD_LANE, MI_DUP_LANE, MI_DEL_LANE, MI_RENAME, MI_TARGET, MI_CLEAR_LANE,
    MI_MUTE, MI_SOLO, MI_BYPASS, MI_INTERP,
    MI_VZOOM_IN, MI_VZOOM_OUT, MI_VZOOM_RESET,
    MI_LANE_UP, MI_LANE_DOWN,
    MI_SEL_ALL, MI_SEL_LANE, MI_SEL_NONE, MI_SEL_LOOP,
    MI_FIT, MI_FIT_SEL, MI_ZOOM_IN, MI_ZOOM_OUT,
    MI_SNAP_TOGGLE, MI_SNAP_FINER, MI_SNAP_COARSER,
    MI_UNDO, MI_REDO,
    MI_COPY, MI_PASTE, MI_DEL_POINTS, MI_DEL_RANGE, MI_ADD_POINT, MI_POINT_VALUE,
    MI_FOLLOW, MI_BARMODE, MI_PROCESS, MI_LOOP_HERE,
    MI_COLOUR_BASE = 600,                  // + tone index
    MI_OP_BASE     = 1000                  // + op index
};

// ---------------------------------------------------------------------------
//  target naming / picker
// ---------------------------------------------------------------------------
Color AutomationView::lane_colour(int row) const {
    if (row >= 0 && row < (int)laneUI_.size()) {
        const LaneUI& u = laneUI_[(size_t)row];
        if (u.mute || u.bypass) return theme().dim;
        return lane_tone(u.colour);
    }
    return theme().accent;
}

std::string AutomationView::target_label(const AutomationLane* L) const {
    if (!L) return std::string();
    char buf[48];
    if (L->target().kind == LaneTargetKind::MidiCC) {
        std::snprintf(buf, sizeof(buf), "CC %u", L->target().id);
        return buf;
    }
    for (const TargetChoice& t : targets_)
        if (t.target == L->target())
            return t.node + " > " + (t.device.empty() ? t.parameter
                                                      : t.device + " > " + t.parameter);
    for (const ParamRef& pr : params_)
        if (pr.id == L->target().id) return "P:" + pr.name;
    std::snprintf(buf, sizeof(buf), "P#%u", L->target().id);
    return buf;
}

std::string AutomationView::picker_label() const {
    if (pickCC_) { char b[24]; std::snprintf(b,sizeof(b),"CC %d",ccSel_); return b; }
    if (!targets_.empty()) {
        const TargetChoice& t = targets_[(size_t)clampi(targetSel_,0,(int)targets_.size()-1)];
        return t.node + " > " + (t.device.empty()?t.parameter:t.device+" > "+t.parameter);
    }
    if (params_.empty()) return "(no params)";
    return params_[(size_t)clampi(paramSel_,0,(int)params_.size()-1)].name;
}

std::string AutomationView::picker_part(int level) const {
    if (pickCC_) return level==2 ? (std::string("CC ")+std::to_string(ccSel_)) : "-";
    if (targets_.empty()) return level==0 ? "(no targets)" : (level==2?picker_label():"-");
    const TargetChoice& t = targets_[(size_t)clampi(targetSel_,0,(int)targets_.size()-1)];
    return level==0 ? t.node : (level==1 ? (t.device.empty()?"(device)":t.device) : t.parameter);
}

void AutomationView::open_picker_level(int level) {
    // Rack contents are mutable while this window remains open.  Refresh on
    // entry to the hierarchy (not every video frame), preserving stable ids.
    if (level == 0) refresh_targets_preserving_selection();
    popupLevel_ = level; popupScroll_ = 0; popupItems_.clear();
    if (level < 2) paramPageStart_ = -1;
    if (pickCC_) {
        if (level == 2) for (int i=0;i<128;++i) popupItems_.push_back("CC "+std::to_string(i));
        return;
    }
    const std::string node = picker_part(0), device = picker_part(1);
    for (const TargetChoice& t : targets_) {
        if (level > 0 && t.node != node) continue;
        if (level > 1 && (t.device.empty()?"(device)":t.device) != device) continue;
        std::string s = level==0 ? t.node
                                 : (level==1 ? (t.device.empty()?"(device)":t.device)
                                             : t.parameter);
        if (std::find(popupItems_.begin(),popupItems_.end(),s)==popupItems_.end())
            popupItems_.push_back(s);
    }
    if (level == 2 && (int)popupItems_.size() > kParameterPageSize) {
        if (paramPageStart_ < 0) {
            const int count = (int)popupItems_.size();
            popupItems_.clear(); popupLevel_ = 3;
            for (int first = 0; first < count; first += kParameterPageSize)
                popupItems_.push_back("Parameters " + std::to_string(first+1) + "-" +
                    std::to_string(std::min(count, first+kParameterPageSize)));
        } else {
            const int first = paramPageStart_;
            const int last  = std::min((int)popupItems_.size(), first+kParameterPageSize);
            std::vector<std::string> page;
            for (int i=first;i<last;++i) page.push_back(popupItems_[(size_t)i]);
            popupItems_.swap(page);
        }
    }
}

// Rows are laid out in ONE place, used by both the painter and the hit-test.
// They used to be computed separately -- drawn from popupRect_.y + 3, clicked
// from popupRect_.y - 2 -- so the row you landed on was five pixels away from
// the row you were pointing at, and near a boundary you picked the wrong
// parameter.  The click also was not bounded to the rows actually drawn, so
// clicking the popup's padding selected an entry scrolled off-screen.
int AutomationView::popup_row_h(ui::App& app) const { return app.font.ch()+6; }
int AutomationView::popup_rows_shown(ui::App& app) const {
    (void)app;
    return std::max(0, std::min(kPopupMaxRows, (int)popupItems_.size()-popupScroll_));
}
int AutomationView::popup_row_at(ui::App& app, int y) const {
    const int rh = popup_row_h(app);
    if (y < popupRect_.y+2) return -1;
    const int row = (y - popupRect_.y - 2) / std::max(1, rh);
    if (row < 0 || row >= popup_rows_shown(app)) return -1;   // padding is not a row
    const int item = popupScroll_ + row;
    return (item >= 0 && item < (int)popupItems_.size()) ? item : -1;
}

void AutomationView::draw_picker_popup(ui::App& app) {
    if (popupLevel_ < 0) return;
    const Theme& t = theme();
    // Plain motion is not delivered without a button held, so poll the pointer
    // here -- without it the menu has no hover feedback at all and you cannot
    // tell which row a click is about to take.
    mouse_logical(app, popMx_, popMy_);
    const SDL_Rect anchor = selLane_ >= 0 ? gutter_rect(selLane_) : rAdd_;
    const int rh = popup_row_h(app);
    popupScroll_ = clampi(popupScroll_, 0,
                          std::max(0,(int)popupItems_.size()-kPopupMaxRows));
    const int shown = popup_rows_shown(app);
    popupRect_ = clamp_popup(SDL_Rect{ anchor.x, anchor.y+anchor.h,
                                       std::max(anchor.w, app.font.cw()*24),
                                       std::max(rh, shown*rh+4) }, rect);
    fill_rect(app.ren, popupRect_, t.panel);
    frame_rect(app.ren, popupRect_, t.accent);

    const int hot = popup_row_at(app, popMy_);
    const bool inX = popMx_>=popupRect_.x && popMx_<popupRect_.x+popupRect_.w;
    for (int i = 0; i < shown; ++i) {
        const int item = i + popupScroll_;
        if (item >= (int)popupItems_.size()) break;
        const SDL_Rect row{ popupRect_.x+1, popupRect_.y+2+i*rh, popupRect_.w-2, rh };
        const bool hover = inX && item == hot;
        if (hover) fill_rect(app.ren, row, t.accent);
        app.font.draw(app.ren, popupRect_.x+5, row.y+3,
                      fit_text(app.font, popupItems_[(size_t)item], popupRect_.w-12),
                      hover ? t.bg : t.text);
    }
    // Say there is more, rather than leaving the list looking complete.
    if (popupScroll_ > 0)
        app.mono.draw(app.ren, popupRect_.x+popupRect_.w-app.mono.cw()-4,
                      popupRect_.y+2, "^", t.dim);
    if (popupScroll_+shown < (int)popupItems_.size())
        app.mono.draw(app.ren, popupRect_.x+popupRect_.w-app.mono.cw()-4,
                      popupRect_.y+popupRect_.h-app.mono.ch()-2, "v", t.dim);
}

// ---------------------------------------------------------------------------
//  gutter geometry -- ONE definition, shared by draw_lane_gutter() and the hit
//  test in on_mouse().  Nothing recomputes a box of its own.
// ---------------------------------------------------------------------------
AutomationView::GutterGeo AutomationView::gutter_geo(ui::App& app, int row) const {
    GutterGeo g;
    const SDL_Rect r = gutter_rect(row);
    if (r.w <= 0 || r.h <= 0) return g;
    const int ch = app.font.ch();
    const int hw = std::max(8, app.font.cw());              // drag-handle strip
    g.handle = SDL_Rect{ r.x+1, r.y+1, hw, r.h-2 };
    g.del    = SDL_Rect{ r.x+r.w-ch-6, r.y+3, ch+2, ch+2 };
    g.name   = SDL_Rect{ r.x+hw+3, r.y+3, std::max(1, g.del.x-(r.x+hw+6)), ch+2 };

    const int by = r.y + 5 + ch + 3;
    const int bw = app.font.text_w("M") + 8, bh = ch + 2;
    int bx = r.x + hw + 3;
    g.mute   = SDL_Rect{ bx, by, bw, bh }; bx += bw + 2;
    g.solo   = SDL_Rect{ bx, by, bw, bh }; bx += bw + 2;
    g.byp    = SDL_Rect{ bx, by, bw, bh }; bx += bw + 4;
    g.colour = SDL_Rect{ bx, by, bw, bh }; bx += bw + 4;
    g.vzDn   = SDL_Rect{ bx, by, bw, bh }; bx += bw + 2;
    g.vzUp   = SDL_Rect{ bx, by, bw, bh };
    // Bottom edge: drag to change this lane's row height.
    g.resize = SDL_Rect{ r.x, r.y+r.h-3, r.w, 5 };
    if (by + bh > r.y + r.h - 2) {          // short row: hide the second line
        g.mute = g.solo = g.byp = g.colour = g.vzDn = g.vzUp = SDL_Rect{0,0,0,0};
    }
    return g;
}

bool AutomationView::draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                              bool active, bool enabled) const {
    if (r.w <= 0 || r.h <= 0) return false;
    const Theme& t = theme();
    fill_rect(app.ren, r, active ? t.accent : t.panel);
    frame_rect(app.ren, r, active ? t.hi : t.dim);
    const Color fg = active ? t.bg : (enabled ? t.text : t.dim);
    app.font.draw_centered(app.ren, r, fit_text(app.font, s, r.w-4), fg);
    return true;
}

// ---------------------------------------------------------------------------
//  painters
// ---------------------------------------------------------------------------
void AutomationView::draw_toolbar(ui::App& app) {
    const Theme& t = theme();
    const SDL_Rect bar{ rect.x, rect.y, rect.w, tbH_ };
    fill_rect(app.ren, bar, t.panel);
    hline(app.ren, rect.x, rect.x+rect.w, rect.y+tbH_, t.dim);

    AutomationLane* sel = lane(selLane_);
    const bool canAdd = pickCC_ || !targets_.empty() || !params_.empty();
    draw_box(app, rAdd_,    "+",   false, canAdd);
    draw_box(app, rRemove_, "-",   false, sel != nullptr);
    draw_box(app, rDup_,    "DUP", false, sel != nullptr);
    draw_box(app, rInterp_, sel ? interp_name(sel->interpolation()) : "LIN",
             false, sel != nullptr);
    draw_box(app, rSnapOn_, "SNAP", snapOn_);
    char sb[24];
    const long denom = snapTicks_ > 0 ? (4L*c_ppqn)/snapTicks_ : 0;
    std::snprintf(sb, sizeof(sb), "1/%ld", denom > 0 ? denom : 1);
    draw_box(app, rSnap_,   sb, false, snapOn_);
    draw_box(app, rFit_,    "FIT", false);
    draw_box(app, rFitSel_, "FIT SEL", false, sel_.active);
    draw_box(app, rFollow_, "FOLLOW", follow_);
    draw_box(app, rFx_,     "PROCESS", menuKind_ == MenuOps, sel_.active);
    draw_box(app, rUndo_,   "UNDO", false, !undo_.empty());
    draw_box(app, rRedo_,   "REDO", false, !redo_.empty());

    draw_status(app);
}

//! Right-aligned strip in the toolbar: the selection in bars.beats AND ticks
//! while there is one, otherwise what this editor is looking at.
void AutomationView::draw_status(ui::App& app) {
    const Theme& t = theme();
    std::string s;
    Color c = t.dim;
    if (sel_.active && sel_.t1 > sel_.t0) {
        const long span = sel_.t1 - sel_.t0;
        char b[160];
        std::snprintf(b, sizeof(b), "SEL %s - %s   %ld ticks / %s bars   %d lane%s",
                      bbt(sel_.t0).c_str(), bbt(sel_.t1).c_str(), span,
                      bars_len(span).c_str(), sel_.lane_count(),
                      sel_.lane_count()==1?"":"s");
        s = b; c = t.accent;
    } else {
        char b[96];
        std::snprintf(b, sizeof(b), "AUTOMATION CLIP  T%d  %d LANES",
                      track_+1, lane_count());
        s = b;
    }
    const int avail = (rect.x + rect.w - 8) - (rRedo_.w ? rRedo_.x + rRedo_.w + 8
                                                        : rect.x + 8);
    if (avail <= app.font.cw()*4) return;
    const std::string fitted = fit_text(app.font, s, avail);
    app.font.draw(app.ren, rect.x+rect.w-app.font.text_w(fitted)-6,
                  rect.y + (tbH_-app.font.ch())/2, fitted, c);
}

//  Bar / beat ruler over the plot column.  Ticks come from the SAME metric()
//  the lane grid uses and are placed with the SAME x_at_tick(), so a ruler mark
//  and its grid line are the same pixel at every zoom.
void AutomationView::draw_ruler(ui::App& app) {
    const Theme& t = theme();
    const SDL_Rect rl = ruler_rect();
    const long len = seq_length();

    // corner above the gutters: which numbering the ruler is showing.
    rBars_ = SDL_Rect{ rect.x, rl.y, gutterW_, rulerH_ };
    fill_rect(app.ren, rBars_, t.panel);
    frame_rect(app.ren, rBars_, t.dim);
    app.mono.draw_centered(app.ren, rBars_,
        fit_text(app.mono, localBars_ ? "CLIP BARS" : "SONG BARS", rBars_.w-6), t.dim);

    fill_rect(app.ren, rl, t.panel);
    hline(app.ren, rl.x, rl.x+rl.w, rl.y+rl.h-1, t.accent);
    ui::ScopedClip clipScope(app.ren, rl);

    const Metric m = metric(app, rl, len);
    const long span = visible_ticks(len);
    const long last = std::min(len, viewStartTick_ + span);
    const long origin = localBars_ ? 0 : clip_origin();
    const long bar = std::max<long>(1, measure_len());
    const long beat = std::max<long>(1, beat_len());

    // subdivisions -> beats -> bars, shortest tick first so bars overprint them.
    if (m.subStep > 0)
        for (long tk = (viewStartTick_/m.subStep)*m.subStep; tk <= last; tk += m.subStep) {
            if (tk < viewStartTick_) continue;
            const int x = x_at_tick(rl, tk, len);
            vline(app.ren, x, rl.y+rl.h-4, rl.y+rl.h-1, t.dim);
        }
    if (m.beatStep > 0)
        for (long tk = (viewStartTick_/m.beatStep)*m.beatStep; tk <= last; tk += m.beatStep) {
            if (tk < viewStartTick_ || tk % bar == 0) continue;
            const int x = x_at_tick(rl, tk, len);
            vline(app.ren, x, rl.y+rl.h/2, rl.y+rl.h-1, t.dim);
        }
    for (long tk = (viewStartTick_/m.barStep)*m.barStep; tk <= last; tk += m.barStep) {
        if (tk < viewStartTick_) continue;
        const int x = x_at_tick(rl, tk, len);
        vline(app.ren, x, rl.y+2, rl.y+rl.h-1, t.accent);
    }
    // labels on their own (coarser or equal) stride so they never overprint.
    for (long tk = (viewStartTick_/m.labelStep)*m.labelStep; tk <= last; tk += m.labelStep) {
        if (tk < viewStartTick_) continue;
        const int x = x_at_tick(rl, tk, len);
        char b[32];
        std::snprintf(b, sizeof(b), "%ld", (tk+origin)/bar + 1);
        if (x + app.mono.text_w(b) < rl.x + rl.w)
            app.mono.draw(app.ren, x+3, rl.y+2, b, t.text);
    }
    // beat numbers only once a beat has room for "bar.beat" twice over.
    if (m.beatStep > 0) {
        const double beatPx = (double)beat * (double)std::max(1,rl.w-1) / (double)span;
        if (beatPx > app.mono.text_w("00.0") * 1.6) {
            for (long tk = (viewStartTick_/beat)*beat; tk <= last; tk += beat) {
                if (tk < viewStartTick_ || tk % bar == 0) continue;
                const int x = x_at_tick(rl, tk, len);
                char b[32];
                std::snprintf(b, sizeof(b), "%ld.%ld", (tk+origin)/bar + 1,
                              ((tk+origin)%bar)/beat + 1);
                app.mono.draw(app.ren, x+3, rl.y+2, b, t.dim);
            }
        }
    }

    // the selection's extent, restated on the ruler
    if (sel_.active && sel_.t1 > sel_.t0) {
        const int x0 = x_at_tick(rl, sel_.t0, len), x1 = x_at_tick(rl, sel_.t1, len);
        fill_alpha(app.ren, SDL_Rect{ x0, rl.y+1, std::max(1,x1-x0), rl.h-2 },
                   t.accent, 70);
        vline(app.ren, x0, rl.y+1, rl.y+rl.h-1, t.hi);
        vline(app.ren, x1, rl.y+1, rl.y+rl.h-1, t.hi);
    }
    // playhead marker
    const long ph = playhead_local();
    if (ph >= 0) {
        const int x = x_at_tick(rl, ph, len);
        if (x >= rl.x && x < rl.x+rl.w) {
            vline(app.ren, x, rl.y+1, rl.y+rl.h-1, t.hi);
            app.add_damage(SDL_Rect{ x-24, rl.y, 48, rl.h });
        }
    }
}

//  Proportional horizontal navigator: the thumb is visible/total wide, dragging
//  it writes viewStartTick_, and the track pages.  A clip overview (every
//  lane's points as dashes) is drawn under it so a jump can be aimed.
void AutomationView::draw_scrollbar(ui::App& app) {
    const Theme& t = theme();
    const long len = seq_length();
    const long view = visible_ticks(len);
    scrollTrack_ = scrollbar_rect();
    fill_rect(app.ren, scrollTrack_, t.panel);
    frame_rect(app.ren, scrollTrack_, t.dim);

    const int trackW = std::max(1, scrollTrack_.w-4);
    int thumbW = std::max(app.font.cw()*2,
                          (int)((double)trackW * (double)view / (double)std::max<long>(1,len)));
    if (thumbW > trackW) thumbW = trackW;
    const long maxStart = std::max<long>(1, max_view_start(len));
    const int travel = trackW - thumbW;
    const int thumbX = scrollTrack_.x + 2 +
        (travel > 0 ? (int)((double)travel * (double)viewStartTick_ / (double)maxStart) : 0);

    // overview: where the material actually is, so the jump has a target.
    const int ox = scrollTrack_.x+2, ow = trackW;
    const int oy = scrollTrack_.y+3, oh = std::max(2, scrollTrack_.h-6);
    for (int row = 0; row < lane_count(); ++row) {
        AutomationLane* L = lane(row);
        if (!L) continue;
        for (const Breakpoint& b : L->breakpoints()) {
            const int x = ox + (int)((double)b.tick / (double)std::max<long>(1,len) * ow);
            if (x >= ox && x < ox+ow)
                fill_rect(app.ren, SDL_Rect{ x, oy, 2, oh }, t.dim);
        }
    }
    if (sel_.active && sel_.t1 > sel_.t0) {
        const int a = ox + (int)((double)sel_.t0 / (double)std::max<long>(1,len) * ow);
        const int b = ox + (int)((double)sel_.t1 / (double)std::max<long>(1,len) * ow);
        fill_alpha(app.ren, SDL_Rect{ a, oy, std::max(1,b-a), oh }, t.accent, 90);
    }
    const long ph = playhead_local();
    if (ph >= 0) {
        const int x = ox + (int)((double)ph / (double)std::max<long>(1,len) * ow);
        if (x >= ox && x < ox+ow)
            vline(app.ren, x, scrollTrack_.y+1, scrollTrack_.y+scrollTrack_.h-1, t.hi);
        app.add_damage(SDL_Rect{ x-24, scrollTrack_.y, 48, scrollTrack_.h });
    }

    scrollThumb_ = SDL_Rect{ thumbX, scrollTrack_.y+2, thumbW,
                             std::max(4, scrollTrack_.h-4) };
    const bool hot = drag_ == Drag::HScroll || in_rect(scrollThumb_, mx_, my_);
    { Color c = hot ? t.hi : t.accent; c.a = 200;
      SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
      fill_round(app.ren, scrollThumb_, 3, c);
      SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE); }
    frame_rect(app.ren, scrollThumb_, t.hi);
}

void AutomationView::draw_lane_gutter(ui::App& app, int row) {
    const Theme& t = theme();
    AutomationLane* L = lane(row);
    if (!L || row >= (int)laneUI_.size()) return;
    const LaneUI& u = laneUI_[(size_t)row];
    const SDL_Rect g = gutter_rect(row);
    if (g.w <= 0 || g.h <= 0) return;
    const GutterGeo geo = gutter_geo(app, row);
    const bool selected = (row == selLane_);
    const bool inSel = sel_.active && sel_.has_lane(row);

    fill_rect(app.ren, g, selected ? t.sel : t.panel);
    frame_rect(app.ren, g, inSel ? t.hi : t.dim);

    // drag handle (reorder) -- three rules, so it reads as a grip
    fill_rect(app.ren, geo.handle, selected ? t.sel : t.panel);
    for (int i = 0; i < 3; ++i)
        hline(app.ren, geo.handle.x+2, geo.handle.x+geo.handle.w-2,
              geo.handle.y + geo.handle.h/2 - 3 + i*3, t.dim);
    // colour chip down the inside edge of the handle
    fill_rect(app.ren, SDL_Rect{ geo.handle.x+geo.handle.w-1, geo.handle.y, 2,
                                 geo.handle.h }, lane_tone(u.colour));

    const Color fg = selected ? t.bg : t.text;
    std::string nm = lane_name(row);
    if (nm.empty()) nm = target_label(L);
    if (!u.name.empty()) nm = u.name;
    app.font.draw(app.ren, geo.name.x, geo.name.y, fit_text(app.font, nm, geo.name.w), fg);
    if (u.mute)         // struck through: a muted lane still shows its curve
        hline(app.ren, geo.name.x, geo.name.x+std::min(geo.name.w,app.font.text_w(nm)),
              geo.name.y+app.font.ch()/2, selected ? t.bg : t.dim);

    draw_box(app, geo.del, "x", false);
    if (geo.mute.w) {
        draw_box(app, geo.mute, "M", u.mute);
        draw_box(app, geo.solo, "S", u.solo);
        draw_box(app, geo.byp,  "B", u.bypass);
        draw_box(app, geo.colour, "C", false);
        fill_rect(app.ren, SDL_Rect{ geo.colour.x+2, geo.colour.y+geo.colour.h-3,
                                     geo.colour.w-4, 2 }, lane_tone(u.colour));
        draw_box(app, geo.vzDn, "-", false, u.vzoom > 1.01f);
        draw_box(app, geo.vzUp, "+", false, u.vzoom < 15.9f);
    }
    // third line, when the row is tall enough: interp, point count, v-zoom
    const int line3 = geo.mute.w ? geo.mute.y + geo.mute.h + 3 : geo.name.y + app.font.ch() + 3;
    if (line3 + app.font.ch() < g.y + g.h - 2) {
        char sub[64];
        std::snprintf(sub, sizeof(sub), "%s  %dpt  x%.1f",
                      interp_name(L->interpolation()), L->size(), (double)u.vzoom);
        app.font.draw(app.ren, geo.name.x, line3,
                      fit_text(app.font, sub, g.w-(geo.name.x-g.x)-4),
                      selected ? t.bg : t.dim);
    }
    // reorder insertion line
    if (drag_ == Drag::LaneReorder && reorderTo_ == row && reorderTo_ != reorderFrom_)
        fill_rect(app.ren, SDL_Rect{ g.x, reorderTo_ > reorderFrom_ ? g.y+g.h-2 : g.y,
                                     g.w, 2 }, t.hi);
}

void AutomationView::draw_lane(ui::App& app, int row) {
    const Theme& t = theme();
    AutomationLane* L = lane(row);
    if (!L || row >= (int)laneUI_.size()) return;
    const LaneUI& u = laneUI_[(size_t)row];
    const long len = seq_length();
    const SDL_Rect p = plot_rect(row);
    if (p.w <= 4 || p.h <= 4) return;
    ui::ScopedClip clipScope(app.ren, p);

    const bool selected = (row == selLane_);
    const bool inSel = sel_.active && sel_.has_lane(row);

    fill_rect(app.ren, p, t.keybg);

    // --- grid: same metric() and same x_at_tick() as the ruler --------------
    const Metric m = metric(app, p, len);
    const long span = visible_ticks(len);
    const long last = std::min(len, viewStartTick_ + span);
    if (m.subStep > 0)
        for (long tk = (viewStartTick_/m.subStep)*m.subStep; tk <= last; tk += m.subStep) {
            if (tk < viewStartTick_) continue;
            const int x = x_at_tick(p, tk, len);
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            { Color c = t.dim; c.a = 70; set_color(app.ren, c);
              SDL_RenderDrawLine(app.ren, x, p.y+1, x, p.y+p.h-1); }
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        }
    if (m.beatStep > 0)
        for (long tk = (viewStartTick_/m.beatStep)*m.beatStep; tk <= last; tk += m.beatStep) {
            if (tk < viewStartTick_ || tk % std::max<long>(1,measure_len()) == 0) continue;
            vline(app.ren, x_at_tick(p, tk, len), p.y+1, p.y+p.h-1, t.dim);
        }
    for (long tk = (viewStartTick_/m.barStep)*m.barStep; tk <= last; tk += m.barStep) {
        if (tk < viewStartTick_) continue;
        vline(app.ren, x_at_tick(p, tk, len), p.y, p.y+p.h, t.accent);
    }
    // centre of the value axis (moves with the per-lane vertical zoom)
    hline(app.ren, p.x, p.x+p.w, y_at_val(p, row, 0.5f), t.dim);

    // --- the clip's loop WINDOW: playback repeats this span ----------------
    // Both edges are shown and both are grabbable, so this reads as the same
    // band the piano roll's ruler draws for the very same clip (it is the same
    // window -- see loop_length()).  Only the end used to be drawn, which made
    // a window starting later than 0 invisible here even while it governed what
    // the notes did.  Everything outside the window is dimmed: it is data the
    // repetition never reaches.  A one-shot clip has no window to speak of --
    // it plays straight through -- so the marks go faint and only the pattern's
    // end is shown.
    const long loopS = (long)std::min<int64_t>(clip_loop_start(), len);
    const long loopT = (long)std::min<int64_t>(clip_loop_end(),   len);
    const bool loopOn = clip_loop_on();
    const int loopSX = x_at_tick(p, loopS, len);
    const int loopX  = x_at_tick(p, loopT, len);
    if (loopOn) {
        if (loopSX > p.x)
            fill_alpha(app.ren, SDL_Rect{ p.x, p.y, loopSX-p.x, p.h }, t.dim, 60);
        if (loopX < p.x + p.w)
            fill_alpha(app.ren, SDL_Rect{ loopX, p.y, p.x+p.w-loopX, p.h }, t.dim, 60);
    }
    const Color loopCol = loopOn ? t.hi : t.dim;
    vline(app.ren, loopX, p.y, p.y+p.h, loopCol);
    const SDL_Rect loopHandle{ loopX-4, p.y, 9, 9 };
    fill_rect(app.ren, loopHandle, loopCol);
    frame_rect(app.ren, loopHandle, t.bg);
    if (loopS > 0) {
        vline(app.ren, loopSX, p.y, p.y+p.h, loopCol);
        const SDL_Rect startHandle{ loopSX-4, p.y, 9, 9 };
        fill_rect(app.ren, startHandle, loopCol);
        frame_rect(app.ren, startHandle, t.bg);
    }
    // Say WHY the marks are faint, once, on the top lane -- "the clip does not
    // repeat" is not something a shade of grey can state on its own.  Same word
    // the piano roll's and the tracker's chips use for the same flag.
    if (!loopOn && row == 0) {
        const int tw = app.font.text_w("1-SHOT");
        const int tx = loopX - tw - 6;
        if (tx > p.x + 2) app.font.draw(app.ren, tx, p.y + 2, "1-SHOT", t.dim);
    }

    // --- range selection: translucent wash + firm edges --------------------
    if (inSel && sel_.t1 > sel_.t0) {
        const int x0 = x_at_tick(p, sel_.t0, len), x1 = x_at_tick(p, sel_.t1, len);
        fill_alpha(app.ren, SDL_Rect{ x0, p.y, std::max(1,x1-x0), p.h }, t.accent, 46);
        vline(app.ren, x0, p.y, p.y+p.h, t.hi);
        vline(app.ren, x1, p.y, p.y+p.h, t.hi);
    }

    // --- envelope ----------------------------------------------------------
    const std::vector<Breakpoint>& bps = L->breakpoints();
    const Interpolation ip = L->interpolation();
    const Color cc = lane_colour(row);
    if (bps.empty()) {
        app.font.draw(app.ren, p.x+6, p.y+p.h/2-app.font.ch()/2,
                      fit_text(app.font, "click to add points", p.w-12), t.dim);
    } else {
        int fx = x_at_tick(p, (long)bps.front().tick, len);
        int fy = y_at_val(p, row, bps.front().value);
        if (fx > p.x) hline(app.ren, p.x, fx, fy, cc);
        for (int i = 0; i + 1 < (int)bps.size(); ++i) {
            const int ax = x_at_tick(p, (long)bps[(size_t)i].tick, len);
            const int ay = y_at_val(p, row, bps[(size_t)i].value);
            const int bx = x_at_tick(p, (long)bps[(size_t)i+1].tick, len);
            const int by = y_at_val(p, row, bps[(size_t)i+1].value);
            if (bx < p.x - 4 || ax > p.x + p.w + 4) continue;      // cull offscreen
            if (ip == Interpolation::Step) {
                hline(app.ren, ax, bx, ay, cc);
                vline(app.ren, bx, std::min(ay,by), std::max(ay,by), cc);
            } else if (ip == Interpolation::Hold) {
                vline(app.ren, ax, std::min(ay,by), std::max(ay,by), cc);
                hline(app.ren, ax, bx, by, cc);
            } else {
                // Sample the exact playback evaluator so the drawn curve and
                // the emitted values cannot disagree.
                int px = ax, py = ay;
                const int steps = std::max(2, std::min(96, bx-ax));
                set_color(app.ren, cc);
                for (int st = 1; st <= steps; ++st) {
                    const int64_t tk = bps[(size_t)i].tick +
                        (bps[(size_t)i+1].tick - bps[(size_t)i].tick) * st / steps;
                    const int nx = x_at_tick(p, (long)tk, len);
                    const int ny = y_at_val(p, row, L->value_at(tk));
                    SDL_RenderDrawLine(app.ren, px, py, nx, ny);
                    SDL_RenderDrawLine(app.ren, px, py+1, nx, ny+1);
                    px = nx; py = ny;
                }
            }
            // Mid-segment curvature handle: drag vertically to bend.  Crossing
            // the centre restores linear, so no reset gesture is needed.
            if (ip == Interpolation::Linear && bx - ax > 14) {
                const int64_t mt = (bps[(size_t)i].tick + bps[(size_t)i+1].tick)/2;
                const int hx = x_at_tick(p, (long)mt, len);
                const int hy = y_at_val(p, row, L->value_at(mt));
                const SDL_Rect c{ hx-3, hy-3, 7, 7 };
                const bool hotc = (drag_ == Drag::Curve && curveLane_ == row && curveSeg_ == i);
                fill_rect(app.ren, c, hotc ? t.hi : t.panel);
                frame_rect(app.ren, c, cc);
            }
        }
        const int lx = x_at_tick(p, (long)bps.back().tick, len);
        const int ly = y_at_val(p, row, bps.back().value);
        if (lx < p.x + p.w) hline(app.ren, lx, p.x+p.w, ly, cc);

        for (int i = 0; i < (int)bps.size(); ++i) {
            const int bx = x_at_tick(p, (long)bps[(size_t)i].tick, len);
            if (bx < p.x-6 || bx > p.x+p.w+6) continue;
            const int by = y_at_val(p, row, bps[(size_t)i].value);
            const bool psel = point_selected(row, bps[(size_t)i].tick);
            const bool hot  = (drag_ == Drag::Point && row == dragLane_ && i == dragBp_);
            const int  r    = psel || hot ? 4 : 3;
            const SDL_Rect h{ bx-r, by-r, r*2+1, r*2+1 };
            fill_rect(app.ren, h, hot ? t.hi : (psel ? t.hi : (selected ? t.notesel : t.note)));
            frame_rect(app.ren, h, psel ? t.bg : t.bg);
        }
    }

    // --- unselected material reads back so the selection reads forward -----
    if (sel_.active) {
        if (!inSel) fill_alpha(app.ren, p, t.bg, 96);
        else if (sel_.t1 > sel_.t0) {
            const int x0 = x_at_tick(p, sel_.t0, len), x1 = x_at_tick(p, sel_.t1, len);
            if (x0 > p.x) fill_alpha(app.ren, SDL_Rect{ p.x, p.y, x0-p.x, p.h }, t.bg, 72);
            if (x1 < p.x+p.w)
                fill_alpha(app.ren, SDL_Rect{ x1, p.y, p.x+p.w-x1, p.h }, t.bg, 72);
        }
    }
    if (u.mute || u.bypass) fill_alpha(app.ren, p, t.bg, 70);

    // --- playhead ----------------------------------------------------------
    const long ph = playhead_local();
    if (ph >= 0) {
        const int x = x_at_tick(p, ph, len);
        if (x >= p.x && x < p.x+p.w) {
            vline(app.ren, x, p.y, p.y+p.h, t.hi);
            app.add_damage(SDL_Rect{ x-24, p.y, 48, p.h });
        }
    }
    frame_rect(app.ren, p, inSel ? t.hi : t.dim);
}

void AutomationView::draw_empty_state(ui::App& app) {
    const Theme& t = theme();
    static const char* kLines[] = {
        "NO AUTOMATION LANES",
        "",
        "Press  +  on the toolbar to add a lane for a plugin",
        "parameter or a MIDI CC -- or right-click anywhere here.",
        "",
        "Then click in a lane to drop breakpoints, drag the ruler",
        "to select a time range across lanes, and right-click the",
        "selection to run a process over it."
    };
    const int n = (int)(sizeof(kLines)/sizeof(kLines[0]));
    const int lh = app.font.ch() + 4;
    int y = rect.y + tbH_ + (rect.h - tbH_ - n*lh)/2;
    for (int i = 0; i < n; ++i) {
        const std::string s = fit_text(app.font, kLines[i], rect.w-24);
        app.font.draw(app.ren, rect.x + (rect.w - app.font.text_w(s))/2, y,
                      s, i == 0 ? t.text : t.dim);
        y += lh;
    }
}

//! Value + position under the pointer.  Drawn last so it floats over the lanes.
void AutomationView::draw_cursor_readout(ui::App& app) {
    if (any_popup_open() || mx_ < 0) return;
    const int row = row_at_y(my_);
    if (row < 0 || row >= lane_count()) return;
    const SDL_Rect p = plot_rect(row);
    if (!in_rect(p, mx_, my_)) return;
    const Theme& t = theme();
    const long len = seq_length();
    const long tk = tick_at_x(p, mx_, len);
    const float v = val_at_y(p, row, my_);
    char b[96];
    std::snprintf(b, sizeof(b), "%s   %.3f", bbt(tk).c_str(), (double)v);
    const int w = app.mono.text_w(b) + 10, h = app.mono.ch() + 6;
    SDL_Rect box{ mx_ + 14, my_ - h - 6, w, h };
    box = clamp_popup(box, rect);
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    app.mono.draw(app.ren, box.x+5, box.y+3, b, t.text);
    app.add_damage(SDL_Rect{ box.x-8, box.y-8, box.w+16, box.h+16 });
}

// ---------------------------------------------------------------------------
//  processes -- the ONE place this view talks to the op engine
// ---------------------------------------------------------------------------
void AutomationView::run_op(ui::App& app, int opIndex) {
    if (opIndex < 0 || opIndex >= OP_COUNT) return;
    if (!sel_.active || sel_.t1 <= sel_.t0) return;
    std::vector<AutomationLane*> ls;
    for (int i = 0; i < lane_count(); ++i)
        if (sel_.has_lane(i)) if (AutomationLane* L = lane(i)) ls.push_back(L);
    if (ls.empty()) return;
    if (op_is_multi(opIndex) && ls.size() < 2) return;   // menu greys these out

    push_undo();
    ao::Range r;
    r.begin = (int64_t)sel_.t0;
    r.end   = (int64_t)sel_.t1;
    ao::Params p;
    p.amount     = (decltype(p.amount))opp_.amount;
    p.pivot      = (decltype(p.pivot))opp_.pivot;
    p.grid       = (decltype(p.grid))(int64_t)opp_.grid;
    p.steps      = (decltype(p.steps))(int)opp_.steps;
    p.freqHz     = (decltype(p.freqHz))opp_.freqHz;
    p.syncCycles = (decltype(p.syncCycles))(int)opp_.syncCycles;
    p.shape      = (decltype(p.shape))(int)opp_.shape;
    p.phase      = (decltype(p.phase))opp_.phase;
    p.lo         = (decltype(p.lo))opp_.lo;
    p.hi         = (decltype(p.hi))opp_.hi;
    p.seed       = (decltype(p.seed))(unsigned)opp_.seed;

    const bool changed = ao::applyMulti(ls, r, kOpEnum[opIndex], p);
    // The engine reports "nothing happened"; do not leave a no-op in history.
    if (!changed && !undo_.empty()) undo_.pop_back();
    selPts_.clear();
    app.request_redraw();
}

int AutomationView::prompt_fields(int op, PromptField* f) {
    int n = 0;
    auto add = [&](const char* l, double* v, double lo, double hi, double st,
                   bool ig = false, bool tk = false) {
        if (n >= 6) return;
        f[n].label = l; f[n].value = v; f[n].lo = lo; f[n].hi = hi;
        f[n].step = st; f[n].integer = ig; f[n].ticks = tk; ++n;
    };
    switch (op) {
    case OP_QUANTISE:   add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_HUMANISE:   add("Amount", &opp_.amount, 0, 1, 0.005);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true);
                        add("Seed", &opp_.seed, 1, 9999, 1, true); break;
    case OP_STRETCH:    add("Factor", &opp_.amount, 0.05, 8, 0.01);
                        add("Pivot", &opp_.pivot, 0, 1, 0.005); break;
    case OP_SHIFTTIME:  add("Steps", &opp_.amount, -64, 64, 0.05);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_SCALEVAL:   add("Factor", &opp_.amount, 0, 4, 0.01);
                        add("Pivot", &opp_.pivot, 0, 1, 0.005); break;
    case OP_OFFSETVAL:  add("Offset", &opp_.amount, -1, 1, 0.005); break;
    case OP_INVERTVAL:  add("Pivot", &opp_.pivot, 0, 1, 0.005); break;
    case OP_NORMALISE:  add("Low", &opp_.lo, 0, 1, 0.005);
                        add("High", &opp_.hi, 0, 1, 0.005); break;
    case OP_CLAMPVAL:   add("Low", &opp_.lo, 0, 1, 0.005);
                        add("High", &opp_.hi, 0, 1, 0.005); break;
    case OP_SMOOTH:     add("Amount", &opp_.amount, 0, 1, 0.005);
                        add("Passes", &opp_.steps, 1, 32, 1, true); break;
    case OP_THIN:       add("Tolerance", &opp_.amount, 0, 0.5, 0.002); break;
    case OP_DENSIFY:    add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true);
                        add("Steps", &opp_.steps, 1, 64, 1, true); break;
    case OP_LFOFILL:    add("Shape", &opp_.shape, 0, 5, 0.05, true);
                        add("Cycles", &opp_.syncCycles, 0, 64, 0.1, true);
                        add("Phase", &opp_.phase, 0, 1, 0.005);
                        add("Low", &opp_.lo, 0, 1, 0.005);
                        add("High", &opp_.hi, 0, 1, 0.005);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_RAMPFILL:   add("From", &opp_.lo, 0, 1, 0.005);
                        add("To", &opp_.hi, 0, 1, 0.005);
                        add("Curve", &opp_.amount, -1, 1, 0.005); break;
    case OP_SCURVEFILL: add("From", &opp_.lo, 0, 1, 0.005);
                        add("To", &opp_.hi, 0, 1, 0.005);
                        add("Steepness", &opp_.amount, 0, 1, 0.005);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_BENDCURVE:  add("Bend", &opp_.amount, -1, 1, 0.005); break;
    case OP_STEPQUANT:  add("Levels", &opp_.steps, 2, 64, 1, true);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_RANDWALK:   add("Step", &opp_.amount, 0, 0.5, 0.002);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true);
                        add("Low", &opp_.lo, 0, 1, 0.005);
                        add("High", &opp_.hi, 0, 1, 0.005);
                        add("Seed", &opp_.seed, 1, 9999, 1, true); break;
    case OP_NOISEFILL:  add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true);
                        add("Low", &opp_.lo, 0, 1, 0.005);
                        add("High", &opp_.hi, 0, 1, 0.005);
                        add("Seed", &opp_.seed, 1, 9999, 1, true); break;
    case OP_COPYSHAPE:  add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_MIRROR:     add("Pivot", &opp_.pivot, 0, 1, 0.005); break;
    case OP_PHASEOFF:   add("Offset", &opp_.amount, 0, 1, 0.005);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    case OP_AVERAGE:    add("Mix", &opp_.amount, 0, 1, 0.005);
                        add("Grid", &opp_.grid, 1, 4*c_ppqn, 1, true, true); break;
    default: break;                       // Reverse / ripple ops take no knobs
    }
    return n;
}

void AutomationView::open_prompt(ui::App& app, int opIndex) {
    PromptField f[8];
    promptOp_ = opIndex;
    if (prompt_fields(opIndex, f) == 0) {   // nothing to adjust: just run it
        promptOp_ = -1;
        run_op(app, opIndex);
        return;
    }
    if (opp_.grid < 1) opp_.grid = (double)std::max<long>(1, snapTicks_);
    promptOpen_ = true;
    promptField_ = -1;
    app.request_redraw();
}

SDL_Rect AutomationView::prompt_box(ui::App& app, int& rowh, int& n) {
    PromptField f[8];
    n = prompt_fields(promptOp_, f);
    rowh = app.font.ch() + 8;
    int w = app.font.text_w(op_label(promptOp_)) + app.font.cw()*8;
    for (int i = 0; i < n; ++i)
        w = std::max(w, app.font.text_w(f[i].label) + app.font.cw()*16);
    w = std::min(std::max(w, app.font.cw()*26), std::max(80, rect.w-8));
    const int h = rowh + n*rowh + rowh + 8;
    return clamp_popup(SDL_Rect{ menuX_, menuY_, w, h }, rect);
}

//! Ticks read as "48 t  1/16" -- a bare tick count is not a duration anyone can
//! place, and the note value is what the musician actually chose.
void AutomationView::draw_prompt(ui::App& app) {
    if (!promptOpen_ || promptOp_ < 0) return;
    const Theme& t = theme();
    PromptField f[8];
    int rowh = 0, n = 0;
    promptRect_ = prompt_box(app, rowh, n);
    prompt_fields(promptOp_, f);
    fill_rect(app.ren, promptRect_, t.panel);
    frame_rect(app.ren, promptRect_, t.accent);

    app.font.draw(app.ren, promptRect_.x+6, promptRect_.y+4,
                  fit_text(app.font, op_label(promptOp_), promptRect_.w-12), t.text);
    const int vx = promptRect_.x + promptRect_.w/2;
    for (int i = 0; i < n; ++i) {
        const SDL_Rect row{ promptRect_.x+1, promptRect_.y+rowh+i*rowh,
                            promptRect_.w-2, rowh };
        app.font.draw(app.ren, row.x+6, row.y+3,
                      fit_text(app.font, f[i].label, vx-row.x-10), t.dim);
        const SDL_Rect vb{ vx, row.y+2, promptRect_.x+promptRect_.w-vx-5, rowh-4 };
        const bool hot = (drag_ == Drag::PromptField && promptField_ == i) ||
                         in_rect(vb, mx_, my_);
        fill_rect(app.ren, vb, hot ? t.accent : t.keybg);
        frame_rect(app.ren, vb, t.dim);
        char b[64];
        const double v = *f[i].value;
        if (f[i].ticks) {
            const long d = v > 0 ? (long)(4L*c_ppqn/(long)v) : 0;
            if (d > 0 && (long)v * d == 4L*c_ppqn)
                std::snprintf(b, sizeof(b), "%ld t  1/%ld", (long)v, d);
            else std::snprintf(b, sizeof(b), "%ld t", (long)v);
        } else if (f[i].integer) std::snprintf(b, sizeof(b), "%ld", (long)std::lround(v));
        else                     std::snprintf(b, sizeof(b), "%.3f", v);
        app.font.draw_centered(app.ren, vb, fit_text(app.font, b, vb.w-4),
                               hot ? t.bg : t.text);
    }
    // Apply / Cancel share this row; both rects are stored for the hit test.
    const int by = promptRect_.y + rowh + n*rowh + 2;
    const int bw = (promptRect_.w - 14)/2;
    promptApply_  = SDL_Rect{ promptRect_.x+5, by, bw, rowh-4 };
    promptCancel_ = SDL_Rect{ promptRect_.x+promptRect_.w-bw-5, by, bw, rowh-4 };
    draw_box(app, promptApply_,  "APPLY",  in_rect(promptApply_, mx_, my_));
    draw_box(app, promptCancel_, "CANCEL", in_rect(promptCancel_, mx_, my_));
    app.font.draw(app.ren, promptRect_.x+6, promptRect_.y+promptRect_.h-app.font.ch()-1,
                  "", t.dim);
}

bool AutomationView::prompt_click(ui::App& app, int x, int y) {
    if (!promptOpen_ || promptOp_ < 0) return false;
    PromptField f[8];
    int rowh = 0, n = 0;
    const SDL_Rect box = prompt_box(app, rowh, n);   // SAME geometry the painter used
    prompt_fields(promptOp_, f);
    if (!in_rect(box, x, y)) { promptOpen_ = false; app.request_redraw(); return true; }
    if (in_rect(promptApply_, x, y)) {
        const int op = promptOp_;
        promptOpen_ = false; promptOp_ = -1;
        run_op(app, op);
        return true;
    }
    if (in_rect(promptCancel_, x, y)) {
        promptOpen_ = false; promptOp_ = -1; app.request_redraw(); return true;
    }
    const int vx = box.x + box.w/2;
    for (int i = 0; i < n; ++i) {
        const SDL_Rect vb{ vx, box.y+rowh+i*rowh+2, box.x+box.w-vx-5, rowh-4 };
        if (in_rect(vb, x, y)) {
            drag_ = Drag::PromptField;
            promptField_ = i;
            promptDragX_ = x;
            promptDragStart_ = *f[i].value;
            app.request_redraw();
            return true;
        }
    }
    return true;                                     // swallow clicks in the box
}

// ---------------------------------------------------------------------------
//  context menus -- one row list, one geometry, four entry points
// ---------------------------------------------------------------------------
void AutomationView::build_menu(MenuKind kind, int row) {
    menuRows_.clear();
    auto item = [&](const char* l, int id, bool en = true) {
        MenuRow r; r.label = l; r.id = id; r.enabled = en; menuRows_.push_back(r);
    };
    auto check = [&](const char* l, int id, bool on, bool en = true) {
        MenuRow r; r.label = l; r.id = id; r.enabled = en; r.check = true; r.on = on;
        menuRows_.push_back(r);
    };
    auto head = [&](const std::string& l) {
        MenuRow r; r.label = l; r.header = true; r.enabled = false;
        menuRows_.push_back(r);
    };
    auto sep = [&]() { MenuRow r; r.separator = true; r.enabled = false;
                       menuRows_.push_back(r); };

    const bool haveSel  = sel_.active && sel_.t1 > sel_.t0;
    const bool havePts  = !selPts_.empty();
    AutomationLane* L   = lane(row);

    switch (kind) {
    case MenuToolbar:
        item("Add lane...",        MI_ADD_LANE);
        item("Duplicate lane",     MI_DUP_LANE, L != nullptr);
        item("Delete lane",        MI_DEL_LANE, L != nullptr);
        sep();
        item("Undo",               MI_UNDO, !undo_.empty());
        item("Redo",               MI_REDO, !redo_.empty());
        sep();
        check("Snap",              MI_SNAP_TOGGLE, snapOn_);
        item("Snap finer",         MI_SNAP_FINER);
        item("Snap coarser",       MI_SNAP_COARSER);
        sep();
        item("Fit window",         MI_FIT);
        item("Fit selection",      MI_FIT_SEL, haveSel);
        sep();
        check("Follow playhead",   MI_FOLLOW, follow_);
        check("Clip bar numbers",  MI_BARMODE, localBars_);
        sep();
        item("Select all",         MI_SEL_ALL);
        item("Select none",        MI_SEL_NONE, sel_.active);
        break;

    case MenuGutter:
        head(L ? lane_name(row) : std::string("lane"));
        item("Rename...",          MI_RENAME, L != nullptr);
        item("Target...",          MI_TARGET, L != nullptr);
        item("Cycle interpolation",MI_INTERP, L != nullptr);
        sep();
        check("Mute",              MI_MUTE,   row < (int)laneUI_.size() && laneUI_[(size_t)row].mute,   L!=nullptr);
        check("Solo",              MI_SOLO,   row < (int)laneUI_.size() && laneUI_[(size_t)row].solo,   L!=nullptr);
        check("Bypass",            MI_BYPASS, row < (int)laneUI_.size() && laneUI_[(size_t)row].bypass, L!=nullptr);
        sep();
        head("COLOUR");
        for (int i = 0; i < kLaneTones; ++i)
            check(lane_tone_name(i), MI_COLOUR_BASE + i,
                  row < (int)laneUI_.size() && laneUI_[(size_t)row].colour == i, L != nullptr);
        sep();
        item("Vertical zoom in",   MI_VZOOM_IN,    L != nullptr);
        item("Vertical zoom out",  MI_VZOOM_OUT,   L != nullptr);
        item("Reset vertical zoom",MI_VZOOM_RESET, L != nullptr);
        sep();
        item("Move lane up",       MI_LANE_UP,   row > 0);
        item("Move lane down",     MI_LANE_DOWN, row >= 0 && row+1 < lane_count());
        item("Duplicate lane",     MI_DUP_LANE,  L != nullptr);
        item("Clear all points",   MI_CLEAR_LANE,L != nullptr);
        item("Delete lane",        MI_DEL_LANE,  L != nullptr);
        break;

    case MenuPlot:
        item("Add point here",      MI_ADD_POINT, L != nullptr);
        item("Delete points",       MI_DEL_POINTS, havePts);
        item("Clear selected range",MI_DEL_RANGE,  haveSel);
        item("Point value...",      MI_POINT_VALUE, selPts_.size() == 1);
        sep();
        item("Copy",                MI_COPY, havePts || haveSel);
        item("Paste",               MI_PASTE, !clip_.empty());
        sep();
        item("Select whole lane",   MI_SEL_LANE, L != nullptr);
        item("Select all lanes",    MI_SEL_ALL);
        item("Select none",         MI_SEL_NONE, sel_.active);
        sep();
        item("Process...",          MI_PROCESS, haveSel);
        sep();
        item("Set loop end here",   MI_LOOP_HERE);
        sep();
        item("Undo",                MI_UNDO, !undo_.empty());
        item("Redo",                MI_REDO, !redo_.empty());
        break;

    case MenuRuler:
        item("Select all lanes",    MI_SEL_ALL);
        item("Select loop range",   MI_SEL_LOOP);
        item("Select none",         MI_SEL_NONE, sel_.active);
        sep();
        item("Fit window",          MI_FIT);
        item("Fit selection",       MI_FIT_SEL, haveSel);
        item("Zoom in",             MI_ZOOM_IN);
        item("Zoom out",            MI_ZOOM_OUT);
        sep();
        check("Clip bar numbers",   MI_BARMODE, localBars_);
        check("Snap",               MI_SNAP_TOGGLE, snapOn_);
        sep();
        item("Process...",          MI_PROCESS, haveSel);
        break;

    case MenuOps: {
        const int nLanes = sel_.lane_count();
        for (int g = 0; g < 4; ++g) {
            head(kGroupName[g]);
            for (int i = kGroupFirst[g]; i < kGroupFirst[g+1]; ++i) {
                // The multi-lane group is only meaningful across 2+ lanes; it
                // stays visible (so the ops are discoverable) but greyed.
                const bool en = haveSel && (!op_is_multi(i) || nLanes >= 2);
                item(op_label(i).c_str(), MI_OP_BASE + i, en);
            }
            if (g < 3) sep();
        }
        break; }
    default: break;
    }
}

void AutomationView::open_menu(ui::App& app, MenuKind kind, int x, int y,
                               int row, long tick) {
    popupLevel_ = -1; popupItems_.clear();
    promptOpen_ = false;
    menuKind_ = kind; menuX_ = x; menuY_ = y; menuRow_ = row; menuTick_ = tick;
    menuScroll_ = 0;
    build_menu(kind, row);
    app.request_redraw();
}

SDL_Rect AutomationView::menu_box(ui::App& app, int& rowh, int& rows) const {
    rowh = app.font.ch() + 6;
    int w = app.font.cw() * 14;
    for (const MenuRow& r : menuRows_)
        w = std::max(w, app.font.text_w(r.label) + app.font.cw()*5);
    w = std::min(w, std::max(80, rect.w - 8));
    const int maxRows = std::max(1, (rect.h - 8) / std::max(1, rowh));
    rows = std::min((int)menuRows_.size() - menuScroll_, maxRows);
    if (rows < 0) rows = 0;
    return clamp_popup(SDL_Rect{ menuX_, menuY_, w, rows*rowh + 4 }, rect);
}

int AutomationView::menu_row_at(ui::App& app, int x, int y) const {
    int rowh = 0, rows = 0;
    const SDL_Rect box = menu_box(app, rowh, rows);
    if (!in_rect(box, x, y)) return -1;
    const int r = (y - box.y - 2) / std::max(1, rowh);
    if (r < 0 || r >= rows) return -1;               // padding is not a row
    const int item = r + menuScroll_;
    return (item >= 0 && item < (int)menuRows_.size()) ? item : -1;
}

void AutomationView::draw_menu(ui::App& app) {
    if (menuKind_ == MenuNone || menuRows_.empty()) return;
    const Theme& t = theme();
    int rowh = 0, rows = 0;
    const SDL_Rect box = menu_box(app, rowh, rows);
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    const int hot = menu_row_at(app, mx_, my_);

    for (int i = 0; i < rows; ++i) {
        const int item = i + menuScroll_;
        if (item >= (int)menuRows_.size()) break;
        const MenuRow& mr = menuRows_[(size_t)item];
        const SDL_Rect row{ box.x+1, box.y+2+i*rowh, box.w-2, rowh };
        if (mr.separator) {
            hline(app.ren, row.x+4, row.x+row.w-4, row.y+rowh/2, t.dim);
            continue;
        }
        if (mr.header) {
            app.mono.draw(app.ren, row.x+5, row.y+3,
                          fit_text(app.mono, mr.label, row.w-10), t.dim);
            hline(app.ren, row.x+4, row.x+row.w-4, row.y+rowh-1, t.dim);
            continue;
        }
        const bool hov = (item == hot) && mr.enabled;
        if (hov) fill_rect(app.ren, row, t.accent);
        int tx = row.x + 6;
        if (mr.check) {
            const SDL_Rect cb{ row.x+5, row.y+3, rowh-8, rowh-8 };
            frame_rect(app.ren, cb, hov ? t.bg : t.dim);
            if (mr.on) fill_rect(app.ren, SDL_Rect{cb.x+2,cb.y+2,cb.w-4,cb.h-4},
                                 hov ? t.bg : t.accent);
            tx = cb.x + cb.w + 6;
        }
        app.font.draw(app.ren, tx, row.y+3,
                      fit_text(app.font, mr.label, row.x+row.w-tx-4),
                      hov ? t.bg : (mr.enabled ? t.text : t.dim));
    }
    if (menuScroll_ > 0)
        app.mono.draw(app.ren, box.x+box.w-app.mono.cw()-3, box.y+2, "^", t.dim);
    if (menuScroll_ + rows < (int)menuRows_.size())
        app.mono.draw(app.ren, box.x+box.w-app.mono.cw()-3,
                      box.y+box.h-app.mono.ch()-2, "v", t.dim);
}

bool AutomationView::menu_click(ui::App& app, int x, int y) {
    if (menuKind_ == MenuNone) return false;
    int rowh = 0, rows = 0;
    const SDL_Rect box = menu_box(app, rowh, rows);
    if (!in_rect(box, x, y)) { close_menus(); app.request_redraw(); return true; }
    const int item = menu_row_at(app, x, y);
    if (item < 0 || item >= (int)menuRows_.size()) return true;    // padding
    const MenuRow mr = menuRows_[(size_t)item];
    if (mr.header || mr.separator || !mr.enabled) return true;
    do_menu(app, mr.id);
    return true;
}

void AutomationView::do_menu(ui::App& app, int id) {
    const int row = menuRow_;
    sync_lane_ui();
    const bool keepOpen = false;

    if (id >= MI_OP_BASE) {
        const int op = id - MI_OP_BASE;
        close_menus();
        open_prompt(app, op);                 // runs straight away if knob-free
        app.request_redraw();
        return;
    }
    if (id >= MI_COLOUR_BASE && id < MI_COLOUR_BASE + kLaneTones) {
        if (row >= 0 && row < (int)laneUI_.size()) {
            push_undo();
            laneUI_[(size_t)row].colour = id - MI_COLOUR_BASE;
        }
        close_menus(); app.request_redraw(); return;
    }

    switch (id) {
    case MI_ADD_LANE:
        refresh_targets_preserving_selection();
        add_lane_for_picker();
        sync_picker_to_lane(selLane_);
        open_picker_level(pickCC_ ? 2 : 0);
        break;
    case MI_DUP_LANE:  duplicate_lane(row >= 0 ? row : selLane_); break;
    case MI_DEL_LANE:  remove_lane(row >= 0 ? row : selLane_); break;
    case MI_RENAME:    close_menus(); begin_rename(app, row >= 0 ? row : selLane_); return;
    case MI_TARGET:
        selLane_ = row >= 0 ? row : selLane_;
        sync_picker_to_lane(selLane_);
        open_picker_level(pickCC_ ? 2 : 0);
        break;
    case MI_INTERP:    selLane_ = row >= 0 ? row : selLane_; cycle_interp(); break;
    case MI_CLEAR_LANE:
        if (AutomationLane* L = lane(row >= 0 ? row : selLane_)) { push_undo(); L->clear(); }
        selPts_.clear();
        break;
    case MI_MUTE:   if (row>=0 && row<(int)laneUI_.size()) laneUI_[(size_t)row].mute   = !laneUI_[(size_t)row].mute;   break;
    case MI_SOLO:   if (row>=0 && row<(int)laneUI_.size()) laneUI_[(size_t)row].solo   = !laneUI_[(size_t)row].solo;   break;
    case MI_BYPASS: if (row>=0 && row<(int)laneUI_.size()) laneUI_[(size_t)row].bypass = !laneUI_[(size_t)row].bypass; break;
    case MI_VZOOM_IN:
        if (row>=0 && row<(int)laneUI_.size())
            laneUI_[(size_t)row].vzoom = clampf(laneUI_[(size_t)row].vzoom*1.5f, 1.f, 16.f);
        break;
    case MI_VZOOM_OUT:
        if (row>=0 && row<(int)laneUI_.size())
            laneUI_[(size_t)row].vzoom = clampf(laneUI_[(size_t)row].vzoom/1.5f, 1.f, 16.f);
        break;
    case MI_VZOOM_RESET:
        if (row>=0 && row<(int)laneUI_.size()) {
            laneUI_[(size_t)row].vzoom = 1.f; laneUI_[(size_t)row].vcenter = 0.5f;
            laneUI_[(size_t)row].height = 0;
        }
        break;
    case MI_LANE_UP:   push_undo(); reorder_lane(row, row-1); break;
    case MI_LANE_DOWN: push_undo(); reorder_lane(row, row+1); break;

    case MI_SEL_ALL:  select_all_lanes(); break;
    case MI_SEL_LANE: select_range(0, seq_length(), false, row >= 0 ? row : selLane_); break;
    case MI_SEL_NONE: sel_.clear(); selPts_.clear(); break;
    // "the loop range" is the clip's window, which need not start at 0.
    case MI_SEL_LOOP: select_range(clip_loop_start(),
                                   (long)std::min<int64_t>(clip_loop_end(), seq_length()),
                                   true, -1); break;

    case MI_FIT:     zoom_to_fit(); break;
    case MI_FIT_SEL: zoom_to_selection(); break;
    case MI_ZOOM_IN:  zoom_about(1.5, ruler_rect().x + ruler_rect().w/2); break;
    case MI_ZOOM_OUT: zoom_about(1.0/1.5, ruler_rect().x + ruler_rect().w/2); break;

    case MI_SNAP_TOGGLE: snapOn_ = !snapOn_; break;
    case MI_SNAP_FINER:
        if (snapTicks_ > 1) snapTicks_ = std::max<long>(1, snapTicks_/2);
        break;
    case MI_SNAP_COARSER:
        snapTicks_ = std::min<long>(4L*c_ppqn, std::max<long>(1, snapTicks_*2));
        break;

    case MI_UNDO: undo(); break;
    case MI_REDO: redo(); break;

    case MI_COPY:  copy_points(); break;
    case MI_PASTE: paste_points(); break;
    case MI_DEL_POINTS: delete_points(); break;
    case MI_DEL_RANGE:  delete_range();  break;
    case MI_ADD_POINT:
        if (AutomationLane* L = lane(row)) {
            push_undo();
            const SDL_Rect p = plot_rect(row);
            const float v = val_at_y(p, row, my_);
            const int i = L->add(snap_tick(menuTick_), v);
            selPts_.clear();
            selPts_.push_back(PointRef{ row, L->at(i).tick });
        }
        break;
    case MI_POINT_VALUE: close_menus(); begin_point_entry(app); return;

    case MI_FOLLOW:  follow_ = !follow_; break;
    case MI_BARMODE: localBars_ = !localBars_; break;
    case MI_PROCESS: open_menu(app, MenuOps, menuX_, menuY_, row, menuTick_); return;
    case MI_LOOP_HERE: set_loop_length(std::max<long>(1, snap_tick(menuTick_))); break;
    default: break;
    }
    if (!keepOpen) close_menus();
    app.request_redraw();
}

// ---------------------------------------------------------------------------
//  inline text entry (lane rename, breakpoint numeric entry)
// ---------------------------------------------------------------------------
void AutomationView::begin_rename(ui::App& app, int row) {
    sync_lane_ui();
    if (row < 0 || row >= (int)laneUI_.size()) return;
    editWhat_ = EditLaneName; editRow_ = row;
    editBuf_ = laneUI_[(size_t)row].name.empty() ? lane_name(row)
                                                 : laneUI_[(size_t)row].name;
    ui::App* ap = &app;
    app.begin_text(&editBuf_, nullptr, [this, ap, row](bool ok) {
        if (ok) {
            push_undo();
            sync_lane_ui();
            if (row >= 0 && row < (int)laneUI_.size()) laneUI_[(size_t)row].name = editBuf_;
        }
        editWhat_ = EditNone; editRow_ = -1;
        ap->request_redraw();
    });
    app.request_redraw();
}

//! "0.75" sets the value; "0.75 480" also moves the point to that tick.
void AutomationView::begin_point_entry(ui::App& app) {
    if (selPts_.size() != 1) return;
    AutomationLane* L = lane(selPts_[0].lane);
    if (!L) return;
    float v = 0.f;
    for (const Breakpoint& b : L->breakpoints())
        if (b.tick == selPts_[0].tick) { v = b.value; break; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f %ld", (double)v, (long)selPts_[0].tick);
    editWhat_ = EditPointValue; editBuf_ = buf;
    ui::App* ap = &app;
    app.begin_text(&editBuf_, nullptr, [this, ap](bool ok) {
        if (ok && selPts_.size() == 1) {
            double nv = 0.0, nt = (double)selPts_[0].tick;
            const int got = std::sscanf(editBuf_.c_str(), "%lf %lf", &nv, &nt);
            if (got >= 1) {
                AutomationLane* L2 = lane(selPts_[0].lane);
                if (L2) {
                    push_undo();
                    const std::vector<Breakpoint>& bps = L2->breakpoints();
                    int idx = -1;
                    for (int i = 0; i < (int)bps.size(); ++i)
                        if (bps[(size_t)i].tick == selPts_[0].tick) { idx = i; break; }
                    if (idx >= 0) {
                        const int ni = L2->move(idx, (int64_t)nt, (float)nv);
                        if (ni >= 0) selPts_[0].tick = L2->at(ni).tick;
                    }
                }
            }
        }
        editWhat_ = EditNone;
        ap->request_redraw();
    });
    app.request_redraw();
}

// ===========================================================================
//  draw -- lanes, then the ruler / scrollbar / toolbar chrome, then every
//  floating overlay LAST so nothing can paint over a menu.
// ===========================================================================
void AutomationView::draw(ui::App& app) {
    // Nodes can be added/restored after this editor was first constructed.
    // Never leave a stale empty selector cached for the lifetime of the window.
    if (targets_.empty() && on_list_targets) { targets_ = on_list_targets(); targetSel_ = 0; }
    // Plain motion is not delivered without a button held, so the pointer is
    // polled once per frame; every hover highlight and the cursor readout use
    // this, never SDL_GetMouseState() (which answers in raw window pixels).
    mouse_logical(app, mx_, my_);
    layout(app);
    // The window can be changed from the piano roll (or a project load) while
    // this editor sits open, and Region::loopLength is what the engine actually
    // folds with -- so re-derive it every frame rather than only when this view
    // edits it.  See loop_length()'s comment for why there is only one loop.
    sync_region_loop();

    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);

    // Follow: keep the playhead on screen, but never fight an active drag.
    if (follow_ && drag_ == Drag::None) {
        const long len = seq_length();
        const long span = visible_ticks(len);
        const long ph = playhead_local();
        if (ph >= 0 && (ph < viewStartTick_ || ph > viewStartTick_ + span*9/10))
            viewStartTick_ = clampl(ph - span/10, 0, max_view_start(len));
    }

    const int n = lane_count();
    if (n == 0) {
        draw_empty_state(app);
    } else {
        const SDL_Rect band{ rect.x, lanes_top(), rect.w,
                             std::max(0, lanes_bottom()-lanes_top()) };
        ui::ScopedClip cs(app.ren, band);
        for (int i = 0; i < n; ++i) {
            const SDL_Rect g = gutter_rect(i);
            if (g.y + g.h < band.y || g.y > band.y + band.h) continue;   // cull
            draw_lane(app, i);
            draw_lane_gutter(app, i);
        }
        if (drag_ == Drag::Marquee && marquee_.w > 1 && marquee_.h > 1) {
            fill_alpha(app.ren, marquee_, t.accent, 50);
            frame_rect(app.ren, marquee_, t.hi);
        }
    }

    draw_ruler(app);
    draw_scrollbar(app);
    draw_toolbar(app);            // never scrolled; drawn over the lane band

    // inline text entry (lane rename / breakpoint numeric entry)
    if (editWhat_ != EditNone) {
        const char* cap = (editWhat_ == EditLaneName) ? "LANE NAME"
                                                      : "VALUE [ TICK ]";
        const int h = app.font.ch() + 12;
        SDL_Rect box{ rect.x + rect.w/6, lanes_top() + 8, rect.w*2/3, h };
        box = clamp_popup(box, rect);
        fill_rect(app.ren, box, t.panel);
        frame_rect(app.ren, box, t.accent);
        const int lw = app.mono.text_w(cap) + 10;
        app.mono.draw(app.ren, box.x+5, box.y+(h-app.mono.ch())/2, cap, t.dim);
        app.font.draw(app.ren, box.x+lw, box.y+(h-app.font.ch())/2,
                      fit_text(app.font, editBuf_ + "_", box.w-lw-6), t.text);
    }

    draw_cursor_readout(app);
    draw_menu(app);
    draw_prompt(app);
    draw_picker_popup(app);
    frame_rect(app.ren, rect, t.dim);
}

// ===========================================================================
//  input
// ===========================================================================
bool AutomationView::on_mouse(ui::App& app, const ui::MouseEv& e) {
    // ---- release: commit whatever the drag was, then clear ALL drag state --
    if (!e.pressed) {
        const Drag was = drag_;
        if (was == Drag::LaneReorder && reorderFrom_ >= 0 && reorderTo_ >= 0 &&
            reorderTo_ != reorderFrom_) {
            push_undo();
            reorder_lane(reorderFrom_, reorderTo_);
        }
        if (was == Drag::Marquee)
            points_in_box(marquee_, marqueeMode_);
        // A range drag that never moved is a CLICK: drop a breakpoint there
        // rather than leaving a zero-width selection nobody asked for.
        if (was == Drag::LaneRange &&
            std::abs(e.x - dragStartX_) <= 3 && std::abs(e.y - dragStartY_) <= 3) {
            if (AutomationLane* L = lane(dragLane_)) {
                push_undo();
                const SDL_Rect p = plot_rect(dragLane_);
                const int i = L->add(snap_tick(dragAnchorTick_),
                                     val_at_y(p, dragLane_, e.y));
                selPts_.clear();
                selPts_.push_back(PointRef{ dragLane_, L->at(i).tick });
            }
            sel_.clear();
        }
        drag_ = Drag::None;
        reorderFrom_ = reorderTo_ = -1; resizeRow_ = -1; promptField_ = -1;
        if (was != Drag::None) { app.request_redraw(); return true; }
        return false;
    }

    mx_ = e.x; my_ = e.y;
    const SDL_Keymod mods = (SDL_Keymod)SDL_GetModState();
    const long len = seq_length();

    // ---- a held drag: the toolkit re-delivers motion as another pressed
    // event, so anything with drag_ set is a MOTION, never a new press. -----
    if (drag_ != Drag::None) {
        switch (drag_) {
        case Drag::HScroll: {
            const int travel = std::max(1, scrollTrack_.w - 4 - scrollThumb_.w);
            const int x = clampi(e.x - scrollGrab_, scrollTrack_.x+2,
                                 scrollTrack_.x+2+travel);
            const double f = (double)(x - scrollTrack_.x - 2) / (double)travel;
            viewStartTick_ = (long)std::lround(f * (double)max_view_start(len));
            clamp_view(len);
            break; }
        case Drag::Point: {
            AutomationLane* L = lane(dragLane_);
            if (L && dragBp_ >= 0 && dragBp_ < L->size()) {
                const SDL_Rect p = plot_rect(dragLane_);
                // AutomationLane::move() is erase + add(), and add() builds a
                // fresh Breakpoint with curve 0 -- so dragging a point sideways
                // silently flattened the curve of the segment leaving it.  The
                // shape belongs to the point, not to its index: carry it across.
                const float curve = L->at(dragBp_).curve;
                dragBp_ = L->move(dragBp_, snap_tick(tick_at_x(p, e.x, len)),
                                  val_at_y(p, dragLane_, e.y));
                if (dragBp_ >= 0) {
                    L->setCurveAfter(dragBp_, curve);
                    selPts_.clear();
                    selPts_.push_back(PointRef{ dragLane_, L->at(dragBp_).tick });
                }
            }
            break; }
        case Drag::Points: {
            const SDL_Rect p = plot_rect(dragLane_);
            const long tk = snap_tick(tick_at_x(p, e.x, len));
            const float v = val_at_y(p, dragLane_, e.y);
            const long  dT = tk - dragLastTick_;
            const float dV = v - dragLastVal_;
            if (dT != 0 || std::fabs(dV) > 0.0005f) nudge_points(dT, dV);
            dragLastTick_ = tk; dragLastVal_ = v;
            break; }
        case Drag::Curve: {
            AutomationLane* L = lane(curveLane_);
            if (L) L->setCurveAfter(curveSeg_,
                                    curveStart_ + (float)(curveStartY_ - e.y)/80.f);
            break; }
        case Drag::Loop: {
            const SDL_Rect p = plot_rect(dragLane_ >= 0 ? dragLane_ : 0);
            const long tk = snap_tick(tick_at_x(p, e.x, len));
            if (dragLoopEdge_ == 0) set_loop_start_ticks(std::max<long>(0, tk));
            else                    set_loop_length(std::max<long>(1, tk));
            break; }
        case Drag::RulerRange:
            select_range(dragAnchorTick_, snap_tick(tick_at_x(ruler_rect(), e.x, len)),
                         true, -1);
            break;
        case Drag::LaneRange: {
            const SDL_Rect p = plot_rect(dragLane_);
            select_range(dragAnchorTick_, snap_tick(tick_at_x(p, e.x, len)),
                         false, dragLane_);
            break; }
        case Drag::Marquee:
            marquee_ = SDL_Rect{ std::min(dragStartX_, e.x), std::min(dragStartY_, e.y),
                                 std::abs(e.x-dragStartX_), std::abs(e.y-dragStartY_) };
            break;
        case Drag::LaneReorder: {
            const int r = row_at_y(e.y);
            reorderTo_ = (r >= 0) ? r
                       : (e.y < lanes_top() ? 0 : std::max(0, lane_count()-1));
            break; }
        case Drag::LaneResize:
            if (resizeRow_ >= 0 && resizeRow_ < (int)laneUI_.size())
                laneUI_[(size_t)resizeRow_].height =
                    clampi(resizeStartH_ + (e.y - dragStartY_), 44, 400);
            break;
        case Drag::VPan:
            if (dragLane_ >= 0 && dragLane_ < (int)laneUI_.size()) {
                LaneUI& u = laneUI_[(size_t)dragLane_];
                const SDL_Rect p = plot_rect(dragLane_);
                u.vcenter = clampf(u.vcenter + (float)(e.y - dragStartY_) /
                                   (float)std::max(1, p.h) / std::max(0.01f, u.vzoom),
                                   0.f, 1.f);
                dragStartY_ = e.y;
            }
            break;
        case Drag::PromptField: {
            PromptField f[8];
            const int n = prompt_fields(promptOp_, f);
            if (promptField_ >= 0 && promptField_ < n) {
                double v = promptDragStart_ +
                           (double)(e.x - promptDragX_) * f[promptField_].step;
                if (f[promptField_].integer) v = std::floor(v + 0.5);
                *f[promptField_].value = clampd(v, f[promptField_].lo, f[promptField_].hi);
            }
            break; }
        default: break;                                    // Drag::Consumed
        }
        app.request_redraw();
        return true;
    }

    if (!hit(e.x, e.y)) return false;

    // ---- overlays are modal: they swallow the press ----------------------
    if (promptOpen_) {
        const bool handled = prompt_click(app, e.x, e.y);
        if (drag_ == Drag::None) drag_ = Drag::Consumed;
        if (handled) return true;
    }
    if (menuKind_ != MenuNone) {
        menu_click(app, e.x, e.y);
        if (drag_ == Drag::None) drag_ = Drag::Consumed;
        return true;
    }
    if (popupLevel_ >= 0) {
        drag_ = Drag::Consumed;
        if (!in_rect(popupRect_, e.x, e.y)) {
            popupLevel_ = -1; popupItems_.clear(); app.request_redraw(); return true;
        }
        const int ix = popup_row_at(app, e.y);   // SAME geometry the painter used
        if (ix >= 0 && ix < (int)popupItems_.size()) {
            const std::string chosen = popupItems_[(size_t)ix];
            if (pickCC_) { if (popupLevel_ == 2) ccSel_ = clampi(std::atoi(chosen.c_str()+3),0,127); }
            else if (popupLevel_ == 3) paramPageStart_ = ix * kParameterPageSize;
            else {
                const std::string oldNode = picker_part(0), oldDev = picker_part(1);
                for (int i = 0; i < (int)targets_.size(); ++i) {
                    const TargetChoice& tc = targets_[(size_t)i];
                    const std::string dev = tc.device.empty() ? "(device)" : tc.device;
                    if ((popupLevel_==0 && tc.node==chosen) ||
                        (popupLevel_==1 && tc.node==oldNode && dev==chosen) ||
                        (popupLevel_==2 && tc.node==oldNode && dev==oldDev &&
                         tc.parameter==chosen)) { targetSel_ = i; break; }
                }
            }
        } else { app.request_redraw(); return true; }      // padding: stay open
        const int completed = popupLevel_;
        popupLevel_ = -1; popupItems_.clear();
        if (completed == 3) open_picker_level(2);
        else if (!pickCC_ && completed == 0 && !targets_.empty()) {
            const TargetChoice& tc = targets_[(size_t)clampi(targetSel_,0,(int)targets_.size()-1)];
            open_picker_level(tc.device.empty() ? 2 : 1);
        } else if (!pickCC_ && completed == 1) open_picker_level(2);
        else if (completed == 2) { paramPageStart_ = -1; apply_picker_to_selected(); }
        app.request_redraw();
        return true;
    }

    // ---- horizontal scrollbar --------------------------------------------
    if (in_rect(scrollTrack_, e.x, e.y)) {
        if (in_rect(scrollThumb_, e.x, e.y)) {
            drag_ = Drag::HScroll; scrollGrab_ = e.x - scrollThumb_.x;
        } else {                                    // track: page by a screenful
            drag_ = Drag::Consumed;
            scroll_by(e.x < scrollThumb_.x ? -visible_ticks(len) : visible_ticks(len));
        }
        app.request_redraw();
        return true;
    }

    // ---- toolbar ----------------------------------------------------------
    if (e.y < rect.y + tbH_) {
        drag_ = Drag::Consumed;
        if (e.button == SDL_BUTTON_RIGHT) {
            open_menu(app, MenuToolbar, e.x, e.y, selLane_, 0); return true;
        }
        if (in_rect(rAdd_, e.x, e.y)) {
            refresh_targets_preserving_selection();
            add_lane_for_picker();
            sync_picker_to_lane(selLane_);
            open_picker_level(pickCC_ ? 2 : 0);
        }
        else if (in_rect(rRemove_, e.x, e.y)) remove_lane(selLane_);
        else if (in_rect(rDup_,    e.x, e.y)) duplicate_lane(selLane_);
        else if (in_rect(rInterp_, e.x, e.y)) cycle_interp();
        else if (in_rect(rSnapOn_, e.x, e.y)) snapOn_ = !snapOn_;
        else if (in_rect(rSnap_,   e.x, e.y)) {
            static const long snaps[] = { 4*c_ppqn, 2*c_ppqn, c_ppqn, c_ppqn/2,
                                          c_ppqn/4, c_ppqn/8, c_ppqn/16, c_ppqn/32 };
            int at = 0;
            for (int i = 0; i < 8; ++i) if (snaps[i] == snapTicks_) at = i;
            snapTicks_ = std::max<long>(1, snaps[(at+1)%8]);
        }
        else if (in_rect(rFit_,    e.x, e.y)) zoom_to_fit();
        else if (in_rect(rFitSel_, e.x, e.y)) zoom_to_selection();
        else if (in_rect(rFollow_, e.x, e.y)) follow_ = !follow_;
        else if (in_rect(rFx_,     e.x, e.y)) {
            if (sel_.active) open_menu(app, MenuOps, rFx_.x, rFx_.y + rFx_.h, selLane_, 0);
        }
        else if (in_rect(rUndo_,   e.x, e.y)) undo();
        else if (in_rect(rRedo_,   e.x, e.y)) redo();
        app.request_redraw();
        return true;
    }

    // ---- ruler corner: which bar numbering is shown -----------------------
    if (in_rect(rBars_, e.x, e.y)) {
        drag_ = Drag::Consumed;
        localBars_ = !localBars_;
        app.request_redraw();
        return true;
    }

    // ---- ruler: range selection across ALL lanes --------------------------
    const SDL_Rect rl = ruler_rect();
    if (in_rect(rl, e.x, e.y)) {
        const long tk = snap_tick(tick_at_x(rl, e.x, len));
        if (e.button == SDL_BUTTON_RIGHT) {
            drag_ = Drag::Consumed;
            open_menu(app, MenuRuler, e.x, e.y, -1, tk);
            return true;
        }
        if ((mods & KMOD_SHIFT) && sel_.active)          // extend from the far edge
            dragAnchorTick_ = (std::labs(tk - sel_.t0) > std::labs(tk - sel_.t1))
                            ? sel_.t0 : sel_.t1;
        else dragAnchorTick_ = tk;
        select_range(dragAnchorTick_, tk, true, -1);
        drag_ = Drag::RulerRange;
        app.request_redraw();
        return true;
    }

    const int row = row_at_y(e.y);

    // ---- lane gutter ------------------------------------------------------
    if (e.x < rect.x + gutterW_) {
        if (row < 0) {
            drag_ = Drag::Consumed;
            if (e.button == SDL_BUTTON_RIGHT) open_menu(app, MenuToolbar, e.x, e.y, -1, 0);
            return true;
        }
        const GutterGeo g = gutter_geo(app, row);
        if (e.button == SDL_BUTTON_RIGHT) {
            drag_ = Drag::Consumed; selLane_ = row;
            open_menu(app, MenuGutter, e.x, e.y, row, 0);
            return true;
        }
        // Ctrl adds / removes this lane from the range selection.
        if ((mods & KMOD_CTRL) && !in_rect(g.del, e.x, e.y)) {
            drag_ = Drag::Consumed;
            sync_lane_ui();
            if (!sel_.active || sel_.t1 <= sel_.t0) { sel_.t0 = 0; sel_.t1 = len; }
            sel_.active = true;
            if (row < (int)sel_.lanes.size())
                sel_.lanes[(size_t)row] = sel_.lanes[(size_t)row] ? 0 : 1;
            app.request_redraw();
            return true;
        }
        selLane_ = row;
        sync_lane_ui();
        LaneUI& u = laneUI_[(size_t)row];
        if      (in_rect(g.del,    e.x, e.y)) { remove_lane(row); drag_ = Drag::Consumed; }
        else if (in_rect(g.mute,   e.x, e.y)) { u.mute   = !u.mute;   drag_ = Drag::Consumed; }
        else if (in_rect(g.solo,   e.x, e.y)) { u.solo   = !u.solo;   drag_ = Drag::Consumed; }
        else if (in_rect(g.byp,    e.x, e.y)) { u.bypass = !u.bypass; drag_ = Drag::Consumed; }
        else if (in_rect(g.colour, e.x, e.y)) { u.colour = (u.colour+1)%kLaneTones;
                                                drag_ = Drag::Consumed; }
        else if (in_rect(g.vzUp,   e.x, e.y)) { u.vzoom = clampf(u.vzoom*1.5f,1.f,16.f);
                                                drag_ = Drag::Consumed; }
        else if (in_rect(g.vzDn,   e.x, e.y)) { u.vzoom = clampf(u.vzoom/1.5f,1.f,16.f);
                                                drag_ = Drag::Consumed; }
        else if (in_rect(g.resize, e.x, e.y)) {
            drag_ = Drag::LaneResize; resizeRow_ = row;
            resizeStartH_ = lane_h(row); dragStartY_ = e.y;
        }
        else if (in_rect(g.handle, e.x, e.y)) {
            drag_ = Drag::LaneReorder; reorderFrom_ = reorderTo_ = row;
        }
        else if (in_rect(g.name, e.x, e.y)) {
            drag_ = Drag::Consumed;
            sync_picker_to_lane(row);
            open_picker_level(pickCC_ ? 2 : 0);   // the lane owns its target picker
        }
        else drag_ = Drag::Consumed;
        app.request_redraw();
        return true;
    }

    // ---- plot -------------------------------------------------------------
    if (row < 0 || row >= lane_count()) { drag_ = Drag::Consumed; return true; }
    AutomationLane* L = lane(row);
    if (!L) { drag_ = Drag::Consumed; return true; }
    const SDL_Rect p = plot_rect(row);
    const long tkRaw = tick_at_x(p, e.x, len);

    if (e.button == SDL_BUTTON_RIGHT) {
        drag_ = Drag::Consumed;
        selLane_ = row;
        open_menu(app, MenuPlot, e.x, e.y, row, tkRaw);
        return true;
    }

    // loop-window handles, top of the lane.  The start handle only exists once
    // the window actually starts later than 0, and the nearer of the two wins
    // so the two cannot fight when they sit on the same pixel.
    const long loopSTick = (long)std::min<int64_t>(clip_loop_start(), len);
    const int loopX  = x_at_tick(p, (long)std::min<int64_t>(clip_loop_end(), len), len);
    const int loopSX = x_at_tick(p, loopSTick, len);
    if (e.y <= p.y + 14) {
        const int dEnd   = std::abs(e.x - loopX);
        const int dStart = loopSTick > 0 ? std::abs(e.x - loopSX) : 1 << 20;
        if (dEnd <= 8 || dStart <= 8) {
            selLane_ = row; dragLane_ = row; drag_ = Drag::Loop;
            dragLoopEdge_ = (dStart < dEnd) ? 0 : 1;
            app.request_redraw();
            return true;
        }
    }

    // curve handles get priority over point picking and over empty-plot clicks
    if (L->interpolation() == Interpolation::Linear) {
        const std::vector<Breakpoint>& bps = L->breakpoints();
        for (int i = 0; i + 1 < (int)bps.size(); ++i) {
            const int64_t mt = (bps[(size_t)i].tick + bps[(size_t)i+1].tick)/2;
            const int hx = x_at_tick(p, (long)mt, len);
            const int hy = y_at_val(p, row, L->value_at(mt));
            if (std::abs(e.x-hx) <= 7 && std::abs(e.y-hy) <= 7) {
                push_undo();
                drag_ = Drag::Curve; curveLane_ = row; curveSeg_ = i;
                curveStartY_ = e.y; curveStart_ = bps[(size_t)i].curve;
                selLane_ = row;
                app.request_redraw();
                return true;
            }
        }
    }

    const int bp = pick_point(L, p, row, len, e.x, e.y);
    if (bp >= 0) {
        selLane_ = row;
        const int64_t tk = L->at(bp).tick;
        if (mods & KMOD_CTRL) {                       // add / remove from the set
            toggle_point(row, tk, true);
            drag_ = Drag::Consumed;
        } else {
            if (!point_selected(row, tk)) {
                selPts_.clear();
                selPts_.push_back(PointRef{ row, tk });
            }
            push_undo();                              // one undo entry per drag
            dragLane_ = row; dragBp_ = bp;
            dragLastTick_ = snap_tick(tick_at_x(p, e.x, len));
            dragLastVal_  = val_at_y(p, row, e.y);
            drag_ = selPts_.size() > 1 ? Drag::Points : Drag::Point;
        }
        app.request_redraw();
        return true;
    }

    // empty plot space
    selLane_ = row;
    dragLane_ = row;
    dragStartX_ = e.x; dragStartY_ = e.y;
    if (mods & KMOD_CTRL) {                           // marquee over breakpoints
        // Latch what this marquee will DO now, while the press modifiers are
        // unambiguous.  Ctrl starts the marquee, so reading Ctrl again on
        // release only ever said "add" -- replace and subtract were unreachable.
        //    Ctrl            -> replace the point selection with the box
        //    Ctrl+Shift      -> add the box to it
        //    Ctrl+Alt        -> remove the box from it
        marqueeMode_ = (mods & KMOD_SHIFT) ? MarqueeAdd
                     : (mods & KMOD_ALT)   ? MarqueeSubtract
                                           : MarqueeReplace;
        marquee_ = SDL_Rect{ e.x, e.y, 0, 0 };
        drag_ = Drag::Marquee;
    } else if (mods & KMOD_ALT) {                     // pan the value axis
        drag_ = Drag::VPan;
    } else {
        // Range-select this lane; a press that never moves becomes "add a
        // breakpoint here" on release (see the release path above).
        if ((mods & KMOD_SHIFT) && sel_.active && sel_.has_lane(row))
            dragAnchorTick_ = (std::labs(snap_tick(tkRaw) - sel_.t0) >
                               std::labs(snap_tick(tkRaw) - sel_.t1)) ? sel_.t0 : sel_.t1;
        else dragAnchorTick_ = snap_tick(tkRaw);
        select_range(dragAnchorTick_, snap_tick(tkRaw), false, row);
        drag_ = Drag::LaneRange;
    }
    app.request_redraw();
    return true;
}

bool AutomationView::on_wheel(ui::App& app, int dx, int dy) {
    (void)dx;
    if (dy == 0) return false;
    if (popupLevel_ >= 0) {
        popupScroll_ = clampi(popupScroll_ - dy*3, 0,
                              std::max(0,(int)popupItems_.size()-kPopupMaxRows));
        app.request_redraw(); return true;
    }
    if (menuKind_ != MenuNone) {
        menuScroll_ = clampi(menuScroll_ - dy*3, 0, std::max(0,(int)menuRows_.size()-4));
        app.request_redraw(); return true;
    }
    const SDL_Keymod mods = (SDL_Keymod)SDL_GetModState();
    int mx = 0, my = 0;
    ui::mouse_logical(app, mx, my);
    const long len = seq_length();
    const int row = row_at_y(my);

    // Ctrl over a lane: that lane's VALUE axis.
    if ((mods & KMOD_CTRL) && row >= 0 && row < (int)laneUI_.size()) {
        LaneUI& u = laneUI_[(size_t)row];
        u.vzoom = clampf(u.vzoom * (dy > 0 ? 1.25f : 0.8f), 1.f, 16.f);
        app.request_redraw(); return true;
    }
    // Shift: scroll time instead of zooming it.
    if (mods & KMOD_SHIFT) {
        scroll_by((long)(-dy * visible_ticks(len) / 8));
        app.request_redraw(); return true;
    }
    // Plain wheel over the plot column: zoom time about the pointer.
    const SDL_Rect rl = ruler_rect();
    if (mx >= rl.x && mx < rl.x + rl.w && my >= rect.y + tbH_) {
        zoom_about(dy > 0 ? 1.25 : 0.8, mx);
        app.request_redraw(); return true;
    }
    // Otherwise scroll the lane stack.
    const int viewH = std::max(1, lanes_bottom() - lanes_top());
    if (content_h() <= viewH) return false;
    scrollY_ = clampi(scrollY_ - dy*24, 0, content_h() - viewH);
    app.request_redraw();
    return true;
}

bool AutomationView::on_key(ui::App& app, SDL_Keycode k) {
    const SDL_Keymod mods = (SDL_Keymod)SDL_GetModState();
    const bool ctrl  = (mods & KMOD_CTRL)  != 0;
    const bool shift = (mods & KMOD_SHIFT) != 0;
    const long len   = seq_length();

    if (ctrl) {
        switch (k) {
        // The shell routes Ctrl+Z globally when it has bound an undo hook; this
        // is the fallback (and the toolbar / menus reach the same history).
        case SDLK_z: if (shift) redo(); else undo(); app.request_redraw(); return true;
        case SDLK_y: redo(); app.request_redraw(); return true;
        case SDLK_c: copy_points();  app.request_redraw(); return true;
        case SDLK_v: paste_points(); app.request_redraw(); return true;
        case SDLK_a: select_all_lanes(); app.request_redraw(); return true;
        case SDLK_d: sel_.clear(); selPts_.clear(); app.request_redraw(); return true;
        default: break;
        }
    }

    switch (k) {
    case SDLK_ESCAPE:
        if (any_popup_open()) close_menus();
        else { sel_.clear(); selPts_.clear(); }
        app.request_redraw(); return true;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        begin_point_entry(app); return true;
    case SDLK_F2:
        begin_rename(app, selLane_); return true;
    case SDLK_i:  cycle_interp();  app.request_redraw(); return true;
    case SDLK_s:  snapOn_ = !snapOn_; app.request_redraw(); return true;
    case SDLK_f:
        if (shift) zoom_to_selection(); else zoom_to_fit();
        app.request_redraw(); return true;
    case SDLK_p:
        if (sel_.active) open_menu(app, MenuOps, rFx_.x, rect.y + tbH_ + 2, selLane_, 0);
        return true;
    case SDLK_DELETE: case SDLK_BACKSPACE:
        //  Delete CLEARS DATA.  It used to fall straight through to
        //  remove_lane() whenever selPts_ was empty -- and a range selection,
        //  the documented primary model, never fills selPts_ -- so dragging
        //  across bars 5..9 of a cutoff lane and pressing Delete threw away the
        //  lane, its target binding, its colour and every breakpoint it had.
        //  Removing a lane is a structural edit and now needs its own gesture.
        if (!selPts_.empty())      delete_points();
        else if (sel_.active)      delete_range();
        else if (shift)            remove_lane(selLane_);   // Shift+Delete
        app.request_redraw(); return true;

    // arrow nudge: Shift = fine (one tick / 1/512), otherwise snap / 1/64
    case SDLK_LEFT: case SDLK_RIGHT: {
        const long d = shift ? 1 : std::max<long>(1, snapOn_ ? snapTicks_ : beat_len());
        if (!selPts_.empty()) nudge_points(k == SDLK_LEFT ? -d : d, 0.f);
        else scroll_by(k == SDLK_LEFT ? -measure_len() : measure_len());
        app.request_redraw(); return true; }
    case SDLK_UP: case SDLK_DOWN: {
        const float d = shift ? (1.f/512.f) : (1.f/64.f);
        if (!selPts_.empty()) nudge_points(0, k == SDLK_UP ? d : -d);
        else scrollY_ = clampi(scrollY_ + (k == SDLK_UP ? -24 : 24), 0,
                               std::max(0, content_h() - (lanes_bottom()-lanes_top())));
        app.request_redraw(); return true; }

    case SDLK_HOME: viewStartTick_ = 0; clamp_view(len); app.request_redraw(); return true;
    case SDLK_END:  viewStartTick_ = max_view_start(len); app.request_redraw(); return true;
    case SDLK_PAGEUP: case SDLK_PAGEDOWN: {
        const int page = std::max(24, lanes_bottom() - lanes_top() - 16);
        scrollY_ = clampi(scrollY_ + (k == SDLK_PAGEUP ? -page : page), 0,
                          std::max(0, content_h() - (lanes_bottom()-lanes_top())));
        app.request_redraw(); return true; }
    default: break;
    }
    return false;
}

// ===========================================================================
//  KeyFollowPanel
// ===========================================================================

sequence* KeyFollowPanel::seq_of(int seqNum) const {
    if (!perf_ || !perf_->is_active(seqNum)) return nullptr;
    return perf_->get_sequence(seqNum);
}

bool KeyFollowPanel::draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                              bool active, bool enabled) const {
    const Theme& t = theme();
    fill_rect(app.ren, r, active ? t.accent : t.panel);
    frame_rect(app.ren, r, t.dim);
    const Color fg = active ? t.bg : (enabled ? t.text : t.dim);
    app.font.draw_centered(app.ren, r, fit_text(app.font, s, r.w-4), fg);
    return true;
}

void KeyFollowPanel::draw(ui::App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.panel);
    frame_rect(app.ren, rect, t.dim);

    headH_ = app.font.ch() + 10;
    pickH_ = app.font.ch() + 12;
    rowH_  = app.font.ch() + 10;

    app.font.draw(app.ren, rect.x + 6, rect.y + 5, "KEYFOLLOW", t.text);
    const int master = perf_ ? perf_->get_scale_master() : -1;
    if (master >= 0) {
        char mb[24]; std::snprintf(mb, sizeof(mb), "master:T%d", master + 1);
        app.font.draw(app.ren, rect.x + rect.w - app.font.text_w(mb) - 6,
                      rect.y + 5, mb, t.accent);
    }
    hline(app.ren, rect.x, rect.x + rect.w, rect.y + headH_, t.dim);

    //  WHOSE key/scale do these pickers edit?  perform::play() reads
    //  m_master_key / m_master_scale off the MASTER sequence and nothing else
    //  (perform.cpp:753-754); a follower's own copy is never looked at.  So
    //  editing the FOCUSED row's values -- which is what this did -- changed
    //  nothing audible unless that row happened to be the master: the reading
    //  moved on screen and not one note shifted, with no hint why.  Point them
    //  at the master whenever there is one (which is also what the "master:Tn"
    //  tag next to them already claims), and fall back to the focused row so a
    //  track's key can still be preset before it is made master.
    pickSeq_ = (master >= 0 && seq_of(master)) ? master : focus_;
    sequence* fs = seq_of(pickSeq_);
    const int py = rect.y + headH_ + 3;
    const int key   = clampi(fs ? fs->get_master_key()   : 0, 0, 11);
    const int scale = clampi(fs ? fs->get_master_scale() : c_scale_off, 0, c_scale_size-1);

    const int bw = app.font.cw()*2 + 6;
    int x = rect.x + 6;
    app.font.draw(app.ren, x, py + 3, "Key", t.dim); x += app.font.cw()*4;
    rKeyPrev_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 2;
    const SDL_Rect keyName{ x, py, app.font.cw()*4, pickH_-6 }; x += app.font.cw()*4 + 2;
    rKeyNext_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 10;
    draw_box(app, rKeyPrev_, "<", false, fs != nullptr);
    draw_box(app, keyName, c_key_text[key], false, fs != nullptr);
    draw_box(app, rKeyNext_, ">", false, fs != nullptr);

    app.font.draw(app.ren, x, py + 3, "Scl", t.dim); x += app.font.cw()*4;
    rScalePrev_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 2;
    const SDL_Rect scName{ x, py, app.font.cw()*7, pickH_-6 }; x += app.font.cw()*7 + 2;
    rScaleNext_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 6;
    draw_box(app, rScalePrev_, "<", false, fs != nullptr);
    draw_box(app, scName, c_scales_text[scale], false, fs != nullptr);
    draw_box(app, rScaleNext_, ">", false, fs != nullptr);
    // ... and say out loud which track they are writing to, so the pickers are
    // never a control whose effect you have to guess at.
    if (fs) {
        char tag[16]; std::snprintf(tag, sizeof(tag), "T%d", pickSeq_ + 1);
        if (x + app.font.text_w(tag) <= rect.x + rect.w - 4)
            app.font.draw(app.ren, x, py + 3, tag,
                          pickSeq_ == master ? t.accent : t.dim);
    }

    hline(app.ren, rect.x, rect.x + rect.w, py + pickH_, t.dim);

    rows_.clear();
    int ry = py + pickH_ + 2;
    const int smW = app.font.cw()*4 + 6, fmW = app.font.cw()*4 + 6;
    for (int s : seqs_) {
        if (ry + rowH_ > rect.y + rect.h) break;
        const SDL_Rect rr{ rect.x, ry, rect.w, rowH_ };
        const bool focused = (s == focus_);
        if (focused) fill_rect(app.ren, SDL_Rect{rr.x+1,rr.y,rr.w-2,rr.h}, t.sel);

        sequence* sq = seq_of(s);
        char lbl[64];
        if (sq) {
            const std::string nm = sq->get_name() ? sq->get_name() : "";
            std::snprintf(lbl, sizeof(lbl), "T%d %s", s + 1, nm.c_str());
        } else std::snprintf(lbl, sizeof(lbl), "T%d --", s + 1);
        app.font.draw(app.ren, rr.x + 6, rr.y + 4,
                      fit_text(app.font, lbl, rr.w - smW - fmW - 20),
                      focused ? t.bg : (sq ? t.text : t.dim));

        const bool isMaster = (perf_ && perf_->get_scale_master() == s);
        const bool follows  = sq ? sq->get_follows_master() : false;
        const SDL_Rect sm{ rr.x + rr.w - smW - fmW - 12, rr.y + 2, smW, rowH_ - 4 };
        const SDL_Rect fm{ rr.x + rr.w - fmW - 6,        rr.y + 2, fmW, rowH_ - 4 };
        draw_box(app, sm, "SM", isMaster, sq != nullptr);
        draw_box(app, fm, "FM", follows,  sq != nullptr);

        rows_.push_back(RowRect{ rr, sm, fm, s });
        ry += rowH_;
    }
}

bool KeyFollowPanel::on_mouse(ui::App& app, const ui::MouseEv& e) {
    if (!e.pressed || !hit(e.x, e.y)) return false;
    if (!perf_) return true;

    // pickSeq_, not focus_: the pickers edit the scale MASTER's key/scale (see
    // draw()) -- the only ones perform::play() ever reads.
    if (sequence* fs = seq_of(pickSeq_)) {
        if (in_rect(rKeyPrev_, e.x, e.y))  { fs->set_master_key((fs->get_master_key()+11)%12); app.request_redraw(); return true; }
        if (in_rect(rKeyNext_, e.x, e.y))  { fs->set_master_key((fs->get_master_key()+1)%12);  app.request_redraw(); return true; }
        if (in_rect(rScalePrev_, e.x, e.y)){ fs->set_master_scale((fs->get_master_scale()+c_scale_size-1)%c_scale_size); app.request_redraw(); return true; }
        if (in_rect(rScaleNext_, e.x, e.y)){ fs->set_master_scale((fs->get_master_scale()+1)%c_scale_size); app.request_redraw(); return true; }
    }
    for (const RowRect& rr : rows_) {
        if (in_rect(rr.sm, e.x, e.y)) {
            const int cur = perf_->get_scale_master();
            perf_->set_scale_master(cur == rr.seq ? -1 : rr.seq);
            app.request_redraw(); return true;
        }
        if (in_rect(rr.fm, e.x, e.y)) {
            if (sequence* sq = seq_of(rr.seq))
                perf_->set_follows_master(rr.seq, !sq->get_follows_master());
            app.request_redraw(); return true;
        }
        if (in_rect(rr.row, e.x, e.y)) { focus_ = rr.seq; app.request_redraw(); return true; }
    }
    return true;
}

// ==== TAIL ====


// ---------------------------------------------------------------------------
//  Completions for declarations the rewrite left without definitions.
// ---------------------------------------------------------------------------

//! Shut every transient overlay.  One place, so a new popup cannot be forgotten
//! by a caller that only knew about the two that existed when it was written.
void AutomationView::close_menus() {
    menuKind_   = MenuNone;
    menuRow_    = -1;
    promptOpen_ = false;
    popupLevel_ = -1;
    popupItems_.clear();
}

bool AutomationView::any_popup_open() const {
    return menuKind_ != MenuNone || promptOpen_ || popupLevel_ >= 0;
}

//! Display name for a lane's target.  Prefers the hierarchical name the picker
//! already resolved (so a rack lane reads "Module > Param" rather than a bare
//! id); falls back to the raw target when the inventory no longer lists it,
//! which happens whenever a rack module is deleted under a live lane.
std::string AutomationView::lane_name(int row) const {
    AutomationLane* L = lane(row);
    if (!L) return std::string();
    const LaneTarget& t = L->target();
    if (t.kind == LaneTargetKind::MidiCC)
        return std::string("CC ") + std::to_string(t.id);
    for (const TargetChoice& c : targets_) {
        if (!(c.target == t)) continue;
        std::string s = c.parameter.empty() ? c.node : c.parameter;
        if (!c.device.empty()) s = c.device + " > " + s;
        return s;
    }
    return std::string("Param ") + std::to_string(t.id);
}

//! focus_ is a SEQUENCE NUMBER, not an index into seqs_.  Clamping it as an
//! index is what made the panel jump to an unrelated track whenever the set of
//! tracks changed; keep it pointing at a track that still exists instead.
void KeyFollowPanel::clamp_focus() {
    if (seqs_.empty()) { focus_ = 0; return; }
    for (size_t i = 0; i < seqs_.size(); ++i)
        if (seqs_[i] == focus_) return;
    focus_ = seqs_.front();
}

} // namespace automation
