//----------------------------------------------------------------------------
//  seq24 Windows port — automation module self-test.
//
//  Exercises the automation data model and the playback runtime end to end:
//
//    [1] AutomationLane value_at(): linear interpolation (midpoint == mean),
//        Step "holds" the previous value, Hold jumps to the next value, and the
//        curve clamps outside its breakpoint range.
//    [2] Breakpoint editing keeps the list sorted + one point per tick, and
//        add/move/remove/clear behave.
//    [3] AutomationPlayer::advance() over tick windows emits the interpolated /
//        stepped value at toTick, and SUB-SAMPLES every breakpoint crossed in a
//        wider window (fast linear ramp is not lost between block boundaries).
//    [4] Coalescing: a flat lane does not re-emit; emitAt()/resetEmitState()
//        force an unconditional emit (locate).
//    [5] CC lanes emit an integer 0..127 (0.5 -> 64, ends map to 0 and 127) and
//        drive the right controller number; multiple lanes / tracks all fire.
//----------------------------------------------------------------------------
#include "automation_lane.h"
#include "automation_track.h"
#include "automation_player.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace seq24::engine;

static int gFail = 0;
#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++gFail; } \
    else         { std::printf("  ok  : %s\n", msg); }          \
} while (0)

static bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

// --- emit capture -----------------------------------------------------------
struct ParamEmit { int track; unsigned id; float value; };
struct CCEmit    { int track; int cc;      int   value; };

static std::vector<ParamEmit> gParams;
static std::vector<CCEmit>    gCCs;

static AutomationPlayer::EmitParam gEP =
    [](int t, unsigned id, float v) { gParams.push_back({ t, id, v }); };
static AutomationPlayer::EmitCC gEC =
    [](int t, int cc, int v) { gCCs.push_back({ t, cc, v }); };

static void resetCapture() { gParams.clear(); gCCs.clear(); }

int main() {
    std::printf("=== seq24 automation_test ===\n\n");

    // =====================================================================
    // [1] value_at(): interpolation semantics.
    // =====================================================================
    std::printf("[1] AutomationLane value_at()\n");
    {
        // --- Linear ------------------------------------------------------
        AutomationLane lin = AutomationLane::paramLane(7, Interpolation::Linear);
        lin.add(0,    0.0f);
        lin.add(1000, 1.0f);
        CHECK(approx(lin.value_at(-50), 0.0f), "linear clamps below range");
        CHECK(approx(lin.value_at(0),   0.0f), "linear at first breakpoint");
        CHECK(approx(lin.value_at(250), 0.25f), "linear quarter point");
        CHECK(approx(lin.value_at(500), 0.5f),  "linear MIDPOINT == mean of neighbours");
        CHECK(approx(lin.value_at(750), 0.75f), "linear three-quarter point");
        CHECK(approx(lin.value_at(1000),1.0f),  "linear at last breakpoint");
        CHECK(approx(lin.value_at(9999),1.0f),  "linear clamps above range");

        // --- Step (previous-value hold) ----------------------------------
        AutomationLane step = AutomationLane::paramLane(8, Interpolation::Step);
        step.add(0,    0.2f);
        step.add(1000, 0.8f);
        CHECK(approx(step.value_at(0),   0.2f), "step at first breakpoint");
        CHECK(approx(step.value_at(1),   0.2f), "step HOLDS just after first bp");
        CHECK(approx(step.value_at(500), 0.2f), "step HOLDS previous value at midpoint");
        CHECK(approx(step.value_at(999), 0.2f), "step HOLDS right up to next bp");
        CHECK(approx(step.value_at(1000),0.8f), "step jumps AT next breakpoint");
        CHECK(approx(step.value_at(1500),0.8f), "step clamps last value");

        // --- Hold (next-value step) --------------------------------------
        AutomationLane hold = AutomationLane::paramLane(9, Interpolation::Hold);
        hold.add(0,    0.2f);
        hold.add(1000, 0.8f);
        CHECK(approx(hold.value_at(0),   0.2f), "hold at first breakpoint == own value");
        CHECK(approx(hold.value_at(1),   0.8f), "hold jumps to next value right after bp");
        CHECK(approx(hold.value_at(500), 0.8f), "hold reports NEXT value at midpoint");
        CHECK(approx(hold.value_at(1000),0.8f), "hold at last breakpoint");
        // Step and Hold must be genuinely different between breakpoints.
        CHECK(!approx(step.value_at(500), hold.value_at(500)),
              "Step and Hold differ strictly between breakpoints");

        // Empty lane is 0.
        AutomationLane emptyLane = AutomationLane::paramLane(0);
        CHECK(approx(emptyLane.value_at(123), 0.0f), "empty lane value_at == 0");
    }

    // =====================================================================
    // [2] Breakpoint editing: sorted, unique-per-tick, add/move/remove/clear.
    // =====================================================================
    std::printf("\n[2] Breakpoint editing keeps sorted + unique\n");
    {
        AutomationLane l = AutomationLane::paramLane(1, Interpolation::Linear);
        l.add(500, 0.5f);
        l.add(0,   0.0f);        // out of order
        l.add(1000,1.0f);
        l.add(250, 0.25f);
        bool sorted = true;
        for (int i = 1; i < l.size(); ++i)
            if (l.at(i).tick <= l.at(i - 1).tick) sorted = false;
        CHECK(l.size() == 4 && sorted, "out-of-order adds land sorted");

        l.add(500, 0.9f);        // duplicate tick -> replace value
        CHECK(l.size() == 4 && approx(l.value_at(500), 0.9f),
              "add at existing tick replaces (no duplicate)");

        // Value clamps to 0..1.
        int idx = l.add(2000, 5.0f);
        CHECK(approx(l.at(idx).value, 1.0f), "add clamps value to <= 1");

        // move: reposition, stays sorted.
        int startIdx = 0;                       // breakpoint at tick 0
        int newIdx = l.move(startIdx, 1500, 0.3f);
        bool stillSorted = true;
        for (int i = 1; i < l.size(); ++i)
            if (l.at(i).tick <= l.at(i - 1).tick) stillSorted = false;
        CHECK(newIdx >= 0 && stillSorted && approx(l.value_at(1500), 0.3f),
              "move repositions and keeps sorted");

        // remove.
        int before = l.size();
        CHECK(l.removeAtTick(1500) && l.size() == before - 1,
              "removeAtTick drops the point");
        CHECK(!l.removeAtTick(99999), "removeAtTick misses gracefully");

        l.clear();
        CHECK(l.empty() && l.size() == 0, "clear empties the lane");
    }

    // =====================================================================
    // [3] advance(): interpolate/step at window end + sub-sample crossings.
    // =====================================================================
    std::printf("\n[3] AutomationPlayer::advance() over tick windows\n");
    {
        // -- Linear: value at toTick is the interpolated value -------------
        AutomationPlayer p(4);
        p.track(0).addLane(LaneTarget{ LaneTargetKind::VstParam, 7 },
                           Interpolation::Linear);
        p.track(0).lane(0).add(0,    0.0f);
        p.track(0).lane(0).add(1000, 1.0f);

        resetCapture();
        p.advance(0, 500, gEP, gEC);            // window ends at the midpoint
        CHECK(gParams.size() == 1 &&
              gParams[0].track == 0 && gParams[0].id == 7 &&
              approx(gParams[0].value, 0.5f),
              "advance emits linear MIDPOINT (0.5) at toTick=500");

        resetCapture();
        p.advance(500, 1000, gEP, gEC);         // ramp to the top
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 1.0f),
              "advance emits 1.0 at toTick=1000");

        // -- Linear sub-sampling: several breakpoints crossed in ONE window
        AutomationPlayer q(1);
        q.track(0).addLane(LaneTarget{ LaneTargetKind::VstParam, 3 },
                           Interpolation::Linear);
        AutomationLane& ll = q.track(0).lane(0);
        ll.add(0,   0.0f);
        ll.add(250, 0.25f);
        ll.add(500, 0.5f);
        ll.add(750, 0.75f);
        ll.add(1000,1.0f);

        resetCapture();
        q.advance(0, 1000, gEP, gEC);
        // Crossings 250/500/750 (strictly inside) + resting value at 1000.
        CHECK(gParams.size() == 4, "advance sub-samples every crossed breakpoint");
        CHECK(gParams.size() == 4 &&
              approx(gParams[0].value, 0.25f) &&
              approx(gParams[1].value, 0.5f)  &&
              approx(gParams[2].value, 0.75f) &&
              approx(gParams[3].value, 1.0f),
              "sub-sampled values are 0.25, 0.5, 0.75, 1.0 in order");

        // -- Step lane HOLDS across a window -------------------------------
        AutomationPlayer s(1);
        s.track(0).addLane(LaneTarget{ LaneTargetKind::VstParam, 8 },
                           Interpolation::Step);
        s.track(0).lane(0).add(0,    0.2f);
        s.track(0).lane(0).add(1000, 0.8f);

        resetCapture();
        s.advance(0, 500, gEP, gEC);            // midpoint of a Step lane
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 0.2f),
              "advance on Step emits HELD 0.2 at midpoint (not 0.5)");

        resetCapture();
        s.advance(500, 1000, gEP, gEC);         // cross the step
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 0.8f),
              "advance on Step jumps to 0.8 at the next breakpoint");

        resetCapture();
        s.advance(1000, 1500, gEP, gEC);        // flat tail
        CHECK(gParams.empty(),
              "Step lane holding 0.8 does not re-emit (coalesced)");

        // -- Hold lane emits the NEXT value in-window ----------------------
        AutomationPlayer h(1);
        h.track(0).addLane(LaneTarget{ LaneTargetKind::VstParam, 9 },
                           Interpolation::Hold);
        h.track(0).lane(0).add(0,    0.2f);
        h.track(0).lane(0).add(1000, 0.8f);

        resetCapture();
        h.advance(0, 500, gEP, gEC);
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 0.8f),
              "advance on Hold emits NEXT value 0.8 at midpoint");

        // Empty/reversed window is a no-op.
        resetCapture();
        h.advance(600, 600, gEP, gEC);
        h.advance(800, 700, gEP, gEC);
        CHECK(gParams.empty() && gCCs.empty(),
              "advance with toTick<=fromTick emits nothing");
    }

    // =====================================================================
    // [4] Coalescing + emitAt()/resetEmitState() locate behaviour.
    // =====================================================================
    std::printf("\n[4] Coalescing + locate (emitAt / resetEmitState)\n");
    {
        AutomationPlayer p(1);
        p.track(0).addLane(LaneTarget{ LaneTargetKind::VstParam, 5 },
                           Interpolation::Linear);
        p.track(0).lane(0).add(0, 0.3f);        // single breakpoint -> flat 0.3

        resetCapture();
        p.advance(0, 100, gEP, gEC);
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 0.3f),
              "flat lane emits its value the first time");
        p.advance(100, 200, gEP, gEC);
        p.advance(200, 300, gEP, gEC);
        CHECK(gParams.size() == 1, "flat lane does NOT re-emit while unchanged");

        // emitAt force-emits even when unchanged (a locate).
        resetCapture();
        p.emitAt(150, gEP, gEC);
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 0.3f),
              "emitAt() force-emits current value on locate");

        // resetEmitState makes the next advance emit unconditionally.
        resetCapture();
        p.resetEmitState();
        p.advance(300, 400, gEP, gEC);
        CHECK(gParams.size() == 1 && approx(gParams[0].value, 0.3f),
              "resetEmitState() forces the next emit");

        // setCoalesce(false): one emit per window regardless of change.
        resetCapture();
        p.setCoalesce(false);
        p.advance(400, 500, gEP, gEC);
        p.advance(500, 600, gEP, gEC);
        CHECK(gParams.size() == 2, "setCoalesce(false) emits every window");
    }

    // =====================================================================
    // [5] CC lanes: emit integer 0..127 on the right controller; multi-lane.
    // =====================================================================
    std::printf("\n[5] MIDI CC lanes (0..127) + multiple lanes/tracks\n");
    {
        // Static quantizer sanity.
        CHECK(AutomationPlayer::ccFromNorm(0.0f) == 0,   "ccFromNorm(0.0) == 0");
        CHECK(AutomationPlayer::ccFromNorm(1.0f) == 127, "ccFromNorm(1.0) == 127");
        CHECK(AutomationPlayer::ccFromNorm(0.5f) == 64,  "ccFromNorm(0.5) == 64");
        CHECK(AutomationPlayer::ccFromNorm(-1.0f) == 0,  "ccFromNorm clamps below to 0");
        CHECK(AutomationPlayer::ccFromNorm(2.0f) == 127, "ccFromNorm clamps above to 127");

        AutomationPlayer p(4);
        // CC controller 10 on track 1, linear 0..1 over 0..1000.
        p.track(1).addLane(LaneTarget{ LaneTargetKind::MidiCC, 10 },
                           Interpolation::Linear);
        p.track(1).lane(0).add(0,    0.0f);
        p.track(1).lane(0).add(1000, 1.0f);

        resetCapture();
        p.emitAt(0, gEP, gEC);                  // locate to start
        CHECK(gCCs.size() == 1 && gCCs[0].track == 1 && gCCs[0].cc == 10 &&
              gCCs[0].value == 0,
              "CC lane emits controller 10, value 0 at start");

        resetCapture();
        p.advance(0, 500, gEP, gEC);            // midpoint -> 64
        CHECK(gCCs.size() == 1 &&
              gCCs[0].value >= 0 && gCCs[0].value <= 127 &&
              gCCs[0].value == 64,
              "CC lane emits 64 (0.5*127 rounded) at midpoint, in 0..127");

        resetCapture();
        p.advance(500, 1000, gEP, gEC);         // top -> 127
        CHECK(gCCs.size() == 1 && gCCs[0].value == 127,
              "CC lane emits 127 at the top of the ramp");

        // CC coalesce is by integer value: sub-127-step wiggles do not re-emit.
        AutomationPlayer c(1);
        c.track(0).addLane(LaneTarget{ LaneTargetKind::MidiCC, 11 },
                           Interpolation::Linear);
        c.track(0).lane(0).add(0,    0.500f);
        c.track(0).lane(0).add(1000, 0.505f);   // normalized drifts, CC stays 64
        resetCapture();
        c.advance(0, 100, gEP, gEC);
        c.advance(100, 200, gEP, gEC);
        CHECK(gCCs.size() == 1 && gCCs[0].value == 64,
              "CC coalesces when the quantized 0..127 value is unchanged");

        // Multiple lanes on one track + a lane on another track all fire.
        AutomationPlayer m(4);
        m.track(2).addLane(LaneTarget{ LaneTargetKind::VstParam, 20 },
                           Interpolation::Linear);
        m.track(2).lane(0).add(0, 0.0f);
        m.track(2).lane(0).add(1000, 1.0f);
        m.track(2).addLane(LaneTarget{ LaneTargetKind::MidiCC, 74 },
                           Interpolation::Linear);
        m.track(2).lane(1).add(0, 0.0f);
        m.track(2).lane(1).add(1000, 1.0f);
        m.track(3).addLane(LaneTarget{ LaneTargetKind::VstParam, 21 },
                           Interpolation::Step);
        m.track(3).lane(0).add(0, 1.0f);        // constant 1.0

        resetCapture();
        m.advance(0, 500, gEP, gEC);
        bool sawT2Param = false, sawT2CC = false, sawT3Param = false;
        for (auto& e : gParams) {
            if (e.track == 2 && e.id == 20 && approx(e.value, 0.5f)) sawT2Param = true;
            if (e.track == 3 && e.id == 21 && approx(e.value, 1.0f)) sawT3Param = true;
        }
        for (auto& e : gCCs)
            if (e.track == 2 && e.cc == 74 && e.value == 64) sawT2CC = true;
        CHECK(sawT2Param && sawT2CC && sawT3Param,
              "multiple lanes across multiple tracks all emit in one advance");

        // laneForTarget upserts rather than duplicating.
        AutomationLane& again =
            m.track(2).laneForTarget(LaneTarget{ LaneTargetKind::VstParam, 20 });
        CHECK(m.track(2).laneCount() == 2 && again.target().id == 20,
              "laneForTarget returns existing lane (no duplicate)");
    }

    // =====================================================================
    std::printf("\n=== %s (%d failure%s) ===\n",
                gFail == 0 ? "PASS" : "FAIL", gFail, gFail == 1 ? "" : "s");
    return gFail == 0 ? 0 : 1;
}
