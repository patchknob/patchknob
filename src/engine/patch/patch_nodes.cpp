//----------------------------------------------------------------------------
//  PatchKnob — built-in patch node implementations.
//  See patch_nodes.h for the design of each node.
//----------------------------------------------------------------------------
#include "patch_nodes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <thread>

namespace PatchKnob { namespace engine { namespace patch {

// ===========================================================================
// PluginNode
// ===========================================================================
PluginNode::PluginNode(IPluginInstance* inst, IMidiOutInstance* midiOut)
    : inst_(inst), midiEmitter_(midiOut) {
    // UNIFORM port layout for every plugin node: stereo audio in + stereo audio
    // out + MIDI in.  This lets ANY plugin -- instrument OR effect -- occupy the
    // node and be hot-swapped (Choose Plugin) without changing the node's ports,
    // so existing wiring survives the swap.  An instrument simply ignores its
    // (unconnected) audio input; an effect uses it.  The wrapper feeds/reads only
    // as many channels as the actual plugin exposes.
    PortId nextId = 0;
    hasAudioIn_  = true;
    ports_.push_back(PortDesc{ nextId++, PortKind::Audio, PortDir::In,  2, "in" });
    hasAudioOut_ = true;
    ports_.push_back(PortDesc{ nextId++, PortKind::Audio, PortDir::Out, 2, "out" });
    ports_.push_back(PortDesc{ nextId++, PortKind::Midi,  PortDir::In,  1, "midi in" });
    if (midiEmitter_) {
        hasMidiOut_ = true;
        ports_.push_back(PortDesc{ nextId++, PortKind::Midi, PortDir::Out, 1, "midi out" });
    }
}

bool PluginNode::prepare(double sampleRate, int maxBlock) {
    IPluginInstance* p = inst_.load(std::memory_order_acquire);
    return p ? p->prepare(sampleRate, maxBlock) : false;
}

IPluginInstance* PluginNode::swapInstance(IPluginInstance* i) {
    // Publish the new instance FIRST, then wait for any in-flight process() to
    // drain.  Both the exchange and the flag reads are seq_cst so they pair with
    // the store/load at the top of process(): once inFlight_ reads false, every
    // running or future block is provably using the NEW pointer, so the old one
    // is safe to hand back for disposal.  This blocks the message thread for at
    // most one audio block — never the audio thread, and never a fixed sleep.
    IPluginInstance* old = inst_.exchange(i, std::memory_order_seq_cst);
    while (inFlight_.load(std::memory_order_seq_cst))
        std::this_thread::yield();
    return old;
}

void PluginNode::setParamNormalized(uint32_t id, float v) {
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;

    if (IPluginInstance* inst = inst_.load(std::memory_order_acquire))
        inst->setParamNormalized(id, v);

    unsigned head = paramHead_.load(std::memory_order_relaxed);
    unsigned tail = paramTail_.load(std::memory_order_acquire);
    if (head - tail >= kParamQueue)
        return;
    paramQueue_[head % kParamQueue] = PendingParam{ id, v };
    paramHead_.store(head + 1, std::memory_order_release);
}

void PluginNode::process(const NodeProcessContext& ctx) {
    // RT gate for swapInstance(): announce "in flight" BEFORE loading the
    // instance pointer (seq_cst, Dekker-style — see swapInstance above).
    //
    // The clear is RAII.  runStep() deliberately swallows a throwing node
    // (patch_graph.cpp), so a plugin that throws out of process() used to leave
    // inFlight_ stuck true forever — and the next swapInstance() then spun on it
    // on the MESSAGE thread and never returned, wedging the UI.  A scope guard
    // makes the drain flag honest on every exit path, normal or exceptional.
    struct FlightGuard {
        std::atomic<bool>& f;
        ~FlightGuard() { f.store(false, std::memory_order_release); }
    } flight{ inFlight_ };
    inFlight_.store(true, std::memory_order_seq_cst);
    IPluginInstance* inst = inst_.load(std::memory_order_seq_cst);
    const int n = ctx.nframes;

    // Audio-out buffers are node-private pool slots; clear them so a synth that
    // ADDS (rather than replaces) starts from silence — matches Track's synth path.
    //
    // This MUST happen before the empty-slot bail-out below.  Pool slots are
    // reassigned on every compileAndPublish(), so an EMPTY plugin node ("Add
    // Effect", plugin chosen later) that returned without clearing re-published
    // whatever a deleted node had left in that slot — a full-level DC/noise buzz
    // that ran forever.
    if (hasAudioOut_ && ctx.numAudioOut > 0)
        for (int c = 0; c < ctx.audioOut[0].channels; ++c)
            std::memset(ctx.audioOut[0].chans[c], 0, sizeof(float) * (size_t)n);
    // Same for the MIDI-out port: an empty slot publishes an empty buffer rather
    // than leaving the previous tenant's event count standing.
    if (hasMidiOut_ && ctx.numMidiOut > 0) ctx.midiOut[0].count = 0;
    if (!inst) return;

    ProcessBlock blk;
    blk.audioIn  = (hasAudioIn_  && ctx.numAudioIn  > 0) ? ctx.audioIn[0].chans  : nullptr;
    blk.audioOut = (hasAudioOut_ && ctx.numAudioOut > 0) ? ctx.audioOut[0].chans : nullptr;
    blk.nframes  = n;
    if (ctx.numMidiIn > 0) {
        const int ch = midiChannel_.load(std::memory_order_relaxed);
        if (ch < 0) {                                   // omni: pass everything
            blk.midiIn = ctx.midiIn[0].ev; blk.numMidiIn = ctx.midiIn[0].count;
        } else {                                        // keep only this channel (+ system msgs)
            int fn = 0;
            for (int i = 0; i < ctx.midiIn[0].count && fn < kNodeMidiCap; ++i) {
                const MidiEvent& m = ctx.midiIn[0].ev[i];
                const unsigned char hi = m.status & 0xF0u;
                if (hi >= 0x80u && hi < 0xF0u) { if ((m.status & 0x0Fu) == (unsigned)ch) midiFilterScratch_[fn++] = m; }
                else                            midiFilterScratch_[fn++] = m;   // realtime/system
            }
            blk.midiIn = midiFilterScratch_; blk.numMidiIn = fn;
        }
    } else { blk.midiIn = nullptr; blk.numMidiIn = 0; }
    int paramCount = 0;
    if (ctx.paramIn) {
        for (int i = 0; i < ctx.numParamIn && paramCount < kParamQueue; ++i)
            paramScratch_[paramCount++] = ctx.paramIn[i];
    }
    unsigned tail = paramTail_.load(std::memory_order_relaxed);
    const unsigned head = paramHead_.load(std::memory_order_acquire);
    while (tail != head && paramCount < kParamQueue) {
        const PendingParam& p = paramQueue_[tail % kParamQueue];
        ParamChange& pc = paramScratch_[paramCount++];
        pc.id = p.id;
        pc.sampleOffset = 0;
        pc.value = p.value;
        ++tail;
    }
    paramTail_.store(tail, std::memory_order_release);

    blk.paramIn             = paramCount > 0 ? paramScratch_ : nullptr;
    blk.numParamIn          = paramCount;
    blk.tempoBpm            = ctx.transport.tempoBpm;
    blk.playPositionSamples = ctx.transport.playPositionSamples;
    blk.isPlaying           = ctx.transport.isPlaying;
    // Tell the host how many channel pointers audioIn/audioOut REALLY hold, so
    // it can clamp a plugin that declares more I/O than our bus carries.
    blk.numAudioIn  = blk.audioIn  ? ctx.audioIn[0].channels  : 0;
    blk.numAudioOut = blk.audioOut ? ctx.audioOut[0].channels : 0;

    inst->process(blk);

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
    // inFlight_ is cleared by FlightGuard above (also on a throwing plugin).
}

// ===========================================================================
// SineSourceNode
// ===========================================================================
void SineSourceNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const float freq = freq_.load(std::memory_order_relaxed);
    const float amp  = amp_.load(std::memory_order_relaxed);
    // sr_ is clamped > 0 in prepare(); still guard the increment so a rogue
    // freq/amp (NaN/Inf from a UI glitch) can never emit non-finite samples.
    double inc = 2.0 * 3.14159265358979323846 * (double)freq / sr_;
    if (!std::isfinite(inc)) inc = 0.0;

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
// TrackFaderNode -- Ardour's Amp.  Ramped gain (Amp::apply_gain interpolates
// between the previous and the target gain across the block, which is what
// stops a fader drag from producing zipper noise) + a stereo peak tap.
// ===========================================================================
void TrackFaderNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& out = ctx.audioOut[0];
    if (ctx.numAudioIn < 1) {
        for (int c = 0; c < out.channels; ++c)
            std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
        peakL_.store(0.f, std::memory_order_relaxed);
        peakR_.store(0.f, std::memory_order_relaxed);
        return;
    }

    const float target = mute_.load(std::memory_order_relaxed)
                       ? 0.f : gain_.load(std::memory_order_relaxed);
    const float start  = cur_;
    const float step   = (n > 0) ? (target - start) / (float)n : 0.f;
    const bool  ramp   = std::fabs(target - start) > 1e-6f;

    const AudioBus& in = ctx.audioIn[0];
    const int cc = std::min(in.channels, out.channels);
    float peakL = 0.f, peakR = 0.f;
    for (int c = 0; c < cc; ++c) {
        const float* sp = in.chans[c];
        float*       dp = out.chans[c];
        float        g  = start;
        for (int i = 0; i < n; ++i) {
            if (ramp) g += step;
            const float v = sp[i] * (ramp ? g : target);
            dp[i] = v;
            const float a = std::fabs(v);
            if (c == 0) { if (a > peakL) peakL = a; }
            else if (c == 1) { if (a > peakR) peakR = a; }
        }
    }
    for (int c = cc; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
    cur_ = target;
    peakL_.store(std::isfinite(peakL) ? peakL : 0.f, std::memory_order_relaxed);
    peakR_.store(std::isfinite(peakR) ? peakR : (std::isfinite(peakL) ? peakL : 0.f),
                 std::memory_order_relaxed);
}

void MonoToStereoNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const AudioBus& out = ctx.audioOut[0];
    const float* src = (ctx.numAudioIn > 0 && ctx.audioIn[0].channels > 0)
                     ? ctx.audioIn[0].chans[0] : nullptr;
    for (int i = 0; i < ctx.nframes; ++i) {
        const float v = src && std::isfinite(src[i]) ? src[i] : 0.f;
        if (out.channels > 0) out.chans[0][i] = v;
        if (out.channels > 1) out.chans[1][i] = v;
    }
    for (int c = 2; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)ctx.nframes);
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
// MixerNode
// ===========================================================================
void MixerNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& out = ctx.audioOut[0];
    for (int c = 0; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);

    const int nin = std::min(channels_.load(std::memory_order_relaxed), ctx.numAudioIn);
    for (int ch = 0; ch < nin; ++ch) {
        const AudioBus& in = ctx.audioIn[ch];

        // ---- RECORD TAP: PRE-FADER, and taken even when the channel is muted.
        // It used to sit below, after the gain/pan maths, and inside the branch
        // the mute `continue` skips -- so riding the fader was baked into the
        // take and arming a muted channel recorded pure silence.  Every DAW taps
        // the channel input; the strip's gain/pan/mute belong to MONITORING.
        for (int slot = 0; slot < kMaxCaptureTaps; ++slot) {
            if (capChan_[slot].load(std::memory_order_relaxed) != ch) continue;
            float* capture = capBuf_[slot].load(std::memory_order_acquire);
            if (!capture) break;
            size_t pos = capPos_[slot].load(std::memory_order_relaxed);
            const size_t cap = capCap_[slot].load(std::memory_order_relaxed);
            const float* L = in.channels > 0 ? in.chans[0] : nullptr;
            const float* R = in.channels > 1 ? in.chans[1] : L;
            for (int i = 0; i < n && pos < cap; ++i, ++pos) {
                const float l = L ? L[i] : 0.f;
                const float r = R ? R[i] : l;
                capture[pos * 2]     = std::isfinite(l) ? l : 0.f;
                capture[pos * 2 + 1] = std::isfinite(r) ? r : 0.f;
            }
            capPos_[slot].store(pos, std::memory_order_release);
            break;                    // one tap per channel
        }

        if (mute_[ch].load(std::memory_order_relaxed)) {
            clearChannelMeter(ch);          // muted: contribute no peak
            continue;
        }
        // ---- PER-CHANNEL NaN/Inf FENCE ------------------------------------
        // The master stage below is also a fence, but it runs AFTER the sum, so
        // one poisoned channel (a misbehaving rack/VST/Csound node) turned the
        // whole mix bus into NaN and every other track went silent with it.
        // Sanitising the coefficients once per channel plus the sample as it is
        // read isolates the offender: it contributes silence, nothing else does.
        float g = gain_[ch].load(std::memory_order_relaxed);
        const float p = pan_[ch].load(std::memory_order_relaxed);
        float panL = p <= 0.f ? 1.f : 1.f - p;    // constant-gain stereo pan
        float panR = p >= 0.f ? 1.f : 1.f + p;
        if (!std::isfinite(g))    g    = 0.f;
        if (!std::isfinite(panL)) panL = 0.f;
        if (!std::isfinite(panR)) panR = 0.f;
        float peak = 0.f, peakL = 0.f, peakR = 0.f;
        const int cc = std::min(in.channels, out.channels);
        for (int c = 0; c < cc; ++c) {
            const float pg = (c == 0) ? panL : (c == 1 ? panR : 1.f);
            const float* sp = in.chans[c];
            float*       dp = out.chans[c];
            for (int i = 0; i < n; ++i) {
                float s = sp[i] * g * pg;
                if (!std::isfinite(s)) s = 0.f;   // NaN/Inf in, or overflow here
                dp[i] += s;
                const float a = std::fabs(s);
                if (a > peak) peak = a;
                if (c == 0 && a > peakL) peakL = a;
                if (c == 1 && a > peakR) peakR = a;
            }
        }
        // An Inf input can latch the meter (NaN can't win a > compare, but Inf
        // does); publish only finite peaks so a poisoned block never sticks.
        // ACCUMULATE the block peak (see the metering contract in the header):
        // storing it here discarded every peak the UI did not happen to sample.
        // An Inf input can latch a meter (NaN cannot win a > compare, but Inf
        // does), so only finite peaks are folded in.
        accumChannelPeak(ch,
                         std::isfinite(peakL) ? peakL : 0.f,
                         std::isfinite(peakR) ? peakR : (std::isfinite(peakL) ? peakL : 0.f),
                         n);
    }
    // master (output) stage: apply the master gain + measure the master VU.
    // This is also the NaN/Inf fence for the mix bus — a non-finite sample from
    // any upstream node becomes silence here instead of reaching the device.
    const float mg = masterMute_.load(std::memory_order_relaxed)
                   ? 0.f : masterGain_.load(std::memory_order_relaxed);
    float mpeak = 0.f, mpeakL = 0.f, mpeakR = 0.f;
    for (int c = 0; c < out.channels; ++c) {
        float* dp = out.chans[c];
        for (int i = 0; i < n; ++i) {
            float s = dp[i] * mg;
            if (!std::isfinite(s)) s = 0.f;
            dp[i] = s;
            const float a = std::fabs(s);
            if (a > mpeak) mpeak = a;
            if (c == 0 && a > mpeakL) mpeakL = a;
            if (c == 1 && a > mpeakR) mpeakR = a;
        }
    }
    // Same contract as the channel meters: accumulate, never overwrite.  The
    // master bus is where a missed transient is most obvious, because it is the
    // meter people watch to judge clipping.
    (void)mpeak;
    accumMasterPeak(std::isfinite(mpeakL) ? mpeakL : 0.f,
                    out.channels > 1 ? (std::isfinite(mpeakR) ? mpeakR : 0.f)
                                     : (std::isfinite(mpeakL) ? mpeakL : 0.f),
                    n);
}

// ===========================================================================
// MasterMixerNode -- dynamic per-track ports (see patch_nodes.h)
// ===========================================================================
MasterMixerNode::~MasterMixerNode() {
    TrackSnap* s = snap_.exchange(nullptr, std::memory_order_acq_rel);
    delete s;
    for (size_t i = 0; i < retired_.size(); ++i) delete retired_[i].p;
    retired_.clear();
}

void MasterMixerNode::publishSnapshot() {
    // Freeze the current track layout into a fresh immutable block and swap it
    // in for the audio thread (message thread only; see the header comment for
    // the retirement/grace scheme).
    TrackSnap* ns = new TrackSnap();
    int cnt = (int)tracks_.size();
    if (cnt > kMaxChannels) cnt = kMaxChannels;   // strip arrays cap at 256 anyway
    ns->count = cnt;
    for (int i = 0; i < cnt; ++i) ns->midi[i] = tracks_[(size_t)i].midi;
    int na = (int)auxSlot_.size();
    if (na > kMaxAuxBuses) na = kMaxAuxBuses;
    ns->auxCount = na;
    for (int i = 0; i < na; ++i) ns->auxSlot[i] = auxSlot_[(size_t)i];
    for (int i = na; i < kMaxAuxBuses; ++i) ns->auxSlot[i] = 0;

    TrackSnap* old = snap_.exchange(ns, std::memory_order_acq_rel);
    const uint64_t g = rtGen_.load(std::memory_order_acquire);
    if (old) retired_.push_back(Retired{ old, g });

    // Free every parked snapshot the audio thread has provably moved past
    // (two block generations: the block in flight at its swap has exited and a
    // full block has since started on the new pointer).  If audio isn't running
    // rtGen_ never advances and the blocks simply wait here / for the dtor.
    const uint64_t now = rtGen_.load(std::memory_order_acquire);
    for (size_t i = 0; i < retired_.size(); ) {
        if (now >= retired_[i].gen + 2) {
            delete retired_[i].p;
            retired_[i] = retired_.back();
            retired_.pop_back();
        } else ++i;
    }
}

// ---------------------------------------------------------------------------
// Aux buses + the send matrix (message thread).
// ---------------------------------------------------------------------------
// SCOPE, deliberately narrow.  Only the state this node OWNS per channel and
// that nothing else re-derives is shifted here: the send matrix and solo.
//
//   * gain / pan / mute are NOT shifted, because they never were -- the app
//     re-pushes them from its own strip model, and quietly changing that on a
//     track delete is a behaviour change in code that has nothing to do with
//     aux sends.  (They are arguably wrong today; that is a separate fix.)
//   * extFader_ is NOT shifted because audio_app's master_chain_track_removed()
//     re-derives it for every channel from the already-shifted processor boxes
//     immediately afterwards -- shifting it here would apply the move TWICE.
void MasterMixerNode::copyStrip(int from, int to) {
    if (from < 0 || to < 0 || from >= kMaxChannels || to >= kMaxChannels) return;
    solo_[to].store(solo_[from].load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int a = 0; a < kMaxAuxBuses; ++a) {
        sendGain_[to][a].store(sendGain_[from][a].load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        sendPre_[to][a].store(sendPre_[from][a].load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        sendOn_[to][a].store(sendOn_[from][a].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        sendNorm_[to][a] = sendNorm_[from][a];
    }
}

void MasterMixerNode::clearStrip(int ch) {
    if (ch < 0 || ch >= kMaxChannels) return;
    solo_[ch].store(false, std::memory_order_relaxed);
    for (int a = 0; a < kMaxAuxBuses; ++a) {
        sendGain_[ch][a].store(0.f, std::memory_order_relaxed);
        sendPre_[ch][a].store(0, std::memory_order_relaxed);
        sendOn_[ch][a].store(1, std::memory_order_relaxed);
        sendNorm_[ch][a] = 0.f;
    }
}

void MasterMixerNode::recountSolo() {
    int n = 0;
    for (int i = 0; i < kMaxChannels; ++i)
        if (solo_[i].load(std::memory_order_relaxed)) ++n;
    soloCount_.store(n, std::memory_order_relaxed);
}

int MasterMixerNode::addAux() {
    if ((int)auxSlot_.size() >= kMaxAuxBuses) return -1;
    // Lowest FREE slot, so a delete-then-add reuses the hole instead of running
    // the slot numbers up; the freed slot's cables were pruned with the bus.
    bool used[kMaxAuxBuses] = { false };
    for (size_t i = 0; i < auxSlot_.size(); ++i) used[auxSlot_[i]] = true;
    int slot = -1;
    for (int i = 0; i < kMaxAuxBuses; ++i) if (!used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    auxSendName_[(size_t)slot] = "AUX " + std::to_string(slot + 1) + " SEND";
    auxRetName_[(size_t)slot]  = "AUX " + std::to_string(slot + 1) + " RET";
    // A recycled slot must not resurrect the previous bus's send levels.
    for (int t = 0; t < kMaxChannels; ++t) {
        sendGain_[t][slot].store(0.f, std::memory_order_relaxed);
        sendPre_[t][slot].store(0, std::memory_order_relaxed);
        sendOn_[t][slot].store(1, std::memory_order_relaxed);
        sendNorm_[t][slot] = 0.f;
    }
    auxSlot_.push_back((uint8_t)slot);
    publishSnapshot();
    return (int)auxSlot_.size() - 1;
}

bool MasterMixerNode::removeAux(int aux) {
    if (aux < 0 || aux >= (int)auxSlot_.size()) return false;
    auxSlot_.erase(auxSlot_.begin() + aux);   // only the INDEX list compacts
    publishSnapshot();
    return true;
}

// ARDOUR::slider_position_to_gain (interp_gain / the "8th-power" fader law),
// rescaled: position_to_gain(1) is +6 dB, and a SEND knob's top is unity, so the
// curve is halved.  norm <= 0 is a hard zero -- see the header.
float MasterMixerNode::sendNormToGain(float norm) {
    if (!(norm > 0.f)) return 0.f;                 // also rejects NaN
    if (norm > 1.f) norm = 1.f;
    const double pos = std::pow((double)norm, 1.0 / 8.0);
    const double g   = std::exp(((pos * 198.0) - 192.0) / 6.0 * std::log(2.0)) * 0.5;
    if (!std::isfinite(g) || g <= 0.0) return 0.f;
    return (float)(g > 1.0 ? 1.0 : g);
}

float MasterMixerNode::sendGainToNorm(float gain) {
    if (!(gain > 0.f)) return 0.f;
    if (gain > 1.f) gain = 1.f;
    const double p   = (double)gain * 2.0;
    const double pos = (6.0 * std::log(p) / std::log(2.0) + 192.0) / 198.0;
    if (!(pos > 0.0)) return 0.f;
    const double n = std::pow(pos, 8.0);
    if (!std::isfinite(n)) return 0.f;
    return (float)(n > 1.0 ? 1.0 : n);
}

float MasterMixerNode::sendLevel(int track, int aux) const {
    const int slot = auxSlot(aux);
    if (slot < 0 || track < 0 || track >= kMaxChannels) return 0.f;
    return sendNorm_[track][slot];
}

void MasterMixerNode::setSendLevel(int track, int aux, float norm) {
    const int slot = auxSlot(aux);
    if (slot < 0 || track < 0 || track >= kMaxChannels) return;
    if (!(norm > 0.f)) norm = 0.f;                 // NaN -> silence
    if (norm > 1.f) norm = 1.f;
    sendNorm_[track][slot] = norm;
    sendGain_[track][slot].store(sendNormToGain(norm), std::memory_order_relaxed);
}

bool MasterMixerNode::sendPreFader(int track, int aux) const {
    const int slot = auxSlot(aux);
    if (slot < 0 || track < 0 || track >= kMaxChannels) return false;
    return sendPre_[track][slot].load(std::memory_order_relaxed) != 0;
}

void MasterMixerNode::setSendPreFader(int track, int aux, bool pre) {
    const int slot = auxSlot(aux);
    if (slot < 0 || track < 0 || track >= kMaxChannels) return;
    sendPre_[track][slot].store(pre ? 1 : 0, std::memory_order_relaxed);
}

bool MasterMixerNode::sendEnabled(int track, int aux) const {
    const int slot = auxSlot(aux);
    if (slot < 0 || track < 0 || track >= kMaxChannels) return false;
    return sendOn_[track][slot].load(std::memory_order_relaxed) != 0;
}

void MasterMixerNode::setSendEnabled(int track, int aux, bool on) {
    const int slot = auxSlot(aux);
    if (slot < 0 || track < 0 || track >= kMaxChannels) return;
    sendOn_[track][slot].store(on ? 1 : 0, std::memory_order_relaxed);
}

void MasterMixerNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& master = ctx.audioOut[0];
    for (int c = 0; c < master.channels; ++c)
        std::memset(master.chans[c], 0, sizeof(float) * (size_t)n);

    // Read ONLY the frozen snapshot — never tracks_, which the message thread
    // may be push_back/erasing (and reallocating) right now.
    const TrackSnap* snap = snap_.load(std::memory_order_acquire);
    const int ntracks = snap ? snap->count : 0;
    const int naux    = snap ? snap->auxCount : 0;

    int aiIdx = 0;   // next audio-in  port (track audio inlets)
    int aoIdx = 1;   // next audio-out port (0 == master, then track outlets)
    int miIdx = 0;   // next midi-in   port (track midi inlets)
    int moIdx = 1;   // next midi-out  port (0 == clock, then track outlets)

    // AUX BANKS.  port() enumerates every track port, then all the aux SENDS,
    // then all the aux RETURNS -- so the banks start exactly where the track
    // walk leaves the two cursors, whatever mix of MIDI/audio tracks exists.
    int audioTracks = 0;
    for (int t = 0; t < ntracks; ++t) if (!snap->midi[t]) ++audioTracks;
    const int auxOutBase = 1 + audioTracks;      // audioOut index of send 0
    const int auxInBase  = audioTracks;          // audioIn  index of return 0
    for (int k = 0; k < naux; ++k) {
        const int op = auxOutBase + k;
        if (op >= ctx.numAudioOut) break;
        for (int c = 0; c < ctx.audioOut[op].channels; ++c)
            std::memset(ctx.audioOut[op].chans[c], 0, sizeof(float) * (size_t)n);
    }

    for (int t = 0; t < ntracks; ++t) {
        if (!snap->midi[t]) {
            // AUDIO channel: sum inlet (gain/pan/mute) into master + echo raw inlet
            // to its own outlet (a direct/insert send).
            float peakL = 0.f, peakR = 0.f;
            if (aiIdx < ctx.numAudioIn) {
                const AudioBus& in = ctx.audioIn[aiIdx];
                // With an EXTERNAL fader (a TrackFaderNode in this track's
                // insert chain carries gain+mute so post-fader inserts are
                // possible) this stage runs at unity: applying the gain twice
                // would square it.  Pan and metering stay here regardless.
                const bool extf = extFader_[t].load(std::memory_order_relaxed);
                // channelSilenced() is mute OR "something else is soloed"; with
                // nothing soloed anywhere it is exactly the old mute test.
                float g = extf ? 1.f : (channelSilenced(t) ? 0.f : gain(t));
                const float p = pan(t);
                float panL = p <= 0.f ? 1.f : 1.f - p;
                float panR = p >= 0.f ? 1.f : 1.f + p;
                // PER-TRACK NaN/Inf FENCE.  The master stage below fences the
                // SUM, which is too late: one poisoned track (a rack/VST/Csound
                // node emitting NaN) turned the whole mix into NaN and muted
                // every other track with it.  Sanitising the coefficients once
                // and each sample as it is read isolates the offender.
                if (!std::isfinite(g))    g    = 0.f;
                if (!std::isfinite(panL)) panL = 0.f;
                if (!std::isfinite(panR)) panR = 0.f;
                // RECORD TAP: PRE-FADER (raw inlet), so riding the fader is not
                // baked into the take and a muted channel still records signal.
                for (int slot = 0; slot < kMaxCaptureTaps; ++slot) {
                    if (capChan_[slot].load(std::memory_order_relaxed) != t) continue;
                    float* capture = capBuf_[slot].load(std::memory_order_acquire);
                    if (!capture) break;
                    size_t pos = capPos_[slot].load(std::memory_order_relaxed);
                    const size_t cap = capCap_[slot].load(std::memory_order_relaxed);
                    const float* L = in.channels > 0 ? in.chans[0] : nullptr;
                    const float* R = in.channels > 1 ? in.chans[1] : L;
                    for (int s = 0; s < n && pos < cap; ++s, ++pos) {
                        const float l = L ? L[s] : 0.f;
                        const float r = R ? R[s] : l;
                        capture[pos * 2] = std::isfinite(l) ? l : 0.f;
                        capture[pos * 2 + 1] = std::isfinite(r) ? r : 0.f;
                    }
                    capPos_[slot].store(pos, std::memory_order_release);
                    break;            // one tap per track
                }
                const int cc = std::min(in.channels, master.channels);
                for (int c = 0; c < cc; ++c) {
                    const float pg = (c == 0) ? panL : (c == 1 ? panR : 1.f);
                    const float* sp = in.chans[c];
                    float*       dp = master.chans[c];
                    float* op = (aoIdx < ctx.numAudioOut && c < ctx.audioOut[aoIdx].channels)
                                    ? ctx.audioOut[aoIdx].chans[c] : nullptr;
                    for (int s = 0; s < n; ++s) {
                        const float raw = sp[s];
                        const float sv = std::isfinite(raw) ? raw : 0.f;
                        float v = sv * g * pg;
                        if (!std::isfinite(v)) v = 0.f;   // overflow guard
                        dp[s] += v;
                        if (op) op[s] = sv;
                        const float a = std::fabs(v);
                        if(c==0&&a>peakL)peakL=a;
                        if(c==1&&a>peakR)peakR=a;
                    }
                }

                // ---- AUX SENDS ------------------------------------------
                // One pass per ACTIVE bus, deliberately outside the master
                // inner loop: a strip that sends nowhere costs nothing, and the
                // dry path stays exactly the code it was.
                //
                //   POST-fader (the default, what a reverb send wants): the very
                //     signal that went to the mix -- fader, mute, solo and pan
                //     all applied (Ardour's aux sends are post-panner too).
                //   PRE-fader (the headphone/monitor feed): the raw inlet, with
                //     the fader, the mute, the solo and the pan all ignored --
                //     so a MUTED track still feeds its pre-fader sends.
                //
                // CAVEAT for a strip carrying POST-FADER INSERTS: its fader has
                // been lifted out of this node into a TrackFaderNode upstream
                // (setExternalFader), so the inlet arriving here is ALREADY
                // post-fader and a pre-fader send on that strip follows the
                // fader.  Only strips with post-fader inserts are affected.
                for (int k = 0; k < naux; ++k) {
                    const int slot = snap->auxSlot[k];
                    if (!sendOn_[t][slot].load(std::memory_order_relaxed)) continue;
                    float sg = sendGain_[t][slot].load(std::memory_order_relaxed);
                    if (!std::isfinite(sg) || sg <= 0.f) continue;   // 0 == silence
                    const bool pre = sendPre_[t][slot].load(std::memory_order_relaxed) != 0;
                    const int  op  = auxOutBase + k;
                    if (op >= ctx.numAudioOut) break;
                    const AudioBus& ab = ctx.audioOut[op];
                    const int ac = std::min(in.channels, ab.channels);
                    for (int c = 0; c < ac; ++c) {
                        const float pg = pre ? 1.f : ((c == 0) ? panL : (c == 1 ? panR : 1.f));
                        const float gg = (pre ? 1.f : g) * pg * sg;
                        if (!std::isfinite(gg) || gg == 0.f) continue;
                        const float* sp = in.chans[c];
                        float*       dp = ab.chans[c];
                        for (int s = 0; s < n; ++s) {
                            const float raw = sp[s];
                            const float sv = std::isfinite(raw) ? raw : 0.f;
                            float v = sv * gg;
                            if (!std::isfinite(v)) v = 0.f;
                            dp[s] += v;
                        }
                    }
                }
            } else if (aoIdx < ctx.numAudioOut) {          // no inlet -> silent outlet
                for (int c = 0; c < ctx.audioOut[aoIdx].channels; ++c)
                    std::memset(ctx.audioOut[aoIdx].chans[c], 0, sizeof(float) * (size_t)n);
            }
            if(!std::isfinite(peakL))peakL=0.f;if(!std::isfinite(peakR))peakR=0.f;
            // Use the same frame-latched contract as MixerNode.  Publishing
            // directly here was immediately overwritten by meterLatch(), whose
            // accumulators were still zero, blanking the master and arrange VUs.
            accumChannelPeak(t, peakL, peakR, n);
            ++aiIdx; ++aoIdx;
        } else {
            // MIDI channel: pass inlet straight through to its outlet.
            if (moIdx < ctx.numMidiOut) {
                MidiBuffer& mo = ctx.midiOut[moIdx];
                int cnt = 0;
                if (miIdx < ctx.numMidiIn) {
                    const MidiBuffer& mi = ctx.midiIn[miIdx];
                    cnt = std::min(mi.count, mo.capacity);
                    for (int e = 0; e < cnt; ++e) mo.ev[e] = mi.ev[e];
                }
                mo.count = cnt;
            }
            ++miIdx; ++moIdx;
            // MIDI-only channels have no audio inlet at this node. Explicitly
            // clear stale readings left from a prior audio track at this index.
            clearChannelMeter(t);
        }
    }

    // ---- AUX RETURNS ------------------------------------------------------
    // Each bus's output is summed into the mix HERE, ahead of the master fader
    // and the master VU -- exactly where Ardour lands an aux return -- so
    // pulling the master down takes the reverb with it and the master meter
    // shows what actually leaves the desk.  Return gain and bus mute live in the
    // AuxBusNode itself, so this stage is a plain sum.
    //
    // These inputs are FEEDBACK ports (portIsFeedback): the bus is downstream of
    // this node, so what arrives is the bus's PREVIOUS block -- one block of
    // delay on the return leg (~2.7 ms at 128/48k), the standard price for a
    // send/return loop in a topologically sorted graph.
    for (int k = 0; k < naux; ++k) {
        const int ip = auxInBase + k;
        if (ip >= ctx.numAudioIn) break;
        const AudioBus& rin = ctx.audioIn[ip];
        const int cc = std::min(rin.channels, master.channels);
        for (int c = 0; c < cc; ++c) {
            const float* sp = rin.chans[c];
            float*       dp = master.chans[c];
            for (int s = 0; s < n; ++s) {
                const float raw = sp[s];
                dp[s] += std::isfinite(raw) ? raw : 0.f;
            }
        }
    }

    // master gain + master VU on the summed output.  Like MixerNode, this is
    // the NaN/Inf fence: a poisoned upstream sample becomes silence here.
    const float mg = masterMute() ? 0.f : masterGain();
    float mpeakL = 0.f, mpeakR = 0.f;
    for (int c = 0; c < master.channels; ++c) {
        float* dp = master.chans[c];
        for (int s = 0; s < n; ++s) { float v = dp[s] * mg;
                                      if (!std::isfinite(v)) v = 0.f;
                                      dp[s] = v;
                                      const float a = std::fabs(v);
                                      if(c==0&&a>mpeakL)mpeakL=a;
                                      if(c==1&&a>mpeakR)mpeakR=a; }
    }
    accumMasterPeak(mpeakL, master.channels>1 ? mpeakR : mpeakL, n);

    // tempo-synced MIDI clock (24-PPQN 0xF8 + Start/Continue/SPP/Stop) on the
    // clock out (port 1 -> midiOut[0]).  clockAcc_ counts SAMPLES UNTIL the
    // next 0xF8 is due, carrying the fractional remainder across blocks, so
    // consecutive clocks land at exact sample spacing (240 @ 500 BPM, 48k).
    // Run-state transitions are block-granular, so the transition offset is 0.
    if (ctx.numMidiOut > 0) {
        MidiBuffer& clk = ctx.midiOut[0];
        int cnt = 0;
        if (ctx.transport.isPlaying) {
            // Interval derives from the transport tempo EVERY block, so a tempo
            // edit retunes the clock at the next block boundary.
            const double bpm = ctx.transport.tempoBpm > 0.0 ? ctx.transport.tempoBpm : 120.0;
            const double interval = (sr_ * 60.0) / (bpm * 24.0);
            const int64_t pos = ctx.transport.playPositionSamples;
            // A transport JUMP TAKEN WHILE PLAYING (loop wrap, click-to-seek,
            // punch) is exactly as much of a discontinuity as a start: the beat
            // grid moves under the clock.  The re-seed used to be gated on
            // `!wasPlaying_` alone, so a jump left clockAcc_ untouched and
            // emitted no SPP/Continue -- the 24-PPQN phase kept free-running
            // from the OLD position, landing the next 0xF8 off the grid by
            // whatever the jump distance was modulo the clock interval, and
            // re-randomising on every loop pass.
            const bool jumped = wasPlaying_ && pos != expectedPos_;
            if (!wasPlaying_ || jumped) {
                if (!wasPlaying_ && pos == 0) {
                    // stop -> play from the top: 0xFA Start.
                    if (cnt < clk.capacity) { clk.ev[cnt].sampleOffset = 0; clk.ev[cnt].status = 0xFA;
                                              clk.ev[cnt].data1 = 0; clk.ev[cnt].data2 = 0; ++cnt; }
                } else {
                    // continue / relocate mid-song: 0xF2 Song Position (14-bit
                    // MIDI beats,
                    // 1 MIDI beat = one 16th note = 6 clocks), then 0xFB Continue.
                    const double qBeats = ((double)pos * bpm) / (sr_ * 60.0);
                    long long beats16 = (long long)std::llround(qBeats * 4.0);
                    if (beats16 < 0)      beats16 = 0;
                    if (beats16 > 0x3FFF) beats16 = 0x3FFF;
                    if (cnt < clk.capacity) { clk.ev[cnt].sampleOffset = 0; clk.ev[cnt].status = 0xF2;
                                              clk.ev[cnt].data1 = (unsigned char)(beats16 & 0x7F);
                                              clk.ev[cnt].data2 = (unsigned char)((beats16 >> 7) & 0x7F); ++cnt; }
                    if (cnt < clk.capacity) { clk.ev[cnt].sampleOffset = 0; clk.ev[cnt].status = 0xFB;
                                              clk.ev[cnt].data1 = 0; clk.ev[cnt].data2 = 0; ++cnt; }
                }
                // Seed so the FIRST 0xF8 coincides with Start/Continue (MIDI
                // spec: the first clock falls exactly on the start point).
                clockAcc_ = 0.0;
            }
            for (int i = 0; i < n; ++i) {
                if (clockAcc_ <= 0.0) {
                    if (cnt < clk.capacity) { clk.ev[cnt].sampleOffset = i; clk.ev[cnt].status = 0xF8;
                                              clk.ev[cnt].data1 = 0; clk.ev[cnt].data2 = 0; ++cnt; }
                    clockAcc_ += interval;   // phase advances even if the buffer is full
                }
                clockAcc_ -= 1.0;
            }
        } else {
            if (wasPlaying_ && cnt < clk.capacity) {
                // play -> stop: 0xFC Stop at the transition.
                clk.ev[cnt].sampleOffset = 0; clk.ev[cnt].status = 0xFC;
                clk.ev[cnt].data1 = 0; clk.ev[cnt].data2 = 0; ++cnt;
            }
            clockAcc_ = 0.0;
        }
        clk.count = cnt;
    }
    wasPlaying_ = ctx.transport.isPlaying;
    // Where the transport SHOULD be at the top of the next block if it just
    // keeps rolling.  Anything else is a jump (see the clock re-seed above).
    expectedPos_ = ctx.transport.isPlaying
                     ? ctx.transport.playPositionSamples + (int64_t)n
                     : ctx.transport.playPositionSamples;

    // Block done: advance the generation so publishSnapshot() can retire old
    // snapshots (a parked block is freed once this has advanced by >= 2).
    rtGen_.fetch_add(1, std::memory_order_release);
}

// ===========================================================================
// AuxBusNode -- the RETURN stage of one aux bus.
// ===========================================================================
void AuxBusNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& out = ctx.audioOut[0];

    // A bus with nothing wired in still has to WRITE its output every block:
    // the master mixer's return port aliases this very buffer and reads it a
    // block late, so leaving it alone would loop the last block round forever.
    if (ctx.numAudioIn < 1) {
        for (int c = 0; c < out.channels; ++c)
            std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
        peakL_.store(0.f, std::memory_order_relaxed);
        peakR_.store(0.f, std::memory_order_relaxed);
        balL_ = balR_ = 0.f; holdL_ = holdR_ = 0;
        return;
    }

    const float target = mute_.load(std::memory_order_relaxed)
                       ? 0.f : returnGain_.load(std::memory_order_relaxed);
    const float start  = cur_;
    const float step   = (n > 0) ? (target - start) / (float)n : 0.f;
    const bool  ramp   = std::fabs(target - start) > 1e-6f;

    const AudioBus& in = ctx.audioIn[0];
    const int cc = std::min(in.channels, out.channels);
    float peakL = 0.f, peakR = 0.f;
    for (int c = 0; c < cc; ++c) {
        const float* sp = in.chans[c];
        float*       dp = out.chans[c];
        float        g  = start;
        for (int i = 0; i < n; ++i) {
            if (ramp) g += step;
            const float raw = sp[i];
            float v = (std::isfinite(raw) ? raw : 0.f) * (ramp ? g : target);
            if (!std::isfinite(v)) v = 0.f;
            dp[i] = v;
            const float a = std::fabs(v);
            if (c == 0) { if (a > peakL) peakL = a; }
            else if (c == 1) { if (a > peakR) peakR = a; }
        }
    }
    // Channels the input could not supply must still be written (see above).
    for (int c = cc; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
    if (cc == 1) peakR = peakL;
    cur_ = target;

    // Published ballistics (instant attack, 21 ms hold, 24 dB/s fall), so any
    // number of readers at any rate see a correct bar -- see the class comment.
    MixerNode::ballistic(balL_, holdL_, peakL, n, sr_);
    MixerNode::ballistic(balR_, holdR_, peakR, n, sr_);
    peakL_.store(std::isfinite(balL_) ? balL_ : 0.f, std::memory_order_relaxed);
    peakR_.store(std::isfinite(balR_) ? balR_ : 0.f, std::memory_order_relaxed);
}

// ===========================================================================
// AudioDeviceOutNode
// ===========================================================================
void AudioDeviceOutNode::process(const NodeProcessContext& ctx) {
    if (!target_ || nframes_ <= 0) return;
    // Never copy more frames than bindDeviceOut() actually provided — the
    // device buffers own exactly nframes_ frames whatever the block asks for.
    const int n = std::min(ctx.nframes, nframes_);
    const AudioBus* in = (ctx.numAudioIn > 0 && ctx.audioIn) ? &ctx.audioIn[0] : nullptr;
    const int cc = in ? std::min(targetCh_, in->channels) : 0;
    for (int c = 0; c < cc; ++c) {
        float* dp = target_[c];
        if (!dp) continue;
        // SUM into the device buffer (PatchGraph::process() pre-zeroed it) so that
        // EVERY "Audio Out" module mixes into the output instead of the last one
        // overwriting the rest.  A NaN/Inf sample must never reach the DAC.
        const float* sp = in->chans[c];
        for (int i = 0; i < n; ++i) {
            const float v = sp[i];
            if (additive_) {
                if (std::isfinite(v)) dp[i] += v;
            } else {
                dp[i] = std::isfinite(v) ? v : 0.f;
            }
        }
        if (!additive_ && n < nframes_)
            std::memset(dp + n, 0, sizeof(float) * (size_t)(nframes_ - n));
    }
    // Direct bindings must also silence unconnected channels and short-block
    // tails. In additive graph mode PatchGraph already cleared the full buffer.
    if (!additive_)
        for (int c = cc; c < targetCh_; ++c)
            if (target_[c]) std::memset(target_[c], 0, sizeof(float) * (size_t)nframes_);
}

// ===========================================================================
// AudioDeviceInNode
// ===========================================================================
void AudioDeviceInNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const int nsrc = std::min(n, nframes_ > 0 ? nframes_ : 0);
    const int ports = std::min(channels_, ctx.numAudioOut);
    for (int p = 0; p < ports; ++p) {
        const AudioBus& out = ctx.audioOut[p];
        float* dst = out.channels > 0 ? out.chans[0] : nullptr;
        const float* source = (src_ && p < srcCh_) ? src_[p] : nullptr;
        if (dst) {
            int i = 0;
            if (source)
                for (; i < nsrc; ++i)
                    dst[i] = std::isfinite(source[i]) ? source[i] : 0.f;
            if (i < n) std::memset(dst + i, 0, sizeof(float) * (size_t)(n - i));
        }
        for (int c = 1; c < out.channels; ++c)
            std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
    }
    for (int p = ports; p < ctx.numAudioOut; ++p)
        for (int c = 0; c < ctx.audioOut[p].channels; ++c)
            std::memset(ctx.audioOut[p].chans[c], 0, sizeof(float) * (size_t)n);
}

// ===========================================================================
// MidiInNode — lock-free SPSC ring of timestamped events
// ===========================================================================
void MidiInNode::setTrackPorts(int n) {
    if (n < 1) n = 1;
    trackPorts_ = n;
    portNames_.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        // "Instrument 1"... for the growable per-track plugs; a single-plug node
        // (the default / hardware inputs) keeps the neutral "out" label.
        //
        // These are NOT virtual ports.  Plug i carries the sequencer output of
        // arrange track i to that track's own instrument -- a fixed, private
        // wire.  The VirtualMidiPortsNode is the different thing: a patchable
        // bank for routing between tracks and external devices.  Labelling
        // these "Virtual out N" made the two indistinguishable in the patcher.
        portNames_[(size_t)i] = "Instrument " + std::to_string(i + 1);
    }
}

bool MidiInNode::push(const MidiEvent& e, int64_t dueSample, int port) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t next = (t + 1) % kRing;
    const uint32_t h = head_.load(std::memory_order_acquire);
    const uint32_t used = (t + kRing - h) % kRing;
    const bool release = midiIsRelease(e);
    // Reserve slots for releases/panic messages. It is safer to reject a late
    // note-on than to admit it and subsequently lose its note-off.
    if (next == h || ((uint32_t)(kRing - 1) - used < kReleaseHeadroom && !release))
        return false;
    ring_[t].dueSample = dueSample;
    ring_[t].ev = e;
    // There is no implicit broadcast. Fan-out is represented by explicit graph
    // edges, so an invalid target is retained as invalid: process() drops a
    // non-release aimed at it, but re-aims a RELEASE at whichever reachable plug
    // still has that note sounding (see deliverSlot).
    ringPort_[t] = (uint8_t)((port < 0 || port > 254) ? 0xFF : port);
    tail_.store(next, std::memory_order_release);
    return true;
}

// ---- consumer-side held-note table ----------------------------------------
// One bit per (plug, channel, note); heldChanMask_ short-circuits the scan and
// soundingCount_ publishes the total so a caller can ask "is anything sounding?"
// without walking 256 plugs.
bool MidiInNode::heldTest(int plug, int ch, int note) const {
    const int bit = ch * 128 + note;
    return (heldBits_[plug][bit >> 5] & (1u << (bit & 31))) != 0u;
}
void MidiInNode::heldSet(int plug, int ch, int note) {
    const int bit = ch * 128 + note;
    uint32_t& w = heldBits_[plug][bit >> 5];
    const uint32_t m = 1u << (bit & 31);
    if (w & m) return;
    w |= m;
    heldChanMask_[plug] = (uint16_t)(heldChanMask_[plug] | (1u << ch));
    soundingCount_.fetch_add(1, std::memory_order_relaxed);
}
void MidiInNode::heldClear(int plug, int ch, int note) {
    const int bit = ch * 128 + note;
    uint32_t& w = heldBits_[plug][bit >> 5];
    const uint32_t m = 1u << (bit & 31);
    if (!(w & m)) return;
    w &= ~m;
    soundingCount_.fetch_sub(1, std::memory_order_relaxed);
    uint32_t any = 0;
    for (int i = ch * 4; i < ch * 4 + 4; ++i) any |= heldBits_[plug][i];
    if (!any) heldChanMask_[plug] = (uint16_t)(heldChanMask_[plug] & ~(1u << ch));
}
void MidiInNode::heldClearChannel(int plug, int ch) {
    for (int i = ch * 4; i < ch * 4 + 4; ++i) {
        uint32_t w = heldBits_[plug][i];
        while (w) { w &= w - 1; soundingCount_.fetch_sub(1, std::memory_order_relaxed); }
        heldBits_[plug][i] = 0;
    }
    heldChanMask_[plug] = (uint16_t)(heldChanMask_[plug] & ~(1u << ch));
}
void MidiInNode::heldClearPlug(int plug) {
    for (int ch = 0; ch < 16; ++ch)
        if (heldChanMask_[plug] & (1u << ch)) heldClearChannel(plug, ch);
}

void MidiInNode::noteBookkeep(int plug, const MidiEvent& e) {
    if (plug < 0 || plug >= kTrackedPlugs) return;   // untracked plug (see kTrackedPlugs)
    const unsigned char hi = e.status & 0xF0u;
    const int ch   = e.status & 0x0F;
    const int note = e.data1 & 0x7F;
    if (hi == 0x90u && e.data2 != 0)                            heldSet(plug, ch, note);
    else if (hi == 0x80u || hi == 0x90u)                        heldClear(plug, ch, note);
    else if (hi == 0xB0u && (e.data1 == 120 || e.data1 == 123)) heldClearChannel(plug, ch);
}

bool MidiInNode::deliverSlot(const NodeProcessContext& ctx, uint32_t slot,
                             int64_t blockStart, int reCh, bool force) {
    const TimedMidi& tm = ring_[slot];
    const int  rp = (int)ringPort_[slot];
    const int  nOut = ctx.numMidiOut;
    MidiEvent  e = tm.ev;
    // Re-stamp channel-voice messages onto outChannel_ (hardware input node) so
    // a keyboard plays the instrument it's wired to regardless of channel.
    if (reCh >= 0) {
        const unsigned char hi = e.status & 0xF0u;
        if (hi >= 0x80u && hi < 0xF0u) {
            int outCh = reCh & 0x0F;
            // A by-note release must land on whatever channel its matching
            // note-on was actually delivered on. outChannel_ can change
            // between a note-on and its note-off (the user retargeting the
            // hardware-input channel live); blindly re-stamping the release
            // onto the CURRENT outChannel_ would send it on a channel with
            // no matching note-on (leaving the real note stuck sounding
            // forever) while never releasing anything. Find the channel
            // this note is actually held on for this plug instead.
            const bool byNote = (hi == 0x80u) || (hi == 0x90u && e.data2 == 0);
            if (byNote && rp >= 0 && rp < kTrackedPlugs) {
                const int note = e.data1 & 0x7F;
                for (int c = 0; c < 16; ++c) {
                    if ((heldChanMask_[rp] & (1u << c)) && heldTest(rp, c, note)) {
                        outCh = c;
                        break;
                    }
                }
            }
            e.status = (unsigned char)(hi | outCh);
        }
    }
    // due<0 ("now"), late events and a force-flushed backlog all clamp to 0.
    e.sampleOffset = force ? 0
        : (int32_t)std::clamp(tm.dueSample - blockStart,
                              (int64_t)0, (int64_t)ctx.nframes - 1);

    if (rp >= 0 && rp < nOut) {                        // one exact track plug
        MidiBuffer& out = ctx.midiOut[rp];
        if (out.count >= out.capacity) return false;   // FULL: keep it queued (rule 2)
        out.ev[out.count++] = e;
        noteBookkeep(rp, e);
        return true;
    }

    // ---- the target plug does not exist (rule 3) --------------------------
    // A note-on is dropped: it is deliberately NOT redirected to plug 0, which
    // would make an absent track play someone else's instrument.
    if (!midiIsRelease(e)) return true;
    // A release, however, has to get out.  Best case it is re-aimed at exactly
    // the reachable plugs that still have this note sounding FROM THIS NODE —
    // precise, and it can never fire a spurious note-off.
    const unsigned char hi = e.status & 0xF0u;
    const int  ch    = e.status & 0x0F;
    const int  note  = e.data1 & 0x7F;
    const bool byNote = (hi == 0x80u || hi == 0x90u);
    bool blocked = false;
    for (int p = 0; p < kTrackedPlugs; ++p) {
        if (!(heldChanMask_[p] & (1u << ch)))     continue;
        if (byNote && !heldTest(p, ch, note))     continue;
        if (p >= nOut) {
            // The plug is GONE from the compiled context: there is no buffer to
            // write to, and a surviving plug belongs to a different instrument —
            // dumping the release there would cut someone else's note.  Drop the
            // event, but stop counting the note as sounding, so a plug that is
            // later re-created does not inherit a phantom held note.
            if (byNote) heldClear(p, ch, note);
            else        heldClearChannel(p, ch);
            continue;
        }
        MidiBuffer& out = ctx.midiOut[p];
        if (out.count >= out.capacity) { blocked = true; continue; }
        out.ev[out.count++] = e;
        noteBookkeep(p, e);                       // clears the bit we just matched
    }
    return !blocked;   // a full plug means "retry next block", never "discard"
}

bool MidiInNode::emitHeldReleases(const NodeProcessContext& ctx) {
    const int nOut = ctx.numMidiOut;
    bool done = true;
    for (int p = 0; p < kTrackedPlugs; ++p) {
        if (!heldChanMask_[p]) continue;
        if (p >= nOut) {          // plug is gone: unreachable, so it cannot be released
            heldClearPlug(p);
            continue;
        }
        MidiBuffer& out = ctx.midiOut[p];
        for (int ch = 0; ch < 16; ++ch) {
            if (!(heldChanMask_[p] & (1u << ch))) continue;
            for (int w = 0; w < 4; ++w) {
                uint32_t bits = heldBits_[p][ch * 4 + w];
                while (bits) {
                    if (out.count >= out.capacity) return false;   // resume next block
                    int b = 0;
                    while (!((bits >> b) & 1u)) ++b;
                    MidiEvent& o = out.ev[out.count++];
                    o.sampleOffset = 0;
                    o.status = (unsigned char)(0x80u | (unsigned)ch);
                    o.data1  = (unsigned char)((w << 5) + b);
                    o.data2  = 0;
                    heldClear(p, ch, (w << 5) + b);
                    bits &= ~(1u << b);
                }
            }
        }
        if (heldChanMask_[p]) done = false;
    }
    return done;
}

void MidiInNode::flushDiscardingHeldNotes() {
    // Consumer side.  Reset each skipped slot's delivered mark BEFORE releasing
    // head_, so the producer can never reuse a slot that still looks delivered.
    uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    while (h != t) { ringState_[h] = kSlotPending; h = (h + 1) % kRing; }
    head_.store(h, std::memory_order_release);
}

void MidiInNode::flush() {
    flushDiscardingHeldNotes();
    // Everything still queued was never delivered, so it strands nothing.  What
    // WAS delivered and is still sounding must be released, or a locate/stop
    // leaves the instrument holding the note forever.
    releaseHeldNotes();
}

void MidiInNode::process(const NodeProcessContext& ctx) {
    const int nOut = ctx.numMidiOut;
    if (nOut < 1 || ctx.nframes < 1) return;
    for (int p = 0; p < nOut; ++p) ctx.midiOut[p].count = 0;   // reset all plugs

    const int64_t blockStart = ctx.transport.playPositionSamples;
    const int64_t blockEnd   = blockStart + (int64_t)ctx.nframes;
    const int reCh = outChannel_.load(std::memory_order_relaxed);   // -1 = passthrough

    // (1) Safety releases armed by flush()/releaseHeldNotes() go out FIRST, so a
    //     note that was sounding across a locate cannot outlive this block.
    if (pendingPanic_.load(std::memory_order_acquire)) {
        if (emitHeldReleases(ctx))
            pendingPanic_.store(false, std::memory_order_release);
    }

    // (2) SCAN-AHEAD drain (rule 1): visit the whole queued window and deliver
    //     everything due inside this block wherever it sits, so a scheduled-ahead
    //     event never delays a live "due now" event queued behind it.  Genuinely
    //     future events stay put and still fire at their exact offsets.
    uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    for (uint32_t i = h; i != t; i = (i + 1) % kRing) {
        if (ringState_[i] == kSlotDelivered) continue;      // already sent, awaiting reclaim
        if (ring_[i].dueSample >= blockEnd)  continue;      // future: leave queued
        if (deliverSlot(ctx, i, blockStart, reCh, false))
            ringState_[i] = kSlotDelivered;
    }

    // (3) Reclaim ONLY the contiguous delivered prefix (rule 2): an event that
    //     did not fit stays queued and is retried next block.
    while (h != t && ringState_[h] == kSlotDelivered) {
        ringState_[h] = kSlotPending;
        h = (h + 1) % kRing;
    }
    // Backlog safety valve.  A future-dated event that never comes due (transport
    // jumped backwards without a flush) would otherwise wedge the head and fill
    // the ring until push() starts refusing releases.  Once free space runs below
    // kBacklogValve, force the head out at offset 0 instead — firing one event
    // early beats jamming the queue for good.  Untouched in normal operation.
    while (h != t &&
           (uint32_t)(kRing - 1) - (uint32_t)((t + kRing - h) % kRing) < kBacklogValve) {
        if (ringState_[h] != kSlotDelivered &&
            !deliverSlot(ctx, h, blockStart, reCh, true)) break;   // every plug full
        ringState_[h] = kSlotPending;
        h = (h + 1) % kRing;
    }
    head_.store(h, std::memory_order_release);

    // Keep each plug's events sorted by sample offset (stable insertion sort).
    for (int p = 0; p < nOut; ++p) {
        MidiBuffer& out = ctx.midiOut[p];
        for (int i = 1; i < out.count; ++i) {
            MidiEvent key = out.ev[i];
            int j = i - 1;
            while (j >= 0 && out.ev[j].sampleOffset > key.sampleOffset) {
                out.ev[j + 1] = out.ev[j]; --j;
            }
            out.ev[j + 1] = key;
        }
    }
}

// ===========================================================================
// MidiOutNode — lock-free SPSC ring of timestamped events
// ===========================================================================
void MidiOutNode::process(const NodeProcessContext& ctx) {
    if (ctx.numMidiIn < 1) return;
    // Stamp each event's ABSOLUTE due sample (block start + in-block offset) so
    // the hardware drain can pace the wire to the audio clock, not the block.
    const int64_t blockStart = ctx.transport.playPositionSamples;
    uint32_t t = tail_.load(std::memory_order_relaxed);
    // head_ only ever ADVANCES (the drain frees slots), so one acquire load is a
    // conservative snapshot: we may under-estimate the free space, never over.
    const uint32_t h = head_.load(std::memory_order_acquire);
    unsigned refused = 0;
    for (int p = 0; p < ctx.numMidiIn; ++p) {
        const MidiBuffer& in = ctx.midiIn[p];
        for (int i = 0; i < in.count; ++i) {
            const MidiEvent& m    = in.ev[i];
            const uint32_t   next = (t + 1) % kRing;
            const uint32_t   used = (t + kRing - h) % kRing;
            const bool       rel  = midiIsRelease(m);
            // Ring genuinely full: nothing can be written.  These events live in
            // per-block buffers, so there is nowhere to keep them — but by the
            // reserve below this can only be reached after kReleaseHeadroom
            // back-to-back releases without a single drain.
            if (next == h) { if (!rel) ++refused; continue; }
            // Release reserve: refuse note-ons early so every note-off still
            // finds a slot.  A lost note-on costs one note; a lost note-off
            // hangs an external synth until it is power-cycled.
            if (!rel && (uint32_t)(kRing - 1) - used < kReleaseHeadroom) { ++refused; continue; }
            ring_[t].dueSample = blockStart + (int64_t)m.sampleOffset;
            ring_[t].ev = m;
            t = next;
        }
    }
    tail_.store(t, std::memory_order_release);
    if (refused)
        dropped_.fetch_add(refused, std::memory_order_relaxed);
}

bool MidiOutNode::pop(TimedMidi& out) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return false;  // empty
    out = ring_[h];
    head_.store((h + 1) % kRing, std::memory_order_release);
    return true;
}

VirtualMidiPortsNode::VirtualMidiPortsNode() {
    for(auto& r:routes_)r.store(-1,std::memory_order_relaxed);
    setPorts(1, 1);
}

void VirtualMidiPortsNode::setPorts(int ins, int outs) {
    inputs_ = std::max(1, std::min(32, ins));
    outputs_ = std::max(1, std::min(32, outs));
    inNames_.resize((size_t)inputs_);
    outNames_.resize((size_t)outputs_);
    for (int i = 0; i < inputs_; ++i)
        inNames_[(size_t)i] = "i-" + std::to_string(i + 1);
    for (int i = 0; i < outputs_; ++i)
        outNames_[(size_t)i] = "o-" + std::to_string(i + 1);
    for(int o=outputs_;o<32;++o)routes_[o].store(-1,std::memory_order_relaxed);
    for(int o=0;o<outputs_;++o)if(routes_[o].load()>=inputs_)routes_[o].store(-1);
}

void VirtualMidiPortsNode::setRoute(int output,int input){if(output>=0&&output<outputs_)
    routes_[output].store(input>=0&&input<inputs_?input:-1,std::memory_order_release);}
int VirtualMidiPortsNode::route(int output) const{return output>=0&&output<outputs_?
    routes_[output].load(std::memory_order_acquire):-1;}

void VirtualMidiPortsNode::process(const NodeProcessContext& ctx) {
    // Metadata-only endpoint bank. Resolved direct graph edges carry MIDI.
    for(int o=0;o<ctx.numMidiOut;++o)ctx.midiOut[o].count=0;
}

// Spool an event for the NEXT block (offset 0 == "as early as possible"), so an
// over-capacity merge delays events instead of truncating them away.  Returns
// false only when even the carry buffer is full of releases.
bool MidiTrackNode::spill(const MidiEvent& e) {
    if(carryCount_>=kCarry){
        if(!midiIsRelease(e)) return false;             // carry full: lose a note-on
        for(int i=carryCount_-1;i>=0;--i)               // ...never a note-off
            if(!midiIsRelease(carry_[i])){ carry_[i]=e; carry_[i].sampleOffset=0; return true; }
        return false;
    }
    carry_[carryCount_]=e; carry_[carryCount_].sampleOffset=0; ++carryCount_; return true;
}

// Emit one event on "_track out".  Nothing is truncated away: what does not fit
// spools into carry_ and goes out first next block.  A RELEASE never waits — a
// late note-off is a note that keeps sounding — so it evicts the newest queued
// note-on and that note-on takes the carry slot instead.
void MidiTrackNode::emitOut(MidiBuffer& out,const MidiEvent& e,bool fromLive) {
    if(out.count<out.capacity){ out.ev[out.count++]=e; markHeld(e,fromLive); return; }
    if(midiIsRelease(e)){
        for(int i=out.count-1;i>=0;--i){
            if(midiIsRelease(out.ev[i])) continue;
            const MidiEvent displaced=out.ev[i];
            out.ev[i]=e; markHeld(e,fromLive);
            if(!spill(displaced)){                      // nowhere left: forget the note-on
                MidiEvent d=displaced;                  // so it cannot leave a phantom held
                d.status=(unsigned char)(0x80u|(unsigned)(displaced.status&0x0F));
                d.data2=0; markHeld(d,false);
            }
            return;
        }
    }
    if(spill(e)) markHeld(e,fromLive);
}

void MidiTrackNode::markHeld(const MidiEvent& e,bool fromLive) {
    const unsigned char hi=e.status&0xF0u;
    const int ch=e.status&0x0F, note=e.data1&0x7F;
    const int bit=ch*128+note; const uint32_t m=1u<<(bit&31); const int w=bit>>5;
    if(hi==0x90u&&e.data2!=0){
        if(!(outHeld_[w]&m)) sounding_.fetch_add(1,std::memory_order_relaxed);
        outHeld_[w]|=m; if(fromLive) liveHeld_[w]|=m;
    } else if(hi==0x80u||hi==0x90u){
        if(outHeld_[w]&m) sounding_.fetch_sub(1,std::memory_order_relaxed);
        outHeld_[w]&=~m; liveHeld_[w]&=~m;
    } else if(hi==0xB0u&&(e.data1==120||e.data1==123)){
        for(int i=ch*4;i<ch*4+4;++i){
            uint32_t b=outHeld_[i];
            while(b){b&=b-1;sounding_.fetch_sub(1,std::memory_order_relaxed);}
            outHeld_[i]=0; liveHeld_[i]=0;
        }
    }
}

// Force a note-off for every note flagged in `bits` (which is either liveHeld_
// -- the monitored notes only -- or outHeld_ -- everything this lane sounded).
void MidiTrackNode::releaseHeld(MidiBuffer& out,uint32_t* bits,bool liveOnly) {
    for(int w=0;w<kHeldWords;++w){
        uint32_t b=bits[w];
        while(b){
            int k=0; while(!((b>>k)&1u)) ++k;
            b&=~(1u<<k);
            const int idx=w*32+k, ch=idx>>7, note=idx&0x7F;
            MidiEvent off{}; off.sampleOffset=0;
            off.status=(unsigned char)(0x80u|(unsigned)ch);
            off.data1=(unsigned char)note; off.data2=0;
            // markHeld() inside emitOut clears BOTH tables for this note, so the
            // live-only sweep cannot leave a stale bit behind in outHeld_.
            emitOut(out,off,liveOnly);
        }
    }
}

void MidiTrackNode::process(const NodeProcessContext& ctx) {
    if(ctx.numMidiOut<1) return;
    MidiBuffer& out=ctx.midiOut[0]; out.count=0;

    // Routing is honoured here, not just stored: -1 == "None" on either endpoint
    // (same convention as VirtualMidiPortsNode::setRoute).
    const int  inSel  = input_.load(std::memory_order_acquire);
    const int  outSel = output_.load(std::memory_order_acquire);
    const bool routed = outSel>=0;                                   // assigned anywhere?
    const bool liveOn = monitor_.load(std::memory_order_acquire)&&inSel>=0;

    // 1. carry-over from an overflowing merge last block goes out first.
    const int carried=carryCount_; carryCount_=0;
    for(int i=0;i<carried;++i){
        if(out.count<out.capacity) out.ev[out.count++]=carry_[i];
        else carry_[carryCount_++]=carry_[i];                        // still no room
    }

    // 2. edges that must not strand a held note.  A routing change rewires
    //    "_track out" to a different endpoint, so release EVERYTHING first;
    //    dropping monitoring releases only the notes monitoring let through.
    if(inSel!=inputState_||outSel!=outputState_){
        releaseHeld(out,outHeld_,false);
        inputState_=inSel; outputState_=outSel;
    }
    if(!liveOn&&liveOpen_) releaseHeld(out,liveHeld_,true);
    liveOpen_=liveOn;

    // 3. merge playback + (monitored) live input.
    if(routed){
        if(ctx.numMidiIn>1){ const MidiBuffer& pb=ctx.midiIn[1];
            for(int i=0;i<pb.count;++i) emitOut(out,pb.ev[i],false); }
        if(liveOn&&ctx.numMidiIn>0){ const MidiBuffer& lv=ctx.midiIn[0];
            for(int i=0;i<lv.count;++i) emitOut(out,lv.ev[i],true); }
    }

    for(int i=1;i<out.count;++i){MidiEvent k=out.ev[i];int j=i-1;
        while(j>=0&&out.ev[j].sampleOffset>k.sampleOffset){out.ev[j+1]=out.ev[j];--j;}
        out.ev[j+1]=k;}

    // 4. capture.  A lane with no input assigned captures nothing (recording is
    //    independent of monitoring, so it is gated on the INPUT route only).
    if(!capture_.load(std::memory_order_relaxed)||inSel<0||ctx.numMidiIn<1) return;
    if(captureReset_.exchange(false,std::memory_order_acq_rel))
        {captureLast_=-1;captureWrap_=0;}
    const MidiBuffer& live=ctx.midiIn[0];
    uint32_t t=tail_.load(std::memory_order_relaxed);
    const uint32_t h=head_.load(std::memory_order_acquire);
    for(int i=0;i<live.count;++i){const uint32_t next=(t+1)%kRing;if(next==h)break;
        const MidiEvent& m=live.ev[i];
        const int64_t raw=m.captureSample>=0?m.captureSample:
            ctx.transport.playPositionSamples+m.sampleOffset;
        int64_t sample=raw+captureWrap_;
        if(captureLast_>=0&&sample<captureLast_){
            captureWrap_+=captureLast_+std::max(1,ctx.nframes)-sample;
            sample=raw+captureWrap_;
        }
        if(sample>captureLast_)captureLast_=sample;
        ring_[t]=Ev{sample,m.status,m.data1,m.data2};t=next;}
    tail_.store(t,std::memory_order_release);
    // captureLast_ used to advance ONLY on a captured event, so a wrap that
    // landed after several silent (event-free) blocks was anchored to a
    // stale position from long before the actual loop boundary: the gap
    // added above (one block's worth) then undershot the true elapsed time
    // by however long that silence was, and EVERY note in the following
    // pass was shifted early by that same shortfall for the rest of the
    // pass. Advancing captureLast_ to this block's own end -- even when
    // nothing was captured in it -- anchors the next wrap to the actual
    // last-seen transport position instead, bounding the error to at most
    // one block's width (the same block-granularity already inherent to
    // event delivery elsewhere in this engine).
    const int64_t blockEndRaw=ctx.transport.playPositionSamples
                              +std::max(1,ctx.nframes)+captureWrap_;
    if(blockEndRaw>captureLast_)captureLast_=blockEndRaw;
}

int MidiTrackNode::drain(Ev* out,int cap) {
    int n=0;uint32_t h=head_.load(std::memory_order_relaxed);
    const uint32_t t=tail_.load(std::memory_order_acquire);
    while(h!=t&&n<cap){out[n++]=ring_[h];h=(h+1)%kRing;}
    head_.store(h,std::memory_order_release);return n;
}

/* Legacy RecordNode removed: MidiTrackNode is the single MIDI capture path. */
#if 0
void RecordNode::process(const NodeProcessContext& ctx) {
    // MIDI thru: pass midi-in (port 1 == first midi-in) to the thru output.
    if (ctx.numMidiOut > 0) {
        int c = 0;
        if (ctx.numMidiIn > 0) {
            c = std::min(ctx.midiIn[0].count, ctx.midiOut[0].capacity);
            for (int i = 0; i < c; ++i) ctx.midiOut[0].ev[i] = ctx.midiIn[0].ev[i];
        }
        ctx.midiOut[0].count = c;
    }

    if (!recording_.load(std::memory_order_relaxed) || ctx.numMidiIn < 1) return;

    // Timestamp each event with the transport tick (ppqn = 192, as PatchKnob core).
    // ONE rounded conversion from the event's absolute sample position — a
    // truncated base tick plus a truncated offset tick loses up to 2 ticks.
    const double bpm = ctx.transport.tempoBpm > 0 ? ctx.transport.tempoBpm : 120.0;
    const double samplesPerTick = (sr_ * 60.0) / (bpm * 192.0);

    uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    for (int i = 0; i < ctx.midiIn[0].count; ++i) {
        if (((t + 1) % kRing) == (h % kRing)) break;   // ring full: drop
        const MidiEvent& m = ctx.midiIn[0].ev[i];
        long tick = samplesPerTick > 0
            ? (long)std::llround((double)(ctx.transport.playPositionSamples
                                          + (int64_t)m.sampleOffset) / samplesPerTick)
            : 0;
        ring_[t] = Ev{ tick, m.status, m.data1, m.data2 };
        t = (t + 1) % kRing;
    }
    tail_.store(t, std::memory_order_release);
}

int RecordNode::drain(Ev* out, int cap) {
    uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    int n = 0;
    while (h != t && n < cap) { out[n++] = ring_[h]; h = (h + 1) % kRing; }
    head_.store(h, std::memory_order_release);
    return n;
}
#endif

}}} // namespace PatchKnob::engine::patch
