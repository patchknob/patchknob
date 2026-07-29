//----------------------------------------------------------------------------
//  sampler_meow_test.cpp -- load the ACTUAL sampler from the user's meow.s24 and
//  play its exact pattern (kick note60 @tick0 with NO note-off; snare note61
//  @tick0, off@tick192, on@tick192).  A/B: render WITH the snare vs WITHOUT it;
//  the difference isolates the snare's effect.  If firing the snare re-onsets the
//  KICK (a fresh copy of the kick waveform appears at tick 192 in the diff), the
//  kick was wrongly retriggered.  Detected by correlating the diff at tick192
//  against the kick sample's own onset.
//----------------------------------------------------------------------------
#include "sampler_instrument.h"
#include "../plugin_api.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

using namespace PatchKnob::engine;

static std::vector<uint8_t> extract_smps(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t)n);
    if (std::fread(d.data(), 1, (size_t)n, f) != (size_t)n) { std::fclose(f); return {}; }
    std::fclose(f);
    for (size_t i = 0; i + 8 < d.size(); ++i)
        if (d[i]=='S'&&d[i+1]=='M'&&d[i+2]=='P'&&d[i+3]=='S' && i>=4) {
            uint32_t len = d[i-4]|(d[i-3]<<8)|(d[i-2]<<16)|((uint32_t)d[i-1]<<24);
            if (i + len <= d.size()) return std::vector<uint8_t>(d.begin()+i, d.begin()+i+len);
        }
    return {};
}

struct Ev { int tick; uint8_t status, d0, d1; };

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "C:/Users/lain/Desktop/meow.s24";
    const double sr = 48000.0; const int block = 128;
    const int spt = 125;              // samples/tick @120bpm 192ppqn 48k

    std::vector<uint8_t> blob = extract_smps(path);
    if (blob.empty()) { std::printf("FAIL: no SMPS blob in %s\n", path); return 1; }
    std::printf("loaded SMPS blob %zu bytes from %s\n", blob.size(), path);

    IPluginInstance* inst = create_sampler_instrument();
    inst->prepare(sr, block); inst->setActive(true);
    inst->loadState(blob);
    std::printf("zones=%d hasEnv=%d\n", sampler_zone_count(inst), (int)sampler_has_envelopes(inst));
    SamplerZoneInfo kickZ;
    if (!sampler_get_zone(inst, 0, kickZ) || kickZ.pcm.empty()) { std::printf("FAIL: no kick zone\n"); return 1; }
    std::printf("kick '%s' root=%d frames=%d\n", kickZ.name.c_str(), kickZ.rootKey, kickZ.numFrames);

    // meow pattern events
    std::vector<Ev> full = {
        {0,   0x90, 60, 100},   // kick ON (no off anywhere)
        {0,   0x90, 61, 100},   // snare ON
        {192, 0x80, 61, 0},     // snare OFF
        {192, 0x90, 61, 100},   // snare ON
    };
    std::vector<Ev> kickOnly = { {0, 0x90, 60, 100} };   // B: kick alone

    const int total = 384 * spt;      // 2 beats
    auto render = [&](const std::vector<Ev>& evs, std::vector<float>& out) {
        // fresh instrument each render so voice state can't carry between A and B
        IPluginInstance* t = create_sampler_instrument();
        t->prepare(sr, block); t->setActive(true); t->loadState(blob);
        out.assign(total, 0.f);
        std::vector<float> bl(block), br(block);
        size_t ei = 0; int pos = 0;
        while (pos < total) {
            int nf = std::min(block, total - pos);
            MidiEvent me[8]; int nm = 0;
            while (ei < evs.size() && evs[ei].tick * spt < pos + nf && nm < 8) {
                int off = evs[ei].tick * spt - pos; if (off < 0) off = 0;
                me[nm++] = MidiEvent{off, evs[ei].status, evs[ei].d0, evs[ei].d1};
                ++ei;
            }
            float* outs[2] = { bl.data(), br.data() };
            ProcessBlock pb{}; pb.audioOut = outs; pb.nframes = nf;
            pb.midiIn = nm ? me : nullptr; pb.numMidiIn = nm;
            pb.numAudioOut = 2; pb.numAudioIn = 0;
            for (int i = 0; i < block; ++i) { bl[i] = br[i] = 0; }
            t->process(pb);
            for (int i = 0; i < nf; ++i) out[pos + i] = bl[i];
            pos += nf;
        }
        t->release(); delete t;
    };

    std::vector<float> A, B;
    std::fprintf(stderr, "=== RENDER A (kick+snare) ===\n"); render(full, A);
    std::fprintf(stderr, "=== RENDER B (kick only) ===\n"); render(kickOnly, B);

    // diff = A - B  -> the snare's contribution (plus any change it caused to the kick)
    std::vector<float> diff(total);
    for (int i = 0; i < total; ++i) diff[i] = A[i] - B[i];

    // normalized cross-correlation of diff at tick192 against the kick onset.  If the
    // snare re-onset the kick, the kick's onset waveform appears here.
    const int at = 192 * spt;
    const int W = std::min(2000, kickZ.numFrames);
    // build a mono kick onset reference from the stored (interleaved) pcm
    const int kch = kickZ.stereo ? 2 : 1;
    std::vector<float> kref(W);
    for (int i = 0; i < W; ++i) kref[i] = kickZ.pcm[(size_t)i * kch];
    auto ncc = [&](const float* x) {
        double xx = 0, kk = 0, xk = 0;
        for (int i = 0; i < W; ++i) { xx += x[i]*x[i]; kk += kref[i]*kref[i]; xk += x[i]*kref[i]; }
        if (xx < 1e-12 || kk < 1e-12) return 0.0;
        return xk / std::sqrt(xx * kk);
    };
    // also build a SNARE reference (zone 1) to tell "snare plays kick" from a windowing artifact
    SamplerZoneInfo snareZ; sampler_get_zone(inst, 1, snareZ);
    const int sch = snareZ.stereo ? 2 : 1;
    std::vector<float> sref(W, 0.f);
    for (int i = 0; i < W && i < snareZ.numFrames; ++i) sref[i] = snareZ.pcm[(size_t)i * sch];
    auto nccRef = [&](const float* x, const std::vector<float>& ref) {
        double xx = 0, kk = 0, xk = 0;
        for (int i = 0; i < W; ++i) { xx += x[i]*x[i]; kk += ref[i]*ref[i]; xk += x[i]*ref[i]; }
        if (xx < 1e-12 || kk < 1e-12) return 0.0;
        return xk / std::sqrt(xx * kk);
    };
    double corrSnare = (at + W <= total) ? nccRef(diff.data() + at, sref) : 0.0;
    std::fprintf(stderr, "diff-vs-KICK corr=%.3f  diff-vs-SNARE corr=%.3f\n",
                 (at + W <= total) ? ncc(diff.data() + at) : 0.0, corrSnare);
    double corrDiff = (at + W <= total) ? ncc(diff.data() + at) : 0.0;
    // baseline: correlate the kick reference against a spot where the kick is NOT (silence-ish)
    double rms192 = 0; for (int i = at; i < std::min(total, at + W); ++i) rms192 += diff[i]*diff[i];
    rms192 = std::sqrt(rms192 / W);

    std::printf("diff (snare effect) at tick192: rms=%.5f  kick-onset correlation=%.3f\n", rms192, corrDiff);
    bool retrigger = corrDiff > 0.5 && rms192 > 1e-3;
    std::printf("\n%s\n", retrigger
        ? "BUG REPRODUCED: firing the snare RE-ONSET the kick (kick waveform reappears at tick192)."
        : "OK: the snare did not retrigger the kick.");

    inst->release(); delete inst;
    return retrigger ? 1 : 0;
}
