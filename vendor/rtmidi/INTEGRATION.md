# RtMidi Integration Notes (seq24 Windows / WinMM port)

This document describes how to compile RtMidi for the MINGW64 Windows target
and the minimal RtMidi API needed to replace seq24's ALSA-based `midibus` /
`mastermidibus` (`src/midibus.h`, `src/midibus.cpp`).

RtMidi version vendored: upstream `thestk/rtmidi`, commit `a3233c2`
(shallow clone). License: see `LICENSE` (MIT-style, permissive).

---

## 1. Build configuration (WinMM / MINGW64)

### Source files to compile

Compile exactly two RtMidi files directly into the app:

- `vendor/rtmidi/RtMidi.cpp`  — the implementation (all backends in one file,
  selected via preprocessor defines)
- include `vendor/rtmidi/RtMidi.h`

You do **not** need `rtmidi_c.cpp` / `rtmidi_c.h` (those are the C wrapper) and
you do **not** need RtMidi's own CMake/autotools build. Just add `RtMidi.cpp`
to seq24's source list and add `vendor/rtmidi` to the include path.

### Required define

```
-D__WINDOWS_MM__
```

This is the **only** backend define needed for Windows MultiMedia. Without an
`__*__` backend define, RtMidi compiles a non-functional dummy API.

### Required link library

```
-lwinmm
```

(Windows MultiMedia library `winmm.dll`, shipped with Windows. No external
dependency to vendor.)

### Verified compile / link commands (MINGW64 g++ 15.2.0)

Compile RtMidi only (confirmed clean, no warnings):

```sh
g++ -c -std=c++11 -D__WINDOWS_MM__ RtMidi.cpp -o RtMidi.o
```

Build the spike (RtMidi.cpp compiled in, linked against WinMM):

```sh
g++ -std=c++11 -D__WINDOWS_MM__ spike_midi.cpp RtMidi.cpp -o spike_midi.exe -lwinmm
```

`-std=c++11` or newer is required (RtMidi uses C++11). seq24 already builds
C++; just ensure the standard is at least C++11.

### CMake snippet (for the top-level build, when wiring it in later)

```cmake
add_library(rtmidi STATIC ${CMAKE_SOURCE_DIR}/vendor/rtmidi/RtMidi.cpp)
target_compile_definitions(rtmidi PUBLIC __WINDOWS_MM__)
target_include_directories(rtmidi PUBLIC ${CMAKE_SOURCE_DIR}/vendor/rtmidi)
target_link_libraries(rtmidi PUBLIC winmm)
# then: target_link_libraries(seq24 PRIVATE rtmidi)
```

---

## 2. Minimal API

RtMidi exposes two classes: `RtMidiOut` (output) and `RtMidiIn` (input).
Constructors throw `RtMidiError` on failure — **always wrap construction and
port operations in try/catch**, because RtMidiError is a fatal-by-default
exception type. MIDI bytes are passed as `std::vector<unsigned char>` (or a raw
`const unsigned char*, size_t` overload).

Pass `RtMidi::WINDOWS_MM` explicitly as the API to skip auto-detection:

```cpp
RtMidiOut *out = new RtMidiOut(RtMidi::WINDOWS_MM, "seq24");
RtMidiIn  *in  = new RtMidiIn (RtMidi::WINDOWS_MM, "seq24", 100 /*queue size*/);
```

### (a) Enumerate output ports

```cpp
unsigned int n = out->getPortCount();
for (unsigned int i = 0; i < n; ++i) {
    std::string name = out->getPortName(i);   // human-readable
}
```

### (b) Open an output port

```cpp
out->openPort(portIndex, "seq24 out");   // by index from enumeration above
// or, to create a virtual port other apps connect to:
//   NOTE: openVirtualPort() is NOT supported on WinMM — it throws.
//   Windows has no app-creatable virtual ports natively; you must open an
//   existing port index. (loopMIDI / similar provide named loopback ports.)
```

WinMM caveat: `openVirtualPort()` is unavailable on Windows. seq24's
"manual ALSA ports" mode (`global_manual_alsa_ports`, which calls
`init_out_sub` / `init_in_sub` -> `openVirtualPort`) has no direct equivalent;
on Windows fall back to enumerating real ports, or document loopMIDI as the
way to get virtual ports.

### (c) Send a raw MIDI message

```cpp
std::vector<unsigned char> msg(3);
msg[0] = status | channel;   // e.g. 0x90 | ch for Note-On
msg[1] = data1;              // note
msg[2] = data2;              // velocity
out->sendMessage(&msg);
// raw-pointer overload also exists:
//   out->sendMessage(const unsigned char *bytes, size_t size);
```

For sysex: build the full vector starting with `0xF0` and ending `0xF7` and
call `sendMessage` once (no chunking/usleep needed — WinMM handles the buffer).

### (d) Enumerate + open input ports, and receive incoming MIDI

```cpp
unsigned int n = in->getPortCount();
std::string name = in->getPortName(i);
in->openPort(portIndex, "seq24 in");
in->ignoreTypes(false, false, false);  // receive sysex/timing/active-sense too
```

Two ways to receive (pick ONE per RtMidiIn instance):

**Polling** (matches seq24's `poll_for_midi` + `get_midi_event` model):

```cpp
std::vector<unsigned char> message;
double stamp = in->getMessage(&message);  // non-blocking; returns delta time
if (!message.empty()) {
    // message[0] = status, message[1..] = data bytes
}
```

`getMessage` is non-blocking and returns immediately with an empty vector when
nothing is queued; the returned `double` is the delta-time in seconds since the
previous message.

**Callback** (alternative, push model):

```cpp
void onMidi(double dt, std::vector<unsigned char>* msg, void* user) { ... }
in->setCallback(&onMidi, userData);   // must be set BEFORE openPort effectively
```

---

## 3. Mapping to seq24's midibus / mastermidibus

seq24's MIDI layer is two classes (`src/midibus.h`):

- `midibus`         — one ALSA port (an output OR an input connection)
- `mastermidibus`   — owns the ALSA sequencer handle + arrays of `midibus`,
                      drives clock/transport, polls input

The ALSA model (one `snd_seq_t` shared, ports as addresses, an output queue
drained via `snd_seq_drain_output`) maps onto RtMidi roughly as:

| seq24 (ALSA)                         | RtMidi (WinMM) replacement |
|--------------------------------------|----------------------------|
| `snd_seq_t *m_alsa_seq` (shared)     | no global handle; each `midibus` owns its own `RtMidiOut`/`RtMidiIn` |
| `m_dest_addr_client` / `_port`       | a single `unsigned int` port index into `getPortCount()` |
| `mastermidibus::init()` client/port scan | loop `getPortCount()`/`getPortName()` on a temp `RtMidiOut` (outputs) and a temp `RtMidiIn` (inputs) to build the bus arrays |
| `midibus::init_out()`                | `RtMidiOut::openPort(index, name)` |
| `midibus::init_in()` / `set_input`   | `RtMidiIn::openPort(index)` + `ignoreTypes(false,...)`; `deinit_in` -> `closePort()` |
| `midibus::init_out_sub()` / `init_in_sub()` (virtual ports, manual mode) | **no WinMM equivalent** — `openVirtualPort` throws on Windows; disable manual-port mode or require loopMIDI |
| `midibus::play(event*, channel)`     | build 3-byte `std::vector` from `event::get_status()\|channel` + `get_data(&d1,&d2)`, then `sendMessage` |
| `midibus::sysex(event*)`             | build vector from `event::get_sysex()` / `get_size()`, single `sendMessage` (drop the chunk loop + `usleep` + `flush`) |
| `midibus::flush()` / `mastermidibus::flush()` | **no-op** — WinMM sends immediately; there is no output queue to drain |
| clock/start/stop/continue (`SND_SEQ_EVENT_CLOCK/START/STOP/CONTINUE/SONGPOS`) | send the raw realtime/status bytes directly: Clock `0xF8`, Start `0xFA`, Continue `0xFB`, Stop `0xFC`, Song Position `0xF2 lsb msb` |
| `mastermidibus::poll_for_midi()` (ALSA pollfd) | RtMidi has no fd to poll; either loop `getMessage()` across all input buses (return >0 if any non-empty), or use `setCallback` and a thread-safe queue |
| `mastermidibus::get_midi_event(event*)` | pull next `getMessage()` result, set `event` status/data/size/timestamp; replicate the "Note-On vel 0 -> Note-Off" fixup; ALSA `PORT_START/EXIT/CHANGE` hot-plug events have no WinMM analog (drop or poll port count periodically) |
| `set_bpm` / `set_ppqn` (ALSA queue tempo) | not a MIDI-layer concern in RtMidi; seq24 already computes clock tick timing itself in `midibus::clock()` — keep that logic and just emit `0xF8` bytes |

### Event byte construction (from `src/event.h`)

- `event::get_status()` returns the status byte (e.g. `EVENT_NOTE_ON` = 0x90).
  OR in the low nibble channel: `status | (channel & 0x0F)`.
- `event::get_data(&d1, &d2)` fills the two data bytes.
- Relevant constants: `EVENT_NOTE_OFF` 0x80, `EVENT_NOTE_ON` 0x90,
  `EVENT_SYSEX` 0xF0, `EVENT_SYSEX_END` 0xF7.

### Threading / locking

seq24 already guards `midibus`/`mastermidibus` with its own `mutex`
(`src/mutex.h`). Keep that. RtMidiOut/RtMidiIn instances are not internally
synchronized for concurrent use, so the existing locks remain necessary and
sufficient if you keep one RtMidiOut per bus and serialize access.

### Key behavioral differences to remember

1. No shared sequencer handle / no global queue — per-bus RtMidi objects.
2. `flush()` becomes a no-op (immediate send).
3. `openVirtualPort` unsupported on WinMM (manual-port mode needs rework).
4. No port hot-plug events; enumerate at init (and optionally re-poll count).
5. Input is pull (`getMessage`, non-blocking) or push (`setCallback`), not
   an fd you `poll()`.
6. Always try/catch around RtMidi construction and `openPort`.

---

## 4. Spike result (reference)

`spike_midi.cpp` built and ran on this machine (MINGW64, g++ 15.2.0):

```
MIDI output ports found: 3
  [0] Microsoft GS Wavetable Synth 0
  [1] iCON iKeyboard 6X V1.11 1
  [2] MIDIOUT2 (iCON iKeyboard 6X V1. 2
Opened port [0]: Microsoft GS Wavetable Synth 0
Sent Note-On  (0x90 60 100)
Sent Note-Off (0x80 60 0)
Done.
```

It also exits cleanly (return 0) when zero ports are present.
