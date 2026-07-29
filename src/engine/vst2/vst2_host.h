//----------------------------------------------------------------------------
//  PatchKnob (ZERO-JUCE) — VST2 host module.
//
//  Vst2PluginInstance implements the shared PatchKnob::engine::IPluginInstance
//  contract (see ../plugin_api.h) on top of the clean-room "vestige"
//  aeffectx.h VST2 ABI header. It loads a 64-bit VST2 .dll, drives it through
//  the flat AEffect C ABI (dispatcher / processReplacing / get/setParameter),
//  delivers MIDI as VstEvents, fills VstTimeInfo for tempo sync, and exposes
//  parameters, editor, and chunk state.
//
//  Threading: every method runs on the message thread EXCEPT process(), which
//  is the realtime audio-thread entry point and is allocation/lock free.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_VST2_VST2_HOST_H
#define PATCHKNOB_ENGINE_VST2_VST2_HOST_H

#include "../plugin_api.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Forward declarations of the clean-room VST2 ABI types so consumers of this
// header do not have to pull in <windows.h> or aeffectx.h.
class AEffect;
class VstTimeInfo;
class VstEvents;

namespace PatchKnob { namespace engine {

class Vst2PluginInstance : public IPluginInstance {
public:
    Vst2PluginInstance();
    ~Vst2PluginInstance() override;

    // Load the DLL named by desc.path and instantiate the AEffect. Returns
    // false (and leaves the object inert) on any failure. Must be called once,
    // before prepare().
    bool load(const PluginDescriptor& desc);

    // TEST-ONLY: adopt an in-process fake AEffect (no DLL) so the hardening
    // paths (channel clamp, fault guard, teardown gate) can be exercised
    // headlessly by vst2_test. Applies the same load-time validation as
    // load(). Not for production use.
    bool adoptEffectForTest(AEffect* eff);

    // True once a fault has been caught inside this plugin (processReplacing
    // or any dispatcher/parameter call). A dead instance no-ops every further
    // plugin call and hands the caller silence.
    bool isDead() const { return dead_.load(std::memory_order_relaxed); }

    // --- IPluginInstance ---------------------------------------------------
    const PluginDescriptor& descriptor() const override { return desc_; }

    bool prepare(double sampleRate, int maxBlockSize) override;
    void setActive(bool active) override;
    void release() override;

    void process(const ProcessBlock& blk) override;

    int       paramCount() const override;
    ParamInfo paramInfo(int index) const override;
    float     getParamNormalized(uint32_t id) const override;
    void      setParamNormalized(uint32_t id, float v) override;

    bool hasEditor() const override;
    bool openEditor(NativeWindowHandle parent) override;
    void closeEditor() override;
    void getEditorSize(int& w, int& h) const override;
    void idleEditor() override;

    std::vector<uint8_t> saveState() const override;
    void                 loadState(const std::vector<uint8_t>& data) override;

private:
    // Non-copyable (owns an HMODULE + raw buffers).
    Vst2PluginInstance(const Vst2PluginInstance&) = delete;
    Vst2PluginInstance& operator=(const Vst2PluginInstance&) = delete;

    intptr_t dispatch(int32_t opcode, int32_t index, intptr_t value,
                      void* ptr, float opt) const;

    // The host callback the plugin invokes; trampolines into hostCallbackImpl.
    static intptr_t hostCallbackStatic(AEffect* effect, int32_t opcode,
                                       int32_t index, intptr_t value,
                                       void* ptr, float opt);
    intptr_t hostCallbackImpl(int32_t opcode, int32_t index, intptr_t value,
                              void* ptr, float opt);

    void allocChannelBuffers();
    void freeChannelBuffers();
    void freeEventBuffer();

    // Shared load-time validation for load()/adoptEffectForTest(): reject
    // insane self-reported AEffect counts before they size any buffer.
    bool validateEffectCounts() const;

    // Latch dead_ and log once; every later plugin call becomes a no-op.
    void markDead(const char* where, uint32_t code) const;

    // Fault-guarded set/getParameter (raw function-pointer calls into the
    // plugin, same fault class as the dispatcher).
    void  guardedSetParameter(int32_t index, float value) const;
    float guardedGetParameter(int32_t index) const;

    // Message-thread half of the teardown gate: stop new process() entries,
    // then wait out any in-flight audio block before touching plugin state.
    void quiesceProcessing();

    PluginDescriptor desc_;

    void*    module_   = nullptr;   // HMODULE (kept void* to avoid windows.h)
    AEffect* effect_   = nullptr;

    double   sampleRate_   = 44100.0;
    int      maxBlockSize_ = 512;
    int      numIn_        = 0;
    int      numOut_       = 0;

    bool     opened_  = false;      // effOpen called
    bool     active_  = false;      // effMainsChanged(1) in effect
    bool     prepared_ = false;

    // Fault / lifetime hardening. dead_ latches after any caught plugin fault
    // (mutable: dispatch() and the param getters are const). alive_ and
    // processing_ form the teardown gate: process() raises processing_ for
    // the duration of a block, release()/prepare() flip alive_ off and
    // spin-wait until processing_ clears before freeing/resizing anything the
    // audio thread touches.
    mutable std::atomic<bool> dead_{false};
    std::atomic<bool>         alive_{false};
    std::atomic<bool>         processing_{false};

    // Realtime scratch (allocated in prepare, used in process).
    std::vector<float*> inPtrs_;
    std::vector<float*> outPtrs_;
    std::vector<float>  inStorage_;    // numIn_ * maxBlockSize_, contiguous
    std::vector<float>  silence_;      // a zeroed input block for unused chans
    std::vector<float>  dump_;         // writable discard block for plugin
                                       // output channels the caller lacks

    // VstEvents scratch for MIDI delivery (over-allocated for maxEvents_).
    VstEvents* vstEvents_   = nullptr;
    void*      eventStorage_ = nullptr; // raw VstMidiEvent array
    int        maxEvents_    = 0;

    // Transport info handed back from audioMasterGetTime; refreshed each block.
    VstTimeInfo* timeInfo_ = nullptr;
    // Snapshot of the current block's transport, read by the host callback.
    double  curTempo_      = 120.0;
    int64_t curPlayPos_    = 0;
    bool    curPlaying_    = false;
};

// Factory: load + return a ready-to-prepare instance, or null on failure.
// Caller owns the returned pointer.
IPluginInstance* createVst2Instance(const PluginDescriptor& desc);

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_VST2_VST2_HOST_H
