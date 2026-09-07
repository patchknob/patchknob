//----------------------------------------------------------------------------
//  rack_buffer_retrig.cpp -- BUFRT: tempo-synced buffer retrigger (glitch).
//
//  A gate freezes a slice of the incoming stereo audio and loops it at a
//  MUSICAL length -- 1/1 down to 1/256 of a whole note, straight / dotted /
//  triplet -- taken from the host tempo, a patched CLOCK pulse or the BPM jack,
//  so a stutter keeps its musical size when the tempo moves.  The SIZE knob is
//  read as 1/N: the panel box shows the denominator (1, 2, 4 ... 256) rather
//  than an opaque index.
//
//  Every repeat is shaped by a TWO-SEGMENT breakpoint envelope with its own
//  exponential bend per segment, edited graphically on the panel (see
//  rackx::ICurveSource + PanelControlStyle::Curve): drag the three points, drag
//  the handle on a segment to bend it.  With both ends high the slice simply
//  repeats; pull the tail down and every repeat gets its own decay -- which is
//  what turns a plain beat repeat into a glitch voice.  The breakpoints live in
//  REAL params, so a shaped glitch saves and restores with the patch.
//
//  Beyond the loop itself:
//    SLICE   size 1/1..1/256, straight/dotted/triplet, start offset, grab from
//            BEFORE the gate (instant) or record forward from it, and a start
//            quantised to free / clock / slice grid
//    REPEAT  gate / latch / one-shot, repeat count, per-repeat length shrink
//            (accelerating ratchets), playback rate and a per-repeat pitch step,
//            reverse / alternate-reverse
//    SHAPE   envelope presets, envelope depth, loop-seam crossfade, and
//            feedback that bakes each enveloped pass back into the buffer
//    OUT     dry/wet, level, plus ENV / per-repeat TRIG / ACTIVE jacks so the
//            rest of the patch can follow the glitch
//----------------------------------------------------------------------------
#include "rack_factory.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using rack::engine::Module;

constexpr float kGateHigh = 2.f;
constexpr float kGateLow  = 0.1f;
constexpr int   kBeatsPerWhole = 4;      // a "1/1" slice is one whole note

// Both buffers are allocated ONCE at construction (the audio thread never
// allocates), which fixes the ceiling in FRAMES rather than seconds: 4 s at
// 48 kHz, 2 s at 96 kHz.  A requested slice longer than that clamps -- only
// reachable at 1/1 with a slow tempo, where a stutter is not the point anyway.
constexpr int kMaxFrames     = 192000;
constexpr int kMinSliceFrames = 32;
constexpr int kMaxDivExp     = 8;        // 2^8 == 1/256
constexpr int kMaxRepeats    = 64;

enum DivMod   { DIVMOD_STRAIGHT = 0, DIVMOD_DOT, DIVMOD_TRIP, DIVMOD_COUNT };
enum TrigMode { TRIG_GATE = 0, TRIG_LATCH, TRIG_SHOT, TRIG_MODES };
enum SyncMode { SYNC_FREE = 0, SYNC_CLOCK, SYNC_GRID, SYNC_MODES };
enum GrabMode { GRAB_BACK = 0, GRAB_FWD, GRAB_MODES };
enum RevMode  { REV_OFF = 0, REV_ON, REV_ALT, REV_MODES };
enum ShapeId  { SHAPE_HOLD = 0, SHAPE_DECAY, SHAPE_PLUCK, SHAPE_SWELL,
                SHAPE_GATE, SHAPE_STAB, SHAPE_TREM, SHAPE_FADE, SHAPE_COUNT };

//! Exponential segment shape.  c == 0 is linear; c > 0 starts slow then
//! accelerates, c < 0 the reverse.  Continuous at c -> 0, always maps 0->0, 1->1.
//! Same law ENV-8 uses, so a curve bent here reads the same as one bent there.
inline float shapeCurve(float x, float c) {
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) return 1.f;
    if (std::fabs(c) < 1e-4f) return x;
    const float k = c * 5.f;
    return (std::exp(k * x) - 1.f) / (std::exp(k) - 1.f);
}

struct BufRetrig final : Module, rackx::ICurveSource {
    enum ParamIds {
        // slice
        SIZE_PARAM, DIVMOD_PARAM, SYNC_PARAM, GRAB_PARAM, OFFSET_PARAM,
        // repeat
        MODE_PARAM, REPEATS_PARAM, SHRINK_PARAM, RATE_PARAM, STEP_PARAM, REV_PARAM,
        // shape / output
        SHAPE_PARAM, APPLY_PARAM, ENVAMT_PARAM, XFADE_PARAM, FDBK_PARAM,
        MIX_PARAM, LEVEL_PARAM,
        // The per-repeat envelope, held in real params so the graphical edit
        // persists with the patch: three breakpoints at t = 0, MIDT, 1 with an
        // exponential bend on each of the two segments.
        L0_PARAM, MIDT_PARAM, L1_PARAM, L2_PARAM, C0_PARAM, C1_PARAM,
        ENVSEL_PARAM,   // which breakpoint the editor has selected
        CURVEUI_PARAM,  // unique id for the curve widget (carries no value)
        NUM_PARAMS
    };
    enum InputIds {
        INL_INPUT, INR_INPUT,      // R normalled to L
        GATE_INPUT, CLOCK_INPUT, TEMPO_INPUT,
        SIZECV_INPUT, RATECV_INPUT, RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        OUTL_OUTPUT, OUTR_OUTPUT,
        ENV_OUTPUT,                // the repeat envelope as CV
        TRIG_OUTPUT,               // 1 ms pulse at the head of every repeat
        ACTIVE_OUTPUT,             // high while retriggering
        NUM_OUTPUTS
    };
    enum LightIds { ACTIVE_LIGHT, ENV_LIGHT, NUM_LIGHTS };

    // Input ring: always recording, so GRAB_BACK can look BEHIND the gate.
    std::vector<float> ring;       // interleaved stereo
    int   wpos = 0;
    // The frozen slice, copied out of the ring (or recorded forward into) at the
    // trigger.  Kept separate so the ring stays live for the next trigger.
    std::vector<float> slice;
    int    sliceLen = 0;
    double playPos  = 0.0;
    int    repeatIdx = 0;
    int    captureCount = 0;
    bool   active = false, capturing = false, pending = false;

    bool  gateHigh = false, clockHigh = false, resetHigh = false, applyHigh = false;
    float clockPeriod = 0.f;       // measured samples per clock pulse (== 1 beat)
    int   clockCount = 0;
    float gridPhase = 0.f;         // samples into the current slice-grid cell
    float trigTimer = 0.f;
    float blend = 0.f;             // click-free wet/dry engage
    float envValue = 0.f;
    float envPhase = 0.f;          // playhead for the editor, 0..1 within a repeat

    // Cached per-repeat derivations.  pow() and exp2() would otherwise run on
    // every sample for values that only move when a repeat starts or a knob is
    // touched, which matters at this engine's module counts.
    float blendCoef = 0.f, blendCoefSr = 0.f;
    int   effLen = 0, effIdx = -1, effSlice = -1;
    float effShrink = -1.f;
    float rateCache = 1.f, rateSemi = 1e9f;

    BufRetrig() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        // Slice size is log2 of the denominator, so displayBase 2 makes the
        // panel box read 1, 2, 4 ... 256 -- i.e. the N of 1/N.
        configParam(SIZE_PARAM, 0.f, (float)kMaxDivExp, 4.f, "Slice size 1/N", "",
                    2.f, 1.f)->snapEnabled = true;
        configSwitch(DIVMOD_PARAM, 0.f, (float)(DIVMOD_COUNT - 1), 0.f, "Slice modifier",
                     { "Straight", "Dotted", "Triplet" });
        configSwitch(SYNC_PARAM, 0.f, (float)(SYNC_MODES - 1), 0.f, "Start sync",
                     { "Free", "Next clock", "Next slice" });
        configSwitch(GRAB_PARAM, 0.f, (float)(GRAB_MODES - 1), 0.f, "Grab",
                     { "Behind gate", "Forward from gate" });
        configParam(OFFSET_PARAM, 0.f, 1.f, 0.f, "Slice start offset");
        configSwitch(MODE_PARAM, 0.f, (float)(TRIG_MODES - 1), 0.f, "Trigger mode",
                     { "Gate", "Latch", "One-shot" });
        configSwitch(REPEATS_PARAM, 0.f, (float)kMaxRepeats, 0.f, "Repeats (0 = hold)");
        configParam(SHRINK_PARAM, 0.25f, 1.f, 1.f, "Length per repeat");
        configParam(RATE_PARAM, -24.f, 24.f, 0.f, "Playback rate", " semi");
        configParam(STEP_PARAM, -12.f, 12.f, 0.f, "Pitch step per repeat", " semi");
        configSwitch(REV_PARAM, 0.f, (float)(REV_MODES - 1), 0.f, "Reverse",
                     { "Off", "On", "Alternate" });
        configSwitch(SHAPE_PARAM, 0.f, (float)(SHAPE_COUNT - 1), 0.f, "Envelope preset",
                     { "Hold", "Decay", "Pluck", "Swell", "Gate", "Stab", "Trem", "Fade" });
        configButton(APPLY_PARAM, "Apply preset");
        configParam(ENVAMT_PARAM, 0.f, 1.f, 1.f, "Envelope depth");
        configParam(XFADE_PARAM, 0.f, 20.f, 2.f, "Loop seam fade", " ms");
        configParam(FDBK_PARAM, 0.f, 1.f, 0.f, "Feedback into buffer");
        configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Dry / wet");
        configParam(LEVEL_PARAM, 0.f, 2.f, 1.f, "Level");
        configParam(L0_PARAM, 0.f, 1.f, 1.f, "Envelope start");
        configParam(MIDT_PARAM, 0.02f, 0.98f, 0.5f, "Envelope mid time");
        configParam(L1_PARAM, 0.f, 1.f, 1.f, "Envelope mid");
        configParam(L2_PARAM, 0.f, 1.f, 1.f, "Envelope end");
        configParam(C0_PARAM, -1.f, 1.f, 0.f, "Envelope curve 1");
        configParam(C1_PARAM, -1.f, 1.f, 0.f, "Envelope curve 2");
        configParam(ENVSEL_PARAM, 0.f, 2.f, 0.f, "Selected breakpoint");
        configParam(CURVEUI_PARAM, 0.f, 1.f, 0.f, "Envelope editor");
        configInput(INL_INPUT, "In L");
        configInput(INR_INPUT, "In R");
        configInput(GATE_INPUT, "Gate");
        configInput(CLOCK_INPUT, "Clock");
        configInput(TEMPO_INPUT, "Tempo");
        configInput(SIZECV_INPUT, "Size CV");
        configInput(RATECV_INPUT, "Rate CV");
        configInput(RESET_INPUT, "Rst");
        configOutput(OUTL_OUTPUT, "Out L");
        configOutput(OUTR_OUTPUT, "Out R");
        configOutput(ENV_OUTPUT, "Env");
        configOutput(TRIG_OUTPUT, "Trig");
        configOutput(ACTIVE_OUTPUT, "Act");
        configLight(ACTIVE_LIGHT, "Active");
        configLight(ENV_LIGHT, "Env");
        configBypass(INL_INPUT, OUTL_OUTPUT);
        configBypass(INR_INPUT, OUTR_OUTPUT);
        ring.assign((size_t)kMaxFrames * 2, 0.f);
        slice.assign((size_t)kMaxFrames * 2, 0.f);
    }

    //! Envelope presets.  HOLD is the identity case the module ships with: both
    //! ends high, so a trigger repeats the buffer untouched.
    void applyShape(int shape) {
        struct Preset { float l0, mt, l1, l2, c0, c1; };
        static const Preset kPresets[SHAPE_COUNT] = {
            { 1.f, 0.50f, 1.00f, 1.00f,  0.00f,  0.00f },   // HOLD  -- plain repeat
            { 1.f, 0.50f, 0.45f, 0.00f, -0.60f, -0.60f },   // DECAY
            { 1.f, 0.12f, 0.35f, 0.00f, -0.90f, -0.80f },   // PLUCK
            { 0.f, 0.50f, 0.50f, 1.00f,  0.50f,  0.50f },   // SWELL
            { 1.f, 0.75f, 1.00f, 0.00f,  0.00f, -1.00f },   // GATE  -- chop the tail
            { 0.f, 0.15f, 1.00f, 0.00f,  0.60f, -0.60f },   // STAB
            { 1.f, 0.50f, 0.00f, 1.00f, -0.40f,  0.40f },   // TREM
            { 1.f, 0.50f, 0.85f, 0.00f,  0.40f,  0.40f }    // FADE
        };
        const Preset& p = kPresets[rack::clamp(shape, 0, SHAPE_COUNT - 1)];
        params[L0_PARAM].setValue(p.l0);
        params[MIDT_PARAM].setValue(p.mt);
        params[L1_PARAM].setValue(p.l1);
        params[L2_PARAM].setValue(p.l2);
        params[C0_PARAM].setValue(p.c0);
        params[C1_PARAM].setValue(p.c1);
    }

    void onReset() override {
        applyShape(SHAPE_HOLD);
        std::fill(ring.begin(), ring.end(), 0.f);
        std::fill(slice.begin(), slice.end(), 0.f);
        wpos = 0; sliceLen = 0; playPos = 0.0; repeatIdx = 0; captureCount = 0;
        active = capturing = pending = false;
        gateHigh = clockHigh = resetHigh = applyHigh = false;
        clockPeriod = 0.f; clockCount = 0; gridPhase = 0.f;
        trigTimer = 0.f; blend = 0.f; envValue = 0.f; envPhase = 0.f;
        effIdx = effSlice = -1; effShrink = -1.f; rateSemi = 1e9f;
    }

    // ---- the two-segment envelope -----------------------------------------
    //! Value at normalised position within ONE repeat.
    float envAt(float t) const {
        const float l0 = params[L0_PARAM].getValue();
        const float l1 = params[L1_PARAM].getValue();
        const float l2 = params[L2_PARAM].getValue();
        const float mt = rack::clamp(params[MIDT_PARAM].getValue(), 0.02f, 0.98f);
        if (t <= 0.f) return l0;
        if (t >= 1.f) return l2;
        if (t < mt)
            return l0 + (l1 - l0) * shapeCurve(t / mt, params[C0_PARAM].getValue());
        return l1 + (l2 - l1) * shapeCurve((t - mt) / (1.f - mt), params[C1_PARAM].getValue());
    }

    //! Envelope scaled by DEPTH: at depth 0 the repeat is flat (unity), at 1 the
    //! drawn shape is applied in full.
    float envGain(float t) const {
        const float amt = rack::clamp(params[ENVAMT_PARAM].getValue(), 0.f, 1.f);
        return 1.f + amt * (rack::clamp(envAt(t), 0.f, 1.f) - 1.f);
    }

    //! Short raised edge at both ends of a repeat.  The envelope alone can leave
    //! a step at the loop seam (both ends high is the whole point of HOLD), so
    //! this is a separate, always-available declick.
    static float seamFade(double pos, int len, float fadeFrames) {
        if (fadeFrames <= 1.f || len <= 2) return 1.f;
        const float f = std::min(fadeFrames, (float)len * 0.25f);
        const float in  = (float)pos / f;
        const float out = (float)((double)len - pos) / f;
        return rack::clamp(std::min(in, out), 0.f, 1.f);
    }

    // ---- rackx::ICurveSource ----------------------------------------------
    // One curve, three fixed breakpoints: the shape IS the two-segment envelope,
    // so points are never added or removed -- only moved and bent.
    int  curveCount() const override { return 1; }
    int  curvePointCount(int) const override { return 3; }
    bool curveGetPoint(int c, int i, rackx::CurvePoint& out) const override {
        if (c != 0) return false;
        switch (i) {
            case 0: out.t = 0.f; out.v = params[L0_PARAM].getValue();
                    out.c = params[C0_PARAM].getValue(); return true;
            case 1: out.t = params[MIDT_PARAM].getValue(); out.v = params[L1_PARAM].getValue();
                    out.c = params[C1_PARAM].getValue(); return true;
            case 2: out.t = 1.f; out.v = params[L2_PARAM].getValue();
                    out.c = 0.f; return true;
            default: return false;
        }
    }
    bool curveSetPoint(int c, int i, const rackx::CurvePoint& p) override {
        if (c != 0) return false;
        // The ends stay pinned at 0 and 1 so the envelope always spans exactly
        // one repeat; their level and bend remain editable.
        switch (i) {
            case 0: params[L0_PARAM].setValue(rack::clamp(p.v, 0.f, 1.f));
                    params[C0_PARAM].setValue(rack::clamp(p.c, -1.f, 1.f)); return true;
            case 1: params[MIDT_PARAM].setValue(rack::clamp(p.t, 0.02f, 0.98f));
                    params[L1_PARAM].setValue(rack::clamp(p.v, 0.f, 1.f));
                    params[C1_PARAM].setValue(rack::clamp(p.c, -1.f, 1.f)); return true;
            case 2: params[L2_PARAM].setValue(rack::clamp(p.v, 0.f, 1.f)); return true;
            default: return false;
        }
    }
    int  curveAddPoint(int, float, float) override { return -1; }
    bool curveRemovePoint(int, int) override { return false; }
    void curveGetInfo(int c, rackx::CurveInfo& out) const override {
        if (c != 0) return;
        out.loopOn = false;
        out.gateStart = 0.f; out.gateEnd = 1.f;
        out.gridStep = 0.25f;      // quarters of a repeat, as a visual guide
        out.spanBars = 0.f;        // one repeat, not a bar count: no ruler
        out.playhead = active ? rack::clamp(envPhase, 0.f, 1.f) : -1.f;
        out.selected = (int)std::lround(params[ENVSEL_PARAM].getValue());
    }
    void curveSetSelected(int c, int i) override {
        if (c == 0) params[ENVSEL_PARAM].setValue((float)rack::clamp(i, 0, 2));
    }
    float curveValueAt(int c, float t) const override { return c == 0 ? envAt(t) : 0.f; }

    //! Exact inverse of shapeCurve() at x = 0.5, so dragging a segment handle
    //! puts the midpoint exactly under the pointer (see ENV-8 for the algebra):
    //!   shapeCurve(0.5,c) = 1/(u+1),  u = e^(5c/2)  ->  c = 2*ln(1/f - 1)/5.
    void curveSetSegmentMid(int c, int seg, float midValue) override {
        if (c != 0 || seg < 0 || seg > 1) return;
        const int lo = (seg == 0) ? L0_PARAM : L1_PARAM;
        const int hi = (seg == 0) ? L1_PARAM : L2_PARAM;
        const float v0 = params[lo].getValue(), v1 = params[hi].getValue();
        if (std::fabs(v1 - v0) < 1e-4f) return;      // flat: no bend to infer
        const float f = rack::clamp((midValue - v0) / (v1 - v0), 0.02f, 0.98f);
        const float u = 1.f / f - 1.f;
        if (u <= 1e-6f) return;
        params[(seg == 0) ? C0_PARAM : C1_PARAM]
            .setValue(rack::clamp(2.f * std::log(u) / 5.f, -1.f, 1.f));
    }

    //! Freeze `frames` of audio ending at the write head -- the audio that has
    //! ALREADY gone past, which is what makes GRAB_BACK instant.
    void grabBehind(int frames) {
        int start = wpos - frames;
        while (start < 0) start += kMaxFrames;
        const int first = std::min(frames, kMaxFrames - start);
        std::copy(ring.begin() + (size_t)start * 2,
                  ring.begin() + (size_t)(start + first) * 2, slice.begin());
        if (first < frames)
            std::copy(ring.begin(), ring.begin() + (size_t)(frames - first) * 2,
                      slice.begin() + (size_t)first * 2);
    }

    void process(const ProcessArgs& args) override {
        // ---- preset button --------------------------------------------------
        const bool applyNow = params[APPLY_PARAM].getValue() >= 0.5f;
        if (applyNow && !applyHigh)
            applyShape((int)std::lround(params[SHAPE_PARAM].getValue()));
        applyHigh = applyNow;

        // ---- tempo ----------------------------------------------------------
        float bpm = args.tempoBpm;
        if (inputs[TEMPO_INPUT].isConnected())
            bpm = inputs[TEMPO_INPUT].getVoltage() * 100.f;      // BPM/100
        bpm = rack::clamp(bpm, 20.f, 999.f);
        float framesPerBeat = args.sampleRate * 60.f / bpm;

        // A patched CLOCK overrides the tempo estimate: one pulse is one beat,
        // measured, so the module follows the patch even with the host stopped.
        const bool clk = inputs[CLOCK_INPUT].getVoltage() >= kGateHigh;
        const bool clkEdge = clk && !clockHigh;
        clockHigh = clk;
        if (inputs[CLOCK_INPUT].isConnected()) {
            ++clockCount;
            if (clkEdge) {
                // Ignore implausible periods (contact bounce, a stalled clock).
                if (clockCount > 8 && clockCount < (int)(args.sampleRate * 8.f))
                    clockPeriod = (float)clockCount;
                clockCount = 0;
            }
            if (clockPeriod > 1.f) framesPerBeat = clockPeriod;
        } else {
            clockPeriod = 0.f; clockCount = 0;
        }

        // ---- slice length ---------------------------------------------------
        float sizeExp = params[SIZE_PARAM].getValue();
        if (inputs[SIZECV_INPUT].isConnected())
            sizeExp += inputs[SIZECV_INPUT].getVoltage() * 0.8f;   // 5 V == 4 halvings
        const int sizeIdx = (int)std::lround(rack::clamp(sizeExp, 0.f, (float)kMaxDivExp));
        float want = framesPerBeat * (float)kBeatsPerWhole / std::exp2((float)sizeIdx);
        switch ((int)std::lround(params[DIVMOD_PARAM].getValue())) {
            case DIVMOD_DOT:  want *= 1.5f;        break;
            case DIVMOD_TRIP: want *= 2.f / 3.f;   break;
            default: break;
        }
        const int wantFrames = rack::clamp((int)std::lround(want),
                                           kMinSliceFrames, kMaxFrames);

        // Free-running slice grid: the anchor SYNC_GRID quantises a trigger to,
        // so a late gate still lands on a musical boundary.
        gridPhase += 1.f;
        bool gridEdge = false;
        if (gridPhase >= (float)wantFrames) { gridPhase -= (float)wantFrames; gridEdge = true; }
        if (clkEdge) { gridPhase = 0.f; gridEdge = true; }

        const bool rst = inputs[RESET_INPUT].getVoltage() >= kGateHigh;
        const bool rstEdge = rst && !resetHigh;
        resetHigh = rst;
        if (rstEdge) {
            active = capturing = pending = false;
            gridPhase = 0.f; playPos = 0.0; repeatIdx = 0;
        }

        // ---- input into the ring (always recording, so GRAB_BACK has history)
        const float inL = inputs[INL_INPUT].getVoltage();
        const float inR = inputs[INR_INPUT].isConnected()
                        ? inputs[INR_INPUT].getVoltage() : inL;
        ring[(size_t)wpos * 2 + 0] = inL;
        ring[(size_t)wpos * 2 + 1] = inR;
        if (++wpos >= kMaxFrames) wpos = 0;

        // ---- trigger --------------------------------------------------------
        const int mode = (int)std::lround(params[MODE_PARAM].getValue());
        const int sync = (int)std::lround(params[SYNC_PARAM].getValue());
        const float gv = inputs[GATE_INPUT].getVoltage();
        bool gateRise = false, gateFall = false;
        if (!gateHigh && gv >= kGateHigh) { gateHigh = true;  gateRise = true; }
        else if (gateHigh && gv <= kGateLow) { gateHigh = false; gateFall = true; }

        if (gateRise) {
            // LATCH toggles: a second tap releases the buffer instead of re-arming.
            if (mode == TRIG_LATCH && (active || pending)) { active = pending = false; }
            else pending = true;
        }
        if (gateFall && mode == TRIG_GATE) { active = pending = false; }

        if (pending) {
            const bool fire = (sync == SYNC_FREE) ||
                              (sync == SYNC_CLOCK ? clkEdge : gridEdge);
            if (fire) {
                sliceLen = wantFrames;
                if ((int)std::lround(params[GRAB_PARAM].getValue()) == GRAB_BACK) {
                    grabBehind(sliceLen);
                    capturing = false;
                } else {
                    capturing = true;      // record forward, playing live meanwhile
                    captureCount = 0;
                }
                playPos = 0.0; repeatIdx = 0; active = true; pending = false;
                trigTimer = 1e-3f;
            }
        }

        // ---- playback -------------------------------------------------------
        // In one-shot mode a repeat count of 0 would never end, so it means one.
        int limit = (int)std::lround(params[REPEATS_PARAM].getValue());
        if (mode == TRIG_SHOT && limit <= 0) limit = 1;

        float wetL = inL, wetR = inR;
        float gain = 0.f;
        if (active && sliceLen >= kMinSliceFrames) {
            const float xfadeFrames =
                params[XFADE_PARAM].getValue() * 0.001f * args.sampleRate;

            if (capturing) {
                // First pass: the slice is being written while it plays, so what
                // is heard is the live input already wearing the envelope.
                slice[(size_t)captureCount * 2 + 0] = inL;
                slice[(size_t)captureCount * 2 + 1] = inR;
                envPhase = (float)captureCount / (float)sliceLen;
                gain = envGain(envPhase)
                     * seamFade((double)captureCount, sliceLen, xfadeFrames);
                wetL = inL * gain; wetR = inR * gain;
                if (++captureCount >= sliceLen) {
                    capturing = false;
                    playPos = 0.0; repeatIdx = 1; trigTimer = 1e-3f;
                    if (limit > 0 && repeatIdx >= limit) active = false;
                }
            } else {
                // SHRINK squeezes every repeat against the one before it, which
                // is what turns a stutter into an accelerating ratchet.  The
                // exponent saturates: past 64 repeats it has already bottomed
                // out on kMinSliceFrames.
                const float shrink = rack::clamp(params[SHRINK_PARAM].getValue(), 0.25f, 1.f);
                if (shrink != effShrink || repeatIdx != effIdx || sliceLen != effSlice) {
                    effShrink = shrink; effIdx = repeatIdx; effSlice = sliceLen;
                    const float scaled = (float)sliceLen
                        * std::pow(shrink, (float)std::min(repeatIdx, 64));
                    effLen = rack::clamp((int)scaled, kMinSliceFrames, sliceLen);
                }

                envPhase = (float)(playPos / (double)effLen);
                gain = envGain(envPhase) * seamFade(playPos, effLen, xfadeFrames);

                const int revMode = (int)std::lround(params[REV_PARAM].getValue());
                const bool rev = revMode == REV_ON ||
                                 (revMode == REV_ALT && (repeatIdx & 1) != 0);
                const double local = rev ? (double)(effLen - 1) - playPos : playPos;

                const int base = (int)(rack::clamp(params[OFFSET_PARAM].getValue(), 0.f, 1.f)
                                       * (float)(sliceLen - 1));
                double idx = (double)base + local;
                while (idx >= (double)sliceLen) idx -= (double)sliceLen;
                while (idx < 0.0) idx += (double)sliceLen;
                int i0 = (int)idx;
                if (i0 >= sliceLen) i0 = sliceLen - 1;
                int i1 = i0 + 1; if (i1 >= sliceLen) i1 = 0;
                const float frac = (float)(idx - (double)i0);
                const float sL = slice[(size_t)i0 * 2 + 0]
                               + (slice[(size_t)i1 * 2 + 0] - slice[(size_t)i0 * 2 + 0]) * frac;
                const float sR = slice[(size_t)i0 * 2 + 1]
                               + (slice[(size_t)i1 * 2 + 1] - slice[(size_t)i0 * 2 + 1]) * frac;
                wetL = sL * gain; wetR = sR * gain;

                // Feedback bakes the enveloped pass back into the buffer, so the
                // loop erodes toward its own envelope instead of staying static.
                const float fb = rack::clamp(params[FDBK_PARAM].getValue(), 0.f, 1.f);
                if (fb > 0.f) {
                    float& dL = slice[(size_t)i0 * 2 + 0];
                    float& dR = slice[(size_t)i0 * 2 + 1];
                    dL = rack::clamp(dL + fb * (wetL - dL), -10.f, 10.f);
                    dR = rack::clamp(dR + fb * (wetR - dR), -10.f, 10.f);
                }

                // Rate: knob + CV + an accumulating step, so each repeat can sit
                // a fixed interval above (or below) the last one.  The step is
                // capped, but the clamp on `semi` is what actually bounds it.
                float semi = params[RATE_PARAM].getValue()
                           + params[STEP_PARAM].getValue() * (float)std::min(repeatIdx, 512);
                if (inputs[RATECV_INPUT].isConnected())
                    semi += inputs[RATECV_INPUT].getVoltage() * 12.f;   // 1 V/oct
                if (semi != rateSemi) {
                    rateSemi = semi;
                    rateCache = rack::clamp(std::exp2(rack::clamp(semi, -60.f, 60.f) / 12.f),
                                            0.0625f, 16.f);
                }

                playPos += (double)rateCache;
                if (playPos >= (double)effLen) {
                    playPos -= (double)effLen;
                    if (playPos < 0.0 || playPos >= (double)effLen) playPos = 0.0;
                    // Saturate rather than wrap: SHRINK and STEP have both
                    // bottomed out long before, so a held retrigger must not
                    // audibly jump back to a full-length, un-stepped repeat.
                    if (repeatIdx < 1000000) ++repeatIdx;
                    trigTimer = 1e-3f;
                    if (limit > 0 && repeatIdx >= limit) active = false;
                }
            }
        }

        // ---- mix ------------------------------------------------------------
        // Engaging and releasing are ramped over ~3 ms: a gate can land anywhere
        // in the waveform and the jump to a frozen buffer would otherwise tick.
        const float target = active ? rack::clamp(params[MIX_PARAM].getValue(), 0.f, 1.f) : 0.f;
        if (args.sampleTime != blendCoefSr) {
            blendCoefSr = args.sampleTime;
            blendCoef = 1.f - std::exp(-args.sampleTime / 0.003f);
        }
        blend += (target - blend) * blendCoef;
        const float level = params[LEVEL_PARAM].getValue();
        const float outL = (inL + (wetL - inL) * blend) * level;
        const float outR = (inR + (wetR - inR) * blend) * level;

        outputs[OUTL_OUTPUT].setVoltage(rack::clamp(outL, -10.f, 10.f));
        outputs[OUTL_OUTPUT].channels = 1;
        outputs[OUTR_OUTPUT].setVoltage(rack::clamp(outR, -10.f, 10.f));
        outputs[OUTR_OUTPUT].channels = 1;

        envValue = active ? rack::clamp(gain, 0.f, 1.f) : 0.f;
        outputs[ENV_OUTPUT].setVoltage(envValue * 10.f);
        outputs[ENV_OUTPUT].channels = 1;

        if (trigTimer > 0.f) trigTimer -= args.sampleTime;
        outputs[TRIG_OUTPUT].setVoltage(trigTimer > 0.f ? 10.f : 0.f);
        outputs[TRIG_OUTPUT].channels = 1;
        outputs[ACTIVE_OUTPUT].setVoltage(active ? 10.f : 0.f);
        outputs[ACTIVE_OUTPUT].channels = 1;

        lights[ACTIVE_LIGHT].setBrightnessRGB(active ? 0.95f : 0.f,
                                              active ? 0.35f : 0.f,
                                              active ? 0.15f : 0.f);
        lights[ENV_LIGHT].setBrightnessRGB(envValue * 0.25f, envValue * 0.95f, envValue);
    }
};

rackx::PanelElement element(int id, float x, float y, float radius,
                            rackx::PanelControlStyle style, const std::string& label) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = radius;
    v.style = style; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

rackx::PanelElement numbox(int id, float x, float y, const std::string& label) {
    rackx::PanelElement v = element(id, x, y, 11.f,
                                    rackx::PanelControlStyle::SegmentDisplay, label);
    v.width = 52.f; v.height = 22.f;   // wide enough for the value readout
    return v;
}

//! A Section frame is drawn CENTRED on x,y, so take TOP-LEFT here and convert:
//! call sites then read like the layout they describe.
rackx::PanelElement section(float x, float y, float w, float h, const std::string& label) {
    rackx::PanelElement v;
    v.style = rackx::PanelControlStyle::Section;
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h;
    v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Above;
    return v;
}

rackx::PanelSpec bufRetrigPanel() {
    using S = rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(28);
    panel.height = 610.f;
    panel.headerHeight = 24.f;

    const float W = panel.width;
    auto cols = [&](int n, int i) { return 20.f + (W - 40.f) / (float)n * ((float)i + 0.5f); };

    // ---- the graphical per-repeat envelope ---------------------------------
    // A control label is clipped to ~42 px (five or six characters), so the
    // legend for what each numbered box SELECTS goes in the section caption,
    // which gets the whole frame width -- the way a silk-screen legend does.
    panel.decor.push_back(section(20.f, 44.f, W - 40.f, 172.f,
                                  "REPEAT ENVELOPE  DRAG PTS, DRAG MID TO BEND"));
    rackx::PanelElement curve;
    curve.id = BufRetrig::CURVEUI_PARAM;    // unique param id
    curve.curveIndex = 0;                   // the module's only curve
    curve.style = S::Curve;
    curve.x = W * 0.5f; curve.y = 130.f;
    curve.width = W - 56.f; curve.height = 148.f;
    curve.labelPlacement = rackx::PanelLabelPlacement::None;
    panel.params.push_back(curve);

    // ---- slice --------------------------------------------------------------
    panel.decor.push_back(section(20.f, 232.f, W - 40.f, 76.f,
                                  "SLICE  MOD STR/DOT/TRP  GRAB BACK/FWD"));
    panel.params.push_back(numbox(BufRetrig::SIZE_PARAM,   cols(5, 0), 272.f, "1/N"));
    panel.params.push_back(numbox(BufRetrig::DIVMOD_PARAM, cols(5, 1), 272.f, "MOD"));
    panel.params.push_back(numbox(BufRetrig::SYNC_PARAM,   cols(5, 2), 272.f, "SYNC"));
    panel.params.push_back(numbox(BufRetrig::GRAB_PARAM,   cols(5, 3), 272.f, "GRAB"));
    panel.params.push_back(element(BufRetrig::OFFSET_PARAM, cols(5, 4), 272.f, 14.f,
                                   S::Knob, "OFFSET"));

    // ---- repeat -------------------------------------------------------------
    panel.decor.push_back(section(20.f, 318.f, W - 40.f, 76.f,
                                  "REPEAT  MODE GATE/LATCH/SHOT  REV OFF/ON/ALT"));
    panel.params.push_back(numbox(BufRetrig::MODE_PARAM,    cols(6, 0), 358.f, "MODE"));
    panel.params.push_back(numbox(BufRetrig::REPEATS_PARAM, cols(6, 1), 358.f, "TIMES"));
    panel.params.push_back(element(BufRetrig::SHRINK_PARAM, cols(6, 2), 358.f, 14.f,
                                   S::Knob, "SHRINK"));
    panel.params.push_back(element(BufRetrig::RATE_PARAM,   cols(6, 3), 358.f, 14.f,
                                   S::Knob, "RATE"));
    panel.params.push_back(element(BufRetrig::STEP_PARAM,   cols(6, 4), 358.f, 14.f,
                                   S::Knob, "STEP"));
    panel.params.push_back(numbox(BufRetrig::REV_PARAM,     cols(6, 5), 358.f, "REV"));

    // ---- shape + output -----------------------------------------------------
    panel.decor.push_back(section(20.f, 404.f, W - 40.f, 76.f,
                                  "SHAPE / OUT   PRESET 0-7, APPLY TO LOAD"));
    panel.params.push_back(numbox(BufRetrig::SHAPE_PARAM,   cols(7, 0), 444.f, "SHAPE"));
    panel.params.push_back(element(BufRetrig::APPLY_PARAM,  cols(7, 1), 444.f, 10.f,
                                   S::Button, "APPLY"));
    panel.params.push_back(element(BufRetrig::ENVAMT_PARAM, cols(7, 2), 444.f, 14.f,
                                   S::Knob, "ENV"));
    panel.params.push_back(element(BufRetrig::XFADE_PARAM,  cols(7, 3), 444.f, 13.f,
                                   S::Knob, "XFADE"));
    panel.params.push_back(element(BufRetrig::FDBK_PARAM,   cols(7, 4), 444.f, 13.f,
                                   S::Knob, "FDBK"));
    panel.params.push_back(element(BufRetrig::MIX_PARAM,    cols(7, 5), 444.f, 14.f,
                                   S::Knob, "MIX"));
    panel.params.push_back(element(BufRetrig::LEVEL_PARAM,  cols(7, 6), 444.f, 14.f,
                                   S::Knob, "LEVEL"));

    // ---- I/O ----------------------------------------------------------------
    // Labels sit BELOW each jack, so the two rows are pitched far enough apart
    // that a caption never lands on the row beneath it.
    panel.decor.push_back(section(20.f, 490.f, W - 40.f, 104.f, "I / O"));
    const float jx0 = 42.f, jdx = (W - 84.f) / 7.f;
    auto jack = [&](int i) { return jx0 + jdx * (float)i; };
    panel.inputs.push_back(element(BufRetrig::INL_INPUT,    jack(0), 512.f, 8.f, S::Knob, "IN L"));
    panel.inputs.push_back(element(BufRetrig::INR_INPUT,    jack(1), 512.f, 8.f, S::Knob, "IN R"));
    panel.inputs.push_back(element(BufRetrig::GATE_INPUT,   jack(2), 512.f, 8.f, S::Knob, "GATE"));
    panel.inputs.push_back(element(BufRetrig::CLOCK_INPUT,  jack(3), 512.f, 8.f, S::Knob, "CLK"));
    panel.inputs.push_back(element(BufRetrig::TEMPO_INPUT,  jack(4), 512.f, 8.f, S::Knob, "BPM"));
    panel.inputs.push_back(element(BufRetrig::SIZECV_INPUT, jack(5), 512.f, 8.f, S::Knob, "SIZE"));
    panel.inputs.push_back(element(BufRetrig::RATECV_INPUT, jack(6), 512.f, 8.f, S::Knob, "RATE"));
    panel.inputs.push_back(element(BufRetrig::RESET_INPUT,  jack(7), 512.f, 8.f, S::Knob, "RST"));
    panel.outputs.push_back(element(BufRetrig::OUTL_OUTPUT,   jack(0), 556.f, 8.f, S::Knob, "OUT L"));
    panel.outputs.push_back(element(BufRetrig::OUTR_OUTPUT,   jack(1), 556.f, 8.f, S::Knob, "OUT R"));
    panel.outputs.push_back(element(BufRetrig::ENV_OUTPUT,    jack(2), 556.f, 8.f, S::Knob, "ENV"));
    panel.outputs.push_back(element(BufRetrig::TRIG_OUTPUT,   jack(3), 556.f, 8.f, S::Knob, "TRG"));
    panel.outputs.push_back(element(BufRetrig::ACTIVE_OUTPUT, jack(4), 556.f, 8.f, S::Knob, "ACT"));
    panel.lights.push_back(element(BufRetrig::ACTIVE_LIGHT, jack(6), 556.f, 5.f, S::Lamp, "ON"));
    panel.lights.push_back(element(BufRetrig::ENV_LIGHT,    jack(7), 556.f, 5.f, S::Lamp, "ENV"));
    return panel;
}

} // namespace

namespace rackx {
void registerBufferRetrigModule() {
    addType("BUFRT", "Buffer Retrig", "Effect", Role::Normal,
            [] { return std::make_unique<BufRetrig>(); }, bufRetrigPanel());
}
} // namespace rackx
