//----------------------------------------------------------------------------
//  seq24 Windows port — patch-graph module self-test.
//
//  Builds a small PatchGraph exercising every core mechanism and asserts on the
//  results (no real plugins required):
//
//     SineSourceNode ─► GainNode ─────────────┐
//                                             ▼
//                                     AudioDeviceOutNode ─► out[]
//                                             ▲
//     MidiInNode ─► PluginNode(instrument) ───┘
//                        │
//                        └► MidiOutNode        (plugin MIDI-out capture)
//
//  Asserts:
//    1. compile/publish succeeds.
//    2. Audio flows: the output is non-silent.
//    3. Fan-in SUMS correctly: out == gain*sine + instrument-constant, sample
//       for sample, on both channels.
//    4. MIDI flows through a source node into a wrapped instrument (its constant
//       only appears because the note-on reached it) and the instrument's
//       MIDI-out is captured and routed to a MIDI sink.
//    5. A cycle is REJECTED at connect() time (plus self-loop + kind-mismatch).
//----------------------------------------------------------------------------
#include "patch_graph.h"
#include "patch_nodes.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace seq24::engine;
using namespace seq24::engine::patch;

static const double kSr    = 48000.0;
static const int    kBlock = 64;
static const float  kPi    = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// A minimal instrument: on note-on it outputs a constant on both channels, and
// echoes the note as MIDI-out (implements the patch-local IMidiOutInstance).
// ---------------------------------------------------------------------------
struct InstrumentStub : public IPluginInstance, public IMidiOutInstance {
    PluginDescriptor desc;
    float            C     = 0.25f;
    bool             active = false;
    MidiEvent        echo_[16];
    int              nEcho_ = 0;

    InstrumentStub() {
        desc.name         = "InstrumentStub";
        desc.isInstrument = true;
        desc.numAudioIn   = 0;
        desc.numAudioOut  = 2;
    }

    // --- IPluginInstance ---
    const PluginDescriptor& descriptor() const override { return desc; }
    bool prepare(double, int) override { return true; }
    void setActive(bool) override {}
    void release() override {}
    void process(const ProcessBlock& blk) override {
        nEcho_ = 0;
        for (int i = 0; i < blk.numMidiIn; ++i) {
            const MidiEvent& m = blk.midiIn[i];
            const uint8_t st = m.status & 0xF0;
            if (st == 0x90 && m.data2 > 0) {
                active = true;
                if (nEcho_ < 16) echo_[nEcho_++] = m;   // echo note-on to MIDI-out
            } else if (st == 0x80 || (st == 0x90 && m.data2 == 0)) {
                active = false;
            }
        }
        const float v = active ? C : 0.0f;
        if (blk.audioOut) {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < blk.nframes; ++i) blk.audioOut[c][i] = v;
        }
    }
    int       paramCount() const override { return 0; }
    ParamInfo paramInfo(int) const override { return ParamInfo{}; }
    float     getParamNormalized(uint32_t) const override { return 0.0f; }
    void      setParamNormalized(uint32_t, float) override {}
    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}
    std::vector<uint8_t> saveState() const override { return {}; }
    void loadState(const std::vector<uint8_t>&) override {}

    // --- IMidiOutInstance ---
    int pullMidiOut(MidiEvent* out, int cap) override {
        const int k = (nEcho_ < cap) ? nEcho_ : cap;
        for (int i = 0; i < k; ++i) out[i] = echo_[i];
        return k;
    }
};

// Find the PortId of the nth port matching (kind,dir) on a node.
static PortId portOf(Node* n, PortKind k, PortDir d, int nth = 0) {
    int c = 0;
    for (int i = 0; i < n->numPorts(); ++i) {
        const PortDesc pd = n->port(i);
        if (pd.kind == k && pd.dir == d) {
            if (c == nth) return pd.id;
            ++c;
        }
    }
    return (PortId)0xFFFF;
}

static bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("  FAIL: %s\n", msg); ++failures; }
        else       { std::printf("  ok:   %s\n", msg); }
    };

    std::printf("[patch_test] building graph...\n");

    PatchGraph g;
    InstrumentStub inst;

    // --- create nodes (keep raw pointers before moving ownership in) ---------
    auto sineUp = std::make_unique<SineSourceNode>(1000.0f, 1.0f);
    auto gainUp = std::make_unique<GainNode>(0.5f);
    auto outUp  = std::make_unique<AudioDeviceOutNode>(2);
    auto midiUp = std::make_unique<MidiInNode>();
    auto instUp = std::make_unique<PluginNode>(&inst, &inst);   // MIDI-out capture
    auto msinkUp= std::make_unique<MidiOutNode>();

    SineSourceNode*     sine  = sineUp.get();
    GainNode*           gain  = gainUp.get();
    AudioDeviceOutNode* outN  = outUp.get();
    MidiInNode*         midi  = midiUp.get();
    PluginNode*         instN = instUp.get();
    MidiOutNode*        msink = msinkUp.get();

    const NodeId sineId = g.addNode(std::move(sineUp));
    const NodeId gainId = g.addNode(std::move(gainUp));
    const NodeId outId  = g.addNode(std::move(outUp));
    const NodeId midiId = g.addNode(std::move(midiUp));
    const NodeId instId = g.addNode(std::move(instUp));
    const NodeId msinkId= g.addNode(std::move(msinkUp));
    check(sineId && gainId && outId && midiId && instId && msinkId, "all nodes added");

    // --- port ids ------------------------------------------------------------
    const PortId sineOut = portOf(sine, PortKind::Audio, PortDir::Out);
    const PortId gainIn  = portOf(gain, PortKind::Audio, PortDir::In);
    const PortId gainOut = portOf(gain, PortKind::Audio, PortDir::Out);
    const PortId outIn   = portOf(outN, PortKind::Audio, PortDir::In);
    const PortId midiOut = portOf(midi, PortKind::Midi,  PortDir::Out);
    const PortId instAOut= portOf(instN,PortKind::Audio, PortDir::Out);
    const PortId instMIn = portOf(instN,PortKind::Midi,  PortDir::In);
    const PortId instMOut= portOf(instN,PortKind::Midi,  PortDir::Out);
    const PortId sinkMIn = portOf(msink,PortKind::Midi,  PortDir::In);

    // --- wire it up. Order matters at the fan-in: gain is summed first. -------
    check(g.connect({{sineId, sineOut}, {gainId, gainIn }}), "connect sine -> gain");
    check(g.connect({{gainId, gainOut}, {outId,  outIn  }}), "connect gain -> out (fan-in A)");
    check(g.connect({{midiId, midiOut}, {instId, instMIn}}), "connect midiIn -> instrument");
    check(g.connect({{instId, instAOut},{outId,  outIn  }}), "connect instrument -> out (fan-in B)");
    check(g.connect({{instId, instMOut},{msinkId,sinkMIn}}), "connect instrument MIDI-out -> sink");

    // --- reject: duplicate, kind-mismatch ------------------------------------
    check(!g.connect({{gainId, gainOut}, {outId, outIn}}), "duplicate edge rejected");
    check(!g.connect({{sineId, sineOut}, {instId, instMIn}}),
          "audio->midi kind mismatch rejected");

    g.setDeviceOutNode(outId);

    // --- prepare + compile ---------------------------------------------------
    const bool prep = g.prepare(kSr, kBlock);
    check(prep, "prepare succeeded");
    check(g.lastCompileOk(), "compile + publish succeeded");

    // --- feed a MIDI note-on at offset 0 -------------------------------------
    MidiEvent noteOn{ /*offset*/0, /*status*/0x90, /*data1 note*/60, /*data2 vel*/100 };
    check(midi->push(noteOn), "pushed note-on into MidiInNode ring");

    // --- render one block ----------------------------------------------------
    std::vector<float> L((size_t)kBlock, -999.0f), R((size_t)kBlock, -999.0f);
    float* out[2] = { L.data(), R.data() };
    RenderContext rc; rc.tempoBpm = 120.0; rc.isPlaying = true;
    g.process(out, 2, kBlock, rc);

    // --- assert audio flowed + fan-in summed correctly -----------------------
    const float gainVal = 0.5f;
    const float C       = inst.C;
    const double inc    = 2.0 * (double)kPi * 1000.0 / kSr;
    double ph = 0.0;
    bool nonSilent = false;
    bool sumOk     = true;
    bool chEq      = true;
    for (int i = 0; i < kBlock; ++i) {
        const float sn       = 1.0f * (float)std::sin(ph);   // sine sample
        const float expGain  = sn * gainVal;                 // after GainNode
        const float expSum   = expGain + C;                  // + instrument DC
        if (!approx(out[0][i], expSum)) sumOk = false;
        if (!approx(out[1][i], expSum)) sumOk = false;
        if (!approx(out[0][i], out[1][i])) chEq = false;
        if (std::fabs(out[0][i]) > 1e-6f) nonSilent = true;
        ph += inc;
        if (ph >= 2.0 * (double)kPi) ph -= 2.0 * (double)kPi;
    }
    check(nonSilent, "audio flows (output non-silent)");
    check(sumOk,     "fan-in sums correctly: out == gain*sine + instrument DC");
    check(chEq,      "both output channels match");

    // The instrument DC only appears if the note-on reached it via the MIDI path.
    check(inst.active, "MIDI reached instrument through MidiInNode->PluginNode");

    // --- assert plugin MIDI-out was captured and routed to the sink ----------
    MidiEvent got{};
    const bool popped = msink->pop(got);
    check(popped, "instrument MIDI-out captured + delivered to MidiOutNode");
    check(popped && (got.status & 0xF0) == 0x90 && got.data1 == 60,
          "captured MIDI-out event is the echoed note-on");

    // --- verify the pure sine path in isolation (single source, aliased) -----
    // Disconnect the instrument audio so out == gain*sine exactly, recompile.
    check(g.disconnect({{instId, instAOut}, {outId, outIn}}),
          "disconnect instrument audio (leave single source at out)");
    check(g.compileAndPublish(), "recompiled after edit");
    for (auto& v : L) v = -999.0f;
    for (auto& v : R) v = -999.0f;
    g.process(out, 2, kBlock, rc);
    // The oscillator is continuous: prepare() zeroed its phase, block 1 advanced
    // it by kBlock samples, so block 2 starts from that carried phase. Mirror it.
    ph = 0.0;
    for (int i = 0; i < kBlock; ++i) { ph += inc; if (ph >= 2.0 * (double)kPi) ph -= 2.0 * (double)kPi; }
    bool singleOk = true;
    for (int i = 0; i < kBlock; ++i) {
        const float sn      = 1.0f * (float)std::sin(ph);
        const float expGain = sn * gainVal;
        if (!approx(out[0][i], expGain)) singleOk = false;
        ph += inc;
        if (ph >= 2.0 * (double)kPi) ph -= 2.0 * (double)kPi;
    }
    check(singleOk, "single-source (aliased) path: out == gain*sine");

    // ======================================================================
    // Cycle rejection (separate graph).
    // ======================================================================
    std::printf("[patch_test] cycle rejection...\n");
    PatchGraph gc;
    auto aUp = std::make_unique<SumNode>();
    auto bUp = std::make_unique<SumNode>();
    auto cUp = std::make_unique<SumNode>();
    Node* A = aUp.get(); Node* B = bUp.get(); Node* Cn = cUp.get();
    const NodeId aId = gc.addNode(std::move(aUp));
    const NodeId bId = gc.addNode(std::move(bUp));
    const NodeId cId = gc.addNode(std::move(cUp));

    const PortId aIn = portOf(A, PortKind::Audio, PortDir::In),  aOut = portOf(A, PortKind::Audio, PortDir::Out);
    const PortId bIn = portOf(B, PortKind::Audio, PortDir::In),  bOut = portOf(B, PortKind::Audio, PortDir::Out);
    const PortId cIn = portOf(Cn,PortKind::Audio, PortDir::In),  cOut = portOf(Cn,PortKind::Audio, PortDir::Out);

    check(gc.connect({{aId, aOut}, {bId, bIn}}), "A -> B ok");
    check(gc.connect({{bId, bOut}, {cId, cIn}}), "B -> C ok");
    check(!gc.connect({{cId, cOut}, {aId, aIn}}), "C -> A rejected (would form cycle)");
    check(!gc.connect({{aId, aOut}, {aId, aIn}}), "A -> A rejected (self-loop)");
    // After the rejections the acyclic chain must still compile.
    check(gc.prepare(kSr, kBlock), "cycle-graph prepare ok");
    check(gc.lastCompileOk(), "acyclic remainder still compiles");

    std::printf("\n[patch_test] %s (%d failure%s)\n",
                failures == 0 ? "PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
