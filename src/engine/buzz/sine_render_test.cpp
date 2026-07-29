//----------------------------------------------------------------------------
//  sine_render_test.cpp -- render a pure sine through the Sampler at several
//  pitches and measure: (a) output frequency vs. expected (sample-rate-
//  conversion correctness + octave tracking), (b) zipper/aliasing via the RMS
//  residual against an ideal sine fitted at the measured frequency, (c) the
//  note-on transient (declicker).  Source is 44.1 kHz, engine is 48 kHz, so the
//  resampler must correct the SR ratio AND the per-note pitch.
//----------------------------------------------------------------------------
#include "sampler_instrument.h"
#include "../plugin_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace PatchKnob::engine;

static const double PI = 3.14159265358979323846;

// Measure fundamental frequency by sub-sample-interpolated upward zero crossings.
static double measure_freq(const float* x, int n, double sr) {
    std::vector<double> cross;
    for (int i = 1; i < n; ++i) {
        if (x[i - 1] <= 0.f && x[i] > 0.f) {
            double frac = (double)(0.f - x[i - 1]) / (double)(x[i] - x[i - 1]);
            cross.push_back((i - 1) + frac);
        }
    }
    if (cross.size() < 3) return 0.0;
    double periods = cross.size() - 1;
    double span    = cross.back() - cross.front();
    return periods * sr / span;
}

// THD+N-ish: fit the single best sine at frequency f (least-squares amp+phase),
// return RMS(residual)/RMS(signal).  Captures harmonics (aliasing/zipper) + noise.
static double residual_ratio(const float* x, int n, double f, double sr, double& sigRms) {
    double w = 2.0 * PI * f / sr, a = 0, b = 0, c2 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        double c = std::cos(w * i), s = std::sin(w * i);
        a += x[i] * c; b += x[i] * s; c2 += c * c; s2 += s * s;
    }
    a = (c2 > 0 ? a / c2 : 0); b = (s2 > 0 ? b / s2 : 0);
    double res = 0, sig = 0;
    for (int i = 0; i < n; ++i) {
        double fit = a * std::cos(w * i) + b * std::sin(w * i);
        double r = x[i] - fit;
        res += r * r; sig += (double)x[i] * x[i];
    }
    sigRms = std::sqrt(sig / n);
    if (sig < 1e-20) return 0.0;
    return std::sqrt(res / (sig > 0 ? sig : 1));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const double engineSR = 48000.0;
    const int    srcSR    = 44100;
    const double F0       = 441.0;          // 100 samples/cycle @44100 -> 441 cycles in 1s
    const int    rootMidi = 60;             // sample's native pitch assigned to C4
    const int    block    = 512;

    // 1s mono sine @44100 -> EXACTLY 441 whole cycles, so [0,44100) loops seamlessly.
    std::vector<float> src(srcSR);
    for (int i = 0; i < srcSR; ++i) src[i] = 0.5f * (float)std::sin(2.0 * PI * F0 * i / srcSR);

    std::printf("source: %.1f Hz sine @ %d Hz, root MIDI %d, looped | engine %.0f Hz\n",
                F0, srcSR, rootMidi, engineSR);
    std::printf("SR-conversion check: at root the 44.1k sample must still sound at %.1f Hz on the 48k engine.\n\n", F0);
    std::printf("%-6s %-10s %-10s %-9s %-9s %-10s %-9s\n",
                "note", "expect Hz", "measry Hz", "err cent", "peak", "zipper%", "onsetΔ");
    std::printf("--------------------------------------------------------------------------\n");

    const int notes[] = { 36, 48, 55, 60, 67, 72, 79, 84 };
    int fails = 0;

    for (int note : notes) {
        // Fresh instrument per note so voices never stack across pitches (and this
        // also exercises machine create/destroy 8x -> a regression check on the
        // heap fixes).  A held looped note rings forever otherwise.
        IPluginInstance* inst = create_sampler_instrument();
        if (!inst) { std::printf("FAIL: no sampler for note %d\n", note); ++fails; continue; }
        inst->prepare(engineSR, block);
        inst->setActive(true);
        sampler_load_sample(inst, 1, 0, src.data(), srcSR, /*stereo*/false,
                            rootMidi, srcSR, /*loopStart*/0, /*loopEnd*/srcSR, /*loop*/true,
                            /*loKey*/0, /*hiKey*/127, "sine441");

        // render ~0.35 s, note-on at the very start
        const int total = (int)(0.35 * engineSR);
        std::vector<float> outL(total, 0.f), outR(total, 0.f);
        std::vector<float> bl(block), br(block);
        int pos = 0;
        bool first = true;
        while (pos < total) {
            int nf = std::min(block, total - pos);
            float* outs[2] = { bl.data(), br.data() };
            MidiEvent ev{0, 0x90, (uint8_t)note, 100};
            ProcessBlock pb{};
            pb.audioOut = outs; pb.nframes = nf;
            pb.midiIn = first ? &ev : nullptr; pb.numMidiIn = first ? 1 : 0;
            pb.numAudioOut = 2; pb.numAudioIn = 0;
            for (int i = 0; i < block; ++i) { bl[i] = 0; br[i] = 0; }
            inst->process(pb);
            for (int i = 0; i < nf; ++i) { outL[pos + i] = bl[i]; outR[pos + i] = br[i]; }
            pos += nf; first = false;
        }

        // steady-state window 30..180 ms (within the shortest playback before the
        // one-shot would end; loop keeps it sustained anyway)
        int w0 = (int)(0.030 * engineSR), w1 = (int)(0.180 * engineSR);
        if (w1 > total) w1 = total;
        const float* win = outL.data() + w0;
        int wn = w1 - w0;

        double sigRms = 0;
        double fmeas  = measure_freq(win, wn, engineSR);
        double zip    = residual_ratio(win, wn, fmeas, engineSR, sigRms) * 100.0;
        double fexp   = F0 * std::pow(2.0, (note - rootMidi) / 12.0);
        double cents  = fmeas > 0 ? 1200.0 * std::log2(fmeas / fexp) : -9999;

        double peak = 0;
        for (int i = w0; i < w1; ++i) peak = std::max(peak, (double)std::fabs(outL[i]));

        // onset transient: max |x[n]-x[n-1]| in the first 8 ms vs the steady max delta
        int on = (int)(0.008 * engineSR);
        double onMax = 0, stMax = 0;
        for (int i = 1; i < on && i < total; ++i) onMax = std::max(onMax, (double)std::fabs(outL[i] - outL[i - 1]));
        for (int i = w0 + 1; i < w1; ++i) stMax = std::max(stMax, (double)std::fabs(outL[i] - outL[i - 1]));
        double onsetRatio = stMax > 1e-9 ? onMax / stMax : 0;

        std::printf("%-6d %-10.1f %-10.1f %-+9.1f %-9.4f %-10.3f %-9.2f\n",
                    note, fexp, fmeas, cents, peak, zip, onsetRatio);

        if (std::fabs(cents) > 15.0) { std::printf("   ^ FAIL: pitch off by >15 cents\n"); ++fails; }
        if (zip > 2.0)               { std::printf("   ^ WARN: zipper/aliasing residual > 2%%\n"); }
        if (onsetRatio > 3.0)        { std::printf("   ^ WARN: onset transient (click) > 3x steady\n"); }

        // note-off + drain, then destroy this voice's machine
        {
            float* outs[2] = { bl.data(), br.data() };
            MidiEvent off{0, 0x80, (uint8_t)note, 0};
            ProcessBlock pb{};
            pb.audioOut = outs; pb.nframes = block;
            pb.midiIn = &off; pb.numMidiIn = 1; pb.numAudioOut = 2; pb.numAudioIn = 0;
            inst->process(pb);
        }
        inst->release();
        delete inst;
    }

    std::printf("\ncent err ~0 across octaves => pitch tracks correctly; err at root ~0 => SR conversion correct.\n");
    std::printf("zipper%% = RMS of everything that isn't the fundamental (aliasing/quantization/zipper).\n");
    std::printf(fails ? "\nSINE RENDER TEST: %d pitch FAILURES\n" : "\nSINE RENDER TEST: pitch OK\n", fails);
    return fails ? 1 : 0;
}
