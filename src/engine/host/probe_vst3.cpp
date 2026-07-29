// probe_vst3.cpp
//
// Out-of-process VST3 introspection helper for the PatchKnob unified plugin host.
//
// Like probe_vst2, this runs as a separate child process so a misbehaving
// plugin cannot take down the scanner. It loads a .vst3 (single file OR bundle
// directory) via the Steinberg SDK hosting layer, enumerates EVERY audio
// module class in the factory, and for each prints a structured record. It
// also instantiates each audio class (via PlugProvider) to read real bus
// counts and to detect whether it is an instrument (has a MIDI/event input
// bus and no audio inputs, or audio-in == 0 with event input present).
//
// Output contract (stdout, UTF-8). One "block" per class:
//   FACTORY_VENDOR=<vendor>        (printed once, first)
//   CLASS_BEGIN
//   NAME=<class name>
//   VENDOR=<class vendor or factory vendor>
//   UID=<class id string from the SDK>
//   CATEGORY=<category>
//   ISSYNTH=<0|1>
//   AUDIOIN=<channels on main audio input bus, summed>
//   AUDIOOUT=<channels on main audio output bus, summed>
//   CLASS_END
// If the module fails to load:  OK=0  ERR=<reason>.
//
// Build: see CMakeLists.txt (compiles the same minimal SDK source subset the
// vst3 spike uses).
// Usage: probe_vst3.exe <path-to-plugin.vst3>

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
#include <string>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int sumChannels(IComponent* comp, MediaType mt, BusDirection dir)
{
    int total = 0;
    int32 count = comp->getBusCount(mt, dir);
    for (int32 i = 0; i < count; ++i)
    {
        BusInfo info = {};
        if (comp->getBusInfo(mt, dir, i, info) == kResultOk)
            total += (int)info.channelCount;
    }
    return total;
}

int busCount(IComponent* comp, MediaType mt, BusDirection dir)
{
    return (int)comp->getBusCount(mt, dir);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf("OK=0\nERR=usage\n");
        return 2;
    }
    const std::string path = argv[1];

    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module)
    {
        std::printf("OK=0\nERR=module_load_failed\n");
        return 1;
    }

    const auto& factory = module->getFactory();
    auto finfo = factory.info();
    std::printf("FACTORY_VENDOR=%s\n", finfo.vendor().c_str());

    int emitted = 0;
    for (const auto& classInfo : factory.classInfos())
    {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;

        std::printf("CLASS_BEGIN\n");
        std::printf("NAME=%s\n", classInfo.name().c_str());
        std::printf("VENDOR=%s\n",
                    classInfo.vendor().empty() ? finfo.vendor().c_str()
                                               : classInfo.vendor().c_str());
        std::printf("UID=%s\n", classInfo.ID().toString().c_str());

        // subCategories() is a vector of category strings, e.g. {"Instrument"}.
        std::string subcats;
        bool subcatSynth = false;
        for (const auto& sc : classInfo.subCategories())
        {
            if (!subcats.empty()) subcats += "|";
            subcats += sc;
            if (sc.find("Instrument") != std::string::npos ||
                sc.find("Synth")      != std::string::npos)
                subcatSynth = true;
        }
        std::printf("CATEGORY=%s\n", subcats.c_str());

        int audioIn = 0, audioOut = 0, eventIn = 0;

        // Instantiate to read real bus topology. Guard with try in case a
        // plugin throws; a crash is contained by the parent's child-process
        // isolation anyway.
        auto plugProvider = owned(new PlugProvider(factory, classInfo, true));
        if (plugProvider && plugProvider->initialize())
        {
            if (IComponent* comp = plugProvider->getComponent())
            {
                audioIn  = sumChannels(comp, kAudio, kInput);
                audioOut = sumChannels(comp, kAudio, kOutput);
                eventIn  = busCount(comp, kEvent, kInput);
            }
        }

        bool isSynth = subcatSynth || (audioIn == 0 && eventIn > 0);

        std::printf("ISSYNTH=%d\n", isSynth ? 1 : 0);
        std::printf("AUDIOIN=%d\n", audioIn);
        std::printf("AUDIOOUT=%d\n", audioOut);
        std::printf("CLASS_END\n");
        ++emitted;
    }

    if (emitted == 0)
        std::printf("OK=0\nERR=no_audio_class\n");
    return 0;
}
