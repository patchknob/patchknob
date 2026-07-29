//----------------------------------------------------------------------------
//  audio_test — headless + smoke tests for PatchKnob::engine::AudioEngine.
//
//  Headless tests (no device needed; drive render_into() directly):
//    1. A throwing render callback is caught at the C boundary: render_into
//       returns paContinue and every output sample is zeroed.
//    2. FTZ/DAZ (MXCSR bits 15 and 6) are set inside the render scope and
//       restored afterwards (ScopedNoDenormals).
//    3. setRenderCallback() hammered from the message thread while a second
//       thread loops render_into(): no crash, no torn functor (every observed
//       callback sees its own intact heap capture), engine always callable.
//    4. stream_finished() without a stop()/close() in flight sets deviceLost().
//
//  Device smoke test (skipped gracefully when no output device opens):
//    440 Hz sine for ~1.5 s, then checks a normal stop()/close() does NOT
//    trip deviceLost(), and that tryRecover() brings a "lost" stream back.
//
//  Exit code 0 == all executed tests passed, 1 == failure.
//----------------------------------------------------------------------------
#include "audio_engine.h"

#include "portaudio.h"   // paContinue, for asserting the callback return code

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

using PatchKnob::engine::AudioEngine;
using PatchKnob::engine::AudioDeviceInfo;

namespace {

int g_failures = 0;

// Minimal check helper: prints PASS/FAIL per condition and tallies failures.
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// A fake non-interleaved output block: `chans` per-channel planar buffers.
struct FakeBlock {
    explicit FakeBlock(int chans, int frames, float fill)
        : storage(chans, std::vector<float>((size_t)frames, fill)) {
        for (auto& v : storage) ptrs.push_back(v.data());
    }
    float** data() { return ptrs.data(); }
    std::vector<std::vector<float>> storage;
    std::vector<float*>             ptrs;
};

constexpr double kFreqHz    = 440.0;
constexpr double kAmplitude = 0.2;
constexpr double kTwoPi     = 6.283185307179586476925286766559;

// Phase state for the sine. Held outside the lambda so it persists across
// callbacks; only touched on the audio thread, so no synchronization needed.
struct SineState {
    double phase    = 0.0;
    double phaseInc = 0.0; // set once sample rate is known (radians/frame)
};

//----------------------------------------------------------------------------
// 1. Throwing render callback: no exception may escape render_into(); the
//    block degrades to silence and the stream keeps running (paContinue).
//----------------------------------------------------------------------------
void test_throwing_callback() {
    std::printf("--- test: throwing render callback ---\n");
    AudioEngine engine;

    engine.setRenderCallback(
        [](float** out, int numChannels, int nframes, double) {
            // Dirty the buffers first so the catch path provably zeroes them.
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < nframes; ++i) out[c][i] = 1.0f;
            throw std::runtime_error("render blew up");
        });

    const int frames = 256;
    FakeBlock blk(2, frames, /*fill=*/123.0f);   // sentinel: must not survive
    const int rc = engine.render_into(blk.data(), (unsigned long)frames, 0);

    check(rc == paContinue, "render_into returned paContinue after a throw");

    bool allZero = true;
    for (const auto& chan : blk.storage)
        for (float v : chan)
            if (v != 0.0f) { allZero = false; break; }
    check(allZero, "all output samples zero-filled after the throw");

    // The engine must remain callable: a well-behaved callback still renders.
    engine.setRenderCallback(
        [](float** out, int numChannels, int nframes, double) {
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < nframes; ++i) out[c][i] = 0.5f;
        });
    const int rc2 = engine.render_into(blk.data(), (unsigned long)frames, 0);
    check(rc2 == paContinue && blk.storage[0][0] == 0.5f && blk.storage[1][frames - 1] == 0.5f,
          "engine still renders normally on the next block");
}

//----------------------------------------------------------------------------
// 2. FTZ/DAZ are set inside the render scope (MXCSR bit 15 = FTZ, bit 6 =
//    DAZ) and restored on exit, per ScopedNoDenormals.
//----------------------------------------------------------------------------
void test_denormal_modes() {
    std::printf("--- test: FTZ/DAZ inside render scope ---\n");
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
    AudioEngine engine;

    // Start from a known-clean MXCSR (FTZ/DAZ off) on this thread.
    const unsigned int saved = _mm_getcsr();
    _mm_setcsr(saved & ~0x8040u);

    std::atomic<bool> ftzOn{false}, dazOn{false};
    engine.setRenderCallback(
        [&ftzOn, &dazOn](float**, int, int, double) {
            const unsigned int csr = _mm_getcsr();
            ftzOn.store((csr & 0x8000u) != 0);   // bit 15: flush-to-zero
            dazOn.store((csr & 0x0040u) != 0);   // bit  6: denormals-are-zero
        });

    FakeBlock blk(2, 64, 0.0f);
    engine.render_into(blk.data(), 64, 0);

    check(ftzOn.load(), "MXCSR FTZ (bit 15) set inside the render callback");
    check(dazOn.load(), "MXCSR DAZ (bit 6) set inside the render callback");
    check((_mm_getcsr() & 0x8040u) == 0, "MXCSR restored after render_into returns");

    _mm_setcsr(saved);
#else
    std::printf("  [SKIP] non-SSE target\n");
#endif
}

//----------------------------------------------------------------------------
// 3. Swap stress: one thread loops render_into() as a fake audio thread while
//    the main thread hammers setRenderCallback().  Each callback carries a
//    heap-allocated magic capture; if the old holder were freed or torn while
//    still running, the magic check (or the process) would blow up.  No TSan
//    on MinGW, so this is a crash/consistency stress, not a race proof.
//----------------------------------------------------------------------------
void test_swap_stress() {
    std::printf("--- test: concurrent setRenderCallback stress ---\n");
    AudioEngine engine;

    std::atomic<bool>               stop{false};
    std::atomic<bool>               torn{false};
    std::atomic<unsigned long long> renders{0};

    std::thread rt([&] {
        FakeBlock blk(2, 128, 0.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            engine.render_into(blk.data(), 128, 0);
            renders.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const int kSwaps = 50000;
    for (int k = 0; k < kSwaps; ++k) {
        const unsigned expect = 0xC0FFEE00u + (unsigned)(k & 0xFF);
        auto magic = std::make_shared<unsigned>(expect);
        engine.setRenderCallback(
            [magic, expect, &torn](float** out, int numChannels, int nframes, double) {
                if (*magic != expect) torn.store(true, std::memory_order_relaxed);
                for (int c = 0; c < numChannels; ++c)
                    for (int i = 0; i < nframes; ++i) out[c][i] = 0.25f;
            });
    }

    stop.store(true, std::memory_order_relaxed);
    rt.join();

    check(!torn.load(), "no callback ever observed a torn/freed capture");
    check(renders.load() > 0, "fake audio thread rendered concurrently");
    std::printf("  (%d swaps interleaved with %llu renders)\n", kSwaps, renders.load());

    // Always-callable: the last installed callback must still run intact.
    FakeBlock blk(2, 64, 0.0f);
    engine.render_into(blk.data(), 64, 0);
    check(blk.storage[0][0] == 0.25f && blk.storage[1][63] == 0.25f,
          "engine callable with the last swapped-in callback");
}

//----------------------------------------------------------------------------
// 4. Device-loss flag: a stream-finished notification that nobody requested
//    (no stop()/close() in flight) must raise deviceLost().
//----------------------------------------------------------------------------
void test_device_lost_flag() {
    std::printf("--- test: unexpected stream finish sets deviceLost ---\n");
    AudioEngine engine;

    check(!engine.deviceLost(), "deviceLost() false on a fresh engine");
    engine.stream_finished();   // simulate PortAudio's finished callback firing
    check(engine.deviceLost(), "deviceLost() true after an unrequested finish");
}

//----------------------------------------------------------------------------
// Device smoke test: real stream, 440 Hz sine, then stop/close + recovery.
// Skipped (not failed) when no output device can be opened.
//----------------------------------------------------------------------------
int smoke_test_device() {
    std::printf("--- smoke test: real device (440 Hz sine) ---\n");
    AudioEngine engine;

    std::vector<AudioDeviceInfo> devices = engine.enumerateOutputDevices();
    if (devices.empty()) {
        std::printf("  [SKIP] no output devices\n");
        return 0;
    }
    std::printf("  %zu output device(s), default id = %u\n",
                devices.size(), engine.defaultOutputDeviceId());

    SineState sine;
    engine.setRenderCallback(
        [&sine](float** out, int numChannels, int nframes, double sampleRate) {
            if (sine.phaseInc == 0.0)
                sine.phaseInc = kTwoPi * kFreqHz / sampleRate;
            for (int i = 0; i < nframes; ++i) {
                const float s = static_cast<float>(kAmplitude * std::sin(sine.phase));
                sine.phase += sine.phaseInc;
                if (sine.phase >= kTwoPi) sine.phase -= kTwoPi;
                for (int c = 0; c < numChannels; ++c)
                    out[c][i] = s; // non-interleaved: per-channel planar buffer
            }
        });

    if (!engine.open(/*sampleRate=*/48000, /*blockSize=*/512, /*numChannels=*/2)) {
        std::printf("  [SKIP] open failed: %s\n", engine.lastError().c_str());
        return 0;
    }
    std::printf("  opened: %u Hz, %u frames, %u ch\n",
                engine.sampleRate(), engine.blockSize(), engine.numChannels());

    if (!engine.start()) {
        std::printf("  [SKIP] start failed: %s\n", engine.lastError().c_str());
        engine.close();
        return 0;
    }

    std::printf("  playing 440 Hz sine for ~1.5 seconds...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Real devices on this box occasionally drop the stream mid-run (endpoint
    // sleep / renegotiation, ~1 in 3).  That is EXACTLY what the device-loss
    // detection is for, so branch: if the stream survived, assert no false
    // positive from our own stop()/close(); if the device dropped it early,
    // assert the loss WAS detected.
    const bool survived = engine.isRunning();
    engine.stop();
    // Give PortAudio's finished callback a moment to run before checking.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (survived) {
        check(!engine.deviceLost(), "requested stop() did not trip deviceLost()");
        engine.close();
        check(!engine.deviceLost(), "requested close() did not trip deviceLost()");
    } else {
        std::printf("  [INFO] device dropped the stream mid-run (real loss)\n");
        check(engine.deviceLost(), "early device drop was detected as loss");
        engine.close();
        check(engine.deviceLost(), "loss flag survives close() until recovery");
    }

    const unsigned long long callbacks = engine.callbackCount();
    const unsigned long long underflow = engine.underflowCount();
    check(callbacks > 0, "audio callbacks were invoked");
    std::printf("  callbacks=%llu underflows=%llu peak=%.4f rms=%.4f\n",
                callbacks, underflow, engine.masterPeak(), engine.masterRms());
    if (underflow != 0) std::printf("  [WARNING] underflows seen\n");

    // Recovery path: reopen/start, fake a device loss, then tryRecover().
    if (engine.open(48000, 512, 2) && engine.start()) {
        engine.stream_finished();   // pretend the device vanished mid-run
        check(engine.deviceLost(), "simulated device loss raised deviceLost()");
        check(engine.tryRecover(), "tryRecover() reopened on the default device");
        check(!engine.deviceLost(), "deviceLost() cleared after recovery");
        check(engine.isRunning(), "stream running again after recovery");
        engine.stop();
        engine.close();
    } else {
        std::printf("  [SKIP] reopen for recovery test failed: %s\n",
                    engine.lastError().c_str());
    }
    return 0;
}

} // namespace

int main() {
    test_throwing_callback();
    test_denormal_modes();
    test_swap_stress();
    test_device_lost_flag();
    smoke_test_device();

    std::printf("\n=== %s (%d failure%s) ===\n",
                g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
