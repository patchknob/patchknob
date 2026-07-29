# VST2 Hosting Notes (PatchKnob Windows port, ZERO-JUCE)

Realistic plan and gotchas for building a full VST2 host on top of the spike in
this directory. Written after the spike successfully loaded and introspected
real 64-bit VST2 plugins (808 Machine x64, miniVerb) with mingw64 g++ 15.2.0.

## The clean-room header

- File: `vestige/aeffectx.h`
- Source: LMMS, `include/aeffectx.h`
  (https://raw.githubusercontent.com/LMMS/lmms/master/include/aeffectx.h),
  "aeffectx.h - simple header to allow VeSTige compilation", (c) 2006 Javier
  Serrano Polo, **GPL v2+**. This is the well-known clean-room reimplementation
  of the VST2 `AEffect` ABI used by LMMS, Ardour, dssi-vst, etc. It is **not**
  derived from the Steinberg VST2 SDK (which is no longer distributable). PatchKnob
  is GPL, so GPL header is license-compatible.
- It is a single self-contained header (no separate `aeffect.h`). It defines
  `AEffect`, `VstMidiEvent`, `VstEvent`, `VstEvents`, `VstTimeInfo`,
  `audioMasterCallback`, the calling-convention macro `VST_CALL_CONV`
  (`__cdecl` on Win32), and the opcode / flag constants.
- It deliberately defines only the subset of opcodes LMMS needs. If you need an
  opcode it does not declare (e.g. `effGetNumProgramCategories`, `effIdentify`),
  just add a `constexpr int` for it -- the numeric values are interface
  constants, not Steinberg source. The spike does this for a couple of them.

## ABI facts that matter

- A VST2 plugin is a DLL exporting **`VSTPluginMain`** (older ones export
  **`main`** -- always try both). Signature:
  `AEffect* VST_CALL_CONV entry(audioMasterCallback host);`
- **Calling convention is `__cdecl`** on Windows for both the entry point and
  every function pointer inside `AEffect` (dispatcher, processReplacing,
  get/setParameter) and the host callback. The header's `VST_CALL_CONV` handles
  this -- do not drop it, or you get stack corruption on 32-bit (harmless but
  technically wrong on x64 where there is one calling convention anyway).
- `AEffect->magic` must equal `kEffectMagic` (`'VstP'` = 0x56737450). Verify it.
- The host drives the plugin **only** through `dispatcher`, `processReplacing`,
  `getParameter`, `setParameter`. Never call virtuals -- `AEffect` is a POD.

## Host callback opcodes you MUST implement

The plugin calls these during load and at runtime. The spike answers a minimal
set; a real host needs more. Minimum viable set:

| Opcode                              | What to return / do |
|-------------------------------------|---------------------|
| `audioMasterVersion`                | `2400` (VST 2.4). Return early-and-correctly or some plugins assume VST1 and bail. |
| `audioMasterGetSampleRate`          | current sample rate as integer |
| `audioMasterGetBlockSize`           | current max block size |
| `audioMasterCurrentId`              | for shell plugins, the sub-plugin id to instantiate (`0` = first/default) |
| `audioMasterGetVendorString` / `...ProductString` / `...VendorVersion` | host identity |
| `audioMasterCanDo`                  | answer `1` for capabilities you support: `"sendVstMidiEvent"`, `"receiveVstMidiEvent"`, `"sizeWindow"`, etc. |
| `audioMasterGetCurrentProcessLevel` | `2` = realtime audio thread, `1` = GUI/user thread (matters for thread-aware plugins) |
| `audioMasterGetTime`                | **return a `VstTimeInfo*`** with valid fields flagged (see transport below). Returning 0 means "no transport" -- many synths still run but lose tempo-sync. |
| `audioMasterAutomate`               | plugin telling host a param changed (from its own GUI). Record/forward for automation. |
| `audioMasterIdle` / `audioMasterNeedIdle` | drive `effEditIdle` (see GUI). |
| `audioMasterSizeWindow`             | plugin requests editor resize; resize the HWND and return 1. |
| `audioMasterIOChanged`              | plugin changed its I/O config; re-query numInputs/numOutputs. |
| `audioMasterBeginEdit` / `audioMasterEndEdit` | automation gesture begin/end for a param index. |
| `audioMasterUpdateDisplay`          | re-read program/param names. |

`audioMasterProcessEvents` is the **plugin -> host** direction (e.g. an arp or
MIDI-FX plugin emitting events). Implement it if you want MIDI *output* from
plugins; for a basic instrument host you can return 0.

### `audioMasterGetTime` / transport (tempo sync)

Keep a `VstTimeInfo` owned by the host, fill it each callback, return its
address. Set the `flags` bits for fields you populated:
`kVstTempoValid`, `kVstPpqPosValid`, `kVstBarsValid`, `kVstTimeSigValid`,
`kVstTransportPlaying`, `kVstTransportChanged`, `kVstTransportCycleActive`.
Fields PatchKnob must drive: `samplePos`, `sampleRate`, `tempo` (BPM), `ppqPos`
(quarter-note position), `timeSigNumerator/Denominator`, `barStartPos`. This is
how tempo-synced delays, arps and LFOs lock to PatchKnob's clock.

## Init / activation sequence

```
LoadLibrary -> GetProcAddress("VSTPluginMain" | "main") -> entry(hostCallback)
effOpen
effSetSampleRate (opt = sampleRate as float)
effSetBlockSize  (value = maxBlockSize)
effMainsChanged  (value = 1)            // "resume" / turn plugin on
... process loop ...
effMainsChanged  (value = 0)            // "suspend"
effClose
FreeLibrary
```

## The audio loop (`processReplacing`)

- Allocate `float**` arrays: `numInputs` input channel pointers, `numOutputs`
  output channel pointers, each pointing at a `float[blockSize]` buffer.
- VST2 audio is **deinterleaved, one buffer per channel, 32-bit float**.
- Per block: deliver any MIDI for this block via `effProcessEvents` *first*
  (see below), then
  `effect->processReplacing(effect, inputs, outputs, numFrames)`.
  `numFrames <= blockSize`. `processReplacing` **overwrites** outputs (vs the
  deprecated `process` which accumulates). Require `effFlagsCanReplacing`.
- Instruments report `numInputs == 0` (the 808 spike showed 0 in / 2 out). Pass
  a valid (possibly empty) input array anyway.
- Run the loop on the audio callback thread (RtAudio in this project). Do **not**
  call the GUI or load/unload from that thread.

## Delivering MIDI (`effProcessEvents` + `VstMidiEvent`)

- Build a `VstEvents` block: `numEvents`, then an array of `VstEvent*` each
  actually pointing at a `VstMidiEvent`. `VstEvents` in the header has a
  trailing `VstEvent* events[1]` -- over-allocate so the array holds all events:
  `malloc(sizeof(VstEvents) + (n-1)*sizeof(VstEvent*))`.
- `VstMidiEvent` fields to set: `type = kVstMidiType` (1), `byteSize =
  sizeof(VstMidiEvent)`, `deltaFrames` = sample offset **within the current
  block** (this is how you get sample-accurate timing -- PatchKnob must convert its
  tick-based events to per-block sample offsets), `midiData[0..2]` = status,
  data1, data2 (running status not allowed; `midiData[3]` = 0). For note-off you
  may optionally set `noteOffVelocity`.
- Call `effect->dispatcher(effect, effProcessEvents, 0, 0, &vstEvents, 0)`
  **before** `processReplacing` for that block. Events apply to the block that
  immediately follows.
- SysEx is a different event type and is rare for instruments; skip initially.

## Parameters / automation

- `numParams` from `AEffect`. Per index:
  - name: `dispatcher(effGetParamName, index, 0, buf)`
  - display string: `effGetParamDisplay`; unit/label: `effGetParamLabel`
  - value: `getParameter(effect, index)` -- **always normalized 0.0..1.0**
  - set: `setParameter(effect, index, value)` -- also normalized 0.0..1.0
- VST2 "automation" = the host calling `setParameter` over time. To record
  moves the user makes in the plugin's own GUI, handle `audioMasterAutomate`
  (the plugin calls it with the param index + new value).
- State save/load uses `effGetChunk` / `effSetChunk` (opaque blob) when the
  plugin sets `effFlagsProgramChunks`; otherwise iterate programs/params.

## Editor GUI in a Win32 HWND

1. Check `effFlagsHasEditor`.
2. Create a host HWND (a child/owned window or your plugin-rack panel).
3. `effEditGetRect` -> returns an `ERect*` (top/left/bottom/right) so you can
   size the window *before* opening.
4. `dispatcher(effEditOpen, 0, 0, (void*)hwnd, 0)` -- pass the **parent HWND**
   as `ptr`. The plugin parents its own child window into yours.
5. Pump `effEditIdle` periodically (e.g. on a ~30-60 Hz timer on the GUI
   thread) -- many editors need it to repaint/animate.
6. Honour `audioMasterSizeWindow` to resize.
7. `effEditClose` before `effClose`. All GUI calls on the GUI/main thread only.

Note: the clean-room header declares `effEditGetRect`/`effEditOpen` etc. but
does **not** declare the `ERect` struct -- define your own
`struct ERect { int16_t top,left,bottom,right; };` (matches the documented
layout). Not needed for the introspection spike.

## 32-bit vs 64-bit constraint (important)

- A 64-bit host can load **only 64-bit** plugin DLLs, and vice versa. There is
  no in-process mixing. On this machine there are both: e.g.
  `808 Machine x64.dll` (64-bit, loads fine) and `808 Machine x86.dll`
  (32-bit). Loading a 32-bit DLL into the 64-bit host fails in `LoadLibrary`
  with `GetLastError() == 193` (`ERROR_BAD_EXE_FORMAT`) -- the spike detects and
  reports this case explicitly.
- Verify arch up front with `objdump -f plugin.dll` (`pei-x86-64` = 64-bit,
  `pei-i386` = 32-bit). Since the PatchKnob port targets 64-bit MINGW64, ship a
  64-bit host and only enumerate 64-bit plugins. Supporting 32-bit plugins would
  require an out-of-process bridge (separate 32-bit child .exe + IPC, like
  jBridge / dssi-vst) -- out of scope.

## mingw-specific issues observed

- Builds cleanly with `g++ -std=c++17 -Wall -Wextra` (no warnings) and links
  with zero extra libraries -- `LoadLibrary`/`GetProcAddress` are in kernel32,
  linked by default. CMake `MinGW Makefiles` generator also works.
- `__cdecl` (`VST_CALL_CONV`) is a no-op on x86-64 (single calling convention),
  so there is no mingw/MSVC ABI mismatch for VST2 on 64-bit. (On 32-bit it
  matters and mingw honours `__cdecl` correctly.)
- The `AEffect` struct layout in the clean-room header is hand-verified against
  the real ABI; the spike confirmed `magic == 'VstP'` and correct
  numInputs/numOutputs/numParams against two real commercial plugins, so the
  struct offsets are right for x64.
- Use `LoadLibraryA` with a UTF-8/ANSI path, or `LoadLibraryW` with a wide path
  for non-ASCII plugin paths. Some plugins also assume their own DLL directory
  is on the search path for resources -- consider `SetDllDirectory` or
  `LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH)` so they find sibling DLLs.

## Why VST2 hosting is simpler than VST3

- **One flat C ABI**: a single `AEffect` struct of function pointers + a single
  host callback. No COM, no `queryInterface`/refcounting, no `FUnknown`, no
  GUIDs, no factory enumeration. The VST3 spike in `../vst3host` needs the whole
  Steinberg hosting layer (`VST3::Hosting::Module`, `IComponent`,
  `IAudioProcessor`, `IEditController`, bus/arrangement negotiation).
- **MIDI is trivial**: VST2 takes raw 3-byte MIDI in `VstMidiEvent`. VST3
  abandons MIDI for typed note/param events and forces you to map MIDI CCs to
  parameters via `IMidiMapping`.
- **One process model**: load DLL, fill callback, go. No module bundles, no
  separate component/controller objects to connect.
- Trade-off: VST2 has no official SDK any more (hence the clean-room header) and
  is "deprecated" by Steinberg, but the installed base is enormous and the host
  side is a few hundred lines. For PatchKnob it is the pragmatic choice.

## Status of this spike

- Header compiles with mingw64 g++ 15.2.0, `-Wall -Wextra` clean.
- `spike_vst2host.cpp` builds via plain g++ and via CMake (MinGW Makefiles).
- Ran successfully against two real 64-bit VST2 plugins on this machine and
  printed identity, topology, flags (correct isSynth), and full parameter lists.
- Not implemented (by design): audio processing, MIDI delivery, GUI, transport.
  Those are the next steps and are described above.
