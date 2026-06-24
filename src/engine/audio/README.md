# seq24 Audio Engine (`seq24::engine::AudioEngine`)

Real-time audio output for the seq24 Windows port. Hand-rolled, zero-JUCE.
Wraps the vendored [RtAudio](../../../vendor/rtaudio) (WASAPI backend) and gives
the rest of the engine a small, stable surface: device enumeration, an output
stream of **non-interleaved float** buffers, a single user render callback run on
the audio thread, lock-free master level meters, and try/catch error handling.

## Files

| File | Purpose |
|------|---------|
| `audio_engine.h` / `.cpp` | The `AudioEngine` class. |
| `audio_test.cpp` | Smoke test: 440 Hz sine, ~1.5 s, prints diagnostics. |
| `CMakeLists.txt` | Builds static lib `seq24_audio` + exe `audio_test`. |

## Build & run

MSYS2 mingw64 toolchain (g++ 15.2.0):

```sh
export PATH="/c/msys64/mingw64/bin:$PATH"
cd src/engine/audio
cmake -G "MinGW Makefiles" -S . -B build
cmake --build build
./build/audio_test.exe
```

The CMake finds RtAudio at `../../../vendor/rtaudio` (override with
`-DRTAUDIO_DIR=...`), compiles `RtAudio.cpp` with `-D__WINDOWS_WASAPI__`, and
links `ole32 winmm ksuser mfplat mfuuid wmcodecdspuuid`. Link your module
against the `seq24_audio` target; its `PUBLIC` include dirs expose
`audio_engine.h`, `RtAudio.h`, and the shared `plugin_api.h`.

## Buffer convention

The engine uses the project-wide layout from
[`plugin_api.h`](../plugin_api.h): **non-interleaved (planar) float**. The render
callback receives `float** out`, an array of `numChannels` per-channel pointers,
each pointing at `nframes` contiguous floats. The stream is opened with
`RTAUDIO_NONINTERLEAVED`, so this maps directly onto VST3-style `float**` buses
with no interleave/deinterleave step.

## Public API

```cpp
namespace seq24 { namespace engine {

struct AudioDeviceInfo {
    unsigned int id;             // RtAudio device id (NOT a 0-based index)
    std::string  name;
    unsigned int outputChannels;
    unsigned int preferredRate;  // device preferred sample rate (Hz)
    bool         isDefault;
};

class AudioEngine {
public:
    using RenderCallback =
        std::function<void(float** out, int numChannels, int nframes, double sampleRate)>;

    AudioEngine();
    ~AudioEngine();

    // device enumeration / selection (message thread)
    std::vector<AudioDeviceInfo> enumerateOutputDevices();
    unsigned int defaultOutputDeviceId();
    void         selectDevice(unsigned int deviceId);   // 0 == use default
    unsigned int selectedDeviceId() const;

    // stream lifecycle (message thread)
    bool open(unsigned int sampleRate  = 48000,
              unsigned int blockSize   = 512,
              unsigned int numChannels = 2);
    bool start();
    void stop();
    void close();
    bool isOpen()    const;
    bool isRunning() const;

    // render callback
    void setRenderCallback(RenderCallback cb);

    // granted stream parameters (valid after open())
    unsigned int sampleRate()  const;
    unsigned int blockSize()   const;   // granted frames/block (re-read from RtAudio)
    unsigned int numChannels() const;

    // master level meters (audio thread -> any thread, lock-free atomics)
    float masterPeak() const;           // peak |sample| of last block, 0..~1
    float masterRms()  const;           // RMS of last block, 0..~1

    // diagnostics
    unsigned long long callbackCount()  const;
    unsigned long long underflowCount() const;  // xruns since open()
    const std::string& lastError()      const;
};

}} // namespace seq24::engine
```

### Typical lifecycle

```cpp
AudioEngine engine;
auto devices = engine.enumerateOutputDevices();   // optional: show to user
engine.selectDevice(chosenId);                     // optional: else default

engine.setRenderCallback(myRenderFn);              // BEFORE start()
if (!engine.open(48000, 512, 2)) { /* engine.lastError() */ }
// re-read what we actually got:
auto sr = engine.sampleRate();
auto bs = engine.blockSize();                       // may differ from request
engine.start();
// ... audio flowing on RtAudio's realtime thread ...
engine.stop();
engine.close();
```

`open()` requests a sample rate / block size; the **granted** values are
re-read and exposed via `sampleRate()` / `blockSize()` — always allocate your
process buffers against those, not the requested values.

All operations wrap RtAudio (which throws) in try/catch and return `false`
(or empty) on failure; inspect `lastError()` for the message.

## How the track/graph module registers its render callback

The track/graph (mixer) module is the **single producer** of audio. It owns one
callback that the AudioEngine invokes once per block on the audio thread:

```cpp
audioEngine.setRenderCallback(
    [graph](float** out, int numChannels, int nframes, double sampleRate) {
        // Pull/mix the hosted-plugin and track outputs into `out`.
        // `out[c]` is a planar float buffer of `nframes` samples for channel c.
        graph->render(out, numChannels, nframes, sampleRate);
    });
```

Register it **before** `start()`. To swap the callback at runtime, `stop()` the
stream first — replacing a live `std::function` is not realtime-safe.

The callback **must fully write** all `numChannels * nframes` samples (the
buffers are not pre-zeroed). If a renderer is not registered, the engine writes
silence itself. The engine computes master peak/RMS from whatever the callback
left in `out`, so the meters reflect the final mix.

### Realtime contract (audio thread)

The render callback runs on RtAudio's dedicated audio thread, requested with
realtime scheduling (`RTAUDIO_SCHEDULE_REALTIME`). Inside it — and everything it
calls (including hosted-plugin `process()`) — you **must not**:

* allocate (`new` / `delete` / `malloc`, container growth),
* lock (`std::mutex`, blocking syscalls),
* do I/O (file / console / network).

Communicate with the UI/sequencer threads only via `std::atomic`, SPSC ring
buffers, or RCU-style pointer swaps. **Pre-allocate every process/mix buffer
before `start()`.** The engine itself follows this rule: the audio thread only
touches `std::atomic` counters/levels and a pre-sized channel-pointer vector.

## Master meters

`masterPeak()` and `masterRms()` are lock-free `std::atomic<float>` readouts
updated every block from the contents of `out` (across all channels). A UI meter
can poll them from any thread at frame rate. For a sine of amplitude A you should
see peak ≈ A and RMS ≈ A/√2.

## Verified result (this machine, WASAPI)

`audio_test.exe` output:

```
=== Output devices (3) ===
default output id = 131
Device id 129: "Speakers (2- High Definition Audio Device)"   2ch @ 48000 Hz
Device id 130: "CABLE Input (VB-Audio Virtual Cable)"         2ch @ 48000 Hz
Device id 131: "1 - 100058007 (AMD High Definition Audio ...)" [DEFAULT] 2ch @ 48000 Hz

Opened stream: 48000 Hz, 512 frames (granted), 2 channels
underflows (xrun) : 0  [OK: zero underflows]
master peak       : 0.2000
master rms        : 0.1426   (= 0.2 / sqrt(2), correct for a sine)
exit code 0
```

> Note: `callbackCount() * blockSize()` underestimates wall-clock time, because
> WASAPI shared mode may deliver more frames per callback than the granted block
> size. Underflow count and the meters are the reliable health signals.
