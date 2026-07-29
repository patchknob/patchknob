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
    inFlight_.store(true, std::memory_order_seq_cst);
    IPluginInstance* inst = inst_.load(std::memory_order_seq_cst);
    if (!inst) { inFlight_.store(false, std::memory_order_release); return; }
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

    inFlight_.store(false, std::memory_order_release);
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
        if (mute_[ch].load(std::memory_order_relaxed)) { vu_[ch].store(0.f, std::memory_order_relaxed); continue; }
        const AudioBus& in = ctx.audioIn[ch];
        const float g = gain_[ch].load(std::memory_order_relaxed);
        const float p = pan_[ch].load(std::memory_order_relaxed);
        const float panL = p <= 0.f ? 1.f : 1.f - p;    // constant-gain stereo pan
        const float panR = p >= 0.f ? 1.f : 1.f + p;
        float peak = 0.f;
        const int cc = std::min(in.channels, out.channels);
        for (int c = 0; c < cc; ++c) {
            const float pg = (c == 0) ? panL : (c == 1 ? panR : 1.f);
            const float* sp = in.chans[c];
            float*       dp = out.chans[c];
            for (int i = 0; i < n; ++i) {
                const float s = sp[i] * g * pg;
                dp[i] += s;
                const float a = std::fabs(s);
                if (a > peak) peak = a;
            }
        }
        // An Inf input can latch the meter (NaN can't win a > compare, but Inf
        // does); publish only finite peaks so a poisoned block never sticks.
        vu_[ch].store(std::isfinite(peak) ? peak : 0.f, std::memory_order_relaxed);
    }
    // master (output) stage: apply the master gain + measure the master VU.
    // This is also the NaN/Inf fence for the mix bus — a non-finite sample from
    // any upstream node becomes silence here instead of reaching the device.
    const float mg = masterGain_.load(std::memory_order_relaxed);
    float mpeak = 0.f;
    for (int c = 0; c < out.channels; ++c) {
        float* dp = out.chans[c];
        for (int i = 0; i < n; ++i) {
            float s = dp[i] * mg;
            if (!std::isfinite(s)) s = 0.f;
            dp[i] = s;
            const float a = std::fabs(s);
            if (a > mpeak) mpeak = a;
        }
    }
    masterVu_.store(mpeak, std::memory_order_relaxed);
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

    int aiIdx = 0;   // next audio-in  port (track audio inlets)
    int aoIdx = 1;   // next audio-out port (0 == master, then track outlets)
    int miIdx = 0;   // next midi-in   port (track midi inlets)
    int moIdx = 1;   // next midi-out  port (0 == clock, then track outlets)

    for (int t = 0; t < ntracks; ++t) {
        if (!snap->midi[t]) {
            // AUDIO channel: sum inlet (gain/pan/mute) into master + echo raw inlet
            // to its own outlet (a direct/insert send).
            float peak = 0.f;
            if (aiIdx < ctx.numAudioIn) {
                const AudioBus& in = ctx.audioIn[aiIdx];
                const float g = mute(t) ? 0.f : gain(t);
                const float p = pan(t);
                const float panL = p <= 0.f ? 1.f : 1.f - p;
                const float panR = p >= 0.f ? 1.f : 1.f + p;
                const int cc = std::min(in.channels, master.channels);
                for (int c = 0; c < cc; ++c) {
                    const float pg = (c == 0) ? panL : (c == 1 ? panR : 1.f);
                    const float* sp = in.chans[c];
                    float*       dp = master.chans[c];
                    float* op = (aoIdx < ctx.numAudioOut && c < ctx.audioOut[aoIdx].channels)
                                    ? ctx.audioOut[aoIdx].chans[c] : nullptr;
                    for (int s = 0; s < n; ++s) {
                        const float v = sp[s] * g * pg;
                        dp[s] += v;
                        if (op) op[s] = sp[s];
                        const float a = std::fabs(v);
                        if (a > peak) peak = a;
                    }
                }
            } else if (aoIdx < ctx.numAudioOut) {          // no inlet -> silent outlet
                for (int c = 0; c < ctx.audioOut[aoIdx].channels; ++c)
                    std::memset(ctx.audioOut[aoIdx].chans[c], 0, sizeof(float) * (size_t)n);
            }
            setVu(t, std::isfinite(peak) ? peak : 0.f);   // Inf must not latch the meter
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
        }
    }

    // master gain + master VU on the summed output.  Like MixerNode, this is
    // the NaN/Inf fence: a poisoned upstream sample becomes silence here.
    const float mg = masterGain();
    float mpeak = 0.f;
    for (int c = 0; c < master.channels; ++c) {
        float* dp = master.chans[c];
        for (int s = 0; s < n; ++s) { float v = dp[s] * mg;
                                      if (!std::isfinite(v)) v = 0.f;
                                      dp[s] = v;
                                      const float a = std::fabs(v); if (a > mpeak) mpeak = a; }
    }
    setMasterVu(mpeak);

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
            if (!wasPlaying_) {
                const int64_t pos = ctx.transport.playPositionSamples;
                if (pos == 0) {
                    // stop -> play from the top: 0xFA Start.
                    if (cnt < clk.capacity) { clk.ev[cnt].sampleOffset = 0; clk.ev[cnt].status = 0xFA;
                                              clk.ev[cnt].data1 = 0; clk.ev[cnt].data2 = 0; ++cnt; }
                } else {
                    // continue mid-song: 0xF2 Song Position (14-bit MIDI beats,
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

    // Block done: advance the generation so publishSnapshot() can retire old
    // snapshots (a parked block is freed once this has advanced by >= 2).
    rtGen_.fetch_add(1, std::memory_order_release);
}

// ===========================================================================
// AudioDeviceOutNode
// ===========================================================================
void AudioDeviceOutNode::process(const NodeProcessContext& ctx) {
    if (!target_ || nframes_ <= 0 || ctx.numAudioIn < 1) return;
    // Never copy more frames than bindDeviceOut() actually provided — the
    // device buffers own exactly nframes_ frames whatever the block asks for.
    const int n = std::min(ctx.nframes, nframes_);
    const AudioBus& in = ctx.audioIn[0];
    const int cc = std::min(targetCh_, in.channels);
    for (int c = 0; c < cc; ++c) {
        float* dp = target_[c];
        if (!dp) continue;
        // SUM into the device buffer (PatchGraph::process() pre-zeroed it) so that
        // EVERY "Audio Out" module mixes into the output instead of the last one
        // overwriting the rest.  A NaN/Inf sample must never reach the DAC.
        const float* sp = in.chans[c];
        for (int i = 0; i < n; ++i) {
            const float v = sp[i];
            if (std::isfinite(v)) dp[i] += v;
        }
    }
    // The whole device buffer (incl. the tail + extra channels) was pre-zeroed by
    // PatchGraph::process(), so nothing to clear here.
}

// ===========================================================================
// AudioDeviceInNode
// ===========================================================================
void AudioDeviceInNode::process(const NodeProcessContext& ctx) {
    if (ctx.numAudioOut < 1) return;
    const int n = ctx.nframes;
    const AudioBus& out = ctx.audioOut[0];
    // Never read more frames than bindDeviceIn() actually provided; the tail
    // beyond the device buffer is zero-filled instead of read OOB.
    const int nsrc = std::min(n, nframes_ > 0 ? nframes_ : 0);
    const int cc = (src_ ? std::min(srcCh_, out.channels) : 0);
    for (int c = 0; c < cc; ++c) {
        if (src_[c]) {
            std::memcpy(out.chans[c], src_[c], sizeof(float) * (size_t)nsrc);
            if (nsrc < n)
                std::memset(out.chans[c] + nsrc, 0, sizeof(float) * (size_t)(n - nsrc));
        } else std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
    }
    for (int c = cc; c < out.channels; ++c)
        std::memset(out.chans[c], 0, sizeof(float) * (size_t)n);
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
        portNames_[(size_t)i] = (n == 1) ? std::string("out")
                                         : ("Instrument " + std::to_string(i + 1));
    }
}

bool MidiInNode::push(const MidiEvent& e, int64_t dueSample, int port) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t next = (t + 1) % kRing;
    if (next == head_.load(std::memory_order_acquire)) return false;  // full
    ring_[t].dueSample = dueSample;
    ring_[t].ev = e;
    // port < 0 == BROADCAST (0xFF): deliver to every out plug.  Used for live /
    // hardware keyboard input so it reaches ALL wired instruments (each channel-
    // filters downstream), the way the single-plug node behaved before per-track
    // plugs.  port >= 0 targets that one plug (per-track sequencer routing).
    ringPort_[t] = (uint8_t)((port < 0) ? 0xFF : (port > 254 ? 254 : port));
    tail_.store(next, std::memory_order_release);
    return true;
}

void MidiInNode::process(const NodeProcessContext& ctx) {
    const int nOut = ctx.numMidiOut;
    if (nOut < 1 || ctx.nframes < 1) return;
    for (int p = 0; p < nOut; ++p) ctx.midiOut[p].count = 0;   // reset all plugs

    // Deliver ONLY the events due inside this block, at exact sample offsets, each
    // to ITS target plug (track).  playPositionSamples is the block-START sample;
    // the ring is non-decreasing in dueSample (single monotonically-scheduling
    // producer), so the first future event ends the drain across every plug and
    // everything behind it waits its turn.
    const int64_t blockStart = ctx.transport.playPositionSamples;
    const int64_t blockEnd   = blockStart + (int64_t)ctx.nframes;

    const int reCh = outChannel_.load(std::memory_order_relaxed);   // -1 = passthrough
    uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    while (h != t) {
        const TimedMidi& tm = ring_[h];
        if (tm.dueSample >= blockEnd) break;           // future block: hold back
        const int rp = (int)ringPort_[h];
        MidiEvent e = tm.ev;                           // due<0 ("now") and late events clamp to 0
        // Re-stamp channel-voice messages onto outChannel_ (hardware input node) so
        // a keyboard plays the instrument it's wired to regardless of channel.
        if (reCh >= 0) {
            const unsigned char hi = e.status & 0xF0u;
            if (hi >= 0x80u && hi < 0xF0u) e.status = hi | (unsigned char)(reCh & 0x0F);
        }
        e.sampleOffset = (int32_t)std::clamp(tm.dueSample - blockStart,
                                             (int64_t)0, (int64_t)ctx.nframes - 1);
        if (rp == 0xFF) {                              // BROADCAST -> every plug
            for (int pp = 0; pp < nOut; ++pp) {
                MidiBuffer& out = ctx.midiOut[pp];
                if (out.count < out.capacity) out.ev[out.count++] = e;
            }
        } else {                                       // one plug (per-track routing)
            int p = (rp >= nOut) ? 0 : rp;             // stray/absent track -> plug 0
            MidiBuffer& out = ctx.midiOut[p];
            if (out.count < out.capacity) out.ev[out.count++] = e;
        }
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
    const MidiBuffer& in = ctx.midiIn[0];
    // Stamp each event's ABSOLUTE due sample (block start + in-block offset) so
    // the hardware drain can pace the wire to the audio clock, not the block.
    const int64_t blockStart = ctx.transport.playPositionSamples;
    uint32_t t = tail_.load(std::memory_order_relaxed);
    for (int i = 0; i < in.count; ++i) {
        const uint32_t next = (t + 1) % kRing;
        if (next == head_.load(std::memory_order_acquire)) break;  // full: drop rest
        ring_[t].dueSample = blockStart + (int64_t)in.ev[i].sampleOffset;
        ring_[t].ev = in.ev[i];
        t = next;
    }
    tail_.store(t, std::memory_order_release);
}

bool MidiOutNode::pop(TimedMidi& out) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return false;  // empty
    out = ring_[h];
    head_.store((h + 1) % kRing, std::memory_order_release);
    return true;
}

// ===========================================================================
// RecordNode
// ===========================================================================
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

}}} // namespace PatchKnob::engine::patch
