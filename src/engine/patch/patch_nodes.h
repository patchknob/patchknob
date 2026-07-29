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

private:
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
                                                 vu_[i].store(0.f); pan_[i].store(0.f); } }

    const char* typeName() const override { return "MixerNode"; }
    int      numPorts() const override { return channels_.load(std::memory_order_relaxed) + 1; }
    // Port 0 is the OUTPUT (stable id, so its wiring survives channel changes);
    // ports 1..channels are the input channels.  Input channel `ch` == port id
    // ch+1 == the ch-th audio-in the graph packs into process().
    PortDesc port(int i) const override {
        if (i == 0) return PortDesc{ 0, PortKind::Audio, PortDir::Out, 2, "out" };
        return PortDesc{ (PortId)i, PortKind::Audio, PortDir::In, 2, "ch" };
    }
    bool prepare(double, int) override { return true; }
    void process(const NodeProcessContext& ctx) override;

    int   channels() const { return channels_.load(std::memory_order_relaxed); }
    void  setChannels(int n) { channels_.store(n < 1 ? 1 : (n > kMaxChannels ? kMaxChannels : n),
                                               std::memory_order_relaxed); }
    void  setGain(int ch, float g) { if (ch >= 0 && ch < kMaxChannels) gain_[ch].store(g); }
    float gain(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? gain_[ch].load() : 0.f; }
    void  setMute(int ch, bool m) { if (ch >= 0 && ch < kMaxChannels) mute_[ch].store(m); }
    bool  mute(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? mute_[ch].load() : false; }
    float vu(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? vu_[ch].load() : 0.f; }
    // master (output) strip: a final gain + its own VU on the summed output.
    void  setMasterGain(float g) { masterGain_.store(g); }
    float masterGain() const { return masterGain_.load(); }
    float masterVu() const { return masterVu_.load(); }
    // per-channel stereo pan (-1 left .. 0 centre .. +1 right).
    void  setPan(int ch, float p) { if (ch >= 0 && ch < kMaxChannels)
                                        pan_[ch].store(p < -1.f ? -1.f : (p > 1.f ? 1.f : p)); }
    float pan(int ch) const { return (ch >= 0 && ch < kMaxChannels) ? pan_[ch].load() : 0.f; }
    void  setVu(int ch, float v) { if (ch >= 0 && ch < kMaxChannels) vu_[ch].store(v); }
    void  setMasterVu(float v) { masterVu_.store(v); }

private:
    // Atomic because the audio thread reads it in process() while the message
    // thread adjusts it via setChannels() (the graph recompiles afterwards).
    std::atomic<int> channels_{4};
    std::atomic<float> gain_[kMaxChannels];
    std::atomic<bool>  mute_[kMaxChannels];
    std::atomic<float> vu_[kMaxChannels];
    std::atomic<float> pan_[kMaxChannels];
    std::atomic<float> masterGain_{1.0f};
    std::atomic<float> masterVu_{0.f};
};

// ---------------------------------------------------------------------------
// MasterMixerNode -- the singleton master HUB.  Rather than hosting nodes, it
// grows PORTS as tracks are added: each track is a channel with an INLET + an
// OUTLET of its kind (MIDI for instrument tracks, AUDIO for audio tracks), so
// you patch the processing yourself in the modular view.
//   fixed ports:  0 = master audio OUT (the mix),  1 = MIDI clock OUT.
//   per track t:  inlet = port(2 + 2t),  outlet = port(3 + 2t).
// Audio channels are summed (gain/pan/mute) into the master out AND their raw
// inlet is echoed to their outlet (a direct/insert send); MIDI channels pass
// their inlet straight to their outlet (thru).  Tempo-synced MIDI clock on
// port 1.  channels() (from MixerNode) == number of tracks, so the strip view +
// audio_app_mixer_* address each track's gain/mute/pan/vu.
// ---------------------------------------------------------------------------
class MasterMixerNode : public MixerNode {
public:
    struct Trk { bool midi; };
    MasterMixerNode() : MixerNode(1) { setChannels(0); publishSnapshot(); }
    ~MasterMixerNode() override;

    // message thread: grow / shrink the per-track ports.  Each edit freezes the
    // track layout into a new immutable snapshot for the audio thread (RCU).
    int  addTrack(bool midi) { tracks_.push_back(Trk{ midi }); setChannels((int)tracks_.size()); publishSnapshot(); return (int)tracks_.size() - 1; }
    void removeTrack(int i)  { if (i >= 0 && i < (int)tracks_.size()) { tracks_.erase(tracks_.begin() + i); setChannels((int)tracks_.size()); publishSnapshot(); } }
    int  trackCount() const  { return (int)tracks_.size(); }
    bool trackIsMidi(int i) const { return i >= 0 && i < (int)tracks_.size() && tracks_[i].midi; }

    const char* typeName() const override { return "MasterMixerNode"; }
    int numPorts() const override { return 2 + (int)tracks_.size() * 2; }
    PortDesc port(int i) const override {
        if (i == 0) return PortDesc{ 0, PortKind::Audio, PortDir::Out, 2, "master" };
        if (i == 1) return PortDesc{ 1, PortKind::Midi,  PortDir::Out, 1, "clock"  };
        const int  t      = (i - 2) / 2;
        const bool outlet = ((i - 2) & 1) != 0;
        const bool midi   = (t < (int)tracks_.size()) ? tracks_[t].midi : false;
        return PortDesc{ (PortId)i, midi ? PortKind::Midi : PortKind::Audio,
                         outlet ? PortDir::Out : PortDir::In,
                         (uint16_t)(midi ? 1 : 2), outlet ? "out" : "in" };
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
    struct TrackSnap { int count; bool midi[kMaxChannels]; };
    struct Retired   { TrackSnap* p; uint64_t gen; };

    void publishSnapshot();   // message thread: freeze tracks_ -> snap_, retire old

    std::vector<Trk>        tracks_;
    std::atomic<TrackSnap*> snap_{nullptr};
    std::vector<Retired>    retired_;      // message thread only
    std::atomic<uint64_t>   rtGen_{0};     // audio-thread block counter
    double sr_       = 48000.0;
    double clockAcc_ = 0.0;
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

// A MIDI event stamped with its ABSOLUTE due-time in samples (transport
// domain).  dueSample < 0 means "due immediately" (delivered at block start).
struct TimedMidi {
    int64_t   dueSample;
    MidiEvent ev;
};

// ---------------------------------------------------------------------------
// MidiInNode — sequencer/hardware MIDI source. The message thread pushes
// TIMESTAMPED events into a lock-free SPSC ring; process() delivers only the
// events due within the current block, at exact sample offsets, holding back
// future events (the producer schedules ahead of the playhead).
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
    // (dueSample < 0 = "now") on OUT-PLUG `port` (track index; clamps into range).
    // Ring order must be non-decreasing dueSample (single monotonically-scheduling
    // producer).  Returns false if full.
    bool push(const MidiEvent& e, int64_t dueSample, int port);
    bool push(const MidiEvent& e, int64_t dueSample) { return push(e, dueSample, 0); }
    bool push(const MidiEvent& e) { return push(e, -1, 0); }   // legacy "now"

    // Audio thread (the same thread that pushes AND processes): discard every
    // queued event.  Used on transport discontinuities (locate/stop) so stale
    // scheduled-ahead events can't fire at the new position.
    void flush() { head_.store(tail_.load(std::memory_order_acquire),
                               std::memory_order_release); }

private:
    static constexpr int kRing = 1024;
    TimedMidi             ring_[kRing];
    uint8_t               ringPort_[kRing] = {0};   // per-event target out-plug
    std::atomic<uint32_t> head_{0};   // consumer (audio) index
    std::atomic<uint32_t> tail_{0};   // producer (message) index
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

    // Message thread: dequeue one timestamped event. Returns false if empty.
    bool pop(TimedMidi& out);
    bool pop(MidiEvent& out) { TimedMidi t; if (!pop(t)) return false; out = t.ev; return true; }

private:
    static constexpr int kRing = 1024;
    TimedMidi             ring_[kRing];
    std::atomic<uint32_t> head_{0};   // consumer (message) index
    std::atomic<uint32_t> tail_{0};   // producer (audio) index
};

// ---------------------------------------------------------------------------
// RecordNode — a live-recording tap for the arrange window.  It has an audio
// input, a MIDI input, and a MIDI THRU output (so the incoming notes still play
// an instrument while you record).  While armed, it timestamps every incoming
// MIDI event with the transport tick and pushes it to a lock-free ring the
// message thread drains to build piano-roll notes.  (Audio capture is exposed
// via the audio-in port for a future audio-to-clip path.)
// ---------------------------------------------------------------------------
class RecordNode : public Node {
public:
    struct Ev { long tick; unsigned char status, d1, d2; };

    RecordNode() = default;
    const char* typeName() const override { return "RecordNode"; }
    int      numPorts() const override { return 3; }
    PortDesc port(int i) const override {
        if (i == 0) return PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "audio in" };
        if (i == 1) return PortDesc{ 1, PortKind::Midi,  PortDir::In,  1, "midi in" };
        return             PortDesc{ 2, PortKind::Midi,  PortDir::Out, 1, "thru" };
    }
    bool prepare(double sr, int) override { sr_ = sr > 0 ? sr : 48000.0; return true; }
    void process(const NodeProcessContext& ctx) override;

    void setRecording(bool r) { recording_.store(r, std::memory_order_relaxed); }
    bool recording() const    { return recording_.load(std::memory_order_relaxed); }
    //! Drain captured events (message thread).  Returns the count written to out.
    int  drain(Ev* out, int cap);

private:
    double sr_ = 48000.0;
    std::atomic<bool>     recording_{false};
    static constexpr int  kRing = 8192;
    Ev                    ring_[kRing];
    std::atomic<uint32_t> head_{0};   // consumer (message)
    std::atomic<uint32_t> tail_{0};   // producer (audio)
};

}}} // namespace PatchKnob::engine::patch

#endif // PATCHKNOB_ENGINE_PATCH_PATCH_NODES_H
