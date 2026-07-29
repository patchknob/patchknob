//----------------------------------------------------------------------------
//  PatchKnob -- BuzzMachineHost: drives a compiled-in Buzz machine.
//
//  Instantiates a registered machine (e.g. the Unwieldy sampler), gives it the
//  master info + our CMICallbacks host (wavetable), and provides the generic
//  Buzz driving model: write per-track parameter values into the machine's
//  TrackVals byte buffer at computed offsets, Tick() to apply them, then
//  Work()/WorkMonoToStereo() to pull audio.  (Mirrors BuzzMachineLoader's
//  bm_new/bm_init/bm_set_track_parameter_value/bm_tick/bm_work.)
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_BUZZ_BUZZ_MACHINE_HOST_H
#define PATCHKNOB_ENGINE_BUZZ_BUZZ_MACHINE_HOST_H

#include "buzz_host.h"

#include <string>

namespace PatchKnob { namespace buzz {

class BuzzMachineHost {
public:
    BuzzMachineHost() = default;
    ~BuzzMachineHost();
    BuzzMachineHost(const BuzzMachineHost&) = delete;
    BuzzMachineHost& operator=(const BuzzMachineHost&) = delete;

    //! Instantiate the registered machine `name`, wire master info + callbacks,
    //! Init() it and set `tracks` voices.  Returns false if not registered.
    bool create(const std::string& name, double sampleRate, int tracks = 16);

    BuzzHost&           host()  { return host_; }        // the wavetable lives here
    CMachineInfo const* info() const { return info_; }
    bool  valid() const { return mac_ != nullptr; }

    void setMasterInfo(int bpm, int tpb, int sampleRate);
    void setNumTracks(int n);
    int  numTrackParams() const;
    int  numGlobalParams() const;
    //! Index of the first track param whose Name starts with `name` (-1 if none).
    int  findTrackParam(const char* name) const;

    void setTrackParam(int track, int index, int value);
    void setGlobalParam(int index, int value);
    void initDefaults();                                  // all params -> NoValue/DefValue

    void tick();
    //! Render `numFrames` of STEREO (interleaved L,R) -- chunks at MAX_BUFFER_LENGTH.
    //! Returns true if any non-silence was produced.
    bool work(float* stereo, int numFrames);

private:
    void* trackParamLoc(int track, int index) const;
    void* globalParamLoc(int index) const;
    int   paramSize(int absIndex) const;                  // 1 (Type<pt_word) or 2

    BuzzHost            host_;
    CMachineInterface*  mac_  = nullptr;
    CMachineInfo const* info_ = nullptr;
    int                 tracks_ = 1;
};

}} // namespace PatchKnob::buzz

#endif // PATCHKNOB_ENGINE_BUZZ_BUZZ_MACHINE_HOST_H
