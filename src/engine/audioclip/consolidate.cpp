#include "consolidate.h"

#include <algorithm>

namespace PatchKnob { namespace engine {

AudioClip consolidateRender(const std::vector<ConsolidatePiece>& pieces,
                            long long startFrame, long long endFrame,
                            double sampleRate)
{
    AudioClip out;
    out.sampleRate = out.sourceSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    out.name = "Consolidated";
    if (endFrame <= startFrame) return out;

    const long long lenF = endFrame - startFrame;
    out.ch[0].assign((size_t)lenF, 0.f);
    out.ch[1].assign((size_t)lenF, 0.f);

    for (size_t i = 0; i < pieces.size(); ++i) {
        const ConsolidatePiece& p = pieces[i];
        if (!p.clip || p.muted || p.len <= 0) continue;      // silence

        const long long nfr = (long long)p.clip->numFrames();
        if (nfr <= 0) continue;

        //  Only the part of the piece that lands inside the output window.
        const long long ovA = std::max(startFrame, p.pos);
        const long long ovB = std::min(endFrame,   p.pos + p.len);
        if (ovB <= ovA) continue;

        const float* L = p.clip->ch[0].data();
        const float* R = p.clip->ch[1].empty() ? p.clip->ch[0].data()
                                               : p.clip->ch[1].data();
        for (long long f = ovA; f < ovB; ++f) {
            long long srcF = p.off + (f - p.pos);
            if (p.loop && srcF >= nfr) srcF %= nfr;
            if (srcF < 0 || srcF >= nfr) continue;
            float g = p.gain;
            //  The piece's own fade, evaluated against ITS length -- the same
            //  curve playback applies, so a consolidated fade is audibly
            //  identical to the fade it replaces.
            if (p.hasFade) g *= p.fade.fadeGain(f - p.pos, p.len);
            out.ch[0][(size_t)(f - startFrame)] += L[(size_t)srcF] * g;
            out.ch[1][(size_t)(f - startFrame)] += R[(size_t)srcF] * g;
        }
    }
    return out;
}

}} // namespace PatchKnob::engine
