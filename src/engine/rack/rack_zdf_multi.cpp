//----------------------------------------------------------------------------
//  rack_zdf_multi.cpp -- ZDFX: multimode / multi-pole / multi-dB ZDF filter.
//
//  ONE structure gives every mode.  A 4-stage zero-delay-feedback (TPT) ladder
//  is solved exactly each sample, and the output is a weighted mix of the input
//  and the four stage taps:
//
//      out = a0*x + a1*y1 + a2*y2 + a3*y3 + a4*y4
//
//  That is the Oberheim Xpander trick: the binomial tap vectors give LP/HP at
//  6/12/18/24 dB, band-pass, notch, all-pass, peaking and phase-shift responses
//  from the SAME four integrators, and interpolating between two vectors morphs
//  continuously between their slopes.  So "multimode", "multi-pole" and
//  "multi-dB" are one axis, not three separate filters.
//
//  WHY IT CANNOT BLOW UP.  The ladder is solved implicitly:
//      y4 = (G^4*x + S) / (1 + k*G^4)
//  with G = g/(1+g) in (0,1) and k >= 0, so the divisor is always >= 1 -- there
//  is no delay-free loop to run away and no resonance setting that makes the
//  linear core unstable.  The nonlinearity is applied as a BOUNDED CORRECTION on
//  top of that solve rather than inside it, so grit cannot turn into divergence.
//  On top: cutoff clamped to [16 Hz, 0.45*rate], saturators bounded by
//  construction, and a per-sample non-finite guard that resets a voice's state
//  instead of letting a NaN reach the mix.
//
//  CHARACTER is a single continuous axis, CLEAN in the middle:
//      < 0.5  GRITTY   input drive + hard, symmetric feedback clipping
//      = 0.5  CLEAN    linear -- the textbook ladder
//      > 0.5  SQUELCHY asymmetric saturation + the 303's feedback highpass,
//                      which is what stops a high-resonance sweep booming and
//                      leaves the vocal "squelch" instead
//
//  SSE: voices are the only axis with real parallelism here -- the four ladder
//  stages are a serial cascade -- so the plan is four polyphonic voices per
//  128-bit lane, the same choice rack_vco4_sse.cpp makes.  NOT YET VECTORISED:
//  the per-voice loop below is still scalar.  The diode Newton solve needs a
//  vectorised sinh/cosh before the lane version is worth writing, so that lands
//  with the SSE pass rather than being faked here.
//
//  Every continuous control is a CONCENTRIC KnobCV: outer ring = CV depth
//  (bipolar attenuverter), inner disc = base value, one jack beneath.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "rack_panel_kit.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#  include <emmintrin.h>
#  define PK_ZDFX_SSE 1
#endif

namespace {

using rack::engine::Module;

constexpr float kPi = 3.14159265358979323846f;
constexpr int   kMaxVoices = rack::engine::PORT_MAX_CHANNELS;   // 16
constexpr int   kLanes     = 4;                                  // one SSE lane

//----------------------------------------------------------------------------
//  Mode table.  Each entry is a tap-mix vector (a0..a4) over {x, y1..y4}.
//  The binomial HP rows are (1-z)^n expanded over the taps, which is why an
//  n-pole highpass falls out of a lowpass ladder at all.
//----------------------------------------------------------------------------
struct ModeDef { const char* name; float a[5]; };

const ModeDef kModes[] = {
    // --- lowpass, 6 -> 24 dB/oct ---
    { "LP 6",      {  0,  1,  0,  0,  0 } },
    { "LP 12",     {  0,  0,  1,  0,  0 } },
    { "LP 18",     {  0,  0,  0,  1,  0 } },
    { "LP 24",     {  0,  0,  0,  0,  1 } },
    // --- highpass: binomial (1-z)^n ---
    { "HP 6",      {  1, -1,  0,  0,  0 } },
    { "HP 12",     {  1, -2,  1,  0,  0 } },
    { "HP 18",     {  1, -3,  3, -1,  0 } },
    { "HP 24",     {  1, -4,  6, -4,  1 } },
    // --- bandpass ---
    { "BP 6/6",    {  0,  2, -2,  0,  0 } },
    { "BP 12/6",   {  0,  0,  4, -4,  0 } },
    { "BP 6/12",   {  0,  2, -4,  2,  0 } },
    { "BP 12/12",  {  0,  0,  4, -8,  4 } },
    // --- notch / band-reject ---
    { "NOTCH 2",   {  1, -2,  2,  0,  0 } },
    { "NOTCH 4",   {  1, -4,  6, -4,  2 } },
    // --- allpass / phase ---
    { "AP 2",      {  1, -2,  2,  0,  0 } },
    { "AP 3",      {  1, -3,  6, -4,  0 } },
    { "AP 4",      {  1, -4, 12, -16, 8 } },
    // --- peaking / shelving oddities ---
    { "PEAK",      {  0,  0,  2, -2,  1 } },
    { "LO SHELF",  {  1,  0,  1,  0,  0 } },
    { "HI SHELF",  {  1, -2,  0,  0,  1 } },
    // --- deliberately strange ---
    { "PHASER",    {  1, -2,  0,  2, -1 } },
    { "COMB-ish",  {  1,  0, -2,  0,  1 } },
    { "HOLLOW",    {  0,  1, -2,  2, -1 } },
    { "VOWEL",     {  0,  3, -6,  4,  0 } }
};
constexpr int kNumModes = (int)(sizeof(kModes) / sizeof(kModes[0]));

//! Bounded odd saturator (clipped cubic, the Open303 shape).  Bounded BY
//! CONSTRUCTION -- |shape(x)| <= sqrt(2)*2/3 -- which is what lets it sit in the
//! feedback correction without threatening the solve.
constexpr float kSqrt2 = 1.41421356237309504880f;
inline float shape(float x) {
    x = x < -kSqrt2 ? -kSqrt2 : (x > kSqrt2 ? kSqrt2 : x);
    return x - x * x * x * (1.f / 6.f);
}
//! Asymmetric variant: squashes the positive half harder, so the residual is
//! even-order.  Even harmonics are most of what reads as "squelchy" / vocal.
inline float shapeAsym(float x, float amt) {
    const float s = shape(x);
    const float e = shape(x * 0.5f + 0.25f) - shape(0.25f);
    return s + amt * (e - s) * 0.5f;
}
inline float softClip(float x) {
    if (x >  1.5f) return  1.f;
    if (x < -1.5f) return -1.f;
    return x - (4.f / 27.f) * x * x * x;
}

//----------------------------------------------------------------------------
//  DIODE DRIVE -- a real Shockley model, not a tanh.
//
//  Worth being precise about why: a BJT differential pair's transfer function
//  IS tanh(v/2Vt) -- that is the physics of the Moog ladder, so "transistor
//  drive" and "tanh" are the same curve.  A DIODE clipper is a different animal.
//  Antiparallel diodes across a series resistor give
//
//      i(v) = Is * (e^(v/nVt) - e^(-v/nVt)) = 2*Is*sinh(v/nVt)
//      x    = y + R*i(y)                    (implicit -- no closed form)
//
//  so the curve is the INVERSE of a sinh: a soft knee that keeps compressing
//  rather than flattening, and whose onset is set by the diode's saturation
//  current.  That is what makes germanium mush early and an LED stay clean and
//  loud before it bites, and it is why swapping diode types is a real tone
//  control rather than a cosmetic one.
//
//  STABILITY: f(y) = y + k*sinh(y/vt) - x has f'(y) = 1 + (k/vt)*cosh(y/vt),
//  which is >= 1 EVERYWHERE.  f is therefore strictly monotonic with a
//  derivative bounded away from zero, so Newton cannot divide by ~0 and cannot
//  oscillate; every step is a contraction toward the single real root.  The
//  exponent is clamped before sinh/cosh so a large transient cannot overflow to
//  inf on the way there.  Warm-started from the previous sample, it converges
//  in one or two iterations at audio rates.
//----------------------------------------------------------------------------
struct DiodeSpec { const char* name; float vt; float k; };
// vt == n*Vt (ideality x thermal voltage), k == R*2*Is scaled into our +/-1
// working range.  Values follow the usual small-signal datasheet figures:
// germanium leaks orders of magnitude more than silicon, an LED far less.
const DiodeSpec kDiodes[] = {
    { "Si 1N4148",  0.050f, 0.35f },   // classic silicon: firm, familiar
    { "Ge OA90",    0.036f, 1.10f },   // germanium: early, mushy, asymmetric-ish
    { "LED red",    0.110f, 0.06f },   // high forward drop: clean then abrupt
    { "Schottky",   0.030f, 1.60f },   // softest, earliest onset
    { "Si x2",      0.100f, 0.35f },   // two in series each way: louder knee
    { "MOSFET",     0.075f, 0.22f }    // square-law-ish middle ground
};
constexpr int kNumDiodes = (int)(sizeof(kDiodes) / sizeof(kDiodes[0]));

inline float diodeClip(float x, float vt, float k, float seed) {
    float y = seed;
    for (int it = 0; it < 3; ++it) {
        const float a  = rack::clamp(y / vt, -18.f, 18.f);   // exp overflow guard
        const float sh = std::sinh(a);
        const float ch = std::cosh(a);
        const float f  = y + k * sh - x;
        const float fd = 1.f + (k / vt) * ch;                // >= 1 always
        y -= f / fd;
        y = rack::clamp(y, -8.f, 8.f);
    }
    return std::isfinite(y) ? y : 0.f;
}

struct ZdfMulti final : Module {
    enum ParamIds {
        // base values
        CUTOFF_PARAM, RESO_PARAM, DRIVE_PARAM, MODE_PARAM, SLOPE_PARAM,
        CHAR_PARAM, ASYM_PARAM, FBHP_PARAM, KEYTRK_PARAM, DIODE_PARAM,
        MIX_PARAM, LEVEL_PARAM, MODEB_PARAM, MORPH_PARAM, EXPAND_PARAM,
        // outer-ring CV depths, in the SAME order
        CUTOFF_CV_PARAM, RESO_CV_PARAM, DRIVE_CV_PARAM, MODE_CV_PARAM,
        SLOPE_CV_PARAM, CHAR_CV_PARAM, ASYM_CV_PARAM, FBHP_CV_PARAM,
        KEYTRK_CV_PARAM, MIX_CV_PARAM, LEVEL_CV_PARAM, MODEB_CV_PARAM,
        MORPH_CV_PARAM, EXPAND_CV_PARAM,
        // One inert readout param per control unit.  A param can own only one
        // panel element, so the segment display needs a key of its own.
        READOUT_BASE,
        NUM_PARAMS = READOUT_BASE + 14
    };
    enum InputIds {
        AUDIO_INPUT, VOCT_INPUT,
        CUTOFF_INPUT, RESO_INPUT, DRIVE_INPUT, MODE_INPUT, SLOPE_INPUT,
        CHAR_INPUT, ASYM_INPUT, FBHP_INPUT, KEYTRK_INPUT, MIX_INPUT,
        LEVEL_INPUT, MODEB_INPUT, MORPH_INPUT, EXPAND_INPUT,
        NUM_INPUTS
    };
    enum OutputIds { MAIN_OUTPUT, LP_OUTPUT, HP_OUTPUT, BP_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { RESO_LIGHT, DRIVE_LIGHT, NUM_LIGHTS };

    // Per-voice ladder state (4 integrators) + feedback highpass state.
    float s1[kMaxVoices] = {}, s2[kMaxVoices] = {}, s3[kMaxVoices] = {}, s4[kMaxVoices] = {};
    float fbHpX[kMaxVoices] = {}, fbHpY[kMaxVoices] = {};
    float lastY4[kMaxVoices] = {};
    float diodeSeed[kMaxVoices] = {};   // Newton warm start, per voice
    float env[kMaxVoices] = {};         // expander envelope follower

    ZdfMulti() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(CUTOFF_PARAM, 0.f, 1.f, 0.7f, "Cutoff");
        configParam(RESO_PARAM,   0.f, 1.f, 0.2f, "Resonance");
        configParam(DRIVE_PARAM,  0.f, 1.f, 0.f,  "Drive");
        {   // Name every mode position, so a readout can show "LP 24" rather
            // than an index, and so MODE reads as a switch not a number.
            std::vector<std::string> modeNames;
            for (int i = 0; i < kNumModes; ++i) modeNames.push_back(kModes[i].name);
            configSwitch(MODE_PARAM,  0.f, (float)(kNumModes - 1), 3.f, "Mode A", modeNames);
            configSwitch(MODEB_PARAM, 0.f, (float)(kNumModes - 1), 7.f, "Mode B", modeNames);
        }
        configParam(MORPH_PARAM,  0.f, 1.f, 0.f,  "A / B morph");
        configParam(SLOPE_PARAM,  0.f, 1.f, 1.f,  "Slope");
        configParam(CHAR_PARAM,  -1.f, 1.f, 0.f,  "Character");
        configParam(ASYM_PARAM,   0.f, 1.f, 0.f,  "Asymmetry");
        configParam(FBHP_PARAM,   0.f, 1.f, 0.f,  "Feedback HP");
        configParam(KEYTRK_PARAM, 0.f, 1.f, 0.f,  "Key track");
        configSwitch(DIODE_PARAM, 0.f, (float)(kNumDiodes - 1), 0.f, "Diode",
                     { "Si 1N4148", "Ge OA90", "LED red", "Schottky", "Si x2", "MOSFET" });
        configParam(MIX_PARAM,    0.f, 1.f, 1.f,  "Dry / wet");
        configParam(LEVEL_PARAM,  0.f, 2.f, 1.f,  "Level");
        configParam(EXPAND_PARAM, 0.f, 1.f, 0.35f, "Expand");
        // Outer rings: bipolar attenuverters, centre-detented at zero.
        const int cvIds[] = { CUTOFF_CV_PARAM, RESO_CV_PARAM, DRIVE_CV_PARAM,
                              MODE_CV_PARAM, SLOPE_CV_PARAM, CHAR_CV_PARAM,
                              ASYM_CV_PARAM, FBHP_CV_PARAM, KEYTRK_CV_PARAM,
                              MIX_CV_PARAM, LEVEL_CV_PARAM, MODEB_CV_PARAM,
                              MORPH_CV_PARAM, EXPAND_CV_PARAM };
        for (int id : cvIds) configParam(id, -1.f, 1.f, 0.f, "CV depth");
        configInput(AUDIO_INPUT, "In");
        configInput(VOCT_INPUT, "V/Oct");
        configInput(CUTOFF_INPUT, "Cutoff CV");  configInput(RESO_INPUT, "Reso CV");
        configInput(DRIVE_INPUT, "Drive CV");    configInput(MODE_INPUT, "Mode CV");
        configInput(SLOPE_INPUT, "Slope CV");    configInput(CHAR_INPUT, "Char CV");
        configInput(ASYM_INPUT, "Asym CV");      configInput(FBHP_INPUT, "FB HP CV");
        configInput(KEYTRK_INPUT, "Keytrk CV");  configInput(MIX_INPUT, "Mix CV");
        configInput(LEVEL_INPUT, "Level CV");    configInput(MODEB_INPUT, "Mode B CV");
        configInput(MORPH_INPUT, "Morph CV");
        configInput(EXPAND_INPUT, "Expand CV");
        configOutput(MAIN_OUTPUT, "Out");
        configOutput(LP_OUTPUT, "LP24");
        configOutput(HP_OUTPUT, "HP24");
        configOutput(BP_OUTPUT, "BP12");
        configLight(RESO_LIGHT, "Resonance");
        configLight(DRIVE_LIGHT, "Drive");
        configBypass(AUDIO_INPUT, MAIN_OUTPUT);
    }

    void onReset() override {
        for (int v = 0; v < kMaxVoices; ++v) {
            s1[v] = s2[v] = s3[v] = s4[v] = 0.f;
            fbHpX[v] = fbHpY[v] = 0.f; lastY4[v] = 0.f; diodeSeed[v] = 0.f;
            env[v] = 0.f;
        }
    }

    //! Knob + (outer ring depth) * jack.  A disconnected jack contributes
    //! nothing regardless of the ring, which is what makes the ring safe to
    //! leave parked anywhere.
    inline float cv(int baseId, int cvId, int inId, int voice, float lo, float hi) const {
        float v = params[baseId].getValue();
        const rack::engine::Input& in = inputs[inId];
        if (in.isConnected())
            v += params[cvId].getValue() * in.getPolyVoltage(voice) * 0.1f * (hi - lo);
        return rack::clamp(v, lo, hi);
    }

    //! Blend two mode vectors, then scale by SLOPE toward the gentler tap.
    void tapVector(float modeA, float modeB, float morph, float slope, float out[5]) const {
        const int ia = rack::clamp((int)std::lround(modeA), 0, kNumModes - 1);
        const int ib = rack::clamp((int)std::lround(modeB), 0, kNumModes - 1);
        const float t = rack::clamp(morph, 0.f, 1.f);
        for (int i = 0; i < 5; ++i)
            out[i] = kModes[ia].a[i] * (1.f - t) + kModes[ib].a[i] * t;
        // SLOPE pulls the vector toward its 1-pole ancestor, which is a
        // continuous dB/oct control rather than a switch between orders.
        const float sl = rack::clamp(slope, 0.f, 1.f);
        if (sl < 1.f) {
            const float gentle[5] = { out[0], out[1] + out[2] + out[3] + out[4], 0, 0, 0 };
            for (int i = 0; i < 5; ++i) out[i] = gentle[i] * (1.f - sl) + out[i] * sl;
        }
    }

    void process(const ProcessArgs& args) override {
        int voices = std::max(1, inputs[AUDIO_INPUT].getChannels());
        if (voices > kMaxVoices) voices = kMaxVoices;

        const float sr = args.sampleRate > 0.f ? args.sampleRate : 48000.f;
        const float nyq = sr * 0.45f;

        float mainOut[kMaxVoices] = {}, lpOut[kMaxVoices] = {};
        float hpOut[kMaxVoices] = {},  bpOut[kMaxVoices] = {};
        float resoLit = 0.f, driveLit = 0.f;

        for (int v = 0; v < voices; ++v) {
            // ---- per-voice controls ----------------------------------------
            const float cut01 = cv(CUTOFF_PARAM, CUTOFF_CV_PARAM, CUTOFF_INPUT, v, 0.f, 1.f);
            const float res01 = cv(RESO_PARAM,   RESO_CV_PARAM,   RESO_INPUT,   v, 0.f, 1.f);
            const float drv01 = cv(DRIVE_PARAM,  DRIVE_CV_PARAM,  DRIVE_INPUT,  v, 0.f, 1.f);
            const float chr   = cv(CHAR_PARAM,   CHAR_CV_PARAM,   CHAR_INPUT,   v, -1.f, 1.f);
            const float asym  = cv(ASYM_PARAM,   ASYM_CV_PARAM,   ASYM_INPUT,   v, 0.f, 1.f);
            const float fbhp  = cv(FBHP_PARAM,   FBHP_CV_PARAM,   FBHP_INPUT,   v, 0.f, 1.f);
            const float ktrk  = cv(KEYTRK_PARAM, KEYTRK_CV_PARAM, KEYTRK_INPUT, v, 0.f, 1.f);
            const float mix   = cv(MIX_PARAM,    MIX_CV_PARAM,    MIX_INPUT,    v, 0.f, 1.f);
            const float lvl   = cv(LEVEL_PARAM,  LEVEL_CV_PARAM,  LEVEL_INPUT,  v, 0.f, 2.f);
            const float slope = cv(SLOPE_PARAM,  SLOPE_CV_PARAM,  SLOPE_INPUT,  v, 0.f, 1.f);
            const float morph = cv(MORPH_PARAM,  MORPH_CV_PARAM,  MORPH_INPUT,  v, 0.f, 1.f);
            const float modeA = cv(MODE_PARAM,   MODE_CV_PARAM,   MODE_INPUT,   v,
                                   0.f, (float)(kNumModes - 1));
            const float modeB = cv(MODEB_PARAM,  MODEB_CV_PARAM,  MODEB_INPUT,  v,
                                   0.f, (float)(kNumModes - 1));

            // ---- cutoff: 16 Hz .. 0.45*rate, key-tracked -------------------
            float hz = 16.f * std::pow(1024.f, cut01);          // 16 Hz .. ~16 kHz
            if (ktrk > 0.f && inputs[VOCT_INPUT].isConnected())
                hz *= std::pow(2.f, inputs[VOCT_INPUT].getPolyVoltage(v) * ktrk);
            hz = rack::clamp(hz, 16.f, nyq);

            const float g = std::tan(kPi * hz / sr);
            const float G = g / (1.f + g);                       // in (0,1)
            const float G2 = G * G, G4 = G2 * G2;

            // Resonance up to self-oscillation, but k is bounded so the solve's
            // divisor (1 + k*G^4) can never approach zero.
            const float k = res01 * 4.2f;

            // ---- input stage ------------------------------------------------
            float x = inputs[AUDIO_INPUT].getPolyVoltage(v) * 0.2f;   // +/-5V -> +/-1
            const float grit = chr < 0.f ? -chr : 0.f;                // < 0 == gritty
            const float squelch = chr > 0.f ? chr : 0.f;              // > 0 == squelchy
            const int di = rack::clamp((int)std::lround(params[DIODE_PARAM].getValue()),
                                       0, kNumDiodes - 1);
            const float expand = cv(EXPAND_PARAM, EXPAND_CV_PARAM, EXPAND_INPUT,
                                    v, 0.f, 1.f);
            if (drv01 > 0.f || grit > 0.f) {
                const float gain = 1.f + (drv01 * 12.f) + grit * 6.f;
                const float driven = x * gain;
                x = diodeClip(driven, kDiodes[di].vt, kDiodes[di].k, diodeSeed[v]);
                diodeSeed[v] = x;                       // warm start the next sample

                // MAKEUP.  The diode's small-signal gain is 1/(1 + k/vt) -- for
                // silicon that is 1/8, about -18 dB, BEFORE it clips anything.
                // The old code multiplied by a further 1/(1+0.6*drive), so more
                // drive meant less level: the stage got quieter the harder you
                // pushed it.  Restore unity for small signals and let the diode
                // compress everything above that, which is what an overdrive
                // should do.
                x *= 1.f + kDiodes[di].k / kDiodes[di].vt;

                // EXPANDER.  Clipping squashes dynamics; a following expander is
                // what real overdrive front-ends use to hand the transients back.
                // Peak follower with a fast attack and a slow release, driving an
                // UPWARD gain (loud gets louder, quiet stays put).  Bounded by
                // construction: env <= 1 after the clip, so gain <= 1 + expand.
                const float rect = std::fabs(x);
                const float coef = rect > env[v] ? 0.30f : 0.0015f;   // ~fast / ~slow
                env[v] += (rect - env[v]) * coef;
                if (!std::isfinite(env[v])) env[v] = 0.f;
                const float e = env[v] > 1.f ? 1.f : env[v];
                x *= 1.f + expand * 2.f * e;

                x = softClip(x);                        // final bound, always
            }

            // ---- feedback path ----------------------------------------------
            float fb = lastY4[v];
            if (fbhp > 0.f) {
                // 303-style highpass INSIDE the loop: this is what stops a
                // high-resonance sweep booming at the low end.
                const float a = std::exp(-2.f * kPi * (20.f + fbhp * 400.f) / sr);
                const float y = 0.5f * (1.f + a) * (fb - fbHpX[v]) + a * fbHpY[v];
                fbHpX[v] = fb; fbHpY[v] = y;
                fb = fb * (1.f - fbhp) + y * fbhp;
            }
            // Nonlinearity as a BOUNDED CORRECTION on the linear solve, never
            // inside it: the divisor stays >= 1 whatever the saturator does.
            float fbCorr = 0.f;
            if (grit > 0.f || squelch > 0.f) {
                const float sat = squelch > 0.f
                    ? shapeAsym(fb, asym * squelch)
                    : diodeClip(fb * 1.5f, kDiodes[di].vt, kDiodes[di].k, fb);
                fbCorr = (sat - fb) * (grit + squelch);
            }

            // ---- exact ZDF solve --------------------------------------------
            const float S = (1.f - G) * (G2 * G * s1[v] + G2 * s2[v] + G * s3[v] + s4[v]);
            const float y4 = (G4 * (x - k * fbCorr) + S) / (1.f + k * G4);
            const float u  = x - k * (y4 + fbCorr);

            // forward pass, updating the integrators
            float in_i = u, y1, y2, y3, y4f;
            { const float vv = (in_i - s1[v]) * G; y1  = vv + s1[v]; s1[v] = y1 + vv; }
            { const float vv = (y1   - s2[v]) * G; y2  = vv + s2[v]; s2[v] = y2 + vv; }
            { const float vv = (y2   - s3[v]) * G; y3  = vv + s3[v]; s3[v] = y3 + vv; }
            { const float vv = (y3   - s4[v]) * G; y4f = vv + s4[v]; s4[v] = y4f + vv; }
            lastY4[v] = y4f;

            // ---- STABILITY GUARD --------------------------------------------
            // A denormal storm, a pathological CV or a bad host sample rate must
            // never propagate: reset this voice rather than emit a NaN.
            if (!std::isfinite(s1[v]) || !std::isfinite(s2[v]) ||
                !std::isfinite(s3[v]) || !std::isfinite(s4[v]) || !std::isfinite(y4f)) {
                s1[v] = s2[v] = s3[v] = s4[v] = 0.f;
                fbHpX[v] = fbHpY[v] = 0.f; lastY4[v] = 0.f;
                y1 = y2 = y3 = y4f = 0.f;
            }

            // ---- tap mix -----------------------------------------------------
            float a[5];
            tapVector(modeA, modeB, morph, slope, a);
            float wet = a[0] * u + a[1] * y1 + a[2] * y2 + a[3] * y3 + a[4] * y4f;
            if (squelch > 0.f) wet = shapeAsym(wet, asym * squelch * 0.5f);

            const float dry = x;
            float o = (dry * (1.f - mix) + wet * mix) * lvl;
            o = softClip(o);

            mainOut[v] = o * 5.f;
            lpOut[v]   = softClip(y4f * lvl) * 5.f;
            hpOut[v]   = softClip((u - 4.f * y1 + 6.f * y2 - 4.f * y3 + y4f) * lvl) * 5.f;
            bpOut[v]   = softClip((4.f * y2 - 8.f * y3 + 4.f * y4f) * lvl) * 5.f;

            resoLit  = std::max(resoLit, res01);
            driveLit = std::max(driveLit, drv01 + (chr < 0.f ? -chr : 0.f));
        }

        auto emit = [&](int id, const float* buf) {
            for (int v = 0; v < voices; ++v)
                outputs[id].setVoltage(rack::clamp(buf[v], -10.f, 10.f), v);
            outputs[id].setChannels(voices);
        };
        emit(MAIN_OUTPUT, mainOut);
        emit(LP_OUTPUT, lpOut);
        emit(HP_OUTPUT, hpOut);
        emit(BP_OUTPUT, bpOut);

        lights[RESO_LIGHT].setBrightnessRGB(resoLit, resoLit * 0.35f, 0.15f);
        lights[DRIVE_LIGHT].setBrightnessRGB(driveLit * 0.95f, driveLit * 0.55f, 0.1f);
    }
};

// ---------------------------------------------------------------------------
//  Panel
// ---------------------------------------------------------------------------
rackx::PanelElement el(int id, float x, float y, float r,
                       rackx::PanelControlStyle style, const std::string& label) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = r; v.style = style; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

//! One CONCENTRIC control + its jack, as a vertical unit.
int g_readout = 0;   // hands out the next inert readout key
//! `cellW` is the column pitch of the row this unit sits in.  Passing it lets
//! the kit shrink the readout and knob to fit: this panel has rows of 7 and 8
//! columns, i.e. ~65 px and ~57 px each, against a nominal 72 px readout -- so
//! without it every display overlapped its neighbours.
void cvKnob(rackx::PanelSpec& p, int baseId, int cvId, int inId,
            float x, float y, const std::string& label, float cellW = 0.f) {
    rackx::kit::addControl(p, baseId, cvId, inId,
                           ZdfMulti::READOUT_BASE + g_readout++, x, y, label,
                           cellW);
}

rackx::PanelElement section(float x, float y, float w, float h, const std::string& label) {
    rackx::PanelElement v;
    v.style = rackx::PanelControlStyle::Section;
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Above;
    return v;
}

rackx::PanelSpec zdfPanel() {
    g_readout = 0;
    using S = rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(34);   // a long module
    panel.height = 400.f;
    panel.headerHeight = 24.f;
    const float W = panel.width;

    auto colx  = [&](int n, int i) { return 28.f + (W - 56.f) / (float)n * ((float)i + 0.5f); };
    // The pitch each unit gets, handed to cvKnob so it can shrink to fit.
    auto cellw = [&](int n) { return (W - 56.f) / (float)n; };

    panel.decor.push_back(section(20.f, 44.f, W - 40.f, 112.f,
        "CORE    outer ring = CV depth, inner = value"));
    cvKnob(panel, ZdfMulti::CUTOFF_PARAM, ZdfMulti::CUTOFF_CV_PARAM,
           ZdfMulti::CUTOFF_INPUT, colx(7, 0), 86.f, "CUTOFF", cellw(7));
    cvKnob(panel, ZdfMulti::RESO_PARAM, ZdfMulti::RESO_CV_PARAM,
           ZdfMulti::RESO_INPUT, colx(7, 1), 86.f, "RESO", cellw(7));
    cvKnob(panel, ZdfMulti::DRIVE_PARAM, ZdfMulti::DRIVE_CV_PARAM,
           ZdfMulti::DRIVE_INPUT, colx(7, 2), 86.f, "DRIVE", cellw(7));
    cvKnob(panel, ZdfMulti::EXPAND_PARAM, ZdfMulti::EXPAND_CV_PARAM,
           ZdfMulti::EXPAND_INPUT, colx(7, 3), 86.f, "EXPAND", cellw(7));
    cvKnob(panel, ZdfMulti::CHAR_PARAM, ZdfMulti::CHAR_CV_PARAM,
           ZdfMulti::CHAR_INPUT, colx(7, 4), 86.f, "CHAR", cellw(7));
    cvKnob(panel, ZdfMulti::ASYM_PARAM, ZdfMulti::ASYM_CV_PARAM,
           ZdfMulti::ASYM_INPUT, colx(7, 5), 86.f, "ASYM", cellw(7));
    cvKnob(panel, ZdfMulti::FBHP_PARAM, ZdfMulti::FBHP_CV_PARAM,
           ZdfMulti::FBHP_INPUT, colx(7, 6), 86.f, "FB HP", cellw(7));

    panel.decor.push_back(section(20.f, 172.f, W - 40.f, 112.f,
        "SHAPE    MODE A/B = tap vectors, MORPH crossfades, DIODE = drive model"));
    cvKnob(panel, ZdfMulti::MODE_PARAM, ZdfMulti::MODE_CV_PARAM,
           ZdfMulti::MODE_INPUT, colx(8, 0), 214.f, "MODE A", cellw(8));
    cvKnob(panel, ZdfMulti::MODEB_PARAM, ZdfMulti::MODEB_CV_PARAM,
           ZdfMulti::MODEB_INPUT, colx(8, 1), 214.f, "MODE B", cellw(8));
    cvKnob(panel, ZdfMulti::MORPH_PARAM, ZdfMulti::MORPH_CV_PARAM,
           ZdfMulti::MORPH_INPUT, colx(8, 2), 214.f, "MORPH", cellw(8));
    cvKnob(panel, ZdfMulti::SLOPE_PARAM, ZdfMulti::SLOPE_CV_PARAM,
           ZdfMulti::SLOPE_INPUT, colx(8, 3), 214.f, "SLOPE", cellw(8));
    cvKnob(panel, ZdfMulti::KEYTRK_PARAM, ZdfMulti::KEYTRK_CV_PARAM,
           ZdfMulti::KEYTRK_INPUT, colx(8, 4), 214.f, "KEYTRK", cellw(8));
    cvKnob(panel, ZdfMulti::MIX_PARAM, ZdfMulti::MIX_CV_PARAM,
           ZdfMulti::MIX_INPUT, colx(8, 5), 214.f, "MIX", cellw(8));
    cvKnob(panel, ZdfMulti::LEVEL_PARAM, ZdfMulti::LEVEL_CV_PARAM,
           ZdfMulti::LEVEL_INPUT, colx(8, 6), 214.f, "LEVEL", cellw(8));

    // DIODE is a discrete choice, so it gets a readout box rather than a knob.
    // 0 Si 1N4148  1 Ge OA90  2 LED red  3 Schottky  4 Si x2  5 MOSFET
    rackx::PanelElement dsel = el(ZdfMulti::DIODE_PARAM, colx(8, 7), 214.f, 11.f,
                                  S::SegmentDisplay, "DIODE");
    // ~8 characters at the panel's mono cell, so a value like "Si 1N4148"
    // or "BP 12/12" lands whole instead of being clipped to nothing.
    dsel.width = 72.f; dsel.height = 22.f;
    panel.params.push_back(dsel);

    panel.decor.push_back(section(20.f, 300.f, W - 40.f, 76.f, "I / O"));
    const float jx = 46.f, jd = (W - 120.f) / 5.f;
    panel.inputs.push_back(el(ZdfMulti::AUDIO_INPUT, jx + jd * 0.f, 336.f, 9.f, S::Knob, "IN"));
    panel.inputs.push_back(el(ZdfMulti::VOCT_INPUT,  jx + jd * 1.f, 336.f, 9.f, S::Knob, "V/OCT"));
    panel.outputs.push_back(el(ZdfMulti::MAIN_OUTPUT, jx + jd * 2.f, 336.f, 9.f, S::Knob, "OUT"));
    panel.outputs.push_back(el(ZdfMulti::LP_OUTPUT,   jx + jd * 3.f, 336.f, 9.f, S::Knob, "LP24"));
    panel.outputs.push_back(el(ZdfMulti::HP_OUTPUT,   jx + jd * 4.f, 336.f, 9.f, S::Knob, "HP24"));
    panel.outputs.push_back(el(ZdfMulti::BP_OUTPUT,   jx + jd * 5.f, 336.f, 9.f, S::Knob, "BP12"));
    panel.lights.push_back(el(ZdfMulti::RESO_LIGHT,  W - 52.f, 336.f, 5.f, S::Lamp, "RES"));
    panel.lights.push_back(el(ZdfMulti::DRIVE_LIGHT, W - 30.f, 336.f, 5.f, S::Lamp, "DRV"));
    return panel;
}

} // namespace

namespace rackx {
void registerZdfMultiModule() {
    addType("ZDFX", "ZDF Multi", "Filter", Role::Normal,
            [] { return std::make_unique<ZdfMulti>(); }, zdfPanel());
}
} // namespace rackx
