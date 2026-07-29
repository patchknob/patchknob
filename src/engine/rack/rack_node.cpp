//----------------------------------------------------------------------------
//  src/engine/rack/rack_node.cpp
//----------------------------------------------------------------------------
#include "rack_node.h"

namespace PatchKnob { namespace engine { namespace patch {

bool RackNode::prepare(double sampleRate, int maxBlock) {
    (void)maxBlock;
    engine_.setSampleRate(sampleRate);
    engine_.ensureDefaultIO();          // always at least an Audio Out to patch to
    return true;
}

void RackNode::process(const NodeProcessContext& ctx) {
    const int n = ctx.nframes;
    static constexpr int kMaxAudioMods = 32;   // per-module audio-in/out ports

    // MIDI IN (already merged, offset-sorted).  When assigned to a clip's channel,
    // keep only that channel's voice messages (per-clip routing).
    const PatchKnob::engine::MidiEvent* midi = nullptr;
    int nMidi = 0;
    if (ctx.numMidiIn > 0) {
        const int filt = midiChannel_.load(std::memory_order_relaxed);
        if (filt < 0) { midi = ctx.midiIn[0].ev; nMidi = ctx.midiIn[0].count; }
        else {
            const MidiBuffer& mb = ctx.midiIn[0];
            int fn = 0;
            for (int i = 0; i < mb.count && fn < kNodeMidiCap; ++i) {
                const MidiEvent& m = mb.ev[i];
                const unsigned char hi = m.status & 0xF0u;
                if (hi >= 0x80u && hi < 0xF0u) {           // channel voice msg
                    if ((m.status & 0x0Fu) == (unsigned)filt) midiFilterScratch_[fn++] = m;
                } else midiFilterScratch_[fn++] = m;        // system/realtime: pass
            }
            midi = midiFilterScratch_; nMidi = fn;
        }
    }

    // One stereo IN port per AudioIn module, one stereo OUT port per AudioOut
    // module (port order == module order == the bus order the graph hands us).
    const float* insL[kMaxAudioMods] = { nullptr };
    const float* insR[kMaxAudioMods] = { nullptr };
    const int numIns = ctx.numAudioIn < kMaxAudioMods ? ctx.numAudioIn : kMaxAudioMods;
    for (int k = 0; k < numIns; ++k) {
        const AudioBus& b = ctx.audioIn[k];
        if (b.channels > 0) { insL[k] = b.chans[0]; insR[k] = b.channels > 1 ? b.chans[1] : b.chans[0]; }
    }
    float* outsL[kMaxAudioMods] = { nullptr };
    float* outsR[kMaxAudioMods] = { nullptr };
    const int numOuts = ctx.numAudioOut < kMaxAudioMods ? ctx.numAudioOut : kMaxAudioMods;
    for (int k = 0; k < numOuts; ++k) {
        AudioBus& b = ctx.audioOut[k];
        if (b.channels > 0) { outsL[k] = b.chans[0]; outsR[k] = b.channels > 1 ? b.chans[1] : nullptr; }
    }

    engine_.processMulti(n, insL, insR, numIns, outsL, outsR, numOuts,
                         midi, nMidi, (float)ctx.transport.tempoBpm, ctx.transport.isPlaying);
}

}}} // namespace PatchKnob::engine::patch
