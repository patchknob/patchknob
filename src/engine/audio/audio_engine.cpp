//----------------------------------------------------------------------------
//  PatchKnob — real-time audio engine implementation (PortAudio).
//
//  PortAudio is built static (vendor/portaudio) with the Windows host APIs
//  WASAPI / WDM-KS / DirectSound / MME (ASIO too when the SDK is present); on
//  Linux it exposes ALSA / JACK / PulseAudio.  paNonInterleaved makes the
//  callback's `output` a `float**` (one buffer per channel), matching the
//  engine's planar float** convention, so we forward it straight through.
//----------------------------------------------------------------------------
#include "audio_engine.h"

#include "portaudio.h"

#include <cmath>
#include <thread>
#include <mutex>
#include <set>

namespace PatchKnob { namespace engine {

// File-static trampoline registered with PortAudio (exact PaStreamCallback sig).
static int pa_trampoline(const void* /*input*/, void* output,
                         unsigned long frameCount,
                         const PaStreamCallbackTimeInfo* /*timeInfo*/,
                         PaStreamCallbackFlags statusFlags,
                         void* userData) {
    return static_cast<AudioEngine*>(userData)
        ->render_into(output, frameCount, (unsigned long)statusFlags);
}

// Liveness registry for the stream-finished callback.  Some host APIs (WASAPI)
// can deliver the finished callback from an internal thread AFTER the stream
// was closed and the engine destroyed; dispatching into a dead engine is a
// use-after-free (observed poisoning a LATER engine's deviceLost_ through heap
// reuse).  The trampoline only dispatches while the engine is registered, and
// deregistration holds the same mutex so no dispatch can be in flight after
// close() returns.  Not on the audio render path (this is the finish path).
static std::mutex               s_finishedMx;
static std::set<AudioEngine*>   s_finishedLive;

// File-static stream-finished trampoline: PortAudio invokes it once when the
// stream goes inactive for any reason (stop, close, or device disappearance).
static void pa_finished_trampoline(void* userData) {
    AudioEngine* e = static_cast<AudioEngine*>(userData);
    std::lock_guard<std::mutex> lk(s_finishedMx);
    if (s_finishedLive.count(e)) e->stream_finished();
}

bool AudioEngine::ensureInit() {
    if (paInited_) return true;
    PaError e = Pa_Initialize();
    if (e != paNoError) { lastError_ = std::string("Pa_Initialize: ") + Pa_GetErrorText(e); return false; }
    paInited_ = true;
    return true;
}

AudioEngine::AudioEngine() { ensureInit(); }

AudioEngine::~AudioEngine() {
    close();
    if (paInited_) { Pa_Terminate(); paInited_ = false; }
    // The stream is closed, so no audio thread can still hold the callback.
    delete render_.exchange(nullptr, std::memory_order_seq_cst);
}

// --- render callback ----------------------------------------------------------
void AudioEngine::setRenderCallback(RenderCallback cb) {
    RenderHolder* fresh = cb ? new RenderHolder{std::move(cb)} : nullptr;
    RenderHolder* old   = render_.exchange(fresh, std::memory_order_seq_cst);
    // Retire the old holder only after the audio thread has provably left
    // render_into(): a callback that entered before the exchange may still be
    // running the old functor.  The audio thread does flag-then-load, we do
    // swap-then-check, both seq_cst, so observing inCallback_ == false here
    // means any still-running callback already loaded the NEW holder.
    while (inCallback_.load(std::memory_order_seq_cst))
        std::this_thread::yield();   // at most the tail of one audio block
    delete old;
}

// --- host APIs --------------------------------------------------------------
std::vector<AudioHostApiInfo> AudioEngine::enumerateHostApis() {
    std::vector<AudioHostApiInfo> out;
    if (!ensureInit()) return out;
    const PaHostApiIndex def = Pa_GetDefaultHostApi();
    const PaHostApiIndex cur = (hostApi_ >= 0) ? hostApi_ : def;
    int n = Pa_GetHostApiCount();
    for (int i = 0; i < n; ++i) {
        const PaHostApiInfo* hi = Pa_GetHostApiInfo(i);
        if (!hi) continue;
        AudioHostApiInfo a;
        a.index = i; a.name = hi->name ? hi->name : "?";
        a.deviceCount = hi->deviceCount;
        a.isCurrent = (i == cur);
        out.push_back(std::move(a));
    }
    return out;
}

void AudioEngine::selectHostApi(int paHostApiIndex) {
    hostApi_ = paHostApiIndex;
    selectedDeviceId_ = 0;    // device ids are host-API relative; reset to default
}

// --- devices ----------------------------------------------------------------
std::vector<AudioDeviceInfo> AudioEngine::enumerateOutputDevices() {
    std::vector<AudioDeviceInfo> result;
    if (!ensureInit()) return result;
    int n = Pa_GetDeviceCount();
    if (n < 0) { lastError_ = std::string("Pa_GetDeviceCount: ") + Pa_GetErrorText(n); return result; }
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di || di->maxOutputChannels == 0) continue;
        if (hostApi_ >= 0 && di->hostApi != hostApi_) continue;   // filter to selected backend
        const PaHostApiInfo* hi = Pa_GetHostApiInfo(di->hostApi);
        AudioDeviceInfo info;
        info.id             = (unsigned int)(i + 1);   // +1 so 0 stays "use default"
        info.name           = di->name ? di->name : "?";
        info.outputChannels = (unsigned int)di->maxOutputChannels;
        info.preferredRate  = (unsigned int)di->defaultSampleRate;
        info.isDefault      = (hi && hi->defaultOutputDevice == i);
        info.hostApi        = di->hostApi;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<AudioDeviceInfo> AudioEngine::enumerateInputDevices() {
    std::vector<AudioDeviceInfo> result;
    if (!ensureInit()) return result;
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (!di || di->maxInputChannels == 0) continue;
        if (hostApi_ >= 0 && di->hostApi != hostApi_) continue;
        const PaHostApiInfo* hi = Pa_GetHostApiInfo(di->hostApi);
        AudioDeviceInfo info;
        info.id             = (unsigned int)(i + 1);
        info.name           = di->name ? di->name : "?";
        info.outputChannels = (unsigned int)di->maxInputChannels;   // input count here
        info.preferredRate  = (unsigned int)di->defaultSampleRate;
        info.isDefault      = (hi && hi->defaultInputDevice == i);
        info.hostApi        = di->hostApi;
        result.push_back(std::move(info));
    }
    return result;
}

unsigned int AudioEngine::defaultOutputDeviceId() {
    if (!ensureInit()) return 0;
    PaDeviceIndex d;
    if (hostApi_ >= 0) {
        const PaHostApiInfo* hi = Pa_GetHostApiInfo(hostApi_);
        d = hi ? hi->defaultOutputDevice : Pa_GetDefaultOutputDevice();
    } else {
        d = Pa_GetDefaultOutputDevice();
    }
    return (d == paNoDevice) ? 0 : (unsigned int)(d + 1);
}

void AudioEngine::selectDevice(unsigned int deviceId) { selectedDeviceId_ = deviceId; }

// --- lifecycle --------------------------------------------------------------
bool AudioEngine::open(unsigned int sampleRate, unsigned int blockSize, unsigned int numChannels) {
    if (!ensureInit()) return false;
    if (isOpen())            { lastError_ = "stream already open"; return false; }
    if (numChannels == 0)    { lastError_ = "numChannels must be >= 1"; return false; }

    // Resolve the device: explicit selection, else host-API/global default.
    PaDeviceIndex dev;
    if (selectedDeviceId_ != 0) {
        dev = (PaDeviceIndex)(selectedDeviceId_ - 1);
    } else if (hostApi_ >= 0) {
        const PaHostApiInfo* hi = Pa_GetHostApiInfo(hostApi_);
        dev = hi ? hi->defaultOutputDevice : Pa_GetDefaultOutputDevice();
    } else {
        dev = Pa_GetDefaultOutputDevice();
    }
    if (dev == paNoDevice) { lastError_ = "no output-capable device found"; return false; }

    const PaDeviceInfo* di = Pa_GetDeviceInfo(dev);
    if (!di) { lastError_ = "invalid device"; return false; }

    PaStreamParameters op;
    op.device           = dev;
    op.channelCount     = (int)numChannels;
    op.sampleFormat     = paFloat32 | paNonInterleaved;   // -> callback gets float**
    op.suggestedLatency = di->defaultLowOutputLatency;
    op.hostApiSpecificStreamInfo = nullptr;

    callbackCount_.store(0, std::memory_order_relaxed);
    underflowCount_.store(0, std::memory_order_relaxed);
    masterPeak_.store(0.0f, std::memory_order_relaxed);
    masterRms_.store(0.0f, std::memory_order_relaxed);

    unsigned long frames = bufferFrames_ ? bufferFrames_ : blockSize;
    if (frames == 0) frames = paFramesPerBufferUnspecified;

    PaStream* s = nullptr;
    PaError e = Pa_OpenStream(&s, /*input=*/nullptr, &op,
                              (double)sampleRate, frames,
                              paClipOff, &pa_trampoline, this);
    if (e != paNoError) {
        lastError_ = std::string("Pa_OpenStream: ") + Pa_GetErrorText(e);
        return false;
    }
    // Device-loss detection: PortAudio fires this when the stream goes
    // inactive; a finish nobody asked for means the device disappeared.
    Pa_SetStreamFinishedCallback(s, &pa_finished_trampoline);
    { std::lock_guard<std::mutex> lk(s_finishedMx); s_finishedLive.insert(this); }
    deviceLost_.store(false, std::memory_order_release);
    expectFinish_.store(false, std::memory_order_release);

    stream_           = s;
    selectedDeviceId_ = (unsigned int)(dev + 1);
    numChannels_      = numChannels;

    const PaStreamInfo* si = Pa_GetStreamInfo(s);
    sampleRate_ = si ? (unsigned int)si->sampleRate : sampleRate;
    // PortAudio doesn't report the granted block size directly; report the
    // requested one (unspecified -> fall back to the request for the UI/graph).
    blockSize_  = (frames == paFramesPerBufferUnspecified) ? blockSize : (unsigned int)frames;

    lastError_.clear();
    return true;
}

bool AudioEngine::start() {
    if (!isOpen()) { lastError_ = "stream not open"; return false; }
    expectFinish_.store(false, std::memory_order_release);   // finishes are unexpected again
    PaError e = Pa_StartStream((PaStream*)stream_);
    if (e != paNoError) { lastError_ = std::string("Pa_StartStream: ") + Pa_GetErrorText(e); return false; }
    lastError_.clear();
    return true;
}

void AudioEngine::stop() {
    if (!stream_) return;
    expectFinish_.store(true, std::memory_order_release);    // this finish is ours
    if (Pa_IsStreamActive((PaStream*)stream_) == 1) Pa_StopStream((PaStream*)stream_);
}

void AudioEngine::close() {
    if (!stream_) return;
    expectFinish_.store(true, std::memory_order_release);    // this finish is ours
    if (Pa_IsStreamActive((PaStream*)stream_) == 1) Pa_StopStream((PaStream*)stream_);
    Pa_CloseStream((PaStream*)stream_);
    stream_ = nullptr;
    // Deregister AFTER the stream is gone: any late finished callback (WASAPI
    // can deliver one post-close) now finds us absent and cannot dispatch into
    // a soon-to-be-destroyed engine.
    { std::lock_guard<std::mutex> lk(s_finishedMx); s_finishedLive.erase(this); }
}

// Invoked by pa_finished_trampoline when the stream goes inactive.  Runs on a
// PortAudio-internal thread, so touch nothing but atomics here and never try
// to reopen from this context — the message thread polls deviceLost() and
// drives tryRecover() itself.
void AudioEngine::stream_finished() {
    if (!expectFinish_.load(std::memory_order_acquire))
        deviceLost_.store(true, std::memory_order_release);
}

bool AudioEngine::tryRecover() {
    // Message thread only: tear down whatever is left of the dead stream,
    // fall back to the default output device and reopen with the last
    // granted parameters.  On success the engine is running again.
    close();
    selectedDeviceId_ = 0;   // 0 == host-API/global default
    if (!open(sampleRate_, blockSize_, numChannels_)) return false;
    if (!start()) {
        close();
        deviceLost_.store(true, std::memory_order_release);   // still lost
        return false;
    }
    deviceLost_.store(false, std::memory_order_release);
    return true;
}

bool AudioEngine::isRunning() const {
    return stream_ && Pa_IsStreamActive((PaStream*)stream_) == 1;
}

// --- realtime audio thread --------------------------------------------------
int AudioEngine::render_into(void* output, unsigned long nFrames, unsigned long statusFlags) {
    // Flush denormals for this callback: decaying reverb/filter tails would
    // otherwise cost 10-100x per sample.  MXCSR is per-thread, so set it here
    // (RAII-scoped; restored on return so a shared thread is left untouched).
    ScopedNoDenormals noDenormals;

    const int    ch  = (int)numChannels_;
    const int    n   = (int)nFrames;
    float**      out = static_cast<float**>(output);   // paNonInterleaved -> float**

    if (statusFlags & paOutputUnderflow)
        underflowCount_.fetch_add(1, std::memory_order_relaxed);
    callbackCount_.fetch_add(1, std::memory_order_relaxed);

    // Flag-then-load handshake with setRenderCallback(): while inCallback_ is
    // true the message thread must not free the holder we are about to run.
    inCallback_.store(true, std::memory_order_seq_cst);
    const RenderHolder* cb = render_.load(std::memory_order_seq_cst);

    // No exception may cross the C pa_trampoline boundary (UB through the C
    // ABI, typically std::terminate): on a throw, degrade this block to
    // silence and keep the stream alive.
    try {
        if (cb && cb->fn) {
            cb->fn(out, ch, n, (double)sampleRate_);
        } else {
            for (int c = 0; c < ch; ++c)
                for (int i = 0; i < n; ++i) out[c][i] = 0.0f;
        }

        // Master peak / RMS across all channels of this block.
        float  peak = 0.0f; double sumSq = 0.0;
        for (int c = 0; c < ch; ++c) {
            const float* b = out[c];
            for (int i = 0; i < n; ++i) {
                const float v = b[i]; const float a = std::fabs(v);
                if (a > peak) peak = a;
                sumSq += (double)v * (double)v;
            }
        }
        const int total = ch * n;
        const float rms = total > 0 ? (float)std::sqrt(sumSq / (double)total) : 0.0f;
        masterPeak_.store(peak, std::memory_order_relaxed);
        masterRms_.store(rms, std::memory_order_relaxed);
    } catch (...) {
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < n; ++i) out[c][i] = 0.0f;
        masterPeak_.store(0.0f, std::memory_order_relaxed);
        masterRms_.store(0.0f, std::memory_order_relaxed);
    }

    inCallback_.store(false, std::memory_order_release);
    return paContinue;
}

}} // namespace PatchKnob::engine
