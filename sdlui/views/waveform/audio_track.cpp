//----------------------------------------------------------------------------
//  sdlui/views/waveform/audio_track.cpp -- see audio_track.h.
//----------------------------------------------------------------------------
#include "audio_track.h"

#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/audio_clip_player.h"
#include "engine/audioclip/wav_loader.h"
#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"

using seq24::engine::AudioClip;
using seq24::engine::AudioClipPlayer;
using seq24::engine::MixerGraph;
using seq24::engine::Track;

namespace waveform {

AudioTrack::AudioTrack() = default;
AudioTrack::~AudioTrack() {
    // Detach from the track before our player is destroyed so the graph never
    // holds a dangling instrument pointer (message-thread teardown).
    if (graph_ && trackIndex_ >= 0) {
        if (Track* t = graph_->track(trackIndex_))
            if (t->instrument() == player_.get()) t->setInstrument(nullptr);
    }
}

bool AudioTrack::mount(MixerGraph* graph, int trackIndex,
                       double sampleRate, int maxBlockSize) {
    if (!graph) return false;
    if (trackIndex < 0 || trackIndex >= graph->trackCount()) return false;
    Track* t = graph->track(trackIndex);
    if (!t) return false;

    player_.reset(new AudioClipPlayer());
    if (!player_->prepare(sampleRate, maxBlockSize)) return false;
    player_->setActive(true);

    t->setInstrument(player_.get());

    graph_      = graph;
    trackIndex_ = trackIndex;
    sampleRate_ = sampleRate;
    return true;
}

const AudioClip* AudioTrack::add_clip(AudioClip clip, int64_t startSample, float gain) {
    if (!player_) return nullptr;
    clips_.push_back(std::unique_ptr<AudioClip>(new AudioClip(std::move(clip))));
    const AudioClip* stored = clips_.back().get();
    if (stored->empty() || !player_->addClip(stored, startSample, gain)) {
        clips_.pop_back();
        return nullptr;
    }
    return stored;
}

const AudioClip* AudioTrack::load_wav_clip(const std::string& path,
                                           double engineSampleRate,
                                           int64_t startSample, float gain,
                                           std::string* error) {
    AudioClip c;
    if (!seq24::engine::loadWav(path, engineSampleRate, c, error)) return nullptr;
    return add_clip(std::move(c), startSample, gain);
}

const AudioClip* AudioTrack::add_test_tone(double freqHz, double durationSeconds,
                                           double sampleRate, int64_t startSample,
                                           float amplitude) {
    AudioClip c = AudioClip::synth_sine(freqHz, durationSeconds, sampleRate,
                                        amplitude, "tone");
    return add_clip(std::move(c), startSample, 1.0f);
}

} // namespace waveform
