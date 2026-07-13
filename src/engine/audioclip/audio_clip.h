//----------------------------------------------------------------------------
//  seq24 Windows port — AudioClip: an in-memory stereo audio buffer.
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
#ifndef SEQ24_ENGINE_AUDIOCLIP_AUDIO_CLIP_H
#define SEQ24_ENGINE_AUDIOCLIP_AUDIO_CLIP_H

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>

namespace seq24 { namespace engine {

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

//! A clip placed on the timeline: play `clip` starting at absolute sample
//! `startSample`, scaled by `gain`. Non-owning: `clip` must outlive the
//! schedule that references it.
struct ScheduledClip {
    const AudioClip* clip        = nullptr;
    int64_t          startSample = 0;      //!< timeline position of clip frame 0
    float            gain        = 1.0f;

    //! One-past-the-end timeline sample of this placement.
    int64_t endSample() const {
        return startSample + (clip ? clip->numFrames() : 0);
    }
};

}} // namespace seq24::engine

#endif // SEQ24_ENGINE_AUDIOCLIP_AUDIO_CLIP_H
