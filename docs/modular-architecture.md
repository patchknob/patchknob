# Modular Audio/MIDI Patch-Graph Architecture (synthesis)

Status: design spec for two build agents. Target: `src/engine/patch/` (engine) and
`src/ui/patch/` (node editor). This document is the contract they implement to.

The three proposals converge on the same proven core, so most of the design is
settled by agreement. This spec adopts that shared core, then makes decisive
picks on the four points where the proposals actually differ.

---

## 1. Verdict on the three proposals

All three propose the *same engine*: a flat `PatchGraph` of typed audio/MIDI
nodes; topology edited on the message thread; a compiler that Kahn-topo-sorts,
assigns pooled buffers, plans fan-in summing/merging, and publishes an immutable
schedule behind one `std::atomic<...*>` — byte-for-byte the RCU pattern already
shipping in `Track::publishChain` (`graph/track.h`) and
`AudioClipPlayer::publishSchedule` (`audioclip/audio_clip_player.h`). The audio
thread does one acquire-load and walks a flat, pre-bound step list: no alloc, no
locks, no per-block sort. All three keep `IPluginInstance`, `Track`,
`MixerGraph`, `VuMeter`, `AudioClipPlayer`, the `audio_app` SPSC rings and
`AudioEngine` intact and recast them as node types. That common core is correct
and is adopted wholesale.

The proposals differ on four axes. The synthesis picks the strongest option on
each:

| Axis | P0 | P1 | P2 | Synthesis pick |
|---|---|---|---|---|
| **Plugin MIDI-out surface** | new `IPluginInstance::pullMidiOut()` pull method | additive `ProcessBlock.midiOut` sink | additive `ProcessBlock.midiOut` sink | **P1/P2**: extend `ProcessBlock`. It matches how VST2 (host callback fires *inside* `processReplacing`) and VST3 (output `IEventList` filled *during* process) actually emit MIDI. A pull method forces the host wrapper to buffer events in a side channel and adds a method to the frozen interface. `ProcessBlock` already carries all per-block I/O; MIDI-out belongs there. |
| **MixerGraph decomposition** | collapse straight to one `MixerNode`; demote `Track`/`MixerGraph` to preset *builders* | **phased**: Phase-1 wrap the whole `MixerGraph` as one shim node (byte-identical), Phase-2 split, Phase-3 free-wire | repackage as one `MixerNode` keeping its `Track` strips | **P1's phasing** as the migration path + **P0's `MixerNode`** as the destination. The Phase-1 shim gives a benchmark that *proves* parity before anything is decomposed (kills P1-risk #5, the perf-regression risk). |
| **DAW track** | track = saved sub-graph preset only | track = a **composite `TrackNode`** wrapping the whole `Track` (black box), optionally "exploded" into sub-nodes | track = instrument node wired to a mixer strip | **P1's composite `TrackNode`** as default (the in-progress DAW/mixer work keeps running verbatim inside it) **plus P0's decomposed preset** as the "explode" form. Engine stays flat; nesting is a UI/organizational affordance, not a hierarchical scheduler. |
| **Sequencer→node binding** | `MidiInNode`s fed by the rings | rings drain into node MIDI inboxes by a track→NodeId map | replace `RouteMsg.track` with a destination NodeId/PortRef | **P0's `MidiInNode`s** (sequencer is just source nodes — uniform) **+ P1's track→node map** kept *behind* the unchanged `audio_app_route_midi(int track,…)` C API so `PatchKnob.cpp`/`midibus.cpp` are untouched. P2's ring-format change is rejected as needless churn to a working lock-free path. |

Everything else — buffer pool with single-source aliasing, fan-in sum/merge,
cycle rejection at `connect()` + an explicit one-block-delay `FeedbackNode`,
fixed compile-time capacities that fail the *edit* (never truncate audio),
deferred node/plan destruction gated on a block counter, the SDL node canvas
reusing `PluginBrowser`/`PluginEditorWindow`/`MixerWindow` — is common to all
three and adopted.

---

## 2. Architecture at a glance

```
                       message thread                          audio thread
  ┌───────────────────────────────────────────┐     ┌──────────────────────────┐
  │  PatchGraph (truth)                        │     │  RenderPlan (immutable)  │
  │   nodes_  : id -> unique_ptr<Node>         │     │   Step[]  (topo order)   │
  │   conns_  : vector<Connection>             │ RCU │   each Step:             │
  │                                            │────►│    node*                 │
  │  edit -> validate -> compile():            │pub  │    resolved audioIn/out  │
  │    Kahn topo-sort                          │lish │    resolved midiIn/out    │
  │    buffer-pool slot assignment (liveness)  │     │    preSums / preMerges   │
  │    fan-in sum/merge planning               │     └──────────┬───────────────┘
  │    -> write inactive planStore_[slot]      │                │ one acquire-load
  │    -> release-store livePlan_              │                ▼   per block
  └───────────────────────────────────────────┘        process(out, n, ctx):
                                                          for each Step:
                                                            run preSums/preMerges
                                                            node->process(ctx)
                                                          AudioDeviceOutNode -> out[]
```

The graph is **flat**. Nesting ("open the device chain of this track") is a UI
grouping over a composite node, not a nested scheduler — see §9.

Reuse map (nothing is discarded):

| Existing piece | Becomes | Reuse |
|---|---|---|
| `IPluginInstance` (VST2/VST3) | `PluginNode` | wraps the pointer; ports from `PluginDescriptor` |
| `AudioClipPlayer` (`: IPluginInstance`) | `AudioClipNode` (or a `PluginNode` over it) | verbatim; already RCU-scheduled |
| `MixerGraph` summing/solo/master/VU | `MixerNode` | `renderBlock` body becomes the node body |
| `MixerGraph` (whole) | `MixerGraphShimNode` | Phase-1 parity bridge, calls `renderBlock` unchanged |
| `Track` (instrument+FX+gain/pan+VU) | composite `TrackNode` | `process()` calls `Track::processBlock` verbatim |
| `Track` pan law + `VuMeter` | `GainPanNode` | constant-power pan lifted from `track.cpp` |
| `AudioEngine` out[]/in[] | `AudioDeviceOutNode` / `AudioDeviceInNode` | callback stages the planar pointers |
| `audio_app` SPSC MIDI ring | `MidiInNode` × N | drain loop moves into the node |
| `audio_app` ParamMsg ring | per-node param inbox | delivered via `NodeProcessContext.paramIn` |
| `VuMeter` | reused as-is on any metering node | lock-free `peak()/rms()` unchanged |

---

## 3. The node model

A `Node` exposes a **flat, ordered port list** (`numPorts()` / `port(i)`), each
`PortDesc` carrying `{id, kind, dir, channels, name}`. Ports are fixed between
`prepare()` calls. The editor addresses ports uniformly by `PortRef{node, port}`.

At **process** time the node receives buffers already resolved, already summed
(audio fan-in) and already merged (MIDI fan-in) by the compiler, presented as
kind/dir-separated arrays:

- audio-in ports, in declaration order, fill `audioIn[0 .. numAudioIn-1]`
- audio-out ports fill `audioOut[...]`, MIDI-in ports fill `midiIn[...]`, etc.

So a node author writes ordinary planar-float / event-list code and never sees
the graph. This is P2's ergonomic `NodeProcessContext` over P0's flat `PortDesc`.
Transport reuses the existing `RenderContext` from `graph/track.h` (do not
reinvent it).

**Fan-in is per input *port*, not per connection.** A port may receive many
wires; the compiler combines them into that port's single bus/buffer before the
node runs. Therefore `numAudioIn` == number of audio-in *ports*.

---

## 4. PatchGraph: compile on the message thread, walk on the audio thread

### 4.1 Topology edits (message thread)

`addNode` / `removeNode` / `connect` / `disconnect` mutate `nodes_`/`conns_` and
mark the graph dirty. `connect()` is rejected (returns false) when:

1. **Kind/direction mismatch** — audio↔midi, or out→out / in→in. Port glows red.
2. **Duplicate** edge.
3. **Cycle** — a DFS reachability test: does `to.node` already reach `from.node`?
   The only legal way to close a loop is through a `FeedbackNode` (§4.5).
4. **Capacity** — would exceed `kMaxFanIn` on the target port (see §10).

Edits are **debounced**: a burst of UI edits coalesces into one `compileAndPublish()`.

### 4.2 compileAndPublish() (message thread)

1. **Kahn topological sort** over the combined audio+MIDI edge DAG. A MIDI edge is
   an ordering constraint too, so a producer of plugin MIDI-out always runs
   before its consumer and the events are ready. Source nodes (`MidiInNode`,
   `AudioDeviceInNode`, instruments with no audio-in) have in-degree 0. If Kahn
   cannot emit every node, a cycle exists → return false, **leave the live plan
   untouched** (the edit is rejected, never a half-graph).
2. **Buffer-pool slot assignment** by liveness / interval colouring: walk the
   topo order; give each output port a pool slot; free a slot back to the pool
   once its last consumer has run. Typical graphs need only a handful of live
   slots. *v1 fallback:* one slot per output port (a few hundred `maxBlock`
   float vectors, all allocated in `prepare()`) — correctness first, colouring
   is a drop-in optimization.
3. **Fan-in planning.** For each input port:
   - 0 sources → point at the shared, permanently-zero buffer.
   - 1 source, matching channels → **alias the source slot** (pointer share,
     zero copy). This is the perf-parity fast path: a straight instrument→FX→mix
     chain costs the same as today's ping-pong.
   - >1 sources → allocate a dedicated sum slot and record an `AudioSum{dst, src[], n}`
     (audio: `memset 0` then `+=` each source, the generalization of
     `mixer_graph.cpp`'s `mL[i] += trkOut[0][i]`) or a `MidiMerge{dst, src[], n}`
     (MIDI: concatenate then stable-sort by `sampleOffset`, capped at `kNodeMidiCap`,
     overflow drops the tail — same policy as the existing `MAX_EV` cap).
4. **Write + publish.** Fill the inactive `planStore_[next]` with the flat
   `Step[]` (each step holds fully-resolved raw pointers into the pool + its
   pre-sum/pre-merge tasks), release-store `livePlan_`, then flip `activeSlot_`.
   Byte-for-byte `Track::publishChain`.

### 4.3 process() (audio thread — alloc-free, lock-free)

```
plan = livePlan_.load(acquire)          // one load; never a half-edited graph
for step in plan.steps[0 .. nSteps):
    run step.preSums     (memset + accumulate)
    run step.preMerges   (concat + offset-sort)
    build NodeProcessContext from resolved pointers
    step.node->process(ctx)             // bypassed nodes copy in->out, pass MIDI
AudioDeviceOutNode writes its resolved audio-in into the staged out[]
```

`PluginNode::process` builds the same `ProcessBlock` `Track::processBlock`
builds today (planar audioIn/out, midiIn, paramIn, transport), points
`blk.midiOut` at its own `midiOutScratch_`, calls `inst_->process(blk)`, and the
plan routes `*blk.numMidiOut` events downstream (§5). Because order is
topological and buffers are pre-bound, there is no per-block sort, allocation or
pointer chasing beyond the flat array.

### 4.4 Buffer pool

Pre-allocated in `prepare()`: audio slots of `channels × maxBlock` planar floats
and MIDI slots of `kNodeMidiCap` events, plus one shared permanently-zero audio
buffer for unwired inputs. Slot count is bounded at compile from the topology;
nothing is ever resized on the audio thread.

### 4.5 Cycles & feedback

Zero-latency loops are physically impossible in single-block pull scheduling, so
`connect()` rejects any edge that would close a cycle. Intentional feedback
(feedback delay, reverb send loop) is expressed with a built-in **`FeedbackNode`**:
a one-block delay whose *output* reads its own previous-block buffer (a retained
pool slot that is **not** recycled) and whose *input* is treated as a graph leaf.
This cuts the cycle at a defined one-block latency so Kahn stays total, while
giving musically-correct feedback. All other loops stay rejected with a UI hint
("would create a feedback loop — insert a Feedback module").

### 4.6 RT-safe lifetime (deferred destruction)

New nodes are `prepare()`d **before** the plan that references them is published.
`removeNode` moves the node to a `gc_` queue; `collectGarbage()` (message thread)
frees a retired node or plan only after a published-block counter proves the
audio thread has advanced past the swap — the exact discipline already documented
for `Track`/`AudioClipPlayer` snapshots and for `g_owned` in `audio_app.cpp`.
This also *fixes* the existing "swap-instrument-while-running" race noted at
`audio_app.cpp` line ~213: instrument swap becomes an atomic plan republish.

---

## 5. plugin_api.h change (additive, non-breaking)

Append three defaulted fields to `ProcessBlock`. Existing implementers (and
`Track::processBlock`, which sets fields individually) keep working; a plugin
that emits no MIDI simply leaves `*numMidiOut == 0`.

```c++
struct ProcessBlock {
    // ... all existing fields unchanged ...
    MidiEvent* midiOut    = nullptr; // node-owned scratch the plugin writes into
    int32_t    midiOutCap = 0;       // capacity of midiOut
    int32_t*   numMidiOut = nullptr; // out-param: events the plugin produced
};
```

Host work (a distinct task behind this additive change — this is *real* host
work, not just a struct tweak; see risks):

- **`vst2_host.cpp`**: `Vst2PluginInstance::process` stashes `blk.midiOut/Cap/numMidiOut`
  in members; the existing `hostCallbackImpl` handles `audioMasterProcessEvents`
  by appending the plugin's outgoing `VstMidiEvent`s into that scratch (it fires
  synchronously inside `processReplacing`).
- **`vst3_host.cpp`**: after the `IAudioProcessor::process` call, drain the output
  `IEventList` into the scratch.
- Audit every aggregate-initialized `ProcessBlock` (none today — `Track` sets
  fields one by one) and keep it a tail append with defaults for source compat.

`AudioClipPlayer` / effect nodes just leave `*numMidiOut = 0`.

---

## 6. Node types (all reuse existing code)

```
PluginNode          wraps IPluginInstance*. Ports from PluginDescriptor:
                    numAudioIn audio-in, numAudioOut audio-out, 1 MIDI-in,
                    and 1 MIDI-out (present iff the plugin can emit events).
                    Owns midiOutScratch_[kNodeMidiCap].
AudioClipNode       embeds AudioClipPlayer (already IPluginInstance + RCU). The
                    "audio clip / timeline" node is essentially free.
MixerNode           N stereo audio-in ports + 1 stereo master out. process() is
                    MixerGraph::renderBlock's body: per-input gain/pan/mute/solo
                    (same relaxed atomics), solo-wins-if-any, sum, master gain,
                    master VuMeter. Per-input strip reuses Track's pan law + VuMeter.
MixerGraphShimNode  Phase-1 only: wraps the WHOLE MixerGraph, process() calls
                    renderBlock() unchanged. The parity benchmark; retired after
                    Phase-2 decomposition proves equal.
TrackNode           composite: owns a PatchKnob::engine::Track; process() calls
                    Track::processBlock verbatim. Ports: 1 MIDI-in, 1 stereo
                    audio-out (+ optional stereo audio-in for monitoring/record).
GainPanNode         constant-power pan (lifted from track.cpp) + inline VuMeter.
AudioDeviceOutNode  the graph sink; copies resolved audio-in into the staged out[]
                    and updates AudioEngine master peak/RMS diagnostics.
AudioDeviceInNode   copies the staged device in[] into its audio-out.
MidiInNode          drains a sequencer/tracker source into its MIDI-out (§8).
MidiOutNode         forwards MIDI-in to a hardware/sequencer sink.
FeedbackNode        one-block delay; the only legal cycle cut (§4.5).
```

---

## 7. audio_app adopts PatchGraph as the render core

`audio_app` keeps owning the singletons; only the render wiring changes.

**Init.** `audio_app_init` builds `g_patch = new PatchGraph()` and constructs the
**default project patch** that reproduces today's fixed topology bit-for-bit:

```
MidiInNode[0..31] ──► TrackNode[0..31] ──► MixerNode ──► AudioDeviceOutNode ──► out[]
                        (each Track = today's instrument+FX+gain/pan+VU)
```

`g_patch->prepare(g_sr, g_block)` prepares every node and compiles once. The
`AudioDeviceOutNode` is designated via `setDeviceOutNode`.

**Render callback** (replaces `audio_render`, still matches
`AudioEngine::RenderCallback`):

```c++
void audio_render(float** out, int numChannels, int nframes, double /*sr*/) {
    drain_midi_ring_into_midiInNodes();   // g_ring  -> MidiInNode scratch, by track
    drain_param_ring_into_node_inboxes(); // g_pring -> per-node ParamChange inbox
    RenderContext ctx = current_transport();
    g_patch->process(out, numChannels, nframes, ctx);   // walks the published plan
}
```

`AudioEngine`, its `RenderCallback` signature, the WASAPI stream and the master
peak/RMS meters are untouched — `AudioDeviceOutNode` writes into `out[]` exactly
where `MixerGraph::renderBlock` writes today.

**Setters preserved.** `audio_app_set_track_instrument` / `audio_app_add_track_fx`
keep their signatures; they now edit the graph (add a `PluginNode`, connect it,
republish) instead of poking a `Track` directly — or, in the composite default,
they forward to the `TrackNode`'s embedded `Track` (identical to today). Either
way the public C API in `audio_app.h` is unchanged, so `PatchKnob.cpp` / `midibus.cpp`
compile untouched.

---

## 8. Sequencer / automation binding (index API preserved)

The sequencer is modeled as **`MidiInNode` source nodes** — one per DAW track by
default, each wired to its track's instrument MIDI-in. This keeps the graph
uniform (the sequencer is "just source nodes") while preserving the existing
lock-free path:

- `audio_app_route_midi(int track, status, d1, d2)` — **unchanged signature**.
  Producers still serialize on `g_midi_producer`; the audio consumer stays
  lock-free. The audio-thread drain (today's loop in `audio_render`) moves into a
  per-block staging step that writes each event into `MidiInNode[track]`'s output
  scratch (sample-offset 0, block-granular, as today). A `track → MidiInNode`
  map (message thread) is the only new state.
- `audio_app_route_param(int track, id, value)` — **unchanged**. Params are
  *node-local control data*, not a graph connection: the `ParamMsg` ring drains
  into the target instrument node's `ParamChange` inbox, delivered via
  `NodeProcessContext.paramIn`. `AutomationPlayer::EmitParam/EmitCC` and the
  tracker FX columns keep their emit path.

Plugin **MIDI-out** re-injection: because the plan is topo-sorted on MIDI edges,
a downstream MIDI-in port's pre-merge reads a `PluginNode`'s `midiOutScratch_`
after it is populated. An arpeggiator/note-FX node therefore feeds a synth node
through an ordinary MIDI wire — this is the capability the whole extension exists
to enable.

Arrangement views may later offer a "target node" chooser to break the 1:1
track↔node default, but the index-based API remains the compatibility floor.

---

## 9. DAW reconciliation: tracks are node-chains, mixer is a module

The in-progress DAW arrangement/track + mixer work is **not invalidated** — it
becomes the default patch, and its objects become node types:

- **Track = a node-chain.** By default a DAW track is a single **composite
  `TrackNode`** that runs the existing `Track` (instrument → FX chain → gain/pan
  → VU) as a black box — so the current track/mixer code runs verbatim. The
  editor's **"explode device chain"** swaps that one composite node for its flat
  equivalent {instrument `PluginNode` → FX `PluginNode`s → `GainPanNode`} for
  users who want to re-wire internals or route plugin MIDI-out mid-chain. The
  scheduler is flat either way; "nesting" is a UI/organizational grouping with a
  breadcrumb, not a hierarchical engine.
- **Mixer = a module.** `MixerNode` renders on-canvas as a wide box: one stereo
  input port per fed track, one master out. Double-click it to pop the existing
  `MixerWindow` (`ui/mixer/mixer_window.h` — channel strips, faders,
  `vu_widget`s) bound to that node's per-input strips and master VU. The rich
  mixer UI is a "zoom into" one node; adding/removing a wire to it adds/removes a
  strip. The `MixerController` pattern (30 Hz Glib timer reading `VuMeter`
  atomics) drives the node meters unchanged.
- **Migration invariant.** At every phase the default patch reproduces today's
  instrument→FX→gain/pan→VU→master→out flow sample-for-sample, so the existing
  work is a strict subset of the new capability. Project save/load serializes
  nodes + connections + UI metadata (canvas x/y, colour, track tags) + each
  plugin's `saveState()`; old projects load by running the default-patch builder
  and re-attaching plugin state.

---

## 10. Fixed capacities & RT-safety checklist

Chosen up front (like `Track::kMaxFx`, `AudioClipPlayer::kMaxClips`); the audio
thread cannot grow them. `compile()` **fails the edit cleanly** when a cap would
be exceeded — it never truncates audio.

```c++
constexpr int kMaxNodes        = 256;   // nodes in one graph
constexpr int kMaxPortsPerNode = 32;    // ports of all kinds on one node
constexpr int kMaxFanIn        = 64;    // sources into one input port
constexpr int kNodeMidiCap     = 512;   // events per MIDI buffer / scratch
constexpr int kMaxAudioSlots   = 512;   // buffer-pool audio slots
```

RT-safety (mirrors the `AudioEngine` contract and the `Track`/clip docs):

- pools + plan double-buffer sized in `prepare()`; `compile()` never runs on the
  audio thread.
- `process()` does one acquire-load + arithmetic; no new/delete/lock/I/O.
- nodes `prepare()`d before the referencing plan is published; removed
  nodes/plans freed only after a block counter advances (`collectGarbage`).
- cross-thread MIDI/param traffic stays on the existing SPSC rings; gain / pan /
  mute / solo / bypass stay plain relaxed atomics.
- **Deferred:** plugin delay compensation (PDC). Each node can report
  `latencySamples()`; the compiler will later sum per-path latency and insert
  delay lines so summed paths align. v1 ships without it — call out that
  look-ahead / linear-phase plugins misalign at fan-in until then.

---

## 11. Node-editor UX (`sdlui/views/patchbay/`)

An SDL canvas styled with the existing monochrome palette, driven by a
`PatchController` modeled on `MixerController` (30 Hz UI tick reading `VuMeter`
atomics for live node meters; instantiate via `audio_app` + `PluginHost`).

- **Node boxes.** Rounded box titled with `typeName()` / plugin name. Input ports
  are dots down the left edge, outputs down the right. **Audio ports draw filled,
  MIDI ports draw hollow/diamond** (distinct palette entries) so kind is legible
  at a glance; multi-channel audio ports show a channel-count badge. Title bar
  carries a bypass toggle and an "editor" button. Nodes with a `VuMeter` show an
  inline meter.
- **Add a module.** Right-click empty canvas → "Add Module": (1) **Built-ins** —
  Mixer, Audio Output, Audio Input, MIDI Input, MIDI Output, Audio Clip Player,
  Gain/Pan, Feedback; (2) **Plugins** — opens the existing `PluginBrowser`
  (`ui/rack/plugin_browser.h`, already background-scanned/cached). Its
  `signal_load_instrument` / `signal_add_fx` collapse into one "descriptor
  chosen" signal → `PluginHost::instantiate` → `PluginNode` → `PatchGraph::addNode`
  at the cursor. Nothing is connected yet, so `compile()` is deferred and cheap.
- **Connect.** Drag from an output dot; a bezier wire (coloured by kind) previews
  live; releasing on a compatible input calls `PatchGraph::connect`. Type/direction
  mismatches snap back with a red flash. Fan-out is free. A fan-in input shows a
  small "Σ" (audio sum) / "merge" (MIDI) badge so implicit combining is visible.
  Right-click a wire → Disconnect; right-click a node → Show Editor / Bypass /
  Remove / Rename. Every edit calls connect/disconnect/addNode then
  `compileAndPublish()`.
- **Mixer as a module / plugin editors.** Double-click `MixerNode` → the full
  `MixerWindow`. Double-click a `PluginNode` → the existing `PluginEditorWindow`
  (`ui/rack/plugin_editor.h`, `openEditor(HWND)` + `idleEditor()` pump).
- **Tracks.** A toolbar "Add Track" drops a pre-wired composite `TrackNode`
  (or the decomposed sub-graph) into a new `MixerNode` input and tags it "Track N"
  so arrangement/sequencer views keep addressing tracks by index. "Explode"
  reveals the device chain; "Save as Track/Preset" groups a node cluster.
- **Feedback affordance.** A drag that would cycle makes `connect()` return false;
  a toast offers to auto-insert a `FeedbackNode` on that path.

---

## 12. Phased rollout

1. **Engine + default patch, headless.** Land `PatchGraph`, `PluginNode`,
   `MixerNode`, `AudioDeviceOutNode`, `MidiInNode`, `TrackNode`, `FeedbackNode`.
   Boot the default patch; keep a `MixerGraphShimNode` that calls
   `MixerGraph::renderBlock` unchanged. Extend `graph_test.cpp`-style self-tests
   to assert the shim's output equals today's `renderBlock` sample-for-sample
   (parity benchmark) before decomposing.
2. **Decompose + wire `audio_app`.** Replace the shim with per-`TrackNode` →
   `MixerNode` → `AudioDeviceOutNode`; move the ring drains into the staging step;
   swap the render callback. Verify the selftest (`audio_app_selftest`) still
   fires a note through to a non-zero master peak.
3. **Plugin MIDI-out host work** behind the additive `ProcessBlock` change
   (vst2 + vst3), enabling arps/note-FX to feed synths.
4. **Node canvas** (`src/ui/patch/`) on top of the working engine.
5. **Later:** buffer-slot graph-colouring, PDC, "target node" arrangement binding.

---

## 13. Canonical header sketch

The exact declarations the build agents implement to. New engine header:
`src/engine/patch/patch_graph.h`. Reuses `MidiEvent`, `ParamChange`,
`ProcessBlock`, `IPluginInstance` from `engine/plugin_api.h`, `RenderContext` and
`VuMeter` from `engine/graph/`.

```c++
// ============================================================================
//  engine/plugin_api.h  — ADDITIVE, non-breaking (append to ProcessBlock)
// ============================================================================
struct ProcessBlock {
    // ... all existing fields unchanged ...
    MidiEvent* midiOut    = nullptr; // node-owned scratch the plugin writes into
    int32_t    midiOutCap = 0;       // capacity of midiOut
    int32_t*   numMidiOut = nullptr; // out-param: events the plugin produced (0 default)
};

// ============================================================================
//  engine/patch/patch_graph.h                        namespace PatchKnob::engine::patch
// ============================================================================
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "../plugin_api.h"     // MidiEvent, ParamChange, ProcessBlock, IPluginInstance
#include "../graph/track.h"    // RenderContext
#include "../graph/vu_meter.h" // VuMeter

namespace PatchKnob { namespace engine { namespace patch {

// ---- compile-time capacities (compile() rejects the EDIT when exceeded) ----
constexpr int kMaxNodes        = 256;
constexpr int kMaxPortsPerNode = 32;
constexpr int kMaxFanIn        = 64;
constexpr int kNodeMidiCap     = 512;
constexpr int kMaxAudioSlots   = 512;

// ---- identifiers & ports ---------------------------------------------------
using NodeId = uint32_t;                 // 0 == invalid
using PortId = uint16_t;                 // unique within a node

enum class PortKind : uint8_t { Audio, Midi };
enum class PortDir  : uint8_t { In, Out };

struct PortDesc {
    PortId   id;         // stable within the node
    PortKind kind;
    PortDir  dir;
    uint16_t channels;   // Audio: bus width (mono=1, stereo=2); Midi: 1
    const char* name;    // "in L/R", "midi in", "sidechain", "midi out", ...
};

struct PortRef    { NodeId node; PortId port; };
struct Connection { PortRef from; PortRef to; };   // from.dir==Out, to.dir==In

// ---- realtime buffer views handed to a node for ONE block (pool-owned) -----
struct AudioBus   { float* const* chans; int channels; };   // planar [ch][nframes]
struct MidiBuffer { MidiEvent* ev; int count; int capacity; }; // out: node sets count

// Ports map to these arrays in PortDesc declaration order within (kind,dir).
// Audio in is pre-SUMMED at fan-in; MIDI in is pre-MERGED (offset-sorted).
struct NodeProcessContext {
    int nframes;
    RenderContext transport;                       // tempoBpm/playPos/isPlaying
    const AudioBus*   audioIn;   int numAudioIn;    // already summed
    AudioBus*         audioOut;  int numAudioOut;   // node writes
    const MidiBuffer* midiIn;    int numMidiIn;     // already merged
    MidiBuffer*       midiOut;   int numMidiOut;    // node writes + sets .count
    const ParamChange* paramIn;  int numParamIn;    // node-local automation inbox
};

// ---- the node base ---------------------------------------------------------
class Node {
public:
    virtual ~Node() = default;
    NodeId id() const { return id_; }
    virtual const char* typeName() const = 0;

    // Static port layout (message thread; fixed between prepare() calls).
    virtual int      numPorts() const = 0;
    virtual PortDesc port(int i) const = 0;

    // Lifecycle (message thread).
    virtual bool prepare(double sampleRate, int maxBlock) = 0;
    virtual void setActive(bool) {}
    virtual void release() {}

    // Realtime (audio thread only; lock-free, allocation-free).
    virtual void process(const NodeProcessContext& ctx) = 0;

    void setBypass(bool b){ bypass_.store(b, std::memory_order_relaxed); }
    bool bypass() const   { return bypass_.load(std::memory_order_relaxed); }
    virtual int latencySamples() const { return 0; }   // for PDC (deferred)

protected:
    NodeId id_ = 0;
    std::atomic<bool> bypass_{false};
    friend class PatchGraph;
};

// ---- the graph -------------------------------------------------------------
class PatchGraph {
public:
    // --- topology edits (message thread) ---
    NodeId addNode(std::unique_ptr<Node> n);             // prepares; does not publish
    bool   removeNode(NodeId id);                        // defers destruction (gc_)
    bool   connect(const Connection& c);                 // false: mismatch/dup/cycle/cap
    bool   disconnect(const Connection& c);
    void   disconnectAll(NodeId id);

    // --- lifecycle (message thread) ---
    bool   prepare(double sampleRate, int maxBlock);     // sizes pools; compiles once
    void   release();
    void   compileAndPublish();                          // topo-sort -> plan -> RCU swap
    void   collectGarbage();                             // free nodes/plans past the swap

    // --- device binding (message thread) ---
    void   setDeviceOutNode(NodeId sink);                // node whose in -> out[]
    void   setDeviceInNode(NodeId source);               // node whose out <- in[]  (opt.)

    // --- realtime (audio thread) ---
    // Stages `in`/`out` into the device nodes (cheap pointer set) then walks the
    // published plan. Drop-in compatible with AudioEngine::RenderCallback.
    void   stageDeviceInput(const float* const* in, int channels, int nframes);
    void   process(float** out, int numChannels, int nframes, const RenderContext& ctx);

    // --- query (message thread; UI) ---
    Node*  node(NodeId id) const;
    std::vector<Connection> connections() const;

private:
    // Immutable compiled schedule, RCU-published exactly like Track's snapshot.
    struct AudioSum  { int dstSlot; int src[kMaxFanIn]; int n; };  // memset 0 then +=
    struct MidiMerge { int dstSlot; int src[kMaxFanIn]; int n; };  // concat + offset-sort
    struct Step {
        Node* node;
        const float* audioIn [kMaxPortsPerNode];   // pool ptrs (or shared zero-buf)
        float*       audioOut[kMaxPortsPerNode];
        MidiEvent*   midiInEv [kMaxPortsPerNode];  int midiInCount [kMaxPortsPerNode];
        MidiEvent*   midiOutEv[kMaxPortsPerNode];  int midiOutCap  [kMaxPortsPerNode];
        AudioSum  preSums  [kMaxPortsPerNode]; int nSums;
        MidiMerge preMerges[kMaxPortsPerNode]; int nMerges;
    };
    struct RenderPlan { Step steps[kMaxNodes]; int nSteps; int deviceOutStep; };

    RenderPlan* compile();               // topo-sort + slot alloc; null on cycle/cap
    void        publish(RenderPlan*);

    // message-thread truth
    std::unordered_map<NodeId, std::unique_ptr<Node>> nodes_;
    std::vector<Connection>                           conns_;
    std::vector<std::unique_ptr<Node>>                gc_;   // retired; freed post-swap
    NodeId nextId_ = 1, deviceOut_ = 0, deviceIn_ = 0;

    // audio-thread view: RCU double-buffer + live pointer
    RenderPlan               planStore_[2];
    std::atomic<int>         activeSlot_{0};
    std::atomic<RenderPlan*> livePlan_{nullptr};

    // pre-allocated pools (sized in prepare(); never touched on the audio thread)
    std::vector<std::vector<float>> audioSlots_;   // each maxBlock * channels
    std::vector<float>              zeroBuf_;       // shared silent input
    std::vector<std::vector<MidiEvent>> midiSlots_; // each kNodeMidiCap
    double sampleRate_ = 48000.0; int maxBlock_ = 0;
};

// ---- representative node declarations (each reuses existing code) ----------
class PluginNode : public Node {            // wraps IPluginInstance* (VST2/VST3)
    IPluginInstance* inst_;                 // ports from inst_->descriptor()
    MidiEvent midiOutScratch_[kNodeMidiCap];
    // process(): build ProcessBlock from ctx; blk.midiOut = midiOutScratch_;
    //            inst_->process(blk); publish *blk.numMidiOut on the MIDI-out port.
};
class AudioClipNode      : public Node { AudioClipPlayer clip_; };          // : IPluginInstance
class MixerNode          : public Node { /* MixerGraph::renderBlock body + VuMeter */ };
class MixerGraphShimNode : public Node { MixerGraph* mg_; };                // Phase-1 parity
class TrackNode          : public Node { Track track_; };                   // composite; Track::processBlock
class GainPanNode        : public Node { /* Track pan law + VuMeter */ };
class AudioDeviceOutNode : public Node { float** target_ = nullptr; };      // -> out[]
class AudioDeviceInNode  : public Node { const float* const* src_ = nullptr; };
class MidiInNode         : public Node { /* ring-drained MIDI -> midiOut */ };
class MidiOutNode        : public Node { /* midiIn -> hardware/sequencer sink */ };
class FeedbackNode       : public Node { /* one-block delay; legal cycle cut */ };

}}} // namespace PatchKnob::engine::patch
```
