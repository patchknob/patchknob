#include "../plugin_api.h"

namespace PatchKnob { namespace engine {

IPluginInstance* createVst2Instance(const PluginDescriptor&)
{
    return nullptr;
}

}} // namespace PatchKnob::engine
