//----------------------------------------------------------------------------
//  sampler_multinote_test.cpp -- reproduce the user's report: a kick on C4
//  (note column 1) and a snare on C#4 (note column 2).  Playing the SNARE must
//  NOT re-trigger the KICK.  We load a low-frequency "kick" on C4 and a
//  high-frequency "snare" on C#4, play the kick (held, no note-off) and let it
//  decay to silence, then fire ONLY the snare and measure whether kick-band
//  (low-freq) energy reappears -- if it does, the kick was wrongly retriggered.
//----------------------------------------------------------------------------
#include "sampler_instrument.h"
#include "../plugin_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace PatchKnob::engine;
static const double PI = 3.14159265358979323846;

// Goertzel single-bin magnitude at exactly `freq` Hz -> clean separation of the
// kick tone from the snare tone (a pure snare has ~0 energy at the kick's freq).
static double toneMag(const float* x, int n, double sr, double freq) {
    double w = 2.0 * PI * freq / sr, c = std::cos(w), coeff = 2.0 * c;
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * c, im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / std::max(1, n);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double sr = 48000.0;
    const int block = 256;

    // kick = 100 Hz, 1 s, LINEAR fade 1->0 so its amplitude at time t is (1 - t).
    // A still-playing kick keeps FALLING; a re-triggered kick JUMPS back up.
    const int kf = (int)(1.00 * sr);
    std::vector<float> kick(kf);
    for (int i = 0; i < kf; ++i)
        kick[i] = (float)(std::sin(2 * PI * 100.0 * i / sr) * (1.0 - (double)i / kf));
    // snare = 2500 Hz, decays by ~0.1 s (0.15 s buffer)
    const int sf = (int)(0.15 * sr);
    std::vector<float> snare(sf);
    for (int i = 0; i < sf; ++i)
        snare[i] = 0.9f * (float)(std::sin(2 * PI * 2500.0 * i / sr) * std::exp(-28.0 * i / sr));

    // IMPORTANT: the app's editor loads every sample with FULL keyrange 0..127
    // (main.cpp load_sampler_path passes key=-1 -> loKey=0,hiKey=127), so zones
    // OVERLAP.  Reproduce that exactly: kick root C4, snare root C#4, BOTH 0..127.
    const int LO = 0, HI = 127;
    IPluginInstance* inst = create_sampler_instrument();
    inst->prepare(sr, block);
    inst->setActive(true);
    sampler_load_sample(inst, 1, 0, kick.data(),  kf, false, 60, (int)sr, 0, kf, false, LO, HI, "kick");
    sampler_load_sample(inst, 1, 1, snare.data(), sf, false, 61, (int)sr, 0, sf, false, LO, HI, "snare");

    // Sanity: which sample does each note actually play?  Fire C4 alone and C#4
    // alone (fresh instrument each) and report the dominant tone.
    {
        auto oneNote = [&](int note, double& lo100, double& hi2500){
            IPluginInstance* t = create_sampler_instrument(); t->prepare(sr, block); t->setActive(true);
            sampler_load_sample(t, 1, 0, kick.data(),  kf, false, 60, (int)sr, 0, kf, false, LO, HI, "kick");
            sampler_load_sample(t, 1, 1, snare.data(), sf, false, 61, (int)sr, 0, sf, false, LO, HI, "snare");
            std::vector<float> o(kf, 0.f), bl(block), br(block); int pos=0; bool first=true;
            while (pos < (int)o.size()) { int nf=std::min(block,(int)o.size()-pos); float* outs[2]={bl.data(),br.data()};
                MidiEvent ev{0,0x90,(uint8_t)note,110}; ProcessBlock pb{}; pb.audioOut=outs; pb.nframes=nf;
                pb.midiIn=first?&ev:nullptr; pb.numMidiIn=first?1:0; pb.numAudioOut=2; pb.numAudioIn=0;
                for(int i=0;i<block;++i){bl[i]=br[i]=0;} t->process(pb);
                for(int i=0;i<nf;++i)o[pos+i]=bl[i]; pos+=nf; first=false; }
            lo100=toneMag(o.data(),(int)o.size(),sr,100.0); hi2500=toneMag(o.data(),(int)o.size(),sr,2500.0);
            t->release(); delete t;
        };
        double k100,k2500,s100,s2500;
        oneNote(60,k100,k2500); oneNote(61,s100,s2500);
        std::printf("[zone routing] C4(kick):  100Hz=%.4f 2500Hz=%.4f  -> %s\n", k100,k2500, k100>k2500?"KICK (ok)":"SNARE (WRONG)");
        std::printf("[zone routing] C#4(snare): 100Hz=%.4f 2500Hz=%.4f  -> %s\n\n", s100,s2500, s2500>s100?"SNARE (ok)":"KICK (WRONG!)");
    }

    // A/B: render the kick ALONE, then the kick WITH a snare fired at 0.40s.  The
    // ONLY difference in the kick's 100Hz energy after 0.40s that survives (B - A)
    // isolates whether the snare affected the kick voice.  snare arg <0 = no snare.
    const int total = (int)(0.60 * sr);
    const int snareAt = (int)(0.40 * sr);
    auto run = [&](std::vector<float>& out, int snareNote, bool fxOnSnare) {
        out.assign(total, 0.f);
        std::vector<float> bl(block), br(block);
        int pos = 0;
        while (pos < total) {
            int nf = std::min(block, total - pos);
            float* outs[2] = { bl.data(), br.data() };
            MidiEvent evs[2]; int ne = 0;
            if (pos == 0) evs[ne++] = MidiEvent{0, 0x90, 60, 110};
            bool snareBlock = snareNote >= 0 && pos <= snareAt && snareAt < pos + nf;
            if (snareBlock) evs[ne++] = MidiEvent{snareAt - pos, 0x90, (uint8_t)snareNote, 110};
            // simulate the tracker sending an FX-param automation value with the note
            // (forces paramsDirty -> applyParams()+tick() over all 16 tracks)
            ParamChange pc{}; int np = 0;
            if (snareBlock && fxOnSnare && inst->paramCount() > 0) {
                pc.id = inst->paramInfo(inst->paramCount() - 1).id;
                pc.sampleOffset = snareAt - pos; pc.value = 0.5f; np = 1;
            }
            ProcessBlock pb{}; pb.audioOut = outs; pb.nframes = nf;
            pb.midiIn = ne ? evs : nullptr; pb.numMidiIn = ne;
            pb.paramIn = np ? &pc : nullptr; pb.numParamIn = np;
            pb.numAudioOut = 2; pb.numAudioIn = 0;
            for (int i = 0; i < block; ++i) { bl[i] = br[i] = 0; }
            inst->process(pb);
            for (int i = 0; i < nf; ++i) out[pos + i] = bl[i];
            pos += nf;
        }
    };
    auto fresh = [&]() {
        inst->release(); delete inst;
        inst = create_sampler_instrument(); inst->prepare(sr, block); inst->setActive(true);
        sampler_load_sample(inst, 1, 0, kick.data(),  kf, false, 60, (int)sr, 0, kf, false, 60, 60, "kick");
        sampler_load_sample(inst, 1, 1, snare.data(), sf, false, 61, (int)sr, 0, sf, false, 61, 61, "snare");
    };
    std::vector<float> outA, outB, outC;
    run(outA, -1, false);           // kick alone
    fresh(); run(outB, 61, false);  // kick + snare
    fresh(); run(outC, 61, true);   // kick + snare + FX-param automation on the snare block

    int w = (int)(0.06 * sr);
    int at = snareAt + (int)(0.04 * sr);   // window well after the snare onset
    double kickAlone = toneMag(outA.data() + at, w, sr, 100.0);   // kick's natural 100Hz here
    double kickWith  = toneMag(outB.data() + at, w, sr, 100.0);   // kick 100Hz WITH snare (no FX)
    double kickWithFx= toneMag(outC.data() + at, w, sr, 100.0);   // kick 100Hz WITH snare + FX
    // isolate the snare's own contribution (B - A) to discount transient leakage
    std::vector<float> diff(w);
    for (int i = 0; i < w; ++i) diff[i] = outB[at + i] - outA[at + i];
    double snareLeak100 = toneMag(diff.data(), w, sr, 100.0);

    std::printf("kick 100Hz, kick ALONE           = %.5f  (natural fade baseline)\n", kickAlone);
    std::printf("kick 100Hz, kick + snare         = %.5f\n", kickWith);
    std::printf("kick 100Hz, kick + snare + FX    = %.5f  <-- FX-param automation on the snare block\n", kickWithFx);
    std::printf("snare's own 100Hz leak (B - A)   = %.5f  (transient leakage, discount this)\n", snareLeak100);

    auto isRetrig = [&](double kw){ return kw > kickAlone * 1.15 && (kw - kickAlone) > snareLeak100 * 1.5 + 1e-3; };
    bool retrigger = isRetrig(kickWith) || isRetrig(kickWithFx);
    std::printf("  plain snare retrigger?  %s\n", isRetrig(kickWith)   ? "YES" : "no");
    std::printf("  snare+FX  retrigger?    %s\n", isRetrig(kickWithFx) ? "YES" : "no");
    std::printf("\n%s\n", retrigger
        ? "BUG REPRODUCED: firing the snare RE-TRIGGERED the kick (kick low-band came back)."
        : "OK: snare did not retrigger the kick.");

    inst->release(); delete inst;
    return retrigger ? 1 : 0;
}
