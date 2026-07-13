//----------------------------------------------------------------------------
//  seq24 Windows port — AutomationPlayer implementation. See header for the
//  advance()/emitAt() contract and threading notes.
//----------------------------------------------------------------------------
#include "automation_player.h"

#include <cmath>

namespace seq24 { namespace engine {

int AutomationPlayer::ccFromNorm(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 127;
    return (int)std::lround((double)v * 127.0);
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
        if (memo_[t].size() != nl) memo_[t].resize(nl);
    }
}

void AutomationPlayer::emitOne(int track, const AutomationLane& lane, float v,
                               LaneMemo& memo,
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
            if (emitParam) emitParam(track, lane.target().id, v);
            memo.lastNorm = v;
            memo.lastCC   = ccFromNorm(v);
            memo.has      = true;
        }
    }
}

void AutomationPlayer::emitForce(int track, const AutomationLane& lane, float v,
                                 LaneMemo& memo,
                                 const EmitParam& emitParam,
                                 const EmitCC& emitCC) const {
    if (lane.isCC()) {
        const int icc = ccFromNorm(v);
        if (emitCC) emitCC(track, (int)lane.target().id, icc);
        memo.lastCC   = icc;
        memo.lastNorm = v;
        memo.has      = true;
    } else {
        if (emitParam) emitParam(track, lane.target().id, v);
        memo.lastNorm = v;
        memo.lastCC   = ccFromNorm(v);
        memo.has      = true;
    }
}

void AutomationPlayer::advance(int64_t fromTick, int64_t toTick,
                               const EmitParam& emitParam,
                               const EmitCC& emitCC) {
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
                        memo, emitParam, emitCC);
            }

            // Always emit the resting value at the end of the window.
            emitOne((int)t, lane, lane.value_at(toTick), memo, emitParam, emitCC);
        }
    }
}

void AutomationPlayer::emitAt(int64_t tick,
                              const EmitParam& emitParam,
                              const EmitCC& emitCC) {
    syncMemo();
    for (size_t t = 0; t < tracks_.size(); ++t) {
        AutomationTrack& at = tracks_[t];
        const int nl = at.laneCount();
        for (int l = 0; l < nl; ++l) {
            const AutomationLane& lane = at.lane(l);
            if (lane.empty()) continue;
            emitForce((int)t, lane, lane.value_at(tick),
                      memo_[t][(size_t)l], emitParam, emitCC);
        }
    }
}

}} // namespace seq24::engine
