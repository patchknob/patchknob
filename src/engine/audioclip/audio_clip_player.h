//----------------------------------------------------------------------------
//  PatchKnob — AudioClipPlayer: a built-in "instrument" that plays
//  scheduled audio clips off a timeline.
//
//  It implements IPluginInstance (plugin_api.h) so it drops into a graph Track
//  exactly where a hosted VST instrument would. An "audio track" is therefore
//  just a Track whose instrument is an AudioClipPlayer: the Track feeds it the
//  transport (playPositionSamples / isPlaying / tempo) each block and the
//  player renders whatever clips overlap that block's window.
//
//  ------------------------------------------------------------------------
//  REALTIME-SAFE SCHEDULING (RCU / atomic snapshot — mirrors graph/track.h)
//  ------------------------------------------------------------------------
//  The message thread edits a plain std::vector<ScheduledClip> (`edit_`). On
//  every mutation it rebuilds an immutable, fixed-capacity ScheduleSnapshot in
//  the currently-unused slot of a pre-allocated double-buffer and publishes it
//  with a single release-store to `liveSchedule_`. process() does ONE
//  acquire-load of that pointer at the top of the block and uses that snapshot
//  for the whole block, so the audio thread never sees a half-edited schedule
//  and never allocates or locks. The clip objects themselves are owned by the
//  caller and must outlive any snapshot that references them (same host
//  lifetime contract as the FX-chain snapshot in the Track).
//
//  ------------------------------------------------------------------------
//  RECORD MODE
//  ------------------------------------------------------------------------
//  startRecord() arms an in-memory capture buffer (pre-reserved so appends do
//  not allocate on the audio thread). While armed, audio can be fed in two
//  equivalent ways, both realtime-safe:
//      * captureBlock(in, nframes) — the coordinator/graph pushes the audio it
//        wants recorded (e.g. an instrument track's output routed to this
//        audio track). This is the integration entry point.
//      * process() also appends its incoming blk.audioIn when armed, so a
//        monitored input is captured automatically.
//  stopRecord() finalises the capture into a new AudioClip (returned as a
//  shared_ptr the caller can immediately schedule).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_PLAYER_H
#define PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_PLAYER_H

#include <atomic>
#include <memory>
#include <vector>

#include "../plugin_api.h"
#include "audio_clip.h"
#include "warp_stretch.h"      // WarpMarker

namespace PatchKnob { namespace engine {

class AudioClipPlayer : public IPluginInstance {
public:
    //! Maximum number of clips that can be scheduled at once (snapshot cap).
    static constexpr int kMaxClips = 256;

    AudioClipPlayer();
    ~AudioClipPlayer() override;

    AudioClipPlayer(const AudioClipPlayer&)            = delete;
    AudioClipPlayer& operator=(const AudioClipPlayer&) = delete;

    // --- schedule editing (message thread) -----------------------------------

    //! Schedule `clip` to start at absolute timeline sample `startSample` with
    //! `gain`. Non-owning: `clip` must outlive the player (or until removed).
    //! Returns false if the schedule is full or `clip` is null/empty.
    bool addClip(const AudioClip* clip, int64_t startSample, float gain = 1.0f);

    //! Remove the scheduled clip at editable index. Returns false if OOB.
    bool removeClip(int index);

    //! Remove every scheduled clip.
    void clearClips();

    //! Set the fade-in/out lengths (frames) + tensions (-1..1) on the scheduled
    //! clip at editable index; republishes the snapshot. Message thread.
    bool setClipFades(int index, int64_t fadeInFrames, int64_t fadeOutFrames,
                      float fadeInTension, float fadeOutTension);

    //! Update the REGION placement of the clip at editable index: timeline start,
    //! start-offset into the source, and length (frames; length<=0 == to source
    //! end).  This is the non-destructive trim/slip/move primitive.  Republishes.
    bool setClipRegion(int index, int64_t startSample, int64_t sourceOffset,
                       int64_t length);

    //! Set the region gain (Ardour scale_amplitude; negative = phase invert) at
    //! editable index.  Republishes.
    bool setClipGain(int index, float gain);

    //! Set the region mute flag at editable index.  Republishes.
    bool setClipMuted(int index, bool muted);

    //! Set the region loop flag (wrap the source to fill `length`).  Republishes.
    bool setClipLoop(int index, bool loop);

    //! Clip at editable index (const access to its ScheduledClip fields).
    const ScheduledClip* scheduled(int index) const {
        return (index >= 0 && index < (int)edit_.size()) ? &edit_[index] : nullptr;
    }

    //! REALTIME WARP: associate a warp map with the scheduled `clip` so process()
    //! time-stretches it LIVE (Ableton-style, so you hear the warp while editing
    //! markers).  An empty/1-marker map clears warp (raw playback).  Message
    //! thread; the audio thread try_locks and falls back to raw on contention.
    void setWarp(const AudioClip* clip, const std::vector<WarpMarker>& markers);
    void clearWarp(const AudioClip* clip);

    //! Current number of scheduled clips (editable list).
    int clipCount() const { return (int)edit_.size(); }

    //! Scheduled clip at editable index (message thread), or a null placement.
    ScheduledClip clipAt(int index) const {
        return (index >= 0 && index < (int)edit_.size()) ? edit_[index]
                                                         : ScheduledClip{};
    }

    // --- record mode ---------------------------------------------------------

    //! Arm recording. Pre-reserves capacity for `maxSeconds` of stereo audio so
    //! captureBlock()/process() append without allocating on the audio thread.
    //! Message thread only.
    void startRecord(double maxSeconds = 600.0);

    //! True while a recording is armed (any thread; atomic).
    bool isRecording() const { return recording_.load(std::memory_order_acquire); }

    //! Frames captured so far (any thread; atomic).
    int64_t recordedFrames() const { return recFrames_.load(std::memory_order_acquire); }

    //! Disarm and finalise the capture into a new AudioClip at the engine rate.
    //! Returns the recorded clip (empty clip if nothing was captured). The
    //! internal buffer is reset. Message thread only.
    std::shared_ptr<AudioClip> stopRecord(const std::string& name = "recording");

    //! Append `nframes` of stereo audio to the capture buffer when armed.
    //! Realtime-safe: no allocation/locks (drops overflow past the reserve).
    //! `in` is [2][nframes] planar (in[1] may be null for a mono feed).
    void captureBlock(const float* const* in, int nframes);

    // --- IPluginInstance: lifecycle (message thread) -------------------------

    const PluginDescriptor& descriptor() const override { return desc_; }
    bool prepare(double sampleRate, int maxBlockSize) override;
    void setActive(bool active) override;
    void release() override;

    // --- IPluginInstance: realtime (audio thread) ----------------------------

    void process(const ProcessBlock& blk) override;

    // --- IPluginInstance: parameters (none) ----------------------------------

    int       paramCount() const override { return 0; }
    ParamInfo paramInfo(int) const override { return ParamInfo{}; }
    float     getParamNormalized(uint32_t) const override { return 0.0f; }
    void      setParamNormalized(uint32_t, float) override {}

    // --- IPluginInstance: editor (none) --------------------------------------

    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}

    // --- IPluginInstance: state (empty) --------------------------------------

    std::vector<uint8_t> saveState() const override { return {}; }
    void                 loadState(const std::vector<uint8_t>&) override {}

private:
    //! Immutable schedule snapshot read by the audio thread (RCU).
    static constexpr int kSnapshotSlots = 8;
    struct ScheduleSnapshot {
        ScheduledClip items[kMaxClips];
        int           count = 0;
    };

    //! Rebuild the snapshot from edit_ into the inactive double-buffer slot and
    //! publish it atomically. Message thread only.
    void publishSchedule();

    PluginDescriptor desc_;

    // Editable schedule (message-thread truth); audio thread never reads this.
    std::vector<ScheduledClip> edit_;

    // Ring-buffered snapshots + the live pointer the audio thread reads.
    ScheduleSnapshot               snapStore_[kSnapshotSlots];
    std::atomic<int>               activeSlot_{0};
    std::atomic<ScheduleSnapshot*> liveSchedule_{nullptr};

    // Prepared state.
    double sampleRate_ = 48000.0;
    int    maxBlock_   = 0;
    bool   active_     = false;

    // Realtime-warp state (pimpl: keeps signalsmith out of this header).  Holds
    // a per-clip {warp map + stretch instance + read cursor}, mutex-guarded.
    struct WarpState;
    std::unique_ptr<WarpState> warp_;
    // Render a warped region into out{L,R} for the block; false = no warp / skip.
    bool renderWarped(const ScheduledClip& sc, int64_t winStart, int n,
                      float* outL, float* outR);

    // Record buffer. cap_[] are pre-reserved on startRecord(); recFrames_ is the
    // published write cursor. recCapacity_ is the reserved frame count so the
    // audio thread can bound its appends without touching the vector's size.
    std::atomic<bool>    recording_{false};
    std::atomic<int64_t> recFrames_{0};
    int64_t              recCapacity_ = 0;
    std::vector<float>   recCh_[2];
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_PLAYER_H
