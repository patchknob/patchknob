//----------------------------------------------------------------------------
//  audio_test — smoke test for seq24::engine::AudioEngine.
//
//  Opens the engine, registers a render callback that synthesizes a 440 Hz sine
//  (non-interleaved float, the engine's native buffer layout), runs ~1.5 s, then
//  prints the device list, callback count and a 0-underflow confirmation, and
//  stops cleanly. Exit code 0 == clean (no underflows), 2 == underflows seen.
//----------------------------------------------------------------------------
#include "audio_engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using seq24::engine::AudioEngine;
using seq24::engine::AudioDeviceInfo;

namespace {
constexpr double kFreqHz    = 440.0;
constexpr double kAmplitude = 0.2;
constexpr double kTwoPi     = 6.283185307179586476925286766559;

// Phase state for the sine. Held outside the lambda so it persists across
// callbacks; only touched on the audio thread, so no synchronization needed.
struct SineState {
    double phase    = 0.0;
    double phaseInc = 0.0; // set once sample rate is known (radians/frame)
};
} // namespace

int main() {
    AudioEngine engine;

    // --- enumerate ---------------------------------------------------------
    std::vector<AudioDeviceInfo> devices = engine.enumerateOutputDevices();
    if (!engine.lastError().empty()) {
        std::fprintf(stderr, "enumerate error: %s\n", engine.lastError().c_str());
    }
    const unsigned int defId = engine.defaultOutputDeviceId();

    std::printf("=== Output devices (%zu) ===\n", devices.size());
    std::printf("default output id = %u\n\n", defId);
    for (const AudioDeviceInfo& d : devices) {
        std::printf("Device id %u: \"%s\"%s\n", d.id, d.name.c_str(),
                    d.isDefault ? "  [DEFAULT OUTPUT]" : "");
        std::printf("    output channels : %u\n", d.outputChannels);
        std::printf("    preferred rate  : %u Hz\n\n", d.preferredRate);
    }

    if (devices.empty()) {
        std::fprintf(stderr, "No output devices; aborting.\n");
        return 1;
    }

    // --- render callback: 440 Hz sine to all channels ----------------------
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

    // --- open / start ------------------------------------------------------
    if (!engine.open(/*sampleRate=*/48000, /*blockSize=*/512, /*numChannels=*/2)) {
        std::fprintf(stderr, "open failed: %s\n", engine.lastError().c_str());
        return 1;
    }

    std::printf("Opened stream:\n");
    std::printf("    sample rate  : %u Hz\n", engine.sampleRate());
    std::printf("    block size   : %u frames (granted)\n", engine.blockSize());
    std::printf("    channels     : %u\n", engine.numChannels());

    if (!engine.start()) {
        std::fprintf(stderr, "start failed: %s\n", engine.lastError().c_str());
        engine.close();
        return 1;
    }

    std::printf("\nPlaying 440 Hz sine for ~1.5 seconds...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    engine.stop();
    engine.close();

    // --- results -----------------------------------------------------------
    const unsigned long long callbacks = engine.callbackCount();
    const unsigned long long underflow = engine.underflowCount();
    const unsigned long long frames    =
        callbacks * static_cast<unsigned long long>(engine.blockSize());

    std::printf("\n=== Results ===\n");
    std::printf("callbacks invoked : %llu\n", callbacks);
    std::printf("approx frames     : %llu (~%.2f s)\n", frames,
                static_cast<double>(frames) / static_cast<double>(engine.sampleRate()));
    std::printf("underflows (xrun) : %llu  %s\n", underflow,
                underflow == 0 ? "[OK: zero underflows]" : "[WARNING: underflows!]");
    std::printf("master peak       : %.4f\n", engine.masterPeak());
    std::printf("master rms        : %.4f\n", engine.masterRms());
    std::printf("Stream closed cleanly.\n");

    return underflow == 0 ? 0 : 2;
}
