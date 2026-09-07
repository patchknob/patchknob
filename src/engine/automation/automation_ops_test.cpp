//----------------------------------------------------------------------------
//  PatchKnob — autoops (automation range-process) self-test.
//
//  Two halves:
//
//  [A] A CROSS PRODUCT SWEEP. Every op is run against every lane fixture,
//      every range and every parameter set -- including deliberately hostile
//      ones (NaN amount, inf pivot, zero/negative steps, swapped lo/hi, grid 0,
//      ranges that miss the data entirely, zero-length ranges). After each of
//      the several thousand runs the lane is re-checked for the whole contract:
//          * strictly ascending ticks (sorted, no duplicates),
//          * every value finite and inside 0..1, every curve inside -1..1,
//          * ticks non-negative,
//          * every breakpoint that was OUTSIDE the range is still present and
//            byte-identical (skipped only for the three ripple ops, which move
//            later material by design),
//          * running the same op twice on identical copies gives identical
//            results (determinism, including the seeded ops).
//
//  [B] BEHAVIOUR SPOT CHECKS. That the sweep passes proves nothing was
//      corrupted; these prove each op actually does its job -- ripple arithmetic,
//      RDP thinning keeping a spike while collapsing a straight line, Smooth
//      pinning the range endpoints, seed sensitivity, and so on.
//----------------------------------------------------------------------------
#include "automation_lane.h"
#include "automation_ops.h"
#include "automation_track.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace PatchKnob::engine;
namespace ops = PatchKnob::engine::autoops;

static int gFail = 0;
static int gChecks = 0;

#define CHECK(cond, msg) do {                                     \
    ++gChecks;                                                    \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++gFail; }   \
} while (0)

#define CHECK_V(cond, fmt, ...) do {                              \
    ++gChecks;                                                    \
    if (!(cond)) { std::printf("  FAIL: " fmt "\n", __VA_ARGS__); ++gFail; } \
} while (0)

static bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------------------
//  invariant checking
// ---------------------------------------------------------------------------

static bool bpSame(const Breakpoint& a, const Breakpoint& b) {
    return a.tick == b.tick && a.value == b.value && a.curve == b.curve;
}

//! The whole structural contract in one place.
static bool laneWellFormed(const AutomationLane& l, std::string& why) {
    const std::vector<Breakpoint>& b = l.breakpoints();
    for (size_t i = 0; i < b.size(); ++i) {
        if (b[i].tick < 0)                     { why = "negative tick"; return false; }
        if (i && b[i].tick <= b[i - 1].tick)   { why = "unsorted or duplicate tick"; return false; }
        const float v = b[i].value;
        if (!(v == v))                         { why = "NaN value"; return false; }
        if (v < 0.f || v > 1.f)                { why = "value outside 0..1"; return false; }
        const float c = b[i].curve;
        if (!(c == c))                         { why = "NaN curve"; return false; }
        if (c < -1.f || c > 1.f)               { why = "curve outside -1..1"; return false; }
    }
    return true;
}

//! Every breakpoint that started outside the range must survive untouched.
static bool outsidePreserved(const std::vector<Breakpoint>& before,
                             const AutomationLane& after, const ops::Range& r) {
    const std::vector<Breakpoint>& a = after.breakpoints();
    for (const Breakpoint& o : before) {
        if (r.contains(o.tick)) continue;
        bool found = false;
        for (const Breakpoint& n : a) if (bpSame(o, n)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

static bool sameLane(const AutomationLane& a, const AutomationLane& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) if (!bpSame(a.at(i), b.at(i))) return false;
    return true;
}

// ---------------------------------------------------------------------------
//  fixtures
// ---------------------------------------------------------------------------

struct Fixture { const char* name; AutomationLane lane; };

static AutomationLane makeLane(const std::vector<std::pair<int64_t, float> >& pts) {
    AutomationLane l = AutomationLane::paramLane(7);
    for (const auto& p : pts) l.add(p.first, p.second);
    return l;
}

static std::vector<Fixture> makeFixtures() {
    std::vector<Fixture> f;

    f.push_back({ "empty", AutomationLane::paramLane(7) });
    f.push_back({ "single", makeLane({ { 200, 0.5f } }) });
    f.push_back({ "pair", makeLane({ { 100, 0.f }, { 500, 1.f } }) });

    {   // dense ramp with points on both sides of the usual test range
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 49; ++i)
            pts.push_back({ (int64_t)i * 20, (float)i / 48.f });
        f.push_back({ "ramp49", makeLane(pts) });
    }
    {   // straight line -- RDP must be able to collapse this
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 33; ++i)
            pts.push_back({ 192 + (int64_t)i * 12, 0.25f + (float)i * (0.5f / 32.f) });
        f.push_back({ "line33", makeLane(pts) });
    }
    {   // values pinned at the 0 and 1 rails, so clamping bugs show up
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 24; ++i)
            pts.push_back({ (int64_t)i * 37, (i & 1) ? 1.f : 0.f });
        f.push_back({ "square24", makeLane(pts) });
    }
    {   // two points only, both inside the middle range
        f.push_back({ "twoInside", makeLane({ { 200, 0.2f }, { 300, 0.8f } }) });
    }
    {   // exactly one point inside the middle range, others outside
        f.push_back({ "oneInside", makeLane({ { 10, 0.1f }, { 250, 0.9f }, { 900, 0.3f } }) });
    }
    {   // curves set, so curve handling is exercised too
        AutomationLane l = makeLane({ { 100, 0.1f }, { 220, 0.7f }, { 340, 0.2f },
                                      { 460, 0.9f }, { 700, 0.4f } });
        l.setCurveAfter(0,  0.5f);
        l.setCurveAfter(1, -0.8f);
        l.setCurveAfter(3,  1.0f);
        f.push_back({ "curved", l });
    }
    return f;
}

static std::vector<std::pair<const char*, ops::Range> > makeRanges() {
    return {
        { "zeroLen",      { 0, 0 } },
        { "zeroLenMid",   { 300, 300 } },
        { "inverted",     { 500, 100 } },
        { "middle",       { 192, 576 } },
        { "wholeish",     { 0, 1000 } },
        { "beyondData",   { 10000, 20000 } },
        { "beforeData",   { 0, 5 } },
        { "tiny",         { 200, 201 } },
        { "hugeSpan",     { 0, 1000000 } },
    };
}

static std::vector<std::pair<const char*, ops::Params> > makeParamSets() {
    std::vector<std::pair<const char*, ops::Params> > out;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    { ops::Params p;                                    out.push_back({ "default", p }); }
    { ops::Params p; p.amount = 0.f;                    out.push_back({ "amount0", p }); }
    { ops::Params p; p.amount = 0.35f;                  out.push_back({ "amount.35", p }); }
    { ops::Params p; p.amount = -2.f;                   out.push_back({ "amountNeg", p }); }
    { ops::Params p; p.amount = 12.f;                   out.push_back({ "amountBig", p }); }
    { ops::Params p; p.amount = nan; p.pivot = inf;     out.push_back({ "nanInf", p }); }
    { ops::Params p; p.lo = nan; p.hi = nan;            out.push_back({ "nanBounds", p }); }
    { ops::Params p; p.grid = 48;                       out.push_back({ "grid48", p }); }
    { ops::Params p; p.grid = 1;                        out.push_back({ "grid1", p }); }
    { ops::Params p; p.grid = -32;                      out.push_back({ "gridNeg", p }); }
    { ops::Params p; p.steps = 0;                       out.push_back({ "steps0", p }); }
    { ops::Params p; p.steps = -5;                      out.push_back({ "stepsNeg", p }); }
    { ops::Params p; p.steps = 1;                       out.push_back({ "steps1", p }); }
    { ops::Params p; p.steps = 257;                     out.push_back({ "steps257", p }); }
    { ops::Params p; p.lo = 0.8f; p.hi = 0.2f;          out.push_back({ "swapped", p }); }
    { ops::Params p; p.lo = -3.f; p.hi = 9.f;           out.push_back({ "wildBounds", p }); }
    { ops::Params p; p.pivot = 0.f;                     out.push_back({ "pivot0", p }); }
    { ops::Params p; p.pivot = 1.f; p.amount = 2.f;     out.push_back({ "pivot1x2", p }); }
    { ops::Params p; p.syncCycles = 4; p.shape = 1;     out.push_back({ "sync4tri", p }); }
    { ops::Params p; p.shape = 5; p.freqHz = 7.f;       out.push_back({ "randShape", p }); }
    { ops::Params p; p.shape = 99;                      out.push_back({ "badShape", p }); }
    { ops::Params p; p.shape = -4; p.phase = 0.37f;     out.push_back({ "negShape", p }); }
    { ops::Params p; p.seed = 0;                        out.push_back({ "seed0", p }); }
    { ops::Params p; p.seed = 0xDEADBEEFu; p.phase=.5f; out.push_back({ "seedBig", p }); }
    { ops::Params p; p.freqHz = 0.f;                    out.push_back({ "freq0", p }); }
    { ops::Params p; p.freqHz = nan; p.phase = nan;     out.push_back({ "nanFreq", p }); }
    return out;
}

// ---------------------------------------------------------------------------
//  [A] sweep
// ---------------------------------------------------------------------------

static void sweep() {
    std::printf("[A] invariant sweep over every op x fixture x range x params\n");

    const std::vector<Fixture> fixtures = makeFixtures();
    const auto ranges = makeRanges();
    const auto paramSets = makeParamSets();

    int runs = 0, changed = 0;
    int badForm = 0, badOutside = 0, badDeterminism = 0;
    std::string firstWhy, firstWhere;

    for (int o = 0; o < ops::OP_COUNT; ++o) {
        const ops::Op op = (ops::Op)o;
        for (const Fixture& fx : fixtures) {
            for (const auto& rr : ranges) {
                for (const auto& pp : paramSets) {
                    AutomationLane a = fx.lane, b = fx.lane;
                    const std::vector<Breakpoint> before = fx.lane.breakpoints();

                    const bool ra = ops::apply(a, rr.second, op, pp.second);
                    const bool rb = ops::apply(b, rr.second, op, pp.second);
                    ++runs;
                    if (ra) ++changed;

                    char where[256];
                    std::snprintf(where, sizeof where, "%s / %s / %s / %s",
                                  ops::opName(op), fx.name, rr.first, pp.first);

                    std::string why;
                    if (!laneWellFormed(a, why)) {
                        if (!badForm) { firstWhy = why; firstWhere = where; }
                        ++badForm;
                    }
                    if (!ops::opIsRipple(op) &&
                        !outsidePreserved(before, a, rr.second)) {
                        if (!badOutside) firstWhere = where;
                        ++badOutside;
                    }
                    // Same input, same params -> same output and same verdict.
                    if (ra != rb || !sameLane(a, b)) {
                        if (!badDeterminism) firstWhere = where;
                        ++badDeterminism;
                    }
                    // apply() must report honestly: false means untouched.
                    if (!ra && !sameLane(a, fx.lane)) {
                        std::printf("  FAIL: %s returned false but mutated\n", where);
                        ++gFail;
                    }
                }
            }
        }
    }

    std::printf("      %d runs, %d of them changed the lane\n", runs, changed);
    CHECK_V(badForm == 0, "%d runs left a malformed lane (first: %s @ %s)",
            badForm, firstWhy.c_str(), firstWhere.c_str());
    CHECK_V(badOutside == 0, "%d runs disturbed a breakpoint outside the range (first: %s)",
            badOutside, firstWhere.c_str());
    CHECK_V(badDeterminism == 0, "%d runs were non-deterministic (first: %s)",
            badDeterminism, firstWhere.c_str());
    CHECK(changed > 0, "the sweep actually exercised real edits");

    // Multi-lane sweep: same contract, several lanes at once.
    int mBadForm = 0, mBadOutside = 0, mBadDet = 0, mRuns = 0, mChanged = 0;
    for (int o = 0; o < ops::OP_COUNT; ++o) {
        const ops::Op op = (ops::Op)o;
        for (const auto& rr : ranges) {
            for (const auto& pp : paramSets) {
                std::vector<AutomationLane> A, B;
                for (const Fixture& fx : fixtures) { A.push_back(fx.lane); B.push_back(fx.lane); }
                std::vector<AutomationLane*> pa, pb;
                for (auto& l : A) pa.push_back(&l);
                for (auto& l : B) pb.push_back(&l);
                pa.push_back(nullptr);                 // null entries must be tolerated
                pb.push_back(nullptr);

                const bool ra = ops::applyMulti(pa, rr.second, op, pp.second);
                const bool rb = ops::applyMulti(pb, rr.second, op, pp.second);
                ++mRuns; if (ra) ++mChanged;
                if (ra != rb) ++mBadDet;
                for (size_t i = 0; i < A.size(); ++i) {
                    std::string why;
                    if (!laneWellFormed(A[i], why)) ++mBadForm;
                    if (!ops::opIsRipple(op) &&
                        !outsidePreserved(fixtures[i].lane.breakpoints(), A[i], rr.second))
                        ++mBadOutside;
                    if (!sameLane(A[i], B[i])) ++mBadDet;
                }
            }
        }
    }
    std::printf("      %d multi-lane runs, %d changed something\n", mRuns, mChanged);
    CHECK_V(mBadForm == 0, "%d multi-lane results were malformed", mBadForm);
    CHECK_V(mBadOutside == 0, "%d multi-lane results disturbed outside points", mBadOutside);
    CHECK_V(mBadDet == 0, "%d multi-lane results were non-deterministic", mBadDet);
    CHECK(mChanged > 0, "the multi-lane sweep exercised real edits");
}

// ---------------------------------------------------------------------------
//  [B] behaviour
// ---------------------------------------------------------------------------

static void metadata() {
    std::printf("[B1] op metadata\n");
    for (int o = 0; o < ops::OP_COUNT; ++o) {
        const char* n = ops::opName((ops::Op)o);
        CHECK(n != nullptr && n[0] != '\0', "opName is a real string for every op");
        for (int q = 0; q < o; ++q)
            CHECK(std::strcmp(n, ops::opName((ops::Op)q)) != 0, "op names are unique");
    }
    CHECK(std::strcmp(ops::opName((ops::Op)9999), "?") == 0, "opName tolerates a bogus op");
    CHECK(ops::opIsMultiLane(ops::CopyShape) && ops::opIsMultiLane(ops::AverageLanes) &&
          ops::opIsMultiLane(ops::MirrorLanes) && ops::opIsMultiLane(ops::PhaseOffsetLanes),
          "the multi-lane group reports itself");
    CHECK(!ops::opIsMultiLane(ops::Quantise) && !ops::opIsMultiLane(ops::NoiseFill),
          "single-lane ops are not multi-lane");
    CHECK(ops::opIsRipple(ops::InsertTime) && ops::opIsRipple(ops::DeleteTime) &&
          ops::opIsRipple(ops::DuplicateRange) && !ops::opIsRipple(ops::Reverse),
          "the ripple group reports itself");

    AutomationLane l = makeLane({ { 0, 0.5f } });
    ops::Params p;
    CHECK(!ops::apply(l, ops::Range{ 0, 100 }, (ops::Op)9999, p), "bogus op is a no-op");
    CHECK(!ops::applyMulti({}, ops::Range{ 0, 100 }, ops::Quantise, p), "empty lane list is a no-op");
}

static void timeOps() {
    std::printf("[B2] time ops\n");
    ops::Params p;

    {   // Reverse mirrors about the selection's own first/last point.
        AutomationLane l = makeLane({ { 50, 0.9f }, { 100, 0.1f }, { 200, 0.4f },
                                      { 400, 0.7f }, { 900, 0.3f } });
        CHECK(ops::apply(l, ops::Range{ 100, 500 }, ops::Reverse, p), "Reverse reports a change");
        CHECK(l.size() == 5, "Reverse loses no points");
        CHECK(l.at(0).tick == 50 && approx(l.at(0).value, 0.9f), "Reverse leaves the point before alone");
        CHECK(l.at(4).tick == 900 && approx(l.at(4).value, 0.3f), "Reverse leaves the point after alone");
        CHECK(l.at(1).tick == 100 && approx(l.at(1).value, 0.7f), "Reverse: 400 -> 100");
        CHECK(l.at(2).tick == 300 && approx(l.at(2).value, 0.4f), "Reverse: 200 -> 300");
        CHECK(l.at(3).tick == 400 && approx(l.at(3).value, 0.1f), "Reverse: 100 -> 400");
    }
    {   // Reverse is an involution on ticks.
        AutomationLane l = makeLane({ { 10, 0.2f }, { 30, 0.4f }, { 70, 0.6f }, { 90, 0.8f } });
        const AutomationLane orig = l;
        ops::apply(l, ops::Range{ 0, 200 }, ops::Reverse, p);
        ops::apply(l, ops::Range{ 0, 200 }, ops::Reverse, p);
        bool ticksBack = l.size() == orig.size();
        for (int i = 0; ticksBack && i < l.size(); ++i)
            ticksBack = l.at(i).tick == orig.at(i).tick && approx(l.at(i).value, orig.at(i).value);
        CHECK(ticksBack, "Reverse twice restores the curve");
    }
    {   // Stretch scales about r.begin and never reorders.
        AutomationLane l = makeLane({ { 100, 0.1f }, { 200, 0.5f }, { 300, 0.9f } });
        ops::Params s; s.amount = 2.f;
        CHECK(ops::apply(l, ops::Range{ 100, 400 }, ops::Stretch, s), "Stretch reports a change");
        CHECK(l.size() == 3 && l.at(0).tick == 100 && l.at(1).tick == 300 && l.at(2).tick == 500,
              "Stretch x2 about r.begin");
        bool ordered = true;
        for (int i = 1; i < l.size(); ++i) if (l.at(i).tick <= l.at(i - 1).tick) ordered = false;
        CHECK(ordered, "Stretch keeps points ordered");
        CHECK(approx(l.at(2).value, 0.9f), "Stretch keeps values");
    }
    {   // Compressing hard fuses points rather than reordering them.
        AutomationLane l = makeLane({ { 0, 0.f }, { 10, .2f }, { 20, .4f }, { 30, .6f }, { 40, .8f } });
        ops::Params s; s.amount = 0.01f;
        ops::apply(l, ops::Range{ 0, 100 }, ops::Stretch, s);
        std::string why;
        CHECK(laneWellFormed(l, why), "Stretch to nothing still leaves a well-formed lane");
        CHECK(l.size() >= 1, "Stretch to nothing keeps at least one point");
    }
    {   // Quantise snaps onto the grid.
        AutomationLane l = makeLane({ { 55, 0.2f }, { 101, 0.4f }, { 190, 0.6f } });
        ops::Params q; q.grid = 48; q.amount = 1.f;
        CHECK(ops::apply(l, ops::Range{ 0, 500 }, ops::Quantise, q), "Quantise reports a change");
        bool onGrid = true;
        for (int i = 0; i < l.size(); ++i) if (l.at(i).tick % 48 != 0) onGrid = false;
        CHECK(onGrid, "Quantise puts every point on the grid at amount 1");
        ops::Params half = q; half.amount = 0.f;
        AutomationLane l2 = makeLane({ { 55, 0.2f } });
        CHECK(!ops::apply(l2, ops::Range{ 0, 500 }, ops::Quantise, half), "Quantise at amount 0 is a no-op");
    }
    {   // ShiftTime moves the selection, grid * amount.
        AutomationLane l = makeLane({ { 100, 0.3f }, { 200, 0.6f }, { 900, 0.9f } });
        ops::Params s; s.grid = 48; s.amount = 2.f;
        CHECK(ops::apply(l, ops::Range{ 0, 500 }, ops::ShiftTime, s), "ShiftTime reports a change");
        CHECK(l.at(0).tick == 196 && l.at(1).tick == 296, "ShiftTime +2 grids");
        CHECK(l.at(2).tick == 900, "ShiftTime leaves out-of-range points alone");
        ops::Params back = s; back.amount = -2.f;
        ops::apply(l, ops::Range{ 0, 500 }, ops::ShiftTime, back);
        CHECK(l.at(0).tick == 100 && l.at(1).tick == 200, "ShiftTime accepts a negative amount");
    }
    {   // A shift far into negative territory clamps at 0 instead of crashing.
        AutomationLane l = makeLane({ { 10, 0.3f }, { 20, 0.6f } });
        ops::Params s; s.grid = 1000; s.amount = -100.f;
        ops::apply(l, ops::Range{ 0, 100 }, ops::ShiftTime, s);
        std::string why;
        CHECK(laneWellFormed(l, why), "a huge negative shift stays well-formed");
        CHECK(l.size() >= 1 && l.at(0).tick == 0, "a huge negative shift clamps at tick 0");
    }
    {   // Humanise: deterministic, in-range, order preserved.
        AutomationLane base = makeLane({ { 200, .2f }, { 240, .4f }, { 280, .6f }, { 320, .8f } });
        ops::Params h; h.grid = 24; h.amount = 1.f; h.seed = 12345;
        AutomationLane a = base, b = base, c = base;
        ops::apply(a, ops::Range{ 192, 576 }, ops::Humanise, h);
        ops::apply(b, ops::Range{ 192, 576 }, ops::Humanise, h);
        ops::Params h2 = h; h2.seed = 999;
        ops::apply(c, ops::Range{ 192, 576 }, ops::Humanise, h2);
        CHECK(sameLane(a, b), "Humanise is reproducible from the same seed");
        CHECK(!sameLane(a, c), "Humanise follows the seed");
        CHECK(a.size() == base.size(), "Humanise never fuses points");
        bool inRange = true;
        for (int i = 0; i < a.size(); ++i)
            if (a.at(i).tick < 192 || a.at(i).tick >= 576) inRange = false;
        CHECK(inRange, "Humanise keeps points inside the range");
    }
}

static void rippleOps() {
    std::printf("[B3] ripple ops\n");
    ops::Params p;
    {   // InsertTime pushes everything from r.begin on.
        AutomationLane l = makeLane({ { 0, .1f }, { 100, .2f }, { 200, .3f } });
        CHECK(ops::apply(l, ops::Range{ 50, 150 }, ops::InsertTime, p), "InsertTime reports a change");
        CHECK(l.size() == 3 && l.at(0).tick == 0 && l.at(1).tick == 200 && l.at(2).tick == 300,
              "InsertTime ripples every later point by the range length");
        CHECK(approx(l.at(1).value, .2f), "InsertTime preserves values");
    }
    {   // DeleteTime closes the gap.
        AutomationLane l = makeLane({ { 0, .1f }, { 100, .2f }, { 200, .3f }, { 300, .4f } });
        CHECK(ops::apply(l, ops::Range{ 100, 300 }, ops::DeleteTime, p), "DeleteTime reports a change");
        CHECK(l.size() == 2 && l.at(0).tick == 0 && l.at(1).tick == 100,
              "DeleteTime drops the range and pulls later points back");
        CHECK(approx(l.at(1).value, .4f), "DeleteTime keeps the surviving point's value");
    }
    {   // Insert then delete the same span is a round trip.
        AutomationLane orig = makeLane({ { 0, .1f }, { 100, .2f }, { 640, .9f } });
        AutomationLane l = orig;
        ops::apply(l, ops::Range{ 50, 250 }, ops::InsertTime, p);
        ops::apply(l, ops::Range{ 50, 250 }, ops::DeleteTime, p);
        CHECK(sameLane(l, orig), "InsertTime then DeleteTime is a round trip");
    }
    {   // DuplicateRange copies right and ripples what follows.
        AutomationLane l = makeLane({ { 0, .1f }, { 100, .2f }, { 200, .3f } });
        CHECK(ops::apply(l, ops::Range{ 0, 200 }, ops::DuplicateRange, p), "DuplicateRange reports a change");
        CHECK(l.size() == 5, "DuplicateRange adds the copied points");
        CHECK(l.at(0).tick == 0 && l.at(1).tick == 100 && l.at(2).tick == 200 &&
              l.at(3).tick == 300 && l.at(4).tick == 400,
              "DuplicateRange copies to [end,end+len) and ripples the rest");
        CHECK(approx(l.at(2).value, .1f) && approx(l.at(3).value, .2f), "the copy carries the values");
        CHECK(approx(l.at(4).value, .3f), "the rippled original keeps its value");
    }
}

static void valueOps() {
    std::printf("[B4] value ops\n");
    {   // ScaleValue about the pivot.
        AutomationLane l = makeLane({ { 0, 0.f }, { 100, 1.f }, { 200, 0.5f } });
        ops::Params p; p.amount = 0.5f; p.pivot = 0.5f;
        CHECK(ops::apply(l, ops::Range{ 0, 300 }, ops::ScaleValue, p), "ScaleValue reports a change");
        CHECK(approx(l.at(0).value, 0.25f) && approx(l.at(1).value, 0.75f) &&
              approx(l.at(2).value, 0.5f), "ScaleValue halves the distance to the pivot");
    }
    {   // OffsetValue is a signed shift and clamps at the rails.
        AutomationLane l = makeLane({ { 0, 0.2f }, { 100, 0.9f } });
        ops::Params p; p.amount = 0.3f;
        ops::apply(l, ops::Range{ 0, 300 }, ops::OffsetValue, p);
        CHECK(approx(l.at(0).value, 0.5f) && approx(l.at(1).value, 1.0f),
              "OffsetValue adds and clamps to 1");
        ops::Params n; n.amount = -5.f;
        ops::apply(l, ops::Range{ 0, 300 }, ops::OffsetValue, n);
        CHECK(approx(l.at(0).value, 0.f) && approx(l.at(1).value, 0.f),
              "a huge negative offset clamps to 0, not to NaN");
    }
    {   // InvertValue about the pivot.
        AutomationLane l = makeLane({ { 0, 0.25f }, { 100, 1.f } });
        ops::Params p;                                    // amount 1, pivot .5
        ops::apply(l, ops::Range{ 0, 300 }, ops::InvertValue, p);
        CHECK(approx(l.at(0).value, 0.75f) && approx(l.at(1).value, 0.f),
              "InvertValue mirrors about the pivot");
    }
    {   // Normalise stretches the range's own extremes onto lo..hi.
        AutomationLane l = makeLane({ { 0, 0.4f }, { 100, 0.5f }, { 200, 0.6f }, { 900, 0.05f } });
        ops::Params p; p.lo = 0.f; p.hi = 1.f;
        CHECK(ops::apply(l, ops::Range{ 0, 300 }, ops::Normalise, p), "Normalise reports a change");
        CHECK(approx(l.at(0).value, 0.f) && approx(l.at(2).value, 1.f) &&
              approx(l.at(1).value, 0.5f), "Normalise maps min->lo and max->hi");
        CHECK(approx(l.at(3).value, 0.05f), "Normalise ignores points outside the range");

        AutomationLane flat = makeLane({ { 0, 0.3f }, { 100, 0.3f } });
        CHECK(!ops::apply(flat, ops::Range{ 0, 300 }, ops::Normalise, p),
              "Normalise refuses a flat range instead of dividing by zero");
    }
    {   // ClampValue with the bounds handed over backwards.
        AutomationLane l = makeLane({ { 0, 0.f }, { 100, 0.5f }, { 200, 1.f } });
        ops::Params p; p.lo = 0.8f; p.hi = 0.2f;          // swapped on purpose
        ops::apply(l, ops::Range{ 0, 300 }, ops::ClampValue, p);
        bool ok = true;
        for (int i = 0; i < l.size(); ++i)
            if (l.at(i).value < 0.2f - 1e-5f || l.at(i).value > 0.8f + 1e-5f) ok = false;
        CHECK(ok, "ClampValue orders swapped bounds instead of collapsing them");
    }
    {   // StepQuantise lands on discrete levels.
        AutomationLane l = makeLane({ { 0, 0.13f }, { 50, 0.42f }, { 100, 0.77f }, { 150, 0.98f } });
        ops::Params p; p.steps = 5; p.lo = 0.f; p.hi = 1.f;   // levels at 0,.25,.5,.75,1
        CHECK(ops::apply(l, ops::Range{ 0, 300 }, ops::StepQuantise, p), "StepQuantise reports a change");
        bool onLevel = true;
        for (int i = 0; i < l.size(); ++i) {
            const float v = l.at(i).value * 4.f;
            if (!approx(v, std::floor(v + 0.5f), 1e-3f)) onLevel = false;
        }
        CHECK(onLevel, "StepQuantise snaps values to `steps` levels");
    }
}

static void densityOps() {
    std::printf("[B5] density ops\n");
    {   // Smooth: ticks fixed, endpoints pinned, interior pulled toward the mean.
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 21; ++i) pts.push_back({ (int64_t)i * 10, (i & 1) ? 1.f : 0.f });
        AutomationLane l = makeLane(pts);
        const AutomationLane orig = l;
        ops::Params p; p.steps = 4; p.amount = 1.f;
        CHECK(ops::apply(l, ops::Range{ 0, 300 }, ops::Smooth, p), "Smooth reports a change");
        CHECK(l.size() == orig.size(), "Smooth adds and removes nothing");
        bool ticksFixed = true;
        for (int i = 0; i < l.size(); ++i) if (l.at(i).tick != orig.at(i).tick) ticksFixed = false;
        CHECK(ticksFixed, "Smooth leaves every tick where it was");
        CHECK(approx(l.at(0).value, orig.at(0).value), "Smooth pins the first point of the range");
        CHECK(approx(l.at(l.size() - 1).value, orig.at(orig.size() - 1).value),
              "Smooth pins the last point of the range");
        // interior alternation must have collapsed toward the middle
        bool flattened = true;
        for (int i = 3; i + 3 < l.size(); ++i)
            if (l.at(i).value < 0.2f || l.at(i).value > 0.8f) flattened = false;
        CHECK(flattened, "Smooth actually averages the interior");
    }
    {   // Smooth keeps the smoothed span attached to its surroundings.
        AutomationLane l = makeLane({ { 0, 0.f }, { 100, 1.f }, { 110, 0.f }, { 120, 1.f },
                                      { 130, 0.f }, { 200, 1.f } });
        const float before0 = l.at(0).value, beforeN = l.at(5).value;
        ops::Params p; p.steps = 6;
        ops::apply(l, ops::Range{ 90, 140 }, ops::Smooth, p);
        CHECK(approx(l.at(0).value, before0) && approx(l.at(5).value, beforeN),
              "Smooth does not touch points outside the range");
    }
    {   // Thin: RDP collapses a straight line to its endpoints...
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 41; ++i)
            pts.push_back({ (int64_t)i * 10, (float)i / 40.f });
        AutomationLane l = makeLane(pts);
        ops::Params p; p.amount = 0.01f;
        CHECK(ops::apply(l, ops::Range{ 0, 1000 }, ops::Thin, p), "Thin reports a change");
        CHECK(l.size() == 2, "Thin collapses a perfectly straight ramp to 2 points");
        CHECK(l.at(0).tick == 0 && l.at(1).tick == 400, "Thin keeps the endpoints");
    }
    {   // ...but never a feature that exceeds the tolerance.
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 41; ++i)
            pts.push_back({ (int64_t)i * 10, (i == 20) ? 1.f : 0.f });
        AutomationLane l = makeLane(pts);
        ops::Params p; p.amount = 0.05f;
        ops::apply(l, ops::Range{ 0, 1000 }, ops::Thin, p);
        bool spikeKept = false;
        for (int i = 0; i < l.size(); ++i)
            if (l.at(i).tick == 200 && approx(l.at(i).value, 1.f)) spikeKept = true;
        CHECK(spikeKept, "Thin keeps a spike that the chord does not predict");
        // RDP also has to keep the two points at the FOOT of the spike: the
        // chord from the flat run up to the peak does not predict them either.
        // Endpoints + foot + peak + foot == 5, and nothing else survives.
        CHECK(l.size() == 5, "Thin keeps the endpoints, the spike and its two feet");
        CHECK(l.at(0).tick == 0 && l.at(1).tick == 190 && l.at(2).tick == 200 &&
              l.at(3).tick == 210 && l.at(4).tick == 400,
              "Thin keeps exactly the points the chords cannot predict");
    }
    {   // A slow ramp must NOT drift away point by point (the naive-neighbour bug).
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 101; ++i) pts.push_back({ (int64_t)i * 4, (float)i / 100.f });
        AutomationLane orig = makeLane(pts);
        AutomationLane l = orig;
        ops::Params p; p.amount = 0.02f;
        ops::apply(l, ops::Range{ 0, 500 }, ops::Thin, p);
        float worst = 0.f;
        for (int i = 0; i < orig.size(); ++i)
            worst = std::max(worst, std::fabs(l.value_at(orig.at(i).tick) - orig.at(i).value));
        CHECK_V(worst <= 0.02f + 1e-4f,
                "Thin stays inside its tolerance across the whole curve (worst error %.4f)",
                (double)worst);
    }
    {   // Densify resamples without disturbing the shape.
        AutomationLane l = makeLane({ { 0, 0.f }, { 400, 1.f } });
        const AutomationLane orig = l;
        ops::Params p; p.grid = 50;
        CHECK(ops::apply(l, ops::Range{ 0, 400 }, ops::Densify, p), "Densify reports a change");
        CHECK(l.size() > orig.size(), "Densify adds points");
        int64_t biggestGap = 0;
        for (int i = 1; i < l.size(); ++i)
            biggestGap = std::max(biggestGap, l.at(i).tick - l.at(i - 1).tick);
        CHECK(biggestGap <= 50, "Densify leaves no gap larger than the grid");
        float worst = 0.f;
        for (int64_t t = 0; t <= 400; t += 7)
            worst = std::max(worst, std::fabs(l.value_at(t) - orig.value_at(t)));
        CHECK_V(worst < 1e-3f, "Densify preserves the curve (worst error %.5f)", (double)worst);
    }
}

static void generatorOps() {
    std::printf("[B6] generators\n");
    {   // LfoFill replaces the range and leaves the outside alone.
        AutomationLane l = makeLane({ { 0, 0.05f }, { 250, 0.5f }, { 900, 0.95f } });
        ops::Params p; p.steps = 17; p.syncCycles = 2; p.shape = 0;
        CHECK(ops::apply(l, ops::Range{ 192, 576 }, ops::LfoFill, p), "LfoFill reports a change");
        int inside = 0;
        bool outsideKept = false, outsideKept2 = false;
        for (int i = 0; i < l.size(); ++i) {
            const int64_t t = l.at(i).tick;
            if (t >= 192 && t < 576) ++inside;
            if (t == 0 && approx(l.at(i).value, 0.05f)) outsideKept = true;
            if (t == 900 && approx(l.at(i).value, 0.95f)) outsideKept2 = true;
        }
        CHECK(inside == 17, "LfoFill writes exactly `steps` points");
        CHECK(outsideKept && outsideKept2, "LfoFill leaves points outside the range untouched");
        bool oldGone = true;
        for (int i = 0; i < l.size(); ++i)
            if (l.at(i).tick == 250 && approx(l.at(i).value, 0.5f)) oldGone = false;
        CHECK(oldGone, "LfoFill REPLACES what was in the range");
    }
    {   // Every shape stays inside lo..hi and is finite.
        for (int shape = 0; shape <= 5; ++shape) {
            AutomationLane l = AutomationLane::paramLane(1);
            ops::Params p; p.shape = shape; p.steps = 64; p.syncCycles = 3;
            p.lo = 0.2f; p.hi = 0.8f;
            ops::apply(l, ops::Range{ 0, 960 }, ops::LfoFill, p);
            bool ok = l.size() > 0;
            for (int i = 0; i < l.size(); ++i) {
                const float v = l.at(i).value;
                if (!(v == v) || v < 0.2f - 1e-4f || v > 0.8f + 1e-4f) ok = false;
            }
            CHECK_V(ok, "LFO shape %d fills inside lo..hi", shape);
        }
    }
    {   // LfoFill works on an empty lane (nothing to modify, everything to make).
        AutomationLane l = AutomationLane::paramLane(3);
        ops::Params p; p.grid = 32; p.syncCycles = 1;
        CHECK(ops::apply(l, ops::Range{ 0, 512 }, ops::LfoFill, p), "LfoFill fills an empty lane");
        CHECK(l.size() == 16, "LfoFill honours the grid (512/32)");
    }
    {   // Noise is deterministic and seed-sensitive.
        AutomationLane a = AutomationLane::paramLane(1), b = a, c = a;
        ops::Params p; p.steps = 32; p.seed = 4242;
        ops::apply(a, ops::Range{ 0, 480 }, ops::NoiseFill, p);
        ops::apply(b, ops::Range{ 0, 480 }, ops::NoiseFill, p);
        ops::Params q = p; q.seed = 4243;
        ops::apply(c, ops::Range{ 0, 480 }, ops::NoiseFill, q);
        CHECK(sameLane(a, b), "NoiseFill is reproducible from the same seed");
        CHECK(!sameLane(a, c), "NoiseFill follows the seed");
    }
    {   // RampFill and SCurveFill both run end to end monotonically.
        AutomationLane r = AutomationLane::paramLane(1), s = AutomationLane::paramLane(1);
        // 321 ticks / 33 points puts a sample exactly on the midpoint, so the
        // symmetry check below is testing the curve and not the tick rounding.
        ops::Params p; p.steps = 33; p.lo = 0.f; p.hi = 1.f;
        ops::apply(r, ops::Range{ 0, 321 }, ops::RampFill, p);
        ops::apply(s, ops::Range{ 0, 321 }, ops::SCurveFill, p);
        CHECK(r.size() == 33 && s.size() == 33, "the fills write `steps` points");
        CHECK(approx(r.at(0).value, 0.f) && approx(r.at(32).value, 1.f), "RampFill spans lo..hi");
        CHECK(approx(s.at(0).value, 0.f) && approx(s.at(32).value, 1.f), "SCurveFill spans lo..hi");
        bool rMono = true, sMono = true;
        for (int i = 1; i < 33; ++i) {
            if (r.at(i).value < r.at(i - 1).value - 1e-5f) rMono = false;
            if (s.at(i).value < s.at(i - 1).value - 1e-5f) sMono = false;
        }
        CHECK(rMono && sMono, "both fills are monotonic from lo to hi");
        CHECK(s.at(8).value < r.at(8).value - 1e-3f, "SCurveFill eases in below the straight ramp");
        CHECK(approx(s.at(16).value, 0.5f, 1e-3f), "SCurveFill is symmetric about the middle");
        // a descending ramp is requested by handing hi < lo
        AutomationLane d = AutomationLane::paramLane(1);
        ops::Params q; q.steps = 5; q.lo = 1.f; q.hi = 0.f;
        ops::apply(d, ops::Range{ 0, 100 }, ops::RampFill, q);
        CHECK(approx(d.at(0).value, 1.f) && approx(d.at(4).value, 0.f),
              "RampFill descends when hi < lo");
    }
    {   // BendCurve writes the per-segment curve field, not new points.
        AutomationLane l = makeLane({ { 0, 0.f }, { 100, 1.f }, { 200, 0.f }, { 900, 0.5f } });
        const int before = l.size();
        ops::Params p; p.amount = 1.f;
        CHECK(ops::apply(l, ops::Range{ 0, 300 }, ops::BendCurve, p), "BendCurve reports a change");
        CHECK(l.size() == before, "BendCurve adds no points");
        CHECK(approx(l.at(0).curve, 1.f) && approx(l.at(1).curve, 1.f), "BendCurve sets the curve");
        CHECK(approx(l.at(3).curve, 0.f), "BendCurve leaves outside segments alone");
        ops::Params half; half.amount = 0.5f;
        ops::apply(l, ops::Range{ 0, 300 }, ops::BendCurve, half);
        CHECK(approx(l.at(0).curve, 0.f), "BendCurve amount 0.5 is straight");
    }
    {   // RandomWalk: deterministic, ticks untouched, stays in 0..1.
        std::vector<std::pair<int64_t, float> > pts;
        for (int i = 0; i < 32; ++i) pts.push_back({ (int64_t)i * 16, 0.5f });
        AutomationLane a = makeLane(pts), b = a, c = a;
        ops::Params p; p.amount = 1.f; p.seed = 77;
        CHECK(ops::apply(a, ops::Range{ 0, 512 }, ops::RandomWalk, p), "RandomWalk reports a change");
        ops::apply(b, ops::Range{ 0, 512 }, ops::RandomWalk, p);
        ops::Params q = p; q.seed = 78;
        ops::apply(c, ops::Range{ 0, 512 }, ops::RandomWalk, q);
        CHECK(sameLane(a, b), "RandomWalk is reproducible from the same seed");
        CHECK(!sameLane(a, c), "RandomWalk follows the seed");
        CHECK(a.size() == (int)pts.size(), "RandomWalk keeps the point count");
        bool ticksFixed = true;
        for (int i = 0; i < a.size(); ++i) if (a.at(i).tick != pts[(size_t)i].first) ticksFixed = false;
        CHECK(ticksFixed, "RandomWalk leaves ticks alone");
        CHECK(approx(a.at(0).value, 0.5f), "RandomWalk starts where the curve already was");
    }
}

static void multiLaneOps() {
    std::printf("[B7] multi-lane ops\n");
    const ops::Range r{ 0, 480 };
    {   // CopyShape stamps lane 0's shape onto the others.
        AutomationLane src = makeLane({ { 0, 0.f }, { 240, 1.f }, { 480, 0.f } });
        AutomationLane d1  = makeLane({ { 0, 0.5f }, { 480, 0.5f } });
        AutomationLane d2  = AutomationLane::paramLane(9);
        std::vector<AutomationLane*> v = { &src, &d1, &d2 };
        ops::Params p; p.steps = 33;
        CHECK(ops::applyMulti(v, r, ops::CopyShape, p), "CopyShape reports a change");
        float worst = 0.f;
        for (int64_t t = 0; t < 480; t += 11)
            worst = std::max(worst, std::fabs(d1.value_at(t) - src.value_at(t)));
        CHECK_V(worst < 0.05f, "CopyShape reproduces the source (worst error %.4f)", (double)worst);
        CHECK(d2.size() > 0, "CopyShape also fills an empty destination");
        CHECK(src.size() == 3, "CopyShape leaves the source lane alone");
    }
    {   // MirrorLanes flips about the pivot.
        AutomationLane src = makeLane({ { 0, 0.f }, { 240, 1.f }, { 480, 0.f } });
        AutomationLane dst = makeLane({ { 0, 0.5f }, { 480, 0.5f } });
        std::vector<AutomationLane*> v = { &src, &dst };
        ops::Params p; p.steps = 33; p.pivot = 0.5f;
        CHECK(ops::applyMulti(v, r, ops::MirrorLanes, p), "MirrorLanes reports a change");
        float worst = 0.f;
        for (int64_t t = 0; t < 480; t += 11)
            worst = std::max(worst, std::fabs(dst.value_at(t) - (1.f - src.value_at(t))));
        CHECK_V(worst < 0.05f, "MirrorLanes mirrors about the pivot (worst error %.4f)", (double)worst);

        // ...and mirrors in place when there is only one lane.
        AutomationLane solo = makeLane({ { 0, 0.25f }, { 100, 0.9f } });
        std::vector<AutomationLane*> one = { &solo };
        CHECK(ops::applyMulti(one, ops::Range{ 0, 200 }, ops::MirrorLanes, p),
              "MirrorLanes on a single lane mirrors it in place");
        CHECK(approx(solo.at(0).value, 0.75f) && approx(solo.at(1).value, 0.1f),
              "the in-place mirror uses the pivot");
    }
    {   // PhaseOffsetLanes chases lane 0 with a wrapped offset.
        AutomationLane src = AutomationLane::paramLane(1);
        ops::Params fill; fill.steps = 49; fill.syncCycles = 1; fill.shape = 0;
        ops::apply(src, r, ops::LfoFill, fill);
        AutomationLane dst = AutomationLane::paramLane(2);
        ops::apply(dst, r, ops::LfoFill, fill);
        std::vector<AutomationLane*> v = { &src, &dst };
        ops::Params p; p.steps = 49; p.phase = 0.25f;
        CHECK(ops::applyMulti(v, r, ops::PhaseOffsetLanes, p), "PhaseOffsetLanes reports a change");
        float worst = 0.f;
        for (int64_t t = 0; t < 480; t += 10) {
            const int64_t shifted = (t + 120) % 480;
            worst = std::max(worst, std::fabs(dst.value_at(t) - src.value_at(shifted)));
        }
        CHECK_V(worst < 0.08f, "PhaseOffsetLanes shifts by phase*length (worst error %.4f)",
                (double)worst);
        CHECK(!ops::applyMulti({ &src }, r, ops::PhaseOffsetLanes, p),
              "PhaseOffsetLanes needs a second lane");
    }
    {   // AverageLanes pulls every lane onto the mean.
        AutomationLane a = makeLane({ { 0, 0.f }, { 480, 0.f } });
        AutomationLane b = makeLane({ { 0, 1.f }, { 480, 1.f } });
        std::vector<AutomationLane*> v = { &a, &b };
        ops::Params p; p.steps = 17;
        CHECK(ops::applyMulti(v, r, ops::AverageLanes, p), "AverageLanes reports a change");
        bool ok = true;
        for (int64_t t = 0; t < 480; t += 13)
            if (!approx(a.value_at(t), 0.5f, 1e-3f) || !approx(b.value_at(t), 0.5f, 1e-3f)) ok = false;
        CHECK(ok, "AverageLanes converges both lanes on the mean");

        AutomationLane c = makeLane({ { 0, 0.f }, { 480, 0.f } });
        AutomationLane d = makeLane({ { 0, 1.f }, { 480, 1.f } });
        std::vector<AutomationLane*> w = { &c, &d };
        ops::Params h; h.steps = 17; h.amount = 0.5f;
        ops::applyMulti(w, r, ops::AverageLanes, h);
        CHECK(approx(c.value_at(240), 0.25f, 1e-3f) && approx(d.value_at(240), 0.75f, 1e-3f),
              "AverageLanes at amount 0.5 goes halfway");
    }
    {   // single-lane ops routed through applyMulti hit every lane.
        AutomationLane a = makeLane({ { 0, 0.2f }, { 100, 0.4f } });
        AutomationLane b = makeLane({ { 0, 0.6f }, { 100, 0.8f } });
        std::vector<AutomationLane*> v = { &a, nullptr, &b };
        ops::Params p; p.amount = 0.1f;
        CHECK(ops::applyMulti(v, ops::Range{ 0, 200 }, ops::OffsetValue, p),
              "applyMulti forwards single-lane ops");
        CHECK(approx(a.at(0).value, 0.3f) && approx(b.at(0).value, 0.7f),
              "every lane got the single-lane op");
    }
    {   // AutomationTrack::allLanes() is the intended bridge.
        AutomationTrack t;
        t.addLane(LaneTarget{ LaneTargetKind::VstParam, 1 }).add(0, 0.f);
        t.lane(0).add(480, 0.f);
        t.addLane(LaneTarget{ LaneTargetKind::MidiCC, 74 }).add(0, 1.f);
        t.lane(1).add(480, 1.f);
        std::vector<AutomationLane*> v = t.allLanes();
        CHECK(v.size() == 2, "allLanes() hands over every lane");
        ops::Params p; p.steps = 9;
        CHECK(ops::applyMulti(v, r, ops::AverageLanes, p), "a track's lanes drive applyMulti");
    }
}

static void edgeCases() {
    std::printf("[B8] edge cases\n");
    ops::Params p;
    const ops::Range zero{ 300, 300 };
    const ops::Range inverted{ 500, 100 };
    const ops::Range far{ 1000000, 2000000 };

    AutomationLane empty = AutomationLane::paramLane(1);
    AutomationLane one   = makeLane({ { 300, 0.5f } });
    AutomationLane many  = makeLane({ { 0, 0.f }, { 100, 0.5f }, { 200, 1.f } });

    for (int o = 0; o < ops::OP_COUNT; ++o) {
        const ops::Op op = (ops::Op)o;
        AutomationLane e = empty, a = one, b = many;
        ops::apply(e, zero, op, p);
        ops::apply(a, zero, op, p);
        ops::apply(b, inverted, op, p);
        ops::apply(b, far, op, p);
        std::string why;
        CHECK_V(e.empty(), "%s leaves an empty lane empty on a zero-length range", ops::opName(op));
        CHECK_V(laneWellFormed(a, why) && laneWellFormed(b, why),
                "%s survives zero-length / inverted / far ranges", ops::opName(op));
        // The four generators legitimately WRITE into an empty region -- that is
        // what a fill is. Everything else must leave a missed lane alone.
        const bool generator = (op == ops::LfoFill || op == ops::RampFill ||
                                op == ops::SCurveFill || op == ops::NoiseFill);
        if (!generator)
            CHECK_V(sameLane(b, many), "%s does nothing for a range that misses the data",
                    ops::opName(op));
    }

    // A range holding exactly one point must not be able to divide by zero.
    for (int o = 0; o < ops::OP_COUNT; ++o) {
        const ops::Op op = (ops::Op)o;
        AutomationLane l = makeLane({ { 10, 0.2f }, { 300, 0.5f }, { 900, 0.8f } });
        ops::apply(l, ops::Range{ 250, 350 }, op, p);
        std::string why;
        CHECK_V(laneWellFormed(l, why), "%s survives a single-point range", ops::opName(op));
    }
    // ...and a lane whose points sit exactly on the range boundaries.
    for (int o = 0; o < ops::OP_COUNT; ++o) {
        const ops::Op op = (ops::Op)o;
        AutomationLane l = makeLane({ { 192, 0.2f }, { 576, 0.5f } });   // 576 == r.end, outside
        ops::apply(l, ops::Range{ 192, 576 }, op, p);
        std::string why;
        CHECK_V(laneWellFormed(l, why), "%s survives points on the range boundary", ops::opName(op));
    }
}

int main() {
    std::printf("=== PatchKnob automation_ops_test ===\n\n");
    sweep();
    metadata();
    timeOps();
    rippleOps();
    valueOps();
    densityOps();
    generatorOps();
    multiLaneOps();
    edgeCases();

    std::printf("\n%d checks, %d failure(s)\n", gChecks, gFail);
    std::printf(gFail == 0 ? "=== PASS ===\n" : "=== FAIL ===\n");
    return gFail == 0 ? 0 : 1;
}
