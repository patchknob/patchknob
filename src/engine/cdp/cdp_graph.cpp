//----------------------------------------------------------------------------
//  src/engine/cdp/cdp_graph.cpp -- see cdp_graph.h.
//----------------------------------------------------------------------------
#include "cdp_graph.h"

#include <algorithm>

namespace PatchKnob { namespace cdp {

NodeId Graph::addSource(Buffer clip, std::string label) {
    Node n;
    n.id = next_++;
    n.label = label.empty() ? std::string("Source") : std::move(label);
    n.source = std::move(clip);
    nodes_.push_back(std::move(n));
    if (output_ == 0) output_ = nodes_.back().id;
    return nodes_.back().id;
}

NodeId Graph::addProcess(const std::string& slug, std::string label) {
    const Process* p = find(slug);
    if (!p) return 0;
    Node n;
    n.id = next_++;
    n.slug = slug;
    n.label = label.empty() ? p->name : std::move(label);
    n.params.reserve(p->params.size());
    for (const ParamSpec& s : p->params) n.params.push_back(s.def);
    // Room for the waveform strip plus one row per parameter.  Per-node, not a
    // fixed rack height.
    n.height = 74.0 + 18.0 * (double)p->params.size();
    nodes_.push_back(std::move(n));
    output_ = nodes_.back().id;
    return nodes_.back().id;
}

void Graph::removeNode(NodeId id) {
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
        [id](const Edge& e) { return e.from == id || e.to == id; }), edges_.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
        [id](const Node& n) { return n.id == id; }), nodes_.end());
    if (output_ == id) output_ = nodes_.empty() ? 0 : nodes_.back().id;
}

bool Graph::connect(NodeId from, NodeId to, int toInput) {
    if (from == to || !node(from) || !node(to) || toInput < 0) return false;
    const Node* dst = node(to);
    const Process* p = dst->slug.empty() ? nullptr : find(dst->slug);
    if (!p || toInput >= p->maxInputs) return false;   // sources take no input
    // Rejecting only self-edges let LONGER cycles in (A->B then B->A), and a
    // cycle is not merely unrenderable -- the editor's downstream-invalidation
    // walk recursed on it and blew the stack the moment the closing cable
    // landed.  Refuse any edge whose destination can already reach its source.
    if (reaches(to, from)) return false;
    disconnect(to, toInput);                            // one feed per slot
    edges_.push_back(Edge{ from, to, toInput });
    return true;
}

// Is `target` downstream of `start` (following edges from->to)?  Iterative and
// visited-guarded, so it terminates even on a graph that already has a cycle.
bool Graph::reaches(NodeId start, NodeId target) const {
    if (start == target) return true;
    std::vector<NodeId> stack{ start };
    std::vector<NodeId> seen{ start };
    while (!stack.empty()) {
        const NodeId cur = stack.back(); stack.pop_back();
        for (const Edge& e : edges_) {
            if (e.from != cur) continue;
            if (e.to == target) return true;
            if (std::find(seen.begin(), seen.end(), e.to) != seen.end()) continue;
            seen.push_back(e.to);
            stack.push_back(e.to);
        }
    }
    return false;
}

void Graph::disconnect(NodeId to, int toInput) {
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
        [&](const Edge& e) { return e.to == to && e.toInput == toInput; }),
        edges_.end());
}

Node* Graph::node(NodeId id) {
    for (Node& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}
const Node* Graph::node(NodeId id) const {
    for (const Node& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}

bool Graph::renderNode(NodeId id, std::map<NodeId, Buffer>& done,
                       std::vector<NodeId>& visiting, Buffer& out,
                       std::string& error, const Progress& progress) const {
    auto cached = done.find(id);
    if (cached != done.end()) { out = cached->second; return true; }
    // A patch cable looped back on itself would otherwise recurse forever.
    if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) {
        error = "CDP patch has a feedback loop"; return false;
    }
    const Node* n = node(id);
    if (!n) { error = "missing node"; return false; }

    if (n->slug.empty()) {                       // source: emit the clip
        done[id] = n->source;
        out = n->source;
        return true;
    }
    const Process* p = find(n->slug);
    if (!p) { error = "unknown process: " + n->slug; return false; }

    visiting.push_back(id);
    std::vector<Buffer> inputs;
    for (int slot = 0; slot < p->maxInputs; ++slot) {
        const Edge* feed = nullptr;
        for (const Edge& e : edges_)
            if (e.to == id && e.toInput == slot) { feed = &e; break; }
        if (!feed) break;                         // inputs are filled in order
        Buffer b;
        if (!renderNode(feed->from, done, visiting, b, error, progress)) {
            visiting.pop_back(); return false;
        }
        inputs.push_back(std::move(b));
    }
    visiting.pop_back();

    if ((int)inputs.size() < p->minInputs) {
        error = n->label + " needs " + std::to_string(p->minInputs) + " input(s)";
        return false;
    }
    Buffer result;
    if (!p->run(inputs, n->params, result, error, progress)) return false;
    done[id] = result;
    out = std::move(result);
    return true;
}

bool Graph::render(Buffer& out, std::string& error, const Progress& progress,
                   NodeId id) const {
    const NodeId target = id >= 0 ? id : output_;
    if (target == 0) { error = "nothing to render"; return false; }
    std::map<NodeId, Buffer> done;
    std::vector<NodeId> visiting;
    return renderNode(target, done, visiting, out, error, progress);
}

int64_t Graph::latencyFrames(int sampleRate, NodeId id) const {
    const NodeId target = id >= 0 ? id : output_;
    const Node* n = node(target);
    if (!n) return 0;
    int64_t own = 0;
    if (!n->slug.empty()) {
        const Process* p = find(n->slug);
        if (p && p->latencyFrames) own = p->latencyFrames(sampleRate, n->params);
    }
    // Serial chain: the delays add.  A fan-in takes the WORST branch, since the
    // host has to line the inputs up against the slowest one.
    int64_t worst = 0;
    for (const Edge& e : edges_)
        if (e.to == target)
            worst = std::max(worst, latencyFrames(sampleRate, e.from));
    return own + worst;
}

bool Graph::canStream(NodeId id) const {
    const NodeId target = id >= 0 ? id : output_;
    const Node* n = node(target);
    if (!n) return false;
    if (!n->slug.empty()) {
        const Process* p = find(n->slug);
        if (!p || !p->streamable) return false;
    }
    for (const Edge& e : edges_)
        if (e.to == target && !canStream(e.from)) return false;
    return true;
}

} } // namespace PatchKnob::cdp
