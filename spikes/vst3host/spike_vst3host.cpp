// spike_vst3host.cpp
//
// ZERO-JUCE VST3 host spike for the seq24 Windows port.
//
// Goal: prove we can load a .vst3 plugin with the mingw64 toolchain using
// Steinberg's VST3 SDK hosting layer (VST3::Hosting::Module), enumerate its
// classes, instantiate the first audio-processor component, query the core
// interfaces (IComponent / IAudioProcessor / IEditController) and print:
//   - plugin name
//   - audio input/output bus counts (and channel counts)
//   - the parameter list (id, title, default normalized value)
//
// No audio processing, no GUI. Just load + introspect.
//
// Usage: spike_vst3host.exe <path-to-plugin.vst3>
//   The path may be a single-file .vst3 or a .vst3 bundle directory
//   (Contents/x86_64-win/...). VST3::Hosting::Module handles both.

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <cstdio>
#include <iostream>
#include <string>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

std::string u16ToUtf8(const Steinberg::Vst::TChar* s)
{
    // SDK helper handles UTF-16 (TChar == char16_t) -> UTF-8.
    return Steinberg::Vst::StringConvert::convert(s);
}

const char* busDirStr(int32 dir)
{
    return dir == kInput ? "in" : "out";
}

const char* mediaTypeStr(int32 t)
{
    return t == kAudio ? "audio" : (t == kEvent ? "event" : "other");
}

void printBuses(IComponent* comp, MediaType mediaType, const char* label)
{
    int32 nIn  = comp->getBusCount(mediaType, kInput);
    int32 nOut = comp->getBusCount(mediaType, kOutput);
    std::printf("  %s buses: %d in, %d out\n", label, (int)nIn, (int)nOut);

    for (int32 dir = 0; dir < 2; ++dir)
    {
        BusDirection bdir = dir == 0 ? kInput : kOutput;
        int32 count = comp->getBusCount(mediaType, bdir);
        for (int32 i = 0; i < count; ++i)
        {
            BusInfo info = {};
            if (comp->getBusInfo(mediaType, bdir, i, info) == kResultOk)
            {
                std::printf("    [%s %s #%d] \"%s\" channels=%d type=%s active=%s\n",
                            mediaTypeStr(mediaType), busDirStr(bdir), (int)i,
                            u16ToUtf8(info.name).c_str(),
                            (int)info.channelCount,
                            info.busType == kMain ? "main" : "aux",
                            (info.flags & BusInfo::kDefaultActive) ? "yes" : "no");
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <path-to-plugin.vst3>\n", argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    std::printf("=== VST3 host spike ===\nLoading: %s\n", path.c_str());

    // ---- 1. Load the module ------------------------------------------------
    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module)
    {
        std::fprintf(stderr, "FAILED to load module: %s\n", error.c_str());
        return 1;
    }
    std::printf("Module loaded OK.\n");

    const auto& factory = module->getFactory();

    // ---- 2. Factory / class enumeration -----------------------------------
    auto finfo = factory.info();
    std::printf("Factory vendor: %s\n", finfo.vendor().c_str());
    std::printf("Factory url:    %s\n", finfo.url().c_str());

    std::printf("\nClasses in factory:\n");
    bool haveAudioClass = false;
    VST3::Hosting::ClassInfo wantedClass;
    for (const auto& classInfo : factory.classInfos())
    {
        std::printf("  - \"%s\"  category=%s  version=%s\n",
                    classInfo.name().c_str(),
                    classInfo.category().c_str(),
                    classInfo.version().c_str());
        if (!haveAudioClass && classInfo.category() == kVstAudioEffectClass)
        {
            wantedClass    = classInfo;
            haveAudioClass = true;
        }
    }

    if (!haveAudioClass)
    {
        std::fprintf(stderr, "\nNo Audio Module Class (%s) found. Nothing to instantiate.\n",
                     kVstAudioEffectClass);
        return 1;
    }

    std::printf("\nInstantiating first audio class: \"%s\"\n", wantedClass.name().c_str());

    // ---- 3. Use PlugProvider to wire IComponent + IEditController ----------
    // PlugProvider handles the single-component-implements-both case and the
    // separated component/controller case, including connecting them.
    auto plugProvider = owned(new PlugProvider(factory, wantedClass, true /*plugIsGlobal*/));
    if (!plugProvider->initialize())
    {
        std::fprintf(stderr, "PlugProvider::initialize() failed.\n");
        return 1;
    }

    IComponent*      component  = plugProvider->getComponent();
    IEditController* controller = plugProvider->getController();

    if (!component)
    {
        std::fprintf(stderr, "No IComponent from PlugProvider.\n");
        return 1;
    }

    // ---- 4. Query IAudioProcessor -----------------------------------------
    IAudioProcessor* processor = nullptr;
    if (component->queryInterface(IAudioProcessor::iid, (void**)&processor) != kResultOk)
        processor = nullptr;

    std::printf("\n--- Interfaces ---\n");
    std::printf("  IComponent:      %s\n", component  ? "yes" : "NO");
    std::printf("  IAudioProcessor: %s\n", processor  ? "yes" : "NO");
    std::printf("  IEditController: %s\n", controller ? "yes" : "NO");

    // ---- 5. Bus introspection ---------------------------------------------
    std::printf("\n--- Buses ---\n");
    printBuses(component, kAudio, "Audio");
    printBuses(component, kEvent, "Event");

    // ---- 6. Parameter list (via IEditController) --------------------------
    std::printf("\n--- Parameters ---\n");
    if (controller)
    {
        int32 nParams = controller->getParameterCount();
        std::printf("  parameter count: %d\n", (int)nParams);
        int32 limit = nParams < 200 ? nParams : 200;
        for (int32 i = 0; i < limit; ++i)
        {
            ParameterInfo pi = {};
            if (controller->getParameterInfo(i, pi) == kResultOk)
            {
                std::printf("    id=%u  \"%s\"  default=%.4f  steps=%d  flags=0x%x\n",
                            (unsigned)pi.id,
                            u16ToUtf8(pi.title).c_str(),
                            pi.defaultNormalizedValue,
                            (int)pi.stepCount,
                            (unsigned)pi.flags);
            }
        }
        if (nParams > limit)
            std::printf("    ... (%d more parameters truncated)\n", (int)(nParams - limit));
    }
    else
    {
        std::printf("  (no IEditController; cannot enumerate parameters)\n");
    }

    // ---- 7. Cleanup --------------------------------------------------------
    if (processor)
        processor->release();

    // PlugProvider owns/terminates component + controller on destruction.
    std::printf("\nDone. Spike succeeded.\n");
    return 0;
}
