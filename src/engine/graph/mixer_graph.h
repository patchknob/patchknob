//----------------------------------------------------------------------------
//  seq24 Windows port — master mixer graph.
//
//  MixerGraph owns N Tracks. Each block it:
//    1. determines the solo state (any track soloed => only soloed tracks sound),
//    2. renders every audible track into a per-track stereo scratch buffer,
//    3. sums them into the master stereo bus,
//    4. applies master gain,
//    5. updates the master stereo VU,
//    6. writes the result into the audio engine's output buffers.
//
//  render() matches AudioEngine::RenderCallback exactly:
//      void(float** out, int numChannels, int nframes, double sampleRate)
//  so it can be handed straight to AudioEngine::setRenderCallback() (wrapped in
//  a lambda/bind that also supplies MIDI + automation + transport for the block).
//
//  Realtime: render() and renderBlock() are audio-thread-only, lock-free,
//  allocation-free. All scratch is sized in prepare().
//----------------------------------------------------------------------------
#ifndef SEQ24_ENGINE_GRAPH_MIXER_GRAPH_H
#define SEQ24_ENGINE_GRAPH_MIXER_GRAPH_H

#include <atomic>
#include <memory>
#include <vector>

#include "track.h"
#include "vu_meter.h"

namespace seq24 { namespace engine {

//! Per-track event/automation slice for one render block. The sequencer fills
//! one of these per track before calling renderBlock(). Pointers must remain
//! valid for the duration of the call (they point into the engine's own buffers).
struct TrackBlockInput {
    const MidiEvent*   midi   = nullptr;
    int                nMidi  = 0;
    const ParamChange* autom  = nullptr;
    int                nAutom = 0;
};

class MixerGraph {
public:
    MixerGraph() = default;
    ~MixerGraph() = default;

    MixerGraph(const MixerGraph&)            = delete;
    MixerGraph& operator=(const MixerGraph&) = delete;

    // --- topology (message thread) -------------------------------------------

    //! Create `n` empty tracks (replaces any existing). Call before prepare().
    void setTrackCount(int n);

    int    trackCount() const { return (int)tracks_.size(); }
    Track* track(int i) {
        return (i >= 0 && i < (int)tracks_.size()) ? tracks_[i].get() : nullptr;
    }

    // --- master controls (any thread) ----------------------------------------

    void  setMasterGain(float g) { masterGain_.store(g, std::memory_order_relaxed); }
    float masterGain() const     { return masterGain_.load(std::memory_order_relaxed); }

    VuMeter& masterVuLeft()  { return masterVuL_; }
    VuMeter& masterVuRight() { return masterVuR_; }

    // --- transport (message thread sets; audio thread reads via render ctx) ---

    void setTransport(double tempoBpm, int64_t playPos, bool playing) {
        tempo_.store(tempoBpm, std::memory_order_relaxed);
        playPos_.store(playPos, std::memory_order_relaxed);
        playing_.store(playing, std::memory_order_relaxed);
    }

    // --- lifecycle (message thread) ------------------------------------------

    bool prepare(double sampleRate, int maxBlockSize);
    void release();

    // --- realtime render -----------------------------------------------------

    //! Full render with per-track MIDI/automation. The sequencer calls this
    //! from the audio thread. `inputs` is an array of `trackCount()` entries
    //! (or null for "no events for any track"). Sums into master `out` (stereo
    //! planar, `out[0]`/`out[1]`), applies master gain, updates master VU.
    void renderBlock(float** out, int numChannels, int nframes,
                     const TrackBlockInput* inputs, int numInputs);

    //! AudioEngine::RenderCallback-compatible entry point. Uses whatever
    //! per-block MIDI/automation was staged via stageInputs() (audio thread),
    //! or silence if none. This is what gets registered with the AudioEngine.
    void render(float** out, int numChannels, int nframes, double sampleRate);

    //! Stage the per-track inputs the next render() call will consume. Intended
    //! to be called on the audio thread right before render(), or for tests.
    //! `inputs` must remain valid until render() returns.
    void stageInputs(const TrackBlockInput* inputs, int numInputs) {
        stagedInputs_   = inputs;
        stagedNumInputs_ = numInputs;
    }

private:
    std::vector<std::unique_ptr<Track>> tracks_;

    std::atomic<float> masterGain_{1.0f};
    VuMeter            masterVuL_;
    VuMeter            masterVuR_;

    std::atomic<double>  tempo_{120.0};
    std::atomic<int64_t> playPos_{0};
    std::atomic<bool>    playing_{false};

    int    maxBlock_   = 0;
    double sampleRate_ = 48000.0;
    bool   prepared_   = false;

    // Per-track stereo scratch (one track rendered at a time, then summed).
    std::vector<float> trkL_;
    std::vector<float> trkR_;

    // Staged inputs for the callback-style render().
    const TrackBlockInput* stagedInputs_    = nullptr;
    int                    stagedNumInputs_ = 0;
};

}} // namespace seq24::engine

#endif // SEQ24_ENGINE_GRAPH_MIXER_GRAPH_H
