//----------------------------------------------------------------------------
//  src/engine/rack/rack_acid_sequencer.cpp
//
//  "303 Sequencer" -- the bassline-box step sequencer, faceplate and all.
//
//  The layout copies the original silver box (an entry keyboard over a lit step
//  grid, with ACCENT / SLIDE / octave pads beside the keys) but carries none of
//  its branding.  Behaviour is what makes that machine sound like itself:
//
//    * ACCENT  raises the accent CV for the step; patch it into the filter's
//      ACCENT jack and it opens the cutoff and pushes the drive, exactly as the
//      hardware's accent bus does.
//    * SLIDE   glides the pitch into the NEXT step over a fixed short time and
//      holds the gate high across the boundary, so the two notes are legato and
//      the envelope never retriggers.  That un-retriggered, gliding note is the
//      whole acid idiom.
//    * REST    a step with GATE off emits no gate at all (the pitch still
//      advances), which is how the pattern gets its syncopation.
//
//  Editing follows the hardware: press a STEP pad to select it, then a key on
//  the keyboard writes that note into it (and un-rests it).  DOWN / UP shift the
//  selected step an octave.  ACCENT / SLIDE beside the keyboard toggle those
//  flags for the selected step, and the grid rows do the same directly.
//
//  CLOCK / RESET match the other sequencer modules (rising edge advances, RESET
//  returns to step 1 and swallows the first clock so the two stay in phase).
//----------------------------------------------------------------------------
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include "rack.hpp"
#include "rack_dsp.h"
#include "rack_factory.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {

using rack::engine::Module;

constexpr int   kSteps    = 16;
//! sinceClock parked here means "gate closed, no clock seen yet".
constexpr long  kGateClosed = 1L << 30;
constexpr float kGateLow  = 0.1f;
constexpr float kGateHigh = 2.0f;

// Keyboard geometry: the five sharps are declared FIRST so the editor's
// hit-test (which walks params in index order) reaches them before the naturals
// they overlap.  Drawing runs the same order, and the sharps are painted after
// the naturals because the panel lists them later -- see acidSeqPanel().
constexpr int kSharpSemis[5]   = { 1, 3, 6, 8, 10 };            // C# D# F# G# A#
constexpr int kNaturalSemis[7] = { 0, 2, 4, 5, 7, 9, 11 };      // C D E F G A B

// Note range: one keyboard octave x three, matching the hardware's keyboard
// plus its DOWN/UP octave buttons.
constexpr int kMaxNote = 36;

struct AcidSequencer final : Module {
    enum ParamIds {
        SHARP_PARAM,                            // 5  momentary black keys
        NATURAL_PARAM = SHARP_PARAM   + 5,      // 7  momentary white keys
        PITCH_PARAM   = NATURAL_PARAM + 7,      // 16 semitone boxes
        GATE_PARAM    = PITCH_PARAM   + kSteps, // 16 note / rest
        ACCENT_PARAM  = GATE_PARAM    + kSteps, // 16 accent flags
        SLIDE_PARAM   = ACCENT_PARAM  + kSteps, // 16 slide flags
        SELECT_PARAM  = SLIDE_PARAM   + kSteps, // 16 momentary "edit this step"
        LAST_PARAM    = SELECT_PARAM  + kSteps, // pattern length 1..16
        OCTDOWN_PARAM, OCTUP_PARAM,             // momentary octave shift
        KACCENT_PARAM, KSLIDE_PARAM,            // keyboard-side flag pads
        TUNE_PARAM,                             // fine tune, +/- 1 semitone
        TRANSPOSE_PARAM,                        // octaves, -2..+2
        GATELEN_PARAM,                          // gate as a fraction of a step
        SLIDETIME_PARAM,                        // glide time, 5..250 ms
        ACCENTAMT_PARAM,                        // accent CV depth
        RUN_PARAM,                              // run / stop latch
        NUM_PARAMS
    };
    enum InputIds  { CLOCK_INPUT, RESET_INPUT, NUM_INPUTS };
    enum OutputIds { CV_OUTPUT, GATE_OUTPUT, ACCENT_OUTPUT, SLIDE_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { STEP_LIGHT, NUM_LIGHTS = STEP_LIGHT + kSteps };

    static int pitchParam (int step) { return PITCH_PARAM  + step; }
    static int gateParam  (int step) { return GATE_PARAM   + step; }
    static int accentParam(int step) { return ACCENT_PARAM + step; }
    static int slideParam (int step) { return SLIDE_PARAM  + step; }
    static int selectParam(int step) { return SELECT_PARAM + step; }

    int   index    = 0;      // playhead
    int   selected = 0;      // step the keyboard writes into
    float cv       = 0.f;    // current (possibly gliding) pitch, volts
    float target   = 0.f;    // pitch this step is heading to
    bool  gliding  = false;  // the previous step asked to slide into this one
    bool  gateHeld  = false; // gate currently high
    long  sinceClock = 0;    // samples since the last clock edge
    long  stepLen    = 0;    // measured samples per step (0 = not yet known)

    rack::dsp::SchmittTrigger clockTrig, resetTrig;
    rack::dsp::SequencerReset sequencerReset;

    AcidSequencer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        for (int k = 0; k < 5; ++k)
            configSwitch(SHARP_PARAM + k, 0.f, 1.f, 0.f, "");
        for (int k = 0; k < 7; ++k)
            configSwitch(NATURAL_PARAM + k, 0.f, 1.f, 0.f, "");

        for (int step = 0; step < kSteps; ++step) {
            const std::string n = std::to_string(step + 1);
            // Three octaves of semitones, snapped: the hardware's one-octave
            // keyboard plus its DOWN/UP buttons spans exactly this much.  The
            // box reads as a note number and the keyboard writes into it.
            auto* pitch = configParam(pitchParam(step), 0.f, float(kMaxNote), 12.f,
                                      "Note " + n);
            if (pitch) pitch->snapEnabled = true;
            configSwitch(gateParam(step),   0.f, 1.f, step % 4 == 0 ? 1.f : 0.f, "Gate " + n);
            configSwitch(accentParam(step), 0.f, 1.f, 0.f, "Accent " + n);
            configSwitch(slideParam(step),  0.f, 1.f, 0.f, "Slide " + n);
            configSwitch(selectParam(step), 0.f, 1.f, 0.f, "");
            configLight(STEP_LIGHT + step, "");
        }

        auto* last = configParam(LAST_PARAM, 1.f, float(kSteps), float(kSteps), "Last step");
        if (last) last->snapEnabled = true;
        configSwitch(OCTDOWN_PARAM, 0.f, 1.f, 0.f, "");
        configSwitch(OCTUP_PARAM,   0.f, 1.f, 0.f, "");
        configSwitch(KACCENT_PARAM, 0.f, 1.f, 0.f, "");
        configSwitch(KSLIDE_PARAM,  0.f, 1.f, 0.f, "");
        configParam(TUNE_PARAM,      -1.f, 1.f, 0.f, "Tune", " semitones");
        auto* transpose = configParam(TRANSPOSE_PARAM, -2.f, 2.f, 0.f, "Transpose", " oct");
        if (transpose) transpose->snapEnabled = true;
        configParam(GATELEN_PARAM,   0.05f, 0.95f, 0.55f, "Gate length", "%", 0.f, 100.f);
        configParam(SLIDETIME_PARAM, 0.005f, 0.25f, 0.06f, "Slide time", " s");
        configParam(ACCENTAMT_PARAM, 0.f, 1.f, 0.7f, "Accent", "%", 0.f, 100.f);
        configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Run");

        configInput(CLOCK_INPUT, "Clk");
        configInput(RESET_INPUT, "Rst");
        configOutput(CV_OUTPUT,     "CV");
        configOutput(GATE_OUTPUT,   "Gate");
        configOutput(ACCENT_OUTPUT, "Acc");
        configOutput(SLIDE_OUTPUT,  "Slide");
    }

    int lastStep() const {
        int n = int(std::lround(params[LAST_PARAM].getValue()));
        return n < 1 ? 1 : (n > kSteps ? kSteps : n);
    }

    // Semitone of a step, before transpose/tune.
    int noteOf(int step) const {
        return int(std::lround(params[pitchParam(step)].getValue()));
    }

    float voltsFor(int step) const {
        const float semis = float(noteOf(step))
                          + 12.f * std::round(params[TRANSPOSE_PARAM].getValue());
        return semis / 12.f + params[TUNE_PARAM].getValue() / 12.f;
    }

    // A momentary pad: true once per press, then cleared so it does not repeat.
    bool consume(int param) {
        if (params[param].getValue() < 0.5f) return false;
        params[param].setValue(0.f);
        return true;
    }

    // Pressing a key keeps the step in the octave it is already in, exactly like
    // the hardware: the keyboard picks the note, DOWN/UP pick the register.
    void setNote(int step, int semitone) {
        if (step < 0 || step >= kSteps) return;
        const int octave = noteOf(step) / 12;
        int note = octave * 12 + semitone;
        if (note > kMaxNote) note -= 12;          // top octave is a partial one
        if (note < 0) note = semitone;
        if (note > kMaxNote) note = kMaxNote;
        params[pitchParam(step)].setValue(float(note));
        params[gateParam(step)].setValue(1.f);   // entering a note un-rests it
    }

    void shiftOctave(int step, int delta) {
        if (step < 0 || step >= kSteps) return;
        const int note = noteOf(step) + 12 * delta;
        if (note < 0 || note > kMaxNote) return;  // refuse, never wrap
        params[pitchParam(step)].setValue(float(note));
    }

    void onReset() override {
        index = 0;
        selected = 0;
        cv = target = voltsFor(0);
        gliding = false;
        gateHeld = false;
        sinceClock = 0;
        stepLen = 0;
        // clockTrig is deliberately NOT reset here: this SchmittTrigger's idle/
        // reset state is "believes the input is already high" (see rack_dsp.h),
        // so forcing it back to that state while CLOCK_INPUT happens to already
        // be sitting high (the common case when RESET and CLOCK share one Clock
        // module, since RESET's own rising edge is sample-coincident with
        // CLOCK's) makes it require a full low-then-high cycle before it will
        // recognize ANY edge -- silently eating the very pulse this reset is
        // supposed to line up with.  Leaving it alone lets it keep tracking the
        // physical clock line uninterrupted; sequencerReset.reset() below is
        // what actually arms "swallow the next genuine edge."
        resetTrig.reset();
        sequencerReset.reset();
    }

    // Entering a step: latch its pitch target and decide whether the gate
    // retriggers.  `slideIn` is the PREVIOUS step's slide flag -- slide is a
    // property of the note it leaves from, exactly as on the hardware.
    void enterStep(int step, bool slideIn) {
        index = step;
        target = voltsFor(step);
        gliding = slideIn;
        if (!slideIn) cv = target;          // no slide: jump straight to pitch
    }

    void process(const ProcessArgs& args) override {
        // ---- panel edits (keyboard / octave / step select) -----------------
        for (int step = 0; step < kSteps; ++step)
            if (consume(selectParam(step))) selected = step;
        for (int k = 0; k < 5; ++k)
            if (consume(SHARP_PARAM + k)) setNote(selected, kSharpSemis[k]);
        for (int k = 0; k < 7; ++k)
            if (consume(NATURAL_PARAM + k)) setNote(selected, kNaturalSemis[k]);
        if (consume(OCTDOWN_PARAM)) shiftOctave(selected, -1);
        if (consume(OCTUP_PARAM))   shiftOctave(selected, +1);
        // The pads beside the keyboard flip the SELECTED step's flags, which is
        // how the flags are entered on the hardware; the grid rows do the same
        // thing directly for whichever step you can see.
        if (consume(KACCENT_PARAM))
            params[accentParam(selected)].setValue(
                params[accentParam(selected)].getValue() >= 0.5f ? 0.f : 1.f);
        if (consume(KSLIDE_PARAM))
            params[slideParam(selected)].setValue(
                params[slideParam(selected)].getValue() >= 0.5f ? 0.f : 1.f);

        const bool running = params[RUN_PARAM].getValue() >= 0.5f;
        const int  last = lastStep();

        // ---- transport ------------------------------------------------------
        if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), kGateLow, kGateHigh)) {
            // Park on step 0 with the gate CLOSED.  Zeroing sinceClock here used
            // to open step 0's gate at reset time, so the step fired once on the
            // reset and again on a clock -- the same double-trigger the grid
            // sequencer had.  The first clock plays step 0 instead (below).
            enterStep(0, false);
            gateHeld = false;
            sinceClock = kGateClosed;
            // Do NOT clockTrig.reset() here (see onReset()'s comment): RESET and
            // CLOCK are typically driven from the SAME master Clock module and so
            // arrive sample-coincident.  Forcing clockTrig back to its "already
            // high" idle state right as CLOCK_INPUT is ALSO going/already high
            // made it silently miss that very edge -- it would only recognize the
            // FOLLOWING pulse instead, permanently dropping one step every single
            // reset cycle regardless of clock rate (bar-relative pulse count).
            // sequencerReset.reset() alone correctly arms "swallow the next
            // genuine edge", which is all that's needed to keep the playhead and
            // clock in phase.
            sequencerReset.reset();
        }
        const bool clock = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), kGateLow, kGateHigh);
        const bool advance = running && sequencerReset.processClock(clock);
        // A clock swallowed by the reset still PLAYS the current step.
        const bool playCurrent = running && clock && !advance;

        ++sinceClock;
        if (advance || playCurrent) {
            // Measure the step period from the clock so the gate length can be
            // a musical fraction of a step rather than a fixed time.
            if (sinceClock > 1 && sinceClock < kGateClosed) stepLen = sinceClock;
            sinceClock = 0;
            if (advance) {
                const bool slideOut = params[slideParam(index)].getValue() >= 0.5f
                                   && params[gateParam(index)].getValue() >= 0.5f;
                enterStep(index + 1 >= last ? 0 : index + 1, slideOut);
            }
        }
        else if (index >= last) {            // pattern shortened under the playhead
            enterStep(0, false);
        }

        // ---- pitch glide -----------------------------------------------------
        if (gliding) {
            // One-pole toward the target; the time constant is the SLIDE knob,
            // so a slide lands well inside one step at normal tempos.
            const float tau = std::max(0.001f, params[SLIDETIME_PARAM].getValue());
            const float coeff = 1.f - std::exp(-1.f / (tau * args.sampleRate));
            cv += (target - cv) * coeff;
            if (std::fabs(target - cv) < 1e-4f) { cv = target; }
        }
        else cv = target;

        // ---- gate / accent / slide ------------------------------------------
        const bool  note   = params[gateParam(index)].getValue() >= 0.5f;
        const bool  slide  = params[slideParam(index)].getValue() >= 0.5f;
        const bool  accent = params[accentParam(index)].getValue() >= 0.5f;
        const long  period = stepLen > 0 ? stepLen : long(args.sampleRate * 0.25f);
        const long  gateSamples = std::max<long>(1,
            long(float(period) * params[GATELEN_PARAM].getValue()));

        // A sliding note holds its gate for the WHOLE step so the next note is
        // legato and the envelope does not retrigger -- that is what makes a
        // slide sound different from two separate notes at the same pitch.
        //
        // But "the whole step" has to END somewhere.  An unconditional hold
        // waits for the next clock, and if the clock never comes -- transport
        // stopped mid-pattern, clock source unpatched -- the step never ends
        // and the gate stays latched at 10 V forever: a permanent drone.
        // Cap the hold at twice the measured step: a genuine tempo slowdown
        // (even to half speed) still glides through the seam untouched, while a
        // stopped clock releases within one step.
        const long slideHold = std::max(gateSamples, period * 2);
        gateHeld = running && note && sinceClock < (slide ? slideHold : gateSamples);

        outputs[CV_OUTPUT].setVoltage(cv);
        outputs[GATE_OUTPUT].setVoltage(gateHeld ? 10.f : 0.f);
        outputs[ACCENT_OUTPUT].setVoltage(gateHeld && accent
                                          ? 10.f * params[ACCENTAMT_PARAM].getValue() : 0.f);
        outputs[SLIDE_OUTPUT].setVoltage(note && slide ? 10.f : 0.f);
        outputs[CV_OUTPUT].channels = 1;
        outputs[GATE_OUTPUT].channels = 1;
        outputs[ACCENT_OUTPUT].channels = 1;
        outputs[SLIDE_OUTPUT].channels = 1;

        // ---- step LEDs -------------------------------------------------------
        for (int step = 0; step < kSteps; ++step) {
            const bool on = params[gateParam(step)].getValue() >= 0.5f;
            const bool acc = params[accentParam(step)].getValue() >= 0.5f;
            if (step == index && step < last)
                lights[STEP_LIGHT + step].setBrightnessRGB(1.f, 0.05f, 0.05f);   // playhead
            else if (step == selected)
                lights[STEP_LIGHT + step].setBrightnessRGB(0.9f, 0.9f, 0.9f);    // edit cursor
            else if (step >= last)
                lights[STEP_LIGHT + step].setBrightnessRGB(0.f, 0.f, 0.f);       // past the end
            else if (on && acc)
                lights[STEP_LIGHT + step].setBrightnessRGB(1.f, 0.55f, 0.f);     // accented
            else if (on)
                lights[STEP_LIGHT + step].setBrightnessRGB(0.05f, 0.8f, 0.15f);  // note
            else
                lights[STEP_LIGHT + step].setBrightnessRGB(0.05f, 0.05f, 0.05f); // rest
        }
    }
};

//============================================================================
//  Faceplate
//============================================================================
rackx::PanelElement el(int id, float x, float y, float radius,
                       rackx::PanelControlStyle style, const std::string& label = "",
                       rackx::PanelLabelPlacement placement = rackx::PanelLabelPlacement::Below) {
    rackx::PanelElement e;
    e.id = id; e.x = x; e.y = y; e.radius = radius;
    e.style = style; e.label = label; e.labelPlacement = placement;
    return e;
}

rackx::PanelElement box(int id, float x, float y, float w, float h,
                        rackx::PanelControlStyle style, const std::string& label = "",
                        rackx::PanelLabelPlacement placement = rackx::PanelLabelPlacement::None) {
    rackx::PanelElement e = el(id, x, y, std::min(w, h) * 0.5f, style, label, placement);
    e.width = w; e.height = h;
    return e;
}

// A pad that fires once per press instead of latching (step select, octave
// shift).  The editor reads widget == "momentary" and drives it like a button.
rackx::PanelElement momentaryPad(int id, float x, float y, float w, float h,
                                 const std::string& label) {
    rackx::PanelElement e = box(id, x, y, w, h, rackx::PanelControlStyle::StepPad, label);
    e.widget = "momentary";
    return e;
}

const char* stepText(int n) {
    static const char* kText[kSteps] = { "1","2","3","4","5","6","7","8",
                                         "9","10","11","12","13","14","15","16" };
    return (n >= 1 && n <= kSteps) ? kText[n - 1] : "";
}

rackx::PanelElement section(float x, float y, float w, float h, const std::string& label) {
    rackx::PanelElement e;
    e.x = x; e.y = y; e.width = w; e.height = h;
    e.style = rackx::PanelControlStyle::Section;
    e.label = label;
    return e;
}

rackx::PanelSpec acidSeqPanel() {
    // Three silk-screened bands, top to bottom, the way the hardware reads: the
    // control row, the entry keyboard with its function pads, and the pattern
    // grid -- then the patch jacks along the bottom.
    constexpr float kW = 880.f;
    constexpr float kH = rackx::RACK_PANEL_HEIGHT;

    rackx::PanelSpec panel;
    panel.width = kW;
    panel.height = kH;

    constexpr float kSecA_Y = 26.f,  kSecA_H = 70.f;    // control row
    constexpr float kSecB_Y = 102.f, kSecB_H = 94.f;    // keyboard
    constexpr float kSecC_Y = 202.f, kSecC_H = 122.f;   // pattern grid
    panel.decor = {
        section(kW * .5f, kSecA_Y + kSecA_H * .5f, kW - 16.f, kSecA_H, "CONTROL"),
        section(kW * .5f, kSecB_Y + kSecB_H * .5f, kW - 16.f, kSecB_H, "KEYBOARD"),
        section(kW * .5f, kSecC_Y + kSecC_H * .5f, kW - 16.f, kSecC_H, "PATTERN")
    };

    // ---- A: control row ----------------------------------------------------
    const float rowY = kSecA_Y + kSecA_H * .5f - 4.f;
    panel.params.push_back(box(AcidSequencer::RUN_PARAM, 58.f, rowY, 62.f, 26.f,
                               rackx::PanelControlStyle::StepPad, "RUN"));
    struct Knob { int id; float x; const char* label; };
    const Knob knobs[5] = {
        { AcidSequencer::TUNE_PARAM,      170.f, "TUNE"   },
        { AcidSequencer::TRANSPOSE_PARAM, 262.f, "OCTAVE" },
        { AcidSequencer::GATELEN_PARAM,   354.f, "GATE"   },
        { AcidSequencer::SLIDETIME_PARAM, 446.f, "SLIDE"  },
        { AcidSequencer::ACCENTAMT_PARAM, 538.f, "ACCENT" }
    };
    for (const Knob& k : knobs)
        panel.params.push_back(el(k.id, k.x, rowY, 15.f,
                                  rackx::PanelControlStyle::Knob, k.label));
    panel.params.push_back(box(AcidSequencer::LAST_PARAM, 660.f, rowY, 54.f, 22.f,
                               rackx::PanelControlStyle::SegmentDisplay, "LAST STEP",
                               rackx::PanelLabelPlacement::Above));

    // ---- B: entry keyboard + function pads ---------------------------------
    // Naturals are listed first so the sharps, listed after, paint on top of
    // them; the editor's hit-test resolves the overlap the other way round.
    constexpr float kKbX = 40.f, kNatW = 32.f, kNatH = 68.f;
    constexpr float kNatY = kSecB_Y + kSecB_H * .5f;
    constexpr float kShpW = 18.f, kShpH = 44.f;
    constexpr float kShpY = kNatY - (kNatH - kShpH) * .5f;
    static const char* kNatName[7] = { "C", "D", "E", "F", "G", "A", "B" };
    for (int i = 0; i < 7; ++i)
        panel.params.push_back(box(AcidSequencer::NATURAL_PARAM + i,
                                   kKbX + kNatW * .5f + i * kNatW, kNatY,
                                   kNatW - 2.f, kNatH,
                                   rackx::PanelControlStyle::PianoKey, kNatName[i]));
    const int kShpAfter[5] = { 0, 1, 3, 4, 5 };            // C# D# F# G# A#
    for (int i = 0; i < 5; ++i) {
        rackx::PanelElement key = box(AcidSequencer::SHARP_PARAM + i,
                                      kKbX + kNatW * (kShpAfter[i] + 1), kShpY,
                                      kShpW, kShpH, rackx::PanelControlStyle::PianoKey);
        key.widget = "black";
        panel.params.push_back(key);
    }

    // The four function pads sit to the right of the keys, exactly as they do on
    // the hardware: the top pair sets the selected step's flags, the bottom pair
    // moves it an octave.
    const float padTop = kNatY - 17.f, padBot = kNatY + 17.f;
    panel.params.push_back(momentaryPad(AcidSequencer::KACCENT_PARAM, 330.f, padTop,
                                        72.f, 26.f, "ACCENT"));
    panel.params.push_back(momentaryPad(AcidSequencer::KSLIDE_PARAM,  410.f, padTop,
                                        72.f, 26.f, "SLIDE"));
    panel.params.push_back(momentaryPad(AcidSequencer::OCTDOWN_PARAM, 330.f, padBot,
                                        72.f, 26.f, "DOWN"));
    panel.params.push_back(momentaryPad(AcidSequencer::OCTUP_PARAM,   410.f, padBot,
                                        72.f, 26.f, "UP"));

    // ---- C: pattern grid ---------------------------------------------------
    // Row captions ride on the first column's element, drawn to its left in the
    // gutter -- the trick the 16x16 sequencer uses for its track numbers.
    constexpr float kGridX = 112.f;
    constexpr float kColW  = (kW - kGridX - 16.f) / kSteps;
    constexpr float kLightY  = kSecC_Y + 12.f;
    constexpr float kSelY    = kSecC_Y + 28.f;
    constexpr float kNoteY   = kSecC_Y + 50.f;
    constexpr float kGateY   = kSecC_Y + 72.f;
    constexpr float kAccentY = kSecC_Y + 90.f;
    constexpr float kSlideY  = kSecC_Y + 108.f;
    for (int step = 0; step < kSteps; ++step) {
        const float cx = kGridX + step * kColW + kColW * .5f;
        const bool  first = step == 0;
        const char* capNote   = first ? "NOTE"   : "";
        const char* capGate   = first ? "GATE"   : "";
        const char* capAccent = first ? "ACCENT" : "";
        const char* capSlide  = first ? "SLIDE"  : "";
        const rackx::PanelLabelPlacement place = first
            ? rackx::PanelLabelPlacement::Left : rackx::PanelLabelPlacement::None;
        panel.lights.push_back(el(AcidSequencer::STEP_LIGHT + step, cx, kLightY, 4.f,
                                  rackx::PanelControlStyle::Knob, "",
                                  rackx::PanelLabelPlacement::None));
        panel.params.push_back(momentaryPad(AcidSequencer::selectParam(step), cx, kSelY,
                                            kColW - 12.f, 16.f, stepText(step + 1)));
        panel.params.push_back(box(AcidSequencer::pitchParam(step), cx, kNoteY,
                                   kColW - 10.f, 17.f,
                                   rackx::PanelControlStyle::SegmentDisplay, capNote, place));
        panel.params.push_back(box(AcidSequencer::gateParam(step), cx, kGateY,
                                   kColW - 12.f, 14.f,
                                   rackx::PanelControlStyle::StepPad, capGate, place));
        panel.params.push_back(box(AcidSequencer::accentParam(step), cx, kAccentY,
                                   kColW - 12.f, 14.f,
                                   rackx::PanelControlStyle::StepPad, capAccent, place));
        panel.params.push_back(box(AcidSequencer::slideParam(step), cx, kSlideY,
                                   kColW - 12.f, 14.f,
                                   rackx::PanelControlStyle::StepPad, capSlide, place));
    }

    // ---- jacks -------------------------------------------------------------
    // 60 px pitch: jack captions are drawn up to 42 px wide, so anything tighter
    // runs them together (the mistake the 8-voice filter panel used to make).
    constexpr float kJackY = 350.f;
    constexpr float kJack0 = 290.f, kJackPitch = 60.f;
    panel.inputs = {
        el(AcidSequencer::CLOCK_INPUT, kJack0,              kJackY, 9.f,
           rackx::PanelControlStyle::Knob, "CLK"),
        el(AcidSequencer::RESET_INPUT, kJack0 + kJackPitch, kJackY, 9.f,
           rackx::PanelControlStyle::Knob, "RST")
    };
    panel.outputs = {
        el(AcidSequencer::CV_OUTPUT,     kJack0 + kJackPitch * 2.5f, kJackY, 9.f,
           rackx::PanelControlStyle::Knob, "CV"),
        el(AcidSequencer::GATE_OUTPUT,   kJack0 + kJackPitch * 3.5f, kJackY, 9.f,
           rackx::PanelControlStyle::Knob, "GATE"),
        el(AcidSequencer::ACCENT_OUTPUT, kJack0 + kJackPitch * 4.5f, kJackY, 9.f,
           rackx::PanelControlStyle::Knob, "ACCENT"),
        el(AcidSequencer::SLIDE_OUTPUT,  kJack0 + kJackPitch * 5.5f, kJackY, 9.f,
           rackx::PanelControlStyle::Knob, "SLIDE")
    };
    return panel;
}

} // namespace

namespace rackx {
void registerAcidSequencerModule() {
    addType("Acid303-SEQ", "303 Sequencer", "Sequencer", Role::Normal,
            [] { return std::make_unique<AcidSequencer>(); }, acidSeqPanel());
}
} // namespace rackx
