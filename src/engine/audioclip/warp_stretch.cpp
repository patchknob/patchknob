//----------------------------------------------------------------------------
//  src/engine/audioclip/warp_stretch.cpp
//----------------------------------------------------------------------------
#include "warp_stretch.h"

#include <algorithm>
#include <vector>

#include "signalsmith-stretch.h"

namespace PatchKnob { namespace engine {

// Linear-interpolated resample of a source segment into a dst segment (Ableton
// Re-Pitch: pitch tracks speed, no phase vocoder).
static void repitch_segment(const std::vector<float>& inL, const std::vector<float>& inR,
                            int64_t s0, int64_t s1, int64_t d0, int64_t d1,
                            float* outL, float* outR)
{
    const int64_t nIn = (int64_t)inL.size();
    const int64_t outLen = d1 - d0, inLen = s1 - s0;
    if (outLen <= 0 || inLen <= 0) return;
    for (int64_t k = 0; k < outLen; ++k) {
        double sp = (double)s0 + (double)k * (double)inLen / (double)outLen;
        int64_t i0 = (int64_t)sp; double fr = sp - (double)i0;
        int64_t i1 = i0 + 1; if (i1 >= nIn) i1 = nIn - 1; if (i0 >= nIn) i0 = nIn - 1;
        if (i0 < 0) { i0 = 0; i1 = 0; fr = 0; }
        outL[d0 + k] = (float)((1.0 - fr) * inL[i0] + fr * inL[i1]);
        outR[d0 + k] = (float)((1.0 - fr) * inR[i0] + fr * inR[i1]);
    }
}

AudioClip warp_render(const AudioClip& in, const std::vector<WarpMarker>& markers,
                      double transposeSemitones, WarpMode mode, double formantSemitones)
{
    AudioClip out;
    if (in.empty() || markers.size() < 2) return out;

    const int64_t outFrames = markers.back().dstSample;
    if (outFrames <= 0) return out;

    const double sr = in.sampleRate > 0.0 ? in.sampleRate : 48000.0;
    out.sampleRate       = in.sampleRate;
    out.sourceSampleRate = in.sourceSampleRate;
    out.name             = in.name;
    out.resize(outFrames);

    // Mutable channel copies (signalsmith takes non-const buffer pointers; it
    // only reads the input).  Duplicate mono into both channels.
    std::vector<float> inL = in.ch[0];
    std::vector<float> inR = in.ch[1].empty() ? in.ch[0] : in.ch[1];
    const int64_t nIn = (int64_t)inL.size();

    float* outL = out.ch[0].data();
    float* outR = out.ch[1].data();

    // --- Re-Pitch: varispeed resample per segment (pitch follows speed) ------
    if (mode == WarpMode::RePitch) {
        for (size_t i = 0; i + 1 < markers.size(); ++i) {
            int64_t s0 = std::max<int64_t>(0, markers[i].srcSample);
            int64_t s1 = std::min<int64_t>(nIn, markers[i + 1].srcSample);
            int64_t d0 = std::max<int64_t>(0, markers[i].dstSample);
            int64_t d1 = std::min<int64_t>(outFrames, markers[i + 1].dstSample);
            if (s1 <= s0 || d1 <= d0) continue;
            repitch_segment(inL, inR, s0, s1, d0, d1, outL, outR);
        }
        return out;
    }

    // --- phase-vocoder modes (Complex / Tones / Texture / Beats / Pro) -------
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    if (mode == WarpMode::Texture || mode == WarpMode::Beats)
        stretch.presetCheaper(2, (float)sr);       // shorter window: crisper transients
    else
        stretch.presetDefault(2, (float)sr);
    if (transposeSemitones != 0.0)
        stretch.setTransposeSemitones((float)transposeSemitones);
    if (mode == WarpMode::ComplexPro)
        stretch.setFormantSemitones((float)formantSemitones, /*compensatePitch*/true);

    // Prime the analysis with the first segment's lead-in so the first emitted
    // output sample is aligned (compensates the phase-vocoder latency).
    {
        const int64_t s0 = std::max<int64_t>(0, markers[0].srcSample);
        const int64_t s1 = std::min<int64_t>(nIn, markers[1].srcSample);
        const int64_t d0 = markers[0].dstSample, d1 = markers[1].dstSample;
        const double rate = (s1 > s0 && d1 > d0) ? (double)(s1 - s0) / (double)(d1 - d0) : 1.0;
        int lat = stretch.inputLatency();
        if (lat > (int)nIn) lat = (int)nIn;
        float* prime[2] = { inL.data(), inR.data() };
        stretch.seek(prime, lat, rate);
    }

    // Time-varying stretch: each segment gets its own in/out length (= ratio).
    for (size_t i = 0; i + 1 < markers.size(); ++i) {
        int64_t s0 = markers[i].srcSample,     s1 = markers[i + 1].srcSample;
        int64_t d0 = markers[i].dstSample,     d1 = markers[i + 1].dstSample;
        if (s0 < 0) s0 = 0;
        if (s1 > nIn) s1 = nIn;
        if (d0 < 0) d0 = 0;
        if (d1 > outFrames) d1 = outFrames;
        if (s1 <= s0 || d1 <= d0) continue;
        float* ip[2] = { inL.data() + s0, inR.data() + s0 };
        float* op[2] = { outL + d0,       outR + d0 };
        stretch.process(ip, (int)(s1 - s0), op, (int)(d1 - d0));
    }

    return out;
}

AudioClip warp_to_length(const AudioClip& in, int64_t outFrames, double transposeSemitones)
{
    if (in.empty() || outFrames <= 0) return AudioClip{};
    std::vector<WarpMarker> m = { { 0, 0 }, { in.numFrames(), outFrames } };
    return warp_render(in, m, transposeSemitones);
}

}} // namespace PatchKnob::engine
