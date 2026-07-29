//----------------------------------------------------------------------------
//  PatchKnob - VST3 host module.
//
//  Vst3PluginInstance implements the shared PatchKnob::engine::IPluginInstance
//  contract (see src/engine/plugin_api.h) on top of Steinberg's ZERO-JUCE
//  VST3 SDK hosting layer (VST3::Hosting::Module, IComponent, IAudioProcessor,
//  IEditController). NON-INTERLEAVED float buffers throughout.
//
//  Only this class (and the createVst3Instance factory) are exposed; nothing
//  in the rest of the engine ever sees a Steinberg type.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_VST3_VST3_HOST_H
#define PATCHKNOB_ENGINE_VST3_VST3_HOST_H

#include "../plugin_api.h"

#include <memory>
#include <vector>
#include <cstdint>

namespace PatchKnob { namespace engine {

// Concrete VST3 IPluginInstance. Construct via createVst3Instance(); after
// construction call prepare() before process().
class Vst3PluginInstance : public IPluginInstance {
public:
    Vst3PluginInstance();
    ~Vst3PluginInstance() override;

    // Load the module + class identified by `desc` and wire component +
    // controller. Returns false on any failure (object is then unusable).
    bool load(const PluginDescriptor& desc);

    // --- IPluginInstance -------------------------------------------------
    const PluginDescriptor& descriptor() const override;

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
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Factory: load and fully wire a VST3 plugin from a descriptor.
// Returns nullptr on failure. Caller owns the result.
IPluginInstance* createVst3Instance(const PluginDescriptor& desc);

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_VST3_VST3_HOST_H
