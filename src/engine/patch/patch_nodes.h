//----------------------------------------------------------------------------
//  seq24 Windows port — built-in patch nodes.
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
#ifndef SEQ24_ENGINE_PATCH_PATCH_NODES_H
#define SEQ24_ENGINE_PATCH_PATCH_NODES_H

#include <atomic>
#include <vector>

#include "patch_graph.h"

namespace seq24 { namespace engine { namespace patch {

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
    void setActive(bool a) override { if (inst_) inst_->setActive(a); }
    void release() override { if (inst_) inst_->release(); }
    void process(const NodeProcessContext& ctx) override;

    IPluginInstance* instance() const { return inst_; }

private:
    IPluginInstance*      inst_;
    IMidiOutInstance*     midiEmitter_;
    std::vector<PortDesc> ports_;
    bool  hasAudioIn_  = false;
    bool  hasAudioOut_ = false;
    bool  hasMidiOut_  = false;
    MidiEvent midiOutScratch_[kNodeMidiCap];
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
        sr_ = sampleRate; phase_ = 0.0; return true;
    }
    void process(const NodeProcessContext& ctx) override;

    void  setFreq(float f) { freq_.store(f, std::memory_order_relaxed); }
    void  setAmp(float a)  { amp_.store(a,  std::memory_order_relaxed); }

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
    void bindDeviceOut(float* const* out, int channels, int nframes) override {
        target_ = out; targetCh_ = channels; nframes_ = nframes;
    }
    void process(const NodeProcessContext& ctx) override;

private:
    int                 channels_ = 2;
    float* const*       target_   = nullptr;
    int                 targetCh_ = 0;
    int                 nframes_  = 0;
};

// ---------------------------------------------------------------------------
// AudioDeviceInNode — copies engine in[] into its stereo audio-out.
// ---------------------------------------------------------------------------
class AudioDeviceInNode : public Node {
public:
    explicit AudioDeviceInNode(int channels = 2) : channels_(channels) {}
    const char* typeName() const override { return "AudioDeviceInNode"; }
    int      numPorts() const override { return 1; }
    PortDesc port(int) const override {
        return PortDesc{ 0, PortKind::Audio, PortDir::Out,
                         (uint16_t)channels_, "out" };
    }
    bool prepare(double, int) override { return true; }
    void bindDeviceIn(const float* const* in, int channels, int nframes) override {
        src_ = in; srcCh_ = channels; nframes_ = nframes;
    }
    void process(const NodeProcessContext& ctx) override;

private:
    int                 channels_ = 2;
    const float* const* src_      = nullptr;
    int                 srcCh_    = 0;
    int                 nframes_  = 0;
};

// ---------------------------------------------------------------------------
// MidiInNode — sequencer/hardware MIDI source. The message thread pushes events
// into a lock-free SPSC ring; process() drains them into the midi-out port.
// ---------------------------------------------------------------------------
class MidiInNode : public Node {
public:
    MidiInNode() = default;
    const char* typeName() const override { return "MidiInNode"; }
    int      numPorts() const override { return 1; }
    PortDesc port(int) const override {
        return PortDesc{ 0, PortKind::Midi, PortDir::Out, 1, "midi out" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    // Message thread (or test): enqueue one event. Returns false if the ring is
    // full (dropped). Lock-free.
    bool push(const MidiEvent& e);

private:
    static constexpr int kRing = 1024;
    MidiEvent             ring_[kRing];
    std::atomic<uint32_t> head_{0};   // consumer (audio) index
    std::atomic<uint32_t> tail_{0};   // producer (message) index
};

// ---------------------------------------------------------------------------
// MidiOutNode — MIDI sink. process() copies its merged midi-in into a lock-free
// ring the message thread drains (to hardware / the sequencer).
// ---------------------------------------------------------------------------
class MidiOutNode : public Node {
public:
    MidiOutNode() = default;
    const char* typeName() const override { return "MidiOutNode"; }
    int      numPorts() const override { return 1; }
    PortDesc port(int) const override {
        return PortDesc{ 0, PortKind::Midi, PortDir::In, 1, "midi in" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    // Message thread: dequeue one event. Returns false if empty.
    bool pop(MidiEvent& out);

private:
    static constexpr int kRing = 1024;
    MidiEvent             ring_[kRing];
    std::atomic<uint32_t> head_{0};   // consumer (message) index
    std::atomic<uint32_t> tail_{0};   // producer (audio) index
};

}}} // namespace seq24::engine::patch

#endif // SEQ24_ENGINE_PATCH_PATCH_NODES_H
