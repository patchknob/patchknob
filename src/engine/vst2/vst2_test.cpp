//----------------------------------------------------------------------------
//  seq24 VST2 host test harness.
//
//  Finds a 64-bit VST2 instrument under C:\Program Files\VstPlugins, loads it
//  through Vst2PluginInstance, prepares at 48000/512, sends a C4 (vel 100)
//  Note-On, processes ~50 blocks, asserts the output is non-silent, writes the
//  rendered audio to vst2_test_out.wav, and prints diagnostics.
//----------------------------------------------------------------------------
#include "vst2_host.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using namespace seq24::engine;

// --- minimal 16-bit PCM WAV writer -----------------------------------------
static void writeWav(const std::string& path, const std::vector<float>& interleaved,
                     int channels, int sampleRate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("WAV: could not open %s\n", path.c_str()); return; }

    uint32_t numSamples = (uint32_t)interleaved.size();        // total samples
    uint32_t dataBytes  = numSamples * 2;                       // 16-bit
    uint32_t byteRate   = (uint32_t)(sampleRate * channels * 2);
    uint16_t blockAlign = (uint16_t)(channels * 2);
    uint32_t riffSize   = 36 + dataBytes;

    auto w32 = [&](uint32_t v){ std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); w32(riffSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16((uint16_t)channels);
    w32((uint32_t)sampleRate); w32(byteRate); w16(blockAlign); w16(16);
    std::fwrite("data", 1, 4, f); w32(dataBytes);

    for (uint32_t i = 0; i < numSamples; ++i)
    {
        float s = interleaved[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)std::lround(s * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// --- objdump-free 64-bit check via PE header --------------------------------
static bool isPe64(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t dos[64];
    if (std::fread(dos, 1, 64, f) == 64 && dos[0] == 'M' && dos[1] == 'Z')
    {
        uint32_t peOff = *(uint32_t*)(dos + 0x3C);
        if (std::fseek(f, (long)peOff, SEEK_SET) == 0)
        {
            uint8_t sig[6];
            if (std::fread(sig, 1, 6, f) == 6 &&
                sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0)
            {
                uint16_t machine = (uint16_t)(sig[4] | (sig[5] << 8));
                ok = (machine == 0x8664); // IMAGE_FILE_MACHINE_AMD64
            }
        }
    }
    std::fclose(f);
    return ok;
}

// --- recursively collect candidate .dll paths ------------------------------
static void collectDlls(const std::string& dir, std::vector<std::string>& out,
                        int depth)
{
    if (depth < 0) return;
    WIN32_FIND_DATAA fd;
    std::string pat = dir + "\\*";
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            collectDlls(full, out, depth - 1);
        else
        {
            size_t dot = name.rfind('.');
            if (dot != std::string::npos)
            {
                std::string ext = name.substr(dot);
                for (auto& c : ext) c = (char)tolower(c);
                if (ext == ".dll") out.push_back(full);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Render a short probe (Note-On C4) and return the peak, to find an instrument
// that actually produces audio with its default preset. (Some synths, e.g.
// sample-based drum machines, ship a silent/empty default program.)
static float probePeak(Vst2PluginInstance* inst, int numOut, int sr, int block)
{
    (void)sr;
    std::vector<std::vector<float>> bufs((size_t)numOut,
                                         std::vector<float>((size_t)block, 0.0f));
    std::vector<float*> ptrs((size_t)numOut);
    float peak = 0.0f; int64_t pos = 0;
    for (int b = 0; b < 40; ++b)
    {
        for (int c = 0; c < numOut; ++c)
        {
            std::fill(bufs[(size_t)c].begin(), bufs[(size_t)c].end(), 0.0f);
            ptrs[(size_t)c] = bufs[(size_t)c].data();
        }
        MidiEvent ev; ProcessBlock pb; std::memset(&pb, 0, sizeof(pb));
        pb.audioOut = ptrs.data(); pb.nframes = block;
        pb.tempoBpm = 120.0; pb.isPlaying = true; pb.playPositionSamples = pos;
        if (b == 0)
        {
            ev.sampleOffset = 0; ev.status = 0x90; ev.data1 = 60; ev.data2 = 110;
            pb.midiIn = &ev; pb.numMidiIn = 1;
        }
        inst->process(pb);
        for (int c = 0; c < numOut; ++c)
            for (int i = 0; i < block; ++i)
            {
                float a = std::fabs(bufs[(size_t)c][(size_t)i]);
                if (a > peak) peak = a;
            }
        pos += block;
    }
    return peak;
}

int main()
{
    const char* root = "C:\\Program Files\\VstPlugins";
    const int sr = 48000;
    const int block = 512;

    // Preferred candidates first (known-audible 64-bit synths), then a scan.
    // 808 Machine is listed but ships a silent default kit, so it is only used
    // as a last resort — the probe below skips silent instruments.
    std::vector<std::string> candidates = {
        std::string(root) + "\\Arturia\\Jup-8000 V.dll",
        std::string(root) + "\\GForce\\impOSCar3.dll",
        std::string(root) + "\\TubeSynth_x64.dll",
        std::string(root) + "\\808 Machine x64.dll",
    };
    std::vector<std::string> scanned;
    collectDlls(root, scanned, 2);
    for (auto& s : scanned) candidates.push_back(s);

    Vst2PluginInstance* inst = nullptr;
    PluginDescriptor desc;
    std::string chosen;
    // Fallback: first loadable instrument even if its default preset is silent.
    Vst2PluginInstance* fallback = nullptr;
    PluginDescriptor fbDesc; std::string fbPath;

    for (const auto& path : candidates)
    {
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (!isPe64(path))
        {
            std::printf("skip (not 64-bit): %s\n", path.c_str());
            continue;
        }
        PluginDescriptor d;
        d.format = PluginFormat::VST2;
        d.path   = path;
        Vst2PluginInstance* candidate = new Vst2PluginInstance();
        if (!candidate->load(d))
        {
            std::printf("load failed: %s\n", path.c_str());
            delete candidate;
            continue;
        }
        const PluginDescriptor& got = candidate->descriptor();
        std::printf("loaded: %-24s (synth=%s, in=%d, out=%d, params=%d)\n",
                    got.name.c_str(), got.isInstrument ? "yes" : "no",
                    got.numAudioIn, got.numAudioOut, candidate->paramCount());

        if (!got.isInstrument || got.numAudioOut <= 0)
        {
            std::printf("  -> not a usable instrument, trying next\n");
            candidate->release(); delete candidate;
            continue;
        }

        // Probe for audible output with the default preset.
        if (!candidate->prepare(sr, block))
        {
            std::printf("  -> prepare() failed, trying next\n");
            candidate->release(); delete candidate;
            continue;
        }
        float pk = probePeak(candidate, got.numAudioOut, sr, block);
        std::printf("  -> probe peak %.6f\n", pk);
        if (pk > 0.0f)
        {
            inst = candidate; desc = got; chosen = path;
            break; // already prepared; reuse for the full render
        }
        // Keep the first usable-but-silent one as a fallback, discard the rest.
        if (!fallback)
        {
            fallback = candidate; fbDesc = got; fbPath = path;
        }
        else
        {
            candidate->release(); delete candidate;
        }
    }

    if (!inst && fallback)
    {
        std::printf("\nNOTE: no instrument was audible with its default preset; "
                    "using first loadable instrument (output may be silent).\n");
        inst = fallback; desc = fbDesc; chosen = fbPath;
    }
    else if (fallback && fallback != inst)
    {
        fallback->release(); delete fallback;
    }

    if (!inst)
    {
        std::printf("FAIL: no usable 64-bit VST2 instrument found under %s\n", root);
        return 1;
    }

    std::printf("\n== testing: %s ==\n", chosen.c_str());
    std::printf("name      : %s\n", desc.name.c_str());
    std::printf("vendor    : %s\n", desc.vendor.c_str());
    std::printf("isSynth   : %s\n", desc.isInstrument ? "yes" : "no");
    std::printf("audio in  : %d\n", desc.numAudioIn);
    std::printf("audio out : %d\n", desc.numAudioOut);
    std::printf("params    : %d\n", inst->paramCount());

    if (!inst->prepare(sr, block))
    {
        std::printf("FAIL: prepare() returned false\n");
        inst->release(); delete inst; return 1;
    }

    const int numOut = desc.numAudioOut;

    // Per-channel non-interleaved output buffers.
    std::vector<std::vector<float>> outBufs((size_t)numOut,
                                            std::vector<float>((size_t)block, 0.0f));
    std::vector<float*> outPtrs((size_t)numOut);

    // Interleaved capture for the WAV (cap channels at 2 for the file).
    int wavCh = numOut >= 2 ? 2 : 1;
    std::vector<float> wavData;

    const int numBlocks = 50;
    float peak = 0.0f;
    int64_t playPos = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int c = 0; c < numOut; ++c)
        {
            std::fill(outBufs[(size_t)c].begin(), outBufs[(size_t)c].end(), 0.0f);
            outPtrs[(size_t)c] = outBufs[(size_t)c].data();
        }

        // Note-On C4 (MIDI 60) vel 100 at the very first block, sample 0.
        MidiEvent ev;
        ProcessBlock pb;
        std::memset(&pb, 0, sizeof(pb));
        pb.audioIn   = nullptr;
        pb.audioOut  = outPtrs.data();
        pb.nframes   = block;
        pb.tempoBpm  = 120.0;
        pb.playPositionSamples = playPos;
        pb.isPlaying = true;

        if (b == 0)
        {
            ev.sampleOffset = 0;
            ev.status = 0x90;   // Note On, channel 0
            ev.data1  = 60;     // C4
            ev.data2  = 100;    // velocity
            pb.midiIn = &ev;
            pb.numMidiIn = 1;
        }
        else
        {
            pb.midiIn = nullptr;
            pb.numMidiIn = 0;
        }

        inst->process(pb);

        // Peak + WAV capture.
        for (int i = 0; i < block; ++i)
        {
            for (int c = 0; c < wavCh; ++c)
            {
                float s = (c < numOut) ? outBufs[(size_t)c][(size_t)i] : 0.0f;
                wavData.push_back(s);
            }
            for (int c = 0; c < numOut; ++c)
            {
                float a = std::fabs(outBufs[(size_t)c][(size_t)i]);
                if (a > peak) peak = a;
            }
        }
        playPos += block;
    }

    std::printf("\nprocessed %d blocks of %d frames @ %d Hz\n",
                numBlocks, block, sr);
    std::printf("peak level: %.6f\n", peak);

    writeWav("vst2_test_out.wav", wavData, wavCh, sr);
    std::printf("wrote vst2_test_out.wav (%d ch, %zu samples)\n",
                wavCh, wavData.size());

    bool nonSilent = peak > 0.0f;
    std::printf("\nRESULT: output is %s (peak %s 0)\n",
                nonSilent ? "NON-SILENT" : "SILENT",
                nonSilent ? ">" : "==");

    inst->setActive(false);
    inst->release();
    delete inst;

    return nonSilent ? 0 : 2;
}
