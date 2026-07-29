//----------------------------------------------------------------------------
//  PatchKnob — RemoteNode self-test (distributed increment 1).
//
//  Headless, deterministic, one process, loopback only — THE gate for the
//  first distributed increment:
//
//    A. An in-process ECHO RESPONDER thread on 127.0.0.1 (recvfrom -> reply
//       the same payload+seq) stands in for a Pi running an identity
//       sub-graph. RemoteNode at 48 kHz / 64 frames stereo, D=2, is driven
//       directly through hand-built NodeProcessContexts with a known ramp
//       x[n] = n for 512 blocks. Asserts BIT-EXACT audioOut[n] ==
//       audioIn[n - 128] (128 == D*block == latencySamples()) past warm-up,
//       underruns()==0, and warm-up blocks silent.
//    B. Negative: the responder DROPS every 50th datagram. The run must
//       complete with counted concealments (repeat-last-block verified for
//       the first loss), no hang, no crash, process() wall time bounded.
//    C. Failure: NO responder at all. After >500 ms of silence linkDown()
//       must be true and the output silent; the node keeps running.
//    D. Woven through A+B: the debug thread-id capture proves that no socket
//       syscall ever happened on the thread calling process() (the audio
//       thread never syscalls — the invariant the whole design hangs on).
//
//  The harness paces itself on RemoteLink::returnReady() so the pass/fail is
//  deterministic regardless of scheduling; the WAITING happens in the TEST,
//  never inside process().
//----------------------------------------------------------------------------
#include "remote_node.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace PatchKnob::engine::patch;
namespace nsock = PatchKnob::engine::patch::net::netsock;

static const double kSr     = 48000.0;
static const int    kBlock  = 64;
static const int    kDepth  = 2;      // D: pipeline delay in blocks
static const int    kBlocks = 512;

using Clock = std::chrono::steady_clock;
static double usSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// EchoResponder — stands in for a Pi running an IDENTITY sub-graph: recvfrom
// a datagram, reply the same payload + seq to the sender. Optionally drops
// every Nth datagram to simulate loss on the wire.
// ---------------------------------------------------------------------------
struct EchoResponder {
    nsock::socket_t       sock = nsock::kInvalidSocket;
    uint16_t              port = 0;
    int                   dropEvery = 0;
    std::thread           th;
    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> dropped{0};

    bool start(int dropEveryN) {
        dropEvery = dropEveryN;
        if (!nsock::startup()) return false;
        sock = nsock::openUdp();
        if (sock == nsock::kInvalidSocket) { nsock::cleanup(); return false; }
        if (!nsock::bindLocal(sock, 0)) return false;   // ephemeral port
        port = nsock::boundPort(sock);
        nsock::setRecvTimeoutMs(sock, 20);              // keeps stop_ responsive
        th = std::thread([this] { loop(); });
        return port != 0;
    }
    void loop() {
        std::vector<uint8_t> buf(2048);
        while (!stop.load(std::memory_order_relaxed)) {
            nsock::Endpoint from;
            const int n = nsock::recvFrom(sock, buf.data(), (int)buf.size(), &from);
            if (n <= 0) continue;                       // timeout: poll stop again
            const uint64_t k = ++received;
            if (dropEvery > 0 && (k % (uint64_t)dropEvery) == 0) { ++dropped; continue; }
            nsock::sendTo(sock, buf.data(), n, from);
        }
    }
    void shutdown() {
        stop.store(true, std::memory_order_relaxed);
        if (th.joinable()) th.join();
        if (sock != nsock::kInvalidSocket) {
            nsock::closeSocket(sock);
            sock = nsock::kInvalidSocket;
            nsock::cleanup();
        }
    }
};

// ---------------------------------------------------------------------------
// Drive `node` for kBlocks blocks of the prefilled ramp, pacing the HARNESS
// (never process()) on returnReady() so results are deterministic. Records
// wall-clock stats for the honesty report.
// ---------------------------------------------------------------------------
struct RunStats {
    double procMaxUs   = 0.0, procTotUs = 0.0;
    double waitMaxUs   = 0.0, waitTotUs = 0.0;
    int    waitTimeouts = 0;
    int    firstMissedSeq = -1;
};

static void driveBlocks(RemoteNode& node, int waitTimeoutMs,
                        std::vector<float> inAll[2], std::vector<float> outAll[2],
                        RunStats& st) {
    float* inPtrs[2]  = { nullptr, nullptr };
    float* outPtrs[2] = { nullptr, nullptr };
    AudioBus inBus{ inPtrs, 2 };
    AudioBus outBus{ outPtrs, 2 };

    NodeProcessContext ctx{};
    ctx.nframes  = kBlock;
    ctx.audioIn  = &inBus;  ctx.numAudioIn  = 1;
    ctx.audioOut = &outBus; ctx.numAudioOut = 1;

    for (int b = 0; b < kBlocks; ++b) {
        for (int c = 0; c < 2; ++c) {
            inPtrs[c]  = &inAll[c][(size_t)b * kBlock];
            outPtrs[c] = &outAll[c][(size_t)b * kBlock];
        }
        // Pace on the DUE return (seq b-D) so a slow scheduler can't turn a
        // healthy link into spurious underruns. On timeout, process() anyway:
        // that is exactly the loss/concealment path.
        if (b >= kDepth) {
            const uint32_t due = (uint32_t)(b - kDepth);
            const Clock::time_point w0 = Clock::now();
            const Clock::time_point deadline =
                w0 + std::chrono::milliseconds(waitTimeoutMs);
            while (!node.link().returnReady(due) && Clock::now() < deadline)
                std::this_thread::yield();
            const double w = usSince(w0);
            st.waitTotUs += w;
            if (w > st.waitMaxUs) st.waitMaxUs = w;
            if (!node.link().returnReady(due)) {
                ++st.waitTimeouts;
                if (st.firstMissedSeq < 0) st.firstMissedSeq = (int)due;
            }
        }
        const Clock::time_point p0 = Clock::now();
        node.process(ctx);
        const double p = usSince(p0);
        st.procTotUs += p;
        if (p > st.procMaxUs) st.procMaxUs = p;
    }
}

// Fill the two input channels with a deterministic ramp: L[n] = n, R[n] =
// n + 0.5. Every value is exactly representable in float over this range, so
// the round trip can be checked BIT-exactly.
static void fillRamp(std::vector<float> inAll[2]) {
    const size_t total = (size_t)kBlocks * kBlock;
    for (int c = 0; c < 2; ++c) inAll[c].assign(total, 0.0f);
    for (size_t n = 0; n < total; ++n) {
        inAll[0][n] = (float)n;
        inAll[1][n] = (float)n + 0.5f;
    }
}

static bool blockIsSilent(const std::vector<float> outAll[2], int b) {
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < kBlock; ++i)
            if (outAll[c][(size_t)b * kBlock + i] != 0.0f) return false;
    return true;
}

static bool blocksEqual(const std::vector<float>& a, int blkA,
                        const std::vector<float>& b, int blkB) {
    return std::memcmp(&a[(size_t)blkA * kBlock], &b[(size_t)blkB * kBlock],
                       (size_t)kBlock * sizeof(float)) == 0;
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("  FAIL: %s\n", msg); ++failures; }
        else       { std::printf("  ok:   %s\n", msg); }
    };

    std::vector<float> inAll[2], outAll[2];
    fillRamp(inAll);

    //=========================================================================
    // A. Clean loopback: bit-exact identity echo through the D=2 pipeline.
    //=========================================================================
    std::printf("[remote_test] A: clean loopback, %d blocks of %d frames @ %g Hz, D=%d\n",
                kBlocks, kBlock, kSr, kDepth);
    {
        EchoResponder echo;
        check(echo.start(0), "echo responder up on 127.0.0.1 (ephemeral port)");

        RemoteNode node(kDepth, 1);
        check(node.numPorts() == 2 &&
              node.port(0).kind == PortKind::Audio && node.port(0).dir == PortDir::In  &&
              node.port(0).channels == 2 &&
              node.port(1).kind == PortKind::Audio && node.port(1).dir == PortDir::Out &&
              node.port(1).channels == 2,
              "ports: stereo audio in + stereo audio out");

        node.setEndpoint("127.0.0.1", echo.port);
        check(node.prepare(kSr, kBlock), "prepare(): link open, rings sized");
        check(node.latencySamples() == kDepth * kBlock,
              "latencySamples() == D*block == 128 (for PDC)");

        for (int c = 0; c < 2; ++c) outAll[c].assign((size_t)kBlocks * kBlock, -999.0f);
        RunStats st;
        driveBlocks(node, /*waitTimeoutMs*/ 200, inAll, outAll, st);

        // Warm-up: the first D blocks have nothing due -> must be silent.
        bool warmupSilent = true;
        for (int b = 0; b < kDepth; ++b) warmupSilent = warmupSilent && blockIsSilent(outAll, b);
        check(warmupSilent, "warm-up blocks (0..D-1) are silent");

        // The oracle: identity echo + fixed pipeline => out[n] == in[n-128],
        // bit-exact, on both channels, for every block past warm-up.
        int mismatched = 0;
        for (int b = kDepth; b < kBlocks; ++b)
            for (int c = 0; c < 2; ++c)
                if (!blocksEqual(outAll[c], b, inAll[c], b - kDepth)) { ++mismatched; break; }
        check(mismatched == 0, "BIT-EXACT: audioOut[n] == audioIn[n-128] for all n past warm-up");

        check(node.link().underruns() == 0, "underruns() == 0");
        check(node.link().sendDrops() == 0, "sendDrops() == 0");
        check(!node.link().linkDown(),      "linkDown() == false");
        check(st.waitTimeouts == 0,         "no harness wait ever timed out");

        // D. the RT invariant: sockets were used, but never on this thread.
        check(node.link().socketCalls() > 0, "net thread made socket calls");
        check(!node.link().socketCalledOnProcessThread(),
              "NO socket call on the process() thread (thread-id capture)");

        check(st.procMaxUs < 20000.0,
              "process() wall time bounded (never blocks on the network)");

        std::printf("  measured: process() avg %.1f us  max %.1f us | "
                    "return-wait avg %.1f us  max %.1f us | rx %llu pkts\n",
                    st.procTotUs / kBlocks, st.procMaxUs,
                    st.waitTotUs / (kBlocks - kDepth), st.waitMaxUs,
                    (unsigned long long)node.link().received());

        node.release();
        echo.shutdown();
    }

    //=========================================================================
    // B. Loss: drop every 50th datagram -> counted concealment, no hang.
    //=========================================================================
    std::printf("[remote_test] B: responder drops every 50th datagram\n");
    {
        EchoResponder echo;
        check(echo.start(50), "lossy echo responder up");

        RemoteNode node(kDepth, 1);
        node.setEndpoint("127.0.0.1", echo.port);
        check(node.prepare(kSr, kBlock), "prepare(): link open");

        for (int c = 0; c < 2; ++c) outAll[c].assign((size_t)kBlocks * kBlock, -999.0f);
        RunStats st;
        driveBlocks(node, /*waitTimeoutMs*/ 25, inAll, outAll, st);

        const uint64_t dropped = echo.dropped.load();
        const uint64_t under   = node.link().underruns();
        check(dropped > 0, "responder really dropped datagrams");
        check(under >= dropped, "every dropped block was counted as an underrun");
        check(under <= dropped + 5, "no runaway underruns (isolated losses only)");
        check(!node.link().linkDown(), "isolated losses never trip linkDown");
        check(st.procMaxUs < 20000.0,
              "process() wall time bounded through the losses (never blocks)");
        check(!node.link().socketCalledOnProcessThread(),
              "NO socket call on the process() thread (lossy run)");

        // Concealment shape: the first lost return seq s plays as a REPEAT of
        // the last good block, i.e. out block s+D == in block s-1. Only
        // deterministic when the loss pattern was exactly the drops, so gate
        // the check on that.
        if (under == dropped && st.firstMissedSeq > 0) {
            const int s = st.firstMissedSeq;
            bool conceal = true;
            for (int c = 0; c < 2; ++c)
                conceal = conceal && blocksEqual(outAll[c], s + kDepth, inAll[c], s - 1);
            check(conceal, "first loss concealed by repeat-last-block");
        } else {
            std::printf("  note: extra timing underruns (%llu vs %llu drops); "
                        "repeat-last shape check skipped\n",
                        (unsigned long long)under, (unsigned long long)dropped);
        }

        std::printf("  measured: drops %llu | underruns %llu | wait timeouts %d | "
                    "process() max %.1f us\n",
                    (unsigned long long)dropped, (unsigned long long)under,
                    st.waitTimeouts, st.procMaxUs);

        node.release();
        echo.shutdown();
    }

    //=========================================================================
    // C. Dead peer: silent for >500 ms -> linkDown, silence, keep running.
    //=========================================================================
    std::printf("[remote_test] C: no responder -> linkDown after %d ms\n",
                net::kLinkDownMs);
    {
        // Find a loopback port with nothing behind it: bind ephemeral, note
        // the number, close. Nothing else grabs it within this test's run.
        uint16_t deadPort = 0;
        {
            nsock::startup();
            nsock::socket_t s = nsock::openUdp();
            nsock::bindLocal(s, 0);
            deadPort = nsock::boundPort(s);
            nsock::closeSocket(s);
            nsock::cleanup();
        }
        check(deadPort != 0, "picked an unoccupied loopback port");

        RemoteNode node(kDepth, 1);
        node.setEndpoint("127.0.0.1", deadPort);
        check(node.prepare(kSr, kBlock), "prepare(): link opens even with no peer");

        float* inPtrs[2]  = { nullptr, nullptr };
        float* outPtrs[2] = { nullptr, nullptr };
        AudioBus inBus{ inPtrs, 2 };
        AudioBus outBus{ outPtrs, 2 };
        NodeProcessContext ctx{};
        ctx.nframes  = kBlock;
        ctx.audioIn  = &inBus;  ctx.numAudioIn  = 1;
        ctx.audioOut = &outBus; ctx.numAudioOut = 1;

        // ~70 blocks over ~700 ms of wall time: comfortably past the 500 ms
        // silence threshold while process() keeps getting called.
        std::vector<float> outL(kBlock), outR(kBlock);
        double procMaxUs = 0.0;
        for (int b = 0; b < 70; ++b) {
            for (int c = 0; c < 2; ++c) inPtrs[c] = &inAll[c][(size_t)b * kBlock];
            outPtrs[0] = outL.data(); outPtrs[1] = outR.data();
            const Clock::time_point p0 = Clock::now();
            node.process(ctx);
            const double p = usSince(p0);
            if (p > procMaxUs) procMaxUs = p;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(node.link().linkDown(), "linkDown() latched after >500 ms of silence");
        bool silent = true;
        for (int i = 0; i < kBlock; ++i)
            silent = silent && outL[i] == 0.0f && outR[i] == 0.0f;
        check(silent, "output is silence while the link is down");
        check(procMaxUs < 20000.0, "process() stays bounded with a dead peer");
        check(!node.link().socketCalledOnProcessThread(),
              "NO socket call on the process() thread (dead-peer run)");
        std::printf("  measured: process() max %.1f us | underruns %llu before latch\n",
                    procMaxUs, (unsigned long long)node.link().underruns());

        node.release();
    }

    if (failures == 0) std::printf("[remote_test] ALL CHECKS PASSED\n");
    else               std::printf("[remote_test] %d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
