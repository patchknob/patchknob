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
static void repitch_segment(const float* inL, const float* inR, int64_t nIn,
                            int64_t s0, int64_t s1, int64_t d0, int64_t d1,
                            float* outL, float* outR)
{
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

    // READ the clip's channels in place -- both repitch_segment and
    // signalsmith only read their input, and the copies this used to take were
    // the full source (2 x 230 MB for a 10-minute clip) on the message thread
    // for every render.  Mono duplicates the left pointer.  The one case that
    // still copies is a genuinely RAGGED clip (ch[1] shorter than ch[0], a
    // mid-edit state): the right channel is zero-padded to ch[0]'s length so
    // the output is bit-identical to what the old always-copy path produced.
    const float* inL = in.ch[0].data();
    const float* inR = in.ch[1].empty() ? inL : in.ch[1].data();
    std::vector<float> paddedR;
    if (!in.ch[1].empty() && in.ch[1].size() < in.ch[0].size()) {
        paddedR = in.ch[1];
        paddedR.resize(in.ch[0].size(), 0.0f);
        inR = paddedR.data();
    }
    const int64_t nIn = (int64_t)in.ch[0].size();

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
            repitch_segment(inL, inR, nIn, s0, s1, d0, d1, outL, outR);
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
        // Prime from the FIRST MARKER's source position, not from sample 0.  A
        // map whose first marker starts partway into the file was primed with
        // audio it never plays, colouring the first window of every render.
        if (lat > (int)(nIn - s0)) lat = (int)(nIn - s0);
        if (lat < 0) lat = 0;
        if (lat > 0) {
            const float* prime[2] = { inL + s0, inR + s0 };
            stretch.seek(prime, lat, rate);
        }
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
        const float* ip[2] = { inL + s0, inR + s0 };
        float*       op[2] = { outL + d0, outR + d0 };
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
