//----------------------------------------------------------------------------
//  PatchKnob — AutomationTrack: all lanes automated on one track.
//
//  A track can automate several destinations at once (e.g. filter cutoff via a
//  VST parameter AND mod-wheel via MIDI CC1). AutomationTrack is just the owned,
//  ordered collection of AutomationLane for a single sequencer track. It adds no
//  playback logic of its own — that lives in AutomationPlayer, which owns one
//  AutomationTrack per track index and walks their lanes each block.
//
//  Pure message-thread data model: build/edit lanes here, then the player reads
//  them. Lanes are stored by value; references returned by lane()/findLane() are
//  invalidated by addLane()/removeLane() (standard std::vector semantics).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_TRACK_H
#define PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_TRACK_H

#include <vector>

#include "automation_lane.h"

namespace PatchKnob { namespace engine {

//! The set of automation lanes belonging to one sequencer track.
class AutomationTrack {
public:
    // --- lane access ---------------------------------------------------------

    int  laneCount() const { return (int)lanes_.size(); }
    bool empty()     const { return lanes_.empty(); }

    AutomationLane&       lane(int i)       { return lanes_[(size_t)i]; }
    const AutomationLane& lane(int i) const { return lanes_[(size_t)i]; }

    // --- lane editing (message thread) ---------------------------------------

    //! Append an empty lane for `target` and return a reference to it.
    AutomationLane& addLane(const LaneTarget& target,
                            Interpolation interp = Interpolation::Linear) {
        lanes_.emplace_back(target, interp);
        return lanes_.back();
    }

    //! Append (or return the existing) lane for `target`. If a lane already
    //! drives that exact target it is returned unchanged; otherwise a new one is
    //! created. Lets callers upsert without scanning first.
    AutomationLane& laneForTarget(const LaneTarget& target,
                                  Interpolation interp = Interpolation::Linear) {
        if (AutomationLane* existing = findLane(target)) return *existing;
        return addLane(target, interp);
    }

    //! First lane driving `target`, or nullptr if none.
    AutomationLane* findLane(const LaneTarget& target) {
        for (auto& l : lanes_)
            if (l.target() == target) return &l;
        return nullptr;
    }
    const AutomationLane* findLane(const LaneTarget& target) const {
        for (auto& l : lanes_)
            if (l.target() == target) return &l;
        return nullptr;
    }

    //! Remove the lane at `index`. Returns false if out of range.
    bool removeLane(int index) {
        if (index < 0 || index >= (int)lanes_.size()) return false;
        lanes_.erase(lanes_.begin() + index);
        return true;
    }

    //! Remove all lanes.
    void clear() { lanes_.clear(); }

private:
    std::vector<AutomationLane> lanes_;
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUTOMATION_AUTOMATION_TRACK_H
