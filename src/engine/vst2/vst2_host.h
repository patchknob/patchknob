//----------------------------------------------------------------------------
//  seq24 Windows port (ZERO-JUCE) — VST2 host module.
//
//  Vst2PluginInstance implements the shared seq24::engine::IPluginInstance
//  contract (see ../plugin_api.h) on top of the clean-room "vestige"
//  aeffectx.h VST2 ABI header. It loads a 64-bit VST2 .dll, drives it through
//  the flat AEffect C ABI (dispatcher / processReplacing / get/setParameter),
//  delivers MIDI as VstEvents, fills VstTimeInfo for tempo sync, and exposes
//  parameters, editor, and chunk state.
//
//  Threading: every method runs on the message thread EXCEPT process(), which
//  is the realtime audio-thread entry point and is allocation/lock free.
//----------------------------------------------------------------------------
#ifndef SEQ24_ENGINE_VST2_VST2_HOST_H
#define SEQ24_ENGINE_VST2_VST2_HOST_H

#include "../plugin_api.h"

#include <cstdint>
#include <string>
#include <vector>

// Forward declarations of the clean-room VST2 ABI types so consumers of this
// header do not have to pull in <windows.h> or aeffectx.h.
class AEffect;
class VstTimeInfo;
class VstEvents;

namespace seq24 { namespace engine {

class Vst2PluginInstance : public IPluginInstance {
public:
    Vst2PluginInstance();
    ~Vst2PluginInstance() override;

    // Load the DLL named by desc.path and instantiate the AEffect. Returns
    // false (and leaves the object inert) on any failure. Must be called once,
    // before prepare().
    bool load(const PluginDescriptor& desc);

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

    // Realtime scratch (allocated in prepare, used in process).
    std::vector<float*> inPtrs_;
    std::vector<float*> outPtrs_;
    std::vector<float>  inStorage_;    // numIn_ * maxBlockSize_, contiguous
    std::vector<float>  silence_;      // a zeroed input block for unused chans

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

}} // namespace seq24::engine

#endif // SEQ24_ENGINE_VST2_VST2_HOST_H
