//----------------------------------------------------------------------------
//  src/engine/cdp/cdp_pvoc.cpp -- see cdp_pvoc.h.
//
//  A textbook phase vocoder, written here rather than lifted from CDP's own
//  pvoc because that one is welded to its analysis-FILE format (headers, disk
//  buffering, the pvxio2 layer).  What must match CDP is the DATA it hands the
//  spectral programs -- amplitude and true frequency per bin -- and that is what
//  Frame carries.
//----------------------------------------------------------------------------
#include "cdp_pvoc.h"

#include <algorithm>
#include <cmath>

namespace PatchKnob { namespace cdp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

//! In-place radix-2 FFT.  `sign` = -1 forward, +1 inverse (unnormalised).
void fft(std::vector<double>& re, std::vector<double>& im, int sign) {
    const int n = (int)re.size();
    if (n < 2) return;
    // bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[(size_t)i], re[(size_t)j]);
                     std::swap(im[(size_t)i], im[(size_t)j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = sign * kTwoPi / (double)len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const size_t a = (size_t)(i + k), b = (size_t)(i + k + len / 2);
                const double xr = re[b] * cr - im[b] * ci;
                const double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

//! Wrap to (-pi, pi] -- the phase-deviation step every phase vocoder needs.
inline double wrap(double p) {
    p = std::fmod(p + kPi, kTwoPi);
    if (p < 0.0) p += kTwoPi;
    return p - kPi;
}

//! Periodic Hann.  Used for BOTH analysis and synthesis; with hop = N/4 the
//! squared-window overlap-add sums to a constant, which the synthesis divides
//! out so a round trip is unity gain.
std::vector<double> hann(int n) {
    std::vector<double> w((size_t)n);
    for (int i = 0; i < n; ++i)
        w[(size_t)i] = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)n);
    return w;
}

int sane_fft(int n) {
    int v = 32;
    while (v < n && v < (1 << 16)) v <<= 1;
    return v;
}

} // namespace

std::vector<Frame> pvoc_analyse(const std::vector<float>& x, const PvocSpec& spec) {
    PvocSpec s = spec;
    s.fftSize = sane_fft(s.fftSize);
    if (s.overlap < 1) s.overlap = 1;
    const int N = s.fftSize, H = s.hop(), B = s.bins();
    std::vector<Frame> frames;
    if (x.empty()) return frames;

    const std::vector<double> w = hann(N);
    // Pad by one window at the front so the first real sample is fully analysed;
    // this padding IS the reported latency.
    std::vector<double> pad((size_t)N, 0.0);
    std::vector<double> sig;
    sig.reserve(x.size() + (size_t)N * 2);
    sig.insert(sig.end(), pad.begin(), pad.end());
    for (float v : x) sig.push_back((double)v);
    sig.insert(sig.end(), pad.begin(), pad.end());

    std::vector<double> lastPhase((size_t)B, 0.0);
    std::vector<double> re((size_t)N), im((size_t)N);
    const double binHz = (double)s.sampleRate / (double)N;
    const double expectAdvance = kTwoPi * (double)H / (double)N;

    for (size_t pos = 0; pos + (size_t)N <= sig.size(); pos += (size_t)H) {
        for (int i = 0; i < N; ++i) {
            re[(size_t)i] = sig[pos + (size_t)i] * w[(size_t)i];
            im[(size_t)i] = 0.0;
        }
        fft(re, im, -1);
        Frame f;
        f.amp.resize((size_t)B); f.freq.resize((size_t)B); f.phase.resize((size_t)B);
        for (int b = 0; b < B; ++b) {
            const double rr = re[(size_t)b], ii = im[(size_t)b];
            const double mag = std::sqrt(rr * rr + ii * ii);
            const double ph  = std::atan2(ii, rr);
            // True frequency from how far the phase advanced beyond what this
            // bin's centre would predict over one hop.
            const double dev = wrap(ph - lastPhase[(size_t)b] - expectAdvance * (double)b);
            lastPhase[(size_t)b] = ph;
            const double devBins = dev * (double)N / (kTwoPi * (double)H);
            f.amp[(size_t)b]   = (float)mag;
            f.freq[(size_t)b]  = (float)(((double)b + devBins) * binHz);
            f.phase[(size_t)b] = (float)ph;
        }
        frames.push_back(std::move(f));
    }
    return frames;
}

std::vector<float> pvoc_synthesise(const std::vector<Frame>& frames,
                                   const PvocSpec& spec, int64_t outFrames,
                                   int64_t skipSamples) {
    PvocSpec s = spec;
    s.fftSize = sane_fft(s.fftSize);
    if (s.overlap < 1) s.overlap = 1;
    const int N = s.fftSize, H = s.hop(), B = s.bins();
    std::vector<float> out;
    if (frames.empty()) return out;

    const std::vector<double> w = hann(N);
    const size_t total = (size_t)N + frames.size() * (size_t)H;
    std::vector<double> acc(total, 0.0), norm(total, 0.0);
    std::vector<double> phase((size_t)B, 0.0), locked((size_t)B, 0.0);
    std::vector<int> owner((size_t)B), peaks;
    std::vector<double> re((size_t)N), im((size_t)N);
    const double binHz = (double)s.sampleRate / (double)N;
    const double expectAdvance = kTwoPi * (double)H / (double)N;

    for (size_t fi = 0; fi < frames.size(); ++fi) {
        const Frame& f = frames[fi];
        std::fill(re.begin(), re.end(), 0.0);
        std::fill(im.begin(), im.end(), 0.0);
        const int nb = std::min(B, f.bins());

        // Invert the analysis: a bin's frequency says how fast its phase must
        // turn, so accumulate that per hop.
        for (int b = 0; b < nb; ++b) {
            const double devBins = (double)f.freq[(size_t)b] / binHz - (double)b;
            phase[(size_t)b] += expectAdvance * (double)b
                              + devBins * kTwoPi * (double)H / (double)N;
        }

        // PHASE LOCKING.  Each spectral peak owns the bins around it -- its
        // window main lobe -- and those bins take the peak's integrated phase
        // plus the offset they had at ANALYSIS time.  That keeps the lobe
        // internally coherent, so the partial resynthesises at full amplitude
        // instead of partly cancelling against itself.  Without a phase array
        // (a process that rebuilt frames from scratch) every bin owns itself
        // and this degrades to plain independent integration.
        const bool havePhase = (int)f.phase.size() >= nb;
        if (havePhase) {
            peaks.clear();
            for (int b = 1; b < nb - 1; ++b)
                if (f.amp[(size_t)b] > f.amp[(size_t)b - 1] &&
                    f.amp[(size_t)b] > f.amp[(size_t)b + 1])
                    peaks.push_back(b);
        }
        if (havePhase && !peaks.empty()) {
            size_t pi = 0;
            for (int b = 0; b < nb; ++b) {          // nearest peak, scanning forward
                while (pi + 1 < peaks.size() &&
                       std::abs(b - peaks[pi + 1]) < std::abs(b - peaks[pi])) ++pi;
                owner[(size_t)b] = peaks[pi];
            }
            for (int b = 0; b < nb; ++b) {
                const int p = owner[(size_t)b];
                locked[(size_t)b] = (p == b)
                    ? phase[(size_t)b]
                    : phase[(size_t)p] + ((double)f.phase[(size_t)b]
                                        - (double)f.phase[(size_t)p]);
            }
            // Carry the locked values forward, so a bin that becomes a peak in
            // a later frame starts from a coherent phase rather than a drifted
            // accumulator of its own.
            for (int b = 0; b < nb; ++b) phase[(size_t)b] = locked[(size_t)b];
        }

        for (int b = 0; b < nb; ++b) {
            const double mag = (double)f.amp[(size_t)b];
            re[(size_t)b] = mag * std::cos(phase[(size_t)b]);
            im[(size_t)b] = mag * std::sin(phase[(size_t)b]);
            if (b > 0 && b < N - b) {          // mirror for a real signal
                re[(size_t)(N - b)] =  re[(size_t)b];
                im[(size_t)(N - b)] = -im[(size_t)b];
            }
        }
        fft(re, im, +1);
        const size_t base = fi * (size_t)H;
        for (int i = 0; i < N; ++i) {
            const double v = re[(size_t)i] / (double)N;
            acc[base + (size_t)i]  += v * w[(size_t)i];
            norm[base + (size_t)i] += w[(size_t)i] * w[(size_t)i];
        }
    }

    // Drop the analysis pad, then divide out the summed window envelope so the
    // overlap-add is unity rather than window-shaped.
    const size_t skip = skipSamples >= 0 ? std::min((size_t)skipSamples, acc.size())
                                         : (size_t)N;
    const size_t avail = acc.size() > skip ? acc.size() - skip : 0;
    size_t want = outFrames > 0 ? (size_t)outFrames : avail;
    want = std::min(want, avail);
    out.resize(want);
    for (size_t i = 0; i < want; ++i) {
        const double n = norm[skip + i];
        const double v = n > 1e-8 ? acc[skip + i] / n : 0.0;
        out[i] = std::isfinite(v) ? (float)v : 0.f;
    }
    return out;
}

bool pvoc_process(const Buffer& in, Buffer& out, const PvocSpec& spec,
                  const std::function<void(std::vector<Frame>&)>& edit,
                  bool keepLength) {
    if (in.empty()) return false;
    PvocSpec s = spec;
    s.sampleRate = in.sampleRate > 0 ? in.sampleRate : 48000;
    out.sampleRate = s.sampleRate;
    out.ch.assign((size_t)in.channels(), {});
    for (int c = 0; c < in.channels(); ++c) {
        std::vector<Frame> fr = pvoc_analyse(in.ch[(size_t)c], s);
        if (edit) edit(fr);
        out.ch[(size_t)c] = pvoc_synthesise(fr, s, keepLength ? in.frames() : 0);
    }
    // Channels must come out the same length even if a process left ragged
    // frame counts, or the Buffer is malformed.
    size_t longest = 0;
    for (const auto& c : out.ch) longest = std::max(longest, c.size());
    for (auto& c : out.ch) c.resize(longest, 0.f);
    return longest > 0;
}

} } // namespace PatchKnob::cdp
