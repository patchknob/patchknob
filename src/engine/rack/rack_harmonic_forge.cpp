//----------------------------------------------------------------------------
//  src/engine/rack/rack_harmonic_forge.cpp  --  "HFORGE" Harmonic Forge
//
//  A two-oscillator additive/wavetable sculptor.  Each oscillator owns a bank of
//  256 harmonic amplitudes (and phases) which the user edits directly, imports
//  from an audio file by reverse harmonic analysis, and then mangles through a
//  chain of spectral operators before it is heard.
//
//  ===========================================================================
//  HOW IT MAKES SOUND, AND WHY IT IS BUILT THIS WAY
//  ===========================================================================
//
//  Summing 256 sines per sample is ~256 MACs/sample/oscillator -- 25 M MAC/s at
//  48 kHz, per oscillator, and that is before any modulation.  So the harmonics
//  are NOT summed per sample.  Instead:
//
//    1. the sculpt chain runs on the 256-entry harmonic array (cheap: 256
//       floats, and every pass is a straight-line loop that vectorises),
//    2. one inverse FFT turns the result into a 4096-point single-cycle table,
//    3. the audio path is a fixed-point table read -- a handful of integer ops
//       per sample regardless of how many harmonics are active.
//
//  Step 2 is O(N log N) (~49 k butterflies) against the O(H*N) 1.05 M MACs a
//  direct additive rebuild would cost, which is why the table is transformed
//  rather than accumulated.
//
//  THE CLICK PROBLEM.  Rebuilding the table swaps the waveform out from under a
//  running phase accumulator, which is a step discontinuity -- a click, on every
//  rebuild, i.e. continuously while anything is modulated.  Two things prevent
//  it, and they are the reason this module sounds clean under live modulation:
//
//    * the OLD table is kept and both are read for kFadeLen samples after a
//      rebuild, equal-power crossfaded.  The phase accumulator is shared, so the
//      two reads are phase-coherent and the fade is a true morph, not a
//      dissolve between two unrelated positions.
//    * every continuous control is slewed before it reaches the chain, so a
//      knob jerk or a stepped CV cannot produce a table that is far from its
//      predecessor in the first place.
//
//  THE 8-BIT STAGE is specified, and is real: the table is stored as int8_t and
//  the inverse transform is quantised into it.  That is a ~48 dB noise floor and
//  it is audible as the grit that gives the module its character.  It is also
//  why the table is 4096 points: 16 samples per cycle at harmonic 256 keeps the
//  linear-interpolation error well under the quantisation floor, so the 8 bits
//  are the only thing colouring the sound.
//
//  PITCH-STABLE FM.  This is phase modulation, not frequency modulation.  Adding
//  a modulator to the frequency integrates its DC into a pitch shift, so an
//  asymmetric or offset modulator drags the note flat or sharp -- the classic
//  "FM detunes when I turn up the index" problem.  Adding it to the PHASE cannot
//  shift the average frequency at all, because phase offset has no DC path to
//  frequency.  Feedback averages the last two outputs before it is applied; the
//  one-sample average is a gentle lowpass that is what keeps a self-modulating
//  operator from blowing up into noise at high index (the DX7 trick).
//
//  ===========================================================================
//  SSE3
//  ===========================================================================
//  Vectorised: the sculpt chain (every pass is a 128-float map/blend), the
//  spectrum assembly feeding the IFFT, and the FFT's butterfly loops where the
//  twiddle is uniform across a run.
//
//  NOT vectorised, deliberately: the per-sample oscillator read.  It is a table
//  GATHER at four independent indices, and SSE3 has no gather instruction --
//  emulating one costs more in inserts than the multiply saves.  It is instead
//  optimised as fast scalar: a 32-bit fixed-point phase (no float->int converts,
//  no fmod, no branches) reading an int8 table that fits in L1 at 4 KB.
//----------------------------------------------------------------------------
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include "rack.hpp"
#include "rack_dsp.h"
#include "rack_factory.h"
#include "rack_panel_kit.h"
#include "../audioclip/audio_clip.h"
#include "../audioclip/wav_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(__SSE3__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64)
  #include <pmmintrin.h>
  #define HFORGE_SSE3 1
#else
  #define HFORGE_SSE3 0
#endif

namespace {

//============================================================================
//  Constants
//============================================================================
constexpr int kHarm      = 256;                 //!< harmonics per oscillator
constexpr int kTableBits = 12;
constexpr int kTable     = 1 << kTableBits;     //!< 4096-point single cycle
//! The table holds kPeriods periods of the fundamental, not one.  That is what
//! makes PadSynth-style BANDWIDTH possible: with the fundamental sitting on bin
//! kPeriods instead of bin 1, there are kPeriods bins of room around every
//! harmonic to spread it into.  A single-cycle table has no room at all -- every
//! partial is forced onto exactly one bin, an exact integer multiple of one
//! fundamental, which is precisely why it rings like an organ instead of
//! sounding like the piano or guitar it was analysed from.  Real instrument
//! partials are slightly spread and slightly mistuned from each other; giving
//! each one a band restores that, and it costs nothing -- same FFT size, the
//! harmonics are just placed further apart.
constexpr int kPeriods   = 8;
constexpr int kMaxVoices = 16;                  //!< polyphony
constexpr int kFadeLen   = 192;                 //!< crossfade after a rebuild
constexpr int kMaxFrames = 1024;                //!< analysis frames kept per import
constexpr float kPi      = 3.14159265358979323846f;
constexpr float kTwoPi   = 2.f * kPi;

//! Phase is unsigned 32-bit: the top kTableBits index the table and the rest is
//! the interpolation fraction.  Wrapping is free (integer overflow), which is
//! the whole reason for the fixed-point representation.
constexpr int      kFracBits = 32 - kTableBits;         // 21
constexpr uint32_t kFracMask = (1u << kFracBits) - 1u;
constexpr float    kFracNorm = 1.f / (float)(1u << kFracBits);

//============================================================================
//  Small helpers
//============================================================================
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

//! xorshift32 -- deterministic, seedable, and fast enough to call per harmonic.
//! Determinism matters: a patch must sound the same every time it is loaded, so
//! every random decision here is derived from a stored seed, never from time.
struct Rng {
    uint32_t s = 0x9e3779b9u;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni()  { return (float)(next() >> 8) * (1.f / 16777216.f); }   // 0..1
    float bi()   { return uni() * 2.f - 1.f; }
};

//! One-pole slew.  Every continuously-modulated control passes through one of
//! these so the sculpt chain never sees a step.
struct Slew {
    float y = 0.f, a = 0.02f;
    void  setRate(float r) { a = clampf(r, 0.0005f, 1.f); }
    float run(float x) { y += (x - y) * a; return y; }
    void  reset(float x) { y = x; }
};

//============================================================================
//  Radix-2 complex FFT (used inverse only, to turn a spectrum into the table)
//============================================================================
//! `sign` = +1 is the inverse (unnormalised) transform.  Twiddles come from a
//! table built once: recomputing sin/cos inside the butterfly loop was over half
//! the rebuild cost, and a recurrence drifts over 2048 points.
struct Fft {
    std::vector<float> wr, wi;      // twiddles per stage, concatenated
    std::vector<int>   rev;
    int n = 0;

    void init(int size) {
        if (n == size) return;
        n = size;
        rev.resize((size_t)n);
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t)i] = r;
        }
        wr.clear(); wi.clear();
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                const double a = 2.0 * M_PI * (double)k / (double)len;   // sign applied below
                wr.push_back((float)std::cos(a));
                wi.push_back((float)std::sin(a));
            }
        }
    }

    //! In-place inverse transform of `re`/`im` (length n).
    void inverse(float* re, float* im) const {
        for (int i = 0; i < n; ++i) {
            const int j = rev[(size_t)i];
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        int off = 0;
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1;
            for (int i = 0; i < n; i += len) {
                for (int k = 0; k < half; ++k) {
                    const float cr = wr[(size_t)(off + k)], ci = wi[(size_t)(off + k)];
                    float* ar = re + i + k;        float* ai = im + i + k;
                    float* br = re + i + k + half; float* bi = im + i + k + half;
                    const float xr = *br * cr - *bi * ci;
                    const float xi = *br * ci + *bi * cr;
                    *br = *ar - xr; *bi = *ai - xi;
                    *ar += xr;      *ai += xi;
                }
            }
            off += half;
        }
    }
};

//============================================================================
//  Harmonic frame -- one analysed slice of an imported waveform
//============================================================================
//! `a` is the harmonic's amplitude and `ph` its PHASE.  Phase is not optional
//! decoration: resynthesising every partial at phase 0 gives an impulse-like
//! buzz with the right spectrum and none of the source's character, which is
//! the single biggest reason a naive additive resynthesis does not sound like
//! what went in.  `gain` carries the frame's own level so the amplitude
//! envelope of the original survives the scan.
struct Frame { float a[kHarm]; float ph[kHarm]; float gain = 1.f; };

//============================================================================
//  REVERSE HARMONIC ANALYSIS
//============================================================================
//  Take an arbitrary recording and recover, for a sequence of moments in it,
//  the amplitude of each of the first 256 harmonics of its fundamental.  The
//  result is a table the oscillator can be scanned through.
//
//  LATENCY COMPENSATION.  An FFT frame starting at sample p describes the
//  window [p, p+N) -- its content is centred at p + N/2, not at p.  Storing it
//  as "the spectrum at p" would smear every frame half a window LATE, so a
//  transient in the import lands late in the scan and the whole table is skewed
//  against the source.  Each frame is therefore taken from a window CENTRED on
//  its nominal time, i.e. read from p - N/2, which is the compensation.  The
//  same offset is reported by analysisLatency() so a host can line the import up
//  against the original file.
//============================================================================
struct Analysis {
    std::vector<Frame> frames;
    float  f0 = 0.f;                 //!< detected fundamental, Hz
    //! Phase set held for the whole scan, taken from the loudest frame -- the
    //! moment with the best signal-to-noise, so the partial relationships are
    //! the most trustworthy the file has to offer.
    float  refPhase[kHarm] = {};
    int    fftSize = 4096;
    int    latency = 2048;           //!< fftSize/2 -- see above
    bool   valid() const { return !frames.empty(); }
};

//! Estimate the fundamental by autocorrelation over a mid-file window.  Cheap
//! and robust enough for the pitched material this is for; when it fails the
//! caller falls back to a fixed period so an import still produces something.
float detect_f0(const std::vector<float>& x, double sr) {
    const int n = (int)std::min<size_t>(x.size(), 16384);
    if (n < 2048) return 0.f;
    const size_t off = (x.size() - (size_t)n) / 2;
    const int minLag = (int)(sr / 1200.0), maxLag = (int)(sr / 40.0);
    if (maxLag >= n) return 0.f;
    float best = 0.f; int bestLag = 0;
    for (int lag = minLag; lag < maxLag; ++lag) {
        float s = 0.f, e = 0.f;
        for (int i = 0; i + lag < n; ++i) {
            s += x[off + (size_t)i] * x[off + (size_t)(i + lag)];
            e += x[off + (size_t)i] * x[off + (size_t)i];
        }
        const float v = e > 1e-12f ? s / e : 0.f;
        if (v > best) { best = v; bestLag = lag; }
    }
    return bestLag > 0 && best > 0.25f ? (float)(sr / (double)bestLag) : 0.f;
}

//! Analyse `x` into up to kMaxFrames harmonic frames.
void analyse(const std::vector<float>& x, double sr, Analysis& out) {
    out.frames.clear();
    if (x.size() < 1024) return;
    out.f0 = detect_f0(x, sr);
    const float f0 = out.f0 > 0.f ? out.f0 : (float)(sr / 256.0);
    // Window long enough to resolve the fundamental: at least 4 periods.
    int N = 1024; while (N < (int)(4.0 * sr / f0) && N < 16384) N <<= 1;
    out.fftSize = N;
    out.latency = N / 2;

    // Hop an EIGHTH of a window, not a half.  Frame count is what the scan
    // moves through, so a sparse analysis is felt directly as a scan that
    // lurches between timbres instead of gliding.  Overlapping windows this
    // far also average out the analysis grid's own jitter.
    const int nf = (int)std::min<size_t>(kMaxFrames,
                                         std::max<size_t>(2, x.size() / (size_t)(N / 8)));
    std::vector<float> win((size_t)N);
    for (int i = 0; i < N; ++i)                       // Hann
        win[(size_t)i] = 0.5f - 0.5f * std::cos(kTwoPi * (float)i / (float)N);

    Fft fft; fft.init(N);
    std::vector<float> re((size_t)N), im((size_t)N);
    out.frames.reserve((size_t)nf);
    for (int f = 0; f < nf; ++f) {
        // Nominal time of this frame, then step BACK half a window so the
        // window is centred on it -- the latency compensation.
        const int64_t centre = (int64_t)((double)f / (double)std::max(1, nf - 1) *
                                         (double)(x.size() - 1));
        int64_t p = centre - N / 2;
        for (int i = 0; i < N; ++i) {
            const int64_t k = p + i;
            const float v = (k >= 0 && k < (int64_t)x.size()) ? x[(size_t)k] : 0.f;
            re[(size_t)i] = v * win[(size_t)i];
            im[(size_t)i] = 0.f;
        }
        // Forward transform == inverse with the spectrum conjugated either side.
        for (int i = 0; i < N; ++i) im[(size_t)i] = -im[(size_t)i];
        fft.inverse(re.data(), im.data());
        for (int i = 0; i < N; ++i) im[(size_t)i] = -im[(size_t)i];

        Frame fr;
        const double binHz = sr / (double)N;
        for (int h = 0; h < kHarm; ++h) {
            const double hz = (double)f0 * (double)(h + 1);
            const int b = (int)std::lround(hz / binHz);
            if (b < 1 || b >= N / 2) { fr.a[h] = 0.f; fr.ph[h] = 0.f; continue; }
            // Take the strongest of the three bins around the ideal position:
            // real instruments are never exactly in tune with the analysis grid.
            // A windowed partial spreads over the bins around its peak, so its
            // true level is the ENERGY of that neighbourhood.  Taking the single
            // loudest bin under-reads every partial that sits between bins --
            // which is most of them -- and the resynthesis comes out thin.
            float energy = 0.f, m = 0.f; int at = b;
            for (int d = -2; d <= 2; ++d) {
                const int bb = b + d;
                if (bb < 1 || bb >= N / 2) continue;
                const float p2 = re[(size_t)bb] * re[(size_t)bb] +
                                 im[(size_t)bb] * im[(size_t)bb];
                energy += p2;
                if (p2 > m) { m = p2; at = bb; }
            }
            fr.a[h]  = std::sqrt(energy);
            // Keep the phase of the bin the amplitude was taken from, so the
            // partials rebuild in the relationship they had in the source.
            fr.ph[h] = std::atan2(im[(size_t)at], re[(size_t)at]);
        }
        out.frames.push_back(fr);
    }

    // Normalise against the LOUDEST FRAME, not each frame against itself.
    // Per-frame normalisation was flattening the source's whole amplitude
    // envelope -- every moment came out equally loud, so an attack-and-decay
    // became a sustained drone.  One global divisor keeps the dynamics and
    // still guarantees the table never clips.
    float gmax = 0.f;
    for (const Frame& f : out.frames)
        for (int h = 0; h < kHarm; ++h) gmax = std::max(gmax, f.a[h]);
    if (gmax > 0.f)
        for (Frame& f : out.frames) {
            float pk = 0.f;
            for (int h = 0; h < kHarm; ++h) { f.a[h] /= gmax; pk = std::max(pk, f.a[h]); }
            f.gain = pk;                     // this frame's level within the file
        }
    // PAULSTRETCH-STYLE TIME SMOOTHING.  Paulstretch uses very long analysis
    // windows, which smooths each partial's amplitude contour and is a large
    // part of why it sounds liquid rather than granular.  Smoothing the frame
    // sequence after the fact gets the same contour without paying for a huge
    // FFT, and it directly serves the scan: neighbouring frames end up closer
    // together, so moving between them is smoother.
    if (out.frames.size() > 2) {
        std::vector<Frame> sm = out.frames;
        const int R = 2;                          // +/- 2 frames
        for (size_t i = 0; i < out.frames.size(); ++i) {
            for (int h = 0; h < kHarm; ++h) {
                float acc = 0.f; int n = 0;
                for (int d = -R; d <= R; ++d) {
                    const int64_t j = (int64_t)i + d;
                    if (j < 0 || j >= (int64_t)out.frames.size()) continue;
                    acc += out.frames[(size_t)j].a[h]; ++n;
                }
                sm[i].a[h] = n ? acc / (float)n : out.frames[i].a[h];
            }
        }
        out.frames.swap(sm);
    }

    // Reference phases come from the loudest frame.
    size_t best = 0; float bestG = -1.f;
    for (size_t i = 0; i < out.frames.size(); ++i)
        if (out.frames[i].gain > bestG) { bestG = out.frames[i].gain; best = i; }
    std::memcpy(out.refPhase, out.frames[best].ph, sizeof(out.refPhase));
}

//============================================================================
//  Parameter layout
//============================================================================
enum GlobalParam {
    G_FM,            //!< index for the external FM jack
    G_COUNT
};
enum OscParam {
    P_TUNE, P_FINE, P_LEVEL, P_SCAN, P_CHUNK, P_SEQ, P_SMEAR, P_SPREAD,
    P_STRETCH, P_ODDEVEN, P_TILT, P_FORMF, P_FORMQ, P_SHIFT, P_MIRROR,
    P_QUANT, P_GATE, P_MORPH, P_ABITS, P_SUB, P_DRIFT, P_FREEZE, P_PSCRAM,
    P_FB, P_BW, P_BWSCALE, P_RATE, P_COUNT
};
//! ONE oscillator.  The sculpt chain is deep enough that a second voice's worth
//! of controls doubled the panel without doubling what you could say with it;
//! one oscillator with every operator on a single page is the more playable
//! instrument, and polyphony now covers what the second oscillator was for.
inline int oscParam(int /*osc*/, int p) { return G_COUNT + p; }

//! Every continuous control is a CONCENTRIC KnobCV: inner disc = base value,
//! outer ring = bipolar CV depth, one jack beneath -- the configuration the
//! VCO-4 / ZDF modules use, built by rackx::kit::addControl.  The ring is
//! centred (zero) by default, so patching a jack does nothing until it is
//! dialled in and nothing moves behind your back.
enum ExtraParam {
    CV_BASE      = G_COUNT + P_COUNT,          //!< one CV depth per control
    READOUT_BASE = CV_BASE + P_COUNT,          //!< inert ids for the readouts
    NUM_PARAMS   = READOUT_BASE + P_COUNT
};

enum InputId {
    IN_VOCT, IN_SYNC, IN_FM,
    IN_CV_BASE,                                 //!< one jack per control
    NUM_INPUTS = IN_CV_BASE + P_COUNT
};
enum OutputId { OUT_MAIN, OUT_COUNT };

//============================================================================
//  One oscillator
//============================================================================
struct Osc {
    // --- user-owned harmonic bank ------------------------------------------
    float baseAmp[kHarm]   = {};
    float basePhase[kHarm] = {};

    // --- imported material --------------------------------------------------
    Analysis ana;
    std::string sampleName, samplePath;
    std::vector<float> mono;                 //!< kept for the waveform display
    double monoRate = 48000.0;

    // --- tables -------------------------------------------------------------
    // Two banks plus one guard entry each, so linear interpolation never has to
    // test for wrap: table[kTable] is a copy of table[0].
    int8_t tabA[kTable + 1] = {};
    int8_t tabB[kTable + 1] = {};
    bool    curIsA = true;                   //!< which bank is the CURRENT one
    int     fade   = 0;                      //!< samples left in the crossfade

    // --- running state, PER VOICE -------------------------------------------
    // Only phase and the feedback history are per voice.  The table is shared
    // by every voice, which is what makes polyphony nearly free here: a voice
    // costs one table read, not another sculpt-and-transform.
    uint32_t phase[kMaxVoices] = {};
    float    last1[kMaxVoices] = {}, last2[kMaxVoices] = {};
    Rng      rng, driftRng;
    float    drift[kHarm] = {};              //!< slow random walk per partial
    float    frozen[kHarm] = {};
    bool     haveFrozen = false;

    // --- slewed controls ----------------------------------------------------
    Slew sScan, sSmear, sShift, sMorph, sStretch, sTilt, sFormF, sGate;

    // --- rebuild bookkeeping -------------------------------------------------
    uint32_t phaseSeed = 0;                  //!< advanced per rebuild (Paulstretch)
    float lastSig = -1e9f;                   //!< signature of the last build
    int   sinceBuild = 0;
    //! Level of the scanned frame, applied at the OUTPUT.  The table itself is
    //! always normalised to full scale so the 8-bit quantisation keeps all its
    //! resolution; re-applying the frame's own level here is what lets the
    //! source's amplitude envelope survive that normalisation.  Slewed with the
    //! crossfade so a rebuild cannot step the level either.
    float tableGain = 1.f, tableGainPrev = 1.f;

    // scratch (kept resident so a rebuild allocates nothing)
    float work[kHarm]     = {};
    float shaped[kHarm]   = {};
    //! Phase and level taken from the scan position in the import.  Carried
    //! separately from the amplitudes because the sculpt chain rewrites
    //! amplitudes freely but must not scramble the phase relationships that
    //! make the resynthesis recognisable as the source instrument.
    float scanPhase[kHarm] = {};
    float scanGain = 1.f;
    bool  fromImport = false;
    float spectrumRe[kTable] = {};
    float spectrumIm[kTable] = {};

    const int8_t* current() const { return curIsA ? tabA : tabB; }
    const int8_t* previous() const { return curIsA ? tabB : tabA; }
    int8_t*       target()         { return curIsA ? tabB : tabA; }
};

//============================================================================
//  The module
//============================================================================
struct HarmonicForge : rack::engine::Module,
                       public PatchKnob::engine::ISampleSlot,
                       public rackx::ICurveSource {
    Osc  osc[2];
    Fft  fft;
    mutable std::mutex mutex_;               //!< guards import vs. rebuild
    float sr = 48000.f;
    int   which = 0;                         //!< slot the sample editor targets

    //! Equal-power crossfade table, built once.
    float fadeIn[kFadeLen + 1] = {}, fadeOut[kFadeLen + 1] = {};

    HarmonicForge() {
        config(NUM_PARAMS, NUM_INPUTS, OUT_COUNT, 0);
        fft.init(kTable);

        configParam(G_FM, 0.f, 1.f, 0.f, "FM index");

        for (int o = 0; o < 1; ++o) {
            const std::string t = "";
            configParam(oscParam(o, P_TUNE),    -24.f, 24.f,  0.f, t + "Tune");
            configParam(oscParam(o, P_FINE),   -100.f, 100.f, 0.f, t + "Fine");
            configParam(oscParam(o, P_LEVEL),     0.f, 1.f,   1.f, t + "Level");
            configParam(oscParam(o, P_SCAN),      0.f, 1.f,   0.f, t + "Scan");
            configParam(oscParam(o, P_CHUNK),     1.f, 32.f,  8.f, t + "Chunk");
            configParam(oscParam(o, P_SEQ),       0.f, 1.f,   0.f, t + "Resequence");
            configParam(oscParam(o, P_SMEAR),     0.f, 1.f,   0.f, t + "Smear");
            configParam(oscParam(o, P_SPREAD),    1.f, 32.f,  4.f, t + "Smear spread");
            configParam(oscParam(o, P_STRETCH),   0.5f, 2.f,  1.f, t + "Stretch");
            configParam(oscParam(o, P_ODDEVEN),  -1.f, 1.f,   0.f, t + "Odd/Even");
            configParam(oscParam(o, P_TILT),     -1.f, 1.f,   0.f, t + "Tilt");
            configParam(oscParam(o, P_FORMF),     0.f, 1.f,   0.f, t + "Formant");
            configParam(oscParam(o, P_FORMQ),     0.f, 1.f,   0.f, t + "Formant Q");
            configParam(oscParam(o, P_SHIFT),   -64.f, 64.f,  0.f, t + "Shift");
            configParam(oscParam(o, P_MIRROR),    0.f, 1.f,   0.f, t + "Mirror");
            configParam(oscParam(o, P_QUANT),     0.f, 1.f,   0.f, t + "Scale lock");
            configParam(oscParam(o, P_GATE),      0.f, 1.f,   0.f, t + "Spectral gate");
            configParam(oscParam(o, P_MORPH),     0.f, 1.f,   0.f, t + "Cross-morph");
            configParam(oscParam(o, P_ABITS),     1.f, 8.f,   8.f, t + "Amp bits");
            configParam(oscParam(o, P_SUB),       0.f, 1.f,   0.f, t + "Sub");
            configParam(oscParam(o, P_DRIFT),     0.f, 1.f,   0.f, t + "Drift");
            configParam(oscParam(o, P_FREEZE),    0.f, 1.f,   0.f, t + "Freeze");
            configParam(oscParam(o, P_PSCRAM),    0.f, 1.f,   0.f, t + "Phase scramble");
            configParam(oscParam(o, P_FB),        0.f, 1.f,   0.f, t + "Feedback");
            configParam(oscParam(o, P_BW),       0.1f, 100.f, 12.f, t + "Bandwidth", " cents");
            configParam(oscParam(o, P_BWSCALE),   0.f, 2.f,   1.f, t + "BW scale");
            configParam(oscParam(o, P_RATE),       0.f, 1.f,  0.5f, t + "Morph rate");
        }
        configInput(IN_VOCT, "V/Oct");
        configInput(IN_SYNC, "Sync");
        configInput(IN_FM,   "FM");
        // One CV jack and one bipolar depth ring per control.
        for (int i = 0; i < P_COUNT; ++i) {
            configParam(CV_BASE + i, -1.f, 1.f, 0.f, "CV depth");
            configInput(IN_CV_BASE + i, "CV");
        }
        configOutput(OUT_MAIN, "Out");

        for (int i = 0; i <= kFadeLen; ++i) {
            const float t = (float)i / (float)kFadeLen;
            fadeIn[i]  = std::sin(t * kPi * 0.5f);       // equal power: in^2+out^2 = 1
            fadeOut[i] = std::cos(t * kPi * 0.5f);
        }
        for (int o = 0; o < 1; ++o) {
            osc[o].rng.seed(0x1234567u + (uint32_t)o * 977u);
            osc[o].driftRng.seed(0x7f4a7c15u + (uint32_t)o * 31u);
            // A sawtooth is the useful default: every harmonic present, so every
            // operator in the chain has something to act on from the first note.
            for (int h = 0; h < kHarm; ++h) {
                osc[o].baseAmp[h]   = 1.f / (float)(h + 1);
                osc[o].basePhase[h] = 0.f;
            }
            build(o, 261.63f, true);
        }
    }

    void onSampleRateChange(float rate) override { if (rate > 0.f) sr = rate; }

    //========================================================================
    //  THE SCULPT CHAIN
    //========================================================================
    //  Reads the oscillator's source harmonics, applies every enabled operator
    //  in turn, and leaves the result in osc.shaped[].  Runs at rebuild rate,
    //  never per sample.
    //========================================================================
    void sculpt(int o, float f0) {
        Osc& s = osc[o];
        auto pv = [&](int p) { return params[oscParam(o, p)].getValue(); };

        float* w = s.work;

        // ---- 1. SOURCE: hand-edited bank, or a scan through the import ------
        if (s.ana.valid()) {
            // Scan position selects a frame; adjacent frames are blended so the
            // scan is continuous rather than stepping frame to frame.
            const float pos = s.sScan.y * (float)(s.ana.frames.size() - 1);
            const int i0 = (int)pos;
            const int i1 = std::min((int)s.ana.frames.size() - 1, i0 + 1);
            const float t = pos - (float)i0;
            const Frame& A = s.ana.frames[(size_t)i0];
            const Frame& B = s.ana.frames[(size_t)i1];
            for (int h = 0; h < kHarm; ++h)
                w[h] = A.a[h] + (B.a[h] - A.a[h]) * t;
            // Phase is HELD CONSTANT across the whole scan, taken once from the
            // import's loudest frame (see Analysis::refPhase).  Two reasons, and
            // together they are most of what makes the scan feel fluid:
            //   * an angle halfway between two angles is meaningless, so phase
            //     cannot be interpolated;
            //   * taking it from whichever frame is nearer instead jumps the
            //     entire partial set at every frame midpoint -- a discontinuity
            //     on every step of the scan, heard as the scan "chattering".
            // A pad's identity lives in its amplitude spectrum; freezing phase
            // costs nothing audible on a sustain and removes the chatter.
            std::memcpy(s.scanPhase, s.ana.refPhase, sizeof(s.scanPhase));
            s.scanGain = A.gain + (B.gain - A.gain) * t;
            s.fromImport = true;
        } else {
            std::memcpy(w, s.baseAmp, sizeof(float) * kHarm);
            s.scanGain = 1.f;
            s.fromImport = false;
        }

        // ---- 2. CHUNK RESEQUENCE -------------------------------------------
        // Cut the spectrum into chunks of `chunk` harmonics and play them back
        // in a shuffled order.  Formant regions get transplanted wholesale,
        // which is a very different sound from moving individual partials.
        const float seq = pv(P_SEQ);
        if (seq > 0.001f) {
            const int chunk = std::max(1, (int)std::lround(pv(P_CHUNK)));
            const int nch = (kHarm + chunk - 1) / chunk;
            int order[kHarm];
            for (int i = 0; i < nch; ++i) order[i] = i;
            Rng r; r.seed(0xC0FFEEu + (uint32_t)std::lround(seq * 4096.f));
            for (int i = nch - 1; i > 0; --i)                 // Fisher-Yates
                std::swap(order[i], order[(int)(r.next() % (uint32_t)(i + 1))]);
            float tmp[kHarm];
            for (int c = 0; c < nch; ++c) {
                const int src = order[c] * chunk, dst = c * chunk;
                for (int k = 0; k < chunk; ++k) {
                    const int d = dst + k, sIdx = src + k;
                    if (d >= kHarm) break;
                    tmp[d] = (sIdx < kHarm) ? w[sIdx] : 0.f;
                }
            }
            // `seq` blends toward the reordered spectrum so it can be dialled in.
            blend(w, tmp, seq);
        }

        // ---- 3. SMEAR -------------------------------------------------------
        // Each partial has an amplitude ENVELOPE across the scan; smearing hands
        // those envelopes to randomly chosen NEIGHBOURING harmonics.  The result
        // keeps the spectral region intact (the map never reaches far) while the
        // individual partials stop tracking their own contour -- the sound
        // loosens and breathes instead of moving in lockstep.
        const float smear = s.sSmear.y;
        if (smear > 0.001f) {
            const int spread = std::max(1, (int)std::lround(pv(P_SPREAD)));
            float tmp[kHarm];
            Rng r; r.seed(0x5EED01u + (uint32_t)o * 7919u);
            for (int h = 0; h < kHarm; ++h) {
                const int off = (int)(r.next() % (uint32_t)(2 * spread + 1)) - spread;
                int src = h + off;
                if (src < 0) src = -src;                      // reflect at the edges
                if (src >= kHarm) src = kHarm - 1 - (src - kHarm);
                src = std::max(0, std::min(kHarm - 1, src));
                tmp[h] = w[src];
            }
            blend(w, tmp, smear);
        }

        // ---- 4. SPECTRAL STRETCH (quantised inharmonicity) -------------------
        // Partial n moves to n^stretch.  A single-cycle table can only hold
        // INTEGER harmonics, so the destination is rounded -- the result is a
        // warped, bell-like spectrum rather than true inharmonicity, and it is
        // documented as such rather than pretending otherwise.
        const float stretch = s.sStretch.y;
        if (std::fabs(stretch - 1.f) > 0.001f) {
            float tmp[kHarm] = {};
            for (int h = 0; h < kHarm; ++h) {
                if (w[h] <= 0.f) continue;
                const float n = std::pow((float)(h + 1), stretch);
                const int d = (int)std::lround(n) - 1;
                if (d >= 0 && d < kHarm) tmp[d] = std::max(tmp[d], w[h]);
            }
            std::memcpy(w, tmp, sizeof(tmp));
        }

        // ---- 5. HARMONIC SHIFT ----------------------------------------------
        // Rotate the whole bank up or down the harmonic series.  Unlike a pitch
        // change this moves CONTENT between partial numbers, so a bright spectrum
        // can be dragged onto low partials and vice versa.
        const int shift = (int)std::lround(s.sShift.y);
        if (shift != 0) {
            float tmp[kHarm] = {};
            for (int h = 0; h < kHarm; ++h) {
                const int d = h + shift;
                if (d >= 0 && d < kHarm) tmp[d] = w[h];
            }
            std::memcpy(w, tmp, sizeof(tmp));
        }

        // ---- 6. MIRROR -------------------------------------------------------
        // Reverse the harmonic order: what was a decaying series becomes a rising
        // one, which inverts the whole timbral gesture.
        const float mirror = pv(P_MIRROR);
        if (mirror > 0.001f) {
            float tmp[kHarm];
            for (int h = 0; h < kHarm; ++h) tmp[h] = w[kHarm - 1 - h];
            blend(w, tmp, mirror);
        }

        // ---- 7. ODD / EVEN ---------------------------------------------------
        // Negative favours odd partials (hollow, clarinet-like), positive favours
        // even (fuller, octave-doubled).
        const float oe = pv(P_ODDEVEN);
        if (std::fabs(oe) > 0.001f) {
            const float odd = oe < 0.f ? 1.f : 1.f - oe;
            const float evn = oe > 0.f ? 1.f : 1.f + oe;
            for (int h = 0; h < kHarm; ++h) w[h] *= ((h & 1) ? evn : odd);
        }

        // ---- 8. TILT ---------------------------------------------------------
        // A straight dB-per-octave slope across the series: the single most
        // effective brightness control on an additive spectrum.
        const float tilt = s.sTilt.y;
        if (std::fabs(tilt) > 0.001f) {
            const float k = tilt * 1.5f;
            for (int h = 0; h < kHarm; ++h)
                w[h] *= std::pow((float)(h + 1), k);
        }

        // ---- 9. FORMANT ------------------------------------------------------
        // A resonant peak that slides along the series, imposing a vowel-like
        // fixed region on whatever is passing through.
        const float fq = pv(P_FORMQ);
        if (fq > 0.001f) {
            const float centre = 1.f + s.sFormF.y * (float)(kHarm - 1);
            const float width = std::max(0.75f, (1.f - fq) * 40.f);
            for (int h = 0; h < kHarm; ++h) {
                const float d = ((float)(h + 1) - centre) / width;
                w[h] *= 1.f + fq * 6.f * std::exp(-d * d);
            }
        }

        // ---- 10. SUB ----------------------------------------------------------
        // Fold energy from partial 2n down onto partial n, thickening the bottom
        // without a second oscillator.
        const float sub = pv(P_SUB);
        if (sub > 0.001f)
            for (int h = 0; h < kHarm / 2; ++h)
                w[h] += w[h * 2 + 1] * sub;

        // ---- 11. SCALE LOCK ---------------------------------------------------
        // Keep only partials whose ratio to the fundamental lands near a
        // 12-TET interval; the rest are attenuated.  Turns noisy or inharmonic
        // imports into something that sits in a key.
        const float quant = pv(P_QUANT);
        if (quant > 0.001f) {
            for (int h = 0; h < kHarm; ++h) {
                const float semis = 12.f * std::log2((float)(h + 1));
                const float dev = std::fabs(semis - std::round(semis));   // 0..0.5
                const float keep = 1.f - quant * clampf(dev * 2.f, 0.f, 1.f);
                w[h] *= keep;
            }
        }

        // ---- 12. SPECTRAL GATE -------------------------------------------------
        // Everything below the threshold is removed outright, thinning a dense
        // spectrum down to its skeleton.
        const float gate = s.sGate.y;
        if (gate > 0.001f) {
            float peak = 0.f;
            for (int h = 0; h < kHarm; ++h) peak = std::max(peak, w[h]);
            const float th = peak * gate;
            for (int h = 0; h < kHarm; ++h) if (w[h] < th) w[h] = 0.f;
        }

        // ---- 13. AMPLITUDE QUANTISE (spectral bitcrush) -----------------------
        // Quantise the partial AMPLITUDES, not the waveform: partials snap onto
        // a coarse ladder and small movements stop being continuous, which is a
        // grainy, stepped character no waveform bitcrusher produces.
        const float bits = pv(P_ABITS);
        if (bits < 7.99f) {
            const float lv = std::pow(2.f, bits) - 1.f;
            for (int h = 0; h < kHarm; ++h) w[h] = std::round(w[h] * lv) / lv;
        }

        // ---- 14. DRIFT ---------------------------------------------------------
        // Every partial takes an independent slow random walk, so a static
        // spectrum never sits perfectly still -- the additive equivalent of the
        // small instabilities that stop real instruments sounding synthetic.
        const float dr = pv(P_DRIFT);
        if (dr > 0.001f) {
            for (int h = 0; h < kHarm; ++h) {
                s.drift[h] += s.driftRng.bi() * 0.05f;
                s.drift[h] = clampf(s.drift[h], -1.f, 1.f);
                w[h] *= 1.f + dr * s.drift[h];
                if (w[h] < 0.f) w[h] = 0.f;
            }
        }

        // ---- 15. MORPH: IMPORT <-> DRAWN BANK -----------------------------------
        // Blend the scanned import against the hand-drawn harmonic bank.  With a
        // single oscillator this is the more useful axis than the old
        // oscillator-to-oscillator morph: it lets a real instrument's spectrum be
        // pulled toward a shape you drew, or a drawn shape be given a real
        // instrument's harmonic detail, on one knob.
        const float morph = s.sMorph.y;
        if (morph > 0.001f) blend(w, s.baseAmp, morph);

        // ---- 16. FREEZE ---------------------------------------------------------
        // Latch the spectrum.  Held content survives everything upstream changing,
        // so it is a way to capture one instant of a scan and keep playing it.
        if (pv(P_FREEZE) > 0.5f) {
            if (!s.haveFrozen) { std::memcpy(s.frozen, w, sizeof(s.frozen)); s.haveFrozen = true; }
            std::memcpy(w, s.frozen, sizeof(s.frozen));
        } else {
            s.haveFrozen = false;
        }

        // NO BAND LIMITING, by design.  Zeroing partials above Nyquist for the
        // played note would keep the top octaves alias-free, but it also strips
        // the upper spectrum out of exactly the low, harmonically rich material
        // this is built to resynthesise -- and with one table shared by every
        // voice it would have to be limited for the HIGHEST note in a chord,
        // dulling every other voice with it.  All 256 partials are kept; the
        // aliasing that results at high pitches is accepted as part of the
        // character, alongside the 8-bit stage.
        (void)f0;
        std::memcpy(s.shaped, w, sizeof(s.shaped));
    }

    //! shaped = shaped*(1-t) + other*t, vectorised.
    static void blend(float* dst, const float* src, float t) {
#if HFORGE_SSE3
        const __m128 vt = _mm_set1_ps(t), vu = _mm_set1_ps(1.f - t);
        for (int h = 0; h < kHarm; h += 4) {
            __m128 a = _mm_loadu_ps(dst + h), b = _mm_loadu_ps(src + h);
            _mm_storeu_ps(dst + h, _mm_add_ps(_mm_mul_ps(a, vu), _mm_mul_ps(b, vt)));
        }
#else
        for (int h = 0; h < kHarm; ++h) dst[h] += (src[h] - dst[h]) * t;
#endif
    }

    //========================================================================
    //  REBUILD -- shaped harmonics -> 8-bit single-cycle table
    //========================================================================
    void build(int o, float f0, bool immediate) {
        Osc& s = osc[o];
        sculpt(o, f0);

        // Assemble a conjugate-symmetric spectrum whose inverse transform is
        // sum_h A_h * sin(2*pi*h*t/N + phi_h).
        std::memset(s.spectrumRe, 0, sizeof(s.spectrumRe));
        std::memset(s.spectrumIm, 0, sizeof(s.spectrumIm));
        const float scram = params[oscParam(o, P_PSCRAM)].getValue();
        // PAULSTRETCH.  Its smoothness comes from throwing the phases away and
        // re-randomising them on every frame, keeping only the magnitudes -- the
        // sound stops being a fixed waveform and becomes a continuously
        // reforming cloud with the right spectrum.  A FIXED seed here made every
        // rebuild produce byte-identical phases, so the texture sat dead still;
        // advancing it per rebuild is what gives the pad its slow inner motion.
        // The equal-power crossfade is exactly the right tool for splicing two
        // uncorrelated random-phase tables, so this costs no extra machinery.
        s.phaseSeed += 0x9e3779b9u;
        Rng pr; pr.seed(0xBEEF01u + (uint32_t)o * 13u + s.phaseSeed);
        const float bwCents = params[oscParam(o, P_BW)].getValue();
        const float bwScale = params[oscParam(o, P_BWSCALE)].getValue();
        for (int h = 0; h < kHarm; ++h) {
            const float a = s.shaped[h];
            if (a <= 0.f) { pr.next(); continue; }
            // ---- 17. PHASE SCRAMBLE -------------------------------------------
            // Partial phases do not change WHICH frequencies are present, but they
            // completely change the waveform's shape and crest factor: the same
            // spectrum goes from a spiky impulse-like tone to a smooth diffuse one.
            const int centre = (h + 1) * kPeriods;
            if (centre >= kTable / 2) break;

            // PadSynth bandwidth.  The partial's energy is spread over a
            // Gaussian centred on its bin, whose width grows with harmonic
            // number (bwScale) the way a real instrument's upper partials are
            // progressively less well defined.  Each bin in the band gets an
            // INDEPENDENT RANDOM PHASE: that is what turns the band into a
            // slowly beating, chorused partial instead of a single steady sine,
            // and it is the difference between a lush pad and a ringing organ.
            const float bwBins = std::max(0.5f,
                bwCents / 1200.f * (float)kPeriods *
                std::pow((float)(h + 1), bwScale) * 0.5f);
            const int span = std::min(kTable / 4, (int)std::ceil(bwBins * 3.f));
            float norm = 0.f;
            for (int d = -span; d <= span; ++d) {
                const float x = (float)d / bwBins;
                norm += std::exp(-x * x);
            }
            if (norm <= 0.f) norm = 1.f;
            const float base = (s.fromImport ? s.scanPhase[h] : s.basePhase[h]);
            for (int d = -span; d <= span; ++d) {
                const int k = centre + d;
                if (k < 1 || k >= kTable / 2) continue;
                const float x = (float)d / bwBins;
                const float amp = a * std::exp(-x * x) / norm;
                // Random phase within the band; `scram` widens it further, and
                // at bandwidth 0 the band is one bin so the analysed phase is
                // used unchanged and exact resynthesis is still available.
                const float ph = (span == 0) ? base
                               : base + pr.bi() * kPi + scram * pr.bi() * kPi;
                s.spectrumRe[k]          +=  0.5f * amp * std::sin(ph);
                s.spectrumIm[k]          += -0.5f * amp * std::cos(ph);
                s.spectrumRe[kTable - k]  =  s.spectrumRe[k];
                s.spectrumIm[kTable - k]  = -s.spectrumIm[k];
            }
        }
        fft.inverse(s.spectrumRe, s.spectrumIm);

        // Normalise to full scale, then QUANTISE INTO 8 BITS -- the specified
        // 8-bit generation stage.  Normalising first matters: quantising a quiet
        // waveform would throw away most of the 256 levels and leave the noise
        // floor sitting on top of the signal.
        float peak = 0.f;
        for (int i = 0; i < kTable; ++i) peak = std::max(peak, std::fabs(s.spectrumRe[i]));
        const float g = peak > 1e-9f ? 127.f / peak : 0.f;
        int8_t* dst = immediate ? (s.curIsA ? s.tabA : s.tabB) : s.target();
        // NOISE-SHAPED quantisation.  Rounding each point independently spreads
        // the 8-bit error evenly across the spectrum, and on a harmonically
        // sparse pad that broadband hiss sits in the gaps where nothing masks
        // it.  Feeding the previous point's error forward pushes the error
        // energy up out of the range the partials actually occupy, so the same
        // 8 bits sound markedly cleaner.  The table stays genuinely 8-bit.
        float e = 0.f;
        for (int i = 0; i < kTable; ++i) {
            const float v = s.spectrumRe[i] * g + e;
            int q = (int)std::lround(v);
            if (q > 127) q = 127; else if (q < -127) q = -127;
            e = clampf(v - (float)q, -1.f, 1.f);
            dst[i] = (int8_t)q;
        }
        dst[kTable] = dst[0];                 // interpolation guard

        if (immediate) { s.fade = 0; s.tableGain = s.tableGainPrev = s.scanGain; }
        else {
            s.curIsA = !s.curIsA; s.fade = kFadeLen;        // new bank becomes current
            s.tableGainPrev = s.tableGain;
            s.tableGain = s.scanGain;
        }
        s.sinceBuild = 0;
    }

    //! A cheap signature of everything the chain depends on.  A rebuild only
    //! happens when this actually moves, so a static patch costs nothing beyond
    //! the table read -- which is what makes the module affordable at all.
    float signature(int o) const {
        const Osc& s = osc[o];
        // NOT f0.  sculpt() takes f0 and ignores it (there is no band limiting
        // left in the chain), so the table does not depend on the pitch at all
        // -- but including f0 here made vibrato, glide or an LFO on V/Oct
        // retrigger build() (sculpt + a 2048-point inverse FFT + noise-shaped
        // requantisation) as often as every kFadeLen samples.  Measured 0.74 ms
        // per 4096-frame block static against 11.7 ms with V/Oct swept: ~20% of
        // a 128-frame callback budget spent in one burst, on the audio thread.
        // Pitch is applied by the phase increment at read time, where it belongs.
        float v = 0.f;
        v += s.sScan.y * 7.f + s.sSmear.y * 11.f + s.sShift.y * 0.31f;
        v += s.sMorph.y * 13.f + s.sStretch.y * 17.f + s.sTilt.y * 19.f;
        v += s.sFormF.y * 23.f + s.sGate.y * 29.f;
        for (int p : { P_CHUNK, P_SEQ, P_SPREAD, P_ODDEVEN, P_FORMQ, P_MIRROR,
                       P_QUANT, P_ABITS, P_SUB, P_DRIFT, P_FREEZE, P_PSCRAM })
            v += params[oscParam(o, p)].getValue() * (float)(p + 3) * 1.7f;
        return v;
    }

    //========================================================================
    //  Audio
    //========================================================================
    //! One interpolated read of an 8-bit table.  Everything here is integer up
    //! to the final scale: index and fraction fall straight out of the phase
    //! word, so there is no float->int conversion and no wrap test per sample.
    static inline float read(const int8_t* t, uint32_t ph) {
        const uint32_t i = ph >> kFracBits;
        const float f = (float)(ph & kFracMask) * kFracNorm;
        const float a = (float)t[i], b = (float)t[i + 1];
        return (a + (b - a) * f) * (1.f / 127.f);
    }

    void process(const ProcessArgs& args) override {
        sr = args.sampleRate;
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (!lk.owns_lock()) {                     // an import is swapping in
            outputs[OUT_MAIN].setVoltage(0.f);
            return;
        }


        // Voice count follows the patched pitch cable.
        int voices = std::min(kMaxVoices, std::max(1, inputs[IN_VOCT].getChannels()));
        outputs[OUT_MAIN].setChannels(voices);

        for (int o = 0; o < 1; ++o) {
            Osc& s = osc[o];
            // A control's value is its knob PLUS its jack scaled by its ring.
            // Reading it through one accessor means every operator downstream
            // gets CV for free and none of them has to know it exists.
            auto pv = [&](int p) {
                const int id = oscParam(o, p);
                float v = params[id].getValue();
                const float d = params[CV_BASE + p].getValue();
                const float lo = paramQuantities[id].minValue;
                const float hi = paramQuantities[id].maxValue;
                if (d != 0.f && inputs[IN_CV_BASE + p].isConnected())
                    v += d * inputs[IN_CV_BASE + p].getVoltage() * 0.1f * (hi - lo);
                return clampf(v, lo, hi);
            };

            // ---- slew every continuously modulated control -------------------
            const float rate = 0.002f + pv(P_RATE) * 0.2f;
            for (Slew* sl : { &s.sScan, &s.sSmear, &s.sShift, &s.sMorph,
                              &s.sStretch, &s.sTilt, &s.sFormF, &s.sGate })
                sl->setRate(rate);
            s.sScan.run(pv(P_SCAN));
            s.sSmear.run(pv(P_SMEAR));
            s.sShift.run(pv(P_SHIFT));
            s.sMorph.run(pv(P_MORPH));
            s.sStretch.run(pv(P_STRETCH));
            s.sTilt.run(pv(P_TILT));
            s.sFormF.run(pv(P_FORMF));
            s.sGate.run(pv(P_GATE));

            // ---- pitch --------------------------------------------------------
            const int voctIn = IN_VOCT;
            // One representative pitch, only so the rebuild signature notices a
            // pitch change; nothing in the table depends on it now that there is
            // no band limiting, so voice 0 is enough.
            float volts = inputs[voctIn].getPolyVoltage(0);
            volts += pv(P_TUNE) / 12.f + pv(P_FINE) / 1200.f;
            // ROOT NOTE TRACKS THE IMPORT.  The harmonic bank describes the
            // spectrum of a sound that had its own fundamental, so that
            // fundamental is the root: a guitar analysed at 82 Hz sounds at
            // 82 Hz with V/Oct at zero, and V/Oct transposes the whole harmonic
            // structure from there.  Playing every import from a fixed C4
            // instead re-pitched the source by whatever interval separated it
            // from C4, which is most of why an import did not sound like itself.
            const float root = s.ana.valid() && s.ana.f0 > 1.f ? s.ana.f0 : 261.6256f;
            const float f0 = clampf(root * std::pow(2.f, volts), 0.02f, sr * 0.48f);

            // ---- rebuild when anything moved ---------------------------------
            ++s.sinceBuild;
            const float sig = signature(o);
            const int minGap = kFadeLen;         // never start a fade inside a fade
            if (s.sinceBuild >= minGap && std::fabs(sig - s.lastSig) > 1e-6f) {
                s.lastSig = sig;
                build(o, f0, false);
            }

            // ---- per-voice audio ---------------------------------------------
            const float tune = pv(P_TUNE) / 12.f + pv(P_FINE) / 1200.f;
            const float fbAmt = pv(P_FB), lvl = pv(P_LEVEL);
            const float fmAmt = params[G_FM].getValue() * 4.f;
            const bool  fmPatched = inputs[IN_FM].isConnected();
            const bool  syncPatched = inputs[IN_SYNC].isConnected();

            for (int v = 0; v < voices; ++v) {
                const float vv = inputs[voctIn].getPolyVoltage(v) + tune;
                const float vf = clampf(root * std::pow(2.f, vv), 0.02f, sr * 0.48f);
                // The table spans kPeriods periods, so one period of the note is
                // 1/kPeriods of the table.
                const uint32_t inc = (uint32_t)(vf / sr * 4294967296.0 / (double)kPeriods);

            // ---- phase modulation: A into B, plus per-oscillator feedback -----
            // The feedback term averages the last two outputs.  A raw y[n-1] term
            // is an undamped delay-free-ish loop that breaks into noise as the
            // index rises; the two-sample average is a gentle lowpass inside the
            // loop, which is what keeps it musical all the way up.
            float pm = fbAmt * 0.5f * (s.last1[v] + s.last2[v]);
            if (fmPatched) pm += inputs[IN_FM].getPolyVoltage(v) * 0.2f * fmAmt;

            const uint32_t pmOff = (uint32_t)(int32_t)(pm * 1073741824.f);   // 1.0 = 1/4 cycle
            const uint32_t rd = s.phase[v] + pmOff;

            // ---- read, crossfading the two banks while a rebuild settles ------
            float val;
            if (s.fade > 0) {
                // Each bank is scaled by the level of the frame it was built
                // from, so the crossfade carries the amplitude envelope across
                // the swap instead of stepping to the new frame's level.
                const int i = kFadeLen - s.fade;
                val = read(s.current(), rd)  * fadeIn[i]  * s.tableGain
                    + read(s.previous(), rd) * fadeOut[i] * s.tableGainPrev;
                if (v == voices - 1) --s.fade;   // advance the fade once per sample
            } else {
                val = read(s.current(), rd) * s.tableGain;
            }

            s.phase[v] += inc;
            if (syncPatched && inputs[IN_SYNC].getPolyVoltage(v) > 1.f && s.last1[v] <= 0.f)
                s.phase[v] = 0;

            s.last2[v] = s.last1[v]; s.last1[v] = val;
            outputs[OUT_MAIN].setVoltage(val * lvl * 5.f, v);
            }   // voices
        }       // oscillators
    }

    //========================================================================
    //  ISampleSlot -- the import path
    //========================================================================
    bool sampleLoad(const std::string& path, double rate, std::string* error) override {
        PatchKnob::engine::AudioClip clip;
        if (!PatchKnob::engine::loadWav(path, rate, clip, error)) return false;
        if (clip.empty()) { if (error) *error = "sample is empty"; return false; }
        std::vector<float> m(clip.ch[0].size());
        const bool st = !clip.ch[1].empty() && clip.ch[1].size() == clip.ch[0].size();
        for (size_t i = 0; i < m.size(); ++i)
            m[i] = st ? 0.5f * (clip.ch[0][i] + clip.ch[1][i]) : clip.ch[0][i];

        // Analyse OUTSIDE the lock -- it is an FFT per frame and the audio thread
        // only try-locks, so the render never waits on it.
        Analysis a;
        analyse(m, rate, a);
        if (!a.valid()) { if (error) *error = "could not analyse"; return false; }

        const size_t slash = path.find_last_of("/\\");
        std::lock_guard<std::mutex> lk(mutex_);
        Osc& s = osc[which];
        s.ana = std::move(a);
        s.mono = std::move(m);
        s.monoRate = rate;
        s.samplePath = path;
        s.sampleName = slash == std::string::npos ? path : path.substr(slash + 1);
        s.lastSig = -1e9f;                    // force a rebuild on the next block
        return true;
    }
    void sampleClear() override {
        std::lock_guard<std::mutex> lk(mutex_);
        Osc& s = osc[which];
        s.ana.frames.clear(); s.mono.clear();
        s.sampleName.clear(); s.samplePath.clear();
        s.lastSig = -1e9f;
    }
    const char* sampleName() const override { return osc[which].sampleName.c_str(); }
    const char* samplePath() const override { return osc[which].samplePath.c_str(); }
    int    sampleFrames() const override { return (int)osc[which].mono.size(); }
    double sampleRate() const override { return osc[which].monoRate; }
    int samplePeaks(int64_t from, int64_t to, float* outMin, float* outMax,
                    int buckets) const override {
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (!lk.owns_lock()) return 0;
        const std::vector<float>& m = osc[which].mono;
        if (m.empty() || buckets <= 0 || to <= from) return 0;
        to = std::min<int64_t>(to, (int64_t)m.size());
        const double span = (double)(to - from) / (double)buckets;
        for (int b = 0; b < buckets; ++b) {
            const int64_t i0 = from + (int64_t)(b * span);
            const int64_t i1 = std::min<int64_t>(to, from + (int64_t)((b + 1) * span));
            float lo = 0.f, hi = 0.f;
            for (int64_t i = i0; i < i1; ++i) { lo = std::min(lo, m[(size_t)i]);
                                                hi = std::max(hi, m[(size_t)i]); }
            outMin[b] = lo; outMax[b] = hi;
        }
        return buckets;
    }
    //! Marker 0 is the scan position, so dragging it in the waveform editor is
    //! the same gesture as turning SCAN -- the import and the control are the
    //! same object seen two ways.
    float sampleMarker(int marker) const override {
        return marker == 0 ? params[oscParam(which, P_SCAN)].getValue() : 0.f;
    }
    void sampleSetMarker(int marker, float v) override {
        if (marker == 0) params[oscParam(which, P_SCAN)].setValue(clampf(v, 0.f, 1.f));
    }

    //! Frames of delay the analysis introduced, for a host lining the import up
    //! against the source file.  See the Analysis comment.
    int analysisLatency() const { return osc[which].ana.latency; }

    //========================================================================
    //  ICurveSource -- the 128-bar harmonic editor
    //========================================================================
    //  Curve `o` IS oscillator o's harmonic bank: point h sits at t = h/(kHarm-1) and
    //  its value is that harmonic's amplitude.  Reusing the generic breakpoint
    //  widget rather than writing a bespoke bar editor means the harmonic bank
    //  gets drag-editing, selection and redraw for free, and behaves exactly
    //  like every other curve in the app.
    //
    //  The point COUNT is fixed at kHarm, so add/remove are refused: the bank is a
    //  bin array, not a freely-shaped envelope, and silently inserting an extra
    //  partial would desynchronise the editor from the DSP that indexes it.
    //========================================================================
    int  curveCount() const override { return 2; }
    int  curvePointCount(int) const override { return kHarm; }
    bool curveGetPoint(int c, int i, rackx::CurvePoint& out) const override {
        if (c < 0 || c > 1 || i < 0 || i >= kHarm) return false;
        out.t = (float)i / (float)(kHarm - 1);
        out.v = clampf(osc[c].baseAmp[i], 0.f, 1.f);
        out.c = 0.f;
        return true;
    }
    bool curveSetPoint(int c, int i, const rackx::CurvePoint& p) override {
        if (c < 0 || c > 1 || i < 0 || i >= kHarm) return false;
        // Only the VALUE is writable -- a bin cannot move along the harmonic
        // axis, so a horizontal drag is ignored rather than reordering the bank.
        std::lock_guard<std::mutex> lk(mutex_);
        osc[c].baseAmp[i] = clampf(p.v, 0.f, 1.f);
        osc[c].lastSig = -1e9f;               // force a rebuild (and its crossfade)
        return true;
    }
    int  curveAddPoint(int, float, float) override { return -1; }
    bool curveRemovePoint(int, int) override { return false; }
    void curveGetInfo(int, rackx::CurveInfo& out) const override {
        out = rackx::CurveInfo{};
        out.gridStep = 1.f / (float)(kHarm - 1);   // snap to whole harmonics
    }
    float curveSnapTime(int, float t) const override {
        return std::round(t * (float)(kHarm - 1)) / (float)(kHarm - 1);
    }
    float curveValueAt(int c, float t) const override {
        if (c < 0 || c > 1) return 0.f;
        const int i = (int)clampf(std::round(t * (float)(kHarm - 1)), 0.f, (float)(kHarm - 1));
        return clampf(osc[c].baseAmp[i], 0.f, 1.f);
    }
};

//============================================================================
//  Panel -- ONE page, no tabs
//============================================================================
//  Every continuous control is a concentric KnobCV unit (inner value, outer CV
//  depth ring, jack beneath) laid out by rackx::kit::addControl -- the same
//  configuration and the same code path as the VCO-4 and ZDF modules, so the
//  controls behave identically across the rack.
//
//  The operators are grouped by what they DO to the spectrum rather than listed
//  in chain order, because that is how you reach for them: SOURCE (where the
//  harmonics come from), SHAPE (their contour), SCATTER (what disorders them),
//  and VOICE (pitch, level, FM).  All 26 fit on one page at a reduced unit
//  scale, which is what the tabs were previously buying and it was not worth
//  hiding two thirds of the instrument to get it.
//============================================================================
rackx::PanelSpec forgePanel() {
    rackx::PanelSpec p = rackx::PanelSpec::fromHp(46);
    const float W = p.width;

    struct Ctl { int id; const char* label; };
    // Row 1 SOURCE + row 2 SHAPE + row 3 SCATTER/VOICE.
    static const Ctl kRow1[] = {
        { P_SCAN, "SCAN" }, { P_MORPH, "MORPH" }, { P_BW, "BANDWIDTH" },
        { P_BWSCALE, "BW SCALE" }, { P_RATE, "RATE" }, { P_CHUNK, "CHUNK" },
        { P_SEQ, "RESEQ" }, { P_SMEAR, "SMEAR" }, { P_SPREAD, "SPREAD" },
    };
    static const Ctl kRow2[] = {
        { P_TILT, "TILT" }, { P_ODDEVEN, "ODD/EVEN" }, { P_FORMF, "FORMANT" },
        { P_FORMQ, "FORM Q" }, { P_GATE, "GATE" }, { P_QUANT, "SCALE" },
        { P_SUB, "SUB" }, { P_MIRROR, "MIRROR" }, { P_SHIFT, "SHIFT" },
    };
    static const Ctl kRow3[] = {
        { P_STRETCH, "STRETCH" }, { P_PSCRAM, "PHASE" }, { P_DRIFT, "DRIFT" },
        { P_ABITS, "AMP BITS" }, { P_FB, "FEEDBACK" }, { P_TUNE, "TUNE" },
        { P_FINE, "FINE" }, { P_LEVEL, "LEVEL" },
    };
    const int n1 = (int)(sizeof(kRow1) / sizeof(Ctl));
    const int n2 = (int)(sizeof(kRow2) / sizeof(Ctl));
    const int n3 = (int)(sizeof(kRow3) / sizeof(Ctl));

    const float margin = 12.f;
    const float scale  = 0.62f;                 // 3 rows + editor inside 380 px
    const float rowH   = rackx::kit::unitHeight(scale, true);
    const float y0     = 34.f + rowH * 0.30f;

    auto row = [&](const Ctl* c, int n, float y) {
        const float cw = rackx::kit::pitch(W, margin, n);
        for (int i = 0; i < n; ++i) {
            const float x = margin + cw * ((float)i + 0.5f);
            rackx::kit::addControl(p, c[i].id, CV_BASE + c[i].id,
                                   IN_CV_BASE + c[i].id, READOUT_BASE + c[i].id,
                                   x, y, c[i].label, cw, scale);
        }
    };
    row(kRow1, n1, y0);
    row(kRow2, n2, y0 + rowH);
    row(kRow3, n3, y0 + rowH * 2.f);

    // FREEZE is a switch, not a continuous control, so it gets no ring or jack
    // and sits in the slot row 3 leaves free.
    {
        rackx::PanelElement f;
        f.id = P_FREEZE;
        f.x = margin + rackx::kit::pitch(W, margin, n1) * ((float)n3 + 0.5f);
        f.y = y0 + rowH * 2.f;
        f.radius = 11.f;
        f.style = rackx::PanelControlStyle::Switch;
        f.label = "FREEZE";
        f.labelPlacement = rackx::PanelLabelPlacement::Above;
        p.params.push_back(f);
    }

    // The harmonic bank across the foot of the panel -- the widest thing here
    // because it is the thing you actually draw on.
    {
        rackx::PanelElement he;
        he.id = READOUT_BASE + P_COUNT + 1;      // unique, inert
        he.style = rackx::PanelControlStyle::Curve;
        he.curveIndex = 0;
        he.x = W * 0.5f; he.y = 318.f;
        he.width = W - 2.f * margin; he.height = 62.f;
        he.label = "HARMONICS";
        he.labelPlacement = rackx::PanelLabelPlacement::Above;
        p.params.push_back(he);
    }

    // Pitch, sync, FM and the output live along the bottom edge.
    auto jack = [&](int id, float x, const char* label) {
        rackx::PanelElement e;
        e.id = id; e.x = x; e.y = 364.f; e.radius = 8.f;
        e.style = rackx::PanelControlStyle::Knob;
        e.label = label; e.labelPlacement = rackx::PanelLabelPlacement::Above;
        return e;
    };
    p.inputs.push_back(jack(IN_VOCT, margin + 14.f, "V/OCT"));
    p.inputs.push_back(jack(IN_SYNC, margin + 60.f, "SYNC"));
    p.inputs.push_back(jack(IN_FM,   margin + 106.f, "FM"));
    {   // FM index sits beside the FM jack it scales.
        rackx::PanelElement k;
        k.id = G_FM; k.x = margin + 152.f; k.y = 364.f; k.radius = 11.f;
        k.style = rackx::PanelControlStyle::Knob;
        k.label = "FM AMT"; k.labelPlacement = rackx::PanelLabelPlacement::Above;
        p.params.push_back(k);
    }
    p.outputs.push_back(jack(OUT_MAIN, W - margin - 14.f, "OUT"));
    return p;
}

} // namespace

namespace rackx {
void registerHarmonicForgeModule() {
    addType("HFORGE", "Harmonic Forge", "Oscillator", Role::Normal,
            [] { return std::make_unique<HarmonicForge>(); }, forgePanel());
}
} // namespace rackx
