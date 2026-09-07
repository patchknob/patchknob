//----------------------------------------------------------------------------
//  src/engine/patch/csound_node.h
//
//  CsoundNode -- a patch-graph node that hosts an embedded Csound engine and
//  runs a .csd (Csound document) through the modular graph:
//
//      audio in (nchnls_i) + MIDI in  ->  Csound orchestra  ->  audio out (nchnls)
//
//  The CSD is held as TEXT and (re)compiled on demand (the SDL editor edits it and
//  Ctrl+E recompiles).  On every successful compile the node re-reads the CSD's
//  declared channel counts (nchnls / nchnls_i) and exposes them as its audio
//  in/out bus widths -- so the patcher GUI's ports auto-update to match the CSD.
//
//  Realtime: process() runs on the audio thread and bridges Csound's ksmps blocks
//  to the engine block.  compile() runs on the message thread; it builds a fresh
//  Csound instance off-lock and swaps it in under a short lock, so the audio thread
//  never sees a half-compiled engine.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_PATCH_CSOUND_NODE_H
#define PATCHKNOB_ENGINE_PATCH_CSOUND_NODE_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "patch_graph.h"

typedef struct CSOUND_ CSOUND;   // opaque Csound engine handle (see csound.h)

namespace PatchKnob { namespace engine { namespace patch {

class CsoundNode : public Node {
public:
    CsoundNode();
    ~CsoundNode() override;

    const char* typeName() const override { return "CsoundNode"; }

    // Ports auto-track the compiled CSD.  Csound channels are exposed as STEREO
    // pairs (the last is mono when the count is odd) so a multichannel CSD becomes
    // several wireable out/in ports -- no engine-wide bus-width change needed:
    //   [ out pair 0 .. N-1 ] [ midi in ] [ in pair 0 .. M-1 ] [ midi out ]
    // where N = ceil(nchnls/2) and M = ceil(nchnls_i/2) (0 when there's no input).
    //
    // "midi out" carries whatever the orchestra emits with the `midiout` opcode
    // (midiout kstatus, kchan, kdata1, kdata2) and friends.  It is APPENDED at
    // the end deliberately: inserting it next to "midi in" would renumber every
    // audio-in port and silently re-wire existing saved patches.
    int outPairs() const { int n = (nchnls_   + 1) / 2; return n < 1 ? 1 : n; }
    int inPairs()  const { return nchnlsIn_ > 0 ? (nchnlsIn_ + 1) / 2 : 0; }
    int      numPorts() const override { return outPairs() + 1 + inPairs() + 1; }
    PortDesc port(int i) const override {
        const int nOut = outPairs();
        if (i < nOut) {                                   // audio-out stereo pair i
            int w = nchnls_ - i * 2; w = w > 2 ? 2 : (w < 1 ? 1 : w);
            return PortDesc{ (PortId)i, PortKind::Audio, PortDir::Out, (uint16_t)w, "out" };
        }
        if (i == nOut) return PortDesc{ (PortId)i, PortKind::Midi, PortDir::In, 1, "midi in" };
        const int pi = i - nOut - 1;                      // audio-in stereo pair
        if (pi >= inPairs())                              // trailing MIDI out
            return PortDesc{ (PortId)i, PortKind::Midi, PortDir::Out, 1, "midi out" };
        int w = nchnlsIn_ - pi * 2; w = w > 2 ? 2 : (w < 1 ? 1 : w);
        return PortDesc{ (PortId)i, PortKind::Audio, PortDir::In, (uint16_t)w, "in" };
    }

    bool prepare(double sampleRate, int maxBlock) override;
    void release() override;
    void process(const NodeProcessContext& ctx) override;

    // --- message thread -----------------------------------------------------
    //! (Re)compile a CSD document from text.  Rebuilds the Csound instance, updates
    //! nchnls_/nchnlsIn_ (so numPorts()/port() change -> recompile the graph after),
    //! and swaps the live engine atomically.  Returns true on success; lastError()
    //! holds the message on failure.
    bool compile(const std::string& csdText);
    //! Re-run compile() on the currently stored CSD text (Ctrl+E).
    bool recompile() { return compile(csd_); }

    const std::string& csdText() const { return csd_; }
    void setCsdText(const std::string& t) { csd_ = t; }   // editor buffer (no compile)
    const std::string& lastError() const { return err_; }
    bool  compiled() const { return cs_.load(std::memory_order_acquire) != nullptr; }
    int   nchnls() const   { return nchnls_; }
    int   nchnlsInput() const { return nchnlsIn_; }

    // MIDI channel filter so a clip can drive this Csound patch as an instrument.
    void setMidiChannel(int c) override { midiChannel_.store(c, std::memory_order_relaxed); }
    int  midiChannel() const override   { return midiChannel_.load(std::memory_order_relaxed); }

    //! Called by Csound's host MIDI read callback to pull the block's staged bytes.
    int drainMidi(unsigned char* buf, int nBytes);
    //! Called by Csound's host MIDI WRITE callback with bytes the orchestra emitted
    //! (`midiout` et al).  Runs on the audio thread inside csoundPerformKsmps(),
    //! which process() drives, so it shares process()'s single-threaded context.
    int writeMidi(const unsigned char* buf, int nBytes);

private:
    void destroyInstance(CSOUND* cs);

    std::atomic<CSOUND*>  cs_{nullptr};    // live compiled engine (audio thread reads)
    std::mutex            mutex_;          // guards process() vs the compile swap
    double                sr_    = 48000.0;
    int                   block_ = 512;
    int                   nchnls_   = 2;   // CSD output channels (message-thread view)
    int                   nchnlsIn_ = 0;   // CSD input channels
    int                   ksmps_    = 32;
    // ksmps->block bridge: leftover computed output frames not yet emitted.
    std::vector<float>    leftover_;       // interleaved by nchnls_ (ksmps carry)
    std::vector<float>    blockOut_;       // interleaved by nchnls_ (this whole block)
    int                   leftoverCount_ = 0;
    int                   leftoverPos_   = 0;
    bool                  finished_ = false;   // CSD score ended -> emit silence
    std::string           csd_;            // editor source-of-truth
    std::string           err_;
    std::atomic<int>      midiChannel_{-1};
    // host-fed MIDI: bytes staged each block, drained by Csound's read callback.
    std::vector<unsigned char> midiBytes_;
    int                        midiPos_ = 0;
    // Csound-emitted MIDI (`midiout`): bytes collected during the block's
    // csoundPerformKsmps() calls, parsed onto the "midi out" port at block end.
    // A parallel per-byte sample offset keeps ksmps-level timing instead of
    // flattening everything to the block edge.
    std::vector<unsigned char> midiOutBytes_;
    std::vector<int>           midiOutAt_;      // sample offset per byte
    int                        midiOutCursor_ = 0;   // frames produced so far
};

}}} // namespace PatchKnob::engine::patch

#endif // PATCHKNOB_ENGINE_PATCH_CSOUND_NODE_H
