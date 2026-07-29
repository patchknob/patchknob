//----------------------------------------------------------------------------
//  PatchKnob -- Buzz machine HOST (backend).
//
//  This is the "background machinery" a Buzz machine (e.g. the Unwieldy tracker
//  sampler) is written against: an implementation of the SDK's CMICallbacks
//  (the host services the machine calls -- wavetable access, oscillator tables,
//  locking, ...) plus a STATIC_BUILD RegisterMachine registry so machine source
//  compiled into the app self-registers instead of being loaded from a DLL.
//
//  The whole thing is 64-bit and compiled with the SAME toolchain as the machine
//  (MinGW), so the CMachineInterface vtable ABI is internally consistent -- no
//  MSVC/GCC or 32/64 mismatch, no DLL loading, no IPC bridge.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_BUZZ_BUZZ_HOST_H
#define PATCHKNOB_ENGINE_BUZZ_BUZZ_HOST_H

#include "MachineInterface.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace PatchKnob { namespace buzz {

// ---------------------------------------------------------------------------
// Wavetable: the samples a machine plays.  A slot holds one or more LEVELS
// (multisample keyranges); the machine picks one per note via GetNearestWaveLevel.
// Buzz sample data is 16-bit signed; stereo is interleaved L,R,L,R.
// ---------------------------------------------------------------------------
struct WaveLevelData {
    std::vector<short> samples;   // owns the PCM the CWaveLevel points at
    CWaveLevel         level{};   // numSamples, pSamples, RootNote, SamplesPerSec, LoopStart/End
    int                loKey = 0; // explicit editor KEYRANGE (buzz notes, inclusive)
    int                hiKey = 127;
};
// An envelope point (Buzz format): x = time on the 0..65535 axis, y = level
// (0..65535, ~unity), flags = EIF_SUSTAIN / EIF_LOOP.  The machine interpolates
// LINEARLY between points, so a curved stage is fed as several dense points.
struct HostEnvPoint { unsigned short x = 0, y = 0; int flags = 0; };

struct WaveSlot {
    bool                      used = false;
    std::string               name;
    CWaveInfo                 info{};        // Flags (WF_LOOP/WF_STEREO/...), Volume
    std::vector<WaveLevelData> levels;       // one per keyrange; index 0 = whole range
    std::vector<std::vector<HostEnvPoint>> envelopes;  // [env 0..4] -> points (vel/pit/cut/res/pan)
};

// ---------------------------------------------------------------------------
// STATIC_BUILD registry -- machines call RegisterMachine() from their init fn.
// ---------------------------------------------------------------------------
typedef CMachineInfo const *(*BuzzGetInfoFn)();
typedef CMachineInterface   *(*BuzzCreateFn)();
struct MachineReg {
    CMachineInfo const* info   = nullptr;
    BuzzGetInfoFn       getInfo = nullptr;
    BuzzCreateFn        create  = nullptr;
    std::string         name;
};
const std::vector<MachineReg>& registered_machines();
CMachineInterface* create_machine(const std::string& name, CMachineInfo const** infoOut);

// Register every machine compiled into the app (Unwieldy, ...).  Call once at
// startup: it references each machine's init symbol so the static-lib object is
// pulled in and RegisterMachine runs.  Idempotent.
void register_builtin_machines();

// ---------------------------------------------------------------------------
// BuzzHost: one per hosted machine instance.  Owns the wavetable + master info
// and implements every CMICallbacks entry the machine can call.
// ---------------------------------------------------------------------------
class BuzzHost : public CMICallbacks {
public:
    BuzzHost();
    ~BuzzHost();   // CMICallbacks has no virtual dtor -> no 'override'

    CMasterInfo master{};                  // set BPM/TPB/rate before Work()
    std::vector<WaveSlot> waves;           // 1-based (index 0 kept as a dead slot)
    std::vector<std::vector<short>> retiredSamples; // old PCM kept alive for voices
    int  outChannels = 1;                  // set by the machine via SetnumOutputChannels
    int  stateFlags  = SF_PLAYING;

    // Load a mono/stereo 16-bit sample into wavetable slot `wave` (1-based),
    // level `level` (multisample keyrange).  Grows the table as needed.
    void setWaveLevel(int wave, int level, const short* interleaved, int numFrames,
                      bool stereo, int rootNote, int samplesPerSec,
                      int loopStart, int loopEnd, bool loop,
                      int loKey, int hiKey, const char* name);
    void clearWave(int wave);

    // Set envelope `env` (0=vel/amp,1=pitch,2=cutoff,3=resonance,4=pan) on wave
    // `wave`.  The machine reads these via GetEnvSize/GetEnvPoint on note-on.
    void setEnvelope(int wave, int env, const HostEnvPoint* pts, int count);

    // --- CMICallbacks (see MachineInterface.h) ------------------------------
    CWaveInfo const*  GetWave(int const i) override;
    CWaveLevel const* GetWaveLevel(int const i, int const level) override;
    void  MessageBox(char const* txt) override;
    void  Lock() override;
    void  Unlock() override;
    int   GetWritePos() override;
    int   GetPlayPos() override;
    float* GetAuxBuffer() override;
    void  ClearAuxBuffer() override;
    int   GetFreeWave() override;
    bool  AllocateWave(int const i, int const size, char const* name) override;
    void  ScheduleEvent(int const time, dword const data) override;
    void  MidiOut(int const dev, dword const data) override;
    short const* GetOscillatorTable(int const waveform) override;
    int   GetEnvSize(int const wave, int const env) override;
    bool  GetEnvPoint(int const wave, int const env, int const i, word& x, word& y, int& flags) override;
    CWaveLevel const* GetNearestWaveLevel(int const i, int const note) override;
    void  SetNumberOfTracks(int const n) override;
    CPattern* CreatePattern(char const* name, int const length) override;
    CPattern* GetPattern(int const index) override;
    char const* GetPatternName(CPattern* ppat) override;
    void  RenamePattern(char const* oldname, char const* newname) override;
    void  DeletePattern(CPattern* ppat) override;
    int   GetPatternData(CPattern* ppat, int const row, int const group, int const track, int const field) override;
    void  SetPatternData(CPattern* ppat, int const row, int const group, int const track, int const field, int const value) override;
    CSequence* CreateSequence() override;
    void  DeleteSequence(CSequence* pseq) override;
    CPattern* GetSequenceData(int const row) override;
    void  SetSequenceData(int const row, CPattern* ppat) override;
    void  SetMachineInterfaceEx(CMachineInterfaceEx* pex) override;
    void  ControlChange__obsolete__(int group, int track, int param, int value) override;
    int   ADGetnumChannels(bool input) override;
    void  ADWrite(int channel, float* psamples, int numsamples) override;
    void  ADRead(int channel, float* psamples, int numsamples) override;
    CMachine* GetThisMachine() override;
    void  ControlChange(CMachine* pmac, int group, int track, int param, int value) override;
    CSequence* GetPlayingSequence(CMachine* pmac) override;
    void* GetPlayingRow(CSequence* pseq, int group, int track) override;
    int   GetStateFlags() override;
    void  SetnumOutputChannels(CMachine* pmac, int n) override;
    void  SetEventHandler(CMachine* pmac, BEventType et, EVENT_HANDLER_PTR p, void* param) override;
    char const* GetWaveName(int const i) override;
    void  SetInternalWaveName(CMachine* pmac, int const i, char const* name) override;
    void  GetMachineNames(CMachineDataOutput* pout) override;
    CMachine* GetMachine(char const* name) override;
    CMachineInfo const* GetMachineInfo(CMachine* pmac) override;
    char const* GetMachineName(CMachine* pmac) override;
    bool  GetInput(int index, float* psamples, int numsamples, bool stereo, float* extrabuffer) override;

private:
    std::mutex           mtx_;
    std::vector<float>   aux_;                 // GetAuxBuffer scratch
    CMachineInterfaceEx* ex_ = nullptr;
    CWaveInfo            dummyInfo_{};
};

}} // namespace PatchKnob::buzz

#endif // PATCHKNOB_ENGINE_BUZZ_BUZZ_HOST_H
