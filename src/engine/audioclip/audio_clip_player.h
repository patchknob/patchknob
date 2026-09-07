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
    uint64_t addRegion(const AudioClip* clip, int64_t startSample, float gain = 1.0f);

    //! Remove the scheduled clip at editable index. Returns false if OOB.
    bool removeClip(int index);

    //! Replace the WHOLE editable schedule in one publish (message thread).
    //! Entries carrying a nonzero regionId KEEP it -- this is what lets a
    //! track partition rebuild the schedule without breaking the stable ids
    //! callers hold on the surviving regions -- and entries with regionId 0
    //! are assigned fresh ids.  Entries with a null/ragged/empty clip are
    //! dropped.  Returns false (and schedules nothing new) if `regions` has
    //! more than kMaxClips usable entries.
    bool setSchedule(const std::vector<ScheduledClip>& regions);

    //! Remove every scheduled clip.
    void clearClips();

    //! Set the fade-in/out lengths (frames) + tensions (-1..1) on the scheduled
    //! clip at editable index; republishes the snapshot. Message thread.
    bool setClipFades(int index, int64_t fadeInFrames, int64_t fadeOutFrames,
                      float fadeInTension, float fadeOutTension);

    //! Set the fade SHAPES (ScheduledClip::kFadeShape*), SLOPES (kFadeSlope*)
    //! and -- when `link` >= 0 -- the edit-time crossfade link of the clip at
    //! editable index; republishes.  Message thread.  Pro Tools ch.32.
    bool setClipFadeShapes(int index, int inShape, int outShape,
                           int inSlope, int outSlope, int link = -1);

    //! AutoFades (PT p752): real-time fade-in/out applied at every FREE-STANDING
    //! region boundary during playback (a boundary carrying a real fade keeps
    //! that fade alone).  Implemented as a widened edge-declick window, so butt
    //! joints crossfade over it (the smoothstep halves sum to unity) and it is
    //! baked into any offline render that goes through process()/mixWindow.
    //! 0 (default) = only the fixed ~2 ms declick.  Message thread; atomic.
    void setAutoFadeFrames(int64_t frames);
    int64_t autoFadeFrames() const { return autoFadeFrames_.load(std::memory_order_acquire); }

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
    //! Switching looping ON captures the region's CURRENT trimmed source span as
    //! the loop period (see ScheduledClip::loopLength), so extending the region
    //! afterwards repeats that span instead of the whole rest of the source.
    bool setClipLoop(int index, bool loop);

    //! Set the loop period explicitly, in SOURCE frames (0 = to the source end).
    //! Republishes.  Message thread.
    bool setClipLoopLength(int index, int64_t frames);

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

    //! Declick ramp length in frames (~2 ms at the prepared rate).  Public so a
    //! test can reproduce the player's own envelope exactly.
    int64_t declickFrames() const { return declickFrames_; }

    //! Current number of scheduled clips (editable list).
    int clipCount() const { return (int)edit_.size(); }
    int regionIndex(uint64_t id) const;

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

    //! Append `nframes` of audio to the capture buffer when armed.
    //! Realtime-safe: no allocation/locks (drops overflow past the reserve).
    //! `in` is [numChannels][nframes] planar.  Pass the caller's REAL channel
    //! count: with numChannels < 2 the left feed is duplicated, and in[1] is
    //! never dereferenced (reading it on a mono bus walked off the host's array).
    void captureBlock(const float* const* in, int nframes, int numChannels = 2);

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

    //! Audio thread: acquire-load the live snapshot AND stamp its slot as in
    //! use, so publishSchedule() cannot recycle it mid-block.  The stamp goes
    //! down before the pointer is trusted and is then validated against the
    //! publish counter (see the implementation); wait-free, bounded retries.
    ScheduleSnapshot* acquireSnapshot();

    PluginDescriptor desc_;

    // Editable schedule (message-thread truth); audio thread never reads this.
    std::vector<ScheduledClip> edit_;
    uint64_t nextRegionId_ = 1;

    // Ring-buffered snapshots + the live pointer the audio thread reads.
    ScheduleSnapshot               snapStore_[kSnapshotSlots];
    std::atomic<int>               activeSlot_{0};
    std::atomic<ScheduleSnapshot*> liveSchedule_{nullptr};
    //! The slot process() is currently walking, so publishSchedule() can avoid
    //! recycling it out from under the audio thread.
    std::atomic<ScheduleSnapshot*> readingSnapshot_{nullptr};
    //! Counts publishes, bumped BEFORE publishSchedule() picks or writes a slot.
    //! acquireSnapshot() reads it either side of stamping readingSnapshot_ to
    //! prove no publish overlapped the stamp -- a pointer compare cannot do it,
    //! the ring is kSnapshotSlots deep so a lap brings the same pointer back.
    std::atomic<uint64_t>          publishSeq_{0};

    // Prepared state.
    double sampleRate_ = 48000.0;
    int    maxBlock_   = 0;
    bool   active_     = false;
    // DECLICK: every region edge, every loop wrap and the transport stop get a
    // short smoothstep ramp, exactly as the sampler declicks a voice start/steal.
    // Derived from the sample rate in prepare(), so it is the same number of
    // MILLISECONDS at 44.1k and at 192k.
    int64_t declickFrames_ = 0;
    //! AutoFade length in frames (0 = off); read by the audio thread each
    //! block, set from the message thread -- hence atomic.
    std::atomic<int64_t> autoFadeFrames_{ 0 };
    // Transport-stop tail: STOP used to clear the buffer to zero mid-waveform.
    // We keep rendering the timeline for one declick window, ramping out.
    bool    wasPlaying_ = false;
    bool    tailArmed_  = false;  // a stop happened; the ramp still owes frames
    int64_t tailPos_    = 0;   // next timeline sample the tail renders
    int64_t tailDone_   = 0;   // frames of the ramp already emitted

    // Realtime-warp state (pimpl: keeps signalsmith out of this header).  Holds
    // a per-clip {warp map + stretch instance + read cursor}, mutex-guarded.
    struct WarpState;
    std::unique_ptr<WarpState> warp_;
    // Render a warped region into out{L,R} for the block; false = no warp / skip.
    // `chGain` folds the mix for a mono output bus, matching the raw path.
    bool renderWarped(const ScheduledClip& sc, int64_t winStart, int n,
                      float* outL, float* outR, float chGain);

    // Mix every region of `snap` that overlaps [winStart, winStart+n) into
    // out{L,R} (which the caller has already cleared).  The whole realtime
    // render path, shared by normal playback and the transport-stop tail.
    void mixWindow(ScheduleSnapshot* snap, int64_t winStart, int n,
                   float* outL, float* outR, float chGain);

    // Lock-free "does this clip have a live warp map?".  The audio thread needs
    // the answer WITHOUT the warp mutex: on a lost try_lock it must not swap in
    // raw playback, which is a different pitch and a different timing.  It is
    // the last resort for a clip that has never rendered a warped block (no
    // anchor yet, see WarpAnchor); everything else continues from the anchor.
    bool clipIsWarped(const AudioClip* clip) const;

    // Message thread (under the warp mutex): keep the lock-free registry above
    // in step with warp_->entries.
    void publishWarpedClips();

    // Message thread: block until the audio thread has provably left any block
    // that could still be writing the record buffers, honouring the graph's
    // keep-alive convention (patch_nodes' two-block generation rule) before
    // storage the audio thread may hold a pointer into is freed or resized.
    void awaitRecordGrace();

    // Record buffer. cap_[] are pre-reserved on startRecord(); recFrames_ is the
    // published write cursor. recCapacity_ is the reserved frame count so the
    // audio thread can bound its appends without touching the vector's size.
    // recCh_ are FULLY SIZED by startRecord() and written by index, so the audio
    // thread never mutates or reads a vector size.  recCapacity_ is atomic
    // because the audio thread reads it while the message thread re-arms.
    // Lock-free registry of clips that currently have a warp map, so the audio
    // thread can tell a warped region from an unwarped one without the mutex.
    static constexpr int kMaxWarpedClips = kMaxClips;
    std::atomic<const AudioClip*> warpedClips_[kMaxWarpedClips];
    std::atomic<int>              warpedCount_{0};

    // LAST-KNOWN WARP MAPPING, one per warped clip.  Written and read ONLY by
    // the audio thread (renderWarped/renderWarpCached), so it needs no atomics
    // and no lock: it is the audio thread's own memory of where the warp map
    // had got to, kept so that a block which loses the warp try_lock can carry
    // the region on instead of emitting silence.  The anchor is in the map's
    // LOCAL dst coordinates and carries no region offset, so it stays valid for
    // every region that shares the clip.
    struct WarpAnchor {
        const AudioClip* clip      = nullptr;  // owner (null == unused slot)
        int64_t          warpedLen = 0;        // dst length of the warp map
        int64_t          dstAnchor = 0;        // local dst position of the anchor
        double           srcAnchor = 0.0;      // source frame it maps to
        double           ratio     = 1.0;      // source frames per dst frame there
    };
    WarpAnchor warpAnchors_[kMaxWarpedClips];
    int        warpAnchorCount_ = 0;
    // Audio thread: this clip's anchor slot, appended on first use.
    WarpAnchor* warpAnchor(const AudioClip* clip, bool create);
    // Audio thread WITHOUT the warp mutex: render one region-block from the
    // anchor above (a linear continuation of the map).  false = nothing known
    // about this clip yet, so there is nothing to continue.
    bool renderWarpCached(const ScheduledClip& sc, int64_t winStart, int n,
                          float* outL, float* outR, float chGain);

    // Audio-thread liveness, for the record-buffer keep-alive (see
    // awaitRecordGrace): rtGen_ counts finished blocks, capBusy_ is set for the
    // duration of a capture so the message thread can wait one out.
    std::atomic<uint64_t> rtGen_{0};
    std::atomic<bool>     capBusy_{false};

    std::atomic<bool>    recording_{false};
    std::atomic<int64_t> recFrames_{0};
    std::atomic<int64_t> recCapacity_{0};
    std::vector<float>   recCh_[2];
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIOCLIP_AUDIO_CLIP_PLAYER_H
