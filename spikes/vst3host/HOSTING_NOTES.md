# VST3 Hosting Notes — seq24 Windows port (ZERO-JUCE)

Status of this spike: **SUCCESS.** The Steinberg VST3 SDK hosting layer
(`VST3::Hosting::Module`, `PlugProvider`, and the `Steinberg::Vst` core
interfaces) compiles and links cleanly with the **mingw64 g++ 15.2.0**
toolchain, and the spike host loads + introspects real plugins on this
machine (Cardinal, CHOWTapeModel, TAL-U-NO-LX). No fork of the SDK and no
source patching was required — only correct CMake source selection and a
couple of Windows defines/link libs.

This document records what it took, the gotchas, and a concrete, honest plan
for the full host.

---

## 1. What the spike proves

`spike_vst3host.cpp` does, with mingw64:

- `VST3::Hosting::Module::create(path, err)` — loads a `.vst3` (works for both
  single-file `.vst3` DLLs **and** bundle directories
  `Foo.vst3/Contents/x86_64-win/Foo.vst3`; the loader resolves the bundle).
- Enumerates the factory classes (`factory.info()`, `factory.classInfos()`).
- Picks the first `kVstAudioEffectClass` ("Audio Module Class").
- Uses `PlugProvider` to instantiate the component, and — for the
  separated-component plugins — to create the controller and **connect**
  component <-> controller via `IConnectionPoint`.
- Queries `IComponent`, `IAudioProcessor`, `IEditController`.
- Prints plugin name, audio bus layout, event (MIDI) bus layout, and the
  parameter list.

Verified plugin topologies (all worked):

| Plugin            | Form         | Classes                         | Audio in/out | Event buses |
|-------------------|--------------|----------------------------------|--------------|-------------|
| Cardinal          | bundle dir   | single (component==controller)   | 14 / 14      | 0           |
| CHOWTapeModel     | bundle dir   | separated component + controller | 1 / 1        | 0           |
| TAL-U-NO-LX-V2    | single file  | separated component + controller | 0 / 1        | 1 in / 1 out (MIDI) |

The TAL case is exactly the shape seq24 needs for an instrument: **no audio
input, stereo audio output, a MIDI event input bus** to receive note on/off.

---

## 2. mingw64 build: what it actually took

The SDK is primarily MSVC-tested, but the hosting subset is portable. The
work was entirely in **which `.cpp` files to compile** and a few defines — not
in patching SDK source.

### 2.1 Do NOT use the SDK's own CMake for a host
The top-level `vst3sdk/CMakeLists.txt` pulls in VSTGUI, the validator,
samples, and plugin-side scaffolding. For a host you only need a small subset.
We compile that subset directly (see `CMakeLists.txt`). This keeps the mingw
surface minimal and the dependencies explicit.

### 2.2 The minimal source set (the non-obvious part)
Two files are easy to miss and produce confusing **link** errors:

1. **`public.sdk/source/vst/vstinitiids.cpp`** — MUST be compiled exactly once
   by the host. It defines every `Steinberg::Vst::IXxx::iid` symbol
   (`IComponent::iid`, `IAudioProcessor::iid`, `IEditController::iid`,
   `IConnectionPoint::iid`, `IHostApplication::iid`, etc.). Without it you get
   a wall of `undefined reference to Steinberg::Vst::...::iid`.
   - Likewise `pluginterfaces/base/coreiids.cpp` +
     `public.sdk/source/common/commoniids.cpp` for the base/common IIDs, and
     `base/source/baseiids.cpp`.

2. **`public.sdk/source/common/threadchecker_win32.cpp`** — provides
   `Steinberg::Vst::ThreadChecker::create()`, referenced by
   `connectionproxy.cpp`. Easy to forget because it is in `common/`, not
   `hosting/`. (There are `_linux` / `_mac` variants; pick the win32 one.)

Full list is in `CMakeLists.txt`. Beyond those two, the set is: the
pluginterfaces IID/string files, `base/source/*` (fobject, fstring, fbuffer,
fdynlib, fstreamer, fdebug, updatehandler), `base/thread/source/*` (flock,
fcondition), the `public.sdk/source/common` helpers (stringconvert,
memorystream, pluginview), and the `public.sdk/source/vst/hosting` layer
(module, **module_win32**, plugprovider, connectionproxy, hostclasses,
pluginterfacesupport) plus `public.sdk/source/vst/utility/stringconvert.cpp`.

> Platform module file: compile **`module_win32.cpp`** (uses `LoadLibrary` /
> `GetProcAddress`). Do not compile `module_mac.mm` / `module_linux.cpp`.

### 2.3 Defines / flags
- `UNICODE` / `_UNICODE` — `module_win32.cpp` uses wide-char Win32 APIs.
- `_WIN32_WINNT=0x0601` — modern Win32 surface.
- `RELEASE=1` — the SDK's `fdebug.h` switches on `DEVELOPMENT` / `RELEASE`.
  (Either is fine; pick one. RELEASE keeps the assert/log noise down.)
- `-std=gnu++17` (C++17 is required by the hosting layer; the SDK uses
  `std::optional`, structured bindings, etc.).
- Link libs: `ole32 uuid shlwapi` (mingw also auto-links the usual
  `user32/gdi32/...`). `ole32` is needed because the SDK touches COM init on
  Windows; `uuid` for known GUIDs.

### 2.4 Warnings (non-fatal, no action needed)
- `std::wstring_convert` / `std::codecvt_utf8_utf16` deprecation warnings from
  `commonstringconvert.cpp` and `stringconvert.cpp` under libstdc++ 15. These
  are warnings only and the conversions work. If we want them gone later we
  can swap in our own UTF-8<->UTF-16 (`MultiByteToWideChar`), but it is not
  necessary for correctness.

### 2.5 COM / GUID notes for mingw
- No MSVC `__uuidof` is used by the hosting path — the SDK uses its own
  `DECLARE_CLASS_IID` / `iid` static members (hence `vstinitiids.cpp`). This
  is the key reason mingw works without GUID hackery.
- `FUnknown` ref-counting (`addRef`/`release`) is the SDK's own; no
  `IUnknown`/`CoCreateInstance` machinery is required to load and drive a
  plugin. We do not need to `CoInitialize` just to load + process; we WILL want
  `CoInitialize(Ex)` on the UI thread once we open `IPlugView` (some plugin
  editors assume COM/OLE is initialized).

**Bottom line:** the SDK hosting layer is mingw-clean. The risk this spike was
meant to retire is retired.

---

## 3. Plan for the full host

Below is the realistic path from "load + introspect" (done) to "play audio
with MIDI from seq24". Effort estimates assume one developer already familiar
with this spike.

### 3.0 Host application context (do first)
Provide an `IHostApplication` implementation and pass it as the context to the
factory / components. The SDK ships `HostApplication` in `hostclasses.cpp`
(already compiled by the spike). Wire it via
`PluginContextFactory::instance().setPluginContext(&hostApp)` **before**
instantiating, and pass it into `IPluginBase::initialize(context)`. Some
plugins query `IHostApplication` / `IPlugInterfaceSupport` during
`initialize()` and misbehave (or refuse) if it is absent.
Also implement/return `IPlugInterfaceSupport`
(`pluginterfacesupport.cpp` provides one) so plugins can ask which optional
interfaces the host supports. **Effort: ~0.5 day.**

### 3.1 Module loading & class selection — DONE
- `Module::create`, enumerate `classInfos()`, choose by category.
- For seq24, let the user pick the class when a module exposes more than one
  audio class (rare, but Arturia/UVI bundles can). Persist the class UID in the
  project file so reopening is deterministic. **Effort: ~0.5 day.**

### 3.2 Component / controller setup
This is the meat. Sequence (mirrors what `PlugProvider` does internally, but
we will likely roll our own so we control lifetime + threading):

1. `factory.createInstance<IComponent>(classID)`.
2. `component->initialize(hostContext)`.
3. Query `IAudioProcessor` from the component.
4. Get the controller:
   - If the component also implements `IEditController`, use it directly.
   - Else `component->getControllerClassId(...)`,
     `factory.createInstance<IEditController>(controllerCID)`,
     `controller->initialize(hostContext)`.
5. **Connect them**: query `IConnectionPoint` on both, then
   `componentCP->connect(controllerCP)` and the reverse. Route messages
   through a proxy so they are delivered on the right thread — the SDK's
   `connectionproxy.cpp` (already compiled) does this; reuse it.
6. Sync state component->controller once at setup:
   `component->getState(stream); controller->setComponentState(stream)`.

**Effort: ~1–2 days** (most of it is getting the message/connection proxy and
state sync right).

### 3.3 Bus arrangement + processing setup
1. Decide arrangements. For seq24's mixer we will typically want one stereo
   main in (effects) or none (instruments) and one stereo main out.
   - `processor->setBusArrangements(inArr*, nIn, outArr*, nOut)` with
     `Steinberg::Vst::SpeakerArr::kStereo`. Honor the plugin's reply — some
     instruments reject inputs (TAL has 0 audio in) and some effects are
     mono/multi-out. Read back with `getBusArrangement`.
2. Activate the buses you will use:
   `component->activateBus(kAudio, kInput/kOutput, index, true)` and, for
   instruments, `component->activateBus(kEvent, kInput, 0, true)` so the MIDI
   bus is live. (The spike shows TAL's MIDI-in bus is `kDefaultActive`, but
   activate explicitly to be safe.)
3. `ProcessSetup setup{ kRealtime, kSample32, maxBlockSize, sampleRate }`;
   `processor->setupProcessing(setup)`.
4. `component->setActive(true)`.
5. `processor->setProcessing(true)` right before the audio thread starts
   pulling, `setProcessing(false)` when it stops.

Ordering matters: `setBusArrangements` -> `setupProcessing` -> `setActive` ->
`setProcessing`. Changing block size / sample rate later requires
`setActive(false)`, re-`setupProcessing`, `setActive(true)`.
**Effort: ~1 day.**

### 3.4 The `process()` loop (real-time audio thread)
Build a `Steinberg::Vst::ProcessData` per block. The SDK's
`public.sdk/source/vst/hosting/processdata.h` (`HostProcessData`) allocates the
`AudioBusBuffers` arrays for you — use it rather than hand-rolling.

Per block:
- `data.numSamples = blockSize;`
- `data.inputs` / `data.outputs` = `AudioBusBuffers` with
  `channelBuffers32` pointing at our float* channel buffers; `numChannels` and
  `silenceFlags` set correctly. (For an instrument with 0 audio inputs,
  `numInputs = 0`.)
- `data.processContext` -> a `ProcessContext` we fill each block: sample rate,
  `projectTimeSamples`, tempo, time-sig, transport `kPlaying` / `kRecording`
  flags, bar position. seq24 already knows tempo/PPQN/transport, so this is a
  mapping job. Many plugins need a valid `ProcessContext` for tempo-synced
  LFOs/delays.
- **MIDI in**: `data.inputEvents` -> an `IEventList`. Use the SDK's
  `EventList` (`public.sdk/source/vst/hosting/eventlist.h`). For each seq24
  note in the block, push a `Steinberg::Vst::Event`:
  - note on  -> `Event::kNoteOnEvent`, fill `noteOn` (pitch, velocity 0..1,
    channel, `sampleOffset` within the block, `noteId = -1` or a real id).
  - note off -> `Event::kNoteOffEvent`, `noteOff`.
  - CC / pitchbend: VST3 prefers these as **parameter changes** via
    `IMidiMapping` (`controller->getMidiControllerAssignment(...)` maps a CC to
    a paramID), not as raw MIDI. So for CCs we look up the paramID once and then
    feed `IParameterChanges` (below). Raw MIDI events in VST3 are essentially
    just note on/off/poly-pressure; everything else is parameter automation.
  - Set each event's `sampleOffset` for sample-accurate timing inside the
    block.
- **Automation in**: `data.inputParameterChanges` -> an `IParameterChanges`
  (SDK `parameterchanges.h`, `ParameterChanges`). For each automated param,
  `addParameterData(paramId, index)` then `queue->addPoint(sampleOffset,
  normValue, ptIndex)`. This is also how we deliver knob moves from the UI and
  CC->param mappings to the processor thread (lock-free queue from UI/seq
  thread to audio thread).
- `data.outputParameterChanges` -> a sink `IParameterChanges` so the plugin
  can report parameter changes back (e.g. its own UI moving a knob); drain it
  on the UI thread to keep our model in sync.
- Call `processor->process(data)`. Read `data.outputs[...].channelBuffers32`
  into seq24's mixer.

Threading rule: **everything in `process()` must be lock-free / no
allocation.** Pre-allocate the EventList, ParameterChanges, and
HostProcessData; reuse them every block (clear, don't reallocate). Parameter
edits and note events cross from the sequencer/UI thread to the audio thread
via a single-producer/single-consumer ring buffer.
**Effort: ~3–5 days** (this is the part that has to be correct AND real-time
safe; budget time for debugging plugins that are picky about ProcessContext,
silence flags, or block size).

### 3.5 Parameter model (UI <-> controller <-> processor)
- Enumerate params from `IEditController` (spike already does this).
- Normalized values are 0..1; convert for display with
  `controller->normalizedParamToPlain` / `getParamStringByValue`.
- UI edit: `controller->setParamNormalized(id, v)` AND queue an
  `IParameterChanges` point for the processor (the controller does NOT
  automatically forward to the processor — the host bridges them).
- Plugin-driven changes arrive via `IComponentHandler::performEdit` /
  `beginEdit` / `endEdit` — we must implement `IComponentHandler` (and ideally
  `IComponentHandler2`) and give it to `controller->setComponentHandler(...)`.
  This is required for plugin UIs to report knob moves and for "begin/end
  gesture" automation grouping. **Effort: ~2 days.**

### 3.6 Opening the editor in a native Win32 HWND
- `view = controller->createView(Steinberg::Vst::ViewType::kEditor)`.
- Check `view->isPlatformTypeSupported(kPlatformTypeHWND)` (Windows).
- Create a host child `HWND`, then `view->attached(hwnd, kPlatformTypeHWND)`.
- Size: `view->getSize(&rect)` for initial size; implement `IPlugFrame`
  (`resizeView`) and pass it via `view->setFrame(plugFrame)` so the plugin can
  request resizes; resize our `HWND` accordingly. Honor
  `view->canResize()` / `checkSizeConstraint`.
- `CoInitialize(Ex)` on that UI thread before attaching — several editors
  (especially ones using OLE drag/drop or WebView) assume it.
- On close: `view->removed()`, then release the view; destroy the HWND.
- mingw specifics: this is plain Win32 (`CreateWindowEx`, message loop) — no
  MSVC dependency. The one thing to watch is that some plugin editors built
  against MSVC may expect a message pump that processes their timer/paint
  messages; ensure the editor HWND lives on a thread with a running
  `GetMessage`/`DispatchMessage` loop (seq24's main UI thread). **Effort:
  ~2–4 days** including making resize and DPI behave.

### 3.7 State save/restore (for the project file)
- Save: `component->getState(compStream)` and
  `controller->getState(ctrlStream)`; store both blobs.
- Restore: `component->setState`, `controller->setComponentState`
  (component blob), then `controller->setState` (controller blob). Order
  matters. Use the SDK `MemoryStream` (`memorystream.cpp`, already compiled) as
  the `IBStream`. **Effort: ~1 day.**

### 3.8 Teardown
`setProcessing(false)` -> `setActive(false)` -> disconnect connection points ->
`controller->terminate()` -> `component->terminate()` -> release all ->
let the `Module` go out of scope (unloads the DLL). Get the order wrong and
some plugins crash on unload. **Effort: folded into the above.**

---

## 4. Honest effort summary

The "can we even do this with mingw" risk is **gone** — proven by this spike.
Remaining work to a usable single-plugin-per-track host:

| Area                                   | Effort      |
|----------------------------------------|-------------|
| Host context / IHostApplication        | ~0.5 day    |
| Component+controller setup + connect   | ~1–2 days   |
| Bus arrangement + processing setup     | ~1 day      |
| Real-time process loop (audio+MIDI+automation) | ~3–5 days |
| Parameter model + IComponentHandler    | ~2 days     |
| Editor in Win32 HWND (IPlugView)       | ~2–4 days   |
| State save/restore                     | ~1 day      |
| **Total (rough)**                      | **~2–3 weeks** |

Add buffer for plugin-specific quirks: real plugins are inconsistent about
ProcessContext requirements, bus arrangement negotiation, editor resize, and
unload ordering. The single biggest source of bugs will be the real-time
process loop (correctness + no allocation/locks). The editor HWML integration
is the second.

### Top gotchas to remember
1. Compile `vstinitiids.cpp` and `threadchecker_win32.cpp` — missing IID/symbol
   link errors otherwise.
2. Compile `module_win32.cpp`, not the mac/linux variants.
3. Provide `IHostApplication` + `IPlugInterfaceSupport` before `initialize()`.
4. Connect `IConnectionPoint`s and sync component->controller state, or the UI
   and DSP will disagree.
5. Setup order: `setBusArrangements` -> `setupProcessing` -> `setActive` ->
   `setProcessing`. Reverse to tear down.
6. CCs/pitchbend go through `IMidiMapping` -> `IParameterChanges`, not raw MIDI
   events. Only note on/off/poly-pressure are real VST3 events.
7. Pre-allocate everything used in `process()`; the audio thread must not lock
   or allocate.
8. `CoInitialize` the UI thread before `IPlugView::attached`.

---

## 5. How to build & run this spike

```sh
export PATH="/c/msys64/mingw64/bin:$PATH"
cd spikes/vst3host
mkdir -p build && cd build
cmake -G Ninja ..
ninja
./spike_vst3host.exe "C:/Program Files/Common Files/VST3/TAL/TAL-U-NO-LX-V2.vst3"
```

`.vst3` argument may be a single-file plugin or a bundle directory.
