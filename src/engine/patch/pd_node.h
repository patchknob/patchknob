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
#include <mutex>
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
    // Layout: [ audio out pairs ][ midi in ][ audio in pairs ][ midi out ].  The
    // MIDI ports let [notein]/[ctlin]/[midiin] receive and [noteout]/[ctlout]/
    // [midiout] send, so you can wire MIDI to and from the Pd patch.
    int      numPorts() const override { return outN() + 1 + inN() + 1; }   // +1 midi out
    PortDesc port(int i) const override {
        const int nOut = outN();
        if (i < nOut)  return PortDesc{ (PortId)i, PortKind::Audio, PortDir::Out, 2, ioName("out", i) };
        if (i == nOut) return PortDesc{ (PortId)i, PortKind::Midi,  PortDir::In,  1, "midi in" };
        const int inBase = nOut + 1;
        if (i < inBase + inN())
            return PortDesc{ (PortId)i, PortKind::Audio, PortDir::In, 2, ioName("in", i - inBase) };
        return PortDesc{ (PortId)i, PortKind::Midi, PortDir::Out, 1, "midi out" };   // last port
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
    // The patch is stored as TEXT on the node (the source of truth) and saved with
    // the PROJECT -- no .pd file lives on disk.  libpd still needs a file to open,
    // so loadPatchText() writes a TRANSIENT temp file (in the OS temp dir), opens it,
    // then deletes it; the canonical patch stays in memory.
    //! Load the patch from in-memory .pd text.  Replaces any current patch.
    bool loadPatchText(const std::string& text);
    const std::string& patchText() const { return patchText_; }
    //! Store new text WITHOUT reloading libpd (persist a GUI value edit; the live
    //! value was already delivered via sendFloat/sendBang).
    void storePatchText(const std::string& text) { patchText_ = text; }
    //! Import an external .pd file (reads it into memory, then loadPatchText()).
    bool loadPatch(const std::string& path);
    const std::string& patchPath() const { return patchPath_; }
    t_pdinstance* instance() const { return pd_; }

    //! Enable / disable this instance's audio DSP ("compute audio", Pd's DSP switch).
    void setDsp(bool on) { dspOn_.store(on ? 1 : 0, std::memory_order_relaxed); }
    bool dsp() const     { return dspOn_.load(std::memory_order_relaxed) != 0; }

    // MIDI channel filter so a clip can drive this Pd patch as an instrument.
    void setMidiChannel(int c) override { midiChannel_.store(c, std::memory_order_relaxed); }
    int  midiChannel() const override   { return midiChannel_.load(std::memory_order_relaxed); }

    // --- live control from the UI thread ------------------------------------
    // The SDL Pd editor drives GUI atoms (toggles / sliders / bangs) live: it sends
    // the value to the atom's RECEIVE symbol so the running patch reacts immediately
    // (e.g. a toggle starts a [metro]) -- no file reload.  Enqueued here and drained
    // on the audio thread inside process(), so libpd is only ever touched there.
    void sendFloat(const std::string& recv, float v);
    void sendBang (const std::string& recv);

    // --- live GUI feedback (the reverse of send*) ---------------------------
    // Bind a GUI atom's SEND symbol so messages that REACH it in the running patch
    // (a bang lighting up, a toggle/slider moving because a wire drove it) come back
    // to the editor.  libpd's hooks fire on the audio thread inside process(); the
    // updates are queued and drainGui()'d by the UI thread.
    struct GuiFb { std::string recv; float val = 0.f; bool bang = false; };
    void subscribeGui(const std::string& sendSym);        // libpd_bind (message thread)
    void clearGuiBinds();                                 // unbind all GUI subscriptions
    int  drainGui(GuiFb* out, int cap);                   // pop queued feedback (UI thread)
    void pushFb(const char* recv, float v, bool bang);    // called by the libpd hooks

    // --- MIDI out: the patch's [noteout]/[ctlout]/[midiout] -> the node's MIDI-out
    // port.  libpd's MIDI hooks (audio thread) stage events; process() flushes them.
    void pushMidiOut(unsigned char status, unsigned char d1, unsigned char d2);
    void pushMidiByte(int byte);                          // raw byte from [midiout]

private:
    struct PdMsg { std::string recv; float val = 0.f; bool bang = false; };
    std::mutex          msgMx_;
    std::vector<PdMsg>  msgQ_;
    std::mutex          fbMx_;
    std::vector<GuiFb>  fbQ_;
    std::vector<void*>  binds_;                           // libpd_bind handles (for unbind)
    std::mutex          midiMx_;
    std::vector<MidiEvent> midiOutStage_;                 // MIDI produced this block
    unsigned char       rawStatus_ = 0;                   // running-status parse for [midiout]
    unsigned char       rawData_[2] = { 0, 0 };
    int                 rawNeed_ = 0, rawGot_ = 0;
    t_pdinstance*      pd_    = nullptr;
    void*              file_  = nullptr;   // libpd patch handle
    double             sr_    = 48000.0;
    int                block_ = 512;
    int                pdBlock_ = 64;
    int                audioInCh_  = 2;     // adc~ channels (>=2); node in-port width
    int                audioOutCh_ = 2;     // dac~ channels (>=2); node out-port width
    std::vector<float> in_, out_;          // interleaved libpd scratch
    std::string        patchPath_;         // import/export path (informational only)
    std::string        patchText_;         // the patch source-of-truth (saved w/ project)
    std::atomic<int>   midiChannel_{-1};   // -1 == omni
    std::atomic<int>   dspOn_{1};          // DSP switch (UI thread sets, audio applies)
    int                dspApplied_ = -1;   // audio thread: last-applied DSP state
};

}}} // namespace PatchKnob::engine::patch

#endif