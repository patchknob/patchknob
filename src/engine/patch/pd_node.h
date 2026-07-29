//----------------------------------------------------------------------------
//  src/engine/patch/pd_node.h
//
//  PdNode -- a patch-graph node that hosts an embedded Pure Data instance via
//  libpd (built static, multi-instance).  It runs a .pd patch through the modular
//  graph: stereo audio in + MIDI in -> the patch's [adc~]/[notein]/[ctlin] ...
//  and the patch's [dac~] -> stereo audio out.  Each PdNode owns its own libpd
//  instance so several can coexist.  The SDL Pd editor edits the same patch.
//
//  Realtime: process() runs on the audio thread.  A graph containing a PdNode
//  is never scheduled on the parallel path (it has a MIDI port), so libpd's
//  per-thread "current instance" is only ever set sequentially -- safe.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_PATCH_PD_NODE_H
#define PATCHKNOB_ENGINE_PATCH_PD_NODE_H

#include <atomic>
#include <string>
#include <vector>

#include "patch_graph.h"

typedef struct _pdinstance t_pdinstance;   // opaque libpd instance handle

namespace PatchKnob { namespace engine { namespace patch {

class PdNode : public Node {
public:
    PdNode();
    ~PdNode() override;

    const char* typeName() const override { return "PdNode"; }

    // Ports auto-track the patch's adc~ / dac~ channels: one stereo OUT port per
    // pair of dac~ channels, one stereo IN port per pair of adc~ channels, plus a
    // MIDI in.  loadPatch() scans the .pd for the highest adc~/dac~ channel and
    // sizes the node accordingly (multichannel), laid out [out..][midi][in..].
    int outN() const { int n = (audioOutCh_ + 1) / 2; return n < 1 ? 1 : n; }
    int inN()  const { return audioInCh_ > 0 ? (audioInCh_ + 1) / 2 : 0; }
    int      numPorts() const override { return outN() + 1 + inN(); }
    PortDesc port(int i) const override {
        const int nOut = outN();
        if (i < nOut)  return PortDesc{ (PortId)i, PortKind::Audio, PortDir::Out, 2, ioName("out", i) };
        if (i == nOut) return PortDesc{ (PortId)i, PortKind::Midi,  PortDir::In,  1, "midi in" };
        return             PortDesc{ (PortId)i, PortKind::Audio, PortDir::In,  2, ioName("in", i - nOut - 1) };
    }
    static const char* ioName(const char* pfx, int i) {
        static std::vector<std::string> outs, ins;
        std::vector<std::string>& v = (pfx[0] == 'o') ? outs : ins;
        while ((int)v.size() <= i) v.push_back(std::string(pfx) + " " + std::to_string((int)v.size() + 1));
        return (i >= 0) ? v[(size_t)i].c_str() : pfx;
    }
    int audioInChannels()  const { return audioInCh_; }
    int audioOutChannels() const { return audioOutCh_; }
    bool prepare(double sampleRate, int maxBlock) override;
    void release() override;
    void process(const NodeProcessContext& ctx) override;

    // --- message thread -----------------------------------------------------
    //! Open a .pd patch (absolute path).  Replaces any current patch.
    bool loadPatch(const std::string& path);
    //! Save the current in-editor patch text to `path` and (re)load it.
    bool reloadFrom(const std::string& path) { return loadPatch(path); }
    const std::string& patchPath() const { return patchPath_; }
    t_pdinstance* instance() const { return pd_; }

    // MIDI channel filter so a clip can drive this Pd patch as an instrument.
    void setMidiChannel(int c) override { midiChannel_.store(c, std::memory_order_relaxed); }
    int  midiChannel() const override   { return midiChannel_.load(std::memory_order_relaxed); }

private:
    t_pdinstance*      pd_    = nullptr;
    void*              file_  = nullptr;   // libpd patch handle
    double             sr_    = 48000.0;
    int                block_ = 512;
    int                pdBlock_ = 64;
    int                audioInCh_  = 2;     // adc~ channels (>=2); node in-port width
    int                audioOutCh_ = 2;     // dac~ channels (>=2); node out-port width
    std::vector<float> in_, out_;          // interleaved libpd scratch
    std::string        patchPath_;
    std::atomic<int>   midiChannel_{-1};   // -1 == omni
};

}}} // namespace PatchKnob::engine::patch

#endif