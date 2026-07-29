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
//----------------------------------------------------------------------------
#include "patch_nodes.h"

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

    TestBus out(bigBlock, 42.f);                  // sentinel: must be overwritten
    AudioBus ob = out.bus();
    NodeProcessContext ctx{};
    ctx.nframes = bigBlock;                       // oversized vs the binding
    ctx.audioOut = &ob; ctx.numAudioOut = 1;
    node.process(ctx);

    CHECK(out.l[0] == 0.75f,             "devin: copies bound frames");
    CHECK(out.l[devFrames - 1] == 0.75f, "devin: copy reaches the bound length");
    bool remainderZero = true;
    for (int i = devFrames; i < bigBlock; ++i)
        if (out.l[i] != 0.f || out.r[i] != 0.f) remainderZero = false;
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
static void test_record_tick_rounding() {
    RecordNode rec;
    rec.prepare(48000.0, 512);
    rec.setRecording(true);

    MidiEvent evs[2]{};
    evs[0].status = 0x90; evs[0].data1 = 60; evs[0].sampleOffset = 20;   // abs 720 -> tick 24 (old: 23+0)
    evs[1].status = 0x80; evs[1].data1 = 60; evs[1].sampleOffset = 50;   // abs 750 -> tick 25 (old: 23+1)
    MidiBuffer mi{ evs, 2, 2 };
    MidiEvent thruEvs[8];
    MidiBuffer thru{ thruEvs, 0, 8 };

    NodeProcessContext ctx{};
    ctx.nframes = 512;
    ctx.midiIn  = &mi;   ctx.numMidiIn  = 1;
    ctx.midiOut = &thru; ctx.numMidiOut = 1;
    ctx.transport.tempoBpm = 500.0;
    ctx.transport.playPositionSamples = 700;
    ctx.transport.isPlaying = true;
    rec.process(ctx);

    RecordNode::Ev out[8];
    const int n = rec.drain(out, 8);
    CHECK(n == 2, "record: both events captured");
    CHECK(n == 2 && out[0].tick == 24, "record: sample 720 @ spt=30 -> tick 24 exact (32nd grid)");
    CHECK(n == 2 && out[1].tick == 25, "record: sample 750 -> tick 25 (old double-floor gave 24)");
}

// ===========================================================================
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
    test_record_tick_rounding();

    if (g_failures == 0) { std::printf("patch_nodes_test: ALL PASS\n"); return 0; }
    std::printf("patch_nodes_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
