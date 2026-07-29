//----------------------------------------------------------------------------
//  src/engine/rack/rack_node.h
//
//  RackNode -- a patch-graph node hosting a RackEngine (a small VCV-Rack-style
//  modular patch).  Same outward shape as PdNode: stereo audio in + MIDI in ->
//  the rack's AudioIn / MIDI-CV modules, and the rack's AudioOut module -> the
//  node's stereo out.  The SDL rack editor drives the SAME engine() live.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_NODE_H
#define PATCHKNOB_ENGINE_RACK_NODE_H

#include <atomic>
#include <string>
#include <vector>

#include "../patch/patch_graph.h"
#include "rack_engine.h"

namespace PatchKnob { namespace engine { namespace patch {

class RackNode : public Node {
public:
    RackNode() = default;
    ~RackNode() override = default;

    const char* typeName() const override { return "RackNode"; }

    // Ports auto-track the rack's audio modules (like the Csound node): one stereo
    // OUT port per AudioOut module, one stereo IN port per AudioIn module, plus a
    // MIDI in.  Load/remove Audio-In/Out modules in the rack editor and the node's
    // patch ports grow/shrink to match (the graph recompiles after the edit):
    //   [ out 0 .. N-1 ] [ midi in ] [ in 0 .. M-1 ]
    int outN() const { int n = engine_.audioOutCount(); return n < 1 ? 1 : n; }
    int inN()  const { return engine_.audioInCount(); }
    int      numPorts() const override { return outN() + 1 + inN(); }
    PortDesc port(int i) const override {
        const int nOut = outN();
        if (i < nOut)  return PortDesc{ (PortId)i, PortKind::Audio, PortDir::Out, 2, ioName("out", i) };
        if (i == nOut) return PortDesc{ (PortId)i, PortKind::Midi,  PortDir::In,  1, "midi in" };
        return             PortDesc{ (PortId)i, PortKind::Audio, PortDir::In,  2, ioName("in", i - nOut - 1) };
    }
    // Numbered, persistent port labels ("out 1", "out 2", "in 1", ...) so the
    // patcher shows the same channel number as the rack's Audio-In/Out modules.
    static const char* ioName(const char* pfx, int i) {
        static std::vector<std::string> outs, ins;
        std::vector<std::string>& v = (pfx[0] == 'o') ? outs : ins;
        while ((int)v.size() <= i) v.push_back(std::string(pfx) + " " + std::to_string((int)v.size() + 1));
        return (i >= 0) ? v[(size_t)i].c_str() : pfx;
    }

    bool prepare(double sampleRate, int maxBlock) override;
    void release() override {}
    void process(const NodeProcessContext& ctx) override;

    //! The live modular patch (shared with the SDL editor, UI thread).
    rackx::RackEngine* engine() { return &engine_; }

    // MIDI channel filter so a clip can drive this modular patch as an instrument.
    void setMidiChannel(int c) override { midiChannel_.store(c, std::memory_order_relaxed); }
    int  midiChannel() const override   { return midiChannel_.load(std::memory_order_relaxed); }

private:
    rackx::RackEngine engine_;
    std::atomic<int>  midiChannel_{-1};     // -1 == omni
    MidiEvent         midiFilterScratch_[kNodeMidiCap];
};

}}} // namespace PatchKnob::engine::patch

#endif
