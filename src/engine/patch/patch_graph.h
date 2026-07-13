//----------------------------------------------------------------------------
//  seq24 Windows port — MODULAR PATCH-GRAPH ENGINE.
//
//  A flat, node-based signal-processing graph. Nodes are wired together by
//  Connections (an output Port of one node -> an input Port of another). Audio
//  and MIDI both flow through ports. The graph is:
//
//    * edited on the MESSAGE THREAD (addNode / connect / disconnect / remove),
//    * COMPILED into an immutable RenderPlan (Kahn topo-sort + pooled-buffer
//      assignment + fan-in sum/merge planning),
//    * PUBLISHED behind a single std::atomic<RenderPlan*> using the exact RCU
//      pattern already shipping in Track::publishChain (see ../graph/track.h).
//
//  The AUDIO THREAD does one acquire-load of the live plan and walks a flat,
//  pre-bound step list. For each step it:
//    1. pre-SUMS all upstream audio into the node's input buses (fan-in),
//    2. pre-MERGES (concat + offset-sort) all upstream MIDI into midiIn,
//    3. calls Node::process(),
//    4. leaves the node's outputs in pool buffers for downstream steps.
//  The device-out node copies the final mix into the engine's out[] buffers.
//
//  Realtime contract (identical to the rest of the engine):
//    * All buffers are pre-allocated in prepare(). process() is lock-free and
//      allocation-free. Graph edits never touch the audio thread's live plan;
//      they build a fresh plan into the inactive double-buffer slot and flip an
//      atomic pointer. Retired nodes are deferred to gc_ and freed by
//      collectGarbage() after a block has certainly passed.
//
//  Cycles are rejected at connect() time (Kahn check on the tentative edge);
//  intentional feedback is expressed with an explicit one-block FeedbackNode.
//  Fixed capacities (kMaxNodes, kMaxFanIn, ...) fail the EDIT — they never
//  truncate audio at runtime.
//----------------------------------------------------------------------------
#ifndef SEQ24_ENGINE_PATCH_PATCH_GRAPH_H
#define SEQ24_ENGINE_PATCH_PATCH_GRAPH_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../plugin_api.h"     // MidiEvent, ParamChange, ProcessBlock, IPluginInstance
#include "../graph/track.h"    // RenderContext
#include "../graph/vu_meter.h" // VuMeter

namespace seq24 { namespace engine { namespace patch {

// ---- compile-time capacities (compile()/connect() reject the EDIT if exceeded)
constexpr int kMaxNodes         = 256;
constexpr int kMaxPortsPerNode  = 32;
constexpr int kMaxFanIn         = 64;
constexpr int kNodeMidiCap      = 512;
constexpr int kMaxAudioSlots    = 512;
constexpr int kMaxMidiSlots     = 512;
constexpr int kMaxBusChan       = 2;      // audio bus width cap (engine is stereo)
constexpr int kMaxSumSrcs       = 2048;   // total summed source-buses across a plan
constexpr int kMaxMidiMergeSrcs = 2048;   // total merged MIDI sources across a plan

// ---- identifiers & ports ---------------------------------------------------
using NodeId = uint32_t;                 // 0 == invalid
using PortId = uint16_t;                 // unique within a node

enum class PortKind : uint8_t { Audio, Midi };
enum class PortDir  : uint8_t { In, Out };

struct PortDesc {
    PortId      id;        // stable within the node
    PortKind    kind;
    PortDir     dir;
    uint16_t    channels;  // Audio: bus width (mono=1, stereo=2); Midi: 1
    const char* name;      // "in L/R", "midi in", "sidechain", "midi out", ...
};

struct PortRef    { NodeId node; PortId port; };
struct Connection { PortRef from; PortRef to; };   // from.dir==Out, to.dir==In

// ---- realtime buffer views handed to a node for ONE block (pool-owned) -----
struct AudioBus   { float* const* chans; int channels; };      // planar [ch][nframes]
struct MidiBuffer { MidiEvent* ev; int count; int capacity; }; // out: node sets count

// Ports map to these arrays in PortDesc declaration order within (kind,dir).
// Audio in is pre-SUMMED at fan-in; MIDI in is pre-MERGED (offset-sorted).
struct NodeProcessContext {
    int                nframes;
    RenderContext      transport;                  // tempoBpm/playPos/isPlaying
    const AudioBus*    audioIn;   int numAudioIn;   // already summed
    AudioBus*          audioOut;  int numAudioOut;  // node writes
    const MidiBuffer*  midiIn;    int numMidiIn;    // already merged
    MidiBuffer*        midiOut;   int numMidiOut;   // node writes + sets .count
    const ParamChange* paramIn;   int numParamIn;   // node-local automation inbox
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

    // Optional device binding. The graph calls these each block (cheap pointer
    // set) so a device sink/source node can reach the engine's out[]/in[].
    virtual void bindDeviceOut(float* const*, int, int) {}
    virtual void bindDeviceIn (const float* const*, int, int) {}
    virtual bool isDeviceSink() const { return false; }

    void setBypass(bool b) { bypass_.store(b, std::memory_order_relaxed); }
    bool bypass() const    { return bypass_.load(std::memory_order_relaxed); }
    virtual int latencySamples() const { return 0; }   // for PDC (deferred)

protected:
    NodeId            id_ = 0;
    std::atomic<bool> bypass_{false};
    friend class PatchGraph;
};

// ---- the graph -------------------------------------------------------------
class PatchGraph {
public:
    PatchGraph() = default;
    ~PatchGraph() = default;

    PatchGraph(const PatchGraph&)            = delete;
    PatchGraph& operator=(const PatchGraph&) = delete;

    // --- topology edits (message thread) ---
    NodeId addNode(std::unique_ptr<Node> n);   // prepares (if prepared); no publish
    bool   removeNode(NodeId id);              // defers destruction (gc_)
    bool   connect(const Connection& c);       // false: mismatch/dup/cycle/cap
    bool   disconnect(const Connection& c);
    void   disconnectAll(NodeId id);

    // --- lifecycle (message thread) ---
    bool   prepare(double sampleRate, int maxBlock);   // sizes pools; compiles once
    void   release();
    bool   compileAndPublish();                        // topo-sort -> plan -> RCU swap
    void   collectGarbage();                           // free nodes/plans past the swap

    // --- device binding (message thread) ---
    void   setDeviceOutNode(NodeId sink);      // node whose in -> out[]
    void   setDeviceInNode(NodeId source);     // node whose out <- in[]  (optional)

    // --- realtime (audio thread) ---
    // Stages `in` into the device-in node (cheap pointer set), if any.
    void   stageDeviceInput(const float* const* in, int channels, int nframes);
    // Drop-in compatible with AudioEngine::RenderCallback.
    void   process(float** out, int numChannels, int nframes, const RenderContext& ctx);

    // --- query (message thread; UI) ---
    Node*                   node(NodeId id) const;
    std::vector<Connection> connections() const { return conns_; }
    int                     nodeCount() const { return (int)nodes_.size(); }
    bool                    lastCompileOk() const { return lastCompileOk_; }

private:
    // ---- immutable compiled schedule, RCU-published like Track's snapshot ----
    struct AudioInPortPlan {
        int    channels;
        float* ch[kMaxBusChan];   // resolved pool pointers (or shared zero-buf)
        bool   needsSum;          // if true: zero ch[] then add sumSrc[first..]
        int    sumFirst, sumCount;
    };
    struct AudioOutPortPlan {
        int    channels;
        float* ch[kMaxBusChan];   // dedicated pool pointers the node writes
    };
    struct MidiInPortPlan {
        int  slot;                // merge/alias slot holding this block's events
        bool needsMerge;          // if false, slot aliases a single source
        int  mergeFirst, mergeCount;
    };
    struct MidiOutPortPlan { int slot; int cap; };

    struct SumSrc { float* ch[kMaxBusChan]; int channels; };

    struct Step {
        Node* node;
        int   numAudioIn;  AudioInPortPlan  ain [kMaxPortsPerNode];
        int   numAudioOut; AudioOutPortPlan aout[kMaxPortsPerNode];
        int   numMidiIn;   MidiInPortPlan   min [kMaxPortsPerNode];
        int   numMidiOut;  MidiOutPortPlan  mout[kMaxPortsPerNode];
    };
    struct RenderPlan {
        Step   steps[kMaxNodes];        int nSteps;
        SumSrc sumSrc[kMaxSumSrcs];     int sumUsed;
        int    midiMergeSrc[kMaxMidiMergeSrcs]; int mergeUsed;
        int    deviceOutStep;
    };

    // ---- compile helpers (message thread) ----
    bool topoSort(std::vector<NodeId>& order) const;   // false on cycle
    bool wouldCycle(const Connection& c) const;         // Kahn on conns_ + c
    bool buildPlan(RenderPlan* plan);                   // false: cap exceeded

    // Locate a port on a node: fills kind/dir/index-within-(kind,dir)/channels.
    struct PortLoc { PortKind kind; PortDir dir; int idx; int channels; bool ok; };
    static PortLoc locate(Node* n, PortId port);

    // ---- message-thread truth ----
    std::unordered_map<NodeId, std::unique_ptr<Node>> nodes_;
    std::vector<Connection>                           conns_;
    std::vector<std::unique_ptr<Node>>                gc_;   // retired; freed post-swap
    NodeId nextId_    = 1;
    NodeId deviceOut_ = 0;
    NodeId deviceIn_  = 0;

    // ---- audio-thread view: RCU double-buffer + live pointer ----
    std::unique_ptr<RenderPlan> planStore_[2];
    std::atomic<int>            activeSlot_{0};
    std::atomic<RenderPlan*>    livePlan_{nullptr};
    std::atomic<Node*>          deviceOutNode_{nullptr};
    std::atomic<Node*>          deviceInNode_{nullptr};

    // Staged device input (set on the audio thread just before process()).
    const float* const* stagedIn_    = nullptr;
    int                 stagedInCh_  = 0;
    int                 stagedInN_   = 0;

    // ---- pre-allocated pools (sized in prepare(); never touched on audio thr) ----
    std::vector<std::vector<float>>     audioSlots_;    // each maxBlock floats (mono)
    std::vector<float>                  zeroBuf_;       // shared silent input
    std::vector<std::vector<MidiEvent>> midiSlots_;     // each kNodeMidiCap events
    std::vector<int>                    midiSlotCount_; // per-slot live event count

    double sampleRate_   = 48000.0;
    int    maxBlock_     = 0;
    bool   prepared_     = false;
    bool   lastCompileOk_ = false;
};

}}} // namespace seq24::engine::patch

#endif // SEQ24_ENGINE_PATCH_PATCH_GRAPH_H
