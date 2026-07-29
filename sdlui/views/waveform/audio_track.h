//----------------------------------------------------------------------------
//  sdlui/views/waveform/audio_track.h -- make a MixerGraph track play audio.
//
//  An "audio track" in this engine is just a MixerGraph Track whose instrument
//  is an AudioClipPlayer (a built-in IPluginInstance). AudioTrack owns that
//  player plus the AudioClip objects it schedules, mounts the player onto a
//  chosen track of a live MixerGraph, and lets clips be added/loaded so they
//  play through the SAME master graph as the VST tracks -- gain/pan/mute/solo,
//  master bus and metering all apply automatically.
//
//  Ownership: the clips live in this object (stable heap addresses), because the
//  AudioClipPlayer schedule references them non-owning. The returned
//  const AudioClip* is stable for the lifetime of this AudioTrack and is exactly
//  what you hand to WaveformView::set_clip() to draw it.
//
//  Threading: mount()/addClip()/loadWavClip() are message-thread only. The audio
//  thread renders the graph and sees a lock-free schedule snapshot.
//
//  Shell wiring:
//      using namespace waveform;
//      AudioTrack atk;
//      atk.mount(PatchKnob::app::audio_app_graph(), /*track*/ 8,
//                sr, block);                 // sr/block = engine device settings
//      const AudioClip* c = atk.loadWavClip("kick.wav", sr, /*start*/ 0);
//      waveformView.set_clip(c);
//      // transport (isPlaying / playPos) is driven by audio_app as usual; the
//      // clip sounds when the play position crosses its scheduled window.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_WAVEFORM_AUDIO_TRACK_H
#define PATCHKNOB_SDLUI_VIEWS_WAVEFORM_AUDIO_TRACK_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PatchKnob { namespace engine {
    struct AudioClip;
    class  AudioClipPlayer;
    class  MixerGraph;
}}

namespace waveform {

class AudioTrack {
public:
    AudioTrack();
    ~AudioTrack();

    AudioTrack(const AudioTrack&)            = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    //! Create an AudioClipPlayer, prepare it at (sampleRate, maxBlockSize) --
    //! which must match what the graph was prepared with -- and attach it as the
    //! instrument of graph track `trackIndex`. Returns false on bad args.
    //! Call AFTER the graph is prepared (audio_app_init() has run); the player is
    //! prepared here since setInstrument() after prepare() won't do it for us.
    bool mount(PatchKnob::engine::MixerGraph* graph, int trackIndex,
               double sampleRate, int maxBlockSize);

    //! True once mount() has attached the player to a track.
    bool mounted() const { return trackIndex_ >= 0; }
    int  track_index() const { return trackIndex_; }

    //! Move `clip` into this track's owned store and schedule it at absolute
    //! timeline sample `startSample` with `gain`. Returns the stable stored clip
    //! pointer (for WaveformView::set_clip), or nullptr on failure.
    const PatchKnob::engine::AudioClip* add_clip(PatchKnob::engine::AudioClip clip,
                                             int64_t startSample = 0,
                                             float   gain        = 1.0f);

    //! Load `path` (WAV) at `engineSampleRate`, store + schedule it. Returns the
    //! stored clip pointer, or nullptr on load failure (reason in *error).
    const PatchKnob::engine::AudioClip* load_wav_clip(const std::string& path,
                                                  double engineSampleRate,
                                                  int64_t startSample = 0,
                                                  float   gain        = 1.0f,
                                                  std::string* error  = nullptr);

    //! Synthesise a stereo sine test clip, store + schedule it. Handy when there
    //! is no file to load. Returns the stored clip pointer.
    const PatchKnob::engine::AudioClip* add_test_tone(double freqHz,
                                                  double durationSeconds,
                                                  double sampleRate,
                                                  int64_t startSample = 0,
                                                  float   amplitude   = 0.8f);

    //! Direct access if the caller needs the raw player (e.g. record mode).
    PatchKnob::engine::AudioClipPlayer* player() { return player_.get(); }
    int clip_count() const { return (int)clips_.size(); }

private:
    std::unique_ptr<PatchKnob::engine::AudioClipPlayer>          player_;
    std::vector<std::unique_ptr<PatchKnob::engine::AudioClip>>   clips_;   // stable addrs
    PatchKnob::engine::MixerGraph* graph_ = nullptr;
    int    trackIndex_ = -1;
    double sampleRate_ = 48000.0;
};

} // namespace waveform

#endif // PATCHKNOB_SDLUI_VIEWS_WAVEFORM_AUDIO_TRACK_H
