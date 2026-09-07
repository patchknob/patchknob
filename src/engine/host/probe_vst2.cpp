// probe_vst2.cpp
//
// Out-of-process VST2 introspection helper for the PatchKnob unified plugin host.
//
// Loading an arbitrary third-party VST2 .dll in-process is dangerous: plugins
// can crash, hang, pop up dialogs, or spawn threads during load. To make the
// SCANNER robust, PluginHost runs THIS small helper as a separate child
// process (with a timeout) for each candidate .dll. If a plugin crashes or
// hangs, only the child dies; the scanner records the plugin as unprobed and
// moves on.
//
// Output contract (stdout, one KEY=VALUE per line, UTF-8 / ANSI):
//   OK=1
//   NAME=<effect name>
//   VENDOR=<vendor string>
//   UID=<uniqueID as 0x%08x>
//   ISSYNTH=<0|1>
//   AUDIOIN=<numInputs>
//   AUDIOOUT=<numOutputs>
//   BUSIN=<bus layout, plugin_api.h wire format>
//   BUSOUT=<bus layout, plugin_api.h wire format>
// On failure it prints  OK=0  and an  ERR=<reason>  line and returns non-zero.
//
// AUDIOIN/AUDIOOUT are the flat channel totals they have always been. VST2 has
// no bus objects, so the GROUPING of those channels has to be asked for pin by
// pin with effGetInputProperties/effGetOutputProperties (VstPinProperties, VST
// 2.4): kVstPinIsStereo on pin i means i and i+1 are one stereo bus. A plugin
// that does not answer, or answers without ever declaring a grouping, emits an
// empty BUSIN/BUSOUT -- which the scanner reads as one main bus of all N
// channels, i.e. the flat list, which is the correct fallback.
//
// SHELL PLUGINS: one .dll/.so can be a "shell" holding many effects (Waves,
// Voxengo and friends ship this way). For those, the flat form above describes
// the shell wrapper, not any usable effect -- which is why a shell used to load
// whatever sub-plugin it defaults to, never the one the user picked. So when
// effGetPlugCategory reports kPlugCategShell we enumerate the contents with
// effShellGetNextPlugin, instantiate each sub-plugin (cheap: the module is
// already loaded) and emit ONE block per sub-plugin instead:
//   SHELL_BEGIN
//   NAME=... / VENDOR=... / UID=shell:0x%08x / ISSYNTH=... / AUDIOIN / AUDIOOUT
//   SHELL_END
// The "shell:" prefix on UID is what tells the VST2 host to answer
// audioMasterCurrentId with that id during construction -- the only way to ask
// a shell for a specific sub-plugin. A plain 0x... UID keeps the old meaning
// (the file's own uniqueID, nothing to select) and is answered with 0.
//
// Uses the clean-room "vestige" aeffectx.h (NOT the Steinberg VST2 SDK).
//
// Build (mingw64):
//   g++ -std=c++17 -O2 -o probe_vst2.exe probe_vst2.cpp
// Usage: probe_vst2.exe <path-to-plugin.dll>

#include "../../../spikes/vst2host/vestige/aeffectx.h"
#include "../plugin_api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
static void* pk_dlopen(const char* path)
{
    HMODULE mod = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) mod = LoadLibraryA(path);
    return mod;
}
static void* pk_dlsym(void* mod, const char* name) { return (void*)GetProcAddress((HMODULE)mod, name); }
#else
static void* pk_dlopen(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void* pk_dlsym(void* mod, const char* name) { return dlsym(mod, name); }
#endif

typedef AEffect* (VST_CALL_CONV* VstEntryProc)(audioMasterCallback);

static double g_sampleRate = 44100.0;
static int    g_blockSize  = 512;

// Published VST2 constants the clean-room header does not declare.
static constexpr int32_t kEffGetPlugCategory    = 35;
static constexpr int32_t kEffShellGetNextPlugin = 70;
static constexpr intptr_t kPlugCategShell       = 10;

// VST 2.4 pin introspection (see the header comment). VstPinProperties is
// exactly 128 bytes on the wire; the static_assert keeps that honest.
static constexpr int32_t kEffGetInputProperties  = 33;
static constexpr int32_t kEffGetOutputProperties = 34;
static constexpr int32_t kPinIsStereo            = 1 << 1;
static constexpr int32_t kPinUseSpeaker          = 1 << 2;
static constexpr int32_t kSpeakerArrMono         = 0;
static constexpr int32_t kSpeakerArrStereo       = 1;

struct ProbePinProperties
{
    char    label[64];
    int32_t flags;
    int32_t arrangementType;
    char    shortLabel[8];
    char    future[48];
};
static_assert(sizeof(ProbePinProperties) == 128, "VstPinProperties ABI");

// Group one direction's pins into buses. Returns "" when the plugin declined to
// describe the grouping, which the scanner turns into a single main bus.
static std::string pinBusLayout(AEffect* e, int32_t opcode, int numPins)
{
    if (!e || numPins <= 0) return "";
    std::vector<PatchKnob::engine::PluginBusInfo> buses;
    bool sawGrouping = false;
    for (int i = 0; i < numPins; )
    {
        ProbePinProperties pp;
        std::memset(&pp, 0, sizeof(pp));
        if (!e->dispatcher(e, opcode, i, 0, &pp, 0.0f))
            return "";
        int width = 1;
        if (pp.flags & kPinIsStereo)  { width = 2; sawGrouping = true; }
        if (pp.flags & kPinUseSpeaker)
        {
            sawGrouping = true;
            if      (pp.arrangementType == kSpeakerArrStereo) width = 2;
            else if (pp.arrangementType == kSpeakerArrMono)   width = 1;
        }
        if (i + width > numPins) width = numPins - i;
        if (width <= 0) return "";
        pp.label[sizeof(pp.label) - 1] = '\0';
        std::string name = pp.label;
        if (name.empty())
        {
            pp.shortLabel[sizeof(pp.shortLabel) - 1] = '\0';
            name = pp.shortLabel;
        }
        if (name.empty())
            name = buses.empty() ? "Main"
                                 : ("Bus " + std::to_string((int)buses.size() + 1));
        PatchKnob::engine::PluginBusInfo b;
        b.name = std::move(name); b.channelCount = width;
        b.isMain = buses.empty(); b.isAux = false;
        buses.push_back(std::move(b));
        i += width;
    }
    if (!sawGrouping) return "";
    return PatchKnob::engine::pluginEncodeBusLayout(buses);
}

// Answered to audioMasterCurrentId. A shell reads it during entry() to decide
// WHICH of its sub-plugins to construct; 0 means "your default".
static int32_t g_currentId = 0;

// Bound on how many sub-plugins we will enumerate/instantiate, so a malformed
// (or merely enormous) shell cannot make the probe run past its timeout.
static constexpr size_t kMaxShellSubPlugins = 512;

static intptr_t VST_CALL_CONV hostCallback(AEffect* effect, int32_t opcode,
                                           int32_t index, intptr_t value,
                                           void* ptr, float opt)
{
    (void)effect; (void)index; (void)value; (void)opt;
    switch (opcode)
    {
    case audioMasterVersion:               return 2400;
    case audioMasterCurrentId:             return g_currentId;
    case audioMasterGetSampleRate:         return (intptr_t)g_sampleRate;
    case audioMasterGetBlockSize:          return (intptr_t)g_blockSize;
    case audioMasterGetVendorString:       if (ptr) std::strcpy((char*)ptr, "PatchKnob"); return 1;
    case audioMasterGetProductString:      if (ptr) std::strcpy((char*)ptr, "PatchKnob scanner"); return 1;
    case audioMasterGetVendorVersion:      return 1;
    case audioMasterCanDo:                 return 0;
    case audioMasterGetCurrentProcessLevel:return 0;
    case audioMasterGetLanguage:           return kVstLangEnglish;
    case audioMasterGetTime:               return 0;
    default:                               return 0;
    }
}

static std::string dispStr(AEffect* e, int32_t opcode, int32_t index = 0)
{
    char buf[256] = {0};
    e->dispatcher(e, opcode, index, 0, buf, 0.0f);
    return std::string(buf);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("OK=0\nERR=usage\n");
        return 2;
    }
    const char* path = argv[1];

    // Some plugins look for sibling libraries/resources next to themselves;
    // pk_dlopen searches the plugin's own directory on Windows the same way.
    void* mod = pk_dlopen(path);
    if (!mod)
    {
#ifdef _WIN32
        std::printf("OK=0\nERR=loadlibrary_%lu\n", (unsigned long)GetLastError());
#else
        std::printf("OK=0\nERR=dlopen_%s\n", dlerror());
#endif
        return 1;
    }

    VstEntryProc entry = (VstEntryProc)pk_dlsym(mod, "VSTPluginMain");
    if (!entry)
        entry = (VstEntryProc)pk_dlsym(mod, "main");
    if (!entry)
    {
        std::printf("OK=0\nERR=no_entry\n");
        return 1;
    }

    AEffect* effect = entry(&hostCallback);
    if (!effect || effect->magic != kEffectMagic)
    {
        // Not unloaded, same reasoning as the successful-probe path below:
        // entry() already ran the plugin's own code, so unloading now carries
        // the same crash-on-unload risk. The process exits right after anyway.
        std::printf("OK=0\nERR=bad_effect\n");
        return 1;
    }

    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, (float)g_sampleRate);
    effect->dispatcher(effect, effSetBlockSize, 0, (intptr_t)g_blockSize, nullptr, 0.0f);

    // --- shell plugins: enumerate the contents and report each sub-plugin ---
    if (effect->dispatcher(effect, kEffGetPlugCategory, 0, 0, nullptr, 0.0f)
            == kPlugCategShell)
    {
        struct Sub { int32_t id; std::string name; };
        std::vector<Sub> subs;
        for (;;)
        {
            char nameBuf[128] = {0};
            intptr_t id = effect->dispatcher(effect, kEffShellGetNextPlugin,
                                             0, 0, nameBuf, 0.0f);
            if (id == 0) break;                       // enumeration finished
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            subs.push_back(Sub{ (int32_t)id, std::string(nameBuf) });
            if (subs.size() >= kMaxShellSubPlugins) break;
        }

        if (!subs.empty())
        {
            // The enumerator instance has done its job; each sub-plugin needs a
            // fresh AEffect built with g_currentId pointing at it.
            effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);

            std::printf("OK=1\n");
            size_t emitted = 0;
            for (const Sub& sub : subs)
            {
                g_currentId = sub.id;
                AEffect* se = entry(&hostCallback);
                if (!se || se->magic != kEffectMagic) continue;
                se->dispatcher(se, effOpen, 0, 0, nullptr, 0.0f);
                se->dispatcher(se, effSetSampleRate, 0, 0, nullptr, (float)g_sampleRate);
                se->dispatcher(se, effSetBlockSize, 0, (intptr_t)g_blockSize, nullptr, 0.0f);

                std::string sname = dispStr(se, effGetEffectName);
                if (sname.empty()) sname = dispStr(se, effGetProductString);
                if (sname.empty()) sname = sub.name;
                std::string svendor = dispStr(se, effGetVendorString);

                std::printf("SHELL_BEGIN\n");
                std::printf("NAME=%s\n", sname.c_str());
                std::printf("VENDOR=%s\n", svendor.c_str());
                std::printf("UID=shell:0x%08x\n", (unsigned)sub.id);
                std::printf("ISSYNTH=%d\n", (se->flags & effFlagsIsSynth) ? 1 : 0);
                std::printf("AUDIOIN=%d\n", (int)se->numInputs);
                std::printf("AUDIOOUT=%d\n", (int)se->numOutputs);
                std::printf("BUSIN=%s\n",
                            pinBusLayout(se, kEffGetInputProperties,
                                         (int)se->numInputs).c_str());
                std::printf("BUSOUT=%s\n",
                            pinBusLayout(se, kEffGetOutputProperties,
                                         (int)se->numOutputs).c_str());
                std::printf("SHELL_END\n");
                ++emitted;

                se->dispatcher(se, effClose, 0, 0, nullptr, 0.0f);
            }
            if (emitted == 0)
            {
                // Enumerated but nothing would instantiate: say so rather than
                // report an inventory of zero effects as a success.
                std::printf("ERR=shell_no_subplugins\n");
                return 1;
            }
            return 0;
        }
        // A shell that enumerates nothing falls through to the flat form below,
        // which is exactly the pre-existing behaviour.
    }

    std::string name   = dispStr(effect, effGetEffectName);
    std::string vendor = dispStr(effect, effGetVendorString);
    if (name.empty())
        name = dispStr(effect, effGetProductString);

    bool isSynth = (effect->flags & effFlagsIsSynth) != 0;

    std::printf("OK=1\n");
    std::printf("NAME=%s\n", name.c_str());
    std::printf("VENDOR=%s\n", vendor.c_str());
    std::printf("UID=0x%08x\n", (unsigned)effect->uniqueID);
    std::printf("ISSYNTH=%d\n", isSynth ? 1 : 0);
    std::printf("AUDIOIN=%d\n", (int)effect->numInputs);
    std::printf("AUDIOOUT=%d\n", (int)effect->numOutputs);
    std::printf("BUSIN=%s\n",
                pinBusLayout(effect, kEffGetInputProperties,
                             (int)effect->numInputs).c_str());
    std::printf("BUSOUT=%s\n",
                pinBusLayout(effect, kEffGetOutputProperties,
                             (int)effect->numOutputs).c_str());

    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    // Intentionally do NOT FreeLibrary here: some plugins crash on unload and
    // we already have what we need. The process exits immediately anyway.
    return 0;
}
