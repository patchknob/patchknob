//----------------------------------------------------------------------------
//  seq24 Windows port — built-in patch node implementations.
//  See patch_nodes.h for the design of each node.
//----------------------------------------------------------------------------
#include "patch_nodes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace seq24 { namespace engine { namespace patch {

// ===========================================================================
// PluginNode
// ===========================================================================
PluginNode::PluginNode(IPluginInstance* inst, IMidiOutInstance* midiOut)
    : inst_(inst), midiEmitter_(midiOut) {
    // Derive a fixed port layout from the plugin descriptor. Order matters: it
    // defines the index each port occupies in NodeProcessContext's arrays.
    PortId nextId = 0;
    if (inst_) {
        const PluginDescriptor& d = inst_->descriptor();
        if (d.numAudioIn > 0) {
            hasAudioIn_ = true;
            const uint16_t ch = (uint16_t)std::min(d.numAudioIn, kMaxBusChan);
            ports_.push_back(PortDesc{ nextId++, PortKind::Audio, PortDir::In,  ch, "in" });
        }
        if (d.numAudioOut > 0) {
            hasAudioOut_ = true;
            const uint16_t ch = (uint16_t)std::min(d.numAudioOut, kMaxBusChan);
            ports_.push_back(PortDesc{ nextId++, PortKind::Audio, PortDir::Out, ch, "out" });
        }
    }
    // Every plugin gets a MIDI-in port (harmless if unconnected) so instruments
    // and MIDI-FX can be driven; a MIDI-out port is exposed only when the wrapper
    // has an emitter to pull from.
    ports_.push_back(PortDesc{ nextId++, PortKind::Midi, PortDir::In, 1, "midi in" });
    if (midiEmitter_) {
        hasMidiOut_ = true;
        ports_.push_back(PortDesc{ nextId++, PortKind::Midi, PortDir::Out, 1, "midi out" });
    }
}

bool PluginNode::prepare(double sampleRate, int maxBlock) {
    return inst_ ? inst_->prepare(sampleRate, maxBlock) : false;
}

void PluginNode::process(const NodeProcessContext& ctx) {
    if (!inst_) return;
    const int n = ctx.nframes;

    // Audio-out buffers are node-private pool slots; clear them so a synth that
    // ADDS (rather than replaces) starts from silence — matches Track's synth path.
    if (hasAudioOut_ && ctx.numAudioOut > 0)
        for (int c = 0; c < ctx.audioOut[0].channels; ++c)
            std::memset(ctx.audioOut[0].chans[c], 0, sizeof(float) * (size_t)n);

    ProcessBlock blk;
    blk.audioIn  = (hasAudioIn_  && ctx.numAudioIn  > 0) ? ctx.audioIn[0].chans  : nullptr;
    blk.audioOut = (hasAudioOut_ && ctx.numAudioOut > 0) ? ctx.audioOut[0].chans : nullptr;
    blk.nframes  = n;
    if (ctx.numMidiIn > 0) { blk.midiIn = ctx.midiIn[0].ev; blk.numMidiIn = ctx.midiIn[0].count; }
    else                   { blk.midiIn = nullptr;          blk.numMidiIn = 0; }
    blk.paramIn             = ctx.paramIn;
    blk.numParamIn          = ctx.numParamIn;
    blk.tempoBpm            = ctx.transport.tempoBpm;
    blk.playPositionSamples = ctx.transport.playPositionSamples;
    blk.isPlaying           = ctx.transport.isPlaying;

    inst_->process(blk);

    // Capture plugin MIDI-out in the wrapper (frozen ProcessBlock has no field
    // for it). Publish onto the MIDI-out port for downstream nodes.
    if (hasMidiOut_ && ctx.numMidiOut > 0) {
        int produced = 0;
        if (midiEmitter_)
            produced = midiEmitter_->pullMidiOut(midiOutScratch_, kNodeMidiCap);
        if (produced < 0) produced = 0;
        if (produced > ctx.midiOut[0].capacity) produced = ctx.midiOut[0].capacity;
        for (int i = 0; i < produced; ++i) ctx.midiOut[0].ev[i] = midiOutScratch_[i];
        ctx.midiOut[0].count = produced;
    }
}

// ===========================================================================
// SineSourceNode
// ===========================================================================
void SineSourceNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const float freq = freq_.load(std::memory_order_relaxed);
    const float amp  = amp_.load(std::memory_order_relaxed);
    const double inc = 2.0 * 3.14159265358979323846 * (double)freq / sr_;

    const AudioBus& out = ctx.audioOut[0];
    double ph = phase_;
    for (int i = 0; i < n; ++i) {
        const float s = amp * (float)std::sin(ph);
        for (int c = 0; c < out.channels; ++c) out.chans[c][i] = s;
        ph += inc;
        if (ph >= 2.0 * 3.14159265358979323846) ph -= 2.0 * 3.14159265358979323846;
    }
    phase_ = ph;
}

// ===========================================================================
// GainNode
// ===========================================================================
void GainNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioIn < 1 || ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const float g = gain_.load(std::memory_order_relaxed);
    const AudioBus& in  = ctx.audioIn[0];
    const AudioBus& out = ctx.audioOut[0];
    const int cc = std::min(in.channels, out.channels);
    for (int c = 0; c < cc; ++c) {
        const float* sp = in.chans[c];
        float*       dp = out.chans[c];
        for (int i = 0; i < n; ++i) dp[i] = sp[i] * g;
    }
    for (int c = cc; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
}

// ===========================================================================
// SumNode
// ===========================================================================
void SumNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& out = ctx.audioOut[0];
    if (ctx.numAudioIn >= 1) {
        const AudioBus& in = ctx.audioIn[0];
        const int cc = std::min(in.channels, out.channels);
        for (int c = 0; c < cc; ++c)
            std::memcpy(out.chans[c], in.chans[c], sizeof(float) * (size_t)n);
        for (int c = cc; c < out.channels; ++c)
            std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
    } else {
        for (int c = 0; c < out.channels; ++c)
            std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
    }
    if (out.channels > 0) vuL_.push(out.chans[0], n);
    if (out.channels > 1) vuR_.push(out.chans[1], n);
}

// ===========================================================================
// AudioDeviceOutNode
// ===========================================================================
void AudioDeviceOutNode::process(const NodeProcessContext& ctx) {
    if (!target_ || ctx.numAudioIn < 1) return;
    const int n = ctx.nframes;
    const AudioBus& in = ctx.audioIn[0];
    const int cc = std::min(targetCh_, in.channels);
    for (int c = 0; c < cc; ++c)
        if (target_[c])
            std::memcpy(target_[c], in.chans[c], sizeof(float) * (size_t)n);
    // Extra device channels were pre-zeroed by PatchGraph::process().
}

// ===========================================================================
// AudioDeviceInNode
// ===========================================================================
void AudioDeviceInNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& out = ctx.audioOut[0];
    const int cc = (src_ ? std::min(srcCh_, out.channels) : 0);
    for (int c = 0; c < cc; ++c) {
        if (src_[c]) std::memcpy(out.chans[c], src_[c], sizeof(float) * (size_t)n);
        else         std::memset(out.chans[c], 0,        sizeof(float) * (size_t)n);
    }
    for (int c = cc; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
}

// ===========================================================================
// MidiInNode — lock-free SPSC ring
// ===========================================================================
bool MidiInNode::push(const MidiEvent& e) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t next = (t + 1) % kRing;
    if (next == head_.load(std::memory_order_acquire)) return false;  // full
    ring_[t] = e;
    tail_.store(next, std::memory_order_release);
    return true;
}

void MidiInNode::process(const NodeProcessContext& ctx) {
    if (ctx.numMidiOut < 1) return;
    MidiBuffer& out = ctx.midiOut[0];
    int count = 0;

    uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    while (h != t && count < out.capacity) {
        out.ev[count++] = ring_[h];
        h = (h + 1) % kRing;
    }
    head_.store(h, std::memory_order_release);

    // Keep events sorted by sample offset (stable insertion sort; small n).
    for (int i = 1; i < count; ++i) {
        MidiEvent key = out.ev[i];
        int j = i - 1;
        while (j >= 0 && out.ev[j].sampleOffset > key.sampleOffset) {
            out.ev[j + 1] = out.ev[j]; --j;
        }
        out.ev[j + 1] = key;
    }
    out.count = count;
}

// ===========================================================================
// MidiOutNode — lock-free SPSC ring
// ===========================================================================
void MidiOutNode::process(const NodeProcessContext& ctx) {
    if (ctx.numMidiIn < 1) return;
    const MidiBuffer& in = ctx.midiIn[0];
    uint32_t t = tail_.load(std::memory_order_relaxed);
    for (int i = 0; i < in.count; ++i) {
        const uint32_t next = (t + 1) % kRing;
        if (next == head_.load(std::memory_order_acquire)) break;  // full: drop rest
        ring_[t] = in.ev[i];
        t = next;
    }
    tail_.store(t, std::memory_order_release);
}

bool MidiOutNode::pop(MidiEvent& out) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return false;  // empty
    out = ring_[h];
    head_.store((h + 1) % kRing, std::memory_order_release);
    return true;
}

}}} // namespace seq24::engine::patch
