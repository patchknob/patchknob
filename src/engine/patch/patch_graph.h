//----------------------------------------------------------------------------
//  PatchKnob — MODULAR PATCH-GRAPH ENGINE.
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
//      they build a fresh plan into a retired rotating slot and flip an atomic
//      pointer. The audio thread publishes a per-block GENERATION counter; a
//      retired plan slot is rewritten — and a retired node freed by
//      collectGarbage() — only once that generation has provably advanced past
//      the retirement, so an in-flight block can never observe a torn plan.
//    * Every node process()/bypass call is guarded: a throwing node degrades
//      to silence (outputs zeroed, MIDI dropped) instead of unwinding into the
//      audio callback or leaving the parallel wave barrier hanging.
//
//  Cycles are rejected at connect() time (Kahn check on the tentative edge);
//  intentional feedback is expressed with an explicit one-block FeedbackNode.
//  Fixed capacities (kMaxNodes, kMaxFanIn, ...) fail the EDIT — they never
//  truncate audio at runtime. addNode()/connect() pre-flight the pool budgets
//  so an edit that could never compile is rejected on the spot.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_PATCH_PATCH_GRAPH_H
#define PATCHKNOB_ENGINE_PATCH_PATCH_GRAPH_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../plugin_api.h"     // MidiEvent, ParamChange, ProcessBlock, IPluginInstance
#include "../graph/track.h"    // RenderContext
#include "../graph/vu_meter.h" // VuMeter

namespace PatchKnob { namespace engine { namespace patch {

// ---- compile-time capacities (addNode()/connect() reject the EDIT if exceeded)
constexpr int kMaxNodes         = 512;    // sized for the 500-node patch goal
constexpr int kMaxPortsPerNode  = 260;    // a 256-channel mixer = 257 ports (+out)
constexpr int kMaxFanIn         = 64;
constexpr int kNodeMidiCap      = 512;
constexpr int kMaxAudioSlots    = 4096;   // 512 stereo node outs = 1024 slots + summing headroom
constexpr int kMaxMidiSlots     = 1024;   // 512 MIDI-out ports + merge slots + headroom
constexpr int kMaxBusChan       = 2;      // audio bus width cap (engine is stereo)
constexpr int kMaxSumSrcs       = 4096;   // total summed source-buses across a plan
constexpr int kMaxMidiMergeSrcs = 4096;   // total merged MIDI sources across a plan
constexpr int kPlanSlots        = 3;      // rotating RCU plan buffers (grace period)

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

    // Optional MIDI channel filter (-1 == omni, default).  Instrument-like nodes
    // (PluginNode / PdNode / RackNode) override so a clip can target exactly one
    // of them by matching its MIDI channel.  Used by the clip instrument picker.
    virtual void setMidiChannel(int ch) { (void)ch; }
    virtual int  midiChannel() const { return -1; }

protected:
    NodeId            id_ = 0;
    std::atomic<bool> bypass_{false};
    friend class PatchGraph;
};

// ---- the graph -------------------------------------------------------------
class PatchGraph {
public:
    PatchGraph() = default;
    ~PatchGraph();

    PatchGraph(const PatchGraph&)            = delete;
    PatchGraph& operator=(const PatchGraph&) = delete;

    // --- topology edits (message thread) ---
    NodeId addNode(std::unique_ptr<Node> n);   // prepares (if prepared); no publish
    bool   removeNode(NodeId id);              // defers destruction (gc_)
    bool   connect(const Connection& c);       // false: mismatch/dup/cycle/cap
    bool   disconnect(const Connection& c);
    void   disconnectAll(NodeId id);
    //! Drop any connection whose endpoints no longer resolve to a live port
    //! (e.g. after a node's port count shrank).  Returns how many were removed.
    int    pruneDanglingConnections();

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

    // --- optional multi-threaded processing (message thread) ---
    //! Enable/disable parallel (multi-core) node processing. Default OFF. While
    //! off — and also whenever the live plan carries MIDI or is tiny — the exact
    //! single-threaded path runs unchanged. Safe to toggle at runtime.
    void   setMultiThreaded(bool on) { multiThreaded_.store(on, std::memory_order_relaxed); }
    bool   multiThreaded() const { return multiThreaded_.load(std::memory_order_relaxed); }

    // --- query (message thread; UI) ---
    Node*                   node(NodeId id) const;
    std::vector<NodeId>     nodeIds() const;
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
        int   level;       // dependency wave: 1 + max upstream level (sources == 0)
        int   numAudioIn;  AudioInPortPlan  ain [kMaxPortsPerNode];
        int   numAudioOut; AudioOutPortPlan aout[kMaxPortsPerNode];
        int   numMidiIn;   MidiInPortPlan   min [kMaxPortsPerNode];
        int   numMidiOut;  MidiOutPortPlan  mout[kMaxPortsPerNode];
    };
    // Node-indexed arrays live on the HEAP (vectors sized once in prepare()),
    // not inline: at kMaxNodes=512 a Step is ~20 KB (four kMaxPortsPerNode port
    // arrays), so an inline steps[] would make each plan slot >10 MB of object.
    // The audio thread only ever indexes the vectors — never resizes them.
    struct RenderPlan {
        std::vector<Step>   steps;          int nSteps;
        std::vector<SumSrc> sumSrc;         int sumUsed;
        std::vector<int>    midiMergeSrc;   int mergeUsed;
        int    deviceOutStep;
        // ---- parallel schedule (built on the message thread in buildPlan;
        //      READ-ONLY on the audio/worker threads during process()) ---------
        std::vector<int>    stepsByLevel;   // step indices bucketed by ascending level
        std::vector<int>    levelOffset;    // wave L == [levelOffset[L], levelOffset[L+1])
        int    numLevels;                   // number of dependency waves
        bool   parallelSafe;                // false if ANY step uses MIDI ports
    };

    // ---- compile helpers (message thread) ----
    bool topoSort(std::vector<NodeId>& order) const;   // false on cycle
    bool wouldCycle(const Connection& c) const;         // Kahn on conns_ + c
    bool buildPlan(RenderPlan* plan);                   // false: cap exceeded
    //! Pre-flight the pool/sum/merge budget for the current topology plus an
    //! optional tentative node/edge, WITHOUT writing a plan. addNode()/connect()
    //! call this so an edit that could never compile is rejected on the spot.
    bool budgetFits(Node* extraNode, const Connection* extraConn) const;
    //! True once the audio thread can no longer be walking retired slot `s`.
    bool planSlotReusable(int s) const;
    //! Pick a rewritable plan slot; waits briefly for an in-flight block to end
    //! (bounded). Returns -1 if none frees up (audio thread wedged mid-block).
    int  acquirePlanSlot();

    // ---- realtime step execution (audio thread + worker threads) ----
    //! Run ONE compiled step: fan-in sum/merge -> node process -> publish counts.
    //! The sequential loop and every parallel worker call this identical body, so
    //! behaviour is the same on either path. Each step owns disjoint pool slots,
    //! so distinct same-level steps are safe to run concurrently.
    void runStep(RenderPlan* plan, int stepIndex, int nframes,
                 const RenderContext& ctx);
    //! Cooperative pull-loop for one wave: claim step indices in [base, end) off
    //! workCursor_ and run them. Called by the audio thread AND every worker.
    void drainLevel(RenderPlan* plan, int64_t base, int64_t end,
                    int nframes, const RenderContext& ctx);

    // ---- worker pool lifecycle (message thread) ----
    void startWorkerPool();   // idempotent; spins up N = min(hw,8)-1 workers
    void stopWorkerPool();    // signals stop, joins, clears (no-op if none)
    void workerLoop();        // persistent worker: sleep -> drain waves -> sleep

    // Locate a port on a node: fills kind/dir/index-within-(kind,dir)/channels.
    struct PortLoc { PortKind kind; PortDir dir; int idx; int channels; bool ok; };
    static PortLoc locate(Node* n, PortId port);

    // A retired node parked until the epoch gate proves no in-flight block can
    // still reference it: `stamped` flips (with the then-current block
    // generation) at the first publish AFTER retirement — only stamped entries
    // whose generation has passed may be freed by collectGarbage().
    struct GcNode { std::unique_ptr<Node> node; uint64_t retireGen; bool stamped; };

    // ---- message-thread truth ----
    std::unordered_map<NodeId, std::unique_ptr<Node>> nodes_;
    std::vector<Connection>                           conns_;
    std::vector<GcNode>                               gc_;   // retired; freed post-swap
    NodeId nextId_    = 1;
    NodeId deviceOut_ = 0;
    NodeId deviceIn_  = 0;

    // ---- audio-thread view: RCU rotating slots + live pointer ----
    // rtGen_ is the audio thread's block generation: incremented (seq_cst) on
    // entering process() and again (release) on leaving, so it is ODD exactly
    // while a block is in flight. A slot retired at an odd generation may be
    // rewritten only once rtGen_ has advanced past it; a slot retired at an
    // even generation (audio idle) is immediately reusable. kPlanSlots >= 3
    // keeps two back-to-back compiles inside one audio block from ever
    // touching the plan that block is walking.
    std::unique_ptr<RenderPlan> planStore_[kPlanSlots];
    uint64_t                    slotRetireGen_[kPlanSlots] = { 0, 0, 0 };
    std::atomic<uint64_t>       rtGen_{0};
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

    // ---- optional multi-threaded processing (default OFF) --------------------
    std::atomic<bool>          multiThreaded_{false};

    std::vector<std::thread>   workers_;            // persistent pool (may be empty)
    std::mutex                 poolMutex_;          // guards blockGen_ + the cv wait
    std::condition_variable    poolCv_;             // workers sleep here between blocks
    unsigned                   blockGen_ = 0;       // bumped per block to wake the pool
    std::atomic<bool>          poolStop_{false};    // shutdown flag

    // Per-block job: written by the audio thread BEFORE the wake, read by the
    // workers AFTER it (publication ordered by ++blockGen_ under poolMutex_).
    RenderPlan*                jobPlan_   = nullptr;
    int                        jobFrames_ = 0;
    RenderContext              jobCtx_{};
    int64_t                    jobBase_   = 0;       // workCursor_ value at block start
    int64_t                    jobEnd_    = 0;       // base + nSteps: the block's last claim

    // Cooperative wave scheduling. Monotonic across blocks (never reset per block),
    // so a straggler's stale read can never disturb a later block's window.
    std::atomic<int64_t>       workCursor_{0};      // shared claim counter (fetch/CAS)
    std::atomic<int64_t>       levelEnd_{0};        // armed wave end (exclusive); the gate
    std::atomic<int>           levelRemaining_{0};  // wave barrier: steps still in flight
};

}}} // namespace PatchKnob::engine::patch

#endif // PATCHKNOB_ENGINE_PATCH_PATCH_GRAPH_H
