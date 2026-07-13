//----------------------------------------------------------------------------
//  seq24 Windows port — PatchGraph implementation. See patch_graph.h for the
//  design and the RCU (atomic snapshot) publication model, which mirrors
//  Track::publishChain in ../graph/track.cpp.
//----------------------------------------------------------------------------
#include "patch_graph.h"

#include <algorithm>
#include <cstring>

namespace seq24 { namespace engine { namespace patch {

namespace {
// Combine (node,port) into a stable 64-bit key for the compile-time out maps.
inline uint64_t outKey(NodeId n, PortId p) {
    return (uint64_t(n) << 16) | uint64_t(p);
}
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

    // Defer destruction: the audio thread may still be walking a plan that
    // references this node. Move it to gc_ and free it in collectGarbage().
    gc_.push_back(std::move(it->second));
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

void PatchGraph::disconnectAll(NodeId id) {
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                    [id](const Connection& e) {
                        return e.from.node == id || e.to.node == id;
                    }),
                 conns_.end());
}

// ===========================================================================
// Cycle detection / topological sort (Kahn)
// ===========================================================================
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
        if (e.from.node != e.to.node) addEdge(e.from.node, e.to.node);
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

    planStore_[0].reset(new RenderPlan());
    planStore_[1].reset(new RenderPlan());
    planStore_[0]->nSteps = 0;
    planStore_[1]->nSteps = 0;
    activeSlot_.store(0, std::memory_order_relaxed);
    livePlan_.store(nullptr, std::memory_order_release);

    bool ok = true;
    for (auto& kv : nodes_) {
        ok = kv.second->prepare(sampleRate, maxBlock) && ok;
        kv.second->setActive(true);
    }

    prepared_ = true;
    compileAndPublish();
    return ok;
}

void PatchGraph::release() {
    for (auto& kv : nodes_) kv.second->setActive(false);
    prepared_ = false;
}

void PatchGraph::collectGarbage() {
    // Safe to free once no audio thread is walking an old plan. The host calls
    // this on the message thread after at least one block has certainly passed.
    for (auto& n : gc_) if (n) n->release();
    gc_.clear();
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

    for (NodeId nid : order) {
        Node* n = nodes_.at(nid).get();
        Step& st = plan->steps[plan->nSteps];
        st.node = n;
        st.numAudioIn = st.numAudioOut = st.numMidiIn = st.numMidiOut = 0;

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

        if (nid == deviceOut_) plan->deviceOutStep = plan->nSteps;
        plan->nSteps++;
    }
    return true;
}

bool PatchGraph::compileAndPublish() {
    lastCompileOk_ = false;
    if (!prepared_) return false;

    const int cur  = activeSlot_.load(std::memory_order_relaxed);
    const int next = cur ^ 1;
    RenderPlan* p = planStore_[next].get();

    if (!buildPlan(p)) return false;   // cap exceeded / cycle: keep old live plan

    // Refresh device-node pointer cache before publishing the plan.
    deviceOutNode_.store(node(deviceOut_), std::memory_order_release);
    deviceInNode_.store(node(deviceIn_),   std::memory_order_release);

    // Publish: release-store the pointer, then record the new active slot.
    livePlan_.store(p, std::memory_order_release);
    activeSlot_.store(next, std::memory_order_relaxed);
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
    if (nframes > maxBlock_) nframes = maxBlock_;   // clamp; never overrun pools

    // Start from silence so the device-out node only needs to write what it has,
    // and so an unconnected/absent sink yields clean silence.
    for (int c = 0; c < numChannels; ++c)
        if (out[c]) std::memset(out[c], 0, sizeof(float) * (size_t)nframes);

    RenderPlan* plan = livePlan_.load(std::memory_order_acquire);
    if (!plan) return;

    // Reset per-slot MIDI counts for this block (bounded; audio-thread private).
    std::fill(midiSlotCount_.begin(), midiSlotCount_.end(), 0);

    // Bind device nodes (cheap pointer set).
    if (Node* dout = deviceOutNode_.load(std::memory_order_acquire))
        dout->bindDeviceOut(out, numChannels, nframes);
    if (Node* din = deviceInNode_.load(std::memory_order_acquire))
        din->bindDeviceIn(stagedIn_, stagedInCh_, nframes);

    for (int s = 0; s < plan->nSteps; ++s) {
        Step& st = plan->steps[s];

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

        // ---- 5. publish this node's MIDI-out event counts to their slots ---
        for (int p = 0; p < st.numMidiOut; ++p) {
            int c = moutB[p].count;
            if (c < 0) c = 0;
            if (c > st.mout[p].cap) c = st.mout[p].cap;
            midiSlotCount_[(size_t)st.mout[p].slot] = c;
        }
    }
}

}}} // namespace seq24::engine::patch
