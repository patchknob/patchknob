//----------------------------------------------------------------------------
//  PatchKnob — built-in patch node hardening self-test.
//
//  Focused checks for the RT-safety / boundary fixes in patch_nodes.cpp
//  (no PatchGraph, no real plugins required — nodes are driven directly with
//  hand-built NodeProcessContexts):
//
//    1. SineSourceNode prepared with sampleRate=0 still emits finite samples.
//    2. MixerNode / MasterMixerNode master stage scrubs NaN/Inf to silence and
//       never latches a non-finite VU.
//    3. AudioDeviceOutNode clamps to the bound frame count (oversized block),
//       zeroes the device tail on a short block, and replaces NaN with 0
//       at the device boundary.
//    4. AudioDeviceInNode clamps its read to the bound frame count and
//       zero-fills the remainder of the block.
//    5. PluginNode::swapInstance() while a fake RT thread loops process():
//       the old instance is NEVER entered after the swap returns, and the
//       block sees the real numAudioIn/numAudioOut channel counts.
//    6. MasterMixerNode addTrack/removeTrack while a fake RT thread loops
//       process() on a frozen context: no crash, meters stay finite.
//
//  MIDI DELIVERY HARDENING (sections 11-19).  One property, restated for every
//  MIDI path in the engine:
//
//        A NOTE-ON THAT GETS THROUGH MUST BE FOLLOWED BY ITS NOTE-OFF
//        GETTING THROUGH.
//
//  The defect class these pin down is "an event is consumed from a ring (head_
//  advanced) but never delivered": every such loss that happens to be a release
//  leaves a PERMANENTLY STUCK NOTE on a soft instrument or on external hardware.
//  Covered conditions: a full destination plug buffer, a target plug that went
//  away under a held note, live "due now" events queued behind scheduled-ahead
//  ones, MidiOutNode ring saturation, MidiTrackNode monitor toggled off mid-note
//  and its over-capacity merge, and MidiInNode::flush() on a transport locate.
//----------------------------------------------------------------------------
#include "patch_nodes.h"
#include "patch_graph.h"
#include "csound_node.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace PatchKnob::engine;
using namespace PatchKnob::engine::patch;

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);        \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static const int kBlock = 64;

// ---------------------------------------------------------------------------
// Small helper: a stereo bus backed by std::vector storage.
// ---------------------------------------------------------------------------
struct TestBus {
    std::vector<float> l, r;
    float* ptrs[2];
    explicit TestBus(int frames, float fill = 0.f)
        : l((size_t)frames, fill), r((size_t)frames, fill) {
        ptrs[0] = l.data(); ptrs[1] = r.data();
    }
    AudioBus bus() { return AudioBus{ ptrs, 2 }; }
};

static bool allFinite(const float* p, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(p[i])) return false;
    return true;
}

// ===========================================================================
// 1. SineSourceNode: prepare(sr=0) must not produce NaN.
// ===========================================================================
static void test_sine_sr_zero() {
    SineSourceNode sine(440.f, 0.5f);
    CHECK(sine.prepare(0.0, kBlock), "sine prepare(sr=0)");

    TestBus out(kBlock, 123.f);
    AudioBus ob = out.bus();
    NodeProcessContext ctx{};
    ctx.nframes = kBlock;
    ctx.audioOut = &ob; ctx.numAudioOut = 1;
    sine.process(ctx);

    CHECK(allFinite(out.l.data(), kBlock), "sine sr=0: left finite");
    CHECK(allFinite(out.r.data(), kBlock), "sine sr=0: right finite");

    // And a rogue frequency must not smuggle a NaN through either.
    sine.setFreq(std::nanf(""));
    sine.process(ctx);
    CHECK(allFinite(out.l.data(), kBlock), "sine NaN freq: left finite");
}

// ===========================================================================
// 2. MixerNode master stage scrubs NaN/Inf; VU stays finite.
// ===========================================================================
static void test_mixer_nan_fence() {
    MixerNode mix(1);
    TestBus in(kBlock, 0.25f);
    in.l[3]  = std::nanf("");
    in.r[7]  = INFINITY;
    in.l[11] = -INFINITY;
    TestBus out(kBlock, 99.f);

    AudioBus ib = in.bus();
    AudioBus ob = out.bus();
    NodeProcessContext ctx{};
    ctx.nframes = kBlock;
    ctx.audioIn = &ib;  ctx.numAudioIn  = 1;
    ctx.audioOut = &ob; ctx.numAudioOut = 1;
    mix.process(ctx);

    CHECK(allFinite(out.l.data(), kBlock), "mixer: left finite after NaN inject");
    CHECK(allFinite(out.r.data(), kBlock), "mixer: right finite after Inf inject");
    CHECK(out.l[3] == 0.f,  "mixer: NaN sample became silence");
    CHECK(out.r[7] == 0.f,  "mixer: +Inf sample became silence");
    CHECK(out.l[11] == 0.f, "mixer: -Inf sample became silence");
    CHECK(out.l[0] == 0.25f, "mixer: clean samples pass unchanged");
    CHECK(std::isfinite(mix.vu(0)),     "mixer: channel VU finite");
    CHECK(std::isfinite(mix.masterVu()), "mixer: master VU finite");
}

// ===========================================================================
// 3. AudioDeviceOutNode: frame clamp + tail zero + NaN scrub.
// ===========================================================================
static void test_device_out_clamp_and_nan() {
    const int devFrames = 256;
    const int bigBlock  = 512;
    const float kCanary = 777.f;

    // Device buffers carry a canary region past devFrames to detect overruns.
    std::vector<float> devL((size_t)bigBlock, kCanary), devR((size_t)bigBlock, kCanary);
    float* devPtrs[2] = { devL.data(), devR.data() };

    AudioDeviceOutNode node(2);
    node.bindDeviceOut(devPtrs, 2, devFrames);

    TestBus in(bigBlock, 0.5f);
    in.l[0] = std::nanf("");
    AudioBus ib = in.bus();
    NodeProcessContext ctx{};
    ctx.nframes = bigBlock;                       // oversized vs the binding
    ctx.audioIn = &ib; ctx.numAudioIn = 1;
    node.process(ctx);

    CHECK(devL[0] == 0.f,             "devout: NaN replaced with 0 at the DAC");
    CHECK(devL[1] == 0.5f,            "devout: clean sample copied");
    CHECK(devL[devFrames - 1] == 0.5f, "devout: copy reaches the bound length");
    bool canaryIntact = true;
    for (int i = devFrames; i < bigBlock; ++i)
        if (devL[i] != kCanary || devR[i] != kCanary) canaryIntact = false;
    CHECK(canaryIntact, "devout: oversized nframes clamped (no write past binding)");

    // Short block: frames [n, devFrames) must be zeroed, not stale.
    std::fill(devL.begin(), devL.end(), kCanary);
    std::fill(devR.begin(), devR.end(), kCanary);
    ctx.nframes = 128;
    node.process(ctx);
    bool tailZeroed = true;
    for (int i = 128; i < devFrames; ++i)
        if (devL[i] != 0.f || devR[i] != 0.f) tailZeroed = false;
    CHECK(tailZeroed, "devout: short block zeroes the device tail");
    CHECK(devL[devFrames] == kCanary, "devout: short block still respects binding");
}

// ===========================================================================
// 4. AudioDeviceInNode: read clamp + remainder zero-fill.
// ===========================================================================
static void test_device_in_clamp() {
    const int devFrames = 256;
    const int bigBlock  = 512;

    std::vector<float> srcL((size_t)devFrames, 0.75f), srcR((size_t)devFrames, -0.75f);
    const float* srcPtrs[2] = { srcL.data(), srcR.data() };

    AudioDeviceInNode node(2);
    node.bindDeviceIn(srcPtrs, 2, devFrames);

    TestBus outL(bigBlock, 42.f), outR(bigBlock, 42.f);
    AudioBus obs[2] = { outL.bus(), outR.bus() };
    NodeProcessContext ctx{};
    ctx.nframes = bigBlock;                       // oversized vs the binding
    ctx.audioOut = obs; ctx.numAudioOut = 2;
    node.process(ctx);

    CHECK(node.numPorts() == 2, "devin: one output port per driver channel");
    CHECK(node.port(0).channels == 1 && node.port(1).channels == 1,
          "devin: driver-channel outputs are mono");
    CHECK(outL.l[0] == 0.75f && outR.l[0] == -0.75f,
          "devin: outputs map to matching driver channels");
    CHECK(outL.l[devFrames - 1] == 0.75f, "devin: copy reaches the bound length");
    bool remainderZero = true;
    for (int i = devFrames; i < bigBlock; ++i)
        if (outL.l[i] != 0.f || outR.l[i] != 0.f) remainderZero = false;
    CHECK(remainderZero, "devin: frames past the binding are zero-filled");
}

// ===========================================================================
// 5. PluginNode swapInstance vs a fake RT thread.
// ===========================================================================
namespace {
std::atomic<int> g_swapViolations{0};   // process() entered on a retired instance

struct SwapStub : public IPluginInstance {
    PluginDescriptor desc;
    std::atomic<bool> retired{false};
    std::atomic<int>  calls{0};
    std::atomic<int>  lastNumIn{-1};
    std::atomic<int>  lastNumOut{-1};

    SwapStub() { desc.name = "SwapStub"; desc.isInstrument = true;
                 desc.numAudioIn = 0; desc.numAudioOut = 2; }

    const PluginDescriptor& descriptor() const override { return desc; }
    bool prepare(double, int) override { return true; }
    void setActive(bool) override {}
    void release() override {}
    void process(const ProcessBlock& blk) override {
        if (retired.load(std::memory_order_relaxed))
            g_swapViolations.fetch_add(1, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
        lastNumIn.store(blk.numAudioIn,  std::memory_order_relaxed);
        lastNumOut.store(blk.numAudioOut, std::memory_order_relaxed);
        if (blk.audioOut)
            for (int c = 0; c < blk.numAudioOut; ++c)
                for (int i = 0; i < blk.nframes; ++i) blk.audioOut[c][i] = 0.1f;
    }
    int       paramCount() const override { return 0; }
    ParamInfo paramInfo(int) const override { return ParamInfo{}; }
    float     getParamNormalized(uint32_t) const override { return 0.f; }
    void      setParamNormalized(uint32_t, float) override {}
    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}
    std::vector<uint8_t> saveState() const override { return {}; }
    void loadState(const std::vector<uint8_t>&) override {}
};
} // namespace

static void test_plugin_swap_rt() {
    SwapStub* first = new SwapStub();
    PluginNode node(first);
    node.prepare(48000.0, kBlock);

    TestBus in(kBlock), out(kBlock);
    AudioBus ib = in.bus();
    AudioBus ob = out.bus();
    NodeProcessContext ctx{};
    ctx.nframes = kBlock;
    ctx.audioIn = &ib;  ctx.numAudioIn  = 1;
    ctx.audioOut = &ob; ctx.numAudioOut = 1;

    // Fake RT thread: hammer process() on the frozen context.
    std::atomic<bool> run{true};
    std::thread rt([&] { while (run.load(std::memory_order_relaxed)) node.process(ctx); });

    // Message thread: swap instances repeatedly.  After swapInstance() returns,
    // the old instance must be fully drained — mark it retired (any later
    // process() on it counts as a violation), give any impossible straggler a
    // beat to trip the flag, then free it for real.
    std::vector<SwapStub*> retiredPool;
    for (int k = 0; k < 400; ++k) {
        SwapStub* next = new SwapStub();
        IPluginInstance* old = node.swapInstance(next);
        static_cast<SwapStub*>(old)->retired.store(true, std::memory_order_seq_cst);
        retiredPool.push_back(static_cast<SwapStub*>(old));
        if (retiredPool.size() > 8) {               // deferred free (UAF probe)
            delete retiredPool.front();
            retiredPool.erase(retiredPool.begin());
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run.store(false);
    rt.join();

    CHECK(g_swapViolations.load() == 0,
          "plugin swap: old instance never entered after swapInstance() returned");
    SwapStub* live = static_cast<SwapStub*>(node.instance());
    CHECK(live && !live->retired.load(), "plugin swap: node holds the newest instance");
    // Drive one deterministic block to pin the numAudioIn/numAudioOut contract.
    node.process(ctx);
    live = static_cast<SwapStub*>(node.instance());
    CHECK(live->lastNumIn.load()  == 2, "plugin: blk.numAudioIn set to passed channels");
    CHECK(live->lastNumOut.load() == 2, "plugin: blk.numAudioOut set to passed channels");

    for (SwapStub* s : retiredPool) delete s;
    delete static_cast<SwapStub*>(node.swapInstance(nullptr));
}

// ===========================================================================
// 6. MasterMixerNode: add/remove tracks under a running fake RT thread.
// ===========================================================================
static void test_master_mixer_edit_race() {
    MasterMixerNode mm;

    // Frozen context: 4 audio-in buses, 5 audio-out (master + 4 outlets),
    // 4 midi-in, 5 midi-out (clock + 4 outlets).  The snapshot may name more
    // tracks than the context carries — process() clamps to the context.
    const int kIn = 4, kOut = 5, kMidi = 5;
    std::vector<TestBus> inBufs, outBufs;
    inBufs.reserve(kIn); outBufs.reserve(kOut);
    std::vector<AudioBus> ins, outs;
    for (int i = 0; i < kIn;  ++i) { inBufs.emplace_back(kBlock, 0.25f); ins.push_back(inBufs.back().bus()); }
    for (int i = 0; i < kOut; ++i) { outBufs.emplace_back(kBlock);       outs.push_back(outBufs.back().bus()); }

    std::vector<std::vector<MidiEvent>> mevs((size_t)kMidi, std::vector<MidiEvent>(64));
    std::vector<MidiBuffer> mins, mouts;
    for (int i = 0; i < kMidi - 1; ++i) mins.push_back(MidiBuffer{ mevs[(size_t)i].data(), 0, 64 });
    for (int i = 0; i < kMidi; ++i)     mouts.push_back(MidiBuffer{ mevs[(size_t)i].data(), 0, 64 });

    NodeProcessContext ctx{};
    ctx.nframes = kBlock;
    ctx.audioIn = ins.data();   ctx.numAudioIn  = kIn;
    ctx.audioOut = outs.data(); ctx.numAudioOut = kOut;
    ctx.midiIn = mins.data();   ctx.numMidiIn   = kMidi - 1;
    ctx.midiOut = mouts.data(); ctx.numMidiOut  = kMidi;
    ctx.transport.isPlaying = true;

    mm.prepare(48000.0, kBlock);

    std::atomic<bool> run{true};
    std::thread rt([&] { while (run.load(std::memory_order_relaxed)) mm.process(ctx); });

    // Message thread: churn the track list hard (push_back/erase both realloc).
    unsigned rnd = 0x1234u;
    for (int k = 0; k < 20000; ++k) {
        rnd = rnd * 1664525u + 1013904223u;
        if ((rnd & 3u) != 0u || mm.trackCount() == 0)
            mm.addTrack((rnd & 4u) != 0u);
        else
            mm.removeTrack((int)((rnd >> 8) % (unsigned)mm.trackCount()));
        if (mm.trackCount() > 64)                    // keep it bounded
            while (mm.trackCount() > 8) mm.removeTrack(0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    run.store(false);
    rt.join();

    CHECK(std::isfinite(mm.masterVu()), "master mixer: master VU finite after churn");
    for (int c = 0; c < 2; ++c)
        CHECK(allFinite(outBufs[0].bus().chans[c], kBlock), "master mixer: master bus finite");

    // NaN fence on the master stage too.
    while (mm.trackCount() > 1) mm.removeTrack(0);
    if (mm.trackCount() == 0) mm.addTrack(false);
    if (mm.trackIsMidi(0)) { mm.removeTrack(0); mm.addTrack(false); }
    inBufs[0].l[5] = std::nanf("");
    mm.process(ctx);
    CHECK(allFinite(outBufs[0].l.data(), kBlock), "master mixer: NaN scrubbed at master");
    CHECK(std::isfinite(mm.masterVu()), "master mixer: VU finite with NaN input");
}

// ===========================================================================
// 7. MidiInNode timed ring: events due at absolute samples 100/720/1440 are
//    delivered in the block that contains them, at EXACT in-block offsets,
//    never early, never snapped to offset 0 (the 32nd-grid acceptance case:
//    720-sample spacing across 512-sample blocks).
// ===========================================================================
static void test_midi_in_timed_holdback() {
    MidiInNode node;
    const int kN = 512;

    MidiEvent e{};
    e.status = 0x90; e.data2 = 100; e.sampleOffset = 0;
    e.data1 = 60; CHECK(node.push(e, 100),  "midiin: push due=100");
    e.data1 = 61; CHECK(node.push(e, 720),  "midiin: push due=720");
    e.data1 = 62; CHECK(node.push(e, 1440), "midiin: push due=1440");

    MidiEvent evs[64];
    MidiBuffer mo{ evs, 0, 64 };
    NodeProcessContext ctx{};
    ctx.nframes = kN;
    ctx.midiOut = &mo; ctx.numMidiOut = 1;
    ctx.transport.isPlaying = true;

    // Block [0,512): only the due=100 event, at offset 100.
    ctx.transport.playPositionSamples = 0;
    node.process(ctx);
    CHECK(mo.count == 1, "midiin blk0: exactly one event (720/1440 held back)");
    CHECK(mo.count == 1 && evs[0].data1 == 60 && evs[0].sampleOffset == 100,
          "midiin blk0: due=100 delivered at offset 100");

    // Block [512,1024): due=720 at offset 208.
    ctx.transport.playPositionSamples = 512;
    node.process(ctx);
    CHECK(mo.count == 1, "midiin blk1: exactly one event");
    CHECK(mo.count == 1 && evs[0].data1 == 61 && evs[0].sampleOffset == 208,
          "midiin blk1: due=720 delivered at offset 208");

    // Block [1024,1536): due=1440 at offset 416.
    ctx.transport.playPositionSamples = 1024;
    node.process(ctx);
    CHECK(mo.count == 1, "midiin blk2: exactly one event");
    CHECK(mo.count == 1 && evs[0].data1 == 62 && evs[0].sampleOffset == 416,
          "midiin blk2: due=1440 delivered at offset 416");

    // Nothing left over.
    ctx.transport.playPositionSamples = 1536;
    node.process(ctx);
    CHECK(mo.count == 0, "midiin blk3: ring drained, nothing spurious");

    // Legacy "now" push (due=-1) and a LATE event both clamp to offset 0 —
    // the only cases that may legitimately land on the block edge.
    e.data1 = 63; CHECK(node.push(e), "midiin: legacy push(e)");
    e.data1 = 64; CHECK(node.push(e, 2000), "midiin: push late due=2000");
    ctx.transport.playPositionSamples = 2048;   // block [2048,2560): both are past-due
    node.process(ctx);
    CHECK(mo.count == 2, "midiin late: both delivered");
    CHECK(mo.count == 2 && evs[0].sampleOffset == 0 && evs[1].sampleOffset == 0,
          "midiin late: due=-1 and past-due clamp to offset 0");
}

// ===========================================================================
// 8. MasterMixerNode MIDI clock: 0xFA+first 0xF8 together on start-from-zero,
//    0xF2+0xFB on continue, 0xFC on stop, and steady 500 BPM clocks EXACTLY
//    240 samples apart (48000*60/(500*24) = 240) across block boundaries.
// ===========================================================================
static void test_master_mixer_clock() {
    MasterMixerNode mm;                          // zero tracks: ports = master + clock
    const int kN = 512;
    mm.prepare(48000.0, kN);

    TestBus master(kN);
    AudioBus ob = master.bus();
    std::vector<MidiEvent> clkEvs(64);
    MidiBuffer clk{ clkEvs.data(), 0, 64 };

    NodeProcessContext ctx{};
    ctx.nframes = kN;
    ctx.audioOut = &ob;  ctx.numAudioOut = 1;
    ctx.midiOut  = &clk; ctx.numMidiOut  = 1;
    ctx.transport.tempoBpm = 500.0;

    // Stopped block first (arms the edge detector; nothing may be emitted).
    ctx.transport.isPlaying = false;
    ctx.transport.playPositionSamples = 0;
    mm.process(ctx);
    CHECK(clk.count == 0, "clock: stopped emits nothing");

    // stop -> play at position 0: 0xFA Start, then the FIRST 0xF8 at the SAME
    // offset (MIDI spec: first clock coincides with Start).
    ctx.transport.isPlaying = true;
    std::vector<long long> dues;                 // absolute due samples of every 0xF8
    for (int blk = 0; blk < 24; ++blk) {         // 24*512 = 12288 samples ≈ 51 clocks
        ctx.transport.playPositionSamples = (int64_t)blk * kN;
        mm.process(ctx);
        if (blk == 0) {
            CHECK(clk.count >= 2, "clock start: at least Start + first 0xF8");
            CHECK(clk.count >= 1 && clkEvs[0].status == 0xFA && clkEvs[0].sampleOffset == 0,
                  "clock start: first event is 0xFA at the transition offset");
            CHECK(clk.count >= 2 && clkEvs[1].status == 0xF8
                      && clkEvs[1].sampleOffset == clkEvs[0].sampleOffset,
                  "clock start: first 0xF8 coincides with Start");
        }
        for (int i = 0; i < clk.count; ++i)
            if (clkEvs[i].status == 0xF8)
                dues.push_back((long long)ctx.transport.playPositionSamples
                               + clkEvs[i].sampleOffset);
    }
    CHECK(dues.size() >= 2, "clock steady: got a clock stream");
    bool spacingExact = dues.size() >= 2 && dues[0] == 0;
    for (size_t i = 1; i < dues.size(); ++i)
        if (dues[i] - dues[i - 1] != 240) spacingExact = false;
    CHECK(spacingExact, "clock steady 500 BPM: consecutive 0xF8 exactly 240 samples apart from 0");

    // play -> stop: one 0xFC, then silence.
    ctx.transport.isPlaying = false;
    mm.process(ctx);
    CHECK(clk.count == 1, "clock stop: exactly one event");
    CHECK(clk.count == 1 && clkEvs[0].status == 0xFC, "clock stop: it is 0xFC");
    mm.process(ctx);
    CHECK(clk.count == 0, "clock stopped again: nothing");

    // stop -> play mid-song: 0xF2 Song Position (MIDI beats = 16ths), 0xFB
    // Continue, then the first 0xF8 at the same offset.  At 500 BPM/48k one
    // 16th = 1440 samples; position 5760 = 4 MIDI beats exactly.
    ctx.transport.isPlaying = true;
    ctx.transport.playPositionSamples = 5760;
    mm.process(ctx);
    CHECK(clk.count >= 3, "clock continue: SPP + Continue + first 0xF8");
    CHECK(clk.count >= 1 && clkEvs[0].status == 0xF2
              && clkEvs[0].data1 == 4 && clkEvs[0].data2 == 0
              && clkEvs[0].sampleOffset == 0,
          "clock continue: 0xF2 carries 4 MIDI beats (LSB=4, MSB=0) at offset 0");
    CHECK(clk.count >= 2 && clkEvs[1].status == 0xFB && clkEvs[1].sampleOffset == 0,
          "clock continue: 0xFB follows SPP");
    CHECK(clk.count >= 3 && clkEvs[2].status == 0xF8 && clkEvs[2].sampleOffset == 0,
          "clock continue: first 0xF8 coincides with Continue");
}

// ===========================================================================
// 9. MidiOutNode: process() stamps blockStart + sampleOffset into the ring;
//    pop(TimedMidi&) hands the absolute due sample to the drain thread and
//    the legacy pop(MidiEvent&) shim still works.
// ===========================================================================
static void test_midi_out_due_stamp() {
    MidiOutNode node;

    MidiEvent evs[3]{};
    evs[0].status = 0x90; evs[0].data1 = 60; evs[0].sampleOffset = 0;
    evs[1].status = 0x80; evs[1].data1 = 60; evs[1].sampleOffset = 208;
    evs[2].status = 0xF8; evs[2].sampleOffset = 464;
    MidiBuffer mi{ evs, 3, 3 };

    NodeProcessContext ctx{};
    ctx.nframes = 512;
    ctx.midiIn = &mi; ctx.numMidiIn = 1;
    ctx.transport.playPositionSamples = 1024;    // block START
    node.process(ctx);

    TimedMidi tm{};
    CHECK(node.pop(tm), "midiout: pop #1");
    CHECK(tm.dueSample == 1024 && tm.ev.status == 0x90 && tm.ev.data1 == 60,
          "midiout: dueSample = blockStart + 0");
    CHECK(node.pop(tm), "midiout: pop #2");
    CHECK(tm.dueSample == 1232 && tm.ev.status == 0x80,
          "midiout: dueSample = blockStart + 208");
    MidiEvent legacy{};
    CHECK(node.pop(legacy), "midiout: legacy pop(MidiEvent) shim");
    CHECK(legacy.status == 0xF8 && legacy.sampleOffset == 464,
          "midiout: legacy pop yields the event bytes");
    CHECK(!node.pop(tm), "midiout: ring empty after three pops");
}

// ===========================================================================
// 10. RecordNode: tick = llround of ONE absolute-sample expression — at
//     500 BPM/48k samplesPerTick = 30.  playPos 700 (NOT a tick multiple):
//     the old floor(base)+floor(offset) math lost up to 2 ticks here.
// ===========================================================================
static void test_midi_track_uses_capture_arrival_sample() {
    MidiTrackNode track;
    track.prepare(48000.0, 512);
    track.setRouting(0, 0);
    track.setCapture(true);
    MidiEvent ev{}; ev.status=0x90; ev.data1=60; ev.data2=100;
    ev.sampleOffset=40; ev.captureSample=777;
    MidiBuffer in{&ev,1,1}, out{};
    NodeProcessContext ctx{}; ctx.nframes=512; ctx.midiIn=&in; ctx.numMidiIn=1;
    ctx.midiOut=&out; ctx.numMidiOut=1; ctx.transport.playPositionSamples=1000;
    track.process(ctx);
    MidiTrackNode::Ev got{};
    CHECK(track.drain(&got,1)==1, "track capture: arrival event captured");
    CHECK(got.sample==777, "track capture: original arrival sample wins over delayed block position");
}

static void test_midi_track_capture_unwraps_loop() {
    MidiTrackNode track;
    track.prepare(48000.0, 64); track.setRouting(0,0); track.setCapture(true);
    MidiEvent ev{}; ev.status=0x90; ev.data1=60; ev.data2=100;
    MidiBuffer in{&ev,1,1}, out{};
    NodeProcessContext ctx{}; ctx.nframes=64; ctx.midiIn=&in; ctx.numMidiIn=1;
    ctx.midiOut=&out; ctx.numMidiOut=1;
    ev.captureSample=1900; track.process(ctx);
    ev.captureSample=100; track.process(ctx);
    MidiTrackNode::Ev got[2]{};
    CHECK(track.drain(got,2)==2, "track capture loop: both passes captured");
    CHECK(got[1].sample>got[0].sample, "track capture loop: take time stays monotonic after transport wrap");
}

// A wrap detected after several event-free blocks used to anchor its gap to
// captureLast_ from the last CAPTURED event, which by then could be many
// blocks stale.  The gap added at the wrap was only ever one block wide, so
// a long run of silence right before the loop point made every note in the
// following pass land far too early.  captureLast_ must also track the
// block boundary itself (even with nothing captured in it) so the gap
// reflects the actual elapsed time, not just distance from the last note.
static void test_midi_track_capture_wrap_survives_silence() {
    MidiTrackNode track;
    track.prepare(48000.0, 64); track.setRouting(0,0); track.setCapture(true);

    MidiEvent ev{}; ev.status=0x90; ev.data1=60; ev.data2=100;
    MidiBuffer out{};
    NodeProcessContext ctx{}; ctx.nframes=64; ctx.numMidiIn=1;
    ctx.midiOut=&out; ctx.numMidiOut=1;

    // Pass 1: one note near the top of the pass...
    ev.sampleOffset=10; ev.captureSample=-1;
    MidiBuffer in{&ev,1,1};
    ctx.midiIn=&in; ctx.transport.playPositionSamples=0;
    track.process(ctx);

    // ...then many silent blocks (no events) walking the transport toward
    // the loop end, exactly as the real engine does between notes.
    MidiBuffer empty{nullptr,0,0};
    ctx.midiIn=&empty;
    for (int64_t pos = 64; pos < 960; pos += 64) {
        ctx.transport.playPositionSamples = pos;
        track.process(ctx);
    }

    // The loop wraps: transport drops back to the top, one note near the
    // start of the new pass.
    ev.sampleOffset=5;
    ctx.midiIn=&in;
    ctx.transport.playPositionSamples=0;
    track.process(ctx);

    MidiTrackNode::Ev got[2]{};
    CHECK(track.drain(got,2)==2, "track capture wrap: both notes captured");
    CHECK(got[1].sample>got[0].sample,
          "track capture wrap: post-wrap sample stays monotonic");
    // The fix anchors the post-wrap gap to the actual last block boundary
    // (~960), landing the new sample close to a full pass later.  The old,
    // event-anchored gap collapsed this to captureLast_(10)+64 == 74 -- a
    // few dozen samples after the first note, nowhere near a real pass.
    CHECK(got[1].sample>800,
          "track capture wrap: gap reflects the silent blocks, not just the last note");
}

// Virtual ports describe routing but never copy or blend MIDI themselves.
static void test_virtual_midi_endpoints_are_independent() {
    VirtualMidiPortsNode vm;
    vm.setPorts(2,3);
    MidiEvent in0ev[2]{{0,0x90,60,100},{12,0x80,60,0}};
    MidiEvent in1ev[1]{{4,0x90,72,100}};
    MidiBuffer ins[2]{{in0ev,2,2},{in1ev,1,1}};
    MidiEvent outEv[3][4]{};
    MidiBuffer outs[3]{{outEv[0],0,4},{outEv[1],0,4},{outEv[2],0,4}};
    NodeProcessContext ctx{};
    ctx.nframes=64; ctx.midiIn=ins; ctx.numMidiIn=2;
    ctx.midiOut=outs; ctx.numMidiOut=3;
    vm.setRoute(2,1);
    vm.process(ctx);
    CHECK(outs[0].count==0&&outs[1].count==0&&outs[2].count==0,
          "virtual midi: route metadata never forwards or blends MIDI");
}

static void test_midi_track_monitor_is_private() {
    MidiTrackNode track;
    CHECK(track.input()==-1&&track.output()==-1,
          "midi track: new track endpoints default to none");
    track.setRouting(0,0);
    MidiEvent liveEv[1]{{6,0x90,72,110}};
    MidiEvent playEv[1]{{2,0x90,60,90}};
    MidiBuffer ins[2]{{liveEv,1,1},{playEv,1,1}};
    MidiEvent outEv[4]{}; MidiBuffer out{outEv,0,4};
    NodeProcessContext ctx{};ctx.nframes=64;ctx.midiIn=ins;ctx.numMidiIn=2;
    ctx.midiOut=&out;ctx.numMidiOut=1;
    track.setMonitor(false);track.process(ctx);
    CHECK(out.count==1&&out.ev[0].data1==60,
          "midi track: unarmed lane plays only its own sequencer data");
    track.setMonitor(true);track.process(ctx);
    CHECK(out.count==2&&out.ev[0].data1==60&&out.ev[1].data1==72,
          "midi track: armed lane adds only its resolved live input");
}

static void test_midi_in_never_falls_back_to_track_zero() {
    MidiInNode node;
    node.setTrackPorts(3);
    MidiEvent e{}; e.status=0x90; e.data1=67; e.data2=100;
    CHECK(node.push(e,-1,2),"midiin exact route: queued track 2 event");
    MidiEvent ev[2][4]{};
    MidiBuffer outs[2]{{ev[0],0,4},{ev[1],0,4}};
    NodeProcessContext ctx{};
    ctx.nframes=64; ctx.midiOut=outs; ctx.numMidiOut=2;
    node.process(ctx);
    CHECK(outs[0].count==0&&outs[1].count==0,
          "midiin exact route: absent track is dropped, never redirected to track 0");
}

static void test_hardware_and_sequencer_sources_are_isolated() {
    MidiInNode sequencer,hardware;
    sequencer.setTrackPorts(2);
    MidiEvent seq{}; seq.status=0x90; seq.data1=60; seq.data2=90;
    MidiEvent hw{};  hw.status =0x90; hw.data1 =72; hw.data2 =110;
    CHECK(sequencer.push(seq,-1,1),"source isolation: sequencer event queued");
    CHECK(hardware.push(hw,-1,0),"source isolation: hardware event queued");
    sequencer.flush();

    MidiEvent seqEv[2][4]{},hwEv[4]{};
    MidiBuffer seqOut[2]{{seqEv[0],0,4},{seqEv[1],0,4}};
    MidiBuffer hwOut{hwEv,0,4};
    NodeProcessContext seqCtx{},hwCtx{};
    seqCtx.nframes=64; seqCtx.midiOut=seqOut; seqCtx.numMidiOut=2;
    hwCtx.nframes=64; hwCtx.midiOut=&hwOut; hwCtx.numMidiOut=1;
    sequencer.process(seqCtx); hardware.process(hwCtx);
    CHECK(seqOut[0].count==0&&seqOut[1].count==0,
          "source isolation: flushing sequencer clears only sequencer queue");
    CHECK(hwOut.count==1&&hwEv[0].data1==72,
          "source isolation: hardware queue and event survive sequencer flush");
}

// ===========================================================================
//  MIDI DELIVERY HARDENING (11-19)
//
//  Shared vocabulary.  A "release" is anything that can end a sounding note:
//  a note-off, a zero-velocity note-on, or an all-sound-off / all-notes-off
//  panic CC.  push() in patch_nodes.cpp already classifies releases exactly this
//  way when it reserves ring headroom, so the tests speak the same language.
// ===========================================================================
static bool isNoteOn(const MidiEvent& e) {
    return (e.status & 0xF0u) == 0x90u && e.data2 != 0;
}
static bool isRelease(const MidiEvent& e) {
    const unsigned hi = e.status & 0xF0u;
    return hi == 0x80u || (hi == 0x90u && e.data2 == 0) ||
           (hi == 0xB0u && (e.data1 == 120 || e.data1 == 123));
}
// Does `e` silence `note`?  Its own note-off, or a panic that silences the lot.
// Deliberately implementation-agnostic: a node may end a stranded note either
// way and still honor the contract.
static bool releasesNote(const MidiEvent& e, int note) {
    const unsigned hi = e.status & 0xF0u;
    if (hi == 0x80u || (hi == 0x90u && e.data2 == 0)) return e.data1 == note;
    return hi == 0xB0u && (e.data1 == 120 || e.data1 == 123);
}

// ===========================================================================
// 11. MidiInNode: a FULL destination plug buffer must not eat events.
//
//     Defect: process() copies to the plug only when `out.count < out.capacity`
//     but advances head_ unconditionally, so everything that did not fit is
//     consumed and gone — note-offs included.  Post-fix an event that cannot be
//     delivered stays queued and comes out in a later block.
// ===========================================================================
static void test_midi_in_full_plug_never_loses_events() {
    MidiInNode node;                       // one plug
    constexpr int kN   = 512;
    constexpr int kCap = 3;                // deliberately smaller than the burst

    for (int k = 0; k < 4; ++k) {          // four note-on/note-off pairs
        MidiEvent on { 0, 0x90, (uint8_t)(60 + k), 100 };
        MidiEvent off{ 0, 0x80, (uint8_t)(60 + k), 0   };
        CHECK(node.push(on,  -1, 0), "midiin full-plug: push note-on");
        CHECK(node.push(off, -1, 0), "midiin full-plug: push note-off");
    }

    MidiEvent  evs[kCap];
    MidiBuffer mo{ evs, 0, kCap };
    NodeProcessContext ctx{};
    ctx.nframes = kN;
    ctx.midiOut = &mo; ctx.numMidiOut = 1;
    ctx.transport.isPlaying = true;

    std::vector<MidiEvent> got;
    for (int b = 0; b < 6; ++b) {          // plenty of blocks to drain 8 events
        ctx.transport.playPositionSamples = (int64_t)b * kN;
        node.process(ctx);
        CHECK(mo.count <= kCap, "midiin full-plug: never writes past plug capacity");
        for (int i = 0; i < mo.count; ++i) got.push_back(evs[i]);
    }

    int releases = 0;
    for (size_t i = 0; i < got.size(); ++i) if (isRelease(got[i])) ++releases;
    CHECK(releases == 4,
          "midiin full-plug: every note-off survives a full destination buffer");
    CHECK((int)got.size() == 8,
          "midiin full-plug: no event is consumed from the ring without being delivered");
    bool order = ((int)got.size() == 8);
    for (size_t i = 0; order && i < got.size(); ++i) {
        const uint8_t wantNote   = (uint8_t)(60 + (int)(i / 2));
        const uint8_t wantStatus = (i % 2 == 0) ? (uint8_t)0x90 : (uint8_t)0x80;
        if (got[i].status != wantStatus || got[i].data1 != wantNote) order = false;
    }
    CHECK(order, "midiin full-plug: retained events keep their FIFO order");
}

// ===========================================================================
// 12. MidiInNode: live "due now" input must not be stuck behind a scheduled
//     event (head-of-line blocking).
//
//     Defect: process() does `if (tm.dueSample >= blockEnd) break;`, which is
//     only sound if the ring is monotonic in dueSample.  It is NOT: hardware
//     input is pushed with dueSample = -1 ("now") while the sequencer schedules
//     a block or more ahead, so ONE scheduled event at the head silences the
//     keyboard behind it.  Post-fix the live pair plays in its own block and the
//     scheduled event is still held back to ITS block, at its exact offset.
// ===========================================================================
static void test_midi_in_live_not_blocked_by_scheduled() {
    MidiInNode node;
    constexpr int   kN   = 512;
    constexpr int64_t kFar = 100000;       // many blocks ahead of the playhead

    MidiEvent sched{ 0, 0x90, 36, 100 };   // sequencer, scheduled far ahead
    MidiEvent lon  { 0, 0x90, 72, 110 };   // live keyboard, "now"
    MidiEvent loff { 0, 0x80, 72, 0   };
    CHECK(node.push(sched, kFar, 0), "midiin hol: push scheduled event first");
    CHECK(node.push(lon,   -1,   0), "midiin hol: push live note-on behind it");
    CHECK(node.push(loff,  -1,   0), "midiin hol: push live note-off behind it");

    MidiEvent  evs[16];
    MidiBuffer mo{ evs, 0, 16 };
    NodeProcessContext ctx{};
    ctx.nframes = kN;
    ctx.midiOut = &mo; ctx.numMidiOut = 1;
    ctx.transport.isPlaying = true;
    ctx.transport.playPositionSamples = 0;
    node.process(ctx);

    bool sawLiveOn = false, sawLiveOff = false, firedEarly = false;
    for (int i = 0; i < mo.count; ++i) {
        if (evs[i].data1 == 72 && isNoteOn(evs[i]))  sawLiveOn  = true;
        if (evs[i].data1 == 72 && isRelease(evs[i])) sawLiveOff = true;
        if (evs[i].data1 == 36)                      firedEarly = true;
    }
    CHECK(sawLiveOn && sawLiveOff,
          "midiin hol: live note pair delivered although a future event heads the ring");
    CHECK(mo.count == 2, "midiin hol: exactly the two live events this block");
    CHECK(!firedEarly, "midiin hol: the scheduled event did NOT fire early");

    // The held-back event must still be there, and land on its exact sample.
    ctx.transport.playPositionSamples = 99840;      // block [99840, 100352)
    node.process(ctx);
    CHECK(mo.count == 1 && evs[0].data1 == 36 && evs[0].sampleOffset == 160,
          "midiin hol: the scheduled event is kept and delivered at its exact offset");
}

// ===========================================================================
// 13. MidiInNode: an event with NO valid target plug.
//
//     Defect: `if (rp < nOut)` dropped every such event while head_ advanced
//     anyway.  push() stamps the 0xFF "no target" sentinel whenever port < 0,
//     and audio_app.cpp passes a negative port on purpose when it cannot resolve
//     a track to a plug — so a note-OFF routinely arrived carrying the sentinel
//     and vanished, stranding the note-on that had already sounded.
//
//     The post-fix contract has three parts, and only the first is a delivery:
//       A. a RELEASE with no target is re-aimed at whichever reachable plug
//          still has that note sounding from this node;
//       B. a NOTE-ON with no target is still dropped — it must never be
//          redirected to plug 0 and play some other track's instrument (the
//          rule test_midi_in_never_falls_back_to_track_zero pins);
//       C. when the target plug is genuinely gone from the compiled context
//          there is no buffer to write to, so the release is discarded — but it
//          must NEVER be misdelivered to a surviving plug.
// ===========================================================================
static void test_midi_in_untargeted_release_is_reaimed() {
    constexpr int kN = 512;

    // --- A. a release carrying the sentinel finds the plug that is sounding ---
    {
        MidiInNode node;
        node.setTrackPorts(3);
        MidiEvent  evs[3][8]{};
        MidiBuffer outs[3]{ { evs[0], 0, 8 }, { evs[1], 0, 8 }, { evs[2], 0, 8 } };
        NodeProcessContext ctx{};
        ctx.nframes = kN;
        ctx.midiOut = outs; ctx.numMidiOut = 3;
        ctx.transport.isPlaying = true;

        MidiEvent on{ 0, 0x90, 64, 100 };
        CHECK(node.push(on, -1, 2), "midiin untargeted: push note-on to plug 2");
        ctx.transport.playPositionSamples = 0;
        node.process(ctx);
        CHECK(outs[2].count == 1 && evs[2][0].data1 == 64,
              "midiin untargeted: note-on sounds on its own plug");

        MidiEvent off{ 0, 0x80, 64, 0 };
        CHECK(node.push(off, -1, -1), "midiin untargeted: push note-off with no target");
        ctx.transport.playPositionSamples = kN;
        node.process(ctx);
        CHECK(outs[2].count == 1 && releasesNote(evs[2][0], 64),
              "midiin untargeted: the release is re-aimed at the plug holding the note");
        CHECK(outs[0].count == 0 && outs[1].count == 0,
              "midiin untargeted: it is not sprayed at plugs that hold nothing");
    }

    // --- B. an untargeted NOTE-ON is still dropped, never redirected ---------
    {
        MidiInNode node;
        node.setTrackPorts(3);
        MidiEvent  evs[3][8]{};
        MidiBuffer outs[3]{ { evs[0], 0, 8 }, { evs[1], 0, 8 }, { evs[2], 0, 8 } };
        NodeProcessContext ctx{};
        ctx.nframes = kN;
        ctx.midiOut = outs; ctx.numMidiOut = 3;
        ctx.transport.isPlaying = true;
        ctx.transport.playPositionSamples = 0;

        MidiEvent on{ 0, 0x90, 70, 100 };
        CHECK(node.push(on, -1, -1), "midiin untargeted: push note-on with no target");
        node.process(ctx);
        CHECK(outs[0].count == 0 && outs[1].count == 0 && outs[2].count == 0,
              "midiin untargeted: an untargeted note-on plays nobody's instrument");
    }

    // --- C. a plug that vanished is unreachable, but never mis-aimed ---------
    {
        MidiInNode node;
        node.setTrackPorts(3);
        MidiEvent  evs[3][8]{};
        MidiBuffer outs3[3]{ { evs[0], 0, 8 }, { evs[1], 0, 8 }, { evs[2], 0, 8 } };
        NodeProcessContext ctx{};
        ctx.nframes = kN;
        ctx.midiOut = outs3; ctx.numMidiOut = 3;
        ctx.transport.isPlaying = true;
        ctx.transport.playPositionSamples = 0;

        MidiEvent on{ 0, 0x90, 64, 100 };
        CHECK(node.push(on, -1, 2), "midiin vanished: push note-on to plug 2");
        node.process(ctx);
        CHECK(outs3[2].count == 1, "midiin vanished: note-on sounded on plug 2");

        // The track is removed under the held note: the context now has 2 plugs.
        MidiEvent off{ 0, 0x80, 64, 0 };
        CHECK(node.push(off, -1, 2), "midiin vanished: push note-off to the gone plug");
        MidiBuffer outs2[2]{ { evs[0], 0, 8 }, { evs[1], 0, 8 } };
        ctx.midiOut = outs2; ctx.numMidiOut = 2;
        ctx.transport.playPositionSamples = kN;
        node.process(ctx);
        CHECK(outs2[0].count == 0 && outs2[1].count == 0,
              "midiin vanished: an unreachable release is never dumped on a surviving plug");
    }
}

// ===========================================================================
// 14. MidiOutNode: ring saturation must never eat a release.
//
//     Defect: process() `break`s when the ring is full and silently drops the
//     rest of the block — note-offs and panic CCs included.  Post-fix the drop
//     policy has to reserve headroom for releases the way MidiInNode::push() and
//     audio_app_route_midi() already do: reject a late note-ON rather than admit
//     it and then lose its note-OFF.
// ===========================================================================
static void test_midi_out_ring_saturation_keeps_releases() {
    MidiOutNode node;
    constexpr int kFill = 1200;            // deliberately > the node's 1024 ring
    constexpr int kRel  = 40;              // releases must fit the reserve

    std::vector<MidiEvent> in((size_t)(kFill + kRel + 1));
    for (int i = 0; i < kFill; ++i)
        in[(size_t)i] = MidiEvent{ 0, 0x90, (uint8_t)(1 + (i % 100)), 100 };
    for (int i = 0; i < kRel; ++i)
        in[(size_t)(kFill + i)] = MidiEvent{ 0, 0x80, (uint8_t)(1 + i), 0 };
    in[(size_t)(kFill + kRel)] = MidiEvent{ 0, 0xB0, 123, 0 };   // all notes off

    MidiBuffer mi{ in.data(), (int)in.size(), (int)in.size() };
    NodeProcessContext ctx{};
    ctx.nframes = 512;
    ctx.midiIn = &mi; ctx.numMidiIn = 1;
    ctx.transport.playPositionSamples = 0;
    node.process(ctx);

    bool sawOff[128]; std::memset(sawOff, 0, sizeof(sawOff));
    bool sawPanic = false;
    TimedMidi tm{};
    while (node.pop(tm)) {
        if ((tm.ev.status & 0xF0u) == 0x80u) sawOff[tm.ev.data1 & 0x7F] = true;
        if ((tm.ev.status & 0xF0u) == 0xB0u && tm.ev.data1 == 123) sawPanic = true;
    }
    bool allOff = true;
    for (int i = 0; i < kRel; ++i) if (!sawOff[1 + i]) allOff = false;
    CHECK(allOff,   "midiout saturation: every note-off survives a full ring");
    CHECK(sawPanic, "midiout saturation: the all-notes-off panic survives a full ring");
}

// ===========================================================================
// 15. MidiTrackNode: disarming the monitor mid-note must not strand the release.
//
//     Defect: process() appends the live lane only while monitor_ is true.  Turn
//     monitoring off with a key still down and the note-off arriving on the live
//     lane is simply not forwarded — the note the node itself let through is on
//     forever.  Post-fix the node has to let go of what it is holding when the
//     lane closes (its own note-off, or a panic; either satisfies the contract).
// ===========================================================================
static void test_midi_track_monitor_off_releases_held_note() {
    MidiTrackNode track;
    track.prepare(48000.0, 512);
    track.setRouting(0,0);
    track.setMonitor(true);

    MidiEvent  none[1]{};
    MidiEvent  liveOn[1]{ { 4, 0x90, 60, 100 } };
    MidiBuffer ins[2]{ { liveOn, 1, 1 }, { none, 0, 1 } };
    MidiEvent  outEv[16]{};
    MidiBuffer out{ outEv, 0, 16 };
    NodeProcessContext ctx{};
    ctx.nframes = 512;
    ctx.midiIn = ins;  ctx.numMidiIn  = 2;
    ctx.midiOut = &out; ctx.numMidiOut = 1;
    ctx.transport.isPlaying = true;
    ctx.transport.playPositionSamples = 0;
    track.process(ctx);
    CHECK(out.count == 1 && outEv[0].data1 == 60,
          "midi track monitor: armed lane passes the live note-on");

    track.setMonitor(false);                       // key is still down
    MidiEvent  liveOff[1]{ { 8, 0x80, 60, 0 } };
    MidiBuffer ins2[2]{ { liveOff, 1, 1 }, { none, 0, 1 } };
    ctx.midiIn = ins2;
    ctx.transport.playPositionSamples = 512;
    track.process(ctx);

    int rel = 0;
    for (int i = 0; i < out.count; ++i) if (releasesNote(outEv[i], 60)) ++rel;
    CHECK(rel >= 1,
          "midi track monitor: disarming mid-note must not strand the note-off");
}

// ===========================================================================
// 16. MidiTrackNode: an over-capacity merge must drop note-ons, not note-offs.
//
//     Defect: append() stopped at out.capacity, and the LIVE lane is appended
//     second — so a busy playback lane pushed exactly the live releases off the
//     end.  Post-fix nothing is truncated away: the overflow is deferred to the
//     following block, so it may arrive LATE but it may not arrive NEVER.
// ===========================================================================
static void test_midi_track_merge_overflow_keeps_releases() {
    MidiTrackNode track;
    track.prepare(48000.0, 512);
    track.setRouting(0,0);
    track.setMonitor(true);

    MidiEvent play[8]{};
    for (int i = 0; i < 8; ++i) play[i] = MidiEvent{ i, 0x90, (uint8_t)(40 + i), 100 };
    MidiEvent  live[2]{ { 9, 0x80, 40, 0 }, { 10, 0x80, 41, 0 } };
    MidiEvent  none[1]{};
    MidiBuffer ins[2]{ { live, 2, 2 }, { play, 8, 8 } };
    MidiEvent  outEv[8]{};
    MidiBuffer out{ outEv, 0, 8 };                 // capacity < 8 playback + 2 live
    NodeProcessContext ctx{};
    ctx.nframes = 512;
    ctx.midiIn = ins;   ctx.numMidiIn  = 2;
    ctx.midiOut = &out; ctx.numMidiOut = 1;
    ctx.transport.isPlaying = true;
    ctx.transport.playPositionSamples = 0;

    int rel = 0, ons = 0;
    for (int b = 0; b < 2; ++b) {                  // block 2 drains the overflow
        ctx.transport.playPositionSamples = (int64_t)b * 512;
        track.process(ctx);
        CHECK(out.count <= 8, "midi track merge: never writes past out.capacity");
        for (int i = 0; i < out.count; ++i) {
            if (isRelease(outEv[i])) ++rel;
            else if (isNoteOn(outEv[i])) ++ons;
        }
        MidiBuffer empty[2]{ { none, 0, 1 }, { none, 0, 1 } };   // no new input
        ins[0] = empty[0]; ins[1] = empty[1];
    }
    CHECK(rel == 2,
          "midi track merge: an over-capacity merge never truncates a note-off away");
    CHECK(ons == 8,
          "midi track merge: the note-ons that overflowed are deferred, not lost");
    CHECK(track.carryOver() == 0, "midi track merge: the carry buffer drains");
}

// ===========================================================================
// 17. MidiInNode: flush() (transport locate / stop) must release what it sounded.
//
//     Defect: flush() just slams head_ up to tail_.  Every note the node ALREADY
//     delivered stays on, and its scheduled note-off is exactly what got thrown
//     away.  Post-fix the flush leaves the node owing releases, which the next
//     block emits.
// ===========================================================================
static void test_midi_in_flush_releases_held_notes() {
    MidiInNode node;
    constexpr int kN = 512;
    MidiEvent  evs[16];
    MidiBuffer mo{ evs, 0, 16 };
    NodeProcessContext ctx{};
    ctx.nframes = kN;
    ctx.midiOut = &mo; ctx.numMidiOut = 1;
    ctx.transport.isPlaying = true;

    MidiEvent on{ 0, 0x90, 55, 100 };
    CHECK(node.push(on, -1, 0), "midiin flush: push note-on");
    ctx.transport.playPositionSamples = 0;
    node.process(ctx);
    CHECK(mo.count == 1 && evs[0].data1 == 55, "midiin flush: note-on delivered (sounding)");

    MidiEvent off{ 0, 0x80, 55, 0 };
    CHECK(node.push(off, 100000, 0), "midiin flush: its note-off is scheduled ahead");
    node.flush();                                  // transport locate

    ctx.transport.playPositionSamples = kN;
    node.process(ctx);
    int rel = 0;
    for (int i = 0; i < mo.count; ++i) if (releasesNote(evs[i], 55)) ++rel;
    CHECK(rel >= 1,
          "midiin flush: a locate must release notes it already sounded, not strand them");
}

// ===========================================================================
// 18. End-to-end pairing stress across the whole node MIDI path.
//
//     MidiInNode -> a deliberately tiny 4-slot plug buffer -> MidiOutNode ring.
//     This is the general statement of the property, exercised over 120 blocks:
//     whatever the drop policy turns out to be, the stream that comes off the
//     far end must never contain a release ahead of its note-on and must never
//     end with a note still held.
// ===========================================================================
static void test_midi_path_note_pairing_stress() {
    MidiInNode  in;
    MidiOutNode out;
    constexpr int kPairs   = 100;
    constexpr int kN       = 512;
    constexpr int kPlugCap = 4;            // far too small for the burst, on purpose

    for (int k = 0; k < kPairs; ++k) {
        MidiEvent on { 0, 0x90, (uint8_t)k, 100 };
        MidiEvent off{ 0, 0x80, (uint8_t)k, 0   };
        CHECK(in.push(on,  (int64_t)k * 100,      0), "pairing: push note-on");
        CHECK(in.push(off, (int64_t)k * 100 + 50, 0), "pairing: push note-off");
    }

    MidiEvent  plug[kPlugCap];
    MidiBuffer mb{ plug, 0, kPlugCap };
    NodeProcessContext inCtx{}, outCtx{};
    inCtx.nframes  = kN; inCtx.midiOut = &mb; inCtx.numMidiOut = 1;
    outCtx.nframes = kN; outCtx.numMidiIn = 1;
    inCtx.transport.isPlaying = outCtx.transport.isPlaying = true;

    for (int b = 0; b < 120; ++b) {
        const int64_t pos = (int64_t)b * kN;
        inCtx.transport.playPositionSamples  = pos;
        outCtx.transport.playPositionSamples = pos;
        in.process(inCtx);
        MidiBuffer fed{ plug, mb.count, kPlugCap };
        outCtx.midiIn = &fed;
        out.process(outCtx);
    }

    int held[128]; std::memset(held, 0, sizeof(held));
    int offBeforeOn = 0, delivered = 0;
    TimedMidi tm{};
    while (out.pop(tm)) {
        ++delivered;
        const int n = tm.ev.data1 & 0x7F;
        if (isNoteOn(tm.ev))       ++held[n];
        else if (isRelease(tm.ev)) { if (held[n] == 0) ++offBeforeOn; else --held[n]; }
    }
    int stuck = 0;
    for (int n = 0; n < 128; ++n) stuck += held[n];
    CHECK(offBeforeOn == 0, "pairing: never a note-off ahead of its note-on");
    CHECK(stuck == 0,
          "pairing: every note-on that got through is released (no stuck notes)");
    CHECK(delivered == kPairs * 2,
          "pairing: the whole stream survives a 4-slot plug buffer");
}

// ===========================================================================
// 19. VirtualMidiPortsNode: the route table is metadata, but it must be HONEST
//     metadata — whatever eventually consumes it (graph edge resolution today,
//     a real routing layer tomorrow) can only be as correct as this table.
//     Complements test_virtual_midi_endpoints_are_independent, which pins that
//     process() itself never forwards or blends MIDI.
// ===========================================================================
static void test_virtual_midi_route_table_contract() {
    VirtualMidiPortsNode vm;
    vm.setPorts(4, 4);
    CHECK(vm.route(0) == -1, "virtual routes: a fresh bank is unrouted");
    vm.setRoute(1, 3);
    CHECK(vm.route(1) == 3,  "virtual routes: setRoute round-trips");
    vm.setRoute(2, 99);
    CHECK(vm.route(2) == -1, "virtual routes: an out-of-range input is rejected, not stored");
    CHECK(vm.route(99) == -1, "virtual routes: an out-of-range output queries as unrouted");
    // A stale route surviving a shrink is exactly how a patchbay strands a stream.
    vm.setPorts(2, 2);
    CHECK(vm.route(1) == -1, "virtual routes: shrinking the input bank clears stale routes");
    vm.setPorts(4, 4);
    CHECK(vm.route(1) == -1, "virtual routes: a cleared route does not reappear on regrow");
}

static void test_mono_to_stereo_duplicates_channel() {
    constexpr int n = 32;
    std::vector<float> mono(n), left(n, 99.f), right(n, 99.f);
    for (int i=0;i<n;++i) mono[(size_t)i] = (float)i / (float)n;
    float* inPtrs[1] = { mono.data() };
    float* outPtrs[2] = { left.data(), right.data() };
    AudioBus in{inPtrs,1}, out{outPtrs,2};
    NodeProcessContext ctx{}; ctx.nframes=n;
    ctx.audioIn=&in; ctx.numAudioIn=1; ctx.audioOut=&out; ctx.numAudioOut=1;
    MonoToStereoNode node; node.process(ctx);
    CHECK(node.port(0).channels==1 && node.port(1).channels==2,
          "mono2stereo: port widths are mono in and stereo out");
    bool same=true;
    for(int i=0;i<n;++i)
        if(left[(size_t)i]!=mono[(size_t)i] || right[(size_t)i]!=mono[(size_t)i]) same=false;
    CHECK(same,"mono2stereo: mono source is copied identically to L and R");
}

static void test_csound_audio_input_roundtrip() {
    CsoundNode node;
    CHECK(node.prepare(48000.0, 64), "csound input: default engine prepares");
    const std::string csd =
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "sr=48000\nksmps=16\nnchnls=2\n0dbfs=1\n"
        "instr 1\n"
        "aL inch 1\n"
        "aR inch 2\n"
        "outs aL, aR\n"
        "endin\n</CsInstruments>\n"
        "<CsScore>\ni 1 0 3600\n</CsScore>\n</CsoundSynthesizer>\n";
    const bool compiled = node.compile(csd);
    if (!compiled && node.lastError().find("not built") != std::string::npos) return;
    CHECK(compiled, "csound input: inch orchestra compiles without explicit nchnls_i");
    if (!compiled) return;
    CHECK(node.nchnlsInput() == 2, "csound input: inch 2 creates a stereo input bus");

    TestBus input(64), output(64);
    for (int i=0;i<64;++i) { input.l[(size_t)i]=(float)(i+1)/64.f;
                             input.r[(size_t)i]=-(float)(i+1)/64.f; }
    AudioBus ib=input.bus(), ob=output.bus();
    NodeProcessContext ctx{}; ctx.nframes=64;
    ctx.audioIn=&ib; ctx.numAudioIn=1; ctx.audioOut=&ob; ctx.numAudioOut=1;
    node.process(ctx);
    float peakL=0.f, peakR=0.f;
    for(int i=0;i<64;++i){peakL=std::max(peakL,std::fabs(output.l[(size_t)i]));
                          peakR=std::max(peakR,std::fabs(output.r[(size_t)i]));}
    CHECK(peakL > 0.1f && peakR > 0.1f,
          "csound input: graph audio reaches spin and returns through spout");
}

static void test_csound_audio_through_compiled_graph() {
    PatchGraph graph;
    const NodeId sourceId = graph.addNode(std::make_unique<SineSourceNode>(997.f, .4f));
    auto csOwner = std::make_unique<CsoundNode>();
    CsoundNode* cs = csOwner.get();
    const NodeId csId = graph.addNode(std::move(csOwner));
    const NodeId sinkId = graph.addNode(std::make_unique<AudioDeviceOutNode>(2));
    CHECK(graph.prepare(48000.,64), "csound graph: graph prepares");
    const std::string csd =
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "sr=48000\nksmps=16\nnchnls=2\n0dbfs=1\n"
        "instr 1\naL inch 1\naR inch 2\nouts aL,aR\nendin\n"
        "</CsInstruments>\n<CsScore>\ni 1 0 3600\n</CsScore>\n</CsoundSynthesizer>\n";
    if (!cs->compile(csd) && cs->lastError().find("not built") != std::string::npos) return;
    CHECK(cs->compiled(), "csound graph: effect CSD compiles");
    if (!cs->compiled()) return;
    // stereo source out=0, Csound out=0 / inferred audio-in=2, sink in=0
    CHECK(graph.connect({{sourceId,0},{csId,2}}), "csound graph: source cable reaches inferred input");
    CHECK(graph.connect({{csId,0},{sinkId,0}}), "csound graph: Csound output cable reaches sink");
    graph.setDeviceOutNode(sinkId);
    CHECK(graph.compileAndPublish(), "csound graph: dynamic Csound ports publish");
    std::vector<float> l(64),r(64); float* out[2]={l.data(),r.data()};
    RenderContext rc{};rc.isPlaying=true;graph.process(out,2,64,rc);
    float peak=0.f;for(float v:l)peak=std::max(peak,std::fabs(v));
    CHECK(peak>.05f,"csound graph: cable audio survives source -> spin -> spout -> sink");
}


// ===========================================================================
// 30. REGRESSIONS for the confirmed defects fixed in this pass.
// ===========================================================================

// --- tiny building blocks shared by the tests below ------------------------
namespace regr {

// A DC source of fixed amplitude, so tracks can be told apart by ear.
class DcNode : public Node {
public:
    explicit DcNode(float v) : v_(v) {}
    const char* typeName() const override { return "RegrDc"; }
    int numPorts() const override { return 1; }
    PortDesc port(int) const override { return PortDesc{0,PortKind::Audio,PortDir::Out,2,"out"}; }
    bool prepare(double,int) override { return true; }
    void process(const NodeProcessContext& c) override {
        for (int ch = 0; ch < c.audioOut[0].channels; ++ch)
            for (int i = 0; i < c.nframes; ++i) c.audioOut[0].chans[ch][i] = v_;
    }
    float v_;
};

// Device sink that copies (or sums) whatever reaches it into out[].
class SinkNode : public Node {
public:
    const char* typeName() const override { return "RegrSink"; }
    int numPorts() const override { return 1; }
    PortDesc port(int) const override { return PortDesc{0,PortKind::Audio,PortDir::In,2,"in"}; }
    bool prepare(double,int) override { return true; }
    bool isDeviceSink() const override { return true; }
    void setDeviceOutAdditive(bool b) override { add_ = b; }
    void bindDeviceOut(float* const* o, int c, int n) override { t_=o; tc_=c; n_=n; }
    void process(const NodeProcessContext& c) override {
        if (!t_ || c.numAudioIn < 1) return;
        const int cc = std::min(tc_, c.audioIn[0].channels);
        for (int ch = 0; ch < cc; ++ch)
            for (int i = 0; i < n_ && i < c.nframes; ++i) {
                if (add_) t_[ch][i] += c.audioIn[0].chans[ch][i];
                else      t_[ch][i]  = c.audioIn[0].chans[ch][i];
            }
    }
    float* const* t_ = nullptr; int tc_ = 0, n_ = 0; bool add_ = false;
};

// Passthrough that remembers what ARRIVED, so a bus's input can be measured
// exactly instead of inferred from the mix.
class TapNode : public Node {
public:
    const char* typeName() const override { return "RegrTap"; }
    int numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return i == 0 ? PortDesc{0,PortKind::Audio,PortDir::In, 2,"in"}
                      : PortDesc{1,PortKind::Audio,PortDir::Out,2,"out"}; }
    bool prepare(double,int) override { return true; }
    void process(const NodeProcessContext& c) override {
        const AudioBus& o = c.audioOut[0];
        if (c.numAudioIn < 1) {
            for (int ch = 0; ch < o.channels; ++ch)
                for (int i = 0; i < c.nframes; ++i) o.chans[ch][i] = 0.f;
            l = r = 0.f; return;
        }
        const AudioBus& in = c.audioIn[0];
        for (int ch = 0; ch < o.channels; ++ch)
            for (int i = 0; i < c.nframes; ++i)
                o.chans[ch][i] = ch < in.channels ? in.chans[ch][i] : 0.f;
        l = in.channels > 0 ? in.chans[0][0] : 0.f;
        r = in.channels > 1 ? in.chans[1][0] : 0.f;
    }
    float l = 0.f, r = 0.f;
};

} // namespace regr

// ---------------------------------------------------------------------------
// 30a. MasterMixerNode::removeTrack() renames its POSITIONAL port ids, so the
//      owning graph's connections must be remapped in the same edit.  Before
//      the fix, deleting track 0 of three DC sources (1.0/2.0/3.0) left the
//      strips reading 1.0 and 2.0 -- i.e. the deleted track kept sounding, both
//      survivors drove the wrong signal and the 3.0 source vanished entirely.
// ---------------------------------------------------------------------------
static void test_master_mixer_remove_track_rewires_connections() {
    const int N = 32;
    PatchGraph g;
    auto mmU = std::make_unique<MasterMixerNode>();
    MasterMixerNode* mm = mmU.get();
    const NodeId mmId = g.addNode(std::move(mmU));
    NodeId src[3];
    for (int i = 0; i < 3; ++i) src[i] = g.addNode(std::make_unique<regr::DcNode>((float)(i+1)));
    for (int i = 0; i < 3; ++i) mm->addTrack(false);
    for (int i = 0; i < 3; ++i)
        CHECK(g.connect(Connection{PortRef{src[i],0}, PortRef{mmId,(PortId)(2+2*i)}}),
              "remove-track: source wired to its track inlet");
    const NodeId sinkId = g.addNode(std::make_unique<regr::SinkNode>());
    g.connect(Connection{PortRef{mmId,0}, PortRef{sinkId,0}});
    g.setDeviceOutNode(sinkId);
    g.prepare(48000.0, N);
    g.compileAndPublish();

    std::vector<float> L(N), R(N);
    float* out[2] = { L.data(), R.data() };
    RenderContext ctx{};
    // Solo each strip in turn and report what IT is actually carrying.
    auto strip = [&](int t) -> float {
        for (int u = 0; u < mm->trackCount(); ++u) mm->setMute(u, u != t);
        g.process(out, 2, N, ctx);
        for (int u = 0; u < mm->trackCount(); ++u) mm->setMute(u, false);
        return L[0];
    };
    CHECK(std::fabs(strip(0) - 1.f) < 1e-4f, "remove-track baseline: strip 0 == 1.0");
    CHECK(std::fabs(strip(1) - 2.f) < 1e-4f, "remove-track baseline: strip 1 == 2.0");
    CHECK(std::fabs(strip(2) - 3.f) < 1e-4f, "remove-track baseline: strip 2 == 3.0");

    mm->removeTrack(0);
    g.remapNodePorts(mmId, [](PortId pid) {
        return MasterMixerNode::portAfterTrackRemoval((int)pid, 0);
    });
    g.pruneDanglingConnections();
    CHECK(g.compileAndPublish(), "remove-track: graph recompiles after the remap");

    CHECK(mm->trackCount() == 2, "remove-track: two tracks left");
    CHECK(std::fabs(strip(0) - 2.f) < 1e-4f,
          "remove-track: surviving strip 0 now carries the old track 1 (2.0)");
    CHECK(std::fabs(strip(1) - 3.f) < 1e-4f,
          "remove-track: surviving strip 1 now carries the old track 2 (3.0) -- not silence");
    // And the deleted track's source must be gone from the mix entirely.
    for (int u = 0; u < mm->trackCount(); ++u) mm->setMute(u, true);
    g.process(out, 2, N, ctx);
    CHECK(std::fabs(L[0]) < 1e-4f, "remove-track: the deleted track no longer sounds");
    for (int u = 0; u < mm->trackCount(); ++u) mm->setMute(u, false);
}

// ---------------------------------------------------------------------------
// 30b. An EMPTY PluginNode ("Add Effect", plugin chosen later) must publish
//      SILENCE.  Pool slots are recycled on every compile, so returning early
//      without clearing re-published a deleted node's leftover buffer forever.
// ---------------------------------------------------------------------------
static void test_empty_plugin_node_publishes_silence() {
    PluginNode pn(nullptr);
    TestBus in(kBlock, 0.7f), out(kBlock, 0.9f);   // out pre-filled: recycled slot
    AudioBus ib = in.bus(), ob = out.bus();
    MidiEvent stale[4]; std::memset(stale, 0, sizeof(stale));
    MidiBuffer mo{ stale, 3, 4 };                  // stale count from the last tenant

    NodeProcessContext ctx{};
    ctx.nframes = kBlock;
    ctx.audioIn = &ib; ctx.numAudioIn = 1;
    ctx.audioOut = &ob; ctx.numAudioOut = 1;
    ctx.midiOut = &mo; ctx.numMidiOut = 1;
    pn.process(ctx);

    bool silent = true;
    for (int i = 0; i < kBlock; ++i)
        if (out.l[(size_t)i] != 0.f || out.r[(size_t)i] != 0.f) silent = false;
    CHECK(silent, "empty plugin node: audio out is cleared, not recycled pool audio");
    // (No MIDI-out PORT exists on an audio-only PluginNode, so its count is
    // left to the graph; the audio clear is the defect that buzzed.)
}

// ---------------------------------------------------------------------------
// 30c. PER-CHANNEL NaN fence: one poisoned channel must not mute the mix.
// ---------------------------------------------------------------------------
static void test_per_channel_nan_isolation() {
    // --- MixerNode ---
    {
        MixerNode mx(2);
        mx.prepare(48000.0, kBlock);
        TestBus a(kBlock, 0.5f), b(kBlock, 0.f), o(kBlock);
        for (int i = 0; i < kBlock; ++i) { b.l[(size_t)i] = NAN; b.r[(size_t)i] = NAN; }
        AudioBus ins[2] = { a.bus(), b.bus() };
        AudioBus ob = o.bus();
        NodeProcessContext ctx{};
        ctx.nframes = kBlock;
        ctx.audioIn = ins; ctx.numAudioIn = 2;
        ctx.audioOut = &ob; ctx.numAudioOut = 1;
        mx.process(ctx);
        CHECK(std::fabs(o.l[0] - 0.5f) < 1e-4f,
              "mixer per-channel fence: the good channel survives a NaN neighbour");
        CHECK(allFinite(o.l.data(), kBlock) && allFinite(o.r.data(), kBlock),
              "mixer per-channel fence: output stays finite");
        // Inf on the bad channel must be scrubbed too.
        for (int i = 0; i < kBlock; ++i) { b.l[(size_t)i] = INFINITY; b.r[(size_t)i] = -INFINITY; }
        mx.process(ctx);
        CHECK(std::fabs(o.l[0] - 0.5f) < 1e-4f,
              "mixer per-channel fence: the good channel survives an Inf neighbour");
    }
    // --- MasterMixerNode ---
    {
        MasterMixerNode mm;
        mm.addTrack(false); mm.addTrack(false);
        mm.prepare(48000.0, kBlock);
        TestBus a(kBlock, 0.5f), b(kBlock, 0.f);
        for (int i = 0; i < kBlock; ++i) { b.l[(size_t)i] = NAN; b.r[(size_t)i] = NAN; }
        TestBus master(kBlock), o0(kBlock), o1(kBlock);
        AudioBus ins[2] = { a.bus(), b.bus() };
        AudioBus outs[3] = { master.bus(), o0.bus(), o1.bus() };
        NodeProcessContext ctx{};
        ctx.nframes = kBlock;
        ctx.audioIn = ins; ctx.numAudioIn = 2;
        ctx.audioOut = outs; ctx.numAudioOut = 3;
        mm.process(ctx);
        CHECK(std::fabs(master.l[0] - 0.5f) < 1e-4f,
              "master mixer per-track fence: the good track survives a NaN neighbour");
        CHECK(allFinite(master.l.data(), kBlock),
              "master mixer per-track fence: master bus stays finite");
        CHECK(allFinite(o1.l.data(), kBlock),
              "master mixer per-track fence: the poisoned track's own OUTLET is scrubbed too");
    }
}

// ---------------------------------------------------------------------------
// 30d. The record tap is PRE-FADER and is taken even on a muted channel.
// ---------------------------------------------------------------------------
static void test_track_capture_is_pre_fader() {
    // --- MixerNode: muted channel must still record ---
    {
        MixerNode mx(1);
        mx.prepare(48000.0, kBlock);
        mx.setGain(0, 0.25f);
        mx.setMute(0, true);
        std::vector<float> cap((size_t)kBlock * 2, -1.f);
        mx.beginTrackCapture(0, cap.data(), (size_t)kBlock);
        TestBus in(kBlock, 0.8f), o(kBlock);
        AudioBus ib = in.bus(), ob = o.bus();
        NodeProcessContext ctx{};
        ctx.nframes = kBlock;
        ctx.audioIn = &ib; ctx.numAudioIn = 1;
        ctx.audioOut = &ob; ctx.numAudioOut = 1;
        mx.process(ctx);
        CHECK(mx.trackCaptureFrames() == (size_t)kBlock,
              "mixer capture: a MUTED channel still records (it used to record nothing)");
        CHECK(std::fabs(cap[0] - 0.8f) < 1e-4f,
              "mixer capture: the take is PRE-fader/pan/mute (0.8, not 0.8*0.25 and not 0)");
        CHECK(std::fabs(o.l[0]) < 1e-6f, "mixer capture: muting still silences the MONITOR path");
        mx.endTrackCapture();
    }
    // --- MasterMixerNode: fader position must not be baked in ---
    {
        MasterMixerNode mm;
        mm.addTrack(false);
        mm.prepare(48000.0, kBlock);
        mm.setGain(0, 0.5f);
        mm.setPan(0, 1.0f);                 // hard right: L would be zeroed post-pan
        std::vector<float> cap((size_t)kBlock * 2, -1.f);
        mm.beginTrackCapture(0, cap.data(), (size_t)kBlock);
        TestBus in(kBlock, 0.8f), master(kBlock), o0(kBlock);
        AudioBus ib = in.bus();
        AudioBus outs[2] = { master.bus(), o0.bus() };
        NodeProcessContext ctx{};
        ctx.nframes = kBlock;
        ctx.audioIn = &ib; ctx.numAudioIn = 1;
        ctx.audioOut = outs; ctx.numAudioOut = 2;
        mm.process(ctx);
        CHECK(std::fabs(cap[0] - 0.8f) < 1e-4f,
              "master capture: LEFT is the raw inlet, not gain*pan (was 0)");
        CHECK(std::fabs(cap[1] - 0.8f) < 1e-4f, "master capture: RIGHT is the raw inlet too");
        mm.endTrackCapture();
    }
}

// ---------------------------------------------------------------------------
// 30e. beginTrackCapture() while a capture is LIVE must retire the old buffer
//      before any of its parameters move (never pair old buffer + new capacity).
// ---------------------------------------------------------------------------
static void test_begin_capture_retires_the_live_one() {
    MasterMixerNode mm;
    mm.addTrack(false);
    mm.prepare(48000.0, kBlock);
    std::vector<float> capA((size_t)kBlock * 2, 0.f), capB((size_t)kBlock * 2, 0.f);
    mm.beginTrackCapture(0, capA.data(), (size_t)kBlock);
    mm.beginTrackCapture(0, capB.data(), (size_t)kBlock);
    CHECK(mm.trackCaptureFrames() == 0, "begin capture: write cursor restarts for the new take");
    float* live = mm.endTrackCapture();
    CHECK(live == capB.data(), "begin capture: the SECOND buffer is the live one");
}

// ---------------------------------------------------------------------------
// 30f. The 24-PPQN MIDI clock re-seeds on a transport jump taken WHILE PLAYING
//      (loop wrap / click-to-seek), instead of free-running off the grid.
// ---------------------------------------------------------------------------
static void test_master_mixer_clock_resyncs_on_jump() {
    MasterMixerNode mm;
    const int kN = 128;
    mm.prepare(48000.0, kN);
    TestBus master(kN);
    AudioBus ob = master.bus();
    std::vector<MidiEvent> clkEvs(64);
    MidiBuffer clk{ clkEvs.data(), 0, 64 };
    NodeProcessContext ctx{};
    ctx.nframes = kN;
    ctx.audioOut = &ob;  ctx.numAudioOut = 1;
    ctx.midiOut  = &clk; ctx.numMidiOut  = 1;
    ctx.transport.tempoBpm = 120.0;   // 48000*60/(120*24) == 1000 samples per clock

    ctx.transport.isPlaying = false;
    ctx.transport.playPositionSamples = 0;
    mm.process(ctx);
    ctx.transport.isPlaying = true;
    for (int blk = 0; blk < 8; ++blk) {        // roll normally to 1024
        ctx.transport.playPositionSamples = (int64_t)blk * kN;
        mm.process(ctx);
    }
    // LOOP WRAP while still rolling: jump back to 500, i.e. mid-clock-interval.
    ctx.transport.playPositionSamples = 500;
    mm.process(ctx);
    bool sawSpp = false, sawContinue = false;
    long long firstF8 = -1;
    for (int i = 0; i < clk.count; ++i) {
        if (clkEvs[i].status == 0xF2) sawSpp = true;
        if (clkEvs[i].status == 0xFB) sawContinue = true;
        if (clkEvs[i].status == 0xF8 && firstF8 < 0) firstF8 = clkEvs[i].sampleOffset;
    }
    CHECK(sawSpp,      "clock jump: 0xF2 Song Position emitted at the relocate");
    CHECK(sawContinue, "clock jump: 0xFB Continue emitted at the relocate");
    CHECK(firstF8 == 0, "clock jump: the clock is re-seeded, first 0xF8 lands on the jump point");

    // ...and the grid stays locked to the NEW origin from there on.
    std::vector<long long> dues;
    for (int blk = 1; blk < 20; ++blk) {
        ctx.transport.playPositionSamples = 500 + (int64_t)blk * kN;
        mm.process(ctx);
        for (int i = 0; i < clk.count; ++i)
            if (clkEvs[i].status == 0xF8)
                dues.push_back((long long)ctx.transport.playPositionSamples
                               + clkEvs[i].sampleOffset - 500);
    }
    bool spacing = !dues.empty();
    long long prev = 0;
    for (size_t i = 0; i < dues.size(); ++i) { if (dues[i] - prev != 1000) spacing = false; prev = dues[i]; }
    CHECK(spacing, "clock jump: post-jump 0xF8 spacing stays exactly 1000 samples off the new origin");

    // A block that simply keeps rolling must NOT be mistaken for a jump.
    clk.count = 0;
    ctx.transport.playPositionSamples += kN;
    mm.process(ctx);
    bool spurious = false;
    for (int i = 0; i < clk.count; ++i)
        if (clkEvs[i].status == 0xF2 || clkEvs[i].status == 0xFB ||
            clkEvs[i].status == 0xFA) spurious = true;
    CHECK(!spurious, "clock jump: a normal contiguous block emits no start/continue");
}

// ---------------------------------------------------------------------------
// 30g. PatchGraph::compileAndPublish() drains the retired-node list, so a
//      removed node is eventually freed AND release()d instead of leaking for
//      the whole session with its plugin/file/device handles open.
// ---------------------------------------------------------------------------
static void test_compile_drains_retired_nodes() {
    struct Flagged : Node {
        std::atomic<int>* released;
        std::atomic<int>* destroyed;
        explicit Flagged(std::atomic<int>* r, std::atomic<int>* d) : released(r), destroyed(d) {}
        ~Flagged() override { destroyed->fetch_add(1); }
        const char* typeName() const override { return "Flagged"; }
        int numPorts() const override { return 1; }
        PortDesc port(int) const override { return PortDesc{0,PortKind::Audio,PortDir::Out,2,"out"}; }
        bool prepare(double,int) override { return true; }
        void release() override { released->fetch_add(1); }
        void process(const NodeProcessContext& c) override {
            for (int ch = 0; ch < c.audioOut[0].channels; ++ch)
                std::memset(c.audioOut[0].chans[ch], 0, sizeof(float)*(size_t)c.nframes);
        }
    };
    std::atomic<int> released{0}, destroyed{0};
    PatchGraph g;
    const NodeId sink = g.addNode(std::make_unique<regr::SinkNode>());
    const NodeId flag = g.addNode(std::make_unique<Flagged>(&released, &destroyed));
    g.connect(Connection{PortRef{flag,0}, PortRef{sink,0}});
    g.setDeviceOutNode(sink);
    g.prepare(48000.0, kBlock);
    g.compileAndPublish();

    std::vector<float> L(kBlock), R(kBlock);
    float* out[2] = { L.data(), R.data() };
    RenderContext rc{};
    g.process(out, 2, kBlock, rc);

    CHECK(g.removeNode(flag), "gc-on-publish: node retired");
    CHECK(destroyed.load() == 0, "gc-on-publish: NOT freed while the live plan still names it");
    g.compileAndPublish();            // stamps the entry
    g.process(out, 2, kBlock, rc);
    g.compileAndPublish();            // this publish must drain it
    CHECK(destroyed.load() == 1, "gc-on-publish: a later compile frees the retired node");
    CHECK(released.load() == 1, "gc-on-publish: release() ran (handles are given back)");
}

// ---------------------------------------------------------------------------
// 30h. A plan carrying MIDI ports is now eligible for the parallel path, and
//      must render BIT-IDENTICALLY to the sequential one.  (A MasterMixerNode's
//      unconditional MIDI clock-out port used to disqualify every plan there
//      is, which is why "Enable multi-core processing" did nothing at all.)
// ---------------------------------------------------------------------------
static void test_parallel_matches_sequential_with_midi() {
    const int N = 64;
    auto build = [&](PatchGraph& g, MasterMixerNode** mmOut) {
        auto mmU = std::make_unique<MasterMixerNode>();
        MasterMixerNode* mm = mmU.get();
        const NodeId mmId = g.addNode(std::move(mmU));
        for (int t = 0; t < 12; ++t) mm->addTrack(false);
        for (int t = 0; t < 12; ++t) {
            const NodeId src = g.addNode(std::make_unique<regr::DcNode>(0.01f * (float)(t + 1)));
            g.connect(Connection{PortRef{src,0}, PortRef{mmId,(PortId)(2 + 2*t)}});
        }
        // A MIDI sink on the clock port, so the plan really does carry MIDI.
        auto moU = std::make_unique<MidiOutNode>();
        const NodeId moId = g.addNode(std::move(moU));
        g.connect(Connection{PortRef{mmId,1}, PortRef{moId,0}});
        const NodeId sink = g.addNode(std::make_unique<regr::SinkNode>());
        g.connect(Connection{PortRef{mmId,0}, PortRef{sink,0}});
        g.setDeviceOutNode(sink);
        g.prepare(48000.0, N);
        g.compileAndPublish();
        *mmOut = mm;
    };

    std::vector<float> seq, par;
    for (int pass = 0; pass < 2; ++pass) {
        PatchGraph g;
        MasterMixerNode* mm = nullptr;
        build(g, &mm);
        g.setMultiThreaded(pass == 1);
        std::vector<float> L(N), R(N);
        float* out[2] = { L.data(), R.data() };
        std::vector<float>& sink = (pass == 0) ? seq : par;
        RenderContext rc{};
        rc.tempoBpm = 120.0; rc.isPlaying = true;
        for (int blk = 0; blk < 32; ++blk) {
            rc.playPositionSamples = (int64_t)blk * N;
            g.process(out, 2, N, rc);
            sink.insert(sink.end(), L.begin(), L.end());
            sink.insert(sink.end(), R.begin(), R.end());
        }
    }
    CHECK(seq.size() == par.size() && !seq.empty(), "parallel/MIDI: both passes captured");
    CHECK(seq.size() == par.size() &&
          std::memcmp(seq.data(), par.data(), seq.size() * sizeof(float)) == 0,
          "parallel/MIDI: multi-core render is bit-identical to the sequential one");
    bool nonZero = false;
    for (size_t i = 0; i < seq.size(); ++i) if (seq[i] != 0.f) nonZero = true;
    CHECK(nonZero, "parallel/MIDI: the captured mix is not trivially silent");
}

// ===========================================================================

// ---------------------------------------------------------------------------
// 37. AUX SENDS / AUX BUSES.  The send matrix lives in MasterMixerNode (it is
//     the only place that has both the track signal and the fader/mute/solo
//     state), each bus is a real AuxBusNode, and the bus RETURNS into the very
//     mixer that feeds it -- a cycle at node granularity, legal only because
//     the return port is a FEEDBACK port (one block late).
//
//     Measured here: a post-fader send follows the fader, a pre-fader send does
//     not, a MUTED track still feeds its pre-fader sends, level 0 is bit-exact
//     silence, and the return lands ahead of the master fader.
// ---------------------------------------------------------------------------
static void test_aux_send_bus_signal_flow() {
    const int N = 64;
    PatchGraph g;
    auto mmU = std::make_unique<MasterMixerNode>();
    MasterMixerNode* mm = mmU.get();
    const NodeId mmId  = g.addNode(std::move(mmU));
    const NodeId dc    = g.addNode(std::make_unique<regr::DcNode>(1.0f));
    auto tapU = std::make_unique<regr::TapNode>(); regr::TapNode* tap = tapU.get();
    const NodeId tapId = g.addNode(std::move(tapU));
    auto busU = std::make_unique<AuxBusNode>("Reverb"); AuxBusNode* bus = busU.get();
    const NodeId busId = g.addNode(std::move(busU));
    const NodeId sink  = g.addNode(std::make_unique<regr::SinkNode>());

    mm->addTrack(false);
    const int aux  = mm->addAux();
    const int slot = mm->auxSlot(aux);
    CHECK(aux == 0 && slot == 0, "aux: first bus is index 0 / slot 0");

    CHECK(g.connect(Connection{PortRef{dc,0}, PortRef{mmId,2}}), "aux: source -> track inlet");
    CHECK(g.connect(Connection{PortRef{mmId,MasterMixerNode::auxSendPort(slot)}, PortRef{tapId,0}}),
          "aux: mixer send -> bus insert chain");
    CHECK(g.connect(Connection{PortRef{tapId,1}, PortRef{busId,0}}), "aux: chain -> bus in");
    CHECK(g.connect(Connection{PortRef{busId,1}, PortRef{mmId,MasterMixerNode::auxReturnPort(slot)}}),
          "aux: the bus RETURN into its own feeder is accepted (feedback port)");
    CHECK(g.connect(Connection{PortRef{mmId,0}, PortRef{sink,0}}), "aux: mix -> device");
    g.setDeviceOutNode(sink);
    g.prepare(48000.0, N);
    CHECK(g.compileAndPublish(), "aux: the send/return loop COMPILES (no cycle)");

    std::vector<float> L(N), R(N);
    float* out[2] = { L.data(), R.data() };
    RenderContext ctx{};
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            std::fill(L.begin(), L.end(), 0.f); std::fill(R.begin(), R.end(), 0.f);
            g.process(out, 2, N, ctx);
        }
    };
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };

    mm->setGain(0, 1.0f); mm->setMasterGain(1.0f); bus->setReturnGain(1.0f);

    run(3);
    CHECK(tap->l == 0.f, "aux: a bus with no send set receives bit-exact silence");

    CHECK(near(MasterMixerNode::sendNormToGain(1.f), 1.f), "aux taper: norm 1.0 == unity");
    CHECK(MasterMixerNode::sendNormToGain(0.f) == 0.f, "aux taper: norm 0.0 == hard zero");
    CHECK(MasterMixerNode::sendNormToGain(-1.f) == 0.f, "aux taper: a negative norm is silence");
    CHECK(MasterMixerNode::sendNormToGain(0.5f) < MasterMixerNode::sendNormToGain(0.9f),
          "aux taper: monotonic");
    CHECK(near(MasterMixerNode::sendGainToNorm(MasterMixerNode::sendNormToGain(0.6f)), 0.6f),
          "aux taper: norm -> gain -> norm round-trips");

    mm->setSendLevel(0, aux, 1.0f);
    run(3);
    CHECK(near(tap->l, 1.0f), "aux: post-fader send at unity, fader at unity -> 1.0");

    mm->setGain(0, 0.5f);
    run(3);
    CHECK(near(tap->l, 0.5f), "aux: a POST-fader send FOLLOWS the fader");

    mm->setSendPreFader(0, aux, true);
    run(3);
    CHECK(near(tap->l, 1.0f), "aux: a PRE-fader send IGNORES the fader");

    mm->setMute(0, true);
    run(3);
    CHECK(near(tap->l, 1.0f), "aux: a MUTED track still feeds its pre-fader send");
    mm->setSendPreFader(0, aux, false);
    run(3);
    CHECK(tap->l == 0.f, "aux: a muted track's post-fader send is silent");
    mm->setMute(0, false);
    mm->setGain(0, 1.0f);

    mm->setSendEnabled(0, aux, false);
    run(3);
    CHECK(tap->l == 0.f, "aux: a disabled send is silent");
    CHECK(near(mm->sendLevel(0, aux), 1.0f), "aux: a disabled send KEEPS its level");
    mm->setSendEnabled(0, aux, true);

    mm->setSendLevel(0, aux, 0.0f);
    run(3);
    CHECK(tap->l == 0.f, "aux: send level 0 is TRUE silence, not merely quiet");
    mm->setSendLevel(0, aux, 1.0f);

    bus->setReturnGain(0.5f);
    run(5);
    CHECK(near(L[0], 1.5f), "aux: the return reaches the mix at its return gain");
    bus->setMute(true);
    run(5);
    CHECK(near(L[0], 1.0f), "aux: a muted bus returns nothing");
    bus->setMute(false); bus->setReturnGain(1.0f);
    run(5);
    CHECK(near(L[0], 2.0f), "aux: return at unity doubles a unity dry signal");

    mm->setMasterGain(0.25f);
    run(5);
    CHECK(near(L[0], 0.5f), "aux: the return passes THROUGH the master fader");
    mm->setMasterGain(1.0f);
    run(5);

    CHECK(near(bus->peakLeft(), 1.0f) && near(bus->peakRight(), 1.0f),
          "aux: the bus publishes a stereo peak meter");

    // The return leg reads the bus's PREVIOUS block -- assert the delay is
    // exactly one block, so a future scheduling change that lost or doubled it
    // shows up here rather than as a phasey reverb.
    mm->setSendLevel(0, aux, 0.0f);
    std::fill(L.begin(), L.end(), 0.f); g.process(out, 2, N, ctx);
    CHECK(near(L[0], 2.0f), "aux: the block after the send is cut still carries the old return");
    std::fill(L.begin(), L.end(), 0.f); g.process(out, 2, N, ctx);
    CHECK(near(L[0], 1.0f), "aux: the return is exactly ONE block late");
    mm->setSendLevel(0, aux, 1.0f);
}

// ---------------------------------------------------------------------------
// 38. AUX PORT IDS ARE STABLE.  This is the whole reason aux ports are keyed by
//     a SLOT out of a free list instead of being appended after the tracks:
//     addTrack/removeTrack renumber every positional id, and removing bus 0 of
//     three would have renumbered the other two.  A wired port id must keep
//     meaning the same signal until the thing it belongs to is deleted.
// ---------------------------------------------------------------------------
static void test_aux_ports_survive_track_and_bus_edits() {
    const int N = 32;
    PatchGraph g;
    auto mmU = std::make_unique<MasterMixerNode>();
    MasterMixerNode* mm = mmU.get();
    const NodeId mmId = g.addNode(std::move(mmU));
    const NodeId dc   = g.addNode(std::make_unique<regr::DcNode>(1.0f));
    auto tapU = std::make_unique<regr::TapNode>(); regr::TapNode* tap = tapU.get();
    const NodeId tapId = g.addNode(std::move(tapU));
    const NodeId sink  = g.addNode(std::make_unique<regr::SinkNode>());

    for (int i = 0; i < 3; ++i) mm->addTrack(false);
    const int a0 = mm->addAux(), a1 = mm->addAux(), a2 = mm->addAux();
    CHECK(a0 == 0 && a1 == 1 && a2 == 2, "aux ids: three buses");
    const PortId p0 = MasterMixerNode::auxSendPort(mm->auxSlot(a0));
    const PortId p2 = MasterMixerNode::auxSendPort(mm->auxSlot(a2));

    g.connect(Connection{PortRef{dc,0}, PortRef{mmId,(PortId)(2+2*1)}});   // track 1
    g.connect(Connection{PortRef{mmId,p2}, PortRef{tapId,0}});             // bus 2's send
    g.connect(Connection{PortRef{mmId,0}, PortRef{sink,0}});
    g.setDeviceOutNode(sink);
    g.prepare(48000.0, N);
    mm->setSendLevel(1, a2, 1.0f);
    CHECK(g.compileAndPublish(), "aux ids: compiles");

    std::vector<float> L(N), R(N);
    float* out[2] = { L.data(), R.data() };
    RenderContext ctx{};
    auto run = [&]{ std::fill(L.begin(), L.end(), 0.f); g.process(out, 2, N, ctx); };
    run(); run();
    CHECK(std::fabs(tap->l - 1.0f) < 1e-5f, "aux ids: baseline -- track 1 sends to bus 2");

    // addTrack used to have no remap step at all, because appending a track
    // never shifted anything.  With aux ports appended after the tracks it
    // would have.
    mm->addTrack(false);
    g.compileAndPublish();
    run(); run();
    CHECK(std::fabs(tap->l - 1.0f) < 1e-5f, "aux ids: addTrack does not move an aux port");
    CHECK(MasterMixerNode::auxSendPort(mm->auxSlot(a2)) == p2, "aux ids: send id unchanged by addTrack");

    // removeTrack COMPACTS the channels, so the send matrix has to shift with
    // them: track 2's sends become track 1's.
    mm->removeTrack(0);
    g.remapNodePorts(mmId, [](PortId pid){ return MasterMixerNode::portAfterTrackRemoval((int)pid, 0); });
    g.pruneDanglingConnections();
    CHECK(g.compileAndPublish(), "aux ids: recompiles after a track removal");
    run(); run();
    CHECK(std::fabs(tap->l - 1.0f) < 1e-5f,
          "aux ids: removeTrack shifts the send MATRIX with the channels");
    CHECK(MasterMixerNode::auxSendPort(mm->auxSlot(a2)) == p2, "aux ids: send id unchanged by removeTrack");
    CHECK(MasterMixerNode::portAfterTrackRemoval((int)p0, 0) == (int)p0,
          "aux ids: portAfterTrackRemoval leaves aux ports ALONE");

    // Deleting bus 0 compacts the INDEX list only.  Bus 2 becomes index 1 and
    // keeps its port ids, so the cable drawn to it still carries its signal.
    mm->removeAux(a0);
    g.pruneDanglingConnections();
    CHECK(g.compileAndPublish(), "aux ids: recompiles after a bus removal");
    CHECK(mm->auxCount() == 2, "aux ids: two buses left");
    CHECK(MasterMixerNode::auxSendPort(mm->auxSlot(1)) == p2,
          "aux ids: the surviving bus KEEPS its port id (only its index moved)");
    run(); run();
    CHECK(std::fabs(tap->l - 1.0f) < 1e-5f,
          "aux ids: deleting another bus does not re-aim this one's send");
    CHECK(std::fabs(mm->sendLevel(0, 1) - 1.0f) < 1e-5f,
          "aux ids: the send matrix followed the bus, not the index");
}

int main() {
    test_sine_sr_zero();
    test_mixer_nan_fence();
    test_device_out_clamp_and_nan();
    test_device_in_clamp();
    test_plugin_swap_rt();
    test_master_mixer_edit_race();
    test_midi_in_timed_holdback();
    test_master_mixer_clock();
    test_midi_out_due_stamp();
    test_midi_track_uses_capture_arrival_sample();
    test_midi_track_capture_unwraps_loop();
    test_midi_track_capture_wrap_survives_silence();
    test_virtual_midi_endpoints_are_independent();
    test_midi_track_monitor_is_private();
    test_midi_in_never_falls_back_to_track_zero();
    test_hardware_and_sequencer_sources_are_isolated();

    // --- MIDI delivery hardening (a note-on that gets through is released) ---
    test_midi_in_full_plug_never_loses_events();
    test_midi_in_live_not_blocked_by_scheduled();
    test_midi_in_untargeted_release_is_reaimed();
    test_midi_out_ring_saturation_keeps_releases();
    test_midi_track_monitor_off_releases_held_note();
    test_midi_track_merge_overflow_keeps_releases();
    test_midi_in_flush_releases_held_notes();
    test_midi_path_note_pairing_stress();
    test_virtual_midi_route_table_contract();
    test_mono_to_stereo_duplicates_channel();
    test_csound_audio_input_roundtrip();
    test_csound_audio_through_compiled_graph();

    // --- regressions for the confirmed-defect pass ---
    test_master_mixer_remove_track_rewires_connections();
    test_empty_plugin_node_publishes_silence();
    test_per_channel_nan_isolation();
    test_track_capture_is_pre_fader();
    test_begin_capture_retires_the_live_one();
    test_master_mixer_clock_resyncs_on_jump();
    test_compile_drains_retired_nodes();
    test_parallel_matches_sequential_with_midi();

    // --- aux sends / aux buses ---
    test_aux_send_bus_signal_flow();
    test_aux_ports_survive_track_and_bus_edits();

    if (g_failures == 0) { std::printf("patch_nodes_test: ALL PASS\n"); return 0; }
    std::printf("patch_nodes_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
