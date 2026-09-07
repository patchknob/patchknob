//----------------------------------------------------------------------------
//  src/engine/rack/rack_engine.h
//
//  RackEngine -- a tiny VCV-Rack-style patch runtime: a set of Modules wired by
//  Cables, stepped one SAMPLE at a time.  Cables carry a 1-sample delay (so
//  feedback patches are stable, exactly like VCV).  Three module ROLES bridge
//  to the outside world (the hosting RackNode's stereo audio + MIDI):
//      AudioOut : node's stereo OUT   = sum of these modules' L/R inputs  (/5)
//      AudioIn  : node's stereo IN   -> these modules' L/R outputs        (*5)
//      MidiCV   : node's MIDI in     -> pitch(1V/oct)/gate(10V)/vel outs
//
//  Threading: the SDL editor (GUI thread) does structural edits + param writes;
//  the audio thread calls process().  Structural edits briefly hold a mutex;
//  process() try_locks it and emits silence for the (sub-millisecond) block in
//  which an edit lands -- never blocking the audio thread, never a data race on
//  the module/cable vectors.  Param writes are plain float stores (a one-sample
//  torn knob value is inaudible), so turning knobs never glitches audio.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_ENGINE_H
#define PATCHKNOB_ENGINE_RACK_ENGINE_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rack.hpp"
#include "rack_factory.h"
#include "rack_script_module.h"
#include "../plugin_api.h"       // PatchKnob::engine::MidiEvent

namespace rackx {

// A placed module instance (owns its DSP object + GUI position).
struct RackModule {
    int          id   = 0;
    std::string  slug;
    std::string  name;
    std::string  category;
    Role         role = Role::Normal;
    float        x = 0.f, y = 0.f;              // editor canvas position
    ModuleHandle mod;                           // the DSP
    PanelSpec    panel;                         // per-instance layout (scripting modules)
};

// A patch cable: source module's output port -> dest module's input port.
struct RackCable {
    int id      = 0;
    int fromMod = 0, outPort = 0;
    int toMod   = 0, inPort  = 0;
};

// A cable with its endpoints RESOLVED to port pointers at edit time, so the
// per-sample hot loop is O(cables) pointer copies with no by-id module scan.
// Module DSP objects are heap-owned (unique_ptr) and their port vectors are
// sized once by config() in the ctor, so these pointers survive mods_
// reallocation; the list is rebuilt under the edit mutex on every edit.
struct CompiledCable {
    rack::engine::Output* out = nullptr;
    rack::engine::Input*  in  = nullptr;
};

class RackEngine {
public:
    RackEngine();
    ~RackEngine();

    RackEngine(const RackEngine&)            = delete;
    RackEngine& operator=(const RackEngine&) = delete;

    // ---- lifecycle (message/GUI thread) -----------------------------------
    void setSampleRate(double sr);
    double sampleRate() const { return sr_; }
    //! Guarantee at least one AudioOut module (so a fresh rack makes sound once
    //! something is patched to it).  Returns its module id.
    int  ensureDefaultIO();

    // ---- structural edits (GUI thread; briefly locks the audio thread out) -
    int  addModule(const std::string& slug, float x, float y);   // -> id, or -1
    void removeModule(int moduleId);
    void moveModule(int moduleId, float x, float y);
    //! Connect out->in.  An input takes only ONE cable (an existing one to the
    //! same input is replaced).  Rejects self/dupe/out-of-range.  -> cable id/-1.
    int  addCable(int fromMod, int outPort, int toMod, int inPort);
    void removeCable(int cableId);
    void clear();

    // ---- param write (GUI thread; lock-free) ------------------------------
    void setParam(int moduleId, int paramId, float value);
    float getParam(int moduleId, int paramId) const;
    //! Number of params on a module (for cloning its state).  0 if unknown.
    int  moduleParamCount(int moduleId) const;

    // ---- scripting modules (Pd / Csound): per-instance panel + patch text -----
    //! Mutable per-instance panel layout (for the panel editor); null if none.
    PanelSpec*  modulePanel(int moduleId);
    //! True if the module hosts a Pd/Csound patch (implements IScriptModule).
    bool        isScriptModule(int moduleId) const;
    std::string moduleScript(int moduleId) const;         // patch text ("" if none)
    const char* moduleScriptKind(int moduleId) const;     // "pd" / "csound" / ""
    std::string moduleScriptError(int moduleId) const;    // last compile messages ("" if none)
    //! Replace the patch text: recompiles the engine, reconfigures jacks/knobs,
    //! prunes now-invalid cables, and rebuilds the instance panel.  GUI thread.
    bool        setModuleScript(int moduleId, const std::string& text);
    //! Reset one module's runtime state synchronously before its next sample.
    bool resetModule(int moduleId);

    // ---- polyphony (GUI thread) -------------------------------------------
    //! Voice count 1..16 for the MIDI-CV module(s): notes are allocated across
    //! this many voices and the pitch/gate/vel outputs become that-wide poly
    //! signals (VCV-style), so a polyphonic module chain plays chords.
    void setPolyphony(int n);
    int  polyphony() const { return polyphony_; }

    // ---- realtime (audio thread) ------------------------------------------
    // in/out are planar node buffers in +/-1.0 audio units (inL/inR may be null
    // when the node has no audio input).  midi is this block's events.
    void process(int nframes,
                 const float* inL, const float* inR,
                 float* outL, float* outR,
                 const PatchKnob::engine::MidiEvent* midi, int numMidi,
                 float hostTempoBpm = 120.f, bool hostPlaying = true,
                 int64_t hostPlayPositionSamples = -1);

    // Per-module I/O variant: each AudioIn module gets its OWN stereo input
    // (insL[k]/insR[k]) and each AudioOut module writes its OWN stereo output
    // (outsL[k]/outsR[k]) -- so the host node can expose one patch port per audio
    // module.  Any pointer may be null; extra modules beyond the array sizes get
    // silence.  Same units/gain as process().
    void processMulti(int nframes,
                      const float* const* insL, const float* const* insR, int numIns,
                      float* const* outsL, float* const* outsR, int numOuts,
                      const PatchKnob::engine::MidiEvent* midi, int numMidi,
                      float hostTempoBpm = 120.f, bool hostPlaying = true,
                      int64_t hostPlayPositionSamples = -1);

    // ---- audio-I/O module counts (GUI thread; drive the host node's port count) -
    int  audioOutCount() const { return (int)audioOutMods_.size(); }
    int  audioInCount()  const { return (int)audioInMods_.size(); }
    //! 1-based channel number of an Audio-In/Out module among its role peers (in
    //! host-port order), or 0 if `moduleId` isn't an audio-I/O module.  The rack
    //! editor labels each module with this and it matches the host node's port.
    int  audioChannelIndex(int moduleId) const;

    // ---- query (GUI thread; renderer reads these) -------------------------
    int          moduleCount() const { return (int)mods_.size(); }
    RackModule*  moduleAt(int index) { return (index >= 0 && index < (int)mods_.size()) ? &mods_[index] : nullptr; }
    RackModule*  moduleById(int id);
    int          cableCount() const { return (int)cables_.size(); }
    RackCable*   cableAt(int index) { return (index >= 0 && index < (int)cables_.size()) ? &cables_[index] : nullptr; }

private:
    void  stepSample(const rack::engine::Module::ProcessArgs& args);
    void  refreshRoles();                       // rebuild role/module pointer caches
    void  rebuildCables();                      // re-resolve cable endpoints (under mtx_)
    int   inputCableCount(int moduleId, int inPort) const;

    std::vector<RackModule> mods_;
    std::vector<RackCable>  cables_;

    // Edit-time-compiled hot-path state (all rebuilt under mtx_; the audio
    // thread only reads them while holding the same mutex via try_lock, so it
    // never observes a half-built list and never allocates or scans by id).
    std::vector<CompiledCable>          compiledCables_;             // resolved cable endpoints
    std::vector<rack::engine::Module*>  procMods_;                   // dense module process list
    std::vector<rack::engine::Module*>  audioOutMods_, audioInMods_, midiCvMods_;   // resolved roles
    double                  sr_     = 48000.0;
    int                     nextId_ = 1;
    int64_t                 frame_  = 0;

    // Polyphonic MIDI voices fed to MidiCV modules (updated from events).  With
    // polyphony_==1 this is the classic mono voice; higher values allocate notes
    // across voices[0..polyphony_-1] and emit them as poly channels.
    struct Voice { int note = -1; float gateV = 0.f; float velV = 0.f; float pitchV = 0.f; };
    Voice                   voices_[16];
    int                     polyphony_ = 1;      // 1..16
    int                     rrNext_ = 0;         // round-robin voice allocator
    int  allocVoice();                           // pick a free/steal voice index
    void noteOn(int note, int vel);
    void noteOff(int note);

    // Notes currently physically held (key down), oldest first.  allocVoice()
    // steals a still-gated voice once polyphony is exceeded, which overwrites
    // that voice's `note` field -- without tracking held notes separately, the
    // stolen note's eventual noteOff() has no voice left whose `note` still
    // matches it, so it is silently a no-op forever.  If the stolen note was
    // the one actually sounding, nothing ever silences it (a stuck gate); if
    // it wasn't, releasing its key does nothing (dropped note-off) and it
    // never gets a chance to sound again even after a voice frees up.  This
    // stack lets noteOff() correctly (a) no-op releasing a voiceless note and
    // (b) hand a just-freed voice to the newest still-held note that lost its
    // voice to stealing, giving standard last-note-priority behaviour instead
    // of losing notes.
    static constexpr int kMaxHeld = 128;
    int  heldNote_[kMaxHeld];
    int  heldVel_[kMaxHeld];
    int  heldCount_ = 0;
    void pushHeld(int note, int vel);
    void popHeld(int note);
    bool noteHasVoice(int note) const;

    mutable std::mutex      mtx_;                // structural edits vs. process()

    // The self-test injects deliberately-corrupt values (polyphony 99, port
    // channels 200) to prove the audio path clamps at every read site.
    friend struct RackEngineTestPeer;
};

} // namespace rackx

#endif
