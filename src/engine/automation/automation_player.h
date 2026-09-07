//----------------------------------------------------------------------------
//  PatchKnob — AutomationPlayer: the automation playback runtime.
//
//  Owns one AutomationTrack per sequencer track and turns transport motion into
//  parameter / CC changes. It does NOT touch the audio engine itself: the
//  coordinator passes two emit callbacks and wires them to the lock-free bridge
//  (audio_app_route_param for VST params, a CC MIDI event for CCs). Keeping the
//  bridge out of this module lets it build and self-test standalone.
//
//  ------------------------------------------------------------------------
//  advance(fromTick, toTick, emitParam, emitCC)
//  ------------------------------------------------------------------------
//  Call once per processed transport window [fromTick, toTick). For every
//  non-empty lane it emits:
//      * the value at each breakpoint strictly crossed inside the window
//        (fromTick < bp.tick < toTick)  -- optional sub-sampling so fast moves
//        are not lost between block boundaries, and
//      * the value at toTick            -- the block's resting value, so a param
//        with no breakpoint in-window still tracks a linear ramp at block rate.
//  Breakpoints at exactly fromTick are skipped: the previous block already
//  emitted them as its own toTick, so contiguous windows neither gap nor
//  double-send. Emits are coalesced by default (see setCoalesce): a value equal
//  to the lane's last-sent value -- for CCs, the same 0..127 integer -- is
//  suppressed so a flat lane does not spam the bridge every block.
//
//  advance() is meant to run on the sequencer/UI thread, which then routes each
//  emit through the lock-free bridge; it is therefore NOT itself lock-free, but
//  it allocates nothing in steady state (memory is only touched when a track's
//  lane count changes).
//
//  ------------------------------------------------------------------------
//  emitAt(tick, ...) / resetEmitState()
//  ------------------------------------------------------------------------
//  On a locate / play-start, call emitAt(startTick) to force every lane to send
//  its current value (advance() alone would skip a breakpoint sitting exactly on
//  the start tick). resetEmitState() forgets the coalesce memory so the next
//  emit is unconditional (e.g. after re-assigning a track's instrument).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_PLAYER_H
#define PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_PLAYER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "automation_track.h"

namespace PatchKnob { namespace engine {

class AutomationPlayer {
public:
    //! Default track count (matches PatchKnob's c_maxBuses / AUDIO_APP_MAX_TRACKS).
    static constexpr int kDefaultTrackCount = 32;

    //! Emit a normalized (0..1) VST parameter change for a track's instrument.
    //! Wire to audio_app_route_param(track, id, value).
    using EmitParam = std::function<void(int track, unsigned id, float value)>;

    //! Emit a MIDI control change for a track (value already quantized 0..127).
    //! Wire to a CC MIDI event on the track's output.
    using EmitCC = std::function<void(int track, int cc, int value0_127)>;
    using EmitTarget = std::function<void(int destinationTrack,
                                          const LaneTarget&, float value)>;
    using EmitParamAt = std::function<void(int track, unsigned id, float value,
                                           int64_t projectTick)>;
    using EmitCCAt = std::function<void(int track, int cc, int value0_127,
                                        int64_t projectTick)>;
    using EmitTargetAt = std::function<void(int destinationTrack,
                                             const LaneTarget&, float value,
                                             int64_t projectTick)>;

    struct Region {
        int id = -1;                 //!< stable arrange region identity (sequence id)
        int destinationTrack = -1;   //!< mixer/MIDI track used by track-local targets
        int64_t position = 0;        //!< absolute project tick
        int64_t length = 1;          //!< half-open duration
        int64_t loopLength = 0;      //!< clip-local loop END; arranged overrun wraps here
        //! Clip-local loop START.  The window an automation region repeats is
        //! [loopStart, loopLength), exactly the window sequence::m_loop_start /
        //! m_loop_end describe for the notes in the same clip.  Only the END
        //! existed here, so a window dragged to start LATER than tick 0 wrapped
        //! at the right place but restarted the curve from tick 0 -- automation
        //! and notes drifted apart inside every repetition.  Defaults to 0,
        //! which is bit-identical to the old fold.
        int64_t loopStart = 0;
        int64_t source = 0;          //!< local curve offset (trim/slip)
        bool muted = false;
        AutomationTrack automation;
        std::string trackerFx;         //!< tracker-column automation owned by this region
    };

    explicit AutomationPlayer(int trackCount = kDefaultTrackCount)
        : tracks_((size_t)(trackCount > 0 ? trackCount : kDefaultTrackCount)),
          memo_((size_t)(trackCount > 0 ? trackCount : kDefaultTrackCount)) {}

    // --- track / lane model (message thread) ---------------------------------

    int trackCount() const { return (int)tracks_.size(); }

    AutomationTrack&       track(int i)       { return tracks_[(size_t)i]; }
    const AutomationTrack& track(int i) const { return tracks_[(size_t)i]; }

    //! Drop all lanes on all tracks and forget emit history.
    void clear() {
        for (auto& t : tracks_) t.clear();
        regions_.clear();
        resetEmitState();
    }

    Region& ensureRegion(int id) {
        if (Region* r = findRegion(id)) return *r;
        regions_.push_back(Region{}); regions_.back().id = id;
        return regions_.back();
    }
    Region* findRegion(int id) {
        for (auto& r : regions_) if (r.id == id) return &r; return nullptr;
    }
    const Region* findRegion(int id) const {
        for (const auto& r : regions_) if (r.id == id) return &r; return nullptr;
    }
    bool removeRegion(int id) {
        for (auto i=regions_.begin(); i!=regions_.end(); ++i) if(i->id==id){regions_.erase(i);return true;}
        return false;
    }
    std::vector<Region>& regions() { return regions_; }
    const std::vector<Region>& regions() const { return regions_; }

    //! Advance clip-owned automation regions in absolute project time.
    void advanceRegions(int64_t fromTick, int64_t toTick,
                        const EmitTarget& emitTarget, const EmitCC& emitCC) const;
    void advanceRegionsScheduled(int64_t fromTick, int64_t toTick,
                                 const EmitTargetAt& emitTarget,
                                 const EmitCCAt& emitCC) const;

    // --- coalescing ----------------------------------------------------------

    //! Suppress consecutive identical emits per lane (default true). Turn off to
    //! force one emit per due tick per block regardless of change.
    void setCoalesce(bool on) { coalesce_ = on; }
    bool coalesce() const { return coalesce_; }

    //! Forget every lane's last-sent value so the next emit is unconditional.
    void resetEmitState();

    // --- playback runtime ----------------------------------------------------

    //! Emit automation due in the half-open window [fromTick, toTick). No-op if
    //! toTick <= fromTick. See the class header for the exact contract.
    //!
    //! `emitTarget` is OPTIONAL but you almost certainly want it.  A track lane
    //! carries a full LaneTarget, exactly like a region lane does, and that
    //! target may name a PATCH-GRAPH node (PatchParam/RackParam) rather than
    //! the track's own instrument.  These used to emit through `emitParam`
    //! only, which passes (trackIndex, target.id) and DISCARDS target.node --
    //! so every patch-graph lane added on a track was routed to a mixer-track
    //! instrument that, in a patch-graph project, does not exist.  The lane
    //! drew and saved correctly and controlled nothing.  When `emitTarget` is
    //! supplied, non-VstParam lanes go through it with their target intact;
    //! VstParam lanes keep using `emitParam` as before.
    void advance(int64_t fromTick, int64_t toTick,
                 const EmitParam& emitParam, const EmitCC& emitCC,
                 const EmitTarget& emitTarget = EmitTarget());
    void advanceScheduled(int64_t fromTick, int64_t toTick,
                          const EmitParamAt& emitParam, const EmitCCAt& emitCC,
                          const EmitTargetAt& emitTarget = EmitTargetAt());

    //! Force every non-empty lane to emit its value at `tick` (ignoring the
    //! coalesce memory), then remember it. Use on locate / transport start.
    void emitAt(int64_t tick,
                const EmitParam& emitParam, const EmitCC& emitCC,
                const EmitTarget& emitTarget = EmitTarget());

    //! Quantize a normalized 0..1 value to a MIDI CC data byte (0..127).
    static int ccFromNorm(float v);

private:
    //! Per-lane coalesce memory (parallel to tracks_[t].lane(l)).
    struct LaneMemo {
        float lastNorm = 0.0f;
        int   lastCC   = -1;
        bool  has      = false;
    };

    //! Grow/shrink memo_ to match each track's current lane count. Allocates
    //! only when a track's lane count changed since the last call.
    void syncMemo();

    //! Emit `v` for one lane, honouring coalesce and updating its memo.
    void emitOne(int track, const AutomationLane& lane, float v, LaneMemo& memo,
                 const EmitTarget& emitTarget,
                 const EmitParam& emitParam, const EmitCC& emitCC) const;

    //! Emit `v` for one lane unconditionally (locate), updating its memo.
    void emitForce(int track, const AutomationLane& lane, float v, LaneMemo& memo,
                   const EmitTarget& emitTarget,
                   const EmitParam& emitParam, const EmitCC& emitCC) const;

    std::vector<AutomationTrack>        tracks_;
    std::vector<std::vector<LaneMemo>>  memo_;
    std::vector<Region>                 regions_;
    bool                                coalesce_ = true;
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_PLAYER_H
