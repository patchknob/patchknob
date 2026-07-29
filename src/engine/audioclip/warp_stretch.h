//----------------------------------------------------------------------------
//  PatchKnob -- audio WARP / time-stretch (Ableton-style).
//
//  A warp map is a piecewise-linear function between SOURCE sample time and
//  TIMELINE (destination) sample time, pinned by warp markers.  Between two
//  adjacent markers the audio is stretched by one constant ratio =
//  (dst span) / (src span).  Rendering the map yields a new AudioClip at the
//  destination length, pitch-preserving (unless a transpose is applied).
//
//  Uses the vendored MIT signalsmith-stretch (header-only phase vocoder); its
//  ratio IS output/input samples per process() call, so a time-varying warp map
//  falls out directly by processing each segment with its own in/out length.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUDIOCLIP_WARP_STRETCH_H
#define PATCHKNOB_ENGINE_AUDIOCLIP_WARP_STRETCH_H

#include <cstdint>
#include <vector>

#include "audio_clip.h"

namespace PatchKnob { namespace engine {

//! One warp control point: SOURCE sample `srcSample` is pinned to TIMELINE
//! sample `dstSample`.  Markers must be sorted ascending in both fields.
struct WarpMarker {
    int64_t srcSample = 0;
    int64_t dstSample = 0;
};

//! Ableton-style warp modes.  Complex/Tones/Texture/Beats all run through the
//! pitch-preserving phase vocoder (signalsmith); RePitch resamples (varispeed,
//! pitch tracks tempo).  ComplexPro adds formant preservation under transpose.
enum class WarpMode { Complex, Tones, Texture, Beats, ComplexPro, RePitch };

//! Render `in` warped per the marker map into a NEW clip of length
//! markers.back().dstSample.  Needs >= 2 markers.  `transposeSemitones` shifts
//! pitch (ignored for RePitch, whose pitch follows the stretch).  `formant`
//! (ComplexPro only) preserves resonances under transpose.  Empty clip on error.
AudioClip warp_render(const AudioClip& in, const std::vector<WarpMarker>& markers,
                      double transposeSemitones = 0.0,
                      WarpMode mode = WarpMode::Complex,
                      double formantSemitones = 0.0);

//! Convenience: stretch the whole clip to `outFrames` at a single fixed ratio
//! (Ableton "Warp as X-bar loop" / clip-to-grid).  Pitch-preserving by default.
AudioClip warp_to_length(const AudioClip& in, int64_t outFrames,
                         double transposeSemitones = 0.0);

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIOCLIP_WARP_STRETCH_H
