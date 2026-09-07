//----------------------------------------------------------------------------
//  PatchKnob — PatchGraph implementation. See patch_graph.h for the
//  design and the RCU (atomic snapshot) publication model, which mirrors
//  Track::publishChain in ../graph/track.cpp.
//----------------------------------------------------------------------------
#include "patch_graph.h"

// VirtualMidiPortsNode only: the graph applies that node's live route table at
// render time (see runStep step 4b), so the compiler must be able to recognise
// the type and read VirtualMidiPortsNode::route(). This is the one built-in the
// scheduler knows by name; everything else stays behind the abstract Node API.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>   // MXCSR intrinsics (FTZ/DAZ) for the denormal guard
#endif

namespace PatchKnob { namespace engine { namespace patch {

namespace {
// Combine (node,port) into a stable 64-bit key for the compile-time out maps.
inline uint64_t outKey(NodeId n, PortId p) {
    return (uint64_t(n) << 16) | uint64_t(p);
}

// Per-thread denormal control: sets the MXCSR FTZ (bit 15) and DAZ (bit 6)
// modes on construction and restores the saved MXCSR on destruction. Denormal
// operands cost 10-100x per sample on x86, so both the audio thread's
// process() AND every worker thread must hold one (MXCSR is per-thread
// state). A deliberate local twin of engine::ScopedNoDenormals in
// ../audio/audio_engine.h so this module does not grow a cross-module
// include. No-op on non-SSE targets.
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
} // namespace

// ===========================================================================
// Port location
// ===========================================================================
PatchGraph::PortLoc PatchGraph::locate(Node* n, PortId port) {
    PortLoc loc{PortKind::Audio, PortDir::In, -1, 0, false};
    int ai = 0, ao = 0, mi = 0, mo = 0;
    const int np = n->numPorts();
    for (int i = 0; i < np; ++i) {
        const PortDesc pd = n->port(i);
        int idx;
        if (pd.kind == PortKind::Audio && pd.dir == PortDir::In)  idx = ai++;
        else if (pd.kind == PortKind::Audio && pd.dir == PortDir::Out) idx = ao++;
        else if (pd.kind == PortKind::Midi && pd.dir == PortDir::In)  idx = mi++;
        else idx = mo++;
        if (pd.id == port) {
            loc.kind = pd.kind; loc.dir = pd.dir; loc.idx = idx;
            loc.channels = (int)pd.channels; loc.ok = true;
            return loc;
        }
    }
    return loc;
}

// ===========================================================================
// Topology edits (message thread)
// ===========================================================================
NodeId PatchGraph::addNode(std::unique_ptr<Node> n) {
    if (!n) return 0;
    if ((int)nodes_.size() >= kMaxNodes) return 0;
    if (n->numPorts() > kMaxPortsPerNode) return 0;

    // Pre-flight the pool budget: a node whose output ports alone would exhaust
    // the audio/MIDI slot pools could never compile, so fail the EDIT here
    // instead of letting a later compile silently drop it.
    if (!budgetFits(n.get(), nullptr)) return 0;

    const NodeId id = nextId_++;
    n->id_ = id;
    if (prepared_) n->prepare(sampleRate_, maxBlock_);
    nodes_.emplace(id, std::move(n));
    return id;
}

bool PatchGraph::removeNode(NodeId id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return false;

    disconnectAll(id);
    if (deviceOut_ == id) { deviceOut_ = 0; deviceOutNode_.store(nullptr, std::memory_order_release); }
    if (deviceIn_  == id) { deviceIn_  = 0; deviceInNode_.store(nullptr,  std::memory_order_release); }

    // Defer destruction: the LIVE plan still references this node until the
    // next compileAndPublish (which stamps the entry with the block generation),
    // and an in-flight block may be walking it. collectGarbage() frees the
    // entry only once both gates have passed.
    gc_.push_back(GcNode{ std::move(it->second), 0, false });
    nodes_.erase(it);
    return true;
}

bool PatchGraph::connect(const Connection& c) {
    Node* fromN = node(c.from.node);
    Node* toN   = node(c.to.node);
    if (!fromN || !toN) return false;

    const PortLoc f = locate(fromN, c.from.port);
    const PortLoc t = locate(toN,   c.to.port);
    if (!f.ok || !t.ok) return false;
    if (f.dir != PortDir::Out || t.dir != PortDir::In) return false;
    if (f.kind != t.kind) return false;          // audio<->audio or midi<->midi only

    // Reject duplicate edges.
    for (const Connection& e : conns_) {
        if (e.from.node == c.from.node && e.from.port == c.from.port &&
            e.to.node   == c.to.node   && e.to.port   == c.to.port)
            return false;
    }

    // Reject fan-in beyond the cap (fail the EDIT; never truncate at runtime).
    int fanIn = 0;
    for (const Connection& e : conns_)
        if (e.to.node == c.to.node && e.to.port == c.to.port) ++fanIn;
    if (fanIn >= kMaxFanIn) return false;

    // Reject cycles (Kahn on the tentative edge). Self-loops included.
    if (wouldCycle(c)) return false;

    // Pre-flight the pool/sum/merge budget with the tentative edge in place: an
    // edge that would overflow a pool could never compile, so fail the EDIT and
    // leave the graph exactly as it was (never truncate audio at runtime).
    if (!budgetFits(nullptr, &c)) return false;

    conns_.push_back(c);
    return true;
}

bool PatchGraph::disconnect(const Connection& c) {
    for (auto it = conns_.begin(); it != conns_.end(); ++it) {
        if (it->from.node == c.from.node && it->from.port == c.from.port &&
            it->to.node   == c.to.node   && it->to.port   == c.to.port) {
            conns_.erase(it);
            return true;
        }
    }
    return false;
}

int PatchGraph::pruneDanglingConnections() {
    const size_t before = conns_.size();
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                    [this](const Connection& c) {
                        Node* fromN = node(c.from.node);
                        Node* toN   = node(c.to.node);
                        if (!fromN || !toN) return true;
                        return !locate(fromN, c.from.port).ok || !locate(toN, c.to.port).ok;
                    }),
                 conns_.end());
    return (int)(before - conns_.size());
}

int PatchGraph::remapNodePorts(NodeId id, const std::function<int(PortId)>& newPort) {
    if (!newPort) return 0;
    std::vector<Connection> kept;
    kept.reserve(conns_.size());
    int dropped = 0;
    for (Connection c : conns_) {
        bool drop = false;
        if (c.from.node == id) {
            const int np = newPort(c.from.port);
            if (np < 0) drop = true; else c.from.port = (PortId)np;
        }
        if (!drop && c.to.node == id) {
            const int np = newPort(c.to.port);
            if (np < 0) drop = true; else c.to.port = (PortId)np;
        }
        if (drop) { ++dropped; continue; }
        kept.push_back(c);
    }
    conns_.swap(kept);
    return dropped;
}

void PatchGraph::disconnectAll(NodeId id) {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                    [id](const Connection& e) {
                        return e.from.node == id || e.to.node == id;
                    }),
                 conns_.end());
}

// ===========================================================================
// Edit-time budget pre-flight (message thread)
//
// Replays buildPlan's slot accounting — output slots, fan-in summing buffers,
// sum sources, MIDI merge slots/sources — over the current topology plus an
// optional tentative node or edge, without writing anything. addNode() and
// connect() reject an edit this says could never compile, keeping the header's
// "fixed capacities fail the EDIT" contract honest. buildPlan keeps its own
// runtime checks as the backstop.
// ===========================================================================
bool PatchGraph::budgetFits(Node* extraNode, const Connection* extraConn) const {
    // Pass 1: per-input-port source count + first-source bus width (the only
    // facts the aliasing decision in buildPlan depends on).
    struct InStat { int count; int firstCh; };
    std::unordered_map<uint64_t, InStat> in;
    auto tally = [&](const Connection& e) {
        int ch = 1;
        if (Node* fromN = node(e.from.node)) {
            const PortLoc f = locate(fromN, e.from.port);
            if (f.ok) ch = std::max(1, std::min(f.channels, kMaxBusChan));
        }
        InStat& s = in[outKey(e.to.node, e.to.port)];
        if (s.count == 0) s.firstCh = ch;
        s.count++;
    };
    for (const Connection& e : conns_) tally(e);
    if (extraConn) tally(*extraConn);

    // Pass 2: sum the demand exactly the way buildPlan allocates.
    int audio = 0, midi = 1;   // MIDI slot 0 is the reserved shared empty buffer
    int sums = 0, merges = 0;
    auto countNode = [&](Node* n, NodeId nid) {
        const int np = n->numPorts();
        for (int i = 0; i < np; ++i) {
            const PortDesc pd = n->port(i);
            const int C = std::max(1, std::min((int)pd.channels, kMaxBusChan));
            if (pd.dir == PortDir::Out) {
                if (pd.kind == PortKind::Audio) audio += C;
                else                            midi += 1;
                continue;
            }
            auto it = in.find(outKey(nid, pd.id));
            const int srcs = (it == in.end()) ? 0 : it->second.count;
            if (pd.kind == PortKind::Audio) {
                if (srcs == 0) continue;                          // zero-buf alias
                if (srcs == 1 && it->second.firstCh == C) continue; // direct alias
                audio += C;                                       // summing buffers
                sums  += srcs;
            } else if (srcs > 1) {
                midi   += 1;                                      // merge slot
                merges += srcs;
            }
        }
    };
    for (const auto& kv : nodes_) countNode(kv.second.get(), kv.first);
    if (extraNode) countNode(extraNode, nextId_);   // the id addNode would assign

    return audio  <= kMaxAudioSlots && midi   <= kMaxMidiSlots &&
           sums   <= kMaxSumSrcs    && merges <= kMaxMidiMergeSrcs;
}

// ===========================================================================
// Cycle detection / topological sort (Kahn)
// ===========================================================================
bool PatchGraph::edgeIsFeedback(const Connection& c) const {
    auto it = nodes_.find(c.to.node);
    if (it == nodes_.end()) return false;
    return it->second->portIsFeedback(c.to.port);
}

bool PatchGraph::wouldCycle(const Connection& c) const {
    // Node-level adjacency over conns_ + the tentative edge c.
    std::unordered_map<NodeId, int> indeg;
    std::unordered_map<NodeId, std::vector<NodeId>> adj;
    for (const auto& kv : nodes_) { indeg[kv.first] = 0; adj[kv.first]; }

    auto addEdge = [&](NodeId a, NodeId b) {
        adj[a].push_back(b);
        indeg[b] += 1;
    };
    for (const Connection& e : conns_)
        if (e.from.node != e.to.node && !edgeIsFeedback(e)) addEdge(e.from.node, e.to.node);
    // A FEEDBACK edge carries no ordering, so it cannot close a cycle -- and a
    // self-loop through one is legal (a node reading its own previous block).
    if (edgeIsFeedback(c)) return false;
    // Tentative edge: a self-loop is trivially a cycle.
    if (c.from.node == c.to.node) return true;
    addEdge(c.from.node, c.to.node);

    std::vector<NodeId> q;
    for (const auto& kv : indeg) if (kv.second == 0) q.push_back(kv.first);
    int visited = 0;
    while (!q.empty()) {
        NodeId u = q.back(); q.pop_back();
        ++visited;
        for (NodeId v : adj[u])
            if (--indeg[v] == 0) q.push_back(v);
    }
    return visited != (int)nodes_.size();   // leftover => cycle
}

bool PatchGraph::topoSort(std::vector<NodeId>& order) const {
    order.clear();
    order.reserve(nodes_.size());
    std::unordered_map<NodeId, int> indeg;
    std::unordered_map<NodeId, std::vector<NodeId>> adj;
    for (const auto& kv : nodes_) { indeg[kv.first] = 0; adj[kv.first]; }
    for (const Connection& e : conns_) {
        if (e.from.node == e.to.node) continue;   // self-loops rejected earlier
        if (edgeIsFeedback(e)) continue;          // one-block return: no ordering
        adj[e.from.node].push_back(e.to.node);
        indeg[e.to.node] += 1;
    }
    std::vector<NodeId> q;
    for (const auto& kv : indeg) if (kv.second == 0) q.push_back(kv.first);
    while (!q.empty()) {
        NodeId u = q.back(); q.pop_back();
        order.push_back(u);
        for (NodeId v : adj[u])
            if (--indeg[v] == 0) q.push_back(v);
    }
    return order.size() == nodes_.size();
}

// ===========================================================================
// Lifecycle
// ===========================================================================
bool PatchGraph::prepare(double sampleRate, int maxBlock) {
    sampleRate_ = sampleRate;
    maxBlock_   = maxBlock;

    // Pre-allocate every pool buffer once. Nothing here runs on the audio thread.
    audioSlots_.assign((size_t)kMaxAudioSlots, std::vector<float>((size_t)maxBlock, 0.0f));
    zeroBuf_.assign((size_t)maxBlock, 0.0f);
    midiSlots_.assign((size_t)kMaxMidiSlots, std::vector<MidiEvent>((size_t)kNodeMidiCap));
    midiSlotCount_.assign((size_t)kMaxMidiSlots, 0);

    // Rotating RCU plan slots: node-indexed arrays live on the heap and are
    // sized here, once, for kMaxNodes — buildPlan never resizes them, so a
    // compile is allocation-free from the audio thread's point of view.
    for (int i = 0; i < kPlanSlots; ++i) {
        planStore_[i].reset(new RenderPlan());
        RenderPlan* p = planStore_[i].get();
        p->steps.resize((size_t)kMaxNodes);
        p->sumSrc.resize((size_t)kMaxSumSrcs);
        p->midiMergeSrc.resize((size_t)kMaxMidiMergeSrcs);
        p->stepsByLevel.resize((size_t)kMaxNodes);
        p->levelOffset.resize((size_t)kMaxNodes + 1);
        p->nSteps        = 0;
        p->sumUsed       = 0;
        p->mergeUsed     = 0;
        p->deviceOutStep = -1;
        p->numLevels     = 0;
        p->parallelSafe  = false;
        slotRetireGen_[i] = 0;
    }
    livePlan_.store(nullptr, std::memory_order_release);

    bool ok = true;
    for (auto& kv : nodes_) {
        ok = kv.second->prepare(sampleRate, maxBlock) && ok;
        kv.second->setActive(true);
    }

    startWorkerPool();

    prepared_ = true;
    compileAndPublish();
    return ok;
}

void PatchGraph::release() {
    stopWorkerPool();
    for (auto& kv : nodes_) kv.second->setActive(false);
    prepared_ = false;
}

PatchGraph::~PatchGraph() {
    stopWorkerPool();
}

void PatchGraph::collectGarbage() {
    // Epoch-gated: a retired node is freed only once (a) a plan compiled AFTER
    // its retirement has been published — compileAndPublish stamps the entry
    // with the audio thread's block generation at that publish — and (b) that
    // generation has provably passed: an even stamp means the audio thread was
    // idle at the publish (any later block loads the new plan), an odd stamp
    // means a block was in flight and rtGen_ must have advanced beyond it.
    // Unstamped entries are still referenced by the LIVE plan and never freed.
    const uint64_t cur = rtGen_.load(std::memory_order_seq_cst);
    for (auto it = gc_.begin(); it != gc_.end();) {
        const bool safe = it->stamped &&
                          (((it->retireGen & 1u) == 0) || cur > it->retireGen);
        if (safe) {
            if (it->node) it->node->release();
            it = gc_.erase(it);
        } else {
            ++it;
        }
    }
}

// ===========================================================================
// Device binding
// ===========================================================================
void PatchGraph::setDeviceOutNode(NodeId sink) {
    deviceOut_ = sink;
    Node* n = node(sink);
    deviceOutNode_.store(n, std::memory_order_release);
}

void PatchGraph::setDeviceInNode(NodeId source) {
    deviceIn_ = source;
    Node* n = node(source);
    deviceInNode_.store(n, std::memory_order_release);
}

// ===========================================================================
// Compile: topo-sort -> pooled-buffer assignment -> RCU publish
// ===========================================================================
bool PatchGraph::buildPlan(RenderPlan* plan) {
    std::vector<NodeId> order;
    if (!topoSort(order)) return false;         // cycle (shouldn't reach here)
    if ((int)order.size() > kMaxNodes) return false;

    plan->nSteps        = 0;
    plan->sumUsed       = 0;
    plan->mergeUsed     = 0;
    plan->deviceOutStep = -1;
    plan->numLevels     = 0;
    plan->parallelSafe  = true;
    plan->levelOffset[0] = 0;

    int audioCursor = 0;    // next free mono audio slot
    int midiCursor  = 1;    // slot 0 reserved as the shared "empty" MIDI buffer

    auto allocAudioChan = [&](float*& out) -> bool {
        if (audioCursor >= kMaxAudioSlots) return false;
        out = audioSlots_[(size_t)audioCursor++].data();
        return true;
    };
    auto allocMidiSlot = [&](int& out) -> bool {
        if (midiCursor >= kMaxMidiSlots) return false;
        out = midiCursor++;
        return true;
    };

    // Output-port -> resolved buffers, filled as we visit nodes in topo order.
    struct AudOut { float* ch[kMaxBusChan]; int channels; };
    std::unordered_map<uint64_t, AudOut> aoutMap;
    std::unordered_map<uint64_t, int>    moutMap;   // -> midi slot
    std::unordered_map<NodeId, int>      nodeLevel; // wave index of each visited node

    int deviceSinkCount = 0;   // >1 sink => additive out[] would race -> stay sequential

    // FEEDBACK inputs are resolved in a SECOND pass: their producer is by
    // definition scheduled after them, so aoutMap does not have its buffers yet
    // while the main loop is on this step.  Everything else about the port is
    // ordinary -- it just points at (or sums) buffers that will be rewritten
    // later in the same block, i.e. it reads the previous block.
    struct PendingFeedback { int step; int ain; NodeId node; PortId port; int channels; };
    std::vector<PendingFeedback> pendingFeedback;

    for (NodeId nid : order) {
        Node* n = nodes_.at(nid).get();
        Step& st = plan->steps[plan->nSteps];
        st.node = n;
        // Resolve the virtual-MIDI patchbay ONCE here (message thread) so the
        // audio thread never pays for an RTTI query. Only the pointer is
        // cached — the route table itself is read live, per block, in runStep.
        // Multiple device sinks SUM into the shared out[] buffer; running them in
        // parallel would be a data race, so force the sequential path.
        if (n->isDeviceSink() && ++deviceSinkCount > 1) plan->parallelSafe = false;
        st.numAudioIn = st.numAudioOut = st.numMidiIn = st.numMidiOut = 0;

        // Wave index: one past the deepest upstream node feeding ANY input of
        // this node; a node with no inputs is a source at level 0. Every upstream
        // node precedes nid in topo order, so its level is already known. Using
        // ALL feeding connections guarantees same-level nodes are truly
        // independent (a data dependency forces a strictly greater level).
        int level = 0;
        for (const Connection& e : conns_) {
            if (e.to.node != nid || e.from.node == e.to.node) continue;
            if (edgeIsFeedback(e)) continue;   // reads last block: not a dependency
            auto lv = nodeLevel.find(e.from.node);
            if (lv != nodeLevel.end() && lv->second + 1 > level) level = lv->second + 1;
        }
        st.level = level;
        nodeLevel[nid] = level;

        const int np = n->numPorts();

        // --- pass 1: assign this node's OUTPUT ports (downstream needs them) --
        for (int i = 0; i < np; ++i) {
            const PortDesc pd = n->port(i);
            if (pd.dir != PortDir::Out) continue;
            if (pd.kind == PortKind::Audio) {
                const int idx = st.numAudioOut++;
                const int C = std::max(1, std::min((int)pd.channels, kMaxBusChan));
                st.aout[idx].channels = C;
                AudOut rec; rec.channels = C;
                for (int c = 0; c < kMaxBusChan; ++c) st.aout[idx].ch[c] = nullptr;
                for (int c = 0; c < C; ++c) {
                    if (!allocAudioChan(st.aout[idx].ch[c])) return false;
                    rec.ch[c] = st.aout[idx].ch[c];
                }
                aoutMap[outKey(nid, pd.id)] = rec;
            } else { // Midi out
                const int idx = st.numMidiOut++;
                if (!allocMidiSlot(st.mout[idx].slot)) return false;
                st.mout[idx].cap = kNodeMidiCap;
                moutMap[outKey(nid, pd.id)] = st.mout[idx].slot;
            }
        }

        // --- pass 2: assign this node's INPUT ports (sum/merge upstream) -----
        for (int i = 0; i < np; ++i) {
            const PortDesc pd = n->port(i);
            if (pd.dir != PortDir::In) continue;

            if (pd.kind == PortKind::Audio) {
                const int idx = st.numAudioIn++;
                const int C = std::max(1, std::min((int)pd.channels, kMaxBusChan));
                AudioInPortPlan& ip = st.ain[idx];
                ip.channels = C;
                for (int c = 0; c < kMaxBusChan; ++c) ip.ch[c] = nullptr;

                // A feedback input is left silent here and patched up below,
                // once every node in the plan has its output buffers.
                if (n->portIsFeedback(pd.id)) {
                    ip.needsSum = false;
                    ip.sumFirst = ip.sumCount = 0;
                    for (int c = 0; c < C; ++c) ip.ch[c] = zeroBuf_.data();
                    pendingFeedback.push_back(
                        PendingFeedback{ plan->nSteps, idx, nid, pd.id, C });
                    continue;
                }

                // Gather upstream sources feeding this input port.
                std::vector<AudOut> srcs;
                for (const Connection& e : conns_) {
                    if (e.to.node != nid || e.to.port != pd.id) continue;
                    auto it = aoutMap.find(outKey(e.from.node, e.from.port));
                    if (it != aoutMap.end()) srcs.push_back(it->second);
                }

                if (srcs.empty()) {
                    ip.needsSum = false;
                    for (int c = 0; c < C; ++c) ip.ch[c] = zeroBuf_.data();
                } else if (srcs.size() == 1 && srcs[0].channels == C) {
                    ip.needsSum = false;                 // alias: no copy in a chain
                    for (int c = 0; c < C; ++c) ip.ch[c] = srcs[0].ch[c];
                } else {
                    ip.needsSum = true;                  // dedicated summing buffers
                    for (int c = 0; c < C; ++c)
                        if (!allocAudioChan(ip.ch[c])) return false;
                    ip.sumFirst = plan->sumUsed;
                    ip.sumCount = 0;
                    for (const AudOut& s : srcs) {
                        if (plan->sumUsed >= kMaxSumSrcs) return false;
                        SumSrc& ss = plan->sumSrc[plan->sumUsed++];
                        ss.channels = s.channels;
                        for (int c = 0; c < kMaxBusChan; ++c) ss.ch[c] = s.ch[c];
                        ip.sumCount++;
                    }
                }
            } else { // Midi in
                const int idx = st.numMidiIn++;
                MidiInPortPlan& mp = st.min[idx];

                std::vector<int> srcSlots;
                for (const Connection& e : conns_) {
                    if (e.to.node != nid || e.to.port != pd.id) continue;
                    auto it = moutMap.find(outKey(e.from.node, e.from.port));
                    if (it != moutMap.end()) srcSlots.push_back(it->second);
                }

                if (srcSlots.empty()) {
                    mp.slot = 0;                 // shared empty (count stays 0)
                    mp.needsMerge = false;
                    mp.mergeFirst = mp.mergeCount = 0;
                } else if (srcSlots.size() == 1) {
                    mp.slot = srcSlots[0];       // alias single source
                    mp.needsMerge = false;
                    mp.mergeFirst = mp.mergeCount = 0;
                } else {
                    if (!allocMidiSlot(mp.slot)) return false;
                    mp.needsMerge = true;
                    mp.mergeFirst = plan->mergeUsed;
                    mp.mergeCount = 0;
                    for (int s : srcSlots) {
                        if (plan->mergeUsed >= kMaxMidiMergeSrcs) return false;
                        plan->midiMergeSrc[plan->mergeUsed++] = s;
                        mp.mergeCount++;
                    }
                }
            }
        }

        // NOTE (was: "any MIDI port disqualifies the plan").  That blanket rule
        // made "Enable multi-core processing" a no-op in the shipping app --
        // MasterMixerNode declares its 24-PPQN MIDI CLOCK OUT unconditionally,
        // every patch has exactly one master mixer, so parallelSafe was false
        // for every plan that ever existed and the sequential branch always ran.
        //
        // The midiSlotCount_/midiSlots_ tables are in fact partitioned exactly
        // like the audio pool, which is already run in parallel:
        //   * every MIDI-OUT port gets its own freshly allocated slot
        //     (allocMidiSlot), so no two steps ever WRITE the same slot;
        //   * a MIDI-IN port with >= 2 sources gets its own merge slot, also
        //     freshly allocated; with exactly one source it ALIASES the
        //     producer's out slot and only ever reads it; with none it aliases
        //     the shared slot 0, whose count is zeroed once per block on the
        //     audio thread and never written by a step (allocMidiSlot starts at
        //     1), so it is read-only for the whole block;
        //   * a step only READS slots produced by nodes it is wired to, and a
        //     data dependency forces a strictly greater level, so the producer
        //     ran in an earlier wave. The wave barrier
        //     (levelRemaining_ release / acquire, then levelEnd_ release /
        //     acquire) is what publishes those writes -- the same edge the audio
        //     buffers already rely on.
        // Cases that ARE unsafe keep their disqualification: >1 device sink
        // (they SUM into the one shared out[] buffer) is handled above.

        if (nid == deviceOut_) plan->deviceOutStep = plan->nSteps;
        plan->nSteps++;
    }

    // ---- resolve FEEDBACK inputs (pass 2b) ---------------------------------
    // aoutMap is complete now, so a return leg can finally see the buffers of
    // the node that is scheduled after it.
    //
    // PARALLEL SAFETY.  The consumer READS a buffer the producer WRITES in the
    // same block.  That is safe exactly while the two sit in DIFFERENT waves --
    // the wave barrier orders them, and the consumer (in the earlier wave)
    // therefore sees the previous block's samples.  Equal levels would let the
    // two run concurrently on different cores, so such a plan drops back to the
    // sequential path (where topo order gives the same one-block semantics).
    for (const PendingFeedback& pf : pendingFeedback) {
        Step& st = plan->steps[pf.step];
        AudioInPortPlan& ip = st.ain[pf.ain];
        const int C = pf.channels;
        std::vector<AudOut> srcs;
        for (const Connection& e : conns_) {
            if (e.to.node != pf.node || e.to.port != pf.port) continue;
            auto it = aoutMap.find(outKey(e.from.node, e.from.port));
            if (it == aoutMap.end()) continue;
            srcs.push_back(it->second);
            auto lp = nodeLevel.find(e.from.node);
            auto lc = nodeLevel.find(e.to.node);
            if (lp != nodeLevel.end() && lc != nodeLevel.end() && lp->second == lc->second)
                plan->parallelSafe = false;
        }
        if (srcs.empty()) continue;                   // stays on the zero buffer
        if (srcs.size() == 1 && srcs[0].channels == C) {
            ip.needsSum = false;
            for (int c = 0; c < C; ++c) ip.ch[c] = srcs[0].ch[c];
        } else {
            ip.needsSum = true;
            for (int c = 0; c < C; ++c)
                if (!allocAudioChan(ip.ch[c])) return false;
            ip.sumFirst = plan->sumUsed;
            ip.sumCount = 0;
            for (const AudOut& src : srcs) {
                if (plan->sumUsed >= kMaxSumSrcs) return false;
                SumSrc& ss = plan->sumSrc[plan->sumUsed++];
                ss.channels = src.channels;
                for (int c = 0; c < kMaxBusChan; ++c) ss.ch[c] = src.ch[c];
                ip.sumCount++;
            }
        }
    }

    // ---- bucket step indices by level (wave) for parallel dispatch ----------
    // Steps stay in topo order; stepsByLevel is a stable per-level view so that
    // wave L occupies the contiguous range [levelOffset[L], levelOffset[L+1]).
    int numLevels = 0;
    for (int s = 0; s < plan->nSteps; ++s)
        if (plan->steps[s].level + 1 > numLevels) numLevels = plan->steps[s].level + 1;
    plan->numLevels = numLevels;

    // Message-thread scratch (heap; buildPlan never runs on the audio thread).
    std::vector<int> counts((size_t)numLevels + 1, 0);
    for (int s = 0; s < plan->nSteps; ++s) counts[(size_t)plan->steps[s].level]++;
    plan->levelOffset[0] = 0;
    for (int L = 0; L < numLevels; ++L)
        plan->levelOffset[L + 1] = plan->levelOffset[L] + counts[(size_t)L];

    std::vector<int> cursor((size_t)numLevels + 1, 0);
    for (int L = 0; L < numLevels; ++L) cursor[(size_t)L] = plan->levelOffset[L];
    for (int s = 0; s < plan->nSteps; ++s) {
        const int L = plan->steps[s].level;
        plan->stepsByLevel[(size_t)cursor[(size_t)L]++] = s;
    }
    return true;
}

bool PatchGraph::planSlotReusable(int s) const {
    if (planStore_[s].get() == livePlan_.load(std::memory_order_relaxed))
        return false;                              // currently published
    const uint64_t r = slotRetireGen_[s];
    if ((r & 1u) == 0) return true;                // retired between blocks (or never live)
    // Retired while a block was in flight: that block ended once the audio
    // thread's generation moved past the recorded value. The seq_cst pairing
    // with process()'s entry increment guarantees any newer block sees the
    // plan published at retirement time, never this slot.
    return rtGen_.load(std::memory_order_seq_cst) > r;
}

int PatchGraph::acquirePlanSlot() {
    // A retired slot stays blocked only while the block that might be walking
    // it is in flight (about one buffer period), so a bounded REAL-TIME wait
    // always suffices when the audio thread is healthy. The deadline is wall
    // clock, not an iteration count: sub-millisecond sleeps round to ~0 on
    // Windows, so a counted loop could expire inside a single audio block. It
    // is also generous — on an oversubscribed box the OS can deschedule an
    // in-flight block for hundreds of milliseconds. Message thread: yielding
    // here is fine.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(1000);
    for (;;) {
        for (int s = 0; s < kPlanSlots; ++s)
            if (planSlotReusable(s)) return s;
        if (std::chrono::steady_clock::now() >= deadline)
            return -1;   // audio thread wedged mid-block; refuse, never tear the plan
        std::this_thread::yield();
    }
}

bool PatchGraph::compileAndPublish() {
    lastCompileOk_ = false;
    if (!prepared_) return false;

    // Drain the retirement list FIRST.  removeNode() parks every deleted node in
    // gc_ and nothing but a unit test ever called collectGarbage(), so a whole
    // session's worth of deleted rack modules, VSTs, Csound nodes and faders
    // stayed allocated -- and, worse, never had release() run, holding their
    // plugin/file/device handles open until the process exited.
    //
    // The RCU grace is unaffected by doing it here: collectGarbage() only frees
    // entries STAMPED by an EARLIER publish whose generation has provably
    // passed, and this call runs before anything about the new plan is touched.
    // Nodes retired since the last publish are still unstamped (still reachable
    // from the live plan) and are correctly left alone until the stamping below.
    collectGarbage();

    // Pick a slot the audio thread can no longer be walking (RCU grace period).
    const int next = acquirePlanSlot();
    if (next < 0) return false;
    RenderPlan* p = planStore_[next].get();

    if (!buildPlan(p)) return false;   // cap exceeded / cycle: keep old live plan

    // Refresh device-node pointer cache before publishing the plan.
    deviceOutNode_.store(node(deviceOut_), std::memory_order_release);
    deviceInNode_.store(node(deviceIn_),   std::memory_order_release);

    // Publish, then retire the previous plan: record the audio thread's block
    // generation so the old slot is rewritten — and any node it referenced is
    // freed — only once that generation has provably advanced (seq_cst orders
    // this read against process()'s entry increment + plan load).
    RenderPlan* old = livePlan_.exchange(p, std::memory_order_seq_cst);
    const uint64_t g = rtGen_.load(std::memory_order_seq_cst);
    for (int s = 0; s < kPlanSlots; ++s)
        if (planStore_[s].get() == old) slotRetireGen_[s] = g;

    // Nodes retired before this publish are no longer referenced by the NEW
    // plan; stamp them so collectGarbage() can free them once `g` has passed.
    for (auto& e : gc_)
        if (!e.stamped) { e.retireGen = g; e.stamped = true; }

    lastCompileOk_ = true;
    return true;
}

// ===========================================================================
// Query
// ===========================================================================
Node* PatchGraph::node(NodeId id) const {
    auto it = nodes_.find(id);
    return (it == nodes_.end()) ? nullptr : it->second.get();
}

std::vector<NodeId> PatchGraph::nodeIds() const {
    std::vector<NodeId> ids;
    ids.reserve(nodes_.size());
    for (const auto& item : nodes_) ids.push_back(item.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// ===========================================================================
// Realtime render (audio thread)
// ===========================================================================
void PatchGraph::stageDeviceInput(const float* const* in, int channels, int nframes) {
    stagedIn_   = in;
    stagedInCh_ = channels;
    stagedInN_  = nframes;
}

void PatchGraph::process(float** out, int numChannels, int nframes,
                         const RenderContext& ctx) {
    if (nframes <= 0) return;

    // Flush denormals for the whole block. MXCSR is per-thread, so the worker
    // threads set their own guard in workerLoop().
    ScopedNoDenormals noDenormals;

    // Start from silence over the FULL requested range so the device-out node
    // only needs to write what it has, an unconnected/absent sink yields clean
    // silence, and — when the host delivers more frames than maxBlock_ — the
    // clamped remainder is guaranteed silence, never stale samples.
    for (int c = 0; c < numChannels; ++c)
        if (out[c]) std::memset(out[c], 0, sizeof(float) * (size_t)nframes);
    if (nframes > maxBlock_) nframes = maxBlock_;   // clamp; never index pools past maxBlock_
    if (nframes <= 0) return;                        // not prepared (maxBlock_ == 0)

    // Publish the block generation: ODD exactly while this block is in flight.
    // compileAndPublish/collectGarbage gate plan-slot reuse and node frees on
    // this counter (the RCU grace period). The exit increment is RAII so no
    // return path — and no throwing node — can leave the generation odd.
    rtGen_.fetch_add(1, std::memory_order_seq_cst);
    struct GenExit {
        std::atomic<uint64_t>& gen;
        ~GenExit() { gen.fetch_add(1, std::memory_order_release); }
    } genExit{ rtGen_ };

    RenderPlan* plan = livePlan_.load(std::memory_order_seq_cst);
    if (!plan) return;

    // Reset per-slot MIDI counts for this block (bounded; audio-thread private).
    std::fill(midiSlotCount_.begin(), midiSlotCount_.end(), 0);

    // Bind device nodes (cheap pointer set).  Bind EVERY device-output sink -- not
    // just the designated one -- so every "Audio Out" module in the patch reaches
    // the device (their outputs SUM; see AudioDeviceOutNode::process).
    for (int s = 0; s < plan->nSteps; ++s)
        if (plan->steps[s].node && plan->steps[s].node->isDeviceSink()) {
            plan->steps[s].node->setDeviceOutAdditive(true);
            plan->steps[s].node->bindDeviceOut(out, numChannels, nframes);
        }
    // Bind every hardware-input source. Audio Input nodes are independent
    // patch points; requiring one hidden/designated node left user-created
    // nodes permanently disconnected from the PortAudio capture buffers.
    for (int s = 0; s < plan->nSteps; ++s)
        if (plan->steps[s].node && plan->steps[s].node->isDeviceSource())
            plan->steps[s].node->bindDeviceIn(stagedIn_, stagedInCh_, nframes);

    // ---- choose the execution path ------------------------------------------
    // Multi-threaded dispatch is OPT-IN and only taken when a worker pool exists,
    // the live plan is parallelSafe (see buildPlan: today that means at most one
    // device sink, since several sinks SUM into the one shared out[] buffer), and
    // the graph is big enough to be worth splitting. The average wave width
    // (nSteps/numLevels) must also make the per-level barrier pay for itself: a
    // nearly-linear chain has no parallelism to extract and would pay one barrier
    // per node for nothing. In every other case the byte-for-byte identical
    // sequential path below runs, exactly as before.
    const bool parallel =
        multiThreaded_.load(std::memory_order_relaxed) &&
        !workers_.empty() &&
        plan->parallelSafe &&
        plan->nSteps >= 4 &&
        plan->numLevels > 0 &&
        (plan->nSteps / plan->numLevels) >= 3;

    if (!parallel) {
        for (int s = 0; s < plan->nSteps; ++s)
            runStep(plan, s, nframes, ctx);
        return;
    }

    // ---- parallel: process the graph one dependency wave (level) at a time ---
    // Publish the block-wide job, then wake the sleeping pool ONCE. For every wave
    // the audio thread and the workers cooperatively pull steps off a shared
    // monotonic cursor (fetch/CAS); an atomic per-wave "remaining" counter is the
    // barrier that keeps wave L+1 from starting before wave L has fully finished.
    // Only atomics and the pre-built level arrays are used here — no locks, no
    // allocation. (The cv/mutex are touched exactly once, to wake the pool.)
    const int64_t base = workCursor_.load(std::memory_order_relaxed);
    {
        // The job fields are PLAIN members shared with the workers, so they are
        // written under the very lock the workers snapshot them under.  They
        // used to be assigned out here, OUTSIDE the mutex: a worker descheduled
        // between the cv-wait's unlock and its own reads could then mix block
        // N's plan pointer with block N+1's base/end, index
        // stepsByLevel[cur - base] outside its window, and over-decrement
        // levelRemaining_ -- which leaves the audio thread's barrier spin below
        // waiting for a count that can never reach zero again.  This adds no new
        // locking on the audio thread: it already took poolMutex_ here to bump
        // blockGen_.
        std::lock_guard<std::mutex> lk(poolMutex_);
        jobPlan_   = plan;
        jobFrames_ = nframes;
        jobCtx_    = ctx;
        jobBase_   = base;
        jobEnd_    = base + plan->levelOffset[plan->numLevels];
        ++blockGen_;
    }
    poolCv_.notify_all();
    for (int L = 0; L < plan->numLevels; ++L) {
        const int     count = plan->levelOffset[L + 1] - plan->levelOffset[L];
        const int64_t end   = base + plan->levelOffset[L + 1];
        levelRemaining_.store(count, std::memory_order_relaxed);
        levelEnd_.store(end, std::memory_order_release);   // arm the wave (opens the gate)
        drainLevel(plan, base, end, nframes, ctx);         // the audio thread helps
        while (levelRemaining_.load(std::memory_order_acquire) != 0) {
            // busy-wait barrier: spin until every worker finishes this wave.
        }
    }
}

// ===========================================================================
// One compiled step. The sequential loop AND every parallel worker call this
// identical body, so behaviour is the same on either path. It touches only this
// step's dedicated pool buffers (disjoint across independent same-level steps),
// which is what makes concurrent execution race-free.
// ===========================================================================
void PatchGraph::runStep(RenderPlan* plan, int stepIndex, int nframes,
                         const RenderContext& ctx) {
    Step& st = plan->steps[stepIndex];

    // ---- 1. pre-SUM audio fan-in into dedicated input buffers ----------
    for (int p = 0; p < st.numAudioIn; ++p) {
        AudioInPortPlan& ip = st.ain[p];
        if (!ip.needsSum) continue;
        for (int c = 0; c < ip.channels; ++c)
            std::memset(ip.ch[c], 0, sizeof(float) * (size_t)nframes);
        for (int k = 0; k < ip.sumCount; ++k) {
            const SumSrc& src = plan->sumSrc[ip.sumFirst + k];
            const int cc = std::min(src.channels, ip.channels);
            for (int c = 0; c < cc; ++c) {
                const float* sp = src.ch[c];
                float*       dp = ip.ch[c];
                for (int i = 0; i < nframes; ++i) dp[i] += sp[i];
            }
        }
    }

    // ---- 2. build audio buses (point at stable plan-owned pointer arrays)
    AudioBus ainB[kMaxPortsPerNode];
    AudioBus aoutB[kMaxPortsPerNode];
    for (int p = 0; p < st.numAudioIn; ++p)
        ainB[p]  = AudioBus{ st.ain[p].ch,  st.ain[p].channels };
    for (int p = 0; p < st.numAudioOut; ++p)
        aoutB[p] = AudioBus{ st.aout[p].ch, st.aout[p].channels };

    // ---- 3. pre-MERGE MIDI fan-in; build MIDI buffers ------------------
    MidiBuffer minB[kMaxPortsPerNode];
    MidiBuffer moutB[kMaxPortsPerNode];
    for (int p = 0; p < st.numMidiIn; ++p) {
        MidiInPortPlan& mp = st.min[p];
        MidiEvent* buf = midiSlots_[(size_t)mp.slot].data();
        int cnt;
        if (mp.needsMerge) {
            int total = 0;
            for (int k = 0; k < mp.mergeCount; ++k) {
                const int srcSlot = plan->midiMergeSrc[mp.mergeFirst + k];
                const int scount  = midiSlotCount_[(size_t)srcSlot];
                const MidiEvent* se = midiSlots_[(size_t)srcSlot].data();
                for (int i = 0; i < scount && total < kNodeMidiCap; ++i)
                    buf[total++] = se[i];
            }
            // Stable insertion sort by sampleOffset (small n; alloc-free).
            for (int i = 1; i < total; ++i) {
                MidiEvent key = buf[i];
                int j = i - 1;
                while (j >= 0 && buf[j].sampleOffset > key.sampleOffset) {
                    buf[j + 1] = buf[j]; --j;
                }
                buf[j + 1] = key;
            }
            midiSlotCount_[(size_t)mp.slot] = total;
            cnt = total;
        } else {
            cnt = midiSlotCount_[(size_t)mp.slot];   // 0 for the empty slot
        }
        minB[p] = MidiBuffer{ buf, cnt, kNodeMidiCap };
    }
    for (int p = 0; p < st.numMidiOut; ++p)
        moutB[p] = MidiBuffer{ midiSlots_[(size_t)st.mout[p].slot].data(),
                               0, st.mout[p].cap };

    // ---- 4. run the node (or bypass passthrough) -----------------------
    NodeProcessContext nctx;
    nctx.nframes     = nframes;
    nctx.transport   = ctx;
    nctx.audioIn     = ainB;  nctx.numAudioIn  = st.numAudioIn;
    nctx.audioOut    = aoutB; nctx.numAudioOut = st.numAudioOut;
    nctx.midiIn      = minB;  nctx.numMidiIn   = st.numMidiIn;
    nctx.midiOut     = moutB; nctx.numMidiOut  = st.numMidiOut;
    nctx.paramIn     = nullptr; nctx.numParamIn = 0;

    // Guarded: node process() bodies are arbitrary (plugins, Pd, racks) and a
    // throw here would otherwise unwind into the noexcept audio callback
    // (std::terminate) or — on the parallel path — kill a worker thread. A
    // throwing node degrades to silence instead: its audio outs are zeroed and
    // its MIDI dropped, and downstream steps keep running on clean inputs.
    try {
        if (st.node->bypass()) {
            // Generic bypass: passthrough matching audio ports, zero the rest;
            // pass MIDI straight through matching ports.
            const int na = std::min(st.numAudioIn, st.numAudioOut);
            for (int p = 0; p < st.numAudioOut; ++p) {
                if (p < na) {
                    const int cc = std::min(ainB[p].channels, aoutB[p].channels);
                    for (int c = 0; c < cc; ++c)
                        std::memcpy(aoutB[p].chans[c], ainB[p].chans[c],
                                    sizeof(float) * (size_t)nframes);
                    for (int c = cc; c < aoutB[p].channels; ++c)
                        std::memset(aoutB[p].chans[c], 0,
                                    sizeof(float) * (size_t)nframes);
                } else {
                    for (int c = 0; c < aoutB[p].channels; ++c)
                        std::memset(aoutB[p].chans[c], 0,
                                    sizeof(float) * (size_t)nframes);
                }
            }
            const int nm = std::min(st.numMidiIn, st.numMidiOut);
            for (int p = 0; p < st.numMidiOut; ++p) {
                int c = 0;
                if (p < nm) {
                    c = std::min(minB[p].count, moutB[p].capacity);
                    for (int i = 0; i < c; ++i) moutB[p].ev[i] = minB[p].ev[i];
                }
                moutB[p].count = c;
            }
        } else {
            st.node->process(nctx);
        }
    } catch (...) {
        for (int p = 0; p < st.numAudioOut; ++p)
            for (int c = 0; c < st.aout[p].channels; ++c)
                if (st.aout[p].ch[c])
                    std::memset(st.aout[p].ch[c], 0,
                                sizeof(float) * (size_t)nframes);
        for (int p = 0; p < st.numMidiOut; ++p) moutB[p].count = 0;
    }

    // ---- 4b. virtual MIDI patchbay: apply the node's LIVE route table --
    // A VirtualMidiPortsNode is a pure endpoint bank: its process() emits
    // nothing, and its input->output matrix lives in an atomic table the UI
    // rewrites at any moment (setRoute). Resolving that table HERE — rather
    // than baking it into the plan at compile time — is what makes a re-route
    // audible on the very next block with no recompile and no hook for
    // audio_app/the UI to remember to call.
    //
    // Realtime: bounded and allocation-free. One acquire load per MIDI-out
    // port (route() is a plain std::atomic<int> read), then at most
    // kNodeMidiCap event copies per port. It runs AFTER process()/bypass and
    // overwrites their result: the route table is the node's entire function,
    // so the generic index-identity bypass passthrough would silently deliver
    // MIDI to the wrong endpoint.
    //
    // No feedback hazard, and no need for a route-level cycle check: a route
    // can only loop if some edge path leads from one of this node's OUTPUTS
    // back to one of its INPUTS, and that is a node-level cycle (or a
    // self-loop) — both already rejected by wouldCycle() at connect() time. By
    // the same argument the source slot can never be one of this node's own
    // out slots; the pointer test below is a belt-and-braces assertion of that
    // invariant, degrading to "unrouted" instead of self-copying.
    // ---- 5. publish this node's MIDI-out event counts to their slots ---
    for (int p = 0; p < st.numMidiOut; ++p) {
        int c = moutB[p].count;
        if (c < 0) c = 0;
        if (c > st.mout[p].cap) c = st.mout[p].cap;
        midiSlotCount_[(size_t)st.mout[p].slot] = c;
    }
}

// ===========================================================================
// Parallel dispatch — persistent worker pool + cooperative per-wave draining.
//
// The barrier: for each wave the audio thread stores levelRemaining_ = (steps in
// the wave) then arms the wave by growing levelEnd_ (release). Workers gate on
// levelEnd_ (acquire) before touching the wave, then everyone pulls indices off
// the monotonic workCursor_ via relaxed CAS. Each finished step does one
// fetch_sub(release) on levelRemaining_; the audio thread advances to the next
// wave only after it reads levelRemaining_ == 0 (acquire). That release/acquire
// pair publishes every upstream buffer written this wave to the consumers of the
// next wave. workCursor_/levelEnd_ never reset between blocks, so a descheduled
// straggler's final read can never fall inside a later block's window.
// ===========================================================================
void PatchGraph::drainLevel(RenderPlan* plan, int64_t base, int64_t end,
                            int nframes, const RenderContext& ctx) {
    for (;;) {
        int64_t cur = workCursor_.load(std::memory_order_relaxed);
        if (cur >= end) break;
        if (workCursor_.compare_exchange_weak(cur, cur + 1,
                                              std::memory_order_relaxed)) {
            // RAII scope guard: the barrier decrement MUST run even if the
            // step throws (runStep guards node code, but this is the invariant
            // that keeps the audio thread's levelRemaining_ spin from ever
            // hanging forever, so it must not depend on runStep's internals).
            struct WaveTicket {
                std::atomic<int>& remaining;
                ~WaveTicket() { remaining.fetch_sub(1, std::memory_order_release); }
            } ticket{ levelRemaining_ };
            runStep(plan, plan->stepsByLevel[(size_t)(cur - base)], nframes, ctx);
        }
    }
}

void PatchGraph::workerLoop() {
    // MXCSR is per-thread: every worker flushes denormals for itself, for the
    // thread's whole lifetime (the audio thread guards its own block in
    // process()).
    ScopedNoDenormals noDenormals;

    unsigned localGen = 0;
    for (;;) {
        // Sleep until the audio thread publishes a new block (or we're shutting
        // down). The generation predicate also absorbs an early wake if this
        // worker has not yet re-parked from the previous block.
        // The job snapshot is taken UNDER THE LOCK, together with the generation
        // it belongs to, so these five values always describe ONE block. Reading
        // them after unlocking was a race: a worker descheduled right there came
        // back with a mixture of two blocks' fields.
        RenderPlan*   plan = nullptr;
        int64_t       base = 0, bend = 0;
        int           nf   = 0;
        RenderContext ctx{};
        {
            std::unique_lock<std::mutex> lk(poolMutex_);
            poolCv_.wait(lk, [&] {
                return poolStop_.load(std::memory_order_relaxed) || blockGen_ != localGen;
            });
            if (poolStop_.load(std::memory_order_relaxed)) return;
            localGen = blockGen_;
            plan = jobPlan_;
            base = jobBase_;
            bend = jobEnd_;
            nf   = jobFrames_;
            ctx  = jobCtx_;
        }
        if (!plan) continue;                      // spurious wake with no job
        // The worker gates purely on levelEnd_ (armed wave by wave on the audio
        // thread) clipped to the job's own [base, end) window: it never reads
        // plan data outside an armed wave of ITS block, so a straggler that
        // wakes or resumes after the block finished only touches atomics — by
        // then the plan slot may legitimately be getting rewritten by a later
        // compile.

        for (;;) {
            const int64_t gate = levelEnd_.load(std::memory_order_acquire);
            const int64_t end  = gate < bend ? gate : bend;
            if (end > base) drainLevel(plan, base, end, nf, ctx);
            if (gate >= bend) break;   // last wave armed and drained: re-park
            if (poolStop_.load(std::memory_order_relaxed)) return;
        }
    }
}

void PatchGraph::startWorkerPool() {
    if (!workers_.empty()) return;                    // already running (idempotent)

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const int n = (int)std::min(hw, 8u) - 1;          // max(1, min(hw, 8)) - 1
    if (n < 1) return;                                // single core: no pool, stay sequential

    poolStop_.store(false, std::memory_order_relaxed);
    blockGen_ = 0;
    workCursor_.store(0, std::memory_order_relaxed);
    levelEnd_.store(0, std::memory_order_relaxed);
    levelRemaining_.store(0, std::memory_order_relaxed);
    jobPlan_ = nullptr;
    jobEnd_  = 0;

    workers_.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
        workers_.emplace_back([this] { workerLoop(); });
}

void PatchGraph::stopWorkerPool() {
    if (workers_.empty()) return;                     // nothing to stop
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        poolStop_.store(true, std::memory_order_relaxed);
        ++blockGen_;                                  // wake any parked worker
    }
    poolCv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    poolStop_.store(false, std::memory_order_relaxed); // ready for a later restart
}

}}} // namespace PatchKnob::engine::patch
