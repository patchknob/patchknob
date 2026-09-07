//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_protools.cpp
//
//  Pro Tools Reference Guide ch.29 (Edit Modes and Tools) + ch.30 (Making
//  Selections) ported onto ArrangeView.  This file carries the NEW machinery:
//
//    * the four Edit modes (Shuffle / Slip / Spot / Grid) with Snap-To-Grid,
//      Relative Grid, Shuffle Lock and the Spot dialog,
//    * the Grid value configuration (dotted / triplet, time-scale choice,
//      Clips/Markers magnetic grid, Follow Main Time Scale),
//    * the extended tool set state (Zoomer / Trim / Selector / Grabber /
//      Scrubber / Pencil / Smart) with per-tool sub-modes,
//    * the zoom system: previous-zoom, overview scale, five zoom presets,
//      Zoom Toggle with its documented preferences,
//    * ch.30 selections: the edit-selection model, link toggles, tab to
//      transients (real onset detection over the region audio), nudging,
//      counters / Edit Selection indicators with typed + calculator entry,
//    * the Universe view strip.
//
//  Gesture routing (press / drag / release) stays in arrange_view.cpp; the
//  helpers here are the model + chrome it calls.  Same two-tone rules: every
//  colour from ui::theme().
//----------------------------------------------------------------------------
#include "arrange_view.h"
#include "perform.h"
#include "sequence.h"
#include "globals.h"
#include "quantize.h"
#include "engine/audioclip/audio_clip.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace ui;

namespace arrange {

// Waveform-cache epoch: bumped by the Pencil's destructive sample redraw so
// draw_waveform() re-reduces the edited audio (defined in arrange_view.cpp).
extern int g_wave_epoch;
// Level-0 peak bins (256 frames/bin, channel 0) of the clip's cached waveform
// pyramid (defined in arrange_view.cpp); feeds the transient detector's skim.
long long wave_overview_ch0(const PatchKnob::engine::AudioClip* clip,
                            const float** mn, const float** mx);

//----------------------------------------------------------------------------
//  local copies of the tiny statics arrange_view.cpp keeps file-private
//----------------------------------------------------------------------------
static SDL_Rect pt_clamp_popup(SDL_Rect box, const SDL_Rect& bounds)
{
    if (box.w > bounds.w) box.w = bounds.w;
    if (box.h > bounds.h) box.h = bounds.h;
    if (box.x + box.w > bounds.x + bounds.w) box.x = bounds.x + bounds.w - box.w;
    if (box.y + box.h > bounds.y + bounds.h) box.y = bounds.y + bounds.h - box.h;
    if (box.x < bounds.x) box.x = bounds.x;
    if (box.y < bounds.y) box.y = bounds.y;
    return box;
}

static std::string pt_fit(const ui::Font& font, std::string text, int maxw)
{
    if (maxw <= 0) return "";
    if (font.text_w(text) <= maxw) return text;
    while (!text.empty() && font.text_w(text) > maxw) text.pop_back();
    return text;
}

// A small labelled chip button (topbar).  `engaged` fills it accent-on-bg.
static void pt_chip(App& app, SDL_Rect q, const char* label, bool engaged, bool hot)
{
    const Theme& t = theme();
    fill_rect (app.ren, q, engaged ? t.accent : (hot ? t.keybg : t.bg));
    frame_rect(app.ren, q, engaged ? t.hi : t.dim);
    app.mono.draw_centered(app.ren, q, label, engaged ? t.bg : (hot ? t.hi : t.text));
}

static bool pt_in(const SDL_Rect& r, int x, int y)
{
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

//----------------------------------------------------------------------------
//  EDIT MODES (ch.29 p671-673)
//----------------------------------------------------------------------------
const char* ArrangeView::mode_name(int m) const
{
    static const char* N[4] = { "SHUFFLE", "SLIP", "SPOT", "GRID" };
    return N[(m < 0 || m > 3) ? 1 : m];
}

void ArrangeView::set_edit_mode(EditMode m)
{
    if (m == EditMode::Shuffle && m_shuffle_lock) return;   // locked out
    m_edit_mode = m;
    if (m == EditMode::Grid) m_snap_to_grid = false;        // implied by the mode
}

// Mode-aware snap for CLIP edits: Grid snaps, Snap-To-Grid snaps the cursor /
// selections while the clip edit itself still follows the other mode -- but
// the manual treats clip edits under Snap-To-Grid as grid-placed too when the
// edit starts from a grid-constrained selection, so both routes snap here.
long ArrangeView::esnap(long tick) const
{
    if (m_edit_mode == EditMode::Grid || m_snap_to_grid) return snap(tick);
    return tick;
}

// Relative Grid: quantise a DELTA to grid increments (the clip keeps its
// offset from the grid; only the distance moved is grid-sized).
long ArrangeView::snap_rel(long delta) const
{
    const long g = m_snap > 0 ? m_snap : 1;
    if (g <= 1) return delta;
    const long half = g / 2;
    return ((delta >= 0 ? delta + half : delta - half) / g) * g;
}

// Shift every clip on the lane at/after `fromTick` by `delta`.  Audio clips
// move their region position; MIDI clips re-seat their triggers.  Gaps between
// clips ride along untouched (Shuffle preserves silence, per the manual).
void ArrangeView::ripple_lane(int laneSeq, long fromTick, long delta, int skipSeq)
{
    if (!m_perf || delta == 0) return;
    for (int cs : lane_sequences(laneSeq)) {
        if (cs == skipSeq) continue;
        if (m_audio.count(cs)) {
            AudioRegion& r = region_for(cs);
            if (r.position >= fromTick) {
                r.position = std::max<long>(0, r.position + delta);
                commit_region(cs);
            }
            continue;
        }
        sequence* s = m_perf->get_sequence(cs);
        if (!s) continue;
        // Collect first, then re-seat: get_next_trigger walks a live list.
        struct T { long on, off, offs; };
        std::vector<T> move;
        s->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        while (s->get_next_trigger(&on, &off, &sel, &offs))
            if (on >= fromTick) move.push_back(T{ on, off, offs });
        // Shift right-to-left when moving right (and vice versa) so a shifted
        // trigger can never land on one that has not moved yet.
        std::sort(move.begin(), move.end(),
                  [&](const T& a, const T& b){ return delta > 0 ? a.on > b.on
                                                                : a.on < b.on; });
        for (const T& t : move) {
            s->del_trigger(t.on);
            const long non = std::max<long>(0, t.on + delta);
            s->add_trigger(non, t.off - t.on + 1, t.offs, false);
        }
        if (!move.empty()) commit_auto_region(cs);
    }
}

// Shuffle placement: the dragged clip packs against the end of the nearest
// other clip that ends at or before `want` (or the lane start).  Clips snap
// to each other and cannot overlap.
long ArrangeView::shuffle_pack(int laneSeq, int seq, long want) const
{
    long best = 0;
    if (!m_perf) return best;
    std::vector<ClipSpan> spans;
    for (int cs : lane_sequences(laneSeq)) {
        if (cs == seq) continue;
        spans.clear();
        clip_spans(cs, spans);
        for (const ClipSpan& sp : spans)
            if (sp.endEx <= want + m_measure_len / 8 && sp.endEx > best)
                best = sp.endEx;
    }
    return best;
}

//----------------------------------------------------------------------------
//  GRID configuration (fig. pt-674-037): value x modifiers x time scale
//----------------------------------------------------------------------------
long ArrangeView::sec_to_ticks(double sec) const
{
    double bpm = m_perf ? m_perf->get_bpm() : 120.0;
    if (bpm < 1.0) bpm = 120.0;
    return std::max<long>(1, (long)(sec * (double)c_ppqn * bpm / 60.0 + 0.5));
}

long ArrangeView::ticks_per_sample() const
{
    double bpm = m_perf ? m_perf->get_bpm() : 120.0;
    if (bpm < 1.0) bpm = 120.0;
    double rate = 48000.0;
    if (!m_audio.empty() && m_audio.begin()->second)
        rate = m_audio.begin()->second->sampleRate > 0
             ? m_audio.begin()->second->sampleRate : 48000.0;
    const double tps = (double)c_ppqn * bpm / 60.0 / rate;   // ticks per sample
    return std::max<long>(1, (long)(tps + 0.999));
}

long ArrangeView::grid_ticks() const
{
    switch (m_grid_scale) {
    case 1: {   // Min:Secs -- the snap index picks from a wall-clock ladder
        static const double S[9] = { 60.0, 10.0, 5.0, 1.0, 0.5, 0.1, 0.01, 0.001, 0.0 };
        const int i = (m_snap_idx < 0 || m_snap_idx > 8) ? 8 : m_snap_idx;
        return S[i] > 0.0 ? sec_to_ticks(S[i]) : 0;
    }
    case 2:     // Samples: one source sample (>= 1 tick at PatchKnob's PPQN)
        return ticks_per_sample();
    case 3:     // Clips/Markers: free placement + magnetic boundaries
        return 0;
    default: {  // Bars|Beats with the dotted / triplet modifiers
        long v = snap_value(m_snap_idx);
        if (v <= 0) return 0;
        if (m_grid_dotted)  v = v * 3 / 2;
        if (m_grid_triplet) v = v * 2 / 3;
        return std::max<long>(1, v);
    }
    }
}

// Clips/Markers grid: events place freely but snap to clip starts/ends, the
// L/R (timeline) markers and the edit-selection boundaries when near them.
long ArrangeView::magnet_snap(long tick) const
{
    const long reach = std::max<long>(1, (long)(8.0 * m_scale_x));   // 8 px
    long best = tick, dist = reach + 1;
    auto consider = [&](long b) {
        const long d = std::labs(b - tick);
        if (d < dist) { dist = d; best = b; }
    };
    for_each_clip([&](const ClipSpan& s) { consider(s.on); consider(s.endEx); });
    if (m_perf) { consider(m_perf->get_left_tick()); consider(m_perf->get_right_tick()); }
    if (m_sel_start >= 0) { consider(m_sel_start); consider(m_sel_end); }
    return dist <= reach ? best : tick;
}

//----------------------------------------------------------------------------
//  SPOT dialog: type an exact location for the pressed clip / edge
//----------------------------------------------------------------------------
// Accepts "bar", "bar.beat", "bar.beat.tick" (also | separators) and
// "m:ss.mmm".  Returns false when nothing parseable was typed.
bool ArrangeView::parse_time(const std::string& s, long& tick) const
{
    if (s.empty()) return false;
    if (s.find(':') != std::string::npos) {                 // m:ss.mmm
        int mins = 0; double sec = 0.0;
        if (std::sscanf(s.c_str(), "%d:%lf", &mins, &sec) < 2) return false;
        tick = sec_to_ticks(mins * 60.0 + sec);
        return true;
    }
    long bar = 1, beat = 1, tk = 0;
    char sep;
    if (std::sscanf(s.c_str(), "%ld%c%ld%c%ld", &bar, &sep, &beat, &sep, &tk) >= 1) {
        if (bar < 1) bar = 1;
        if (beat < 1) beat = 1;
        tick = (bar - 1) * m_measure_len + (beat - 1) * m_beat_len + tk;
        return true;
    }
    return false;
}

void ArrangeView::open_spot(App& app, int seq, int kind)
{
    if (!m_perf || !m_perf->is_active(seq)) return;
    m_spot_open = true;
    m_spot_seq  = seq;
    m_spot_kind = kind;
    // Prime the field with the edge's current location so Enter is a no-op.
    long cur = 0;
    std::vector<ClipSpan> spans; clip_spans(seq, spans);
    if (!spans.empty())
        cur = (kind == 2 || kind == 4) ? spans[0].endEx : spans[0].on;
    m_spot_buf = bbt(cur);
    // The original time stamp: where the region was FIRST placed.
    if (!m_timestamp.count(seq)) m_timestamp[seq] = spans.empty() ? 0 : spans[0].on;
    app.begin_text(&m_spot_buf, nullptr, [this, &app](bool ok) {
        if (ok) commit_spot();
        m_spot_open = false; m_spot_seq = -1;
        app.request_redraw();
    });
    app.request_redraw();
}

void ArrangeView::commit_spot()
{
    long t = 0;
    if (!parse_time(m_spot_buf, t) || m_spot_seq < 0 || !m_perf ||
        !m_perf->is_active(m_spot_seq)) return;
    if (t < 0) t = 0;
    const int seq = m_spot_seq;
    push_undo("Spot Clip");
    if (m_audio.count(seq)) {
        AudioRegion& r = region_for(seq);
        switch (m_spot_kind) {
        case 0: r.position = t; break;                                // move
        case 1: {                                                     // trim start
            const long end = r.position + r.length;
            if (t >= end) t = end - 1;
            const long d = t - r.position;
            r.position = t; r.source += d; r.length = end - t;
            break;
        }
        case 2:                                                       // trim end
            if (t <= r.position) t = r.position + 1;
            r.length = t - r.position;
            break;
        case 3: case 4: {                                             // TCE
            long newLen = (m_spot_kind == 4) ? t - r.position
                                             : (r.position + r.length) - t;
            if (newLen < 1) newLen = 1;
            if (m_spot_kind == 3) { r.position = t; }
            if (on_clip_tce) on_clip_tce(seq, newLen);
            break;
        }
        }
        commit_region(seq);
        return;
    }
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    std::vector<ClipSpan> spans; clip_spans(seq, spans);
    if (spans.empty()) return;
    s->select_trigger(spans[0].on);
    switch (m_spot_kind) {
    case 0: s->move_selected_triggers_to(t, true); break;
    case 1: s->move_selected_triggers_to(std::min(t, spans[0].endEx - 1), false, 0); break;
    default: s->move_selected_triggers_to(std::max(t, spans[0].on + 1) - 1, false, 1); break;
    }
    commit_auto_region(seq);
}

void ArrangeView::draw_spot(App& app)
{
    if (!m_spot_open) return;
    const Theme& t = theme();
    const int lh = app.mono.ch() + 4;
    SDL_Rect box{ 0, 0, 320, lh * 6 + 20 };
    box.x = rect.x + (rect.w - box.w) / 2;
    box.y = rect.y + (rect.h - box.h) / 3;
    box = pt_clamp_popup(box, rect);
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);

    static const char* K[5] = { "SPOT  clip to...", "SPOT  start to...",
                                "SPOT  end to...",  "SPOT/TCE  start to...",
                                "SPOT/TCE  end to..." };
    int y = box.y + 8;
    app.mono.draw(app.ren, box.x + 10, y, K[(m_spot_kind < 0 || m_spot_kind > 4)
                                            ? 0 : m_spot_kind], t.hi);
    y += lh;
    sequence* s = (m_perf && m_spot_seq >= 0 && m_perf->is_active(m_spot_seq))
                ? m_perf->get_sequence(m_spot_seq) : nullptr;
    if (s && s->get_name())
        app.mono.draw(app.ren, box.x + 10, y, pt_fit(app.mono, s->get_name(),
                                                     box.w - 20), t.text);
    y += lh;
    std::vector<ClipSpan> spans;
    if (m_spot_seq >= 0) clip_spans(m_spot_seq, spans);
    const long cur = spans.empty() ? 0 : spans[0].on;
    app.mono.draw(app.ren, box.x + 10, y,
                  "now      " + bbt(cur) + "   " + time_str(cur), t.dim);
    y += lh;
    std::map<int,long>::const_iterator ts = m_timestamp.find(m_spot_seq);
    const long stamp = ts != m_timestamp.end() ? ts->second : cur;
    app.mono.draw(app.ren, box.x + 10, y,
                  "stamp    " + bbt(stamp) + "   " + time_str(stamp), t.dim);
    y += lh + 2;
    SDL_Rect fld{ box.x + 10, y, box.w - 20, lh };
    fill_rect (app.ren, fld, t.bg);
    frame_rect(app.ren, fld, t.hi);
    app.mono.draw(app.ren, fld.x + 4, fld.y + 2, m_spot_buf, t.text);
    vline(app.ren, fld.x + 4 + app.mono.text_w(m_spot_buf), fld.y + 2,
          fld.y + 2 + app.mono.ch(), t.hi);
    y += lh + 2;
    app.mono.draw(app.ren, box.x + 10, y,
                  "bar.beat.tick or m:ss.mmm - Enter / Esc", t.dim);
}

//----------------------------------------------------------------------------
//  TOOLS: selection, sub-mode cycling, names
//----------------------------------------------------------------------------
const char* ArrangeView::trim_mode_name(int m) const
{
    static const char* N[4] = { "Standard", "TCE", "Scrub", "Loop" };
    return N[(m < 0 || m > 3) ? 0 : m];
}

const char* ArrangeView::grab_mode_name(int m) const
{
    static const char* N[3] = { "Time", "Separation", "Object" };
    return N[(m < 0 || m > 2) ? 0 : m];
}

// F-key / click selection.  A repeat on the already-current tool cycles its
// sub-modes -- unless Edit/Tool Mode Keyboard Lock is on AND the repeat came
// from a key (the mouse / right-click may still change modes, per the manual).
void ArrangeView::select_tool(EditTool t, bool from_key)
{
    if (m_edit_tool == t) {
        if (from_key && m_tool_lock) return;
        switch (t) {
        case EditTool::Zoom:  m_zoom_mode = (m_zoom_mode + 1) % 2; break;
        case EditTool::Trim:  m_trim_mode = (m_trim_mode + 1) % 4; break;
        case EditTool::Grab:  m_grab_mode = (m_grab_mode + 1) % 3; break;
        default: break;
        }
        return;
    }
    if (t == EditTool::Zoom && m_edit_tool != EditTool::Zoom)
        m_tool_before_zoom = m_edit_tool;      // Single Zoom returns here
    m_edit_tool = t;
}

// Smart-tool zone map (fig. p693).  Body coordinates only; -1 when outside.
int ArrangeView::smart_zone(int seq, const SDL_Rect& body, int mx, int my) const
{
    if (!pt_in(body, mx, my)) return -1;
    const bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    // Smart Tool Fade Adjustment preference (ch.32 p755): when set, the
    // fade-reshape zone wants the Ctrl key -- so test it BEFORE Ctrl falls
    // through to the temporary Scrubber.
    if (ctrl && m_smart_fade_ctrl && m_audio.count(seq)) {
        FadeGrab g = fade_at(seq, body, mx, my);
        if (g == FadeGrab::InCurve || g == FadeGrab::OutCurve) return 7;
    }
    if (ctrl) return 8;                                     // temporary Scrubber
    if (is_automation(seq)) {
        // Automation lanes: top 25% = Trim, bottom 75% = Selector.
        if (my < body.y + body.h / 4)
            return (mx < body.x + body.w / 2) ? 2 : 3;
        return 0;
    }
    const int edge = 6;
    const bool audio = m_audio.count(seq) != 0;
    // over an existing fade, vertical middle -> adjust fade shape (unless the
    // preference reserves that gesture for Ctrl, handled above)
    if (audio && !m_smart_fade_ctrl) {
        FadeGrab g = fade_at(seq, body, mx, my);
        if (g == FadeGrab::InCurve || g == FadeGrab::OutCurve) return 7;
    }
    // top corners -> fade-in / fade-out drags (audio clips only)
    if (audio && my <= body.y + kFadeStripH_pt()) {
        if (mx - body.x <= 14) return 4;
        if (body.x + body.w - mx <= 14) return 5;
    }
    // bottom edge near a boundary with an ADJACENT clip -> crossfade; an
    // existing OVERLAP (a real crossfade window) is a crossfade zone anywhere
    // along its bottom edge (ch.32)
    if (audio && my >= body.y + body.h - 8) {
        const long lt = x_to_tick(mx);
        int xl = -1, xr = -1;
        long xa = 0, xb = 0;
        if (xfade_pair(seq, xl, xr) && xfade_window(xl, xr, xa, xb) &&
            lt >= xa && lt < xb)
            return 6;
        std::vector<ClipSpan> spans; clip_spans(seq, spans);
        for (const ClipSpan& sp : spans) {
            for (int cs : lane_sequences(seq)) {
                if (cs == seq || !m_audio.count(cs)) continue;
                std::vector<ClipSpan> os; clip_spans(cs, os);
                for (const ClipSpan& o : os) {
                    const long gap = std::min(std::labs(o.on - sp.endEx),
                                              std::labs(sp.on - o.endEx));
                    if (gap <= (long)(8 * m_scale_x) &&
                        std::labs(lt - (o.on < sp.on ? sp.on : sp.endEx))
                            <= (long)(10 * m_scale_x))
                        return 6;
                }
            }
        }
    }
    if (mx - body.x <= edge)          return 2;   // trim start
    if (body.x + body.w - mx <= edge) return 3;   // trim end
    return (my < body.y + body.h / 2) ? 0 : 1;    // upper=Selector, lower=Grabber
}

int ArrangeView::kFadeStripH_pt() { return 9; }

// Pointer-side glyph naming the Smart tool's active zone -- the cursor
// feedback the manual's figures show, mapped to a letter chip in our theme.
void ArrangeView::draw_smart_glyph(App& app)
{
    if (m_edit_tool != EditTool::Smart || m_mx < 0) return;
    if (m_mx < canvas_x() || m_my < canvas_y()) return;
    const int r = row_at(m_my);
    if (r < 0) return;
    std::vector<int> act = active_list();
    const int idx = m_v_offset + r;
    if (idx < 0 || idx >= (int)act.size()) return;
    const int cs = clip_sequence_at(act[(size_t)idx], x_to_tick(m_mx));
    if (cs < 0) return;
    int zone = -1;
    for_each_visible_clip([&](const ClipSpan& sp, const SDL_Rect& body) {
        if (sp.seq != cs) return;
        const int z = smart_zone(cs, body, m_mx, m_my);
        if (z >= 0) zone = z;
    });
    if (zone < 0) return;
    static const char* Z[9] = { "S", "G", "T", "T", "F", "F", "X", "~", "SCRB" };
    const Theme& t = theme();
    const std::string s = Z[zone];
    SDL_Rect b{ m_mx + 12, m_my + 12, app.mono.text_w(s) + 8, app.mono.ch() + 4 };
    b = pt_clamp_popup(b, rect);
    fill_rect (app.ren, b, t.panel);
    frame_rect(app.ren, b, t.accent);
    app.mono.draw(app.ren, b.x + 4, b.y + 2, s, t.hi);
}

//----------------------------------------------------------------------------
//  SHUTTLE LOCK (numeric keypad; ch.29 p664-665)
//----------------------------------------------------------------------------
void ArrangeView::set_shuttle(int digit, bool negative)
{
    // 5 = 1x; 6..8 ramp up; 9 = Custom Shuttle Lock Speed; 4..1 mirror as
    // rewind speeds.  0 stops.
    static const double SPD[10] = { 0.0, 8.0, 4.0, 2.0, 1.5, 1.0,
                                    1.5, 2.0, 4.0, 0.0 /*custom*/ };
    if (digit <= 0) { m_shuttle = 0.0; return; }
    double v = (digit == 9) ? (double)m_shuttle_custom / 100.0 : SPD[digit];
    const bool rew = negative || digit < 5;
    m_shuttle = rew ? -v : v;
    m_shuttle_ms = SDL_GetTicks();
}

//----------------------------------------------------------------------------
//  PENCIL: destructive audio waveform repair (ch.29 p666)
//----------------------------------------------------------------------------
void ArrangeView::pencil_redraw(int seq, int px, int py)
{
    // Only meaningful at sample zoom: one pixel column <= one source sample.
    std::map<int, const PatchKnob::engine::AudioClip*>::const_iterator it = m_audio.find(seq);
    if (it == m_audio.end() || !it->second) return;
    SDL_Rect body;
    if (!clip_rect_of(seq, body) || body.h < 4) return;
    const AudioRegion& r = region_for(seq);
    std::map<int,long>::const_iterator lit = m_audioLen.find(seq);
    const long fullTicks = (lit != m_audioLen.end() && lit->second > 0)
                         ? lit->second : r.length;
    const long t = x_to_tick(px);
    if (t < r.position || t >= r.position + r.length || fullTicks < 1) return;
    const long srcTick = r.source + (t - r.position);
    // DESTRUCTIVE, deliberately: the manual's Pencil permanently modifies the
    // audio.  The clip is message-thread owned (see audio_clip.h) and the view
    // runs on that thread; the const in our map is a drawing convenience, not
    // an engine contract, so writing samples here is the real feature.
    PatchKnob::engine::AudioClip* clip =
        const_cast<PatchKnob::engine::AudioClip*>(it->second);
    const long long n = clip->numFrames();
    long long f = (long long)((double)srcTick / (double)fullTicks * (double)n);
    if (f < 0 || f >= n) return;
    float v = 1.f - 2.f * (float)(py - body.y) / (float)body.h;
    v /= (m_wave_zoom > 0.01f ? m_wave_zoom : 1.f);
    if (v < -1.f) v = -1.f;
    if (v > 1.f)  v = 1.f;
    clip->ch[0][(size_t)f] = v;
    if (!clip->ch[1].empty() && (size_t)f < clip->ch[1].size())
        clip->ch[1][(size_t)f] = v;
    ++g_wave_epoch;                     // invalidate the reduced-envelope cache
}

//----------------------------------------------------------------------------
//  ZOOM SYSTEM (ch.29 p675-684)
//----------------------------------------------------------------------------
void ArrangeView::remember_zoom()
{
    m_prev_scale  = m_scale_x;
    m_prev_scroll = m_scroll_ticks;
}

void ArrangeView::recall_prev_zoom()
{
    if (m_prev_scale <= 0.0) return;
    std::swap(m_prev_scale, m_scale_x);
    std::swap(m_prev_scroll, m_scroll_ticks);
    clamp_scroll();
}

void ArrangeView::zoom_at(int anchorX, double factor)
{
    remember_zoom();
    const long anchorTick = x_to_tick(anchorX);
    double ns = m_scale_x * factor;
    if (ns < kZoomMin) ns = kZoomMin;
    if (ns > kZoomMax) ns = kZoomMax;
    m_scale_x = ns;
    m_scroll_ticks = anchorTick - (long)((anchorX - canvas_x()) * m_scale_x);
    clamp_scroll();
}

// "Overview scale" = 256 samples per pixel (Ctrl-click the Zoomer tool).
void ArrangeView::zoom_overview()
{
    remember_zoom();
    double bpm = m_perf ? m_perf->get_bpm() : 120.0;
    if (bpm < 1.0) bpm = 120.0;
    double rate = 48000.0;
    if (!m_audio.empty() && m_audio.begin()->second &&
        m_audio.begin()->second->sampleRate > 0)
        rate = m_audio.begin()->second->sampleRate;
    const double tps = (double)c_ppqn * bpm / 60.0 / rate;
    double ns = 256.0 * tps;
    if (ns < kZoomMin) ns = kZoomMin;
    if (ns > kZoomMax) ns = kZoomMax;
    m_scale_x = ns;
    clamp_scroll();
}

void ArrangeView::zt_capture(ZoomToggleState& s) const
{
    s.scale = m_scale_x;  s.scroll = m_scroll_ticks;
    s.wave = m_wave_zoom; s.midi = m_midi_zoom;
    s.rowh = row_h;       s.trackH = m_trackH;
    s.snap_idx = m_snap_idx; s.dotted = m_grid_dotted;
    s.triplet = m_grid_triplet; s.grid_scale = m_grid_scale;
}

void ArrangeView::zt_restore(const ZoomToggleState& s)
{
    if (s.scale > 0) { m_scale_x = s.scale; m_scroll_ticks = s.scroll; }
    m_wave_zoom = s.wave; m_midi_zoom = s.midi;
    row_h = s.rowh; m_trackH = s.trackH;
    if (m_zt_sep_grid) {
        m_snap_idx = s.snap_idx; m_grid_dotted = s.dotted;
        m_grid_triplet = s.triplet; m_grid_scale = s.grid_scale;
        m_snap = grid_ticks();
    }
    clamp_scroll();
}

void ArrangeView::zoom_toggle(bool cancel)
{
    if (!m_zt_on) {
        // TOGGLE IN: remember the way back, then apply the preferences.
        zt_capture(m_zt_out);
        int lo = -1, hi = -1; sel_rows(lo, hi);
        // Horizontal zoom: Selection -> frame the edit selection; Last Used ->
        // the stored toggled-in zoom (an "editor window" of its own).
        if (m_zt_pref_h == 0 && m_sel_start >= 0 && m_sel_end > m_sel_start) {
            remember_zoom();
            const long span = m_sel_end - m_sel_start;
            int cw = canvas_w(); if (cw < 16) cw = 16;
            double sx = (double)span * 1.12 / (double)cw;
            if (sx < kZoomMin) sx = kZoomMin;
            if (sx > kZoomMax) sx = kZoomMax;
            m_scale_x = sx;
            m_scroll_ticks = std::max<long>(0, m_sel_start - (long)(span * 0.06));
        } else if (m_zt_in_valid && m_zt_in.scale > 0) {
            remember_zoom();
            m_scale_x = m_zt_in.scale; m_scroll_ticks = m_zt_in.scroll;
        }
        // Vertical zoom: Last Used recalls the stored wave/MIDI zoom.
        // "Selection" has no per-selection amplitude here -> no change.
        if (m_zt_pref_v == 1 && m_zt_in_valid) {
            m_wave_zoom = m_zt_in.wave; m_midi_zoom = m_zt_in.midi;
        }
        // Track Height for the lanes containing the edit selection.
        if (lo >= 0) {
            std::vector<int> act = active_list();
            int nh = 0;
            switch (m_zt_pref_height) {
            case 0: nh = m_zt_in_valid ? m_zt_in.rowh : 0; break;   // Last Used
            case 1: nh = 60;  break;   // Medium
            case 2: nh = 100; break;   // Large
            case 3: nh = 160; break;   // Jumbo
            case 4: nh = 300; break;   // Extreme
            default:                   // Fit To Window
                nh = std::max(20, canvas_h() / std::max(1, hi - lo + 1));
                break;
            }
            if (nh > 0)
                for (int rr = lo; rr <= hi && rr < (int)act.size(); ++rr)
                    m_trackH[lane_key(act[(size_t)rr])] = std::min(400, nh);
            m_zt_lane = lo;
        }
        if (m_zt_remove_range && m_sel_start >= 0 && m_sel_end > m_sel_start)
            set_edit_selection(m_sel_start, m_sel_start, m_sel_lo, m_sel_hi);
        m_zt_on = true;
        clamp_scroll();
        return;
    }
    // TOGGLE OUT: the current view becomes the "Last Used" toggled-in state,
    // then the stored state is restored (unless cancelled, which keeps the
    // current view and simply drops the toggle).
    zt_capture(m_zt_in);
    m_zt_in_valid = true;
    m_zt_on = false;
    m_zt_lane = -1;
    if (!cancel) zt_restore(m_zt_out);
}

//----------------------------------------------------------------------------
//  CH.30 -- EDIT SELECTION MODEL
//----------------------------------------------------------------------------
void ArrangeView::set_edit_selection(long a, long b, int lo, int hi)
{
    if (a > b) std::swap(a, b);
    if (lo > hi) std::swap(lo, hi);
    m_sel_start = a; m_sel_end = b;
    m_sel_lo = lo;   m_sel_hi = hi;
    selection_changed();
}

void ArrangeView::selection_changed()
{
    if (m_sel_start < 0) return;
    // Restore Last Selection bookkeeping (only ranges are worth restoring).
    if (m_sel_end > m_sel_start) {
        m_lastsel[0] = m_sel_start; m_lastsel[1] = m_sel_end;
        m_lastsel_rows[0] = m_sel_lo; m_lastsel_rows[1] = m_sel_hi;
    }
    // Linked timeline: the edit selection IS the play range.
    if (m_link_timeline && m_perf && m_sel_end > m_sel_start) {
        m_perf->set_left_tick(m_sel_start);
        m_perf->set_right_tick(m_sel_end);
    }
    // Linked track: the lane carrying the selection becomes the focused lane.
    if (m_link_track && m_sel_lo >= 0) {
        std::vector<int> act = active_list();
        if (m_sel_lo < (int)act.size())
            m_focus_lane = lane_key(act[(size_t)m_sel_lo]);
    }
    // Auto-toggle: with Zoom Toggle in and following the selection, moving to
    // a different track re-applies the height preference there (length changes
    // deliberately do not re-zoom -- ch.29 p652).
    if (m_zt_on && m_zt_follow_sel && m_sel_lo >= 0 && m_sel_lo != m_zt_lane &&
        m_zt_pref_height != 5 /* Fit To Window never auto-toggles across tracks */) {
        std::vector<int> act = active_list();
        int nh = 0;
        switch (m_zt_pref_height) {
        case 1: nh = 60; break; case 2: nh = 100; break;
        case 3: nh = 160; break; case 4: nh = 300; break;
        default: nh = 0; break;
        }
        if (nh > 0) {
            if (m_zt_lane >= 0 && m_zt_lane < (int)act.size())
                m_trackH.erase(lane_key(act[(size_t)m_zt_lane]));   // restore out state
            if (m_sel_lo < (int)act.size())
                m_trackH[lane_key(act[(size_t)m_sel_lo])] = nh;
        }
        m_zt_lane = m_sel_lo;
    }
    scroll_lane_into_view(m_sel_lo);
}

void ArrangeView::restore_last_selection()
{
    if (m_lastsel[0] < 0) return;
    set_edit_selection(m_lastsel[0], m_lastsel[1],
                       m_lastsel_rows[0], m_lastsel_rows[1]);
}

void ArrangeView::sel_rows(int& lo, int& hi) const
{
    lo = m_sel_lo; hi = m_sel_hi;
    if (lo >= 0) { if (hi < lo) hi = lo; return; }
    // Fall back to the focused lane so track commands still have a target.
    std::vector<int> act = active_list();
    for (int i = 0; i < (int)act.size(); ++i)
        if (m_focus_lane >= 0 && lane_key(act[(size_t)i]) == m_focus_lane) {
            lo = hi = i; return;
        }
}

void ArrangeView::scroll_lane_into_view(int row)
{
    if (row < 0) return;
    if (row < m_v_offset) { m_v_offset = row; return; }
    while (row >= m_v_offset + visible_rows() && m_v_offset < max_v_offset())
        ++m_v_offset;
}

// Translucent band over the selected lanes' tick span; a blinking beam when
// the selection is a bare insertion point.  Drawn OVER the clips so the range
// reads on top of material, like every DAW's edit selection.
void ArrangeView::draw_edit_selection(App& app)
{
    if (m_sel_start < 0) return;
    const Theme& t = theme();
    std::vector<int> act = active_list();
    int lo = m_sel_lo, hi = m_sel_hi;
    if (lo < 0) { lo = 0; hi = (int)act.size() - 1; }
    if (hi < lo) hi = lo;
    const int y0r = lo - m_v_offset, y1r = hi - m_v_offset;
    int ya = row_top(y0r);
    int yb = row_top(y1r) + ((m_v_offset + y1r) >= 0 &&
                             (m_v_offset + y1r) < (int)act.size()
                             ? track_h(act[(size_t)(m_v_offset + y1r)]) : row_h);
    ya = std::max(ya, canvas_y());
    yb = std::min(yb, canvas_y() + canvas_h());
    if (yb <= ya) return;
    const int cvx = canvas_x(), cvr = canvas_x() + canvas_w();
    if (m_sel_end > m_sel_start) {
        int xa = std::max(cvx, tick_to_x(m_sel_start));
        int xb = std::min(cvr, tick_to_x(m_sel_end));
        if (xb > xa) {
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            Color band = t.hi; band.a = 46;
            fill_rect(app.ren, SDL_Rect{ xa, ya, xb - xa, yb - ya }, band);
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
            vline(app.ren, xa, ya, yb, t.hi);
            vline(app.ren, xb, ya, yb, t.hi);
        }
    } else {
        const int x = tick_to_x(m_sel_start);
        if (x >= cvx && x <= cvr) {
            // blink so the insertion point stays findable while stopped
            if ((SDL_GetTicks() / 500) & 1) vline(app.ren, x, ya, yb, t.hi);
            else                            vline(app.ren, x, ya, yb, t.accent);
            app.add_damage(SDL_Rect{ x - 2, ya, 5, yb - ya });
        }
    }
}

//----------------------------------------------------------------------------
//  TAB TO TRANSIENTS (ch.30 p681-682): real onset detection over region audio
//----------------------------------------------------------------------------
// Next clip / clip-group boundary in tab order (Tab with transients OFF).
long ArrangeView::next_boundary(long fromTick, bool backward) const
{
    long best = -1;
    int lo, hi; sel_rows(lo, hi);
    std::vector<int> act = active_list();
    auto consider = [&](long b) {
        if (backward) { if (b < fromTick && (best < 0 || b > best)) best = b; }
        else          { if (b > fromTick && (best < 0 || b < best)) best = b; }
    };
    auto scan_lane = [&](int laneSeq) {
        std::vector<ClipSpan> spans;
        for (int cs : lane_sequences(laneSeq)) {
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& s : spans) { consider(s.on); consider(s.endEx); }
        }
    };
    if (lo >= 0) {
        for (int r = lo; r <= hi && r < (int)act.size(); ++r)
            scan_lane(act[(size_t)r]);
    } else {
        for (int seq : act) scan_lane(seq);
    }
    return best;
}

// Scan the audio under the selected lanes for the next onset: a short-window
// peak that rises well above the trailing envelope.  Cheap and real -- this
// is a detector over the actual region samples, not a stub.
long ArrangeView::next_transient(long fromTick, bool backward) const
{
    long best = -1;
    int lo, hi; sel_rows(lo, hi);
    std::vector<int> act = active_list();
    std::vector<int> lanes;
    if (lo >= 0) for (int r = lo; r <= hi && r < (int)act.size(); ++r)
        lanes.push_back(act[(size_t)r]);
    else lanes = act;

    for (int laneSeq : lanes) {
        for (int cs : lane_sequences(laneSeq)) {
            std::map<int, const PatchKnob::engine::AudioClip*>::const_iterator ai =
                m_audio.find(cs);
            if (ai == m_audio.end() || !ai->second) continue;
            const PatchKnob::engine::AudioClip* clip = ai->second;
            std::map<int, AudioRegion>::const_iterator ri = m_region.find(cs);
            if (ri == m_region.end()) continue;
            const AudioRegion& r = ri->second;
            std::map<int,long>::const_iterator lit = m_audioLen.find(cs);
            const long fullTicks = (lit != m_audioLen.end() && lit->second > 0)
                                 ? lit->second : r.length;
            if (fullTicks < 1) continue;
            const long long n = clip->numFrames();
            if (n < 512) continue;
            const double fpt = (double)n / (double)fullTicks;   // frames per tick
            auto tick_of_frame = [&](long long f) {
                return r.position + (long)((double)f / fpt) - r.source
                     + r.source /* frame is already source-absolute */
                     - r.source;
            };
            (void)tick_of_frame;
            // region window in source frames
            long long f0 = (long long)((double)r.source * fpt);
            long long f1 = (long long)((double)(r.source + r.length) * fpt);
            if (f1 > n) f1 = n;
            if (f1 <= f0) continue;
            // start scanning just past `fromTick` inside this region
            const long relFrom = fromTick - r.position + r.source;   // source ticks
            long long fs = (long long)((double)relFrom * fpt);
            const float* L = clip->ch[0].data();
            const int hop = 64, win = 256;
            double env = 0.0;
            auto peak_at = [&](long long f) {
                float p = 0.f;
                const long long e = std::min(f1, f + win);
                for (long long i = std::max(f0, f); i < e; ++i) {
                    const float a = std::fabs(L[i]);
                    if (a > p) p = a;
                }
                return (double)p;
            };
            auto frame_tick = [&](long long f) {
                return r.position + (long)((double)f / fpt - (double)r.source);
            };
            // ---- pyramid skim + exact refine -------------------------------
            // The raw walk reads every source sample at 4x overlap (win 256,
            // hop 64): ~115 M reads for a 10-minute clip on EVERY Tab press.
            // The waveform pyramid already holds the exact per-256-frame peak
            // of channel 0, so first SKIM those bins for "peak rises well
            // above the trailing envelope" candidates (with a wide margin,
            // 1.2x vs the detector's 2.0x, so nothing real slips through),
            // then run the ORIGINAL hop/win detector only inside each
            // candidate's neighbourhood.  Hits therefore still come from the
            // real detector at hop resolution; quiet stretches cost one read
            // per 256 frames.  Anything the pyramid does not cover (short
            // clips, the trailing partial bin) keeps the raw walk.
            const long long kBin = 256;
            const float *bmn = nullptr, *bmx = nullptr;
            const long long bins = wave_overview_ch0(clip, &bmn, &bmx);
            // Both refiners snap their start onto the hop grid the FULL walk
            // used (origin max(f0,fs) forward, f0 backward), so a refined hit
            // is the very frame -- and therefore the very tick -- the old
            // whole-region scan returned.
            const long long gridO = backward ? f0 : std::max(f0, fs);
            auto snap_grid = [&](long long a) {
                if (a <= gridO) return gridO;
                return gridO + ((a - gridO + hop - 1) / hop) * hop;
            };
            // First hit with tk > fromTick in [a,b), original maths.
            auto refine_fwd = [&](long long a, long long b) -> long {
                a = snap_grid(a);
                if (b > f1) b = f1;
                double e2 = peak_at(std::max(f0, a - 4 * hop));
                for (long long f = a; f + win < b; f += hop) {
                    const double p = peak_at(f);
                    const long tk = frame_tick(f);
                    if (tk > fromTick && p > 0.02 && p > e2 * 2.0) return tk;
                    e2 = e2 * 0.8 + p * 0.2;
                }
                return -1;
            };
            // Last hit with tk < fromTick in [a,b), original maths.
            auto refine_back = [&](long long a, long long b) -> long {
                a = snap_grid(a);
                if (b > f1) b = f1;
                double e2 = (a <= f0) ? 0.0 : peak_at(std::max(f0, a - 4 * hop));
                long lastHit = -1;
                for (long long f = a; f + win < b; f += hop) {
                    const double p = peak_at(f);
                    const long tk = frame_tick(f);
                    if (tk >= fromTick) break;
                    if (p > 0.02 && p > e2 * 2.0) lastHit = tk;
                    e2 = e2 * 0.8 + p * 0.2;
                }
                return lastHit;
            };
            if (bins > 0 && f1 - f0 >= 4 * kBin) {
                const long long skimFrom = backward ? f0 : std::max(f0, fs);
                const long long bA = std::max<long long>(0, skimFrom / kBin);
                const long long bB = std::min(bins, (f1 + kBin - 1) / kBin);
                long hit = -1;
                double benv = 0.0;      // bin-rate envelope: 0.8^4 / matching gain
                for (long long bi = bA; bi < bB; ++bi) {
                    const long long fBin = bi * kBin;
                    if (backward && frame_tick(fBin) >= fromTick) break;
                    const double p = std::max(std::fabs((double)bmn[bi]),
                                              std::fabs((double)bmx[bi]));
                    if (p > 0.02 && p > benv * 1.2) {
                        const long long ra = fBin - 4 * hop;
                        const long long rb = fBin + kBin + win;
                        if (!backward) {
                            hit = refine_fwd(ra, rb);
                            if (hit >= 0) break;
                        } else {
                            const long h2 = refine_back(ra, rb);
                            if (h2 >= 0) hit = h2;      // keep the LAST onset
                        }
                    }
                    benv = benv * 0.41 + p * 0.59;
                }
                // Trailing frames past the pyramid's complete-bin coverage.
                const long long tail = bins * kBin;
                if (tail < f1) {
                    if (!backward) {
                        if (hit < 0) hit = refine_fwd(tail, f1);
                    } else if (frame_tick(tail) < fromTick) {
                        const long h2 = refine_back(tail, f1);
                        if (h2 >= 0) hit = h2;
                    }
                }
                if (hit >= 0) {
                    if (!backward) { if (best < 0 || hit < best) best = hit; }
                    else           { if (best < 0 || hit > best) best = hit; }
                }
            } else if (!backward) {
                long long f = std::max(f0, fs);
                env = peak_at(std::max(f0, f - 4 * hop));
                for (; f + win < f1; f += hop) {
                    const double p = peak_at(f);
                    const long tk = frame_tick(f);
                    if (tk > fromTick && p > 0.02 && p > env * 2.0) {
                        if (best < 0 || tk < best) best = tk;
                        break;
                    }
                    env = env * 0.8 + p * 0.2;
                }
            } else {
                // walk the whole region forward, remember the last onset
                // before fromTick (regions are short enough for this).
                long long f = f0;
                env = 0.0;
                long lastHit = -1;
                for (; f + win < f1; f += hop) {
                    const double p = peak_at(f);
                    const long tk = frame_tick(f);
                    if (tk >= fromTick) break;
                    if (p > 0.02 && p > env * 2.0) lastHit = tk;
                    env = env * 0.8 + p * 0.2;
                }
                if (lastHit >= 0 && (best < 0 || lastHit > best)) best = lastHit;
            }
        }
    }
    return best;
}

long ArrangeView::tab_target(long fromTick, bool backward) const
{
    if (m_tab_transients) {
        const long t = next_transient(fromTick, backward);
        if (t >= 0) return t;
    }
    return next_boundary(fromTick, backward);
}

//----------------------------------------------------------------------------
//  NUDGING + selection movement (ch.30 p675, p679-680, p690)
//----------------------------------------------------------------------------
long ArrangeView::nudge_ticks() const
{
    // ch.31 p732: a typed CUSTOM value wins; "Follow Main Time Scale" rides
    // the Grid value; otherwise the ladder entry.
    if (m_nudge_custom > 0) return m_nudge_custom;
    if (m_nudge_follow) {
        const long g = grid_ticks();
        if (g > 0) return g;
    }
    const long v = snap_value(m_nudge_idx);
    return v > 0 ? v : m_beat_len;
}

void ArrangeView::nudge_selection(int which, long delta)
{
    if (m_sel_start < 0) return;
    long a = m_sel_start, b = m_sel_end;
    if (which == 0) { a += delta; b += delta; }
    else if (which == 1) { a += delta; if (a > b) a = b; }
    else { b += delta; if (b < a) b = a; }
    if (a < 0) { if (which == 0) { b -= a; } a = 0; }
    set_edit_selection(a, b, m_sel_lo, m_sel_hi);
}

// dir -1 = up, +1 = down.  extend widens the row range; remove peels a row
// off the top (dir<0) or bottom (dir>0) of a multi-track selection.
void ArrangeView::move_selection_lane(int dir, bool extend, bool remove)
{
    if (m_sel_start < 0) return;
    std::vector<int> act = active_list();
    const int last = (int)act.size() - 1;
    if (last < 0) return;
    int lo = m_sel_lo < 0 ? 0 : m_sel_lo;
    int hi = m_sel_hi < 0 ? lo : m_sel_hi;
    if (remove) {
        if (hi <= lo) return;                       // single row: nothing to peel
        if (dir < 0) ++lo; else --hi;
    } else if (extend) {
        if (dir < 0) lo = std::max(0, lo - 1);
        else         hi = std::min(last, hi + 1);
    } else {
        const int span = hi - lo;
        lo = std::max(0, std::min(last - span, lo + dir));
        hi = lo + span;
    }
    set_edit_selection(m_sel_start, m_sel_end, lo, hi);
}

//----------------------------------------------------------------------------
//  COUNTERS / EDIT SELECTION INDICATORS (ch.30 p677-678, p684)
//----------------------------------------------------------------------------
void ArrangeView::counter_load(int which)
{
    long v = 0;
    switch (which) {
    case 0: v = playhead(); break;
    case 1: v = m_sel_start < 0 ? 0 : m_sel_start; break;
    case 2: v = m_sel_end   < 0 ? 0 : m_sel_end;   break;
    default: v = m_sel_start < 0 ? 0 : m_sel_end - m_sel_start; break;
    }
    if (v < 0) v = 0;
    if (which == 3) {   // a LENGTH is bars.beats.ticks from zero
        m_counter_v[0] = v / m_measure_len;
        m_counter_v[1] = (v % m_measure_len) / m_beat_len;
        m_counter_v[2] = v % m_beat_len;
    } else {
        m_counter_v[0] = v / m_measure_len + 1;
        m_counter_v[1] = (v % m_measure_len) / m_beat_len + 1;
        m_counter_v[2] = v % m_beat_len;
    }
}

long ArrangeView::counter_value() const
{
    if (m_counter_edit == 3)
        return m_counter_v[0] * m_measure_len + m_counter_v[1] * m_beat_len
             + m_counter_v[2];
    const long bar  = std::max<long>(1, m_counter_v[0]);
    const long beat = std::max<long>(1, m_counter_v[1]);
    return (bar - 1) * m_measure_len + (beat - 1) * m_beat_len + m_counter_v[2];
}

void ArrangeView::counter_begin(int which)
{
    counter_commit(false);
    m_counter_edit = which;
    m_counter_sub  = 0;
    m_counter_calc = 0;
    m_counter_buf.clear();
    counter_load(which);
}

void ArrangeView::counter_apply(int which, long tick)
{
    if (tick < 0) tick = 0;
    switch (which) {
    case 0:                             // Main counter navigates the transport
        seek_to(tick);
        break;
    case 1: {
        long b = m_sel_end < 0 ? tick : m_sel_end;
        if (b < tick) b = tick;
        set_edit_selection(tick, b, m_sel_lo, m_sel_hi);
        break;
    }
    case 2: {
        long a = m_sel_start < 0 ? 0 : m_sel_start;
        if (tick < a) tick = a;
        set_edit_selection(a, tick, m_sel_lo, m_sel_hi);
        break;
    }
    default: {
        const long a = m_sel_start < 0 ? 0 : m_sel_start;
        set_edit_selection(a, a + std::max<long>(0, tick), m_sel_lo, m_sel_hi);
        break;
    }
    }
}

void ArrangeView::counter_commit(bool apply)
{
    if (m_counter_edit < 0) return;
    if (apply) {
        // flush a pending typed subfield first
        if (!m_counter_buf.empty()) {
            m_counter_v[m_counter_sub] = std::atol(m_counter_buf.c_str());
            m_counter_buf.clear();
        }
        long v = counter_value();
        if (m_counter_calc != 0) {
            // calculator entry: the typed triple is an amount added/subtracted
            long base = 0;
            switch (m_counter_edit) {
            case 0: base = playhead(); break;
            case 1: base = m_sel_start < 0 ? 0 : m_sel_start; break;
            case 2: base = m_sel_end   < 0 ? 0 : m_sel_end;   break;
            default: base = m_sel_start < 0 ? 0 : m_sel_end - m_sel_start; break;
            }
            // the typed value as a bare duration (bar 1 beat 1 == zero offset)
            long amt = (std::max<long>(1, m_counter_v[0]) - 1) * m_measure_len
                     + (std::max<long>(1, m_counter_v[1]) - 1) * m_beat_len
                     + m_counter_v[2];
            if (m_counter_edit == 3)
                amt = m_counter_v[0] * m_measure_len + m_counter_v[1] * m_beat_len
                    + m_counter_v[2];
            v = base + m_counter_calc * amt;
        }
        counter_apply(m_counter_edit, v);
    }
    m_counter_edit = -1;
    m_counter_calc = 0;
    m_counter_buf.clear();
}

void ArrangeView::counter_bump(int dir)
{
    if (m_counter_edit < 0) return;
    long& v = m_counter_v[m_counter_sub];
    v += dir;
    const long floor_v = (m_counter_edit == 3 || m_counter_sub == 2) ? 0 : 1;
    if (v < floor_v) v = floor_v;
    if (m_counter_sub == 1) {
        const long beats = m_measure_len / std::max<long>(1, m_beat_len);
        const long cap = m_counter_edit == 3 ? beats - 1 : beats;
        if (v > cap) v = cap;
    }
    if (m_counter_sub == 2 && v >= m_beat_len) v = m_beat_len - 1;
}

// Keys while a counter/indicator is active.  Returns true when consumed.
bool ArrangeView::counter_key(App& app, SDL_Keycode k)
{
    if (m_counter_edit < 0) return false;
    auto flush = [&]() {
        if (!m_counter_buf.empty()) {
            m_counter_v[m_counter_sub] = std::atol(m_counter_buf.c_str());
            m_counter_buf.clear();
        }
    };
    int digit = -1;
    if (k >= SDLK_0 && k <= SDLK_9) digit = (int)(k - SDLK_0);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) digit = (int)(k - SDLK_KP_1) + 1;
    if (k == SDLK_KP_0) digit = 0;
    if (digit >= 0) {
        if (m_counter_buf.size() < 6) m_counter_buf += (char)('0' + digit);
        m_counter_v[m_counter_sub] = std::atol(m_counter_buf.c_str());
        app.request_redraw();
        return true;
    }
    switch (k) {
    case SDLK_PERIOD: case SDLK_KP_PERIOD:      // next subfield
        flush(); m_counter_sub = (m_counter_sub + 1) % 3; break;
    case SDLK_LEFT:
        flush(); m_counter_sub = (m_counter_sub + 2) % 3; break;
    case SDLK_RIGHT:
        flush(); m_counter_sub = (m_counter_sub + 1) % 3; break;
    case SDLK_UP:   flush(); counter_bump(+1); break;
    case SDLK_DOWN: flush(); counter_bump(-1); break;
    case SDLK_SLASH: case SDLK_KP_DIVIDE: {     // cycle Start -> End -> Length
        const int w = m_counter_edit;
        counter_commit(true);
        counter_begin(w == 0 ? 0 : (w == 3 ? 1 : w + 1));
        break;
    }
    case SDLK_PLUS: case SDLK_KP_PLUS:
        flush(); m_counter_calc = +1;
        m_counter_v[0] = m_counter_v[1] = m_counter_v[2] = 0;
        m_counter_sub = 0;
        break;
    case SDLK_MINUS: case SDLK_KP_MINUS:
        flush(); m_counter_calc = -1;
        m_counter_v[0] = m_counter_v[1] = m_counter_v[2] = 0;
        m_counter_sub = 0;
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        counter_commit(true); break;
    case SDLK_ESCAPE:
        counter_commit(false); break;
    default:
        return true;    // swallow everything else while editing a field
    }
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  UNIVERSE VIEW (ch.30 p686-688)
//----------------------------------------------------------------------------
void ArrangeView::draw_universe(App& app)
{
    if (!m_universe_on) return;
    const Theme& t = theme();
    m_universe_rect = SDL_Rect{ canvas_x(), rect.y + toolbar_h(),
                                canvas_w(), m_universe_h };
    const SDL_Rect& u = m_universe_rect;
    fill_rect (app.ren, u, t.bg);
    frame_rect(app.ren, u, t.dim);

    std::vector<int> act = active_list();
    const int n = std::max(1, (int)act.size());
    const long view  = std::max<long>(1, (long)(canvas_w() * m_scale_x));
    const long total = std::max<long>(view, song_end() + m_measure_len);
    const double px_per_tick = (double)(u.w - 2) / (double)total;
    const int laneH = std::max(2, (u.h - 2) / n);

    // every track's material as coloured lines, in track order.  Lane
    // membership grouped in ONE pass -- lane_sequences() rescanned the whole
    // sequence table per lane, every frame the strip was visible.
    const std::vector<std::vector<int>> groups = lane_groups(act);
    std::vector<ClipSpan> spans;
    for (int i = 0; i < (int)act.size(); ++i) {
        const int y = u.y + 1 + i * laneH;
        if (y + laneH > u.y + u.h - 1) break;
        for (int cs : groups[(size_t)i]) {
            spans.clear();
            clip_spans(cs, spans);
            const Color c = clip_color(cs);
            for (const ClipSpan& s : spans) {
                int a = u.x + 1 + (int)((double)s.on    * px_per_tick);
                int b = u.x + 1 + (int)((double)s.endEx * px_per_tick);
                if (b <= a) b = a + 1;
                if (b > u.x + u.w - 1) b = u.x + u.w - 1;
                fill_rect(app.ren, SDL_Rect{ a, y + std::max(0, laneH / 2 - 1),
                                             b - a, std::max(1, laneH - 2) }, c);
            }
        }
    }

    // the framed area = what the canvas currently shows (both axes)
    {
        int fx = u.x + 1 + (int)((double)m_scroll_ticks * px_per_tick);
        int fw = std::max(6, (int)((double)view * px_per_tick));
        const int rows = visible_rows();
        int fy = u.y + 1 + m_v_offset * laneH;
        int fh = std::max(4, std::min(rows, n - m_v_offset) * laneH);
        if (fx + fw > u.x + u.w - 1) fw = u.x + u.w - 1 - fx;
        if (fy + fh > u.y + u.h - 1) fh = u.y + u.h - 1 - fy;
        if (fw > 0 && fh > 0) {
            frame_rect(app.ren, SDL_Rect{ fx, fy, fw, fh }, t.hi);
            frame_rect(app.ren, SDL_Rect{ fx - 1, fy - 1, fw + 2, fh + 2 }, t.accent);
        }
    }

    // resize grip on the bottom edge
    const bool ghot = (m_my >= u.y + u.h - 4 && m_my <= u.y + u.h &&
                       m_mx >= u.x && m_mx < u.x + u.w);
    if (ghot || m_univ_resize) {
        for (int gx = u.x + u.w / 2 - 8; gx <= u.x + u.w / 2 + 8; gx += 4)
            fill_rect(app.ren, SDL_Rect{ gx, u.y + u.h - 3, 2, 2 }, t.hi);
        if (ghot && !m_univ_resize) tip("Drag to resize the Universe view", m_mx + 12, m_my - 20);
    }
    app.add_damage(u);      // playhead-adjacent frame moves during playback
}

// Centre the canvas (both axes) on a Universe click.
void ArrangeView::universe_goto(int mx, int my)
{
    const SDL_Rect& u = m_universe_rect;
    if (u.w < 3) return;
    std::vector<int> act = active_list();
    const int n = std::max(1, (int)act.size());
    const long view  = std::max<long>(1, (long)(canvas_w() * m_scale_x));
    const long total = std::max<long>(view, song_end() + m_measure_len);
    const double frac = (double)(mx - u.x - 1) / (double)std::max(1, u.w - 2);
    m_scroll_ticks = (long)(frac * total) - view / 2;
    clamp_scroll();
    const int laneH = std::max(2, (u.h - 2) / n);
    int row = (my - u.y - 1) / laneH - visible_rows() / 2;
    if (row < 0) row = 0;
    if (row > max_v_offset()) row = max_v_offset();
    m_v_offset = row;
}

bool ArrangeView::universe_mouse(App& app, const MouseEv& e)
{
    if (!m_universe_on) return false;
    const SDL_Rect& u = m_universe_rect;
    if (!e.pressed) {
        if (m_univ_drag || m_univ_resize) {
            m_univ_drag = m_univ_resize = false;
            return true;
        }
        return false;
    }
    if (m_univ_resize) {
        m_universe_h = std::max(16, std::min(160, e.y - u.y));
        app.request_redraw();
        return true;
    }
    if (m_univ_drag) {
        universe_goto(e.x, e.y);
        app.request_redraw();
        return true;
    }
    if (!pt_in(u, e.x, e.y)) return false;
    if (e.button != SDL_BUTTON_LEFT) return true;
    if (e.y >= u.y + u.h - 4) m_univ_resize = true;
    else { m_univ_drag = true; universe_goto(e.x, e.y); }
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  TOPBAR: the Edit-window strip from fig. pt-718-106 -- mode block, tool
//  strip, zoom presets + toggle, link/tab chips, counters and the Grid/Nudge
//  fields.  Two 22 px rows; the mode block sits over the header column.
//----------------------------------------------------------------------------
namespace {
// display order of the tool icons (values are EditTool enum ints)
const int kToolOrder[8] = { 4, 5, 1, 0, 6, 3, 7, 2 };
}

void ArrangeView::draw_topbar(App& app)
{
    const Theme& t = theme();
    const int cw = app.mono.cw() ? app.mono.cw() : 6;
    SDL_Rect A{ rect.x, rect.y,      rect.w, 22 };
    SDL_Rect B{ rect.x, rect.y + 22, rect.w, 22 };
    fill_rect(app.ren, A, t.panel);
    fill_rect(app.ren, B, t.panel);
    hline(app.ren, rect.x, rect.x + rect.w, B.y + B.h - 1, t.accent);

    // ---- EDIT MODE block (2x2, over the header column) ---------------------
    {
        const int mw = (header_w - 10) / 2, mh = 19;
        // PT layout: SHUFFLE SPOT / SLIP GRID.  Rects indexed by EditMode.
        struct Cell { int mode, col, row; };
        static const Cell C[4] = { { 0,0,0 }, { 2,1,0 }, { 1,0,1 }, { 3,1,1 } };
        for (const Cell& c : C) {
            SDL_Rect q{ rect.x + 3 + c.col * (mw + 2), rect.y + 1 + c.row * 21,
                        mw, mh };
            m_mode_rect[c.mode] = q;
            const bool on  = (int)m_edit_mode == c.mode;
            const bool hot = pt_in(q, m_mx, m_my);
            // Snap-To-Grid combined: the Grid cell frames bright while another
            // mode holds the fill, so the combination reads as a combination.
            const bool half = (c.mode == 3 && m_snap_to_grid && !on);
            fill_rect (app.ren, q, on ? t.accent : (hot ? t.keybg : t.bg));
            frame_rect(app.ren, q, on || half ? t.hi : t.dim);
            std::string label = mode_name(c.mode);
            if (c.mode == 3 && m_grid_relative) label += " R";
            app.mono.draw_centered(app.ren, q, pt_fit(app.mono, label, q.w - 4),
                                   on ? t.bg : (hot ? t.hi : t.text));
            if (c.mode == 0 && m_shuffle_lock) {      // lock glyph on Shuffle
                const Color lc = on ? t.bg : t.hi;
                SDL_Rect body{ q.x + 3, q.y + mh - 9, 7, 6 };
                fill_rect(app.ren, body, lc);
                frame_rect(app.ren, SDL_Rect{ q.x + 4, q.y + mh - 13, 5, 5 }, lc);
            }
            if (hot) {
                if (c.mode == 0)
                    tip(m_shuffle_lock ? "Shuffle LOCKED (Ctrl-click unlocks)"
                        : "Shuffle (F1) - ripple edits; Ctrl-click locks it out",
                        q.x, B.y + B.h + 2);
                else if (c.mode == 3)
                    tip(std::string("Grid (F4) - right-click: Absolute/Relative; "
                        "Shift-click: Snap To Grid  [now ")
                        + (m_grid_relative ? "Relative]" : "Absolute]"),
                        q.x - 80, B.y + B.h + 2);
                else
                    tip(std::string(mode_name(c.mode)) + " (F" +
                        std::to_string(c.mode == 1 ? 2 : 3) + ")",
                        q.x, B.y + B.h + 2);
            }
        }
    }

    int tx = rect.x + header_w + 4;

    // ---- row A: tool strip -------------------------------------------------
    for (int i = 0; i < 8; ++i) {
        const int tool = kToolOrder[i];
        SDL_Rect q{ tx, A.y + 2, 24, 18 };
        m_tool_rect[tool] = q;
        const bool on  = (int)m_edit_tool == tool;
        const bool hot = pt_in(q, m_mx, m_my);
        fill_rect (app.ren, q, on ? t.accent : (hot ? t.keybg : t.bg));
        frame_rect(app.ren, q, on ? t.hi : t.dim);
        draw_tool_icon(app, q, tool, on ? t.bg : (hot ? t.hi : t.text));
        // sub-mode tag in the corner (Trim/Grab/Zoom carry modes)
        const char* tag = nullptr;
        if (tool == (int)EditTool::Trim) {
            static const char* T[4] = { "S", "T", "Sc", "L" };
            tag = T[m_trim_mode & 3];
        } else if (tool == (int)EditTool::Grab) {
            static const char* G[3] = { "T", "S", "O" };
            tag = G[m_grab_mode % 3];
        } else if (tool == (int)EditTool::Zoom && m_zoom_mode == 1) {
            tag = ">";                     // Single Zoom's arrow
        }
        if (tag)
            app.mono.draw(app.ren, q.x + q.w - app.mono.text_w(tag) - 1,
                          q.y + q.h - app.mono.ch(), tag, on ? t.bg : t.accent);
        if (hot) {
            std::string s = std::string(tool_name(tool)) + " (" + tool_key(tool) + ")";
            if (tool == (int)EditTool::Trim) s += std::string(" - ") + trim_mode_name(m_trim_mode);
            if (tool == (int)EditTool::Grab) s += std::string(" - ") + grab_mode_name(m_grab_mode);
            if (tool == (int)EditTool::Zoom) s += m_zoom_mode ? " - Single" : " - Normal";
            s += "  (hold: modes)";
            tip(s, q.x, B.y + B.h + 2);
        }
        tx += 25;
    }
    // click-and-hold on a tool opens its sub-mode pop-up; keep frames coming
    // while a hold is armed or the timer can never fire without mouse motion
    if (m_hold_tool >= 0 || m_hold_preset >= 0) app.request_redraw();
    if (m_hold_tool >= 0 && SDL_GetTicks() - m_hold_ms > 350 && !m_submode_menu) {
        m_submode_menu = true;
        m_submode_tool = m_hold_tool;
        m_submode_rect = SDL_Rect{ m_tool_rect[m_hold_tool].x, B.y + B.h, 150, 0 };
        m_hold_tool = -1;
    }
    tx += 4;

    // ---- row A: EDIT / VIEW command menus + Layered Editing toggle ---------
    {
        m_editbtn_rect = SDL_Rect{ tx, A.y + 2, 4 * cw + 8, 18 };
        pt_chip(app, m_editbtn_rect, "EDIT", m_edit_menu, pt_in(m_editbtn_rect, m_mx, m_my));
        if (pt_in(m_editbtn_rect, m_mx, m_my))
            tip("Edit commands: Cut/Copy/Paste, Separate, Trim, Consolidate...",
                tx - 40, B.y + B.h + 2);
        tx += m_editbtn_rect.w + 2;
        m_view_rect = SDL_Rect{ tx, A.y + 2, 4 * cw + 8, 18 };
        pt_chip(app, m_view_rect, "VIEW", m_view_menu, pt_in(m_view_rect, m_mx, m_my));
        if (pt_in(m_view_rect, m_mx, m_my))
            tip("View options: Waveforms (Peak/Power/Rectified), Clip display",
                tx - 40, B.y + B.h + 2);
        tx += m_view_rect.w + 2;
        m_lay_rect = SDL_Rect{ tx, A.y + 2, 3 * cw + 8, 18 };
        pt_chip(app, m_lay_rect, "LAY", m_layered, pt_in(m_lay_rect, m_mx, m_my));
        if (pt_in(m_lay_rect, m_mx, m_my))
            tip(m_layered ? "Layered Editing ON: edits reveal overlapped clips"
                          : "Layered Editing OFF: overlapped clips trim to the overlapper",
                tx - 80, B.y + B.h + 2);
        tx += m_lay_rect.w + 4;
    }

    // ---- row A: link / tab / universe chips -------------------------------
    {
        m_link_tl_rect = SDL_Rect{ tx, A.y + 2, 3 * cw + 8, 18 };
        pt_chip(app, m_link_tl_rect, "T<E", m_link_timeline, pt_in(m_link_tl_rect, m_mx, m_my));
        if (pt_in(m_link_tl_rect, m_mx, m_my))
            tip("Link Timeline and Edit Selection (Shift+/)", tx - 40, B.y + B.h + 2);
        tx += m_link_tl_rect.w + 2;
        m_link_tr_rect = SDL_Rect{ tx, A.y + 2, 3 * cw + 8, 18 };
        pt_chip(app, m_link_tr_rect, "TRK", m_link_track, pt_in(m_link_tr_rect, m_mx, m_my));
        if (pt_in(m_link_tr_rect, m_mx, m_my))
            tip("Link Track and Edit Selection (Shift+T)", tx - 40, B.y + B.h + 2);
        tx += m_link_tr_rect.w + 2;
        m_tab_rect = SDL_Rect{ tx, A.y + 2, 3 * cw + 8, 18 };
        pt_chip(app, m_tab_rect, "TAB", m_tab_transients, pt_in(m_tab_rect, m_mx, m_my));
        if (pt_in(m_tab_rect, m_mx, m_my))
            tip("Tab to Transients - Tab/Ctrl+Tab move, Shift extends", tx - 60, B.y + B.h + 2);
        tx += m_tab_rect.w + 2;
        m_univ_btn_rect = SDL_Rect{ tx, A.y + 2, 3 * cw + 8, 18 };
        pt_chip(app, m_univ_btn_rect, "UNI", m_universe_on, pt_in(m_univ_btn_rect, m_mx, m_my));
        if (pt_in(m_univ_btn_rect, m_mx, m_my))
            tip("Universe view (Alt+7)", tx - 30, B.y + B.h + 2);
        tx += m_univ_btn_rect.w + 4;
        m_help_rect = SDL_Rect{ tx, A.y + 2, 18, 18 };
        pt_chip(app, m_help_rect, "?", m_help_open, pt_in(m_help_rect, m_mx, m_my));
        if (pt_in(m_help_rect, m_mx, m_my))
            tip("Keys & gestures (/)", tx - 20, B.y + B.h + 2);
    }

    // ---- row B: zoom presets, zoom toggle, zoom buttons, vertical zooms ----
    int bx = rect.x + header_w + 4;
    for (int i = 0; i < 5; ++i) {
        SDL_Rect q{ bx, B.y + 2, 16, 18 };
        m_preset_rect[i] = q;
        const bool stored = m_zoom_preset[i] > 0.0;
        const bool on = stored && std::fabs(m_zoom_preset[i] - m_scale_x) < 1e-9;
        char n[2] = { (char)('1' + i), 0 };
        pt_chip(app, q, n, on, pt_in(q, m_mx, m_my));
        if (!stored)     // hollow digit = empty slot
            frame_rect(app.ren, SDL_Rect{ q.x + 2, q.y + 14, q.w - 4, 2 }, t.dim);
        if (pt_in(q, m_mx, m_my))
            tip(stored ? "Zoom preset (Ctrl+1..5) - Shift-click or hold to store"
                       : "Empty zoom preset - Shift-click or hold to store",
                q.x - 60, B.y + B.h + 2);
        bx += 17;
    }
    // click-and-hold a preset = Save Zoom Preset (the pop-up menu collapsed)
    if (m_hold_preset >= 0 && SDL_GetTicks() - m_hold_ms > 500) {
        m_zoom_preset[m_hold_preset] = m_scale_x;
        m_hold_preset = -1;
        app.request_redraw();
    }
    bx += 3;
    m_zt_rect = SDL_Rect{ bx, B.y + 2, 3 * cw + 8, 18 };
    pt_chip(app, m_zt_rect, "Z<>", m_zt_on, pt_in(m_zt_rect, m_mx, m_my));
    if (pt_in(m_zt_rect, m_mx, m_my))
        tip("Zoom Toggle (E) - Alt-click clears, right-click: preferences",
            bx - 80, B.y + B.h + 2);
    bx += m_zt_rect.w + 4;

    // zoom out / in / fit (kept from the old strip; drag = continuous zoom)
    static const char* zoomTip[3] = { "Zoom out (-) - drag for continuous",
                                      "Zoom in (+) - drag for continuous",
                                      "Fit song (F)" };
    for (int i = 0; i < 3; ++i) {
        const int zw = (i == 2) ? 3 * cw + 8 : 18;
        m_zoom_rect[i] = SDL_Rect{ bx, B.y + 2, zw, 18 };
        const bool hot = pt_in(m_zoom_rect[i], m_mx, m_my);
        fill_rect (app.ren, m_zoom_rect[i], hot ? t.accent : t.bg);
        frame_rect(app.ren, m_zoom_rect[i], t.dim);
        const Color fg = hot ? t.bg : t.text;
        const int zcx = bx + zw / 2, zcy = B.y + 11;
        if (i == 2) app.mono.draw_centered(app.ren, m_zoom_rect[i], "FIT", fg);
        else {
            hline(app.ren, zcx - 5, zcx + 5, zcy, fg);
            hline(app.ren, zcx - 5, zcx + 5, zcy + 1, fg);
            if (i == 1) { vline(app.ren, zcx,     zcy - 5, zcy + 6, fg);
                          vline(app.ren, zcx + 1, zcy - 5, zcy + 6, fg); }
        }
        if (hot) tip(zoomTip[i], bx, B.y + B.h + 2);
        bx += zw + 2;
    }
    // vertical zoom buttons: audio waveform -, +, then MIDI -, +
    static const char* VL[4] = { "a-", "a+", "m-", "m+" };
    static const char* VT[4] = {
        "Audio vertical zoom out (Alt+Shift+wheel); Ctrl+Shift-click resets heights",
        "Audio vertical zoom in (Alt+Shift+wheel); Alt-click = previous zoom",
        "MIDI vertical zoom out (Alt+Ctrl+wheel)",
        "MIDI vertical zoom in (Alt+Ctrl+wheel)" };
    for (int i = 0; i < 4; ++i) {
        m_vzoom_rect[i] = SDL_Rect{ bx, B.y + 2, 2 * cw + 6, 18 };
        pt_chip(app, m_vzoom_rect[i], VL[i], false, pt_in(m_vzoom_rect[i], m_mx, m_my));
        if (pt_in(m_vzoom_rect[i], m_mx, m_my)) tip(VT[i], bx - 80, B.y + B.h + 2);
        bx += m_vzoom_rect[i].w + 2;
    }

    // loop chip + transport readout (moved here from the old ruler strip)
    m_loopchip_rect = SDL_Rect{ 0, 0, 0, 0 };
    m_readout_rect  = SDL_Rect{ 0, 0, 0, 0 };
    {
        const long lt = m_perf ? m_perf->get_left_tick() : 0;
        const long rt = m_perf ? m_perf->get_right_tick() : 0;
        if (rt > lt) {
            const std::string ls = "LOOP " + bars_len(rt - lt);
            const int lw = app.mono.text_w(ls) + 10;
            if (bx + lw < rect.x + rect.w - 360) {
                SDL_Rect lb{ bx + 2, B.y + 2, lw, 18 };
                m_loopchip_rect = lb;
                fill_rect (app.ren, lb, t.bg);
                frame_rect(app.ren, lb, t.dim);
                app.mono.draw(app.ren, lb.x + 5, lb.y + (18 - app.mono.ch()) / 2,
                              ls, t.accent);
                if (pt_in(lb, m_mx, m_my))
                    tip("Timeline selection " + bbt(lt) + " -> " + bbt(rt),
                        lb.x, B.y + B.h + 2);
                bx = lb.x + lb.w + 2;
            }
        }
    }

    // ---- right-aligned counters + indicators + Grid/Nudge fields -----------
    // A field paints its value, or -- while being edited -- the typed triple
    // with the active subfield boxed.  Row A: MAIN | START | END.
    // Row B: SUB (m:ss) | LENGTH | GRID | NUDGE.
    auto field = [&](SDL_Rect q, const char* name, const std::string& val,
                     int which) {
        const bool editing = which >= 0 && m_counter_edit == which;
        const bool hot = pt_in(q, m_mx, m_my);
        fill_rect (app.ren, q, editing ? t.keybg : t.bg);
        frame_rect(app.ren, q, editing ? t.hi : (hot ? t.accent : t.dim));
        app.mono.draw(app.ren, q.x + 3, q.y + (q.h - app.mono.ch()) / 2, name, t.dim);
        const int vx = q.x + 3 + app.mono.text_w(name) + 3;
        if (!editing) {
            app.mono.draw(app.ren, vx, q.y + (q.h - app.mono.ch()) / 2,
                          pt_fit(app.mono, val, q.x + q.w - vx - 2), t.hi);
            return;
        }
        // typed view: three subfields, active one boxed; calc sign leads
        int x = vx;
        if (m_counter_calc != 0) {
            app.mono.draw(app.ren, x, q.y + 3, m_counter_calc > 0 ? "+" : "-", t.hi);
            x += cw + 2;
        }
        for (int f = 0; f < 3; ++f) {
            char b[16];
            std::snprintf(b, sizeof(b), f == 2 ? "%03ld" : "%ld", m_counter_v[f]);
            const int w = app.mono.text_w(b);
            if (f == m_counter_sub)
                frame_rect(app.ren, SDL_Rect{ x - 1, q.y + 2, w + 2, q.h - 4 }, t.hi);
            app.mono.draw(app.ren, x, q.y + (q.h - app.mono.ch()) / 2, b, t.text);
            x += w;
            if (f < 2) { app.mono.draw(app.ren, x, q.y + 3, ".", t.dim); x += cw; }
        }
    };

    const int fw  = 13 * cw;          // one indicator field's width
    const int gap = 3;
    int rxA = rect.x + rect.w - scrollbar_w - 2;
    int rxB = rxA;
    if (rect.w > header_w + 620) {
        // Row A: END, START, MAIN (laid right to left)
        SDL_Rect qEnd  { rxA - fw, A.y + 2, fw, 18 };            rxA -= fw + gap;
        SDL_Rect qStart{ rxA - fw, A.y + 2, fw, 18 };            rxA -= fw + gap;
        SDL_Rect qMain { rxA - fw, A.y + 2, fw, 18 };            rxA -= fw + gap;
        m_counter_rect[2] = qEnd; m_counter_rect[1] = qStart; m_counter_rect[0] = qMain;
        field(qMain,  "MAIN", bbt(playhead()), 0);
        field(qStart, "ST",   m_sel_start < 0 ? "-" : bbt(m_sel_start), 1);
        field(qEnd,   "EN",   m_sel_end   < 0 ? "-" : bbt(m_sel_end),   2);
        app.add_damage(qMain);        // main counter follows the playhead
        // Row B: NUDGE, GRID, LENGTH, SUB
        SDL_Rect qNud { rxB - fw + 2 * cw, B.y + 2, fw - 2 * cw, 18 }; rxB -= fw - 2 * cw + gap;
        SDL_Rect qGrid{ rxB - fw, B.y + 2, fw, 18 };             rxB -= fw + gap;
        SDL_Rect qLen { rxB - fw, B.y + 2, fw, 18 };             rxB -= fw + gap;
        SDL_Rect qSub { rxB - fw, B.y + 2, fw, 18 };             rxB -= fw + gap;
        m_counter_rect[3] = qLen; m_grid_rect = qGrid; m_nudge_rect = qNud;
        field(qSub, "SUB", time_str(playhead()), -1);
        field(qLen, "LEN", m_sel_start < 0 ? "-" : bars_len(m_sel_end - m_sel_start), 3);
        app.add_damage(qSub);
        // Grid field: value label + modifiers + the scale
        {
            std::string g;
            switch (m_grid_scale) {
            case 1: {   // the Min:Secs ladder grid_ticks() indexes
                static const char* S[9] = { "1min", "10s", "5s", "1s", "500ms",
                                            "100ms", "10ms", "1ms", "OFF" };
                g = S[(m_snap_idx < 0 || m_snap_idx > 8) ? 8 : m_snap_idx];
                break;
            }
            case 2: g = "smpl";   break;
            case 3: g = "clips";  break;
            default:
                g = snap_label(m_snap_idx);
                if (m_grid_dotted)  g += ".";
                if (m_grid_triplet) g += "3";
                break;
            }
            const bool hot = pt_in(qGrid, m_mx, m_my);
            fill_rect (app.ren, qGrid, m_grid_menu ? t.keybg : t.bg);
            frame_rect(app.ren, qGrid, hot ? t.accent : t.dim);
            app.mono.draw(app.ren, qGrid.x + 3, qGrid.y + 3, "GRID", t.dim);
            app.mono.draw(app.ren, qGrid.x + 3 + app.mono.text_w("GRID") + 3,
                          qGrid.y + 3, pt_fit(app.mono, g, qGrid.w - 40), t.accent);
            if (hot) tip("Grid value (S cycles; Shift+= / Shift+- step)",
                         qGrid.x - 80, B.y + B.h + 2);
        }
        {
            const bool hot = pt_in(qNud, m_mx, m_my);
            fill_rect (app.ren, qNud, m_nudge_menu ? t.keybg : t.bg);
            frame_rect(app.ren, qNud, hot ? t.accent : t.dim);
            app.mono.draw(app.ren, qNud.x + 3, qNud.y + 3, "NDG", t.dim);
            const char* nl = m_nudge_custom > 0 ? "CUST"
                           : (m_nudge_follow ? "MAIN" : snap_label(m_nudge_idx));
            app.mono.draw(app.ren, qNud.x + 3 + app.mono.text_w("NDG") + 3,
                          qNud.y + 3, nl, t.accent);
            if (hot) tip("Nudge value (numpad +/- nudges the selection)",
                         qNud.x - 80, B.y + B.h + 2);
        }
    } else {
        for (int i = 0; i < 4; ++i) m_counter_rect[i] = SDL_Rect{ 0,0,0,0 };
        m_grid_rect = m_nudge_rect = SDL_Rect{ 0,0,0,0 };
    }

    // playhead readout between the vzoom chips and the right-side fields
    {
        const std::string rd = bbt(playhead()) + "  " + time_str(playhead());
        const int rw = app.mono.text_w(rd) + 10;
        if (bx + rw < rxB - 10) {
            SDL_Rect rb{ bx + 2, B.y + 2, rw, 18 };
            m_readout_rect = rb;
            fill_rect (app.ren, rb, t.keybg);
            frame_rect(app.ren, rb, t.dim);
            app.mono.draw(app.ren, rb.x + 5, rb.y + (18 - app.mono.ch()) / 2, rd, t.hi);
            app.add_damage(rb);
        }
    }
}

//----------------------------------------------------------------------------
//  topbar hit-testing.  Returns true when the press was consumed.
//----------------------------------------------------------------------------
bool ArrangeView::topbar_click(App& app, const MouseEv& e)
{
    if (e.y >= rect.y + toolbar_h()) return false;
    const SDL_Keymod mod = SDL_GetModState();
    const bool ctrl  = (mod & KMOD_CTRL)  != 0;
    const bool shift = (mod & KMOD_SHIFT) != 0;
    const bool alt   = (mod & KMOD_ALT)   != 0;

    // ---- EDIT MODE block ---------------------------------------------------
    for (int m = 0; m < 4; ++m) {
        if (!pt_in(m_mode_rect[m], e.x, e.y)) continue;
        if (m == 0 && ctrl) {
            // Shuffle Lock: only from OUTSIDE Shuffle; Ctrl-click again unlocks.
            if (m_shuffle_lock) m_shuffle_lock = false;
            else if (m_edit_mode != EditMode::Shuffle) m_shuffle_lock = true;
        } else if (m == 3 && e.button == SDL_BUTTON_RIGHT) {
            m_grid_relative = !m_grid_relative;       // Absolute <-> Relative
        } else if (m == 3 && shift) {
            m_snap_to_grid = !m_snap_to_grid;         // Snap To Grid combine
        } else if (m != 3 && shift && m_edit_mode == EditMode::Grid) {
            set_edit_mode((EditMode)m);               // Shuffle/Slip/Spot + grid
            m_snap_to_grid = true;
        } else {
            set_edit_mode((EditMode)m);
        }
        m_mouse_down = false;
        app.request_redraw();
        return true;
    }

    // ---- tool strip --------------------------------------------------------
    for (int tool = 0; tool < 8; ++tool) {
        if (m_tool_rect[tool].w <= 0 || !pt_in(m_tool_rect[tool], e.x, e.y))
            continue;
        const Uint32 now = SDL_GetTicks();
        static Uint32 lastToolMs = 0; static int lastTool = -1;
        const bool dbl = (lastTool == tool && now - lastToolMs < 400);
        lastTool = tool; lastToolMs = now;
        if (e.button == SDL_BUTTON_RIGHT) {           // right-click: mode menu
            if (tool == (int)EditTool::Trim || tool == (int)EditTool::Grab ||
                tool == (int)EditTool::Zoom) {
                m_submode_menu = true; m_submode_tool = tool;
                m_submode_rect = SDL_Rect{ m_tool_rect[tool].x,
                                           rect.y + toolbar_h(), 150, 0 };
            }
            m_mouse_down = false; app.request_redraw(); return true;
        }
        if (tool == (int)EditTool::Zoom && dbl) { zoom_to_fit(); }
        else if (tool == (int)EditTool::Zoom && ctrl) { zoom_overview(); }
        else if (tool == (int)EditTool::Grab && dbl) {
            // double-click the Grabber: TIME selection -> OBJECT selection.
            // Ctrl includes clips only partially inside the range.
            if (m_sel_start >= 0 && m_sel_end > m_sel_start) {
                unselect_all_triggers();
                int lo, hi; sel_rows(lo, hi);
                std::vector<int> act = active_list();
                std::vector<ClipSpan> spans;
                for (int r = std::max(0, lo); r <= hi && r < (int)act.size(); ++r)
                    for (int cs : lane_sequences(act[(size_t)r])) {
                        spans.clear(); clip_spans(cs, spans);
                        for (const ClipSpan& sp : spans) {
                            const bool whole = sp.on >= m_sel_start && sp.endEx <= m_sel_end;
                            const bool touch = sp.on < m_sel_end && sp.endEx > m_sel_start;
                            if (whole || (ctrl && touch)) {
                                if (m_audio.count(cs)) region_for(cs).selected = true;
                                else if (sequence* sq = m_perf->get_sequence(cs))
                                    sq->select_trigger(sp.on);
                            }
                        }
                    }
                m_grab_mode = 2;                    // now an Object selection
            }
        } else if (tool == (int)EditTool::Range && dbl) {
            // double-click the Selector: OBJECT selection -> TIME selection
            long lo = -1, hi = -1;
            int rlo = 1 << 20, rhi = -1;
            std::vector<int> act = active_list();
            for_each_clip([&](const ClipSpan& s) {
                if (!s.selected) return;
                if (lo < 0 || s.on < lo) lo = s.on;
                if (s.endEx > hi) hi = s.endEx;
                for (int r = 0; r < (int)act.size(); ++r)
                    for (int cs : lane_sequences(act[(size_t)r]))
                        if (cs == s.seq) { rlo = std::min(rlo, r); rhi = std::max(rhi, r); }
            });
            if (lo >= 0 && hi > lo)
                set_edit_selection(lo, hi, rlo > rhi ? -1 : rlo, rhi);
        } else {
            select_tool((EditTool)tool, /*from_key=*/false);
        }
        m_hold_tool = tool; m_hold_ms = now;          // maybe a click-and-hold
        m_mouse_down = false;
        app.request_redraw();
        return true;
    }

    // ---- EDIT / VIEW menus + Layered Editing chip --------------------------
    if (pt_in(m_editbtn_rect, e.x, e.y)) {
        m_edit_menu = true;
        m_edit_menu_rect = SDL_Rect{ m_editbtn_rect.x - 20, rect.y + toolbar_h(), 210, 0 };
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_view_rect, e.x, e.y)) {
        m_view_menu = true;
        m_view_menu_rect = SDL_Rect{ m_view_rect.x - 20, rect.y + toolbar_h(), 210, 0 };
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_lay_rect, e.x, e.y)) {
        m_layered = !m_layered;
        m_mouse_down = false; app.request_redraw(); return true;
    }

    // ---- link / tab / universe / help chips --------------------------------
    if (pt_in(m_link_tl_rect, e.x, e.y)) {
        m_link_timeline = !m_link_timeline;
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_link_tr_rect, e.x, e.y)) {
        m_link_track = !m_link_track;
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_tab_rect, e.x, e.y)) {
        m_tab_transients = !m_tab_transients;
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_univ_btn_rect, e.x, e.y)) {
        m_universe_on = !m_universe_on;
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_help_rect, e.x, e.y)) {
        m_help_open = !m_help_open;
        m_mouse_down = false; app.request_redraw(); return true;
    }

    // ---- zoom presets / zoom toggle ---------------------------------------
    for (int i = 0; i < 5; ++i) {
        if (!pt_in(m_preset_rect[i], e.x, e.y)) continue;
        if (shift) { m_zoom_preset[i] = m_scale_x; }
        else if (m_zoom_preset[i] > 0.0) {
            remember_zoom();
            m_scale_x = m_zoom_preset[i];
            clamp_scroll();
        }
        m_hold_preset = i; m_hold_ms = SDL_GetTicks();
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_zt_rect, e.x, e.y)) {
        if (e.button == SDL_BUTTON_RIGHT) {
            m_zt_menu = true;
            m_zt_menu_rect = SDL_Rect{ m_zt_rect.x - 60, rect.y + toolbar_h(), 240, 0 };
        } else if (alt) {                              // clear the stored state
            m_zt_in_valid = false; m_zt_in = ZoomToggleState{};
        } else zoom_toggle(false);
        m_mouse_down = false; app.request_redraw(); return true;
    }

    // ---- zoom buttons (click = one step; alt-click = previous zoom) --------
    for (int i = 0; i < 3; ++i) {
        if (m_zoom_rect[i].w <= 0 || !pt_in(m_zoom_rect[i], e.x, e.y)) continue;
        if (i == 2) { remember_zoom(); zoom_to_fit(); }
        else if (alt) recall_prev_zoom();
        else zoom_at(canvas_x() + canvas_w() / 2, i == 0 ? 1.4 : 1.0 / 1.4);
        // drag on the button continues zooming (continuous zoom)
        m_zoomer_cont = (i != 2); m_zoomer_cx = e.x; m_zoomer_cy = e.y;
        m_zoomer_s0 = m_scale_x; m_zoomer_h0 = row_h;
        app.request_redraw(); return true;
    }
    // vertical zoom buttons
    for (int i = 0; i < 4; ++i) {
        if (m_vzoom_rect[i].w <= 0 || !pt_in(m_vzoom_rect[i], e.x, e.y)) continue;
        if (alt) { recall_prev_zoom(); }
        else if (i < 2 && ctrl && shift) {
            // Ctrl+Shift-click the audio zoom: match every waveform height to
            // the topmost track (all height offsets are lost, as documented).
            m_trackH.clear();
        } else if (i == 0) m_wave_zoom = std::max(0.25f, m_wave_zoom / 1.25f);
        else if (i == 1)   m_wave_zoom = std::min(8.f,   m_wave_zoom * 1.25f);
        else if (i == 2)   m_midi_zoom = std::max(0.25f, m_midi_zoom / 1.25f);
        else               m_midi_zoom = std::min(4.f,   m_midi_zoom * 1.25f);
        // dragging up/down on the button continues the zoom (ch.29 p647)
        m_vzoom_drag = i; m_vzoom_y0 = e.y;
        m_vzoom_v0 = (i < 2) ? m_wave_zoom : m_midi_zoom;
        app.request_redraw(); return true;
    }

    // ---- counters / indicators / grid / nudge fields ----------------------
    for (int i = 0; i < 4; ++i) {
        if (m_counter_rect[i].w <= 0 || !pt_in(m_counter_rect[i], e.x, e.y))
            continue;
        counter_begin(i);
        // press-and-drag scrubs the value; a bare click leaves typed entry up
        m_counter_scrub = true; m_counter_scrub_which = i;
        m_counter_scrub_y0 = e.y;
        m_counter_scrub_v0 = counter_value();
        app.request_redraw(); return true;
    }
    if (pt_in(m_grid_rect, e.x, e.y)) {
        m_grid_menu = true;
        m_grid_menu_rect = SDL_Rect{ m_grid_rect.x - 40, rect.y + toolbar_h(), 190, 0 };
        m_mouse_down = false; app.request_redraw(); return true;
    }
    if (pt_in(m_nudge_rect, e.x, e.y)) {
        m_nudge_menu = true;
        m_nudge_menu_rect = SDL_Rect{ m_nudge_rect.x - 20, rect.y + toolbar_h(), 100, 0 };
        m_mouse_down = false; app.request_redraw(); return true;
    }

    // a press anywhere else on the strip is dead space -- swallow it
    m_mouse_down = false;
    return true;
}

//----------------------------------------------------------------------------
//  pop-ups: tool sub-modes, the Grid value menu, Zoom Toggle preferences
//----------------------------------------------------------------------------
void ArrangeView::draw_submode_menu(App& app)
{
    if (!m_submode_menu) return;
    const Theme& t = theme();
    const int rowh = std::max(20, app.font.ch() + 8);
    int n = m_submode_tool == (int)EditTool::Trim ? 4
          : m_submode_tool == (int)EditTool::Grab ? 3 : 2;
    SDL_Rect box = pt_clamp_popup(SDL_Rect{ m_submode_rect.x, m_submode_rect.y,
                                            150, n * rowh + 2 }, rect);
    m_submode_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    for (int i = 0; i < n; ++i) {
        SDL_Rect row{ box.x + 1, box.y + 1 + i * rowh, box.w - 2, rowh - 1 };
        const bool hot = pt_in(row, m_mx, m_my);
        int cur = m_submode_tool == (int)EditTool::Trim ? m_trim_mode
                : m_submode_tool == (int)EditTool::Grab ? m_grab_mode : m_zoom_mode;
        const bool on = (i == cur);
        if (hot || on) fill_rect(app.ren, row, hot ? t.accent : t.keybg);
        const char* label =
            m_submode_tool == (int)EditTool::Trim ? trim_mode_name(i)
          : m_submode_tool == (int)EditTool::Grab ? grab_mode_name(i)
          : (i == 0 ? "Normal Zoom" : "Single Zoom");
        app.font.draw(app.ren, row.x + 8, row.y + 4, label,
                      hot ? t.bg : (on ? t.hi : t.text));
    }
}

bool ArrangeView::submode_click(App& app, int mx, int my)
{
    const int rowh = std::max(20, app.font.ch() + 8);
    const SDL_Rect& b = m_submode_rect;
    if (pt_in(b, mx, my)) {
        const int i = (my - b.y - 1) / rowh;
        if (m_submode_tool == (int)EditTool::Trim && i >= 0 && i < 4) m_trim_mode = i;
        if (m_submode_tool == (int)EditTool::Grab && i >= 0 && i < 3) m_grab_mode = i;
        if (m_submode_tool == (int)EditTool::Zoom && i >= 0 && i < 2) m_zoom_mode = i;
        m_edit_tool = (EditTool)m_submode_tool;
    }
    m_submode_menu = false; m_submode_tool = -1;
    app.request_redraw();
    return true;
}

namespace {
// Grid value menu rows (fig. pt-674-037): value ladder, modifiers, time
// scale, follow-main.  Negative ids are separators.
struct GridRow { int id; const char* label; };
const GridRow kGridRows[] = {
    { 0, "1 bar" }, { 1, "1/2 note" }, { 2, "1/4 note" }, { 3, "1/8 note" },
    { 4, "1/16 note" }, { 5, "1/32 note" }, { 6, "1/64 note" },
    { -1, "" },
    { 10, "dotted" }, { 11, "triplet" },
    { -1, "" },
    { 20, "Bars|Beats" }, { 21, "Min:Secs" }, { 22, "Samples" },
    { 23, "Clips/Markers" },
    { -1, "" },
    { 30, "Follow Main Time Scale" },
    { 31, "Edit/Tool Mode Kbd Lock" },
};
const int kGridRowsN = (int)(sizeof(kGridRows) / sizeof(kGridRows[0]));
}

void ArrangeView::draw_grid_menu(App& app)
{
    if (!m_grid_menu) return;
    const Theme& t = theme();
    const int rowh = std::max(18, app.font.ch() + 6);
    int h = 2;
    for (int i = 0; i < kGridRowsN; ++i) h += kGridRows[i].id < 0 ? 5 : rowh;
    SDL_Rect box = pt_clamp_popup(SDL_Rect{ m_grid_menu_rect.x,
                                            m_grid_menu_rect.y, 190, h }, rect);
    m_grid_menu_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    int y = box.y + 1;
    for (int i = 0; i < kGridRowsN; ++i) {
        const GridRow& r = kGridRows[i];
        if (r.id < 0) { hline(app.ren, box.x + 4, box.x + box.w - 4, y + 2, t.dim);
                        y += 5; continue; }
        SDL_Rect row{ box.x + 1, y, box.w - 2, rowh };
        const bool hot = pt_in(row, m_mx, m_my);
        bool on = false;
        if (r.id < 10)       on = (m_snap_idx == r.id && m_grid_scale != 3);
        else if (r.id == 10) on = m_grid_dotted;
        else if (r.id == 11) on = m_grid_triplet;
        else if (r.id < 30)  on = (m_grid_scale == r.id - 20);
        else if (r.id == 30) on = m_grid_follow_main;
        else                 on = m_tool_lock;
        if (hot) fill_rect(app.ren, row, t.accent);
        if (on)  app.font.draw(app.ren, row.x + 5, row.y + 3, "*",
                               hot ? t.bg : t.hi);
        app.font.draw(app.ren, row.x + 18, row.y + 3, r.label,
                      hot ? t.bg : (on ? t.hi : t.text));
        y += rowh;
    }
}

bool ArrangeView::grid_menu_click(App& app, int mx, int my)
{
    const int rowh = std::max(18, app.font.ch() + 6);
    const SDL_Rect& b = m_grid_menu_rect;
    if (pt_in(b, mx, my)) {
        int y = b.y + 1;
        for (int i = 0; i < kGridRowsN; ++i) {
            const GridRow& r = kGridRows[i];
            const int rh = r.id < 0 ? 5 : rowh;
            if (my >= y && my < y + rh && r.id >= 0) {
                if (r.id < 10)       { m_snap_idx = r.id;
                                       if (m_grid_scale == 3) m_grid_scale = 0; }
                else if (r.id == 10) m_grid_dotted  = !m_grid_dotted;
                else if (r.id == 11) m_grid_triplet = !m_grid_triplet;
                else if (r.id < 30)  { m_grid_scale = r.id - 20;
                                       m_grid_follow_main = (m_grid_scale == 0); }
                else if (r.id == 30) { m_grid_follow_main = !m_grid_follow_main;
                                       if (m_grid_follow_main) m_grid_scale = 0; }
                else                 m_tool_lock = !m_tool_lock;
                m_snap = grid_ticks();
                break;
            }
            y += rh;
        }
    }
    m_grid_menu = false;
    app.request_redraw();
    return true;
}

namespace {
const char* kZtHeights[6] = { "Last Used", "Medium", "Large", "Jumbo",
                              "Extreme", "Fit To Window" };
}

void ArrangeView::draw_zt_menu(App& app)
{
    if (!m_zt_menu) return;
    const Theme& t = theme();
    const int rowh = std::max(18, app.font.ch() + 6);
    const int n = 8;
    SDL_Rect box = pt_clamp_popup(SDL_Rect{ m_zt_menu_rect.x, m_zt_menu_rect.y,
                                            250, n * rowh + 2 }, rect);
    m_zt_menu_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    char line[8][64];
    std::snprintf(line[0], 64, "Vertical Zoom: %s",
                  m_zt_pref_v == 0 ? "Selection" : "Last Used");
    std::snprintf(line[1], 64, "Horizontal Zoom: %s",
                  m_zt_pref_h == 0 ? "Selection" : "Last Used");
    std::snprintf(line[2], 64, "Track Height: %s", kZtHeights[m_zt_pref_height]);
    std::snprintf(line[3], 64, "Track View: No Change");
    std::snprintf(line[4], 64, "[%c] Remove Range After Zoom In",
                  m_zt_remove_range ? '*' : ' ');
    std::snprintf(line[5], 64, "[%c] Separate Grid When Zoomed",
                  m_zt_sep_grid ? '*' : ' ');
    std::snprintf(line[6], 64, "[%c] Follows Edit Selection",
                  m_zt_follow_sel ? '*' : ' ');
    std::snprintf(line[7], 64, "Clear stored state");
    for (int i = 0; i < n; ++i) {
        SDL_Rect row{ box.x + 1, box.y + 1 + i * rowh, box.w - 2, rowh };
        const bool hot = pt_in(row, m_mx, m_my);
        if (hot) fill_rect(app.ren, row, t.accent);
        app.font.draw(app.ren, row.x + 6, row.y + 3, line[i],
                      hot ? t.bg : (i == 3 ? t.dim : t.text));
    }
}

bool ArrangeView::zt_menu_click(App& app, int mx, int my)
{
    const int rowh = std::max(18, app.font.ch() + 6);
    const SDL_Rect& b = m_zt_menu_rect;
    if (pt_in(b, mx, my)) {
        const int i = (my - b.y - 1) / rowh;
        switch (i) {
        case 0: m_zt_pref_v = (m_zt_pref_v + 1) % 2; break;
        case 1: m_zt_pref_h = (m_zt_pref_h + 1) % 2; break;
        case 2: m_zt_pref_height = (m_zt_pref_height + 1) % 6; break;
        case 3: /* Track View: PatchKnob has one playlist view */ break;
        case 4: m_zt_remove_range = !m_zt_remove_range; break;
        case 5: m_zt_sep_grid = !m_zt_sep_grid; break;
        case 6: m_zt_follow_sel = !m_zt_follow_sel; break;
        case 7: m_zt_in_valid = false; m_zt_in = ZoomToggleState{}; break;
        default: break;
        }
        // preference rows stay open so several can be set in one visit
        if (i >= 0 && i <= 6) { app.request_redraw(); return true; }
    }
    m_zt_menu = false;
    app.request_redraw();
    return true;
}

// Nudge value picker (fig. pt-732-128): the musical ladder, plus "Follow
// Main Time Scale" and a typed custom value (ch.31 p732).
void ArrangeView::draw_nudge_menu(App& app)
{
    if (!m_nudge_menu) return;
    const Theme& t = theme();
    const int rowh = std::max(18, app.font.ch() + 6);
    SDL_Rect box = pt_clamp_popup(SDL_Rect{ m_nudge_menu_rect.x,
                                            m_nudge_menu_rect.y, 150,
                                            10 * rowh + 2 }, rect);
    m_nudge_menu_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    for (int i = 0; i < 10; ++i) {
        SDL_Rect row{ box.x + 1, box.y + 1 + i * rowh, box.w - 2, rowh };
        const bool hot = pt_in(row, m_mx, m_my);
        bool on = false;
        const char* label = "";
        if (i < 8) { on = (i == m_nudge_idx && m_nudge_custom <= 0 && !m_nudge_follow);
                     label = snap_label(i); }
        else if (i == 8) { on = m_nudge_follow && m_nudge_custom <= 0;
                           label = "Follow Main Time Scale"; }
        else { on = m_nudge_custom > 0; label = "Custom (type ticks)..."; }
        if (hot || on) fill_rect(app.ren, row, hot ? t.accent : t.keybg);
        app.mono.draw(app.ren, row.x + 8, row.y + 3, label,
                      hot ? t.bg : (on ? t.hi : t.text));
    }
}

bool ArrangeView::nudge_menu_click(App& app, int mx, int my)
{
    const int rowh = std::max(18, app.font.ch() + 6);
    const SDL_Rect& b = m_nudge_menu_rect;
    if (pt_in(b, mx, my)) {
        const int i = (my - b.y - 1) / rowh;
        if (i >= 0 && i < 8) { m_nudge_idx = i; m_nudge_custom = 0; m_nudge_follow = false; }
        else if (i == 8)     { m_nudge_follow = !m_nudge_follow; m_nudge_custom = 0; }
        else if (i == 9) {
            // typed custom nudge value: plain ticks, or "Nms" for milliseconds
            m_dlg_buf = m_nudge_custom > 0 ? std::to_string(m_nudge_custom) : "96";
            app.begin_text(&m_dlg_buf, nullptr, [this, &app](bool ok) {
                if (ok) {
                    const char* c = m_dlg_buf.c_str();
                    long v = std::atol(c);
                    if (m_dlg_buf.find("ms") != std::string::npos)
                        v = sec_to_ticks((double)v / 1000.0);
                    m_nudge_custom = v > 0 ? v : 0;
                }
                app.request_redraw();
            });
        }
    }
    m_nudge_menu = false;
    app.request_redraw();
    return true;
}

} // namespace arrange

