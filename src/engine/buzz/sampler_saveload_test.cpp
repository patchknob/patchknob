//----------------------------------------------------------------------------
//  sampler_saveload_test.cpp -- round-trip the sampler's state through
//  saveState()/loadState(): load a kick (C4) + snare (D4) with an amp envelope,
//  serialize, restore into a FRESH instrument, and verify the samples, zones,
//  and envelope survive AND still route correctly (C4->kick, D4->snare).
//----------------------------------------------------------------------------
#include "sampler_instrument.h"
#include "../plugin_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace PatchKnob::engine;
static const double PI = 3.14159265358979323846;

static double toneMag(const float* x, int n, double sr, double freq) {
    double w = 2.0 * PI * freq / sr, c = std::cos(w), coeff = 2.0 * c, s1 = 0, s2 = 0, s0;
    for (int i = 0; i < n; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * c, im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / std::max(1, n);
}

static double playNote(IPluginInstance* inst, int note, double sr, int block, int frames, double hz) {
    std::vector<float> out(frames, 0.f), bl(block), br(block);
    int pos = 0; bool first = true;
    while (pos < frames) {
        int nf = std::min(block, frames - pos);
        float* outs[2] = { bl.data(), br.data() };
        MidiEvent ev{0, 0x90, (uint8_t)note, 110};
        ProcessBlock pb{}; pb.audioOut = outs; pb.nframes = nf;
        pb.midiIn = first ? &ev : nullptr; pb.numMidiIn = first ? 1 : 0;
        pb.numAudioOut = 2; pb.numAudioIn = 0;
        for (int i = 0; i < block; ++i) { bl[i] = br[i] = 0; }
        inst->process(pb);
        for (int i = 0; i < nf; ++i) out[pos + i] = bl[i];
        pos += nf; first = false;
    }
    return toneMag(out.data(), frames, sr, hz);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double sr = 48000.0; const int block = 256; int fails = 0;

    const int kf = (int)(0.25 * sr), sf = (int)(0.12 * sr);
    std::vector<float> kick(kf), snare(sf);
    for (int i = 0; i < kf; ++i) kick[i]  = 0.8f * (float)(std::sin(2*PI*100.0*i/sr) * std::exp(-14.0*i/sr));
    for (int i = 0; i < sf; ++i) snare[i] = 0.8f * (float)(std::sin(2*PI*2500.0*i/sr) * std::exp(-26.0*i/sr));

    // 1) build a sampler with two zones + an amp envelope
    IPluginInstance* a = create_sampler_instrument();
    a->prepare(sr, block); a->setActive(true);
    sampler_load_sample(a, 1, 0, kick.data(),  kf, false, 60, (int)sr, 0, kf, false, 0, 127, "kick");
    sampler_load_sample(a, 1, 1, snare.data(), sf, false, 62, (int)sr, 0, sf, false, 0, 127, "snare");
    // amp envelope (dense points on the 0..65535 axes)
    bool useEnv = !(argc > 1 && std::string(argv[1]) == "noenv");
    if (useEnv) {
        unsigned short xs[4] = {0, 5000, 20000, 65535};
        unsigned short ys[4] = {0, 65535, 40000, 0};
        int flags[4] = {0, 0, 1 /*sustain*/, 0};
        sampler_set_envelope(a, 0, xs, ys, flags, 4);
    }
    std::printf("built: zones=%d hasEnv=%d (useEnv=%d)\n", sampler_zone_count(a), (int)sampler_has_envelopes(a), (int)useEnv);
    // sanity: does the ORIGINAL instrument play the kick BEFORE save?
    std::printf("original: C4 kick 100Hz=%.4f\n", playNote(a, 60, sr, block, (int)(0.2*sr), 100.0));

    // 2) serialize -> 3) restore into a FRESH instrument
    std::vector<uint8_t> blob = a->saveState();
    std::printf("saveState blob = %zu bytes\n", blob.size());
    IPluginInstance* b = create_sampler_instrument();
    b->prepare(sr, block); b->setActive(true);
    b->loadState(blob);

    // 4) verify restored metadata
    int zc = sampler_zone_count(b);
    bool hasEnv = sampler_has_envelopes(b);
    std::printf("restored: zones=%d hasEnv=%d\n", zc, (int)hasEnv);
    if (zc != 2)  { std::printf("FAIL: expected 2 zones, got %d\n", zc); ++fails; }
    if (!hasEnv)  { std::printf("FAIL: envelope not restored\n"); ++fails; }
    SamplerZoneInfo z0, z1;
    if (sampler_get_zone(b, 0, z0) && sampler_get_zone(b, 1, z1)) {
        std::printf("  zone0 root=%d name=%s frames=%d pcm=%zu\n", z0.rootKey, z0.name.c_str(), z0.numFrames, z0.pcm.size());
        std::printf("  zone1 root=%d name=%s frames=%d pcm=%zu\n", z1.rootKey, z1.name.c_str(), z1.numFrames, z1.pcm.size());
        if (z0.pcm.empty() || z1.pcm.empty()) { std::printf("FAIL: PCM not restored\n"); ++fails; }
    } else { std::printf("FAIL: zone read-back failed\n"); ++fails; }

    // 5) verify audio routes correctly after restore (nearest-root)
    double k = playNote(b, 60, sr, block, (int)(0.2*sr), 100.0);   // C4 -> kick 100Hz
    double s = playNote(b, 62, sr, block, (int)(0.2*sr), 2500.0);  // D4 -> snare 2500Hz
    std::printf("restored audio: C4 kick 100Hz=%.4f | D4 snare 2500Hz=%.4f\n", k, s);
    if (k < 0.01) { std::printf("FAIL: C4 did not play the kick after restore\n"); ++fails; }
    if (s < 0.01) { std::printf("FAIL: D4 did not play the snare after restore\n"); ++fails; }

    a->release(); delete a; b->release(); delete b;
    std::printf(fails ? "\nSAMPLER SAVE/LOAD: %d FAILURES\n" : "\nSAMPLER SAVE/LOAD: OK\n", fails);
    return fails ? 1 : 0;
}
