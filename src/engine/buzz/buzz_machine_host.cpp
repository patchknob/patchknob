//----------------------------------------------------------------------------
//  src/engine/buzz/buzz_machine_host.cpp -- see buzz_machine_host.h.
//----------------------------------------------------------------------------
#include "buzz_machine_host.h"

#include <algorithm>
#include <cstring>

namespace PatchKnob { namespace buzz {

BuzzMachineHost::~BuzzMachineHost() {
    if (mac_) { mac_->Stop(); delete mac_; mac_ = nullptr; }
}

void BuzzMachineHost::setMasterInfo(int bpm, int tpb, int sr) {
    host_.master.BeatsPerMin   = bpm;
    host_.master.TicksPerBeat  = tpb;
    host_.master.SamplesPerSec = sr;
    host_.master.SamplesPerTick = (int)((60.0 * sr) / (double)(bpm * tpb));
    if (host_.master.SamplesPerTick < 1) host_.master.SamplesPerTick = 1;
    host_.master.PosInTick     = 0;
    host_.master.TicksPerSec   = (float)sr / (float)host_.master.SamplesPerTick;
}

bool BuzzMachineHost::create(const std::string& name, double sampleRate, int tracks) {
    register_builtin_machines();
    info_ = nullptr;
    mac_  = create_machine(name, &info_);
    if (!mac_ || !info_) { mac_ = nullptr; return false; }

    setMasterInfo(126, 4, (int)sampleRate);
    mac_->pMasterInfo = &host_.master;
    mac_->pCB         = &host_;

    // Seed ATTRIBUTE defaults BEFORE Init.  The machine's ctor points AttrVals at its
    // internal attribute struct but leaves it UNINITIALIZED; Init() then reads those
    // values (Unwieldy computes the amp-ramp declicker astep = smark/aval.ramp and
    // picks the sample interpolation mode from aval.inter=2/spline).  Without this,
    // AttrVals is garbage -> divide-by-garbage ramps, RANDOM interpolation
    // (zipper/aliasing), and a bogus pattern-transpose that detunes every note.
    if (mac_->AttrVals && info_->numAttributes > 0)
        for (int i = 0; i < info_->numAttributes; ++i)
            mac_->AttrVals[i] = info_->Attributes[i]->DefValue;

    // Init() (the machine may allocate/point GlobalVals/TrackVals + do the MDK
    // handshake and read the attributes above), THEN push default params, THEN voices.
    mac_->Init(nullptr);
    initDefaults();
    tracks_ = std::max(info_->minTracks, std::min(tracks, info_->maxTracks));
    mac_->SetNumTracks(tracks_);
    mac_->AttributesChanged();
    return true;
}

int BuzzMachineHost::numTrackParams()  const { return info_ ? info_->numTrackParameters  : 0; }
int BuzzMachineHost::numGlobalParams() const { return info_ ? info_->numGlobalParameters : 0; }

int BuzzMachineHost::paramSize(int abs) const {
    // CMPType: pt_note=0, pt_switch=1, pt_byte=2, pt_word=3.  <pt_word -> 1 byte.
    return (info_->Parameters[abs]->Type < pt_word) ? 1 : 2;
}

int BuzzMachineHost::findTrackParam(const char* name) const {
    if (!info_ || !name) return -1;
    const int ng = info_->numGlobalParameters;
    for (int i = 0; i < info_->numTrackParameters; ++i) {
        const char* pn = info_->Parameters[ng + i]->Name;
        if (pn && std::strncmp(pn, name, std::strlen(name)) == 0) return i;
    }
    return -1;
}

void* BuzzMachineHost::globalParamLoc(int index) const {
    if (!mac_ || !mac_->GlobalVals) return nullptr;
    unsigned char* p = (unsigned char*)mac_->GlobalVals;
    for (int i = 0; i < index; ++i) p += paramSize(i);
    return p;
}
void* BuzzMachineHost::trackParamLoc(int track, int index) const {
    if (!mac_ || !mac_->TrackVals) return nullptr;
    const int ng = info_->numGlobalParameters;
    unsigned char* p = (unsigned char*)mac_->TrackVals;
    for (int j = 0; j <= track; ++j)
        for (int i = 0; i < info_->numTrackParameters; ++i) {
            if (j == track && i == index) return p;
            p += paramSize(ng + i);
        }
    return nullptr;
}

void BuzzMachineHost::setGlobalParam(int index, int value) {
    if (!info_ || index < 0 || index >= info_->numGlobalParameters) return;
    void* p = globalParamLoc(index);
    if (!p) return;
    if (paramSize(index) == 1) *(unsigned char*)p = (unsigned char)value;
    else                       *(unsigned short*)p = (unsigned short)value;
}
void BuzzMachineHost::setTrackParam(int track, int index, int value) {
    if (!info_ || index < 0 || index >= info_->numTrackParameters) return;
    if (track < 0 || track >= info_->maxTracks) return;
    void* p = trackParamLoc(track, index);
    if (!p) return;
    if (paramSize(info_->numGlobalParameters + index) == 1) *(unsigned char*)p = (unsigned char)value;
    else                                                    *(unsigned short*)p = (unsigned short)value;
}

void BuzzMachineHost::initDefaults() {
    if (!info_) return;
    const int ng = info_->numGlobalParameters;
    for (int i = 0; i < ng; ++i)
        setGlobalParam(i, info_->Parameters[i]->NoValue);
    for (int t = 0; t < info_->maxTracks; ++t)
        for (int i = 0; i < info_->numTrackParameters; ++i)
            setTrackParam(t, i, info_->Parameters[ng + i]->NoValue);
}

void BuzzMachineHost::setNumTracks(int n) {
    if (!mac_ || !info_) return;
    tracks_ = std::max(info_->minTracks, std::min(n, info_->maxTracks));
    mac_->SetNumTracks(tracks_);
}

void BuzzMachineHost::tick() { if (mac_) mac_->Tick(); }

bool BuzzMachineHost::work(float* stereo, int numFrames) {
    if (!mac_ || !stereo || numFrames <= 0) return false;
    // CRITICAL: mi::WorkMonoToStereo unconditionally ZeroMemory()s a FULL
    // 2*MAX_BUFFER_LENGTH-float block into its output pointer, ignoring numsamples.
    // Passing a pointer into the middle of `stereo` (as we do for sub-block chunks
    // split at MIDI offsets) would overrun the caller's buffer whenever the chunk
    // has < MAX_BUFFER_LENGTH frames of tail left -> heap corruption / glitches.
    // So always render into a full-size staging buffer and copy out just n frames.
    float stage[2 * MAX_BUFFER_LENGTH];
    bool any = false;
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, MAX_BUFFER_LENGTH);
        if (mac_->WorkMonoToStereo(nullptr, stage, n, WM_WRITE)) any = true;
        std::memcpy(stereo + (size_t)done * 2, stage, (size_t)n * 2 * sizeof(float));
        done += n;
    }
    return any;
}

}} // namespace PatchKnob::buzz
