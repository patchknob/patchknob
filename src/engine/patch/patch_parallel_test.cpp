//----------------------------------------------------------------------------
//  PatchKnob — patch-graph PARALLEL/HARDENING self-test.
//
//  Exercises the multi-core dispatch path and the realtime-hardening measures
//  against the identical single-threaded baseline (no real plugins required):
//
//    1. EQUIVALENCE : a seeded ~200-node layered random audio DAG rendered 64
//                     blocks sequentially, then rebuilt identically and
//                     rendered with setMultiThreaded(true) — the two captures
//                     must be BIT-EXACT (memcmp == 0). This is the permanent
//                     "parallel == sequential" safety net.
//    2. CAPACITY    : a 500-node graph (sine/gain chains) must add, compile
//                     and render finite audio (kMaxNodes >= 512 + heap plans).
//    3. THROWING    : a node whose process() throws must degrade to silence on
//                     BOTH paths — downstream nodes still run, the parallel
//                     wave barrier still drains, nothing hangs (a watchdog
//                     thread turns a hang into a test failure).
//    4. RCU STRESS  : an audio thread loops process() while the message thread
//                     does rapid add/connect/compileAndPublish/removeNode/
//                     collectGarbage for a few seconds — no crash, no torn
//                     plan (output stays finite throughout).
//    5. GC EPOCH    : a retired node is freed only after a compile has
//                     published a plan without it (epoch-gated collectGarbage).
//----------------------------------------------------------------------------
#include "patch_graph.h"
#include "patch_nodes.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace PatchKnob::engine;
using namespace PatchKnob::engine::patch;

static const double kSr  = 48000.0;
static const int    kBlk = 128;

// ---------------------------------------------------------------------------
// Tiny deterministic LCG so both builds of the equivalence DAG draw the exact
// same numbers on every platform (no std::mt19937 dependency).
// ---------------------------------------------------------------------------
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
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

// ---------------------------------------------------------------------------
// A node whose process() always throws — the hostile-plugin stand-in.
// ---------------------------------------------------------------------------
class ThrowNode : public Node {
public:
    const char* typeName() const override { return "ThrowNode"; }
    int      numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return (i == 0) ? PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "in"  }
                        : PortDesc{ 1, PortKind::Audio, PortDir::Out, 2, "out" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext&) override {
        throw std::runtime_error("hostile node");
    }
};

// ---------------------------------------------------------------------------
// A port-less node whose destructor raises a flag — observes collectGarbage.
// ---------------------------------------------------------------------------
class DtorFlagNode : public Node {
public:
    explicit DtorFlagNode(bool* flag) : flag_(flag) {}
    ~DtorFlagNode() override { *flag_ = true; }
    const char* typeName() const override { return "DtorFlagNode"; }
    int      numPorts() const override { return 0; }
    PortDesc port(int) const override { return PortDesc{ 0, PortKind::Audio, PortDir::In, 1, "" }; }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext&) override {}
private:
    bool* flag_;
};

// ---------------------------------------------------------------------------
// Equivalence DAG: kLayers x kWidth layered graph. Layer 0 is sines; every
// later node is a GainNode fed by 1..4 random picks from the previous layer
// (edges only flow lower layer -> higher layer, so the DAG is acyclic and the
// dependency waves are genuinely wide). The last four nodes feed the device
// sink. Identical seed => identical graph, gains, frequencies and phases.
// ---------------------------------------------------------------------------
static const int kLayers = 12;
static const int kWidth  = 16;

static void buildLayered(PatchGraph& g, uint32_t seed) {
    Lcg rng(seed);
    NodeId prev[kWidth];
    PortId prevOut[kWidth];

    for (int w = 0; w < kWidth; ++w) {
        const float freq = 100.0f + (float)rng.range(0, 4000);
        const float amp  = 0.02f + 0.002f * (float)rng.range(0, 20);
        auto n = std::make_unique<SineSourceNode>(freq, amp);
        prevOut[w] = portOf(n.get(), PortKind::Audio, PortDir::Out);
        prev[w]    = g.addNode(std::move(n));
    }
    for (int L = 1; L < kLayers; ++L) {
        NodeId cur[kWidth];
        PortId curOut[kWidth];
        for (int w = 0; w < kWidth; ++w) {
            auto n = std::make_unique<GainNode>(0.2f + 0.01f * (float)rng.range(0, 50));
            const PortId in = portOf(n.get(), PortKind::Audio, PortDir::In);
            curOut[w] = portOf(n.get(), PortKind::Audio, PortDir::Out);
            cur[w]    = g.addNode(std::move(n));
            const int fan = rng.range(1, 4);
            for (int k = 0; k < fan; ++k) {
                const int pick = rng.range(0, kWidth - 1);
                // Duplicate picks are rejected by connect(); both builds draw
                // the same picks, so the surviving edge set is identical.
                g.connect({{prev[pick], prevOut[pick]}, {cur[w], in}});
            }
        }
        std::memcpy(prev, cur, sizeof(prev));
        std::memcpy(prevOut, curOut, sizeof(prevOut));
    }
    auto outUp = std::make_unique<AudioDeviceOutNode>(2);
    const PortId outIn = portOf(outUp.get(), PortKind::Audio, PortDir::In);
    const NodeId outId = g.addNode(std::move(outUp));
    for (int w = 0; w < 4; ++w)
        g.connect({{prev[w], prevOut[w]}, {outId, outIn}});
    g.setDeviceOutNode(outId);
}

// Render `blocks` blocks of kBlk frames and append every output sample to cap.
static void renderInto(PatchGraph& g, int blocks, std::vector<float>& cap) {
    std::vector<float> L((size_t)kBlk), R((size_t)kBlk);
    float* out[2] = { L.data(), R.data() };
    RenderContext rc; rc.tempoBpm = 120.0; rc.isPlaying = true;
    for (int b = 0; b < blocks; ++b) {
        g.process(out, 2, kBlk, rc);
        cap.insert(cap.end(), L.begin(), L.end());
        cap.insert(cap.end(), R.begin(), R.end());
    }
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("  FAIL: %s\n", msg); ++failures; }
        else       { std::printf("  ok:   %s\n", msg); }
    };

    // Global watchdog: a hang anywhere (a lost wave-barrier decrement would
    // spin the audio thread forever) becomes a hard test FAILURE, not a stuck
    // CI job.
    std::atomic<bool> allDone{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 1200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (allDone.load()) return;
        }
        std::fprintf(stderr, "\n[patch_parallel_test] WATCHDOG TIMEOUT (hang) — FAILED\n");
        std::_Exit(3);
    });

    // ======================================================================
    // 1. EQUIVALENCE: sequential vs parallel renders must be bit-exact.
    // ======================================================================
    std::printf("[patch_parallel_test] equivalence (seq vs parallel, %d nodes)...\n",
                kLayers * kWidth + 1);
    {
        const uint32_t seed   = 0xC0FFEEu;
        const int      blocks = 64;

        PatchGraph gSeq;
        buildLayered(gSeq, seed);
        check(gSeq.prepare(kSr, kBlk), "sequential graph prepared");
        check(gSeq.lastCompileOk(), "sequential graph compiled");
        std::vector<float> seqCap;
        renderInto(gSeq, blocks, seqCap);

        PatchGraph gPar;
        buildLayered(gPar, seed);
        gPar.setMultiThreaded(true);
        check(gPar.prepare(kSr, kBlk), "parallel graph prepared");
        check(gPar.lastCompileOk(), "parallel graph compiled");
        std::vector<float> parCap;
        renderInto(gPar, blocks, parCap);

        bool nonSilent = false;
        for (float v : seqCap) if (std::fabs(v) > 1e-6f) { nonSilent = true; break; }
        check(nonSilent, "equivalence DAG produces audio");
        check(seqCap.size() == parCap.size(), "capture sizes match");
        const bool exact = seqCap.size() == parCap.size() &&
                           std::memcmp(seqCap.data(), parCap.data(),
                                       seqCap.size() * sizeof(float)) == 0;
        if (!exact) {
            for (size_t i = 0; i < seqCap.size() && i < parCap.size(); ++i) {
                if (seqCap[i] != parCap[i]) {
                    const int blk = (int)(i / ((size_t)kBlk * 2));
                    const int ch  = (int)((i / (size_t)kBlk) % 2);
                    const int smp = (int)(i % (size_t)kBlk);
                    std::printf("  first diff at block %d ch %d sample %d: %.9g vs %.9g\n",
                                blk, ch, smp, seqCap[i], parCap[i]);
                    break;
                }
            }
        }
        check(exact, "parallel render is BIT-EXACT vs sequential (memcmp == 0)");
    }

    // ======================================================================
    // 2. CAPACITY: 500 nodes must add, compile and render.
    // ======================================================================
    std::printf("[patch_parallel_test] capacity (500 nodes)...\n");
    {
        PatchGraph g;
        bool allAdded = true;
        int  nodeCount = 0;

        auto outUp = std::make_unique<AudioDeviceOutNode>(2);
        const PortId devIn = portOf(outUp.get(), PortKind::Audio, PortDir::In);
        const NodeId devId = g.addNode(std::move(outUp));
        allAdded = allAdded && devId != 0; ++nodeCount;

        // 10 sine sources feeding 10 gain chains (489 gains total), every
        // chain tail summed into the device sink: 1 + 10 + 489 == 500 nodes.
        const int kChains = 10, kGains = 489;
        for (int c = 0; c < kChains; ++c) {
            const int len = kGains / kChains + (c < kGains % kChains ? 1 : 0);
            auto sineUp = std::make_unique<SineSourceNode>(110.0f * (float)(c + 1), 0.05f);
            const PortId sineOut = portOf(sineUp.get(), PortKind::Audio, PortDir::Out);
            NodeId prev = g.addNode(std::move(sineUp));
            PortId prevOut = sineOut;
            allAdded = allAdded && prev != 0; ++nodeCount;
            for (int i = 0; i < len; ++i) {
                auto gUp = std::make_unique<GainNode>(0.995f);
                const PortId gIn  = portOf(gUp.get(), PortKind::Audio, PortDir::In);
                const PortId gOut = portOf(gUp.get(), PortKind::Audio, PortDir::Out);
                const NodeId gid  = g.addNode(std::move(gUp));
                allAdded = allAdded && gid != 0; ++nodeCount;
                if (!gid) continue;
                g.connect({{prev, prevOut}, {gid, gIn}});
                prev = gid; prevOut = gOut;
            }
            g.connect({{prev, prevOut}, {devId, devIn}});
        }
        g.setDeviceOutNode(devId);

        check(nodeCount == 500, "built exactly 500 nodes");
        check(allAdded, "every addNode returned nonzero");
        g.setMultiThreaded(true);
        check(g.prepare(kSr, kBlk), "500-node prepare succeeded");
        check(g.lastCompileOk(), "500-node compile + publish succeeded");

        std::vector<float> cap;
        renderInto(g, 4, cap);
        bool finite = true, nonSilent = false;
        for (float v : cap) {
            if (!std::isfinite(v)) finite = false;
            if (std::fabs(v) > 1e-6f) nonSilent = true;
        }
        check(finite,    "500-node render is finite");
        check(nonSilent, "500-node render is non-silent");
    }

    // ======================================================================
    // 3. THROWING NODE: both paths return, thrower degrades to silence,
    //    downstream still runs, no hang (the watchdog is the hang detector).
    // ======================================================================
    std::printf("[patch_parallel_test] throwing node (sequential + parallel)...\n");
    for (int pass = 0; pass < 2; ++pass) {
        const bool mt = pass == 1;
        PatchGraph g;
        // Layer 0: 8 sines. The thrower's feed has a HUGE amplitude so any
        // leak of its audio would be unmistakable. Layer 1: 7 gains + the
        // thrower. Layer 2: a gain fed by the thrower (downstream must still
        // run). Everything sums into the device sink: 18 steps, 4 waves.
        NodeId sine[8]; PortId sineOut[8];
        for (int i = 0; i < 8; ++i) {
            const float amp = (i == 7) ? 1000.0f : 0.05f;
            auto n = std::make_unique<SineSourceNode>(200.0f + 50.0f * (float)i, amp);
            sineOut[i] = portOf(n.get(), PortKind::Audio, PortDir::Out);
            sine[i]    = g.addNode(std::move(n));
        }
        NodeId mid[8]; PortId midOut[8];
        for (int i = 0; i < 7; ++i) {
            auto n = std::make_unique<GainNode>(1.0f);
            const PortId in = portOf(n.get(), PortKind::Audio, PortDir::In);
            midOut[i] = portOf(n.get(), PortKind::Audio, PortDir::Out);
            mid[i]    = g.addNode(std::move(n));
            g.connect({{sine[i], sineOut[i]}, {mid[i], in}});
        }
        auto thrUp = std::make_unique<ThrowNode>();
        const PortId thrIn = portOf(thrUp.get(), PortKind::Audio, PortDir::In);
        midOut[7] = portOf(thrUp.get(), PortKind::Audio, PortDir::Out);
        mid[7]    = g.addNode(std::move(thrUp));
        g.connect({{sine[7], sineOut[7]}, {mid[7], thrIn}});

        auto dsUp = std::make_unique<GainNode>(1.0f);
        const PortId dsIn  = portOf(dsUp.get(), PortKind::Audio, PortDir::In);
        const PortId dsOut = portOf(dsUp.get(), PortKind::Audio, PortDir::Out);
        const NodeId dsId  = g.addNode(std::move(dsUp));
        g.connect({{mid[7], midOut[7]}, {dsId, dsIn}});

        auto outUp = std::make_unique<AudioDeviceOutNode>(2);
        const PortId devIn = portOf(outUp.get(), PortKind::Audio, PortDir::In);
        const NodeId devId = g.addNode(std::move(outUp));
        for (int i = 0; i < 7; ++i) g.connect({{mid[i], midOut[i]}, {devId, devIn}});
        g.connect({{dsId, dsOut}, {devId, devIn}});
        g.setDeviceOutNode(devId);

        g.setMultiThreaded(mt);
        check(g.prepare(kSr, kBlk), mt ? "throw graph (parallel) prepared"
                                       : "throw graph (sequential) prepared");
        std::vector<float> cap;
        renderInto(g, 8, cap);   // throws inside EVERY block

        bool finite = true, nonSilent = false, bounded = true;
        for (float v : cap) {
            if (!std::isfinite(v)) finite = false;
            if (std::fabs(v) > 1e-6f) nonSilent = true;
            if (std::fabs(v) > 10.0f) bounded = false;   // 1000-amp leak check
        }
        check(finite, "throwing-node render returned finite output");
        check(nonSilent, "downstream/sibling nodes still ran (non-silent)");
        check(bounded, "thrower's outputs were zeroed (no 1000-amp leak)");
    }

    // ======================================================================
    // 4. RCU STRESS: continuous audio blocks vs rapid message-thread edits.
    // ======================================================================
    std::printf("[patch_parallel_test] RCU stress (~3 s)...\n");
    {
        PatchGraph g;
        NodeId sine[4]; PortId sineOut[4];
        for (int i = 0; i < 4; ++i) {
            auto n = std::make_unique<SineSourceNode>(220.0f * (float)(i + 1), 0.05f);
            sineOut[i] = portOf(n.get(), PortKind::Audio, PortDir::Out);
            sine[i]    = g.addNode(std::move(n));
        }
        auto outUp = std::make_unique<AudioDeviceOutNode>(2);
        const PortId devIn = portOf(outUp.get(), PortKind::Audio, PortDir::In);
        const NodeId devId = g.addNode(std::move(outUp));
        for (int i = 0; i < 4; ++i) {
            auto gUp = std::make_unique<GainNode>(0.5f);
            const PortId gIn  = portOf(gUp.get(), PortKind::Audio, PortDir::In);
            const PortId gOut = portOf(gUp.get(), PortKind::Audio, PortDir::Out);
            const NodeId gid  = g.addNode(std::move(gUp));
            g.connect({{sine[i], sineOut[i]}, {gid, gIn}});
            g.connect({{gid, gOut}, {devId, devIn}});
        }
        g.setDeviceOutNode(devId);
        g.setMultiThreaded(true);
        check(g.prepare(kSr, kBlk), "stress graph prepared");

        std::atomic<bool> stop{false};
        std::atomic<bool> sawNonFinite{false};
        std::atomic<long> blocksRendered{0};
        std::thread audio([&] {
            std::vector<float> L((size_t)kBlk), R((size_t)kBlk);
            float* out[2] = { L.data(), R.data() };
            RenderContext rc; rc.tempoBpm = 120.0; rc.isPlaying = true;
            while (!stop.load(std::memory_order_relaxed)) {
                g.process(out, 2, kBlk, rc);
                for (int i = 0; i < kBlk; ++i)
                    if (!std::isfinite(L[(size_t)i]) || !std::isfinite(R[(size_t)i]))
                        sawNonFinite.store(true, std::memory_order_relaxed);
                blocksRendered.fetch_add(1, std::memory_order_relaxed);
            }
        });

        long compiles = 0, compileFails = 0;
        std::vector<NodeId> extras;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
            auto gUp = std::make_unique<GainNode>(0.01f);
            const PortId gIn  = portOf(gUp.get(), PortKind::Audio, PortDir::In);
            const PortId gOut = portOf(gUp.get(), PortKind::Audio, PortDir::Out);
            const NodeId gid  = g.addNode(std::move(gUp));
            if (gid) {
                g.connect({{sine[0], sineOut[0]}, {gid, gIn}});
                g.connect({{gid, gOut}, {devId, devIn}});
                extras.push_back(gid);
            }
            if (g.compileAndPublish()) ++compiles; else ++compileFails;
            if (extras.size() > 32) {
                g.removeNode(extras.front());
                extras.erase(extras.begin());
                if (g.compileAndPublish()) ++compiles; else ++compileFails;
                g.collectGarbage();
            }
        }
        stop.store(true);
        audio.join();

        std::printf("  (%ld compiles, %ld failed, %ld audio blocks)\n",
                    compiles, compileFails, blocksRendered.load());
        check(compiles > 100, "stress performed a meaningful number of compiles");
        check(compileFails == 0, "no compile was starved of a plan slot");
        check(blocksRendered.load() > 100, "audio thread kept rendering throughout");
        check(!sawNonFinite.load(), "no torn plan: output stayed finite throughout");
    }

    // ======================================================================
    // 5. GC EPOCH: retired nodes free only after a publish has passed.
    // ======================================================================
    std::printf("[patch_parallel_test] epoch-gated collectGarbage...\n");
    {
        bool freed = false;
        PatchGraph g;
        const NodeId id = g.addNode(std::make_unique<DtorFlagNode>(&freed));
        check(id != 0, "flag node added");
        check(g.prepare(kSr, kBlk), "gc graph prepared");
        check(g.removeNode(id), "flag node retired");

        // The LIVE plan was compiled before the removal and still references
        // the node: collectGarbage must NOT free it yet.
        g.collectGarbage();
        check(!freed, "retired node NOT freed while the live plan references it");

        // After a publish without the node (audio idle => even generation) the
        // epoch gate opens and the node is freed.
        check(g.compileAndPublish(), "recompiled without the node");
        g.collectGarbage();
        check(freed, "retired node freed once the publish generation passed");
    }

    std::printf("\n[patch_parallel_test] %s (%d failure%s)\n",
                failures == 0 ? "PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    allDone.store(true);
    watchdog.join();
    return failures == 0 ? 0 : 1;
}
