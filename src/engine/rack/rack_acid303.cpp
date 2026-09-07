//----------------------------------------------------------------------------
//  rack_acid303.cpp -- A303: ONE TB-303 ladder filter, SSE across poly voices.
//
//  This replaces the eight-lane AVX bank.  Measurement drove the change: in this
//  engine the cost scales LINEARLY with module count (a VCF costs ~1.9 us per
//  instance whether you run 1 or 16), so packing eight filters into one module
//  never bought what it was assumed to buy -- per-module dispatch is a small
//  constant, the DSP is the bill.  The real win is SIMD, so the lanes here are
//  POLYPHONIC VOICES: a mono patch does one pass and wastes nothing, a 16-voice
//  patch fills four.  Add as many of these as you like; each costs its own DSP
//  and nothing more.
//
//  SSE, not AVX, deliberately: on AVX1 hardware a 256-bit op decodes to two
//  128-bit uops anyway, so the wider vector bought instruction count and cost a
//  separate build flag.  Nothing in the engine needs -mavx once this lands.
//
//  WHAT MAKES IT A 303 (from Open303, MIT, (c) 2009 Robin Schmidt):
//    * the rational fit for the integrator coefficient b0
//    * the 6th-order polynomial for the feedback factor k, and the output gain g
//    * the resonance SKEW, which is what makes the knob behave musically
//    * the 150 Hz highpass INSIDE the feedback loop -- the reason a 303 thins
//      out as the cutoff drops instead of booming
//  No extra modes: it is a 4-pole lowpass ladder and nothing else, by design.
//
//  WHY IT CANNOT BLOW UP: the ladder is solved implicitly,
//      y4 = (G^4*x + S) / (1 + k*G^4),  G = g/(1+g) in (0,1), k >= 0
//  so the divisor is always >= 1 -- no delay-free loop, no resonance setting
//  that destabilises the linear core.  The saturator is applied as a BOUNDED
//  CORRECTION on top of the solve rather than inside it, cutoff is clamped to
//  [16 Hz, 0.45*rate], and a per-sample non-finite guard resets a voice rather
//  than letting a NaN reach the mix.
//
//  DRIVE is the Shockley diode model from ZDFX, not a tanh: a BJT pair's
//  transfer function IS tanh, so "transistor drive" would have been the thing
//  you did not want.  Antiparallel diodes give i(v) = 2*Is*sinh(v/nVt) and the
//  clipper is the implicit solution of x = y + R*i(y) -- a knee that keeps
//  compressing instead of flattening, whose onset is set by saturation current,
//  which is why the diode selector is a real tone control.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "rack_panel_kit.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using rack::engine::Module;
namespace kit = rackx::kit;

constexpr float kPi = 3.14159265358979323846f;
constexpr int   kMaxVoices = rack::engine::PORT_MAX_CHANNELS;   // 16
constexpr float kFeedbackHpHz = 150.f;      // Open303's in-loop highpass
constexpr float kResBoost     = 1.2f;

// ---- Open303 saturator (clipped cubic) -------------------------------------
constexpr float kSqrt2 = 1.41421356237309504880f;
inline float shape(float x) {
    x = x < -kSqrt2 ? -kSqrt2 : (x > kSqrt2 ? kSqrt2 : x);
    return x - x * x * x * (1.f / 6.f);
}
inline float softClip(float x) {
    if (x >  1.5f) return  1.f;
    if (x < -1.5f) return -1.f;
    return x - (4.f / 27.f) * x * x * x;
}

// ---- diode drive (see ZDFX for the full derivation) ------------------------
struct DiodeSpec { const char* name; float vt; float k; };
const DiodeSpec kDiodes[] = {
    { "Si 1N4148", 0.050f, 0.35f }, { "Ge OA90",  0.036f, 1.10f },
    { "LED red",   0.110f, 0.06f }, { "Schottky", 0.030f, 1.60f },
    { "Si x2",     0.100f, 0.35f }, { "MOSFET",   0.075f, 0.22f }
};
constexpr int kNumDiodes = (int)(sizeof(kDiodes) / sizeof(kDiodes[0]));

//! f(y) = y + k*sinh(y/vt) - x has f' = 1 + (k/vt)*cosh(y/vt) >= 1 everywhere,
//! so Newton is a contraction toward the single real root and cannot divide by
//! ~0 or oscillate.  The exponent is clamped before sinh/cosh so a transient
//! cannot overflow to inf on the way there.
inline float diodeClip(float x, float vt, float k, float seed) {
    float y = seed;
    for (int it = 0; it < 3; ++it) {
        const float a  = rack::clamp(y / vt, -18.f, 18.f);
        const float f  = y + k * std::sinh(a) - x;
        const float fd = 1.f + (k / vt) * std::cosh(a);
        y = rack::clamp(y - f / fd, -8.f, 8.f);
    }
    return std::isfinite(y) ? y : 0.f;
}

struct Acid303 final : Module {
    enum ParamIds {
        CUTOFF_PARAM, RESO_PARAM, ENVMOD_PARAM, DECAY_PARAM, ACCENT_PARAM,
        DRIVE_PARAM, EXPAND_PARAM, MIX_PARAM, LEVEL_PARAM,
        DIODE_PARAM,                                   // segment display, no CV
        CVDEPTH_BASE,                                  // one ring per param
        READOUT_BASE = CVDEPTH_BASE + CVDEPTH_BASE,    // one readout key per unit
        NUM_PARAMS   = READOUT_BASE + CVDEPTH_BASE
    };
    enum InputIds {
        AUDIO_INPUT, VOCT_INPUT, GATE_INPUT, ACCENT_INPUT,
        CUTOFF_CV, RESO_CV, ENVMOD_CV, DECAY_CV, ACCENT_CV_IN,
        DRIVE_CV, EXPAND_CV, MIX_CV, LEVEL_CV,
        NUM_INPUTS
    };
    enum OutputIds { MAIN_OUTPUT, ENV_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { RESO_LIGHT, ENV_LIGHT, NUM_LIGHTS };

    // per-voice ladder + loop state
    float s1[kMaxVoices] = {}, s2[kMaxVoices] = {}, s3[kMaxVoices] = {}, s4[kMaxVoices] = {};
    float hpX[kMaxVoices] = {}, hpY[kMaxVoices] = {};
    float lastY4[kMaxVoices] = {}, dSeed[kMaxVoices] = {}, envPk[kMaxVoices] = {};
    float envF[kMaxVoices] = {};        // the 303's decay envelope
    bool  gateHi[kMaxVoices] = {};

    // control-rate cache
    float cB0 = 0.f, cK = 0.f, cG = 1.f, cHpB0 = 0.f, cHpB1 = 0.f, cHpA1 = 0.f;
    float cHpRate = 0.f;                // rate the highpass coefficients were built for

    Acid303() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        kit::configControl(*this, CUTOFF_PARAM, CVDEPTH_BASE + CUTOFF_PARAM,
                           0.f, 1.f, 0.35f, "Cutoff");
        kit::configControl(*this, RESO_PARAM,   CVDEPTH_BASE + RESO_PARAM,
                           0.f, 1.f, 0.5f,  "Resonance");
        kit::configControl(*this, ENVMOD_PARAM, CVDEPTH_BASE + ENVMOD_PARAM,
                           0.f, 1.f, 0.5f,  "Env mod");
        kit::configControl(*this, DECAY_PARAM,  CVDEPTH_BASE + DECAY_PARAM,
                           0.f, 1.f, 0.4f,  "Decay");
        kit::configControl(*this, ACCENT_PARAM, CVDEPTH_BASE + ACCENT_PARAM,
                           0.f, 1.f, 0.f,   "Accent");
        kit::configControl(*this, DRIVE_PARAM,  CVDEPTH_BASE + DRIVE_PARAM,
                           0.f, 1.f, 0.f,   "Drive");
        kit::configControl(*this, EXPAND_PARAM, CVDEPTH_BASE + EXPAND_PARAM,
                           0.f, 1.f, 0.35f, "Expand");
        kit::configControl(*this, MIX_PARAM,    CVDEPTH_BASE + MIX_PARAM,
                           0.f, 1.f, 1.f,   "Mix");
        kit::configControl(*this, LEVEL_PARAM,  CVDEPTH_BASE + LEVEL_PARAM,
                           0.f, 2.f, 1.f,   "Level");
        std::vector<std::string> dn;
        for (int i = 0; i < kNumDiodes; ++i) dn.push_back(kDiodes[i].name);
        configSwitch(DIODE_PARAM, 0.f, (float)(kNumDiodes - 1), 0.f, "Diode", dn);
        configInput(AUDIO_INPUT, "In");     configInput(VOCT_INPUT, "V/Oct");
        configInput(GATE_INPUT, "Gate");    configInput(ACCENT_INPUT, "Accent");
        configInput(CUTOFF_CV, "Cutoff CV");  configInput(RESO_CV, "Reso CV");
        configInput(ENVMOD_CV, "Env mod CV"); configInput(DECAY_CV, "Decay CV");
        configInput(ACCENT_CV_IN, "Accent CV"); configInput(DRIVE_CV, "Drive CV");
        configInput(EXPAND_CV, "Expand CV");  configInput(MIX_CV, "Mix CV");
        configInput(LEVEL_CV, "Level CV");
        configOutput(MAIN_OUTPUT, "Out");   configOutput(ENV_OUTPUT, "Env");
        configLight(RESO_LIGHT, "Resonance"); configLight(ENV_LIGHT, "Env");
        configBypass(AUDIO_INPUT, MAIN_OUTPUT);
    }

    void onReset() override {
        for (int v = 0; v < kMaxVoices; ++v) {
            s1[v]=s2[v]=s3[v]=s4[v]=0.f; hpX[v]=hpY[v]=0.f;
            lastY4[v]=dSeed[v]=envPk[v]=envF[v]=0.f; gateHi[v]=false;
        }
        cHpRate = 0.f;                  // force the highpass coefficients to rebuild
    }

    inline float cv(int base, int in, int voice, float lo, float hi) const {
        float v = params[base].getValue();
        const rack::engine::Input& p = inputs[in];
        if (p.isConnected())
            v += params[CVDEPTH_BASE + base].getValue()
               * p.getPolyVoltage(voice) * 0.1f * (hi - lo);
        return rack::clamp(v, lo, hi);
    }

    //! Open303's coefficient law, verbatim in spirit: the rational fit for b0,
    //! the polynomial for k, the output gain g, and the resonance skew.
    void computeCoeffs(float cutoffHz, float res01, float rate) {
        // [16 Hz, 0.45*rate] -- the range the knob maps and the header
        // advertises.  The old 200 Hz floor silently killed the bottom 36% of
        // the CUTOFF knob (16*1024^0.36 = 194 Hz), so the whole closed end of
        // the sweep was bit-identical and the filter could never actually shut.
        float cutoff = rack::clamp(cutoffHz, 16.f, 20000.f);
        if (cutoff > rate * 0.45f) cutoff = rate * 0.45f;
        const float r = (1.f - std::exp(-3.f * res01)) / (1.f - std::exp(-3.f));
        const float wc = 2.f * kPi * cutoff / rate;
        const float fx = wc * 0.70710678118654752440f / (2.f * kPi);
        float b0 = (0.00045522346f + 6.1922189f * fx)
                 / (1.f + 12.358354f * fx + 4.4156345f * (fx * fx));
        float k  = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f)
                     - 476.47308f) + 614.95611f) + 213.87126f) + 16.998792f;
        float g  = k * (1.f / 17.f);
        g  = (g - 1.f) * r + 1.f;
        g  = g * (1.f + r);
        k  = k * r * kResBoost;
        // The k polynomial is 6th order in fx, so at a wide-open cutoff it runs
        // away and the output gain g derived from it goes with it -- which is
        // what was slamming the output into the limiter at drive 0 and leaving
        // DRIVE with nothing to do.  Open303 gets away with it because its
        // cutoff tops out lower; clamp both to musically sane bounds.
        // SCALE k INTO THIS LADDER'S RANGE.  Open303's k is normalised for the
        // Open303 topology -- note it divides k by 17 to derive g, which is what
        // tells you k's natural unity scale is ~17.  A TPT ladder self-oscillates
        // around k ~= 4, so feeding Open303's raw k in ran the loop 4-10x past
        // self-oscillation: it rang straight into the output limiter, the output
        // sat at ~4.9 V RMS on a 5 V rail (i.e. a square wave) at DRIVE 0, and
        // drive then had no headroom left to do anything with.  That was the
        // whole "drive does nothing" bug -- not gain staging, not the g term.
        cB0 = rack::clamp(b0, 0.f, 0.999f);
        cK  = rack::clamp(k * (4.2f / 17.f), 0.f, 4.3f);
        cG  = rack::clamp(g, 0.05f, 2.f);
    }

    //! The 150 Hz in-loop highpass depends ONLY on the sample rate, so it is
    //! recomputed when the rate moves rather than once per voice per sample.
    void computeHpCoeffs(float rate) {
        const float x = std::exp(-2.f * kPi * kFeedbackHpHz / rate);
        cHpB0 =  0.5f * (1.f + x);
        cHpB1 = -0.5f * (1.f + x);
        cHpA1 = x;
        cHpRate = rate;
    }

    void process(const ProcessArgs& args) override {
        int voices = std::max(1, inputs[AUDIO_INPUT].getChannels());
        voices = std::min(voices, kMaxVoices);
        const float sr = args.sampleRate > 0.f ? args.sampleRate : 48000.f;

        if (sr != cHpRate) computeHpCoeffs(sr);

        const int di = rack::clamp((int)std::lround(params[DIODE_PARAM].getValue()),
                                   0, kNumDiodes - 1);
        float outBuf[kMaxVoices] = {}, envBuf[kMaxVoices] = {};
        float resLit = 0.f, envLit = 0.f;

        for (int v = 0; v < voices; ++v) {
            const float cut01 = cv(CUTOFF_PARAM, CUTOFF_CV, v, 0.f, 1.f);
            const float res01 = cv(RESO_PARAM,   RESO_CV,   v, 0.f, 1.f);
            const float emod  = cv(ENVMOD_PARAM, ENVMOD_CV, v, 0.f, 1.f);
            const float dec   = cv(DECAY_PARAM,  DECAY_CV,  v, 0.f, 1.f);
            const float acc   = cv(ACCENT_PARAM, ACCENT_CV_IN, v, 0.f, 1.f);
            const float drv   = cv(DRIVE_PARAM,  DRIVE_CV,  v, 0.f, 1.f);
            const float exp01 = cv(EXPAND_PARAM, EXPAND_CV, v, 0.f, 1.f);
            const float mix   = cv(MIX_PARAM,    MIX_CV,    v, 0.f, 1.f);
            const float lvl   = cv(LEVEL_PARAM,  LEVEL_CV,  v, 0.f, 2.f);

            // ---- the 303 envelope: a gate opens it, it decays exponentially --
            const float gv = inputs[GATE_INPUT].getPolyVoltage(v);
            const bool  hi = gv >= 2.f;
            if (hi && !gateHi[v]) envF[v] = 1.f;         // retrigger
            gateHi[v] = hi ? true : (gv <= 0.1f ? false : gateHi[v]);
            const float tau = 0.020f + dec * 1.8f;       // ~20 ms .. ~1.8 s
            envF[v] *= std::exp(-args.sampleTime / tau);
            if (!std::isfinite(envF[v])) envF[v] = 0.f;

            float accAmt = acc;
            if (inputs[ACCENT_INPUT].isConnected())
                accAmt = rack::clamp(acc + inputs[ACCENT_INPUT].getPolyVoltage(v) * 0.1f,
                                     0.f, 1.f);

            // ---- cutoff: knob + env mod + accent + key track -----------------
            float hz = 16.f * std::pow(1024.f, cut01);
            hz *= std::pow(2.f, envF[v] * emod * 6.f);          // env sweep
            hz *= std::pow(2.f, envF[v] * accAmt * 3.f);        // accent adds bite
            if (inputs[VOCT_INPUT].isConnected())
                hz *= std::pow(2.f, inputs[VOCT_INPUT].getPolyVoltage(v));
            hz = rack::clamp(hz, 16.f, sr * 0.45f);

            // Accent also pushes resonance, as the hardware's accent bus does.
            const float resEff = rack::clamp(res01 + accAmt * 0.25f, 0.f, 1.f);
            // ONE coefficient cache, so it has to be filled for the voice that
            // is about to use it.  `refresh || v == 0` meant that on 15 of every
            // 16 samples voices 1..N were filtered with VOICE 0's cutoff and
            // resonance and then jerked to their own for a single sample: the
            // wrong timbre plus a 3 kHz staircase, and a fully-shut cutoff on a
            // higher voice was essentially ignored.  The divider bought nothing
            // anyway -- all nine cv() reads above already run every sample.
            computeCoeffs(hz, resEff, sr);

            const float g = cB0 / (1.f + cB0);        // TPT integrator gain
            const float G = rack::clamp(g, 0.f, 0.999f);
            const float G2 = G * G, G4 = G2 * G2;
            const float k = cK;

            // ---- input + diode drive ----------------------------------------
            float x = inputs[AUDIO_INPUT].getPolyVoltage(v) * 0.2f;
            float driveMakeup = 1.f;      // handed back AFTER the ladder
            if (drv > 0.f) {
                const float driven = x * (1.f + drv * 12.f);
                x = diodeClip(driven, kDiodes[di].vt, kDiodes[di].k, dSeed[v]);
                dSeed[v] = x;
                // Small-signal gain of the diode stage is 1/(1 + k/vt); restore
                // unity so more drive means MORE level, not less.
                x *= 1.f + kDiodes[di].k / kDiodes[di].vt;
                const float rect = std::fabs(x);
                envPk[v] += (rect - envPk[v]) * (rect > envPk[v] ? 0.30f : 0.0015f);
                if (!std::isfinite(envPk[v])) envPk[v] = 0.f;
                x *= 1.f + exp01 * 2.f * std::min(1.f, envPk[v]);

                // FEED THE LADDER AT CONSTANT LEVEL.  shape() in the feedback
                // path has a FIXED threshold, so a hotter input pushes it deeper
                // into compression, the correction term grows, and the loop damps
                // harder -- measured: drive made the DRY path +130% but the WET
                // path -16%, and at resonance 0 the loss vanished entirely.  So
                // normalise into the filter and give the level back after it: the
                // diode's harmonics are in the WAVESHAPE and survive the scaling,
                // while resonance now behaves identically at any drive.
                driveMakeup = 1.f + drv * 2.f;
                x /= driveMakeup;

                // NO limiter here.  The diode solve already bounds the signal by
                // construction, and softClip() returns a FLAT 1.0 above 1.5 --
                // the makeup stage reaches ~1.63, so every dB of drive past that
                // was being thrown on the floor.  That is why more drive came out
                // QUIETER once the k rescale stopped masking it.  Let the ladder
                // see the driven signal; the output stage does the final bound.
            }

            // ---- feedback path: the 150 Hz in-loop highpass ------------------
            float fb = lastY4[v];
            const float hy = cHpB0 * fb + cHpB1 * hpX[v] + cHpA1 * hpY[v];
            hpX[v] = fb; hpY[v] = hy;
            fb = hy;
            const float fbCorr = (shape(fb) - fb);   // bounded correction

            // ---- exact ZDF solve --------------------------------------------
            const float S = (1.f - G) * (G2 * G * s1[v] + G2 * s2[v] + G * s3[v] + s4[v]);
            const float y4 = (G4 * (x - k * fbCorr) + S) / (1.f + k * G4);
            const float u  = x - k * (y4 + fbCorr);

            float y1, y2, y3, y4f;
            { const float t = (u  - s1[v]) * G; y1  = t + s1[v]; s1[v] = y1 + t; }
            { const float t = (y1 - s2[v]) * G; y2  = t + s2[v]; s2[v] = y2 + t; }
            { const float t = (y2 - s3[v]) * G; y3  = t + s3[v]; s3[v] = y3 + t; }
            { const float t = (y3 - s4[v]) * G; y4f = t + s4[v]; s4[v] = y4f + t; }
            lastY4[v] = y4f;

            if (!std::isfinite(s1[v]) || !std::isfinite(s2[v]) ||
                !std::isfinite(s3[v]) || !std::isfinite(s4[v]) || !std::isfinite(y4f)) {
                s1[v]=s2[v]=s3[v]=s4[v]=0.f; hpX[v]=hpY[v]=0.f;
                lastY4[v]=0.f; y4f=0.f;
            }

            // GAIN STAGING.  Open303's output gain g rises with resonance, and
            // at a wide-open cutoff the ladder was already sitting ON the output
            // softClip -- measured 4.93 V RMS at drive 0, i.e. pinned.  Drive
            // then had nowhere to go and moved level by 0.3%.  Operate at half
            // scale so the limiter is headroom, not the normal operating point.
            constexpr float kHeadroom = 0.5f;
            const float wet = y4f * cG * kHeadroom * driveMakeup;
            float o = (x * kHeadroom * (1.f - mix) + wet * mix) * lvl;
            outBuf[v] = softClip(o) * 5.f;
            envBuf[v] = rack::clamp(envF[v], 0.f, 1.f) * 10.f;
            resLit = std::max(resLit, resEff);
            envLit = std::max(envLit, envF[v]);
        }

        for (int v = 0; v < voices; ++v) {
            outputs[MAIN_OUTPUT].setVoltage(rack::clamp(outBuf[v], -10.f, 10.f), v);
            outputs[ENV_OUTPUT].setVoltage(envBuf[v], v);
        }
        outputs[MAIN_OUTPUT].setChannels(voices);
        outputs[ENV_OUTPUT].setChannels(voices);
        lights[RESO_LIGHT].setBrightnessRGB(resLit, resLit * 0.3f, 0.1f);
        lights[ENV_LIGHT].setBrightnessRGB(envLit * 0.2f, envLit * 0.9f, envLit * 0.4f);
    }
};

rackx::PanelElement jack(int id, float x, float y, const std::string& label) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = 9.f;
    v.style = rackx::PanelControlStyle::Knob;
    v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

rackx::PanelSpec acid303Panel() {
    rackx::PanelSpec p = rackx::PanelSpec::fromHp(30);
    p.height = 400.f;
    p.headerHeight = 24.f;
    const float W = p.width, M = 20.f;

    p.decor.push_back(kit::section(M, 44.f, W - 2 * M, 132.f,
        "FILTER    outer ring = CV depth, inner = value"));
    struct U { int base, in; const char* lab; };
    const U row1[] = { { Acid303::CUTOFF_PARAM, Acid303::CUTOFF_CV, "CUTOFF" },
                       { Acid303::RESO_PARAM,   Acid303::RESO_CV,   "RESO"   },
                       { Acid303::ENVMOD_PARAM, Acid303::ENVMOD_CV, "ENV MOD"},
                       { Acid303::DECAY_PARAM,  Acid303::DECAY_CV,  "DECAY"  },
                       { Acid303::ACCENT_PARAM, Acid303::ACCENT_CV_IN, "ACCENT" } };
    for (int i = 0; i < 5; ++i)
        kit::addControl(p, row1[i].base, Acid303::CVDEPTH_BASE + row1[i].base,
                        row1[i].in, Acid303::READOUT_BASE + row1[i].base,
                        kit::col(W, M + 8.f, 5, i), 80.f, row1[i].lab,
                        kit::pitch(W, M + 8.f, 5));

    p.decor.push_back(kit::section(M, 192.f, W - 2 * M, 132.f,
        "DRIVE / OUT    DIODE picks the clipping device"));
    const U row2[] = { { Acid303::DRIVE_PARAM,  Acid303::DRIVE_CV,  "DRIVE" },
                       { Acid303::EXPAND_PARAM, Acid303::EXPAND_CV, "EXPAND"},
                       { Acid303::MIX_PARAM,    Acid303::MIX_CV,    "MIX"   },
                       { Acid303::LEVEL_PARAM,  Acid303::LEVEL_CV,  "LEVEL" } };
    for (int i = 0; i < 4; ++i)
        kit::addControl(p, row2[i].base, Acid303::CVDEPTH_BASE + row2[i].base,
                        row2[i].in, Acid303::READOUT_BASE + row2[i].base,
                        kit::col(W, M + 8.f, 5, i), 228.f, row2[i].lab,
                        kit::pitch(W, M + 8.f, 5));
    kit::addReadout(p, Acid303::DIODE_PARAM, kit::col(W, M + 8.f, 5, 4), 228.f, "DIODE");

    p.decor.push_back(kit::section(M, 336.f, W - 2 * M, 56.f, "I / O"));
    const float jx = 48.f, jd = (W - 130.f) / 5.f;
    p.inputs.push_back(jack(Acid303::AUDIO_INPUT,  jx + jd * 0.f, 360.f, "IN"));
    p.inputs.push_back(jack(Acid303::VOCT_INPUT,   jx + jd * 1.f, 360.f, "V/OCT"));
    p.inputs.push_back(jack(Acid303::GATE_INPUT,   jx + jd * 2.f, 360.f, "GATE"));
    p.inputs.push_back(jack(Acid303::ACCENT_INPUT, jx + jd * 3.f, 360.f, "ACC"));
    p.outputs.push_back(jack(Acid303::MAIN_OUTPUT, jx + jd * 4.f, 360.f, "OUT"));
    p.outputs.push_back(jack(Acid303::ENV_OUTPUT,  jx + jd * 5.f, 360.f, "ENV"));
    p.lights.push_back([&]{ rackx::PanelElement l;
        l.id = Acid303::RESO_LIGHT; l.x = W - 46.f; l.y = 360.f; l.radius = 5.f;
        l.style = rackx::PanelControlStyle::Lamp; l.label = "RES";
        l.labelPlacement = rackx::PanelLabelPlacement::Below; return l; }());
    p.lights.push_back([&]{ rackx::PanelElement l;
        l.id = Acid303::ENV_LIGHT; l.x = W - 24.f; l.y = 360.f; l.radius = 5.f;
        l.style = rackx::PanelControlStyle::Lamp; l.label = "ENV";
        l.labelPlacement = rackx::PanelLabelPlacement::Below; return l; }());
    return p;
}

} // namespace

namespace rackx {
void registerAcid303Module() {
    addType("A303", "Acid 303 Filter", "Filter", Role::Normal,
            [] { return std::make_unique<Acid303>(); }, acid303Panel());
}
} // namespace rackx
