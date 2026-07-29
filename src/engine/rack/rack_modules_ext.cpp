//----------------------------------------------------------------------------
//  src/engine/rack/rack_modules_ext.cpp
//
//  A SECOND batch of VCV-Rack-style modules, implemented against the GUI-free
//  rack::engine API (rack.hpp / rack_dsp.h) and registered through the factory
//  (rack_factory.h).  The core set (VCO/VCF/VCA/ADSR/LFO/Noise/Mixer + I/O)
//  lives in rack_modules.cpp; this file adds clocks, a sequencer, utilities,
//  effects and a few more sources.  Same rules as the core set: every module is
//  data-driven (constructor declares params/inputs/outputs/lights with short
//  on-screen labels) and process() is allocation-free and lock-free -- all
//  state (filters / oscillators / delay lines / RNG) lives in the struct and
//  any buffer is sized ONCE in the constructor.
//
//  Signal conventions (match VCV so modules interoperate):
//    * audio  = +/-5V,  CV = +/-10V,  gate/trigger = 0/10V
//    * pitch  = 1V/octave, 0V == middle C via rack::FREQ_C4
//----------------------------------------------------------------------------

// rack_dsp.h references M_PI; MinGW/MSVC gate it behind _USE_MATH_DEFINES under
// a strict -std=c++17.  Request it before <cmath> is first pulled in.  Guarded
// so it never clashes with a build that already defines it on the command line.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include "rack.hpp"
#include "rack_dsp.h"
#include "rack_factory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

using rack::engine::Module;

rackx::PanelElement panelElement(int id, float x, float y, float radius,
                                 const char* label = "",
                                 rackx::PanelLabelPlacement placement = rackx::PanelLabelPlacement::Below)
{
    return rackx::PanelElement{ id, x, y, radius, rackx::PanelControlStyle::Knob, label, placement };
}

rackx::PanelElement panelStyled(int id, float x, float y, float radius,
                                rackx::PanelControlStyle style, const char* label = "",
                                rackx::PanelLabelPlacement placement = rackx::PanelLabelPlacement::None)
{
    rackx::PanelElement element{ id, x, y, radius, style, label, placement };
    element.width = 2.f * radius;
    element.height = 2.f * radius;
    return element;
}

rackx::PanelSpec clockPanel()
{
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(10);
    panel.inputs = {
        panelElement(0, 26.f, 62.f, 8.f, "RST", rackx::PanelLabelPlacement::Above),
        panelElement(1, 26.f, 100.f, 8.f, "GATE", rackx::PanelLabelPlacement::Above)
    };
    panel.outputs = {
        panelElement(0, 132.f, 62.f, 8.f, "CLK", rackx::PanelLabelPlacement::Above),
        panelElement(1, 32.f, 126.f, 8.f, "*2", rackx::PanelLabelPlacement::Above),
        panelElement(2, 32.f, 178.f, 8.f, "*4", rackx::PanelLabelPlacement::Above),
        panelElement(3, 32.f, 230.f, 8.f, "*8", rackx::PanelLabelPlacement::Above),
        panelElement(4, 32.f, 282.f, 8.f, "*16", rackx::PanelLabelPlacement::Above),
        panelElement(5, 32.f, 334.f, 8.f, "*32", rackx::PanelLabelPlacement::Above),
        panelElement(6, 120.f, 126.f, 8.f, "/2", rackx::PanelLabelPlacement::Above),
        panelElement(7, 120.f, 178.f, 8.f, "/4", rackx::PanelLabelPlacement::Above),
        panelElement(8, 120.f, 230.f, 8.f, "/8", rackx::PanelLabelPlacement::Above),
        panelElement(9, 120.f, 282.f, 8.f, "/16", rackx::PanelLabelPlacement::Above),
        panelElement(10, 120.f, 334.f, 8.f, "/32", rackx::PanelLabelPlacement::Above)
    };
    panel.lights = {
        panelElement(0, 110.f, 62.f, 5.f),
        panelElement(1, 52.f, 126.f, 5.f),
        panelElement(2, 52.f, 178.f, 5.f),
        panelElement(3, 52.f, 230.f, 5.f),
        panelElement(4, 52.f, 282.f, 5.f),
        panelElement(5, 52.f, 334.f, 5.f),
        panelElement(6, 100.f, 126.f, 5.f),
        panelElement(7, 100.f, 178.f, 5.f),
        panelElement(8, 100.f, 230.f, 5.f),
        panelElement(9, 100.f, 282.f, 5.f),
        panelElement(10, 100.f, 334.f, 5.f)
    };
    return panel;
}

// Small step-number strings for grid headers ("1".."16"); returned as stable
// C-string literals so PanelElement's `const char* label` stays valid.
const char* stepLabel(int number)
{
    static const char* kLabels[16] = {
        "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };
    return (number >= 1 && number <= 16) ? kLabels[number - 1] : "";
}

// 16-track x 16-step grid, paged 4 tracks per tab (standard 3U height).  Layout
// must stay in lock-step with the SEQ8 enum:
//   value knob   param  = track*16 + step
//   gate LED     param  = 256  + track*16 + step
//   prob box     param  = 512  + track*16 + step
//   start knob   param  = 768  + track
//   end knob     param  = 784  + track
//   cell light   index  = track*16 + step
//   CV out       output = track
//   gate out     output = 16 + track
rackx::PanelSpec seq8Panel()
{
    constexpr int   kTracks = 16;
    constexpr int   kSteps  = 16;
    constexpr int   kPerTab = 4;
    constexpr float kW = 960.f;
    constexpr float kH = rackx::RACK_PANEL_HEIGHT;   // 380 -- same as other modules
    constexpr float kStartX = 40.f;      // per-track loop-start selector column
    constexpr float kEndX   = 62.f;      // per-track loop-end selector column
    constexpr float kGridX  = 86.f;      // first step column
    constexpr float kContentY = 56.f;    // title (20) + tab bar (18) + header room
    constexpr float kStripY = 302.f;     // shared output strip starts here
    constexpr float kColW   = (kW - kGridX - 16.f) / kSteps;
    constexpr float kRowH   = (kStripY - kContentY) / kPerTab;

    rackx::PanelSpec panel;
    panel.width = kW;
    panel.height = kH;
    panel.tabs = { "1-4", "5-8", "9-12", "13-16" };

    for (int track = 0; track < kTracks; ++track) {
        const int   tab = track / kPerTab;
        const int   rowInTab = track % kPerTab;
        const float rowY = kContentY + rowInTab * kRowH;
        const bool  topRow = rowInTab == 0;      // step-number headers per tab

        auto tabbed = [tab](rackx::PanelElement element) {
            element.tab = tab;
            return element;
        };

        // Per-track loop range selectors (left gutter); the START knob carries
        // the row number so tracks are numbered down the left edge.
        panel.params.push_back(tabbed(panelElement(768 + track, kStartX, rowY + 22.f, 6.f,
                                      stepLabel(track + 1), rackx::PanelLabelPlacement::Left)));
        panel.params.push_back(tabbed(panelElement(784 + track, kEndX, rowY + 22.f, 6.f)));

        for (int step = 0; step < kSteps; ++step) {
            const float cx = kGridX + step * kColW + kColW * 0.5f;
            const int   valueId = track * kSteps + step;
            const int   gateId = 256 + track * kSteps + step;
            const int   probId = 512 + track * kSteps + step;

            const char* header = topRow ? stepLabel(step + 1) : "";
            const rackx::PanelLabelPlacement headerPlace = topRow
                ? rackx::PanelLabelPlacement::Above : rackx::PanelLabelPlacement::None;
            panel.params.push_back(tabbed(panelElement(valueId, cx, rowY + 24.f, 10.f, header, headerPlace)));
            panel.params.push_back(tabbed(panelStyled(gateId, cx - 14.f, rowY + 50.f, 5.f,
                                          rackx::PanelControlStyle::Gate)));
            // Probability: a tiny numeric box (drag to edit), not a knob.
            rackx::PanelElement prob = panelStyled(probId, cx + 11.f, rowY + 50.f, 8.f,
                                                   rackx::PanelControlStyle::NumberBox);
            prob.width = 22.f;
            prob.height = 15.f;
            panel.params.push_back(tabbed(prob));
            panel.lights.push_back(tabbed(panelElement(valueId, cx, rowY + 8.f, 3.f)));
        }
    }

    // Shared output strip (tab -1 = always visible): CLOCK/RESET on the left,
    // then a column per track with its CV out (top, numbered) and gate out.
    panel.inputs = {
        panelElement(0, 30.f, kStripY + 30.f, 8.f, "CLK", rackx::PanelLabelPlacement::Above),
        panelElement(1, 64.f, kStripY + 30.f, 8.f, "RST", rackx::PanelLabelPlacement::Above)
    };
    const float kOutX = 104.f;
    const float kOutColW = (kW - kOutX - 14.f) / kTracks;
    const float kCvY = kStripY + 28.f;
    const float kGateY = kStripY + 56.f;
    for (int track = 0; track < kTracks; ++track) {
        const float cx = kOutX + track * kOutColW + kOutColW * 0.5f;
        panel.outputs.push_back(panelElement(track, cx, kCvY, 7.f,
                                             stepLabel(track + 1), rackx::PanelLabelPlacement::Above));
        panel.outputs.push_back(panelElement(kTracks + track, cx, kGateY, 7.f));
    }
    return panel;
}

static constexpr float kPi = 3.14159265358979323846f;

// Gate threshold pair used everywhere we Schmitt-trigger a 0/10V gate/trigger.
static constexpr float kGateLow  = 0.1f;
static constexpr float kGateHigh = 2.0f;

//============================================================================
//  Clock -- host-synced master clock.  GATE mutes its outputs, RESET zeroes
//  phase, and each output has a matching LED.
//============================================================================
struct Clock : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { RESET_INPUT, GATE_INPUT, NUM_INPUTS };
    enum OutputIds { CLK_OUTPUT, M2_OUTPUT, M4_OUTPUT, M8_OUTPUT, M16_OUTPUT, M32_OUTPUT,
                     D2_OUTPUT, D4_OUTPUT, D8_OUTPUT, D16_OUTPUT, D32_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { CLK_LIGHT, M2_LIGHT, M4_LIGHT, M8_LIGHT, M16_LIGHT, M32_LIGHT,
                     D2_LIGHT, D4_LIGHT, D8_LIGHT, D16_LIGHT, D32_LIGHT, NUM_LIGHTS };

    float phase = 0.f;
    int counter = 0;
    rack::dsp::SchmittTrigger resetTrig, gateTrig;

    Clock() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(RESET_INPUT, "Rst");
        configInput(GATE_INPUT, "Gate");
        configOutput(CLK_OUTPUT, "Clk");
        configOutput(M2_OUTPUT, "*2");
        configOutput(M4_OUTPUT, "*4");
        configOutput(M8_OUTPUT, "*8");
        configOutput(M16_OUTPUT, "*16");
        configOutput(M32_OUTPUT, "*32");
        configOutput(D2_OUTPUT, "/2");
        configOutput(D4_OUTPUT, "/4");
        configOutput(D8_OUTPUT, "/8");
        configOutput(D16_OUTPUT, "/16");
        configOutput(D32_OUTPUT, "/32");
        configLight(CLK_LIGHT, "Clk");
        configLight(M2_LIGHT, "*2");
        configLight(M4_LIGHT, "*4");
        configLight(M8_LIGHT, "*8");
        configLight(M16_LIGHT, "*16");
        configLight(M32_LIGHT, "*32");
        configLight(D2_LIGHT, "/2");
        configLight(D4_LIGHT, "/4");
        configLight(D8_LIGHT, "/8");
        configLight(D16_LIGHT, "/16");
        configLight(D32_LIGHT, "/32");
    }

    void setOutputsLow() {
        for (rack::engine::Output& output : outputs) {
            output.setVoltage(0.f);
            output.channels = 1;
        }
        for (rack::engine::Light& light : lights)
            light.setBrightnessRGB(0.f, 0.f, 0.f);
    }

    void resetClock() {
        phase = 0.f;
        counter = 0;
    }

    void onReset() override {
        resetClock();
        resetTrig.reset();
        gateTrig.reset();
        setOutputsLow();
    }

    void process(const ProcessArgs& args) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), kGateLow, kGateHigh))
            resetClock();

        const bool gateOpen = !inputs[GATE_INPUT].isConnected()
                           || inputs[GATE_INPUT].getVoltage() >= kGateHigh;
        if (inputs[GATE_INPUT].isConnected() &&
            gateTrig.process(inputs[GATE_INPUT].getVoltage(), kGateLow, kGateHigh))
            resetClock();

        if (!args.isPlaying || !gateOpen) {
            setOutputsLow();
            return;
        }

        float freq = rack::clamp(args.tempoBpm, 20.f, 999.f) / 60.f;
        phase += freq * args.sampleTime;
        if (phase >= 1.f) {
            phase -= std::floor(phase);
            counter = (counter + 1) & 31;
        }

        bool high = phase < 0.5f;
        outputs[CLK_OUTPUT].setVoltage(high ? 10.f : 0.f);
        outputs[CLK_OUTPUT].channels = 1;
        lights[CLK_LIGHT].setBrightnessRGB(high ? 0.1f : 0.f, high ? 0.78f : 0.f, high ? 1.f : 0.f);

        static const int kMult[5] = { 2, 4, 8, 16, 32 };
        static const float kMultColors[5][3] = {
            { 0.15f, 0.75f, 1.f }, { 0.25f, 1.f, 0.55f }, { 0.75f, 0.30f, 1.f },
            { 1.f, 0.30f, 0.70f }, { 1.f, 0.45f, 0.15f }
        };
        for (int i = 0; i < 5; ++i) {
            bool multipliedHigh = std::fmod(phase * kMult[i], 1.f) < 0.5f;
            outputs[M2_OUTPUT + i].setVoltage(multipliedHigh ? 10.f : 0.f);
            outputs[M2_OUTPUT + i].channels = 1;
            lights[M2_LIGHT + i].setBrightnessRGB(
                multipliedHigh ? kMultColors[i][0] : 0.f,
                multipliedHigh ? kMultColors[i][1] : 0.f,
                multipliedHigh ? kMultColors[i][2] : 0.f);
        }

        static const int kDiv[5] = { 2, 4, 8, 16, 32 };
        static const float kDivColors[5][3] = {
            { 0.25f, 1.f, 0.30f }, { 1.f, 0.68f, 0.05f },
            { 1.f, 0.18f, 0.75f }, { 1.f, 0.18f, 0.18f }, { 0.40f, 0.55f, 1.f }
        };
        for (int i = 0; i < 5; ++i) {
            bool dividedHigh = (counter % kDiv[i]) < (kDiv[i] / 2);
            outputs[D2_OUTPUT + i].setVoltage(dividedHigh ? 10.f : 0.f);
            outputs[D2_OUTPUT + i].channels = 1;
            lights[D2_LIGHT + i].setBrightnessRGB(
                dividedHigh ? kDivColors[i][0] : 0.f,
                dividedHigh ? kDivColors[i][1] : 0.f,
                dividedHigh ? kDivColors[i][2] : 0.f);
        }
    }
};

//============================================================================
//  Clock Div -- four divided-square outputs (/2 /4 /8 /16) from one clock.
//  Counter advances on each rising edge; output N is high while the counter
//  (mod div) is in its first half -- a square at 1/div the input rate.
//============================================================================
struct ClockDiv : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { CLK_INPUT, NUM_INPUTS };
    enum OutputIds { D2_OUTPUT, D4_OUTPUT, D8_OUTPUT, D16_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    int counter = 0;                            // wraps at 16 (LCM of divisors)
    rack::dsp::SchmittTrigger clockTrig;

    ClockDiv() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(CLK_INPUT, "Clk");
        configOutput(D2_OUTPUT,  "/2");
        configOutput(D4_OUTPUT,  "/4");
        configOutput(D8_OUTPUT,  "/8");
        configOutput(D16_OUTPUT, "/16");
    }

    void onReset() override {
        counter = 0;
        clockTrig.reset();
    }

    void process(const ProcessArgs&) override {
        if (clockTrig.process(inputs[CLK_INPUT].getVoltage(), kGateLow, kGateHigh))
            counter = (counter + 1) & 15;       // 0..15

        static const int kDiv[4] = { 2, 4, 8, 16 };
        for (int i = 0; i < 4; ++i) {
            int d = kDiv[i];
            bool high = (counter % d) < (d / 2);
            outputs[D2_OUTPUT + i].setVoltage(high ? 10.f : 0.f);
            outputs[D2_OUTPUT + i].channels = 1;
        }
    }
};

//============================================================================
//  8-Step Seq -- classic step sequencer.  CLOCK advances the step, RESET
//  returns to step 0, CV out is the active step's knob, GATE passes the clock.
//============================================================================
//  Sixteen independent sequencers in one module.  A shared CLOCK advances every
//  track's playhead (RESET returns each to its own start step).  Each of the 16
//  tracks has its OWN start/end selectors, so tracks loop over different ranges
//  and drift against each other (polymeter).  Per step, each track has a VALUE
//  knob (0..10V CV, default midway), a GATE toggle (default off), and a tiny
//  PROBABILITY knob (0..100%, default 50%) that gates how often an enabled step
//  actually fires.  Each step's LED reads GREEN when its gate is enabled and
//  flips RED on the step currently under the playhead.  CV and GATE are
//  polyphonic 16-channel outputs -- channel t carries track t.
struct SEQ8 : Module {
    static constexpr int TRACKS = 16;
    static constexpr int STEPS  = 16;

    enum ParamIds  {
        VALUE_PARAM,                                    // TRACKS*STEPS CV knobs
        GATE_PARAM  = VALUE_PARAM + TRACKS * STEPS,     // TRACKS*STEPS gate toggles
        PROB_PARAM  = GATE_PARAM  + TRACKS * STEPS,     // TRACKS*STEPS probability knobs
        START_PARAM = PROB_PARAM  + TRACKS * STEPS,     // per-track loop start 1..16
        END_PARAM   = START_PARAM + TRACKS,             // per-track loop end   1..16
        NUM_PARAMS  = END_PARAM   + TRACKS
    };
    enum InputIds  { CLOCK_INPUT, RESET_INPUT, NUM_INPUTS };
    enum OutputIds {
        CV_OUTPUT,                                  // per-track CV out  (16 mono)
        GATE_OUTPUT = CV_OUTPUT + TRACKS,           // per-track gate out (16 mono)
        NUM_OUTPUTS = GATE_OUTPUT + TRACKS
    };
    enum LightIds  { STEP_LIGHT, NUM_LIGHTS = STEP_LIGHT + TRACKS * STEPS };  // per-cell LED

    static int valueParam(int track, int step) { return VALUE_PARAM + track * STEPS + step; }
    static int gateParam(int track, int step)  { return GATE_PARAM + track * STEPS + step; }
    static int probParam(int track, int step)  { return PROB_PARAM + track * STEPS + step; }
    static int cellLight(int track, int step)  { return STEP_LIGHT + track * STEPS + step; }
    static int cvOut(int track)                { return CV_OUTPUT + track; }
    static int gateOut(int track)              { return GATE_OUTPUT + track; }

    int index[TRACKS] = {};
    bool gateLatched[TRACKS] = {};          // did this step win its probability roll?
    std::uint32_t rngState = 0x9e3779b9u;
    rack::dsp::SchmittTrigger clockTrig, resetTrig;
    rack::dsp::SequencerReset sequencerReset;

    SEQ8() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        // Vary the RNG seed per instance so stacked sequencers don't roll in
        // lock-step (no wall-clock needed -- just an order-dependent salt).
        static std::uint32_t seedSalt = 0x12345677u;
        seedSalt += 0x9e3779b9u;
        rngState = seedSalt | 1u;
        for (int track = 0; track < TRACKS; ++track) {
            for (int step = 0; step < STEPS; ++step) {
                // Defaults: value midway (5V), gates off, probability 100%.
                configParam(valueParam(track, step), 0.f, 10.f, 5.f);
                configSwitch(gateParam(track, step), 0.f, 1.f, 0.f, "");
                configParam(probParam(track, step), 0.f, 1.f, 1.f, "Prob", "%", 0.f, 100.f);
                configLight(cellLight(track, step), "");
            }
            auto* start = configParam(START_PARAM + track, 1.f, float(STEPS), 1.f, "Start");
            if (start) start->snapEnabled = true;
            auto* end = configParam(END_PARAM + track, 1.f, float(STEPS), float(STEPS), "End");
            if (end) end->snapEnabled = true;
        }
        configInput(CLOCK_INPUT, "Clk");
        configInput(RESET_INPUT, "Rst");
        for (int track = 0; track < TRACKS; ++track) {
            configOutput(cvOut(track), stepLabel(track + 1));
            configOutput(gateOut(track), stepLabel(track + 1));
        }
    }

    // xorshift32 -> [0, 1); one roll per step entry keeps the gate stable for
    // the whole step instead of flickering per sample.
    float nextRandom() {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return float(rngState & 0xffffffu) / float(0x1000000);
    }

    // A track loops over [lo, hi] (inclusive, 0-based); the selectors are
    // 1-based and order-agnostic (start > end still loops the span).
    void trackRange(int track, int& lo, int& hi) const {
        int a = int(std::lround(params[START_PARAM + track].getValue())) - 1;
        int b = int(std::lround(params[END_PARAM + track].getValue())) - 1;
        a = a < 0 ? 0 : (a > STEPS - 1 ? STEPS - 1 : a);
        b = b < 0 ? 0 : (b > STEPS - 1 ? STEPS - 1 : b);
        lo = a < b ? a : b;
        hi = a < b ? b : a;
    }

    // Roll the probability for a track's freshly-entered step.
    void latchGate(int track, int position) {
        const bool gateOn = params[gateParam(track, position)].getValue() >= 0.5f;
        const float prob = params[probParam(track, position)].getValue();
        gateLatched[track] = gateOn && nextRandom() < prob;
    }

    void onReset() override {
        for (int track = 0; track < TRACKS; ++track) {
            int lo, hi; trackRange(track, lo, hi);
            index[track] = lo;
            latchGate(track, lo);
        }
        clockTrig.reset();
        resetTrig.reset();
        sequencerReset.reset();
    }

    void process(const ProcessArgs&) override {
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), kGateLow, kGateHigh)) {
            for (int track = 0; track < TRACKS; ++track) {
                int lo, hi; trackRange(track, lo, hi);
                index[track] = lo;
                latchGate(track, lo);
            }
            clockTrig.reset();
            sequencerReset.reset();
        }
        const bool clock = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), kGateLow, kGateHigh);
        const bool advance = sequencerReset.processClock(clock);
        const bool clockHigh = clockTrig.isHigh();

        for (int track = 0; track < TRACKS; ++track) {
            int lo, hi; trackRange(track, lo, hi);
            int position = index[track];
            bool stepped = false;
            if (advance) {
                position = (position < lo || position >= hi) ? lo : position + 1;
                stepped = true;
            }
            else if (position < lo || position > hi) {        // range moved live
                position = lo;
                stepped = true;
            }
            index[track] = position;
            if (stepped) latchGate(track, position);

            const float cv = params[valueParam(track, position)].getValue();
            outputs[cvOut(track)].setVoltage(cv);
            outputs[cvOut(track)].channels = 1;
            outputs[gateOut(track)].setVoltage(gateLatched[track] && clockHigh ? 10.f : 0.f);
            outputs[gateOut(track)].channels = 1;

            // LED: RED on the playhead, GREEN on an enabled step, otherwise a
            // dim in-range dot (off outside the loop range).
            for (int step = 0; step < STEPS; ++step) {
                const bool gateOn = params[gateParam(track, step)].getValue() >= 0.5f;
                const bool inRange = step >= lo && step <= hi;
                if (step == position)
                    lights[cellLight(track, step)].setBrightnessRGB(1.f, 0.06f, 0.06f);
                else if (gateOn)
                    lights[cellLight(track, step)].setBrightnessRGB(0.05f, 0.85f, 0.15f);
                else {
                    const float dim = inRange ? 0.05f : 0.f;
                    lights[cellLight(track, step)].setBrightnessRGB(dim, dim, dim);
                }
            }
        }
    }
};

//============================================================================
//  S&H -- sample & hold: latch IN on a rising TRIG, hold it on OUT.
//============================================================================
struct SampHold : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { IN_INPUT, TRIG_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    float held[rack::engine::PORT_MAX_CHANNELS] = {};
    rack::dsp::SchmittTrigger trig[rack::engine::PORT_MAX_CHANNELS];

    SampHold() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(IN_INPUT, "In");
        configInput(TRIG_INPUT, "Trg");
        configOutput(OUT_OUTPUT, "Out");
    }

    void process(const ProcessArgs&) override {
        // Channels follow max(IN, TRIG); each channel holds its own value and
        // has its own trigger detector.
        int chans = std::max(std::max(1, inputs[IN_INPUT].getChannels()),
                             inputs[TRIG_INPUT].getChannels());
        for (int c = 0; c < chans; ++c) {
            if (trig[c].process(inputs[TRIG_INPUT].getPolyVoltage(c), kGateLow, kGateHigh))
                held[c] = inputs[IN_INPUT].getPolyVoltage(c);
            outputs[OUT_OUTPUT].setVoltage(held[c], c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Quantizer -- snap incoming pitch CV to the nearest semitone (chromatic).
//============================================================================
struct Quantizer : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Quantizer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(IN_INPUT, "In");
        configOutput(OUT_OUTPUT, "Out");
    }

    void process(const ProcessArgs&) override {
        // Channels follow the input; snap each channel independently.
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        for (int c = 0; c < chans; ++c) {
            float in = inputs[IN_INPUT].getPolyVoltage(c);
            outputs[OUT_OUTPUT].setVoltage(std::round(in * 12.f) / 12.f, c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Attenuverter -- two independent bipolar gain stages (negative = invert).
//============================================================================
struct Atten : Module {
    enum ParamIds  { G1_PARAM, G2_PARAM, NUM_PARAMS };
    enum InputIds  { IN1_INPUT, IN2_INPUT, NUM_INPUTS };
    enum OutputIds { OUT1_OUTPUT, OUT2_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Atten() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(G1_PARAM, -1.f, 1.f, 1.f, "G1");
        configParam(G2_PARAM, -1.f, 1.f, 1.f, "G2");
        configInput(IN1_INPUT, "1");
        configInput(IN2_INPUT, "2");
        configOutput(OUT1_OUTPUT, "1");
        configOutput(OUT2_OUTPUT, "2");
    }

    void process(const ProcessArgs&) override {
        // Two independent stages; each output follows its own input's channels.
        for (int i = 0; i < 2; ++i) {
            int chans = std::max(1, inputs[IN1_INPUT + i].getChannels());
            for (int c = 0; c < chans; ++c) {
                outputs[OUT1_OUTPUT + i].setVoltage(inputs[IN1_INPUT + i].getPolyVoltage(c)
                                                    * params[G1_PARAM + i].getValue(), c);
            }
            outputs[OUT1_OUTPUT + i].setChannels(chans);
        }
    }
};

//============================================================================
//  Slew -- portamento / lag.  Each knob 0..1 maps (exponentially) to a slew
//  rate: 0 = near-instant, 1 = very slow.  Separate rise & fall.
//============================================================================
struct Slew : Module {
    enum ParamIds  { RISE_PARAM, FALL_PARAM, NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    rack::dsp::SlewLimiter slew[rack::engine::PORT_MAX_CHANNELS];
    // knob -> rate memo: pow only when a knob actually moves (exact-equality
    // key, bit-identical to recomputing per sample).
    float lastKnob[NUM_PARAMS];
    float rateMemo[NUM_PARAMS] = {};

    Slew() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(RISE_PARAM, 0.f, 1.f, 0.f, "Ris");
        configParam(FALL_PARAM, 0.f, 1.f, 0.f, "Fal");
        configInput(IN_INPUT, "In");
        configOutput(OUT_OUTPUT, "Out");
        for (int i = 0; i < NUM_PARAMS; ++i)
            lastKnob[i] = std::numeric_limits<float>::quiet_NaN();
    }

    // knob 0..1 -> volts/second, fast (1e5) down to slow (1).
    float rateOf(int paramId) {
        float knob = params[paramId].getValue();
        if (knob != lastKnob[paramId]) {
            lastKnob[paramId] = knob;
            const float fast = 1e5f, slow = 1.f;
            rateMemo[paramId] = fast * std::pow(slow / fast, rack::clamp(knob, 0.f, 1.f));
        }
        return rateMemo[paramId];
    }

    void process(const ProcessArgs& args) override {
        // Channels follow the input; each channel has its own slew state.  The
        // rise/fall rates come from shared knobs.
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        float rise = rateOf(RISE_PARAM);
        float fall = rateOf(FALL_PARAM);
        for (int c = 0; c < chans; ++c) {
            slew[c].setRiseFall(rise, fall);
            outputs[OUT_OUTPUT].setVoltage(slew[c].process(args.sampleTime,
                                                           inputs[IN_INPUT].getPolyVoltage(c)), c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Multiple -- 1-in, 4-out buffered signal splitter.
//============================================================================
struct Mult : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { O1_OUTPUT, O2_OUTPUT, O3_OUTPUT, O4_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Mult() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(IN_INPUT, "In");
        configOutput(O1_OUTPUT, "1");
        configOutput(O2_OUTPUT, "2");
        configOutput(O3_OUTPUT, "3");
        configOutput(O4_OUTPUT, "4");
    }

    void process(const ProcessArgs&) override {
        // Copy the input's channel count and per-channel values to every output.
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        for (int c = 0; c < chans; ++c) {
            float in = inputs[IN_INPUT].getPolyVoltage(c);
            for (int i = 0; i < 4; ++i)
                outputs[O1_OUTPUT + i].setVoltage(in, c);
        }
        for (int i = 0; i < 4; ++i)
            outputs[O1_OUTPUT + i].setChannels(chans);
    }
};

//============================================================================
//  Delay -- interpolated feedback delay, per polyphony channel.  ONE flat ring
//  buffer (PORT_MAX_CHANNELS * kBufSize) is allocated in the ctor; channel c
//  owns the slice [c*kBufSize, (c+1)*kBufSize).  The per-channel max delay is
//  capped at 1s @ 96k so 16 channels cost 16 * ~96k floats ~= 6MB (keeping the
//  original 2s would be ~48MB across 16 channels).  TIME maps 1ms..1s, FB feeds
//  back the read (clamped < 0.99 and hard-limited to +/-10V so it never blows
//  up), MIX crossfades dry/wet.  Channels follow inputs[IN].
//============================================================================
struct Delay : Module {
    enum ParamIds  { TIME_PARAM, FB_PARAM, MIX_PARAM, NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    static constexpr int kBufSize = 96000 + 4;    // per channel: 1s @ 96k +interp
    std::vector<float> buffer;                    // PORT_MAX_CHANNELS * kBufSize
    int writeIdx = 0;                             // shared: one sample/ch per call

    Delay() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(TIME_PARAM, 0.f, 1.f, 0.3f, "Time", " s");
        configParam(FB_PARAM,   0.f, 1.f, 0.3f, "FB");
        configParam(MIX_PARAM,  0.f, 1.f, 0.5f, "Mix");
        configInput(IN_INPUT, "In");
        configOutput(OUT_OUTPUT, "Out");
        buffer.resize((size_t)rack::engine::PORT_MAX_CHANNELS * kBufSize, 0.f);  // one-time alloc
    }

    void process(const ProcessArgs& args) override {
        const int size = kBufSize;                // per-channel ring length
        int chans = std::max(1, inputs[IN_INPUT].getChannels());

        // TIME knob -> delay length in samples (bounded so interpolation stays
        // in-bounds regardless of sample rate).  Channel-independent (no CV).
        float delaySec = rack::rescale(rack::clamp(params[TIME_PARAM].getValue(), 0.f, 1.f),
                                       0.f, 1.f, 0.001f, 1.f);
        float delaySamples = rack::clamp(delaySec * args.sampleRate, 1.f, (float)(size - 2));
        float fb  = rack::clamp(params[FB_PARAM].getValue(),  0.f, 0.99f);
        float mix = rack::clamp(params[MIX_PARAM].getValue(), 0.f, 1.f);

        for (int c = 0; c < chans; ++c) {
            float* buf = &buffer[(size_t)c * size];   // this channel's ring slice

            // Linear-interpolated read, delaySamples behind the write head.
            float readPos = (float)writeIdx - delaySamples;
            while (readPos < 0.f) readPos += (float)size;
            int i0 = (int)readPos;
            float frac = readPos - (float)i0;
            int i1 = i0 + 1; if (i1 >= size) i1 -= size;
            float delayed = buf[i0] + (buf[i1] - buf[i0]) * frac;

            float in = inputs[IN_INPUT].getPolyVoltage(c);

            // Write dry + feedback; guard against NaN and hard-limit the tail.
            float w = rack::math::normalizeZero(in + fb * delayed);
            buf[writeIdx] = rack::clamp(w, -10.f, 10.f);

            outputs[OUT_OUTPUT].setVoltage(rack::crossfade(in, delayed, mix), c);
        }
        // One sample elapsed for every channel: advance the shared head once.
        if (++writeIdx >= size) writeIdx = 0;
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Waveshaper -- tanh soft-clip drive.  Unity at low drive for small signals;
//  higher DRIVE saturates harder.
//============================================================================
struct Shaper : Module {
    enum ParamIds  { DRIVE_PARAM, NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Shaper() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(DRIVE_PARAM, 1.f, 20.f, 1.f, "Drv");
        configInput(IN_INPUT, "In");
        configOutput(OUT_OUTPUT, "Out");
    }

    void process(const ProcessArgs&) override {
        // Channels follow the input; shared DRIVE knob applied per channel.
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        float drive = params[DRIVE_PARAM].getValue();
        for (int c = 0; c < chans; ++c) {
            float in = inputs[IN_INPUT].getPolyVoltage(c);
            outputs[OUT_OUTPUT].setVoltage(std::tanh(in / 5.f * drive) * 5.f, c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Wavefolder -- FOLD drives the signal into repeated sine folds.  Output is
//  bounded to +/-5V (each sine stage already maps into [-1, 1]).
//============================================================================
struct Folder : Module {
    enum ParamIds  { FOLD_PARAM, NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Folder() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(FOLD_PARAM, 0.f, 1.f, 0.f, "Fold");
        configInput(IN_INPUT, "In");
        configOutput(OUT_OUTPUT, "Out");
    }

    void process(const ProcessArgs&) override {
        // Channels follow the input; shared FOLD knob applied per channel.
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        float fold = rack::clamp(params[FOLD_PARAM].getValue(), 0.f, 1.f);
        for (int c = 0; c < chans; ++c) {
            float x = inputs[IN_INPUT].getPolyVoltage(c) / 5.f * (1.f + fold * 4.f);
            for (int k = 0; k < 3; ++k)
                x = std::sin(x * kPi * 0.5f);
            outputs[OUT_OUTPUT].setVoltage(rack::clamp(x * 5.f, -5.f, 5.f), c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Ring Mod -- four-quadrant multiplier, A * B / 5V.
//============================================================================
struct RingMod : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { A_INPUT, B_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    RingMod() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(A_INPUT, "A");
        configInput(B_INPUT, "B");
        configOutput(OUT_OUTPUT, "Out");
    }

    void process(const ProcessArgs&) override {
        // Four-quadrant multiply; channels follow max(A, B).
        int chans = std::max(1, std::max(inputs[A_INPUT].getChannels(),
                                         inputs[B_INPUT].getChannels()));
        for (int c = 0; c < chans; ++c) {
            outputs[OUT_OUTPUT].setVoltage(inputs[A_INPUT].getPolyVoltage(c)
                                           * inputs[B_INPUT].getPolyVoltage(c) / 5.f, c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Panner -- equal-power stereo pan, knob + CV.
//============================================================================
struct Pan : Module {
    enum ParamIds  { PAN_PARAM, NUM_PARAMS };
    enum InputIds  { IN_INPUT, PAN_CV_INPUT, NUM_INPUTS };
    enum OutputIds { L_OUTPUT, R_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Pan() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PAN_PARAM, -1.f, 1.f, 0.f, "Pan");
        configInput(IN_INPUT, "In");
        configInput(PAN_CV_INPUT, "CV");
        configOutput(L_OUTPUT, "L");
        configOutput(R_OUTPUT, "R");
    }

    void process(const ProcessArgs&) override {
        // Channels follow the input; L and R carry the same channel count.
        // PAN CV is read per channel (a mono CV broadcasts).
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        for (int c = 0; c < chans; ++c) {
            float p = rack::clamp(params[PAN_PARAM].getValue()
                                  + inputs[PAN_CV_INPUT].getPolyVoltage(c) / 5.f, -1.f, 1.f);
            float a = (p + 1.f) * 0.5f;
            float in = inputs[IN_INPUT].getPolyVoltage(c);
            outputs[L_OUTPUT].setVoltage(in * std::cos(a * kPi * 0.5f), c);
            outputs[R_OUTPUT].setVoltage(in * std::sin(a * kPi * 0.5f), c);
        }
        outputs[L_OUTPUT].setChannels(chans);
        outputs[R_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Crossfader -- blend A and B by MIX (knob + CV).
//============================================================================
struct XFade : Module {
    enum ParamIds  { MIX_PARAM, NUM_PARAMS };
    enum InputIds  { A_INPUT, B_INPUT, MIX_CV_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    XFade() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix");
        configInput(A_INPUT, "A");
        configInput(B_INPUT, "B");
        configInput(MIX_CV_INPUT, "CV");
        configOutput(OUT_OUTPUT, "Out");
    }

    void process(const ProcessArgs&) override {
        // Channels follow max(A, B); MIX CV is read per channel (mono broadcasts).
        int chans = std::max(1, std::max(inputs[A_INPUT].getChannels(),
                                         inputs[B_INPUT].getChannels()));
        for (int c = 0; c < chans; ++c) {
            float mix = rack::clamp(params[MIX_PARAM].getValue()
                                    + inputs[MIX_CV_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);
            outputs[OUT_OUTPUT].setVoltage(rack::crossfade(inputs[A_INPUT].getPolyVoltage(c),
                                                           inputs[B_INPUT].getPolyVoltage(c), mix), c);
        }
        outputs[OUT_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Random -- sample a new random value on each TRIG.  UNI = 0..10V,
//  BI = -5..+5V (independent draws), both held until the next trigger.
//============================================================================
struct RandCV : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { TRIG_INPUT, NUM_INPUTS };
    enum OutputIds { UNI_OUTPUT, BI_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    uint32_t rngState[rack::engine::PORT_MAX_CHANNELS];
    float uni[rack::engine::PORT_MAX_CHANNELS] = {};
    float bi[rack::engine::PORT_MAX_CHANNELS]  = {};
    rack::dsp::SchmittTrigger trig[rack::engine::PORT_MAX_CHANNELS];

    RandCV() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(TRIG_INPUT, "Trg");
        configOutput(UNI_OUTPUT, "Uni");
        configOutput(BI_OUTPUT, "Bi");
        // Per-instance, per-channel seed: decorrelates modules AND channels.
        // Never 0.
        for (int c = 0; c < rack::engine::PORT_MAX_CHANNELS; ++c) {
            rngState[c] = 0x9E3779B9u ^ (uint32_t)(uintptr_t)this
                        ^ (0x85EBCA6Bu * (uint32_t)(c + 1));
            if (rngState[c] == 0u) rngState[c] = 1u + (uint32_t)c;
        }
    }

    inline uint32_t nextRandom(int c) {
        uint32_t x = rngState[c];
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rngState[c] = x;
        return x;
    }
    inline float nextUnit(int c) { return (float)nextRandom(c) * (1.f / 4294967296.f); }  // [0,1)

    void process(const ProcessArgs&) override {
        // Channels follow the trigger input; each channel draws independently.
        int chans = std::max(1, inputs[TRIG_INPUT].getChannels());
        for (int c = 0; c < chans; ++c) {
            if (trig[c].process(inputs[TRIG_INPUT].getPolyVoltage(c), kGateLow, kGateHigh)) {
                uni[c] = nextUnit(c) * 10.f;
                bi[c]  = nextUnit(c) * 10.f - 5.f;
            }
            outputs[UNI_OUTPUT].setVoltage(uni[c], c);
            outputs[BI_OUTPUT].setVoltage(bi[c], c);
        }
        outputs[UNI_OUTPUT].setChannels(chans);
        outputs[BI_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  Comparator -- HI = 10V when IN > THRESH (else 0), LO is the complement.
//============================================================================
struct Compare : Module {
    enum ParamIds  { THRESH_PARAM, NUM_PARAMS };
    enum InputIds  { IN_INPUT, NUM_INPUTS };
    enum OutputIds { HI_OUTPUT, LO_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    Compare() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(THRESH_PARAM, -5.f, 5.f, 0.f, "Thr");
        configInput(IN_INPUT, "In");
        configOutput(HI_OUTPUT, "Hi");
        configOutput(LO_OUTPUT, "Lo");
    }

    void process(const ProcessArgs&) override {
        // Channels follow the input; compare each channel to the shared thresh.
        int chans = std::max(1, inputs[IN_INPUT].getChannels());
        float thresh = params[THRESH_PARAM].getValue();
        for (int c = 0; c < chans; ++c) {
            bool hi = inputs[IN_INPUT].getPolyVoltage(c) > thresh;
            outputs[HI_OUTPUT].setVoltage(hi ? 10.f : 0.f, c);
            outputs[LO_OUTPUT].setVoltage(hi ? 0.f : 10.f, c);
        }
        outputs[HI_OUTPUT].setChannels(chans);
        outputs[LO_OUTPUT].setChannels(chans);
    }
};

//============================================================================
//  AD Envelope -- non-sustaining attack/decay.  A rising TRIG restarts the
//  attack (0->10V over ATT), then it decays back to 0 over DEC.  LED follows.
//============================================================================
struct ADEnv : Module {
    enum ParamIds  { ATT_PARAM, DEC_PARAM, NUM_PARAMS };
    enum InputIds  { TRIG_INPUT, NUM_INPUTS };
    enum OutputIds { ENV_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { ENV_LIGHT, NUM_LIGHTS };

    enum Stage { STAGE_IDLE, STAGE_ATTACK, STAGE_DECAY };

    rack::dsp::SchmittTrigger trig[rack::engine::PORT_MAX_CHANNELS];
    float env[rack::engine::PORT_MAX_CHANNELS]   = {};   // normalized 0..1, per channel
    int   stage[rack::engine::PORT_MAX_CHANNELS] = {};   // STAGE_IDLE == 0

    // knob -> time memo: pow only when a knob actually moves (same pattern as
    // ADSR/Slew; the envelope math is unchanged).
    float lastKnob[NUM_PARAMS];
    float timeMemo[NUM_PARAMS] = {};

    ADEnv() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(ATT_PARAM, 0.f, 1.f, 0.1f, "Att");
        configParam(DEC_PARAM, 0.f, 1.f, 0.4f, "Dec");
        configInput(TRIG_INPUT, "Trg");
        configOutput(ENV_OUTPUT, "Env");
        configLight(ENV_LIGHT, "Env");
        for (int i = 0; i < NUM_PARAMS; ++i)
            lastKnob[i] = std::numeric_limits<float>::quiet_NaN();
    }

    // knob 0..1 -> segment time in seconds (1ms .. 5s), exponential.
    float timeOf(int paramId) {
        float knob = params[paramId].getValue();
        if (knob != lastKnob[paramId]) {
            lastKnob[paramId] = knob;
            timeMemo[paramId] = 0.001f * std::pow(5000.f, rack::clamp(knob, 0.f, 1.f));
        }
        return timeMemo[paramId];
    }

    void process(const ProcessArgs& args) override {
        // Channels follow the trigger input; each channel keeps its own stage.
        int chans = std::max(1, inputs[TRIG_INPUT].getChannels());
        for (int c = 0; c < chans; ++c) {
            if (trig[c].process(inputs[TRIG_INPUT].getPolyVoltage(c), kGateLow, kGateHigh))
                stage[c] = STAGE_ATTACK;

            if (stage[c] == STAGE_ATTACK) {
                env[c] += args.sampleTime / timeOf(ATT_PARAM);
                if (env[c] >= 1.f) { env[c] = 1.f; stage[c] = STAGE_DECAY; }
            }
            else if (stage[c] == STAGE_DECAY) {
                env[c] -= args.sampleTime / timeOf(DEC_PARAM);
                if (env[c] <= 0.f) { env[c] = 0.f; stage[c] = STAGE_IDLE; }
            }
            else {
                env[c] = 0.f;
            }

            env[c] = rack::clamp(env[c], 0.f, 1.f);
            outputs[ENV_OUTPUT].setVoltage(env[c] * 10.f, c);
        }
        outputs[ENV_OUTPUT].setChannels(chans);
        lights[ENV_LIGHT].setBrightness(env[0]);   // LED follows channel 0
    }
};

//============================================================================
//  LFO2 -- compact low-frequency oscillator: sine + square, bipolar +/-5V.
//============================================================================
struct LFO2 : Module {
    enum ParamIds  { RATE_PARAM, NUM_PARAMS };
    enum InputIds  { NUM_INPUTS };
    enum OutputIds { SIN_OUTPUT, SQR_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { SIN_LIGHT, NUM_LIGHTS };

    rack::dsp::BlepOsc osc;
    // rate -> freq memo: pow only when the knob (or sample rate) changes.
    float lastRate = std::numeric_limits<float>::quiet_NaN();
    float lastSr   = -1.f;
    float freqMemo = 0.f;

    LFO2() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(RATE_PARAM, -8.f, 10.f, 1.f, "Rate");   // 2^rate Hz
        configOutput(SIN_OUTPUT, "Sin");
        configOutput(SQR_OUTPUT, "Sqr");
        configLight(SIN_LIGHT, "Sin");
    }

    void process(const ProcessArgs& args) override {
        float rate = params[RATE_PARAM].getValue();
        if (rate != lastRate || args.sampleRate != lastSr) {
            lastRate = rate;
            lastSr   = args.sampleRate;
            freqMemo = rack::clamp(std::pow(2.f, rate), 0.f, args.sampleRate * 0.49f);
        }
        float dt = freqMemo * args.sampleTime;

        osc.advance(dt);
        float s = osc.sine();
        outputs[SIN_OUTPUT].setVoltage(5.f * s);
        outputs[SQR_OUTPUT].setVoltage(5.f * osc.square(dt));
        outputs[SIN_OUTPUT].channels = 1;
        outputs[SQR_OUTPUT].channels = 1;
        lights[SIN_LIGHT].setBrightness(0.5f + 0.5f * s);
    }
};

} // anonymous namespace

//============================================================================
//  Registration -- one addType() per module (stable slug, role, category).
//============================================================================
namespace rackx {

void registerBuiltinModulesExt() {
    addType("Clock", "Clock", "Clock", Role::Normal,
            [] { return std::make_unique<Clock>(); }, clockPanel());
    addType("ClockDiv", "Clock Div", "Clock", Role::Normal,
            [] { return std::make_unique<ClockDiv>(); }, {}, false);
    addType("SEQ8", "16×16 Step Seq", "Sequencer", Role::Normal,
            [] { return std::make_unique<SEQ8>(); }, seq8Panel());
    addType("SampHold", "S&H", "Utility", Role::Normal,
            [] { return std::make_unique<SampHold>(); });
    addType("Quantizer", "Quantizer", "Pitch", Role::Normal,
            [] { return std::make_unique<Quantizer>(); });
    addType("Atten", "Attenuverter", "Utility", Role::Normal,
            [] { return std::make_unique<Atten>(); });
    addType("Slew", "Slew", "Utility", Role::Normal,
            [] { return std::make_unique<Slew>(); });
    addType("Mult", "Multiple", "Utility", Role::Normal,
            [] { return std::make_unique<Mult>(); });
    addType("Delay", "Delay", "Effect", Role::Normal,
            [] { return std::make_unique<Delay>(); });
    addType("Shaper", "Waveshaper", "Effect", Role::Normal,
            [] { return std::make_unique<Shaper>(); });
    addType("Folder", "Wavefolder", "Effect", Role::Normal,
            [] { return std::make_unique<Folder>(); });
    addType("RingMod", "Ring Mod", "Effect", Role::Normal,
            [] { return std::make_unique<RingMod>(); });
    addType("Pan", "Panner", "Mix", Role::Normal,
            [] { return std::make_unique<Pan>(); });
    addType("XFade", "Crossfader", "Mix", Role::Normal,
            [] { return std::make_unique<XFade>(); });
    addType("RandCV", "Random", "Random", Role::Normal,
            [] { return std::make_unique<RandCV>(); });
    addType("Compare", "Comparator", "Logic", Role::Normal,
            [] { return std::make_unique<Compare>(); });
    addType("ADEnv", "AD Envelope", "Envelope", Role::Normal,
            [] { return std::make_unique<ADEnv>(); });
    addType("LFO2", "LFO2", "Modulation", Role::Normal,
            [] { return std::make_unique<LFO2>(); });
}

} // namespace rackx
