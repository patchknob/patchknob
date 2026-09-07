#include "../rack/rack_engine.h"
#include "../rack/rack_factory.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    rackx::registerRackExtModules();
    rackx::RackEngine rack;
    rack.setSampleRate(48000.0);
    rack.setPolyphony(8);
    const int cs = rack.addModule("PKCsound", 0.f, 0.f);
    if (cs < 0) {
        std::fprintf(stderr, "failed to insert PKCsound\n");
        return 1;
    }
    // Saved project text must remain byte-for-byte authoritative, even when it
    // contains phrases that appeared in an older generated starter orchestra.
    const std::string original = rack.moduleScript(cs);
    const std::string preserve =
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "ksmps=32\nnchnls=1\n0dbfs=1\n"
        "; Rack pitch is 1 V/oct with 0 V = C4:\n"
        "instr 7\nkvoct chnget \"jack-1\"\n"
        "chnset kvoct, \"jack-2\"\nendin\n"
        "</CsInstruments>\n<CsScore>\ni 7 0 z\n</CsScore>\n"
        "</CsoundSynthesizer>\n";
    if (!rack.setModuleScript(cs, preserve) || rack.moduleScript(cs) != preserve) {
        std::fprintf(stderr, "PKCsound changed saved CSD text\n");
        return 5;
    }
    rack.setModuleScript(cs, original);
    // Exercise the actual saved demo when present.
    std::ifstream project("build/moo.s24", std::ios::binary);
    if (project) {
        const std::string bytes((std::istreambuf_iterator<char>(project)),
                                std::istreambuf_iterator<char>());
        const std::string open = "<CsoundSynthesizer>";
        const std::string close = "</CsoundSynthesizer>";
        const size_t a = bytes.find(open), b = bytes.find(close, a);
        if (a != std::string::npos && b != std::string::npos)
            if (!rack.setModuleScript(cs, bytes.substr(a, b + close.size() - a))) {
                std::fprintf(stderr, "moo.s24 Csound script failed to load\n");
                return 4;
            }
    }
    rackx::RackModule* module = nullptr;
    for (int i = 0; i < rack.moduleCount(); ++i)
        if (rack.moduleAt(i) && rack.moduleAt(i)->id == cs) module = rack.moduleAt(i);
    if (!module || !module->mod || module->mod->inputs.size() < 2) return 2;
    module->mod->inputs[0].setChannels(2);
    module->mod->inputs[0].voltages[0] = 0.f;       // C4
    module->mod->inputs[0].voltages[1] = 7.f / 12.f;// G4
    module->mod->inputs[1].setChannels(2);
    // Only the second VCA lane is open. This specifically catches the old
    // getVoltage() bridge, which discarded every a-rate lane except voice zero.
    module->mod->inputs[1].voltages[0] = 0.f;
    module->mod->inputs[1].voltages[1] = 10.f;
    float peak = 0.f;
    float l[257] = {}, r[257] = {};
    for (int block = 0; block < 24; ++block) {
        rack.process(257, nullptr, nullptr, l, r, nullptr, 0);
        const float x = module->mod->outputs[0].getVoltage();
        if (x < -peak || x > peak) peak = x < 0 ? -x : x;
    }
    if (!(peak > 0.001f)) {
        std::fprintf(stderr, "poly Csound produced silence\n");
        return 3;
    }
    std::printf("PKCsound insert + two-voice render OK (id=%d peak=%.4f)\n", cs, peak);
    return 0;
}
