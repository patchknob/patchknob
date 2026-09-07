//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_modify.cpp
//
//  Ported from the CDP MODIFY family (vendor/cdp8/dev/modify), which is
//      Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Specifically:
//      gain.c      gain() / do_normalise() / do_phase_invert() /
//                  get_normalisation_gain() / find_maxsamp()   -- `modify loudness`
//      strans.c    strans_process() and its read_wrapped_samps_mono/stereo
//                  guard-point trick                           -- `modify speed 1/2`
//                  do_vibrato() + interp_read_sintable() +
//                  vtrans_preprocess()'s vibrato table setup   -- `modify speed 6`
//      radical.c   do_lowbit() + lobit_pconsistency()          -- `modify radical 4`
//                  ring_modulate() + read_rmod_tab() +
//                  create_rm_sintab()                          -- `modify radical 5`
//                  shred() + get_basis_lengths() + normal_scat() + heavy_scat() +
//                  permute_chunks()/insert()/prefix()/shuflup() +
//                  do_startsplice()/do_endsplice()/do_bufend_splices() +
//                  ptr_sort() + shred_pconsistency()           -- `modify radical 2`
//                  do_stack() + stack_preprocess() +
//                  get_obuf_sampoffset() +
//                  transpos_and_add_to_obuf()                  -- `modify stack`
//      ap_modify.c the usage text every ParamSpec help line is condensed from,
//                  and semitone_to_ratio()
//      cdp2k/tklib1.c  dbtogain(), and the lo/hi/default tables that fix each
//                  parameter's range (ap->lo/ap->hi/default_val)
//
//  WHAT WAS WELDED ON, AND HAS BEEN DROPPED.  Four layers, none of them DSP:
//
//   1. DISK BUFFERING.  Every one of these runs over dz->sampbuf[] one disk
//      buffer at a time, and a surprising amount of the source is boundary
//      repair rather than algorithm.  strans/vibrato keep a GUARD POINT: the
//      last sample of the previous buffer is copied to the slot immediately
//      before the new one so that `true_lbuf[place+1]` can always be read --
//      which is why strans_process starts its read pointer at 1.0, not 0.0.
//      Over one contiguous buffer that offset is exactly "sample 0", and the
//      guard is only real at the very start of the file, where it is silence.
//      shred re-derives its chunk count per buffer (SHR_LAST_CHCNT and friends)
//      and warns the user that a long file gets shredded in independent
//      sectors; with the whole clip in memory there is one sector, which is the
//      case CDP's own documentation calls the good one.  stack seeks the input
//      file once per layer per output buffer; here each layer just walks the
//      input array.
//
//   2. FILE FORMAT.  CDP's sample values are floats but its arithmetic still
//      remembers 16-bit files: gain() counts and clips samples over F_MAXSAMP
//      (== 1.0) unless the output is float, do_lowbit switches maxsampval and
//      the bit divisor on the header's samptype, and normalise measures against
//      MAXSHORT.  We are always float, so the FLOAT32 branch is the one ported
//      and the clip counter -- which only ever produced a warning string -- is
//      gone.  Nothing is hard-clipped at 1.0; a float host has headroom.
//
//   3. BREAKPOINT AUTOMATION.  loudness, ring modulation and vibrato can all
//      take a time-varying parameter file, read through read_value_from_brktable
//      every RING_MOD_BLOK/VIB_BLOKSIZE samples and interpolated (ring modulation
//      interpolates in log10 of frequency, vibrato linearly).  ParamSpec is a
//      scalar, so the constant-parameter branch is the one ported -- with a
//      constant table the interpolators produce exactly the constant, so the
//      two agree sample for sample.  A host that wants motion automates the knob.
//
//   4. INFO MODES.  `modify speed 3` and `4` print the output duration and
//      write no audio at all (strans_process's MOD_TRANSPOS_INFO cases), so
//      they have no meaning against a Buffer-in/Buffer-out contract.
//
//  CHANNELS.  CDP works on the interleaved buffer, so a cut, a chunk boundary
//  or a read position is shared by every channel and only the sample values
//  differ.  That is preserved: shred permutes the same frame ranges in all
//  channels, ring modulation drives one oscillator into all channels, speed and
//  stack read every channel at one position, and normalise takes a single peak
//  across the whole buffer.  Per-channel state exists only where CDP has it.
//
//  ZERO LATENCY.  Two of these are genuinely sample-by-sample -- a gain and a
//  multiplication by an oscillator -- so they carry makeStream() and the
//  generated rack module runs them with no added delay.  Everything else either
//  looks ahead (lobit averages a block before it can emit its first sample) or
//  cannot stream at all: speed, vibrato and stack re-time the material so the
//  output is a different length from the input, normalise has to scan the whole
//  clip before it knows its gain, and shred permutes across the entire buffer.
//  Those say so honestly through `streamable` and `latencyFrames`.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

// CDP's own constants, from dev/include/{globcon,modicon,txtucon}.h.
constexpr double kOctavesPerSemitone = 0.08333333333;   // globcon.h, as written
constexpr int    kMaxBitDiv    = 16;                    // modicon.h MAX_BIT_DIV
constexpr int    kMaxSrateDiv  = 256;                   // modicon.h MAX_SRATE_DIV
constexpr int    kShredSplice  = 256;                   // modicon.h SHRED_SPLICELEN
constexpr int    kRmSinTabSize = 512;                   // modicon.h RM_SINTABSIZE
constexpr int    kVibTabSize   = 1024;                  // modicon.h VIB_TABSIZE

inline double at(const std::vector<double>& p, size_t i, double d) {
    return i < p.size() ? p[i] : d;
}
inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline int64_t clampi(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

//! tklib1.c dbtogain().  -96 dB (MIN_DB_ON_16_BIT) is CDP's floor and means
//! silence outright, not a very small number.
double db_to_gain(double db) {
    if (db <= -96.0) return 0.0;
    if (std::fabs(db) < 1e-12) return 1.0;
    const double v = std::pow(10.0, std::fabs(db) / 20.0);
    return db < 0.0 ? 1.0 / v : v;
}

//! ap_modify.c semitone_to_ratio().
inline double semitone_to_ratio(double s) { return std::pow(2.0, s / 12.0); }

//! CDP's sample interpolation, which every re-timing process here shares
//! (strans.c strans_process/do_vibrato, radical.c transpos_and_add_to_obuf):
//! truncate to an integer index, take the step to the NEXT sample, add
//! frac*step.  Out-of-range reads give silence, which is what the guard point
//! holds at the start of a file.
inline double lerp_read(const std::vector<float>& x, double pos) {
    const int64_t n = (int64_t)x.size();
    if (n == 0) return 0.0;
    const int64_t i = (int64_t)std::floor(pos);
    const double  frac = pos - (double)i;
    const double  a = (i < 0 || i >= n) ? 0.0 : (double)x[(size_t)i];
    const double  b = (i + 1 < 0 || i + 1 >= n) ? 0.0 : (double)x[(size_t)(i + 1)];
    return a + frac * (b - a);
}

//! CDP shuffles with drand48() seeded by initrand48() -- i.e. from the clock,
//! so `shred` never renders the same way twice.  An offline editor with undo
//! needs a render to be repeatable, so the generator is the SAME one (drand48's
//! 48-bit LCG, reproduced exactly) but seeded from a parameter.
struct Drand48 {
    uint64_t x = 0;
    void seed(uint32_t s) { x = (((uint64_t)s) << 16) | 0x330EULL; }
    double next() {
        x = (0x5DEECE66DULL * x + 0xBULL) & 0xFFFFFFFFFFFFULL;
        return (double)x / 281474976710656.0;   // 2^48
    }
};

inline bool tick(const Progress& prog, double f, std::string& err) {
    if (prog && !prog(f)) { err = "cancelled"; return false; }
    return true;
}

//============================================================================
//  LOUDNESS  --  gain.c
//============================================================================

//! gain.c gain() (LOUDNESS_DBGAIN, after loudness_process has passed the
//! parameter through dbtogain) plus do_phase_invert() (LOUDNESS_PHASE), which
//! is the same loop with a multiplier of -1.  CDP counts and clips samples that
//! pass F_MAXSAMP; that only applied to integer output files and is dropped.
struct LoudStream final : Stream {
    double g = 1.0;
    void process(float* const* ch, int channels, int n) override {
        for (int c = 0; c < channels; ++c) {
            float* x = ch[c];
            for (int i = 0; i < n; ++i) x[i] = (float)((double)x[i] * g);
        }
    }
    void reset() override {}      // a gain has no state to clear
};

double loudness_gain(const std::vector<double>& p) {
    const double db = clampd(at(p, 0, 0.0), -96.0, 96.0);
    double g = db_to_gain(db);
    if (at(p, 1, 0.0) >= 0.5) g = -g;             // LOUDNESS_PHASE
    return g;
}

bool loudness_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                  Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    out = in[0];
    const double g = loudness_gain(p);
    for (int c = 0; c < out.channels(); ++c) {
        std::vector<float>& x = out.ch[(size_t)c];
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)((double)x[i] * g);
        if (!tick(prog, (double)(c + 1) / out.channels(), err)) return false;
    }
    return true;
}

//! gain.c get_normalisation_gain() + do_normalise().  CDP has two modes:
//! LOUDNESS_NORM refuses when the file is ALREADY louder than the target, and
//! LOUDNESS_SET just sets it either way.  SET is the one ported -- a node that
//! errors out depending on what is plugged into it is useless in a graph.
//! find_max() scans the interleaved buffer, so the peak is over all channels
//! together and one gain is applied to all of them; that is preserved.
bool normalise_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const double level = clampd(at(p, 0, 0.9), 1.0 / 32767.0, 1.0);
    out = in[0];
    double peak = 0.0;
    for (int c = 0; c < out.channels(); ++c)
        for (const float v : out.ch[(size_t)c]) peak = std::max(peak, (double)std::fabs(v));
    if (!tick(prog, 0.5, err)) return false;
    // Silence cannot be normalised (CDP divides by maxfound).  Pass it through
    // rather than failing: a silent lead-in is a normal thing to feed a graph.
    if (peak <= 0.0) return true;
    const double g = level / peak;
    for (int c = 0; c < out.channels(); ++c) {
        std::vector<float>& x = out.ch[(size_t)c];
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)((double)x[i] * g);
    }
    return tick(prog, 1.0, err) ? true : false;
}

//============================================================================
//  SPEED / TRANSPOSE  --  strans.c
//============================================================================

//! strans.c strans_process(), mode MOD_TRANSPOS_SEMIT: read the source with
//! linear interpolation at `ratio` samples per output sample.  Pitch and
//! duration move together (this is varispeed, not a phase vocoder), so the
//! output is inlen/ratio frames long and the process cannot stream.
//!
//! CDP's loop is `while(flplace < ssampsread)` with flplace starting at 1.0
//! against a buffer whose index 0 is the previous buffer's last sample -- i.e.
//! read position 0 of the real signal, running while position < inlen-1 so that
//! the interpolation partner exists.  That is the bound used here.
bool speed_run(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const double semis = clampd(at(p, 0, 12.0), -96.0, 96.0);
    const double ratio = semitone_to_ratio(semis);
    const Buffer& src  = in[0];
    const int64_t n    = src.frames();
    if (n < 2 || ratio <= 0.0) { out = src; return true; }

    int64_t outlen = (int64_t)std::floor(((double)(n - 1) - 1e-12) / ratio) + 1;
    outlen = std::max<int64_t>(1, outlen);
    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), outlen);
    for (int c = 0; c < src.channels(); ++c) {
        const std::vector<float>& x = src.ch[(size_t)c];
        std::vector<float>&       y = out.ch[(size_t)c];
        for (int64_t j = 0; j < outlen; ++j)
            y[(size_t)j] = (float)lerp_read(x, (double)j * ratio);
        if (!tick(prog, (double)(c + 1) / src.channels(), err)) return false;
    }
    return true;
}

//! strans.c do_vibrato() (`modify speed 6`), with vtrans_preprocess()'s table
//! setup.  The read pointer advances by 2^(sin * depth) per output sample, so
//! the source is scanned at a wobbling rate -- vibrato by varispeed, sidebands
//! and all.  CDP's own conversions are kept: frequency becomes a step through a
//! 1024-point sine table (frq * VIB_TABSIZE / srate) and depth is turned from
//! semitones into OCTAVES before it is used as an exponent.
//!
//! Output length is signal-dependent: the mean of 2^(sin*d) is slightly above
//! 1, so the source is consumed a shade faster than realtime and the result is
//! a shade shorter.  CDP stops the moment the source runs out; so does this.
bool vibrato_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                 Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    const int64_t n   = src.frames();
    const double  sr  = src.sampleRate > 0 ? src.sampleRate : 48000;
    const double  frq = clampd(at(p, 0, 12.0), 0.0, 120.0);      // MAX_VIB_FRQ
    const double  depth = clampd(at(p, 1, 0.667), 0.0, 96.0) * kOctavesPerSemitone;
    if (n < 2) { out = src; return true; }

    // vtrans_preprocess(): sin table of VIB_TABSIZE, with a zero written in the
    // guard slot so interp_read_sintable can always read index+1.
    std::vector<double> tab((size_t)kVibTabSize + 1, 0.0);
    for (int i = 0; i < kVibTabSize; ++i)
        tab[(size_t)i] = std::sin(kTwoPi * (double)i / (double)kVibTabSize);

    const double step = frq * (double)kVibTabSize / sr;

    // Worst case the pointer crawls at 2^-depth per sample, so reserve that
    // much and shrink to what was actually produced.
    const double slowest = std::pow(2.0, -depth);
    const int64_t cap = (int64_t)std::ceil((double)n / std::max(1e-6, slowest)) + 2;
    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), cap);

    const int chans = src.channels();
    double sfindex = 0.0;
    // CDP emits the first sample verbatim, then starts moving the pointer; its
    // flplace runs against the guarded buffer, so in real-signal terms it
    // starts one sample early and the first advance lands back on sample 0.
    for (int c = 0; c < chans; ++c) out.ch[(size_t)c][0] = src.ch[(size_t)c][0];
    double pos = -1.0;
    int64_t j = 1;
    for (; j < cap; ++j) {
        sfindex += step;
        while (sfindex >= (double)kVibTabSize) sfindex -= (double)kVibTabSize;
        const int    si   = (int)sfindex;
        const double frac = sfindex - (double)si;
        const double sinval = tab[(size_t)si] + (tab[(size_t)si + 1] - tab[(size_t)si]) * frac;
        const double incr = std::pow(2.0, sinval * depth);
        pos += incr;
        if (pos >= (double)(n - 1)) break;              // source exhausted
        for (int c = 0; c < chans; ++c)
            out.ch[(size_t)c][(size_t)j] = (float)lerp_read(src.ch[(size_t)c], pos);
        if ((j & 0xFFFF) == 0 && !tick(prog, (double)j / (double)cap, err)) return false;
    }
    for (int c = 0; c < chans; ++c) out.ch[(size_t)c].resize((size_t)j);
    return true;
}

//============================================================================
//  LOSE RESOLUTION (LOBIT)  --  radical.c
//============================================================================

//! radical.c lobit_pconsistency() turns the 1..16 "bit resolution" parameter
//! into the integer divisor do_lowbit() rounds against:
//!
//!     if(bres != MAX_BIT_DIV) { e = 16 - bres; garf = 2; while(++m < e) garf *= 2;
//!                               bres = garf; }
//!     bres *= 256 * 256;                    // the FLOAT32/INT_32 branch
//!
//! For bres in 1..15 that lands on a quantisation step of 2^(1-bres) over the
//! [-1,1] range -- exactly `bres` bits.  bres == 16 skips the block entirely
//! and leaves the divisor at a literal 16, which after the *65536 is a step of
//! 2^-11: CDP's top setting is about ELEVEN bits, not sixteen, and it is CDP's
//! own default.  That is reproduced rather than corrected -- it is the sound
//! `modify radical 4` makes -- but our default is 8, where the parameter means
//! what it says.
int lobit_divisor(int bits) {
    int bres = (int)clampi(bits, 1, kMaxBitDiv);
    if (bres != kMaxBitDiv) {
        const int e = kMaxBitDiv - bres;
        int garf = 2;
        for (int m = 1; m < e; ++m) garf *= 2;
        bres = garf;
    }
    return bres * (256 * 256);           // FLOAT32 sample type
}

//! radical.c do_lowbit().  Two reductions at once: `tscan` consecutive frames
//! are averaged and the average is held across all of them (a sample-rate
//! division), and the held value is rounded to a multiple of the bit divisor.
//! CDP trims the buffer to a whole multiple of tscan and ZEROES the remainder;
//! over a whole file that is the final partial group, up to tscan-1 frames, and
//! it is kept so the result matches the program.
bool lobit_run(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int bits  = (int)clampi((int64_t)std::lround(at(p, 0, 8.0)), 1, kMaxBitDiv);
    const int tscan = (int)clampi((int64_t)std::lround(at(p, 1, 1.0)), 1, kMaxSrateDiv);
    const double bres = (double)lobit_divisor(bits);
    const double maxsampval = 2147483647.0;          // FLOAT32/INT_32 branch

    out = in[0];
    const int64_t n = out.frames();
    const int64_t todo = (n / tscan) * tscan;        // CDP's todo_samples
    for (int c = 0; c < out.channels(); ++c) {
        std::vector<float>& x = out.ch[(size_t)c];
        for (int64_t i = todo; i < n; ++i) x[(size_t)i] = 0.f;
        for (int64_t i = 0; i < todo; i += tscan) {
            double v = 0.0;
            for (int64_t k = i; k < i + tscan; ++k) v += (double)x[(size_t)k];
            v /= (double)tscan;                      // time average
            v *= maxsampval;                         // to 'integer' range
            v /= bres;
            v  = std::round(v);                      // round mod bres
            v *= bres;
            v /= maxsampval;
            v  = clampd(v, -1.0, 1.0);
            for (int64_t k = i; k < i + tscan; ++k) x[(size_t)k] = (float)v;
        }
        if (!tick(prog, (double)(c + 1) / out.channels(), err)) return false;
    }
    return true;
}

//============================================================================
//  RING MODULATION  --  radical.c
//============================================================================

//! radical.c create_rm_sintab(): RM_SINTABSIZE points plus a zero guard slot,
//! read by read_rmod_tab() with linear interpolation.  Built once; the table is
//! constant, so the streaming instance can share it without allocating.
const double* rm_sintab() {
    static double tab[kRmSinTabSize + 1];
    static const bool once = [] {
        for (int i = 0; i < kRmSinTabSize; ++i)
            tab[i] = std::sin(kTwoPi * (double)i / (double)kRmSinTabSize);
        tab[kRmSinTabSize] = 0.0;
        return true;
    }();
    (void)once;
    return tab;
}

//! radical.c ring_modulate().  One oscillator multiplies every channel -- CDP's
//! inner `for(j=0;j<chans;j++)` applies the same sinval across the frame -- so
//! the state is a single table index and the process is exactly sample by
//! sample.  CDP's per-RING_MOD_BLOK interpolation of the frequency in log10 is
//! the breakpoint path; with a constant frequency pitchstep is 0 and this is
//! what remains.
struct RingModStream final : Stream {
    const double* tab = rm_sintab();
    double tabstep = 0.0;
    double tabindex = 0.0;
    void process(float* const* ch, int channels, int n) override {
        for (int i = 0; i < n; ++i) {
            const int    ti   = (int)tabindex;              // CDP truncates
            const double frac = tabindex - (double)ti;
            const double sinval = tab[ti] + (tab[ti + 1] - tab[ti]) * frac;
            for (int c = 0; c < channels; ++c)
                ch[c][i] = (float)((double)ch[c][i] * sinval);
            tabindex += tabstep;
            while (tabindex >= (double)kRmSinTabSize) tabindex -= (double)kRmSinTabSize;
        }
    }
    void reset() override { tabindex = 0.0; }
};

double ringmod_step(const std::vector<double>& p, double sr) {
    if (sr <= 0.0) sr = 48000.0;
    // CDP's range is MIN_RING_MOD_FRQ .. nyquist; nyquist is not knowable from
    // a ParamSpec, so it is clamped here instead of refused.
    const double frq = clampd(at(p, 0, 60.0), 0.1, sr * 0.5);
    return frq * (double)kRmSinTabSize / sr;
}

bool ringmod_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                 Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    out = in[0];
    RingModStream s;
    s.tabstep = ringmod_step(p, out.sampleRate);
    const int chans = out.channels();
    std::vector<float*> ptr((size_t)chans);
    for (int c = 0; c < chans; ++c) ptr[(size_t)c] = out.ch[(size_t)c].data();
    s.process(ptr.data(), chans, (int)out.frames());
    return tick(prog, 1.0, err) ? true : false;
}

//============================================================================
//  SHRED  --  radical.c
//============================================================================

//! The state shred() needs, which in CDP is a dozen dz->iparam slots.
struct ShredPlan {
    int64_t worklen = 0;      // SHR_WORKLEN: buffer minus one splice reserve
    int64_t unitlen = 0;      // SHR_UNITLEN
    int64_t rawlen  = 0;      // SHR_UNITLEN - splice
    int64_t endrawlen = 0;
    int64_t splen   = 0;      // SHRED_SPLICELEN, in frames
    int     chcnt   = 2;      // SHR_CHCNT
    int     iscat   = 0;      // SHRED_SCAT as an integer group size, 0 == none
    double  scat    = 1.0;    // SHRED_SCAT as CDP's double
    int64_t range = 0, endrange = 0;
    int     scatgrpcnt = 0, endscat = 0;
};

//! radical.c get_basis_lengths().  CDP computes SHR_UNITLEN with an integer
//! division inside round(), so it truncates; that is kept.
void shred_basis(ShredPlan& s) {
    s.unitlen = s.worklen / s.chcnt;
    const int64_t excess = s.worklen - s.unitlen * s.chcnt;
    s.rawlen    = s.unitlen - s.splen;
    s.endrawlen = s.rawlen + excess;
    if (s.iscat) {
        s.scatgrpcnt = s.chcnt / s.iscat;
        s.endscat    = s.chcnt - s.scatgrpcnt * s.iscat;
        s.range      = s.unitlen * s.iscat;
        s.endrange   = ((int64_t)s.endscat - 1) * s.unitlen + (s.unitlen + excess);
    }
}

//! radical.c normal_scat(): jitter each cut by up to +-scatter/2 of a chunk.
//! The first cut cannot move (it is the start of the buffer).  CDP's last-chunk
//! branch SUBTRACTS a negative offset, which pushes the final cut later rather
//! than earlier -- reproduced as written.
void shred_normal_scat(const ShredPlan& s, std::vector<int64_t>& ptr, Drand48& rng) {
    int64_t total = s.unitlen;
    int n = 1;
    for (; n < s.chcnt - 1; ++n) {
        const double sc = (rng.next() - 0.5) * s.scat;
        ptr[(size_t)n] = total + (int64_t)(sc * (double)s.rawlen);
        total += s.unitlen;
    }
    const double sc = (rng.next() - 0.5) * s.scat;
    if (sc < 0.0) ptr[(size_t)n] = total - (int64_t)(sc * (double)s.rawlen);
    else          ptr[(size_t)n] = total + (int64_t)(sc * (double)s.endrawlen);
}

//! radical.c heavy_scat(): for scatter > 1 the cuts inside each group of
//! `iscat` chunks are thrown at random over the group's whole span, rejecting
//! any that land within a splice of a neighbour.  CDP's own shred_process warns
//! "there is a finite possibility program will not terminate" about exactly
//! this rejection loop; a library may not gamble, so the retry is capped and
//! the last candidate is accepted when the cap is hit.
void shred_heavy_scat(const ShredPlan& s, std::vector<int64_t>& ptr, Drand48& rng) {
    int thiss = 1, first = 1;
    int64_t startptr = 0, endptr = 0;
    for (int n = 0; n < s.scatgrpcnt && thiss < s.chcnt; ++n) {
        const int start = thiss;
        endptr += s.range;
        for (int m = first; m < s.iscat && thiss < s.chcnt; ++m) {
            for (int tryn = 0; tryn < 1000; ++tryn) {
                ptr[(size_t)thiss] = (int64_t)(rng.next() * (double)s.range) + startptr;
                bool ok = true;
                for (int that = start - 1; that < thiss; ++that) {
                    if (std::llabs(ptr[(size_t)thiss] - ptr[(size_t)that]) < s.splen ||
                        std::llabs(endptr - ptr[(size_t)thiss]) < s.splen) { ok = false; break; }
                }
                if (ok) break;
            }
            ++thiss;
        }
        startptr += s.range;
        first = 0;
    }
    endptr += s.endrange;
    if (s.endscat) {
        const int start = thiss;
        for (int m = 0; m < s.endscat && thiss < s.chcnt; ++m) {
            for (int tryn = 0; tryn < 1000; ++tryn) {
                ptr[(size_t)thiss] = (int64_t)(rng.next() * (double)s.endrange) + startptr;
                bool ok = true;
                for (int that = start - 1; that < thiss; ++that) {
                    if (std::llabs(ptr[(size_t)thiss] - ptr[(size_t)that]) < s.splen ||
                        std::llabs(endptr - ptr[(size_t)thiss]) < s.splen) { ok = false; break; }
                }
                if (ok) break;
            }
            ++thiss;
        }
    }
    std::sort(ptr.begin() + 1, ptr.begin() + s.chcnt);      // radical.c ptr_sort()
}

//! radical.c permute_chunks()/insert()/prefix()/shuflup(): CDP's own shuffle,
//! which builds the permutation by inserting each new index at a random place
//! in the list built so far.
void shred_permute(int chcnt, std::vector<int>& perm, Drand48& rng) {
    perm.assign((size_t)chcnt, 0);
    for (int n = 0; n < chcnt; ++n) {
        const int t = (int)(rng.next() * (double)(n + 1));
        const int k = (t == n) ? 0 : t + 1;              // prefix() vs insert()
        for (int i = chcnt - 1; i > k; --i) perm[(size_t)i] = perm[(size_t)(i - 1)];
        perm[(size_t)k] = n;
    }
}

//! radical.c shred() over one contiguous buffer, for one channel.  `src` is
//! read, `dst` is written; the two swap between repeats exactly as CDP
//! ping-pongs sampbuf[0] and sampbuf[1].  Splices ADD into the destination
//! (do_startsplice/do_endsplice), which is why dst starts zeroed.
void shred_one(const std::vector<float>& src, std::vector<float>& dst,
               const ShredPlan& s, const std::vector<int64_t>& ptr,
               const std::vector<int64_t>& len, const std::vector<int>& perm,
               bool smooth) {
    std::fill(dst.begin(), dst.end(), 0.f);
    const int64_t total = (int64_t)dst.size();
    const int64_t sp = s.splen;
    auto startsplice = [&](int64_t i, int64_t j) {
        for (int64_t k = 0; k < sp; ++k) {
            if (i + k >= total || j + k >= total) break;
            dst[(size_t)(j + k)] += (float)((double)src[(size_t)(i + k)] * (double)k / (double)sp);
        }
    };
    auto endsplice = [&](int64_t i, int64_t j) {
        for (int64_t k = 0; k < sp; ++k) {
            if (i + k >= total || j + k >= total) break;
            dst[(size_t)(j + k)] +=
                (float)((double)src[(size_t)(i + k)] * (double)(sp - 1 - k) / (double)sp);
        }
    };
    auto blockcopy = [&](int64_t i, int64_t j, int64_t cnt) {
        if (cnt <= 0) return;
        cnt = std::min(cnt, std::min(total - i, total - j));
        if (cnt > 0) std::memcpy(&dst[(size_t)j], &src[(size_t)i], (size_t)cnt * sizeof(float));
    };

    const int last = s.chcnt - 1;
    int64_t oldp = ptr[(size_t)perm[0]];
    int64_t newp = 0;
    int64_t clen = len[(size_t)perm[0]];
    if (smooth) {                                        // CDP's -n flag
        startsplice(oldp, newp);
        blockcopy(oldp + sp, newp + sp, clen - sp);
    } else {
        blockcopy(oldp, newp, clen);
    }
    endsplice(oldp + clen, newp + clen);
    for (int n = 1; n < last; ++n) {
        oldp  = ptr[(size_t)perm[(size_t)n]];
        newp += clen;
        clen  = len[(size_t)perm[(size_t)n]];
        startsplice(oldp, newp);
        blockcopy(oldp + sp, newp + sp, clen - sp);
        endsplice(oldp + clen, newp + clen);
    }
    if (last >= 1) {
        oldp  = ptr[(size_t)perm[(size_t)last]];
        newp += clen;
        clen  = len[(size_t)perm[(size_t)last]];
        startsplice(oldp, newp);
        if (smooth) endsplice(oldp + clen, newp + clen);
        blockcopy(oldp + sp, newp + sp, clen);
    }
    // do_bufend_splices(): fade the very start and end of the buffer.
    for (int64_t k = 0; k < sp && k < total; ++k)
        dst[(size_t)k] = (float)((double)dst[(size_t)k] * (double)k / (double)sp);
    for (int64_t k = 0; k < sp && k < total; ++k) {
        const int64_t i = total - sp + k;
        if (i < 0) continue;
        dst[(size_t)i] = (float)((double)dst[(size_t)i] * (double)(sp - 1 - k) / (double)sp);
    }
}

bool shred_run(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    out = in[0];
    const int64_t n  = out.frames();
    const double  sr = out.sampleRate > 0 ? out.sampleRate : 48000;
    const double  dur = (double)n / sr;

    const int repeats = (int)clampi((int64_t)std::lround(at(p, 0, 1.0)), 1, 100);
    const double chunklen = std::max(1e-4, at(p, 1, 0.05));
    const double scatter  = clampd(at(p, 2, 1.0), 0.0, 8.0);   // MAX_SHR_SCATTER
    const bool   smooth   = at(p, 3, 0.0) >= 0.5;              // CDP's -n
    const uint32_t seed   = (uint32_t)clampi((int64_t)std::lround(at(p, 4, 1.0)), 0, 65535);

    // A clip shorter than a few splices cannot be shredded at all; CDP errors
    // ("SOUNDFILE TOO SMALL to shred").  Shortening the splice instead keeps
    // every input workable, and only bites on clips under ~40 ms.
    ShredPlan s;
    s.splen = std::min<int64_t>(kShredSplice, std::max<int64_t>(1, n / 8));
    if (n < 16) return true;                       // nothing meaningful to do
    s.worklen = n - s.splen;
    s.scat = scatter;
    s.iscat = scatter > 1.0 ? (int)std::lround(scatter) : 0;
    // shred_pconsistency(): chunk count from duration/chunklen.  Clamped, not
    // refused: CDP rejects a chunk longer than half the file, and rejects a
    // chunk too short to hold two splices.
    int64_t chcnt = (int64_t)std::llround(dur / chunklen);
    chcnt = clampi(chcnt, 2, std::max<int64_t>(2, s.worklen / s.splen));
    s.chcnt = (int)chcnt;
    if (s.iscat > s.chcnt) s.iscat = s.chcnt;      // CDP: scatter <= chunk count
    shred_basis(s);
    if (s.unitlen < s.splen) return true;          // degenerate: leave as-is

    std::vector<int64_t> ptr((size_t)s.chcnt, 0), len((size_t)s.chcnt, 0);
    std::vector<int>     perm;
    std::vector<float>   tmp((size_t)n, 0.f);
    Drand48 rng;
    rng.seed(seed);

    for (int r = 0; r < repeats; ++r) {
        // The cuts and the permutation are shared by every channel, exactly as
        // CDP's interleaved buffer forces them to be.
        ptr[0] = 0;
        if (!s.iscat) shred_normal_scat(s, ptr, rng);
        else          shred_heavy_scat(s, ptr, rng);
        for (int i = 1; i < s.chcnt; ++i)          // keep the cuts legal
            ptr[(size_t)i] = clampi(ptr[(size_t)i], ptr[(size_t)(i - 1)] + s.splen,
                                    s.worklen - s.splen);
        for (int i = 0; i < s.chcnt - 1; ++i) len[(size_t)i] = ptr[(size_t)(i + 1)] - ptr[(size_t)i];
        len[(size_t)(s.chcnt - 1)] = s.worklen - ptr[(size_t)(s.chcnt - 1)];
        shred_permute(s.chcnt, perm, rng);

        for (int c = 0; c < out.channels(); ++c) {
            shred_one(out.ch[(size_t)c], tmp, s, ptr, len, perm, smooth);
            out.ch[(size_t)c].swap(tmp);
        }
        if (!tick(prog, (double)(r + 1) / (double)repeats, err)) return false;
    }
    return true;
}

//============================================================================
//  STACK  --  radical.c
//============================================================================

//! radical.c stack_preprocess() + do_stack() + get_obuf_sampoffset() +
//! transpos_and_add_to_obuf().  A mix of `count` varispeed copies of the source
//! at successive transpositions, aligned so that their attacks coincide.
//!
//! stack_preprocess builds the transposition list geometrically from one
//! semitone value: ratios 1, m, m^2 ... for m > 1, and the same run built
//! downwards (so the unshifted copy stays on top) for m < 1.  The slowest ratio
//! sets the output length, since that copy takes longest to consume the source.
//!
//! Dropped: the -s flag (it only printed the layer levels), the transposition
//! FILE (a list of arbitrary ratios instead of a geometric stack -- there is no
//! ParamSpec for a list), and the double pass CDP makes over the whole file
//! just to measure the peak, which is here one pass over the finished buffer.
bool stack_run(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const Buffer& src = in[0];
    const int64_t n   = src.frames();
    const double  sr  = src.sampleRate > 0 ? src.sampleRate : 48000;
    if (n < 2) { out = src; return true; }

    const double semis = clampd(at(p, 0, 12.0), -96.0, 96.0);
    const int    cnt   = (int)clampi((int64_t)std::lround(at(p, 1, 3.0)), 2, 32);
    const double lean  = clampd(at(p, 2, 1.0), 0.01, 100.0);
    const double offs  = clampd(at(p, 3, 0.0), 0.0, (double)n / sr);
    const double gain  = clampd(at(p, 4, 1.0), 0.1, 10.0);
    // CDP rounds DUR to one decimal and refuses 0 ("You have asked to make NONE
    // of the output"); clamped to a tenth instead so no setting yields nothing.
    const double dur   = clampd(std::round(clampd(at(p, 5, 1.0), 0.0, 1.0) * 10.0) / 10.0,
                                0.1, 1.0);
    const bool   norm  = at(p, 6, 0.0) >= 0.5;

    // stack_preprocess(): the geometric transposition list.
    std::vector<double> trans((size_t)cnt, 1.0);
    const double mult = semitone_to_ratio(semis);
    double thisstep = 1.0;
    if (mult > 1.0) {
        for (int i = 0; i < cnt; ++i) { trans[(size_t)i] = thisstep; thisstep *= mult; }
    } else {
        for (int i = cnt - 1; i >= 0; --i) { trans[(size_t)i] = thisstep; thisstep *= mult; }
    }
    double mintrans = trans[0];
    for (int i = 0; i < cnt; ++i) mintrans = std::min(mintrans, trans[(size_t)i]);
    if (!(mintrans > 1e-9)) { err = "stack transposition collapsed to zero"; return false; }

    // stack_preprocess(): levels ramp linearly from LEAN (the lowest layer) to
    // 1.0 (the highest), then are normalised so they sum to GAIN.  With
    // lean == 1 that degenerates to CDP's flat gain/count branch.
    std::vector<double> amp((size_t)cnt, 1.0);
    const int k = cnt - 1;
    amp[0] = lean; amp[(size_t)k] = 1.0;
    const double step = (amp[(size_t)k] - amp[0]) / (double)k;
    for (int i = 1; i < k; ++i) amp[(size_t)i] = amp[(size_t)(i - 1)] + step;
    double sum = 0.0;
    for (int i = 0; i < cnt; ++i) sum += amp[(size_t)i];
    if (sum <= 0.0) sum = 1.0;
    for (int i = 0; i < cnt; ++i) amp[(size_t)i] = amp[(size_t)i] / sum * gain;

    // get_obuf_sampoffset(): a faster copy reaches the attack sooner, so it is
    // delayed by the fraction of the attack time it saves.
    int64_t offsamps = (int64_t)std::llround(offs * sr);
    if (mintrans < 1.0) offsamps = (int64_t)std::llround((double)offsamps / mintrans);
    std::vector<int64_t> skip((size_t)cnt, 0), lens((size_t)cnt, 0);
    int64_t outlen = 0;
    for (int i = 0; i < cnt; ++i) {
        const double ratio = trans[(size_t)i] / mintrans;
        skip[(size_t)i] = (std::fabs(ratio - 1.0) < 1e-9)
                        ? 0
                        : (int64_t)std::llround((double)offsamps * (1.0 - 1.0 / ratio));
        lens[(size_t)i] = (int64_t)std::floor(((double)(n - 1) - 1e-12) / trans[(size_t)i]) + 1;
        outlen = std::max(outlen, skip[(size_t)i] + lens[(size_t)i]);
    }
    if (dur < 1.0) {                         // STACK_DUR: how much to make
        const int64_t lim = (int64_t)std::llround((dur / mintrans) * (double)n);
        outlen = std::min(outlen, std::max<int64_t>(1, lim));
    }
    out.sampleRate = src.sampleRate;
    out.resize(src.channels(), outlen);

    for (int i = 0; i < cnt; ++i) {
        const double  t  = trans[(size_t)i];
        const double  a  = amp[(size_t)i];
        const int64_t s0 = skip[(size_t)i];
        for (int c = 0; c < src.channels(); ++c) {
            const std::vector<float>& x = src.ch[(size_t)c];
            std::vector<float>&       y = out.ch[(size_t)c];
            double pos = 0.0;
            for (int64_t j = 0; j < lens[(size_t)i]; ++j, pos += t) {
                const int64_t o = s0 + j;
                if (o >= outlen) break;
                y[(size_t)o] = (float)((double)y[(size_t)o] + lerp_read(x, pos) * a);
            }
        }
        if (!tick(prog, (double)(i + 1) / (double)cnt, err)) return false;
    }
    if (norm) {                              // CDP's -n flag
        double peak = 0.0;
        for (int c = 0; c < out.channels(); ++c)
            for (const float v : out.ch[(size_t)c]) peak = std::max(peak, (double)std::fabs(v));
        if (peak > 0.0) {
            const double g = 0.95 / peak;
            for (int c = 0; c < out.channels(); ++c)
                for (float& v : out.ch[(size_t)c]) v = (float)((double)v * g);
        }
    }
    return true;
}

} // namespace

void register_modify_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug  = "modify.loudness";
        p.name  = "Loudness";
        p.group = "Modify";
        p.help  = "Change level by a number of decibels, optionally inverting phase.";
        p.streamable = true;
        // A gain is causal and memoryless: no lookahead whatsoever.
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Gain", "dB", -96.0, 96.0, 0.0, false,
              "Level change in dB; -96 is silence (CDP's MIN_DB_ON_16_BIT)." },
            { "Invert", "", 0, 1, 0, true, "Invert the phase of the signal." },
        };
        p.makeStream = [](int, const std::vector<double>& pr) -> std::unique_ptr<Stream> {
            auto s = std::unique_ptr<LoudStream>(new LoudStream());
            s->g = loudness_gain(pr);
            return s;
        };
        p.run = &loudness_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.normalise";
        p.name  = "Normalise";
        p.group = "Modify";
        p.help  = "Scale the whole clip so its loudest peak sits at the given level.";
        // The gain cannot be known until the last sample has been seen.
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Level", "", 1.0 / 32767.0, 1.0, 0.9, false,
              "Peak level the loudest sample is moved to." },
        };
        p.run = &normalise_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.speed";
        p.name  = "Speed";
        p.group = "Modify";
        p.help  = "Varispeed: shift pitch and duration together, as with tape.";
        // Re-times the material, so the output is a different length: there is
        // no block-by-block form of this at all.
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Transpose", "semitones", -96.0, 96.0, 12.0, false,
              "Up shortens and raises, down lengthens and lowers." },
        };
        p.run = &speed_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.vibrato";
        p.name  = "Vibrato";
        p.group = "Modify";
        p.help  = "Wobble the playback speed, giving vibrato with tape sidebands.";
        p.streamable = false;      // variable read rate, and it re-times the clip
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Rate", "Hz", 0.0, 120.0, 12.0, false, "Vibrato frequency." },
            { "Depth", "semitones", 0.0, 96.0, 0.667, false,
              "Pitch swing either side of centre; CDP's default is 2/3 of a semitone." },
        };
        p.run = &vibrato_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.lobit";
        p.name  = "Lose Resolution";
        p.group = "Modify";
        p.help  = "Crush bit depth and divide the sample rate, as a cheap converter would.";
        p.streamable = true;
        // The average of a scan block must be complete before its first sample
        // can be emitted; with the default scan of 1 there is no delay at all.
        p.latencyFrames = [](int, const std::vector<double>& pr) -> int64_t {
            const int64_t tscan = clampi((int64_t)std::lround(at(pr, 1, 1.0)), 1, kMaxSrateDiv);
            return tscan - 1;
        };
        p.params = {
            { "Bits", "", 1, kMaxBitDiv, 8, true,
              "Bit resolution; 16 hits CDP's own top-of-range case and is ~11 bits." },
            { "Scan", "", 1, kMaxSrateDiv, 1, true,
              "Frames averaged and held together: 1 is untouched, higher divides the rate." },
        };
        p.run = &lobit_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.ringmod";
        p.name  = "Ring Modulate";
        p.group = "Modify";
        p.help  = "Multiply the sound by a sine tone, creating sum and difference bands.";
        p.streamable = true;
        // One oscillator, one multiply per sample: causal, no lookahead.
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Frequency", "Hz", 0.1, 20000.0, 60.0, false,
              "Modulating frequency; clamped to Nyquist for the clip's rate." },
        };
        p.makeStream = [](int sr, const std::vector<double>& pr) -> std::unique_ptr<Stream> {
            auto s = std::unique_ptr<RingModStream>(new RingModStream());
            s->tabstep = ringmod_step(pr, sr > 0 ? sr : 48000);
            s->reset();
            return s;
        };
        p.run = &ringmod_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.shred";
        p.name  = "Shred";
        p.group = "Modify";
        p.help  = "Cut the clip into chunks and permute them, keeping its duration.";
        // The permutation moves material across the whole buffer.
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Repeats", "", 1, 100, 1, true, "How many times the shredding is applied." },
            { "Chunk", "s", 0.001, 2.0, 0.05, false,
              "Average length of the chunks cut and permuted." },
            { "Scatter", "", 0.0, 8.0, 1.0, false,
              "Randomisation of the cut points; above 1 the cuts are thrown across groups." },
            { "Smooth", "", 0, 1, 0, true, "Splice the first and last chunk too." },
            { "Seed", "", 0, 65535, 1, true,
              "Random seed; CDP seeds from the clock, a render must repeat." },
        };
        p.run = &shred_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "modify.stack";
        p.name  = "Stack";
        p.group = "Modify";
        p.help  = "Mix transposed varispeed copies of the sound on top of each other.";
        // Every layer re-times the source, and the slowest sets the length.
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Transpose", "semitones", -96.0, 96.0, 12.0, false,
              "Interval between successive copies in the stack." },
            { "Count", "", 2, 32, 3, true, "How many copies are stacked." },
            { "Lean", "", 0.01, 100.0, 1.0, false,
              "Loudness of the lowest copy relative to the highest." },
            { "Offset", "s", 0.0, 10.0, 0.0, false,
              "Time of the attack, used to line the copies up." },
            { "Gain", "", 0.1, 10.0, 1.0, false, "Overall output gain." },
            { "Duration", "", 0.1, 1.0, 1.0, false,
              "Proportion of the output to make." },
            { "Normalise", "", 0, 1, 0, true, "Scale the result to 0.95 peak." },
        };
        p.run = &stack_run;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
