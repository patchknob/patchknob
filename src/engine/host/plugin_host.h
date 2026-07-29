//----------------------------------------------------------------------------
//  PatchKnob — unified plugin host + scanner.
//
//  PluginHost implements PatchKnob::engine::IPluginHost. It is the single entry
//  point the rest of the engine uses to:
//    * scan the standard VST2 / VST3 directories (plus extra paths) and return
//      a flat list of PluginDescriptor, and
//    * instantiate a chosen descriptor into an IPluginInstance, dispatching to
//      the VST2 or VST3 concrete host by descriptor.format.
//
//  SCAN ROBUSTNESS — out-of-process probing:
//    Loading arbitrary third-party plugins in-process is dangerous (crashes,
//    hangs, modal dialogs, runaway threads on load). So the scanner does NOT
//    load plugins itself. For every candidate file it runs a tiny helper
//    executable (probe_vst2.exe / probe_vst3.exe) as a child process with a
//    timeout and parses its KEY=VALUE stdout. A plugin that crashes or hangs
//    only kills its own probe child; the scan continues. This is the most
//    robust approach on Windows and is what this module ships with.
//
//  The probe executables are built alongside this library and are located at
//  runtime next to the host's own module (see findProbeExe()).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_HOST_PLUGIN_HOST_H
#define PATCHKNOB_ENGINE_HOST_PLUGIN_HOST_H

#include "../plugin_api.h"

#include <string>
#include <vector>

namespace PatchKnob { namespace engine {

class PluginHost : public IPluginHost {
public:
    PluginHost();
    ~PluginHost() override;

    // Enumerate the standard VST2 dirs for 64-bit .dll and the VST3 dir for
    // .vst3, plus any extraPaths (each searched for BOTH .dll and .vst3).
    // Each discovered plugin is probed out-of-process to fill the descriptor.
    std::vector<PluginDescriptor> scan(const std::vector<std::string>& extraPaths) override;

    // Dispatch to createVst2Instance / createVst3Instance by desc.format.
    IPluginInstance* instantiate(const PluginDescriptor& desc) override;

    // --- configuration / nice-to-haves ---

    // Per-probe timeout in milliseconds (default 15000). A plugin whose probe
    // exceeds this is skipped.
    void setProbeTimeoutMs(unsigned ms) { probeTimeoutMs_ = ms; }

    // Optional cache file. If set before scan(), scan() writes a JSON-ish
    // inventory there; loadCache() can read it back without re-probing.
    void setCachePath(const std::string& path) { cachePath_ = path; }
    bool saveCache(const std::vector<PluginDescriptor>& plugins) const;
    bool loadCache(std::vector<PluginDescriptor>& out) const;

private:
    unsigned    probeTimeoutMs_ = 15000;
    std::string cachePath_;

    // Locate probe_vst2.exe / probe_vst3.exe (next to this module, then PATH).
    std::string findProbeExe(const char* exeName) const;

    // Probe one file; append 0..N descriptors. Returns false if the probe
    // could not run / timed out / reported failure (file then skipped).
    bool probeVst2(const std::string& dllPath, std::vector<PluginDescriptor>& out) const;
    bool probeVst3(const std::string& vst3Path, std::vector<PluginDescriptor>& out) const;
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_HOST_PLUGIN_HOST_H
