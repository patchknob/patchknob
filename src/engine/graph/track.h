//----------------------------------------------------------------------------
//  PatchKnob — per-track audio graph node.
//
//  Signal flow for one Track:
//
//      MIDI ─┐
//            ▼
//      [ instrument ] ─► [ FX1 ] ─► [ FX2 ] ─► ... ─► [ gain/pan ] ─► [ VU ] ─► out
//        (synth, may       (insert effects, in series)
//         be null)
//
//  All audio is NON-INTERLEAVED stereo (float**), matching plugin_api.h. The
//  Track only ever talks to IPluginInstance — never to a concrete VST class.
//
//  Realtime: processBlock() is audio-thread-only, lock-free, allocation-free.
//  All scratch buffers are sized once in prepare().
//
//  ------------------------------------------------------------------------
//  FX-CHAIN HOT-SWAP APPROACH (RCU / atomic snapshot pointer)
//  ------------------------------------------------------------------------
//  The audio thread must always see a *consistent* FX chain even while the
//  message thread is adding / removing / reordering effects. We use a
//  read-copy-update scheme:
//
//    * The live chain is an immutable `FxChainSnapshot` (a fixed-capacity
//      array of IPluginInstance* + a count) referenced by a single
//      std::atomic<FxChainSnapshot*> `liveChain_`.
//    * The audio thread does ONE acquire-load of that pointer at the top of
//      processBlock() and uses that snapshot for the whole block. It never
//      sees a half-edited chain.
//    * The message thread keeps the *editable* list in `fxEdit_`. On any
//      mutation it builds a brand-new snapshot into the currently-unused slot
//      of a double-buffer (`snapStore_[2]`), then atomically publishes it with
//      a release-store. The old snapshot is simply the other buffer; because
//      we only ever have the audio thread reading one snapshot at a time and
//      the message thread is single-threaded, the inactive buffer is free to
//      reuse on the next edit.
//
//  This needs no locks on the audio thread and no allocation at swap time
//  (the two snapshot buffers are pre-allocated in the Track). Plugin objects
//  themselves are owned elsewhere and must outlive any swap that drops them
//  (standard host responsibility — defer destruction until after a block has
//  passed; not the Track's concern).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_GRAPH_TRACK_H
#define PATCHKNOB_ENGINE_GRAPH_TRACK_H

#include <atomic>
#include <vector>
#include <cstdint>

#include "../plugin_api.h"
#include "vu_meter.h"

namespace PatchKnob { namespace engine {

//! Extra transport context handed down to each block (mirrors ProcessBlock).
struct RenderContext {
    double  tempoBpm            = 120.0;
    int64_t playPositionSamples = 0;
    bool    isPlaying           = false;
};

class Track {
public:
    //! Maximum number of insert FX in a single track's chain.
    static constexpr int kMaxFx = 32;

    Track();
    ~Track();

    Track(const Track&)            = delete;
    Track& operator=(const Track&) = delete;

    // --- configuration (message thread) --------------------------------------

    //! Set the instrument (synth). May be null (audio-only / bus track).
    //! Ownership stays with the caller (the host).
    void setInstrument(IPluginInstance* inst) { instrument_ = inst; }
    IPluginInstance* instrument() const { return instrument_; }

    //! Add an FX to the end of the insert chain. Returns false if full.
    //! Message thread only. Publishes a new snapshot atomically.
    bool addFx(IPluginInstance* fx);

    //! Remove the FX at `index` from the chain. Returns false if out of range.
    bool removeFx(int index);

    //! Move the FX at `from` to position `to` (reorder). Returns false on bad
    //! indices. Message thread only.
    bool moveFx(int from, int to);

    //! Current number of FX in the (editable) chain.
    int fxCount() const { return (int)fxEdit_.size(); }

    //! FX at editable index (message thread). Null if out of range.
    IPluginInstance* fxAt(int index) const {
        return (index >= 0 && index < (int)fxEdit_.size()) ? fxEdit_[index] : nullptr;
    }

    // --- mix controls (any thread; atomics) ----------------------------------

    void  setGain(float g)  { gain_.store(g, std::memory_order_relaxed); }
    float gain() const      { return gain_.load(std::memory_order_relaxed); }

    void  setPan(float p)   { pan_.store(p,  std::memory_order_relaxed); }   // -1..1
    float pan() const       { return pan_.load(std::memory_order_relaxed); }

    void setMute(bool m)    { mute_.store(m, std::memory_order_relaxed); }
    bool mute() const       { return mute_.load(std::memory_order_relaxed); }

    void setSolo(bool s)    { solo_.store(s, std::memory_order_relaxed); }
    bool solo() const       { return solo_.load(std::memory_order_relaxed); }

    //! Disable the whole signal chain: processBlock outputs silence and runs
    //! NO instrument/FX (frees CPU).  Used by track-freeze to idle the source
    //! track's devices while its frozen audio plays elsewhere.
    void setDisabled(bool d) { disabled_.store(d, std::memory_order_relaxed); }
    bool disabled() const    { return disabled_.load(std::memory_order_relaxed); }

    // --- metering (UI thread reads) ------------------------------------------

    VuMeter& vuLeft()  { return vuL_; }
    VuMeter& vuRight() { return vuR_; }
    const VuMeter& vuLeft()  const { return vuL_; }
    const VuMeter& vuRight() const { return vuR_; }

    // --- lifecycle -----------------------------------------------------------

    //! Prepare instrument + all FX and size scratch buffers. Message thread.
    //! Returns false if any sub-plugin prepare fails (others still prepared).
    bool prepare(double sampleRate, int maxBlockSize);

    //! Suspend all plugins (message thread).
    void release();

    // --- realtime ------------------------------------------------------------

    //! Render one block into the track's stereo output buffers `out[0]`/`out[1]`.
    //! Runs instrument (fed MIDI + automation), then the FX chain in series,
    //! applies gain/pan, updates the stereo VU, and writes the result to `out`.
    //!
    //! `out` must be 2 planar buffers of at least `nframes` floats. The caller
    //! (MixerGraph) owns them. Audio thread only; lock-free, no allocation.
    void processBlock(const MidiEvent* midi, int nMidi,
                      const ParamChange* autom, int nAutom,
                      float** out, int nframes,
                      const RenderContext& ctx);

private:
    //! Immutable FX chain snapshot read by the audio thread (RCU).
    struct FxChainSnapshot {
        IPluginInstance* fx[kMaxFx];
        int              count = 0;
    };

    //! Build a fresh snapshot from fxEdit_ into the inactive double-buffer slot
    //! and publish it atomically. Message thread only.
    void publishChain();

    IPluginInstance* instrument_ = nullptr;

    // Editable chain (message thread truth). Audio thread never touches this.
    std::vector<IPluginInstance*> fxEdit_;

    // Double-buffered snapshots + the live pointer the audio thread reads.
    FxChainSnapshot               snapStore_[2];
    std::atomic<int>              activeSlot_{0};
    std::atomic<FxChainSnapshot*> liveChain_{nullptr};

    std::atomic<float> gain_{1.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<bool>  mute_{false};
    std::atomic<bool>  solo_{false};
    std::atomic<bool>  disabled_{false};

    VuMeter vuL_;
    VuMeter vuR_;

    // Scratch buffers (sized in prepare(); never resized on audio thread).
    // We keep two stereo working buffers so FX can ping-pong in series, plus
    // a small set of channel-pointer arrays for ProcessBlock plumbing.
    int                 maxBlock_ = 0;
    double              sampleRate_ = 48000.0;
    bool                prepared_ = false;

    std::vector<float>  bufA_[2];   // working buffer A (L,R)
    std::vector<float>  bufB_[2];   // working buffer B (L,R)
    float*              chA_[2] = {nullptr, nullptr};
    float*              chB_[2] = {nullptr, nullptr};
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_GRAPH_TRACK_H
