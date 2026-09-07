//----------------------------------------------------------------------------
//  PatchKnob — autoops implementation.
//
//  PER-OP PARAMETER MAP (which Params fields each op reads):
//
//    Quantise    grid|steps, amount    snap ticks to the grid; amount is the
//                                      strength (0 = none, 1 = fully snapped).
//    Humanise    grid|steps, amount, seed
//                                      deterministic tick jitter, clamped so a
//                                      point can never pass its neighbours.
//    Stretch     amount                scale ticks about r.begin.
//    Reverse     -                     mirror ticks about the selection's own
//                                      first/last point (a bijection, so no
//                                      point is ever lost).
//    ShiftTime   grid, amount          delta = grid*amount, or range*amount
//                                      when grid == 0. amount may be negative.
//    DuplicateRange  -                 copy the range in right after itself,
//                                      rippling later points by the length.
//    InsertTime  grid                  open range-length (or grid) ticks of
//                                      silence at r.begin. RIPPLE.
//    DeleteTime  -                     close the range up. RIPPLE.
//    ScaleValue  amount, pivot         v = pivot + (v - pivot) * amount.
//    OffsetValue amount                v += amount (SIGNED, normalised units).
//    InvertValue amount, pivot         v -> 2*pivot - v, blended by amount.
//    Normalise   lo, hi, amount        rescale the range's own min/max onto
//                                      lo..hi.
//    ClampValue  lo, hi                clamp values into lo..hi.
//    Smooth      steps, amount         moving average, window radius steps/2,
//                                      ticks fixed, range endpoints pinned.
//    Thin        amount                Ramer-Douglas-Peucker, amount = the
//                                      value-error tolerance.
//    Densify     grid|steps            resample the EXISTING curve onto a
//                                      grid, keeping every original point.
//    LfoFill     shape, freqHz, syncCycles, phase, amount, lo, hi, grid|steps, seed
//    RampFill    lo, hi, amount, grid|steps
//    SCurveFill  lo, hi, amount, grid|steps
//    NoiseFill   lo, hi, amount, seed, grid|steps
//    BendCurve   amount                writes Breakpoint::curve; amount 0..1
//                                      maps onto the bipolar -1..+1 bend.
//    StepQuantise steps, lo, hi, amount   quantise VALUES to `steps` levels.
//    RandomWalk  amount, seed          deterministic walk from the first
//                                      in-range value; ticks untouched.
//    CopyShape / MirrorLanes / PhaseOffsetLanes / AverageLanes
//                amount, pivot, phase, grid|steps
//
//  The generator ops (LfoFill/RampFill/SCurveFill/NoiseFill) REPLACE the
//  range's breakpoints with a fresh shape sampled at `grid` ticks, or at
//  `steps` evenly spaced points when grid == 0.
//----------------------------------------------------------------------------
#include "automation_ops.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace PatchKnob { namespace engine { namespace autoops {
namespace {

// ===========================================================================
//  small numeric helpers
// ===========================================================================

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

inline float clampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

//! Finite guard: an op must never be able to write NaN/inf into a lane.
inline float safeValue(float v) {
    if (!(v == v)) return 0.f;               // NaN
    if (v > 1e30f) return 1.f;
    if (v < -1e30f) return 0.f;
    return clamp01(v);
}

inline bool finiteF(float v) { return v == v && v < 1e30f && v > -1e30f; }

//! 192 PPQN (globals.h c_ppqn) at 120 BPM = 384 ticks per second. Only used to
//! turn Params::freqHz into cycles when syncCycles == 0; the lane carries no
//! tempo map, so a tempo-locked LFO should pass syncCycles instead.
const double kTicksPerSecond = 384.0;

//! Hard ceilings so a pathological grid/steps cannot allocate the world.
const int64_t kMaxGeneratedPoints = 200000;

// --- deterministic, stateless randomness -----------------------------------
// Position-hashed rather than sequential: re-running a process on the same
// lane with the same seed reproduces every draw exactly, and the draws do not
// shift when an unrelated part of the lane is edited.

inline uint32_t hash32(uint32_t seed, uint32_t idx) {
    uint32_t h = seed * 0x9E3779B9u + idx * 0x85EBCA6Bu + 0x165667B1u;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

//! Uniform in [0,1).
inline float rand01(uint32_t seed, uint32_t idx) {
    return (float)(hash32(seed, idx) >> 8) * (1.0f / 16777216.0f);
}

//! Uniform in [-1,1).
inline float randBi(uint32_t seed, uint32_t idx) {
    return rand01(seed, idx) * 2.0f - 1.0f;
}

//! Fold a tick into a hash index so jitter is stable per musical position.
inline uint32_t tickIdx(int64_t t) {
    const uint64_t u = (uint64_t)t;
    return (uint32_t)(u ^ (u >> 32));
}

// --- LFO shapes -------------------------------------------------------------
//! ph is the running phase in cycles (not radians); returns -1..1.
float waveform(int shape, double ph, uint32_t seed) {
    double f = ph - std::floor(ph);            // fractional phase 0..1
    switch (shape) {
        case 1:                                 // triangle (sine-aligned)
            if (f < 0.25) return (float)(4.0 * f);
            if (f < 0.75) return (float)(2.0 - 4.0 * f);
            return (float)(4.0 * f - 4.0);
        case 2: return (float)(1.0 - 2.0 * f);  // saw down
        case 3: return (float)(2.0 * f - 1.0);  // ramp up
        case 4: return f < 0.5 ? 1.f : -1.f;    // square
        case 5: {                               // sample & hold, one per cycle
            const double c = std::floor(ph);
            return randBi(seed, (uint32_t)(int64_t)c + 0x51EDu);
        }
        case 0:
        default: return (float)std::sin(6.283185307179586 * f);
    }
}

// ===========================================================================
//  range extraction / commit
//
//  Every non-ripple op follows the same shape:
//      1. locate [i0,i1) — the breakpoints inside the range,
//      2. build a replacement vector for just that slice,
//      3. mergeCommit() splices it back, enforcing "outside wins" on any tick
//         collision, then writes the lane only if something actually changed.
// ===========================================================================

inline bool bpEqual(const Breakpoint& a, const Breakpoint& b) {
    return a.tick == b.tick && a.value == b.value && a.curve == b.curve;
}

inline bool byTick(const Breakpoint& a, const Breakpoint& b) { return a.tick < b.tick; }

void rangeIndices(const std::vector<Breakpoint>& b, const Range& r,
                  size_t& i0, size_t& i1) {
    i0 = (size_t)(std::lower_bound(b.begin(), b.end(), Breakpoint{ r.begin, 0.f, 0.f },
                                   byTick) - b.begin());
    i1 = (size_t)(std::lower_bound(b.begin(), b.end(), Breakpoint{ r.end, 0.f, 0.f },
                                   byTick) - b.begin());
    if (i1 < i0) i1 = i0;
}

//! Write `out` to the lane if it differs from what is there. Returns false when
//! the op was a no-op so the caller can skip the undo entry.
bool commit(AutomationLane& lane, std::vector<Breakpoint>& out) {
    const std::vector<Breakpoint>& cur = lane.breakpoints();
    if (out.size() == cur.size() &&
        std::equal(cur.begin(), cur.end(), out.begin(), bpEqual))
        return false;
    lane.setBreakpoints(std::move(out));
    return true;
}

//! Splice a processed slice back into the lane.
//!   * every breakpoint outside [i0,i1) is carried over untouched,
//!   * processed points are clamped, sorted, and reduced to one per tick
//!     (first of an equal-tick group wins),
//!   * a processed point that lands on a tick an outside point already owns is
//!     DROPPED — the outside point is authoritative. That is what makes
//!     "outside is byte-identical" an invariant and not a hope.
bool mergeCommit(AutomationLane& lane, size_t i0, size_t i1,
                 std::vector<Breakpoint> inside) {
    const std::vector<Breakpoint>& all = lane.breakpoints();

    for (auto& b : inside) {
        b.value = safeValue(b.value);
        b.curve = finiteF(b.curve) ? clampf(b.curve, -1.f, 1.f) : 0.f;
        if (b.tick < 0) b.tick = 0;
    }
    std::stable_sort(inside.begin(), inside.end(), byTick);
    {   // collapse equal ticks, first wins
        size_t w = 0;
        for (size_t i = 0; i < inside.size(); ++i)
            if (w == 0 || inside[w - 1].tick != inside[i].tick) inside[w++] = inside[i];
        inside.resize(w);
    }

    std::vector<Breakpoint> out;
    out.reserve(all.size() + inside.size());
    out.insert(out.end(), all.begin(), all.begin() + (std::ptrdiff_t)i0);
    out.insert(out.end(), all.begin() + (std::ptrdiff_t)i1, all.end());
    // `out` is still sorted here: the head's ticks are all < r.begin and the
    // tail's are all >= r.end, so binary search over it is valid.
    const size_t keptCount = out.size();
    for (const auto& b : inside) {
        const bool taken = std::binary_search(out.begin(),
                                              out.begin() + (std::ptrdiff_t)keptCount,
                                              b, byTick);
        if (!taken) out.push_back(b);
    }
    std::sort(out.begin(), out.end(), byTick);
    return commit(lane, out);
}

// ===========================================================================
//  tick grids for the generator / resampling ops
// ===========================================================================

//! The ticks a generator writes: multiples of `grid` inside the range, or
//! `steps` evenly spaced positions spanning [begin, end-1] when grid == 0.
std::vector<int64_t> genTicks(const Range& r, const Params& p) {
    std::vector<int64_t> out;
    const int64_t len = r.length();
    if (len <= 0) return out;

    if (p.grid > 0) {
        if (len / p.grid > kMaxGeneratedPoints) return out;   // refuse, don't hang
        for (int64_t t = r.begin; t < r.end; t += p.grid) out.push_back(t);
        if (out.empty()) out.push_back(r.begin);
        return out;
    }

    int n = p.steps < 2 ? 2 : p.steps;
    if ((int64_t)n > kMaxGeneratedPoints) n = (int)kMaxGeneratedPoints;
    if (len == 1) { out.push_back(r.begin); return out; }
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double f = (double)i / (double)(n - 1);
        out.push_back(r.begin + (int64_t)std::llround(f * (double)(len - 1)));
    }
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

//! lo/hi as an ordered, 0..1-clamped pair (the UI may hand them over swapped).
void orderedBounds(const Params& p, float& lo, float& hi) {
    lo = finiteF(p.lo) ? clamp01(p.lo) : 0.f;
    hi = finiteF(p.hi) ? clamp01(p.hi) : 1.f;
    if (lo > hi) std::swap(lo, hi);
}

// ===========================================================================
//  TIME OPS
// ===========================================================================

bool opQuantise(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i0 >= i1) return false;

    int64_t g = p.grid > 0 ? p.grid : (r.length() / (p.steps > 0 ? p.steps : 1));
    if (g <= 0) return false;
    const float a = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;
    if (a <= 0.f) return false;

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    for (auto& b : inside) {
        const int64_t snap = (int64_t)std::llround((double)b.tick / (double)g) * g;
        b.tick += (int64_t)std::llround((double)(snap - b.tick) * (double)a);
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opHumanise(AutomationLane& lane, const Range& r, const Params& p) {
    const std::vector<Breakpoint>& all = lane.breakpoints();
    size_t i0, i1; rangeIndices(all, r, i0, i1);
    if (i0 >= i1) return false;

    int64_t window = p.grid > 0 ? p.grid
                                : r.length() / (p.steps > 0 ? p.steps : 1);
    if (window <= 0) window = 1;
    const float a = finiteF(p.amount) ? std::fabs(p.amount) : 1.f;
    if (a <= 0.f) return false;

    std::vector<Breakpoint> inside(all.begin() + (std::ptrdiff_t)i0,
                                   all.begin() + (std::ptrdiff_t)i1);
    // Jitter, then clamp against the ALREADY-PLACED previous point and the next
    // untouched point. Order is preserved by construction, so humanising never
    // collapses or reorders the curve.
    for (size_t i = 0; i < inside.size(); ++i) {
        const int64_t orig = inside[i].tick;
        const double  d    = (double)randBi(p.seed, tickIdx(orig)) * (double)a
                           * (double)window * 0.5;
        int64_t t = orig + (int64_t)std::llround(d);

        int64_t lo = r.begin;
        if (i > 0)                    lo = std::max(lo, inside[i - 1].tick + 1);
        else if (i0 > 0)              lo = std::max(lo, all[i0 - 1].tick + 1);
        int64_t hi = r.end - 1;
        if (i + 1 < inside.size())    hi = std::min(hi, inside[i + 1].tick - 1);
        else if (i1 < all.size())     hi = std::min(hi, all[i1].tick - 1);
        if (hi < lo) { t = orig; }
        else         { t = t < lo ? lo : (t > hi ? hi : t); }
        inside[i].tick = t < 0 ? 0 : t;
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opStretch(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i0 >= i1) return false;
    const float s = p.amount;
    if (!finiteF(s) || s <= 0.f || s == 1.f) return false;

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    // A positive scale about a fixed origin is monotonic, so relative order is
    // preserved; only rounding can fuse two points, which mergeCommit collapses.
    for (auto& b : inside) {
        const double rel = (double)(b.tick - r.begin) * (double)s;
        b.tick = r.begin + (int64_t)std::llround(rel);
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opReverse(AutomationLane& lane, const Range& r, const Params& p) {
    (void)p;
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i1 - i0 < 2) return false;

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    // Mirror about the SELECTION'S OWN first/last point rather than about the
    // range bounds: that keeps the map a bijection on the existing ticks, so no
    // point can be rounded onto another and lost, and nothing leaves the range.
    const int64_t lo = inside.front().tick, hi = inside.back().tick;
    const size_t n = inside.size();
    std::vector<Breakpoint> out(n);
    for (size_t j = 0; j < n; ++j) {
        const size_t src = n - 1 - j;
        out[j].tick  = lo + hi - inside[src].tick;
        out[j].value = inside[src].value;
        // A segment's bend belongs to the segment, not the point: after the
        // flip, point j leads into what used to be the segment arriving at it,
        // traversed backwards -- hence the index shift and the sign flip. The
        // last point's segment leaves the selection and is left alone.
        out[j].curve = (j + 1 < n) ? -inside[n - 2 - j].curve : inside[n - 1].curve;
    }
    return mergeCommit(lane, i0, i1, std::move(out));
}

bool opShiftTime(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i0 >= i1) return false;
    if (!finiteF(p.amount)) return false;

    const double base = (p.grid != 0) ? (double)p.grid : (double)r.length();
    const int64_t delta = (int64_t)std::llround(base * (double)p.amount);
    if (delta == 0) return false;

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    for (auto& b : inside) {
        const int64_t t = b.tick + delta;
        b.tick = t < 0 ? 0 : t;
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

// --- ripple edits ----------------------------------------------------------
// These three are the documented exception to "outside is untouched": moving
// later material is the whole point of a ripple.

bool opDuplicateRange(AutomationLane& lane, const Range& r, const Params& p) {
    (void)p;
    const std::vector<Breakpoint>& all = lane.breakpoints();
    const int64_t len = r.length();
    if (len <= 0) return false;
    size_t i0, i1; rangeIndices(all, r, i0, i1);
    if (i0 >= i1) return false;

    std::vector<Breakpoint> out;
    out.reserve(all.size() + (i1 - i0));
    for (size_t i = 0; i < all.size(); ++i) {
        Breakpoint b = all[i];
        if (b.tick >= r.end) b.tick += len;     // ripple later material right
        out.push_back(b);
    }
    for (size_t i = i0; i < i1; ++i) {          // the copy lands in [end,end+len)
        Breakpoint b = all[i];
        b.tick += len;
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(), byTick);
    return commit(lane, out);
}

bool opInsertTime(AutomationLane& lane, const Range& r, const Params& p) {
    int64_t len = r.length();
    if (len <= 0) len = p.grid > 0 ? p.grid : 0;
    if (len <= 0) return false;

    std::vector<Breakpoint> out = lane.breakpoints();
    bool moved = false;
    for (auto& b : out)
        if (b.tick >= r.begin) { b.tick += len; moved = true; }
    if (!moved) return false;
    return commit(lane, out);               // a uniform shift of a suffix stays sorted
}

bool opDeleteTime(AutomationLane& lane, const Range& r, const Params& p) {
    (void)p;
    const int64_t len = r.length();
    if (len <= 0) return false;

    std::vector<Breakpoint> out;
    out.reserve(lane.breakpoints().size());
    for (const auto& b : lane.breakpoints()) {
        if (b.tick >= r.begin && b.tick < r.end) continue;   // swallowed
        Breakpoint n = b;
        if (n.tick >= r.end) n.tick -= len;
        out.push_back(n);
    }
    // Kept heads are strictly < begin and shifted tails land at >= begin, so
    // the result is sorted and duplicate-free without further work.
    return commit(lane, out);
}

// ===========================================================================
//  VALUE OPS  (ticks untouched -> they can share one path)
// ===========================================================================

template <typename F>
bool mapValues(AutomationLane& lane, const Range& r, F fn) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i0 >= i1) return false;
    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    for (auto& b : inside) b.value = safeValue(fn(b.value));
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opScaleValue(AutomationLane& lane, const Range& r, const Params& p) {
    if (!finiteF(p.amount) || !finiteF(p.pivot)) return false;
    const float a = p.amount, pv = p.pivot;
    return mapValues(lane, r, [a, pv](float v) { return pv + (v - pv) * a; });
}

bool opOffsetValue(AutomationLane& lane, const Range& r, const Params& p) {
    if (!finiteF(p.amount) || p.amount == 0.f) return false;
    const float a = p.amount;
    return mapValues(lane, r, [a](float v) { return v + a; });
}

bool opInvertValue(AutomationLane& lane, const Range& r, const Params& p) {
    if (!finiteF(p.amount) || !finiteF(p.pivot)) return false;
    const float a = p.amount, pv = p.pivot;
    return mapValues(lane, r, [a, pv](float v) { return lerpf(v, 2.f * pv - v, a); });
}

bool opClampValue(AutomationLane& lane, const Range& r, const Params& p) {
    float lo, hi; orderedBounds(p, lo, hi);
    return mapValues(lane, r, [lo, hi](float v) { return clampf(v, lo, hi); });
}

bool opNormalise(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i0 >= i1) return false;

    float vmin = 1.f, vmax = 0.f;
    for (size_t i = i0; i < i1; ++i) {
        vmin = std::min(vmin, lane.at((int)i).value);
        vmax = std::max(vmax, lane.at((int)i).value);
    }
    const float span = vmax - vmin;
    if (span < 1e-6f) return false;         // flat: nothing to normalise against

    float lo, hi; orderedBounds(p, lo, hi);
    const float a = finiteF(p.amount) ? p.amount : 1.f;
    return mapValues(lane, r, [=](float v) {
        return lerpf(v, lo + (v - vmin) / span * (hi - lo), a);
    });
}

// ===========================================================================
//  DENSITY OPS
// ===========================================================================

bool opSmooth(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    const size_t n = i1 - i0;
    if (n < 3) return false;                // nothing between the pinned ends
    const float a = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;
    if (a <= 0.f) return false;
    int radius = p.steps / 2;
    if (radius < 1) radius = 1;
    if ((size_t)radius > n) radius = (int)n;

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    const std::vector<Breakpoint> src = inside;     // average the ORIGINAL values

    // Ticks stay put, and the first/last point of the range keeps its exact
    // value: the smoothed span must not detach from the curve on either side.
    for (size_t i = 1; i + 1 < n; ++i) {
        const size_t lo = (i >= (size_t)radius) ? i - (size_t)radius : 0;
        const size_t hi = std::min(n - 1, i + (size_t)radius);
        double sum = 0.0;
        for (size_t k = lo; k <= hi; ++k) sum += (double)src[k].value;
        const float avg = (float)(sum / (double)(hi - lo + 1));
        inside[i].value = safeValue(lerpf(src[i].value, avg, a));
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opThin(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    const size_t n = i1 - i0;
    if (n < 3) return false;
    const float eps = finiteF(p.amount) ? std::fabs(p.amount) : 0.f;
    if (eps <= 0.f) return false;

    const std::vector<Breakpoint> src(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                      lane.breakpoints().begin() + (std::ptrdiff_t)i1);

    // Ramer-Douglas-Peucker. A neighbour-by-neighbour test would happily delete
    // a slow ramp one point at a time and drift arbitrarily far from the
    // original; RDP measures every candidate against the chord that would
    // actually replace it, so `eps` is a real error bound on the whole curve.
    // Distance is vertical (value error at the point's own tick), which is the
    // right metric here because ticks and values are not commensurable.
    std::vector<char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t> > stack;
    stack.push_back(std::make_pair((size_t)0, n - 1));
    while (!stack.empty()) {
        const size_t a = stack.back().first, b = stack.back().second;
        stack.pop_back();
        if (b <= a + 1) continue;
        const double t0 = (double)src[a].tick, t1 = (double)src[b].tick;
        const double v0 = (double)src[a].value, v1 = (double)src[b].value;
        const double dt = t1 - t0;
        double best = -1.0; size_t bestI = a;
        for (size_t i = a + 1; i < b; ++i) {
            const double predicted = (dt > 0.0)
                ? v0 + (v1 - v0) * (((double)src[i].tick - t0) / dt)
                : v0;
            const double d = std::fabs((double)src[i].value - predicted);
            if (d > best) { best = d; bestI = i; }
        }
        if (best > (double)eps) {
            keep[bestI] = 1;
            stack.push_back(std::make_pair(a, bestI));
            stack.push_back(std::make_pair(bestI, b));
        }
    }

    std::vector<Breakpoint> inside;
    inside.reserve(n);
    for (size_t i = 0; i < n; ++i) if (keep[i]) inside.push_back(src[i]);
    if (inside.size() == n) return false;
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opDensify(AutomationLane& lane, const Range& r, const Params& p) {
    if (r.length() <= 0 || lane.empty()) return false;
    // Densify RESAMPLES an existing curve; it is not a fill. A range that sits
    // past either end of the lane's data has nothing to resample, and padding
    // it with flat points there would be a surprise, not a service.
    if (r.end <= lane.at(0).tick || r.begin >= lane.at(lane.size() - 1).tick)
        return false;
    const std::vector<int64_t> ticks = genTicks(r, p);
    if (ticks.empty()) return false;

    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    // Sample the EXISTING curve so densifying is shape-preserving; original
    // points are kept verbatim and win any tick collision (first-wins ordering
    // in mergeCommit does that for us because they are appended first).
    const size_t originals = inside.size();
    for (int64_t t : ticks)
        inside.push_back(Breakpoint{ t, lane.value_at(t), 0.f });
    if (inside.size() == originals) return false;
    std::stable_sort(inside.begin(), inside.end(), byTick);
    return mergeCommit(lane, i0, i1, std::move(inside));
}

// ===========================================================================
//  GENERATORS
// ===========================================================================

//! Shared skeleton for the four fills: build the tick grid, ask `gen` for a
//! normalised 0..1 value per point, and REPLACE the range's breakpoints.
template <typename G>
bool fillRange(AutomationLane& lane, const Range& r, const Params& p, G gen) {
    const std::vector<int64_t> ticks = genTicks(r, p);
    if (ticks.empty()) return false;
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);

    std::vector<Breakpoint> inside;
    inside.reserve(ticks.size());
    // Normalise over the span the points ACTUALLY cover, not over the range
    // length. The last generated tick is end-1 (or the last grid line before
    // end), so dividing by the range length would stop a ramp fill just short
    // of `hi` and leave an LFO's final cycle visibly clipped.
    const int64_t first = ticks.front(), last = ticks.back();
    const double span = (double)(last - first);
    for (size_t i = 0; i < ticks.size(); ++i) {
        const double f = span > 0.0 ? (double)(ticks[i] - first) / span : 0.0;
        inside.push_back(Breakpoint{ ticks[i], safeValue(gen(f, (int)i)), 0.f });
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opLfoFill(AutomationLane& lane, const Range& r, const Params& p) {
    float lo, hi; orderedBounds(p, lo, hi);
    const float mid = 0.5f * (lo + hi), half = 0.5f * (hi - lo);
    const float amt = finiteF(p.amount) ? p.amount : 1.f;
    const float ph0 = finiteF(p.phase) ? p.phase : 0.f;

    double cycles;
    if (p.syncCycles > 0) cycles = (double)p.syncCycles;
    else {
        const double hz = finiteF(p.freqHz) ? (double)p.freqHz : 1.0;
        cycles = hz * ((double)r.length() / kTicksPerSecond);
    }
    if (!(cycles == cycles)) cycles = 1.0;

    const int shape = (p.shape < 0 || p.shape > 5) ? 0 : p.shape;
    const uint32_t seed = p.seed;
    return fillRange(lane, r, p, [=](double f, int) {
        return mid + half * amt * waveform(shape, (double)ph0 + cycles * f, seed);
    });
}

bool opRampFill(AutomationLane& lane, const Range& r, const Params& p) {
    // Direction comes from the lo/hi order the caller gave, NOT from a sorted
    // pair -- hi < lo is how you ask for a falling ramp.
    const float lo = finiteF(p.lo) ? clamp01(p.lo) : 0.f;
    const float hi = finiteF(p.hi) ? clamp01(p.hi) : 1.f;
    const float amt = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;
    const float mid = 0.5f * (lo + hi);
    return fillRange(lane, r, p, [=](double f, int) {
        const float v = lerpf(lo, hi, (float)f);
        return lerpf(mid, v, amt);          // amount = depth about the midpoint
    });
}

bool opSCurveFill(AutomationLane& lane, const Range& r, const Params& p) {
    const float lo = finiteF(p.lo) ? clamp01(p.lo) : 0.f;
    const float hi = finiteF(p.hi) ? clamp01(p.hi) : 1.f;
    const float amt = finiteF(p.amount) ? clampf(p.amount, 0.f, 4.f) : 1.f;
    // Symmetric logistic-style S: e == 1 is a straight line, larger e tightens
    // the knee toward a step. Chosen over smoothstep because the steepness is
    // continuously adjustable from the same 0..1 amount control.
    const double e = 1.0 + (double)amt * 4.0;
    return fillRange(lane, r, p, [=](double f, int) {
        const double a = std::pow(f, e), b = std::pow(1.0 - f, e);
        const double denom = a + b;
        const double s = denom > 1e-12 ? a / denom : f;
        return lerpf(lo, hi, (float)s);
    });
}

bool opNoiseFill(AutomationLane& lane, const Range& r, const Params& p) {
    float lo, hi; orderedBounds(p, lo, hi);
    const float mid = 0.5f * (lo + hi), half = 0.5f * (hi - lo);
    const float amt = finiteF(p.amount) ? p.amount : 1.f;
    const uint32_t seed = p.seed;
    return fillRange(lane, r, p, [=](double, int i) {
        return mid + half * amt * randBi(seed, (uint32_t)i);
    });
}

bool opBendCurve(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    if (i0 >= i1) return false;
    if (!finiteF(p.amount)) return false;
    // The lane already models per-segment bend (Breakpoint::curve), so bending
    // is non-destructive: no points are added and the data survives an undo of
    // the shape rather than of a resample. amount 0..1 spans the bipolar range,
    // 0.5 == straight.
    const float c = clampf(p.amount * 2.f - 1.f, -1.f, 1.f);

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    for (auto& b : inside) b.curve = c;
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool opStepQuantise(AutomationLane& lane, const Range& r, const Params& p) {
    const int levels = p.steps < 2 ? 2 : p.steps;
    float lo, hi; orderedBounds(p, lo, hi);
    const float span = hi - lo;
    if (span < 1e-6f) return false;
    const float a = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;
    if (a <= 0.f) return false;
    const float stepv = span / (float)(levels - 1);
    return mapValues(lane, r, [=](float v) {
        const float q = lo + std::floor((clampf(v, lo, hi) - lo) / stepv + 0.5f) * stepv;
        return lerpf(v, q, a);
    });
}

bool opRandomWalk(AutomationLane& lane, const Range& r, const Params& p) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    const size_t n = i1 - i0;
    if (n < 2) return false;
    const float amt = finiteF(p.amount) ? std::fabs(p.amount) : 1.f;
    if (amt <= 0.f) return false;

    std::vector<Breakpoint> inside(lane.breakpoints().begin() + (std::ptrdiff_t)i0,
                                   lane.breakpoints().begin() + (std::ptrdiff_t)i1);
    // Per-step size falls off as 1/sqrt(n) so the walk's total excursion is
    // about `amount` of the value range whatever the point density -- a dense
    // lane and a sparse one get the same musical amount of drift.
    const float stepMag = amt / (float)std::sqrt((double)(n - 1));
    float v = inside[0].value;              // start from where the curve already is
    for (size_t i = 1; i < n; ++i) {
        v = clamp01(v + randBi(p.seed, (uint32_t)i) * stepMag);
        inside[i].value = v;
    }
    return mergeCommit(lane, i0, i1, std::move(inside));
}

// ===========================================================================
//  MULTI-LANE OPS
// ===========================================================================

//! Replace one lane's in-range points with (tick, value) pairs.
bool writeShape(AutomationLane& lane, const Range& r,
                const std::vector<int64_t>& ticks, const std::vector<float>& values) {
    size_t i0, i1; rangeIndices(lane.breakpoints(), r, i0, i1);
    std::vector<Breakpoint> inside;
    inside.reserve(ticks.size());
    for (size_t i = 0; i < ticks.size(); ++i)
        inside.push_back(Breakpoint{ ticks[i], safeValue(values[i]), 0.f });
    return mergeCommit(lane, i0, i1, std::move(inside));
}

bool multiCopyShape(const std::vector<AutomationLane*>& v, const Range& r,
                    const Params& p, bool mirror) {
    if (v.size() < 2) {
        // A single lane has no source to copy FROM, but "mirror this lane" is
        // still a meaningful in-place request, so honour that one.
        if (!mirror || v.empty() || v[0] == nullptr) return false;
        const float a = finiteF(p.amount) ? p.amount : 1.f;
        const float pv = finiteF(p.pivot) ? p.pivot : 0.5f;
        return mapValues(*v[0], r, [a, pv](float x) {
            return lerpf(x, 2.f * pv - x, a);
        });
    }
    if (v[0] == nullptr || v[0]->empty()) return false;   // nothing to copy FROM
    const std::vector<int64_t> ticks = genTicks(r, p);
    if (ticks.empty()) return false;
    const float a  = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;
    const float pv = finiteF(p.pivot) ? p.pivot : 0.5f;

    std::vector<float> src(ticks.size());
    for (size_t i = 0; i < ticks.size(); ++i) {
        float s = v[0]->value_at(ticks[i]);
        if (mirror) s = 2.f * pv - s;
        src[i] = s;
    }
    bool any = false;
    std::vector<float> vals(ticks.size());
    for (size_t li = 1; li < v.size(); ++li) {
        if (v[li] == nullptr) continue;
        for (size_t i = 0; i < ticks.size(); ++i)
            vals[i] = lerpf(v[li]->value_at(ticks[i]), src[i], a);
        any |= writeShape(*v[li], r, ticks, vals);
    }
    return any;
}

bool multiPhaseOffset(const std::vector<AutomationLane*>& v, const Range& r,
                      const Params& p) {
    if (v.size() < 2 || v[0] == nullptr || v[0]->empty()) return false;
    const int64_t len = r.length();
    if (len <= 0) return false;
    const std::vector<int64_t> ticks = genTicks(r, p);
    if (ticks.empty()) return false;
    const float a  = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;
    const float ph = finiteF(p.phase) ? p.phase : 0.f;

    bool any = false;
    std::vector<float> vals(ticks.size());
    for (size_t li = 1; li < v.size(); ++li) {
        if (v[li] == nullptr) continue;
        // Lane i reads the source shape (i * phase) of the range later, wrapped
        // inside the range -- so a stack of lanes becomes a phased chase.
        const int64_t off = (int64_t)std::llround((double)ph * (double)li * (double)len);
        for (size_t i = 0; i < ticks.size(); ++i) {
            int64_t rel = (ticks[i] - r.begin + off) % len;
            if (rel < 0) rel += len;
            vals[i] = lerpf(v[li]->value_at(ticks[i]), v[0]->value_at(r.begin + rel), a);
        }
        any |= writeShape(*v[li], r, ticks, vals);
    }
    return any;
}

bool multiAverage(const std::vector<AutomationLane*>& v, const Range& r,
                  const Params& p) {
    // Only lanes that actually carry a curve contribute to the mean; an empty
    // lane is "no opinion", not "a flat zero that drags the average down".
    size_t live = 0;
    for (AutomationLane* l : v) if (l && !l->empty()) ++live;
    if (live < 2) return false;
    const std::vector<int64_t> ticks = genTicks(r, p);
    if (ticks.empty()) return false;
    const float a = finiteF(p.amount) ? clampf(p.amount, 0.f, 1.f) : 1.f;

    std::vector<float> mean(ticks.size(), 0.f);
    for (AutomationLane* l : v) {
        if (!l || l->empty()) continue;
        for (size_t i = 0; i < ticks.size(); ++i) mean[i] += l->value_at(ticks[i]);
    }
    for (float& m : mean) m /= (float)live;

    bool any = false;
    std::vector<float> vals(ticks.size());
    for (AutomationLane* l : v) {
        if (!l) continue;
        for (size_t i = 0; i < ticks.size(); ++i)
            vals[i] = lerpf(l->empty() ? mean[i] : l->value_at(ticks[i]), mean[i], a);
        any |= writeShape(*l, r, ticks, vals);
    }
    return any;
}

} // namespace

// ===========================================================================
//  public entry points
// ===========================================================================

const char* opName(Op op) {
    switch (op) {
        case Quantise:         return "Quantise";
        case Humanise:         return "Humanise";
        case Stretch:          return "Stretch";
        case Reverse:          return "Reverse";
        case ShiftTime:        return "Shift Time";
        case DuplicateRange:   return "Duplicate Range";
        case InsertTime:       return "Insert Time";
        case DeleteTime:       return "Delete Time";
        case ScaleValue:       return "Scale Value";
        case OffsetValue:      return "Offset Value";
        case InvertValue:      return "Invert Value";
        case Normalise:        return "Normalise";
        case ClampValue:       return "Clamp Value";
        case Smooth:           return "Smooth";
        case Thin:             return "Thin";
        case Densify:          return "Densify";
        case LfoFill:          return "LFO Fill";
        case RampFill:         return "Ramp Fill";
        case SCurveFill:       return "S-Curve Fill";
        case BendCurve:        return "Bend Curve";
        case StepQuantise:     return "Step Quantise";
        case RandomWalk:       return "Random Walk";
        case NoiseFill:        return "Noise Fill";
        case CopyShape:        return "Copy Shape";
        case MirrorLanes:      return "Mirror Lanes";
        case PhaseOffsetLanes: return "Phase Offset Lanes";
        case AverageLanes:     return "Average Lanes";
        case OP_COUNT:
        default:               return "?";
    }
}

bool opIsMultiLane(Op op) {
    return op >= CopyShape && op <= AverageLanes;
}

bool opIsRipple(Op op) {
    return op == InsertTime || op == DeleteTime || op == DuplicateRange;
}

bool apply(AutomationLane& lane, const Range& r, Op op, const Params& p) {
    if ((int)op < 0 || (int)op >= (int)OP_COUNT) return false;
    if (opIsMultiLane(op)) {
        std::vector<AutomationLane*> one(1, &lane);
        return applyMulti(one, r, op, p);
    }
    // InsertTime is the one op with something to do on an empty range: it can
    // open `grid` ticks. Everything else needs a real span or real points.
    if (r.empty() && op != InsertTime) return false;
    if (lane.empty() && op != LfoFill && op != RampFill &&
        op != SCurveFill && op != NoiseFill) return false;

    switch (op) {
        case Quantise:       return opQuantise(lane, r, p);
        case Humanise:       return opHumanise(lane, r, p);
        case Stretch:        return opStretch(lane, r, p);
        case Reverse:        return opReverse(lane, r, p);
        case ShiftTime:      return opShiftTime(lane, r, p);
        case DuplicateRange: return opDuplicateRange(lane, r, p);
        case InsertTime:     return opInsertTime(lane, r, p);
        case DeleteTime:     return opDeleteTime(lane, r, p);
        case ScaleValue:     return opScaleValue(lane, r, p);
        case OffsetValue:    return opOffsetValue(lane, r, p);
        case InvertValue:    return opInvertValue(lane, r, p);
        case Normalise:      return opNormalise(lane, r, p);
        case ClampValue:     return opClampValue(lane, r, p);
        case Smooth:         return opSmooth(lane, r, p);
        case Thin:           return opThin(lane, r, p);
        case Densify:        return opDensify(lane, r, p);
        case LfoFill:        return opLfoFill(lane, r, p);
        case RampFill:       return opRampFill(lane, r, p);
        case SCurveFill:     return opSCurveFill(lane, r, p);
        case BendCurve:      return opBendCurve(lane, r, p);
        case StepQuantise:   return opStepQuantise(lane, r, p);
        case RandomWalk:     return opRandomWalk(lane, r, p);
        case NoiseFill:      return opNoiseFill(lane, r, p);
        default:             return false;
    }
}

bool applyMulti(const std::vector<AutomationLane*>& lanes, const Range& r,
                Op op, const Params& p) {
    if ((int)op < 0 || (int)op >= (int)OP_COUNT || lanes.empty()) return false;

    if (!opIsMultiLane(op)) {
        bool any = false;
        for (AutomationLane* l : lanes)
            if (l) any |= apply(*l, r, op, p);
        return any;
    }
    if (r.empty()) return false;

    switch (op) {
        case CopyShape:        return multiCopyShape(lanes, r, p, false);
        case MirrorLanes:      return multiCopyShape(lanes, r, p, true);
        case PhaseOffsetLanes: return multiPhaseOffset(lanes, r, p);
        case AverageLanes:     return multiAverage(lanes, r, p);
        default:               return false;
    }
}

}}} // namespace PatchKnob::engine::autoops
