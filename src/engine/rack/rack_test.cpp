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
        "AudioOut", "AudioIn", "MIDI-CV", "VCO", "VCF", "VCA", "ADSR", "LFO", "Noise", "Mixer", "SEQ8"
    };
    for (const char* slug : corePanelSlugs) {
        const rackx::ModuleType* type = rackx::findType(slug);
        CHECK(type && type->panel.valid());
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
    auto clock = rackx::createModule("Clock");
    CHECK(clock && clock->params.empty() && clock->inputs.size() == 2 &&
          clock->outputs.size() == 11 && clock->lights.size() == 11);
    CHECK(clock->outputInfos[5] == "*32" && clock->outputInfos[10] == "/32");
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
    bool sawGate = false, sawKnob = false, sawNumberBox = false, sawTab3 = false;
    for (const rackx::PanelElement& element : seq8Type->panel.params) {
        if (element.style == rackx::PanelControlStyle::Gate) sawGate = true;
        if (element.style == rackx::PanelControlStyle::Knob) sawKnob = true;
        if (element.style == rackx::PanelControlStyle::NumberBox) sawNumberBox = true;
        if (element.tab == 3) sawTab3 = true;
    }
    CHECK(sawGate && sawKnob && sawNumberBox && sawTab3);
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
    // 1..2.  RESET arms the sequencer so the FIRST clock is consumed (stays on
    // the start step), then it advances and wraps 1 -> 2 -> 1.
    seq8->params[768 + 0].setValue(2.f);   // START track 0
    seq8->params[784 + 0].setValue(3.f);   // END   track 0
    seq8->onReset();
    seq8->inputs[0].setVoltage(0.f);
    seq8->process(seq8Args);
    CHECK(seq8->outputs[kCvOut].getVoltage() == 1.f);   // parked on loop start (index 1)
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 1.f);   // first clock consumed by reset
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 2.f);   // advance to index 2 (loop end)
    clockPulse();
    CHECK(seq8->outputs[kCvOut].getVoltage() == 1.f);   // wrap back to loop start
    // Track 1 is independent and silent here (its gates default off).
    CHECK(seq8->outputs[1].getChannels() == 1);
    CHECK(seq8->outputs[kGateOut + 1].getVoltage() == 0.f);

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
int main() {
    testCardinalTranslations();
    testScale500();
    testCorruptInjection();
    std::printf("rack_test: ALL PASS\n");
    return 0;
}
