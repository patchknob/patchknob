//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_extend.cpp
//
//  Ported from the CDP EXTEND family (vendor/cdp8/dev/extend), which is
//      Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Files ported here:
//      extend/iterate.c    do_iteration(), iter(), iter_shift_interp(),
//                          get_gain(), get_pshift(), get_next_writestart()
//      extend/extprepro.c  iterate_preprocess() / set_default_gain(),
//                          generate_zigzag_table(), sort_zigs(),
//                          generate_loop_table(), scramble_rand(),
//                          make_zigsplice()
//      extend/zigzag.c     zigzag(), do_zigzags(), zig_or_zag(), reverse_it(),
//                          add_to_splicebuf(), do_down_splice(), setup_splices()
//
//  WHAT THESE ACTUALLY ARE.  Every one of the four is the same shape: CDP first
//  builds a LIST of (start,end) sample positions in the source -- randomly, in a
//  loop, or by a walk -- and then plays that list back, splicing the joins.  The
//  list-building lives in extprepro.c, the playback in zigzag.c/iterate.c, and
//  between them sits ~2000 lines of CDP's two-buffer ping-pong (adjust_buffer(),
//  find_zzchunk(), the four-way branch in zig_or_zag() over "does the outbuffer
//  reach into the end splice"), all of which exists ONLY because CDP streams
//  through a file it cannot hold in memory.  Over one contiguous buffer none of
//  those cases can arise, so the port is the list plus one segment writer.
//
//  THE SPLICE.  CDP fades the tail of a segment down into a splice buffer
//  (do_down_splice) and ADDS the head of the next segment on top of it
//  (add_to_splicebuf), so consecutive segments overlap by exactly one splice
//  length and no join can click.  add_segment() below reproduces that: linear
//  fade in and out of `splice` frames, summed into the output, with the write
//  cursor advancing by (len - splice).  The splice ramp is CDP's, n/splicecnt.
//
//  RANDOMNESS.  CDP calls drand48() off a global seed (initrand48()/srand()).
//  Here every random process takes a Seed parameter and drives a LOCAL xorshift
//  from it, so a render is reproducible: same seed, byte-identical output.
//
//  LENGTH.  These processes change duration by design.  Produced length is
//  clamped to at most 60x the input or 10 minutes, whichever is SMALLER, so a
//  runaway parameter truncates instead of exhausting memory.
//
//  PROPORTIONAL PARAMETERS.  CDP takes segment lengths and delays in absolute
//  seconds and REFUSES when the source is shorter.  Every time-like parameter
//  here is instead a fraction of the source duration (splice lengths, which must
//  stay short in absolute terms to be inaudible, are the exception and are
//  clamped to half the segment).  So the defaults run on a source of any length.
//
//  NOT STREAMABLE.  All four reorder or overlay material from across the whole
//  file -- zigzag reads backwards, scramble picks random positions, iterate sums
//  copies at future offsets -- so none can be produced block by block from a
//  live input.  streamable = false and no makeStream is offered.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

//! Hard ceiling on produced length: 60x the source, or 10 minutes, whichever is
//! smaller.  Nothing below may allocate past this, whatever the parameters say.
int64_t length_cap(int64_t inFrames, int sampleRate) {
    const int sr = sampleRate > 0 ? sampleRate : 48000;
    const int64_t byGrowth  = inFrames > 0 ? inFrames * 60 : 0;
    const int64_t bySeconds = (int64_t)sr * 600;
    int64_t cap = std::min(byGrowth, bySeconds);
    if (cap < 1) cap = 1;
    return cap;
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

//! Local RNG, seeded from a parameter.  CDP uses drand48() off a global (or
//! time) seed; a render here must be repeatable, so the state is local and the
//! seed is data.  xorshift64 -- small, no dependencies, good enough for
//! choosing splice points.
struct Rng {
    uint64_t s;
    explicit Rng(double seed) {
        int64_t k = (int64_t)std::llround(seed);
        s = (uint64_t)k * 6364136223846793005ULL + 1442695040888963407ULL;
        if (s == 0) s = 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < 4; ++i) next();          // wash small seeds
    }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    //! Uniform in [0,1), CDP's drand48() equivalent.
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
};

//! One playback of a stretch of the source.  `rev` is CDP's ZAG: zigzag.c
//! reverse_it() copies the span backwards a frame at a time, which is exactly
//! reading it in reverse with the channels kept together.
struct Seg { int64_t start; int64_t len; bool rev; };

//! Write one segment into the output, ADDING, with CDP's linear splice ramp at
//! each end.  Channels are stepped together, so a stereo source stays
//! phase-aligned: every channel is read from the same source frame.
void add_segment(Buffer& out, const Buffer& src, int64_t outPos,
                 int64_t srcStart, int64_t len, bool rev, int64_t splice) {
    const int64_t on = out.frames(), sn = src.frames();
    if (len <= 0 || on <= 0 || sn <= 0) return;
    const int64_t sp = std::max<int64_t>(0, std::min(splice, len / 2));
    const int chans = std::min(out.channels(), src.channels());
    for (int64_t i = 0; i < len; ++i) {
        const int64_t o = outPos + i;
        if (o < 0) continue;
        if (o >= on) break;
        const int64_t s = rev ? (srcStart + len - 1 - i) : (srcStart + i);
        if (s < 0 || s >= sn) continue;
        double g = 1.0;
        if (sp > 0) {
            if (i < sp)             g = (double)i / (double)sp;
            else if (i >= len - sp) g = (double)(len - 1 - i) / (double)sp;
        }
        for (int c = 0; c < chans; ++c)
            out.ch[(size_t)c][(size_t)o] =
                (float)(out.ch[(size_t)c][(size_t)o] + src.ch[(size_t)c][(size_t)s] * g);
    }
}

//! Play a segment list end to end, overlapping each join by one splice.  This is
//! zigzag.c's do_zigzags()/zig_or_zag() with the file plumbing removed: the
//! cursor advances by (len - splice) so the next segment's fade-in lands on this
//! one's fade-out, which is what CDP's splice buffer achieves.
bool render_segments(const Buffer& src, const std::vector<Seg>& segs, int64_t splice,
                     int64_t cap, Buffer& out, std::string& err, const Progress& prog) {
    if (segs.empty()) { err = "no segments produced (check the parameters)"; return false; }
    int64_t total = 0;
    for (const Seg& s : segs) {
        const int64_t sp = std::max<int64_t>(0, std::min(splice, s.len / 2));
        total += std::max<int64_t>(1, s.len - sp);
        if (total >= cap) { total = cap; break; }
    }
    // The last segment's fade-out lives past the advance point.
    total = std::min(cap, total + splice);
    if (total < 1) total = 1;

    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), total);

    int64_t pos = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (pos >= total) break;
        add_segment(out, src, pos, segs[i].start, segs[i].len, segs[i].rev, splice);
        const int64_t sp = std::max<int64_t>(0, std::min(splice, segs[i].len / 2));
        pos += std::max<int64_t>(1, segs[i].len - sp);
        if (prog && (i % 64) == 0 && !prog((double)i / (double)segs.size())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//! Splice length in frames, from a millisecond parameter.  CDP defaults to 15ms
//! (extdcon.h ZIG_SPLICELEN) and refuses when the splice will not fit; we clamp
//! to a quarter of the segment instead, which is the same intent without the
//! refusal.
int64_t splice_frames(double ms, int sr, int64_t segFrames) {
    int64_t sp = (int64_t)std::llround(clampd(ms, 0.0, 500.0) * 0.001 * (double)(sr > 0 ? sr : 48000));
    if (sp < 0) sp = 0;
    const int64_t lim = segFrames / 4;
    if (sp > lim) sp = lim;
    return sp;
}

//---------------------------------------------------------------------------
//  EXTEND ITERATE  (iterate.c do_iteration + extprepro.c iterate_preprocess)
//
//  Sum repeated copies of the whole source at successive delays.  Each copy may
//  be delayed irregularly (ITER_RANDOM), attenuated at random (ITER_ASCAT),
//  faded progressively (ITER_FADE) and transposed at random (ITER_PSCAT, done
//  by CDP with a linearly interpolated read at a fractional step -- see
//  iter_shift_interp(); that is reproduced exactly).
//
//  CDP runs the whole thing TWICE: pass 0 measures the peak, pass 1 rewrites the
//  file with the gains scaled so the peak lands on ACCEPTABLE_LEVEL (0.75).
//  Holding the output in memory makes that one pass with a scan at the end.  The
//  0.75 constant is exposed here as the Level parameter.
//---------------------------------------------------------------------------
bool extend_iterate(const std::vector<Buffer>& in, const std::vector<double>& p,
                    Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    const int64_t n   = src.frames();
    const int chans   = src.channels();
    const int sr      = src.sampleRate > 0 ? src.sampleRate : 48000;
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    // Delay is a FRACTION of the source, not seconds: CDP's default is "infile
    // duration" and its absolute seconds break on short clips.
    const double delayFrac = clampd(at(0, 0.35), 0.001, 8.0);
    const int    repeats   = (int)clampd(std::floor(at(1, 8.0) + 0.5), 0.0, 500.0);
    const double rnd       = clamp01(at(2, 0.0));
    const double pscat     = clampd(at(3, 0.0), 0.0, 12.0);     // ITER_MAXPSHIFT
    const double ascat     = clamp01(at(4, 0.0));
    const double fade      = clamp01(at(5, 0.0));
    const double level     = clampd(at(6, 0.75), 0.01, 1.0);    // ACCEPTABLE_LEVEL
    Rng rng(at(7, 1.0));

    const int64_t del = std::max<int64_t>(1, (int64_t)std::llround(delayFrac * (double)n));
    const int64_t cap = length_cap(n, sr);

    struct Copy { int64_t start; double gain; double ratio; };
    std::vector<Copy> copies;
    int64_t pos = 0, end = 0;
    for (int k = 0; k <= repeats; ++k) {
        if (pos >= cap) break;
        Copy c;
        c.start = pos;
        // get_gain(): amplitude scatter is a random CUT, never a boost.
        // The progressive fade is CDP's `level`, multiplied once per iteration.
        c.gain  = (1.0 - rng.uniform() * ascat) * std::pow(1.0 - fade, (double)k);
        // get_pshift(): +/- pscat semitones.  The first sound is an exact copy
        // (extprepro.c sets ITER_STEP = 1.0 before the first pass).
        c.ratio = (k == 0 || pscat <= 0.0)
                ? 1.0
                : std::pow(2.0, ((rng.uniform() * 2.0 - 1.0) * pscat) / 12.0);
        copies.push_back(c);
        const int64_t len = (int64_t)std::ceil((double)n / c.ratio);
        end = std::max(end, std::min(cap, c.start + len));
        // get_next_writestart(): delay randomised by +/- rnd of itself.
        double d = 1.0;
        if (rnd > 0.0) d = 1.0 + ((rng.uniform() * 2.0) - 1.0) * rnd;
        int64_t step = (int64_t)std::llround((double)del * d);
        if (step < 1) step = 1;
        pos += step;
    }
    if (end < 1) end = 1;
    out.sampleRate = sr;
    out.resize(chans, end);

    for (size_t k = 0; k < copies.size(); ++k) {
        const Copy& c = copies[k];
        if (c.ratio == 1.0) {                                   // iter()
            for (int ch = 0; ch < chans; ++ch) {
                const std::vector<float>& s = src.ch[(size_t)ch];
                std::vector<float>&       o = out.ch[(size_t)ch];
                for (int64_t i = 0; i < n; ++i) {
                    const int64_t j = c.start + i;
                    if (j >= end) break;
                    o[(size_t)j] = (float)(o[(size_t)j] + s[(size_t)i] * c.gain);
                }
            }
        } else {                                                // iter_shift_interp()
            double d = 0.0, part = 0.0;
            int64_t i = 0, j = c.start;
            while (i < n && j < end) {
                for (int ch = 0; ch < chans; ++ch) {
                    const std::vector<float>& s = src.ch[(size_t)ch];
                    const double a = s[(size_t)i];
                    const double b = (i + 1 < n) ? s[(size_t)(i + 1)] : 0.0;  // CDP's guard point
                    out.ch[(size_t)ch][(size_t)j] =
                        (float)(out.ch[(size_t)ch][(size_t)j] + (a + (b - a) * part) * c.gain);
                }
                ++j;
                d += c.ratio;
                i = (int64_t)d;
                part = d - (double)i;
            }
        }
        if (prog && !prog((double)(k + 1) / (double)copies.size())) {
            err = "cancelled"; return false;
        }
    }

    // CDP's second pass: scale so the peak sits at ACCEPTABLE_LEVEL.  Its rule
    // is "leave alone if already between level and 0.99", reproduced here.
    double peak = 0.0;
    for (int ch = 0; ch < chans; ++ch)
        for (int64_t i = 0; i < end; ++i)
            peak = std::max(peak, (double)std::fabs(out.ch[(size_t)ch][(size_t)i]));
    if (peak > 0.0 && (peak < level || peak > 0.99)) {
        const double g = level / peak;
        for (int ch = 0; ch < chans; ++ch)
            for (int64_t i = 0; i < end; ++i)
                out.ch[(size_t)ch][(size_t)i] = (float)(out.ch[(size_t)ch][(size_t)i] * g);
    }
    return true;
}

//---------------------------------------------------------------------------
//  EXTEND LOOP  (extprepro.c generate_loop_table + zigzag.c playback)
//
//  Repeat a segment, advancing its start through the source by `step` on each
//  repeat and jittering that start inside a search field.  CDP's loop table is
//  a list of (play,skip) pairs; with the segment list held in memory the skips
//  are simply not emitted.
//---------------------------------------------------------------------------
bool extend_loop(const std::vector<Buffer>& in, const std::vector<double>& p,
                 Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    const int64_t n = src.frames();
    const int sr = src.sampleRate > 0 ? src.sampleRate : 48000;
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    const double startF = clamp01(at(0, 0.0));
    const double lenF   = clampd(at(1, 0.25), 0.005, 1.0);
    const double stepF  = clampd(at(2, 0.0), -1.0, 1.0);
    const int    reps   = (int)clampd(std::floor(at(3, 8.0) + 0.5), 1.0, 500.0);
    const double srchF  = clamp01(at(4, 0.0));
    Rng rng(at(6, 1.0));

    int64_t seg = (int64_t)std::llround(lenF * (double)n);
    if (seg < 2) seg = 2;
    if (seg > n) seg = n;
    const int64_t splice = splice_frames(at(5, 15.0), sr, seg);
    const int64_t base = (int64_t)std::llround(startF * (double)n);
    const int64_t step = (int64_t)std::llround(stepF  * (double)n);
    const int64_t srch = (int64_t)std::llround(srchF  * (double)n);
    const int64_t last = std::max<int64_t>(0, n - seg);
    const int64_t cap  = length_cap(n, sr);

    std::vector<Seg> segs;
    segs.reserve((size_t)reps);
    int64_t produced = 0;
    for (int i = 0; i < reps && produced < cap; ++i) {
        int64_t pos = base + (int64_t)i * step;
        if (srch > 0) pos += (int64_t)std::llround(rng.uniform() * (double)srch);
        if (pos < 0)    pos = 0;
        if (pos > last) pos = last;
        segs.push_back(Seg{ pos, seg, false });
        produced += std::max<int64_t>(1, seg - splice);
    }
    return render_segments(src, segs, splice, cap, out, err, prog);
}

//---------------------------------------------------------------------------
//  EXTEND SCRAMBLE  (extprepro.c scramble_rand, mode 1)
//
//  Cut chunks of random length from random positions and splice them end to end
//  until the requested duration is reached.  CDP's arithmetic is kept: the
//  chunk length is min + rand*(max-min), the position is a random fraction of
//  (filelen - splice - chunklen), and the played span is chunk + one splice so
//  that the splice overlap does not eat into the chunk.
//---------------------------------------------------------------------------
bool extend_scramble(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    const int64_t n = src.frames();
    const int sr = src.sampleRate > 0 ? src.sampleRate : 48000;
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    double minF = clampd(at(0, 0.05), 0.002, 1.0);
    double maxF = clampd(at(1, 0.25), 0.002, 1.0);
    if (maxF < minF) std::swap(minF, maxF);                 // CDP demands max > min
    const double durMult = clampd(at(2, 1.0), 0.02, 60.0);
    Rng rng(at(4, 1.0));

    int64_t minLen = std::max<int64_t>(2, (int64_t)std::llround(minF * (double)n));
    int64_t maxLen = std::max<int64_t>(minLen, (int64_t)std::llround(maxF * (double)n));
    if (minLen > n) minLen = n;
    if (maxLen > n) maxLen = n;
    const int64_t splice = splice_frames(at(3, 15.0), sr, minLen);
    const int64_t cap    = length_cap(n, sr);
    int64_t target = (int64_t)std::llround(durMult * (double)n);
    if (target < 1)   target = 1;
    if (target > cap) target = cap;

    std::vector<Seg> segs;
    int64_t produced = 0;
    // Bounded: every chunk is at least minLen(>=2) frames, so this terminates.
    while (produced < target && (int64_t)segs.size() < 1000000) {
        const int64_t range = maxLen - minLen;
        int64_t thislen = minLen + (range > 0 ? (int64_t)std::llround(rng.uniform() * (double)range) : 0);
        if (thislen > n) thislen = n;
        int64_t effective = n - splice - thislen;
        if (effective < 0) effective = 0;
        int64_t pos = (int64_t)std::llround(rng.uniform() * (double)effective);
        int64_t span = thislen + splice;
        if (pos + span > n) span = n - pos;
        if (span < 2) { pos = 0; span = std::min<int64_t>(n, 2); }
        segs.push_back(Seg{ pos, span, false });
        produced += std::max<int64_t>(1, span - splice);
    }
    return render_segments(src, segs, splice, std::min(cap, target + splice), out, err, prog);
}

//---------------------------------------------------------------------------
//  EXTEND ZIGZAG  (extprepro.c generate_zigzag_table + sort_zigs, zigzag.c)
//
//  Read back and forth inside the source.  The time list alternates direction;
//  each step is a random length between minzig and maxzig, clipped to the
//  distance remaining to the start/end bound.  Steps that run forward play
//  forward, steps that run backward play the span REVERSED (zigzag.c
//  reverse_it), and every join is spliced because consecutive steps always
//  change direction.
//
//  Two things from CDP are deliberately reproduced: the list begins at sample 0
//  (generate_zigzag_table writes times[0] = 0 regardless of the start time) and
//  ends at the end of the file.  One thing is fixed: CDP's generation loop can
//  spin forever when no step is possible (its `continue` skips the progress
//  counter), so the loop below has a step budget and gives up gracefully.
//---------------------------------------------------------------------------
bool extend_zigzag(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    const int64_t n = src.frames();
    const int sr = src.sampleRate > 0 ? src.sampleRate : 48000;
    auto at = [&](size_t i, double d) { return i < p.size() ? p[i] : d; };

    double startF = clamp01(at(0, 0.0));
    double endF   = clamp01(at(1, 1.0));
    if (endF < startF) std::swap(startF, endF);
    const double durMult = clampd(at(2, 2.0), 0.02, 60.0);
    double minF = clampd(at(3, 0.10), 0.002, 1.0);          // fraction of the span
    double maxF = clampd(at(4, 0.40), 0.002, 1.0);
    if (maxF < minF) std::swap(minF, maxF);
    Rng rng(at(6, 1.0));

    const int64_t lo = (int64_t)std::llround(startF * (double)n);
    int64_t hi = (int64_t)std::llround(endF * (double)n);
    if (hi <= lo) hi = std::min(n, lo + 2);
    const int64_t span = hi - lo;
    int64_t minZig = std::max<int64_t>(2, (int64_t)std::llround(minF * (double)span));
    int64_t maxZig = std::max<int64_t>(minZig + 1, (int64_t)std::llround(maxF * (double)span));
    if (maxZig > span) maxZig = span;
    if (minZig >= maxZig) minZig = std::max<int64_t>(1, maxZig - 1);

    const int64_t splice = splice_frames(at(5, 15.0), sr, minZig);
    const int64_t cap    = length_cap(n, sr);
    int64_t goal = (int64_t)std::llround(durMult * (double)n);
    if (goal < 1)   goal = 1;
    if (goal > cap) goal = cap;

    // ---- generate_zigzag_table: the list of turning points -----------------
    // CDP's goal is  dur - (infiledur - end):  the final run to the file end is
    // played whatever happens, so it is discounted from the travel budget.
    int64_t goalTravel = goal - (n - hi);
    if (goalTravel < 1) goalTravel = 1;
    std::vector<int64_t> times;
    times.push_back(0);                                     // CDP: always sample 0
    int64_t here = lo, travelled = lo;
    int dir = +1;
    const int kStepBudget = 200000;
    for (int guard = 0; guard < kStepBudget && travelled < goalTravel; ++guard) {
        const int64_t room = (dir > 0) ? (hi - here) : (here - lo);
        const int64_t diff = std::min(maxZig, room);
        if (diff > minZig) {
            const int64_t rl = minZig + (int64_t)std::llround(rng.uniform() * (double)(diff - minZig));
            here += dir * rl;
            travelled += rl;
            times.push_back(here);
        }
        dir = -dir;
        // If neither direction can move, the parameters are degenerate: stop.
        if (diff <= minZig && std::min(maxZig, (dir > 0) ? (hi - here) : (here - lo)) <= minZig)
            break;
    }
    times.push_back(n);                                     // CDP: run to file end

    // ---- sort_zigs: drop steps too short to splice, merge equal directions --
    std::vector<Seg> segs;
    for (size_t i = 0; i + 1 < times.size(); ++i) {
        const int64_t a = times[i], b = times[i + 1];
        const int64_t len = (a < b) ? (b - a) : (a - b);
        if (len < std::max<int64_t>(2, splice * 2 + 1)) continue;
        segs.push_back(Seg{ std::min(a, b), len, b < a });
    }
    if (segs.empty()) segs.push_back(Seg{ 0, n, false });    // degenerate: play it straight
    return render_segments(src, segs, splice, std::min(cap, goal + splice), out, err, prog);
}

} // namespace

void register_extend_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug  = "extend.iterate";
        p.name  = "Iterate";
        p.group = "Extend";
        p.help  = "Sum repeated copies of the sound at successive delays, with "
                  "random delay, pitch, amplitude scatter and a progressive fade.";
        p.streamable = false;                 // sums copies at future offsets
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Delay", "x source", 0.001, 8.0, 0.35, false,
              "Gap between iterations, as a fraction of the source duration." },
            { "Repeats", "", 0, 500, 8, true, "How many extra copies are added." },
            { "Delay rand", "", 0.0, 1.0, 0.0, false,
              "Randomises each delay by this fraction of itself." },
            { "Pitch scatter", "semitones", 0.0, 12.0, 0.0, false,
              "Each copy is transposed at random by up to this much." },
            { "Amp scatter", "", 0.0, 1.0, 0.0, false,
              "Each copy is cut at random by up to this fraction." },
            { "Fade", "", 0.0, 1.0, 0.0, false,
              "Progressive amplitude fade from one iteration to the next." },
            { "Level", "", 0.01, 1.0, 0.75, false,
              "Peak the result is scaled to (CDP's ACCEPTABLE_LEVEL)." },
            { "Seed", "", 0, 100000, 1, true,
              "Same seed gives an identical render." },
        };
        p.run = &extend_iterate;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "extend.loop";
        p.name  = "Loop";
        p.group = "Extend";
        p.help  = "Repeat a segment of the sound, advancing its start through the "
                  "source on each repeat.";
        p.streamable = false;                 // loops back over already-read material
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Start", "x source", 0.0, 1.0, 0.0, false,
              "Where the loop begins, as a fraction of the source." },
            { "Length", "x source", 0.005, 1.0, 0.25, false,
              "Loop length, as a fraction of the source." },
            { "Step", "x source", -1.0, 1.0, 0.0, false,
              "How far the loop start advances on each repeat (0 = stay put)." },
            { "Repeats", "", 1, 500, 8, true, "Number of loop repeats." },
            { "Search", "x source", 0.0, 1.0, 0.0, false,
              "Randomises each loop start within this window." },
            { "Splice", "ms", 0.0, 500.0, 15.0, false,
              "Crossfade at each join; clamped to a quarter of the loop." },
            { "Seed", "", 0, 100000, 1, true,
              "Same seed gives an identical render." },
        };
        p.run = &extend_loop;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "extend.scramble";
        p.name  = "Scramble";
        p.group = "Extend";
        p.help  = "Cut random chunks from the sound and splice them end to end.";
        p.streamable = false;                 // picks chunks from anywhere in the file
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Min chunk", "x source", 0.002, 1.0, 0.05, false,
              "Shortest chunk, as a fraction of the source." },
            { "Max chunk", "x source", 0.002, 1.0, 0.25, false,
              "Longest chunk, as a fraction of the source." },
            { "Duration", "x source", 0.02, 60.0, 1.0, false,
              "Output length, as a multiple of the source." },
            { "Splice", "ms", 0.0, 500.0, 15.0, false,
              "Crossfade at each join; clamped to a quarter of a chunk." },
            { "Seed", "", 0, 100000, 1, true,
              "Same seed gives an identical render." },
        };
        p.run = &extend_scramble;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "extend.zigzag";
        p.name  = "Zigzag";
        p.group = "Extend";
        p.help  = "Read back and forth inside the sound; backward steps play in "
                  "reverse.";
        p.streamable = false;                 // reads backwards through the file
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Start", "x source", 0.0, 1.0, 0.0, false,
              "Earliest point the zigzag reaches, as a fraction of the source." },
            { "End", "x source", 0.0, 1.0, 1.0, false,
              "Latest point the zigzag reaches." },
            { "Duration", "x source", 0.02, 60.0, 2.0, false,
              "Output length, as a multiple of the source." },
            { "Min step", "x span", 0.002, 1.0, 0.10, false,
              "Shortest zig, as a fraction of the start..end span." },
            { "Max step", "x span", 0.002, 1.0, 0.40, false,
              "Longest zig, as a fraction of the start..end span." },
            { "Splice", "ms", 0.0, 500.0, 15.0, false,
              "Crossfade at each turn; clamped to a quarter of the shortest zig." },
            { "Seed", "", 0, 100000, 1, true,
              "Same seed gives an identical render." },
        };
        p.run = &extend_zigzag;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
