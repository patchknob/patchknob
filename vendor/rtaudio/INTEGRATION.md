# RtAudio Integration Notes (seq24 Windows audio engine)

This documents how the vendored RtAudio is built and used for the seq24
real-time audio engine. It targets the **MINGW64** toolchain (g++ 15.2.0)
and the **WASAPI** backend. The audio callback documented here is the model
for the real-time thread that will later pull rendered audio from hosted
VST3 plugins and mix it for output.

- RtAudio version: upstream `master` (commit `e5f0774`), shallow-cloned.
- Source of truth files: `RtAudio.h`, `RtAudio.cpp` (single .cpp, all backends
  selected by preprocessor defines).
- Verified: `RtAudio.cpp` compiles with **zero warnings/errors** under
  `g++ -std=c++17 -O2 -D__WINDOWS_WASAPI__`. The spike runs and plays a
  440 Hz sine with **0 underflows**.

---

## 1. Compile flags / defines (WASAPI on MINGW64)

Select the backend purely with a preprocessor define. For WASAPI:

```
-D__WINDOWS_WASAPI__
```

All required Windows SDK headers ship with the MINGW64 environment
(`wrl/client.h`, `mmdeviceapi.h`, `audioclient.h`,
`functiondiscoverykeys_devpkey.h`) — no proprietary SDK and no extra include
path is needed for WASAPI. RtAudio internally uses `Microsoft::WRL::ComPtr`.

Recommended standard: `-std=c++17` (RtAudio uses `std::function`, default
member initializers, `override`, etc.). C++14 also works; we standardize on
C++17 to match the rest of the project.

### Link libraries (WASAPI)

```
-lole32 -lwinmm -lksuser -lmfplat -lmfuuid -lwmcodecdspuuid
```

(`ole32` for COM, `winmm` is RtAudio's baseline Windows lib, the rest are the
Media Foundation / WASAPI resampler dependencies.)

### Files to compile

Just two files — compile `RtAudio.cpp` into your build and include `RtAudio.h`:

```
RtAudio.cpp     # the implementation (all APIs, guarded by defines)
RtAudio.h       # public API header
```

No files from `include/` are needed for WASAPI (those are ASIO + the bundled
`dsound.h`). `rtaudio_c.cpp` / `rtaudio_c.h` are the optional C wrapper — not
needed for our C++ usage.

### Exact verified command (spike)

```sh
export PATH="/c/msys64/mingw64/bin:$PATH"

# Compile-check RtAudio alone:
g++ -std=c++17 -O2 -D__WINDOWS_WASAPI__ -c RtAudio.cpp -o RtAudio.o

# Build the spike (RtAudio + spike in one go):
g++ -std=c++17 -O2 -D__WINDOWS_WASAPI__ \
    spike_audio.cpp RtAudio.cpp -o spike_audio.exe \
    -lole32 -lwinmm -lksuser -lmfplat -lmfuuid -lwmcodecdspuuid
```

---

## 2. Backend options / fallbacks

| Backend       | Define                | Status |
|---------------|-----------------------|--------|
| WASAPI        | `__WINDOWS_WASAPI__`  | **Primary.** Compiles & runs cleanly. Low latency, modern default. |
| DirectSound   | `__WINDOWS_DS__`      | **Easy fallback.** Compiles & links cleanly alongside WASAPI. |
| ASIO          | `__WINDOWS_ASIO__`    | Deferred. Needs proprietary Steinberg ASIO SDK (the headers in `include/` are the SDK glue but the SDK license must be obtained). Lowest latency on pro audio interfaces; revisit later. |

### DirectSound fallback (verified)

DirectSound is trivial to add as a secondary backend. It uses RtAudio's
**bundled** `include/dsound.h`, so you must add `-Iinclude` and link `-ldsound`:

```sh
g++ -std=c++17 -O2 -D__WINDOWS_WASAPI__ -D__WINDOWS_DS__ -Iinclude \
    spike_audio.cpp RtAudio.cpp -o spike.exe \
    -lole32 -lwinmm -lksuser -lmfplat -lmfuuid -lwmcodecdspuuid -ldsound
```

When multiple backends are compiled in, choose one at runtime by passing the
`RtAudio::Api` to the constructor (e.g. `RtAudio::WINDOWS_WASAPI` or
`RtAudio::WINDOWS_DS`), or pass `RtAudio::UNSPECIFIED` to let RtAudio pick the
first compiled-in API. Enumerate compiled APIs with
`RtAudio::getCompiledApi()`.

### ASIO (later)

To add ASIO eventually: obtain the Steinberg ASIO SDK, compile
`-D__WINDOWS_ASIO__` together with the ASIO glue sources from `include/`
(`asio.cpp`, `asiodrivers.cpp`, `asiolist.cpp`, `iasiothiscallresolver.cpp`)
and `-Iinclude`. Not pursued now.

---

## 3. Minimal API to open a float output stream

```cpp
#include "RtAudio.h"

// 1. Construct, choosing the backend explicitly.
RtAudio dac( RtAudio::WINDOWS_WASAPI, &errorCallback /* optional */ );

// 2. Pick an output device.
unsigned int dev = dac.getDefaultOutputDevice();   // 0 if none

// 3. Describe the output stream.
RtAudio::StreamParameters oParams;
oParams.deviceId     = dev;
oParams.nChannels    = 2;     // stereo
oParams.firstChannel = 0;

RtAudio::StreamOptions options;
options.flags = RTAUDIO_SCHEDULE_REALTIME;  // request RT-priority callback thread

unsigned int bufferFrames = 512;   // in/out: RtAudio may change it to the
                                   // actual granted size — always re-read it.

// 4. Open. format = RTAUDIO_FLOAT32, interleaved by default.
RtAudioErrorType err = dac.openStream(
    &oParams,            // output params
    nullptr,             // no input
    RTAUDIO_FLOAT32,     // sample format
    48000,               // sample rate (Hz)
    &bufferFrames,       // desired frames/callback (updated in place)
    &audioCallback,      // RtAudioCallback (std::function)
    &userData,           // passed to callback
    &options );

if ( err != RTAUDIO_NO_ERROR ) { /* dac.getErrorText() */ }

// 5. Run / stop / close.
dac.startStream();
// ... audio is now flowing on RtAudio's callback thread ...
if ( dac.isStreamRunning() ) dac.stopStream();
if ( dac.isStreamOpen() )    dac.closeStream();
```

`openStream` returns `RTAUDIO_NO_ERROR` on success; otherwise inspect
`dac.getErrorText()`. An optional error callback can be passed to the
`RtAudio` constructor (signature `void(RtAudioErrorType, const std::string&)`).

---

## 4. Callback signature & threading model (the RT audio thread)

```cpp
typedef std::function<int(void* outputBuffer,
                          void* inputBuffer,
                          unsigned int nFrames,
                          double streamTime,
                          RtAudioStreamStatus status,
                          void* userData)> RtAudioCallback;
```

- Runs on **RtAudio's own dedicated WASAPI thread** (not the caller's thread).
  With `RTAUDIO_SCHEDULE_REALTIME` it requests realtime scheduling priority.
  This is our real-time audio thread.
- `outputBuffer` is a raw buffer of `nFrames * nChannels` samples. With
  `RTAUDIO_FLOAT32` cast to `float*`. **Default layout is interleaved**
  (L,R,L,R,...). Pass `RTAUDIO_NONINTERLEAVED` for planar buffers
  (channel blocks back-to-back) — likely preferable when bridging to VST3,
  which uses non-interleaved `float**` buses.
- `inputBuffer` is null for an output-only stream.
- `nFrames` is the block size for this call (== granted `bufferFrames`, but
  always honor the argument).
- `streamTime` is seconds of audio time since the stream started — useful for
  the sequencer transport / PDC scheduling.
- `status` is a bitmask; check `RTAUDIO_OUTPUT_UNDERFLOW` (xrun: we produced
  data too slowly) and `RTAUDIO_INPUT_OVERFLOW`.
- Return value: `0` = continue, `1` = drain & stop, `2` = abort immediately.

### Real-time rules (must hold for the VST3-host path)

This thread must be lock-free and allocation-free:
- No `malloc`/`new`/`delete`, no `std::mutex` locking, no file or console I/O,
  no blocking syscalls.
- Communicate with the UI/sequencer thread via lock-free structures
  (`std::atomic`, SPSC ring buffers, RCU-style pointer swaps).
- The VST3 render path (mixing plugin outputs) will live entirely inside this
  callback. Pre-allocate all plugin/process buffers before `startStream()`.
- The spike demonstrates the pattern: it uses only `std::atomic` counters
  inside the callback and does all printing on the main thread after stop.

---

## 5. Query / select device, sample rate, buffer size

### Enumerate devices

```cpp
std::vector<unsigned int> ids = dac.getDeviceIds();      // stable device IDs
unsigned int defaultOut       = dac.getDefaultOutputDevice();

for ( unsigned int id : ids ) {
    RtAudio::DeviceInfo info = dac.getDeviceInfo( id );
    // info.name                  std::string
    // info.outputChannels        max output channels (0 == input-only)
    // info.inputChannels
    // info.duplexChannels
    // info.isDefaultOutput / isDefaultInput
    // info.sampleRates           std::vector<unsigned int> of supported rates
    // info.preferredSampleRate   device's preferred rate (use this for WASAPI)
    // info.currentSampleRate
    // info.nativeFormats         RtAudioFormat bitmask
}
```

Device IDs are **not** 0-based indices — use exactly the IDs returned by
`getDeviceIds()`. `getDefaultOutputDevice()` returns `0` when there is no
default; fall back to the first device whose `outputChannels > 0`.

### Sample rate

Pass the desired rate to `openStream()`. For WASAPI (shared mode) the device
runs at its system rate; use `DeviceInfo::preferredSampleRate` to match it and
avoid an internal resample. `DeviceInfo::sampleRates` lists the standard rates
RtAudio considers supported.

### Buffer size

Pass desired frames via the `bufferFrames` in/out parameter of `openStream()`.
RtAudio **may change it** to the actual granted size, so re-read `bufferFrames`
after the call and use that value for all buffer allocations. (In the spike,
512 was granted exactly on WASAPI.) Smaller = lower latency but more risk of
underflows; 256-512 frames at 48 kHz is a sane starting point.

---

## 6. Spike verification (recorded)

Command:
```
g++ -std=c++17 -O2 -D__WINDOWS_WASAPI__ spike_audio.cpp RtAudio.cpp \
    -o spike_audio.exe -lole32 -lwinmm -lksuser -lmfplat -lmfuuid -lwmcodecdspuuid
./spike_audio.exe
```

Result:
- WASAPI enumerated 5 devices (3 output-capable shown); default output id 131.
- Opened default output: 48000 Hz, float32, 2 ch interleaved, 512 frames
  requested == 512 granted.
- ~1.5 s of 440 Hz sine: **142 callbacks, 72704 frames (~1.51 s), 0 underflows.**
- Stream stopped and closed cleanly; process exit code 0.
