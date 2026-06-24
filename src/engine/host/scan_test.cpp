// scan_test.cpp
//
// Standalone test/inventory for the seq24 unified plugin host + scanner.
// Runs PluginHost::scan({}) and prints EVERY plugin discovered on this
// machine, grouped by format, with name / vendor / instrument flag / I/O.
//
// Build & run: see CMakeLists.txt (target scan_test). The probe helper exes
// (probe_vst2.exe / probe_vst3.exe) are built into the same directory so the
// scanner can find them at runtime.

#include "plugin_host.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace seq24::engine;

static void printGroup(const std::vector<PluginDescriptor>& all, PluginFormat fmt,
                       const char* label)
{
    int count = 0;
    for (const auto& p : all) if (p.format == fmt) ++count;

    std::printf("\n================ %s (%d) ================\n", label, count);
    int idx = 0;
    for (const auto& p : all)
    {
        if (p.format != fmt) continue;
        std::printf("[%3d] %-40s  %-9s  in=%-2d out=%-2d  vendor=%s\n",
                    ++idx,
                    p.name.c_str(),
                    p.isInstrument ? "INSTR" : "effect",
                    p.numAudioIn, p.numAudioOut,
                    p.vendor.c_str());
        std::printf("      path: %s\n", p.path.c_str());
        if (!p.uid.empty())
            std::printf("      uid : %s\n", p.uid.c_str());
    }
}

int main(int argc, char** argv)
{
    PluginHost host;

    // Optional cache path as argv[1].
    if (argc > 1)
        host.setCachePath(argv[1]);

    std::printf("Scanning standard VST2 + VST3 directories (out-of-process probing)...\n");
    std::printf("This loads each plugin in a child process; please wait.\n");

    std::vector<std::string> extra; // none for the inventory run
    std::vector<PluginDescriptor> all = host.scan(extra);

    printGroup(all, PluginFormat::VST2, "VST2 plugins");
    printGroup(all, PluginFormat::VST3, "VST3 plugins");

    int vst2 = 0, vst3 = 0, instr = 0;
    for (const auto& p : all)
    {
        if (p.format == PluginFormat::VST2) ++vst2;
        else                                ++vst3;
        if (p.isInstrument) ++instr;
    }

    std::printf("\n================ SUMMARY ================\n");
    std::printf("total plugins : %zu\n", all.size());
    std::printf("  VST2        : %d\n", vst2);
    std::printf("  VST3        : %d\n", vst3);
    std::printf("  instruments : %d\n", instr);
    if (argc > 1)
        std::printf("cache written : %s\n", argv[1]);

    return 0;
}
