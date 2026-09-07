//----------------------------------------------------------------------------
//  PatchKnob — built-in patch nodes.
//
//  Concrete Node implementations that either reuse existing engine code or
//  provide the small built-ins the graph needs:
//
//    * PluginNode        wraps an IPluginInstance (VST2/VST3). Reuses the frozen
//                        plugin_api.h contract untouched. Because ProcessBlock in
//                        the frozen header carries no MIDI-OUT field, plugin MIDI
//                        output is captured in the WRAPPER via the small,
//                        patch-local IMidiOutInstance hook (see below) rather than
//                        by editing plugin_api.h. Instruments/MIDI-FX that emit
//                        MIDI implement IMidiOutInstance alongside IPluginInstance.
//    * SineSourceNode    deterministic sine generator (audio source).
//    * GainNode          multiplies its audio-in bus by an atomic gain.
//    * SumNode           explicit summing/mix bus (audio-in is already fan-in
//                        summed by the graph; this copies it to audio-out).
//    * AudioDeviceOutNode copies its (summed) audio-in into the engine's out[].
//    * AudioDeviceInNode  copies the engine's in[] into its audio-out.
//    * MidiInNode        lock-free SPSC ring -> midiOut (sequencer/hardware src).
//    * MidiOutNode       midiIn -> lock-free ring drained by the message thread.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_PATCH_PATCH_NODES_H
#define PATCHKNOB_ENGINE_PATCH_PATCH_NODES_H

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

#include "patch_graph.h"

namespace PatchKnob { namespace engine { namespace patch {

// ---------------------------------------------------------------------------
// Patch-local MIDI-OUT hook.
//
// The frozen engine/plugin_api.h ProcessBlock has no MIDI-out field, so a plugin
// cannot emit MIDI through the standard process() call. Rather than edit the
// frozen header, a plugin that produces MIDI (an instrument echoing note-offs,
// an arpeggiator, a MIDI-FX) also implements this tiny interface. PluginNode
// calls pullMidiOut() on the AUDIO THREAD immediately after IPluginInstance::
// process(), so the emitter simply hands back whatever it buffered during that
// call. Copy up to `cap` events into `out` and return the count produced.
// ---------------------------------------------------------------------------
class IMidiOutInstance {
public:
    virtual ~IMidiOutInstance() = default;
    virtual int pullMidiOut(MidiEvent* out, int cap) = 0;
};

// ---------------------------------------------------------------------------
// PluginNode — wraps IPluginInstance. Ports are derived from the descriptor.
// ---------------------------------------------------------------------------
class PluginNode : public Node {
public:
    // `midiOut` may be null (audio-only FX). Ownership of both stays with caller.
    explicit PluginNode(IPluginInstance* inst, IMidiOutInstance* midiOut = nullptr);

    const char* typeName() const override { return "PluginNode"; }
    int      numPorts() const override { return (int)ports_.size(); }
    PortDesc port(int i) const override { return ports_[(size_t)i]; }

    bool prepare(double sampleRate, int maxBlock) override;
    void setActive(bool a) override { IPluginInstance* p = inst_.load(std::memory_order_acquire); if (p) p->setActive(a); }
    void release() override { IPluginInstance* p = inst_.load(std::memory_order_acquire); if (p) p->release(); }
    void process(const NodeProcessContext& ctx) override;

    IPluginInstance* instance() const { return inst_.load(std::memory_order_acquire); }
    void setParamNormalized(uint32_t id, float v);

    // Replace the wrapped instance in place, keeping this node's port layout and
    // all connections.  Message-thread only, but RT-SAFE against a concurrent
    // process(): the new pointer is published atomically, then this call waits
    // until any in-flight block has drained (see inFlight_), so the returned OLD
    // instance can no longer be executing when the caller disposes it.  No
    // sleep-based pause of the audio thread is required.
    IPluginInstance* swapInstance(IPluginInstance* i);

    // MIDI channel filter: -1 == omni (play everything); 0..15 == only channel-
    // voice messages on that channel reach the plugin (system messages always
    // pass).  Lets a clip target ONE instrument node by matching its channel.
    void setMidiChannel(int c) { midiChannel_.store(c, std::memory_order_relaxed); }
    int  midiChannel() const   { return midiChannel_.load(std::memory_order_relaxed); }

protected:
    // inst_ is swapped by the message thread while the audio thread runs, so it
    // lives behind an atomic.  inFlight_ is TRUE exactly while process() is
    // inside a block; process() publishes it (seq_cst) BEFORE loading inst_ and
    // swapInstance() reads it (seq_cst) AFTER storing the new pointer — the
    // Dekker-style pairing that lets the swapper prove the old instance has
    // drained before handing it back.
    std::atomic<IPluginInstance*> inst_;
    std::atomic<bool>             inFlight_{false};
    IMidiOutInstance*     midiEmitter_;
    std::vector<PortDesc> ports_;
    bool  hasAudioIn_  = false;
    bool  hasAudioOut_ = false;
    bool  hasMidiOut_  = false;
    std::atomic<int> midiChannel_{-1};
    MidiEvent midiOutScratch_[kNodeMidiCap];
    MidiEvent midiFilterScratch_[kNodeMidiCap];

    static constexpr int kParamQueue = 256;
    struct PendingParam { uint32_t id; float value; };
    PendingParam         paramQueue_[kParamQueue];
    std::atomic<unsigned> paramHead_{0};
    std::atomic<unsigned> paramTail_{0};
    ParamChange          paramScratch_[kParamQueue];
};

// ---------------------------------------------------------------------------
// SineSourceNode — deterministic sine oscillator (one stereo audio-out bus).
// ---------------------------------------------------------------------------
class SineSourceNode : public Node {
public:
    SineSourceNode(float freqHz = 440.0f, float amp = 1.0f)
        : freq_(freqHz), amp_(amp) {}

    const char* typeName() const override { return "SineSourceNode"; }
    int      numPorts() const override { return 1; }
    PortDesc port(int) const override {
        return PortDesc{ 0, PortKind::Audio, PortDir::Out, 2, "out" };
    }
    bool prepare(double sampleRate, int) override {
        // Clamp like the sibling nodes: a non-positive rate would make the
        // phase increment inf/NaN and poison the whole graph with NaN samples.
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0; phase_ = 0.0; return true;
    }
    void process(const NodeProcessContext& ctx) override;

    void  setFreq(float f) { freq_.store(f, std::memory_order_relaxed); }
    void  setAmp(float a)  { amp_.store(a,  std::memory_order_relaxed); }
    float freq() const     { return freq_.load(std::memory_order_relaxed); }
    float amp() const      { return amp_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> freq_;
    std::atomic<float> amp_;
    double sr_    = 48000.0;
    double phase_ = 0.0;
};

// ---------------------------------------------------------------------------
// GainNode — audio-in (stereo) * gain -> audio-out (stereo).
// ---------------------------------------------------------------------------
class GainNode : public Node {
public:
    explicit GainNode(float gain = 1.0f) : gain_(gain) {}

    const char* typeName() const override { return "GainNode"; }
    int      numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return (i == 0) ? PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "in"  }
                        : PortDesc{ 1, PortKind::Audio, PortDir::Out, 2, "out" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    void  setGain(float g) { gain_.store(g, std::memory_order_relaxed); }
    float gain() const     { return gain_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> gain_;
};

// ---------------------------------------------------------------------------
// TrackFaderNode — Ardour's `Amp` processor as a real graph node.
//
// In Ardour the FADER is not a property of the mixer strip, it is one entry in
// the route's processor list (gtk2_ardour/processor_box.cc setup_entry_positions
// splits the list on the Amp with GainAutomation).  Everything above it is
// "pre-fader", everything below "post-fader".  MasterMixerNode applies its
// per-track gain at the inlet, so an insert plugged in front of the inlet is
// necessarily PRE-fader.  To place processors AFTER the fader, the strip's
// channel is switched to unity (MasterMixerNode::setExternalFader) and this node
// carries the gain instead — it is the Amp, and the inserts wired after it are
// genuinely post-fader.
//
// The gain is ramped across the block (Ardour's Amp::apply_gain does the same)
// so dragging a fader does not produce zipper noise, and the node publishes its
// own stereo peak so a strip metered at the fader tap still reads correctly.
// ---------------------------------------------------------------------------
class TrackFaderNode : public Node {
public:
    explicit TrackFaderNode(float gain = 1.0f) : gain_(gain), cur_(gain) {}

    const char* typeName() const override { return "TrackFaderNode"; }
    int      numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return (i == 0) ? PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "in"  }
                        : PortDesc{ 1, PortKind::Audio, PortDir::Out, 2, "out" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    void  setGain(float g) { gain_.store(g, std::memory_order_relaxed); }
    float gain() const     { return gain_.load(std::memory_order_relaxed); }
    void  setMute(bool m)  { mute_.store(m, std::memory_order_relaxed); }
    bool  mute() const     { return mute_.load(std::memory_order_relaxed); }
    float peakLeft()  const { return peakL_.load(std::memory_order_relaxed); }
    float peakRight() const { return peakR_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> gain_{1.0f};
    std::atomic<bool>  mute_{false};
    std::atomic<float> peakL_{0.f}, peakR_{0.f};
    float              cur_ = 1.0f;      // audio-thread ramp state
};

// One mono input duplicated sample-for-sample to a stereo output.
class MonoToStereoNode : public Node {
public:
    const char* typeName() const override { return "MonoToStereoNode"; }
    int numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return i == 0 ? PortDesc{0,PortKind::Audio,PortDir::In,1,"MONO IN"}
                      : PortDesc{1,PortKind::Audio,PortDir::Out,2,"STEREO OUT"};
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;
};

// ---------------------------------------------------------------------------
// SumNode — explicit stereo mix bus. The graph already fan-in sums into the
// single audio-in bus; this node forwards it to its audio-out (and meters it).
// ---------------------------------------------------------------------------
class SumNode : public Node {
public:
    SumNode() = default;
    const char* typeName() const override { return "SumNode"; }
    int      numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return (i == 0) ? PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "in"  }
                        : PortDesc{ 1, PortKind::Audio, PortDir::Out, 2, "out" };
    }
    bool prepare(double sr, int) override {
        vuL_.setSampleRate(sr); vuR_.setSampleRate(sr); return true;
    }
    void process(const NodeProcessContext& ctx) override;

    VuMeter& vuLeft()  { return vuL_; }
    VuMeter& vuRight() { return vuR_; }

private:
    VuMeter vuL_, vuR_;
};

// ---------------------------------------------------------------------------
// MixerNode — a summing mixer with N stereo input channels (each gain + mute)
// summed to one stereo output, plus per-channel peak metering.  The channel
// count is adjustable at runtime (recompile the graph after changing it).
// Patch a mixer's output into another mixer's input to build submixes.
// ---------------------------------------------------------------------------
class MixerNode : public Node {
public:
    static constexpr int kMaxChannels = 256;
    explicit MixerNode(int channels = 4) { setChannels(channels);
        for (int i = 0; i < kMaxChannels; ++i) { gain_[i].store(1.0f); mute_[i].store(false);
                                                 vuAccL_[i].store(0.f); vuAccR_[i].store(0.f);
                                                 vu_[i].store(0.f); vuL_[i].store(0.f);
                                                 vuR_[i].store(0.f); pan_[i].store(0.f);
                                                 balPubL_[i].store(0.f); balPubR_[i].store(0.f); }
        for (int s = 0; s < kMaxCaptureTaps; ++s) { capChan_[s].store(-1); capBuf_[s].store(nullptr);
                                                    capCap_[s].store(0); capPos_[s].store(0); } }

    const char* typeName() const override { return "MixerNode"; }
    int      numPorts() const override { return channels_.load(std::memory_order_relaxed) + 1; }
    // Port 0 is the OUTPUT (stable id, so its wiring survives channel changes);
    // ports 1..channels are the input channels.  Input channel `ch` == port id
    // ch+1 == the ch-th audio-in the graph packs into process().
    PortDesc port(int i) const override {
        if (i == 0) return PortDesc{ 0, PortKind::Audio, PortDir::Out, 2, "out" };
        return PortDesc{ (PortId)i, PortKind::Audio, PortDir::In, 2, "ch" };
    }
    bool prepare(double sr, int) override { meterSr_ = sr > 0.0 ? sr : 48000.0; return true; }
    void process(const NodeProcessContext& ctx) override;

    int   channels() const { return channels_.load(std::memory_order_relaxed); }
    void  setChannels(int n) { channels_.store(n < 0 ? 0 : (n > kMaxChannels ? kMaxChannels : n),
                                               std::memory_order_relaxed); }
    void  setGain(int ch, float g) { if (ch >= 0 && ch < kMaxChannels) gain_[ch].store(g); }
    float gain(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? gain_[ch].load() : 0.f; }
    void  setMute(int ch, bool m) { if (ch >= 0 && ch < kMaxChannels) mute_[ch].store(m); }
    bool  mute(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? mute_[ch].load() : false; }
    /*  METERING CONTRACT.
     *
     *  The audio thread MAX-ACCUMULATES into vuAcc*_ and never clears them; the
     *  UI calls meterLatch() exactly once per frame, which moves the accumulated
     *  peak into vu*_ and resets the accumulator.
     *
     *  It used to store() the current block's peak straight into vu*_ every
     *  block.  With 128-sample blocks that is ~6 writes per 60 Hz frame and the
     *  UI only ever read the LAST one, so five of every six peaks were thrown
     *  away -- and if the final block before a frame happened to be quiet the
     *  meter read near zero.  That is what made the meters miss transients, lag,
     *  and flicker in and out: a kick's attack lives in one block and the UI was
     *  usually looking at a different one.
     *
     *  Latching in one place (rather than each reader doing its own
     *  read-and-reset) means several views can show the same meter without
     *  stealing each other's peaks.  */
    void  meterLatch() {
        const int n = channels_.load(std::memory_order_relaxed);
        for (int ch = 0; ch < n && ch < kMaxChannels; ++ch) {
            const float al = vuAccL_[ch].exchange(0.f, std::memory_order_relaxed);
            const float ar = vuAccR_[ch].exchange(0.f, std::memory_order_relaxed);
            const float bl = balPubL_[ch].load(std::memory_order_relaxed);
            const float br = balPubR_[ch].load(std::memory_order_relaxed);
            const float l = al > bl ? al : bl;
            const float r = ar > br ? ar : br;
            vuL_[ch].store(l, std::memory_order_relaxed);
            vuR_[ch].store(r, std::memory_order_relaxed);
            vu_[ch].store(l > r ? l : r, std::memory_order_relaxed);
        }
        const float aml = masterAccL_.exchange(0.f, std::memory_order_relaxed);
        const float amr = masterAccR_.exchange(0.f, std::memory_order_relaxed);
        const float bml = balPubML_.load(std::memory_order_relaxed);
        const float bmr = balPubMR_.load(std::memory_order_relaxed);
        const float ml = aml > bml ? aml : bml;
        const float mr = amr > bmr ? amr : bmr;
        masterVuL_.store(ml, std::memory_order_relaxed);
        masterVuR_.store(mr, std::memory_order_relaxed);
        masterVu_.store(ml > mr ? ml : mr, std::memory_order_relaxed);
    }
    //! Audio thread: fold this block's peak in without losing an earlier one.
    static void accumPeak(std::atomic<float>& a, float v) {
        if (!(v > 0.f)) return;                       // also rejects NaN
        float cur = a.load(std::memory_order_relaxed);
        while (v > cur &&
               !a.compare_exchange_weak(cur, v, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) { }
    }
    /*  BALLISTICS (audio thread).  Ported from engine::VuMeter, which had the
     *  right maths but sat on the fixed-graph path that stops running the
     *  moment g_modular latches -- i.e. as soon as any plugin, rack or VST is
     *  loaded, which is always.  Here it runs where the meters actually are.
     *
     *  Instant attack, a 21 ms plateau, then a constant 24 dB/s fall computed
     *  from n/sampleRate -- so the decay is a function of ELAPSED TIME, not of
     *  how often the UI happens to look.  That is the whole point: the old
     *  published value was a bare max-since-last-read, which for a decaying
     *  note is the level at the START of the read window.  Read it at 4 Hz and
     *  the meter sat flat for 240 ms and then fell ~5 dB in one step.  */
    static void ballistic(float& state, int& holdFrames, float blockPeak,
                          int n, double sr) {
        if (n <= 0) return;
        const double rate = sr > 0.0 ? sr : 48000.0;
        if (blockPeak >= state) {
            state = blockPeak;
            holdFrames = (int) (rate * 0.021);
        } else if (holdFrames > 0) {
            holdFrames -= n;
        } else if (state > 0.0000001f) {
            const float fallDb = 24.0f * (float) n / (float) rate;
            state *= std::pow(10.0f, -fallDb / 20.0f);
            if (state < 0.0000001f) state = 0.f;
        }
    }
    void  accumChannelPeak(int ch, float l, float r, int n) {
        if (ch < 0 || ch >= kMaxChannels) return;
        // The accumulator still catches every transient between reads; the
        // ballistic state is what gives the bar a correct fall.  meterLatch()
        // publishes the max of the two, so attacks stay exact and releases
        // stop depending on the frame rate.
        accumPeak(vuAccL_[ch], l); accumPeak(vuAccR_[ch], r);
        ballistic(balL_[ch], balHoldL_[ch], l, n, meterSr_);
        ballistic(balR_[ch], balHoldR_[ch], r, n, meterSr_);
        balPubL_[ch].store(balL_[ch], std::memory_order_relaxed);
        balPubR_[ch].store(balR_[ch], std::memory_order_relaxed);
    }
    void  accumMasterPeak(float l, float r, int n) {
        accumPeak(masterAccL_, l); accumPeak(masterAccR_, r);
        ballistic(balML_, balHoldML_, l, n, meterSr_);
        ballistic(balMR_, balHoldMR_, r, n, meterSr_);
        balPubML_.store(balML_, std::memory_order_relaxed);
        balPubMR_.store(balMR_, std::memory_order_relaxed);
    }

    float vu(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? vu_[ch].load() : 0.f; }
    float vuLeft(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? vuL_[ch].load() : 0.f; }
    float vuRight(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? vuR_[ch].load() : 0.f; }
    // master (output) strip: a final gain + its own VU on the summed output.
    void  setMasterGain(float g) { masterGain_.store(g); }
    float masterGain() const { return masterGain_.load(); }
    //! Master MUTE.  Ardour's master bus is mutable like any other route, and
    //! doing it by zeroing the gain would destroy the fader position, so it is
    //! its own flag applied alongside the master gain.
    void  setMasterMute(bool m) { masterMute_.store(m, std::memory_order_relaxed); }
    bool  masterMute() const { return masterMute_.load(std::memory_order_relaxed); }
    float masterVu() const { return masterVu_.load(); }
    float masterVuLeft() const { return masterVuL_.load(); }
    float masterVuRight() const { return masterVuR_.load(); }
    // per-channel stereo pan (-1 left .. 0 centre .. +1 right).
    void  setPan(int ch, float p) { if (ch >= 0 && ch < kMaxChannels)
                                        pan_[ch].store(p < -1.f ? -1.f : (p > 1.f ? 1.f : p)); }
    float pan(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? pan_[ch].load() : 0.f; }
    void  setVu(int ch, float v) { if (ch >= 0 && ch < kMaxChannels) vu_[ch].store(v); }
    void  clearChannelMeter(int ch) { if(ch>=0&&ch<kMaxChannels){
        vuAccL_[ch].store(0.f,std::memory_order_relaxed);
        vuAccR_[ch].store(0.f,std::memory_order_relaxed);
        balL_[ch]=balR_[ch]=0.f; balHoldL_[ch]=balHoldR_[ch]=0;
        balPubL_[ch].store(0.f,std::memory_order_relaxed);
        balPubR_[ch].store(0.f,std::memory_order_relaxed); } }
    void  setVuStereo(int ch,float l,float r) { if(ch>=0&&ch<kMaxChannels){
        vuL_[ch].store(l,std::memory_order_relaxed);vuR_[ch].store(r,std::memory_order_relaxed);
        vu_[ch].store(l>r?l:r,std::memory_order_relaxed); } }
    void  setMasterVu(float v) { masterVu_.store(v); }
    void  setMasterVuStereo(float l,float r) { masterVuL_.store(l,std::memory_order_relaxed);
        masterVuR_.store(r,std::memory_order_relaxed);masterVu_.store(l>r?l:r,std::memory_order_relaxed); }
    // Preallocated, lock-free stereo capture taps -- kMaxCaptureTaps of them,
    // so several channels can record their INPUT simultaneously (punch modes
    // arm multiple tracks; each tap copies the raw PRE-FADER inlet of its
    // channel, i.e. whatever the patcher routes into that strip).  All slot
    // management runs on the MESSAGE thread; the audio thread only scans the
    // slots.  The owner keeps a buffer alive until the audio thread has passed
    // a block boundary after endTrackCaptureFor() -- the same keep-alive
    // contract the old single tap had.
    static constexpr int kMaxCaptureTaps = 8;
    //! Claim a free tap for channel `ch`.  Refused (false) when every slot is
    //! busy or `ch` already has one -- the owner must endTrackCaptureFor()
    //! first, because replacing a live tap here would leak its buffer.
    bool beginTrackCapture(int ch, float* interleavedStereo, size_t capacityFrames) {
        int free_ = -1;
        for (int s = 0; s < kMaxCaptureTaps; ++s) {
            if (capChan_[s].load(std::memory_order_relaxed) == ch) return false;
            if (free_ < 0 && !capBuf_[s].load(std::memory_order_acquire) &&
                capChan_[s].load(std::memory_order_relaxed) < 0) free_ = s;
        }
        if (free_ < 0) return false;
        // pos/cap/channel settle BEFORE the buffer publishes the slot: the
        // audio thread tests the channel, then acquire-loads the buffer, so a
        // half-programmed slot can never pair the new buffer with stale sizes.
        capPos_[free_].store(0, std::memory_order_relaxed);
        capCap_[free_].store(capacityFrames, std::memory_order_relaxed);
        capChan_[free_].store(ch, std::memory_order_relaxed);
        capBuf_[free_].store(interleavedStereo, std::memory_order_release);
        return true;
    }
    //! Close channel `ch`'s tap and hand its buffer back (null if none).
    float* endTrackCaptureFor(int ch) {
        for (int s = 0; s < kMaxCaptureTaps; ++s)
            if (capChan_[s].load(std::memory_order_relaxed) == ch) {
                capChan_[s].store(-1, std::memory_order_relaxed);
                return capBuf_[s].exchange(nullptr, std::memory_order_acq_rel);
            }
        return nullptr;
    }
    //! Frames channel `ch`'s tap has captured so far (0 if it has no tap).
    size_t trackCaptureFramesFor(int ch) const {
        for (int s = 0; s < kMaxCaptureTaps; ++s)
            if (capChan_[s].load(std::memory_order_relaxed) == ch)
                return capPos_[s].load(std::memory_order_acquire);
        return 0;
    }

protected:
    // Atomic because the audio thread reads it in process() while the message
    // thread adjusts it via setChannels() (the graph recompiles afterwards).
    std::atomic<int> channels_{4};
    std::atomic<float> gain_[kMaxChannels];
    std::atomic<bool>  mute_[kMaxChannels];
    // Peak accumulators: written by the audio thread, drained by meterLatch().
    std::atomic<float> vuAccL_[kMaxChannels], vuAccR_[kMaxChannels];
    std::atomic<float> masterAccL_{0.f}, masterAccR_{0.f};
    // Ballistic state: written ONLY by the audio thread (bal*_, balHold*_),
    // mirrored into balPub*_ each block for readers on any thread.
    double meterSr_ = 48000.0;
    float  balL_[kMaxChannels]{}, balR_[kMaxChannels]{};
    int    balHoldL_[kMaxChannels]{}, balHoldR_[kMaxChannels]{};
    float  balML_ = 0.f, balMR_ = 0.f;
    int    balHoldML_ = 0, balHoldMR_ = 0;
    std::atomic<float> balPubL_[kMaxChannels], balPubR_[kMaxChannels];
    std::atomic<float> balPubML_{0.f}, balPubMR_{0.f};
    std::atomic<float> vu_[kMaxChannels];
    std::atomic<float> vuL_[kMaxChannels];
    std::atomic<float> vuR_[kMaxChannels];
    std::atomic<float> pan_[kMaxChannels];
    std::atomic<float> masterGain_{1.0f};
    std::atomic<bool>  masterMute_{false};
    std::atomic<float> masterVu_{0.f};
    std::atomic<float> masterVuL_{0.f}, masterVuR_{0.f};
    std::atomic<int>    capChan_[kMaxCaptureTaps];   // -1 = slot free
    std::atomic<float*> capBuf_[kMaxCaptureTaps];
    std::atomic<size_t> capCap_[kMaxCaptureTaps];
    std::atomic<size_t> capPos_[kMaxCaptureTaps];
};

// ---------------------------------------------------------------------------
// AuxBusNode -- ONE aux (send/return) bus.
//
// A real node, not a hidden stage inside the mixer, so the bus is visible and
// patchable in the modular view and so an ordinary insert chain (a reverb, a
// delay) can be wired in front of it with the same machinery every mixer strip
// uses.  Signal flow for bus k:
//
//    MasterMixerNode "AUX k SEND" out -> [bus insert chain] -> AuxBusNode in
//    AuxBusNode out -> MasterMixerNode "AUX k RETURN" in (a FEEDBACK port)
//
// The node itself is the RETURN stage: return gain (ramped, so riding it does
// not zipper), a mute, and a stereo peak meter on what it actually returns.
// Because the gain sits at the END of the bus, everything wired in FRONT of the
// node is inherently pre-fader -- which is why an aux strip's inserts are all
// pre-fader, the mirror image of the master strip's being all post-fader.
//
// Metering follows TrackFaderNode's published-ballistic contract rather than
// MixerNode's latched accumulator: peakLeft()/peakRight() are read straight off
// the audio thread's ballistic state (instant attack, 21 ms hold, 24 dB/s fall)
// so any number of views can read the bus meter at any rate without stealing
// each other's peaks and without a per-frame latch call.
// ---------------------------------------------------------------------------
class AuxBusNode : public Node {
public:
    explicit AuxBusNode(const char* name = "Aux") : name_(name ? name : "Aux") {}

    const char* typeName() const override { return "AuxBusNode"; }
    int      numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return (i == 0) ? PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "in"  }
                        : PortDesc{ 1, PortKind::Audio, PortDir::Out, 2, "out" };
    }
    bool prepare(double sr, int) override { sr_ = sr > 0.0 ? sr : 48000.0; return true; }
    void process(const NodeProcessContext& ctx) override;

    // ---- message thread ----
    const std::string& name() const { return name_; }
    void  setName(const char* n)    { name_ = n ? n : ""; }
    void  setReturnGain(float g)    { returnGain_.store(std::isfinite(g) && g > 0.f ? g : 0.f,
                                                        std::memory_order_relaxed); }
    float returnGain() const        { return returnGain_.load(std::memory_order_relaxed); }
    void  setMute(bool m)           { mute_.store(m, std::memory_order_relaxed); }
    bool  mute() const              { return mute_.load(std::memory_order_relaxed); }
    float peakLeft()  const { return peakL_.load(std::memory_order_relaxed); }
    float peakRight() const { return peakR_.load(std::memory_order_relaxed); }

private:
    std::string        name_;                    // message thread only
    std::atomic<float> returnGain_{1.0f};
    std::atomic<bool>  mute_{false};
    std::atomic<float> peakL_{0.f}, peakR_{0.f};
    // audio-thread only
    float  cur_    = 1.0f;                       // gain ramp state
    float  balL_   = 0.f, balR_ = 0.f;
    int    holdL_  = 0,   holdR_ = 0;
    double sr_     = 48000.0;
};

// ---------------------------------------------------------------------------
// MasterMixerNode -- the singleton master HUB.  Rather than hosting nodes, it
// grows PORTS as tracks are added: each track is a channel with an INLET + an
// OUTLET of its kind (MIDI for instrument tracks, AUDIO for audio tracks), so
// you patch the processing yourself in the modular view.
//   fixed ports:  0 = master audio OUT (the mix),  1 = MIDI clock OUT.
//   per track t:  inlet = port(2 + 2t),  outlet = port(3 + 2t).
//   per aux SLOT s (see below): send OUT = 1024 + s, return IN = 2048 + s.
//
// AUX PORT IDS ARE NOT POSITIONAL, DELIBERATELY.  The per-track ids above are,
// and that is exactly what made removeTrack() silently re-aim every surviving
// strip until portAfterTrackRemoval() + PatchGraph::remapNodePorts() were added.
// Appending aux ports after the tracks would have handed that same defect to
// the aux buses AND made addTrack() renumber them (it has no remap step at all,
// because appending never used to shift anything).  So an aux bus owns a SLOT
// out of a small free list, its two port ids are derived from the SLOT, and the
// bus INDEX the mixer API speaks in is just a position in auxSlot_.  Adding or
// deleting tracks cannot touch an aux port; deleting aux bus 0 of 3 does not
// renumber buses 1 and 2's ports either.  A wired port id keeps meaning the
// same signal until the thing it belongs to is deleted -- which is the whole
// invariant, and it is why saved projects need no port-id migration for this.
// Audio channels are summed (gain/pan/mute) into the master out AND their raw
// inlet is echoed to their outlet (a direct/insert send); MIDI channels pass
// their inlet straight to their outlet (thru).  Tempo-synced MIDI clock on
// port 1.  channels() (from MixerNode) == number of tracks, so the strip view +
// audio_app_mixer_* address each track's gain/mute/pan/vu.
// ---------------------------------------------------------------------------
class MasterMixerNode : public MixerNode {
public:
    struct Trk { bool midi; };
    // ---- aux (send/return) buses --------------------------------------------
    static constexpr int kMaxAuxBuses      = 16;
    static constexpr int kAuxSendPortBase   = 1024;   // + SLOT (never + index)
    static constexpr int kAuxReturnPortBase = 2048;   // + SLOT

    MasterMixerNode() : MixerNode(1) {
        for (int i = 0; i < kMaxChannels; ++i) {
            extFader_[i].store(false);
            solo_[i].store(false);
            for (int a = 0; a < kMaxAuxBuses; ++a) {
                sendGain_[i][a].store(0.f, std::memory_order_relaxed);
                sendPre_[i][a].store(0,   std::memory_order_relaxed);
                sendOn_[i][a].store(1,    std::memory_order_relaxed);
                sendNorm_[i][a] = 0.f;
            }
        }
        setChannels(0); publishSnapshot();
    }
    ~MasterMixerNode() override;

    // message thread: grow / shrink the per-track ports.  Each edit freezes the
    // track layout into a new immutable snapshot for the audio thread (RCU).
    int  addTrack(bool midi) {
        tracks_.push_back(Trk{ midi });
        const int t = (int)tracks_.size() - 1;
        clearStrip(t);                       // a reused channel starts clean
        setChannels((int)tracks_.size()); publishSnapshot(); return t;
    }
    //! Message thread.  NOTE: this node's per-track port IDS ARE POSITIONAL
    //! (inlet == 2 + 2t), so compacting tracks_ RENAMES every port id at or
    //! after `i`.  The owning PatchGraph still holds the OLD ids in its
    //! connection list, so the caller MUST rewrite them through
    //! PatchGraph::remapNodePorts(id, portAfterTrackRemoval-with-i) in the same
    //! edit -- otherwise every surviving track's fader/pan/mute/meter/record
    //! drives the wrong signal, the deleted track keeps sounding through the
    //! strip that inherited its port, and the last track goes silent.
    void removeTrack(int i)  {
        if (i < 0 || i >= (int)tracks_.size()) return;
        tracks_.erase(tracks_.begin() + i);
        // The CHANNEL indices compact with the ports, so everything keyed by
        // channel -- the send matrix and the solo flags, exactly like gain/pan/
        // mute already were -- has to shift in lockstep or the surviving strips
        // inherit the deleted one's sends.
        for (int t = i; t + 1 < kMaxChannels; ++t) copyStrip(t + 1, t);
        clearStrip(kMaxChannels - 1);
        recountSolo();
        setChannels((int)tracks_.size()); publishSnapshot();
    }

    //! The port-id rename that accompanies removeTrack(removedTrack): returns
    //! the new id for old port `port`, or -1 when the port belonged to the
    //! removed track and its connections must be dropped.
    static int portAfterTrackRemoval(int port, int removedTrack) {
        if (removedTrack < 0) return port;
        if (port < 2) return port;                  // 0 = master out, 1 = clock out
        // Aux send/return ids are slot-derived, not positional: a track edit
        // must leave them EXACTLY as they are (the naive `(port-2)/2` below
        // would have "shifted" port 1024 down to 1022 and silently re-aimed
        // every aux cable at another bus).
        if (port >= kAuxSendPortBase) return port;
        const int t = (port - 2) / 2;
        if (t <  removedTrack) return port;         // below the cut: unchanged
        if (t == removedTrack) return -1;           // gone
        return port - 2;                            // survivors shift down one track
    }
    int  trackCount() const  { return (int)tracks_.size(); }
    bool trackIsMidi(int i) const { return i >= 0 && i < (int)tracks_.size() && tracks_[i].midi; }

    // ---- aux buses (message thread) -----------------------------------------
    //! Create a bus.  Returns its INDEX, or -1 when kMaxAuxBuses are already up.
    //! The graph must be recompiled afterwards (the port list grew).
    int  addAux();
    //! Delete bus `aux` (an INDEX).  The surviving buses keep their port ids --
    //! only their indices compact -- so the caller only has to drop the deleted
    //! bus's own cables (PatchGraph::pruneDanglingConnections does it).
    bool removeAux(int aux);
    int  auxCount() const { return (int)auxSlot_.size(); }
    //! The stable SLOT behind bus index `aux` (-1 if out of range).  Port ids,
    //! the send matrix and the saved project all key off THIS, never the index.
    int  auxSlot(int aux) const {
        return (aux >= 0 && aux < (int)auxSlot_.size()) ? (int)auxSlot_[(size_t)aux] : -1;
    }
    static PortId auxSendPort(int slot)   { return (PortId)(kAuxSendPortBase + slot); }
    static PortId auxReturnPort(int slot) { return (PortId)(kAuxReturnPortBase + slot); }
    //! The aux RETURN legs are the graph's feedback edges: the bus is fed BY
    //! this node, so its output coming back here is a cycle at node granularity.
    //! Marking the return port makes the edge carry signal without carrying
    //! ordering -- the return is summed in one block late.  See
    //! Node::portIsFeedback / PatchGraph::edgeIsFeedback.
    bool portIsFeedback(PortId p) const override {
        return p >= (PortId)kAuxReturnPortBase &&
               p <  (PortId)(kAuxReturnPortBase + kMaxAuxBuses);
    }

    // ---- per-track SENDS (message thread; `aux` is a bus INDEX) --------------
    //! Level is NORMALISED 0..1 to match the strip UI; the engine owns the
    //! taper (sendNormToGain).  0 is EXACT silence, not merely quiet.
    float sendLevel(int track, int aux) const;
    void  setSendLevel(int track, int aux, float norm);
    //! Pre-fader sends ignore the strip fader, its mute AND solo (the classic
    //! headphone/monitor feed); post-fader sends follow all three, which is what
    //! a reverb send wants.  Post-fader is the default.
    bool  sendPreFader(int track, int aux) const;
    void  setSendPreFader(int track, int aux, bool pre);
    //! A disabled send KEEPS its level so it can be toggled back on.
    bool  sendEnabled(int track, int aux) const;
    void  setSendEnabled(int track, int aux, bool on);

    //! SEND TAPER.  Ardour's fader curve (gain_to_slider_position's inverse),
    //! rescaled so norm==1 is UNITY (a send knob's top is unity, unlike a strip
    //! fader whose top is +6 dB) and norm<=0 is a hard zero -- a "quiet" send is
    //! not silence, and a reverb fed -80 dB still smears the mix.
    static float sendNormToGain(float norm);
    static float sendGainToNorm(float gain);

    // ---- SOLO (message thread) ----------------------------------------------
    //! Real solo state, in the engine, where the send matrix can see it.  It
    //! used to live in a function-local `static bool g_busSolo[]` in main.cpp
    //! that was folded into the node's MUTE by recompute_mutes(), so it could
    //! never round-trip through a project file and nothing below the UI could
    //! tell "muted" from "not soloed".  With no bus soloed anywhere this is
    //! inert, so folding solo into mute upstream still behaves identically.
    void setSolo(int ch, bool on) {
        if (ch < 0 || ch >= kMaxChannels) return;
        solo_[ch].store(on, std::memory_order_relaxed);
        recountSolo();
    }
    bool solo(int ch) const {
        return (ch >= 0 && ch < kMaxChannels) && solo_[ch].load(std::memory_order_relaxed);
    }
    int  soloCount() const { return soloCount_.load(std::memory_order_relaxed); }
    //! The audible test the fader stage applies: explicitly muted, or something
    //! else is soloed and this is not it.
    bool channelSilenced(int ch) const {
        if (ch < 0 || ch >= kMaxChannels) return true;
        if (mute_[ch].load(std::memory_order_relaxed)) return true;
        return soloCount_.load(std::memory_order_relaxed) > 0 &&
               !solo_[ch].load(std::memory_order_relaxed);
    }

    //! EXTERNAL FADER (Ardour: the Amp processor sits in the route's processor
    //! list, not in the strip).  When set for a channel this node applies UNITY
    //! and ignores mute for it, because a TrackFaderNode upstream in that
    //! track's insert chain is carrying the gain/mute — which is what makes
    //! post-fader inserts possible.  Pan and metering stay here (Ardour panner
    //! is after the fader too), so the strip meter remains post-everything.
    void setExternalFader(int ch, bool on) {
        if (ch >= 0 && ch < kMaxChannels) extFader_[ch].store(on, std::memory_order_relaxed);
    }
    bool externalFader(int ch) const {
        return (ch >= 0 && ch < kMaxChannels) && extFader_[ch].load(std::memory_order_relaxed);
    }

    const char* typeName() const override { return "MasterMixerNode"; }
    // Enumeration order matters: locate() derives a port's index WITHIN its
    // (kind, dir) group from this walk, and process() mirrors it.  All the aux
    // SENDS come after every track port, then all the aux RETURNS -- so the aux
    // send bank starts at audioOut index 1 + (#audio tracks) and the return bank
    // at audioIn index (#audio tracks), whatever mix of MIDI/audio tracks exists.
    int numPorts() const override {
        return 2 + (int)tracks_.size() * 2 + (int)auxSlot_.size() * 2;
    }
    PortDesc port(int i) const override {
        if (i == 0) return PortDesc{ 0, PortKind::Audio, PortDir::Out, 2, "master" };
        if (i == 1) return PortDesc{ 1, PortKind::Midi,  PortDir::Out, 1, "clock"  };
        const int trackPorts = 2 + (int)tracks_.size() * 2;
        if (i < trackPorts) {
            const int  t      = (i - 2) / 2;
            const bool outlet = ((i - 2) & 1) != 0;
            const bool midi   = (t < (int)tracks_.size()) ? tracks_[t].midi : false;
            return PortDesc{ (PortId)i, midi ? PortKind::Midi : PortKind::Audio,
                             outlet ? PortDir::Out : PortDir::In,
                             (uint16_t)(midi ? 1 : 2), outlet ? "out" : "in" };
        }
        const int na = (int)auxSlot_.size();
        int a = i - trackPorts;
        if (a < na) {
            const int slot = (int)auxSlot_[(size_t)a];
            return PortDesc{ auxSendPort(slot), PortKind::Audio, PortDir::Out, 2,
                             auxSendName_[(size_t)slot].c_str() };
        }
        a -= na;
        if (a >= na) a = na > 0 ? na - 1 : 0;
        const int slot = na > 0 ? (int)auxSlot_[(size_t)a] : 0;
        return PortDesc{ auxReturnPort(slot), PortKind::Audio, PortDir::In, 2,
                         auxRetName_[(size_t)slot].c_str() };
    }
    bool prepare(double sr, int) override { sr_ = sr > 0.0 ? sr : 48000.0; return true; }
    void process(const NodeProcessContext& ctx) override;

private:
    // ---- RT snapshot of the track layout (RCU, like the graph's RenderPlan) --
    //
    // tracks_ is a std::vector the message thread push_back/erases, so the audio
    // thread must NEVER touch it: process() reads ONLY an immutable heap
    // TrackSnap published through snap_.  numPorts()/port()/trackCount()/
    // trackIsMidi() stay on the vector — they are message-thread queries and the
    // graph recompiles after every layout change anyway.
    //
    // Retirement: the old snapshot cannot be freed while a block that loaded it
    // is still running, so it is parked in retired_ stamped with the audio
    // thread's block generation (rtGen_, bumped at the end of each process()).
    // The NEXT edit frees every parked snapshot whose generation has advanced by
    // >= 2 blocks (one for the block possibly in flight at the swap, one for
    // publication slack).  All leftovers are freed in the destructor.
    struct TrackSnap {
        int     count;
        bool    midi[kMaxChannels];
        int     auxCount;                     // live aux buses, in INDEX order
        uint8_t auxSlot[kMaxAuxBuses];        // index -> stable slot
    };
    struct Retired   { TrackSnap* p; uint64_t gen; };

    void publishSnapshot();   // message thread: freeze tracks_ -> snap_, retire old

    // message thread: per-channel state that must follow a channel when
    // removeTrack() compacts the indices (gain/pan/mute already did).
    void copyStrip(int from, int to);
    void clearStrip(int ch);
    void recountSolo();

    std::vector<Trk>        tracks_;
    // Aux buses.  auxSlot_ is INDEX -> SLOT; the slot is what port ids, the send
    // matrix and the project file are keyed by, so neither a track edit nor
    // deleting another bus can renumber a live cable.
    std::vector<uint8_t>    auxSlot_;
    std::string             auxSendName_[kMaxAuxBuses];   // stable port labels
    std::string             auxRetName_[kMaxAuxBuses];
    // Send matrix, [channel][SLOT].  sendGain_ is the tapered LINEAR gain the
    // audio thread multiplies by; sendNorm_ is the 0..1 the UI set, kept so the
    // readback is exactly what was written (the taper is not perfectly
    // invertible in float).
    std::atomic<float>      sendGain_[kMaxChannels][kMaxAuxBuses];
    std::atomic<uint8_t>    sendPre_[kMaxChannels][kMaxAuxBuses];
    std::atomic<uint8_t>    sendOn_[kMaxChannels][kMaxAuxBuses];
    float                   sendNorm_[kMaxChannels][kMaxAuxBuses];
    std::atomic<bool>       solo_[kMaxChannels];
    std::atomic<int>        soloCount_{0};
    std::atomic<bool>       extFader_[kMaxChannels];   // gain carried upstream
    std::atomic<TrackSnap*> snap_{nullptr};
    std::vector<Retired>    retired_;      // message thread only
    std::atomic<uint64_t>   rtGen_{0};     // audio-thread block counter
    double sr_       = 48000.0;
    double clockAcc_ = 0.0;
    // Where the transport is expected at the top of the NEXT block if it simply
    // keeps rolling; any other position is a jump and re-seeds the MIDI clock.
    int64_t expectedPos_ = 0;
    // MIDI clock generator state: detect transport run-state edges so we can
    // emit real 0xFA Start / 0xFB Continue / 0xFC Stop (+ 0xF2 Song Position)
    // and seed the first 0xF8 to coincide with Start per the MIDI spec.
    bool   wasPlaying_ = false;
};

// ---------------------------------------------------------------------------
// AudioDeviceOutNode — copies its (summed) stereo audio-in into engine out[].
// ---------------------------------------------------------------------------
class AudioDeviceOutNode : public Node {
public:
    explicit AudioDeviceOutNode(int channels = 2) : channels_(channels) {}
    const char* typeName() const override { return "AudioDeviceOutNode"; }
    int      numPorts() const override { return 1; }
    PortDesc port(int) const override {
        return PortDesc{ 0, PortKind::Audio, PortDir::In,
                         (uint16_t)channels_, "in" };
    }
    bool prepare(double, int) override { return true; }
    bool isDeviceSink() const override { return true; }
    void setDeviceOutAdditive(bool enabled) override { additive_ = enabled; }
    void bindDeviceOut(float* const* out, int channels, int nframes) override {
        target_ = out; targetCh_ = channels; nframes_ = nframes;
    }
    void process(const NodeProcessContext& ctx) override;

private:
    int                 channels_ = 2;
    float* const*       target_   = nullptr;
    int                 targetCh_ = 0;
    int                 nframes_  = 0;
    bool                additive_ = false;
};

// ---------------------------------------------------------------------------
// AudioDeviceInNode — copies engine in[] into its stereo audio-out.
// ---------------------------------------------------------------------------
class AudioDeviceInNode : public Node {
public:
    explicit AudioDeviceInNode(int channels = 2)
        : channels_(channels < 1 ? 1 : (channels > kMaxPortsPerNode ? kMaxPortsPerNode : channels)) {
        portNames_.reserve((size_t)channels_);
        for (int i = 0; i < channels_; ++i)
            portNames_.push_back(std::string("CH ") + std::to_string(i + 1));
    }
    const char* typeName() const override { return "AudioDeviceInNode"; }
    int      numPorts() const override { return channels_; }
    PortDesc port(int i) const override {
        if (i < 0 || i >= channels_) i = 0;
        return PortDesc{ (PortId)i, PortKind::Audio, PortDir::Out, 1,
                         portNames_[(size_t)i].c_str() };
    }
    bool prepare(double, int) override { return true; }
    bool isDeviceSource() const override { return true; }
    void bindDeviceIn(const float* const* in, int channels, int nframes) override {
        src_ = in; srcCh_ = channels; nframes_ = nframes;
    }
    void process(const NodeProcessContext& ctx) override;

private:
    int                 channels_ = 2;
    const float* const* src_      = nullptr;
    int                 srcCh_    = 0;
    int                 nframes_  = 0;
    std::vector<std::string> portNames_;
};

// A MIDI event stamped with its ABSOLUTE due-time in samples (transport
// domain).  dueSample < 0 means "due immediately" (delivered at block start).
struct TimedMidi {
    int64_t   dueSample;
    MidiEvent ev;
};

// ---------------------------------------------------------------------------
// A "RELEASE" message: one whose loss leaves a note sounding forever (note-off,
// note-on with velocity 0, CC 120 All Sound Off, CC 123 All Notes Off).  The
// whole MIDI path obeys one rule: a release is NEVER silently discarded.  Every
// queue below either (a) keeps an unsent event queued until it can be delivered,
// or (b) reserves headroom so a release still fits when note-ons no longer do.
// ---------------------------------------------------------------------------
inline bool midiIsRelease(const MidiEvent& e) {
    const unsigned char hi = e.status & 0xF0u;
    return hi == 0x80u || (hi == 0x90u && e.data2 == 0) ||
           (hi == 0xB0u && (e.data1 == 120 || e.data1 == 123));
}

// ---------------------------------------------------------------------------
// MidiInNode — sequencer/hardware MIDI source. The producer thread pushes
// TIMESTAMPED events into a lock-free SPSC ring; process() delivers only the
// events due within the current block, at exact sample offsets, holding back
// future events (the producer schedules ahead of the playhead).
//
// DELIVERY DISCIPLINE (three rules, all of them "never strand a note"):
//
//  1. SCAN-AHEAD, NOT HEAD-OF-LINE.  The ring is NOT sorted by dueSample: live
//     hardware input is stamped "now" (dueSample < 0) or roughly one block
//     ahead, while sequencer events are stamped inside the current block, and
//     the two interleave in push order.  process() therefore SCANS the whole
//     queued window and delivers every event due inside this block wherever it
//     sits, instead of stopping at the first future event — a scheduled-ahead
//     event can never delay a live "due now" event queued behind it.  Genuinely
//     future events are left queued and still fire at their exact offsets.
//
//  2. HEAD ONLY ADVANCES OVER DELIVERED EVENTS.  Each slot carries a consumer-
//     private delivered mark; head_ is reclaimed over the contiguous delivered
//     prefix only.  An event that could not be handed to its plug this block
//     (destination buffer full) STAYS QUEUED and is retried next block instead
//     of being dropped — a dropped note-off is a permanently stuck note.
//
//  3. AN UNTARGETED RELEASE IS RE-AIMED, NOT DROPPED.  push() stamps the 0xFF
//     "no target" sentinel whenever port < 0, and a track index can also outrun
//     the compiled plug count.  A NOTE-ON with no target is dropped, as before —
//     it must never be redirected to plug 0 and play another track's instrument.
//     A RELEASE (see midiIsRelease) is instead re-aimed at whichever reachable
//     plug still has that exact note sounding FROM THIS NODE, so it lands on the
//     instrument that is actually holding it and nowhere else.  If the plug it
//     sounded on is genuinely gone from the compiled context there is no buffer
//     to write to and the release is discarded rather than mis-aimed at a
//     surviving plug (that would cut a different instrument's note) — the node
//     merely stops counting the note as sounding.  Re-silencing an instrument
//     whose MIDI source is being unwired is the graph compiler's job.
//
// The node also remembers which notes it has actually delivered, so flush()
// (transport locate/stop) can release them instead of stranding them.
// ---------------------------------------------------------------------------
class MidiInNode : public Node {
public:
    MidiInNode() = default;
    const char* typeName() const override { return "MidiInNode"; }

    // One MIDI-OUT plug per instrument track ("Instrument out").  The plug count
    // grows/shrinks with the arrange track count (mirrors the master mixer), and
    // an event pushed with track index t emerges ONLY on plug t -- so each track
    // feeds its own instrument with no MIDI-channel filtering.  Defaults to one
    // plug (also the shape every hardware-input MidiInNode keeps).
    int      numPorts() const override { return trackPorts_ < 1 ? 1 : trackPorts_; }
    PortDesc port(int i) const override {
        const char* nm = (i >= 0 && i < (int)portNames_.size())
                             ? portNames_[(size_t)i].c_str() : "out";
        return PortDesc{ (PortId)i, PortKind::Midi, PortDir::Out, 1, nm };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    // message thread: set how many per-track MIDI-out plugs this node exposes.
    // Rebuilds the plug labels ("Instrument 1", ...); the graph recompiles after.
    void setTrackPorts(int n);
    int  trackPorts() const { return trackPorts_ < 1 ? 1 : trackPorts_; }

    // Force every emitted channel-voice message onto channel `c` (0..15), or -1 to
    // pass the source channel through unchanged.  Used for a HARDWARE input node so
    // a keyboard (whatever channel it sends) drives the instrument it's wired to
    // (which filters by its own channel).  The singleton sequencer node leaves this
    // at -1 so per-track channels are preserved.
    void setOutChannel(int c) { outChannel_.store(c, std::memory_order_relaxed); }
    int  outChannel() const   { return outChannel_.load(std::memory_order_relaxed); }

    // Producer thread: enqueue one event due at an absolute transport sample
    // (dueSample < 0 = "now") on the exact OUT-PLUG `port` (track index).
    // Ring order is push order; dueSample need NOT be monotonic (process()
    // scans the queued window — see rule 1 above).  Returns false if full.
    // A late note-on is rejected before the ring runs out of room, so a release
    // pushed afterwards is still guaranteed a slot.
    bool push(const MidiEvent& e, int64_t dueSample, int port);
    bool push(const MidiEvent& e, int64_t dueSample) { return push(e, dueSample, 0); }
    bool push(const MidiEvent& e) { return push(e, -1, 0); }   // legacy "now"

    // ---- CONSUMER-SIDE ONLY (the thread that calls process(), i.e. audio) ----
    // These touch the consumer's head_ / delivered marks / held-note table, so
    // they must NOT be called from a producer thread.  (releaseHeldNotes() is the
    // exception: it only sets an atomic flag and is safe from any thread.)

    //! Transport discontinuity (locate / stop): discard every queued event AND
    //! arm a note-off for every note this node has ACTUALLY delivered and not yet
    //! released, emitted at the top of the next process().  Queued-but-never-
    //! delivered events strand nothing, so they are simply dropped: a node that
    //! never sounded a note emits nothing here.
    void flush();

    //! flush()'s pre-fix behaviour: drop the queue and let held notes hang.
    //! Only for a caller that is about to hard-reset the instruments itself.
    void flushDiscardingHeldNotes();

    //! Panic: emit a note-off for everything this node has sounding, WITHOUT
    //! touching the queue.  Safe from any thread (sets an atomic flag; the
    //! releases are emitted by the next process()).
    void releaseHeldNotes() {
        if (soundingCount_.load(std::memory_order_relaxed) > 0)
            pendingPanic_.store(true, std::memory_order_release);
    }

    //! Diagnostics: notes this node has delivered and not yet released.
    int  soundingNotes() const { return soundingCount_.load(std::memory_order_relaxed); }
    //! True while releases armed by flush()/releaseHeldNotes() are still pending.
    bool releasesPending() const { return pendingPanic_.load(std::memory_order_acquire); }

private:
    static constexpr int kRing = 1024;
    //! Reject non-releases once the ring is this close to full, so a release
    //! pushed later always fits (mirrored by MidiOutNode).
    static constexpr uint32_t kReleaseHeadroom = 64;
    //! Below this much free space process() force-flushes the head even if it is
    //! still future-dated: a backlog must never wedge the queue permanently.
    static constexpr uint32_t kBacklogValve = 64;
    //! Plugs whose sounding notes are tracked (MasterMixerNode caps tracks at
    //! 256).  Notes delivered to a plug beyond this are not tracked, so they are
    //! not auto-released by flush().
    static constexpr int kTrackedPlugs = 256;
    static constexpr int kHeldWords    = 16 * 128 / 32;   // 16 ch x 128 notes

    // Per-slot consumer-private delivery mark (rule 2).  Only the consumer reads
    // or writes it, and it is reset to Pending before head_ is released past the
    // slot, so the producer can never observe a stale mark on a reused slot.
    enum : uint8_t { kSlotPending = 0, kSlotDelivered = 1 };

    // ---- consumer-side helpers (audio thread) ----
    //! Try to hand slot `slot` to its plug.  Returns true when the event has been
    //! delivered OR is provably safe to discard (a non-release with no target);
    //! false means "leave it queued and retry next block".
    bool deliverSlot(const NodeProcessContext& ctx, uint32_t slot,
                     int64_t blockStart, int reCh, bool force);
    //! Emit note-offs for every reachable held note.  Returns true when none are
    //! left (i.e. the panic is complete).
    bool emitHeldReleases(const NodeProcessContext& ctx);
    void noteBookkeep(int plug, const MidiEvent& e);
    bool heldTest(int plug, int ch, int note) const;
    void heldSet(int plug, int ch, int note);
    void heldClear(int plug, int ch, int note);
    void heldClearChannel(int plug, int ch);
    void heldClearPlug(int plug);

    TimedMidi             ring_[kRing];
    uint8_t               ringPort_[kRing]  = {0};   // per-event target out-plug
    uint8_t               ringState_[kRing] = {0};   // consumer-private delivered mark
    std::atomic<uint32_t> head_{0};   // consumer (audio) index
    std::atomic<uint32_t> tail_{0};   // producer (message) index
    // Consumer-private sounding-note table: bit (ch*128 + note) per plug.
    uint32_t              heldBits_[kTrackedPlugs][kHeldWords] = {};
    uint16_t              heldChanMask_[kTrackedPlugs] = {};   // channels with any note
    std::atomic<int>      soundingCount_{0};    // published for diagnostics
    std::atomic<bool>     pendingPanic_{false}; // flush()/releaseHeldNotes() armed
    // message-thread-only layout (numPorts()/port() are message-thread queries;
    // the audio thread reads the compiled port count from its NodeProcessContext).
    int                      trackPorts_ = 1;
    std::vector<std::string> portNames_{ std::string("out") };
    std::atomic<int>         outChannel_{-1};   // -1 = passthrough (see setOutChannel)
};

// ---------------------------------------------------------------------------
// MidiOutNode — MIDI sink. process() copies its merged midi-in into a lock-free
// ring the message thread drains (to hardware / the sequencer).  Each element
// carries its absolute due-sample (block start + event offset) so the hardware
// drain can pace the wire to the audio clock instead of bursting.
//
// The events live in per-block buffers, so an event that does not fit CANNOT be
// retried later — there is nowhere to keep it.  Instead the ring RESERVES
// kReleaseHeadroom slots for releases (exactly like MidiInNode::push): once the
// ring gets that full, note-ons are refused so that every note-off / all-notes-
// off still finds a slot.  Dropping a note-on costs one missing note; dropping
// its note-off hangs a hardware synth until you power-cycle it.
// ---------------------------------------------------------------------------
class MidiOutNode : public Node {
public:
    MidiOutNode() = default;
    const char* typeName() const override { return "MidiOutNode"; }
    int      numPorts() const override { return ports_; }
    PortDesc port(int i) const override {
        return PortDesc{ (PortId)i, PortKind::Midi, PortDir::In, 1,
                         i >= 0 && i < (int)portNames_.size()
                            ? portNames_[(size_t)i].c_str() : "midi in" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    // Message thread: dequeue one timestamped event. Returns false if empty.
    bool pop(TimedMidi& out);
    bool pop(MidiEvent& out) { TimedMidi t; if (!pop(t)) return false; out = t.ev; return true; }
    void setPorts(int n) {
        ports_ = n < 1 ? 1 : (n > 32 ? 32 : n);
        portNames_.resize((size_t)ports_);
        for (int i = 0; i < ports_; ++i)
            portNames_[(size_t)i] = "Virtual in " + std::to_string(i + 1);
    }

    //! Diagnostics (message thread): note-ons this node had to refuse because the
    //! ring was inside the release reserve.  Never counts a dropped release.
    unsigned droppedNoteOns() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr int      kRing            = 1024;
    static constexpr uint32_t kReleaseHeadroom = 64;
    TimedMidi             ring_[kRing];
    std::atomic<uint32_t> head_{0};   // consumer (message) index
    std::atomic<uint32_t> tail_{0};   // producer (audio) index
    std::atomic<unsigned> dropped_{0};
    int ports_ = 1;
    std::vector<std::string> portNames_{std::string("Virtual in 1")};
};

// ---------------------------------------------------------------------------
// RecordNode — a live-recording tap for the arrange window.  It has an audio
// input, a MIDI input, and a MIDI THRU output (so the incoming notes still play
// an instrument while you record).  While armed, it timestamps every incoming
// MIDI event with the transport tick and pushes it to a lock-free ring the
// message thread drains to build piano-roll notes.  (Audio capture is exposed
// via the audio-in port for a future audio-to-clip path.)
// ---------------------------------------------------------------------------
// Patch-visible bank of independent virtual MIDI endpoints. Public inputs are
// exposed to track nodes on hidden source ports; hidden track-output sinks feed
// public outputs. There is deliberately NO input->output matrix here.
class VirtualMidiPortsNode : public Node {
public:
    VirtualMidiPortsNode();
    const char* typeName() const override { return "VirtualMidiPortsNode"; }
    int numPorts() const override { return inputs_ + outputs_; }
    PortDesc port(int i) const override {
        if (i < inputs_) return PortDesc{(PortId)i, PortKind::Midi, PortDir::In, 1,
                                         inNames_[(size_t)i].c_str()};
        const int o=i-inputs_;
        return PortDesc{(PortId)(100+o),PortKind::Midi,PortDir::Out,1,
                        outNames_[(size_t)o].c_str()};
    }
    bool prepare(double sr, int) override { sr_ = sr > 0 ? sr : 48000.; return true; }
    void process(const NodeProcessContext& ctx) override;
    void setPorts(int ins, int outs);
    int inputCount() const { return inputs_; }
    int outputCount() const { return outputs_; }
    void setRoute(int output,int input);
    int route(int output) const;
private:
    int inputs_ = 1, outputs_ = 1;
    double sr_ = 48000.;
    std::vector<std::string> inNames_{std::string("Virtual MIDI In 1")};
    std::vector<std::string> outNames_{std::string("Virtual MIDI Out 1")};
    std::atomic<int> routes_[32]; // metadata only; process() never crosses buses
};

// ---------------------------------------------------------------------------
// MidiTrackNode — one arrangement MIDI lane: "_live in" (port 0) is what the
// armed input feeds it, "_playback in" (port 1) is its own sequenced data, and
// "_track out" (port 2) is the merged result.
//
// ROUTING.  setRouting(input, output) names the VIRTUAL MIDI endpoints this lane
// is assigned to (indices into VirtualMidiPortsNode; -1 == "None", the same
// convention VirtualMidiPortsNode::setRoute uses).  Wiring the actual edges is
// the graph compiler's job, but the assignment is honoured HERE too:
//   * input < 0  — the lane has no input, so it neither monitors nor captures
//                  whatever happens to be merged into "_live in".
//   * output < 0 — the lane is not assigned anywhere, so it emits nothing.
//   * changing either endpoint releases every note the lane still has sounding,
//     BEFORE the change takes effect: re-routing mid-note must not strand it on
//     the instrument that is about to be unwired.
//
// OVERFLOW.  The two input lanes carry up to 2*kNodeMidiCap events but "_track
// out" holds kNodeMidiCap, so a merge can overflow.  The tail is NOT truncated:
// it spools into a fixed carry-over buffer and is emitted first next block.
// Only if BOTH are full is anything discarded, and then never a release.
// ---------------------------------------------------------------------------
class MidiTrackNode : public Node {
public:
    struct Ev { int64_t sample; unsigned char status,d1,d2; };
    const char* typeName() const override { return "MidiTrackNode"; }
    int numPorts() const override { return 3; }
    PortDesc port(int i) const override {
        if(i==0) return PortDesc{0,PortKind::Midi,PortDir::In,1,"_live in"};
        if(i==1) return PortDesc{1,PortKind::Midi,PortDir::In,1,"_playback in"};
        return PortDesc{2,PortKind::Midi,PortDir::Out,1,"_track out"};
    }
    bool prepare(double sr,int) override { sr_=sr>0?sr:48000.; return true; }
    void process(const NodeProcessContext& ctx) override;
    void setMonitor(bool on){monitor_.store(on,std::memory_order_release);}
    void setCapture(bool on){if(on)discardQueued();capture_.store(on,std::memory_order_release);
                             if(on)captureReset_.store(true,std::memory_order_release);}
    void setRouting(int input,int output){input_.store(input,std::memory_order_release);
                                          output_.store(output,std::memory_order_release);}
    int input() const{return input_.load(std::memory_order_acquire);}
    int output() const{return output_.load(std::memory_order_acquire);}
    void discardQueued(){head_.store(tail_.load(std::memory_order_acquire),std::memory_order_release);}
    int drain(Ev* out,int cap);
    //! Diagnostics: notes this lane has emitted on "_track out" and not released.
    int soundingNotes() const{return sounding_.load(std::memory_order_relaxed);}
    //! Events spooled by an overflowing merge, still waiting for the next block.
    int carryOver() const{return carryCount_;}
private:
    static constexpr int kHeldWords = 16*128/32;   // 16 ch x 128 notes, 1 bit each
    static constexpr int kCarry     = kNodeMidiCap;

    // consumer-side (audio thread) helpers
    bool spill(const MidiEvent& e);
    void emitOut(MidiBuffer& out,const MidiEvent& e,bool fromLive);
    void releaseHeld(MidiBuffer& out,uint32_t* bits,bool liveOnly);
    void markHeld(const MidiEvent& e,bool fromLive);

    double sr_=48000.;
    std::atomic<bool> monitor_{false},capture_{false};
    std::atomic<bool> captureReset_{true};
    std::atomic<int> input_{-1},output_{-1};
    std::atomic<int> sounding_{0};
    // consumer-private: last observed routing/monitor state (edge detection) and
    // the note tables backing the forced releases described above.
    int  inputState_=0,outputState_=0;
    bool liveOpen_=false;
    uint32_t outHeld_[kHeldWords]={};    // everything emitted on "_track out"
    uint32_t liveHeld_[kHeldWords]={};   // the subset that came from "_live in"
    MidiEvent carry_[kCarry];            // merge overflow, emitted next block
    int carryCount_=0;
    int64_t captureLast_=-1,captureWrap_=0;
    static constexpr int kRing=8192;
    Ev ring_[kRing];
    std::atomic<uint32_t> head_{0},tail_{0};
};

}}} // namespace PatchKnob::engine::patch

#endif // PATCHKNOB_ENGINE_PATCH_PATCH_NODES_H
