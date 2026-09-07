//----------------------------------------------------------------------------
//  PatchKnob — rack module self-test + 500-module scaling bench.
//
//  Exercises RackEngine headlessly (no node graph, no audio device):
//
//    1. SCALE: builds a programmatic patch of 500 DSP modules (250 VCO->VCA
//       chains) + I/O modules, wired with ~750 cables (MIDI-CV pitch/gate
//       fan-out + per-chain audio), fires a note, renders 100 blocks of 512
//       frames at 48 kHz and asserts:
//         * every addModule/addCable succeeded,
//         * audio flows (non-silent after the note-on),
//         * every output sample is finite,
//       and PRINTS measured wall-clock per block vs the 10.67 ms realtime
//       budget.  Timing is reported, not asserted -- machines differ.
//
//    2. SELF-DEFENSE: injects deliberately-corrupt values past the setters
//       (polyphony_ = 99 via a test peer, Port::channels = 200 directly) and
//       asserts the audio path clamps at every read site: output stays finite,
//       no crash, and no observable channel count ever exceeds 16.
//----------------------------------------------------------------------------
#include "rack_engine.h"
#include "rack_dsp.h"
#include "rack_factory.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Release builds define NDEBUG (which no-ops assert), so use an explicit
// check that always fires.
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    std::abort(); } } while (0)

using rackx::RackEngine;
using PatchKnob::engine::MidiEvent;

static const double kSr     = 48000.0;
static const int    kBlock  = 512;
static const double kBudget = 1000.0 * kBlock / kSr;   // ms per block (10.67)

namespace rackx {
// Friend peer: pokes polyphony_ PAST the clamping setter to prove the audio
// path self-defends at the read sites (setPolyphony alone must not be load-
// bearing for memory safety).
struct RackEngineTestPeer {
    static void setPolyphonyRaw(RackEngine& e, int v) { e.polyphony_ = v; }
};
} // namespace rackx

// ---------------------------------------------------------------------------
static bool allFinite(const float* p, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(p[i])) return false;
    return true;
}

static void testCardinalTranslations() {
    std::printf("[cardinal] panel specs + DSP translations...\n");
    rackx::registerBuiltins();
    rack::dsp::SchmittTrigger startupTrigger;
    CHECK(!startupTrigger.process(10.f, 0.1f, 2.f));
    CHECK(startupTrigger.isHigh());
    CHECK(!startupTrigger.process(0.f, 0.1f, 2.f));
    CHECK(startupTrigger.process(10.f, 0.1f, 2.f));
    const rackx::ModuleType* atteType = rackx::findType("Befaco.Atte");
    const rackx::ModuleType* abcType = rackx::findType("Befaco.ABC");
    const rackx::ModuleType* vcaType = rackx::findType("VCA");
    CHECK(atteType && abcType && vcaType);
    if (const rackx::ModuleType* bridgeType = rackx::findType("GoodSheperd.Stable16")) {
        auto bridge = rackx::createModule(bridgeType->slug);
        CHECK(bridge && bridge->params.size() == 172 && bridge->outputs.size() == 17);
    }
    CHECK(atteType->panel.valid() && atteType->panel.params.size() == 8);
    CHECK(abcType->panel.valid() && abcType->panel.inputs.size() == 6);
    CHECK(vcaType->panel.valid() && vcaType->panel.height == 380.f);
    static const char* corePanelSlugs[] = {
        "AudioOut", "AudioIn", "MIDI-CV", "VCO", "VCF", "Acid303-ZDF", "VCA", "ADSR", "LFO", "Noise", "Mixer", "SEQ8"
    };
    for (const char* slug : corePanelSlugs) {
        const rackx::ModuleType* type = rackx::findType(slug);
        CHECK(type && type->panel.valid());
    }

    // VCO-8 was removed (superseded by VCO-4-SSE); its coverage went with it.
    auto acid303 = rackx::createModule("Acid303-ZDF");
    const rackx::ModuleType* acid303Type = rackx::findType("Acid303-ZDF");
    CHECK(acid303 && acid303->params.size() == 32 && acid303->inputs.size() == 40);
    CHECK(acid303->outputs.size() == 8);
    CHECK(acid303Type && acid303Type->panel.tabs.empty());
    CHECK(std::fabs(acid303Type->panel.width - 96.f * rackx::RACK_HP_WIDTH) < 0.01f);
    rack::engine::Module::ProcessArgs acidArgs;
    acidArgs.sampleRate = kSr;
    acidArgs.sampleTime = 1.f / kSr;
    acid303->inputs[0].setChannels(4);
    for (int voice = 0; voice < 4; ++voice)
        acid303->inputs[0].setVoltage(voice == 0 ? 5.f : 2.f + voice, voice);
    for (int frame = 0; frame < 256; ++frame) acid303->process(acidArgs);
    CHECK(acid303->outputs[0].getChannels() == 4);
    for (int voice = 0; voice < 4; ++voice)
        CHECK(std::isfinite(acid303->outputs[0].getVoltage(voice)));
    CHECK(std::fabs(acid303->outputs[0].getVoltage(0)) > 1e-5f);
    // Worst-case stability: sustained full-scale DC, maximum cutoff,
    // resonance, drive, envelope amount and accent must never blow up.
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex) {
        acid303->params[filterIndex].setValue(1.f);
        acid303->params[8 + filterIndex].setValue(1.f);
        acid303->params[16 + filterIndex].setValue(1.f);
        acid303->params[24 + filterIndex].setValue(1.f);
        acid303->inputs[filterIndex].setChannels(1);
        acid303->inputs[filterIndex].setVoltage(10.f);
        acid303->inputs[32 + filterIndex].setChannels(1);
        acid303->inputs[32 + filterIndex].setVoltage(10.f);
    }
    for (int frame = 0; frame < 8192; ++frame) acid303->process(acidArgs);
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex) {
        const float output = acid303->outputs[filterIndex].getVoltage();
        CHECK(std::isfinite(output));
        CHECK(std::fabs(output) <= 5.01f);
    }
    // Below the deliberately narrow self-oscillation knee, ringing must decay.
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex) {
        acid303->inputs[filterIndex].setVoltage(0.f);
        acid303->inputs[32 + filterIndex].setVoltage(0.f);
        acid303->params[8 + filterIndex].setValue(0.9f);
    }
    for (int frame = 0; frame < 32768; ++frame) acid303->process(acidArgs);
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex) {
        const float output = acid303->outputs[filterIndex].getVoltage();
        CHECK(std::isfinite(output));
        CHECK(std::fabs(output) < 0.05f);
    }
    // The final 10% may self-oscillate, but its nonlinear state and output
    // stages must remain bounded indefinitely rather than blow up.
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex) {
        acid303->params[8 + filterIndex].setValue(1.f);
        acid303->inputs[filterIndex].setVoltage(10.f);
    }
    for (int frame = 0; frame < 64; ++frame) acid303->process(acidArgs);
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex)
        acid303->inputs[filterIndex].setVoltage(0.f);
    for (int frame = 0; frame < 65536; ++frame) acid303->process(acidArgs);
    for (int filterIndex = 0; filterIndex < 8; ++filterIndex) {
        const float output = acid303->outputs[filterIndex].getVoltage();
        CHECK(std::isfinite(output));
        CHECK(std::fabs(output) <= 5.01f);
    }

    auto atte = rackx::createModule("Befaco.Atte");
    CHECK(atte && atte->params.size() == 8 && atte->outputs.size() == 4);
    atte->inputs[0].setVoltage(4.f); atte->inputs[0].setChannels(1);
    atte->process({});
    CHECK(std::fabs(atte->outputs[0].getVoltage() - 4.f) < 1e-5f);
    CHECK(std::fabs(atte->outputs[3].getVoltage() - 4.f) < 1e-5f);
    CHECK(atte->lights[0].isColor() && atte->lights[0].getGreen() > 0.f);

    auto abc = rackx::createModule("Befaco.ABC");
    CHECK(abc && abc->params.size() == 4 && abc->outputs.size() == 2);
    abc->inputs[0].setVoltage(2.f); abc->inputs[0].setChannels(1);
    abc->params[0].setValue(0.5f); abc->params[1].setValue(0.25f);
    abc->process({});
    CHECK(std::fabs(abc->outputs[0].getVoltage() - 4.5f) < 1e-5f);
    CHECK(abc->lights[0].isColor() && abc->lights[0].getGreen() > 0.f);

    RackEngine eng;
    const int atteId = eng.addModule("Befaco.Atte", 0.f, 0.f);
    CHECK(atteId > 0);
    eng.setParam(atteId, 4, 0.49f);
    CHECK(eng.getParam(atteId, 4) == 0.f);
    eng.setParam(atteId, 4, 0.51f);
    CHECK(eng.getParam(atteId, 4) == 1.f);

    const rackx::ModuleType* clockType = rackx::findType("Clock");
    const rackx::ModuleType* clockDivType = rackx::findType("ClockDiv");
    CHECK(clockType && clockDivType && clockType->panel.valid());
    CHECK(!clockDivType->paletteVisible);
    // ClockDiv gained a RESET_INPUT appended after CLK_INPUT (index 0 stays
    // CLK so existing saved patches referencing it are unaffected).
    {
        auto clockDiv = rackx::createModule("ClockDiv");
        CHECK(clockDiv && clockDiv->inputs.size() == 2);
    }
    auto clock = rackx::createModule("Clock");
    // Assert Clock's SHAPE by name, not by index.  Hard-coded counts and
    // positions ("outputs.size() == 11", "outputInfos[5] == \"*32\"") broke the
    // moment the module gained more divisions -- the test was pinning an
    // implementation detail rather than the contract.  What matters is that a
    // clock has an in, a reset, and named multiply/divide taps.
    CHECK(clock && clock->params.empty() && clock->inputs.size() == 2);
    CHECK(clock->outputs.size() >= 11 && !clock->lights.empty());
    CHECK(clock->lights.size() <= clock->outputs.size());
    auto hasOutput = [&](const char* name) {
        for (const std::string& s : clock->outputInfos) if (s == name) return true;
        return false;
    };
    CHECK(hasOutput("*2") && hasOutput("*32"));
    CHECK(hasOutput("/2") && hasOutput("/32"));
    rack::engine::Module::ProcessArgs clockArgs;
    clockArgs.sampleTime = 0.15f;
    clockArgs.tempoBpm = 120.f;
    clock->process(clockArgs);
    CHECK(clock->outputs[0].getVoltage() == 10.f);
    CHECK(clock->outputs[1].getVoltage() == 0.f && clock->outputs[2].getVoltage() == 10.f);
    CHECK(clock->outputs[3].getVoltage() == 10.f && clock->outputs[4].getVoltage() == 0.f);
    CHECK(clock->outputs[5].getVoltage() == 0.f && clock->outputs[6].getVoltage() == 10.f);
    clockArgs.sampleTime = 0.36f;
    clock->process(clockArgs);
    CHECK(clock->outputs[6].getVoltage() == 0.f && clock->outputs[7].getVoltage() == 10.f);
    clock->inputs[1].setVoltage(0.f);
    clock->inputs[1].setChannels(1);
    clockArgs.sampleTime = 0.f;
    clock->process(clockArgs);
    CHECK(clock->outputs[0].getVoltage() == 0.f && clock->outputs[5].getVoltage() == 0.f);
    clock->inputs[1].setVoltage(10.f);
    clock->process(clockArgs);
    CHECK(clock->outputs[0].getVoltage() == 10.f);
    clock->inputs[1].setVoltage(0.f);
    clockArgs.sampleTime = 0.3f;
    clock->process(clockArgs);
    clock->inputs[1].setVoltage(10.f);
    clockArgs.sampleTime = 0.001f;
    clock->process(clockArgs);
    CHECK(clock->outputs[0].getVoltage() == 10.f);
    clockArgs.isPlaying = false;
    clock->process(clockArgs);
    CHECK(clock->outputs[0].getVoltage() == 0.f && clock->outputs[10].getVoltage() == 0.f);
    CHECK(clock->lights[0].isColor() && clock->lights[1].isColor());

    // Rack ports retain cable state even before an upstream module emits a
    // channel. Fundamental VCF uses this output-connectivity guard to skip
    // DSP work, so this verifies an audio path actually reaches its LP output.
    RackEngine filterEngine;
    filterEngine.setSampleRate(kSr);
    const int audioIn = filterEngine.addModule("AudioIn", 0.f, 0.f);
    const int filter = filterEngine.addModule("VCF", 0.f, 0.f);
    const int audioOut = filterEngine.ensureDefaultIO();
    CHECK(audioIn > 0 && filter > 0 && audioOut > 0);
    CHECK(filterEngine.addCable(audioIn, 0, filter, 3) > 0);
    CHECK(filterEngine.addCable(filter, 0, audioOut, 0) > 0);
    std::vector<float> filterIn(kBlock, 1.f), filterOut(kBlock), filterRight(kBlock);
    filterEngine.process(kBlock, filterIn.data(), nullptr, filterOut.data(), filterRight.data(), nullptr, 0);
    CHECK(std::fabs(filterOut.back()) > 0.01f);

    auto stable16 = rackx::createModule("GoodSheperd.Stable16");
    const rackx::ModuleType* stable16Type = rackx::findType("GoodSheperd.Stable16");
    CHECK(stable16 && stable16Type && stable16Type->panel.valid());
    CHECK(stable16->params.size() == 172 && stable16->inputs.size() == 3 && stable16->outputs.size() == 17);
    const auto stableParam = [&](int id) -> const rackx::PanelElement* {
        for (const rackx::PanelElement& element : stable16Type->panel.params)
            if (element.id == id) return &element;
        return nullptr;
    };
    CHECK(stable16Type->panel.textureAsset == "GoodSheperd/res/Stable16.svg");
    CHECK(stableParam(3) && stableParam(3)->style == rackx::PanelControlStyle::Button);
    CHECK(stableParam(147) && stableParam(147)->style == rackx::PanelControlStyle::Button);
    CHECK(stableParam(171) && stableParam(171)->style == rackx::PanelControlStyle::Switch);
    stable16->process({});
    stable16->params[3].setValue(1.f);
    stable16->process({});
    CHECK(stable16->outputs[1].getVoltage() == 10.f);

    // 16-track x 16-step sequencer.  Params: 256 value + 256 gate + 256 prob +
    // 16 start + 16 end = 800; 256 per-cell lights; per-track outputs = 16 CV +
    // 16 gate = 32 mono; 4 tab pages of 4 tracks.
    auto seq8 = rackx::createModule("SEQ8");
    const rackx::ModuleType* seq8Type = rackx::findType("SEQ8");
    CHECK(seq8 && seq8Type);
    CHECK(seq8->params.size() == 800 && seq8->lights.size() == 256);
    CHECK(seq8->outputs.size() == 32 && seq8->inputs.size() == 2);
    CHECK(seq8Type->panel.tabs.size() == 4);
    CHECK(seq8Type->panel.outputs.size() == 32);
    // Tab-tagged step controls + always-visible output jacks (tab == -1).
    bool sawGate = false, sawKnob = false, sawSegmentDisplay = false, sawTab3 = false;
    for (const rackx::PanelElement& element : seq8Type->panel.params) {
        if (element.style == rackx::PanelControlStyle::Gate) sawGate = true;
        if (element.style == rackx::PanelControlStyle::Knob) sawKnob = true;
        if (element.style == rackx::PanelControlStyle::SegmentDisplay) sawSegmentDisplay = true;
        if (element.tab == 3) sawTab3 = true;
    }
    CHECK(sawGate && sawKnob && sawSegmentDisplay && sawTab3);
    for (const rackx::PanelElement& out : seq8Type->panel.outputs) CHECK(out.tab == -1);
    // Gates default OFF (zeroed) and probability defaults to 100%.
    CHECK(seq8->params[256].getValue() == 0.f);
    CHECK(seq8->params[512].getValue() == 1.f);

    rack::engine::Module::ProcessArgs seq8Args;
    seq8Args.sampleTime = 0.01f;
    // Program track 0: CV = step index, gate on every step, probability 100%
    // (deterministic fire) over the default full loop.  Outputs: CV out for
    // track t is outputs[t]; gate out is outputs[16 + t].
    const int kValueBase = 0, kGateBase = 256, kProbBase = 512;
    const int kCvOut = 0, kGateOut = 16;
    for (int step = 0; step < 16; ++step) {
        seq8->params[kValueBase + step].setValue(float(step));   // track 0 == row 0
        seq8->params[kGateBase + step].setValue(1.f);
        seq8->params[kProbBase + step].setValue(1.f);
    }
    seq8->inputs[0].setChannels(1);
    seq8->inputs[1].setChannels(1);
    // Rising-edge clock helper: settle low, then go high (the edge that steps).
    const auto clockPulse = [&]() {
        seq8->inputs[0].setVoltage(0.f);
        seq8->process(seq8Args);
        seq8->inputs[0].setVoltage(10.f);
        seq8->process(seq8Args);
    };

    // Fresh module (no reset armed): playhead at step 0.  CV out is mono.
    seq8->inputs[0].setVoltage(0.f);
    seq8->inputs[1].setVoltage(0.f);
    seq8->process(seq8Args);
    CHECK(seq8->outputs[kCvOut].getChannels() == 1);
    CHECK(seq8->outputs[kCvOut].getVoltage() == 0.f);
    // First rising edge -> step 1; gate high while the clock is high.
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 1.f);
    CHECK(seq8->outputs[kGateOut].getVoltage() == 10.f);
    // Clock low: the gate falls though the CV holds.
    seq8->inputs[0].setVoltage(0.f);
    seq8->process(seq8Args);
    CHECK(seq8->outputs[kGateOut].getVoltage() == 0.f);
    // Next edge -> step 2.
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 2.f);

    // Per-track loop range: shrink track 0 to steps 2..3 (1-based) = indices
    // 1..2. RESET is ABSOLUTE: it returns to physical step 1 even when that is
    // outside the loop. The first clock plays step 1, then traversal enters and
    // wraps through the configured range.
    seq8->params[768 + 0].setValue(2.f);   // START track 0
    seq8->params[784 + 0].setValue(3.f);   // END   track 0
    seq8->onReset();
    seq8->inputs[0].setVoltage(0.f);
    seq8->process(seq8Args);
    CHECK(seq8->outputs[kCvOut].getVoltage() == 0.f);   // parked on physical step 1
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 0.f);   // first clock plays reset step
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 1.f);   // enter loop at index 1
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 2.f);   // advance to loop end
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 1.f);   // wrap to loop start

    // The external RESET jack uses a rising gate edge and must do exactly the
    // same thing as onReset(). Holding it high must not repeatedly reset.
    seq8->inputs[1].setVoltage(0.f); seq8->process(seq8Args);
    seq8->inputs[1].setVoltage(10.f); seq8->process(seq8Args);
    CHECK(seq8->outputs[kCvOut].getVoltage() == 0.f);
    seq8->inputs[1].setVoltage(10.f); seq8->process(seq8Args);
    CHECK(seq8->outputs[kCvOut].getVoltage() == 0.f);
    // Track 1 is independent and silent here (its gates default off).
    CHECK(seq8->outputs[1].getChannels() == 1);
    CHECK(seq8->outputs[kGateOut + 1].getVoltage() == 0.f);

    // ---- 303 Sequencer: accents, slides, rests, clock/reset ----------------
    // Params: 12 keys + 5*16 grid + 11 globals = 103; 16 step lights; 2 in, 4 out.
    auto acidSeq = rackx::createModule("Acid303-SEQ");
    const rackx::ModuleType* acidSeqType = rackx::findType("Acid303-SEQ");
    CHECK(acidSeq && acidSeqType && acidSeqType->panel.valid());
    CHECK(acidSeq->params.size() == 103 && acidSeq->lights.size() == 16);
    CHECK(acidSeq->inputs.size() == 2 && acidSeq->outputs.size() == 4);
    CHECK(acidSeqType->panel.inputs.size() == 2 && acidSeqType->panel.outputs.size() == 4);
    // The faceplate carries a one-octave keyboard: 7 naturals + 5 sharps.
    {
        int naturals = 0, sharps = 0, pads = 0;
        for (const rackx::PanelElement& e : acidSeqType->panel.params) {
            if (e.style == rackx::PanelControlStyle::PianoKey)
                (e.widget == "black" ? sharps : naturals)++;
            if (e.style == rackx::PanelControlStyle::StepPad) ++pads;
        }
        CHECK(naturals == 7 && sharps == 5);
        // per-step select+gate+accent+slide, plus RUN and the four keyboard-side
        // function pads (ACCENT / SLIDE / DOWN / UP)
        CHECK(pads == 16 * 4 + 5);
        // Three silk-screened section frames group the panel.
        int sections = 0;
        for (const rackx::PanelElement& e : acidSeqType->panel.decor)
            if (e.style == rackx::PanelControlStyle::Section) ++sections;
        CHECK(sections == 3);
    }

    {
        // Param map (must track AcidSequencer::ParamIds):
        //   0 sharps(5) | 5 naturals(7) | 12 pitch(16) | 28 gate(16)
        //   44 accent(16) | 60 slide(16) | 76 select(16) | 92 last
        //   93 octDown | 94 octUp | 95 kAccent | 96 kSlide | 97 tune
        //   98 transpose | 99 gateLen | 100 slideTime | 101 accentAmt | 102 run
        const int kPitch = 12, kGate = 12 + 16, kAccent = 12 + 32, kSlide = 12 + 48;
        const int kSelect = 12 + 64, kLast = 12 + 80;
        const int kOctUp = kLast + 2, kKeyAccent = kLast + 3, kKeySlide = kLast + 4;
        const int kGateLen = kLast + 7, kAccentAmt = kLast + 9;
        const int kCv = 0, kGateOut303 = 1, kAccOut = 2, kSlideOut = 3;
        rack::engine::Module::ProcessArgs seqArgs;
        seqArgs.sampleRate = 48000.f;
        seqArgs.sampleTime = 1.f / 48000.f;

        acidSeq->inputs[0].setChannels(1);
        acidSeq->inputs[1].setChannels(1);
        // A four-step pattern: every step a note, step 2 accented, step 1 slides.
        acidSeq->params[kLast].setValue(4.f);
        acidSeq->params[kGateLen].setValue(0.5f);
        acidSeq->params[kAccentAmt].setValue(1.f);
        for (int s = 0; s < 4; ++s) {
            acidSeq->params[kPitch + s].setValue(float(s * 12));   // 0, 1, 2, 3 volts
            acidSeq->params[kGate + s].setValue(1.f);
            acidSeq->params[kAccent + s].setValue(s == 2 ? 1.f : 0.f);
            acidSeq->params[kSlide + s].setValue(s == 1 ? 1.f : 0.f);
        }
        acidSeq->onReset();

        const auto seqClock = [&]() {
            acidSeq->inputs[0].setVoltage(0.f);
            acidSeq->process(seqArgs);
            acidSeq->inputs[0].setVoltage(10.f);
            acidSeq->process(seqArgs);
        };

        acidSeq->inputs[0].setVoltage(0.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->outputs[kCv].getVoltage() == 0.f);          // parked on step 1
        CHECK(acidSeq->outputs[kGateOut303].getVoltage() == 10.f); // and gating

        // onReset() arms the shared reset contract, so the next edge is consumed
        // and the playhead stays on step 1 (same rule as SEQ8).
        seqClock();
        CHECK(acidSeq->outputs[kCv].getVoltage() == 0.f);

        seqClock();                                                 // -> step 2
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 1.f) < 1e-5f);
        CHECK(acidSeq->outputs[kSlideOut].getVoltage() == 10.f);    // step 2 slides
        CHECK(acidSeq->outputs[kAccOut].getVoltage() == 0.f);

        seqClock();                                                 // -> step 3, gliding
        // Step 2 asked to slide, so the pitch RAMPS toward 2V instead of jumping
        // and the gate does not drop between the two notes.
        CHECK(acidSeq->outputs[kCv].getVoltage() < 2.f);
        CHECK(acidSeq->outputs[kCv].getVoltage() > 1.f);
        CHECK(acidSeq->outputs[kGateOut303].getVoltage() == 10.f);
        CHECK(acidSeq->outputs[kAccOut].getVoltage() == 10.f);      // step 3 accented
        // The glide is a one-pole with tau = SLIDE TIME (60 ms by default), so it
        // is ~95% there after 3 tau and asymptotic after that; half a second is
        // 8 tau, which lands inside a few thousandths of a volt.
        for (int frame = 0; frame < 48000 / 2; ++frame) acidSeq->process(seqArgs);
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 2.f) < 5e-3f);  // glide lands

        // A rest emits no gate but still advances the pitch.
        acidSeq->params[kGate + 3].setValue(0.f);
        seqClock();                                                 // -> step 4 (rest)
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 3.f) < 1e-5f);
        CHECK(acidSeq->outputs[kGateOut303].getVoltage() == 0.f);

        // LAST STEP wraps the pattern back to step 1.
        seqClock();
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 0.f) < 1e-5f);

        // RESET returns to step 1 and swallows the first clock, as SEQ8 does.
        seqClock();
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 1.f) < 1e-5f);
        acidSeq->inputs[1].setVoltage(10.f);
        acidSeq->process(seqArgs);
        acidSeq->inputs[1].setVoltage(0.f);
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 0.f) < 1e-5f);
        seqClock();
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 0.f) < 1e-5f);  // consumed
        seqClock();
        CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - 1.f) < 1e-5f);

        // Keyboard entry: press a key -> the selected step takes that note, the
        // step un-rests, and the momentary key clears itself.
        acidSeq->params[kPitch + 3].setValue(0.f);       // bottom octave
        acidSeq->params[kSelect + 3].setValue(1.f);      // SELECT step 4
        acidSeq->params[kGate + 3].setValue(0.f);        // ...currently a rest
        acidSeq->process(seqArgs);
        acidSeq->params[5 + 4].setValue(1.f);            // NATURAL[4] == G (7 semis)
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kPitch + 3].getValue() == 7.f);
        CHECK(acidSeq->params[kGate + 3].getValue() == 1.f);
        CHECK(acidSeq->params[5 + 4].getValue() == 0.f);
        // UP walks the step through the three-octave range; the press that would
        // leave it is refused rather than wrapping.
        acidSeq->params[kOctUp].setValue(1.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kPitch + 3].getValue() == 19.f);
        acidSeq->params[kOctUp].setValue(1.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kPitch + 3].getValue() == 31.f);
        acidSeq->params[kOctUp].setValue(1.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kPitch + 3].getValue() == 31.f);   // 43 > range
        // A key press keeps the register it landed in: C in the top octave.
        acidSeq->params[5 + 0].setValue(1.f);            // NATURAL[0] == C
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kPitch + 3].getValue() == 24.f);
        // The pads beside the keyboard toggle the SELECTED step's flags.
        CHECK(acidSeq->params[kAccent + 3].getValue() == 0.f);
        acidSeq->params[kKeyAccent].setValue(1.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kAccent + 3].getValue() == 1.f);
        acidSeq->params[kKeyAccent].setValue(1.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kAccent + 3].getValue() == 0.f);   // toggles back
        acidSeq->params[kKeySlide].setValue(1.f);
        acidSeq->process(seqArgs);
        CHECK(acidSeq->params[kSlide + 3].getValue() == 1.f);
    }

    // ---- 303 Oscillator: band-limited saw/square blend --------------------
    {
        auto osc = rackx::createModule("Acid303-OSC");
        const rackx::ModuleType* oscType = rackx::findType("Acid303-OSC");
        CHECK(osc && oscType && oscType->panel.valid());
        CHECK(osc->params.size() == 4 && osc->inputs.size() == 4 && osc->outputs.size() == 1);

        rack::engine::Module::ProcessArgs oa;
        oa.sampleRate = 48000.f; oa.sampleTime = 1.f / 48000.f;
        osc->inputs[0].setChannels(1);

        // Measure one cycle at a known pitch and check the waveform is sane:
        // finite, bipolar, and periodic at the requested frequency.
        auto renderCycle = [&](float volts, float wave, std::vector<float>& buf) {
            osc->params[2].setValue(wave);
            osc->inputs[0].setVoltage(volts);
            osc->onReset();
            const float hz = 261.6256f * std::pow(2.f, volts);
            const int period = (int)(48000.f / hz + 0.5f);
            for (int n = 0; n < period * 4; ++n) osc->process(oa);   // settle
            buf.clear();
            for (int n = 0; n < period; ++n) { osc->process(oa); buf.push_back(osc->outputs[0].getVoltage()); }
        };

        std::vector<float> saw, sqr;
        renderCycle(0.f, 0.f, saw);
        renderCycle(0.f, 1.f, sqr);
        float sawMin = 1e9f, sawMax = -1e9f, sqrMin = 1e9f, sqrMax = -1e9f;
        for (float s : saw) { CHECK(std::isfinite(s)); sawMin = std::min(sawMin, s); sawMax = std::max(sawMax, s); }
        for (float s : sqr) { CHECK(std::isfinite(s)); sqrMin = std::min(sqrMin, s); sqrMax = std::max(sqrMax, s); }
        CHECK(sawMax > 1.f && sawMin < -1.f);          // bipolar, non-trivial
        CHECK(sqrMax > 0.5f && sqrMin < -0.5f);
        // The saw's prototype is a ramp, so consecutive samples mostly rise:
        // count the ascending steps over the cycle.
        int rising = 0;
        for (size_t i = 1; i < saw.size(); ++i) if (saw[i] > saw[i - 1]) ++rising;
        CHECK(rising > (int)saw.size() * 3 / 4);
        // The two waveforms are genuinely different, not a gain change.
        double diff = 0.0;
        for (size_t i = 0; i < saw.size() && i < sqr.size(); ++i)
            diff += std::fabs((double)saw[i] - (double)sqr[i]);
        CHECK(diff / (double)saw.size() > 0.25);

        // High notes must stay band-limited: no sample may exceed the low-note
        // peak by much, which is what aliasing blowup would show up as.
        std::vector<float> high;
        renderCycle(4.f, 0.f, high);                    // ~4.2 kHz
        for (float s : high) CHECK(std::isfinite(s) && std::fabs(s) < sawMax * 1.5f);

        // Polyphony follows V/Oct.
        osc->inputs[0].setChannels(4);
        for (int v = 0; v < 4; ++v) osc->inputs[0].setVoltage(float(v) / 12.f, v);
        for (int n = 0; n < 64; ++n) osc->process(oa);
        CHECK(osc->outputs[0].getChannels() == 4);
        for (int v = 0; v < 4; ++v) CHECK(std::isfinite(osc->outputs[0].getVoltage(v)));
    }

    const rackx::ModuleType* seq3Type = rackx::findType("SEQ3");
    CHECK(seq3Type && seq3Type->panel.valid());
    const auto seq3Param = [&](int id) -> const rackx::PanelElement* {
        for (const rackx::PanelElement& element : seq3Type->panel.params)
            if (element.id == id) return &element;
        return nullptr;
    };
    CHECK(seq3Param(1) && seq3Param(1)->style == rackx::PanelControlStyle::Button);
    CHECK(seq3Param(2) && seq3Param(2)->style == rackx::PanelControlStyle::Button);
    CHECK(seq3Param(28) && seq3Param(28)->style == rackx::PanelControlStyle::Button);
    auto seq3 = rackx::createModule("SEQ3");
    CHECK(seq3 && seq3->inputs.size() > 2 && seq3->outputs.size() > 5);
    rack::engine::Module::ProcessArgs sequenceArgs;
    seq3->inputs[1].setVoltage(10.f);
    seq3->inputs[1].setChannels(1);
    seq3->process(sequenceArgs);
    CHECK(seq3->outputs[4].getVoltage() == 10.f && seq3->outputs[5].getVoltage() == 0.f);
    seq3->inputs[1].setVoltage(0.f);
    seq3->process(sequenceArgs);
    seq3->inputs[1].setVoltage(10.f);
    seq3->process(sequenceArgs);
    CHECK(seq3->outputs[5].getVoltage() == 10.f);
    seq3->params[2].setValue(1.f);
    seq3->process(sequenceArgs);
    CHECK(seq3->outputs[4].getVoltage() == 10.f && seq3->outputs[5].getVoltage() == 0.f);
    seq3->params[2].setValue(0.f);
    seq3->process(sequenceArgs);
    seq3->inputs[1].setVoltage(0.f);
    seq3->inputs[2].setVoltage(10.f);
    seq3->inputs[2].setChannels(1);
    seq3->process(sequenceArgs);
    CHECK(seq3->outputs[4].getVoltage() == 10.f && seq3->outputs[5].getVoltage() == 0.f);
    sequenceArgs.sampleTime = 0.01f;
    seq3->inputs[2].setVoltage(0.f);
    seq3->process(sequenceArgs);
    seq3->inputs[1].setVoltage(10.f);
    seq3->process(sequenceArgs);
    CHECK(seq3->outputs[4].getVoltage() == 10.f && seq3->outputs[5].getVoltage() == 0.f);
    seq3->inputs[1].setVoltage(0.f);
    seq3->process(sequenceArgs);
    seq3->inputs[1].setVoltage(10.f);
    seq3->process(sequenceArgs);
    CHECK(seq3->outputs[5].getVoltage() == 10.f);

    RackEngine resetEngine;
    const int resetSeq3Id = resetEngine.addModule("SEQ3", 0.f, 0.f);
    rackx::RackModule* resetSeq3 = resetEngine.moduleById(resetSeq3Id);
    CHECK(resetSeq3 && resetSeq3->mod);
    resetSeq3->mod->inputs[1].setVoltage(0.f);
    resetSeq3->mod->inputs[1].setChannels(1);
    resetSeq3->mod->process(sequenceArgs);
    resetSeq3->mod->inputs[1].setVoltage(10.f);
    resetSeq3->mod->process(sequenceArgs);
    CHECK(resetSeq3->mod->outputs[5].getVoltage() == 10.f);
    CHECK(resetEngine.resetModule(resetSeq3Id));
    resetSeq3->mod->process(sequenceArgs);
    CHECK(resetSeq3->mod->outputs[4].getVoltage() == 10.f &&
          resetSeq3->mod->outputs[5].getVoltage() == 0.f);
    std::printf("[cardinal] registry, geometry, DSP, and switch snapping -- OK\n");
}

// ---------------------------------------------------------------------------
//  Test 1: 500 modules / ~750 cables, timed.
// ---------------------------------------------------------------------------
static void testScale500() {
    std::printf("[scale] building 500-module rack...\n");
    RackEngine eng;
    eng.setSampleRate(kSr);
    const int outId  = eng.ensureDefaultIO();
    const int midiId = eng.addModule("MIDI-CV", 0.f, 0.f);
    CHECK(outId > 0 && midiId > 0);

    // 250 chains of VCO -> VCA == 500 DSP modules.  Every chain hangs off the
    // MIDI-CV pitch/gate outputs (fan-out is legal; fan-IN is one per input).
    const int kChains = 250;
    std::vector<int> vco(kChains), vca(kChains);
    for (int i = 0; i < kChains; ++i) {
        vco[i] = eng.addModule("VCO", 0.f, 0.f);
        vca[i] = eng.addModule("VCA", 0.f, 0.f);
        CHECK(vco[i] > 0 && vca[i] > 0);
    }
    // VCO out enum: SIN=0 TRI=1 SAW=2 SQR=3; in: PITCH=0 FM=1.
    // VCA in: IN=0 CV=1; out: OUT=0.  MIDI-CV out: PITCH=0 GATE=1 VEL=2.
    // AudioOut in: L=0 R=1.
    int nCables = 0;
    for (int i = 0; i < kChains; ++i) {
        CHECK(eng.addCable(midiId, 0, vco[i], 0) > 0); ++nCables;   // pitch -> VCO
        CHECK(eng.addCable(midiId, 1, vca[i], 1) > 0); ++nCables;   // gate  -> VCA CV
        CHECK(eng.addCable(vco[i], 1, vca[i], 0) > 0); ++nCables;   // saw   -> VCA in
    }
    CHECK(eng.addCable(vca[0], 0, outId, 0) > 0); ++nCables;        // -> out L
    CHECK(eng.addCable(vca[1], 0, outId, 1) > 0); ++nCables;        // -> out R
    std::printf("[scale] %d modules, %d cables\n", eng.moduleCount(), nCables);
    CHECK(eng.moduleCount() >= 502);

    std::vector<float> outL(kBlock), outR(kBlock);

    // Note-on in the first block, then let it ring.
    MidiEvent on; on.sampleOffset = 0; on.status = 0x90; on.data1 = 69; on.data2 = 100;
    eng.process(kBlock, nullptr, nullptr, outL.data(), outR.data(), &on, 1);
    CHECK(allFinite(outL.data(), kBlock) && allFinite(outR.data(), kBlock));

    // Warm-up (page in, settle envelopes) before timing.
    for (int b = 0; b < 5; ++b)
        eng.process(kBlock, nullptr, nullptr, outL.data(), outR.data(), nullptr, 0);

    const int kBlocks = 100;
    double maxMs = 0.0, sumMs = 0.0;
    float  peak  = 0.f;
    for (int b = 0; b < kBlocks; ++b) {
        auto t0 = std::chrono::steady_clock::now();
        eng.process(kBlock, nullptr, nullptr, outL.data(), outR.data(), nullptr, 0);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        sumMs += ms; if (ms > maxMs) maxMs = ms;
        CHECK(allFinite(outL.data(), kBlock) && allFinite(outR.data(), kBlock));
        for (int i = 0; i < kBlock; ++i) {
            float a = std::fabs(outL[i]); if (a > peak) peak = a;
        }
    }
    CHECK(peak > 0.f);           // the note actually made it through the rack
    const double avgMs = sumMs / kBlocks;
    std::printf("[scale] per-block: avg %.3f ms, max %.3f ms  (budget %.2f ms @ 48kHz/512, %.1fx headroom)\n",
                avgMs, maxMs, kBudget, kBudget / avgMs);
    std::printf("[scale] peak |out| = %.3f  -- OK\n", peak);
}

// ---------------------------------------------------------------------------
//  Test 2: hostile polyphony/channel injection -> clamped, finite, no OOB.
// ---------------------------------------------------------------------------
static void testCorruptInjection() {
    std::printf("[inject] poly=99 + Port::channels=200...\n");
    RackEngine eng;
    eng.setSampleRate(kSr);
    const int outId  = eng.ensureDefaultIO();
    const int midiId = eng.addModule("MIDI-CV", 0.f, 0.f);
    const int vcoId  = eng.addModule("VCO", 0.f, 0.f);
    const int vcaId  = eng.addModule("VCA", 0.f, 0.f);
    CHECK(outId > 0 && midiId > 0 && vcoId > 0 && vcaId > 0);
    CHECK(eng.addCable(midiId, 0, vcoId, 0) > 0);   // pitch -> VCO
    CHECK(eng.addCable(vcoId, 0, vcaId, 0) > 0);    // sin   -> VCA
    CHECK(eng.addCable(vcaId, 0, outId, 0) > 0);    // out   -> L

    // Corrupt values PAST the validating setters: polyphony_ direct write and
    // Port::channels direct writes on both an output and an unfed input.
    rackx::RackEngineTestPeer::setPolyphonyRaw(eng, 99);
    rackx::RackModule* vco = eng.moduleById(vcoId);
    rackx::RackModule* vca = eng.moduleById(vcaId);
    CHECK(vco && vco->mod && vca && vca->mod);
    vco->mod->outputs[0].channels = 200;             // cable source port
    vca->mod->inputs[1].channels  = 200;             // unfed CV input (module read site)

    std::vector<float> outL(kBlock), outR(kBlock);
    MidiEvent on; on.sampleOffset = 0; on.status = 0x90; on.data1 = 60; on.data2 = 127;
    eng.process(kBlock, nullptr, nullptr, outL.data(), outR.data(), &on, 1);
    for (int b = 0; b < 10; ++b)
        eng.process(kBlock, nullptr, nullptr, outL.data(), outR.data(), nullptr, 0);

    // Still alive and finite; every observable channel count clamped to <=16.
    CHECK(allFinite(outL.data(), kBlock) && allFinite(outR.data(), kBlock));
    CHECK(vca->mod->inputs[0].getChannels()  <= 16);   // cable copy clamped
    CHECK(vca->mod->inputs[1].getChannels()  <= 16);   // read-site clamp
    CHECK(vco->mod->outputs[0].getChannels() <= 16);   // read-site clamp
    rackx::RackModule* mc = eng.moduleById(midiId);
    CHECK(mc && mc->mod && mc->mod->outputs[0].getChannels() <= 16);  // poly clamped
    std::printf("[inject] finite output, all channel counts <= 16 -- OK\n");
}

// ---------------------------------------------------------------------------
//  Test 3: voice stealing beyond polyphony must not strand a note.  At the
//  default polyphony (1), holding a second note while the first is still
//  down steals the only voice; the stolen note's OWN release must be a
//  harmless no-op (nothing was sounding "as" it), and releasing whichever
//  note IS currently sounding must either silence the voice (nothing else
//  held) or hand it back to the still-held note that got buried by the
//  steal (last-note-priority) -- never leave a gate stuck high with no way
//  to reach it, and never drop a still-held note's chance to sound again.
// ---------------------------------------------------------------------------
static void testVoiceStealNoStuckGate() {
    std::printf("[voicesteal] mono note-priority under voice steal...\n");
    RackEngine eng;
    eng.setSampleRate(kSr);
    const int outId  = eng.ensureDefaultIO();
    const int midiId = eng.addModule("MIDI-CV", 0.f, 0.f);
    CHECK(outId > 0 && midiId > 0);
    rackx::RackModule* mc = eng.moduleById(midiId);
    CHECK(mc && mc->mod);

    std::vector<float> outL(1), outR(1);
    auto gate  = [&]{ return mc->mod->outputs[1].voltages[0]; };
    auto pitch = [&]{ return mc->mod->outputs[0].voltages[0]; };
    auto step  = [&](MidiEvent* e, int n){
        eng.process(1, nullptr, nullptr, outL.data(), outR.data(), e, n);
    };

    MidiEvent onA{};  onA.status  = 0x90; onA.data1  = 60; onA.data2 = 100;
    MidiEvent onB{};  onB.status  = 0x90; onB.data1  = 64; onB.data2 = 100;
    MidiEvent offA{}; offA.status = 0x80; offA.data1 = 60;
    MidiEvent offB{}; offB.status = 0x80; offB.data1 = 64;

    step(&onA, 1); CHECK(gate() > 5.f);
    const float pitchA = pitch();
    step(&onB, 1); CHECK(gate() > 5.f);           // still sounding (poly==1 steals A's voice)
    const float pitchB = pitch();
    CHECK(pitchB != pitchA);

    // Release the buried note (A): no voice currently claims it, so this
    // must be a no-op -- B keeps sounding, unaffected.
    step(&offA, 1);
    CHECK(gate() > 5.f);
    CHECK(pitch() == pitchB);

    // Release the sounding note (B): nothing else held -> silence.
    step(&offB, 1);
    CHECK(gate() < 5.f);

    // Reverse order: release the SOUNDING note first while the buried note
    // is still physically held.  It must be re-triggered, not left silent
    // (a dropped note) and not left as a phantom stuck voice under B's name.
    step(&onA, 1);                                 // A sounds
    step(&onB, 1);                                 // B steals A's voice
    step(&offB, 1);                                 // release the sounding note
    CHECK(gate() > 5.f);                            // A still held -> re-triggered
    CHECK(pitch() == pitchA);                       // really A, not a stuck B
    step(&offA, 1);                                 // finally release A too
    CHECK(gate() < 5.f);                            // fully silent -- nothing stuck
    std::printf("[voicesteal] no dropped release, no stuck gate -- OK\n");
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Test 4: RESET sample-coincident with the FIRST CLOCK edge must not drop a
//  step.  This is the NORMAL patching pattern when RESET and CLOCK are both
//  taken from one master Clock module: its BAR output and any of its
//  multiplier/divider outputs are derived from the same phase in the same
//  process() call, so they rise on the identical sample.
//
//  Regression for a SchmittTrigger-polarity bug: this codebase's
//  rack::dsp::SchmittTrigger treats its reset()/idle state as "already high"
//  (see rack_dsp.h), so calling clockTrig.reset() from the live RESET_INPUT
//  handler -- while CLOCK_INPUT was ALSO rising to high on that exact sample
//  -- made the trigger require a full low-then-high cycle before it would
//  recognize ANY edge.  It silently ate the coincident pulse; the NEXT real
//  pulse got swallowed by the reset-arm logic instead (playing step 0 a
//  second time), and every subsequent step landed one pulse late -- forever
//  losing exactly one step every single reset cycle, at any clock rate.
//  Exercises both SEQ8 and the 303 Sequencer (AcidSequencer), which share the
//  same reset/swallow pattern.
// ---------------------------------------------------------------------------
static void testCoincidentResetClockNoDroppedStep() {
    std::printf("[barsync] coincident RESET+CLOCK must not drop a step...\n");
    rack::engine::Module::ProcessArgs args;
    args.sampleTime = 1.f / 48000.f;
    args.sampleRate = 48000.f;

    // ---- SEQ8: program track 0 with CV = step index, gate always on -------
    {
        auto seq8 = rackx::createModule("SEQ8");
        CHECK(seq8);
        const int kValueBase = 0, kGateBase = 256, kProbBase = 512;
        const int kCvOut = 0;
        for (int step = 0; step < 16; ++step) {
            seq8->params[kValueBase + step].setValue(float(step));
            seq8->params[kGateBase + step].setValue(1.f);
            seq8->params[kProbBase + step].setValue(1.f);
        }
        seq8->inputs[0].setChannels(1);   // CLOCK
        seq8->inputs[1].setChannels(1);   // RESET
        seq8->inputs[0].setVoltage(0.f);
        seq8->inputs[1].setVoltage(0.f);
        seq8->process(args);              // settle both trigger detectors low

        // Coincident rising edge: RESET and CLOCK both go high on the SAME
        // process() call, exactly as Clock.BAR_OUTPUT and one of its
        // multiplier outputs would.
        seq8->inputs[0].setVoltage(10.f);
        seq8->inputs[1].setVoltage(10.f);
        seq8->process(args);
        seq8->inputs[1].setVoltage(0.f);  // RESET is a brief trigger
        seq8->process(args);

        // 15 more clock pulses must visit steps 1..15 with NONE skipped or
        // repeated: pulse N (1-based, counting the coincident one as #1) must
        // land on step N-1.
        for (int step = 1; step < 16; ++step) {
            seq8->inputs[0].setVoltage(0.f);
            seq8->process(args);
            seq8->inputs[0].setVoltage(10.f);
            seq8->process(args);
            CHECK(seq8->outputs[kCvOut].getVoltage() == float(step));
        }
    }

    // ---- 303 Sequencer: same coincidence, CV should read 0,1,2,3 in order -
    {
        auto acidSeq = rackx::createModule("Acid303-SEQ");
        CHECK(acidSeq);
        const int kPitch = 12, kGate = 12 + 16, kLast = 12 + 80;
        const int kCv = 0;
        acidSeq->params[kLast].setValue(4.f);
        for (int s = 0; s < 4; ++s) {
            acidSeq->params[kPitch + s].setValue(float(s * 12));   // 0,1,2,3 V
            acidSeq->params[kGate + s].setValue(1.f);
        }
        acidSeq->inputs[0].setChannels(1);
        acidSeq->inputs[1].setChannels(1);
        acidSeq->inputs[0].setVoltage(0.f);
        acidSeq->inputs[1].setVoltage(0.f);
        acidSeq->process(args);

        acidSeq->inputs[0].setVoltage(10.f);
        acidSeq->inputs[1].setVoltage(10.f);
        acidSeq->process(args);
        acidSeq->inputs[1].setVoltage(0.f);
        acidSeq->process(args);

        for (int s = 1; s < 4; ++s) {
            acidSeq->inputs[0].setVoltage(0.f);
            acidSeq->process(args);
            acidSeq->inputs[0].setVoltage(10.f);
            acidSeq->process(args);
            CHECK(std::fabs(acidSeq->outputs[kCv].getVoltage() - float(s)) < 1e-5f);
        }
    }
    std::printf("[barsync] all steps played through a coincident reset -- OK\n");
}

// ---------------------------------------------------------------------------
//  ClockDiv gained a RESET_INPUT (previously it free-ran with no way to
//  phase-lock its counter to the master Clock's bar, so a Clock -> ClockDiv ->
//  sequencer chain could drift out of sync).  Verify RESET zeroes the counter
//  (so all /N outputs snap back into phase) and, since a bar reset commonly
//  arrives on the SAME sample as the next CLK edge (both driven off the same
//  master Clock), verify that coincident edge is still recognized -- i.e. the
//  RESET_INPUT fix does not reset/force clockTrig's state (see rack_dsp.h's
//  SchmittTrigger idle-high polarity note) and so does not eat it.
// ---------------------------------------------------------------------------
static void testClockDivReset() {
    std::printf("[barsync] ClockDiv RESET zeroes counter without dropping a coincident CLK edge...\n");
    rack::engine::Module::ProcessArgs args;
    args.sampleTime = 1.f / 48000.f;

    auto div = rackx::createModule("ClockDiv");
    CHECK(div && div->inputs.size() == 2 && div->outputs.size() == 4);
    // D2_OUTPUT is high while counter is even, D4_OUTPUT while (counter%4)<2 --
    // enough to read the counter's value back through the public port API
    // alone (counter itself is a private field of an anonymous-namespace type).
    const int kClk = 0, kReset = 1, kD2 = 0, kD4 = 1;
    div->inputs[kClk].setChannels(1);
    div->inputs[kReset].setChannels(1);
    div->inputs[kClk].setVoltage(0.f);
    div->inputs[kReset].setVoltage(0.f);
    div->process(args);   // settle both trigger detectors low

    // Advance the counter to 3 (odd, non-zero) so a later zero is observable.
    for (int i = 0; i < 3; ++i) {
        div->inputs[kClk].setVoltage(10.f);
        div->process(args);
        div->inputs[kClk].setVoltage(0.f);
        div->process(args);
    }
    CHECK(div->outputs[kD2].getVoltage() == 0.f);   // counter==3: odd -> D2 low

    // Coincident RESET + CLK rising edge, exactly as Clock.BAR_OUTPUT and
    // CLK_OUTPUT would arrive together.
    div->inputs[kClk].setVoltage(10.f);
    div->inputs[kReset].setVoltage(10.f);
    div->process(args);
    // RESET fires and zeros counter; the coincident CLK edge is ALSO
    // consumed by clockTrig on this same call (counter advances to 1 right
    // after the reset branch runs) -- so counter reads 1, not stuck at 0 and
    // not dropped. D2 (odd->low) and D4 (1%4=1<2->high) pin that down.
    CHECK(div->outputs[kD2].getVoltage() == 0.f);
    CHECK(div->outputs[kD4].getVoltage() == 10.f);

    div->inputs[kReset].setVoltage(0.f);
    div->inputs[kClk].setVoltage(0.f);
    div->process(args);

    // The next genuine CLK edge after the coincident reset must still be
    // recognized (this is exactly the class of bug that bit SEQ8/AcidSeq:
    // forcing a trigger's state while its monitored signal was live) --
    // counter advances 1 -> 2, so D2 goes high again.
    div->inputs[kClk].setVoltage(10.f);
    div->process(args);
    CHECK(div->outputs[kD2].getVoltage() == 10.f);

    std::printf("[barsync] ClockDiv RESET OK\n");
}

int main() {
    testCardinalTranslations();
    testScale500();
    testCorruptInjection();
    testVoiceStealNoStuckGate();
    testCoincidentResetClockNoDroppedStep();
    testClockDivReset();
    std::printf("rack_test: ALL PASS\n");
    return 0;
}
