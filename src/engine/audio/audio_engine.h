//----------------------------------------------------------------------------
//  seq24 Windows port — real-time audio engine.
//
//  seq24::engine::AudioEngine wraps the vendored RtAudio (WASAPI backend) and
//  presents the rest of the engine with:
//    * output device enumeration / selection,
//    * an output stream of NON-INTERLEAVED float buffers (see plugin_api.h),
//    * a single user render callback invoked from the RtAudio audio thread,
//    * lock-free master peak/RMS level meters,
//    * try/catch error wrapping with a last-error string.
//
//  Buffer convention (matches plugin_api.h): the render callback receives
//  `float** out`, an array of `numChannels` per-channel pointers, each pointing
//  at `nframes` contiguous floats (planar / non-interleaved). The engine opens
//  the RtAudio stream with RTAUDIO_NONINTERLEAVED so RtAudio hands us exactly
//  this layout (channel blocks back-to-back) and we expose per-channel pointers.
//----------------------------------------------------------------------------
#ifndef SEQ24_ENGINE_AUDIO_AUDIO_ENGINE_H
#define SEQ24_ENGINE_AUDIO_AUDIO_ENGINE_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declare RtAudio so this header has no hard dependency on RtAudio.h.
// (The .cpp owns the concrete RtAudio instance via a pimpl-style unique_ptr.)
// RtAudio lives in namespace rt::audio; the .cpp uses the type via this alias,
// which stays valid whether or not RtAudio.h's `using namespace rt::audio;` is
// in effect at the point of use.
namespace rt { namespace audio { class RtAudio; } }

namespace seq24 { namespace engine {

//! Description of one output-capable audio device.
struct AudioDeviceInfo {
    unsigned int id            = 0;     //!< RtAudio device id (NOT a 0-based index).
    std::string  name;                  //!< Human-readable device name.
    unsigned int outputChannels = 0;    //!< Max output channels.
    unsigned int preferredRate  = 0;    //!< Device preferred sample rate (Hz).
    bool         isDefault      = false;//!< True if this is the system default output.
};

//----------------------------------------------------------------------------
//  AudioEngine
//
//  Lifecycle:  construct -> enumerateOutputDevices() -> selectDevice(...) ->
//              setRenderCallback(...) -> open() -> start() ... stop() -> close()
//
//  The render callback is the ONE place the track/graph module produces audio.
//  It runs on RtAudio's dedicated realtime audio thread. See the realtime
//  contract documented on setRenderCallback() below.
//----------------------------------------------------------------------------
class AudioEngine {
public:
    //! Signature of the user render callback (called on the audio thread).
    //!   out          : array of `numChannels` planar float buffers.
    //!   numChannels  : channel count of the open stream.
    //!   nframes      : frames to fill in this block (== granted block size).
    //!   sampleRate   : granted stream sample rate (Hz).
    //! The callback MUST fully write all numChannels*nframes samples (write
    //! silence if it has nothing to play). Buffers are NOT pre-zeroed.
    using RenderCallback =
        std::function<void(float** out, int numChannels, int nframes, double sampleRate)>;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // --- device enumeration / selection (message thread) ---------------------

    //! Enumerate all output-capable devices. Returns empty on error
    //! (check lastError()).
    std::vector<AudioDeviceInfo> enumerateOutputDevices();

    //! RtAudio id of the system default output (0 if none).
    unsigned int defaultOutputDeviceId();

    //! Select the device to open by id. If never called (or id==0), open() uses
    //! the default output, falling back to the first output-capable device.
    void selectDevice(unsigned int deviceId);

    //! Currently selected device id (0 == "use default").
    unsigned int selectedDeviceId() const { return selectedDeviceId_; }

    // --- stream lifecycle (message thread) -----------------------------------

    //! Open the output stream. `sampleRate`/`blockSize` are requests; the
    //! granted values are re-read and exposed via sampleRate()/blockSize().
    //! Returns false on error (check lastError()). Must register the render
    //! callback before audio will be meaningful.
    bool open(unsigned int sampleRate = 48000,
              unsigned int blockSize  = 512,
              unsigned int numChannels = 2);

    //! Start the audio thread / callback flow. Returns false on error.
    bool start();

    //! Stop the audio thread (drains). Safe to call when not running.
    void stop();

    //! Close the stream (stops first if needed). Safe to call when not open.
    void close();

    bool isOpen()    const;
    bool isRunning() const;

    // --- render callback -----------------------------------------------------

    //! Register the render callback. Set this BEFORE start(). Replacing it while
    //! the stream is running is NOT realtime-safe; stop the stream first.
    //!
    //! REALTIME CONTRACT — the callback runs on RtAudio's dedicated audio
    //! thread (requested with realtime scheduling). It MUST be:
    //!   * allocation-free   : no new/delete/malloc, no container growth.
    //!   * lock-free         : no std::mutex, no blocking syscalls.
    //!   * I/O-free          : no file/console/network I/O.
    //! Communicate with other threads only via std::atomic / SPSC ring buffers /
    //! RCU-style pointer swaps. Pre-allocate every buffer before start().
    void setRenderCallback(RenderCallback cb);

    // --- granted stream parameters (valid after open()) ----------------------

    unsigned int sampleRate()  const { return sampleRate_; }
    unsigned int blockSize()   const { return blockSize_; }   //!< Granted frames/block.
    unsigned int numChannels() const { return numChannels_; }

    // --- master level meters (audio thread -> any thread, lock-free) ---------

    //! Peak absolute sample of the most recent block, across all channels (0..~1).
    float masterPeak() const { return masterPeak_.load(std::memory_order_relaxed); }

    //! RMS level of the most recent block, across all channels (0..~1).
    float masterRms() const { return masterRms_.load(std::memory_order_relaxed); }

    // --- diagnostics ---------------------------------------------------------

    //! Number of audio callbacks invoked since open() (lock-free).
    unsigned long long callbackCount() const {
        return callbackCount_.load(std::memory_order_relaxed);
    }

    //! Number of output underflows (xruns) observed since open() (lock-free).
    unsigned long long underflowCount() const {
        return underflowCount_.load(std::memory_order_relaxed);
    }

    //! Last error text (empty if none). Set by any failing operation.
    const std::string& lastError() const { return lastError_; }

private:
    // C-style trampoline registered with RtAudio; forwards to the member impl.
    static int rtCallback(void* outputBuffer, void* inputBuffer,
                          unsigned int nFrames, double streamTime,
                          unsigned int status, void* userData);
    int handleCallback(void* outputBuffer, unsigned int nFrames, unsigned int status);

    std::unique_ptr<rt::audio::RtAudio> dac_;

    RenderCallback render_;

    unsigned int selectedDeviceId_ = 0;   // 0 == use default
    unsigned int sampleRate_       = 48000;
    unsigned int blockSize_        = 512;
    unsigned int numChannels_      = 2;

    // Per-channel pointer scratch handed to the user callback (planar view of
    // RtAudio's non-interleaved output buffer). Sized at open(); never resized
    // on the audio thread.
    std::vector<float*> channelPtrs_;

    std::atomic<float> masterPeak_{0.0f};
    std::atomic<float> masterRms_{0.0f};
    std::atomic<unsigned long long> callbackCount_{0};
    std::atomic<unsigned long long> underflowCount_{0};

    std::string lastError_;
};

}} // namespace seq24::engine

#endif // SEQ24_ENGINE_AUDIO_AUDIO_ENGINE_H
