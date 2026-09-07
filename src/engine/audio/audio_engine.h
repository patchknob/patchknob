//----------------------------------------------------------------------------
//  PatchKnob — real-time audio engine (PortAudio backend).
//
//  PatchKnob::engine::AudioEngine wraps the vendored PortAudio (built static) and
//  presents the rest of the engine with:
//    * host-API (backend) enumeration / selection  -- ASIO / WASAPI / WDM-KS /
//      DirectSound / MME on Windows; ALSA / JACK / PulseAudio on Linux
//      (PipeWire is reached through its ALSA/Pulse/JACK compatibility layers),
//    * output device enumeration / selection,
//    * a selectable buffer size (frames/block, i.e. latency),
//    * an output stream of NON-INTERLEAVED float buffers (see plugin_api.h),
//    * a single user render callback invoked from PortAudio's audio thread,
//    * lock-free master peak/RMS meters + plain-string error reporting,
//    * an exception fence: a throwing render callback degrades the block to
//      silence instead of unwinding into PortAudio's C callback frame.
//
//  Buffer convention (matches plugin_api.h): the render callback receives
//  `float** out`, an array of `numChannels` per-channel pointers each pointing
//  at `nframes` contiguous floats (planar / non-interleaved).  The stream is
//  opened with paNonInterleaved so PortAudio hands the callback exactly a
//  `float**`, which we forward straight through.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUDIO_AUDIO_ENGINE_H
#define PATCHKNOB_ENGINE_AUDIO_AUDIO_ENGINE_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>   // MXCSR intrinsics (FTZ/DAZ) for ScopedNoDenormals
#endif

namespace PatchKnob { namespace engine {

//----------------------------------------------------------------------------
//  ScopedNoDenormals
//
//  RAII guard that flushes denormals for the current thread: sets the MXCSR
//  FTZ (bit 15) and DAZ (bit 6) modes on construction and restores the saved
//  MXCSR on destruction.  Denormal operands cost 10-100x per sample on x86,
//  so every realtime render path (audio callback, DSP worker threads) should
//  hold one of these at the top of its block.  MXCSR is per-thread state, so
//  each thread must set it for itself.  No-op on non-SSE targets.
//----------------------------------------------------------------------------
class ScopedNoDenormals {
public:
    ScopedNoDenormals() {
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
        mxcsr_ = _mm_getcsr();
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    }
    ~ScopedNoDenormals() {
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
        _mm_setcsr(mxcsr_);
#endif
    }

    ScopedNoDenormals(const ScopedNoDenormals&)            = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
    unsigned int mxcsr_ = 0;
#endif
};

//! Description of one output-capable audio device.
struct AudioDeviceInfo {
    unsigned int id             = 0;    //!< (PaDeviceIndex + 1); 0 == "use default".
    std::string  name;                  //!< Human-readable device name.
    unsigned int outputChannels = 0;    //!< Max output channels.
    unsigned int preferredRate  = 0;    //!< Device preferred sample rate (Hz).
    bool         isDefault      = false;//!< True if this is the host-API default output.
    int          hostApi        = -1;   //!< Owning PaHostApiIndex.
};

//! Description of one host API (audio backend / driver model).
struct AudioHostApiInfo {
    int          index       = -1;      //!< PaHostApiIndex.
    std::string  name;                  //!< "Windows WASAPI", "Windows WDM-KS", ...
    int          deviceCount = 0;
    bool         isCurrent   = false;   //!< Currently selected for the engine.
};

//----------------------------------------------------------------------------
//  AudioEngine
//
//  Lifecycle:  construct -> [selectHostApi] -> [selectDevice] -> [setBufferSize]
//              -> setRenderCallback(...) -> open() -> start() ... stop() -> close()
//----------------------------------------------------------------------------
class AudioEngine {
public:
    //! `in` is the device CAPTURE buffer (paNonInterleaved, so one pointer per
    //! channel) or nullptr when the stream has no input.  It was absent
    //! entirely: the stream was opened output-only, which left every audio-input
    //! path in the app -- the patcher's Audio In node, input recording -- wired
    //! to nothing.
    using RenderCallback =
        std::function<void(const float* const* in, int numInputChannels,
                           float** out, int numChannels, int nframes,
                           double sampleRate)>;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // --- host API (backend) enumeration / selection (message thread) ---------
    std::vector<AudioHostApiInfo> enumerateHostApis();
    //! Select the backend to open on.  -1 == PortAudio's default host API.
    void selectHostApi(int paHostApiIndex);
    int  selectedHostApi() const { return hostApi_; }

    // --- device enumeration / selection (message thread) ---------------------
    //! Enumerate output-capable devices (of the selected host API, or all when
    //! none is selected).  Returns empty on error (check lastError()).
    std::vector<AudioDeviceInfo> enumerateOutputDevices();
    //! Enumerate input-capable devices (for a future capture/record path).  The
    //! `outputChannels` field carries the device's input-channel count here.
    std::vector<AudioDeviceInfo> enumerateInputDevices();
    unsigned int defaultOutputDeviceId();
    //! Select the device by id (PaDeviceIndex+1).  0 == use the host-API default.
    void selectDevice(unsigned int deviceId);
    void selectInputDevice(unsigned int deviceId) { selectedInputDeviceId_ = deviceId; }
    //! How many capture channels to request when opening.  0 disables input.
    //! Opening is best-effort: if the device cannot do duplex the stream still
    //! opens output-only rather than failing, so input support can never cost
    //! you playback.
    void setInputChannels(unsigned int n) { wantInputChannels_ = n; }
    unsigned int inputChannels() const { return numInputChannels_; }
    unsigned int selectedDeviceId() const { return selectedDeviceId_; }
    unsigned int selectedInputDeviceId() const { return selectedInputDeviceId_; }

    // --- buffer size (latency) -----------------------------------------------
    //! Requested frames/block for the next open().  0 == let PortAudio choose.
    void setBufferSize(unsigned int frames) { bufferFrames_ = frames; }
    unsigned int bufferSize() const { return blockSize_; }

    // --- stream lifecycle (message thread) -----------------------------------
    bool open(unsigned int sampleRate = 48000,
              unsigned int blockSize  = 512,
              unsigned int numChannels = 2);
    bool start();
    void stop();
    void close();
    bool isOpen()    const { return stream_ != nullptr; }
    bool isRunning() const;

    // --- render callback -----------------------------------------------------
    //! REALTIME CONTRACT: the callback runs on PortAudio's audio thread. It
    //! must be allocation-free, lock-free and I/O-free.  Safe to call while
    //! the stream is running: the new callback is published atomically and the
    //! old one is retired only after the audio thread has left render_into()
    //! (blocks the caller for at most the tail of one audio block).  Must NOT
    //! be called from inside the render callback itself.
    void setRenderCallback(RenderCallback cb);

    // --- granted stream parameters (valid after open()) ----------------------
    unsigned int sampleRate()  const { return sampleRate_; }
    unsigned int blockSize()   const { return blockSize_; }
    unsigned int numChannels() const { return numChannels_; }

    // --- master level meters (audio thread -> any thread, lock-free) ---------
    float masterPeak() const { return masterPeak_.load(std::memory_order_relaxed); }
    float masterRms()  const { return masterRms_.load(std::memory_order_relaxed); }

    // --- device loss / recovery (message thread) -----------------------------
    //! True once the stream finished without a stop()/close() being requested
    //! (device unplugged, driver error).  Poll from the message thread; cleared
    //! by a successful tryRecover() or the next open().
    bool deviceLost() const { return deviceLost_.load(std::memory_order_acquire); }
    //! Recovery hook for the app layer: tears down the dead stream, falls back
    //! to the current default output device and reopens + restarts with the
    //! last granted parameters.  Message thread only; never call from the
    //! audio thread or the stream-finished callback.  Returns false (with
    //! lastError() set) if the reopen failed; deviceLost() then stays true.
    bool tryRecover();

    // --- diagnostics ---------------------------------------------------------
    unsigned long long callbackCount() const { return callbackCount_.load(std::memory_order_relaxed); }
    unsigned long long underflowCount() const { return underflowCount_.load(std::memory_order_relaxed); }
    const std::string& lastError() const { return lastError_; }

    // Called by the file-static PortAudio trampolines; not for external use.
    int  render_into(const void* input, void* output, unsigned long nFrames, unsigned long statusFlags);
    void stream_finished();
#ifdef __ANDROID__
    void* androidStream() const;
#endif

private:
    bool ensureInit();

    void*        stream_        = nullptr;   // PaStream*
    bool         paInited_      = false;

    // The render callback lives behind an atomically-swapped heap holder so
    // setRenderCallback() on the message thread can never race the audio
    // thread into a torn / half-constructed std::function.  The audio thread
    // does flag-then-load (inCallback_ then render_), the message thread does
    // swap-then-wait, both seq_cst, so the old holder is freed only once no
    // callback can still be running it.  The RT path stays lock-free.
    struct RenderHolder { RenderCallback fn; };
    std::atomic<RenderHolder*> render_{nullptr};
    std::atomic<bool>          inCallback_{false};   // audio thread inside render_into()
    std::atomic<bool>          deviceLost_{false};   // stream finished uninvited
    std::atomic<bool>          expectFinish_{false}; // stop()/close() in progress

    int          hostApi_       = -1;        // PaHostApiIndex, -1 == default
    unsigned int selectedDeviceId_ = 0;      // PaDeviceIndex+1, 0 == default
    unsigned int selectedInputDeviceId_ = 0; // PaDeviceIndex+1, 0 == default
    unsigned int sampleRate_    = 48000;
    unsigned int blockSize_     = 512;       // granted frames/block
    unsigned int numChannels_   = 2;
    unsigned int wantInputChannels_ = 2;   // requested
    unsigned int numInputChannels_  = 0;   // actually opened
    unsigned int bufferFrames_  = 512;       // requested frames/block (0 = auto)

    std::atomic<float> masterPeak_{0.0f};
    std::atomic<float> masterRms_{0.0f};
    std::atomic<unsigned long long> callbackCount_{0};
    std::atomic<unsigned long long> underflowCount_{0};

    std::string lastError_;
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIO_AUDIO_ENGINE_H
