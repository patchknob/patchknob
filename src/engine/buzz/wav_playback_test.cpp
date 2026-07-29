//----------------------------------------------------------------------------
//  wav_playback_test.cpp -- load a real WAV from disk, play it through the
//  Sampler at its ROOT note with the engine at the WAV's own sample rate (so
//  playback is 1:1 -- no resampling), and measure output vs input: peak, gain,
//  and clipped-sample count.  This isolates gain-staging / clipping distortion
//  from any resampling artifacts.  Usage: wav_playback_test [file.wav] [velocity]
//----------------------------------------------------------------------------
#include "sampler_instrument.h"
#include "../plugin_api.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

using namespace PatchKnob::engine;

// Minimal WAV reader: PCM 16/24/32-bit int + 32-bit float, mono/stereo.
static bool loadWavFloat(const char* path, std::vector<float>& interleaved,
                         int& channels, int& sampleRate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return false; }
    std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b((size_t)sz);
    if (std::fread(b.data(), 1, (size_t)sz, f) != (size_t)sz) { std::fclose(f); return false; }
    std::fclose(f);
    if (sz < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data()+8, "WAVE", 4)) return false;

    int bits = 0, fmt = 1; size_t dataOff = 0, dataLen = 0;
    size_t p = 12;
    auto u16 = [&](size_t o){ return (uint16_t)(b[o] | (b[o+1]<<8)); };
    auto u32 = [&](size_t o){ return (uint32_t)(b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((uint32_t)b[o+3]<<24)); };
    while (p + 8 <= (size_t)sz) {
        size_t clen = u32(p+4); const uint8_t* id = b.data()+p;
        if (!std::memcmp(id, "fmt ", 4)) {
            fmt = u16(p+8); channels = u16(p+10); sampleRate = (int)u32(p+12); bits = u16(p+22);
        } else if (!std::memcmp(id, "data", 4)) {
            dataOff = p + 8; dataLen = clen;
        }
        p += 8 + clen + (clen & 1);
    }
    if (!dataOff || !bits) return false;
    if (dataOff + dataLen > (size_t)sz) dataLen = (size_t)sz - dataOff;

    const int bytes = bits / 8;
    const size_t nsmp = dataLen / bytes;
    interleaved.resize(nsmp);
    const uint8_t* d = b.data() + dataOff;
    for (size_t i = 0; i < nsmp; ++i) {
        const uint8_t* s = d + i * bytes;
        float v = 0;
        if (fmt == 3 && bits == 32) { std::memcpy(&v, s, 4); }
        else if (bits == 16) { int16_t x; std::memcpy(&x, s, 2); v = x / 32768.f; }
        else if (bits == 24) { int32_t x = (s[0]) | (s[1]<<8) | (s[2]<<16); if (x & 0x800000) x |= ~0xFFFFFF; v = x / 8388608.f; }
        else if (bits == 32) { int32_t x; std::memcpy(&x, s, 4); v = (float)(x / 2147483648.0); }
        interleaved[i] = v;
    }
    return true;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "C:/Users/lain/Desktop/11.wav";
    const int   vel  = argc > 2 ? std::atoi(argv[2]) : 127;

    std::vector<float> il; int ch = 0, sr = 0;
    if (!loadWavFloat(path, il, ch, sr)) { std::printf("FAIL: could not read %s\n", path); return 1; }
    const int frames = (int)(il.size() / (ch ? ch : 1));
    // input peak
    double inPeak = 0; for (float v : il) inPeak = std::max(inPeak, (double)std::fabs(v));
    std::printf("WAV: %s\n  %d ch, %d Hz, %d frames (%.2fs), input peak %.4f\n",
                path, ch, sr, frames, (double)frames / sr, inPeak);

    IPluginInstance* inst = create_sampler_instrument();
    inst->prepare((double)sr, 512);          // engine == WAV rate -> root plays 1:1
    inst->setActive(true);
    // load stereo if 2ch (sampler wants interleaved L/R); mono passes through
    sampler_load_sample(inst, 1, 0, il.data(), frames, ch >= 2,
                        /*root*/60, sr, 0, frames, /*loop*/false, 0, 127, "wav");

    const int total = frames + 4800;         // a little tail
    std::vector<float> outL(total, 0), outR(total, 0), bl(512), br(512);
    int pos = 0; bool first = true;
    while (pos < total) {
        int nf = std::min(512, total - pos);
        float* outs[2] = { bl.data(), br.data() };
        MidiEvent ev{0, 0x90, 60, (uint8_t)vel};
        ProcessBlock pb{}; pb.audioOut = outs; pb.nframes = nf;
        pb.midiIn = first ? &ev : nullptr; pb.numMidiIn = first ? 1 : 0;
        pb.numAudioOut = 2; pb.numAudioIn = 0;
        for (int i = 0; i < 512; ++i) { bl[i] = br[i] = 0; }
        inst->process(pb);
        for (int i = 0; i < nf; ++i) { outL[pos+i] = bl[i]; outR[pos+i] = br[i]; }
        pos += nf; first = false;
    }

    // measure output
    double outPeak = 0; int clip = 0; double sumSq = 0;
    for (int i = 0; i < total; ++i) {
        double a = std::fabs(outL[i]); if (a > outPeak) outPeak = a;
        if (a >= 0.999) ++clip;
        sumSq += outL[i]*outL[i];
    }
    double gain = inPeak > 1e-9 ? outPeak / inPeak : 0;
    std::printf("OUT (vel %d): peak %.4f | gain vs input %.3fx | clipped(|x|>=0.999) samples %d | rms %.4f\n",
                vel, outPeak, gain, clip, std::sqrt(sumSq / total));
    std::printf("  verdict: %s\n",
                (outPeak > 1.0001 || clip > 0) ? "DISTORTED (output exceeds 0dBFS / clips)"
                : (gain > 1.05 ? "hot (gain >1.05x -- will clip louder material)" : "clean (<= unity, no clipping)"));

    inst->release(); delete inst;
    return 0;
}
