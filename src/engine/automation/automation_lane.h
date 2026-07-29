//----------------------------------------------------------------------------
//  PatchKnob — AutomationLane: one automated target + its breakpoints.
//
//  An AutomationLane is the atom of the automation data model. It binds ONE
//  target to a time-ordered list of breakpoints and an interpolation mode:
//
//      * target      : what this lane drives. Either a hosted VST parameter
//                      (LaneTargetKind::VstParam + paramId) OR a MIDI control
//                      change (LaneTargetKind::MidiCC + controller number 0..127).
//                      The value semantics are identical (a normalized 0..1
//                      float); only the emit path at the coordinator differs
//                      (audio_app_route_param vs. a CC MIDI event).
//      * breakpoints : {int64 tick; float value(0..1)} pairs, ALWAYS kept sorted
//                      ascending by tick with at most one breakpoint per tick.
//      * interp      : how value_at() fills the gaps between breakpoints.
//
//  INTERPOLATION MODES (all agree exactly at breakpoint ticks; they differ only
//  strictly BETWEEN two breakpoints):
//      * Linear : straight line between the two neighbouring breakpoints.
//      * Step   : previous-value staircase — holds the LEFT breakpoint's value
//                 across the segment, then jumps at the next breakpoint. This is
//                 the classic "sample & hold" sequencer step ("a step lane
//                 holds").
//      * Hold   : next-value staircase — jumps to the RIGHT breakpoint's target
//                 immediately after the left breakpoint and holds it. (The exact
//                 breakpoint tick still reports that breakpoint's own value.)
//
//  Outside the breakpoint range value_at() clamps to the first / last value.
//
//  value_at() and the range helpers are O(log n) via binary search and perform
//  NO allocation, so the realtime playback runtime (AutomationPlayer) can call
//  them per audio block. The lane itself is a pure data model: it holds no
//  playback / emit state.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_LANE_H
#define PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_LANE_H

#include <cstdint>
#include <vector>

namespace PatchKnob { namespace engine {

//! What kind of thing a lane drives.
enum class LaneTargetKind {
    VstParam,   //!< a hosted instrument parameter, addressed by paramId.
    MidiCC      //!< a MIDI control change, addressed by controller number 0..127.
};

//! Interpolation between breakpoints. See file header for exact semantics.
enum class Interpolation {
    Linear,     //!< straight line between neighbouring breakpoints.
    Step,       //!< hold previous value, jump at next breakpoint (staircase).
    Hold        //!< jump to next value right after a breakpoint and hold.
};

//! The target a lane drives: a VST parameter id OR a MIDI CC number.
struct LaneTarget {
    LaneTargetKind kind = LaneTargetKind::VstParam;
    unsigned int   id   = 0;   //!< paramId (VstParam) or controller 0..127 (MidiCC).

    bool operator==(const LaneTarget& o) const {
        return kind == o.kind && id == o.id;
    }
};

//! One point on the automation curve.
struct Breakpoint {
    int64_t tick  = 0;      //!< timeline position (sequencer ticks / samples).
    float   value = 0.0f;   //!< normalized 0..1.
};

//! A single automated target and its ordered breakpoint curve.
class AutomationLane {
public:
    AutomationLane() = default;

    AutomationLane(LaneTarget target, Interpolation interp = Interpolation::Linear)
        : target_(target), interp_(interp) {}

    //! Convenience: build a VST-parameter lane.
    static AutomationLane paramLane(unsigned int paramId,
                                    Interpolation interp = Interpolation::Linear) {
        return AutomationLane(LaneTarget{ LaneTargetKind::VstParam, paramId }, interp);
    }

    //! Convenience: build a MIDI-CC lane (controller 0..127).
    static AutomationLane ccLane(unsigned int controller,
                                 Interpolation interp = Interpolation::Linear) {
        return AutomationLane(LaneTarget{ LaneTargetKind::MidiCC, controller }, interp);
    }

    // --- target / mode -------------------------------------------------------

    const LaneTarget& target() const { return target_; }
    void setTarget(const LaneTarget& t) { target_ = t; }

    bool isCC()    const { return target_.kind == LaneTargetKind::MidiCC; }
    bool isParam() const { return target_.kind == LaneTargetKind::VstParam; }

    Interpolation interpolation() const { return interp_; }
    void setInterpolation(Interpolation i) { interp_ = i; }

    // --- breakpoint access ---------------------------------------------------

    int  size()  const { return (int)bps_.size(); }
    bool empty() const { return bps_.empty(); }

    const std::vector<Breakpoint>& breakpoints() const { return bps_; }

    const Breakpoint& at(int index) const { return bps_[(size_t)index]; }

    // --- breakpoint editing (message thread; list kept sorted) ---------------

    //! Insert a breakpoint, keeping the list sorted by tick. If a breakpoint
    //! already exists at `tick`, its value is overwritten (one point per tick).
    //! `value` is clamped to 0..1. Returns the index of the inserted/updated
    //! breakpoint.
    int add(int64_t tick, float value) {
        value = clamp01(value);
        int lo = lowerBoundIndex(tick);
        if (lo < (int)bps_.size() && bps_[(size_t)lo].tick == tick) {
            bps_[(size_t)lo].value = value;    // replace at existing tick
            return lo;
        }
        bps_.insert(bps_.begin() + lo, Breakpoint{ tick, value });
        return lo;
    }

    //! Remove the breakpoint at editable index. Returns false if out of range.
    bool removeAt(int index) {
        if (index < 0 || index >= (int)bps_.size()) return false;
        bps_.erase(bps_.begin() + index);
        return true;
    }

    //! Remove the breakpoint at exactly `tick`, if any. Returns true if removed.
    bool removeAtTick(int64_t tick) {
        int lo = lowerBoundIndex(tick);
        if (lo < (int)bps_.size() && bps_[(size_t)lo].tick == tick) {
            bps_.erase(bps_.begin() + lo);
            return true;
        }
        return false;
    }

    //! Move the breakpoint at `index` to a new tick and value, keeping the list
    //! sorted. Returns the breakpoint's new index, or -1 if `index` is invalid.
    //! If moving onto another breakpoint's tick, that target is overwritten.
    int move(int index, int64_t newTick, float newValue) {
        if (index < 0 || index >= (int)bps_.size()) return -1;
        newValue = clamp01(newValue);
        if (bps_[(size_t)index].tick == newTick) {
            bps_[(size_t)index].value = newValue;   // pure value change
            return index;
        }
        bps_.erase(bps_.begin() + index);
        return add(newTick, newValue);
    }

    //! Remove every breakpoint (target/interpolation unchanged).
    void clear() { bps_.clear(); }

    // --- evaluation (realtime-safe: O(log n), no allocation) -----------------

    //! Curve value at `tick`, honouring the interpolation mode. Empty lane -> 0.
    float value_at(int64_t tick) const {
        if (bps_.empty()) return 0.0f;
        if (tick <= bps_.front().tick) return bps_.front().value;
        if (tick >= bps_.back().tick)  return bps_.back().value;

        const int hi = firstIndexAfter(tick);   // 1..size-1 here
        const Breakpoint& a = bps_[(size_t)(hi - 1)];
        const Breakpoint& b = bps_[(size_t)hi];

        switch (interp_) {
            case Interpolation::Step:
                return a.value;                  // hold left value across segment
            case Interpolation::Hold:
                // jump to the right target just past the left breakpoint; the
                // exact breakpoint tick still reports its own value.
                return (tick == a.tick) ? a.value : b.value;
            case Interpolation::Linear:
            default: {
                const int64_t span = b.tick - a.tick;
                if (span <= 0) return b.value;
                const double f = (double)(tick - a.tick) / (double)span;
                return a.value + (float)f * (b.value - a.value);
            }
        }
    }

    //! Index of the first breakpoint whose tick is strictly greater than `tick`
    //! (== size() if none). Binary search; used by the player to find crossings.
    int firstIndexAfter(int64_t tick) const {
        int lo = 0, hi = (int)bps_.size();
        while (lo < hi) {
            const int mid = lo + ((hi - lo) >> 1);
            if (bps_[(size_t)mid].tick <= tick) lo = mid + 1;
            else                                hi = mid;
        }
        return lo;
    }

private:
    //! Index of the first breakpoint whose tick is >= `tick` (insertion point).
    int lowerBoundIndex(int64_t tick) const {
        int lo = 0, hi = (int)bps_.size();
        while (lo < hi) {
            const int mid = lo + ((hi - lo) >> 1);
            if (bps_[(size_t)mid].tick < tick) lo = mid + 1;
            else                               hi = mid;
        }
        return lo;
    }

    static float clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    LaneTarget              target_;
    Interpolation           interp_ = Interpolation::Linear;
    std::vector<Breakpoint> bps_;    //!< sorted ascending by tick, unique ticks.
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_LANE_H
