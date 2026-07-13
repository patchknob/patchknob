//----------------------------------------------------------------------------
//  seq24 Windows port — MixerGraph implementation. See mixer_graph.h.
//----------------------------------------------------------------------------
#include "mixer_graph.h"

#include <algorithm>
#include <cmath>

namespace seq24 { namespace engine {

void MixerGraph::setTrackCount(int n) {
    if (n < 0) n = 0;
    tracks_.clear();
    tracks_.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
        tracks_.emplace_back(new Track());
}

bool MixerGraph::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    maxBlock_   = maxBlockSize;

    trkL_.assign((size_t)maxBlockSize, 0.0f);
    trkR_.assign((size_t)maxBlockSize, 0.0f);

    masterVuL_.setSampleRate(sampleRate);
    masterVuR_.setSampleRate(sampleRate);
    masterVuL_.reset();
    masterVuR_.reset();

    bool ok = true;
    for (auto& t : tracks_)
        ok = t->prepare(sampleRate, maxBlockSize) && ok;

    prepared_ = true;
    return ok;
}

void MixerGraph::release() {
    for (auto& t : tracks_) t->release();
    prepared_ = false;
}

void MixerGraph::renderBlock(float** out, int numChannels, int nframes,
                             const TrackBlockInput* inputs, int numInputs) {
    if (nframes <= 0 || numChannels <= 0) return;
    if (nframes > maxBlock_) nframes = maxBlock_;

    // Master accumulators live directly in out[0]/out[1]; zero them first.
    const int outCh = numChannels;
    for (int c = 0; c < outCh; ++c)
        for (int i = 0; i < nframes; ++i) out[c][i] = 0.0f;

    float* mL = out[0];
    float* mR = (outCh > 1) ? out[1] : out[0];

    // Solo logic: if ANY track is soloed, only soloed (and non-muted) tracks
    // are audible.
    bool anySolo = false;
    for (auto& t : tracks_) if (t->solo()) { anySolo = true; break; }

    RenderContext ctx;
    ctx.tempoBpm            = tempo_.load(std::memory_order_relaxed);
    ctx.playPositionSamples = playPos_.load(std::memory_order_relaxed);
    ctx.isPlaying           = playing_.load(std::memory_order_relaxed);

    float* trkOut[2] = { trkL_.data(), trkR_.data() };

    const int nt = (int)tracks_.size();
    for (int ti = 0; ti < nt; ++ti) {
        Track* t = tracks_[ti].get();

        // Efficiency: a track with no instrument AND no insert FX can only ever
        // produce silence, so skip it entirely.  With few instruments loaded
        // this avoids processing the ~30 empty tracks every block, freeing CPU
        // for audio (esp. on a minimal-Linux/framebuffer target).
        if (t->instrument() == nullptr && t->fxCount() == 0)
            continue;

        // Audibility: muted tracks are silent; with solo active, only soloed.
        const bool audible = !t->mute() && (!anySolo || t->solo());

        const MidiEvent*   midi   = nullptr; int nMidi  = 0;
        const ParamChange* autom  = nullptr; int nAutom = 0;
        if (inputs && ti < numInputs) {
            midi   = inputs[ti].midi;   nMidi  = inputs[ti].nMidi;
            autom  = inputs[ti].autom;  nAutom = inputs[ti].nAutom;
        }

        // Always process the track (so its plugins/VU stay live and tails ring
        // out), but only sum it into the master when audible. Track mute zeros
        // its own output internally too; the audible gate covers solo.
        t->processBlock(midi, nMidi, autom, nAutom, trkOut, nframes, ctx);

        if (audible) {
            for (int i = 0; i < nframes; ++i) {
                mL[i] += trkOut[0][i];
                mR[i] += trkOut[1][i];
            }
        }
    }

    // Master gain.
    const float mg = masterGain_.load(std::memory_order_relaxed);
    if (mg != 1.0f) {
        for (int i = 0; i < nframes; ++i) { mL[i] *= mg; mR[i] *= mg; }
    }

    // Master VU (post master gain).
    masterVuL_.push(mL, nframes);
    masterVuR_.push(mR, nframes);

    // If more than 2 output channels were requested, leave 2..N as already
    // zeroed (we only render a stereo master here).
}

void MixerGraph::render(float** out, int numChannels, int nframes, double sampleRate) {
    (void)sampleRate; // stream SR is fixed at prepare(); kept for callback ABI.
    renderBlock(out, numChannels, nframes, stagedInputs_, stagedNumInputs_);
}

}} // namespace seq24::engine
