//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_distort.cpp
//
//  Ported from the CDP DISTORT family (vendor/cdp8/dev/distort), which is
//      Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  These are WAVESET distortions: CDP calls the span between two upward zero
//  crossings a "cycle", and the family works by grouping cycles and rewriting
//  them.  The algorithms are small; what made them un-callable was everything
//  around them.  In distortr.c the reversal itself is eight lines
//  (reverse_cycles), wrapped in CDP's `dataptr dz` global state, a two-buffer
//  ping-pong with a cross-buffer special case (reverse_cycles_crosbuf), and a
//  write_samps() call straight to a soundfile.  Lifting it means keeping the
//  algorithm and dropping the plumbing: over a contiguous buffer the
//  cross-buffer case cannot arise at all.
//
//  Cycle detection matches CDP's: a cycle boundary is a sample where the signal
//  crosses from negative to non-negative.  Channels are treated independently,
//  which is what the CDP programs do for multichannel input.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

//! Upward zero-crossing indices of one channel, plus the trailing end so the
//! last (possibly partial) cycle is a closed span.
std::vector<int64_t> cycle_bounds(const std::vector<float>& x) {
    std::vector<int64_t> b;
    const int64_t n = (int64_t)x.size();
    if (n == 0) return b;
    b.push_back(0);
    for (int64_t i = 1; i < n; ++i)
        if (x[(size_t)(i - 1)] < 0.f && x[(size_t)i] >= 0.f) b.push_back(i);
    b.push_back(n);
    return b;
}

//! DISTORT REVERSE -- reverse the sample order of each group of `cyclecnt`
//! cycles (distortr.c: reverse_cycles).  A partial group at the end is
//! reversed as it stands, which is what CDP does when the file runs out.
bool distort_reverse(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int group = std::max(1, (int)std::lround(p.empty() ? 1.0 : p[0]));
    out = in[0];
    for (int c = 0; c < out.channels(); ++c) {
        std::vector<float>& x = out.ch[(size_t)c];
        const std::vector<int64_t> b = cycle_bounds(x);
        if (b.size() < 2) continue;
        for (size_t g = 0; g + 1 < b.size(); g += (size_t)group) {
            const size_t last = std::min(g + (size_t)group, b.size() - 1);
            std::reverse(x.begin() + (ptrdiff_t)b[g], x.begin() + (ptrdiff_t)b[last]);
        }
        if (prog && !prog((double)(c + 1) / (double)out.channels())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//! DISTORT OMIT -- silence `omit` cycles out of every `keep + omit`
//! (distorto.c).  Sample-by-sample once the cycle boundary is known, so it is
//! genuinely streamable with only one cycle of lookahead.
bool distort_omit(const std::vector<Buffer>& in, const std::vector<double>& p,
                  Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int keep = std::max(1, (int)std::lround(p.size() > 0 ? p[0] : 1.0));
    const int omit = std::max(1, (int)std::lround(p.size() > 1 ? p[1] : 1.0));
    out = in[0];
    for (int c = 0; c < out.channels(); ++c) {
        std::vector<float>& x = out.ch[(size_t)c];
        const std::vector<int64_t> b = cycle_bounds(x);
        int inGroup = 0;
        for (size_t g = 0; g + 1 < b.size(); ++g, ++inGroup) {
            if (inGroup >= keep + omit) inGroup = 0;
            if (inGroup >= keep)                       // inside the omitted run
                std::fill(x.begin() + (ptrdiff_t)b[g],
                          x.begin() + (ptrdiff_t)b[g + 1], 0.f);
        }
        if (prog && !prog((double)(c + 1) / (double)out.channels())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

} // namespace

void register_distort_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug = "distort.reverse";
        p.name = "Distort Reverse";
        p.group = "Distort";
        p.help = "Reverse the sample order of each group of wavesets.";
        p.streamable = true;
        // Bounded lookahead: the group must be complete before it can be
        // emitted.  There is no fixed frame count -- it depends on the signal's
        // own period -- so report the worst case the host should reserve.
        p.latencyFrames = [](int sr, const std::vector<double>& pr) -> int64_t {
            const double groups = pr.empty() ? 1.0 : std::max(1.0, pr[0]);
            return (int64_t)(groups * (double)sr / 20.0);   // a 20 Hz period
        };
        p.params = { { "Cycles", "", 1, 64, 1, true,
                       "How many wavesets are reversed together." } };
        p.run = &distort_reverse;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "distort.omit";
        p.name = "Distort Omit";
        p.group = "Distort";
        p.help = "Silence some wavesets out of every group, thinning the sound.";
        p.streamable = true;
        p.latencyFrames = [](int sr, const std::vector<double>&) -> int64_t {
            return (int64_t)((double)sr / 20.0);            // one 20 Hz cycle
        };
        p.params = { { "Keep", "", 1, 64, 1, true, "Wavesets kept per group." },
                     { "Omit", "", 1, 64, 1, true, "Wavesets silenced per group." } };
        p.run = &distort_omit;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
