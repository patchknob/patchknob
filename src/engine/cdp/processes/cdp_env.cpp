//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_env.cpp
//
//  Ported from the CDP ENV family (vendor/cdp8/dev/env), which is
//      Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Specifically:
//      envxtract.c   extract_env_from_sndfile / getenv_of_buffer / getmaxsamp
//      envimpos.c    impose_envel_on_sndfile / apply_envel_to_buffer /
//                    apply_env_to_window / step_gen / adjust_envelope
//      envfuncs.c    envelope_warp and its 15 modes (envnorm, envrevers,
//                    envexagg, envatten, envlift, envtstretch/envshortn/
//                    envlenthn, envflatn, envgate/gatefilt, envinvert,
//                    envlimit, envkorrug/ispeak/istrough/digout_trough,
//                    envexpand, envceiling, envduck), envreplace,
//                    create_envelope, envelope_tremol
//      envprepro.c   create_initial_envelope_params, create_tremol_sintable
//      envprocess.c  process_envelope (which mode calls what, in what order)
//      ap_envel.c    generate_samp_windowsize, the usage text every ParamSpec
//                    below is copied from
//
//  WHAT CDP'S ENVELOPE IS.  Not a smoothed follower: `envel` slices the file
//  into fixed windows and takes the largest |sample| in each one (envxtract.c
//  getmaxsamp, whose comment records that it was REVERTED from power back to
//  peak).  One value per window, one envelope for all channels -- getmaxsamp
//  scans the interleaved buffer, so every channel feeds the same contour and
//  imposition multiplies every channel by the same gain.  That is preserved
//  here: extraction maxes across Buffer::ch, imposition applies one gain.
//
//  WHAT WAS DROPPED, AND WHY.  Three layers of plumbing, none of it DSP:
//
//   1. FILE TYPES.  Half the ENV family exists only to move an envelope between
//      three on-disk representations -- a binary "envfile" (one float per
//      window), a text breakpoint file, and the same in dB.  envtobrk, envtodb,
//      brktoenv, dbtoenv, dbtogain, gaintodb, create, cyclic, reshape and
//      replot are all format conversions whose output is a file, not audio, so
//      they have no meaning against a Buffer-in/Buffer-out contract and are not
//      registered.  `impose` and `replace` keep only their mode 1 -- envelope
//      taken from a second SOUNDFILE -- which is exactly the two-input case the
//      editor can express by dragging a second clip.  With the file modes gone,
//      convert_envelope_to_brkpnt_table and its datareduce() peer went too.
//
//   2. BUFFERING.  CDP streams through dz->sampbuf[0] one disk buffer at a
//      time, with the envelope window loop restarting at every buffer boundary
//      (apply_envel_to_buffer's trailing short-window branch fires per buffer).
//      Buffer lengths are chosen to be whole multiples of the envelope window
//      precisely so that this is invisible; over one contiguous buffer the
//      boundary case cannot arise at all, and only the genuine final partial
//      window remains.  Likewise rejig_buffering(), which only exists to
//      rebuild those buffers when the second file has a different sample rate,
//      is replaced by simply asking each Buffer for its own rate.
//
//   3. TIME-VARYING PARAMETERS.  Nearly every ENV parameter may be a breakpoint
//      file, read per window through read_value_from_brktable().  ParamSpec is
//      a scalar, so the constant-parameter branch of each function is the one
//      ported (CDP writes both branches out longhand and they agree exactly
//      when the table is constant).  A host that wants motion automates the
//      knob.  One consequence is noted per process where it changes anything.
//
//  WHAT IS *NOT* DROPPED: the numerics.  The window size is quantised by
//  CDP's own generate_samp_windowsize(); step_gen's peculiar log-interpolation
//  escape hatch for extreme envelope steps is kept; imposition still runs
//  CDP's two-pass "measure the peak, then scale the envelope down" (which is
//  what makes it non-streamable); envkorrug still fails to reset its direction
//  flag between passes, because that is what the program does.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

//============================================================================
//  Constants, straight from the CDP headers they are named after.
//============================================================================
const int    ENV_FSECSIZE          = 256;        // envel.h
const double ENV_MIN_WSIZE         = 5.0;        // envlcon.h, ms
const double ENV_MAX_WSIZE         = 10000.0;    // envlcon.h, ms
const double ENV_DEFAULT_WSIZE     = 50.0;       // envlcon.h, ms
const double MIN_FRACTION_OF_LEVEL = 1.0 / 32767.0;  // globcon.h
const double F_MAXSAMP             = 1.0;        // globcon.h
const double F_MINSAMP             = -1.0;       // globcon.h
const double FLTERR                = 0.000002;   // globcon.h
const double STEP_LINEAR_LIMIT     = 1.0;        // envimpos.c
const int    ENV_TREM_TABSIZE      = 4096;       // envlcon.h
const double ENV_TREM_MAXFRQ       = 500.0;      // envlcon.h
const double ENV_TREM_DEFAULT_DEPTH= 0.25;       // envlcon.h
const int    MINSPAN               = 16;         // envfuncs.c
const double ATTENATOR             = 1.7;        // envfuncs.c
const double MAX_ENV_FLATN         = 5000.0;     // envlcon.h
const double MAX_ENV_SMOOTH        = 32767.0;    // envlcon.h
const double MAX_PEAK_SEPARATION   = 32767.0;    // envlcon.h

//! CDP's flteq() (cdp2k/tklib1.c): equality within FLTERR.
inline bool flteq(double a, double b) { return std::fabs(a - b) < FLTERR; }

inline double at(const std::vector<double>& p, size_t i, double dflt) {
    return i < p.size() ? p[i] : dflt;
}
inline int64_t iat(const std::vector<double>& p, size_t i, double dflt) {
    return (int64_t)std::llround(at(p, i, dflt));
}

//============================================================================
//  Envelope window size -- ap_envel.c generate_samp_windowsize().
//
//  CDP computes this in INTERLEAVED samples against chansecsize =
//  ENV_FSECSIZE * channels.  Every term on both sides of every comparison
//  carries the same factor `channels`, so expressed in FRAMES the channel
//  count cancels exactly and the section size is just ENV_FSECSIZE.  The
//  quantisation itself (snap to a multiple of 256 frames, or to the nearer of
//  the two bracketing powers of two below it) is preserved, because the window
//  size is what the whole family's resolution is made of.
//============================================================================
int64_t env_window_frames(double wsize_ms, int sampleRate) {
    const int sr = sampleRate > 0 ? sampleRate : 48000;
    double ms = wsize_ms;
    if (!(ms > 0.0)) ms = ENV_DEFAULT_WSIZE;      // a zero/negative knob is not
                                                   // a division by zero here
    const int64_t sec = ENV_FSECSIZE;
    int64_t un = (int64_t)std::llround(ms * 0.001 * (double)sr);
    // CDP relies on its own 5 ms lower bound to keep this >= 1.  We are called
    // from a host with an arbitrary knob, and un == 0 would run the halving
    // loop below down to k == 0 and then divide by it.
    if (un < 1) un = 1;
    if (un < sec) {
        int64_t k = sec;
        while (un < k) k /= 2;
        const int64_t j = k * 2;
        return (j - un > un - k) ? k : j;
    }
    const int64_t k = (int64_t)std::llround((double)un / (double)sec);
    return sec * (k < 1 ? 1 : k);
}

//============================================================================
//  EXTRACTION -- envxtract.c getenv_of_buffer() + getmaxsamp().
//
//  One float per window: the largest |sample| in that window across all
//  channels.  CDP scales by 1/F_ABSMAXSAMP, which is 1/1.0 in the floatsam
//  build, so the value is the peak itself.  A trailing partial window is
//  measured over what is actually there, exactly as CDP's "Handle any final
//  short buffer" branch does.
//============================================================================
std::vector<float> extract_env(const Buffer& b, int64_t win) {
    std::vector<float> env;
    const int64_t n = b.frames();
    if (n <= 0 || win <= 0) return env;
    const int64_t cnt = (n + win - 1) / win;      // windows_in_sndfile()
    env.reserve((size_t)cnt);
    for (int64_t w = 0; w < cnt; ++w) {
        const int64_t s = w * win;
        const int64_t e = std::min(n, s + win);
        double mx = 0.0;
        for (int c = 0; c < b.channels(); ++c) {
            const std::vector<float>& x = b.ch[(size_t)c];
            for (int64_t i = s; i < e; ++i) {
                const double v = std::fabs((double)x[(size_t)i]);
                if (v > mx) mx = v;
            }
        }
        env.push_back((float)mx);
    }
    return env;
}

//============================================================================
//  IMPOSITION -- envimpos.c apply_envel_to_buffer() / apply_env_to_window() /
//  step_gen().
//
//  Per window the gain ramps from env[w] to env[w+1].  Two details that look
//  like mistakes and are not:
//
//   * step_gen always divides by the FULL window length, even for the trailing
//     short window, which is then applied over fewer frames -- so the last ramp
//     deliberately stops short of its target.  Kept.
//   * When |step| exceeds 1.0 per frame, CDP abandons linear interpolation and
//     ramps in log10 instead (its "LOG INTERP FOR EXTREME CASES ONLY").  For an
//     envelope inside 0..1 this can never fire, since a step > 1 per frame
//     needs a jump > 1.  It exists for warped envelopes -- envreplace divides
//     one envelope by another and can produce values far above 1 -- and it is
//     kept for those.  CDP does not guard log10(next/this) against a zero
//     denominator; we fall back to the linear ramp there rather than emit inf.
//
//  `measureOnly` runs the pass without writing, returning the peak magnitude
//  the gains would produce.  That is CDP's test_impose_envel_on_sndfile(), and
//  it is the reason imposition cannot stream: the envelope is retroactively
//  scaled by F_MAXSAMP/peak before the real pass, which needs the whole file.
//============================================================================
double apply_env(Buffer& b, const std::vector<float>& env, int64_t win,
                 bool measureOnly) {
    const int64_t n = b.frames();
    const int64_t ecnt = (int64_t)env.size();
    if (n <= 0 || ecnt <= 0 || win <= 0) return 0.0;

    size_t ei = 0;
    double thisval = (double)env[0], nextval = (double)env[0];
    double step = 0.0;
    int64_t cnt = 0;
    double peak = 0.0;
    const int chans = b.channels();

    for (int64_t s = 0; s < n; s += win) {
        const int64_t len = std::min(win, n - s);     // frames actually written
        bool is_log = false;
        if (++cnt < ecnt) {                            // step_gen()
            thisval = nextval;
            nextval = (double)env[++ei];
            step = (nextval - thisval) / (double)win;  // NB: full window
            if (std::fabs(step) > STEP_LINEAR_LIMIT && thisval > 0.0 && nextval > 0.0) {
                step = std::log10(nextval / thisval);
                is_log = true;
            }
        } else {
            thisval = nextval;                         // hold the last value
            step = 0.0;
        }
        for (int64_t i = 0; i < len; ++i) {
            const double g = is_log
                ? thisval * std::pow(10.0, ((double)i / (double)len) * step)
                : thisval + step * (double)i;
            for (int c = 0; c < chans; ++c) {
                float& x = b.ch[(size_t)c][(size_t)(s + i)];
                double v = (double)x * g;
                if (measureOnly) {
                    const double a = std::fabs(v);
                    if (a > peak) peak = a;
                } else {
                    v = std::min(v, F_MAXSAMP);
                    v = std::max(v, F_MINSAMP);
                    x = (float)v;
                }
            }
        }
    }
    return peak;
}

//! impose_envel_on_sndfile(): measure, rescale the envelope if it would clip,
//! then apply.  `env` is taken by value because the rescale mutates it.
bool impose_env(Buffer& b, std::vector<float> env, int64_t win, std::string& err) {
    if (env.empty()) { err = "envelope is empty"; return false; }
    const double peak = apply_env(b, env, win, true);
    if (peak > F_MAXSAMP) {                             // adjust_envelope()
        const double adj = F_MAXSAMP / peak;
        for (float& v : env) v = (float)((double)v * adj);
    }
    apply_env(b, env, win, false);
    return true;
}

//============================================================================
//  envfuncs.c envreplace().
//
//  Turns "the envelope I want" into "the gain that gets me there", by dividing
//  it by the envelope the sound already has.  Where the source envelope is
//  effectively zero there is nothing to divide into, so CDP holds the previous
//  gain (or zero at the very start).  The result is truncated to the shorter of
//  the two -- which matters for warp timestretch, the one mode that changes the
//  envelope's length.
//============================================================================
bool env_replace(std::vector<float>& env, const std::vector<float>& orig,
                 std::string& err) {
    const double gate = MIN_FRACTION_OF_LEVEL;
    const size_t n = std::min(env.size(), orig.size());
    double maxval = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if ((double)orig[i] <= gate) {
            if ((double)env[i] <= gate || i == 0) env[i] = 0.f;
            else                                  env[i] = env[i - 1];
        } else {
            env[i] = (float)((double)env[i] / (double)orig[i]);
        }
        maxval = std::max((double)env[i], maxval);
    }
    env.resize(n);
    if (maxval <= gate) { err = "new envelope is effectively zero"; return false; }
    return true;
}

//============================================================================
//  The warp modes.  Each rewrites the extracted envelope in place; p[0] is the
//  window size, so a mode's own parameters start at p[1] exactly as they do on
//  CDP's command line ("envel warp <mode> in out wsize various_params").
//============================================================================
typedef bool (*EnvOp)(std::vector<float>&, const std::vector<double>&, std::string&);

//! Mode 1 NORMALISE -- envnorm().
bool op_normalise(std::vector<float>& e, const std::vector<double>&, std::string& err) {
    double mx = 0.0;
    for (float v : e) mx = std::max((double)v, mx);
    if (flteq(mx, 0.0)) { err = "envelope level is effectively zero: cannot normalise"; return false; }
    const double conv = 1.0 / mx;
    for (float& v : e) v = (float)std::min((double)v * conv, 1.0);
    return true;
}

//! Mode 2 REVERSE -- envrevers().
bool op_reverse(std::vector<float>& e, const std::vector<double>&, std::string&) {
    std::reverse(e.begin(), e.end());
    return true;
}

//! Mode 3 EXAGGERATE -- envexagg().  pow() on a 0..1 envelope: <1 lifts the
//! quiet parts, >1 digs them out.
bool op_exaggerate(std::vector<float>& e, const std::vector<double>& p, std::string&) {
    const double x = std::max(0.01, at(p, 1, 2.0));
    for (float& v : e) v = (float)std::pow(std::max(0.0, (double)v), x);
    return true;
}

//! Mode 4 ATTENUATE -- envatten().
bool op_attenuate(std::vector<float>& e, const std::vector<double>& p, std::string&) {
    const double a = at(p, 1, 0.5);
    for (float& v : e) v = (float)((double)v * a);
    return true;
}

//! Mode 5 LIFT -- envlift().  Saturates at 1; CDP prints a warning there and
//! carries on, which is the same thing as clamping.
bool op_lift(std::vector<float>& e, const std::vector<double>& p, std::string&) {
    const double l = at(p, 1, 0.0);
    for (float& v : e) v = (float)std::min((double)v + l, 1.0);
    return true;
}

//! envfuncs.c envintpl().
inline float envintpl(double here, double winbase, double base, double next) {
    return (float)(base + ((here - winbase) * (next - base)));
}

//! envshortn() -- timestretch < 1.
std::vector<float> env_shortn(const std::vector<float>& e, double ts) {
    const int64_t oldlen = (int64_t)e.size();
    if (oldlen < 2) return e;
    const double skip = 1.0 / ts;
    std::vector<float> nu;
    nu.push_back(e[0]);
    for (int64_t m = 1;; ++m) {
        const double here = skip * (double)m;
        // CDP writes one more value here and then excludes it from the new
        // length, so the trailing sample is discarded.  Same result: stop.
        if (here >= (double)(oldlen - 1)) break;
        const double winbase = std::floor(here);
        int64_t n = (int64_t)winbase;
        if (n < 0) n = 0;
        if (n > oldlen - 2) n = oldlen - 2;         // here < oldlen-1 already
        nu.push_back(envintpl(here, winbase, (double)e[(size_t)n],
                              (double)e[(size_t)(n + 1)]));
    }
    return nu;
}

//! envlenthn() -- timestretch > 1.
std::vector<float> env_lenthn(const std::vector<float>& e, double ts) {
    const int64_t oldlen = (int64_t)e.size();
    if (oldlen < 2) return e;
    const double skip = 1.0 / ts;
    std::vector<float> nu;
    double here = 0.0, next = (double)e[0];
    int64_t n = 0, m = 0;
    for (;;) {
        const double base = next;
        const double winbase = (double)n;
        if (++n > oldlen - 1) break;
        next = (double)e[(size_t)n];
        while (here <= (double)n) {
            nu.push_back(envintpl(here, winbase, base, next));
            here = skip * (double)(++m);
        }
    }
    return nu;
}

//! Mode 6 TIMESTRETCH -- envtstretch().  This is the only mode that changes the
//! envelope's LENGTH, not its values; the sound keeps its own duration, so a
//! stretched envelope is truncated by envreplace and a shrunk one has its last
//! value held over the tail by the imposition loop.
bool op_timestretch(std::vector<float>& e, const std::vector<double>& p, std::string& err) {
    const double ts = at(p, 1, 2.0);
    if (!(ts > 0.0)) { err = "timestretch must be greater than zero"; return false; }
    // An envelope of under two points has no span to stretch, so it is left
    // alone here rather than inside the two helpers.  Deciding it at the call
    // site also means neither helper's degenerate copy-the-input path survives
    // inlining, which is what GCC was warning about.
    if (e.size() < 2) return true;
    // Built into a separate vector and swapped in, rather than assigned over
    // its own source: the direct form is safe (the temporary is complete before
    // the assignment) but reads as self-assignment.
    if (ts > 1.0)      { std::vector<float> nu = env_lenthn(e, ts); e.swap(nu); }
    else if (ts < 1.0) { std::vector<float> nu = env_shortn(e, ts); e.swap(nu); }
    return true;
}

//! Mode 7 FLATTEN -- envflatn().  Moving average over N windows, over a copy
//! padded at both ends by repeating the first/last value; the split is
//! deliberately asymmetric (N/2 behind, N-N/2-1 ahead).
bool op_flatten(std::vector<float>& e, const std::vector<double>& p, std::string& err) {
    const int64_t envlen = (int64_t)e.size();
    int64_t flat = iat(p, 1, 4.0);
    if (flat < 1) flat = 1;
    if (flat >= envlen) { err = "flattening param too large for this input"; return false; }
    const int64_t back = flat / 2;
    const int64_t fwd  = flat - back - 1;
    std::vector<float> nu;
    nu.reserve((size_t)(envlen + flat));
    for (int64_t i = 0; i < back; ++i) nu.push_back(e[0]);
    nu.insert(nu.end(), e.begin(), e.end());
    for (int64_t i = 0; i < fwd; ++i)  nu.push_back(e[(size_t)(envlen - 1)]);
    for (int64_t i = 0; i < envlen; ++i) {
        double sum = 0.0;
        for (int64_t k = 0; k < flat; ++k) sum += (double)nu[(size_t)(i + k)];
        e[(size_t)i] = (float)(sum / (double)flat);
    }
    return true;
}

//! gatefilt(): TRUE while the mean of the next `smoothing` windows is still at
//! or below the gate -- i.e. "this rise is too brief to be worth opening for".
bool gatefilt(const std::vector<float>& e, size_t i, double gate, int64_t smoothing) {
    const int64_t remain = (int64_t)e.size() - (int64_t)i;
    const int64_t k = std::min(remain, smoothing);
    if (k <= 0) return true;
    double avg = 0.0;
    for (int64_t j = 0; j < k; ++j) avg += (double)e[i + (size_t)j];
    avg /= (double)k;
    return avg <= gate;
}

//! Mode 8 GATE -- envgate() / do_simple_gating() / do_smoothed_gating().
bool op_gate(std::vector<float>& e, const std::vector<double>& p, std::string&) {
    const double gate = at(p, 1, 0.3);
    const int64_t smoothing = std::max<int64_t>(0, iat(p, 2, 0.0));
    if (smoothing == 0) {
        for (float& v : e) if ((double)v < gate) v = 0.f;
        return true;
    }
    bool gating = true;
    for (size_t i = 0; i < e.size(); ++i) {
        if ((double)e[i] >= gate && (!gating || !gatefilt(e, i, gate, smoothing))) {
            gating = false;                       // let it through untouched
        } else {
            e[i] = 0.f;
            gating = true;
        }
    }
    return true;
}

//! Mode 9 INVERT -- envinvert() / do_env_invert().  Reflects the envelope about
//! MIRROR: what was loud becomes quiet and vice versa, with everything below
//! GATE forced to silence so that noise floors are not amplified into the fore-
//! ground.  ENV_MIRROR and ENV_THRESHOLD are the same parameter slot (pnames.h
//! defines both as 2), which is why envinvert() computes ranges from "mirror"
//! and do_env_invert() then reads "threshold".
bool op_invert(std::vector<float>& e, const std::vector<double>& p, std::string& err) {
    const double gate   = at(p, 1, 0.3);
    const double mirror = at(p, 2, 0.5);
    const double hirange = 1.0 - mirror;
    const double lorange = mirror - gate;
    if (lorange <= 0.0) { err = "invert: mirror must be above gate"; return false; }
    if (hirange <= 0.0) { err = "invert: mirror must be below 1.0"; return false; }
    const double upratio = hirange / lorange;
    const double dnratio = lorange / hirange;
    for (float& v : e) {
        const double x = (double)v;
        if (x < gate)             v = 0.f;
        else if (x <= mirror)     v = (float)(((mirror - x) * upratio) + mirror);
        else                      v = (float)(mirror - ((x - mirror) * dnratio));
    }
    return true;
}

//! Mode 10 LIMIT -- envlimit().  Everything above THRESHOLD is squeezed so the
//! envelope's top lands on LIMIT.
bool op_limit(std::vector<float>& e, const std::vector<double>& p, std::string& err) {
    const double limit     = at(p, 1, 1.0);
    const double threshold = at(p, 2, 0.3);
    const double toprange  = 1.0 - threshold;
    if (toprange <= 0.0) { err = "limit: threshold must be below 1.0"; return false; }
    const double squeeze = (limit - threshold) / toprange;
    for (float& v : e)
        if ((double)v > threshold)
            v = (float)(threshold + (((double)v - threshold) * squeeze));
    return true;
}

//! ispeak() / istrough() -- is this window the max/min of the `width` windows
//! centred (asymmetrically, for even widths) on it?
bool is_extreme(const std::vector<float>& e, int64_t q, int64_t width, bool peak) {
    if (width < 2) return true;
    int64_t up = width / 2, down = up;
    if (width % 2 == 0) down = up - 1;
    const int64_t lo = std::max<int64_t>(0, q - down);
    const int64_t hi = std::min<int64_t>((int64_t)e.size() - 1, q + up);
    for (int64_t r = lo; r <= hi; ++r) {
        if (peak) { if (e[(size_t)q] < e[(size_t)r]) return false; }
        else      { if (e[(size_t)q] > e[(size_t)r]) return false; }
    }
    return true;
}

//! digout_trough() -- zero `del` windows around a trough, stopping short of any
//! window already marked as a peak so that corrugation never eats a peak.
void digout_trough(std::vector<float>& e, const std::vector<float>& shadow,
                   int64_t del, int64_t q) {
    if (del < 2) { e[(size_t)q] = 0.f; return; }
    int64_t up = del / 2, down = up;
    if (del % 2 == 0) down = up - 1;
    int64_t lo = std::max<int64_t>(0, q - down);
    int64_t hi = std::min<int64_t>((int64_t)e.size() - 1, q + up);
    for (int64_t r = q; r >= lo; --r) if (shadow[(size_t)r] > FLTERR) { lo = r + 1; break; }
    for (int64_t r = q; r <= hi; ++r) if (shadow[(size_t)r] > FLTERR) { hi = r - 1; break; }
    for (int64_t r = lo; r <= hi; ++r) e[(size_t)r] = 0.f;
}

//! Mode 11 CORRUGATE -- envkorrug().  Two passes: mark the peaks, then dig out
//! the troughs between them.  Note that `upwards` is NOT reset between the two
//! passes in CDP -- the trough scan starts from whatever direction the peak
//! scan happened to finish in.  That is reproduced, not fixed: it changes which
//! trough (if any) at the very start of the file gets dug out, and "fixing" it
//! would make this port disagree with the program it claims to be.
bool op_corrugate(std::vector<float>& e, const std::vector<double>& p, std::string&) {
    const int64_t n = (int64_t)e.size();
    if (n < 2) return true;
    const int64_t del   = std::max<int64_t>(1, iat(p, 1, 2.0));
    const int64_t width = std::max<int64_t>(2, iat(p, 2, 4.0));
    std::vector<float> shadow((size_t)n, 0.f);

    bool upwards = e[1] > e[0];
    for (int64_t i = 1; i < n; ++i) {
        if (upwards) {
            if (e[(size_t)i] <= e[(size_t)(i - 1)]) {
                if (is_extreme(e, i - 1, width, true)) shadow[(size_t)(i - 1)] = 1.f;
                upwards = false;
            }
        } else if (e[(size_t)i] > e[(size_t)(i - 1)]) {
            upwards = true;
        }
    }
    for (int64_t i = 1; i < n; ++i) {
        if (upwards) {
            if (e[(size_t)i] <= e[(size_t)(i - 1)]) upwards = false;
        } else if (e[(size_t)i] > e[(size_t)(i - 1)]) {
            if (is_extreme(e, i - 1, width, false)) digout_trough(e, shadow, del, i - 1);
            upwards = true;
        }
    }
    return true;
}

//! Mode 12 EXPAND -- envexpand() / do_expanding().
bool op_expand(std::vector<float>& e, const std::vector<double>& p, std::string& err) {
    const double gate      = at(p, 1, 0.3);
    const double threshold = at(p, 2, 0.15);
    const int64_t smoothing = std::max<int64_t>(0, iat(p, 3, 0.0));
    const double toprange = 1.0 - gate;
    if (toprange <= 0.0) { err = "expand: gate must be below 1.0"; return false; }
    const double squeeze = (1.0 - threshold) / toprange;
    bool gating = true;
    for (size_t i = 0; i < e.size(); ++i) {
        if (smoothing) {
            if ((double)e[i] > gate &&
                (!gating || !gatefilt(e, i, gate, smoothing))) {
                e[i] = (float)(1.0 - ((1.0 - (double)e[i]) * squeeze));
                gating = false;
            } else {
                e[i] = 0.f;
                gating = true;
            }
        } else {
            if ((double)e[i] > gate) e[i] = (float)(1.0 - ((1.0 - (double)e[i]) * squeeze));
            else                     e[i] = 0.f;
        }
    }
    return true;
}

//! Mode 14 CEILING -- envceiling().  Flatten the envelope to its own maximum
//! everywhere; after envreplace that is a level-riding compressor.
bool op_ceiling(std::vector<float>& e, const std::vector<double>&, std::string&) {
    double mx = 0.0;
    for (float v : e) mx = std::max((double)v, mx);
    for (float& v : e) v = (float)mx;
    return true;
}

//! Mode 15 DUCKED -- envduck(), ENV_WARPING branch: where the envelope exceeds
//! THRESHOLD, pull it down to GATE; leave it alone elsewhere.
bool op_duck(std::vector<float>& e, const std::vector<double>& p, std::string&) {
    const double gate      = at(p, 1, 0.3);
    const double threshold = at(p, 2, 0.15);
    for (float& v : e) if ((double)v > threshold) v = (float)gate;
    return true;
}

//============================================================================
//  The warp pipeline -- envprocess.c, case ENV_WARPING:
//      extract origenv -> copy -> envelope_warp -> envreplace -> impose.
//
//  CORRUGATE (and TRIGGER, not ported) skip envreplace and are imposed as they
//  stand; corrugate additionally binarises to a 0/1 gate first, which is why
//  its own source comment warns "don't use REPLACE with corrugation".
//
//  One deliberate divergence: CDP passes the ORIGINAL window count to
//  impose_envel_on_sndfile() even after timestretch has shortened the envelope
//  array, so warp mode 6 with timestretch < 1 reads past the end of its own
//  allocation.  We pass the array's actual length.  Up to the point where CDP
//  starts reading uninitialised memory the two agree exactly.
//============================================================================
bool warp_run(const std::vector<Buffer>& in, const std::vector<double>& p,
              Buffer& out, std::string& err, const Progress& prog,
              EnvOp op, bool doReplace, bool binarise) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int64_t win = env_window_frames(at(p, 0, ENV_DEFAULT_WSIZE), in[0].sampleRate);
    out = in[0];

    const std::vector<float> orig = extract_env(out, win);
    if (orig.empty()) { err = "input too short for the envelope window"; return false; }
    if (prog && !prog(0.33)) { err = "cancelled"; return false; }

    std::vector<float> env = orig;
    if (!op(env, p, err)) return false;
    if (env.empty()) { err = "warped envelope is empty"; return false; }
    if (binarise) for (float& v : env) if (v > 0.f) v = 1.f;
    if (doReplace && !env_replace(env, orig, err)) return false;
    if (prog && !prog(0.66)) { err = "cancelled"; return false; }

    if (!impose_env(out, env, win, err)) return false;
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//============================================================================
//  The three registered non-warp sndfile operations.
//============================================================================

//! ENV_EXTRACT.  CDP writes the envelope to a binary envfile (one float per
//! window) or a breakpoint text file; neither is audio, so neither survives the
//! Buffer contract.  What is registered instead is the same measurement
//! rendered back out at audio rate through CDP's own imposition interpolator --
//! i.e. an envelope FOLLOWER, mono, ready to be looked at or multiplied into
//! something else.  The measurement (peak per quantised window) and the
//! interpolation (linear ramp window to window, last value held) are CDP's; the
//! choice to emit it as audio instead of a file is ours, and is the only way
//! this transformation can exist here at all.
bool env_extract_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int64_t win = env_window_frames(at(p, 0, ENV_DEFAULT_WSIZE), in[0].sampleRate);
    const std::vector<float> env = extract_env(in[0], win);
    if (env.empty()) { err = "input too short for the envelope window"; return false; }
    if (prog && !prog(0.5)) { err = "cancelled"; return false; }

    out.sampleRate = in[0].sampleRate;
    out.resize(1, in[0].frames());
    for (size_t i = 0; i < out.ch[0].size(); ++i) out.ch[0][i] = 1.f;
    apply_env(out, env, win, false);          // ramp the envelope onto DC 1.0
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//! ENV_IMPOSE, mode 1 (envelope from a second soundfile).  Input 0 is the
//! carrier, input 1 the donor.  Each file's envelope window is derived from its
//! OWN sample rate, as CDP does via rejig_buffering().  If the donor is shorter
//! its last envelope value is held for the rest of the carrier; if longer, the
//! tail is unused -- both are CDP's behaviour, not a fixup.
bool env_impose_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                    Buffer& out, std::string& err, const Progress& prog) {
    if (in.size() < 2)                     { err = "impose needs a carrier and an envelope donor"; return false; }
    if (in[0].empty())                     { err = "no input"; return false; }
    if (in[1].empty())                     { err = "envelope donor is empty"; return false; }
    const double ms = at(p, 0, ENV_DEFAULT_WSIZE);
    const std::vector<float> env = extract_env(in[1], env_window_frames(ms, in[1].sampleRate));
    if (env.empty()) { err = "envelope donor too short for the envelope window"; return false; }
    if (prog && !prog(0.5)) { err = "cancelled"; return false; }
    out = in[0];
    if (!impose_env(out, env, env_window_frames(ms, in[0].sampleRate), err)) return false;
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//! ENV_REPLACE, mode 1.  As impose, but the carrier's own envelope is divided
//! out first, so the result follows the donor's contour instead of merely being
//! multiplied by it.  CDP's own note: especially useful for restoring amplitude
//! after filtering with a time-varying Q.
bool env_replace_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.size() < 2)                     { err = "replace needs a carrier and an envelope donor"; return false; }
    if (in[0].empty())                     { err = "no input"; return false; }
    if (in[1].empty())                     { err = "envelope donor is empty"; return false; }
    const double ms = at(p, 0, ENV_DEFAULT_WSIZE);
    const int64_t winC = env_window_frames(ms, in[0].sampleRate);
    std::vector<float> env = extract_env(in[1], env_window_frames(ms, in[1].sampleRate));
    if (env.empty()) { err = "envelope donor too short for the envelope window"; return false; }
    out = in[0];
    const std::vector<float> orig = extract_env(out, winC);
    if (orig.empty()) { err = "input too short for the envelope window"; return false; }
    if (prog && !prog(0.5)) { err = "cancelled"; return false; }
    if (!env_replace(env, orig, err)) return false;
    if (!impose_env(out, env, winC, err)) return false;
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//! ENV_TREMOL -- envfuncs.c envelope_tremol() + envprepro.c
//! create_tremol_sintable().  The table is a UNIPOLAR sine, (sin+1)/2, so the
//! gain runs over [1-depth, 1] * gain and never changes sign.  This is the one
//! process in the family that is genuinely sample-by-sample: no window, no
//! lookahead, no second pass -- so it streams at zero latency.
//!
//! CDP's two modes select linear vs logarithmic interpolation BETWEEN
//! breakpoints in a frequency table.  With a scalar frequency they are
//! identical, so only one process is registered.
bool env_tremolo_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const double frq   = std::max(0.0, at(p, 0, 5.0));
    const double depth = std::min(1.0, std::max(0.0, at(p, 1, ENV_TREM_DEFAULT_DEPTH)));
    const double amp   = std::min(1.0, std::max(0.0, at(p, 2, 1.0)));

    static std::vector<double> sintab;            // built once; read-only after
    if (sintab.empty()) {
        sintab.resize(ENV_TREM_TABSIZE + 1);
        for (int n = 0; n < ENV_TREM_TABSIZE; ++n)
            sintab[(size_t)n] =
                (std::sin(3.14159265358979323846 * 2.0 * ((double)n / (double)ENV_TREM_TABSIZE)) + 1.0) * 0.5;
        sintab[(size_t)ENV_TREM_TABSIZE] = 0.5;   // wrap-around point
    }

    out = in[0];
    const int sr = out.sampleRate > 0 ? out.sampleRate : 48000;
    const double tabsize_over_srate = (double)ENV_TREM_TABSIZE / (double)sr;
    const int64_t n = out.frames();
    const int chans = out.channels();
    double fsinpos = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        int sinpos = (int)fsinpos;                                  // truncate
        if (sinpos < 0) sinpos = 0;
        if (sinpos >= ENV_TREM_TABSIZE) sinpos = ENV_TREM_TABSIZE - 1;
        const double frac = fsinpos - (double)sinpos;
        const double losin = sintab[(size_t)sinpos];
        const double hisin = sintab[(size_t)(sinpos + 1)];
        double val = losin + ((hisin - losin) * frac);
        val *= depth;
        val += (1.0 - depth);
        val *= amp;
        fsinpos += frq * tabsize_over_srate;
        fsinpos = std::fmod(fsinpos, (double)ENV_TREM_TABSIZE);
        for (int c = 0; c < chans; ++c) {
            float& x = out.ch[(size_t)c][(size_t)i];
            x = (float)((double)x * val);
        }
        if (prog && (i & 0xFFFF) == 0 && !prog((double)i / (double)n)) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//============================================================================
//  create_envelope() (envfuncs.c) -- the shared machine behind SWELL,
//  DOVETAIL and CURTAIL.
//
//  A handful of (time, level, slope) control points is expanded into a dense
//  breakpoint table.  Linear segments stay two points; exponential ones are
//  chopped into at most MINSPAN (16) pieces and each piece's level is
//  pow(ratio, attenator) -- so CDP's "exponential" fade is really a 16-point
//  piecewise-linear approximation of x^1.7 (x^3.4 for its doubly-exponential,
//  steeper variants).  Coarse, and reproduced as such: it is audibly what the
//  program sounds like.
//============================================================================
enum { ENVTYPE_LIN = 0, ENVTYPE_EXP = 1, ENVTYPE_DBL = 2 };

struct CtrlPt { double t, lev; int slope; };

std::vector<std::pair<double, double>>
create_envelope(const std::vector<CtrlPt>& in, double minQuantum) {
    std::vector<std::pair<double, double>> out;
    if (in.empty()) return out;
    out.push_back(std::make_pair(in[0].t, in[0].lev));
    for (size_t n = 1; n < in.size(); ++n) {
        const double dur = in[n].t - in[n - 1].t;
        const double levelstep = in[n].lev - in[n - 1].lev;
        const bool falling = levelstep <= 0.0;
        if (in[n].slope == ENVTYPE_LIN || !(dur > 0.0)) {
            out.push_back(std::make_pair(in[n].t, in[n].lev));
            continue;
        }
        const double atten = (in[n].slope == ENVTYPE_DBL) ? ATTENATOR * 2.0 : ATTENATOR;
        const double quantum = std::max(dur / (double)MINSPAN, minQuantum);
        int64_t newcnt = (int64_t)(dur / quantum);          // CDP truncates
        if (newcnt < 1) newcnt = 1;
        const double timestep = dur / (double)newcnt;
        for (int64_t m = 1; m <= newcnt; ++m) {
            const double t = in[n - 1].t + ((double)m * timestep);
            double lev;
            if (falling) {
                double ratio = std::max(1.0 - (((double)m * timestep) / dur), 0.0);
                ratio = std::pow(ratio, atten);
                lev = (std::fabs(levelstep) * ratio) + in[n].lev;
            } else {
                double ratio = ((double)m * timestep) / dur;
                ratio = std::min(std::pow(ratio, atten), 1.0);
                lev = (std::fabs(levelstep) * ratio) + in[n - 1].lev;
            }
            out.push_back(std::make_pair(t, lev));
        }
        out.back().first = in[n].t;      // CDP forces the segment end exactly
    }
    return out;
}

//! envel.c apply_brkpnt_envelope(): ramp the gain linearly between breakpoints,
//! sample by sample.  calc_samps_to_process() ends the OUTPUT at the last
//! breakpoint time unless that already equals the file duration -- which is how
//! `curtail` shortens the file as well as fading it.  CDP's per-sample loop
//! advances the gain before applying it (one increment of lead); that
//! half-sample bias is not reproduced, because plain interpolation by time is
//! what the loop is trying to be.
bool apply_brk(Buffer& b, const std::vector<std::pair<double, double>>& brk,
               Buffer& out, std::string& err) {
    if (brk.size() < 2) { err = "envelope needs at least two breakpoints"; return false; }
    const int64_t n = b.frames();
    const int sr = b.sampleRate > 0 ? b.sampleRate : 48000;
    const double dur = (double)n / (double)sr;
    const double lasttime = brk.back().first;
    int64_t endFrame = n;
    if (!flteq(lasttime, dur))
        endFrame = std::min<int64_t>((int64_t)std::llround(lasttime * (double)sr), n);
    if (endFrame <= 0) { err = "envelope ends at or before the start of the file"; return false; }

    out.sampleRate = b.sampleRate;
    out.resize(b.channels(), endFrame);
    size_t seg = 0;
    for (int64_t i = 0; i < endFrame; ++i) {
        const double t = (double)i / (double)sr;
        while (seg + 2 < brk.size() && t > brk[seg + 1].first) ++seg;
        const double t0 = brk[seg].first,     v0 = brk[seg].second;
        const double t1 = brk[seg + 1].first, v1 = brk[seg + 1].second;
        double g;
        if (t <= t0)                 g = v0;
        else if (t >= t1)            g = v1;
        else if (t1 - t0 <= 0.0)     g = v1;
        else                         g = v0 + ((t - t0) / (t1 - t0)) * (v1 - v0);
        for (int c = 0; c < b.channels(); ++c)
            out.ch[(size_t)c][(size_t)i] = (float)((double)b.ch[(size_t)c][(size_t)i] * g);
    }
    return true;
}

//! ENV_DOVETAILING (envprepro.c) -- fade in over the head, out over the tail.
//! CDP's `-t` unit flag (seconds/samples/grouped-samples) is dropped: seconds
//! only.  The doubly-exponential mode 2 is folded in as slope type 2 on each
//! end, which is exactly what that mode substitutes.
bool env_dovetail_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                      Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int sr = in[0].sampleRate > 0 ? in[0].sampleRate : 48000;
    const double dur = (double)in[0].frames() / (double)sr;
    const double startTrim = std::max(0.0, at(p, 0, 0.05));
    const double outFade   = std::max(0.0, at(p, 1, 0.05));
    const double endTrim   = dur - outFade;                 // CDP stores a TIME
    const int inType  = (int)std::min<int64_t>(2, std::max<int64_t>(0, iat(p, 2, 1.0)));
    const int outType = (int)std::min<int64_t>(2, std::max<int64_t>(0, iat(p, 3, 1.0)));
    if (endTrim < 0.0)         { err = "out-fade is longer than the file"; return false; }
    if (startTrim > dur)       { err = "in-fade is longer than the file"; return false; }
    if (endTrim < startTrim)   { err = "in-fade and out-fade overlap"; return false; }
    if (flteq(endTrim, dur) && flteq(startTrim, 0.0)) { err = "both fades are zero: no change"; return false; }

    std::vector<CtrlPt> c;
    if (flteq(startTrim, 0.0)) {
        c.push_back(CtrlPt{ 0.0, 1.0, ENVTYPE_LIN });
    } else {
        c.push_back(CtrlPt{ 0.0, 0.0, ENVTYPE_LIN });
        c.push_back(CtrlPt{ startTrim, 1.0, inType });
    }
    if (flteq(endTrim, dur)) {
        c.push_back(CtrlPt{ dur, 1.0, ENVTYPE_LIN });
    } else {
        c.push_back(CtrlPt{ endTrim, 1.0, ENVTYPE_LIN });
        c.push_back(CtrlPt{ dur, 0.0, outType });
    }
    Buffer src = in[0];
    if (prog && !prog(0.5)) { err = "cancelled"; return false; }
    // CDP's time_quantum floor for the sndfile fades is 2 samples, not 5 ms.
    if (!apply_brk(src, create_envelope(c, 2.0 / (double)sr), out, err)) return false;
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//! ENV_CURTAILING -- fade to zero somewhere inside the file, and END the file
//! there.  Modes 1/2/3 differ only in how the fade end is specified and 4/5/6
//! only in forcing the doubly-exponential slope, so both collapse into "fade
//! start, fade end, slope type".
bool env_curtail_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                     Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int sr = in[0].sampleRate > 0 ? in[0].sampleRate : 48000;
    const double dur = (double)in[0].frames() / (double)sr;
    const double start = at(p, 0, 0.0);
    double end = at(p, 1, 0.0);
    if (end <= 0.0) end = dur;                        // CDP mode 3: to file end
    const int type = (int)std::min<int64_t>(2, std::max<int64_t>(0, iat(p, 2, 1.0)));
    if (start < 0.0 || start >= dur) { err = "fade start is outside the file"; return false; }
    if (end > dur) end = dur;                         // CDP warns and clamps
    if (start >= end)                { err = "fade times are too close or reversed"; return false; }

    std::vector<CtrlPt> c;
    c.push_back(CtrlPt{ 0.0,   1.0, ENVTYPE_LIN });
    c.push_back(CtrlPt{ start, 1.0, ENVTYPE_LIN });
    c.push_back(CtrlPt{ end,   0.0, type });
    Buffer src = in[0];
    if (prog && !prog(0.5)) { err = "cancelled"; return false; }
    if (!apply_brk(src, create_envelope(c, 2.0 / (double)sr), out, err)) return false;
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//! ENV_SWELL -- fade in to a peak moment and out again.  Unlike dovetail and
//! curtail, swell is NOT in create_envelope()'s sample-resolution list, so its
//! exponential segments are quantised at ENV_MIN_WSIZE (5 ms) instead of two
//! samples.  Kept as-is.
bool env_swell_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (in.empty() || in[0].empty()) { err = "no input"; return false; }
    const int sr = in[0].sampleRate > 0 ? in[0].sampleRate : 48000;
    const double dur = (double)in[0].frames() / (double)sr;
    // An ABSOLUTE time cannot have a default that is valid for every input, so
    // 0 means "halfway" and anything past the end is clamped inside instead of
    // refused.  Refusing made the process unusable at its own defaults -- the
    // editor creates every node with defaults, so it failed the moment you
    // added it to a shorter clip.
    double peak = at(p, 0, 0.0);
    if (!(peak > 0.0)) peak = dur * 0.5;
    const double lo = dur * 0.001, hi = dur * 0.999;
    peak = std::max(lo, std::min(hi, peak));
    const int type = (int)std::min<int64_t>(2, std::max<int64_t>(0, iat(p, 1, 1.0)));
    if (!(dur > 0.0)) { err = "empty input"; return false; }

    std::vector<CtrlPt> c;
    c.push_back(CtrlPt{ 0.0,  0.0, ENVTYPE_LIN });
    c.push_back(CtrlPt{ peak, 1.0, type });
    c.push_back(CtrlPt{ dur,  0.0, type });
    Buffer src = in[0];
    if (prog && !prog(0.5)) { err = "cancelled"; return false; }
    if (!apply_brk(src, create_envelope(c, ENV_MIN_WSIZE * 0.001), out, err)) return false;
    if (prog && !prog(1.0)) { err = "cancelled"; return false; }
    return true;
}

//============================================================================
//  Registration helpers.
//============================================================================

//! The window-size parameter every windowed ENV process leads with.
ParamSpec wsizeSpec() {
    return ParamSpec{ "Window", "ms", ENV_MIN_WSIZE, ENV_MAX_WSIZE, ENV_DEFAULT_WSIZE,
                      false, "Envelope scanning window: the contour's time resolution." };
}

//! Latency of a windowed envelope process: the imposition ramp for window w
//! needs env[w] AND env[w+1], so two whole windows must be buffered before the
//! first frame can leave.  Reported for the non-streamable ones too -- it is
//! the real algorithmic delay; `streamable` is what says they cannot run live.
int64_t windowLatency(int sr, const std::vector<double>& p) {
    return 2 * env_window_frames(p.empty() ? ENV_DEFAULT_WSIZE : p[0], sr);
}

//! Register one warp mode.  `extra` are its parameters after the window size.
void addWarp(std::vector<Process>& r, const char* slug, const char* name,
             const char* help, EnvOp op, bool doReplace, bool binarise,
             std::vector<ParamSpec> extra) {
    Process p;
    p.slug = slug;
    p.name = name;
    p.group = "Envelope";
    p.help = help;
    // Every warp mode measures the whole envelope before it can impose it: the
    // imposition pass alone scans the entire file to find the peak it would
    // produce, and normalise/ceiling need a global maximum on top of that.
    // None of this can be done from a stream.
    p.streamable = false;
    p.latencyFrames = &windowLatency;
    p.params.push_back(wsizeSpec());
    for (ParamSpec& s : extra) p.params.push_back(std::move(s));
    p.run = [op, doReplace, binarise](const std::vector<Buffer>& in,
                                      const std::vector<double>& pr, Buffer& out,
                                      std::string& err, const Progress& prog) {
        return warp_run(in, pr, out, err, prog, op, doReplace, binarise);
    };
    r.push_back(std::move(p));
}

} // namespace

void register_env_processes() {
    std::vector<Process>& r = mutable_registry();

    // ---- extraction ------------------------------------------------------
    {
        Process p;
        p.slug = "env.extract";
        p.name = "Envelope Extract";
        p.group = "Envelope";
        p.help = "Follow the amplitude contour and emit it as a mono control signal.";
        // Genuinely block-by-block: the only thing held back is the next
        // window's peak, needed to close the ramp out of the current one.
        p.streamable = true;
        p.latencyFrames = &windowLatency;
        p.params = { wsizeSpec() };
        p.run = &env_extract_run;
        r.push_back(p);
    }

    // ---- imposition (two inputs: carrier + envelope donor) ----------------
    {
        Process p;
        p.slug = "env.impose";
        p.name = "Envelope Impose";
        p.group = "Envelope";
        p.help = "Multiply one sound by the amplitude contour of another.";
        p.minInputs = 2;
        p.maxInputs = 2;
        // Two passes over the whole carrier: measure the peak the envelope
        // would produce, scale the envelope to fit, then apply.
        p.streamable = false;
        p.latencyFrames = &windowLatency;
        p.params = { wsizeSpec() };
        p.run = &env_impose_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "env.replace";
        p.name = "Envelope Replace";
        p.group = "Envelope";
        p.help = "Strip a sound's own amplitude contour and give it another's.";
        p.minInputs = 2;
        p.maxInputs = 2;
        p.streamable = false;
        p.latencyFrames = &windowLatency;
        p.params = { wsizeSpec() };
        p.run = &env_replace_run;
        r.push_back(p);
    }

    // ---- tremolo: the one sample-by-sample member of the family -----------
    {
        Process p;
        p.slug = "env.tremolo";
        p.name = "Tremolo";
        p.group = "Envelope";
        p.help = "Modulate the level with a sine, superimposed on the existing level.";
        p.streamable = true;
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Frequency", "Hz", 0.0, ENV_TREM_MAXFRQ, 5.0, false,
              "Tremolo rate." },
            { "Depth", "", 0.0, 1.0, ENV_TREM_DEFAULT_DEPTH, false,
              "How far the level dips: gain swings between 1-depth and 1." },
            { "Gain", "", 0.0, 1.0, 1.0, false,
              "Overall signal gain applied on top of the tremolo." },
        };
        p.run = &env_tremolo_run;
        r.push_back(p);
    }

    // ---- fades built from a generated envelope ---------------------------
    {
        Process p;
        p.slug = "env.dovetail";
        p.name = "Dovetail";
        p.group = "Envelope";
        p.help = "Fade the start in and the end out, so the sound cannot click.";
        p.streamable = false;      // the out-fade is placed from the file's end
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "In fade", "s", 0.0, 3600.0, 0.05, false, "Duration of the start-of-file fade-in." },
            { "Out fade", "s", 0.0, 3600.0, 0.05, false, "Duration of the end-of-file fade-out." },
            { "In type", "", 0, 2, 1, true, "0 linear, 1 exponential, 2 doubly exponential (steeper)." },
            { "Out type", "", 0, 2, 1, true, "0 linear, 1 exponential, 2 doubly exponential (steeper)." },
        };
        p.run = &env_dovetail_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "env.curtail";
        p.name = "Curtail";
        p.group = "Envelope";
        p.help = "Fade to zero at a chosen time and end the sound there.";
        p.streamable = false;      // it also shortens the output
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Fade start", "s", 0.0, 3600.0, 0.0, false, "Time at which the fade begins." },
            { "Fade end", "s", 0.0, 3600.0, 0.0, false, "Time at which it reaches silence; 0 means the end of the file." },
            { "Type", "", 0, 2, 1, true, "0 linear, 1 exponential, 2 doubly exponential (steeper)." },
        };
        p.run = &env_curtail_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug = "env.swell";
        p.name = "Swell";
        p.group = "Envelope";
        p.help = "Fade in to a peak moment and back out from it.";
        p.streamable = false;      // the peak is placed relative to the whole
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Peak time", "s", 0.0, 3600.0, 0.0, false, "Where the peak falls; 0 = halfway." },
            { "Type", "", 0, 2, 1, true, "0 linear, 1 exponential, 2 doubly exponential (steeper)." },
        };
        p.run = &env_swell_run;
        r.push_back(p);
    }

    // ---- warp: reshape the sound's own envelope --------------------------
    // Mode numbers below are CDP's own ("envel warp 1-15"); mode 13 TRIGGER is
    // absent because it needs a user-supplied breakpoint ramp file, and there is
    // no way to hand one to a Process.
    addWarp(r, "env.warp.normalise", "Envelope Normalise",              // 1
            "Expand the envelope so its highest point is the maximum possible.",
            &op_normalise, true, false, {});
    addWarp(r, "env.warp.reverse", "Envelope Reverse",                  // 2
            "Time-reverse the amplitude contour while the sound plays forwards.",
            &op_reverse, true, false, {});
    addWarp(r, "env.warp.exaggerate", "Envelope Exaggerate",            // 3
            "Exaggerate the contour: below 1 boosts quiet parts, above 1 boosts loud.",
            &op_exaggerate, true, false,
            { { "Exaggerate", "", 0.01, 100.0, 2.0, false,
                "Below 1 lifts low levels, above 1 digs them out; 1 is no change." } });
    addWarp(r, "env.warp.attenuate", "Envelope Attenuate",              // 4
            "Scale the whole contour down.",
            &op_attenuate, true, false,
            { { "Attenuation", "", 0.0, 1.0, 0.5, false, "Multiplier applied to the envelope." } });
    addWarp(r, "env.warp.lift", "Envelope Lift",                        // 5
            "Raise the whole contour by a fixed amount, filling in the quiet parts.",
            &op_lift, true, false,
            { { "Lift", "", 0.0, 1.0, 0.0, false, "Added to every envelope value, clipped at 1." } });
    addWarp(r, "env.warp.timestretch", "Envelope Timestretch",          // 6
            "Stretch or shrink the contour in time while the sound keeps its length.",
            &op_timestretch, true, false,
            { { "Timestretch", "", 0.001, 1000.0, 2.0, false,
                "Above 1 stretches the contour, below 1 shrinks it." } });
    addWarp(r, "env.warp.flatten", "Envelope Flatten",                  // 7
            "Smooth the contour by averaging over neighbouring windows.",
            &op_flatten, true, false,
            { { "Flatten", "windows", 2, MAX_ENV_FLATN, 4, true,
                "Number of envelope windows averaged together." } });
    addWarp(r, "env.warp.gate", "Envelope Gate",                        // 8
            "Silence everything below a level.",
            &op_gate, true, false,
            { { "Gate", "", 0.0, 1.0, 0.3, false, "Levels below this are set to zero." },
              { "Smoothing", "windows", 0, MAX_ENV_SMOOTH, 0, true,
                "Excise low-level bursts shorter than this many windows; 0 turns it off." } });
    addWarp(r, "env.warp.invert", "Envelope Invert",                    // 9
            "Turn the contour upside down about a mirror level.",
            &op_invert, true, false,
            { { "Gate", "", 0.0, 1.0, 0.3, false, "Levels below this are set to zero." },
              { "Mirror", "", 0.0, 1.0, 0.5, false,
                "Level the contour is reflected about; must be above Gate and below 1." } });
    addWarp(r, "env.warp.limit", "Envelope Limit",                      // 10
            "Squeeze everything above a threshold down towards a ceiling.",
            &op_limit, true, false,
            { { "Limit", "", 0.0, 1.0, 1.0, false, "Level the loudest point is squeezed to." },
              { "Threshold", "", 0.0, 1.0, 0.3, false, "Levels above this are squeezed." } });
    addWarp(r, "env.warp.corrugate", "Envelope Corrugate",              // 11
            "Take every trough in the contour to zero, chopping the sound into blips.",
            &op_corrugate, /*replace*/ false, /*binarise*/ true,
            { { "Trough width", "windows", 1, MAX_PEAK_SEPARATION, 2, true,
                "Windows set to zero at each trough." },
              { "Peak separation", "windows", 2, MAX_PEAK_SEPARATION, 4, true,
                "Minimum window distance between peaks." } });
    addWarp(r, "env.warp.expand", "Envelope Expand",                    // 12
            "Push the contour upwards so its minimum becomes a threshold, gating below.",
            &op_expand, true, false,
            { { "Gate", "", 0.0, 1.0, 0.3, false, "Levels below this are set to zero." },
              { "Threshold", "", 0.0, 1.0, 0.15, false, "Level the gate point is expanded to." },
              { "Smoothing", "windows", 0, MAX_ENV_SMOOTH, 0, true,
                "Excise low-level bursts shorter than this many windows; 0 turns it off." } });
    addWarp(r, "env.warp.ceiling", "Envelope Ceiling",                  // 14
            "Force the contour to its own maximum everywhere: a level-rider.",
            &op_ceiling, true, false, {});
    addWarp(r, "env.warp.duck", "Envelope Duck",                        // 15
            "Pull the level down to a gate wherever the contour crosses a threshold.",
            &op_duck, true, false,
            { { "Gate", "", 0.0, 1.0, 0.3, false, "Level the loud parts are ducked to." },
              { "Threshold", "", 0.0, 1.0, 0.15, false, "Contour level that triggers ducking." } });
}

} } // namespace PatchKnob::cdp
