//----------------------------------------------------------------------------
//  PatchKnob — Track implementation. See track.h for the design and
//  the FX-chain hot-swap (RCU snapshot) documentation.
//----------------------------------------------------------------------------
#include "track.h"

#include <algorithm>
#include <cmath>

namespace PatchKnob { namespace engine {

Track::Track() {
    // Publish an initial empty snapshot so the audio thread always has a valid
    // pointer even before prepare()/any edit.
    snapStore_[0].count = 0;
    activeSlot_.store(0, std::memory_order_relaxed);
    liveChain_.store(&snapStore_[0], std::memory_order_release);
}

Track::~Track() = default;

// ---------------------------------------------------------------------------
// FX chain editing (message thread). All mutations rebuild the editable list
// then republish an immutable snapshot via publishChain().
// ---------------------------------------------------------------------------

bool Track::addFx(IPluginInstance* fx) {
    if (!fx) return false;
    if ((int)fxEdit_.size() >= kMaxFx) return false;
    fxEdit_.push_back(fx);
    publishChain();
    return true;
}

bool Track::removeFx(int index) {
    if (index < 0 || index >= (int)fxEdit_.size()) return false;
    fxEdit_.erase(fxEdit_.begin() + index);
    publishChain();
    return true;
}

bool Track::moveFx(int from, int to) {
    const int n = (int)fxEdit_.size();
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    IPluginInstance* moved = fxEdit_[from];
    fxEdit_.erase(fxEdit_.begin() + from);
    fxEdit_.insert(fxEdit_.begin() + to, moved);
    publishChain();
    return true;
}

void Track::publishChain() {
    // Build the new snapshot into the inactive slot, then atomically flip.
    const int cur  = activeSlot_.load(std::memory_order_relaxed);
    const int next = cur ^ 1;
    FxChainSnapshot& dst = snapStore_[next];

    const int count = std::min((int)fxEdit_.size(), kMaxFx);
    for (int i = 0; i < count; ++i) dst.fx[i] = fxEdit_[i];
    dst.count = count;

    // Publish: release-store the pointer, then record the new active slot.
    liveChain_.store(&dst, std::memory_order_release);
    activeSlot_.store(next, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Track::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    maxBlock_   = maxBlockSize;

    for (int c = 0; c < 2; ++c) {
        bufA_[c].assign((size_t)maxBlockSize, 0.0f);
        bufB_[c].assign((size_t)maxBlockSize, 0.0f);
        chA_[c] = bufA_[c].data();
        chB_[c] = bufB_[c].data();
    }

    vuL_.setSampleRate(sampleRate);
    vuR_.setSampleRate(sampleRate);
    vuL_.reset();
    vuR_.reset();

    bool ok = true;
    if (instrument_) {
        ok = instrument_->prepare(sampleRate, maxBlockSize) && ok;
        instrument_->setActive(true);
    }
    for (IPluginInstance* fx : fxEdit_) {
        if (fx) {
            ok = fx->prepare(sampleRate, maxBlockSize) && ok;
            fx->setActive(true);
        }
    }

    // Make sure the live snapshot reflects the current editable list.
    publishChain();

    prepared_ = true;
    return ok;
}

void Track::release() {
    if (instrument_) instrument_->setActive(false);
    for (IPluginInstance* fx : fxEdit_) if (fx) fx->setActive(false);
    prepared_ = false;
}

// ---------------------------------------------------------------------------
// Realtime render
// ---------------------------------------------------------------------------

void Track::processBlock(const MidiEvent* midi, int nMidi,
                         const ParamChange* autom, int nAutom,
                         float** out, int nframes,
                         const RenderContext& ctx) {
    if (nframes <= 0) return;
    if (nframes > maxBlock_) nframes = maxBlock_;   // clamp; never overrun scratch

    // Disabled (e.g. a frozen source track): emit silence and run no plugins.
    if (disabled_.load(std::memory_order_relaxed)) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < nframes; ++i) out[c][i] = 0.0f;
        return;
    }

    // Ping-pong pointers: cur holds the "current" audio, nxt the destination.
    float* cur[2] = { chA_[0], chA_[1] };
    float* nxt[2] = { chB_[0], chB_[1] };

    // ---- 1. Instrument (synth) -------------------------------------------
    if (instrument_) {
        // Synth: clear input-side buffers (it produces audio from MIDI), then
        // process into cur[].
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < nframes; ++i) cur[c][i] = 0.0f;

        ProcessBlock blk;
        const float* inPtrs[2]  = { cur[0], cur[1] };
        float*       outPtrs[2] = { cur[0], cur[1] };
        blk.audioIn             = inPtrs;
        blk.audioOut            = outPtrs;
        blk.nframes             = nframes;
        blk.midiIn              = midi;
        blk.numMidiIn           = nMidi;
        blk.paramIn             = autom;
        blk.numParamIn          = nAutom;
        blk.tempoBpm            = ctx.tempoBpm;
        blk.playPositionSamples = ctx.playPositionSamples;
        blk.isPlaying           = ctx.isPlaying;
        instrument_->process(blk);
    } else {
        // No instrument: start from silence (an audio-input track would copy
        // its source here; we have no live audio input in this graph).
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < nframes; ++i) cur[c][i] = 0.0f;
    }

    // ---- 2. FX chain in series (RCU snapshot read once) ------------------
    FxChainSnapshot* chain = liveChain_.load(std::memory_order_acquire);
    if (chain) {
        for (int fxi = 0; fxi < chain->count; ++fxi) {
            IPluginInstance* fx = chain->fx[fxi];
            if (!fx) continue;

            ProcessBlock blk;
            const float* inPtrs[2]  = { cur[0], cur[1] };
            float*       outPtrs[2] = { nxt[0], nxt[1] };
            blk.audioIn             = inPtrs;
            blk.audioOut            = outPtrs;
            blk.nframes             = nframes;
            blk.midiIn              = midi;        // FX may consume MIDI (e.g. arps)
            blk.numMidiIn           = nMidi;
            blk.paramIn             = autom;
            blk.numParamIn          = nAutom;
            blk.tempoBpm            = ctx.tempoBpm;
            blk.playPositionSamples = ctx.playPositionSamples;
            blk.isPlaying           = ctx.isPlaying;
            fx->process(blk);

            // swap cur <-> nxt
            float* t0 = cur[0]; float* t1 = cur[1];
            cur[0] = nxt[0]; cur[1] = nxt[1];
            nxt[0] = t0;     nxt[1] = t1;
        }
    }

    // ---- 3. Gain / pan ---------------------------------------------------
    const float g  = gain_.load(std::memory_order_relaxed);
    const float p  = std::max(-1.0f, std::min(1.0f, pan_.load(std::memory_order_relaxed)));
    const bool  mu = mute_.load(std::memory_order_relaxed);

    // Constant-power pan law: angle 0..pi/2 as pan goes -1..1.
    const float panAngle = (p + 1.0f) * 0.25f * 3.14159265358979323846f; // 0..pi/2
    float gl = std::cos(panAngle) * g;
    float gr = std::sin(panAngle) * g;
    if (mu) { gl = 0.0f; gr = 0.0f; }

    for (int i = 0; i < nframes; ++i) {
        out[0][i] = cur[0][i] * gl;
        out[1][i] = cur[1][i] * gr;
    }

    // ---- 4. VU (post gain/pan, what the listener hears) ------------------
    vuL_.push(out[0], nframes);
    vuR_.push(out[1], nframes);
}

}} // namespace PatchKnob::engine
