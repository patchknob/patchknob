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

using namespace ui;
using seq24::engine::AutomationPlayer;
using seq24::engine::AutomationTrack;
using seq24::engine::AutomationLane;
using seq24::engine::LaneTarget;
using seq24::engine::LaneTargetKind;
using seq24::engine::Interpolation;

namespace automation {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static inline int   clampi(int v, int lo, int hi) { return v<lo?lo:(v>hi?hi:v); }
static inline float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v);}
static inline long  clampl(long v, long lo, long hi){ return v<lo?lo:(v>hi?hi:v); }

static std::string clip_text(const std::string& s, int cells) {
    if (cells <= 0) return std::string();
    if ((int)s.size() <= cells) return s;
    return s.substr(0, (size_t)cells);
}

static const char* interp_name(Interpolation i) {
    switch (i) {
        case Interpolation::Step: return "STEP";
        case Interpolation::Hold: return "HOLD";
        case Interpolation::Linear: default: return "LIN";
    }
}

// ===========================================================================
//  AutomationView
// ===========================================================================
AutomationView::AutomationView(sequence* seq, int track, AutomationPlayer* player)
    : seq_(seq), track_(track), player_(player) {
    refresh_params();
}

void AutomationView::set_target(sequence* seq, int track) {
    seq_ = seq; track_ = track;
    selLane_ = -1; dragging_ = false; dragLane_ = dragBp_ = -1; scrollY_ = 0;
    refresh_params();
}

void AutomationView::refresh_params() {
    params_.clear();
    const int n = seq24::app::audio_app_track_param_count(track_);
    for (int i = 0; i < n; ++i) {
        unsigned int id = 0; float def = 0.0f; char nm[64] = {0};
        if (seq24::app::audio_app_track_param_info(track_, i, &id, nm, (int)sizeof(nm), &def))
            params_.push_back(ParamRef{ id, std::string(nm) });
    }
    if (paramSel_ >= (int)params_.size()) paramSel_ = 0;
}

// --- model access ----------------------------------------------------------
AutomationTrack* AutomationView::atrack() const {
    if (!player_) return nullptr;
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
    long L = seq_ ? seq_->get_length() : 0;
    if (L < 1) L = 4L * 4 * c_ppqn;     // default 4 bars @ 4/4
    return L;
}

// --- layout / mapping ------------------------------------------------------
void AutomationView::layout(ui::App& app) {
    tbH_     = app.font.ch() + 12;
    gutterW_ = std::max(110, app.font.cw() * 14);

    const int y = rect.y + 4;
    int x = rect.x + 6;
    auto seg = [&](SDL_Rect& r, int w) {
        r = SDL_Rect{ x, y, w, tbH_ - 8 };
        x += w + 4;
    };
    seg(rType_,  app.font.cw()*7);      // [PARAM]/[CC]
    seg(rPrev_,  app.font.cw()*2 + 6);  // <
    seg(rName_,  app.font.cw()*16);     // target name / number
    seg(rNext_,  app.font.cw()*2 + 6);  // >
    seg(rAdd_,   app.font.cw()*7);      // + Lane
    seg(rInterp_,app.font.cw()*6);      // LIN/STEP/HOLD

    // lane row height: fit all lanes when few, else fixed with scroll.
    const int avail = rect.h - tbH_ - 4;
    const int n = lane_count();
    if (n <= 0)      laneH_ = 64;
    else             laneH_ = clampi(avail / n, 44, 120);
}

SDL_Rect AutomationView::gutter_rect(int row) const {
    const int y0 = rect.y + tbH_ + 2;
    const int top = y0 + row * laneH_ - scrollY_;
    return SDL_Rect{ rect.x, top, gutterW_, laneH_ - 2 };
}
SDL_Rect AutomationView::plot_rect(int row) const {
    const int y0 = rect.y + tbH_ + 2;
    const int top = y0 + row * laneH_ - scrollY_;
    return SDL_Rect{ rect.x + gutterW_ + 2, top + 3,
                     rect.w - gutterW_ - 8, laneH_ - 8 };
}
int AutomationView::x_at_tick(const SDL_Rect& p, long tick, long len) const {
    if (len < 1) len = 1;
    double f = (double)clampl(tick,0,len) / (double)len;
    return p.x + (int)std::lround(f * (p.w - 1));
}
long AutomationView::tick_at_x(const SDL_Rect& p, int x, long len) const {
    if (p.w <= 1) return 0;
    double f = (double)(x - p.x) / (double)(p.w - 1);
    return clampl((long)std::lround(f * len), 0, len);
}
int AutomationView::y_at_val(const SDL_Rect& p, float v) const {
    return p.y + (int)std::lround((1.0f - clampf(v,0,1)) * (p.h - 1));
}
float AutomationView::val_at_y(const SDL_Rect& p, int y) const {
    if (p.h <= 1) return 0.0f;
    return clampf(1.0f - (float)(y - p.y) / (float)(p.h - 1), 0.0f, 1.0f);
}

// nearest breakpoint index within an 8px pick radius, or -1.
int AutomationView::pick_point(AutomationLane* L, const SDL_Rect& p, long len,
                               int px, int py) const {
    if (!L) return -1;
    const int R = 8;
    int best = -1; double bestD = (double)(R*R) + 1.0;
    const auto& bps = L->breakpoints();
    for (int i = 0; i < (int)bps.size(); ++i) {
        int bx = x_at_tick(p, (long)bps[(size_t)i].tick, len);
        int by = y_at_val(p, bps[(size_t)i].value);
        double d = (double)(bx-px)*(bx-px) + (double)(by-py)*(by-py);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// --- editing ---------------------------------------------------------------
void AutomationView::add_lane_for_picker() {
    AutomationTrack* at = atrack();
    if (!at) return;
    LaneTarget tgt;
    if (pickCC_) { tgt.kind = LaneTargetKind::MidiCC;   tgt.id = (unsigned)ccSel_; }
    else {
        if (params_.empty()) return;                     // nothing to bind
        tgt.kind = LaneTargetKind::VstParam; tgt.id = params_[(size_t)paramSel_].id;
    }
    at->laneForTarget(tgt, Interpolation::Linear);
    // select whichever lane now drives that target
    for (int i = 0; i < at->laneCount(); ++i)
        if (at->lane(i).target() == tgt) { selLane_ = i; break; }
}

void AutomationView::cycle_interp() {
    AutomationLane* L = lane(selLane_);
    if (!L) return;
    switch (L->interpolation()) {
        case Interpolation::Linear: L->setInterpolation(Interpolation::Step); break;
        case Interpolation::Step:   L->setInterpolation(Interpolation::Hold); break;
        case Interpolation::Hold: default: L->setInterpolation(Interpolation::Linear); break;
    }
}

std::string AutomationView::target_label(const AutomationLane* L) const {
    if (!L) return std::string();
    char buf[48];
    if (L->target().kind == LaneTargetKind::MidiCC) {
        std::snprintf(buf, sizeof(buf), "CC %u", L->target().id);
        return buf;
    }
    // VST param: try to resolve a friendly name from the picker cache.
    for (const auto& pr : params_)
        if (pr.id == L->target().id) return "P:" + pr.name;
    std::snprintf(buf, sizeof(buf), "P#%u", L->target().id);
    return buf;
}

std::string AutomationView::picker_label() const {
    if (pickCC_) { char b[24]; std::snprintf(b,sizeof(b),"CC %d",ccSel_); return b; }
    if (params_.empty()) return "(no params)";
    return params_[(size_t)clampi(paramSel_,0,(int)params_.size()-1)].name;
}

// --- drawing ---------------------------------------------------------------
bool AutomationView::draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                              bool active, bool enabled) const {
    const Theme& t = theme();
    fill_rect(app.ren, r, active ? t.accent : t.panel);
    frame_rect(app.ren, r, t.dim);
    Color fg = active ? t.bg : (enabled ? t.text : t.dim);
    app.font.draw_centered(app.ren, r, clip_text(s, r.w / std::max(1,app.font.cw())), fg);
    return true;
}

void AutomationView::draw_toolbar(ui::App& app) {
    const Theme& t = theme();
    SDL_Rect bar{ rect.x, rect.y, rect.w, tbH_ };
    fill_rect(app.ren, bar, t.panel);
    hline(app.ren, rect.x, rect.x + rect.w, rect.y + tbH_, t.dim);

    draw_box(app, rType_, pickCC_ ? "CC" : "PARAM", false);
    draw_box(app, rPrev_, "<", false);
    draw_box(app, rName_, picker_label(), false, pickCC_ || !params_.empty());
    draw_box(app, rNext_, ">", false);
    draw_box(app, rAdd_,  "+Lane", false, pickCC_ || !params_.empty());

    AutomationLane* sel = lane(selLane_);
    draw_box(app, rInterp_, sel ? interp_name(sel->interpolation()) : "-",
             false, sel != nullptr);

    // track label at the far right of the toolbar
    char tl[32]; std::snprintf(tl, sizeof(tl), "AUTO  T%d", track_ + 1);
    int tw = app.font.text_w(tl);
    app.font.draw(app.ren, rect.x + rect.w - tw - 8,
                  rect.y + (tbH_ - app.font.ch())/2, tl, t.dim);
}

void AutomationView::draw_lane(ui::App& app, int row) {
    const Theme& t = theme();
    AutomationLane* L = lane(row);
    if (!L) return;
    const long len = seq_length();

    SDL_Rect g = gutter_rect(row);
    SDL_Rect p = plot_rect(row);

    // --- gutter ---
    bool selected = (row == selLane_);
    fill_rect(app.ren, g, selected ? t.sel : t.panel);
    frame_rect(app.ren, g, t.dim);
    app.font.draw(app.ren, g.x + 6, g.y + 5, clip_text(target_label(L), (g.w-30)/std::max(1,app.font.cw())),
                  selected ? t.bg : t.text);
    // interp + point count
    char sub[40];
    std::snprintf(sub, sizeof(sub), "%s  %dpt", interp_name(L->interpolation()), L->size());
    app.font.draw(app.ren, g.x + 6, g.y + 5 + app.font.ch() + 3,
                  clip_text(sub, (g.w-12)/std::max(1,app.font.cw())),
                  selected ? t.bg : t.dim);
    // remove [x] box (top-right of gutter)
    SDL_Rect x{ g.x + g.w - 18, g.y + 4, 14, 14 };
    draw_box(app, x, "x", false);

    // --- plot background + grid ---
    fill_rect(app.ren, p, t.keybg);
    frame_rect(app.ren, p, t.dim);
    // mid line (value 0.5)
    hline(app.ren, p.x, p.x + p.w, p.y + p.h/2, t.dim);
    // beat / bar grid (skip if it would be too dense)
    long beats = len / c_ppqn;
    if (beats > 0 && beats <= 96) {
        for (long b = 1; b < beats; ++b) {
            int gx = x_at_tick(p, b * c_ppqn, len);
            bool bar = (b % 4 == 0);
            vline(app.ren, gx, p.y + 1, p.y + p.h - 1, bar ? t.scale : t.dim);
        }
    }

    // --- envelope line ---
    const auto& bps = L->breakpoints();
    Interpolation ip = L->interpolation();
    if (bps.empty()) {
        app.font.draw(app.ren, p.x + 6, p.y + p.h/2 - app.font.ch()/2,
                      "click to add points", t.dim);
    } else {
        set_color(app.ren, t.accent);
        // leading flat segment
        int fx = x_at_tick(p, (long)bps.front().tick, len);
        int fy = y_at_val(p, bps.front().value);
        if (fx > p.x) { hline(app.ren, p.x, fx, fy, t.accent); }
        // segments between points
        for (int i = 0; i + 1 < (int)bps.size(); ++i) {
            int ax = x_at_tick(p, (long)bps[(size_t)i].tick, len);
            int ay = y_at_val(p, bps[(size_t)i].value);
            int bx = x_at_tick(p, (long)bps[(size_t)i+1].tick, len);
            int by = y_at_val(p, bps[(size_t)i+1].value);
            if (ip == Interpolation::Step) {
                hline(app.ren, ax, bx, ay, t.accent);
                vline(app.ren, bx, std::min(ay,by), std::max(ay,by), t.accent);
            } else if (ip == Interpolation::Hold) {
                vline(app.ren, ax, std::min(ay,by), std::max(ay,by), t.accent);
                hline(app.ren, ax, bx, by, t.accent);
            } else { // Linear: diagonal (2px weight)
                set_color(app.ren, t.accent);
                SDL_RenderDrawLine(app.ren, ax, ay, bx, by);
                SDL_RenderDrawLine(app.ren, ax, ay+1, bx, by+1);
            }
        }
        // trailing flat segment
        int lx = x_at_tick(p, (long)bps.back().tick, len);
        int ly = y_at_val(p, bps.back().value);
        if (lx < p.x + p.w) hline(app.ren, lx, p.x + p.w, ly, t.accent);

        // --- breakpoint handles ---
        for (int i = 0; i < (int)bps.size(); ++i) {
            int bx = x_at_tick(p, (long)bps[(size_t)i].tick, len);
            int by = y_at_val(p, bps[(size_t)i].value);
            SDL_Rect h{ bx - 3, by - 3, 7, 7 };
            bool hot = (dragging_ && row == dragLane_ && i == dragBp_);
            Color c = hot ? t.hi : (selected ? t.notesel : t.note);
            fill_rect(app.ren, h, c);
            frame_rect(app.ren, h, t.bg);
        }
    }
}

void AutomationView::draw(ui::App& app) {
    layout(app);
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);

    const int n = lane_count();
    if (n == 0) {
        app.font.draw_centered(app.ren,
            SDL_Rect{ rect.x, rect.y + tbH_, rect.w, rect.h - tbH_ },
            "no lanes -- pick a target above and press +Lane", t.dim);
    } else {
        // clip lane drawing to the area under the toolbar so scrolled rows do
        // not bleed over it.
        SDL_Rect clip{ rect.x, rect.y + tbH_ + 1, rect.w, rect.h - tbH_ - 1 };
        SDL_RenderSetClipRect(app.ren, &clip);
        for (int i = 0; i < n; ++i) {
            SDL_Rect g = gutter_rect(i);
            if (g.y + laneH_ < clip.y || g.y > clip.y + clip.h) continue; // cull
            draw_lane(app, i);
        }
        SDL_RenderSetClipRect(app.ren, nullptr);
    }

    draw_toolbar(app);                 // last, on top (never scrolled)
    frame_rect(app.ren, rect, t.dim);
}

// --- input -----------------------------------------------------------------
bool AutomationView::on_mouse(ui::App& app, const ui::MouseEv& e) {
    // release: end any drag (may arrive with the pointer outside the view).
    if (!e.pressed) {
        if (dragging_) { dragging_ = false; app.request_redraw(); return true; }
        return false;
    }

    // motion while dragging (button-mask synthesizes pressed=true motions).
    if (dragging_) {
        AutomationLane* L = lane(dragLane_);
        if (L) {
            SDL_Rect p = plot_rect(dragLane_);
            long len = seq_length();
            long tk  = tick_at_x(p, e.x, len);
            float v  = val_at_y(p, e.y);
            dragBp_ = L->move(dragBp_, tk, v);
            app.request_redraw();
        }
        return true;
    }

    if (!hit(e.x, e.y)) return false;

    // toolbar hits
    auto in = [&](const SDL_Rect& r){ return e.x>=r.x&&e.x<r.x+r.w&&e.y>=r.y&&e.y<r.y+r.h; };
    if (e.y < rect.y + tbH_) {
        if (in(rType_)) { pickCC_ = !pickCC_; app.request_redraw(); return true; }
        if (in(rPrev_)) {
            if (pickCC_) ccSel_ = clampi(ccSel_-1, 0, 127);
            else if (!params_.empty()) paramSel_ = (paramSel_ + (int)params_.size()-1) % (int)params_.size();
            app.request_redraw(); return true;
        }
        if (in(rNext_)) {
            if (pickCC_) ccSel_ = clampi(ccSel_+1, 0, 127);
            else if (!params_.empty()) paramSel_ = (paramSel_ + 1) % (int)params_.size();
            app.request_redraw(); return true;
        }
        if (in(rAdd_))    { add_lane_for_picker(); app.request_redraw(); return true; }
        if (in(rInterp_)) { cycle_interp();        app.request_redraw(); return true; }
        return true;    // swallow other toolbar clicks
    }

    // which lane row?
    const int n = lane_count();
    const int y0 = rect.y + tbH_ + 2;
    int row = (e.y - y0 + scrollY_) / std::max(1, laneH_);
    if (row < 0 || row >= n) return true;

    // gutter interaction
    SDL_Rect g = gutter_rect(row);
    if (e.x < g.x + g.w) {
        SDL_Rect xbox{ g.x + g.w - 18, g.y + 4, 14, 14 };
        if (in(xbox)) {                             // remove whole lane
            AutomationTrack* at = atrack();
            if (at) at->removeLane(row);
            if (selLane_ == row) selLane_ = -1;
            else if (selLane_ > row) --selLane_;
        } else {
            selLane_ = row;                         // select
        }
        app.request_redraw();
        return true;
    }

    // plot interaction
    AutomationLane* L = lane(row);
    if (!L) return true;
    SDL_Rect p = plot_rect(row);
    long len = seq_length();
    int bp = pick_point(L, p, len, e.x, e.y);

    if (e.button == SDL_BUTTON_RIGHT) {
        if (bp >= 0) { L->removeAt(bp); app.request_redraw(); }
        return true;
    }

    selLane_ = row;
    if (bp >= 0) {                                  // grab existing point
        dragging_ = true; dragLane_ = row; dragBp_ = bp;
    } else {                                        // add + grab new point
        long tk = tick_at_x(p, e.x, len);
        float v = val_at_y(p, e.y);
        dragging_ = true; dragLane_ = row; dragBp_ = L->add(tk, v);
    }
    app.request_redraw();
    return true;
}

bool AutomationView::on_wheel(ui::App& app, int dx, int dy) {
    (void)dx;
    const int n = lane_count();
    const int contentH = n * laneH_;
    const int viewH = rect.h - tbH_ - 2;
    if (contentH <= viewH) return false;
    scrollY_ = clampi(scrollY_ - dy * 24, 0, contentH - viewH);
    app.request_redraw();
    return true;
}

bool AutomationView::on_key(ui::App& app, SDL_Keycode k) {
    if (k == SDLK_i) { cycle_interp(); app.request_redraw(); return true; }
    if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
        AutomationTrack* at = atrack();
        if (at && selLane_ >= 0 && selLane_ < at->laneCount()) {
            at->removeLane(selLane_);
            selLane_ = -1;
            app.request_redraw();
        }
        return true;
    }
    return false;
}

// ===========================================================================
//  KeyFollowPanel
// ===========================================================================
void KeyFollowPanel::clamp_focus() {
    if (seqs_.empty()) { focus_ = 0; return; }
    // keep focus_ pointing at a seq that is in the shown list
    for (int s : seqs_) if (s == focus_) return;
    focus_ = seqs_.front();
}
void KeyFollowPanel::set_focus(int seqNum) { focus_ = seqNum; }

sequence* KeyFollowPanel::seq_of(int seqNum) const {
    if (!perf_ || !perf_->is_active(seqNum)) return nullptr;
    return perf_->get_sequence(seqNum);
}

bool KeyFollowPanel::draw_box(ui::App& app, const SDL_Rect& r, const std::string& s,
                              bool active, bool enabled) const {
    const Theme& t = theme();
    fill_rect(app.ren, r, active ? t.accent : t.panel);
    frame_rect(app.ren, r, t.dim);
    Color fg = active ? t.bg : (enabled ? t.text : t.dim);
    app.font.draw_centered(app.ren, r, s, fg);
    return true;
}

void KeyFollowPanel::draw(ui::App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.panel);
    frame_rect(app.ren, rect, t.dim);

    headH_ = app.font.ch() + 10;
    pickH_ = app.font.ch() + 12;
    rowH_  = app.font.ch() + 10;

    // header
    app.font.draw(app.ren, rect.x + 6, rect.y + 5, "KEYFOLLOW", t.text);
    int master = perf_ ? perf_->get_scale_master() : -1;
    if (master >= 0) {
        char mb[24]; std::snprintf(mb, sizeof(mb), "master:T%d", master + 1);
        int tw = app.font.text_w(mb);
        app.font.draw(app.ren, rect.x + rect.w - tw - 6, rect.y + 5, mb, t.accent);
    }
    hline(app.ren, rect.x, rect.x + rect.w, rect.y + headH_, t.dim);

    // master root / scale pickers (act on the focused seq)
    sequence* fs = seq_of(focus_);
    int py = rect.y + headH_ + 3;
    int key   = fs ? fs->get_master_key()   : 0;
    int scale = fs ? fs->get_master_scale() : c_scale_off;
    key   = clampi(key, 0, 11);
    scale = clampi(scale, 0, c_scale_size - 1);

    int bw = app.font.cw()*2 + 6;
    int x = rect.x + 6;
    app.font.draw(app.ren, x, py + 3, "Key", t.dim); x += app.font.cw()*4;
    rKeyPrev_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 2;
    SDL_Rect keyName{ x, py, app.font.cw()*4, pickH_-6 }; x += app.font.cw()*4 + 2;
    rKeyNext_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 10;
    draw_box(app, rKeyPrev_, "<", false, fs != nullptr);
    draw_box(app, keyName, c_key_text[key], false, fs != nullptr);
    draw_box(app, rKeyNext_, ">", false, fs != nullptr);

    app.font.draw(app.ren, x, py + 3, "Scl", t.dim); x += app.font.cw()*4;
    rScalePrev_ = SDL_Rect{ x, py, bw, pickH_-6 }; x += bw + 2;
    SDL_Rect scName{ x, py, app.font.cw()*7, pickH_-6 }; x += app.font.cw()*7 + 2;
    rScaleNext_ = SDL_Rect{ x, py, bw, pickH_-6 };
    draw_box(app, rScalePrev_, "<", false, fs != nullptr);
    draw_box(app, scName, c_scales_text[scale], false, fs != nullptr);
    draw_box(app, rScaleNext_, ">", false, fs != nullptr);

    hline(app.ren, rect.x, rect.x + rect.w, py + pickH_, t.dim);

    // per-track rows
    rows_.clear();
    int ry = py + pickH_ + 2;
    int smW = app.font.cw()*4 + 6;
    int fmW = app.font.cw()*4 + 6;
    for (int s : seqs_) {
        if (ry + rowH_ > rect.y + rect.h) break;
        SDL_Rect rr{ rect.x, ry, rect.w, rowH_ };
        bool focused = (s == focus_);
        if (focused) fill_rect(app.ren, SDL_Rect{rr.x+1,rr.y,rr.w-2,rr.h}, t.sel);

        sequence* sq = seq_of(s);
        char lbl[40];
        if (sq) {
            std::string nm = sq->get_name() ? sq->get_name() : "";
            std::snprintf(lbl, sizeof(lbl), "T%d %s", s + 1, nm.c_str());
        } else std::snprintf(lbl, sizeof(lbl), "T%d --", s + 1);
        app.font.draw(app.ren, rr.x + 6, rr.y + 4,
                      clip_text(lbl, (rr.w - smW - fmW - 20)/std::max(1,app.font.cw())),
                      focused ? t.bg : (sq ? t.text : t.dim));

        bool isMaster = (perf_ && perf_->get_scale_master() == s);
        bool follows  = sq ? sq->get_follows_master() : false;
        SDL_Rect sm{ rr.x + rr.w - smW - fmW - 12, rr.y + 2, smW, rowH_ - 4 };
        SDL_Rect fm{ rr.x + rr.w - fmW - 6,        rr.y + 2, fmW, rowH_ - 4 };
        draw_box(app, sm, "SM", isMaster, sq != nullptr);
        draw_box(app, fm, "FM", follows,  sq != nullptr);

        rows_.push_back(RowRect{ rr, sm, fm, s });
        ry += rowH_;
    }
}

bool KeyFollowPanel::on_mouse(ui::App& app, const ui::MouseEv& e) {
    if (!e.pressed || !hit(e.x, e.y)) return false;
    if (!perf_) return true;
    auto in = [&](const SDL_Rect& r){ return e.x>=r.x&&e.x<r.x+r.w&&e.y>=r.y&&e.y<r.y+r.h; };

    sequence* fs = seq_of(focus_);
    if (fs) {
        if (in(rKeyPrev_))  { fs->set_master_key((fs->get_master_key()+11)%12); app.request_redraw(); return true; }
        if (in(rKeyNext_))  { fs->set_master_key((fs->get_master_key()+1)%12);  app.request_redraw(); return true; }
        if (in(rScalePrev_)){ fs->set_master_scale((fs->get_master_scale()+c_scale_size-1)%c_scale_size); app.request_redraw(); return true; }
        if (in(rScaleNext_)){ fs->set_master_scale((fs->get_master_scale()+1)%c_scale_size); app.request_redraw(); return true; }
    }

    for (const auto& rr : rows_) {
        if (in(rr.sm)) {
            int cur = perf_->get_scale_master();
            perf_->set_scale_master(cur == rr.seq ? -1 : rr.seq);
            app.request_redraw(); return true;
        }
        if (in(rr.fm)) {
            sequence* sq = seq_of(rr.seq);
            if (sq) perf_->set_follows_master(rr.seq, !sq->get_follows_master());
            app.request_redraw(); return true;
        }
        if (in(rr.row)) { focus_ = rr.seq; app.request_redraw(); return true; }
    }
    return true;
}

} // namespace automation
