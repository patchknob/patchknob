//----------------------------------------------------------------------------
//  PatchKnob — autoops: range processes for automation lanes.
//
//  A "range process" is a non-interactive edit applied to every breakpoint that
//  falls inside a half-open tick range [begin, end). This is the layer the
//  automation editor's right-click / process menu drives: quantise, humanise,
//  stretch, reverse, LFO fill, thin, smooth, and so on.
//
//  CONTRACT — every op in this file honours all of the following:
//
//    * SCOPE.    Only breakpoints with begin <= tick < end are considered.
//                Breakpoints outside the range are preserved BYTE-IDENTICALLY
//                (tick, value AND curve), with two documented exceptions that
//                are ripple edits by definition: InsertTime, DeleteTime and
//                DuplicateRange, which deliberately move everything after the
//                range. opIsRipple() reports which ones.
//
//    * COLLISIONS. When a time-moving op lands a processed breakpoint on a tick
//                that an untouched outside breakpoint already occupies, the
//                OUTSIDE breakpoint wins and the moved one is dropped. That is
//                what makes "outside is untouched" an invariant rather than a
//                best effort. Within the processed set itself the FIRST point
//                of a colliding group survives.
//
//    * INVARIANT. The lane is left sorted ascending by tick with exactly one
//                breakpoint per tick, and every value in 0..1. No op can emit
//                NaN: all divisions are guarded and all generated values are
//                clamped before they are committed.
//
//    * DETERMINISM. Humanise, RandomWalk and NoiseFill draw from a stateless
//                hash of (seed, index-or-tick), never from a global RNG. The
//                same lane + same Params always yields the same result, so a
//                musician can re-run a process and get their sound back.
//
//    * SAFETY.   Every op is a no-op (returns false) rather than a crash on an
//                empty lane, a zero-length range, a range holding 0 or 1
//                points, or a range that misses the lane's data entirely.
//
//  apply() returns false when the op changed nothing at all, so the caller can
//  skip pushing an undo entry.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_OPS_H
#define PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_OPS_H

#include <cstdint>
#include <vector>

#include "automation_lane.h"

namespace PatchKnob { namespace engine { namespace autoops {

//! Half-open tick range [begin, end). end <= begin means "empty".
struct Range {
    int64_t begin = 0, end = 0;

    int64_t length() const { return end > begin ? end - begin : 0; }
    bool    empty()  const { return end <= begin; }
    bool    contains(int64_t t) const { return t >= begin && t < end; }
};

//! Every knob a range process can read. One struct for all ops: each op
//! documents the fields it actually uses (see opName()/the .cpp header block).
struct Params {
    float    amount  = 1.0f;   //!< generic depth/strength 0..1 (or more).
    float    pivot   = 0.5f;   //!< value pivot for scale/invert.
    int64_t  grid    = 0;      //!< ticks; 0 == no grid.
    int      steps   = 8;      //!< for step-quantise / densify.
    float    freqHz  = 1.0f;   //!< LFO rate (or cycles-per-range if syncCycles>0).
    int      syncCycles = 0;   //!< >0 == that many cycles across the range.
    int      shape   = 0;      //!< 0 sine 1 tri 2 saw 3 ramp 4 square 5 random.
    float    phase   = 0.0f;   //!< 0..1.
    float    lo = 0.0f, hi = 1.0f;   //!< clamp / normalise bounds.
    uint32_t seed    = 1;      //!< deterministic randomness.
};

//! The range processes. Grouped: time, ripple, value, density, generators,
//! multi-lane. Append new ops BEFORE OP_COUNT only — the UI indexes this enum.
enum Op {
    // --- time -------------------------------------------------------------
    Quantise, Humanise, Stretch, Reverse, ShiftTime, DuplicateRange,
    // --- ripple (these intentionally move breakpoints after the range) ----
    InsertTime, DeleteTime,
    // --- value ------------------------------------------------------------
    ScaleValue, OffsetValue, InvertValue, Normalise, ClampValue,
    // --- density ----------------------------------------------------------
    Smooth, Thin, Densify,
    // --- generators -------------------------------------------------------
    LfoFill, RampFill, SCurveFill, BendCurve, StepQuantise, RandomWalk, NoiseFill,
    // --- multi-lane -------------------------------------------------------
    CopyShape, MirrorLanes, PhaseOffsetLanes, AverageLanes,
    OP_COUNT
};

//! Stable display name ("Quantise", "LFO Fill", ...). Never null, even for an
//! out-of-range op (returns "?").
const char* opName(Op op);

//! True for the CopyShape..AverageLanes group — ops that read lanes[0] as the
//! source and write the others. These are the ops applyMulti() exists for.
bool opIsMultiLane(Op op);

//! True for the ops that deliberately move breakpoints outside the range
//! (InsertTime, DeleteTime, DuplicateRange). Everything else leaves outside
//! breakpoints byte-identical.
bool opIsRipple(Op op);

//! Apply to ONE lane. Returns false if the op did nothing.
//! Multi-lane ops are forwarded to applyMulti() with a single-lane vector,
//! where most of them are no-ops (MirrorLanes is the useful exception: it
//! mirrors the lane about Params::pivot in place).
bool apply(AutomationLane& lane, const Range& r, Op op, const Params& p);

//! Apply across several lanes at once (multi-lane ops read lanes[0] as source).
//! Single-lane ops are applied independently to every lane; the return value is
//! true if ANY lane changed. Null entries are skipped.
bool applyMulti(const std::vector<AutomationLane*>& lanes, const Range& r,
                Op op, const Params& p);

}}} // namespace PatchKnob::engine::autoops

#endif // PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_OPS_H
