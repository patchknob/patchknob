//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_focus.cpp
//
//  Ported from the CDP FOCUS family (vendor/cdp8/dev/focus/focus.c), which is
//      Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Four spectral processes that all work by DECIDING WHICH WINDOW A BIN COMES
//  FROM, rather than by filtering:
//
//    ACCUMULATE  specaccu() + rwd_accumulate().  A running per-bin MAXIMUM: a
//                bin keeps the loudest value it has ever held, so a sound piles
//                up into a sustained chord of everything that has passed.  Decay
//                bleeds the accumulator down; glissando slides its frequencies.
//
//    EXAGGERATE  specexag().  Normalise the window to its own peak, raise every
//                bin to a power, then scale the whole window back to its
//                original total amplitude.  Above 1 the spectral contour is
//                deepened (quiet partials pushed further down); below 1 it is
//                flattened.  The re-normalisation is the point -- without it
//                this would just be a volume change.
//
//    STEP        do_specstep().  Hold the window at the start of every group of
//                N and repeat it through the group: a spectral sample-and-hold,
//                which stutters the sound onto a grid.
//
//    FREEZE      Hold ONE window from a chosen moment for the rest of the file.
//
//  CDP's own per-second-to-per-window conversions are kept verbatim: decay is
//  decay^frametime and glissando is 2^(octaves_per_sec * frametime), where
//  frametime is hop/samplerate.  Getting these wrong makes every parameter
//  scale with the FFT size, which is exactly what a user does not expect.
//
//  Not zero latency -- one analysis window, like every spectral process here.
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
double at(const std::vector<double>& p, size_t i, double d) {
    return i < p.size() ? p[i] : d;
}
//! frametime -- seconds per analysis window.  CDP's dz->frametime.
double frametime(const PvocSpec& s) {
    return (double)s.hop() / (double)(s.sampleRate > 0 ? s.sampleRate : 48000);
}

//! tklib3.c normalise(): put a rewritten window back to the total amplitude it
//! had before, so a spectral edit does not double as a level change.  CDP leaves
//! the window untouched when it has collapsed to nothing.
void renormalise(Frame& f, double pre, double post) {
    if (post < 1e-20 || pre <= 0.0) return;
    const float g = (float)(pre / post);
    for (float& a : f.amp) a *= g;
}

//----------------------------------------------------------------------------
//  ACCUMULATE
//----------------------------------------------------------------------------
bool focus_accumulate(const std::vector<Buffer>& in, const std::vector<double>& p,
                      Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const PvocSpec s = spec_from(p, 2, in[0].sampleRate);
    const double ft    = frametime(s);
    const double decay = std::max(0.0, std::min(1.0, at(p, 0, 1.0)));
    const double glis  = at(p, 1, 0.0);
    // CDP's conversions: a per-SECOND decay factor and a glissando in octaves
    // per second, both reduced to one window's worth.
    const double dIndex = decay < 1.0 ? std::exp(std::log(std::max(1e-12, decay)) * ft) : 1.0;
    const double gIndex = std::pow(2.0, glis * ft);

    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        if (fr.empty()) return;
        const int B = fr[0].bins();
        std::vector<float> accA((size_t)B, 0.f), accF((size_t)B, 0.f);
        for (Frame& f : fr) {
            for (int b = 0; b < B; ++b) {
                if (decay < 1.0) accA[(size_t)b] = (float)(accA[(size_t)b] * dIndex);
                if (glis != 0.0) accF[(size_t)b] = (float)(accF[(size_t)b] * gIndex);
                // rwd_accumulate(): whichever is louder wins, and its FREQUENCY
                // travels with it -- accumulating amplitude alone would leave
                // the held partial sitting on whatever pitch happened to arrive.
                if (f.amp[(size_t)b] > accA[(size_t)b]) {
                    accA[(size_t)b] = f.amp[(size_t)b];
                    accF[(size_t)b] = f.freq[(size_t)b];
                } else {
                    f.amp[(size_t)b]  = accA[(size_t)b];
                    f.freq[(size_t)b] = accF[(size_t)b];
                }
            }
            // A held bin's amplitude no longer belongs to the lobe its analysis
            // phase describes, so the frames are handed back unlocked.
            f.phase.clear();
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  EXAGGERATE
//----------------------------------------------------------------------------
bool focus_exaggerate(const std::vector<Buffer>& in, const std::vector<double>& p,
                      Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const double ex = std::max(0.01, std::min(10.0, at(p, 0, 2.0)));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);

    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        for (Frame& f : fr) {
            const int B = f.bins();
            double pre = 0.0, maxamp = 0.0;
            for (int b = 0; b < B; ++b) {
                pre += (double)f.amp[(size_t)b];
                maxamp = std::max(maxamp, (double)f.amp[(size_t)b]);
            }
            if (maxamp <= 0.0) continue;        // CDP's zero_set: leave it alone
            const double norm = 1.0 / maxamp;
            double post = 0.0;
            for (int b = 0; b < B; ++b) {
                const double v = std::pow((double)f.amp[(size_t)b] * norm, ex);
                f.amp[(size_t)b] = (float)v;
                post += v;
            }
            renormalise(f, pre, post);
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  STEP  --  spectral sample-and-hold
//----------------------------------------------------------------------------
bool focus_step(const std::vector<Buffer>& in, const std::vector<double>& p,
                Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int step = std::max(1, (int)std::lround(at(p, 0, 8.0)));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);

    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        // The held window is copied WHOLE, phases included, so the repeated
        // frames stay a coherent spectrum rather than a re-randomised one.
        for (size_t i = 0; i < fr.size(); ++i) {
            const size_t hold = (i / (size_t)step) * (size_t)step;
            if (i != hold) fr[i] = fr[hold];
        }
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

//----------------------------------------------------------------------------
//  FREEZE
//----------------------------------------------------------------------------
bool focus_freeze(const std::vector<Buffer>& in, const std::vector<double>& p,
                  Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const double when = std::max(0.0, at(p, 0, 0.5));
    const PvocSpec s = spec_from(p, 1, in[0].sampleRate);
    const double ft = frametime(s);

    const bool ok = pvoc_process(in[0], out, s, [&](std::vector<Frame>& fr) {
        if (fr.empty()) return;
        size_t at_ = (size_t)std::lround(when / std::max(1e-9, ft));
        if (at_ >= fr.size()) at_ = fr.size() - 1;
        for (size_t i = at_ + 1; i < fr.size(); ++i) fr[i] = fr[at_];
    }, true);
    if (prog) prog(1.0);
    if (!ok) { err = "analysis failed"; return false; }
    return true;
}

} // namespace

void register_focus_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug = "focus.accumulate"; p.name = "Accumulate"; p.group = "Spectral";
        p.help = "Pile the sound up: every bin keeps the loudest value it has held.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 2); };
        p.params = { { "Decay", "/s", 0.0, 1.0, 1.0, false,
                       "Per second. 1 holds forever, lower bleeds the pile away." },
                     { "Glissando", "8va/s", -4.0, 4.0, 0.0, false,
                       "Octaves per second the held partials slide." },
                     fftParam() };
        p.run = &focus_accumulate;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "focus.exaggerate"; p.name = "Exaggerate"; p.group = "Spectral";
        p.help = "Deepen or flatten the spectral contour at constant loudness.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 1); };
        p.params = { { "Exaggerate", "", 0.01, 10.0, 2.0, false,
                       "Above 1 deepens the contour, below 1 flattens it." },
                     fftParam() };
        p.run = &focus_exaggerate;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "focus.step"; p.name = "Spectral Step"; p.group = "Spectral";
        p.help = "Sample-and-hold the spectrum, stuttering it onto a grid.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 1); };
        p.params = { { "Step", "windows", 1, 200, 8, true,
                       "How many analysis windows each held spectrum lasts." },
                     fftParam() };
        p.run = &focus_step;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "focus.freeze"; p.name = "Spectral Freeze"; p.group = "Spectral";
        p.help = "Hold the spectrum from one moment for the rest of the sound.";
        p.streamable = false;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) { return lat(sr, pr, 1); };
        p.params = { { "Freeze at", "s", 0.0, 3600.0, 0.5, false,
                       "The moment whose spectrum is held." },
                     fftParam() };
        p.run = &focus_freeze;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
