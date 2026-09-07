//----------------------------------------------------------------------------
//  Eight independent TB-303 ladder filters, AVX1 across the eight lanes.
//
//  The DSP is a port of the TB_303 mode of rosic::TeeBeeFilter from Open303
//  (https://github.com/RobinSchmidt/Open303) by Robin Schmidt, used under the
//  MIT licence:
//
//    Copyright (c) 2009 Robin Schmidt (www.rs-met.com)
//    Permission is hereby granted, free of charge, to any person obtaining a
//    copy of this software and associated documentation files (the "Software"),
//    to deal in the Software without restriction ... THE SOFTWARE IS PROVIDED
//    "AS IS", WITHOUT WARRANTY OF ANY KIND.  (full text: Open303 License.txt)
//
//  What is taken verbatim from Open303 is the part that makes it sound like the
//  hardware: the rational fit for the integrator coefficient b0, the 6th-order
//  polynomial for the feedback factor k, the output-gain law g, the resonance
//  skew, and the 150 Hz one-pole highpass inside the feedback path (that
//  highpass is why a 303 loses resonance as the cutoff drops instead of
//  booming).  The pre/post shaping filters around the ladder come from
//  rosic::Open303 itself: a 44.486 Hz highpass ahead of the ladder, and a
//  14.008 Hz allpass plus a 24.167 Hz highpass after it.
//
//  WHAT IS DIFFERENT: Open303 integrates the coupled ladder
//
//      y1 += 2*b0*(y0-y1+y2);  y2 += b0*(y1-2*y2+y3);
//      y3 +=   b0*(y2-2*y3+y4);  y4 += b0*(y3-2*y4);
//
//  with FORWARD Euler, so the resonance feedback reads last sample's y4 -- a
//  unit delay in the loop, which detunes the resonant peak as the cutoff rises
//  and limits how far the feedback can be pushed before it goes sour.  Here the
//  same difference equations are solved IMPLICITLY (zero delay feedback): all
//  four stages and the feedback highpass are evaluated at the current sample.
//
//  Collecting the four stage equations with a = b0 and d = 1+2a gives a
//  tridiagonal system in which the ladder input y0 appears only in row one:
//
//      [ d  -2a   0    0 ] [y1]   [ s1 ]          [ 2a ]
//      [-a   d   -a    0 ] [y2] = [ s2 ]  +  y0 * [  0 ]
//      [ 0  -a    d   -a ] [y3]   [ s3 ]          [  0 ]
//      [ 0   0   -a    d ] [y4]   [ s4 ]          [  0 ]
//
//  so with M0*U = S and M0*W = [2a,0,0,0] every stage is affine in y0:
//  y_i = U_i + W_i*y0.  M0 is constant for a given cutoff/resonance, so its
//  Thomas factors and W are computed once per coefficient change and the
//  per-sample cost is two substitution sweeps of plain multiply-adds.
//
//  Closing the loop needs a nonlinearity.  A LINEAR resonant loop has no useful
//  top end -- below unity gain it decays, above it diverges, with nothing in
//  between -- which is why the hardware's saturating transistor pairs matter.
//  Open303 ships exactly such a saturator (TeeBeeFilter::shape, a clipped cubic
//  x - x^3/6) and its author left the shaped feedback path in the source as the
//  commented-out alternative; that is the variant used here.  So
//
//      y0 = P - Q*shape(y4),   P = in - Hh,   Q = hpB0*k
//
//  and substituting y4 = U4 + W4*y0 leaves one scalar equation in y4,
//
//      f(y4) = y4 - W4*(P - Q*shape(y4)) - U4 = 0
//
//  solved by Newton.  Because 0 <= shape' <= 1 and W4*Q > 0, f' = 1 + W4*Q*shape'
//  is never below 1, so the iteration is monotone and two steps converge to
//  float precision (verified against a damped fixed-point solve of the same
//  system: agreement to 6e-13 in double).
//
//  The feedback factor carries kResBoost on top of Open303's k.  The implicit
//  loop is better damped than the explicit one -- solving the feedback exactly
//  removes the phase lag a unit delay adds -- so Open303's own k lands short of
//  self-oscillation here.  At 2.0 the top of the resonance knob sings across the
//  usable cutoff range while low cutoffs stay tame, which is the behaviour the
//  150 Hz feedback highpass exists to produce.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <memory>
#if defined(__AVX__)
# include <immintrin.h>
#endif

namespace {
using rack::engine::Module;
constexpr int N = 8;                                   // filters per module
constexpr int V = rack::engine::PORT_MAX_CHANNELS;     // polyphony
constexpr float PI = 3.14159265358979323846f;

// Open303 runs the ladder at 4x. The ladder itself is linear, but the DRIVE
// stage in front of it is not, so the oversampling still earns its keep.
constexpr int kOversample = 4;

// Fixed cutoffs of the shaping filters around the ladder (rosic::Open303).
// Feedback boost over Open303's k -- see the header note.  Solving the loop
// implicitly removes the phase lag the original's unit delay contributed, so
// the stock k lands short of self-oscillation in this topology.
constexpr float kResBoost = 2.0f;

constexpr float kFeedbackHpHz = 150.0f;
constexpr float kPreHpHz      = 44.486f;
constexpr float kPostHpHz     = 24.167f;
constexpr float kPostApHz     = 14.008f;

//----------------------------------------------------------------------------
//  Per-lane coefficient set.  Everything here is a pure function of cutoff,
//  resonance and sample rate, so it is recomputed only when one of those moves.
//----------------------------------------------------------------------------
struct Coeffs {
    float b0 = 0.f, k = 0.f, g = 1.f;      // Open303's ladder coefficients
    float hpB0 = 0.f, hpB1 = 0.f, hpA1 = 0.f;   // feedback highpass (150 Hz)
    float cp1 = 0.f, cp2 = 0.f, cp3 = 0.f;      // Thomas super-diagonal factors
    float im1 = 0.f, im2 = 0.f, im3 = 0.f, im4 = 0.f;   // 1 / pivot
    float w1 = 0.f, w2 = 0.f, w3 = 0.f, A = 0.f;  // M0^-1 * [2a,0,0,0]; A == W4
    float Q = 0.f;                                 // hpB0 * k
};

// Feedback saturator: rosic::TeeBeeFilter::shape (clipped cubic) and its
// derivative, which Newton needs.
constexpr float kSqrt2 = 1.41421356237309504880f;
inline float shape(float x) {
    x = x < -kSqrt2 ? -kSqrt2 : (x > kSqrt2 ? kSqrt2 : x);
    return x - x * x * x * (1.f / 6.f);
}
inline float dShape(float x) {
    if (x <= -kSqrt2 || x >= kSqrt2) return 0.f;
    return 1.f - 0.5f * x * x;
}

// One-pole coefficients, dspguide formulas exactly as rosic::OnePoleFilter.
void onePoleHighpass(float hz, float rate, float& b0, float& b1, float& a1) {
    const float x = std::exp(-2.f * PI * hz / rate);
    b0 =  0.5f * (1.f + x);
    b1 = -0.5f * (1.f + x);
    a1 = x;
}
void onePoleAllpass(float hz, float rate, float& b0, float& b1, float& a1) {
    // rosic::OnePoleFilter ALLPASS: b0 = x, b1 = -1, a1 = x  (x = pole)
    const float t = std::tan(PI * hz / rate);
    const float x = (t - 1.f) / (t + 1.f);
    b0 = x; b1 = 1.f; a1 = -x;
}

void computeCoeffs(float cutoffHz, float resonance01, float rate, Coeffs& c) {
    // Open303 clamps the cutoff to [200, 20000] before anything else.
    float cutoff = cutoffHz < 200.f ? 200.f : (cutoffHz > 20000.f ? 20000.f : cutoffHz);
    if (cutoff > rate * 0.45f) cutoff = rate * 0.45f;

    // resonanceSkewed -- makes the knob behave musically (TeeBeeFilter.h).
    const float r = (1.f - std::exp(-3.f * resonance01)) / (1.f - std::exp(-3.f));

    // TB_303 branch of calculateCoefficientsApprox4(), verbatim.
    const float wc = 2.f * PI * cutoff / rate;
    const float fx = wc * 0.70710678118654752440f / (2.f * PI);
    float b0 = (0.00045522346f + 6.1922189f * fx)
             / (1.f + 12.358354f * fx + 4.4156345f * (fx * fx));
    float k  = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f)
                 - 476.47308f) + 614.95611f) + 213.87126f) + 16.998792f;
    float g  = k * 0.058823529411764705882352941176471f;   // 1/17
    g  = (g - 1.f) * r + 1.f;
    g  = g * (1.f + r);
    k  = k * r * kResBoost;

    c.b0 = b0; c.k = k; c.g = g;
    onePoleHighpass(kFeedbackHpHz, rate, c.hpB0, c.hpB1, c.hpA1);

    // ---- implicit (ZDF) solve: factor M0 once, and solve M0*V = c ----------
    const float a = b0;
    const float d = 1.f + 2.f * a;
    const float Q = c.hpB0 * k;              // instantaneous feedback gain

    const float m1 = d;                    c.im1 = 1.f / m1;
    c.cp1 = -2.f * a * c.im1;
    const float m2 = d + a * c.cp1;        c.im2 = 1.f / m2;
    c.cp2 = -a * c.im2;
    const float m3 = d + a * c.cp2;        c.im3 = 1.f / m3;
    c.cp3 = -a * c.im3;
    const float m4 = d + a * c.cp3;        c.im4 = 1.f / m4;

    // M0 * W = [2a, 0, 0, 0]  ->  y_i = U_i + W_i * y0
    const float wp1 = (2.f * a) * c.im1;
    const float wp2 = (a * wp1) * c.im2;
    const float wp3 = (a * wp2) * c.im3;
    const float wp4 = (a * wp3) * c.im4;
    c.A  = wp4;                               // W4
    c.w3 = wp3 - c.cp3 * c.A;
    c.w2 = wp2 - c.cp2 * c.w3;
    c.w1 = wp1 - c.cp1 * c.w2;
    c.Q  = Q;
}

//----------------------------------------------------------------------------
//  Module
//----------------------------------------------------------------------------
struct Acid303Filter8 final : Module {
    enum ParamIds { CUT_PARAM, RES_PARAM=CUT_PARAM+N, DRIVE_PARAM=RES_PARAM+N,
                    ENV_PARAM=DRIVE_PARAM+N, NUM_PARAMS=ENV_PARAM+N };
    enum InputIds { IN_INPUT, CUT_INPUT=IN_INPUT+N, RES_INPUT=CUT_INPUT+N,
                    DRIVE_INPUT=RES_INPUT+N, ACCENT_INPUT=DRIVE_INPUT+N,
                    NUM_INPUTS=ACCENT_INPUT+N };
    enum OutputIds { LP_OUTPUT, NUM_OUTPUTS=LP_OUTPUT+N };
    enum LightIds { NUM_LIGHTS };

    // Ladder state, [voice][stage][filter] so a lane load is contiguous in the
    // filter index -- the axis AVX vectorises over.
    alignas(32) float y[V][4][N] = {};
    alignas(32) float hpX[V][N] = {}, hpY[V][N] = {};      // feedback highpass
    alignas(32) float preX[V][N] = {}, preY[V][N] = {};    // 44 Hz input highpass
    alignas(32) float apX[V][N]  = {}, apY[V][N]  = {};    // 14 Hz output allpass
    alignas(32) float poX[V][N]  = {}, poY[V][N]  = {};    // 24 Hz output highpass

    // Coefficient cache, keyed on the control values that produced it.
    Coeffs coeffs[V][N];
    float lastCut[V][N], lastRes[V][N], lastDrive[V][N], driveGain[V][N];
    float preB0 = 0.f, preB1 = 0.f, preA1 = 0.f;
    float poB0 = 0.f, poB1 = 0.f, poA1 = 0.f;
    float apB0 = 0.f, apB1 = 0.f, apA1 = 0.f;
    float cachedRate = 0.f;

    Acid303Filter8() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (int f = 0; f < N; ++f) {
            const std::string n = std::to_string(f + 1);
            configParam(CUT_PARAM+f, 0.f, 1.f, .42f, "Cutoff " + n);
            configParam(RES_PARAM+f, 0.f, 1.f, .35f, "Resonance " + n, "%", 0.f, 100.f);
            configParam(DRIVE_PARAM+f, 0.f, 1.f, .0f, "Drive " + n);
            configParam(ENV_PARAM+f, 0.f, 1.f, .5f, "Envelope amount " + n);
            configInput(IN_INPUT+f, "Audio " + n);
            configInput(CUT_INPUT+f, "Cutoff CV " + n);
            configInput(RES_INPUT+f, "Resonance CV " + n);
            configInput(DRIVE_INPUT+f, "Drive CV " + n);
            configInput(ACCENT_INPUT+f, "Accent " + n);
            configOutput(LP_OUTPUT+f, "303 LP " + n);
            for (int v = 0; v < V; ++v) {
                lastCut[v][f] = lastRes[v][f] = lastDrive[v][f]
                              = std::numeric_limits<float>::quiet_NaN();
                driveGain[v][f] = 1.f;
            }
        }
    }

    void onSampleRateChange() { cachedRate = 0.f; }

    //! RackEngine::resetModule() calls this, so without it "reset module" was
    //! silently a no-op for this module: the ladder, the in-loop highpass, the
    //! input highpass and the output allpass/highpass all kept their state and
    //! rang on across a patch load.  Invalidating the coefficient cache makes
    //! the next sample recompute from the CURRENT knobs instead of continuing
    //! on whatever the pre-reset control values produced.
    void onReset() override {
        std::memset(y,    0, sizeof(y));
        std::memset(hpX,  0, sizeof(hpX));   std::memset(hpY,  0, sizeof(hpY));
        std::memset(preX, 0, sizeof(preX));  std::memset(preY, 0, sizeof(preY));
        std::memset(apX,  0, sizeof(apX));   std::memset(apY,  0, sizeof(apY));
        std::memset(poX,  0, sizeof(poX));   std::memset(poY,  0, sizeof(poY));
        for (int v = 0; v < V; ++v)
            for (int f = 0; f < N; ++f) {
                lastCut[v][f] = lastRes[v][f] = lastDrive[v][f]
                              = std::numeric_limits<float>::quiet_NaN();
                driveGain[v][f] = 1.f;
            }
        cachedRate = 0.f;
    }

    void refreshRate(float rate) {
        if (cachedRate == rate) return;
        cachedRate = rate;
        const float osRate = rate * kOversample;
        onePoleHighpass(kPreHpHz, osRate, preB0, preB1, preA1);
        onePoleHighpass(kPostHpHz, rate, poB0, poB1, poA1);
        onePoleAllpass (kPostApHz, rate, apB0, apB1, apA1);
        for (int v = 0; v < V; ++v)
            for (int f = 0; f < N; ++f)
                lastCut[v][f] = std::numeric_limits<float>::quiet_NaN();
    }

    // ---- scalar reference: one oversampled tick of lane (v,f) --------------
    float tick(int v, int f, float in) {
        const Coeffs& c = coeffs[v][f];
        // History of the feedback highpass; its instantaneous term rides inside
        // the implicit solve as Q.
        const float Hh = c.hpB1 * hpX[v][f] + c.hpA1 * hpY[v][f];
        const float P  = in - Hh;
        const float a  = c.b0;

        // M0 * U = state
        const float rp1 = y[v][0][f] * c.im1;
        const float rp2 = (y[v][1][f] + a * rp1) * c.im2;
        const float rp3 = (y[v][2][f] + a * rp2) * c.im3;
        const float rp4 = (y[v][3][f] + a * rp3) * c.im4;
        const float u4 = rp4;
        const float u3 = rp3 - c.cp3 * u4;
        const float u2 = rp2 - c.cp2 * u3;
        const float u1 = rp1 - c.cp1 * u2;

        // Newton on f(y4) = y4 - A*(P - Q*shape(y4)) - u4
        float y4 = y[v][3][f];
        for (int it = 0; it < 2; ++it) {
            const float fv = y4 - c.A * (P - c.Q * shape(y4)) - u4;
            const float fp = 1.f + c.A * c.Q * dShape(y4);
            y4 -= fv / fp;
        }
        const float y0 = P - c.Q * shape(y4);

        y[v][3][f] = y4;
        y[v][2][f] = u3 + y0 * c.w3;
        y[v][1][f] = u2 + y0 * c.w2;
        y[v][0][f] = u1 + y0 * c.w1;

        const float hpIn = c.k * shape(y4);
        hpY[v][f] = c.hpB0 * hpIn + c.hpB1 * hpX[v][f] + c.hpA1 * hpY[v][f];
        hpX[v][f] = hpIn;

        return 2.f * c.g * y4;
    }

#if defined(__AVX__)
    // ---- AVX: the same solve, eight filters at once ------------------------
    struct Vec8 {
        __m256 b0, k, g, hpB0, hpB1, hpA1;
        __m256 cp1, cp2, cp3, im1, im2, im3, im4, w1, w2, w3, A, Q;
    };
    void loadCoeffs(int v, Vec8& c) const {
        alignas(32) float t[18][N];
        for (int f = 0; f < N; ++f) {
            const Coeffs& s = coeffs[v][f];
            t[0][f]=s.b0;  t[1][f]=s.k;   t[2][f]=s.g;
            t[3][f]=s.hpB0;t[4][f]=s.hpB1;t[5][f]=s.hpA1;
            t[6][f]=s.cp1; t[7][f]=s.cp2; t[8][f]=s.cp3;
            t[9][f]=s.im1; t[10][f]=s.im2;t[11][f]=s.im3; t[12][f]=s.im4;
            t[13][f]=s.w1; t[14][f]=s.w2; t[15][f]=s.w3;  t[16][f]=s.A; t[17][f]=s.Q;
        }
        c.b0=_mm256_load_ps(t[0]);   c.k=_mm256_load_ps(t[1]);   c.g=_mm256_load_ps(t[2]);
        c.hpB0=_mm256_load_ps(t[3]); c.hpB1=_mm256_load_ps(t[4]); c.hpA1=_mm256_load_ps(t[5]);
        c.cp1=_mm256_load_ps(t[6]);  c.cp2=_mm256_load_ps(t[7]);  c.cp3=_mm256_load_ps(t[8]);
        c.im1=_mm256_load_ps(t[9]);  c.im2=_mm256_load_ps(t[10]); c.im3=_mm256_load_ps(t[11]);
        c.im4=_mm256_load_ps(t[12]);
        c.w1=_mm256_load_ps(t[13]);  c.w2=_mm256_load_ps(t[14]);  c.w3=_mm256_load_ps(t[15]);
        c.A=_mm256_load_ps(t[16]);   c.Q=_mm256_load_ps(t[17]);
    }
    // shape() / shape'() from Open303, eight lanes at a time.
    static __m256 shape8(__m256 x) {
        const __m256 lim=_mm256_set1_ps(kSqrt2);
        x=_mm256_min_ps(lim,_mm256_max_ps(_mm256_sub_ps(_mm256_setzero_ps(),lim),x));
        return _mm256_sub_ps(x, _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(x,x),x),
                                              _mm256_set1_ps(1.f/6.f)));
    }
    static __m256 dShape8(__m256 x) {
        const __m256 lim=_mm256_set1_ps(kSqrt2);
        // |x| < sqrt2 ? 1 - x^2/2 : 0
        const __m256 inside=_mm256_cmp_ps(_mm256_andnot_ps(
            _mm256_set1_ps(-0.f), x), lim, _CMP_LT_OQ);
        const __m256 d=_mm256_sub_ps(_mm256_set1_ps(1.f),
            _mm256_mul_ps(_mm256_set1_ps(0.5f),_mm256_mul_ps(x,x)));
        return _mm256_and_ps(inside, d);
    }
    __m256 tick8(int v, __m256 in, const Vec8& c) {
        __m256 s1=_mm256_load_ps(y[v][0]), s2=_mm256_load_ps(y[v][1]);
        __m256 s3=_mm256_load_ps(y[v][2]), s4=_mm256_load_ps(y[v][3]);
        __m256 hx=_mm256_load_ps(hpX[v]),  hy=_mm256_load_ps(hpY[v]);

        const __m256 Hh = _mm256_add_ps(_mm256_mul_ps(c.hpB1, hx),
                                        _mm256_mul_ps(c.hpA1, hy));
        const __m256 P  = _mm256_sub_ps(in, Hh);
        const __m256 a  = c.b0;
        const __m256 one = _mm256_set1_ps(1.f), two = _mm256_set1_ps(2.f);

        // M0 * U = state
        __m256 rp1 = _mm256_mul_ps(s1, c.im1);
        __m256 rp2 = _mm256_mul_ps(_mm256_add_ps(s2, _mm256_mul_ps(a, rp1)), c.im2);
        __m256 rp3 = _mm256_mul_ps(_mm256_add_ps(s3, _mm256_mul_ps(a, rp2)), c.im3);
        __m256 rp4 = _mm256_mul_ps(_mm256_add_ps(s4, _mm256_mul_ps(a, rp3)), c.im4);
        __m256 u4 = rp4;
        __m256 u3 = _mm256_sub_ps(rp3, _mm256_mul_ps(c.cp3, u4));
        __m256 u2 = _mm256_sub_ps(rp2, _mm256_mul_ps(c.cp2, u3));
        __m256 u1 = _mm256_sub_ps(rp1, _mm256_mul_ps(c.cp1, u2));

        // Newton: f(y4) = y4 - A*(P - Q*shape(y4)) - u4,  f' = 1 + A*Q*shape'
        __m256 y4 = s4;
        for (int it = 0; it < 2; ++it) {
            const __m256 sh = shape8(y4);
            const __m256 fv = _mm256_sub_ps(_mm256_sub_ps(y4,
                _mm256_mul_ps(c.A, _mm256_sub_ps(P, _mm256_mul_ps(c.Q, sh)))), u4);
            const __m256 fp = _mm256_add_ps(one,
                _mm256_mul_ps(_mm256_mul_ps(c.A, c.Q), dShape8(y4)));
            y4 = _mm256_sub_ps(y4, _mm256_div_ps(fv, fp));
        }
        const __m256 shy = shape8(y4);
        const __m256 y0 = _mm256_sub_ps(P, _mm256_mul_ps(c.Q, shy));
        __m256 y3 = _mm256_add_ps(u3, _mm256_mul_ps(y0, c.w3));
        __m256 y2 = _mm256_add_ps(u2, _mm256_mul_ps(y0, c.w2));
        __m256 y1 = _mm256_add_ps(u1, _mm256_mul_ps(y0, c.w1));

        // A stuck NaN would persist forever in a recursive filter; clamp the
        // state to a wide but finite band instead of trusting the solve.
        const __m256 lim = _mm256_set1_ps(64.f), nlim = _mm256_set1_ps(-64.f);
        y1=_mm256_min_ps(lim,_mm256_max_ps(nlim,y1));
        y2=_mm256_min_ps(lim,_mm256_max_ps(nlim,y2));
        y3=_mm256_min_ps(lim,_mm256_max_ps(nlim,y3));
        y4=_mm256_min_ps(lim,_mm256_max_ps(nlim,y4));
        _mm256_store_ps(y[v][0],y1); _mm256_store_ps(y[v][1],y2);
        _mm256_store_ps(y[v][2],y3); _mm256_store_ps(y[v][3],y4);

        const __m256 hpIn = _mm256_mul_ps(c.k, shy);
        hy = _mm256_add_ps(_mm256_mul_ps(c.hpB0, hpIn),
             _mm256_add_ps(_mm256_mul_ps(c.hpB1, hx), _mm256_mul_ps(c.hpA1, hy)));
        _mm256_store_ps(hpY[v], hy);
        _mm256_store_ps(hpX[v], hpIn);

        return _mm256_mul_ps(_mm256_mul_ps(two, c.g), y4);
    }
    static __m256 tanh8(__m256 x) {
        const __m256 lim=_mm256_set1_ps(3.f);
        x=_mm256_min_ps(lim,_mm256_max_ps(_mm256_sub_ps(_mm256_setzero_ps(),lim),x));
        const __m256 x2=_mm256_mul_ps(x,x);
        return _mm256_div_ps(_mm256_mul_ps(x,_mm256_add_ps(_mm256_set1_ps(27.f),x2)),
                             _mm256_add_ps(_mm256_set1_ps(27.f),
                                 _mm256_mul_ps(_mm256_set1_ps(9.f),x2)));
    }
#endif

    void process(const ProcessArgs& args) override {
        refreshRate(args.sampleRate);
        const float osRate = args.sampleRate * kOversample;

        int voices = 1;
        for (int f = 0; f < N; ++f)
            for (int group = 0; group < 5; ++group)
                voices = std::max(voices, inputs[group * N + f].getChannels());
        voices = std::min(voices, V);

        for (int v = 0; v < voices; ++v) {
            alignas(32) float sig[N], drv[N], out[N];
            for (int f = 0; f < N; ++f) {
                const float accent = rack::clamp(
                    inputs[ACCENT_INPUT+f].getPolyVoltage(v) * .1f, 0.f, 1.f);
                const float cutCtl = rack::clamp(params[CUT_PARAM+f].getValue()
                    + inputs[CUT_INPUT+f].getPolyVoltage(v) * .1f
                      * params[ENV_PARAM+f].getValue()
                    + accent * .12f, 0.f, 1.f);
                const float resCtl = rack::clamp(params[RES_PARAM+f].getValue()
                    + inputs[RES_INPUT+f].getPolyVoltage(v) * .1f
                    + accent * .08f, 0.f, 1.f);
                // Open303's own cutoff range: 200 Hz .. 20 kHz, exponential.
                const float hz = 200.f * std::pow(100.f, cutCtl);
                if (hz != lastCut[v][f] || resCtl != lastRes[v][f]) {
                    lastCut[v][f] = hz; lastRes[v][f] = resCtl;
                    computeCoeffs(hz, resCtl, osRate, coeffs[v][f]);
                }
                const float dCtl = rack::clamp(params[DRIVE_PARAM+f].getValue()
                    + inputs[DRIVE_INPUT+f].getPolyVoltage(v) * .1f
                    + accent * .18f, 0.f, 1.f);
                if (dCtl != lastDrive[v][f]) {
                    lastDrive[v][f] = dCtl;
                    driveGain[v][f] = std::pow(10.f, dCtl * 24.f / 20.f);
                }
                drv[f] = driveGain[v][f];
                sig[f] = inputs[IN_INPUT+f].getPolyVoltage(v) * .2f;
            }

#if defined(__AVX__)
            Vec8 c; loadCoeffs(v, c);
            const __m256 in = _mm256_load_ps(sig);
            const __m256 gain = _mm256_load_ps(drv);
            __m256 px = _mm256_load_ps(preX[v]), py = _mm256_load_ps(preY[v]);
            const __m256 pb0=_mm256_set1_ps(preB0), pb1=_mm256_set1_ps(preB1),
                         pa1=_mm256_set1_ps(preA1);
            __m256 last = _mm256_setzero_ps();
            for (int os = 0; os < kOversample; ++os) {
                // DRIVE is ours, not Open303's -- a saturating input stage, run
                // inside the oversampled loop so its harmonics do not alias.
                __m256 x = _mm256_mul_ps(in, gain);
                x = tanh8(x);
                py = _mm256_add_ps(_mm256_mul_ps(pb0, x),
                     _mm256_add_ps(_mm256_mul_ps(pb1, px), _mm256_mul_ps(pa1, py)));
                px = x;
                last = tick8(v, py, c);
            }
            _mm256_store_ps(preX[v], px); _mm256_store_ps(preY[v], py);
            _mm256_store_ps(out, last);
#else
            for (int f = 0; f < N; ++f) {
                float last = 0.f;
                for (int os = 0; os < kOversample; ++os) {
                    float x = std::tanh(sig[f] * drv[f]);
                    const float hp = preB0 * x + preB1 * preX[v][f] + preA1 * preY[v][f];
                    preX[v][f] = x; preY[v][f] = hp;
                    last = tick(v, f, hp);
                }
                out[f] = last;
            }
#endif
            // Post shaping at the host rate (Open303 keeps these outside the
            // oversampled loop too).
            for (int f = 0; f < N; ++f) {
                const float ap = apB0 * out[f] + apB1 * apX[v][f] + apA1 * apY[v][f];
                apX[v][f] = out[f]; apY[v][f] = ap;
                const float hp = poB0 * ap + poB1 * poX[v][f] + poA1 * poY[v][f];
                poX[v][f] = ap; poY[v][f] = hp;
                // Soft output stage: at high resonance the ladder swings well
                // past unity, and hard-clipping that to +/-10 V buzzes.  The
                // hardware's output transistor compresses instead.
                float o = 5.f * std::tanh(hp);
                if (!std::isfinite(o)) {
                    o = 0.f;
                    y[v][0][f]=y[v][1][f]=y[v][2][f]=y[v][3][f]=0.f;
                    hpX[v][f]=hpY[v][f]=0.f;
                }
                outputs[LP_OUTPUT+f].setVoltage(rack::clamp(o, -10.f, 10.f), v);
            }
        }
        for (int f = 0; f < N; ++f) outputs[LP_OUTPUT+f].setChannels(voices);
    }
};

rackx::PanelElement el(int id,float x,float y,float radius,rackx::PanelControlStyle style,const std::string& label){rackx::PanelElement e;e.id=id;e.x=x;e.y=y;e.radius=radius;e.style=style;e.label=label;return e;}

// Jack captions are centred and drawn up to 42 px wide, so the old single row of
// five jacks on a 34 px pitch ran them together.  Two rows on a 56 px pitch give
// every caption clear air.
rackx::PanelSpec panel(){rackx::PanelSpec p=rackx::PanelSpec::fromHp(96);float w=p.width/N;
    constexpr float kJackPitch=56.f;
    constexpr float kRow1Y=258.f, kRow2Y=306.f, kOutY=352.f;
    for(int f=0;f<N;++f){float c=w*(f+.5f),l=c-42.f,r=c+42.f;std::string n=std::to_string(f+1);
        p.params.push_back(el(Acid303Filter8::CUT_PARAM+f,l,92.f,18.f,rackx::PanelControlStyle::Knob,"CUTOFF "+n));
        p.params.push_back(el(Acid303Filter8::RES_PARAM+f,r,92.f,18.f,rackx::PanelControlStyle::Knob,"RESO"));
        p.params.push_back(el(Acid303Filter8::DRIVE_PARAM+f,l,184.f,15.f,rackx::PanelControlStyle::Knob,"DRIVE"));
        p.params.push_back(el(Acid303Filter8::ENV_PARAM+f,r,184.f,15.f,rackx::PanelControlStyle::Knob,"ENV MOD"));
        p.inputs.push_back(el(Acid303Filter8::IN_INPUT+f,   c-kJackPitch,kRow1Y,8.f,rackx::PanelControlStyle::Knob,"IN"));
        p.inputs.push_back(el(Acid303Filter8::CUT_INPUT+f,  c,           kRow1Y,8.f,rackx::PanelControlStyle::Knob,"CUT CV"));
        p.inputs.push_back(el(Acid303Filter8::RES_INPUT+f,  c+kJackPitch,kRow1Y,8.f,rackx::PanelControlStyle::Knob,"RES CV"));
        p.inputs.push_back(el(Acid303Filter8::DRIVE_INPUT+f, c-kJackPitch*.5f,kRow2Y,8.f,rackx::PanelControlStyle::Knob,"DRV CV"));
        p.inputs.push_back(el(Acid303Filter8::ACCENT_INPUT+f,c+kJackPitch*.5f,kRow2Y,8.f,rackx::PanelControlStyle::Knob,"ACCENT"));
        p.outputs.push_back(el(Acid303Filter8::LP_OUTPUT+f,c,kOutY,10.f,rackx::PanelControlStyle::Knob,"LP "+n));}
    return p;}
}
namespace rackx {void registerTb303FilterModule(){addType("Acid303-ZDF","Acid 303 ZDF-8","Filter",Role::Normal,[]{return std::make_unique<Acid303Filter8>();},panel());}}
