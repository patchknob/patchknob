//----------------------------------------------------------------------------
//  PatchKnob — AudioClip: an in-memory stereo audio buffer.
//
//  An AudioClip is the raw material an audio track plays back. It is a pair of
//  planar (non-interleaved) float channel buffers plus metadata:
//
//      * name              : display name (e.g. the source file's stem)
//      * sourceSampleRate  : the sample rate of the ORIGINAL source (metadata).
//                            Files loaded via wav_loader keep this for display
//                            even after the audio is resampled to engine rate.
//      * sampleRate        : the sample rate the STORED samples are in. After
//                            loading/resampling this equals the engine rate, so
//                            the AudioClipPlayer can read frames 1:1.
//      * ch[0] / ch[1]     : left / right planar sample buffers, ALWAYS both
//                            present and the same length. Mono sources are
//                            duplicated into both channels at load time so the
//                            realtime player never has to branch on channel
//                            count.
//
//  A ScheduledClip places a clip on the timeline at an absolute start sample
//  with a per-clip gain. It is a non-owning reference: the clip must outlive
//  any schedule that names it (standard host lifetime responsibility, same as
//  the FX-chain snapshot in graph/track.h).
//
//  Buffer convention matches plugin_api.h everywhere: float, non-interleaved.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_H
#define PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_H

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>

namespace PatchKnob { namespace engine {

//! In-memory stereo audio buffer + metadata. Message-thread owned; the audio
//! thread only ever reads a clip's samples through a published schedule.
struct AudioClip {
    std::string        name;
    double             sampleRate       = 48000.0;  //!< rate of the stored samples
    double             sourceSampleRate = 48000.0;  //!< original source rate (metadata)
    std::vector<float> ch[2];                        //!< L / R, always same length

    //! Number of sample frames in the clip (both channels are this long).
    int64_t numFrames() const { return (int64_t)ch[0].size(); }

    //! True if the clip carries no audio.
    bool empty() const { return ch[0].empty(); }

    //! Allocate `frames` of stereo silence (message thread).
    void resize(int64_t frames) {
        if (frames < 0) frames = 0;              // never cast a negative to size_t
        ch[0].assign((size_t)frames, 0.0f);
        ch[1].assign((size_t)frames, 0.0f);
    }

    //! Frames that are safe to read on BOTH channels.  The invariant is "both
    //! channels always the same length", but a clip is message-thread owned and
    //! can be edited after it was scheduled, so every realtime reader bounds
    //! against this rather than trusting numFrames().
    int64_t safeFrames() const {
        const size_t a = ch[0].size(), b = ch[1].size();
        return (int64_t)(a < b ? a : b);
    }

    //! True when the two channel buffers disagree -- i.e. the clip is mid-edit
    //! or was built by hand and must not be read by the audio thread.
    bool channelsRagged() const { return ch[0].size() != ch[1].size(); }

    //! Test/helper: synthesise a stereo sine tone clip so unit tests need no
    //! file on disk. Amplitude 0..1, both channels identical.
    static AudioClip synth_sine(double freqHz,
                                double durationSeconds,
                                double sampleRate,
                                float  amplitude = 0.8f,
                                const std::string& name = "sine") {
        AudioClip c;
        c.name             = name;
        c.sampleRate       = sampleRate > 0.0 ? sampleRate : 48000.0;
        c.sourceSampleRate = c.sampleRate;
        // BUG: a negative duration (or rate) reached resize() and was cast to
        // size_t, asking for a ~2^64 allocation -- length_error or a hard OOM
        // rather than an empty clip.
        int64_t n = (int64_t)std::llround(durationSeconds * c.sampleRate);
        if (n < 0) n = 0;
        c.resize(n);
        const double twoPi = 2.0 * 3.14159265358979323846;
        const double w = twoPi * freqHz / sampleRate;
        for (int64_t i = 0; i < n; ++i) {
            const float s = amplitude * (float)std::sin(w * (double)i);
            c.ch[0][(size_t)i] = s;
            c.ch[1][(size_t)i] = s;
        }
        return c;
    }
};

//! A REGION: an audio `clip` (the source) placed on the timeline as an
//! independent, non-destructive view -- the Ardour model.  The region occupies
//! timeline samples [startSample, startSample+regionLength()) and plays SOURCE
//! samples [sourceOffset, sourceOffset+regionLength()).  Trimming changes
//! `length` (and, from the left, `sourceOffset` too); slipping changes
//! `sourceOffset` alone; moving changes `startSample` alone.  Non-owning: `clip`
//! must outlive the schedule that references it.
struct ScheduledClip {
    uint64_t         regionId     = 0;       //!< stable identity; slices may share `clip`
    const AudioClip* clip         = nullptr;
    int64_t          startSample  = 0;     //!< timeline sample of the region START
    int64_t          sourceOffset = 0;     //!< first SOURCE frame the region plays
    int64_t          length       = 0;     //!< region length in frames (0 = to source end)
    float            gain         = 1.0f;  //!< region gain (Ardour _scale_amplitude)
    bool             muted        = false; //!< region mute (silent but kept)
    bool             loop         = false; //!< loop the source to fill `length`
    //! LOOP PERIOD in SOURCE frames: how much of the source one loop pass plays,
    //! starting at `sourceOffset`.  0 == "everything from sourceOffset to the end
    //! of the source" (the old, and still the default, behaviour).
    //!
    //! It has to be its own field.  Once `loop` is set, `length` is the region's
    //! TIMELINE span -- the thing the user drags out to fill four bars -- so it
    //! can no longer also carry the trimmed source span, and the wrap fell back
    //! to the whole remaining source: a region trimmed to one bar out of a long
    //! file looped the whole file instead of that bar.  AudioClipPlayer captures
    //! it from the region's own trim when looping is switched on.
    int64_t          loopLength   = 0;
    //! Fade envelope (applied on top of gain in AudioClipPlayer::process).
    //! Lengths in frames; tension in [-1,+1] (0 linear, >0 convex/slow-start,
    //! <0 concave/fast-start).  Overlapping fades on adjacent regions crossfade.
    int64_t          fadeInFrames  = 0;
    int64_t          fadeOutFrames = 0;
    float            fadeInTension  = 0.0f;
    float            fadeOutTension = 0.0f;
    //! Fade SHAPE per half (Pro Tools ch.32): Standard is the hand-editable
    //! tension curve above, S-Curve mirrors that tension into an S, and the
    //! seven parabolic presets are fixed curves (preset 1 = full level for the
    //! whole fade then an instant drop, ... 4 = linear, ... 7 = silent until
    //! the very end).  See fadeCurve().
    static constexpr uint8_t kFadeShapeStandard = 0;
    static constexpr uint8_t kFadeShapeSCurve   = 1;
    static constexpr uint8_t kFadeShapePreset1  = 2;   //!< presets occupy 2..8
    //! Fade SLOPE per half: Equal Gain plays the curve as-is (two linear halves
    //! sum to unity -- phase-coherent material); Equal Power plays the square
    //! root of the curve (the two halves sum to unity POWER -- unrelated
    //! material, avoids the mid-crossfade dip).
    static constexpr uint8_t kFadeSlopeEqualGain  = 0;
    static constexpr uint8_t kFadeSlopeEqualPower = 1;
    uint8_t          fadeInShape  = 0;   //!< kFadeShape* for the fade-in half
    uint8_t          fadeOutShape = 0;   //!< kFadeShape* for the fade-out half
    uint8_t          fadeInSlope  = 0;   //!< kFadeSlope* for the fade-in half
    uint8_t          fadeOutSlope = 0;   //!< kFadeSlope* for the fade-out half
    //! Crossfade LINK (0 Equal Power, 1 Equal Gain, 2 None) of the crossfade
    //! this region's fade-IN belongs to.  EDIT-TIME metadata only -- the
    //! renderer never reads it -- carried here so the arrangement's crossfade
    //! editing behaviour survives the project save/load round trip.
    uint8_t          xfadeLink = 0;

    //! The source offset actually used for playback.  regionLength() used to
    //! clamp this into [0,n] in a LOCAL, while process() read the raw field --
    //! so a region with a negative or past-the-end sourceOffset computed its
    //! length from the clamped value but fetched samples from the unclamped one
    //! and rendered silence.  Both now go through here.
    int64_t effectiveSourceOffset() const {
        if (!clip) return 0;
        const int64_t n = clip->safeFrames();
        return sourceOffset < 0 ? 0 : (sourceOffset > n ? n : sourceOffset);
    }

    //! Effective region length in frames, clamped to the source available from
    //! `sourceOffset`.  `length==0` means "to the end of the source".
    int64_t regionLength() const {
        if (!clip) return 0;
        const int64_t n = clip->safeFrames();
        const int64_t off = effectiveSourceOffset();
        int64_t len = length > 0 ? length : (n - off);
        if (!loop && off + len > n) len = n - off;             // clamp to source (unless looping)
        return len < 0 ? 0 : len;
    }

    //! One-past-the-end timeline sample of this placement.
    int64_t endSample() const { return startSample + regionLength(); }

    //! SOURCE frames one loop pass plays: the region's own trimmed span
    //! (`loopLength`) when it has one, otherwise everything from `sourceOffset`
    //! to the end of the source.  Always clamped to the audio that exists.
    int64_t loopSpan() const {
        if (!clip) return 0;
        const int64_t n   = clip->safeFrames();
        const int64_t off = effectiveSourceOffset();
        int64_t avail = n - off;
        if (avail < 0) avail = 0;
        const int64_t want = loopLength > 0 ? loopLength : avail;
        return want < avail ? want : avail;
    }

    // ---- declick ------------------------------------------------------------
    //! Declick ramp shape (smoothstep) -- the SAME curve the sampler's declick
    //! uses.  s'(0) == s'(1) == 0, so the ramp adds no corner of its own, and
    //! s(t) + s(1-t) == 1, so a ramp down and the matching ramp up sum to unity.
    static float declickShape(float t) {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t * t * (3.0f - 2.0f * t);
    }

    //! The declick window actually used across `span` frames: the requested
    //! length, never more than a quarter of the span, so the two ends of a very
    //! short region (or loop pass) can never meet or overlap.
    static int64_t declickWindow(int64_t want, int64_t span) {
        if (want <= 0 || span <= 0) return 0;
        const int64_t cap = span / 4;
        return want < cap ? want : cap;
    }

    //! Region-EDGE declick gain at region-relative frame `ri`: a `want`-frame
    //! ramp in from silence at the region start and out to silence at its end,
    //! so a trimmed region never starts or ends on a raw step.  An edge that
    //! carries a real (user) fade is left to that fade alone.
    float edgeDeclickGain(int64_t ri, int64_t regionLen, int64_t want) const {
        const int64_t d = declickWindow(want, regionLen);
        if (d <= 0) return 1.0f;
        float g = 1.0f;
        if (fadeInFrames  <= 0 && ri < d)
            g *= declickShape((float)ri / (float)d);
        if (fadeOutFrames <= 0 && ri >= regionLen - d)
            g *= declickShape((float)(regionLen - ri) / (float)d);
        return g;
    }

    //! Tension-shaped 0..1 ramp: u in [0,1] -> [0,1]; k bends the curve.
    static float fadeShape(float u, float k) {
        if (u <= 0.0f) return 0.0f;
        if (u >= 1.0f) return 1.0f;
        if (k > -1e-4f && k < 1e-4f) return u;                 // linear
        const float a = k * 3.0f;                              // steepness
        return (std::exp(a * u) - 1.0f) / (std::exp(a) - 1.0f);
    }

    //! The full Pro Tools fade curve: level (0..1) at ramp position `u`
    //! (0 = silent end, 1 = loud end -- a fade-in feeds its progress, a
    //! fade-out its REMAINING fraction, so one function serves both halves and
    //! the preset descriptions mirror exactly as the manual's do).  `shape` is
    //! kFadeShape*, `k` the hand-edited tension (Standard / S-Curve only),
    //! `slope` kFadeSlope*.  shape 0 / slope 0 reproduces fadeShape(u,k)
    //! bit-for-bit, which is what keeps the ch.27 punch crossfades (linear
    //! tension-0 fades) rendering exactly as before.
    static float fadeCurve(float u, int shape, float k, int slope) {
        if (u <= 0.0f) return 0.0f;
        if (u >= 1.0f) return 1.0f;
        float a;
        switch (shape) {
        default:
        case 0: a = fadeShape(u, k); break;                    // Standard
        case 1:                                                // S-Curve
            a = (u < 0.5f) ? 0.5f * fadeShape(2.0f * u, k)
                           : 1.0f - 0.5f * fadeShape(2.0f * (1.0f - u), k);
            break;
        // The seven parabolic presets (p745-746): 1 holds full level for the
        // whole fade, 7 holds silence; between them power-law curves step from
        // "keeps the volume fairly high" to "drops the volume quickly".
        case 2: a = 1.0f; break;                               // preset 1
        case 3: a = std::pow(u, 0.25f); break;                 // preset 2
        case 4: a = std::sqrt(u); break;                       // preset 3
        case 5: a = u; break;                                  // preset 4 (linear)
        case 6: a = u * u; break;                              // preset 5
        case 7: a = u * u * u * u; break;                      // preset 6
        case 8: a = 0.0f; break;                               // preset 7
        }
        if (slope == kFadeSlopeEqualPower) a = std::sqrt(a);
        return a;
    }

    //! Fade lengths actually applied, clamped to the region and shrunk
    //! proportionally when they would overlap.
    //!
    //! Clamping lives HERE, not in the setters, because regionLength() is
    //! dynamic: trimming a region shorter (or clearing its loop flag) left the
    //! stored fade lengths untouched, and a fadeOutFrames longer than the region
    //! made `ri >= regionLen - fadeOutFrames` true for EVERY frame -- the whole
    //! region came out attenuated, never reaching unity.  Overlapping fades also
    //! used to multiply, dipping the middle far below either curve.
    void effectiveFades(int64_t regionLen, int64_t& fin, int64_t& fout) const {
        fin  = fadeInFrames  > 0 ? fadeInFrames  : 0;
        fout = fadeOutFrames > 0 ? fadeOutFrames : 0;
        if (regionLen <= 0) { fin = fout = 0; return; }
        if (fin  > regionLen) fin  = regionLen;
        if (fout > regionLen) fout = regionLen;
        if (fin + fout > regionLen) {                 // meet, never overlap
            const double total = (double)fin + (double)fout;
            const double scale = (double)regionLen / total;
            fin  = (int64_t)((double)fin  * scale);
            fout = regionLen - fin;
        }
    }

    //! Fade gain multiplier at REGION-relative frame `ri` (0..regionLen-1), so
    //! fades track the region edges, not the source's.
    float fadeGain(int64_t ri, int64_t regionLen) const {
        int64_t fin = 0, fout = 0;
        effectiveFades(regionLen, fin, fout);
        float envIn = 1.0f, envOut = 1.0f;
        if (fin > 0 && ri < fin)
            envIn = fadeCurve((float)ri / (float)fin, fadeInShape,
                              fadeInTension, fadeInSlope);
        // Reach zero at regionLen (one past the last frame), so the fade STARTS
        // at exactly unity.  Using (regionLen-1-ri) started it one step below,
        // putting a 1/fadeOutFrames discontinuity at the top of every fade-out.
        if (fout > 0 && ri >= regionLen - fout) {
            const float u = (float)(regionLen - ri) / (float)fout;
            envOut = fadeCurve(u > 1.0f ? 1.0f : u, fadeOutShape,
                               fadeOutTension, fadeOutSlope);
        }
        return envIn * envOut;
    }
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_H
