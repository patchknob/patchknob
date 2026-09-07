//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_fades.cpp
//
//  Pro Tools ch.32 -- FADES AND CROSSFADES (manual p741-761).
//
//  The fade MODEL: every audio clip carries a ClipFade (lengths, tensions,
//  shapes, slopes; see arrange_view.h).  A CROSSFADE is two lane-mates whose
//  regions OVERLAP, the left carrying a fade-out and the right a fade-in over
//  the overlap window -- the engine sums the two enveloped regions, so a
//  crossfade is heard through the real signal path, not just drawn.  The
//  curves themselves are evaluated by engine ScheduledClip::fadeCurve, so the
//  line drawn here is bit-for-bit the envelope played.
//
//  In this file:
//    * crossfade creation (Centered / Pre / Post placements, material checks)
//    * the Fade In / Fade Out / Crossfade dialog (fig. pt-743-148/149):
//      curve+waveform plot, view modes, amplitude zoom, Audition, shape /
//      slope / link groups with preset thumbnails, curve dragging, Link=None
//      endpoint handles, Presets 1-5, the settings-file menu and COMPARE
//    * Edit > Fades commands: Create (Ctrl+F), default-settings create
//      (Ctrl+Win+F), Fade To Start / End (Win+D / Win+G), Delete, Batch Fades
//    * the Batch Fades dialog (fig. pt-753-186)
//    * fade selection with the Grabber: move / nudge / delete a fade or
//      crossfade independently of its clips
//    * the fade & crossfade preferences (default shapes, preview pre/post
//      roll, AutoFades, Preserve Fades when Editing, Smart-tool modifier)
//
//  Undo: every fade edit runs through fade_edit_op(), which snapshots the
//  fade+region state of the touched clips and queues ONE entry on the ch.28
//  Multiple-Undo queue -- the same closure pattern the region trims use.
//----------------------------------------------------------------------------
#include "arrange_view.h"
#include "perform.h"
#include "sequence.h"
#include "globals.h"
#include "engine/audioclip/audio_clip.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <vector>

using namespace ui;
using PatchKnob::engine::AudioClip;
using PatchKnob::engine::ScheduledClip;

namespace arrange {

namespace {

bool fd_in(const SDL_Rect& r, int x, int y)
{
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

SDL_Rect fd_clamp(SDL_Rect box, const SDL_Rect& bounds)
{
    if (box.w > bounds.w) box.w = bounds.w;
    if (box.h > bounds.h) box.h = bounds.h;
    if (box.x + box.w > bounds.x + bounds.w) box.x = bounds.x + bounds.w - box.w;
    if (box.y + box.h > bounds.y + bounds.h) box.y = bounds.y + bounds.h - box.h;
    if (box.x < bounds.x) box.x = bounds.x;
    if (box.y < bounds.y) box.y = bounds.y;
    return box;
}

//! Display names for the shape encoding (0 Standard, 1 S-Curve, 2..8 presets).
const char* shape_name(int s)
{
    static const char* N[9] = { "Standard", "S-Curve", "Preset 1", "Preset 2",
                                "Preset 3", "Preset 4", "Preset 5", "Preset 6",
                                "Preset 7" };
    return N[(s < 0 || s > 8) ? 0 : s];
}

const char* slope_name(int s) { return s == 1 ? "Equal Power" : "Equal Gain"; }
const char* link_name(int l)
{
    return l == 0 ? "Equal Power" : (l == 1 ? "Equal Gain" : "None");
}

} // anonymous namespace

//----------------------------------------------------------------------------
//  model helpers
//----------------------------------------------------------------------------
float ArrangeView::fade_curve(float u, int shape, float k, int slope)
{
    return ScheduledClip::fadeCurve(u, shape, k, slope);
}

ArrangeView::ClipFade ArrangeView::fade_of(int seq) const
{
    std::map<int, ClipFade>::const_iterator it = m_clipFade.find(seq);
    return it == m_clipFade.end() ? ClipFade{} : it->second;
}

long ArrangeView::ms_ticks(int ms) const
{
    long t = sec_to_ticks((double)ms / 1000.0);
    return t < 1 ? 1 : t;
}

// A crossfade is two audio lane-mates whose regions overlap.  Given either
// contributor, resolve the pair (earliest-starting = left).
bool ArrangeView::xfade_pair(int seq, int& left, int& right) const
{
    if (!m_audio.count(seq)) return false;
    std::map<int, AudioRegion>::const_iterator ri = m_region.find(seq);
    if (ri == m_region.end()) return false;
    const AudioRegion& r = ri->second;
    for (int cs : lane_sequences(seq)) {
        if (cs == seq || !m_audio.count(cs)) continue;
        std::map<int, AudioRegion>::const_iterator oi = m_region.find(cs);
        if (oi == m_region.end()) continue;
        const AudioRegion& o = oi->second;
        if (o.position < r.position + r.length &&
            o.position + o.length > r.position) {
            if (o.position <= r.position) { left = cs;  right = seq; }
            else                          { left = seq; right = cs;  }
            return true;
        }
    }
    return false;
}

// Overlap window [a,b) of a crossfade pair.
bool ArrangeView::xfade_window(int left, int right, long& a, long& b) const
{
    std::map<int, AudioRegion>::const_iterator li = m_region.find(left);
    std::map<int, AudioRegion>::const_iterator ri = m_region.find(right);
    if (li == m_region.end() || ri == m_region.end()) return false;
    a = ri->second.position;
    b = std::min(li->second.position + li->second.length,
                 ri->second.position + ri->second.length);
    return b > a;
}

// Source material (ticks) available before / after a clip's region window --
// the manual's "a crossfade cannot be performed on clips that do not contain
// audio material beyond their clip boundaries" check.
long ArrangeView::avail_before(int seq) const
{
    std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
    return it == m_region.end() ? 0 : std::max<long>(0, it->second.source);
}

long ArrangeView::avail_after(int seq) const
{
    std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
    if (it == m_region.end()) return 0;
    const AudioRegion& r = it->second;
    if (r.loop) return 1L << 30;               // a looped source wraps forever
    std::map<int, long>::const_iterator lit = m_audioLen.find(seq);
    const long srcLen = (lit != m_audioLen.end() && lit->second > 0)
                      ? lit->second : r.source + r.length;
    return std::max<long>(0, srcLen - (r.source + r.length));
}

//----------------------------------------------------------------------------
//  undo plumbing: fade+region snapshots on the ch.28 Multiple-Undo queue
//----------------------------------------------------------------------------
void ArrangeView::capture_fade_state(const std::vector<int>& seqs,
                                     std::vector<FadeState>& out) const
{
    for (int seq : seqs) {
        FadeState st; st.seq = seq;
        std::map<int, ClipFade>::const_iterator fi = m_clipFade.find(seq);
        st.hadFade = fi != m_clipFade.end();
        if (st.hadFade) st.fade = fi->second;
        std::map<int, AudioRegion>::const_iterator ri = m_region.find(seq);
        st.hadRegion = ri != m_region.end();
        if (st.hadRegion) st.region = ri->second;
        out.push_back(st);
    }
}

void ArrangeView::restore_fade_state(const std::vector<FadeState>& st)
{
    for (const FadeState& s : st) {
        if (s.hadRegion) { m_region[s.seq] = s.region; commit_region(s.seq); }
        if (s.hadFade) m_clipFade[s.seq] = s.fade;
        else           m_clipFade.erase(s.seq);
        commit_fade(s.seq);
    }
}

void ArrangeView::fade_edit_op(const char* name, const std::vector<int>& seqs,
                               const std::function<void()>& edit)
{
    std::vector<FadeState> before;
    capture_fade_state(seqs, before);
    edit();
    std::vector<FadeState> after;
    capture_fade_state(seqs, after);
    UndoOp op; op.name = name ? name : "Fade";
    op.undo.push_back([this, before]{ restore_fade_state(before); });
    op.redo.push_back([this, after]{ restore_fade_state(after); });
    push_undo_op(std::move(op));
}

//----------------------------------------------------------------------------
//  crossfade creation / removal / recalculation
//----------------------------------------------------------------------------
// Make the overlap of `left`/`right` the window [a,b) and give both halves the
// settings `s`.  The window is CLAMPED to the source material available beyond
// each clip's boundary (the manual's "adjust the bounds of the selection"
// path); returns false when no overlap is possible at all (the "skip this
// fade" path).  Undo handling belongs to the caller.
bool ArrangeView::create_crossfade(int left, int right, long a, long b,
                                   const FadeSettings& s, bool clampNote)
{
    if (left == right || left < 0 || right < 0) return false;
    if (!m_audio.count(left) || !m_audio.count(right)) return false;
    AudioRegion& L = region_for(left);
    AudioRegion& R = region_for(right);
    if (L.position > R.position) return false;      // caller passes them sorted

    // Clamp the wanted window to what the sources can actually supply, and
    // keep at least one tick of each clip un-crossfaded.
    const long minA = std::max(L.position + 1, R.position - avail_before(right));
    const long maxB = std::min(R.position + R.length - 1,
                               L.position + L.length + avail_after(left));
    bool clamped = false;
    if (a < minA) { a = minA; clamped = true; }
    if (b > maxB) { b = maxB; clamped = true; }
    if (b - a < 1) {
        flash("No audio beyond the clip boundary: crossfade skipped");
        return false;
    }
    if (clamped && clampNote)
        flash("Crossfade bounds adjusted to the available audio");

    // Left contributes its tail up to `b`; right starts at `a` (its source
    // offset slides so the audio underneath does not move).
    const long lEnd = L.position + L.length;
    if (b != lEnd) L.length += b - lEnd;
    const long d = R.position - a;                  // >0 extends the front
    R.position = a; R.source -= d; R.length += d;
    if (R.source < 0) R.source = 0;                 // paranoia (avail_before clamped)

    const long w = b - a;
    ClipFade lf = fade_of(left);
    ClipFade rf = fade_of(right);
    lf.outTicks = w; lf.outK = s.outK; lf.outShape = s.outShape;
    rf.inTicks  = w; rf.inK  = s.inK;  rf.inShape  = s.inShape;
    rf.link = s.link;
    // Link Equal Power / Equal Gain force both slopes; None frees them.
    if      (s.link == 0) { lf.outSlope = 1; rf.inSlope = 1; }
    else if (s.link == 1) { lf.outSlope = 0; rf.inSlope = 0; }
    else                  { lf.outSlope = s.outSlope; rf.inSlope = s.inSlope; }
    m_clipFade[left] = lf; m_clipFade[right] = rf;
    commit_region(left); commit_region(right);
    commit_fade(left); commit_fade(right);
    return true;
}

// Remove the crossfade: fades cleared, regions retracted to a butt joint at
// the overlap midpoint (the splice is not stored, so the midpoint is the
// symmetric choice; a Centered crossfade returns exactly to its splice).
void ArrangeView::remove_crossfade(int left, int right)
{
    long a = 0, b = 0;
    if (!xfade_window(left, right, a, b)) return;
    const long mid = a + (b - a) / 2;
    AudioRegion& L = region_for(left);
    AudioRegion& R = region_for(right);
    L.length = std::max<long>(1, mid - L.position);
    const long d = mid - R.position;
    R.position = mid; R.source += d; R.length = std::max<long>(1, R.length - d);
    ClipFade lf = fade_of(left);  lf.outTicks = 0; lf.outK = 0.f;
    ClipFade rf = fade_of(right); rf.inTicks = 0; rf.inK = 0.f; rf.link = 0;
    m_clipFade[left] = lf; m_clipFade[right] = rf;
    commit_region(left); commit_region(right);
    commit_fade(left); commit_fade(right);
}

// After a region edit (trim / nudge / move), recalculate the crossfades the
// clip contributes to: trimming either side of a crossfade re-renders it for
// the new overlap (p752 "Trim a crossfade"); with Link = None the halves are
// only clamped, never forced equal.
void ArrangeView::sync_xfade_after_region_edit(int seq)
{
    if (!m_audio.count(seq)) return;
    int l = -1, r = -1;
    if (!xfade_pair(seq, l, r)) return;
    long a = 0, b = 0;
    if (!xfade_window(l, r, a, b)) return;
    const long w = b - a;
    ClipFade lf = fade_of(l), rf = fade_of(r);
    if (lf.outTicks <= 0 && rf.inTicks <= 0) return;   // plain overlap, no xfade
    bool changed = false;
    if (rf.link == 2) {                                // None: clamp only
        if (lf.outTicks > w) { lf.outTicks = w; changed = true; }
        if (rf.inTicks  > w) { rf.inTicks  = w; changed = true; }
    } else {
        if (lf.outTicks != w) { lf.outTicks = w; changed = true; }
        if (rf.inTicks  != w) { rf.inTicks  = w; changed = true; }
    }
    if (changed) {
        m_clipFade[l] = lf; m_clipFade[r] = rf;
        commit_fade(l); commit_fade(r);
    }
}

// After nudging/moving a clip that contributed to a crossfade: the crossfade
// stretches to keep its outer points; pushed past the available overlap it is
// removed, and clips separated cleanly keep or drop their fades per the
// Preserve Fades when Editing preference (p757-759).
void ArrangeView::stretch_xfades_after_nudge(int seq, long oldPos, long oldEnd)
{
    if (!m_audio.count(seq)) return;
    const AudioRegion& r = region_for(seq);
    ClipFade f = fade_of(seq);
    // Did this clip's fades come from a crossfade that is now gone?
    int l = -1, rr = -1;
    const bool paired = xfade_pair(seq, l, rr);
    if (!paired && (f.inTicks > 0 || f.outTicks > 0)) {
        // find the former partner: any lane-mate that used to butt/overlap us
        for (int cs : lane_sequences(seq)) {
            if (cs == seq || !m_audio.count(cs)) continue;
            const AudioRegion& o = region_for(cs);
            ClipFade of = fade_of(cs);
            const bool wasLeftOfUs  = of.outTicks > 0 && f.inTicks > 0 &&
                                      o.position + o.length > oldPos - 1 &&
                                      o.position < oldPos + 1;
            const bool wasRightOfUs = of.inTicks > 0 && f.outTicks > 0 &&
                                      o.position < oldEnd + 1 &&
                                      o.position + o.length > oldEnd - 1;
            if (!wasLeftOfUs && !wasRightOfUs) continue;
            if (!m_preserve_fades) {
                if (wasLeftOfUs)  { f.inTicks = 0; f.inK = 0.f;
                                    of.outTicks = 0; of.outK = 0.f; }
                if (wasRightOfUs) { f.outTicks = 0; f.outK = 0.f;
                                    of.inTicks = 0; of.inK = 0.f; }
                m_clipFade[seq] = f; m_clipFade[cs] = of;
                commit_fade(seq); commit_fade(cs);
            }
            // Preserve Fades ON keeps the halves as a plain fade-out + fade-in.
        }
        return;
    }
    (void)r;
    sync_xfade_after_region_edit(seq);
}

//----------------------------------------------------------------------------
//  Edit > Fades commands (p749-753)
//----------------------------------------------------------------------------
// Ctrl+F: the selection decides what is created (p749).  At a clip start ->
// fade-in, at a clip end -> fade-out, across a splice -> crossfade; several
// WHOLE clips -> the Batch Fades dialog.  `useDefaults` (Ctrl+Win+F) applies
// the Editing-preferences base settings without opening the dialog.
void ArrangeView::create_fades_from_selection(App& app, bool useDefaults)
{
    if (!have_range()) { flash("Make an Edit selection across a clip edge first"); return; }
    const long A = m_sel_start, B = m_sel_end;
    std::vector<int> lanes;
    range_lanes(lanes);

    std::vector<FadeTarget> targets;
    bool batch = false;
    std::vector<char> seen(c_max_sequence, 0);
    for (int lane : lanes) {
        // audio clips on this lane, sorted by position
        struct Span { int seq; long on, endEx; };
        std::vector<Span> spans;
        for (int cs : lane_sequences(lane)) {
            if (!m_audio.count(cs) || seen[(size_t)cs]) continue;
            seen[(size_t)cs] = 1;
            std::map<int, AudioRegion>::const_iterator it = m_region.find(cs);
            long on, endEx;
            if (it != m_region.end()) { on = it->second.position; endEx = on + it->second.length; }
            else {
                std::vector<ClipSpan> tmp; clip_spans(cs, tmp);
                if (tmp.empty()) continue;
                on = tmp[0].on; endEx = tmp[0].endEx;
            }
            spans.push_back(Span{ cs, on, endEx });
        }
        std::sort(spans.begin(), spans.end(),
                  [](const Span& x, const Span& y){ return x.on < y.on; });
        int whole = 0;
        for (const Span& sp : spans)
            if (sp.on >= A && sp.endEx <= B) ++whole;
        // several whole clips -> Batch Fades (p753); ONE whole clip also goes
        // through Batch (it gets its edge fade-in + fade-out at the typed
        // lengths -- a whole-clip window is no length for a single fade).
        if (whole >= 1) { batch = true; continue; }

        // crossfade: the selection crosses the joint between two neighbours
        bool made = false;
        for (size_t i = 0; i + 1 < spans.size() && !made; ++i) {
            const Span& l = spans[i]; const Span& rr = spans[i + 1];
            const long splice = rr.on;                 // butt joint or overlap start
            if (A > l.on && A < splice + 1 && B > splice - 1 && B < rr.endEx &&
                A >= l.on && B <= std::max(rr.endEx, l.endEx)) {
                if (A < splice || B > splice) {
                    FadeTarget t; t.kind = 2; t.left = l.seq; t.right = rr.seq;
                    t.a = A; t.b = B;
                    targets.push_back(t);
                    made = true;
                }
            }
        }
        if (made) continue;
        // fade-in: selection reaches (or precedes) a clip's start
        for (const Span& sp : spans) {
            if (A <= sp.on && B > sp.on && B <= sp.endEx) {
                FadeTarget t; t.kind = 0; t.seq = sp.seq; t.a = sp.on; t.b = B;
                targets.push_back(t); made = true; break;
            }
        }
        if (made) continue;
        // fade-out: selection reaches (or passes) a clip's end
        for (const Span& sp : spans) {
            if (B >= sp.endEx && A < sp.endEx && A >= sp.on) {
                FadeTarget t; t.kind = 1; t.seq = sp.seq; t.a = A; t.b = sp.endEx;
                targets.push_back(t); break;
            }
        }
    }

    if (batch) { open_batch_dialog(app); return; }
    if (targets.empty()) { flash("Selection does not reach an audio clip edge"); return; }
    if (!useDefaults) { open_fade_dialog(app, targets); return; }

    // default-settings create, no dialog (Ctrl+Win+F, p748)
    std::vector<int> seqs;
    for (const FadeTarget& t : targets) {
        if (t.kind == 2) { seqs.push_back(t.left); seqs.push_back(t.right); }
        else             seqs.push_back(t.seq);
    }
    fade_edit_op("Create Fades", seqs, [&]{
        for (const FadeTarget& t : targets) {
            if (t.kind == 2) {
                create_crossfade(t.left, t.right, t.a, t.b, m_def_xfade, true);
            } else if (t.kind == 0) {
                ClipFade f = fade_of(t.seq);
                f.inTicks = t.b - t.a; f.inK = m_def_fadein.inK;
                f.inShape = m_def_fadein.inShape; f.inSlope = m_def_fadein.inSlope;
                m_clipFade[t.seq] = f; commit_fade(t.seq);
            } else {
                ClipFade f = fade_of(t.seq);
                f.outTicks = t.b - t.a; f.outK = m_def_fadeout.outK;
                f.outShape = m_def_fadeout.outShape; f.outSlope = m_def_fadeout.outSlope;
                m_clipFade[t.seq] = f; commit_fade(t.seq);
            }
        }
    });
    flash("Fades created (" + std::to_string(targets.size()) + ")");
}

// Win+D / Win+G: fade from the edit insertion point to the clip start / end,
// with the default Fade In / Fade Out preferences (p751).
void ArrangeView::fade_to_start(App& app)
{
    (void)app;
    if (m_sel_start < 0) { flash("Place the cursor in a clip first"); return; }
    const long cur = m_sel_start;
    int lo, hi; sel_rows(lo, hi);
    std::vector<int> act = active_list();
    for (int rw = std::max(0, lo); rw <= hi && rw < (int)act.size(); ++rw) {
        const int cs = clip_sequence_at(act[(size_t)rw], cur);
        if (cs < 0 || !m_audio.count(cs)) continue;
        const AudioRegion& r = region_for(cs);
        if (cur <= r.position) continue;
        fade_edit_op("Fade To Start", { cs }, [&]{
            ClipFade f = fade_of(cs);
            f.inTicks = cur - r.position; f.inK = m_def_fadein.inK;
            f.inShape = m_def_fadein.inShape; f.inSlope = m_def_fadein.inSlope;
            m_clipFade[cs] = f; commit_fade(cs);
        });
        flash("Fade to start");
        return;
    }
    flash("No audio clip at the cursor");
}

void ArrangeView::fade_to_end(App& app)
{
    (void)app;
    if (m_sel_start < 0) { flash("Place the cursor in a clip first"); return; }
    const long cur = m_sel_start;
    int lo, hi; sel_rows(lo, hi);
    std::vector<int> act = active_list();
    for (int rw = std::max(0, lo); rw <= hi && rw < (int)act.size(); ++rw) {
        const int cs = clip_sequence_at(act[(size_t)rw], cur);
        if (cs < 0 || !m_audio.count(cs)) continue;
        const AudioRegion& r = region_for(cs);
        const long end = r.position + r.length;
        if (cur >= end) continue;
        fade_edit_op("Fade To End", { cs }, [&]{
            ClipFade f = fade_of(cs);
            f.outTicks = end - cur; f.outK = m_def_fadeout.outK;
            f.outShape = m_def_fadeout.outShape; f.outSlope = m_def_fadeout.outSlope;
            m_clipFade[cs] = f; commit_fade(cs);
        });
        flash("Fade to end");
        return;
    }
    flash("No audio clip at the cursor");
}

// Edit > Fades > Delete: clear every fade the selection touches; a crossfade
// fully inside the range is removed outright (regions retract to a butt
// joint).  With no range, the Grabber's fade selection is the target (p751).
void ArrangeView::delete_fades_selection()
{
    if (!have_range() && m_fade_sel_seq >= 0) {
        const int seq = m_fade_sel_seq, which = m_fade_sel_which;
        int l = -1, r = -1;
        if (which == 2 && xfade_pair(seq, l, r)) {
            fade_edit_op("Delete Crossfade", { l, r }, [&]{ remove_crossfade(l, r); });
        } else {
            fade_edit_op("Delete Fade", { seq }, [&]{
                ClipFade f = fade_of(seq);
                if (which == 0) { f.inTicks = 0; f.inK = 0.f; }
                else            { f.outTicks = 0; f.outK = 0.f; }
                m_clipFade[seq] = f; commit_fade(seq);
            });
        }
        m_fade_sel_seq = -1; m_fade_move = false;
        flash("Fade deleted");
        return;
    }
    if (!have_range()) { flash("Select the fades to delete"); return; }
    const long A = m_sel_start, B = m_sel_end;
    std::vector<int> lanes; range_lanes(lanes);
    std::vector<int> seqs;
    std::vector<char> seen(c_max_sequence, 0);
    for (int lane : lanes)
        for (int cs : lane_sequences(lane))
            if (m_audio.count(cs) && !seen[(size_t)cs]) { seen[(size_t)cs] = 1; seqs.push_back(cs); }
    if (seqs.empty()) { flash("No audio clips in the selection"); return; }
    int n = 0;
    fade_edit_op("Delete Fades", seqs, [&]{
        // crossfades first (both contributors in `seqs`)
        for (int cs : seqs) {
            int l = -1, r = -1;
            if (!xfade_pair(cs, l, r) || cs != r) continue;   // visit once, from the right
            long a = 0, b = 0;
            if (!xfade_window(l, r, a, b)) continue;
            if (a >= A && b <= B && (fade_of(l).outTicks > 0 || fade_of(r).inTicks > 0)) {
                remove_crossfade(l, r); ++n;
            }
        }
        for (int cs : seqs) {
            ClipFade f = fade_of(cs);
            const AudioRegion& rg = region_for(cs);
            bool ch = false;
            if (f.inTicks > 0 && rg.position >= A && rg.position + f.inTicks <= B) {
                f.inTicks = 0; f.inK = 0.f; ch = true;
            }
            const long end = rg.position + rg.length;
            if (f.outTicks > 0 && end <= B && end - f.outTicks >= A) {
                f.outTicks = 0; f.outK = 0.f; ch = true;
            }
            if (ch) { m_clipFade[cs] = f; commit_fade(cs); ++n; }
        }
    });
    flash(n ? "Deleted fades (" + std::to_string(n) + ")" : "No fades in the selection");
}

//----------------------------------------------------------------------------
//  fade selection helpers + shape cycling (p755-757)
//----------------------------------------------------------------------------
// Fades completely inside the edit selection (or the Grabber's fade
// selection).  Each entry: {seq, half} with half 0 = fade-in, 1 = fade-out.
void ArrangeView::selected_fades(std::vector<std::pair<int,int>>& out) const
{
    if (m_fade_sel_seq >= 0) {
        if (m_fade_sel_which == 2) {
            int l = -1, r = -1;
            if (xfade_pair(m_fade_sel_seq, l, r)) {
                out.push_back(std::make_pair(l, 1));
                out.push_back(std::make_pair(r, 0));
                return;
            }
        }
        out.push_back(std::make_pair(m_fade_sel_seq, m_fade_sel_which == 1 ? 1 : 0));
        return;
    }
    if (!have_range()) return;
    const long A = m_sel_start, B = m_sel_end;
    std::vector<int> lanes; range_lanes(lanes);
    std::vector<char> seen(c_max_sequence, 0);
    for (int lane : lanes)
        for (int cs : lane_sequences(lane)) {
            if (!m_audio.count(cs) || seen[(size_t)cs]) continue;
            seen[(size_t)cs] = 1;
            std::map<int, AudioRegion>::const_iterator it = m_region.find(cs);
            if (it == m_region.end()) continue;
            const AudioRegion& r = it->second;
            const ClipFade f = fade_of(cs);
            if (f.inTicks > 0 && r.position >= A && r.position + f.inTicks <= B)
                out.push_back(std::make_pair(cs, 0));
            const long end = r.position + r.length;
            if (f.outTicks > 0 && end - f.outTicks >= A && end <= B)
                out.push_back(std::make_pair(cs, 1));
        }
}

// Alt+Win+Left/Right: cycle the selected fades through Standard, S-Curve and
// the seven presets, in order (p756).
void ArrangeView::cycle_fade_shape(int dir)
{
    std::vector<std::pair<int,int>> sel;
    selected_fades(sel);
    if (sel.empty()) { flash("No fade selected"); return; }
    std::vector<int> seqs;
    for (const auto& p : sel) seqs.push_back(p.first);
    int shown = -1;
    fade_edit_op("Fade Shape", seqs, [&]{
        for (const auto& p : sel) {
            ClipFade f = fade_of(p.first);
            int& sh = p.second == 0 ? f.inShape : f.outShape;
            sh = (sh + 9 + dir) % 9;
            if (shown < 0) shown = sh;
            m_clipFade[p.first] = f; commit_fade(p.first);
        }
    });
    if (shown >= 0) flash(std::string("Fade shape: ") + shape_name(shown));
}

// Right-click Fades submenu: apply a Shape (which 0: value 0 Standard /
// 1 S-Curve) or a Slope (which 1: value 0 Equal Power / 1 Equal Gain) to
// every selected fade (p756-757).
void ArrangeView::set_selected_fade_shape(int which, int value)
{
    std::vector<std::pair<int,int>> sel;
    selected_fades(sel);
    if (sel.empty()) { flash("No fade in the Edit selection"); return; }
    std::vector<int> seqs;
    for (const auto& p : sel) seqs.push_back(p.first);
    fade_edit_op(which == 0 ? "Fade Shape" : "Fade Slope", seqs, [&]{
        for (const auto& p : sel) {
            ClipFade f = fade_of(p.first);
            if (which == 0) {
                (p.second == 0 ? f.inShape : f.outShape) = value ? 1 : 0;
            } else {
                // menu order: 0 Equal Power (engine slope 1), 1 Equal Gain (0)
                (p.second == 0 ? f.inSlope : f.outSlope) = value == 0 ? 1 : 0;
            }
            m_clipFade[p.first] = f; commit_fade(p.first);
        }
    });
    flash(which == 0 ? (value ? "Shape: S-Curve" : "Shape: Standard")
                     : (value == 0 ? "Slope: Equal Power" : "Slope: Equal Gain"));
}

//----------------------------------------------------------------------------
//  Grabber fade selection: select / move / nudge a fade or crossfade
//  independently of its clips (p757-759)
//----------------------------------------------------------------------------
// What fade sits under `tick` on clip `seq`?  0 fade-in, 1 fade-out,
// 2 crossfade (the overlap window); -1 none.
bool ArrangeView::fade_hit(int seq, long tick, int& which) const
{
    if (!m_audio.count(seq)) return false;
    int l = -1, r = -1;
    if (xfade_pair(seq, l, r)) {
        long a = 0, b = 0;
        if (xfade_window(l, r, a, b) && tick >= a && tick < b &&
            (fade_of(l).outTicks > 0 || fade_of(r).inTicks > 0)) {
            which = 2;
            return true;
        }
    }
    std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
    if (it == m_region.end()) return false;
    const AudioRegion& rg = it->second;
    const ClipFade f = fade_of(seq);
    if (f.inTicks > 0 && tick >= rg.position && tick < rg.position + f.inTicks) {
        which = 0; return true;
    }
    const long end = rg.position + rg.length;
    if (f.outTicks > 0 && tick >= end - f.outTicks && tick < end) {
        which = 1; return true;
    }
    return false;
}

// Drag/nudge the SELECTED fade: a fade-in/out slides its clip edge (revealing
// or hiding audio, the fade length constant); a crossfade slides the overlap
// point of both clips.  Constrained by the underlying source material.
void ArrangeView::move_fade_to(long tick)
{
    const int seq = m_fade_sel_seq;
    if (seq < 0) return;
    long delta = tick - m_fade_move_ref;
    if (delta == 0) return;
    if (m_fade_sel_which == 2) {
        int l = -1, r = -1;
        if (!xfade_pair(seq, l, r)) return;
        // The window slides by delta from the grab refs (p0/p1 = window at
        // grab); create_crossfade clamps it to the source material available
        // on either side, so the overlap point never outruns the audio.
        const long a = m_fade_move_p0 + delta, b = m_fade_move_p1 + delta;
        FadeSettings s;
        const ClipFade lf = fade_of(l), rf = fade_of(r);
        s.outK = lf.outK; s.outShape = lf.outShape; s.outSlope = lf.outSlope;
        s.inK = rf.inK; s.inShape = rf.inShape; s.inSlope = rf.inSlope;
        s.link = rf.link;
        create_crossfade(l, r, a, b, s, false);
        return;
    }
    AudioRegion& rg = region_for(seq);
    const ClipFade f = fade_of(seq);
    if (m_fade_sel_which == 0) {
        // slide the clip START (trim front), fade-in length constant
        long ns = m_fade_move_p0 + delta;
        const long minS = m_fade_move_p0 - (m_fade_move_p1);        // p1 = source at grab
        const long maxS = rg.position + rg.length - std::max<long>(1, f.inTicks) - 1;
        if (ns < minS) ns = minS;
        if (ns > maxS) ns = maxS;
        const long d = ns - rg.position;
        if (d == 0) return;
        rg.position = ns; rg.source += d; rg.length -= d;
        commit_region(seq);
    } else {
        // slide the clip END (trim end), fade-out length constant
        long ne = m_fade_move_p0 + delta;                            // p0 = end at grab
        const long maxE = m_fade_move_p0 + avail_after(seq)
                        - (m_fade_move_p0 - (rg.position + rg.length));
        const long minE = rg.position + std::max<long>(1, f.outTicks) + 1;
        if (ne > maxE) ne = maxE;
        if (ne < minE) ne = minE;
        if (ne == rg.position + rg.length) return;
        rg.length = ne - rg.position;
        commit_region(seq);
    }
}

void ArrangeView::nudge_fade(long delta)
{
    if (m_fade_sel_seq < 0) return;
    std::vector<int> seqs{ m_fade_sel_seq };
    int l = -1, r = -1;
    if (m_fade_sel_which == 2 && xfade_pair(m_fade_sel_seq, l, r)) {
        seqs.clear(); seqs.push_back(l); seqs.push_back(r);
    }
    // capture refs exactly as a grab would
    m_fade_move_ref = 0;
    if (m_fade_sel_which == 2 && l >= 0) {
        long a = 0, b = 0;
        xfade_window(l, r, a, b);
        m_fade_move_p0 = a; m_fade_move_p1 = b;
    } else {
        const AudioRegion& rg = region_for(m_fade_sel_seq);
        if (m_fade_sel_which == 0) { m_fade_move_p0 = rg.position; m_fade_move_p1 = rg.source; }
        else                       { m_fade_move_p0 = rg.position + rg.length; m_fade_move_p1 = 0; }
    }
    fade_edit_op("Nudge Fade", seqs, [&]{ move_fade_to(delta); });
}

// Accent highlight over the selected fade's span (the manual's "the fade
// highlights"), drawn after the clips.
void ArrangeView::draw_fade_selection(App& app)
{
    if (m_fade_sel_seq < 0 || !m_audio.count(m_fade_sel_seq)) return;
    long a = 0, b = 0;
    int seq = m_fade_sel_seq;
    if (m_fade_sel_which == 2) {
        int l = -1, r = -1;
        if (!xfade_pair(seq, l, r) || !xfade_window(l, r, a, b)) return;
        seq = r;
    } else {
        std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
        if (it == m_region.end()) return;
        const AudioRegion& rg = it->second;
        const ClipFade f = fade_of(seq);
        if (m_fade_sel_which == 0) { a = rg.position; b = a + f.inTicks; }
        else { b = rg.position + rg.length; a = b - f.outTicks; }
        if (b <= a) return;
    }
    SDL_Rect body;
    if (!clip_rect_of(seq, body)) return;
    int x0 = std::max(canvas_x(), tick_to_x(a));
    int x1 = std::min(canvas_x() + canvas_w(), tick_to_x(b));
    if (x1 <= x0) return;
    const Theme& t = theme();
    SDL_Rect hi{ x0, body.y, x1 - x0, body.h };
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
    Color c = t.accent; c.a = 70;
    fill_rect(app.ren, hi, c);
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    frame_rect(app.ren, hi, t.accent);
}

//----------------------------------------------------------------------------
//  fade & crossfade preferences (p748, p752, p757) + persistence
//----------------------------------------------------------------------------
std::string ArrangeView::fade_root_dir() const
{
    // Same org/app as the shell's other preference files, so everything lands
    // in one per-user settings directory on both Linux and Windows.
    char* pref = SDL_GetPrefPath("PatchKnob", "PatchKnob");
    std::string p = pref ? std::string(pref) : std::string();
    if (pref) SDL_free(pref);
    return p;
}

void ArrangeView::save_fade_prefs() const
{
    std::ofstream f(fade_root_dir() + "fade_prefs.txt", std::ios::trunc);
    if (!f) return;
    auto putset = [&](const char* tag, const FadeSettings& s) {
        f << tag << "=" << s.inShape << "," << s.outShape << ","
          << s.inSlope << "," << s.outSlope << "," << s.link << ","
          << s.inK << "," << s.outK << "\n";
    };
    putset("fade_in", m_def_fadein);
    putset("fade_out", m_def_fadeout);
    putset("xfade", m_def_xfade);
    f << "preroll_ms="  << m_fade_preroll_ms  << "\n"
      << "postroll_ms=" << m_fade_postroll_ms << "\n"
      << "auto_fade_ms=" << m_auto_fade_ms << "\n"
      << "preserve_fades=" << (m_preserve_fades ? 1 : 0) << "\n"
      << "smart_fade_ctrl=" << (m_smart_fade_ctrl ? 1 : 0) << "\n"
      << "save_to_session=" << (m_fade_save_session ? 1 : 0) << "\n";
    for (int i = 0; i < 5; ++i) {
        if (!m_fade_preset_set[i]) continue;
        char tag[16]; std::snprintf(tag, sizeof(tag), "preset%d", i + 1);
        putset(tag, m_fade_preset[i]);
    }
}

void ArrangeView::load_fade_prefs()
{
    std::ifstream f(fade_root_dir() + "fade_prefs.txt");
    if (!f) return;
    auto getset = [](const std::string& v, FadeSettings& s) {
        std::sscanf(v.c_str(), "%d,%d,%d,%d,%d,%f,%f", &s.inShape, &s.outShape,
                    &s.inSlope, &s.outSlope, &s.link, &s.inK, &s.outK);
        auto cs = [](int& x, int hi){ if (x < 0) x = 0; if (x > hi) x = hi; };
        cs(s.inShape, 8); cs(s.outShape, 8); cs(s.inSlope, 1); cs(s.outSlope, 1);
        cs(s.link, 2);
    };
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if      (k == "fade_in")  getset(v, m_def_fadein);
            else if (k == "fade_out") getset(v, m_def_fadeout);
            else if (k == "xfade")    getset(v, m_def_xfade);
            else if (k == "preroll_ms")  m_fade_preroll_ms  = std::stoi(v);
            else if (k == "postroll_ms") m_fade_postroll_ms = std::stoi(v);
            else if (k == "auto_fade_ms") m_auto_fade_ms = std::stoi(v);
            else if (k == "preserve_fades") m_preserve_fades = std::stoi(v) != 0;
            else if (k == "smart_fade_ctrl") m_smart_fade_ctrl = std::stoi(v) != 0;
            else if (k == "save_to_session") m_fade_save_session = std::stoi(v) != 0;
            else if (k.size() == 7 && k.compare(0, 6, "preset") == 0) {
                const int i = k[6] - '1';
                if (i >= 0 && i < 5) { getset(v, m_fade_preset[i]); m_fade_preset_set[i] = true; }
            }
        } catch (...) {}
    }
    if (m_auto_fade_ms < 0) m_auto_fade_ms = 0;
    if (m_auto_fade_ms > 10) m_auto_fade_ms = 10;
    if (m_fade_preroll_ms < 0) m_fade_preroll_ms = 0;
    if (m_fade_postroll_ms < 0) m_fade_postroll_ms = 0;
}

// The preferences pop-up (Setup > Preferences > Editing in the manual; here a
// panel reached from the Edit menu and the Fades submenu).  Every row cycles
// its value on click; changes persist immediately.
namespace {
struct FPRow { int id; const char* label; };
const FPRow kFPRows[] = {
    { -1, "FADE & CROSSFADE PREFERENCES" },
    {  0, "" },   // Fade In shape
    {  1, "" },   // Fade In slope
    {  2, "" },   // Fade Out shape
    {  3, "" },   // Fade Out slope
    {  4, "" },   // Crossfade in shape
    {  5, "" },   // Crossfade out shape
    {  6, "" },   // Crossfade link
    { -1, "" },
    {  7, "" },   // preview pre-roll
    {  8, "" },   // preview post-roll
    { -1, "" },
    {  9, "" },   // AutoFades
    { 10, "" },   // Preserve Fades when Editing
    { 11, "" },   // Smart-tool fade adjust needs Ctrl
};
const int kFPRowsN = (int)(sizeof(kFPRows) / sizeof(kFPRows[0]));
}

void ArrangeView::draw_fadepref(App& app)
{
    if (!m_fadepref_open) return;
    const Theme& t = theme();
    const int rowh = app.font.ch() + 6;
    SDL_Rect box{ rect.x + rect.w / 2 - 170, rect.y + rect.h / 2 - (kFPRowsN * rowh) / 2,
                  340, kFPRowsN * rowh + 8 };
    box = fd_clamp(box, rect);
    m_fadepref_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    int y = box.y + 4;
    for (int i = 0; i < kFPRowsN; ++i) {
        const int id = kFPRows[i].id;
        std::string s;
        switch (id) {
        case -1: s = kFPRows[i].label; break;
        case 0: s = std::string("Fade In shape: ") + shape_name(m_def_fadein.inShape); break;
        case 1: s = std::string("Fade In slope: ") + slope_name(m_def_fadein.inSlope); break;
        case 2: s = std::string("Fade Out shape: ") + shape_name(m_def_fadeout.outShape); break;
        case 3: s = std::string("Fade Out slope: ") + slope_name(m_def_fadeout.outSlope); break;
        case 4: s = std::string("Crossfade In shape: ") + shape_name(m_def_xfade.inShape); break;
        case 5: s = std::string("Crossfade Out shape: ") + shape_name(m_def_xfade.outShape); break;
        case 6: s = std::string("Crossfade link: ") + link_name(m_def_xfade.link); break;
        case 7: s = "Fade preview pre-roll: " + std::to_string(m_fade_preroll_ms) + " ms"; break;
        case 8: s = "Fade preview post-roll: " + std::to_string(m_fade_postroll_ms) + " ms"; break;
        case 9: s = "AutoFades (0 = off): " + std::to_string(m_auto_fade_ms) + " ms"; break;
        case 10: s = std::string("Preserve Fades when Editing: ") + (m_preserve_fades ? "ON" : "OFF"); break;
        case 11: s = std::string("Smart fade-adjust needs Ctrl: ") + (m_smart_fade_ctrl ? "ON" : "OFF"); break;
        }
        SDL_Rect row{ box.x + 1, y, box.w - 2, rowh };
        const bool hot = id >= 0 && fd_in(row, m_mx, m_my);
        if (hot) fill_rect(app.ren, row, t.accent);
        app.font.draw(app.ren, row.x + 8, row.y + 3, s,
                      hot ? t.bg : (id < 0 ? t.hi : t.text));
        y += rowh;
    }
}

bool ArrangeView::fadepref_click(App& app, int mx, int my)
{
    const SDL_Rect& b = m_fadepref_rect;
    if (!fd_in(b, mx, my)) { m_fadepref_open = false; app.request_redraw(); return true; }
    const int rowh = app.font.ch() + 6;
    const int i = (my - (b.y + 4)) / std::max(1, rowh);
    if (i < 0 || i >= kFPRowsN) return true;
    const int id = kFPRows[i].id;
    auto cyc9 = [](int& v){ v = (v + 1) % 9; };
    auto cyc2 = [](int& v){ v = v ? 0 : 1; };
    auto cycms = [](int& v){
        static const int steps[] = { 0, 250, 500, 1000, 2000, 3000 };
        int k = 0;
        for (int j = 0; j < 6; ++j) if (steps[j] == v) { k = (j + 1) % 6; break; }
        v = steps[k];
    };
    switch (id) {
    case 0: cyc9(m_def_fadein.inShape); break;
    case 1: cyc2(m_def_fadein.inSlope); break;
    case 2: cyc9(m_def_fadeout.outShape); break;
    case 3: cyc2(m_def_fadeout.outSlope); break;
    case 4: cyc9(m_def_xfade.inShape); break;
    case 5: cyc9(m_def_xfade.outShape); break;
    case 6: m_def_xfade.link = (m_def_xfade.link + 1) % 3; break;
    case 7: cycms(m_fade_preroll_ms); break;
    case 8: cycms(m_fade_postroll_ms); break;
    case 9:
        m_auto_fade_ms = (m_auto_fade_ms + 2) > 10 ? 0 : m_auto_fade_ms + 2;
        if (on_auto_fade_changed) on_auto_fade_changed(m_auto_fade_ms);
        break;
    case 10: m_preserve_fades = !m_preserve_fades; break;
    case 11: m_smart_fade_ctrl = !m_smart_fade_ctrl; break;
    default: return true;
    }
    save_fade_prefs();
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  the FADES dialog (Fade In / Fade Out / Crossfade; figs. pt-743-148/149)
//----------------------------------------------------------------------------
namespace {
// button indices into m_fdlg_btn (draw stores, mouse hit-tests)
enum { FB_P1 = 0, FB_P2, FB_P3, FB_P4, FB_P5,
       FB_SETTINGS = 5, FB_COMPARE = 6, FB_AUDITION = 7,
       FB_VIEW0 = 8, FB_VIEW1, FB_VIEW2, FB_VIEW3,
       FB_TRK1 = 12, FB_TRK2, FB_TRKB,
       FB_ZIN = 15, FB_ZOUT = 16,
       FB_IN_STD = 17, FB_IN_S = 18, FB_IN_PRESET = 19,
       FB_MID0 = 20, FB_MID1 = 21, FB_MID2 = 22,
       FB_OUT_STD = 23, FB_OUT_S = 24, FB_OUT_PRESET = 25,
       FB_CANCEL = 26, FB_OK = 27,
       FB_PRESET_POP = 28, FB_SETTINGS_POP = 29 };
}

// Open the dialog on a set of targets; the first target's window is plotted.
// The dialog edits LIVE (so Audition plays exactly what OK will keep); Cancel
// restores the state captured here, OK queues it as ONE undo entry.
void ArrangeView::open_fade_dialog(App& app, const std::vector<FadeTarget>& targets)
{
    if (targets.empty()) return;
    close_all_menus();
    m_fdlg_targets = targets;
    const FadeTarget& t0 = targets[0];
    m_fdlg_kind = t0.kind;
    m_fdlg_seq = t0.seq; m_fdlg_left = t0.left; m_fdlg_right = t0.right;
    m_fdlg_a = t0.a; m_fdlg_b = t0.b;
    m_fdlg_view = 0; m_fdlg_trk = 0; m_fdlg_zoom = 1.f;
    m_fdlg_drag = 0; m_fdlg_preset_menu = -1; m_fdlg_settings_menu = false;
    m_fdlg_compare = false; m_fdlg_auditioning = false;

    std::vector<int> seqs;
    for (const FadeTarget& t : m_fdlg_targets) {
        if (t.kind == 2) { seqs.push_back(t.left); seqs.push_back(t.right); }
        else             seqs.push_back(t.seq);
    }
    m_fdlg_before.clear();
    capture_fade_state(seqs, m_fdlg_before);

    // seed the working record from the defaults (or the existing fades)
    ClipFade w;
    const long win = std::max<long>(1, t0.b - t0.a);
    if (t0.kind == 2) {
        const ClipFade lf = fade_of(t0.left), rf = fade_of(t0.right);
        const bool existing = lf.outTicks > 0 || rf.inTicks > 0;
        w.inShape  = existing ? rf.inShape  : m_def_xfade.inShape;
        w.outShape = existing ? lf.outShape : m_def_xfade.outShape;
        w.inSlope  = existing ? rf.inSlope  : m_def_xfade.inSlope;
        w.outSlope = existing ? lf.outSlope : m_def_xfade.outSlope;
        w.link     = existing ? rf.link     : m_def_xfade.link;
        w.inK  = existing ? rf.inK  : m_def_xfade.inK;
        w.outK = existing ? lf.outK : m_def_xfade.outK;
        w.inTicks = w.outTicks = win;
    } else if (t0.kind == 0) {
        const ClipFade f = fade_of(t0.seq);
        const bool existing = f.inTicks > 0;
        w.inShape = existing ? f.inShape : m_def_fadein.inShape;
        w.inSlope = existing ? f.inSlope : m_def_fadein.inSlope;
        w.inK     = existing ? f.inK     : m_def_fadein.inK;
        w.inTicks = win;
    } else {
        const ClipFade f = fade_of(t0.seq);
        const bool existing = f.outTicks > 0;
        w.outShape = existing ? f.outShape : m_def_fadeout.outShape;
        w.outSlope = existing ? f.outSlope : m_def_fadeout.outSlope;
        w.outK     = existing ? f.outK     : m_def_fadeout.outK;
        w.outTicks = win;
    }
    m_fdlg_fade = w;
    m_fdlg_orig = w;
    m_fdlg_open = true;
    fade_dialog_live_apply();
    app.request_redraw();
}

ArrangeView::FadeSettings ArrangeView::fade_dialog_settings() const
{
    FadeSettings s;
    s.inShape = m_fdlg_fade.inShape; s.outShape = m_fdlg_fade.outShape;
    s.inSlope = m_fdlg_fade.inSlope; s.outSlope = m_fdlg_fade.outSlope;
    s.link = m_fdlg_fade.link; s.inK = m_fdlg_fade.inK; s.outK = m_fdlg_fade.outK;
    return s;
}

void ArrangeView::fade_dialog_load(const FadeSettings& s)
{
    m_fdlg_fade.inShape = s.inShape; m_fdlg_fade.outShape = s.outShape;
    m_fdlg_fade.inSlope = s.inSlope; m_fdlg_fade.outSlope = s.outSlope;
    m_fdlg_fade.link = s.link;
    m_fdlg_fade.inK = s.inK; m_fdlg_fade.outK = s.outK;
    // shapes/link changes re-span the halves over the whole window
    const long win = std::max<long>(1, m_fdlg_b - m_fdlg_a);
    if (m_fdlg_fade.link != 2) { m_fdlg_fade.inTicks = win; m_fdlg_fade.outTicks = win; }
    fade_dialog_live_apply();
}

// Push the working settings to every target (live; Cancel restores).
void ArrangeView::fade_dialog_live_apply()
{
    const ClipFade& w = m_fdlg_fade;
    const long win0 = std::max<long>(1, m_fdlg_b - m_fdlg_a);
    for (const FadeTarget& t : m_fdlg_targets) {
        const long win = std::max<long>(1, t.b - t.a);
        if (t.kind == 2) {
            FadeSettings s = fade_dialog_settings();
            create_crossfade(t.left, t.right, t.a, t.b, s, false);
            if (w.link == 2) {
                // Link None: the halves cover their own sub-windows
                ClipFade lf = fade_of(t.left), rf = fade_of(t.right);
                lf.outTicks = std::max<long>(1, (long)((double)w.outTicks / win0 * win));
                rf.inTicks  = std::max<long>(1, (long)((double)w.inTicks  / win0 * win));
                m_clipFade[t.left] = lf; m_clipFade[t.right] = rf;
                commit_fade(t.left); commit_fade(t.right);
            }
        } else if (t.kind == 0) {
            ClipFade f = fade_of(t.seq);
            f.inTicks = win; f.inK = w.inK;
            f.inShape = w.inShape; f.inSlope = w.inSlope;
            m_clipFade[t.seq] = f; commit_fade(t.seq);
        } else {
            ClipFade f = fade_of(t.seq);
            f.outTicks = win; f.outK = w.outK;
            f.outShape = w.outShape; f.outSlope = w.outSlope;
            m_clipFade[t.seq] = f; commit_fade(t.seq);
        }
    }
}

void ArrangeView::fade_dialog_apply(bool ok)
{
    if (!m_fdlg_open) return;
    if (m_fdlg_auditioning && on_audition_stop) on_audition_stop();
    m_fdlg_auditioning = false;
    if (m_fdlg_compare) {           // COMPARE left the ORIGINAL applied: redo the edit
        m_fdlg_compare = false;
        fade_dialog_live_apply();
    }
    if (ok) {
        std::vector<int> seqs;
        for (const FadeState& st : m_fdlg_before) seqs.push_back(st.seq);
        std::vector<FadeState> after;
        capture_fade_state(seqs, after);
        UndoOp op;
        op.name = m_fdlg_kind == 2 ? "Crossfade" : (m_fdlg_kind == 0 ? "Fade In" : "Fade Out");
        const std::vector<FadeState> before = m_fdlg_before;
        op.undo.push_back([this, before]{ restore_fade_state(before); });
        op.redo.push_back([this, after]{ restore_fade_state(after); });
        push_undo_op(std::move(op));
    } else {
        restore_fade_state(m_fdlg_before);
    }
    m_fdlg_open = false;
    m_fdlg_targets.clear();
    m_fdlg_before.clear();
}

// Stop a running audition once the playhead passes the post-roll point.
void ArrangeView::audition_poll()
{
    if (!m_fdlg_auditioning) return;
    if (playhead() >= m_fdlg_audit_end) {
        if (on_audition_stop) on_audition_stop();
        m_fdlg_auditioning = false;
    }
}

// A little curve thumbnail (the preset pictures on p745-746): rising for the
// fade-in side, falling for the fade-out side.
void ArrangeView::draw_shape_thumb(App& app, SDL_Rect r, int shape, bool rising,
                                   float k, ui::Color c)
{
    frame_rect(app.ren, r, c);
    const int n = std::max(8, r.w - 4);
    int px = -1, py = -1;
    for (int i = 0; i <= n; ++i) {
        const float u = (float)i / (float)n;
        const float lvl = fade_curve(rising ? u : 1.f - u, shape, k, 0);
        const int x = r.x + 2 + (r.w - 4) * i / n;
        const int y = r.y + r.h - 2 - (int)(lvl * (r.h - 4));
        if (px >= 0) SDL_RenderDrawLine(app.ren, px, py, x, y);
        px = x; py = y;
    }
}

// The big curve + waveform plot.
void ArrangeView::draw_fade_plot(App& app)
{
    const Theme& t = theme();
    const SDL_Rect& P = m_fdlg_plot;
    fill_rect(app.ren, P, t.keybg);
    frame_rect(app.ren, P, t.dim);
    if (P.w < 8 || P.h < 8) return;
    const ClipFade& w = m_fdlg_compare ? m_fdlg_orig : m_fdlg_fade;
    const long A = m_fdlg_a, B = m_fdlg_b;
    const long win = std::max<long>(1, B - A);
    const int x0 = P.x + 2, x1 = P.x + P.w - 2, pw = x1 - x0;
    const int y0 = P.y + 2, ph = P.h - 4;

    // effective slopes (crossfade link overrides the per-half slopes)
    int inSlope = w.inSlope, outSlope = w.outSlope;
    if (m_fdlg_kind == 2) {
        if (w.link == 0) inSlope = outSlope = 1;
        else if (w.link == 1) inSlope = outSlope = 0;
    }
    const double inFrac  = w.link == 2 && m_fdlg_kind == 2
                         ? (double)std::min(w.inTicks, win) / (double)win : 1.0;
    const double outFrac = w.link == 2 && m_fdlg_kind == 2
                         ? (double)std::min(w.outTicks, win) / (double)win : 1.0;

    // envelope of a half at window position u (0 at A, 1 at B)
    auto envIn = [&](double u) -> float {
        if (m_fdlg_kind == 1) return 1.f;
        const double s = 1.0 - inFrac;              // fade-in starts here
        if (u <= s) return 0.f;
        return fade_curve((float)((u - s) / std::max(1e-9, inFrac)),
                          w.inShape, w.inK, inSlope);
    };
    auto envOut = [&](double u) -> float {
        if (m_fdlg_kind == 0) return 1.f;
        if (u >= outFrac) return 0.f;
        return fade_curve((float)(1.0 - u / std::max(1e-9, outFrac)),
                          w.outShape, w.outK, outSlope);
    };

    // ---- waveforms ---------------------------------------------------------
    // Custom min/max sampler straight off the clip buffers (the dialog windows
    // are short; sampling is stride-capped so long windows stay cheap).
    auto wave = [&](int seq, SDL_Rect area, Color col, int envHalf) {
        std::map<int, const AudioClip*>::const_iterator ci = m_audio.find(seq);
        if (ci == m_audio.end() || !ci->second) return;
        const AudioClip* clip = ci->second;
        const long long nf = clip->safeFrames();
        if (nf < 1) return;
        std::map<int, AudioRegion>::const_iterator ri = m_region.find(seq);
        if (ri == m_region.end()) return;
        const AudioRegion& rg = ri->second;
        std::map<int, long>::const_iterator li = m_audioLen.find(seq);
        const long fullTicks = (li != m_audioLen.end() && li->second > 0)
                             ? li->second : std::max<long>(1, rg.length);
        const double fpt = (double)nf / (double)fullTicks;   // frames per tick
        const int mid = area.y + area.h / 2;
        const float sc = 0.5f * (float)(area.h - 2) * m_fdlg_zoom;
        const float* ch = clip->ch[0].data();
        for (int px = 0; px < pw; ++px) {
            const double tA = A + (double)win * px / pw;
            const double tB = A + (double)win * (px + 1) / pw;
            const double sA = ((tA - rg.position) + rg.source) * fpt;
            const double sB = ((tB - rg.position) + rg.source) * fpt;
            long long ia = (long long)sA, ib = (long long)sB;
            if (ib <= ia) ib = ia + 1;
            if (ib <= 0 || ia >= nf) continue;
            if (ia < 0) ia = 0;
            if (ib > nf) ib = nf;
            const long long stride = std::max<long long>(1, (ib - ia) / 64);
            float mn = 1e9f, mx = -1e9f;
            for (long long s = ia; s < ib; s += stride) {
                const float v = ch[(size_t)s];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (mn > mx) continue;
            float e = 1.f;
            if (envHalf == 0) e = envIn((double)px / pw);
            else if (envHalf == 1) e = envOut((double)px / pw);
            int ya = mid - (int)(mx * sc * e), yb = mid - (int)(mn * sc * e);
            if (ya < area.y) ya = area.y;
            if (yb > area.y + area.h - 1) yb = area.y + area.h - 1;
            if (yb < ya) continue;
            vline(app.ren, area.x + px, ya, yb, col);
        }
    };
    // summed crossfade: per-pixel min/max of the envelope-weighted SUM
    auto waveSum = [&](int lseq, int rseq, SDL_Rect area, Color col) {
        std::map<int, const AudioClip*>::const_iterator lc = m_audio.find(lseq);
        std::map<int, const AudioClip*>::const_iterator rc = m_audio.find(rseq);
        if (lc == m_audio.end() || rc == m_audio.end()) return;
        const AudioClip* L = lc->second; const AudioClip* R = rc->second;
        if (!L || !R) return;
        std::map<int, AudioRegion>::const_iterator lr = m_region.find(lseq);
        std::map<int, AudioRegion>::const_iterator rr = m_region.find(rseq);
        if (lr == m_region.end() || rr == m_region.end()) return;
        std::map<int, long>::const_iterator ll = m_audioLen.find(lseq);
        std::map<int, long>::const_iterator rl = m_audioLen.find(rseq);
        const long lFull = (ll != m_audioLen.end() && ll->second > 0) ? ll->second : 1;
        const long rFull = (rl != m_audioLen.end() && rl->second > 0) ? rl->second : 1;
        const double lfpt = (double)L->safeFrames() / (double)lFull;
        const double rfpt = (double)R->safeFrames() / (double)rFull;
        const int mid = area.y + area.h / 2;
        const float sc = 0.5f * (float)(area.h - 2) * m_fdlg_zoom;
        for (int px = 0; px < pw; ++px) {
            const double u = (double)px / pw;
            const double tA = A + (double)win * px / pw;
            const double tB = A + (double)win * (px + 1) / pw;
            const float eo = envOut(u), ei = envIn(u);
            const long long n = std::max<long long>(1, (long long)((tB - tA) * lfpt));
            const long long stride = std::max<long long>(1, n / 64);
            float mn = 1e9f, mx = -1e9f;
            for (long long s = 0; s < n; s += stride) {
                const double tk = tA + (tB - tA) * (double)s / (double)n;
                float v = 0.f;
                const long long li2 = (long long)(((tk - lr->second.position) + lr->second.source) * lfpt);
                const long long ri2 = (long long)(((tk - rr->second.position) + rr->second.source) * rfpt);
                if (li2 >= 0 && li2 < L->safeFrames()) v += eo * L->ch[0][(size_t)li2];
                if (ri2 >= 0 && ri2 < R->safeFrames()) v += ei * R->ch[0][(size_t)ri2];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (mn > mx) continue;
            int ya = mid - (int)(mx * sc), yb = mid - (int)(mn * sc);
            if (ya < area.y) ya = area.y;
            if (yb > area.y + area.h - 1) yb = area.y + area.h - 1;
            if (yb < ya) continue;
            vline(app.ren, area.x + px, ya, yb, col);
        }
    };

    SDL_Rect full{ x0, y0, pw, ph };
    Color outCol = t.text, inCol = t.hi;
    if (m_fdlg_view != 0) {
        if (m_fdlg_kind != 2) {
            wave(m_fdlg_seq, full, t.dim, m_fdlg_view == 3 ? m_fdlg_kind : -1);
        } else if (m_fdlg_view == 1) {
            SDL_Rect top{ x0, y0, pw, ph / 2 - 1 };
            SDL_Rect bot{ x0, y0 + ph / 2 + 1, pw, ph / 2 - 1 };
            if (m_fdlg_trk != 2) wave(m_fdlg_left, top, t.dim, -1);
            if (m_fdlg_trk != 1) wave(m_fdlg_right, bot, t.dim, -1);
            hline(app.ren, x0, x1, y0 + ph / 2, t.dim);
        } else if (m_fdlg_view == 2) {
            if (m_fdlg_trk != 2) wave(m_fdlg_left, full, t.dim, -1);
            if (m_fdlg_trk != 1) wave(m_fdlg_right, full, t.dim, -1);
        } else {
            if (m_fdlg_trk == 1)      wave(m_fdlg_left, full, t.dim, 1);
            else if (m_fdlg_trk == 2) wave(m_fdlg_right, full, t.dim, 0);
            else                      waveSum(m_fdlg_left, m_fdlg_right, full, t.dim);
        }
    }

    // ---- curves ------------------------------------------------------------
    auto curve = [&](bool rising, Color col) {
        int px = -1, py = -1;
        for (int i = 0; i <= pw; ++i) {
            const double u = (double)i / pw;
            const float lvl = rising ? envIn(u) : envOut(u);
            const int x = x0 + i;
            const int y = y0 + ph - 1 - (int)(lvl * (ph - 1));
            if (px >= 0) SDL_RenderDrawLine(app.ren, px, py, x, y);
            px = x; py = y;
        }
    };
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
    if (m_fdlg_kind != 0) { set_color(app.ren, outCol); curve(false, outCol); }
    if (m_fdlg_kind != 1) { set_color(app.ren, inCol);  curve(true, inCol); }
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);

    // Link = None: black square endpoint handles (p754-755): the fade-out's
    // END and the fade-in's START are draggable along the bottom edge.
    if (m_fdlg_kind == 2 && w.link == 2) {
        const int hs = 7;
        SDL_Rect ho{ x0 + (int)(outFrac * pw) - hs / 2, y0 + ph - hs, hs, hs };
        SDL_Rect hi2{ x0 + (int)((1.0 - inFrac) * pw) - hs / 2, y0 + ph - hs, hs, hs };
        fill_rect(app.ren, ho, t.text); frame_rect(app.ren, ho, t.bg);
        fill_rect(app.ren, hi2, t.text); frame_rect(app.ren, hi2, t.bg);
    }
    // COMPARE showing the original: badge it so the state is unmistakable
    if (m_fdlg_compare)
        app.mono.draw(app.ren, P.x + 6, P.y + 4, "ORIGINAL", t.accent);
}

void ArrangeView::draw_fade_dialog(App& app)
{
    if (!m_fdlg_open) return;
    const Theme& t = theme();
    for (int i = 0; i < 32; ++i) m_fdlg_btn[i] = SDL_Rect{ 0, 0, 0, 0 };
    SDL_Rect box{ rect.x + rect.w / 2 - 330, rect.y + rect.h / 2 - 210, 660, 420 };
    box = fd_clamp(box, rect);
    m_fdlg_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);

    // title
    const char* title = m_fdlg_kind == 2 ? "Crossfade"
                      : (m_fdlg_kind == 0 ? "Fade In" : "Fade Out");
    app.font.draw(app.ren, box.x + (box.w - app.font.text_w(title)) / 2,
                  box.y + 4, title, t.hi);
    hline(app.ren, box.x + 1, box.x + box.w - 1, box.y + 20, t.dim);

    auto btn = [&](int id, SDL_Rect r, const std::string& label, bool on) {
        m_fdlg_btn[id] = r;
        const bool hot = fd_in(r, m_mx, m_my);
        fill_rect(app.ren, r, hot ? t.accent : (on ? t.sel : t.keybg));
        frame_rect(app.ren, r, on ? t.hi : t.dim);
        const int tw = app.font.text_w(label);
        app.font.draw(app.ren, r.x + (r.w - tw) / 2, r.y + (r.h - app.font.ch()) / 2,
                      label, hot ? t.bg : (on ? t.hi : t.text));
    };
    auto radio = [&](int id, int x, int y, const std::string& label, bool on) {
        SDL_Rect r{ x, y, 12, 12 };
        m_fdlg_btn[id] = SDL_Rect{ x, y - 2, 16 + app.font.text_w(label) + 6, 16 };
        frame_rect(app.ren, r, t.dim);
        if (on) { SDL_Rect d{ x + 3, y + 3, 6, 6 }; fill_rect(app.ren, d, t.hi); }
        const bool hot = fd_in(m_fdlg_btn[id], m_mx, m_my);
        app.font.draw(app.ren, x + 16, y - 1, label, hot ? t.accent : t.text);
    };

    // ---- presets row -------------------------------------------------------
    int px = box.x + 8, py = box.y + 25;
    app.font.draw(app.ren, px, py + 2, "Presets:", t.text);
    px += app.font.text_w("Presets:") + 6;
    for (int i = 0; i < 5; ++i) {
        btn(FB_P1 + i, SDL_Rect{ px, py, 22, 18 }, std::to_string(i + 1),
            m_fade_preset_set[i]);
        px += 26;
    }
    px += 8;
    const std::string setName = m_fade_settings_name.empty()
                              ? "<factory default>" : m_fade_settings_name;
    btn(FB_SETTINGS, SDL_Rect{ px, py, 170, 18 }, setName, m_fdlg_settings_menu);
    px += 178;
    btn(FB_COMPARE, SDL_Rect{ px, py, 76, 18 }, "COMPARE", m_fdlg_compare);

    // ---- left button column ------------------------------------------------
    int bx = box.x + 8, by = box.y + 52;
    btn(FB_AUDITION, SDL_Rect{ bx, by, 28, 20 }, m_fdlg_auditioning ? "|>" : "<)",
        m_fdlg_auditioning);
    by += 26;
    static const char* vlbl[4] = { "X", "=", "%", "S" };   // curves/sep/super/sum
    for (int i = 0; i < 4; ++i) {
        btn(FB_VIEW0 + i, SDL_Rect{ bx, by, 28, 18 }, vlbl[i], m_fdlg_view == i);
        by += 21;
    }
    if (m_fdlg_kind == 2) {
        by += 4;
        static const char* tl[3] = { "1", "2", "Bo" };
        for (int i = 0; i < 3; ++i) {
            btn(FB_TRK1 + i, SDL_Rect{ bx, by, 28, 18 }, tl[i],
                m_fdlg_trk == (i == 2 ? 0 : i + 1));
            by += 21;
        }
    }
    by += 4;
    btn(FB_ZIN,  SDL_Rect{ bx, by, 28, 16 }, "/\\", false); by += 19;
    btn(FB_ZOUT, SDL_Rect{ bx, by, 28, 16 }, "\\/", false);

    // ---- plot --------------------------------------------------------------
    m_fdlg_plot = SDL_Rect{ box.x + 44, box.y + 50, box.w - 52, 180 };
    draw_fade_plot(app);

    // ---- shape / slope / link groups ---------------------------------------
    const int gy = box.y + 238, gh = 136;
    const int gw = (box.w - 32) / 3;
    auto group = [&](int gx, const char* head) -> SDL_Rect {
        SDL_Rect g{ gx, gy, gw, gh };
        fill_rect(app.ren, SDL_Rect{ g.x, g.y, g.w, 16 }, t.keybg);
        frame_rect(app.ren, g, t.dim);
        app.font.draw(app.ren, g.x + 6, g.y + 2, head, t.hi);
        return g;
    };
    const ClipFade& w = m_fdlg_fade;
    if (m_fdlg_kind == 2) {
        SDL_Rect g1 = group(box.x + 8, "In Shape");
        radio(FB_IN_STD, g1.x + 8, g1.y + 24, "Standard", w.inShape == 0);
        radio(FB_IN_S,   g1.x + 8, g1.y + 42, "S-Curve",  w.inShape == 1);
        {   // preset radio + thumbnail
            radio(FB_IN_PRESET, g1.x + 8, g1.y + 62, "", w.inShape >= 2);
            SDL_Rect th{ g1.x + 28, g1.y + 58, 46, 38 };
            m_fdlg_btn[FB_IN_PRESET] = SDL_Rect{ g1.x + 8, g1.y + 58, 70, 40 };
            draw_shape_thumb(app, th, w.inShape >= 2 ? w.inShape : 5, true, 0.f,
                             w.inShape >= 2 ? t.hi : t.dim);
        }
        SDL_Rect g2 = group(box.x + 12 + gw, "Link Out / In");
        radio(FB_MID0, g2.x + 8, g2.y + 24, "Equal Power", w.link == 0);
        radio(FB_MID1, g2.x + 8, g2.y + 42, "Equal Gain",  w.link == 1);
        radio(FB_MID2, g2.x + 8, g2.y + 60, "None",        w.link == 2);
        SDL_Rect g3 = group(box.x + 16 + 2 * gw, "Out Shape");
        radio(FB_OUT_STD, g3.x + 8, g3.y + 24, "Standard", w.outShape == 0);
        radio(FB_OUT_S,   g3.x + 8, g3.y + 42, "S-Curve",  w.outShape == 1);
        {
            radio(FB_OUT_PRESET, g3.x + 8, g3.y + 62, "", w.outShape >= 2);
            SDL_Rect th{ g3.x + 28, g3.y + 58, 46, 38 };
            m_fdlg_btn[FB_OUT_PRESET] = SDL_Rect{ g3.x + 8, g3.y + 58, 70, 40 };
            draw_shape_thumb(app, th, w.outShape >= 2 ? w.outShape : 5, false, 0.f,
                             w.outShape >= 2 ? t.hi : t.dim);
        }
    } else {
        const bool isIn = m_fdlg_kind == 0;
        SDL_Rect g1 = group(box.x + 8 + gw / 2, "Slope");
        const int slope = isIn ? w.inSlope : w.outSlope;
        radio(FB_MID0, g1.x + 8, g1.y + 24, "Equal Power", slope == 1);
        radio(FB_MID1, g1.x + 8, g1.y + 42, "Equal Gain",  slope == 0);
        SDL_Rect g3 = group(box.x + 12 + gw + gw / 2, isIn ? "In Shape" : "Out Shape");
        const int shape = isIn ? w.inShape : w.outShape;
        radio(isIn ? FB_IN_STD : FB_OUT_STD, g3.x + 8, g3.y + 24, "Standard", shape == 0);
        radio(isIn ? FB_IN_S : FB_OUT_S,     g3.x + 8, g3.y + 42, "S-Curve",  shape == 1);
        {
            const int id = isIn ? FB_IN_PRESET : FB_OUT_PRESET;
            radio(id, g3.x + 8, g3.y + 62, "", shape >= 2);
            SDL_Rect th{ g3.x + 28, g3.y + 58, 46, 38 };
            m_fdlg_btn[id] = SDL_Rect{ g3.x + 8, g3.y + 58, 70, 40 };
            draw_shape_thumb(app, th, shape >= 2 ? shape : 5, isIn, 0.f,
                             shape >= 2 ? t.hi : t.dim);
        }
    }

    // ---- footer ------------------------------------------------------------
    btn(FB_CANCEL, SDL_Rect{ box.x + box.w - 170, box.y + box.h - 26, 76, 20 },
        "Cancel", false);
    btn(FB_OK, SDL_Rect{ box.x + box.w - 88, box.y + box.h - 26, 80, 20 },
        "OK", false);

    // ---- preset-thumbnail pop-up (the 7 parabolic curves) ------------------
    if (m_fdlg_preset_menu >= 0) {
        const int which = m_fdlg_preset_menu;     // 0 = in side, 1 = out side
        SDL_Rect anchor = m_fdlg_btn[which == 0 ? FB_IN_PRESET : FB_OUT_PRESET];
        SDL_Rect pop{ anchor.x, anchor.y - 46, 7 * 40 + 8, 44 };
        pop = fd_clamp(pop, rect);
        m_fdlg_btn[FB_PRESET_POP] = pop;
        fill_rect(app.ren, pop, t.panel);
        frame_rect(app.ren, pop, t.accent);
        const int cur = which == 0 ? w.inShape : w.outShape;
        for (int i = 0; i < 7; ++i) {
            SDL_Rect th{ pop.x + 4 + i * 40, pop.y + 4, 36, 36 };
            if (cur == 2 + i) fill_rect(app.ren, th, t.sel);
            draw_shape_thumb(app, th, 2 + i, which == 0, 0.f,
                             fd_in(th, m_mx, m_my) ? t.accent : t.text);
        }
    }

    // ---- settings menu pop-up ---------------------------------------------
    if (m_fdlg_settings_menu) {
        static const char* rows[6] = { "<factory default>", "Save Settings",
                                       "Save Settings As...", "Import Settings...",
                                       "Delete Current Settings File",
                                       "Save Fade Settings To: " };
        const int rowh = app.font.ch() + 5;
        SDL_Rect anchor = m_fdlg_btn[FB_SETTINGS];
        SDL_Rect pop{ anchor.x, anchor.y + anchor.h, 220, 6 * rowh + 4 };
        pop = fd_clamp(pop, rect);
        m_fdlg_btn[FB_SETTINGS_POP] = pop;
        fill_rect(app.ren, pop, t.panel);
        frame_rect(app.ren, pop, t.accent);
        for (int i = 0; i < 6; ++i) {
            SDL_Rect row{ pop.x + 1, pop.y + 2 + i * rowh, pop.w - 2, rowh };
            const bool hot = fd_in(row, m_mx, m_my);
            if (hot) fill_rect(app.ren, row, t.accent);
            std::string s = rows[i];
            if (i == 5) s += m_fade_save_session ? "Session" : "Root";
            app.font.draw(app.ren, row.x + 6, row.y + 2, s, hot ? t.bg : t.text);
        }
    }
}

bool ArrangeView::fade_dialog_key(App& app, SDL_Keycode k)
{
    if (!m_fdlg_open) return false;
    const bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    if (k == SDLK_ESCAPE) { fade_dialog_apply(false); app.request_redraw(); return true; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        fade_dialog_apply(true); app.request_redraw(); return true;
    }
    if (k >= SDLK_1 && k <= SDLK_5) {
        const int i = (int)(k - SDLK_1);
        if (ctrl) {          // store (Ctrl-click equivalent, p748)
            m_fade_preset[i] = fade_dialog_settings();
            m_fade_preset_set[i] = true;
            save_fade_prefs();
            flash("Stored fade preset " + std::to_string(i + 1));
        } else if (m_fade_preset_set[i]) {
            fade_dialog_load(m_fade_preset[i]);
            flash("Recalled fade preset " + std::to_string(i + 1));
        } else flash("Preset " + std::to_string(i + 1) + " is empty");
        app.request_redraw();
        return true;
    }
    return true;   // the dialog is modal: swallow everything else
}

// Fade Settings menu actions: 0 factory, 1 save, 2 save-as, 3 import,
// 4 delete, 5 toggle destination folder.
void ArrangeView::fade_settings_file(int action, App& app)
{
    const std::string dir = (m_fade_save_session && !m_session_dir.empty())
                          ? m_session_dir + "/Fades Presets/" : fade_root_dir();
    auto path_of = [dir](const std::string& name) { return dir + name + ".fdpreset"; };
    auto write_file = [this, path_of](const std::string& name) -> bool {
        if (m_fade_save_session && !m_session_dir.empty()) {
            // best-effort folder create, portable via std::filesystem-free path:
            // the shell created the session dir; the sub-dir may not exist yet.
#ifdef _WIN32
            std::string cmd = "mkdir \"" + m_session_dir + "\\Fades Presets\" 2> NUL";
#else
            std::string cmd = "mkdir -p '" + m_session_dir + "/Fades Presets'";
#endif
            (void)std::system(cmd.c_str());
        }
        std::ofstream f(path_of(name), std::ios::trunc);
        if (!f) return false;
        const FadeSettings s = fade_dialog_settings();
        f << "inShape=" << s.inShape << "\noutShape=" << s.outShape
          << "\ninSlope=" << s.inSlope << "\noutSlope=" << s.outSlope
          << "\nlink=" << s.link << "\ninK=" << s.inK << "\noutK=" << s.outK << "\n";
        return true;
    };
    switch (action) {
    case 0:                                    // <factory default>
        fade_dialog_load(FadeSettings{});
        m_fade_settings_name.clear();
        flash("Factory default fade settings");
        break;
    case 1:                                    // Save Settings
        if (m_fade_settings_name.empty()) { fade_settings_file(2, app); return; }
        flash(write_file(m_fade_settings_name) ? "Fade settings saved"
                                               : "Could not write settings file");
        break;
    case 2:                                    // Save Settings As...
        m_fdlg_buf = m_fade_settings_name.empty() ? "fade" : m_fade_settings_name;
        app.begin_text(&m_fdlg_buf, nullptr, [this, write_file](bool okd) {
            if (!okd || m_fdlg_buf.empty()) return;
            m_fade_settings_name = m_fdlg_buf;
            flash(write_file(m_fade_settings_name) ? "Saved " + m_fade_settings_name
                                                   : "Could not write settings file");
        });
        flash("Type a settings name, Enter to save");
        break;
    case 3:                                    // Import Settings...
        m_fdlg_buf.clear();
        app.begin_text(&m_fdlg_buf, nullptr, [this, path_of](bool okd) {
            if (!okd || m_fdlg_buf.empty()) return;
            // a bare name reads from the current folder; a path is used as-is
            std::string p = m_fdlg_buf.find('/') == std::string::npos &&
                            m_fdlg_buf.find('\\') == std::string::npos
                          ? path_of(m_fdlg_buf) : m_fdlg_buf;
            std::ifstream f(p);
            if (!f) { flash("Cannot open " + p); return; }
            FadeSettings s = fade_dialog_settings();
            std::string line;
            while (std::getline(f, line)) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = line.substr(0, eq), v = line.substr(eq + 1);
                try {
                    if      (key == "inShape")  s.inShape  = std::stoi(v);
                    else if (key == "outShape") s.outShape = std::stoi(v);
                    else if (key == "inSlope")  s.inSlope  = std::stoi(v);
                    else if (key == "outSlope") s.outSlope = std::stoi(v);
                    else if (key == "link")     s.link     = std::stoi(v);
                    else if (key == "inK")      s.inK      = std::stof(v);
                    else if (key == "outK")     s.outK     = std::stof(v);
                } catch (...) {}
            }
            fade_dialog_load(s);
            m_fade_settings_name = m_fdlg_buf;
            flash("Imported fade settings");
        });
        flash("Type a settings name or path, Enter to import");
        break;
    case 4:                                    // Delete Current Settings File
        if (m_fade_settings_name.empty()) { flash("No settings file selected"); break; }
        std::remove(path_of(m_fade_settings_name).c_str());
        flash("Deleted " + m_fade_settings_name);
        m_fade_settings_name.clear();
        break;
    case 5:                                    // Save Fade Settings To
        m_fade_save_session = !m_fade_save_session;
        save_fade_prefs();
        break;
    }
}

bool ArrangeView::fade_dialog_mouse(App& app, const ui::MouseEv& e)
{
    if (!m_fdlg_open) return false;
    static bool  s_down = false;
    static int   s_x0 = 0;
    static float s_k0in = 0.f, s_k0out = 0.f;
    static long  s_in0 = 0, s_out0 = 0;
    static bool  s_moved = false;
    const bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    const bool alt  = (SDL_GetModState() & KMOD_ALT) != 0;
    ClipFade& w = m_fdlg_fade;
    const long win = std::max<long>(1, m_fdlg_b - m_fdlg_a);

    // ---- release -----------------------------------------------------------
    if (!e.pressed) {
        if (s_down && m_fdlg_drag != 0 && !s_moved && alt &&
            (m_fdlg_drag >= 1 && m_fdlg_drag <= 3)) {
            // Alt-click: reset the curve(s) to the default shape (p747)
            if (m_fdlg_drag != 2) w.inK = 0.f;
            if (m_fdlg_drag != 1) w.outK = 0.f;
            fade_dialog_live_apply();
        }
        s_down = false; m_fdlg_drag = 0;
        app.request_redraw();
        return true;
    }

    // ---- drag --------------------------------------------------------------
    if (s_down) {
        const int dx = e.x - s_x0;
        if (dx != 0) s_moved = true;
        if (m_fdlg_drag >= 1 && m_fdlg_drag <= 3) {
            // drag left/right to reshape (p754); Standard + S-Curve only
            const float dk = (float)dx * 0.008f;
            auto clampk = [](float v){ return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
            if (m_fdlg_drag != 2 && w.inShape <= 1)  w.inK  = clampk(s_k0in + dk);
            if (m_fdlg_drag != 1 && w.outShape <= 1) w.outK = clampk(s_k0out - dk);
            if (m_fdlg_kind == 2 && w.link != 2 && m_fdlg_drag == 3)
                w.outK = -w.inK;                   // linked halves mirror
            fade_dialog_live_apply();
        } else if (m_fdlg_drag == 4 || m_fdlg_drag == 5) {
            // Link None endpoint squares: drag the fade-out end / fade-in start
            const long dt = (long)((double)dx / std::max(1, m_fdlg_plot.w - 4) * win);
            if (m_fdlg_drag == 4)
                w.outTicks = std::max<long>(1, std::min(win, s_out0 + dt));
            else
                w.inTicks  = std::max<long>(1, std::min(win, s_in0 - dt));
            fade_dialog_live_apply();
        }
        app.request_redraw();
        return true;
    }

    // ---- press -------------------------------------------------------------
    // settings menu pop-up rows
    if (m_fdlg_settings_menu) {
        const SDL_Rect& pop = m_fdlg_btn[FB_SETTINGS_POP];
        m_fdlg_settings_menu = false;
        if (fd_in(pop, e.x, e.y)) {
            const int rowh = app.font.ch() + 5;
            const int i = (e.y - (pop.y + 2)) / std::max(1, rowh);
            if (i >= 0 && i < 6) fade_settings_file(i, app);
        }
        app.request_redraw();
        return true;
    }
    // preset-thumbnail pop-up
    if (m_fdlg_preset_menu >= 0) {
        const SDL_Rect& pop = m_fdlg_btn[FB_PRESET_POP];
        const int which = m_fdlg_preset_menu;
        m_fdlg_preset_menu = -1;
        if (fd_in(pop, e.x, e.y)) {
            const int i = (e.x - (pop.x + 4)) / 40;
            if (i >= 0 && i < 7) {
                if (which == 0) w.inShape = 2 + i; else w.outShape = 2 + i;
                fade_dialog_live_apply();
            }
        }
        app.request_redraw();
        return true;
    }

    if (!fd_in(m_fdlg_rect, e.x, e.y)) return true;   // modal: swallow

    auto hit = [&](int id) { return fd_in(m_fdlg_btn[id], e.x, e.y); };

    for (int i = 0; i < 5; ++i)
        if (hit(FB_P1 + i)) {
            if (ctrl) {                       // Ctrl-click stores (p748)
                m_fade_preset[i] = fade_dialog_settings();
                m_fade_preset_set[i] = true;
                save_fade_prefs();
                flash("Stored fade preset " + std::to_string(i + 1));
            } else if (m_fade_preset_set[i]) {
                fade_dialog_load(m_fade_preset[i]);
            } else flash("Preset " + std::to_string(i + 1) + " is empty (Ctrl-click stores)");
            app.request_redraw();
            return true;
        }
    if (hit(FB_SETTINGS)) { m_fdlg_settings_menu = true; app.request_redraw(); return true; }
    if (hit(FB_COMPARE)) {
        // COMPARE toggles the ORIGINAL settings onto the targets, so both the
        // plot and the Audition button play what you are comparing against.
        m_fdlg_compare = !m_fdlg_compare;
        if (m_fdlg_compare) restore_fade_state(m_fdlg_before);
        else                fade_dialog_live_apply();
        app.request_redraw();
        return true;
    }
    if (hit(FB_AUDITION)) {
        if (m_fdlg_auditioning) {
            if (on_audition_stop) on_audition_stop();
            m_fdlg_auditioning = false;
        } else if (on_audition_start) {
            const long pre  = ms_ticks(m_fade_preroll_ms);
            const long post = ms_ticks(m_fade_postroll_ms);
            const long a = std::max<long>(0, m_fdlg_a - pre);
            m_fdlg_audit_end = m_fdlg_b + post;
            on_audition_start(a, m_fdlg_audit_end);
            m_fdlg_auditioning = true;
        } else flash("Audition needs the shell transport (unbound)");
        app.request_redraw();
        return true;
    }
    for (int i = 0; i < 4; ++i)
        if (hit(FB_VIEW0 + i)) { m_fdlg_view = i; app.request_redraw(); return true; }
    if (m_fdlg_kind == 2)
        for (int i = 0; i < 3; ++i)
            if (hit(FB_TRK1 + i)) {
                m_fdlg_trk = i == 2 ? 0 : i + 1;
                app.request_redraw();
                return true;
            }
    if (hit(FB_ZIN))  { m_fdlg_zoom = ctrl ? 1.f : std::min(16.f, m_fdlg_zoom * 1.5f);
                        app.request_redraw(); return true; }
    if (hit(FB_ZOUT)) { m_fdlg_zoom = ctrl ? 1.f : std::max(0.1f, m_fdlg_zoom / 1.5f);
                        app.request_redraw(); return true; }

    auto shapes_changed = [&]{ fade_dialog_live_apply(); app.request_redraw(); };
    if (hit(FB_IN_STD))  { w.inShape = 0; shapes_changed(); return true; }
    if (hit(FB_IN_S))    { w.inShape = 1; shapes_changed(); return true; }
    if (hit(FB_IN_PRESET))  { m_fdlg_preset_menu = 0; app.request_redraw(); return true; }
    if (hit(FB_OUT_STD)) { w.outShape = 0; shapes_changed(); return true; }
    if (hit(FB_OUT_S))   { w.outShape = 1; shapes_changed(); return true; }
    if (hit(FB_OUT_PRESET)) { m_fdlg_preset_menu = 1; app.request_redraw(); return true; }
    if (m_fdlg_kind == 2) {
        if (hit(FB_MID0)) { w.link = 0; shapes_changed(); return true; }
        if (hit(FB_MID1)) { w.link = 1; shapes_changed(); return true; }
        if (hit(FB_MID2)) { w.link = 2; shapes_changed(); return true; }
    } else {
        const bool isIn = m_fdlg_kind == 0;
        if (hit(FB_MID0)) { (isIn ? w.inSlope : w.outSlope) = 1; shapes_changed(); return true; }
        if (hit(FB_MID1)) { (isIn ? w.inSlope : w.outSlope) = 0; shapes_changed(); return true; }
    }
    if (hit(FB_CANCEL)) { fade_dialog_apply(false); app.request_redraw(); return true; }
    if (hit(FB_OK))     { fade_dialog_apply(true);  app.request_redraw(); return true; }

    if (fd_in(m_fdlg_plot, e.x, e.y)) {
        s_down = true; s_moved = false;
        s_x0 = e.x; s_k0in = w.inK; s_k0out = w.outK;
        s_in0 = w.inTicks; s_out0 = w.outTicks;
        // Link None endpoint handles first (black squares, bottom edge)
        if (m_fdlg_kind == 2 && w.link == 2 &&
            e.y > m_fdlg_plot.y + m_fdlg_plot.h - 14) {
            const int pw = std::max(1, m_fdlg_plot.w - 4);
            const int xo = m_fdlg_plot.x + 2 +
                (int)((double)std::min(w.outTicks, win) / win * pw);
            const int xi = m_fdlg_plot.x + 2 +
                (int)((1.0 - (double)std::min(w.inTicks, win) / win) * pw);
            if (std::abs(e.x - xo) <= 6) { m_fdlg_drag = 4; return true; }
            if (std::abs(e.x - xi) <= 6) { m_fdlg_drag = 5; return true; }
        }
        // curve drag: Alt = fade-in half only, Ctrl = fade-out only (p754)
        if      (m_fdlg_kind == 0) m_fdlg_drag = 1;
        else if (m_fdlg_kind == 1) m_fdlg_drag = 2;
        else if (alt)              m_fdlg_drag = 1;
        else if (ctrl)             m_fdlg_drag = 2;
        else                       m_fdlg_drag = 3;
        if ((m_fdlg_drag == 1 && w.inShape > 1) ||
            (m_fdlg_drag == 2 && w.outShape > 1) ||
            (m_fdlg_drag == 3 && w.inShape > 1 && w.outShape > 1))
            flash("Preset curves are fixed; pick Standard or S-Curve to reshape");
        return true;
    }
    return true;
}

//----------------------------------------------------------------------------
//  the BATCH FADES dialog (fig. pt-753-186)
//----------------------------------------------------------------------------
namespace {
// button indices into m_bdlg_btn
enum { BB_IN_CREATE = 0, BB_IN_SHAPE, BB_IN_LEN, BB_IN_MS,
       BB_X_CREATE, BB_X_SHAPE, BB_X_LEN, BB_X_MS,
       BB_OUT_CREATE, BB_OUT_SHAPE, BB_OUT_LEN, BB_OUT_MS,
       BB_PLACE0, BB_PLACE1, BB_PLACE2,
       BB_INSET_SHAPE, BB_INSET_SLOPE,
       BB_XSET_IN, BB_XSET_LINK, BB_XSET_OUT,
       BB_OUTSET_SHAPE, BB_OUTSET_SLOPE,
       BB_CANCEL, BB_OK };
}

void ArrangeView::open_batch_dialog(App& app)
{
    if (!have_range()) { flash("Select across whole clips first"); return; }
    close_all_menus();
    m_b_in = m_def_fadein; m_b_x = m_def_xfade; m_b_out = m_def_fadeout;
    m_b_edit = -1;
    m_bdlg_open = true;
    app.request_redraw();
}

void ArrangeView::draw_batch_dialog(App& app)
{
    if (!m_bdlg_open) return;
    const Theme& t = theme();
    for (int i = 0; i < 24; ++i) m_bdlg_btn[i] = SDL_Rect{ 0, 0, 0, 0 };
    SDL_Rect box{ rect.x + rect.w / 2 - 330, rect.y + rect.h / 2 - 170, 660, 340 };
    box = fd_clamp(box, rect);
    m_bdlg_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    const char* title = "Batch Fades";
    app.font.draw(app.ren, box.x + (box.w - app.font.text_w(title)) / 2, box.y + 4,
                  title, t.hi);
    hline(app.ren, box.x + 1, box.x + box.w - 1, box.y + 20, t.dim);

    const int cw = (box.w - 32) / 3;
    auto check = [&](int id, int x, int y, const std::string& label, bool on) {
        SDL_Rect r{ x, y, 12, 12 };
        m_bdlg_btn[id] = SDL_Rect{ x, y - 2, 16 + app.font.text_w(label) + 4, 16 };
        frame_rect(app.ren, r, t.dim);
        if (on) {
            SDL_RenderDrawLine(app.ren, x + 2, y + 6, x + 5, y + 9);
            SDL_RenderDrawLine(app.ren, x + 5, y + 9, x + 10, y + 2);
        }
        const bool hot = fd_in(m_bdlg_btn[id], m_mx, m_my);
        app.font.draw(app.ren, x + 16, y - 1, label, hot ? t.accent : t.text);
    };
    auto cycler = [&](int id, int x, int y, int w2, const std::string& s) {
        SDL_Rect r{ x, y, w2, app.font.ch() + 4 };
        m_bdlg_btn[id] = r;
        const bool hot = fd_in(r, m_mx, m_my);
        if (hot) fill_rect(app.ren, r, t.accent);
        app.font.draw(app.ren, r.x + 4, r.y + 2, s, hot ? t.bg : t.text);
    };

    // three columns: Fade Ins | Crossfades | Fade Outs.  Top: a preview curve
    // + the shape settings; bottom: the operation checkboxes + length.
    static const char* heads[3] = { "Fade In", "Crossfades", "Fade Out" };
    for (int c = 0; c < 3; ++c) {
        const int gx = box.x + 8 + c * (cw + 8), gy = box.y + 26;
        SDL_Rect g{ gx, gy, cw, box.h - 60 };
        frame_rect(app.ren, g, t.dim);
        fill_rect(app.ren, SDL_Rect{ g.x, g.y, g.w, 15 }, t.keybg);
        app.font.draw(app.ren, g.x + 6, g.y + 1, heads[c], t.hi);
        // preview curves
        SDL_Rect pv{ g.x + 6, g.y + 20, g.w - 12, 54 };
        fill_rect(app.ren, pv, t.keybg);
        set_color(app.ren, t.hi);
        if (c == 0) draw_shape_thumb(app, pv, m_b_in.inShape, true, m_b_in.inK, t.hi);
        else if (c == 2) draw_shape_thumb(app, pv, m_b_out.outShape, false, m_b_out.outK, t.hi);
        else {
            draw_shape_thumb(app, pv, m_b_x.outShape, false, m_b_x.outK, t.text);
            draw_shape_thumb(app, pv, m_b_x.inShape, true, m_b_x.inK, t.hi);
        }
        int y = pv.y + pv.h + 8;
        if (c == 0) {
            cycler(BB_INSET_SHAPE, g.x + 6, y, g.w - 12,
                   std::string("Shape: ") + shape_name(m_b_in.inShape)); y += 18;
            cycler(BB_INSET_SLOPE, g.x + 6, y, g.w - 12,
                   std::string("Slope: ") + slope_name(m_b_in.inSlope)); y += 22;
            check(BB_IN_CREATE, g.x + 6, y, "Create new fade ins", m_b_create[0]); y += 18;
            check(BB_IN_SHAPE, g.x + 6, y, "Adjust existing shape+slope", m_b_shape[0]); y += 18;
            check(BB_IN_LEN, g.x + 6, y, "Adjust existing length", m_b_length[0]); y += 20;
            cycler(BB_IN_MS, g.x + 6, y, g.w - 12,
                   "Length: " + std::to_string(m_b_ms[0]) + " ms" +
                   (m_b_edit == 0 ? " _" : ""));
        } else if (c == 1) {
            cycler(BB_XSET_IN, g.x + 6, y, g.w - 12,
                   std::string("In: ") + shape_name(m_b_x.inShape)); y += 18;
            cycler(BB_XSET_LINK, g.x + 6, y, g.w - 12,
                   std::string("Link: ") + link_name(m_b_x.link)); y += 18;
            cycler(BB_XSET_OUT, g.x + 6, y, g.w - 12,
                   std::string("Out: ") + shape_name(m_b_x.outShape)); y += 4;
            y += 18;
            check(BB_X_CREATE, g.x + 6, y, "Create new crossfades", m_b_create[1]); y += 18;
            check(BB_X_SHAPE, g.x + 6, y, "Adjust existing shape+slope", m_b_shape[1]); y += 18;
            check(BB_X_LEN, g.x + 6, y, "Adjust existing length", m_b_length[1]); y += 20;
            cycler(BB_X_MS, g.x + 6, y, g.w - 12,
                   "Length: " + std::to_string(m_b_ms[1]) + " ms" +
                   (m_b_edit == 1 ? " _" : ""));
            y += 20;
            // placement radios (Pre-splice / Centered / Post-splice)
            static const char* pl[3] = { "Pre-splice", "Centered", "Post-splice" };
            for (int i = 0; i < 3; ++i) {
                SDL_Rect r{ g.x + 6, y, 12, 12 };
                m_bdlg_btn[BB_PLACE0 + i] =
                    SDL_Rect{ g.x + 6, y - 2, 16 + app.font.text_w(pl[i]) + 4, 16 };
                frame_rect(app.ren, r, t.dim);
                if (m_b_place == i) {
                    SDL_Rect d{ r.x + 3, r.y + 3, 6, 6 };
                    fill_rect(app.ren, d, t.hi);
                }
                app.font.draw(app.ren, g.x + 22, y - 1, pl[i],
                              fd_in(m_bdlg_btn[BB_PLACE0 + i], m_mx, m_my)
                              ? t.accent : t.text);
                y += 16;
            }
        } else {
            cycler(BB_OUTSET_SHAPE, g.x + 6, y, g.w - 12,
                   std::string("Shape: ") + shape_name(m_b_out.outShape)); y += 18;
            cycler(BB_OUTSET_SLOPE, g.x + 6, y, g.w - 12,
                   std::string("Slope: ") + slope_name(m_b_out.outSlope)); y += 22;
            check(BB_OUT_CREATE, g.x + 6, y, "Create new fade outs", m_b_create[2]); y += 18;
            check(BB_OUT_SHAPE, g.x + 6, y, "Adjust existing shape+slope", m_b_shape[2]); y += 18;
            check(BB_OUT_LEN, g.x + 6, y, "Adjust existing length", m_b_length[2]); y += 20;
            cycler(BB_OUT_MS, g.x + 6, y, g.w - 12,
                   "Length: " + std::to_string(m_b_ms[2]) + " ms" +
                   (m_b_edit == 2 ? " _" : ""));
        }
    }

    auto btn = [&](int id, SDL_Rect r, const char* label) {
        m_bdlg_btn[id] = r;
        const bool hot = fd_in(r, m_mx, m_my);
        fill_rect(app.ren, r, hot ? t.accent : t.keybg);
        frame_rect(app.ren, r, t.dim);
        app.font.draw(app.ren, r.x + (r.w - app.font.text_w(label)) / 2,
                      r.y + (r.h - app.font.ch()) / 2, label, hot ? t.bg : t.text);
    };
    btn(BB_CANCEL, SDL_Rect{ box.x + box.w - 170, box.y + box.h - 26, 76, 20 }, "Cancel");
    btn(BB_OK,     SDL_Rect{ box.x + box.w - 88,  box.y + box.h - 26, 80, 20 }, "OK");
}

bool ArrangeView::batch_dialog_mouse(App& app, const ui::MouseEv& e)
{
    if (!m_bdlg_open) return false;
    if (!e.pressed) return true;
    auto hit = [&](int id) { return fd_in(m_bdlg_btn[id], e.x, e.y); };
    auto cyc9 = [](int& v){ v = (v + 1) % 9; };
    auto cyc2 = [](int& v){ v = v ? 0 : 1; };
    auto edit_ms = [&](int i) {
        m_b_edit = i;
        m_fdlg_buf = std::to_string(m_b_ms[i]);
        app.begin_text(&m_fdlg_buf, nullptr, [this, i](bool okd) {
            if (okd) {
                try {
                    int v = std::stoi(m_fdlg_buf);
                    if (v < 1) v = 1;
                    if (v > 60000) v = 60000;
                    m_b_ms[i] = v;
                } catch (...) {}
            }
            m_b_edit = -1;
        });
    };
    if      (hit(BB_IN_CREATE)) m_b_create[0] = !m_b_create[0];
    else if (hit(BB_IN_SHAPE))  m_b_shape[0]  = !m_b_shape[0];
    else if (hit(BB_IN_LEN))    m_b_length[0] = !m_b_length[0];
    else if (hit(BB_IN_MS))     edit_ms(0);
    else if (hit(BB_X_CREATE))  m_b_create[1] = !m_b_create[1];
    else if (hit(BB_X_SHAPE))   m_b_shape[1]  = !m_b_shape[1];
    else if (hit(BB_X_LEN))     m_b_length[1] = !m_b_length[1];
    else if (hit(BB_X_MS))      edit_ms(1);
    else if (hit(BB_OUT_CREATE)) m_b_create[2] = !m_b_create[2];
    else if (hit(BB_OUT_SHAPE))  m_b_shape[2]  = !m_b_shape[2];
    else if (hit(BB_OUT_LEN))    m_b_length[2] = !m_b_length[2];
    else if (hit(BB_OUT_MS))     edit_ms(2);
    else if (hit(BB_PLACE0)) m_b_place = 0;
    else if (hit(BB_PLACE1)) m_b_place = 1;
    else if (hit(BB_PLACE2)) m_b_place = 2;
    else if (hit(BB_INSET_SHAPE)) cyc9(m_b_in.inShape);
    else if (hit(BB_INSET_SLOPE)) cyc2(m_b_in.inSlope);
    else if (hit(BB_XSET_IN))   cyc9(m_b_x.inShape);
    else if (hit(BB_XSET_LINK)) m_b_x.link = (m_b_x.link + 1) % 3;
    else if (hit(BB_XSET_OUT))  cyc9(m_b_x.outShape);
    else if (hit(BB_OUTSET_SHAPE)) cyc9(m_b_out.outShape);
    else if (hit(BB_OUTSET_SLOPE)) cyc2(m_b_out.outSlope);
    else if (hit(BB_CANCEL)) { m_bdlg_open = false; }
    else if (hit(BB_OK)) { m_bdlg_open = false; batch_apply(); }
    app.request_redraw();
    return true;
}

// Apply the batch: on every selected lane, a crossfade at each internal clip
// boundary, a fade-in on the first clip and a fade-out on the last (p753).
// Clips without material beyond a boundary have that crossfade clamped to
// what exists, or skipped outright -- reported in the status line.
void ArrangeView::batch_apply()
{
    if (!have_range()) return;
    const long A = m_sel_start, B = m_sel_end;
    std::vector<int> lanes; range_lanes(lanes);
    struct Span { int seq; long on, endEx; };
    std::vector<std::vector<Span>> laneSpans;
    std::vector<int> seqs;
    std::vector<char> seen(c_max_sequence, 0);
    for (int lane : lanes) {
        std::vector<Span> spans;
        for (int cs : lane_sequences(lane)) {
            if (!m_audio.count(cs) || seen[(size_t)cs]) continue;
            std::map<int, AudioRegion>::const_iterator it = m_region.find(cs);
            if (it == m_region.end()) continue;
            const long on = it->second.position, endEx = on + it->second.length;
            if (on < A - 1 || endEx > B + 1) continue;   // whole clips only
            seen[(size_t)cs] = 1;
            spans.push_back(Span{ cs, on, endEx });
        }
        if (spans.empty()) continue;
        std::sort(spans.begin(), spans.end(),
                  [](const Span& x, const Span& y){ return x.on < y.on; });
        for (const Span& sp : spans) seqs.push_back(sp.seq);
        laneSpans.push_back(spans);
    }
    if (seqs.empty()) { flash("No whole audio clips in the selection"); return; }

    int made = 0, skipped = 0;
    fade_edit_op("Batch Fades", seqs, [&]{
        const long xf = ms_ticks(m_b_ms[1]);
        const long fi = ms_ticks(m_b_ms[0]);
        const long fo = ms_ticks(m_b_ms[2]);
        for (const std::vector<Span>& spans : laneSpans) {
            // crossfades at the internal boundaries
            for (size_t i = 0; i + 1 < spans.size(); ++i) {
                const int l = spans[i].seq, r = spans[i + 1].seq;
                long a0 = 0, b0 = 0;
                const bool existing = xfade_window(l, r, a0, b0) &&
                                      (fade_of(l).outTicks > 0 || fade_of(r).inTicks > 0);
                long splice = existing ? a0 + (b0 - a0) / 2
                                       : region_for(r).position;
                long a, b;
                switch (m_b_place) {
                case 0:  a = splice - xf; b = splice; break;       // pre-splice
                case 2:  a = splice; b = splice + xf; break;       // post-splice
                default: a = splice - xf / 2; b = splice + xf - xf / 2; break;
                }
                if (existing) {
                    FadeSettings s = m_b_x;
                    ClipFade lf = fade_of(l), rf = fade_of(r);
                    if (!m_b_shape[1]) {       // keep the existing shapes
                        s.inShape = rf.inShape; s.inSlope = rf.inSlope; s.inK = rf.inK;
                        s.outShape = lf.outShape; s.outSlope = lf.outSlope; s.outK = lf.outK;
                        s.link = rf.link;
                    }
                    if (!m_b_length[1]) { a = a0; b = b0; }        // keep the window
                    if (create_crossfade(l, r, a, b, s, false)) ++made; else ++skipped;
                } else if (m_b_create[1]) {
                    if (create_crossfade(l, r, a, b, m_b_x, false)) ++made; else ++skipped;
                }
            }
            // fade-in on the first clip, fade-out on the last
            const Span& first = spans.front();
            ClipFade f = fade_of(first.seq);
            const bool hasIn = f.inTicks > 0;
            if ((hasIn && (m_b_shape[0] || m_b_length[0])) || (!hasIn && m_b_create[0])) {
                const long len = std::min(fi, (first.endEx - first.on) / 2);
                if (!hasIn || m_b_length[0]) f.inTicks = std::max<long>(1, len);
                if (!hasIn || m_b_shape[0]) {
                    f.inShape = m_b_in.inShape; f.inSlope = m_b_in.inSlope;
                    f.inK = m_b_in.inK;
                }
                m_clipFade[first.seq] = f; commit_fade(first.seq); ++made;
            }
            const Span& last = spans.back();
            ClipFade g = fade_of(last.seq);
            const bool hasOut = g.outTicks > 0;
            if ((hasOut && (m_b_shape[2] || m_b_length[2])) || (!hasOut && m_b_create[2])) {
                const long len = std::min(fo, (last.endEx - last.on) / 2);
                if (!hasOut || m_b_length[2]) g.outTicks = std::max<long>(1, len);
                if (!hasOut || m_b_shape[2]) {
                    g.outShape = m_b_out.outShape; g.outSlope = m_b_out.outSlope;
                    g.outK = m_b_out.outK;
                }
                m_clipFade[last.seq] = g; commit_fade(last.seq); ++made;
            }
        }
    });
    flash("Batch Fades: " + std::to_string(made) + " created/adjusted" +
          (skipped ? ", " + std::to_string(skipped) + " skipped (no audio beyond boundary)"
                   : ""));
}

//----------------------------------------------------------------------------
//  right-click Fades submenu (p756-757)
//----------------------------------------------------------------------------
namespace {
const char* kFadesMenuRows[] = { "Create...        ^F", "Delete Fades",
                                 "Batch Fades...", "-",
                                 "Shape: Standard", "Shape: S-Curve", "-",
                                 "Slope: Equal Power", "Slope: Equal Gain", "-",
                                 "Fade Preferences..." };
const int kFadesMenuN = (int)(sizeof(kFadesMenuRows) / sizeof(kFadesMenuRows[0]));
}

void ArrangeView::draw_fadesmenu(App& app)
{
    if (!m_fadesmenu_open) return;
    const Theme& t = theme();
    const int rowh = app.font.ch() + 6;
    int h = 4;
    for (int i = 0; i < kFadesMenuN; ++i)
        h += kFadesMenuRows[i][0] == '-' ? 5 : rowh;
    SDL_Rect box = fd_clamp(SDL_Rect{ m_fadesmenu_rect.x, m_fadesmenu_rect.y, 170, h },
                            rect);
    m_fadesmenu_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);

    // tick / mixed markers for the selection's current Shape and Slope values:
    // "*" = every selected fade has this value, "~" = the selection mixes
    // values (the manual's italics-no-checkmark state).
    std::vector<std::pair<int,int>> sel;
    selected_fades(sel);
    int shpAll = -2, slpAll = -2;      // -2 none seen, -1 mixed, else the value
    for (const auto& p : sel) {
        const ClipFade f = fade_of(p.first);
        const int sh = p.second == 0 ? f.inShape : f.outShape;
        const int sl = p.second == 0 ? f.inSlope : f.outSlope;
        shpAll = shpAll == -2 ? sh : (shpAll == sh ? shpAll : -1);
        slpAll = slpAll == -2 ? sl : (slpAll == sl ? slpAll : -1);
    }
    auto marker = [&](int i) -> std::string {
        int want = -3, cur = -3;
        if (i == 4) { want = 0; cur = shpAll; }
        else if (i == 5) { want = 1; cur = shpAll; }
        else if (i == 7) { want = 1; cur = slpAll; }
        else if (i == 8) { want = 0; cur = slpAll; }
        else return "  ";
        if (cur == -1) return "~ ";
        return cur == want ? "* " : "  ";
    };
    int y = box.y + 2;
    for (int i = 0; i < kFadesMenuN; ++i) {
        if (kFadesMenuRows[i][0] == '-') {
            hline(app.ren, box.x + 4, box.x + box.w - 4, y + 2, t.dim);
            y += 5;
            continue;
        }
        SDL_Rect row{ box.x + 1, y, box.w - 2, rowh };
        const bool hot = fd_in(row, m_mx, m_my);
        if (hot) fill_rect(app.ren, row, t.accent);
        app.font.draw(app.ren, row.x + 6, row.y + 3, marker(i) + kFadesMenuRows[i],
                      hot ? t.bg : t.text);
        y += rowh;
    }
}

bool ArrangeView::fadesmenu_click(App& app, int mx, int my)
{
    const SDL_Rect& b = m_fadesmenu_rect;
    m_fadesmenu_open = false;
    if (!fd_in(b, mx, my)) { app.request_redraw(); return true; }
    const int rowh = app.font.ch() + 6;
    int y = b.y + 2, id = -1;
    for (int i = 0; i < kFadesMenuN; ++i) {
        const int rh = kFadesMenuRows[i][0] == '-' ? 5 : rowh;
        if (my >= y && my < y + rh) { id = i; break; }
        y += rh;
    }
    switch (id) {
    case 0: create_fades_from_selection(app, false); break;
    case 1: delete_fades_selection(); break;
    case 2: open_batch_dialog(app); break;
    case 4: set_selected_fade_shape(0, 0); break;
    case 5: set_selected_fade_shape(0, 1); break;
    case 7: set_selected_fade_shape(1, 0); break;
    case 8: set_selected_fade_shape(1, 1); break;
    case 10: m_fadepref_open = true; break;
    default: break;
    }
    app.request_redraw();
    return true;
}

} // namespace arrange
