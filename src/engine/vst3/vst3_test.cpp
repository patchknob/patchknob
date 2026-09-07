//----------------------------------------------------------------------------
//  vst3_test.cpp - smoke test for the PatchKnob VST3 host module.
//
//  Loads an instrument (TAL-U-NO-LX-V2 by default), prepares 48000/512, sends
//  a Note-On (C4), processes ~50 blocks, asserts the output is non-silent, and
//  writes vst3_test_out.wav. Prints name, bus layout, and peak.
//----------------------------------------------------------------------------

#include "vst3_host.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <system_error>

using namespace PatchKnob::engine;

namespace {

// Candidate instruments to try, in order. First one that loads + has a MIDI
// event-in bus is used.
// Tried in order. The first that loads, is an instrument (MIDI event-in bus),
// AND produces non-silent output for a plain Note-On is used. (Some synths -
// e.g. TAL-U-NO-LX-V2, TAL-Drum - default to a silent patch/empty kit and so
// make no sound without further setup; we skip past those for the smoke test.)
// The list was Windows-only absolute paths, so on the Linux build every
// candidate "could not load" and the test reported FAILED on a machine that
// simply has no VST3 installed.  Keep the Windows names, add the standard
// Linux search paths, and let the caller pass a path as argv[1].
const char* kCandidates[] = {
#ifdef _WIN32
    "C:/Program Files/Common Files/VST3/TAL/TAL-U-NO-LX-V2.vst3",
    "C:/Program Files/Common Files/VST3/TAL/TAL-BassLine-101.vst3",
    "C:/Program Files/Common Files/VST3/TAL/TAL-J-8.vst3",
    "C:/Program Files/Common Files/VST3/TAL/TAL-Sampler.vst3",
#endif
};

// Every .vst3 bundle under the standard user/system directories, so the test
// finds whatever this machine actually has.
void collectInstalledVst3(std::vector<std::string>& out)
{
#ifndef _WIN32
    namespace fs = std::filesystem;
    std::vector<std::string> roots;
    if (const char* home = std::getenv("HOME"))
        roots.emplace_back(std::string(home) + "/.vst3");
    roots.emplace_back("/usr/lib/vst3");
    roots.emplace_back("/usr/local/lib/vst3");
    for (const std::string& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".vst3") out.push_back(it->path().string());
    }
#else
    (void)out;
#endif
}

void writeWav(const char* path, const std::vector<float>& interleaved,
              int channels, int sampleRate)
{
    const uint32_t numSamples = static_cast<uint32_t>(interleaved.size());
    const uint16_t bits = 16;
    const uint32_t byteRate = sampleRate * channels * (bits / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t dataBytes = numSamples * (bits / 8);

    FILE* f = std::fopen(path, "wb");
    if (!f) { std::printf("could not open %s for writing\n", path); return; }

    auto w32 = [&](uint32_t v){ std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(static_cast<uint16_t>(channels));
    w32(static_cast<uint32_t>(sampleRate)); w32(byteRate); w16(blockAlign); w16(bits);
    std::fwrite("data", 1, 4, f); w32(dataBytes);

    for (float s : interleaved)
    {
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = static_cast<int16_t>(s * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// Render a Note-On C4 through the instance for `numBlocks` blocks at sr/block.
// Returns the peak abs sample and fills `recorded` (interleaved stereo).
float renderNote(IPluginInstance* inst, double sr, int block, int numBlocks,
                 std::vector<float>& recorded)
{
    const PluginDescriptor& d = inst->descriptor();
    const int nOut = d.numAudioOut > 0 ? d.numAudioOut : 2;

    std::vector<std::vector<float>> outBufs(static_cast<size_t>(nOut),
                                            std::vector<float>(static_cast<size_t>(block), 0.0f));
    std::vector<float*> outPtrs(static_cast<size_t>(nOut));
    for (int c = 0; c < nOut; ++c)
        outPtrs[static_cast<size_t>(c)] = outBufs[static_cast<size_t>(c)].data();

    MidiEvent noteOn{};  noteOn.sampleOffset = 0; noteOn.status = 0x90; noteOn.data1 = 60; noteOn.data2 = 100;
    MidiEvent noteOff{}; noteOff.sampleOffset = 0; noteOff.status = 0x80; noteOff.data1 = 60; noteOff.data2 = 0;

    recorded.clear();
    recorded.reserve(static_cast<size_t>(numBlocks) * static_cast<size_t>(block) * 2);

    float peak = 0.0f;
    int64_t pos = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int c = 0; c < nOut; ++c)
            std::memset(outPtrs[static_cast<size_t>(c)], 0, sizeof(float) * static_cast<size_t>(block));

        ProcessBlock pb{};
        pb.audioIn = nullptr;
        pb.audioOut = outPtrs.data();
        pb.nframes = block;
        if (b == 0)        { pb.midiIn = &noteOn;  pb.numMidiIn = 1; }
        else if (b == 45)  { pb.midiIn = &noteOff; pb.numMidiIn = 1; }
        else               { pb.midiIn = nullptr;  pb.numMidiIn = 0; }
        pb.paramIn = nullptr; pb.numParamIn = 0;
        pb.tempoBpm = 120.0;
        pb.playPositionSamples = pos;
        pb.isPlaying = true;

        inst->process(pb);

        for (int i = 0; i < block; ++i)
        {
            float l = outBufs[0][static_cast<size_t>(i)];
            float r = (nOut > 1) ? outBufs[1][static_cast<size_t>(i)] : l;
            recorded.push_back(l);
            recorded.push_back(r);
            float a  = l < 0 ? -l : l;
            float ar = r < 0 ? -r : r;
            if (a  > peak) peak = a;
            if (ar > peak) peak = ar;
        }
        pos += block;
    }
    return peak;
}

void printInfo(IPluginInstance* inst, const std::string& path)
{
    const PluginDescriptor& d = inst->descriptor();
    std::printf("Loaded: %s\n", path.c_str());
    std::printf("  name        : %s\n", d.name.c_str());
    std::printf("  vendor      : %s\n", d.vendor.c_str());
    std::printf("  audio in    : %d\n", d.numAudioIn);
    std::printf("  audio out   : %d\n", d.numAudioOut);
    std::printf("  isInstrument: %s\n", d.isInstrument ? "yes" : "no");
    std::printf("  param count : %d\n", inst->paramCount());
}

} // namespace

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so output survives a crash
    std::printf("=== PatchKnob VST3 host test ===\n");

    const double sr = 48000.0;
    const int    block = 512;
    const int    numBlocks = 50;
    const float  kThreshold = 1e-4f;

    std::vector<std::string> paths;
    if (argc > 1)
        paths.emplace_back(argv[1]);
    else {
        for (const char* c : kCandidates) paths.emplace_back(c);
        collectInstalledVst3(paths);
    }

    IPluginInstance* chosen = nullptr;
    bool loadedAny = false;          // did ANY candidate even open?
    std::string chosenPath;
    float chosenPeak = 0.0f;
    std::vector<float> chosenRec;

    for (const std::string& path : paths)
    {
        PluginDescriptor desc;
        desc.format = PluginFormat::VST3;
        desc.path = path;
        IPluginInstance* inst = createVst3Instance(desc);
        if (!inst)
        {
            std::printf("(skip) could not load %s\n", path.c_str());
            continue;
        }
        loadedAny = true;
        if (!inst->descriptor().isInstrument)
        {
            std::printf("(skip) %s has no MIDI event-in bus\n", path.c_str());
            inst->release(); delete inst; continue;
        }

        printInfo(inst, path);
        if (!inst->prepare(sr, block))
        {
            std::printf("(skip) prepare failed for %s\n", path.c_str());
            inst->release(); delete inst; continue;
        }
        inst->setActive(true);

        std::vector<float> rec;
        float peak = renderNote(inst, sr, block, numBlocks, rec);
        std::printf("  peak        : %.6f\n", peak);

        inst->setActive(false);

        if (peak >= kThreshold || argc > 1)
        {
            chosen = inst; chosenPath = path; chosenPeak = peak; chosenRec.swap(rec);
            break;
        }
        std::printf("  (silent default patch; trying next candidate)\n");
        inst->release(); delete inst;
    }

    if (!chosen)
    {
        // Nothing on this machine could even be opened -- there is no VST3
        // installed to smoke-test against.  That is not a host defect, and
        // failing on it made the suite red on any clean checkout.
        if (!loadedAny) {
            std::printf("SKIP: no VST3 plugin found to test against "
                        "(pass one as argv[1] to force it).\n");
            return 0;
        }
        std::printf("FAILED: no instrument produced non-silent output.\n");
        return 1;
    }

    writeWav("vst3_test_out.wav", chosenRec, 2, static_cast<int>(sr));
    std::printf("  wrote vst3_test_out.wav (%zu frames stereo)\n", chosenRec.size() / 2);

    chosen->release();
    delete chosen;

    if (chosenPeak < kThreshold)
    {
        std::printf("FAILED: output is silent (peak %.8f < %.8f) for %s.\n",
                    chosenPeak, kThreshold, chosenPath.c_str());
        return 1;
    }

    std::printf("PASS: non-silent output (peak %.6f) from %s.\n",
                chosenPeak, chosenPath.c_str());
    return 0;
}
