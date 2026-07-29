// spike_vst2host.cpp
//
// ZERO-JUCE VST2 host spike for the PatchKnob Windows port.
//
// Goal: prove we can load and instantiate a VST2 plugin DLL with the mingw64
// toolchain, using the clean-room "vestige" aeffectx.h header (NOT the
// proprietary Steinberg VST2 SDK).
//
// What it does, given a path to a 64-bit VST2 .dll as argv[1]:
//   1. LoadLibrary the DLL.
//   2. GetProcAddress for "VSTPluginMain" (fallback legacy "main").
//   3. Call it with a minimal hostCallback -> get the AEffect*.
//   4. dispatcher(effOpen).
//   5. Introspect and print:
//        - effect/vendor/product strings, vendor version, VST version
//        - numInputs / numOutputs
//        - numParams / numPrograms
//        - effFlagsIsSynth, effFlagsHasEditor, effFlagsCanReplacing
//        - each parameter's name (effGetParamName), display + label, and
//          current normalized value (getParameter).
//   6. dispatcher(effClose).
//
// No audio processing, no GUI -- just load + introspect.
//
// Usage: spike_vst2host.exe <path-to-plugin.dll>
//
// Build (mingw64):
//   g++ -std=c++17 -O2 -municode -o spike_vst2host.exe spike_vst2host.cpp
// Links only against Win32 (LoadLibrary etc.) -- no extra libs needed.

#include "vestige/aeffectx.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Plugin entry point types.
// Modern VST2 plugins export "VSTPluginMain"; very old ones export "main".
// Signature: AEffect* entry(audioMasterCallback host);
// ---------------------------------------------------------------------------
typedef AEffect* (VST_CALL_CONV* VstEntryProc)(audioMasterCallback);

// Some opcodes we want are not present in the clean-room header (it only
// defines the ones LMMS' VeSTige needs). Define the few extras we use here.
// These integer values are part of the published VST2 opcode enumeration and
// are interface constants, not Steinberg source.
#ifndef effGetNumProgramCategories
constexpr int effGetNumProgramCategories = 7;
#endif
#ifndef effIdentify
constexpr int effIdentify = 22;
#endif

static double g_sampleRate = 44100.0;
static int    g_blockSize  = 512;

// ---------------------------------------------------------------------------
// Host callback. The plugin calls this to query host services. For an
// introspection-only spike we answer the bare minimum and politely decline
// everything else (return 0).
// ---------------------------------------------------------------------------
static intptr_t VST_CALL_CONV hostCallback(AEffect* effect, int32_t opcode,
                                           int32_t index, intptr_t value,
                                           void* ptr, float opt)
{
    (void)effect; (void)index; (void)value; (void)opt;

    switch (opcode)
    {
    case audioMasterVersion:
        // VST 2.4 == 2400. Plugins ask this very early; returning 0 makes
        // some plugins assume VST1 / bail out, so we must answer.
        return 2400;

    case audioMasterCurrentId:
        // Shell plugins ask which sub-plugin to instantiate. 0 = the default
        // / first. (Shell plugins -- e.g. Waves -- would need real handling.)
        return 0;

    case audioMasterGetSampleRate:
        return (intptr_t)g_sampleRate;

    case audioMasterGetBlockSize:
        return (intptr_t)g_blockSize;

    case audioMasterGetVendorString:
        if (ptr) std::strcpy((char*)ptr, "PatchKnob-spike");
        return 1;

    case audioMasterGetProductString:
        if (ptr) std::strcpy((char*)ptr, "PatchKnob VST2 host spike");
        return 1;

    case audioMasterGetVendorVersion:
        return 1;

    case audioMasterCanDo:
        // ptr is a C string naming a host capability. We claim none for the
        // spike. A real host would answer "sendVstMidiEvent",
        // "receiveVstMidiEvent", "sizeWindow", etc.
        return 0;

    case audioMasterGetCurrentProcessLevel:
        return 0; // 0 == unknown / not realtime

    case audioMasterGetLanguage:
        return kVstLangEnglish;

    case audioMasterGetTime:
        // A real host returns a VstTimeInfo* here. For introspection we have
        // none, so return 0; plugins must cope with a null time info.
        return 0;

    case audioMasterAutomate:
    case audioMasterIdle:
    case audioMasterWantMidi:
    case audioMasterUpdateDisplay:
    case audioMasterIOChanged:
    case audioMasterSizeWindow:
    case audioMasterBeginEdit:
    case audioMasterEndEdit:
    case audioMasterNeedIdle:
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Helper: call dispatcher with a string-out buffer (effGetEffectName etc.)
// ---------------------------------------------------------------------------
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
        std::fprintf(stderr, "usage: %s <path-to-vst2-plugin.dll>\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    std::printf("== PatchKnob VST2 host spike (clean-room vestige header) ==\n");
    std::printf("loading: %s\n", path);

    // LoadLibraryA so we can pass the argv string straight through.
    HMODULE mod = LoadLibraryA(path);
    if (!mod)
    {
        DWORD err = GetLastError();
        std::fprintf(stderr,
            "LoadLibrary FAILED, GetLastError=%lu\n", (unsigned long)err);
        if (err == 193)
            std::fprintf(stderr,
                "  (error 193 = ERROR_BAD_EXE_FORMAT: almost always a "
                "32-bit/64-bit mismatch -- this is a 64-bit host)\n");
        return 1;
    }

    // Resolve the entry point.
    VstEntryProc entry =
        (VstEntryProc)(void*)GetProcAddress(mod, "VSTPluginMain");
    const char* entryName = "VSTPluginMain";
    if (!entry)
    {
        entry = (VstEntryProc)(void*)GetProcAddress(mod, "main");
        entryName = "main";
    }
    if (!entry)
    {
        std::fprintf(stderr,
            "no VSTPluginMain / main export found -- not a VST2 DLL?\n");
        FreeLibrary(mod);
        return 1;
    }
    std::printf("entry point: %s\n", entryName);

    // Instantiate.
    AEffect* effect = entry(&hostCallback);
    if (!effect)
    {
        std::fprintf(stderr, "entry() returned null AEffect*\n");
        FreeLibrary(mod);
        return 1;
    }

    // Sanity-check the magic number.
    if (effect->magic != kEffectMagic)
    {
        std::fprintf(stderr,
            "WARNING: AEffect->magic = 0x%08x, expected kEffectMagic 0x%08x\n",
            (unsigned)effect->magic, (unsigned)kEffectMagic);
    }
    else
    {
        std::printf("AEffect magic OK (0x%08x 'VstP')\n",
                    (unsigned)effect->magic);
    }

    // Open the plugin.
    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);

    // Configure (harmless; lets plugins finish init even though we won't run).
    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr,
                       (float)g_sampleRate);
    effect->dispatcher(effect, effSetBlockSize, 0, (intptr_t)g_blockSize,
                       nullptr, 0.0f);

    // ---- Introspect ----
    std::printf("\n-- identity --\n");
    std::printf("  effect name   : %s\n", dispStr(effect, effGetEffectName).c_str());
    std::printf("  vendor        : %s\n", dispStr(effect, effGetVendorString).c_str());
    std::printf("  product       : %s\n", dispStr(effect, effGetProductString).c_str());
    std::printf("  vendor version: %ld\n",
                (long)effect->dispatcher(effect, effGetVendorVersion, 0, 0, nullptr, 0.0f));
    std::printf("  vst version   : %ld\n",
                (long)effect->dispatcher(effect, effGetVstVersion, 0, 0, nullptr, 0.0f));
    std::printf("  uniqueID      : 0x%08x\n", (unsigned)effect->uniqueID);

    std::printf("\n-- topology --\n");
    std::printf("  audio inputs  : %d\n", (int)effect->numInputs);
    std::printf("  audio outputs : %d\n", (int)effect->numOutputs);
    std::printf("  parameters    : %d\n", (int)effect->numParams);
    std::printf("  programs      : %d\n", (int)effect->numPrograms);

    std::printf("\n-- flags (0x%08x) --\n", (unsigned)effect->flags);
    std::printf("  isSynth       : %s\n",
                (effect->flags & effFlagsIsSynth) ? "yes" : "no");
    std::printf("  hasEditor     : %s\n",
                (effect->flags & effFlagsHasEditor) ? "yes" : "no");
    std::printf("  canReplacing  : %s\n",
                (effect->flags & effFlagsCanReplacing) ? "yes" : "no");

    // Parameters: name, current value, display + label.
    int nparams = (int)effect->numParams;
    if (nparams > 0)
    {
        std::printf("\n-- parameters (%d) --\n", nparams);
        int limit = nparams > 64 ? 64 : nparams; // cap noisy plugins
        for (int i = 0; i < limit; ++i)
        {
            std::string name  = dispStr(effect, effGetParamName, i);
            std::string disp  = dispStr(effect, effGetParamDisplay, i);
            std::string label = dispStr(effect, effGetParamLabel, i);
            float val = effect->getParameter(effect, i);
            std::printf("  [%3d] %-24s = %.4f  (%s %s)\n",
                        i, name.c_str(), val, disp.c_str(), label.c_str());
        }
        if (limit < nparams)
            std::printf("  ... (%d more parameters omitted)\n", nparams - limit);
    }
    else
    {
        std::printf("\n(plugin reports 0 parameters)\n");
    }

    // Clean up.
    std::printf("\nclosing plugin...\n");
    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    FreeLibrary(mod);

    std::printf("done.\n");
    return 0;
}
