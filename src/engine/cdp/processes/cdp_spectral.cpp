//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_spectral.cpp
//
//  Ported from the CDP STRETCH / REPITCH / MORPH families
//  (vendor/cdp8/dev/stretch, dev/repitch, dev/morph), which are
//      Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  All three are spectral and share cdp_pvoc.h's analysis (amplitude and true
//  frequency per bin -- CDP's own AMPP/frequency pair layout).  What each one
//  actually is, once the analysis-file plumbing is removed:
//
//    TIME STRETCH  resample the FRAME SEQUENCE.  The phase vocoder's whole
//                  point: reading frames slower spreads the sound in time
//                  without moving its partials, because each frame carries true
//                  frequency rather than raw phase.  This is the one process
//                  here that changes output length, so it synthesises from its
//                  own frame count (keepLength=false) rather than the input's.
//
//    TRANSPOSE     move each partial's FREQUENCY, and move its amplitude to the
//                  bin that frequency now belongs in.  Doing only the first
//                  (the obvious mistake) shifts the reported pitch while
//                  leaving the energy in the original bins, which resynthesises
//                  as the original pitch with a phase mess on top.
//
//    MORPH         interpolate amplitude and frequency between TWO analyses.
//                  This is the first spectral process taking two sources, so it
//                  is what the editor's drag-several-clips-in flow exists for.
//                  CDP morphs amplitude and frequency on separate curves; that
//                  is kept, as it is what lets one sound take another's spectral
//                  shape while holding its own pitch, or vice versa.
//
//  NONE ARE ZERO LATENCY.  Each reports one analysis window through
//  pvoc_latency() and is marked streamable=false: a phase vocoder must fill a
//  window before emitting, and stretching/morphing needs material from across
//  the file besides.
//----------------------------------------------------------------------------
#include "../cdp_pvoc.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

PvocSpec spec_from(const std::vector<double>& p, size_t idx, int sr) {
    PvocSpec s;
    int n = (int)std::lround(idx < p.size() ? p[idx] : 1024.0);
    if (n < 128) n = 128;
    if (n > 8192) n = 8192;
    int pow2 = 128; while (pow2 < n) pow2 <<= 1;
    s.fftSize = pow2; s.overlap = 4;
    s.sampleRate = sr > 0 ? sr : 48000;
    return s;
}
ParamSpec fftParam() {
    return { "FFT size", "", 128, 8192, 1024, true,
             "Analysis window; larger = finer pitch, more time smearing." };
}
int64_t lat(int sr, const std::vector<double>& p, size_t i) {
    return pvoc_latency(spec_from(p, i, sr));
}

//----------------------------------------------------------------------------
//  TIME STRETCH
//----------------------------------------------------------------------------
bool stretch_time(const std::vector<Buffer>& in, const std::vector<double>& p,
                  Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    double ratio = p.empty() ? 2.0 : p[0];
    if (!(ratio > 0.0) || !std::isfinite(ratio)) ratio = 1.0;
    ratio = std::max(0.05, std::min(20.0, ratio));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);

    // Length and head are set explicitly rather than derived from the frame
    // count: resampling the frame sequence stretches the analysis PADDING too,
    // so the output would otherwise run long by one stretched window and open
    // with that much stretched silence.
    const int64_t wantLen  = (int64_t)std::llround((double)in[0].frames() * ratio);
    const int64_t wantSkip = (int64_t)std::llround((double)s.fftSize * ratio);

    out.sampleRate = s.sampleRate;
    out.ch.assign((size_t)in[0].channels(), {});
    size_t longest = 0;
    for (int c = 0; c < in[0].channels(); ++c) {
        std::vector<Frame> fr = pvoc_analyse(in[0].ch[(size_t)c], s);
        if (fr.size() >= 2) {
            const size_t nOut = (size_t)std::max<double>(1.0, (double)fr.size() * ratio);
            std::vector<Frame> dst;
            dst.reserve(nOut);
            const int B = fr[0].bins();
            for (size_t j = 0; j < nOut; ++j) {
                const double srcPos = (double)j / ratio;
                const size_t i0 = (size_t)std::min<double>((double)fr.size() - 1,
                                                           std::floor(srcPos));
                const size_t i1 = std::min(fr.size() - 1, i0 + 1);
                const double t = srcPos - (double)i0;
                Frame f;
                f.amp.resize((size_t)B); f.freq.resize((size_t)B);
                for (int b = 0; b < B; ++b) {
                    f.amp[(size_t)b]  = (float)((1.0 - t) * fr[i0].amp[(size_t)b]
                                                      + t * fr[i1].amp[(size_t)b]);
                    f.freq[(size_t)b] = (float)((1.0 - t) * fr[i0].freq[(size_t)b]
                                                      + t * fr[i1].freq[(size_t)b]);
                }
                // Phase is NOT interpolated -- an angle halfway between two
                // angles is meaningless.  The nearer source frame's phases are
                // taken whole, which is all the lock needs: it uses only the
                // OFFSETS within a main lobe, and those are coherent in any one
                // analysis frame.
                f.phase = fr[t < 0.5 ? i0 : i1].phase;
                dst.push_back(std::move(f));
            }
            fr.swap(dst);
        }
        out.ch[(size_t)c] = pvoc_synthesise(fr, s, wantLen, wantSkip);
        longest = std::max(longest, out.ch[(size_t)c].size());
        if (prog && !prog((double)(c + 1) / (double)in[0].channels())) {
            err = "cancelled"; return false;
        }
    }
    for (auto& c : out.ch) c.resize(longest, 0.f);
    if (longest == 0) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  TRANSPOSE
//----------------------------------------------------------------------------
bool spec_transpose(const std::vector<Buffer>& in, const std::vector<double>& p,
                    Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const double semis = p.empty() ? 0.0 : std::max(-48.0, std::min(48.0, p[0]));
    const double ratio = std::pow(2.0, semis / 12.0);
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);

    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        if (fr.empty()) return;
        const int B = fr[0].bins();
        std::vector<float> a((size_t)B), f((size_t)B), ph((size_t)B);
        for (Frame& fm : fr) {
            std::fill(a.begin(), a.end(), 0.f);
            std::fill(f.begin(), f.end(), 0.f);
            std::fill(ph.begin(), ph.end(), 0.f);
            const bool havePhase = (int)fm.phase.size() >= B;
            for (int b = 0; b < B; ++b) {
                const double nf = (double)fm.freq[(size_t)b] * ratio;
                // The amplitude must travel WITH its partial, into the bin the
                // new frequency belongs to.
                const int nb = (int)std::lround((double)b * ratio);
                if (nb < 0 || nb >= B) continue;              // shifted out of band
                if (fm.amp[(size_t)b] > a[(size_t)nb]) {      // loudest wins the bin
                    a[(size_t)nb] = fm.amp[(size_t)b];
                    f[(size_t)nb] = (float)nf;
                    // Phase rides along, or the lock would pair each bin with a
                    // main-lobe offset that no longer belongs to it.
                    if (havePhase) ph[(size_t)nb] = fm.phase[(size_t)b];
                }
            }
            fm.amp = a; fm.freq = f;
            if (havePhase) fm.phase = ph;
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  MORPH -- two sources
//----------------------------------------------------------------------------
bool spec_morph(const std::vector<Buffer>& in, const std::vector<double>& p,
                Buffer& out, std::string& err, const Progress& prog) {
    if (in.size() < 2 || in[0].empty() || in[1].empty()) {
        err = "morph needs two sources"; return false;
    }
    if (in[0].sampleRate != in[1].sampleRate) {
        err = "sources must share a sample rate"; return false;
    }
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };
    const double aStart = std::max(0.0, std::min(1.0, at(0, 0.0)));
    const double aEnd   = std::max(0.0, std::min(1.0, at(1, 1.0)));
    const double fStart = std::max(0.0, std::min(1.0, at(2, 0.0)));
    const double fEnd   = std::max(0.0, std::min(1.0, at(3, 1.0)));
    const PvocSpec s = spec_from(p, 4, in[0].sampleRate);

    // Analyse the SECOND source once; slot 0 is the one being morphed FROM.
    const int chans = std::max(in[0].channels(), in[1].channels());
    out.sampleRate = s.sampleRate;
    out.ch.assign((size_t)chans, {});
    size_t longest = 0;
    for (int c = 0; c < chans; ++c) {
        const std::vector<float>& x0 = in[0].ch[(size_t)std::min(c, in[0].channels() - 1)];
        const std::vector<float>& x1 = in[1].ch[(size_t)std::min(c, in[1].channels() - 1)];
        std::vector<Frame> f0 = pvoc_analyse(x0, s);
        std::vector<Frame> f1 = pvoc_analyse(x1, s);
        if (f0.empty()) continue;
        const int B = f0[0].bins();
        for (size_t i = 0; i < f0.size(); ++i) {
            // Interpolation runs over the FIRST source's duration; past the end
            // of the second, its last frame is held rather than fading to
            // silence, which would put a hole in the morph.
            const double t = f0.size() > 1 ? (double)i / (double)(f0.size() - 1) : 0.0;
            const double ka = aStart + (aEnd - aStart) * t;
            const double kf = fStart + (fEnd - fStart) * t;
            const Frame& g = f1[std::min(i, f1.size() - 1)];
            const int nb = std::min(B, g.bins());
            for (int b = 0; b < nb; ++b) {
                f0[i].amp[(size_t)b]  = (float)((1.0 - ka) * f0[i].amp[(size_t)b]
                                                      + ka * g.amp[(size_t)b]);
                f0[i].freq[(size_t)b] = (float)((1.0 - kf) * f0[i].freq[(size_t)b]
                                                      + kf * g.freq[(size_t)b]);
            }
            // Phase belongs to whichever source is currently dominant: past
            // halfway the spectrum is mostly source 2's, and pairing it with
            // source 1's main-lobe offsets would smear the very partials the
            // morph just handed over.
            if (ka > 0.5 && (int)g.phase.size() >= nb) f0[i].phase = g.phase;
        }
        out.ch[(size_t)c] = pvoc_synthesise(f0, s, (int64_t)x0.size());
        longest = std::max(longest, out.ch[(size_t)c].size());
        if (prog && !prog((double)(c + 1) / (double)chans)) { err = "cancelled"; return false; }
    }
    for (auto& c : out.ch) c.resize(longest, 0.f);
    return longest > 0;
}

} // namespace

void register_spectral_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug = "stretch.time"; p.name = "Time Stretch"; p.group = "Spectral";
        p.help = "Stretch or shrink in time without moving the pitch.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 1); };
        p.params = { { "Ratio", "x", 0.05, 20.0, 2.0, false,
                       "Above 1 lengthens, below 1 shortens." },
                     fftParam() };
        p.run = &stretch_time;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "pitch.transpose"; p.name = "Transpose"; p.group = "Spectral";
        p.help = "Shift the pitch without changing the duration.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 1); };
        p.params = { { "Semitones", "st", -48.0, 48.0, 0.0, false,
                       "How far to transpose." },
                     fftParam() };
        p.run = &spec_transpose;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "morph.spectral"; p.name = "Spectral Morph"; p.group = "Spectral";
        p.help = "Interpolate between two sounds' spectra over time.";
        p.minInputs = 2; p.maxInputs = 2;      // slot 0 = from, slot 1 = to
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 4); };
        p.params = { { "Amp start", "", 0.0, 1.0, 0.0, false,
                       "How much of source 2's loudness at the start." },
                     { "Amp end", "", 0.0, 1.0, 1.0, false,
                       "...and at the end." },
                     { "Freq start", "", 0.0, 1.0, 0.0, false,
                       "How much of source 2's pitch at the start." },
                     { "Freq end", "", 0.0, 1.0, 1.0, false,
                       "...and at the end." },
                     fftParam() };
        p.run = &spec_morph;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
