//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_grain.cpp
//
//  Ported from the CDP GRAIN family (vendor/cdp8/dev/grain), which is
//      Copyright (c) 1983-2020 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Files ported here:
//      grain/grain.c   process_grains(), grains()   -- the gate/hole grain
//                      detector that every program in the family is built on
//      grain/grain1.c  do_the_grain(), retime_grain(), keep_non_omitted_grains(),
//                      duplicate_grain(), do_the_reversing_process(),
//                      store_up_halfsplice()/store_dn_halfsplice()/
//                      output_grain_link()  -- the grain writer and its splices
//      grain/ap_grain.c, include/graicon.h  -- gate and splice defaults
//
//  WHAT A "GRAIN" IS HERE.  CDP does not window the sound at a fixed rate; it
//  FINDS grains in it.  Scanning frame by frame, a sample outside +/-gate is
//  "in a grain"; a run of at least `minhole` frames inside the gate is a hole.
//  When signal returns after a long enough hole, the previous grain ends and a
//  new one begins.  A grain therefore spans from one onset to the NEXT onset --
//  the sound plus the silence that follows it, which is what makes reordering
//  and duplicating them rhythmically sensible.  That is grains() exactly.
//
//  THE SPLICE.  Following retime_grain(), a grain is written as the source span
//      [onset - h, next_onset - h)      h = half the splice
//  with a linear fade up over the first h frames and down over the last h.  That
//  is precisely what CDP's stored up- and down-halfsplices amount to: the fade
//  lands in the inter-grain hole where there is little signal, so joins are
//  inaudible and no grain boundary can click.  Grains are butt-joined; they do
//  not overlap (CDP writes, it does not add, in this path).
//
//  WHAT WAS DROPPED.  CDP scans through a double sample buffer and every one of
//  these functions carries a `crosbuf` case for a grain that straddles the
//  buffer boundary, plus grainstore reallocation, an "output grain too small"
//  warning path, and write_samps() straight to a soundfile.  Over one
//  contiguous buffer a grain cannot straddle anything, so all of that goes.
//
//  TWO DELIBERATE CHANGES, both so the defaults are usable:
//    * GATE IS RELATIVE TO THE PEAK.  CDP compares against an absolute level
//      (gate * 1.0), and ships `grain assess` as a separate program whose whole
//      job is to hunt for a gate that finds any grains at all.  Here the gate is
//      a fraction of the source's own peak, so 0.3 means something on quiet
//      material too.
//    * NO REFUSALS.  CDP errors with "No grains found" when the detector comes
//      up empty.  Here a source with fewer than two grains simply passes
//      through: a node created with defaults must never fail.
//
//  LENGTH.  Duplicate and omit change duration.  Produced length is clamped to
//  at most 60x the input or 10 minutes, whichever is SMALLER.
//
//  NOT STREAMABLE.  Reverse needs the last grain before it can emit the first,
//  and omit/duplicate need a whole grain (unbounded, signal-dependent) of
//  lookahead before anything can be written, so none of these offers a Stream.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

//! Hard ceiling on produced length: 60x the source, or 10 minutes, whichever is
//! smaller.  Nothing below may allocate past this.
int64_t length_cap(int64_t inFrames, int sampleRate) {
    const int sr = sampleRate > 0 ? sampleRate : 48000;
    const int64_t byGrowth  = inFrames > 0 ? inFrames * 60 : 0;
    const int64_t bySeconds = (int64_t)sr * 600;
    int64_t cap = std::min(byGrowth, bySeconds);
    if (cap < 1) cap = 1;
    return cap;
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

//! grain.c grains(): the onset of every grain, in frames.
//!
//! `gate` is a fraction of the source peak (see the header note); `minHole` is
//! the shortest run of below-gate frames that counts as a gap.  A frame is "in
//! a grain" if ANY channel is outside the gate, which is CDP's test -- so all
//! channels share one grain map and stay phase-aligned.
std::vector<int64_t> find_onsets(const Buffer& src, double gateFrac, int64_t minHole) {
    std::vector<int64_t> onsets;
    const int64_t n = src.frames();
    const int chans = src.channels();
    if (n <= 0 || chans <= 0) return onsets;

    double peak = 0.0;
    for (int c = 0; c < chans; ++c)
        for (int64_t i = 0; i < n; ++i)
            peak = std::max(peak, (double)std::fabs(src.ch[(size_t)c][(size_t)i]));
    if (peak <= 0.0) return onsets;                 // silence has no grains
    const double gate = gateFrac * peak;

    bool started = false;
    int64_t hole = 0;
    for (int64_t i = 0; i < n; ++i) {
        bool inGrain = false;
        for (int c = 0; c < chans && !inGrain; ++c) {
            const float v = src.ch[(size_t)c][(size_t)i];
            if (v > gate || v < -gate) inGrain = true;
        }
        if (inGrain) {
            if (!started) { onsets.push_back(i); started = true; }
            else if (hole >= minHole) onsets.push_back(i);
            hole = 0;
        } else if (started) {
            ++hole;
        }
    }
    return onsets;
}

//! One grain as it will be written: a source span plus the half-splice fades.
struct Grain { int64_t src; int64_t len; };

//! Turn onsets into grain spans, following retime_grain(): the span begins one
//! half-splice BEFORE the onset so the fade-up sits in the preceding hole.  The
//! last grain runs to the end of the file rather than stopping a half-splice
//! short, so no material is silently lost.
std::vector<Grain> grains_from_onsets(const std::vector<int64_t>& onsets,
                                      int64_t frames, int64_t h) {
    std::vector<Grain> g;
    const size_t n = onsets.size();
    g.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        int64_t start = onsets[k] - h;
        if (start < 0) start = 0;
        int64_t end = (k + 1 < n) ? (onsets[k + 1] - h) : frames;
        if (end > frames) end = frames;
        if (end - start >= 2) g.push_back(Grain{ start, end - start });
    }
    return g;
}

//! Write one grain at `outPos`, with CDP's linear half-splice fade at each end.
//! All channels take the same source frames, so stereo stays phase-aligned.
void write_grain(Buffer& out, const Buffer& src, int64_t outPos,
                 const Grain& g, int64_t h) {
    const int64_t on = out.frames(), sn = src.frames();
    if (g.len <= 0 || on <= 0) return;
    const int64_t sp = std::max<int64_t>(0, std::min(h, g.len / 2));
    const int chans = std::min(out.channels(), src.channels());
    for (int64_t i = 0; i < g.len; ++i) {
        const int64_t o = outPos + i;
        if (o < 0) continue;
        if (o >= on) break;
        const int64_t s = g.src + i;
        if (s < 0 || s >= sn) continue;
        double gain = 1.0;
        if (sp > 0) {
            if (i < sp)               gain = (double)i / (double)sp;
            else if (i >= g.len - sp) gain = (double)(g.len - 1 - i) / (double)sp;
        }
        for (int c = 0; c < chans; ++c)
            out.ch[(size_t)c][(size_t)o] = (float)(src.ch[(size_t)c][(size_t)s] * gain);
    }
}

//! Shared front end: read the three detector parameters, find the grains.
//! Returns false only when the caller should pass the input straight through.
struct Detected {
    std::vector<Grain> grains;
    int64_t            preLen = 0;      //!< material before the first onset
    int64_t            h = 0;           //!< half-splice, frames
};

bool detect(const Buffer& src, double gateP, double minHoleMs, double spliceMs,
            Detected& d) {
    const int64_t n = src.frames();
    const int sr = src.sampleRate > 0 ? src.sampleRate : 48000;

    const double gate = clampd(gateP, 0.0, 0.999);
    // minhole and splice are genuine absolute times (a splice has to be short in
    // milliseconds to be inaudible), but both are clamped against the source so
    // they can never be longer than the material they act on.
    int64_t minHole = (int64_t)std::llround(clampd(minHoleMs, 0.1, 10000.0) * 0.001 * (double)sr);
    if (minHole < 1)     minHole = 1;
    if (minHole > n / 4) minHole = std::max<int64_t>(1, n / 4);
    int64_t splice = (int64_t)std::llround(clampd(spliceMs, 0.0, 500.0) * 0.001 * (double)sr);
    if (splice < 0)      splice = 0;
    if (splice > n / 8)  splice = n / 8;
    d.h = splice / 2;

    const std::vector<int64_t> onsets = find_onsets(src, gate, minHole);
    if (onsets.size() < 2) return false;            // nothing to reorder
    d.grains = grains_from_onsets(onsets, n, d.h);
    if (d.grains.size() < 2) return false;
    d.preLen = d.grains.front().src;                // [0, first grain start)
    return true;
}

//! Copy the source unchanged -- what every process does when the detector finds
//! fewer than two grains.  CDP errors here; refusing would break a node built
//! with defaults on unsuitable material.
bool passthrough(const Buffer& src, Buffer& out) { out = src; return true; }

//! Write the material before the first onset, with a fade DOWN over its last
//! half-splice so it meets the first grain's fade up.
void write_pre(Buffer& out, const Buffer& src, int64_t preLen, int64_t h) {
    if (preLen <= 0) return;
    const int chans = std::min(out.channels(), src.channels());
    const int64_t sp = std::max<int64_t>(0, std::min(h, preLen / 2));
    const int64_t on = out.frames();
    for (int64_t i = 0; i < preLen && i < on; ++i) {
        double gain = 1.0;
        if (sp > 0 && i >= preLen - sp) gain = (double)(preLen - 1 - i) / (double)sp;
        for (int c = 0; c < chans; ++c)
            out.ch[(size_t)c][(size_t)i] = (float)(src.ch[(size_t)c][(size_t)i] * gain);
    }
}

//---------------------------------------------------------------------------
//  GRAIN REVERSE  (grain1.c do_the_reversing_process)
//
//  Play the grains in reverse order, each one still forwards internally -- so
//  the rhythm runs backwards but nothing sounds "rewound".  CDP clears the
//  output buffer before it starts, discarding the material before the first
//  onset; that is reproduced.
//---------------------------------------------------------------------------
bool grain_reverse(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    Detected d;
    if (!detect(src, at(0, 0.30), at(1, 32.0), at(2, 15.0), d))
        return passthrough(src, out);

    const int64_t cap = length_cap(src.frames(), src.sampleRate);
    int64_t total = 0;
    for (const Grain& g : d.grains) total += g.len;
    if (total > cap) total = cap;
    if (total < 1)   total = 1;

    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), total);

    int64_t pos = 0;
    for (size_t k = d.grains.size(); k-- > 0 && pos < total; ) {
        write_grain(out, src, pos, d.grains[k], d.h);
        pos += d.grains[k].len;
        if (prog && (k % 64) == 0 && !prog(1.0 - (double)k / (double)d.grains.size())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//---------------------------------------------------------------------------
//  GRAIN OMIT  (grain1.c keep_non_omitted_grains)
//
//  Keep the first `keep` grains of every `out of` and drop the rest, thinning a
//  grainy texture and shortening it.  CDP counts with a modulo counter that
//  resets at `out of`; same here.
//---------------------------------------------------------------------------
bool grain_omit(const std::vector<Buffer>& in, const std::vector<double>& p,
                Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    int keep   = (int)clampd(std::floor(at(0, 1.0) + 0.5), 1.0, 64.0);
    const int outOf = (int)clampd(std::floor(at(1, 2.0) + 0.5), 1.0, 64.0);
    if (keep > outOf) keep = outOf;                 // CDP's consistency check,
                                                    // clamped rather than refused
    Detected d;
    if (!detect(src, at(2, 0.30), at(3, 32.0), at(4, 15.0), d))
        return passthrough(src, out);

    std::vector<size_t> kept;
    for (size_t k = 0; k < d.grains.size(); ++k)
        if ((int)(k % (size_t)outOf) < keep) kept.push_back(k);
    if (kept.empty()) return passthrough(src, out);

    const int64_t cap = length_cap(src.frames(), src.sampleRate);
    int64_t total = d.preLen;
    for (size_t k : kept) total += d.grains[k].len;
    if (total > cap) total = cap;
    if (total < 1)   total = 1;

    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), total);
    write_pre(out, src, std::min(d.preLen, total), d.h);

    int64_t pos = d.preLen;
    for (size_t i = 0; i < kept.size() && pos < total; ++i) {
        write_grain(out, src, pos, d.grains[kept[i]], d.h);
        pos += d.grains[kept[i]].len;
        if (prog && (i % 64) == 0 && !prog((double)i / (double)kept.size())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//---------------------------------------------------------------------------
//  GRAIN DUPLICATE  (grain1.c duplicate_grain)
//
//  Repeat every grain N times in place, stuttering the texture and lengthening
//  it by roughly N.  CDP emits N-1 copies with store_end off and the last with
//  it on; with the source in memory the copies are identical writes.
//---------------------------------------------------------------------------
bool grain_duplicate(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    const int dupl = (int)clampd(std::floor(at(0, 2.0) + 0.5), 1.0, 64.0);

    Detected d;
    if (!detect(src, at(1, 0.30), at(2, 32.0), at(3, 15.0), d))
        return passthrough(src, out);

    const int64_t cap = length_cap(src.frames(), src.sampleRate);
    int64_t total = d.preLen;
    for (const Grain& g : d.grains) total += g.len * dupl;
    if (total > cap) total = cap;                   // absurd N truncates, never
    if (total < 1)   total = 1;                     // allocates without bound

    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), total);
    write_pre(out, src, std::min(d.preLen, total), d.h);

    int64_t pos = d.preLen;
    for (size_t k = 0; k < d.grains.size() && pos < total; ++k) {
        for (int r = 0; r < dupl && pos < total; ++r) {
            write_grain(out, src, pos, d.grains[k], d.h);
            pos += d.grains[k].len;
        }
        if (prog && (k % 64) == 0 && !prog((double)k / (double)d.grains.size())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

} // namespace

void register_grain_processes() {
    // Every process in this family shares the detector's three parameters; the
    // help text is CDP's own, from grain/ap_grain.c usage2().
    const ParamSpec gate    { "Gate", "x peak", 0.0, 0.999, 0.30, false,
        "Level a sample must exceed to count as being inside a grain." };
    const ParamSpec minhole { "Min hole", "ms", 0.1, 10000.0, 32.0, false,
        "Shortest gap that separates two grains." };
    const ParamSpec splice  { "Splice", "ms", 0.0, 500.0, 15.0, false,
        "Fade at each grain edge; clamped against the grain length." };

    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug  = "grain.reverse";
        p.name  = "Grain Reverse";
        p.group = "Grain";
        p.help  = "Play the grains in reverse order without reversing the grains "
                  "themselves.";
        p.streamable = false;                 // needs the last grain first
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = { gate, minhole, splice };
        p.run = &grain_reverse;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "grain.omit";
        p.name  = "Grain Omit";
        p.group = "Grain";
        p.help  = "Keep some grains out of every group and drop the rest.";
        p.streamable = false;                 // a whole grain of lookahead
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Keep", "", 1, 64, 1, true, "Grains kept from the start of each group." },
            { "Out of", "", 1, 64, 2, true, "Grains in a group." },
            gate, minhole, splice,
        };
        p.run = &grain_omit;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "grain.duplicate";
        p.name  = "Grain Duplicate";
        p.group = "Grain";
        p.help  = "Repeat every grain, stuttering the texture and lengthening it.";
        p.streamable = false;                 // a whole grain of lookahead
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Repeats", "", 1, 64, 2, true, "How many times each grain is played." },
            gate, minhole, splice,
        };
        p.run = &grain_duplicate;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
