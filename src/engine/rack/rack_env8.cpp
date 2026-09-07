//----------------------------------------------------------------------------
//  rack_env8.cpp -- ENV-8: eight tempo-synced, loopable breakpoint envelopes.
//
//  Each envelope is a breakpoint list over a musical span of up to 8 bars.
//  Time comes from the host tempo, the Clock module's BPM/BAR jacks, or an
//  external CLOCK pulse, so an envelope keeps its MUSICAL length when the tempo
//  changes -- it is not a seconds-based ADSR.
//
//  Editing is graphical (see rackx::ICurveSource + PanelControlStyle::Curve):
//  drag breakpoints, drag the handle on a segment to bend its curve, click empty
//  space to add, DEL PT to remove.  The ruler above the curve shows bar.beat
//  step numbers so points can be placed against the musical grid by eye.
//
//  Per envelope:
//    TIMING   length 1/4..8 bars, grid snap, clock division, sync-to-beat/bar,
//             phase offset
//    LOOP     off / forward / ping-pong / reverse, with loop start+end
//    GATE     gate / one-shot / free-run, retrigger-or-legato, gate window
//    OUTPUT   level (+CV), offset, unipolar/bipolar, invert, slew, value
//             quantise, mute
//    TOOLS    shape presets, randomise, copy/paste between slots
//    OUT      ENV plus an EOC trigger at each cycle end
//----------------------------------------------------------------------------
#include "rack_factory.h"

#include <atomic>
#include <thread>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace {

using rack::engine::Module;

constexpr int   kEnvs        = 8;
constexpr int   kMaxPoints   = 24;
constexpr float kMaxBars     = 8.f;
constexpr int   kBeatsPerBar = 4;
constexpr float kGateHigh    = 2.f;
constexpr float kGateLow     = 0.1f;

// Grid divisions for point snapping, in BEATS.  Index 0 == off.
const float kSnapBeats[] = { 0.f, 4.f, 2.f, 1.f, 0.5f, 0.25f, 0.125f };
constexpr int kNumSnaps = (int)(sizeof(kSnapBeats) / sizeof(kSnapBeats[0]));

// Clock divisions applied to the musical rate.
const float kClockDiv[] = { 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f };
constexpr int kNumDivs = (int)(sizeof(kClockDiv) / sizeof(kClockDiv[0]));

enum LoopMode { LOOP_OFF = 0, LOOP_FWD, LOOP_PINGPONG, LOOP_REV, LOOP_MODES };
enum TrigMode { TRIG_GATE = 0, TRIG_ONESHOT, TRIG_FREE, TRIG_MODES };
enum SyncMode { SYNC_FREE = 0, SYNC_BEAT, SYNC_BAR, SYNC_MODES };
enum ShapeId  { SHAPE_RAMPUP = 0, SHAPE_RAMPDOWN, SHAPE_TRI, SHAPE_PULSE,
                SHAPE_ADSR, SHAPE_SCURVE, SHAPE_EXPFALL, SHAPE_RANDOM, SHAPE_COUNT };

//! Exponential segment shape.  c == 0 is linear; c > 0 starts slow then
//! accelerates, c < 0 the reverse.  Continuous at c -> 0, always maps 0->0, 1->1.
inline float shapeCurve(float x, float c) {
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) return 1.f;
    if (std::fabs(c) < 1e-4f) return x;
    const float k = c * 5.f;
    return (std::exp(k * x) - 1.f) / (std::exp(k) - 1.f);
}

inline float frand01() { return (float)std::rand() / (float)RAND_MAX; }

struct Env8 final : Module, rackx::ICurveSource {
    enum ParamIds {
        // timing
        LENGTH_PARAM,
        SNAP_PARAM      = LENGTH_PARAM     + kEnvs,
        CLKDIV_PARAM    = SNAP_PARAM       + kEnvs,
        SYNCSTART_PARAM = CLKDIV_PARAM     + kEnvs,
        PHASE_PARAM     = SYNCSTART_PARAM  + kEnvs,
        // loop
        LOOPMODE_PARAM  = PHASE_PARAM      + kEnvs,
        LOOPSTART_PARAM = LOOPMODE_PARAM   + kEnvs,
        LOOPEND_PARAM   = LOOPSTART_PARAM  + kEnvs,
        // gate
        TRIGMODE_PARAM  = LOOPEND_PARAM    + kEnvs,
        RETRIG_PARAM    = TRIGMODE_PARAM   + kEnvs,
        GATESTART_PARAM = RETRIG_PARAM     + kEnvs,
        GATEEND_PARAM   = GATESTART_PARAM  + kEnvs,
        // output
        LEVEL_PARAM     = GATEEND_PARAM    + kEnvs,
        OFFSET_PARAM    = LEVEL_PARAM      + kEnvs,
        POLARITY_PARAM  = OFFSET_PARAM     + kEnvs,
        INVERT_PARAM    = POLARITY_PARAM   + kEnvs,
        SLEW_PARAM      = INVERT_PARAM     + kEnvs,
        VQUANT_PARAM    = SLEW_PARAM       + kEnvs,
        MUTE_PARAM      = VQUANT_PARAM     + kEnvs,
        // tools
        SHAPE_PARAM     = MUTE_PARAM       + kEnvs,
        APPLY_PARAM     = SHAPE_PARAM      + kEnvs,
        RANDOM_PARAM    = APPLY_PARAM      + kEnvs,
        COPY_PARAM      = RANDOM_PARAM     + kEnvs,
        PASTE_PARAM     = COPY_PARAM       + kEnvs,
        // breakpoint bookkeeping (the graphical editor drives these)
        SEL_PARAM       = PASTE_PARAM      + kEnvs,
        PTTIME_PARAM    = SEL_PARAM        + kEnvs,
        PTVAL_PARAM     = PTTIME_PARAM     + kEnvs,
        PTCURVE_PARAM   = PTVAL_PARAM      + kEnvs,
        DEL_PARAM       = PTCURVE_PARAM    + kEnvs,
        // one unique id per curve widget (carries no value; find_element() keys
        // on id, so the editor box needs an id of its own)
        CURVEUI_PARAM   = DEL_PARAM        + kEnvs,
        NUM_PARAMS      = CURVEUI_PARAM    + kEnvs
    };
    enum InputIds {
        GATE_INPUT,
        DEPTH_INPUT = GATE_INPUT + kEnvs,      // per-envelope depth CV
        CLOCK_INPUT = DEPTH_INPUT + kEnvs,     // external clock pulse
        BAR_INPUT,                             // bar pulse (phase anchor)
        TEMPO_INPUT,                           // BPM/100 from Clock
        RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        ENV_OUTPUT,
        EOC_OUTPUT = ENV_OUTPUT + kEnvs,       // end-of-cycle trigger
        NUM_OUTPUTS = EOC_OUTPUT + kEnvs
    };
    enum LightIds { ENV_LIGHT, NUM_LIGHTS = ENV_LIGHT + kEnvs };

    struct Pt { float t, v, c; };   // t,v normalised 0..1; c = curve toward next

    Pt    pts[kEnvs][kMaxPoints];
    int   nPts[kEnvs];
    float pos[kEnvs];
    int   dir[kEnvs];               // +1 / -1 for ping-pong
    bool  running[kEnvs], gateHigh[kEnvs], released[kEnvs];
    float slewed[kEnvs];
    float eocTimer[kEnvs];
    bool  pendingStart[kEnvs];      // waiting for the next beat/bar

    int   lastSel[kEnvs];
    float lastPt[kEnvs][3];
    bool  delHigh[kEnvs], applyHigh[kEnvs], randHigh[kEnvs];
    bool  copyHigh[kEnvs], pasteHigh[kEnvs];
    bool  barHigh = false, resetHigh = false, clockHigh = false;
    float beatAccum = 0.f;          // fractional beats since the last beat line

    Pt    clip[kMaxPoints];         // copy/paste clipboard
    int   clipN = 0;

    // THE POINT TABLES ARE SHARED BETWEEN TWO THREADS.  The GUI drags points
    // through ICurveSource while process() reads them -- curveAddPoint did
    // `pts[e][nPts[e]++] = ...` and then re-sorted, so the audio thread could
    // read an incremented count over a half-shuffled array (delPoint shifts
    // BEFORE decrementing, which is the same hazard from the other end), and
    // the RackEngine edit mutex does not cover this path at all.  process()
    // itself also edits, on the tool-button edges, so this is a genuine
    // TWO-WRITER race, not just a torn read.
    //
    // Guarded by a seqlock rather than a mutex, DELIBERATELY.  The curve widget
    // calls curveValueAt() once per pixel of its width every frame it draws, so
    // a mutex here would have the GUI blocking on the audio thread hundreds of
    // times a frame -- and every one of those makes the AUDIO thread's unlock
    // issue a FUTEX_WAKE, i.e. a syscall on the realtime path, which is worse
    // than the bug it fixes.  This costs the audio thread one uncontended CAS
    // and one store (no kernel, no allocation) and never blocks it:
    //
    //   even -> the tables are stable      odd -> a writer is mid-mutation
    //
    // Writers CAS even->odd to take it.  The GUI spins for that (it is not
    // realtime); process() simply declines, deferring its tool-button edge to
    // the next sample and reusing the previous envelope value for this one.
    mutable std::atomic<uint32_t> curveSeq_{0};
    float lastEval_[kEnvs] = {};    // last good evalAt(), for a contended sample

    //! Exclusive access to the point tables.  `spin` = true for the GUI thread
    //! (wait for it), false for the audio thread (take it or carry on without).
    struct CurveWriter {
        std::atomic<uint32_t>& seq;
        uint32_t base = 0;
        bool     held = false;
        CurveWriter(std::atomic<uint32_t>& s, bool spin) : seq(s) {
            for (;;) {
                uint32_t v = seq.load(std::memory_order_relaxed);
                if (!(v & 1u) &&
                    seq.compare_exchange_weak(v, v + 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                    base = v; held = true; return;
                }
                if (!spin) return;
                std::this_thread::yield();
            }
        }
        ~CurveWriter() { if (held) seq.store(base + 2, std::memory_order_release); }
        bool ok() const { return held; }
        CurveWriter(const CurveWriter&) = delete;
        CurveWriter& operator=(const CurveWriter&) = delete;
    };

    Env8() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (int e = 0; e < kEnvs; ++e) {
            const std::string n = std::to_string(e + 1);
            configParam(LENGTH_PARAM + e, 0.25f, kMaxBars, 1.f, "Length " + n, " bars");
            configParam(SNAP_PARAM + e, 0.f, (float)(kNumSnaps - 1), 3.f, "Grid " + n);
            configParam(CLKDIV_PARAM + e, 0.f, (float)(kNumDivs - 1), 2.f, "Clock div " + n);
            configParam(SYNCSTART_PARAM + e, 0.f, (float)(SYNC_MODES - 1), 0.f, "Sync start " + n);
            configParam(PHASE_PARAM + e, 0.f, 1.f, 0.f, "Phase " + n);
            configParam(LOOPMODE_PARAM + e, 0.f, (float)(LOOP_MODES - 1), 0.f, "Loop mode " + n);
            configParam(LOOPSTART_PARAM + e, 0.f, 1.f, 0.f, "Loop start " + n);
            configParam(LOOPEND_PARAM + e, 0.f, 1.f, 1.f, "Loop end " + n);
            configParam(TRIGMODE_PARAM + e, 0.f, (float)(TRIG_MODES - 1), 0.f, "Trigger mode " + n);
            configParam(RETRIG_PARAM + e, 0.f, 1.f, 1.f, "Retrigger " + n);
            configParam(GATESTART_PARAM + e, 0.f, 1.f, 0.f, "Gate start " + n);
            configParam(GATEEND_PARAM + e, 0.f, 1.f, 1.f, "Gate end " + n);
            configParam(LEVEL_PARAM + e, 0.f, 1.f, 1.f, "Level " + n);
            configParam(OFFSET_PARAM + e, -1.f, 1.f, 0.f, "Offset " + n);
            configParam(POLARITY_PARAM + e, 0.f, 1.f, 0.f, "Bipolar " + n);
            configParam(INVERT_PARAM + e, 0.f, 1.f, 0.f, "Invert " + n);
            configParam(SLEW_PARAM + e, 0.f, 1.f, 0.f, "Slew " + n);
            configParam(VQUANT_PARAM + e, 0.f, 16.f, 0.f, "Value steps " + n);
            configParam(MUTE_PARAM + e, 0.f, 1.f, 0.f, "Mute " + n);
            configParam(SHAPE_PARAM + e, 0.f, (float)(SHAPE_COUNT - 1), 0.f, "Shape " + n);
            configParam(APPLY_PARAM + e, 0.f, 1.f, 0.f, "Apply shape " + n);
            configParam(RANDOM_PARAM + e, 0.f, 1.f, 0.f, "Randomise " + n);
            configParam(COPY_PARAM + e, 0.f, 1.f, 0.f, "Copy " + n);
            configParam(PASTE_PARAM + e, 0.f, 1.f, 0.f, "Paste " + n);
            configParam(SEL_PARAM + e, 0.f, (float)(kMaxPoints - 1), 0.f, "Point " + n);
            configParam(PTTIME_PARAM + e, 0.f, 1.f, 0.f, "Point time " + n);
            configParam(PTVAL_PARAM + e, 0.f, 1.f, 0.f, "Point value " + n);
            configParam(PTCURVE_PARAM + e, -1.f, 1.f, 0.f, "Point curve " + n);
            configParam(DEL_PARAM + e, 0.f, 1.f, 0.f, "Delete point " + n);
            configParam(CURVEUI_PARAM + e, 0.f, 1.f, 0.f, "Curve editor " + n);
            configInput(GATE_INPUT + e, "Gate " + n);
            configInput(DEPTH_INPUT + e, "Depth " + n);
            configOutput(ENV_OUTPUT + e, "Env " + n);
            configOutput(EOC_OUTPUT + e, "EOC " + n);
            configLight(ENV_LIGHT + e, "Env " + n);
        }
        configInput(CLOCK_INPUT, "Clock");
        configInput(BAR_INPUT, "Bar");
        configInput(TEMPO_INPUT, "Tempo");
        configInput(RESET_INPUT, "Rst");
        onReset();
    }

    void applyShape(int e, int shape) {
        auto put = [&](int i, float t, float v, float c) { pts[e][i] = Pt{ t, v, c }; };
        switch (shape) {
            case SHAPE_RAMPUP:   nPts[e] = 2; put(0,0.f,0.f,0.f); put(1,1.f,1.f,0.f); break;
            case SHAPE_RAMPDOWN: nPts[e] = 2; put(0,0.f,1.f,0.f); put(1,1.f,0.f,0.f); break;
            case SHAPE_TRI:      nPts[e] = 3; put(0,0.f,0.f,0.f); put(1,0.5f,1.f,0.f);
                                 put(2,1.f,0.f,0.f); break;
            case SHAPE_PULSE:    nPts[e] = 4; put(0,0.f,1.f,0.f); put(1,0.5f,1.f,0.f);
                                 put(2,0.5f,0.f,0.f); put(3,1.f,0.f,0.f); break;
            case SHAPE_ADSR:     nPts[e] = 4; put(0,0.f,0.f,0.3f); put(1,0.1f,1.f,-0.4f);
                                 put(2,0.35f,0.6f,0.f); put(3,1.f,0.f,0.f); break;
            case SHAPE_SCURVE:   nPts[e] = 3; put(0,0.f,0.f,0.9f); put(1,0.5f,0.5f,-0.9f);
                                 put(2,1.f,1.f,0.f); break;
            case SHAPE_EXPFALL:  nPts[e] = 2; put(0,0.f,1.f,-0.9f); put(1,1.f,0.f,0.f); break;
            case SHAPE_RANDOM:
            default: {
                const int n = 4 + (int)(frand01() * 6.f);
                nPts[e] = std::min(n, kMaxPoints);
                for (int i = 0; i < nPts[e]; ++i) {
                    const float t = (float)i / (float)(nPts[e] - 1);
                    put(i, t, frand01(), frand01() * 2.f - 1.f);
                }
                pts[e][0].t = 0.f; pts[e][nPts[e]-1].t = 1.f;
                break;
            }
        }
        lastSel[e] = -1;
    }

    void resetEnv(int e) {
        applyShape(e, SHAPE_ADSR);
        pos[e] = 0.f; dir[e] = 1; running[e] = false; gateHigh[e] = false;
        released[e] = false; slewed[e] = 0.f; eocTimer[e] = 0.f;
        pendingStart[e] = false; lastSel[e] = -1;
        delHigh[e] = applyHigh[e] = randHigh[e] = copyHigh[e] = pasteHigh[e] = false;
    }

    void onReset() override {
        for (int e = 0; e < kEnvs; ++e) resetEnv(e);
        barHigh = resetHigh = clockHigh = false;
        beatAccum = 0.f; clipN = 0;
    }

    float snapTime(int e, float t) const {
        const int   si   = (int)std::lround(params[SNAP_PARAM + e].getValue());
        const float bars = std::max(0.25f, params[LENGTH_PARAM + e].getValue());
        if (si <= 0 || si >= kNumSnaps) return rack::clamp(t, 0.f, 1.f);
        const float step = kSnapBeats[si] / (bars * (float)kBeatsPerBar);
        if (step <= 0.f) return rack::clamp(t, 0.f, 1.f);
        return rack::clamp(std::round(t / step) * step, 0.f, 1.f);
    }

    void sortPoints(int e) {
        for (int i = 1; i < nPts[e]; ++i) {
            Pt k = pts[e][i];
            int j = i - 1;
            while (j >= 0 && pts[e][j].t > k.t) { pts[e][j + 1] = pts[e][j]; --j; }
            pts[e][j + 1] = k;
        }
    }

    void delPoint(int e, int idx) {
        if (nPts[e] <= 2) return;                 // always keep the two ends
        if (idx <= 0 || idx >= nPts[e]) return;   // ends are not removable
        for (int i = idx; i + 1 < nPts[e]; ++i) pts[e][i] = pts[e][i + 1];
        --nPts[e];
        lastSel[e] = -1;
    }

    float evalAt(int e, float t) const {
        const int n = nPts[e];
        if (n <= 0) return 0.f;
        if (t <= pts[e][0].t) return pts[e][0].v;
        if (t >= pts[e][n - 1].t) return pts[e][n - 1].v;
        for (int i = 0; i + 1 < n; ++i) {
            const Pt& a = pts[e][i];
            const Pt& b = pts[e][i + 1];
            if (t >= a.t && t <= b.t) {
                const float span = b.t - a.t;
                if (span <= 1e-6f) return b.v;
                return a.v + (b.v - a.v) * shapeCurve((t - a.t) / span, a.c);
            }
        }
        return pts[e][n - 1].v;
    }

    void syncEditor(int e) {
        int sel = (int)std::lround(params[SEL_PARAM + e].getValue());
        sel = rack::clamp(sel, 0, nPts[e] - 1);
        if (sel != lastSel[e]) {
            params[PTTIME_PARAM + e].setValue(pts[e][sel].t);
            params[PTVAL_PARAM + e].setValue(pts[e][sel].v);
            params[PTCURVE_PARAM + e].setValue(pts[e][sel].c);
            lastSel[e] = sel;
        } else {
            const float t = params[PTTIME_PARAM + e].getValue();
            const float v = params[PTVAL_PARAM + e].getValue();
            const float c = params[PTCURVE_PARAM + e].getValue();
            if (t != lastPt[e][0] || v != lastPt[e][1] || c != lastPt[e][2]) {
                if (sel > 0 && sel < nPts[e] - 1) pts[e][sel].t = snapTime(e, t);
                pts[e][sel].v = rack::clamp(v, 0.f, 1.f);
                pts[e][sel].c = rack::clamp(c, -1.f, 1.f);
                sortPoints(e);
            }
        }
        lastPt[e][0] = params[PTTIME_PARAM + e].getValue();
        lastPt[e][1] = params[PTVAL_PARAM + e].getValue();
        lastPt[e][2] = params[PTCURVE_PARAM + e].getValue();
    }

    // ---- rackx::ICurveSource ----------------------------------------------
    int  curveCount() const override { return kEnvs; }
    int  curvePointCount(int e) const override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        return (e >= 0 && e < kEnvs) ? nPts[e] : 0;
    }
    bool curveGetPoint(int e, int i, rackx::CurvePoint& out) const override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        if (e < 0 || e >= kEnvs || i < 0 || i >= nPts[e]) return false;
        out.t = pts[e][i].t; out.v = pts[e][i].v; out.c = pts[e][i].c;
        return true;
    }
    bool curveSetPoint(int e, int i, const rackx::CurvePoint& p) override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        if (e < 0 || e >= kEnvs || i < 0 || i >= nPts[e]) return false;
        // Span ends stay pinned at 0 and 1 so the envelope always covers the
        // whole musical length; their value and curve remain editable.
        if (i > 0 && i < nPts[e] - 1) pts[e][i].t = rack::clamp(p.t, 0.f, 1.f);
        pts[e][i].v = rack::clamp(p.v, 0.f, 1.f);
        pts[e][i].c = rack::clamp(p.c, -1.f, 1.f);
        sortPoints(e);
        lastSel[e] = -1;
        return true;
    }
    int curveAddPoint(int e, float t, float v) override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        if (e < 0 || e >= kEnvs || nPts[e] >= kMaxPoints) return -1;
        t = rack::clamp(snapTime(e, t), 0.f, 1.f);
        pts[e][nPts[e]++] = Pt{ t, rack::clamp(v, 0.f, 1.f), 0.f };
        sortPoints(e);
        lastSel[e] = -1;
        for (int i = 0; i < nPts[e]; ++i) if (pts[e][i].t == t) return i;
        return nPts[e] - 1;
    }
    bool curveRemovePoint(int e, int i) override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        if (e < 0 || e >= kEnvs) return false;
        const int before = nPts[e];
        delPoint(e, i);
        return nPts[e] != before;
    }
    void curveGetInfo(int e, rackx::CurveInfo& out) const override {
        if (e < 0 || e >= kEnvs) return;
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        // THE EDITOR BINDING BELONGS ON THE GUI THREAD.  syncEditor() WRITES
        // params[PTTIME/PTVAL/PTCURVE] -- knobs the GUI owns and the user may be
        // dragging -- so running it from process() had the audio thread yanking
        // the readouts out from under the pointer.  It is pure editor
        // book-keeping (selected point <-> the three numeric readouts), it only
        // matters while the panel is on screen, and this is the call the panel
        // makes every frame it draws the curve widget.
        const_cast<Env8*>(this)->syncEditor(e);
        const int lm = (int)std::lround(params[LOOPMODE_PARAM + e].getValue());
        out.loopStart = params[LOOPSTART_PARAM + e].getValue();
        out.loopEnd   = params[LOOPEND_PARAM + e].getValue();
        out.loopOn    = lm != LOOP_OFF;
        out.gateStart = params[GATESTART_PARAM + e].getValue();
        out.gateEnd   = params[GATEEND_PARAM + e].getValue();
        const int si  = (int)std::lround(params[SNAP_PARAM + e].getValue());
        const float bars = std::max(0.25f, params[LENGTH_PARAM + e].getValue());
        out.gridStep  = (si > 0 && si < kNumSnaps)
                      ? kSnapBeats[si] / (bars * (float)kBeatsPerBar) : 0.f;
        out.playhead  = running[e] ? rack::clamp(pos[e], 0.f, 1.f) : -1.f;
        out.selected  = (int)std::lround(params[SEL_PARAM + e].getValue());
        out.spanBars  = bars;
        out.beatsPerBar = kBeatsPerBar;
    }
    void curveSetSelected(int e, int i) override {
        if (e < 0 || e >= kEnvs) return;
        params[SEL_PARAM + e].setValue((float)rack::clamp(i, 0, kMaxPoints - 1));
    }
    float curveSnapTime(int e, float t) const override { return snapTime(e, t); }
    float curveValueAt(int e, float t) const override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        return evalAt(e, t);
    }

    //! Exact inverse of shapeCurve() at x = 0.5, so dragging a segment handle
    //! puts the midpoint exactly under the pointer.
    //!   shapeCurve(0.5,c) = (e^(k/2)-1)/(e^k-1) = 1/(u+1),  u = e^(k/2), k = 5c
    //! so f = 1/(u+1)  ->  u = 1/f - 1  ->  c = 2*ln(u)/5.
    void curveSetSegmentMid(int e, int seg, float midValue) override {
        CurveWriter w(curveSeq_, true);        // GUI thread: wait for it
        if (e < 0 || e >= kEnvs || seg < 0 || seg + 1 >= nPts[e]) return;
        const float v0 = pts[e][seg].v, v1 = pts[e][seg + 1].v;
        if (std::fabs(v1 - v0) < 1e-4f) return;      // flat: no bend to infer
        float f = (midValue - v0) / (v1 - v0);
        f = rack::clamp(f, 0.02f, 0.98f);
        const float u = 1.f / f - 1.f;
        if (u <= 1e-6f) return;
        pts[e][seg].c = rack::clamp(2.f * std::log(u) / 5.f, -1.f, 1.f);
        lastSel[e] = -1;
    }

    void process(const ProcessArgs& args) override {
        // Never blocks and never enters the kernel: on a miss the tool-button
        // edges are left un-consumed (so they fire on the next sample) and the
        // envelopes reuse their last value for this one sample.
        CurveWriter curveLock(curveSeq_, false);
        const bool haveCurves = curveLock.ok();

        // ---- tempo ---------------------------------------------------------
        float bpm = args.tempoBpm;
        if (inputs[TEMPO_INPUT].isConnected())
            bpm = inputs[TEMPO_INPUT].getVoltage() * 100.f;      // BPM/100
        bpm = rack::clamp(bpm, 20.f, 999.f);
        float beatsPerSample = (bpm / 60.f) * args.sampleTime;

        // An external CLOCK overrides the tempo estimate: measure its period and
        // treat one pulse as one beat, so the module follows a patched clock even
        // when the host transport is not running.
        const bool clk = inputs[CLOCK_INPUT].getVoltage() >= kGateHigh;
        const bool clkEdge = clk && !clockHigh;
        clockHigh = clk;

        const bool rst = inputs[RESET_INPUT].getVoltage() >= kGateHigh;
        const bool rstEdge = rst && !resetHigh;
        resetHigh = rst;

        const bool bar = inputs[BAR_INPUT].getVoltage() >= kGateHigh;
        const bool barEdge = bar && !barHigh;
        barHigh = bar;

        // Beat-line detection for SYNC START (free / next beat / next bar).
        beatAccum += beatsPerSample;
        bool beatEdge = false;
        if (beatAccum >= 1.f) { beatAccum -= std::floor(beatAccum); beatEdge = true; }
        if (clkEdge) { beatEdge = true; beatAccum = 0.f; }
        if (barEdge) beatAccum = 0.f;

        for (int e = 0; e < kEnvs; ++e) {
            // ---- tool buttons ---------------------------------------------
            // Every one of these RESHAPES the point table, so they only run
            // when this sample actually owns it.  Leaving the edge latches
            // alone on a miss means the press is seen on the next sample
            // rather than swallowed.
            if (haveCurves) {
                const bool delNow = params[DEL_PARAM + e].getValue() >= 0.5f;
                if (delNow && !delHigh[e])
                    delPoint(e, (int)std::lround(params[SEL_PARAM + e].getValue()));
                delHigh[e] = delNow;

                const bool applyNow = params[APPLY_PARAM + e].getValue() >= 0.5f;
                if (applyNow && !applyHigh[e])
                    applyShape(e, (int)std::lround(params[SHAPE_PARAM + e].getValue()));
                applyHigh[e] = applyNow;

                const bool randNow = params[RANDOM_PARAM + e].getValue() >= 0.5f;
                if (randNow && !randHigh[e]) applyShape(e, SHAPE_RANDOM);
                randHigh[e] = randNow;

                const bool copyNow = params[COPY_PARAM + e].getValue() >= 0.5f;
                if (copyNow && !copyHigh[e]) {
                    clipN = nPts[e];
                    for (int i = 0; i < clipN; ++i) clip[i] = pts[e][i];
                }
                copyHigh[e] = copyNow;

                const bool pasteNow = params[PASTE_PARAM + e].getValue() >= 0.5f;
                if (pasteNow && !pasteHigh[e] && clipN >= 2) {
                    nPts[e] = clipN;
                    for (int i = 0; i < clipN; ++i) pts[e][i] = clip[i];
                    lastSel[e] = -1;
                }
                pasteHigh[e] = pasteNow;
            }

            // ---- windows ---------------------------------------------------
            const float gs = params[GATESTART_PARAM + e].getValue();
            float ge = params[GATEEND_PARAM + e].getValue();
            if (ge < gs) ge = gs;
            float ls = params[LOOPSTART_PARAM + e].getValue();
            float le = params[LOOPEND_PARAM + e].getValue();
            if (le <= ls) le = std::min(1.f, ls + 1e-3f);
            const int loopMode = (int)std::lround(params[LOOPMODE_PARAM + e].getValue());
            const int trigMode = (int)std::lround(params[TRIGMODE_PARAM + e].getValue());
            const int syncMode = (int)std::lround(params[SYNCSTART_PARAM + e].getValue());
            const bool retrig  = params[RETRIG_PARAM + e].getValue() >= 0.5f;

            // ---- gate ------------------------------------------------------
            const float gv = inputs[GATE_INPUT + e].getVoltage();
            const bool  hi = gv >= kGateHigh;
            const bool  lo = gv <= kGateLow;

            auto startNow = [&]() {
                pos[e] = gs; dir[e] = 1; running[e] = true; released[e] = false;
            };

            if (trigMode == TRIG_FREE) {
                if (!running[e]) startNow();      // free-run: always going
            } else if (hi && !gateHigh[e]) {
                gateHigh[e] = true;
                if (retrig || !running[e]) {
                    if (syncMode == SYNC_FREE) startNow();
                    else pendingStart[e] = true;  // wait for the next beat/bar
                }
            } else if (lo && gateHigh[e]) {
                gateHigh[e] = false;
                if (trigMode == TRIG_GATE) released[e] = true;
            }
            if (pendingStart[e]) {
                const bool fire = (syncMode == SYNC_BAR) ? barEdge : beatEdge;
                if (fire) { startNow(); pendingStart[e] = false; }
            }
            if (rstEdge) {
                running[e] = false; released[e] = false;
                pendingStart[e] = false; pos[e] = gs; dir[e] = 1;
            }

            // ---- advance ---------------------------------------------------
            bool cycled = false;
            if (running[e]) {
                const float bars = std::max(0.25f, params[LENGTH_PARAM + e].getValue());
                const int   di   = (int)std::lround(params[CLKDIV_PARAM + e].getValue());
                const float div  = kClockDiv[rack::clamp(di, 0, kNumDivs - 1)];
                const float span = bars * (float)kBeatsPerBar * div;     // beats
                const float step = beatsPerSample / std::max(1e-6f, span);

                pos[e] += step * (float)dir[e];

                if (!released[e] && loopMode != LOOP_OFF) {
                    if (loopMode == LOOP_REV) {
                        if (pos[e] <= ls) { pos[e] = le; cycled = true; }
                        if (dir[e] > 0 && pos[e] >= le) { dir[e] = -1; }
                    } else if (loopMode == LOOP_PINGPONG) {
                        if (pos[e] >= le) { pos[e] = le; dir[e] = -1; cycled = true; }
                        else if (pos[e] <= ls) { pos[e] = ls; dir[e] = 1; cycled = true; }
                    } else {                                   // LOOP_FWD
                        if (pos[e] >= le) { pos[e] = ls + (pos[e] - le); cycled = true; }
                    }
                    if (pos[e] < 0.f) pos[e] = 0.f;
                } else if (!released[e] && pos[e] > ge) {
                    pos[e] = ge;                                // hold at gate end
                } else if (pos[e] >= 1.f) {
                    pos[e] = 1.f; cycled = true;
                    if (trigMode == TRIG_FREE) { pos[e] = gs; }
                    else running[e] = false;
                }
            }
            if (cycled) eocTimer[e] = 1e-3f;                    // 1 ms EOC pulse

            // ---- shape -> output -------------------------------------------
            const float phase = params[PHASE_PARAM + e].getValue();
            float rp = pos[e] + phase;
            rp -= std::floor(rp);                                // phase wraps
            float v;
            if (haveCurves) v = lastEval_[e] = evalAt(e, rack::clamp(rp, 0.f, 1.f));
            else            v = lastEval_[e];      // one contended sample: hold

            if (params[INVERT_PARAM + e].getValue() >= 0.5f) v = 1.f - v;

            const int steps = (int)std::lround(params[VQUANT_PARAM + e].getValue());
            if (steps >= 2) v = std::round(v * (steps - 1)) / (float)(steps - 1);

            float depth = params[LEVEL_PARAM + e].getValue();
            if (inputs[DEPTH_INPUT + e].isConnected())
                depth *= rack::clamp(inputs[DEPTH_INPUT + e].getVoltage() * 0.1f, 0.f, 1.f);
            v *= depth;
            v += params[OFFSET_PARAM + e].getValue();

            // Slew is musical-length independent: a one-pole whose time constant
            // runs from instant to ~250 ms.
            const float slew = params[SLEW_PARAM + e].getValue();
            if (slew > 0.f) {
                const float tau = 0.001f + slew * 0.25f;
                const float a = 1.f - std::exp(-args.sampleTime / tau);
                slewed[e] += (v - slewed[e]) * a;
            } else {
                slewed[e] = v;
            }
            float outV = slewed[e];

            const bool bipolar = params[POLARITY_PARAM + e].getValue() >= 0.5f;
            outV = bipolar ? rack::clamp(outV, -1.f, 1.f) * 5.f
                           : rack::clamp(outV,  0.f, 1.f) * 10.f;
            if (params[MUTE_PARAM + e].getValue() >= 0.5f) outV = 0.f;

            outputs[ENV_OUTPUT + e].setVoltage(outV);
            outputs[ENV_OUTPUT + e].channels = 1;

            if (eocTimer[e] > 0.f) eocTimer[e] -= args.sampleTime;
            outputs[EOC_OUTPUT + e].setVoltage(eocTimer[e] > 0.f ? 10.f : 0.f);
            outputs[EOC_OUTPUT + e].channels = 1;

            const float lit = rack::clamp(std::fabs(outV) * 0.1f, 0.f, 1.f);
            lights[ENV_LIGHT + e].setBrightnessRGB(lit * 0.25f, lit * 0.95f, lit);
        }
    }
};

rackx::PanelElement element(int id, float x, float y, float radius,
                            rackx::PanelControlStyle style, const std::string& label,
                            int tab = -1) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = radius;
    v.style = style; v.label = label; v.tab = tab;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

rackx::PanelElement numbox(int id, float x, float y, const std::string& label, int tab) {
    rackx::PanelElement v = element(id, x, y, 11.f,
                                    rackx::PanelControlStyle::SegmentDisplay, label, tab);
    v.width = 52.f; v.height = 22.f;   // wide enough for the value readout
    return v;
}

rackx::PanelElement section(float x, float y, float w, float h,
                            const std::string& label, int tab) {
    rackx::PanelElement v;
    v.style = rackx::PanelControlStyle::Section;
    // The editor draws a Section frame CENTRED on x,y (it renders
    // {dx - dw/2, dy - dh/2, dw, dh}).  Take TOP-LEFT here and convert, so the
    // call sites read like the layout they describe instead of silently
    // drawing each frame half its width off to the left.
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h;
    v.label = label; v.tab = tab;
    v.labelPlacement = rackx::PanelLabelPlacement::Above;
    return v;
}

rackx::PanelSpec env8Panel() {
    using S = rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(46);   // wide
    panel.height = 760.f;                                    // and tall
    panel.headerHeight = 24.f;

    const float W = panel.width;
    for (int e = 0; e < kEnvs; ++e) panel.tabs.push_back(std::to_string(e + 1));

    for (int e = 0; e < kEnvs; ++e) {
        const int t = e;

        // ---- the graphical breakpoint editor ---------------------------------
        rackx::PanelElement curve;
        curve.id = Env8::CURVEUI_PARAM + e;    // unique param id
        curve.curveIndex = e;                  // which curve it edits
        curve.style = S::Curve;
        curve.x = W * 0.5f; curve.y = 178.f;
        curve.width = W - 44.f; curve.height = 220.f;
        curve.tab = t;
        curve.labelPlacement = rackx::PanelLabelPlacement::None;
        panel.params.push_back(curve);

        // ---- grouped control rows -------------------------------------------
        const float colW = (W - 40.f) / 5.f;
        auto col = [&](int i) { return 20.f + colW * ((float)i + 0.5f); };

        panel.decor.push_back(section(20.f, 316.f, W - 40.f, 76.f, "TIMING", t));
        panel.params.push_back(element(Env8::LENGTH_PARAM + e,    col(0), 356.f, 15.f, S::Knob,      "BARS",  t));
        panel.params.push_back(numbox(Env8::SNAP_PARAM + e, col(1), 356.f, "GRID", t));
        panel.params.push_back(numbox(Env8::CLKDIV_PARAM + e, col(2), 356.f, "CLK DIV", t));
        panel.params.push_back(numbox(Env8::SYNCSTART_PARAM + e, col(3), 356.f, "SYNC", t));
        panel.params.push_back(element(Env8::PHASE_PARAM + e,     col(4), 356.f, 13.f, S::Knob,      "PHASE", t));

        panel.decor.push_back(section(20.f, 402.f, (W - 50.f) * 0.5f, 76.f, "LOOP", t));
        const float lc = (W - 50.f) * 0.5f / 3.f;
        panel.params.push_back(numbox(Env8::LOOPMODE_PARAM + e, 20.f + lc * 0.5f, 442.f, "MODE", t));
        panel.params.push_back(element(Env8::LOOPSTART_PARAM + e, 20.f + lc * 1.5f, 442.f, 13.f, S::Knob,      "START", t));
        panel.params.push_back(element(Env8::LOOPEND_PARAM + e,   20.f + lc * 2.5f, 442.f, 13.f, S::Knob,      "END",   t));

        const float gx = 30.f + (W - 50.f) * 0.5f;
        panel.decor.push_back(section(gx, 402.f, (W - 50.f) * 0.5f, 76.f, "GATE", t));
        const float gc = (W - 50.f) * 0.5f / 4.f;
        panel.params.push_back(numbox(Env8::TRIGMODE_PARAM + e, gx + gc * 0.5f, 442.f, "MODE", t));
        panel.params.push_back(element(Env8::RETRIG_PARAM + e,    gx + gc * 1.5f, 442.f, 10.f, S::Switch,    "RETRIG",t));
        panel.params.push_back(element(Env8::GATESTART_PARAM + e, gx + gc * 2.5f, 442.f, 13.f, S::Knob,      "START", t));
        panel.params.push_back(element(Env8::GATEEND_PARAM + e,   gx + gc * 3.5f, 442.f, 13.f, S::Knob,      "END",   t));

        panel.decor.push_back(section(20.f, 488.f, W - 40.f, 76.f, "OUTPUT", t));
        const float oc = (W - 40.f) / 7.f;
        auto ocol = [&](int i) { return 20.f + oc * ((float)i + 0.5f); };
        panel.params.push_back(element(Env8::LEVEL_PARAM + e,    ocol(0), 528.f, 14.f, S::Knob,      "LEVEL",  t));
        panel.params.push_back(element(Env8::OFFSET_PARAM + e,   ocol(1), 528.f, 13.f, S::Knob,      "OFFSET", t));
        panel.params.push_back(element(Env8::POLARITY_PARAM + e, ocol(2), 528.f, 10.f, S::Switch,    "BIPOLR", t));
        panel.params.push_back(element(Env8::INVERT_PARAM + e,   ocol(3), 528.f, 10.f, S::Switch,    "INVERT", t));
        panel.params.push_back(element(Env8::SLEW_PARAM + e,     ocol(4), 528.f, 13.f, S::Knob,      "SLEW",   t));
        panel.params.push_back(numbox(Env8::VQUANT_PARAM + e, ocol(5), 528.f, "STEPS", t));
        panel.params.push_back(element(Env8::MUTE_PARAM + e,     ocol(6), 528.f, 10.f, S::Switch,    "MUTE",   t));

        panel.decor.push_back(section(20.f, 574.f, W - 40.f, 62.f, "TOOLS", t));
        const float tc = (W - 40.f) / 6.f;
        auto tcol = [&](int i) { return 20.f + tc * ((float)i + 0.5f); };
        panel.params.push_back(numbox(Env8::SHAPE_PARAM + e, tcol(0), 608.f, "SHAPE", t));
        panel.params.push_back(element(Env8::APPLY_PARAM + e,  tcol(1), 608.f, 10.f, S::Button,    "APPLY",  t));
        panel.params.push_back(element(Env8::RANDOM_PARAM + e, tcol(2), 608.f, 10.f, S::Button,    "RANDOM", t));
        panel.params.push_back(element(Env8::COPY_PARAM + e,   tcol(3), 608.f, 10.f, S::Button,    "COPY",   t));
        panel.params.push_back(element(Env8::PASTE_PARAM + e,  tcol(4), 608.f, 10.f, S::Button,    "PASTE",  t));
        panel.params.push_back(element(Env8::DEL_PARAM + e,    tcol(5), 608.f, 10.f, S::Button,    "DEL PT", t));

        // per-envelope depth CV lives on its own tab, next to LEVEL
        panel.inputs.push_back(element(Env8::DEPTH_INPUT + e, ocol(0) + 30.f, 528.f, 8.f,
                                       S::Knob, "CV", t));
    }

    // ---- always-visible I/O ------------------------------------------------
    // Three labelled rows: gates in, envelopes out, end-of-cycle out.  Labels
    // sit BELOW each jack, so rows are pitched far enough apart that a caption
    // never lands on the row beneath it.
    panel.decor.push_back(section(20.f, 646.f, W - 40.f, 100.f, "I / O", -1));
    const float jx0 = 44.f, jdx = (W - 220.f) / 7.f;
    for (int e = 0; e < kEnvs; ++e) {
        const float x = jx0 + jdx * (float)e;
        const std::string n = std::to_string(e + 1);
        panel.inputs.push_back(element(Env8::GATE_INPUT + e,  x, 664.f, 8.f, S::Knob, "G" + n));
        panel.outputs.push_back(element(Env8::ENV_OUTPUT + e, x, 700.f, 8.f, S::Knob, "E" + n));
        panel.outputs.push_back(element(Env8::EOC_OUTPUT + e, x, 733.f, 7.f, S::Knob, "C" + n));
        panel.lights.push_back(element(Env8::ENV_LIGHT + e, x + 17.f, 700.f, 4.f, S::Lamp, ""));
    }
    panel.inputs.push_back(element(Env8::CLOCK_INPUT, W - 150.f, 664.f, 8.f, S::Knob, "CLK"));
    panel.inputs.push_back(element(Env8::BAR_INPUT,   W - 110.f, 664.f, 8.f, S::Knob, "BAR"));
    panel.inputs.push_back(element(Env8::TEMPO_INPUT, W - 150.f, 700.f, 8.f, S::Knob, "BPM"));
    panel.inputs.push_back(element(Env8::RESET_INPUT, W - 110.f, 700.f, 8.f, S::Knob, "RST"));
    return panel;
}

} // namespace

namespace rackx {
void registerEnv8Module() {
    addType("ENV8", "Env-8 Tempo", "Envelope", Role::Normal,
            [] { return std::make_unique<Env8>(); }, env8Panel());
}
} // namespace rackx
