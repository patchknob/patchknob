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
        ch[0].assign((size_t)frames, 0.0f);
        ch[1].assign((size_t)frames, 0.0f);
    }

    //! Test/helper: synthesise a stereo sine tone clip so unit tests need no
    //! file on disk. Amplitude 0..1, both channels identical.
    static AudioClip synth_sine(double freqHz,
                                double durationSeconds,
                                double sampleRate,
                                float  amplitude = 0.8f,
                                const std::string& name = "sine") {
        AudioClip c;
        c.name             = name;
        c.sampleRate       = sampleRate;
        c.sourceSampleRate = sampleRate;
        const int64_t n = (int64_t)std::llround(durationSeconds * sampleRate);
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
    const AudioClip* clip         = nullptr;
    int64_t          startSample  = 0;     //!< timeline sample of the region START
    int64_t          sourceOffset = 0;     //!< first SOURCE frame the region plays
    int64_t          length       = 0;     //!< region length in frames (0 = to source end)
    float            gain         = 1.0f;  //!< region gain (Ardour _scale_amplitude)
    bool             muted        = false; //!< region mute (silent but kept)
    bool             loop         = false; //!< loop the source to fill `length`
    //! Fade envelope (applied on top of gain in AudioClipPlayer::process).
    //! Lengths in frames; tension in [-1,+1] (0 linear, >0 convex/slow-start,
    //! <0 concave/fast-start).  Overlapping fades on adjacent regions crossfade.
    int64_t          fadeInFrames  = 0;
    int64_t          fadeOutFrames = 0;
    float            fadeInTension  = 0.0f;
    float            fadeOutTension = 0.0f;

    //! Effective region length in frames, clamped to the source available from
    //! `sourceOffset`.  `length==0` means "to the end of the source".
    int64_t regionLength() const {
        if (!clip) return 0;
        const int64_t n = clip->numFrames();
        int64_t off = sourceOffset < 0 ? 0 : (sourceOffset > n ? n : sourceOffset);
        int64_t len = length > 0 ? length : (n - off);
        if (!loop && off + len > n) len = n - off;             // clamp to source (unless looping)
        return len < 0 ? 0 : len;
    }

    //! One-past-the-end timeline sample of this placement.
    int64_t endSample() const { return startSample + regionLength(); }

    //! Tension-shaped 0..1 ramp: u in [0,1] -> [0,1]; k bends the curve.
    static float fadeShape(float u, float k) {
        if (u <= 0.0f) return 0.0f;
        if (u >= 1.0f) return 1.0f;
        if (k > -1e-4f && k < 1e-4f) return u;                 // linear
        const float a = k * 3.0f;                              // steepness
        return (std::exp(a * u) - 1.0f) / (std::exp(a) - 1.0f);
    }

    //! Fade gain multiplier at REGION-relative frame `ri` (0..regionLen-1), so
    //! fades track the region edges, not the source's.
    float fadeGain(int64_t ri, int64_t regionLen) const {
        float envIn = 1.0f, envOut = 1.0f;
        if (fadeInFrames > 0 && ri < fadeInFrames)
            envIn = fadeShape((float)ri / (float)fadeInFrames, fadeInTension);
        if (fadeOutFrames > 0 && ri >= regionLen - fadeOutFrames) {
            const float u = (float)(regionLen - 1 - ri) / (float)fadeOutFrames;
            envOut = fadeShape(u, fadeOutTension);
        }
        return envIn * envOut;
    }
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_H
