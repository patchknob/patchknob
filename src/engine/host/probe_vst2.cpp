// probe_vst2.cpp
//
// Out-of-process VST2 introspection helper for the seq24 unified plugin host.
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
// On failure it prints  OK=0  and an  ERR=<reason>  line and returns non-zero.
//
// Uses the clean-room "vestige" aeffectx.h (NOT the Steinberg VST2 SDK).
//
// Build (mingw64):
//   g++ -std=c++17 -O2 -o probe_vst2.exe probe_vst2.cpp
// Usage: probe_vst2.exe <path-to-plugin.dll>

#include "../../../spikes/vst2host/vestige/aeffectx.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef AEffect* (VST_CALL_CONV* VstEntryProc)(audioMasterCallback);

static double g_sampleRate = 44100.0;
static int    g_blockSize  = 512;

static intptr_t VST_CALL_CONV hostCallback(AEffect* effect, int32_t opcode,
                                           int32_t index, intptr_t value,
                                           void* ptr, float opt)
{
    (void)effect; (void)index; (void)value; (void)opt;
    switch (opcode)
    {
    case audioMasterVersion:               return 2400;
    case audioMasterCurrentId:             return 0;
    case audioMasterGetSampleRate:         return (intptr_t)g_sampleRate;
    case audioMasterGetBlockSize:          return (intptr_t)g_blockSize;
    case audioMasterGetVendorString:       if (ptr) std::strcpy((char*)ptr, "seq24"); return 1;
    case audioMasterGetProductString:      if (ptr) std::strcpy((char*)ptr, "seq24 scanner"); return 1;
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

    // Some plugins look for sibling DLLs / resources next to themselves; let
    // LoadLibrary search the plugin's own directory.
    HMODULE mod = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod)
    {
        DWORD err = GetLastError();
        std::printf("OK=0\nERR=loadlibrary_%lu\n", (unsigned long)err);
        return 1;
    }

    VstEntryProc entry = (VstEntryProc)(void*)GetProcAddress(mod, "VSTPluginMain");
    if (!entry)
        entry = (VstEntryProc)(void*)GetProcAddress(mod, "main");
    if (!entry)
    {
        std::printf("OK=0\nERR=no_entry\n");
        FreeLibrary(mod);
        return 1;
    }

    AEffect* effect = entry(&hostCallback);
    if (!effect || effect->magic != kEffectMagic)
    {
        std::printf("OK=0\nERR=bad_effect\n");
        FreeLibrary(mod);
        return 1;
    }

    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, (float)g_sampleRate);
    effect->dispatcher(effect, effSetBlockSize, 0, (intptr_t)g_blockSize, nullptr, 0.0f);

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

    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    // Intentionally do NOT FreeLibrary here: some plugins crash on unload and
    // we already have what we need. The process exits immediately anyway.
    return 0;
}
