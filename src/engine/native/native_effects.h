#pragma once

#include "../plugin_api.h"
#include <vector>

namespace PatchKnob { namespace engine {

// Built-in effects vendored with PatchKnob.  The stable builtin:// paths are
// serialized in projects just like an external plug-in path.
std::vector<PluginDescriptor> nativeEffectDescriptors();
IPluginInstance* createNativeEffect(const PluginDescriptor& descriptor);

}} // namespace PatchKnob::engine
