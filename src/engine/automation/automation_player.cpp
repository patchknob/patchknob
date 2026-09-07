//----------------------------------------------------------------------------
//  PatchKnob — AutomationPlayer implementation. See header for the
//  advance()/emitAt() contract and threading notes.
//----------------------------------------------------------------------------
#include "automation_player.h"

#include <cmath>

namespace PatchKnob { namespace engine {

/*  The clip-local position of an arranged tick, folded into the region's own
    loop window [loopStart, loopLength).  Folding by the END alone (which is all
    this used to do) is only correct for a window that starts at 0: any other
    window wrapped at the right tick but replayed the curve from the top. */
static inline int64_t fold_into_window(int64_t raw, int64_t loopStart, int64_t loopEnd) {
    const int64_t ls = loopStart > 0 ? loopStart : 0;
    const int64_t le = loopEnd > ls ? loopEnd : ls + 1;
    const int64_t period = le - ls;
    int64_t p = (raw - ls) % period;
    if (p < 0) p += period;
    return ls + p;
}

int AutomationPlayer::ccFromNorm(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 127;
    return (int)std::lround((double)v * 127.0);
}

void AutomationPlayer::advanceRegions(int64_t fromTick, int64_t toTick,
                                      const EmitTarget& emitTarget,
                                      const EmitCC& emitCC) const {
    if (toTick <= fromTick) return;
    for (const Region& r : regions_) {
        if (r.muted || r.length <= 0) continue;
        const int64_t begin = std::max(fromTick, r.position);
        const int64_t end   = std::min(toTick, r.position + r.length);
        if (end <= begin) continue;
        const int64_t ls = std::max<int64_t>(0, r.loopStart);
        const int64_t le = std::max<int64_t>(ls + 1, r.loopLength);
        const int64_t localEnd = fold_into_window(r.source + (end - r.position), ls, le);
        for (int li=0; li<r.automation.laneCount(); ++li) {
            const AutomationLane& lane = r.automation.lane(li);
            if (lane.empty()) continue;
            const float value = lane.value_at(localEnd);
            if (lane.isCC()) {
                if (emitCC) emitCC(r.destinationTrack, (int)lane.target().id,
                                   ccFromNorm(value));
            } else if (emitTarget) {
                emitTarget(r.destinationTrack, lane.target(), value);
            }
        }
    }
}

void AutomationPlayer::advanceRegionsScheduled(int64_t fromTick, int64_t toTick,
                                                const EmitTargetAt& emitTarget,
                                                const EmitCCAt& emitCC) const {
    if (toTick <= fromTick) return;
    for (const Region& r : regions_) {
        if (r.muted || r.length <= 0) continue;
        const int64_t begin = std::max(fromTick, r.position);
        const int64_t end = std::min(toTick, r.position + r.length);
        if (end <= begin) continue;
        const int64_t ls = std::max<int64_t>(0, r.loopStart);
        const int64_t le = std::max<int64_t>(ls + 1, r.loopLength);
        for (int li = 0; li < r.automation.laneCount(); ++li) {
            const AutomationLane& lane = r.automation.lane(li);
            if (lane.empty()) continue;
            // Split at every source-loop boundary and emit every breakpoint in
            // every repetition.  The absolute tick travels with the value.
            int64_t p = begin;
            while (p < end) {
                const int64_t local = fold_into_window(r.source + (p - r.position), ls, le);
                /* to the END of the window, not to loopLength from zero */
                const int64_t span = std::min(end - p, le - local);
                const int64_t localEnd = local + span;
                const auto& bps = lane.breakpoints();
                int bi = lane.firstIndexAfter(local - 1);
                for (; bi < (int)bps.size() && bps[(size_t)bi].tick < localEnd; ++bi) {
                    const int64_t due = p + (bps[(size_t)bi].tick - local);
                    const float v = lane.value_at(bps[(size_t)bi].tick);
                    if (lane.isCC()) { if (emitCC) emitCC(r.destinationTrack,
                        (int)lane.target().id, ccFromNorm(v), due); }
                    else if (emitTarget) emitTarget(r.destinationTrack, lane.target(), v, due);
                }
                // A continuous curve needs a value at the scheduling-window end;
                // timestamp it instead of applying it on the message thread.
                const int64_t due = p + span - 1;
                const float v = lane.value_at(localEnd);
                if (lane.isCC()) { if (emitCC) emitCC(r.destinationTrack,
                    (int)lane.target().id, ccFromNorm(v), due); }
                else if (emitTarget) emitTarget(r.destinationTrack, lane.target(), v, due);
                p += span;
            }
        }
    }
}

void AutomationPlayer::resetEmitState() {
    for (auto& tm : memo_)
        for (auto& m : tm)
            m = LaneMemo{};
}

void AutomationPlayer::syncMemo() {
    // memo_ always has one entry per track (sized in the ctor); only the
    // inner per-lane vectors track structural edits. Resizing a shrunk/grown
    // track appends fresh (has=false) memos for new lanes so they emit once,
    // and preserves memory for the lanes that kept their index.
    const size_t nt = tracks_.size();
    if (memo_.size() != nt) memo_.resize(nt);
    for (size_t t = 0; t < nt; ++t) {
        const size_t nl = (size_t)tracks_[t].laneCount();
        // A lane-count change means a structural edit (add/delete/reorder).
        // Deleting a middle lane shifts every later lane down one index, so a
        // memo kept by index would now belong to a DIFFERENT lane and wrongly
        // coalesce away its first emit.  Reset the whole track's memo so every
        // lane re-emits once cleanly.
        if (memo_[t].size() != nl) {
            memo_[t].assign(nl, LaneMemo{});
        }
    }
}

/*  Route ONE non-CC track-lane value.  A track lane's LaneTarget can name a
    patch-graph node (PatchParam/RackParam) just as a region lane's can; the
    (track, id) EmitParam signature cannot express that, because it drops
    target.node.  Prefer the target-aware callback whenever the lane is not a
    plain VstParam and the caller supplied one -- otherwise fall back to the
    historical behaviour so nothing that already worked changes. */
static inline void emitNonCC(int track, const AutomationLane& lane, float v,
                             const AutomationPlayer::EmitTarget& emitTarget,
                             const AutomationPlayer::EmitParam& emitParam) {
    if (lane.target().kind != LaneTargetKind::VstParam && emitTarget)
        emitTarget(track, lane.target(), v);
    else if (emitParam)
        emitParam(track, lane.target().id, v);
}

void AutomationPlayer::emitOne(int track, const AutomationLane& lane, float v,
                               LaneMemo& memo,
                               const EmitTarget& emitTarget,
                               const EmitParam& emitParam,
                               const EmitCC& emitCC) const {
    if (lane.isCC()) {
        const int icc = ccFromNorm(v);
        if (!coalesce_ || !memo.has || icc != memo.lastCC) {
            if (emitCC) emitCC(track, (int)lane.target().id, icc);
            memo.lastCC   = icc;
            memo.lastNorm = v;
            memo.has      = true;
        }
    } else {
        if (!coalesce_ || !memo.has || std::fabs(v - memo.lastNorm) > 1e-6f) {
            emitNonCC(track, lane, v, emitTarget, emitParam);
            memo.lastNorm = v;
            memo.lastCC   = ccFromNorm(v);
            memo.has      = true;
        }
    }
}

void AutomationPlayer::emitForce(int track, const AutomationLane& lane, float v,
                                 LaneMemo& memo,
                                 const EmitTarget& emitTarget,
                                 const EmitParam& emitParam,
                                 const EmitCC& emitCC) const {
    if (lane.isCC()) {
        const int icc = ccFromNorm(v);
        if (emitCC) emitCC(track, (int)lane.target().id, icc);
        memo.lastCC   = icc;
        memo.lastNorm = v;
        memo.has      = true;
    } else {
        emitNonCC(track, lane, v, emitTarget, emitParam);
        memo.lastNorm = v;
        memo.lastCC   = ccFromNorm(v);
        memo.has      = true;
    }
}

void AutomationPlayer::advance(int64_t fromTick, int64_t toTick,
                               const EmitParam& emitParam,
                               const EmitCC& emitCC,
                               const EmitTarget& emitTarget) {
    if (toTick <= fromTick) return;   // empty / reversed window: nothing due.
    syncMemo();

    for (size_t t = 0; t < tracks_.size(); ++t) {
        AutomationTrack& at = tracks_[t];
        const int nl = at.laneCount();
        for (int l = 0; l < nl; ++l) {
            const AutomationLane& lane = at.lane(l);
            if (lane.empty()) continue;
            LaneMemo& memo = memo_[t][(size_t)l];

            // Sub-sample every breakpoint strictly crossed inside the window:
            // firstIndexAfter(fromTick) skips a point sitting exactly on the
            // window start (already emitted as the previous block's toTick).
            const std::vector<Breakpoint>& bps = lane.breakpoints();
            int i = lane.firstIndexAfter(fromTick);
            for (; i < (int)bps.size() && bps[(size_t)i].tick < toTick; ++i) {
                emitOne((int)t, lane, lane.value_at(bps[(size_t)i].tick),
                        memo, emitTarget, emitParam, emitCC);
            }

            // Always emit the resting value at the end of the window.
            emitOne((int)t, lane, lane.value_at(toTick), memo, emitTarget,
                    emitParam, emitCC);
        }
    }
}

void AutomationPlayer::advanceScheduled(int64_t fromTick, int64_t toTick,
                                        const EmitParamAt& emitParam,
                                        const EmitCCAt& emitCC,
                                        const EmitTargetAt& emitTarget) {
    /*  Same target-aware routing as emitNonCC(), timestamped.  Without it a
        PatchParam/RackParam lane on a TRACK was emitted as (trackIndex,
        target.id) with target.node thrown away -- i.e. aimed at a mixer-track
        instrument instead of the patch node the user picked. */
    auto route = [&](int t, const AutomationLane& lane, float v, int64_t due) {
        if (lane.target().kind != LaneTargetKind::VstParam && emitTarget)
            emitTarget(t, lane.target(), v, due);
        else if (emitParam)
            emitParam(t, lane.target().id, v, due);
    };
    if (toTick <= fromTick) return;
    for (size_t t = 0; t < tracks_.size(); ++t) {
        const AutomationTrack& at = tracks_[t];
        for (int l = 0; l < at.laneCount(); ++l) {
            const AutomationLane& lane = at.lane(l);
            if (lane.empty()) continue;
            const auto& bps = lane.breakpoints();
            int i = lane.firstIndexAfter(fromTick - 1);
            for (; i < (int)bps.size() && bps[(size_t)i].tick < toTick; ++i) {
                const float v = lane.value_at(bps[(size_t)i].tick);
                if (lane.isCC()) { if (emitCC) emitCC((int)t, (int)lane.target().id,
                    ccFromNorm(v), bps[(size_t)i].tick); }
                else route((int)t, lane, v, bps[(size_t)i].tick);
            }
            const int64_t due = toTick - 1;
            const float v = lane.value_at(toTick);
            if (lane.isCC()) { if (emitCC) emitCC((int)t, (int)lane.target().id,
                ccFromNorm(v), due); }
            else route((int)t, lane, v, due);
        }
    }
}

void AutomationPlayer::emitAt(int64_t tick,
                              const EmitParam& emitParam,
                              const EmitCC& emitCC,
                              const EmitTarget& emitTarget) {
    syncMemo();
    for (size_t t = 0; t < tracks_.size(); ++t) {
        AutomationTrack& at = tracks_[t];
        const int nl = at.laneCount();
        for (int l = 0; l < nl; ++l) {
            const AutomationLane& lane = at.lane(l);
            if (lane.empty()) continue;
            emitForce((int)t, lane, lane.value_at(tick),
                      memo_[t][(size_t)l], emitTarget, emitParam, emitCC);
        }
    }
}

}} // namespace PatchKnob::engine
