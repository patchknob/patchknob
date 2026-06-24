//----------------------------------------------------------------------------
//  seq24 Windows port — real-time audio engine implementation (RtAudio/WASAPI).
//----------------------------------------------------------------------------
#include "audio_engine.h"

#include "RtAudio.h"

#include <cmath>

namespace seq24 { namespace engine {

namespace {
// RtAudio's WASAPI errorCallback fires on misconfiguration. We keep it quiet
// here (errors are surfaced via getErrorText() at the call site) but it must be
// a valid target. A no-op avoids RtAudio printing to stderr from any thread.
void rtErrorSink(RtAudioErrorType /*type*/, const std::string& /*msg*/) {}
} // namespace

AudioEngine::AudioEngine() {
    try {
        // Prefer the WASAPI backend (proven on this machine). If RtAudio was
        // compiled with multiple backends, UNSPECIFIED would also work, but we
        // pin WASAPI for predictable low-latency behavior.
        dac_ = std::make_unique<RtAudio>(RtAudio::WINDOWS_WASAPI, &rtErrorSink);
    } catch (const std::exception& e) {
        lastError_ = std::string("RtAudio construction failed: ") + e.what();
    } catch (...) {
        lastError_ = "RtAudio construction failed: unknown error";
    }
}

AudioEngine::~AudioEngine() {
    close();
}

std::vector<AudioDeviceInfo> AudioEngine::enumerateOutputDevices() {
    std::vector<AudioDeviceInfo> result;
    if (!dac_) { lastError_ = "audio backend not initialized"; return result; }
    try {
        const std::vector<unsigned int> ids = dac_->getDeviceIds();
        for (unsigned int id : ids) {
            RtAudio::DeviceInfo di = dac_->getDeviceInfo(id);
            if (di.outputChannels == 0)
                continue; // output engine: skip input-only devices
            AudioDeviceInfo info;
            info.id             = di.ID;
            info.name           = di.name;
            info.outputChannels = di.outputChannels;
            info.preferredRate  = di.preferredSampleRate;
            info.isDefault      = di.isDefaultOutput;
            result.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        lastError_ = std::string("enumerateOutputDevices failed: ") + e.what();
        result.clear();
    } catch (...) {
        lastError_ = "enumerateOutputDevices failed: unknown error";
        result.clear();
    }
    return result;
}

unsigned int AudioEngine::defaultOutputDeviceId() {
    if (!dac_) { lastError_ = "audio backend not initialized"; return 0; }
    try {
        return dac_->getDefaultOutputDevice();
    } catch (...) {
        lastError_ = "getDefaultOutputDevice failed";
        return 0;
    }
}

void AudioEngine::selectDevice(unsigned int deviceId) {
    selectedDeviceId_ = deviceId;
}

void AudioEngine::setRenderCallback(RenderCallback cb) {
    render_ = std::move(cb);
}

bool AudioEngine::open(unsigned int sampleRate,
                       unsigned int blockSize,
                       unsigned int numChannels) {
    if (!dac_) { lastError_ = "audio backend not initialized"; return false; }
    if (isOpen()) { lastError_ = "stream already open"; return false; }
    if (numChannels == 0) { lastError_ = "numChannels must be >= 1"; return false; }

    try {
        // Resolve the device to open: explicit selection, else default, else the
        // first output-capable device.
        unsigned int dev = selectedDeviceId_;
        if (dev == 0) {
            dev = dac_->getDefaultOutputDevice();
            if (dev == 0) {
                for (unsigned int id : dac_->getDeviceIds()) {
                    if (dac_->getDeviceInfo(id).outputChannels > 0) { dev = id; break; }
                }
            }
        }
        if (dev == 0) { lastError_ = "no output-capable device found"; return false; }

        RtAudio::StreamParameters oParams;
        oParams.deviceId     = dev;
        oParams.nChannels    = numChannels;
        oParams.firstChannel = 0;

        RtAudio::StreamOptions options;
        // Non-interleaved so RtAudio hands us planar channel blocks matching the
        // engine's float** convention; realtime scheduling for the callback.
        options.flags = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME;

        unsigned int bufferFrames = blockSize; // in/out: RtAudio may change it.

        // Reset diagnostics for this session.
        callbackCount_.store(0, std::memory_order_relaxed);
        underflowCount_.store(0, std::memory_order_relaxed);
        masterPeak_.store(0.0f, std::memory_order_relaxed);
        masterRms_.store(0.0f, std::memory_order_relaxed);

        RtAudioErrorType err = dac_->openStream(
            &oParams, /*inputParameters=*/nullptr,
            RTAUDIO_FLOAT32, sampleRate, &bufferFrames,
            &AudioEngine::rtCallback, this, &options);

        if (err != RTAUDIO_NO_ERROR) {
            lastError_ = "openStream failed: " + dac_->getErrorText();
            return false;
        }

        // Re-read granted parameters.
        selectedDeviceId_ = dev;
        numChannels_      = numChannels;
        blockSize_        = bufferFrames;
        try {
            unsigned int granted = dac_->getStreamSampleRate();
            sampleRate_ = granted ? granted : sampleRate;
        } catch (...) {
            sampleRate_ = sampleRate;
        }

        // Pre-allocate the per-channel pointer scratch off the audio thread.
        channelPtrs_.assign(numChannels_, nullptr);

        lastError_.clear();
        return true;
    } catch (const std::exception& e) {
        lastError_ = std::string("open failed: ") + e.what();
        return false;
    } catch (...) {
        lastError_ = "open failed: unknown error";
        return false;
    }
}

bool AudioEngine::start() {
    if (!dac_) { lastError_ = "audio backend not initialized"; return false; }
    if (!isOpen()) { lastError_ = "stream not open"; return false; }
    try {
        RtAudioErrorType err = dac_->startStream();
        if (err != RTAUDIO_NO_ERROR) {
            lastError_ = "startStream failed: " + dac_->getErrorText();
            return false;
        }
        lastError_.clear();
        return true;
    } catch (const std::exception& e) {
        lastError_ = std::string("start failed: ") + e.what();
        return false;
    } catch (...) {
        lastError_ = "start failed: unknown error";
        return false;
    }
}

void AudioEngine::stop() {
    if (!dac_) return;
    try {
        if (dac_->isStreamRunning())
            dac_->stopStream();
    } catch (const std::exception& e) {
        lastError_ = std::string("stop failed: ") + e.what();
    } catch (...) {
        lastError_ = "stop failed: unknown error";
    }
}

void AudioEngine::close() {
    if (!dac_) return;
    try {
        if (dac_->isStreamRunning())
            dac_->stopStream();
        if (dac_->isStreamOpen())
            dac_->closeStream();
    } catch (const std::exception& e) {
        lastError_ = std::string("close failed: ") + e.what();
    } catch (...) {
        lastError_ = "close failed: unknown error";
    }
}

bool AudioEngine::isOpen() const {
    return dac_ && dac_->isStreamOpen();
}

bool AudioEngine::isRunning() const {
    return dac_ && dac_->isStreamRunning();
}

// --- realtime audio thread --------------------------------------------------

int AudioEngine::rtCallback(void* outputBuffer, void* /*inputBuffer*/,
                            unsigned int nFrames, double /*streamTime*/,
                            unsigned int status, void* userData) {
    return static_cast<AudioEngine*>(userData)
        ->handleCallback(outputBuffer, nFrames, status);
}

int AudioEngine::handleCallback(void* outputBuffer, unsigned int nFrames,
                                unsigned int status) {
    // RTAUDIO contract: this runs on RtAudio's realtime thread. Everything below
    // is allocation-free and lock-free (only atomics + pre-sized vectors).
    const int   ch  = static_cast<int>(numChannels_);
    float*      buf = static_cast<float*>(outputBuffer);

    if (status & RTAUDIO_OUTPUT_UNDERFLOW)
        underflowCount_.fetch_add(1, std::memory_order_relaxed);
    callbackCount_.fetch_add(1, std::memory_order_relaxed);

    // Non-interleaved: channel blocks are back-to-back, nFrames each. Build the
    // planar float** view into the pre-sized scratch (no allocation).
    const int n = static_cast<int>(nFrames);
    for (int c = 0; c < ch; ++c)
        channelPtrs_[static_cast<size_t>(c)] = buf + static_cast<size_t>(c) * n;

    if (render_) {
        render_(channelPtrs_.data(), ch, n, static_cast<double>(sampleRate_));
    } else {
        // No renderer registered: output silence so we never emit garbage.
        for (int i = 0; i < ch * n; ++i) buf[i] = 0.0f;
    }

    // Master peak/RMS across all channels of this block (for a future meter).
    float    peak    = 0.0f;
    double   sumSq   = 0.0;
    const int total  = ch * n;
    for (int i = 0; i < total; ++i) {
        const float v = buf[i];
        const float a = std::fabs(v);
        if (a > peak) peak = a;
        sumSq += static_cast<double>(v) * static_cast<double>(v);
    }
    const float rms = total > 0
        ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(total)))
        : 0.0f;

    masterPeak_.store(peak, std::memory_order_relaxed);
    masterRms_.store(rms, std::memory_order_relaxed);

    return 0; // keep running
}

}} // namespace seq24::engine
