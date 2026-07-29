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
        resetEmitState();
    }

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
    void advance(int64_t fromTick, int64_t toTick,
                 const EmitParam& emitParam, const EmitCC& emitCC);

    //! Force every non-empty lane to emit its value at `tick` (ignoring the
    //! coalesce memory), then remember it. Use on locate / transport start.
    void emitAt(int64_t tick,
                const EmitParam& emitParam, const EmitCC& emitCC);

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
                 const EmitParam& emitParam, const EmitCC& emitCC) const;

    //! Emit `v` for one lane unconditionally (locate), updating its memo.
    void emitForce(int track, const AutomationLane& lane, float v, LaneMemo& memo,
                   const EmitParam& emitParam, const EmitCC& emitCC) const;

    std::vector<AutomationTrack>        tracks_;
    std::vector<std::vector<LaneMemo>>  memo_;
    bool                                coalesce_ = true;
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_PLAYER_H
