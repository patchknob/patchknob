//----------------------------------------------------------------------------
//  src/engine/cdp/cdp_graph.h
//
//  The offline CDP editor is a PATCH, not a single effect slot: sources (clips
//  dragged in from the timeline), process nodes chained together, and one
//  output that renders back to a track.  This is that graph, headless -- the
//  editor draws it and the evaluator below renders it, so the model can be
//  tested without any UI.
//
//  It is deliberately NOT the realtime patch graph (src/engine/patch): that one
//  is built for lock-free block processing with fixed-size ports.  These
//  processes are whole-buffer transforms, several of which cannot stream at
//  all, so the evaluation model is a pull over complete buffers with memoised
//  node results.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_CDP_GRAPH_H
#define PATCHKNOB_CDP_GRAPH_H

#include "cdp_process.h"

#include <map>
#include <string>
#include <vector>

namespace PatchKnob { namespace cdp {

using NodeId = int;

struct Node {
    NodeId      id = 0;
    //! Empty slug == a SOURCE node: it emits `source` rather than running a
    //! process.  That is how a clip dragged in from the timeline appears.
    std::string slug;
    std::string label;
    Buffer      source;             //!< source nodes only
    std::vector<double> params;     //!< sized/defaulted from the Process spec

    //! Editor geometry.  Height is per-node -- a CDP node carries a waveform
    //! view and as many parameter rows as the process declares, so unlike the
    //! rack these are not on a fixed-height grid.
    double x = 0.0, y = 0.0;
    double width = 220.0, height = 120.0;
    bool   showWave = true;
};

//! `to` node's input slot `toInput` is fed by `from` node's single output.
struct Edge {
    NodeId from = 0;
    NodeId to = 0;
    int    toInput = 0;
};

class Graph {
public:
    NodeId addSource(Buffer clip, std::string label);
    NodeId addProcess(const std::string& slug, std::string label = {});
    void   removeNode(NodeId id);
    bool   connect(NodeId from, NodeId to, int toInput);
    //! True when `target` is reachable downstream of `start`.  connect() uses
    //! it to refuse cycles; the editor uses it to grey out illegal cables.
    bool   reaches(NodeId start, NodeId target) const;
    void   disconnect(NodeId to, int toInput);

    Node*       node(NodeId id);
    const Node* node(NodeId id) const;
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }

    //! The node whose result "render to track" takes.  Defaults to the last
    //! process added; the editor can retarget it.
    NodeId output() const { return output_; }
    void   setOutput(NodeId id) { output_ = id; }

    //! Render `id` (default: the output node).  Evaluates its dependencies
    //! depth-first, memoising each node so a fan-out is not recomputed.
    //! Returns false with `error` set on a cycle, a missing input, or a process
    //! failure.
    bool render(Buffer& out, std::string& error,
                const Progress& progress = {}, NodeId id = -1) const;

    //! Total inherent latency along the path feeding `id`, in frames -- the sum
    //! of each streamable process's own delay (a PVOC window, a waveset
    //! lookahead).  What the host must compensate when previewing live.
    int64_t latencyFrames(int sampleRate, NodeId id = -1) const;

    //! False when any process on the path cannot stream, i.e. live preview is
    //! impossible and only offline render applies.
    bool canStream(NodeId id = -1) const;

private:
    bool renderNode(NodeId id, std::map<NodeId, Buffer>& done,
                    std::vector<NodeId>& visiting, Buffer& out,
                    std::string& error, const Progress& progress) const;

    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    NodeId next_ = 1;
    NodeId output_ = 0;
};

} } // namespace PatchKnob::cdp

#endif
