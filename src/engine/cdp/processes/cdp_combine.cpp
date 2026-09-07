//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_combine.cpp
//
//  Ported from the CDP COMBINE family (vendor/cdp8/dev/combine), which is
//      Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Exactly which files, and which function in each:
//      combine.c    specsum    -> combine.sum
//                   specdiff   -> combine.difference
//                   specmax / do_specmax -> combine.max
//                   speccross  -> combine.cross
//      ap_combine.c the usage text those four are documented by, and
//                   force_file_zero_to_be_largest_file (why slot 0 is the base)
//
//  ---- WHY THIS FILE CARRIES A PHASE VOCODER ----
//
//  COMBINE is not a waveform family.  Every one of its programs reads CDP
//  ANALYSIS FILES: a stream of windows, each holding (amplitude, frequency)
//  pairs for every analysis channel, produced by the separate `pvoc anal`
//  program.  specsum is literally two lines --
//
//      for(vc = 0; vc < dz->wanted; vc += 2)
//          dz->flbufptr[0][vc] += dz->param[SUM_CROSS] * dz->flbufptr[1][vc];
//
//  -- adding one file's amplitudes into another's and leaving the frequencies
//  alone.  The interesting part is not the arithmetic, it is the domain.
//
//  A library that takes audio buffers has to supply that domain itself, so this
//  file contains a compact phase vocoder: Hann window, hop = window/4, forward
//  FFT to (amplitude, true frequency) per bin, the CDP operation, then phase
//  accumulation and overlap-add back to samples.  The representation is the one
//  CDP's analysis files hold, so the ported operations are unchanged; what is
//  NOT identical is the analysis itself -- CDP's pvoc has its own window and
//  overlap, so a round trip through this file is not sample-identical to a
//  round trip through `pvoc anal` / `pvoc synth`.  What IS exact is the
//  relationship the operation claims: the transform is linear in amplitude, so
//  summing a source with itself at crossover 1 gives exactly twice the
//  amplitude, and differencing it against itself gives exact digital silence.
//
//  Dropped along the way: argv parsing, the `dataptr dz` blocks, the analysis
//  file reader/writer and its window accounting, breakpoint sweeping of the
//  crossover parameters, and the descriptor/formant machinery the family's
//  other programs need.
//
//  ---- SOURCE-MISMATCH POLICY, applied identically by every process here ----
//
//  SAMPLE RATE  Sources MUST agree.  A mismatch is REFUSED, naming both rates.
//               Nothing here resamples; bin k means a different frequency in
//               each file if the rates differ, so combining them is meaningless
//               rather than merely wrong.
//  LENGTH       The shorter source is PADDED WITH SILENCE to the longest, which
//               is what CDP's own MAX_ANALFILE logic does for sum and max, and
//               output length = the longest source.  (CDP's diff insists on
//               EQUAL lengths; padding is the gentler equivalent.)
//  CHANNELS     Output takes the LARGEST channel count; a narrower source is
//               read with its channel index wrapped, and each channel is
//               analysed and resynthesised independently.
//  LEVEL        No normalisation, in either direction.  combine.sum adds
//               amplitudes and CAN exceed full scale -- that is exactly what
//               CDP does, and the float path keeps the headroom rather than
//               re-gaining a chain behind the user's back.
//  FINITENESS   Every output sample is checked before it is written.
//
//  ---- ON STREAMING ----
//  These need two aligned analysis streams and Stream::process is handed one
//  buffer, so they declare streamable = false.  latencyFrames still reports the
//  analysis window honestly, since that is the delay the algorithm would owe if
//  the host ever ran it live.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

//! Parameter `i`, or `d` when the host has not supplied that many -- and when
//! it is not finite, since the window size becomes an allocation.
inline double at(const std::vector<double>& p, size_t i, double d) {
    return (i < p.size() && std::isfinite(p[i])) ? p[i] : d;
}

//! Sample `f` of channel `c` of one source: silence outside it, channel index
//! wrapped.  Every read goes through here, so the shorter source can never be
//! indexed past its end.
inline float tap(const Buffer& b, int c, int64_t f) {
    const int nc = b.channels();
    if (nc <= 0 || f < 0 || f >= b.frames()) return 0.f;
    return b.ch[(size_t)(c % nc)][(size_t)f];
}

bool accept(const std::vector<Buffer>& in, int need, std::string& err) {
    if ((int)in.size() < need) {
        err = "needs " + std::to_string(need) + " sources, got " +
              std::to_string((int)in.size());
        return false;
    }
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].empty()) {
            err = "source " + std::to_string((int)i + 1) + " is empty";
            return false;
        }
        if (in[i].sampleRate != in[0].sampleRate) {
            err = "sources have different sample rates (source 1 is " +
                  std::to_string(in[0].sampleRate) + " Hz, source " +
                  std::to_string((int)i + 1) + " is " +
                  std::to_string(in[i].sampleRate) +
                  " Hz) -- resample them to match, this will not do it for you";
            return false;
        }
    }
    return true;
}

//! Window sizes are powers of two; the knob is in samples, so round DOWN to one.
int fft_size(double v) {
    int want = (int)std::lround(v);
    want = std::max(256, std::min(4096, want));
    int n = 256;
    while (n * 2 <= want) n *= 2;
    return n;
}

//----------------------------------------------------------------------------
//  Iterative radix-2 FFT with a precomputed twiddle table (exact per stage --
//  no recurrence drift), in place, complex in and out.
//----------------------------------------------------------------------------
struct Fft {
    size_t n = 0;
    std::vector<double> cw, sw;         // cos/sin of -2*pi*t/n

    void init(size_t size) {
        n = size;
        cw.resize(n); sw.resize(n);
        for (size_t t = 0; t < n; ++t) {
            const double a = -kTwoPi * (double)t / (double)n;
            cw[t] = std::cos(a); sw[t] = std::sin(a);
        }
    }
    void run(double* re, double* im, bool inverse) const {
        for (size_t i = 1, j = 0; i < n; ++i) {         // bit reversal
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const size_t half = len >> 1, step = n / len;
            for (size_t i = 0; i < n; i += len) {
                for (size_t k = 0; k < half; ++k) {
                    const size_t t = k * step;
                    const double wr = cw[t];
                    const double wi = inverse ? -sw[t] : sw[t];
                    const size_t a = i + k, b = a + half;
                    const double vr = re[b] * wr - im[b] * wi;
                    const double vi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - vr; im[b] = im[a] - vi;
                    re[a] += vr;        im[a] += vi;
                }
            }
        }
        if (inverse) {
            const double k = 1.0 / (double)n;
            for (size_t i = 0; i < n; ++i) { re[i] *= k; im[i] *= k; }
        }
    }
};

//----------------------------------------------------------------------------
//  The analysis/synthesis pair.  A window's data is exactly what a CDP analysis
//  file window holds: amp[k] and frq[k] in Hz for each of the N/2+1 channels.
//----------------------------------------------------------------------------
struct Pvoc {
    int N = 1024, H = 256, K = 513;
    double sr = 48000.0;
    std::vector<double> win;
    double winNorm = 1.5;
    Fft fft;

    void init(int fftSize, int rate) {
        N = fftSize; H = N / 4; K = N / 2 + 1;
        sr = rate > 0 ? (double)rate : 48000.0;
        win.resize((size_t)N);
        for (int i = 0; i < N; ++i)
            win[(size_t)i] = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)N);
        // The window is applied at BOTH ends, so overlap-add sums w^2.  For a
        // Hann window at hop N/4 that sum is a constant 1.5, but measure it
        // rather than assume it: changing the hop would otherwise change the
        // gain silently.
        winNorm = 0.0;
        for (int o = 0; o < H; ++o) {
            double s = 0.0;
            for (int m = o; m < N; m += H) s += win[(size_t)m] * win[(size_t)m];
            winNorm = std::max(winNorm, s);
        }
        if (!(winNorm > 1e-12)) winNorm = 1.0;
        fft.init((size_t)N);
    }

    //! One window of samples -> amplitude and true frequency per channel.  The
    //! frequency is the bin centre corrected by the phase advance since the
    //! previous window, which is what makes an analysis file's FREQ field a
    //! real frequency and not just a bin number.
    void analyse(const double* seg, std::vector<double>& lastPhase,
                 std::vector<double>& amp, std::vector<double>& frq,
                 std::vector<double>& re, std::vector<double>& im) const {
        for (int i = 0; i < N; ++i) {
            re[(size_t)i] = seg[i] * win[(size_t)i];
            im[(size_t)i] = 0.0;
        }
        fft.run(re.data(), im.data(), false);
        const double expct = kTwoPi * (double)H / (double)N;
        const double binHz = sr / (double)N;
        for (int k = 0; k < K; ++k) {
            const double rr = re[(size_t)k], ii = im[(size_t)k];
            const double ph = std::atan2(ii, rr);
            double d = ph - lastPhase[(size_t)k];
            lastPhase[(size_t)k] = ph;
            d -= (double)k * expct;                       // expected advance
            d -= kTwoPi * std::floor(d / kTwoPi + 0.5);   // wrap into +-pi
            amp[(size_t)k] = std::sqrt(rr * rr + ii * ii);
            frq[(size_t)k] = ((double)k + d / expct) * binHz;
        }
    }

    //! Amplitude and frequency -> one window of samples.  Phase is accumulated
    //! per channel from the frequency, which is what lets an operation rewrite
    //! amplitudes and frequencies independently and still resynthesise.
    void synthesise(const std::vector<double>& amp, const std::vector<double>& frq,
                    std::vector<double>& sumPhase, double* seg,
                    std::vector<double>& re, std::vector<double>& im) const {
        const double perHop = kTwoPi * (double)H / sr;
        std::fill(re.begin(), re.end(), 0.0);
        std::fill(im.begin(), im.end(), 0.0);
        for (int k = 0; k < K; ++k) {
            double ph = sumPhase[(size_t)k] + frq[(size_t)k] * perHop;
            ph = std::fmod(ph, kTwoPi);                   // bounded accumulator
            if (!std::isfinite(ph)) ph = 0.0;
            sumPhase[(size_t)k] = ph;
            const double a = amp[(size_t)k];
            re[(size_t)k] = a * std::cos(ph);
            im[(size_t)k] = a * std::sin(ph);
        }
        im[0] = 0.0;                                      // DC and Nyquist real
        im[(size_t)(N / 2)] = 0.0;
        for (int k = 1; k < N / 2; ++k) {                 // conjugate symmetry
            re[(size_t)(N - k)] =  re[(size_t)k];
            im[(size_t)(N - k)] = -im[(size_t)k];
        }
        fft.run(re.data(), im.data(), true);
        const double k = 1.0 / winNorm;
        for (int i = 0; i < N; ++i)
            seg[i] = re[(size_t)i] * win[(size_t)i] * k;
    }
};

//! One CDP window operation: read the sources' channels, write the output's.
using SpectralOp = void (*)(const std::vector<std::vector<double>>& amp,
                            const std::vector<std::vector<double>>& frq,
                            const std::vector<double>& params,
                            std::vector<double>& outAmp,
                            std::vector<double>& outFrq);

//----------------------------------------------------------------------------
//  The driver every process here shares: analyse each source window by window,
//  apply the CDP operation, resynthesise.  Frames are aligned in TIME, so
//  window w of every source covers the same samples -- which is what makes
//  "the same analysis channel of two files" mean anything.
//----------------------------------------------------------------------------
bool run_spectral(const std::vector<Buffer>& in, const std::vector<double>& p,
                  int fftSize, SpectralOp op,
                  Buffer& out, std::string& err, const Progress& prog) {
    int64_t len = 0; int chans = 1;
    for (const Buffer& b : in) {
        len = std::max(len, b.frames());
        chans = std::max(chans, b.channels());
    }
    Pvoc pv;
    pv.init(fftSize, in[0].sampleRate);
    const int N = pv.N, H = pv.H, K = pv.K;

    // Pad a window in front so the first sample is fully covered by the
    // overlap-add, and two behind so the last one is; then trim both away.
    const int64_t pad = N;
    const int64_t total = pad + len + 2 * (int64_t)N;
    const int64_t frames = (total - N) / H + 1;

    out.sampleRate = in[0].sampleRate;
    out.resize(chans, len);

    const size_t S = in.size();
    std::vector<std::vector<double>> lastPhase(S, std::vector<double>((size_t)K, 0.0));
    std::vector<std::vector<double>> amp(S, std::vector<double>((size_t)K, 0.0));
    std::vector<std::vector<double>> frq(S, std::vector<double>((size_t)K, 0.0));
    std::vector<double> sumPhase((size_t)K, 0.0);
    std::vector<double> outAmp((size_t)K, 0.0), outFrq((size_t)K, 0.0);
    std::vector<double> re((size_t)N), im((size_t)N);
    std::vector<double> seg((size_t)N), osg((size_t)N), ola;

    for (int c = 0; c < chans; ++c) {
        for (std::vector<double>& v : lastPhase) std::fill(v.begin(), v.end(), 0.0);
        std::fill(sumPhase.begin(), sumPhase.end(), 0.0);
        ola.assign((size_t)total, 0.0);

        for (int64_t f = 0; f < frames; ++f) {
            const int64_t pos = f * H;
            for (size_t s = 0; s < S; ++s) {
                for (int i = 0; i < N; ++i)
                    seg[(size_t)i] = (double)tap(in[s], c, pos + i - pad);
                pv.analyse(seg.data(), lastPhase[s], amp[s], frq[s], re, im);
            }
            op(amp, frq, p, outAmp, outFrq);
            pv.synthesise(outAmp, outFrq, sumPhase, osg.data(), re, im);
            for (int i = 0; i < N; ++i) ola[(size_t)(pos + i)] += osg[(size_t)i];
        }
        std::vector<float>& o = out.ch[(size_t)c];
        for (int64_t i = 0; i < len; ++i) {
            const double v = ola[(size_t)(pad + i)];
            o[(size_t)i] = std::isfinite(v) ? (float)v : 0.f;
        }
        if (prog && !prog((double)(c + 1) / (double)chans)) {
            err = "cancelled"; return false;
        }
    }
    return true;
}

//----------------------------------------------------------------------------
//  The four operations, each one CDP's loop body over analysis channels.
//----------------------------------------------------------------------------

//! specsum: amplitudes add, slot 0 keeps its frequencies.
void op_sum(const std::vector<std::vector<double>>& amp,
            const std::vector<std::vector<double>>& frq,
            const std::vector<double>& p,
            std::vector<double>& outAmp, std::vector<double>& outFrq) {
    const double cross = clamp01(at(p, 0, 1.0));
    for (size_t k = 0; k < outAmp.size(); ++k) {
        outAmp[k] = amp[0][k] + cross * amp[1][k];
        outFrq[k] = frq[0][k];
    }
}

//! specdiff: slot 1's amplitudes are SUBTRACTED FROM slot 0's, clipped at zero
//! unless CDP's -a flag ("retains any subzero amplitudes") is set.
void op_diff(const std::vector<std::vector<double>>& amp,
             const std::vector<std::vector<double>>& frq,
             const std::vector<double>& p,
             std::vector<double>& outAmp, std::vector<double>& outFrq) {
    const double cross = clamp01(at(p, 0, 1.0));
    const bool subzero = at(p, 1, 0.0) >= 0.5;
    for (size_t k = 0; k < outAmp.size(); ++k) {
        const double d = amp[0][k] - cross * amp[1][k];
        outAmp[k] = subzero ? d : std::max(0.0, d);
        outFrq[k] = frq[0][k];
    }
}

//! do_specmax: the loudest source wins the channel, and brings ITS frequency
//! with it -- amplitude and frequency are never taken from different files.
void op_max(const std::vector<std::vector<double>>& amp,
            const std::vector<std::vector<double>>& frq,
            const std::vector<double>&,
            std::vector<double>& outAmp, std::vector<double>& outFrq) {
    for (size_t k = 0; k < outAmp.size(); ++k) {
        double a = amp[0][k], f = frq[0][k];
        for (size_t s = 1; s < amp.size(); ++s)
            if (amp[s][k] > a) { a = amp[s][k]; f = frq[s][k]; }
        outAmp[k] = a;
        outFrq[k] = f;
    }
}

//! speccross: slot 1's amplitudes replace slot 0's, interpolated -- and the
//! frequencies stay slot 0's, which is the whole point.  At interpolation 1 you
//! hear slot 1's spectral shape sung by slot 0's partials.
void op_cross(const std::vector<std::vector<double>>& amp,
              const std::vector<std::vector<double>>& frq,
              const std::vector<double>& p,
              std::vector<double>& outAmp, std::vector<double>& outFrq) {
    const double t = clamp01(at(p, 0, 1.0));
    for (size_t k = 0; k < outAmp.size(); ++k) {
        outAmp[k] = amp[0][k] + (amp[1][k] - amp[0][k]) * t;
        outFrq[k] = frq[0][k];
    }
}

bool sum_run(const std::vector<Buffer>& in, const std::vector<double>& p,
             Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    return run_spectral(in, p, fft_size(at(p, 1, 1024.0)), &op_sum, out, err, prog);
}
bool diff_run(const std::vector<Buffer>& in, const std::vector<double>& p,
              Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    return run_spectral(in, p, fft_size(at(p, 2, 1024.0)), &op_diff, out, err, prog);
}
bool max_run(const std::vector<Buffer>& in, const std::vector<double>& p,
             Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    return run_spectral(in, p, fft_size(at(p, 0, 1024.0)), &op_max, out, err, prog);
}
bool cross_run(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    return run_spectral(in, p, fft_size(at(p, 1, 1024.0)), &op_cross, out, err, prog);
}

//! The analysis window is the delay the algorithm owes: nothing can be emitted
//! before one is full.  Reported honestly even though these do not stream.
ParamSpec window_param() {
    return { "Window", "samples", 256, 4096, 1024, true,
             "FFT analysis window; longer resolves frequency better, shorter "
             "resolves time better. Rounded down to a power of two." };
}

} // namespace

void register_combine_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug  = "combine.sum";
        p.name  = "Spectral Sum";
        p.group = "Combine";
        p.help  = "Add the spectrum of slot 1 into the spectrum of slot 0. "
                  "Slot 0's frequencies are kept, so slot 1 thickens it rather "
                  "than detuning it.";
        p.minInputs = 2; p.maxInputs = 2;
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>& pr) -> int64_t {
            return fft_size(at(pr, 1, 1024.0));
        };
        p.params = {
            { "Crossover", "", 0.0, 1.0, 1.0, false,
              "How much of slot 1's spectrum is added to slot 0's." },
            window_param(),
        };
        p.run = &sum_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "combine.difference";
        p.name  = "Spectral Difference";
        p.group = "Combine";
        p.help  = "SLOT 1 IS SUBTRACTED FROM SLOT 0: what is left is the part "
                  "of slot 0 that slot 1 does not account for. Subtracting a "
                  "sound from itself gives silence.";
        p.minInputs = 2; p.maxInputs = 2;
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>& pr) -> int64_t {
            return fft_size(at(pr, 2, 1024.0));
        };
        p.params = {
            { "Crossover", "", 0.0, 1.0, 1.0, false,
              "How much of slot 1's spectrum is subtracted from slot 0's." },
            { "Keep negative", "", 0, 1, 0, true,
              "Retain amplitudes driven below zero instead of clipping them "
              "there (CDP's -a flag)." },
            window_param(),
        };
        p.run = &diff_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "combine.max";
        p.name  = "Spectral Max";
        p.group = "Combine";
        p.help  = "In every analysis channel take the loudest source, with its "
                  "own frequency. The result is the envelope of all of them at "
                  "once. Slot order does not matter.";
        p.minInputs = 2; p.maxInputs = 8;
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>& pr) -> int64_t {
            return fft_size(at(pr, 0, 1024.0));
        };
        p.params = { window_param() };
        p.run = &max_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "combine.cross";
        p.name  = "Spectral Cross";
        p.group = "Combine";
        p.help  = "Put slot 1's spectral AMPLITUDES onto slot 0's FREQUENCIES: "
                  "slot 0 supplies the pitch and slot 1 the colour.";
        p.minInputs = 2; p.maxInputs = 2;
        p.streamable = false;
        p.latencyFrames = [](int, const std::vector<double>& pr) -> int64_t {
            return fft_size(at(pr, 1, 1024.0));
        };
        p.params = {
            { "Interpolation", "", 0.0, 1.0, 1.0, false,
              "0 leaves slot 0 alone, 1 takes slot 1's amplitudes entirely." },
            window_param(),
        };
        p.run = &cross_run;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
