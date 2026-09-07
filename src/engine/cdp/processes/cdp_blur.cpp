//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_blur.cpp
//
//  Ported from the CDP BLUR family (vendor/cdp8/dev/blur), which is
//      Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  These are the first SPECTRAL ports.  CDP's spectral programs never touch
//  samples: they read an analysis file of amplitude/frequency pairs produced by
//  its `pvoc` program, and walk it as
//
//      for (vc = 0; vc < dz->wanted; vc += 2)   // AMPP at vc, frequency at vc+1
//
//  cdp_pvoc.h reproduces exactly that data (Frame::amp / Frame::freq), so the
//  loops below stay close to the originals; what has been dropped is the
//  analysis-FILE plumbing -- headers, disk buffering, the pvxio2 layer, the
//  window-at-a-time read/write ping-pong -- not the arithmetic.
//
//  NONE OF THESE ARE ZERO LATENCY, and they say so.  A phase vocoder cannot
//  emit anything until it has filled an analysis window, so each reports
//  pvoc_latency() (one FFT frame).  blur.blur additionally needs its whole
//  averaging group before it can emit, and averaging across the file is not a
//  streaming operation at all -- so it is honestly marked streamable=false and
//  lives in the offline editor, which is where CDP's own blur lives too.
//----------------------------------------------------------------------------
#include "../cdp_pvoc.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

//! Shared analysis settings, driven by an "FFT size" parameter so a user can
//! trade frequency resolution against time smearing -- the single most audible
//! choice in any spectral process.
PvocSpec spec_from(const std::vector<double>& p, size_t idx, int sr) {
    PvocSpec s;
    const double v = idx < p.size() ? p[idx] : 1024.0;
    int n = (int)std::lround(v);
    if (n < 128) n = 128;
    if (n > 8192) n = 8192;
    int pow2 = 128; while (pow2 < n) pow2 <<= 1;
    s.fftSize = pow2;
    s.overlap = 4;
    s.sampleRate = sr > 0 ? sr : 48000;
    return s;
}

//! Deterministic local RNG: a render must be reproducible from its seed, and a
//! global or time-seeded generator would make two renders of the same patch
//! differ.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9e3779b9u) {}
    float next() {                       // 0..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s & 0xffffffu) / (float)0x1000000;
    }
    float bi() { return next() * 2.f - 1.f; }
};

//----------------------------------------------------------------------------
//  BLUR -- average each group of N analysis windows (blur.c).
//  The signature CDP blur: time detail is smeared because a run of windows is
//  replaced by their mean, so transients spread across the group.
//----------------------------------------------------------------------------
bool blur_blur(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int group = std::max(1, (int)std::lround(p.empty() ? 4.0 : p[0]));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);
    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        if (fr.empty()) return;
        const int B = fr[0].bins();
        for (size_t g = 0; g < fr.size(); g += (size_t)group) {
            const size_t last = std::min(g + (size_t)group, fr.size());
            const double n = (double)(last - g);
            for (int b = 0; b < B; ++b) {
                double a = 0.0, f = 0.0;
                for (size_t i = g; i < last; ++i) {
                    a += fr[i].amp[(size_t)b];
                    f += fr[i].freq[(size_t)b];
                }
                const float am = (float)(a / n), fm = (float)(f / n);
                for (size_t i = g; i < last; ++i) {
                    fr[i].amp[(size_t)b] = am;
                    fr[i].freq[(size_t)b] = fm;
                }
            }
        }
        // Drop the analysis phases: they describe the spectrum these frames USED
        // to hold, and locking the averaged spectrum to them re-sharpens exactly
        // the transients blur exists to smear.  Free integration is what smearing
        // wants here.
        for (Frame& f : fr) f.phase.clear();
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  SUPPRESS -- keep only the N loudest bins in each window, zero the rest
//  (blur.c: delete_unwanted_bloks, which walks the window zeroing AMPP).
//  Thins a dense spectrum down to its strongest partials.
//----------------------------------------------------------------------------
bool blur_suppress(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int keep = std::max(1, (int)std::lround(p.empty() ? 8.0 : p[0]));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);
    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        std::vector<int> idx;
        for (Frame& f : fr) {
            const int B = f.bins();
            if (keep >= B) continue;
            idx.resize((size_t)B);
            std::iota(idx.begin(), idx.end(), 0);
            std::partial_sort(idx.begin(), idx.begin() + keep, idx.end(),
                [&](int a, int b) { return f.amp[(size_t)a] > f.amp[(size_t)b]; });
            std::vector<char> survive((size_t)B, 0);
            for (int i = 0; i < keep; ++i) survive[(size_t)idx[(size_t)i]] = 1;
            for (int b = 0; b < B; ++b) if (!survive[(size_t)b]) f.amp[(size_t)b] = 0.f;
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  CHORUS -- randomise each bin's amplitude and frequency a little, every
//  window (blur.c: do_a / do_fvar and friends).  The partials wander, so a
//  steady tone becomes a shimmering ensemble of itself.
//----------------------------------------------------------------------------
bool blur_chorus(const std::vector<Buffer>& in, const std::vector<double>& p,
                 Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };
    const double aSpread = std::max(0.0, std::min(1.0, at(0, 0.4)));
    const double fSpread = std::max(0.0, std::min(1.0, at(1, 0.1)));
    const PvocSpec s = spec_from(p, 2, in[0].sampleRate);
    const uint32_t seed = (uint32_t)std::lround(at(3, 1.0));
    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        Rng rng(seed);
        for (Frame& f : fr) {
            const int B = f.bins();
            for (int b = 0; b < B; ++b) {
                const double a = 1.0 + aSpread * (double)rng.bi();
                const double g = 1.0 + fSpread * (double)rng.bi();
                f.amp[(size_t)b]  = (float)std::max(0.0, (double)f.amp[(size_t)b] * a);
                f.freq[(size_t)b] = (float)((double)f.freq[(size_t)b] * g);
            }
            // Detuning each bin independently IS the effect; phase locking would
            // hand every non-peak bin its peak's phase and quietly discard most
            // of the detuning.
            f.phase.clear();
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  AVERAGE -- smooth each window ACROSS its bins (spectral, not temporal).
//  Where blur smears in time, this smears in frequency: neighbouring partials
//  are averaged together, softening the spectral contour.
//----------------------------------------------------------------------------
bool blur_average(const std::vector<Buffer>& in, const std::vector<double>& p,
                  Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int span = std::max(1, (int)std::lround(p.empty() ? 4.0 : p[0]));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);
    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        std::vector<float> tmp;
        for (Frame& f : fr) {
            const int B = f.bins();
            tmp.assign((size_t)B, 0.f);
            for (int b = 0; b < B; ++b) {
                const int lo = std::max(0, b - span), hi = std::min(B - 1, b + span);
                double a = 0.0;
                for (int k = lo; k <= hi; ++k) a += f.amp[(size_t)k];
                tmp[(size_t)b] = (float)(a / (double)(hi - lo + 1));
            }
            f.amp.swap(tmp);
            // Smoothing across bins flattens the main lobes themselves, so the
            // per-lobe phase offsets no longer describe anything.
            f.phase.clear();
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//! Every spectral process carries one analysis window of delay.
int64_t spectral_latency(int sr, const std::vector<double>& p, size_t fftIdx) {
    return pvoc_latency(spec_from(p, fftIdx, sr));
}

ParamSpec fftParam() {
    return { "FFT size", "", 128, 8192, 1024, true,
             "Analysis window; larger = finer pitch, more time smearing." };
}

} // namespace

void register_blur_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug = "blur.blur";  p.name = "Blur";  p.group = "Spectral";
        p.help = "Average groups of analysis windows, smearing time detail.";
        p.streamable = false;      // averaging a group needs the whole group
        p.latencyFrames = [](int sr, const std::vector<double>& pr) {
            return spectral_latency(sr, pr, 1); };
        p.params = { { "Windows", "", 1, 200, 4, true,
                       "How many analysis windows are averaged together." },
                     fftParam() };
        p.run = &blur_blur;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "blur.suppress";  p.name = "Suppress";  p.group = "Spectral";
        p.help = "Keep only the loudest partials in each window.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) {
            return spectral_latency(sr, pr, 1); };
        p.params = { { "Keep", "", 1, 512, 8, true,
                       "How many of the loudest bins survive." },
                     fftParam() };
        p.run = &blur_suppress;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "blur.chorus";  p.name = "Spectral Chorus";  p.group = "Spectral";
        p.help = "Randomise partial amplitudes and frequencies into an ensemble.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) {
            return spectral_latency(sr, pr, 2); };
        p.params = { { "Amp spread", "", 0.0, 1.0, 0.4, false,
                       "How far partial loudness wanders." },
                     { "Freq spread", "", 0.0, 1.0, 0.1, false,
                       "How far partial pitch wanders." },
                     fftParam(),
                     { "Seed", "", 1, 99999, 1, true,
                       "Same seed gives the same render." } };
        p.run = &blur_chorus;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "blur.average";  p.name = "Spectral Average";  p.group = "Spectral";
        p.help = "Smooth each window across its bins, softening the spectrum.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) {
            return spectral_latency(sr, pr, 1); };
        p.params = { { "Span", "bins", 1, 64, 4, true,
                       "How many neighbouring bins are averaged." },
                     fftParam() };
        p.run = &blur_average;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
