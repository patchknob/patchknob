//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_distort2.cpp
//
//  Ported from the CDP DISTORT family (vendor/cdp8/dev/distort), which is
//      Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
//      http://www.trevorwishart.co.uk  /  http://www.composersdesktop.com
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  This file continues cdp_distort.cpp (which holds distortr.c "reverse" and
//  distorto.c "omit") with the rest of the waveset family.  One process per CDP
//  program, and the exact function each algorithm came out of:
//
//      distrpt.c   do_repeat()                       -> distort.repeat
//      distortm.c  do_multiply() / set_phaseswitch()  -> distort.multiply
//      distorta.c  distorta_func() / get_cyc_average() -> distort.average
//      disttel.c   do_cycle_tele() / get_longcycle()  -> distort.telescope
//      distrpl.c   do_cycle_loudrep() / get_ffcycle() -> distort.replace
//      distorth.c  do_harmonic()                      -> distort.harmonics
//      distortf.c  do_fractal() / get_scaled_halfcycle() -> distort.fractal
//      distorte.c  distorte_rising/falling/troffed + do_*_trof()
//                                                     -> distort.envelope
//      distdel.c   distort_del() / do_cycle_loud() / do_cycle_quiet()
//                                                     -> distort.delete
//      distflt.c   is_filtered_out()                  -> distort.filter
//      distortp.c  get_distort() / do_distrt()        -> distort.pitch
//
//  WHAT WAS DROPPED.  Every one of those programs is a main() that reads argv,
//  opens soundfiles through CDP's sfsys layer, and then ping-pongs the audio
//  through two fixed-size sample buffers.  Because a waveset can straddle the
//  seam between those two buffers, EVERY algorithm above is written twice --
//  once normally and once as a `_crosbuf` / `_bufcros` variant that stitches
//  the two halves together (distorta_func_crosbuf, do_repeat_bufcros,
//  do_harmonic_crosbuf, do_cycle_tele_crosbuf, ...).  Over one contiguous
//  buffer that case cannot arise, so only the plain form is ported; the
//  `_crosbuf` twins are plumbing, not algorithm.  Also dropped: the `dataptr
//  dz` global parameter blocks, breakpoint-table sweeping, the CYCBUFLEN
//  "wavelength exceeds maximum size" bailouts (a std::vector has no such
//  limit), and the 16-bit overflow reporting.
//
//  CYCLE DETECTION is cdp_distort.cpp's, unchanged, so the whole family agrees
//  on what a waveset is: a boundary is a sample where the signal crosses from
//  negative to non-negative.  distort.multiply and distort.pitch work on HALF
//  wavesets (CDP does too -- see the comment atop mdistort()), so they use
//  half_bounds(), which fires on a sign change in EITHER direction.  The full
//  boundaries are exactly the negative-to-non-negative subset of the half
//  boundaries, so the two are the same convention read at two resolutions.
//  Channels are processed independently, as the CDP programs do.
//
//  LENGTH.  repeat / average / telescope / replace / delete / filter / pitch
//  all change the output length, and repeat's multiplier is under user control.
//  Every generating loop stops once it has produced kMaxGrowth (60) times the
//  input frame count, so no parameter -- however wrong -- can exhaust memory.
//  Channels that end up different lengths are zero-padded to the longest.
//
//  STREAMING.  A waveset process needs bounded lookahead: the group has to be
//  complete before it can be emitted.  The four that emit exactly as many
//  frames as they consume (multiply, harmonics, fractal, envelope) say
//  streamable=true and report that lookahead honestly, sized from a 20 Hz
//  worst-case period like the exemplar.  The seven that change length cannot
//  run in a fixed-rate graph at all and say streamable=false.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

//! Hard ceiling on produced length, as a multiple of the input length.  A
//! runaway parameter therefore costs bounded memory, never the machine.
constexpr int64_t kMaxGrowth = 60;

//! The lowest period the lookahead estimates assume, in Hz.  Below this the
//! host would have to buffer more than we promise; 20 Hz is the bottom of
//! hearing and matches cdp_distort.cpp.
constexpr double kLowestHz = 20.0;

//----------------------------------------------------------------------------
//  Waveset boundaries
//----------------------------------------------------------------------------

//! Upward zero-crossing indices of one channel, plus the trailing end so the
//! last (possibly partial) cycle is a closed span.  Identical to
//! cdp_distort.cpp's cycle_bounds -- the family must agree on this.
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

//! HALF-waveset boundaries: every sign change, in either direction.  CDP's
//! mdistort() and distort_pitch() both scan half cycles this way (their
//! `while(b[i]>=0)` / `while(b[i]<=0)` pairs).  Splitting on `x < 0` keeps this
//! consistent with cycle_bounds, whose boundaries are the subset of these where
//! the sign goes negative -> non-negative.
std::vector<int64_t> half_bounds(const std::vector<float>& x) {
    std::vector<int64_t> b;
    const int64_t n = (int64_t)x.size();
    if (n == 0) return b;
    b.push_back(0);
    for (int64_t i = 1; i < n; ++i)
        if ((x[(size_t)(i - 1)] < 0.f) != (x[(size_t)i] < 0.f)) b.push_back(i);
    b.push_back(n);
    return b;
}

//----------------------------------------------------------------------------
//  Small shared helpers
//----------------------------------------------------------------------------

//! Growing output for one channel that refuses to exceed the 60x clamp.
struct Sink {
    std::vector<float> v;
    size_t             cap;
    explicit Sink(int64_t inFrames) {
        const int64_t n = inFrames < 1 ? 1 : inFrames;
        cap = (size_t)(n * kMaxGrowth);
        v.reserve((size_t)std::min<int64_t>(n * 2, (int64_t)cap));
    }
    bool full() const { return v.size() >= cap; }
    void push(float s) { if (v.size() < cap) v.push_back(s); }
    //! Append x[a,b) verbatim, stopping at the clamp.
    void append(const std::vector<float>& x, int64_t a, int64_t b) {
        if (a < 0) a = 0;
        if (b > (int64_t)x.size()) b = (int64_t)x.size();
        for (int64_t i = a; i < b; ++i) {
            if (v.size() >= cap) return;
            v.push_back(x[(size_t)i]);
        }
    }
};

//! Move per-channel results into `out`, padding short channels with silence.
//! Cycle boundaries are found per channel, so two channels can legitimately end
//! up a few samples apart; the buffer has to be rectangular.
void assemble(Buffer& out, int sampleRate, std::vector<std::vector<float>>& chans) {
    size_t maxLen = 0;
    for (const std::vector<float>& c : chans) maxLen = std::max(maxLen, c.size());
    if (maxLen == 0) maxLen = 1;      // never hand a downstream node nothing
    for (std::vector<float>& c : chans) c.resize(maxLen, 0.f);
    out.sampleRate = sampleRate > 0 ? sampleRate : 48000;
    out.ch = std::move(chans);
}

inline double par(const std::vector<double>& p, size_t i, double def) {
    return i < p.size() ? p[i] : def;
}
inline int ipar(const std::vector<double>& p, size_t i, double def, int lo, int hi) {
    const int v = (int)std::lround(par(p, i, def));
    return v < lo ? lo : (v > hi ? hi : v);
}
inline double dpar(const std::vector<double>& p, size_t i, double def,
                   double lo, double hi) {
    const double v = par(p, i, def);
    if (!std::isfinite(v)) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}

//! CDP's cycle "loudness": the sum of absolute sample values over the cycle
//! (distrpl.c accumulates fabs(); distdel.c adds the positive half and
//! subtracts the negative half, which is the same number).
double cycle_loudness(const std::vector<float>& x, int64_t a, int64_t b) {
    double s = 0.0;
    for (int64_t i = a; i < b; ++i) s += std::fabs((double)x[(size_t)i]);
    return s;
}

//! Read x at `start + round(ratio * len)`, fenced to the cycle and the buffer.
//! CDP indexes exactly this way (disttel.c indexed_value, distorta_func) and
//! can land one past the cycle end, which is legal there only because its
//! buffers are over-allocated.
inline float indexed_value(const std::vector<float>& x, int64_t start, int64_t len,
                           double ratio) {
    int64_t i = start + (int64_t)std::lround(ratio * (double)len);
    if (i < start) i = start;
    if (i >= (int64_t)x.size()) i = (int64_t)x.size() - 1;
    if (i < 0) i = 0;
    return x[(size_t)i];
}

//! Local, seeded RNG (xorshift64*).  CDP calls drand48(), i.e. one global
//! stream; renders here have to be reproducible, so every process that needs
//! randomness carries its own.
struct Rng {
    uint64_t s;
    explicit Rng(uint32_t seed)
        : s(6364136223846793005ULL * (uint64_t)seed + 1442695040888963407ULL) {
        if (s == 0) s = 88172645463325252ULL;
        next(); next();
    }
    uint32_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return (uint32_t)((s * 2685821657736338717ULL) >> 32);
    }
    //! Uniform in [0,1), CDP's drand48() equivalent.
    double uniform() { return (double)next() * (1.0 / 4294967296.0); }
};

//! Every process below shares this shape: guard the input, walk the channels,
//! report progress, assemble.  `fn(inChannel, sink)` does the per-channel work.
template <class Fn>
bool for_each_channel(const std::vector<Buffer>& in, Buffer& out, std::string& err,
                      const Progress& prog, Fn fn) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    std::vector<std::vector<float>> chans;
    chans.reserve((size_t)src.channels());
    for (int c = 0; c < src.channels(); ++c) {
        Sink sink(src.frames());
        fn(src.ch[(size_t)c], sink);
        chans.push_back(std::move(sink.v));
        if (prog && !prog((double)(c + 1) / (double)src.channels())) {
            err = "cancelled"; return false;
        }
    }
    assemble(out, src.sampleRate, chans);
    return true;
}

//! In-place variant, for the processes that emit exactly what they consume.
template <class Fn>
bool for_each_channel_inplace(const std::vector<Buffer>& in, Buffer& out,
                              std::string& err, const Progress& prog, Fn fn) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    out = in[0];
    for (int c = 0; c < out.channels(); ++c) {
        fn(out.ch[(size_t)c]);
        if (prog && !prog((double)(c + 1) / (double)out.channels())) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//----------------------------------------------------------------------------
//  DISTORT REPEAT -- distrpt.c, do_repeat()
//----------------------------------------------------------------------------
//! Each group of `Wavesets` cycles is written out `Repeats` times, so the file
//! lengthens by exactly the repeat count.  CDP's version exists only to feed
//! write_samps(); the algorithm is the memmove loop in do_repeat().
bool distort_repeat(const std::vector<Buffer>& in, const std::vector<double>& p,
                    Buffer& out, std::string& err, const Progress& prog) {
    const int mult = ipar(p, 0, 2.0, 1, 32);
    const int grp  = ipar(p, 1, 1.0, 1, 32);
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            const std::vector<int64_t> b = cycle_bounds(x);
            if (b.size() < 2) return;
            for (size_t g = 0; g + 1 < b.size(); g += (size_t)grp) {
                const size_t last = std::min(g + (size_t)grp, b.size() - 1);
                for (int r = 0; r < mult; ++r) {
                    if (sink.full()) return;
                    sink.append(x, b[g], b[last]);
                }
            }
        });
}

//----------------------------------------------------------------------------
//  DISTORT MULTIPLY -- distortm.c, do_multiply() + set_phaseswitch()
//----------------------------------------------------------------------------
//! Each HALF waveset is squeezed to 1/factor of its length by decimation and
//! then laid down `factor` times with alternating polarity, so the pitch rises
//! by `factor` while the file length is untouched.  `remnant` samples (the
//! division leftover) become half-value interpolating pads between the copies,
//! exactly as CDP does.
//!
//! CDP tracks a running `output_phase` across half cycles so the emitted
//! polarity keeps alternating (set_phaseswitch, whose comment admits "it takes
//! a moment to deduce this").  Working it through: with an EVEN factor the
//! phase switch is just the sign of the input half cycle, so the output always
//! starts positive; with an ODD factor the switch never changes at all.  Both
//! branches are reproduced below, which is why odd and even factors sound
//! different on the same source.
void multiply_channel(std::vector<float>& x, int factor, bool smooth) {
    const std::vector<int64_t> hb = half_bounds(x);
    if (hb.size() < 2) return;
    std::vector<float> o;
    for (size_t h = 0; h + 1 < hb.size(); ++h) {
        const int64_t s = hb[h], e = hb[h + 1];
        const int64_t halflen = e - s;
        if (halflen <= 0) continue;

        // CDP finds the half cycle's polarity at its first non-zero sample.
        int64_t marker = s;
        while (marker < e && x[(size_t)marker] == 0.f) ++marker;
        const bool negative = (marker < e) && (x[(size_t)marker] < 0.f);
        const int  ph = ((factor % 2) == 0) ? (negative ? -1 : 1) : 1;

        const int64_t mid = halflen / factor;
        if (mid == 0) {                       // more copies than samples: copy
            if (ph < 0)
                for (int64_t i = s; i < e; ++i) x[(size_t)i] = -x[(size_t)i];
            continue;
        }
        int64_t remnant = halflen - mid * factor;
        o.assign((size_t)halflen, 0.f);

        int64_t pos = 0;
        for (int64_t m = 0; m < mid; ++m) {   // the compressed half cycle
            int64_t idx = (int64_t)std::lround((double)m * (double)factor);
            if (idx > halflen - 1) idx = halflen - 1;
            o[(size_t)pos++] = (float)ph * x[(size_t)(s + idx)];
        }
        if (remnant > 0 && pos < halflen) {   // interpolating pad
            o[(size_t)pos] = o[(size_t)(pos - 1)] * 0.5f;
            ++pos; --remnant;
        }
        for (int k = 1; k < factor; ++k) {    // the repeats, alternating sign
            const int64_t start = pos;
            for (int64_t j = 0; j < mid && start + j < halflen; ++j)
                o[(size_t)(start + j)] = (k % 2) ? -o[(size_t)j] : o[(size_t)j];
            pos = std::min<int64_t>(start + mid, halflen);
            if (remnant > 0 && pos < halflen && pos > 0) {
                o[(size_t)pos] = o[(size_t)(pos - 1)] * 0.5f;
                ++pos; --remnant;
            }
        }
        // CDP's TW MAR:1995 smoothing decays the tail; without the flag its
        // output buffer keeps stale samples there, which we will not copy.
        if (smooth) {
            while (pos < halflen && pos > 0) {
                o[(size_t)pos] = o[(size_t)(pos - 1)] * 0.5f;
                ++pos;
            }
        }
        for (int64_t i = 0; i < halflen; ++i) x[(size_t)(s + i)] = o[(size_t)i];
    }
}

bool distort_multiply(const std::vector<Buffer>& in, const std::vector<double>& p,
                      Buffer& out, std::string& err, const Progress& prog) {
    const int  factor = ipar(p, 0, 2.0, 1, 32);
    const bool smooth = par(p, 1, 1.0) >= 0.5;
    return for_each_channel_inplace(in, out, err, prog,
        [&](std::vector<float>& x) { multiply_channel(x, factor, smooth); });
}

//----------------------------------------------------------------------------
//  DISTORT AVERAGE -- distorta.c, distorta_func() + get_cyc_average()
//----------------------------------------------------------------------------
//! Every group of N cycles is replaced by N copies of their AVERAGE cycle: the
//! group's mean length, filled by reading all N cycles at the same fractional
//! position and averaging.  Length is preserved to within the rounding of the
//! mean, which is why this one cannot stream.
bool distort_average(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    const int cnt = ipar(p, 0, 4.0, 1, 64);
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            const std::vector<int64_t> b = cycle_bounds(x);
            if (b.size() < 2) return;
            const size_t ncyc = b.size() - 1;
            std::vector<float> cyc;
            for (size_t g = 0; g < ncyc; g += (size_t)cnt) {
                const size_t k = std::min((size_t)cnt, ncyc - g);
                int64_t total = 0;
                for (size_t i = 0; i < k; ++i) total += b[g + i + 1] - b[g + i];
                int64_t avg = (int64_t)std::lround((double)total / (double)k);
                if (avg < 1) avg = 1;
                cyc.assign((size_t)avg, 0.f);
                for (int64_t m = 0; m < avg; ++m) {
                    const double zorg = (double)m / (double)avg;
                    double sum = 0.0;
                    for (size_t i = 0; i < k; ++i)
                        sum += (double)indexed_value(x, b[g + i],
                                                     b[g + i + 1] - b[g + i], zorg);
                    cyc[(size_t)m] = (float)(sum / (double)k);
                }
                for (size_t i = 0; i < k; ++i) {
                    if (sink.full()) return;
                    sink.append(cyc, 0, avg);
                }
            }
        });
}

//----------------------------------------------------------------------------
//  DISTORT TELESCOPE -- disttel.c, do_cycle_tele() + get_longcycle/meancycle
//----------------------------------------------------------------------------
//! A group of N cycles collapses to ONE cycle, whose samples are the average of
//! all N read at the same fractional position.  The output cycle is as long as
//! the longest of the group, or their mean if `Length` says so (CDP's
//! -a flag / IS_DISTTEL_AVG).  The file shortens by roughly N.
bool distort_telescope(const std::vector<Buffer>& in, const std::vector<double>& p,
                       Buffer& out, std::string& err, const Progress& prog) {
    const int cnt  = ipar(p, 0, 4.0, 2, 64);
    const int mode = ipar(p, 1, 0.0, 0, 1);     // 0 longest, 1 mean
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            const std::vector<int64_t> b = cycle_bounds(x);
            if (b.size() < 2) return;
            const size_t ncyc = b.size() - 1;
            for (size_t g = 0; g < ncyc; g += (size_t)cnt) {
                const size_t k = std::min((size_t)cnt, ncyc - g);
                int64_t reflen = 0, total = 0;
                for (size_t i = 0; i < k; ++i) {
                    const int64_t len = b[g + i + 1] - b[g + i];
                    total += len;
                    reflen = std::max(reflen, len);
                }
                if (mode == 1) reflen = (int64_t)std::lround((double)total / (double)k);
                if (reflen < 1) continue;
                for (int64_t j = 0; j < reflen; ++j) {
                    if (sink.full()) return;
                    const double ratio = (double)j / (double)reflen;
                    double sum = 0.0;
                    for (size_t i = 0; i < k; ++i)
                        sum += (double)indexed_value(x, b[g + i],
                                                     b[g + i + 1] - b[g + i], ratio);
                    sink.push((float)(sum / (double)k));
                }
            }
        });
}

//----------------------------------------------------------------------------
//  DISTORT REPLACE -- distrpl.c, do_cycle_loudrep() + get_ffcycle()
//----------------------------------------------------------------------------
//! Every group of N cycles is replaced by N copies of the LOUDEST cycle in that
//! group -- the local peak eats its neighbours.  Because the copies inherit the
//! loudest cycle's length rather than the group's, the file's length drifts.
bool distort_replace(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    const int cnt = ipar(p, 0, 4.0, 2, 64);
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            const std::vector<int64_t> b = cycle_bounds(x);
            if (b.size() < 2) return;
            const size_t ncyc = b.size() - 1;
            for (size_t g = 0; g < ncyc; g += (size_t)cnt) {
                const size_t k = std::min((size_t)cnt, ncyc - g);
                size_t loudest = 0;
                double best = 0.0;              // CDP starts its max at zero
                for (size_t i = 0; i < k; ++i) {
                    const double e = cycle_loudness(x, b[g + i], b[g + i + 1]);
                    if (e > best) { best = e; loudest = i; }
                }
                for (size_t i = 0; i < k; ++i) {
                    if (sink.full()) return;
                    sink.append(x, b[g + loudest], b[g + loudest + 1]);
                }
            }
        });
}

//----------------------------------------------------------------------------
//  DISTORT HARMONICS -- distorth.c, do_harmonic()
//----------------------------------------------------------------------------
//! Each waveset has copies of ITSELF, read at 2x, 3x ... the rate and wrapped
//! round the cycle, added on top: harmonics of whatever that waveset happens to
//! be, however inharmonic the source.  CDP reads the harmonic numbers and their
//! amplitudes from a text file; there is no file here, so the series is
//! generated -- harmonics 2..count+1, amplitude `Amplitude * Rolloff^(n-2)`.
//! The foldover guard is CDP's: a harmonic at or above cyclelen/4 is skipped.
void harmonics_channel(std::vector<float>& x, int count, double amp, double rolloff) {
    const std::vector<int64_t> b = cycle_bounds(x);
    if (b.size() < 2) return;
    std::vector<float> cyc;
    for (size_t g = 0; g + 1 < b.size(); ++g) {
        const int64_t s = b[g], e = b[g + 1], len = e - s;
        if (len < 4) continue;
        cyc.assign(x.begin() + (ptrdiff_t)s, x.begin() + (ptrdiff_t)e);
        double a = amp;
        for (int h = 0; h < count; ++h, a *= rolloff) {
            const int64_t hno = h + 2;               // 2nd harmonic upwards
            if (hno >= len / 4) continue;            // foldover
            if (a == 0.0) break;
            int64_t i = hno;
            for (int64_t j = s; j < e; ++j) {
                x[(size_t)j] = (float)((double)x[(size_t)j] +
                                       (double)cyc[(size_t)i] * a);
                i += hno;
                while (i >= len) i -= len;
            }
        }
    }
}

bool distort_harmonics(const std::vector<Buffer>& in, const std::vector<double>& p,
                       Buffer& out, std::string& err, const Progress& prog) {
    const int    count   = ipar(p, 0, 3.0, 1, 16);
    const double amp     = dpar(p, 1, 0.3, 0.0, 1.0);
    const double rolloff = dpar(p, 2, 0.6, 0.0, 1.0);
    return for_each_channel_inplace(in, out, err, prog,
        [&](std::vector<float>& x) { harmonics_channel(x, count, amp, rolloff); });
}

//----------------------------------------------------------------------------
//  DISTORT FRACTAL -- distortf.c, do_fractal() + get_scaled_halfcycle()
//----------------------------------------------------------------------------
//! "Distort file by adding smaller scale perturbations modelled on large."  A
//! group of `Scale` wavesets is decimated by `Scale` -- every Nth sample kept --
//! giving a miniature of the whole group; that miniature is then stretched onto
//! each individual waveset of the group and added in.  The shape of the group
//! reappears inside each of its own cycles.
void fractal_channel(std::vector<float>& x, int scale, double amp) {
    const std::vector<int64_t> b = cycle_bounds(x);
    if (b.size() < 2) return;
    const size_t ncyc = b.size() - 1;
    std::vector<float> mini;
    for (size_t g = 0; g < ncyc; g += (size_t)scale) {
        const size_t k = std::min((size_t)scale, ncyc - g);
        const int64_t gs = b[g], ge = b[g + k];
        // Collect one sample in every `scale`, exactly as CDP's counter does:
        // ++cnt, and on cnt >= scale store the CURRENT sample and reset.
        mini.clear();
        int cnt = 0;
        for (int64_t i = gs; i < ge; ++i)
            if (++cnt >= scale) { mini.push_back(x[(size_t)i]); cnt = 0; }
        if (mini.empty()) continue;
        const int64_t mlen = (int64_t)mini.size();
        for (size_t i = 0; i < k; ++i) {
            const int64_t cs = b[g + i], ce = b[g + i + 1];
            const int64_t len = ce - cs;
            if (len <= 0) continue;
            const double ratio = (double)mlen / (double)len;
            int64_t kk = 0;
            for (int64_t j = cs; j < ce; ++j) {
                int64_t idx = (int64_t)std::lround(ratio * (double)kk++);
                if (idx < 0) idx = 0;
                if (idx > mlen - 1) idx = mlen - 1;
                x[(size_t)j] = (float)((double)x[(size_t)j] +
                                       amp * (double)mini[(size_t)idx]);
            }
        }
    }
}

bool distort_fractal(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    const int    scale = ipar(p, 0, 4.0, 2, 32);
    const double amp   = dpar(p, 1, 0.5, 0.0, 2.0);
    return for_each_channel_inplace(in, out, err, prog,
        [&](std::vector<float>& x) { fractal_channel(x, scale, amp); });
}

//----------------------------------------------------------------------------
//  DISTORT ENVELOPE -- distorte.c, distorte_rising/falling/troffed + do_*_trof
//----------------------------------------------------------------------------
//! An amplitude shape is imposed over every group of N wavesets, so the shape
//! repeats at the waveform's own period and reads as timbre rather than
//! dynamics.  CDP has ELEVEN modes, but they are three shapes crossed with two
//! options:
//!
//!     rising   z = (k/len)^expon                 [DISTORTE_RISING]
//!     falling  z = (1 - k/len)^expon             [DISTORTE_FALLING]
//!     trough   z = (1 - k/half)^expon rising to  [DISTORTE_TROFFED]
//!              z = (k/half)^expon in the 2nd half
//!     then, for the _TR modes,   z = z*(1-trof) + trof
//!     and the LIN* modes are the same with expon = 1.
//!
//! With Exponent 1 and Trough 0 this reproduces LINRISE/LINFALL/LINTROF; with
//! Trough 0 it reproduces RISING/FALLING/TROFFED; with both it reproduces the
//! four *_TR modes.  Only DISTORTE_USERDEF is missing -- it reads a breakpoint
//! envelope from a file, which is not expressible as a ParamSpec.
void envelope_channel(std::vector<float>& x, int cnt, int shape, double expon,
                      double trof) {
    const std::vector<int64_t> b = cycle_bounds(x);
    if (b.size() < 2) return;
    const size_t ncyc = b.size() - 1;
    const double oneLess = 1.0 - trof;
    for (size_t g = 0; g < ncyc; g += (size_t)cnt) {
        const size_t k = std::min((size_t)cnt, ncyc - g);
        const int64_t s = b[g], e = b[g + k], len = e - s;
        if (len <= 0) continue;
        if (shape == 2 && len >= 2) {                       // troughed
            const int64_t half = len / 2;
            const int64_t mid  = s + half;
            for (int64_t j = s, i = 0; j < mid; ++j, ++i) {
                double z = 1.0 - (double)i / (double)half;
                z = std::pow(z < 0.0 ? 0.0 : z, expon) * oneLess + trof;
                x[(size_t)j] = (float)((double)x[(size_t)j] * z);
            }
            const int64_t half2 = len - half;
            for (int64_t j = mid, i = 0; j < e; ++j, ++i) {
                double z = (double)i / (double)half2;
                z = std::pow(z < 0.0 ? 0.0 : z, expon) * oneLess + trof;
                x[(size_t)j] = (float)((double)x[(size_t)j] * z);
            }
        } else {
            for (int64_t j = s, i = 0; j < e; ++j, ++i) {
                double z = (double)i / (double)len;
                if (shape == 1) z = 1.0 - z;                 // falling
                z = std::pow(z < 0.0 ? 0.0 : z, expon) * oneLess + trof;
                x[(size_t)j] = (float)((double)x[(size_t)j] * z);
            }
        }
    }
}

bool distort_envelope(const std::vector<Buffer>& in, const std::vector<double>& p,
                      Buffer& out, std::string& err, const Progress& prog) {
    const int    cnt   = ipar(p, 0, 1.0, 1, 32);
    const int    shape = ipar(p, 1, 0.0, 0, 2);
    const double expon = dpar(p, 2, 1.0, 0.05, 8.0);
    const double trof  = dpar(p, 3, 0.0, 0.0, 1.0);
    return for_each_channel_inplace(in, out, err, prog,
        [&](std::vector<float>& x) { envelope_channel(x, cnt, shape, expon, trof); });
}

//----------------------------------------------------------------------------
//  DISTORT DELETE -- distdel.c, distort_del() / do_cycle_loud() / do_cycle_quiet()
//----------------------------------------------------------------------------
//! Wavesets are thrown away rather than silenced (that is distort.omit), so the
//! file gets shorter and the pitch rises.  CDP splits this across two entry
//! points and a mode flag; all three survive here as `Mode`:
//!     0  Keep first   -- distort_del(): keep one cycle, lose the next N-1.
//!     1  Keep loudest -- do_cycle_loud(), CDP mode KEEP_STRONGEST.
//!     2  Drop quietest-- do_cycle_quiet(), CDP mode DELETE_WEAKEST.
bool distort_delete(const std::vector<Buffer>& in, const std::vector<double>& p,
                    Buffer& out, std::string& err, const Progress& prog) {
    const int grp  = ipar(p, 0, 2.0, 2, 64);
    const int mode = ipar(p, 1, 0.0, 0, 2);
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            const std::vector<int64_t> b = cycle_bounds(x);
            if (b.size() < 2) return;
            const size_t ncyc = b.size() - 1;
            for (size_t g = 0; g < ncyc; g += (size_t)grp) {
                if (sink.full()) return;
                const size_t k = std::min((size_t)grp, ncyc - g);
                if (mode == 0) {                       // keep the first
                    sink.append(x, b[g], b[g + 1]);
                    continue;
                }
                size_t pick = 0;
                double best = cycle_loudness(x, b[g], b[g + 1]);
                for (size_t i = 1; i < k; ++i) {
                    const double e = cycle_loudness(x, b[g + i], b[g + i + 1]);
                    if (mode == 1 ? (e > best) : (e < best)) { best = e; pick = i; }
                }
                if (mode == 1) {                       // keep only the loudest
                    sink.append(x, b[g + pick], b[g + pick + 1]);
                } else {                               // drop only the quietest
                    for (size_t i = 0; i < k; ++i) {
                        if (i == pick) continue;
                        if (sink.full()) return;
                        sink.append(x, b[g + i], b[g + i + 1]);
                    }
                }
            }
        });
}

//----------------------------------------------------------------------------
//  DISTORT FILTER -- distflt.c, is_filtered_out()
//----------------------------------------------------------------------------
//! A filter built out of nothing but waveset LENGTH: a cycle of L samples is
//! treated as a component at sr/L Hz, and cycles outside the band are dropped
//! from the output altogether.  Nothing is smoothed and nothing is convolved,
//! so it sounds nothing like a real filter -- which is the point.
bool distort_filter(const std::vector<Buffer>& in, const std::vector<double>& p,
                    Buffer& out, std::string& err, const Progress& prog) {
    const double sr = in.empty() || in[0].sampleRate <= 0 ? 48000.0
                                                          : (double)in[0].sampleRate;
    double lo = dpar(p, 0, 40.0, 1.0, 20000.0);
    double hi = dpar(p, 1, 8000.0, 1.0, 20000.0);
    if (lo > hi) std::swap(lo, hi);
    const int mode = ipar(p, 2, 0.0, 0, 2);     // 0 band, 1 high, 2 low
    // CDP precomputes these as DISTFLT_LOFRQ_CYCLELEN / DISTFLT_HIFRQ_CYCLELEN.
    const int64_t longest  = (int64_t)std::lround(sr / lo);   // low frequency
    const int64_t shortest = (int64_t)std::lround(sr / hi);   // high frequency
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            const std::vector<int64_t> b = cycle_bounds(x);
            for (size_t g = 0; g + 1 < b.size(); ++g) {
                if (sink.full()) return;
                const int64_t len = b[g + 1] - b[g];
                bool drop = false;
                if (mode == 0) drop = (len < shortest) || (len > longest);
                else if (mode == 1) drop = (len > longest);
                else               drop = (len < shortest);
                if (!drop) sink.append(x, b[g], b[g + 1]);
            }
        });
}

//----------------------------------------------------------------------------
//  DISTORT PITCH -- distortp.c, get_distort() + do_distrt()
//----------------------------------------------------------------------------
//! Each HALF waveset is resampled to a randomly transposed length, with the
//! transposition drifting linearly towards a new random target every 1..N half
//! cycles.  The number of half cycles is unchanged, so the waveform stays
//! recognisable while its pitch staggers.
//!
//! CDP uses drand48(), a single global stream that makes two renders of the
//! same edit differ.  Here the generator is local and seeded, so a render is
//! reproducible; the sequence of draws matches CDP's get_distort() exactly:
//! one for the segment length, one for the next transposition target.
//!
//! The resampler is CDP's do_distrt() verbatim, guard point and all: the
//! previous half cycle's last sample sits at index 0 of the store so the
//! interpolation at the very start of a cycle has something to lean on.
void pitch_channel(const std::vector<float>& x, Sink& sink, double range,
                   int group, uint32_t seed) {
    const std::vector<int64_t> hb = half_bounds(x);
    if (hb.size() < 2) return;
    Rng rng(seed);
    double last = 0.0, next = 0.0, step = 0.0;
    int    pos = 0, seglen = 0;
    bool   init = true;
    float  guard = 0.f;
    for (size_t h = 0; h + 1 < hb.size(); ++h) {
        const int64_t s = hb[h], e = hb[h + 1], len = e - s;
        if (len <= 0) continue;
        if (sink.full()) return;

        if (init) {                                   // CDP get_distort(), init
            seglen = (int)std::lround(rng.uniform() * (double)group) + 1;
            next   = (rng.uniform() * 2.0 - 1.0) * range;
            step   = (next - last) / (double)seglen;
            pos    = 0;
            init   = false;
        } else if (++pos < seglen) {
            last += step;
        } else {
            seglen = (int)std::lround(rng.uniform() * (double)group) + 1;
            last   = next;
            next   = (rng.uniform() * 2.0 - 1.0) * range;
            step   = (next - last) / (double)seglen;
            pos    = 0;
        }
        int64_t newlen = (int64_t)std::lround((double)len * std::pow(2.0, last));
        if (newlen < 1) newlen = 1;

        if (newlen == len) {
            sink.append(x, s, e);
        } else {
            const double stp = (double)len / (double)newlen;
            double here = stp;
            while ((int64_t)here < len) {
                const int64_t k = (int64_t)here;
                const float thisin = (k == 0) ? guard : x[(size_t)(s + k - 1)];
                const float nextin = x[(size_t)(s + k)];
                const double ratio = here - (double)k;
                if (sink.full()) return;
                sink.push((float)(((double)nextin - (double)thisin) * ratio +
                                  (double)thisin));
                here += stp;
            }
        }
        guard = x[(size_t)(e - 1)];
    }
}

bool distort_pitch(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    const double   range = dpar(p, 0, 0.3, 0.0, 2.0);
    const int      group = ipar(p, 1, 4.0, 1, 64);
    const uint32_t seed  = (uint32_t)ipar(p, 2, 1.0, 0, 65535);
    return for_each_channel(in, out, err, prog,
        [&](const std::vector<float>& x, Sink& sink) {
            pitch_channel(x, sink, range, group, seed);
        });
}

//----------------------------------------------------------------------------
//  Registration
//----------------------------------------------------------------------------

//! Worst-case lookahead for a process that must see `groups` wavesets before it
//! can emit anything, assuming nothing slower than kLowestHz.  Same reasoning as
//! cdp_distort.cpp: there is no fixed frame count, it depends on the signal's
//! own period, so this is what the host should reserve.
int64_t waveset_latency(int sr, double groups) {
    const double rate = sr > 0 ? (double)sr : 48000.0;
    return (int64_t)(std::max(1.0, groups) * rate / kLowestHz);
}

} // namespace

void register_distort2_processes() {
    std::vector<Process>& r = mutable_registry();

    {   // ---- repeat ---------------------------------------------------------
        Process p;
        p.slug  = "distort.repeat";
        p.name  = "Distort Repeat";
        p.group = "Distort";
        p.help  = "Repeat each group of wavesets, stuttering and lengthening the sound.";
        // Emits more frames than it consumes, so it cannot run in a fixed-rate
        // graph at all.
        p.streamable = false;
        p.params = {
            { "Repeats",  "", 1, 32, 2, true, "How many times each group is written out." },
            { "Wavesets", "", 1, 32, 1, true, "Wavesets per repeated group." },
        };
        p.run = &distort_repeat;
        r.push_back(p);
    }
    {   // ---- multiply -------------------------------------------------------
        Process p;
        p.slug  = "distort.multiply";
        p.name  = "Distort Multiply";
        p.group = "Distort";
        p.help  = "Squeeze each half waveset and repeat it, multiplying the pitch.";
        p.streamable = true;
        p.latencyFrames = [](int sr, const std::vector<double>&) -> int64_t {
            return waveset_latency(sr, 1.0);      // one waveset of lookahead
        };
        p.params = {
            { "Factor", "", 1, 32, 2, true,
              "Copies of the squeezed half waveset; the pitch rises by this." },
            { "Smooth", "", 0, 1, 1, true,
              "Decay the leftover tail of each waveset instead of leaving it bare." },
        };
        p.run = &distort_multiply;
        r.push_back(p);
    }
    {   // ---- average --------------------------------------------------------
        Process p;
        p.slug  = "distort.average";
        p.name  = "Distort Average";
        p.group = "Distort";
        p.help  = "Replace each group of wavesets by copies of their average waveset.";
        p.streamable = false;      // mean cycle length rounds; length drifts
        p.params = {
            { "Wavesets", "", 1, 64, 4, true, "Wavesets averaged together." },
        };
        p.run = &distort_average;
        r.push_back(p);
    }
    {   // ---- telescope ------------------------------------------------------
        Process p;
        p.slug  = "distort.telescope";
        p.name  = "Distort Telescope";
        p.group = "Distort";
        p.help  = "Collapse each group of wavesets into one, shortening the sound.";
        p.streamable = false;
        p.params = {
            { "Wavesets", "", 2, 64, 4, true, "Wavesets collapsed into one." },
            { "Length",   "", 0, 1, 0, true,
              "0 = as long as the longest waveset, 1 = their mean length." },
        };
        p.run = &distort_telescope;
        r.push_back(p);
    }
    {   // ---- replace --------------------------------------------------------
        Process p;
        p.slug  = "distort.replace";
        p.name  = "Distort Replace";
        p.group = "Distort";
        p.help  = "Replace every group of wavesets by copies of its loudest one.";
        p.streamable = false;      // copies inherit the loudest cycle's length
        p.params = {
            { "Wavesets", "", 2, 64, 4, true,
              "Wavesets per group; the loudest replaces the rest." },
        };
        p.run = &distort_replace;
        r.push_back(p);
    }
    {   // ---- harmonics ------------------------------------------------------
        Process p;
        p.slug  = "distort.harmonics";
        p.name  = "Distort Harmonics";
        p.group = "Distort";
        p.help  = "Add harmonics of each waveset back onto itself.";
        p.streamable = true;
        p.latencyFrames = [](int sr, const std::vector<double>&) -> int64_t {
            return waveset_latency(sr, 1.0);
        };
        p.params = {
            { "Harmonics", "", 1, 16, 3, true,
              "How many harmonics are added, starting at the 2nd." },
            { "Amplitude", "", 0.0, 1.0, 0.3, false, "Level of the first harmonic." },
            { "Rolloff",   "", 0.0, 1.0, 0.6, false,
              "Each harmonic is this much quieter than the one below." },
        };
        p.run = &distort_harmonics;
        r.push_back(p);
    }
    {   // ---- fractal --------------------------------------------------------
        Process p;
        p.slug  = "distort.fractal";
        p.name  = "Distort Fractal";
        p.group = "Distort";
        p.help  = "Add a miniature of each waveset group onto every waveset in it.";
        p.streamable = true;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) -> int64_t {
            return waveset_latency(sr, pr.empty() ? 4.0 : pr[0]);
        };
        p.params = {
            { "Scale",     "", 2, 32, 4, true,
              "Wavesets per group, and the factor the miniature is shrunk by." },
            { "Amplitude", "", 0.0, 2.0, 0.5, false, "Level of the added miniature." },
        };
        p.run = &distort_fractal;
        r.push_back(p);
    }
    {   // ---- envelope -------------------------------------------------------
        Process p;
        p.slug  = "distort.envelope";
        p.name  = "Distort Envelope";
        p.group = "Distort";
        p.help  = "Impose an amplitude shape over every group of wavesets.";
        p.streamable = true;
        p.latencyFrames = [](int sr, const std::vector<double>& pr) -> int64_t {
            return waveset_latency(sr, pr.empty() ? 1.0 : pr[0]);
        };
        p.params = {
            { "Wavesets", "", 1, 32, 1, true, "Wavesets the shape spans." },
            { "Shape",    "", 0, 2, 0, true, "0 = rising, 1 = falling, 2 = troughed." },
            { "Exponent", "", 0.05, 8.0, 1.0, false,
              "Curve of the shape; 1 is a straight line." },
            { "Trough",   "", 0.0, 1.0, 0.0, false,
              "Floor the shape never drops below." },
        };
        p.run = &distort_envelope;
        r.push_back(p);
    }
    {   // ---- delete ---------------------------------------------------------
        Process p;
        p.slug  = "distort.delete";
        p.name  = "Distort Delete";
        p.group = "Distort";
        p.help  = "Throw wavesets away, shortening the sound and raising its pitch.";
        p.streamable = false;
        p.params = {
            { "Wavesets", "", 2, 64, 2, true, "Wavesets examined per group." },
            { "Mode",     "", 0, 2, 0, true,
              "0 = keep the first, 1 = keep the loudest, 2 = drop the quietest." },
        };
        p.run = &distort_delete;
        r.push_back(p);
    }
    {   // ---- filter ---------------------------------------------------------
        Process p;
        p.slug  = "distort.filter";
        p.name  = "Distort Filter";
        p.group = "Distort";
        p.help  = "Keep only wavesets whose length falls in a frequency band.";
        p.streamable = false;
        p.params = {
            { "Low",  "Hz", 1.0, 20000.0, 40.0, false,
              "Wavesets longer than one cycle of this are dropped." },
            { "High", "Hz", 1.0, 20000.0, 8000.0, false,
              "Wavesets shorter than one cycle of this are dropped." },
            { "Mode", "", 0, 2, 0, true,
              "0 = band-pass, 1 = high-pass, 2 = low-pass." },
        };
        p.run = &distort_filter;
        r.push_back(p);
    }
    {   // ---- pitch ----------------------------------------------------------
        Process p;
        p.slug  = "distort.pitch";
        p.name  = "Distort Pitch";
        p.group = "Distort";
        p.help  = "Randomly transpose each half waveset by resampling it.";
        p.streamable = false;      // resampling changes how many frames come out
        p.params = {
            { "Range", "octaves", 0.0, 2.0, 0.3, false,
              "How far the random transposition wanders." },
            { "Group", "", 1, 64, 4, true,
              "Half wavesets each drift towards a new random target." },
            { "Seed",  "", 0, 65535, 1, true,
              "Random seed; the same seed always renders the same result." },
        };
        p.run = &distort_pitch;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
