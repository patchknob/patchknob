//----------------------------------------------------------------------------
//  rack_vco4_sse.cpp -- VCO-4 SSE: four oscillators, fully voltage-controlled.
//
//  The same voice the removed AVX VCO-8 had (tune/fine, wave morph, pulse width, phase
//  warp, wavefolder, linear FM with self-feedback and cross-FM), but:
//
//    * FOUR oscillators, which is exactly one 128-bit SSE lane -- no splitting,
//      no remainder, one vector op per stage for the whole module.
//    * SSE2 only (floor and blend are open-coded rather than taken from SSE4.1),
//      so it needs no -mavx and runs where the AVX module cannot.
//    * EVERY knob has its own CV jack directly beneath it.
//    * ONE flat faceplate: four strips side by side, everything visible at
//      once.  No tabs -- you can see and reach the whole module at a glance.
//
//  CV sums with the knob at +/-5 V == full parameter range, the same convention
//  the rest of the rack uses.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "rack_panel_kit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#  include <emmintrin.h>
#  define PK_VCO4_SSE 1
#endif

namespace {

using rack::engine::Module;

constexpr int kOsc = 4;                 // exactly one SSE lane

//! knob + (outer-ring depth) * jack.  A disconnected jack contributes nothing
//! whatever the ring is set to, so the ring is always safe to leave parked.
inline float cvSum(float knob, float depth, const rack::engine::Input& in,
                   float lo, float hi, int voice) {
    float v = knob;
    if (in.isConnected()) v += depth * in.getPolyVoltage(voice) * 0.1f * (hi - lo);
    return rack::clamp(v, lo, hi);
}

#ifdef PK_VCO4_SSE
//! floor() for SSE2 (SSE4.1's _mm_floor_ps is not assumed).
inline __m128 sse_floor(__m128 x) {
    const __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(x));   // truncates toward 0
    const __m128 tooBig = _mm_cmpgt_ps(t, x);                // wrong way for negatives
    return _mm_sub_ps(t, _mm_and_ps(tooBig, _mm_set1_ps(1.f)));
}
//! mask ? a : b   (SSE2 form of _mm_blendv_ps)
inline __m128 sse_sel(__m128 mask, __m128 a, __m128 b) {
    return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}
#endif

inline float waveform(float p, int type) {
    if (type == 1) return 1.f - 4.f * std::fabs(p - 0.5f);   // triangle
    if (type == 2) return 2.f * p - 1.f;                     // saw
    if (type == 3) return p < 0.5f ? 1.f : -1.f;             // square
    return std::sin(6.28318530717958647692f * p);            // sine
}

struct VCO4SSE final : Module {
    enum ParamIds {
        TUNE_PARAM,
        FINE_PARAM     = TUNE_PARAM     + kOsc,
        TYPE_PARAM     = FINE_PARAM     + kOsc,
        PW_PARAM       = TYPE_PARAM     + kOsc,
        WARP_PARAM     = PW_PARAM       + kOsc,
        FOLD_PARAM     = WARP_PARAM     + kOsc,
        FM_PARAM       = FOLD_PARAM     + kOsc,
        FEEDBACK_PARAM = FM_PARAM       + kOsc,
        CROSS_PARAM    = FEEDBACK_PARAM + kOsc,
        LEVEL_PARAM    = CROSS_PARAM    + kOsc,
        // CV DEPTH rings for the concentric knobs, APPENDED so every saved
        // patch keeps its existing param indices (project_io stores params
        // positionally).  The depth groups mirror the base groups 1:1, so the
        // ring for base param p is simply p + CVDEPTH_BASE.
        CVDEPTH_BASE   = LEVEL_PARAM    + kOsc,
        // One inert readout key per control unit (see rack_panel_kit.h): a param
        // can own only one panel element, and the knob already owns it.
        READOUT_BASE   = CVDEPTH_BASE   + CVDEPTH_BASE,
        NUM_PARAMS     = READOUT_BASE   + CVDEPTH_BASE
    };
    enum InputIds {
        PITCH_INPUT,
        SYNC_INPUT     = PITCH_INPUT    + kOsc,
        TUNE_CV        = SYNC_INPUT     + kOsc,
        FINE_CV        = TUNE_CV        + kOsc,
        TYPE_CV        = FINE_CV        + kOsc,
        PW_CV          = TYPE_CV        + kOsc,
        WARP_CV        = PW_CV          + kOsc,
        FOLD_CV        = WARP_CV        + kOsc,
        FM_CV          = FOLD_CV        + kOsc,
        FEEDBACK_CV    = FM_CV          + kOsc,
        CROSS_CV       = FEEDBACK_CV    + kOsc,
        LEVEL_CV       = CROSS_CV       + kOsc,
        NUM_INPUTS     = LEVEL_CV       + kOsc
    };
    enum OutputIds { OSC_OUTPUT, MIX_OUTPUT = OSC_OUTPUT + kOsc, NUM_OUTPUTS };
    enum LightIds  { OSC_LIGHT, NUM_LIGHTS = OSC_LIGHT + kOsc };

    // Knob+CV is refreshed at CONTROL rate.  Reading ten ports per oscillator
    // every sample costs far more than the SSE maths it feeds, and a knob does
    // not need sample-accurate resolution.  Audio-rate FM and hard sync are the
    // exceptions and stay per-sample.
    static const int kCtrlDiv = 16;
    int ctrlCount = 0, cachedVoices = 0;
    // The phase increment is derived from pitch AND the sample rate.  Memoising
    // it on pitch alone left every voice running the OLD increment after a
    // device/rate change -- 48k -> 96k played an octave sharp forever, because
    // the pitch never "moved" so the cache never refreshed.  Key on the rate as
    // well, exactly as the stock VCO does with its own lastSr.
    float lastSr = -1.f;
    alignas(16) float cMorph[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    alignas(16) float cPW   [rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    alignas(16) float cWarp [rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    alignas(16) float cFold [rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    float cLevel  [rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    float cFmDepth[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    float cFb     [rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    float cCross  [rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    bool  fmConnected[kOsc] = {}, syncConnected[kOsc] = {};
    // Which shape/level jacks actually have a cable.  A patched jack is an
    // AUDIO-RATE signal and is re-read every sample (see process); an unpatched
    // one is just the knob, so it keeps the control-rate cache and costs nothing.
    bool  morphConnected[kOsc] = {}, pwConnected[kOsc] = {}, warpConnected[kOsc] = {},
          foldConnected[kOsc] = {}, levelConnected[kOsc] = {};
    bool  anyShapeCv = false;

    alignas(16) float phaseState[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    alignas(16) float incrementState[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    alignas(16) float lastOutputState[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    float lastPitchState[rack::engine::PORT_MAX_CHANNELS][kOsc];
    bool  syncHighState[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    // DC blocker on the summed modulation -- see where it is applied.
    float dcX[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};
    float dcY[rack::engine::PORT_MAX_CHANNELS][kOsc] = {};

    VCO4SSE() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (int i = 0; i < kOsc; ++i) {
            const std::string n = std::to_string(i + 1);
            configParam(TUNE_PARAM + i, -54.f, 54.f, 0.f, "Tune " + n, " semi");
            configParam(FINE_PARAM + i, -1.f, 1.f, 0.f, "Fine " + n, " semi");
            configParam(TYPE_PARAM + i, 0.f, 3.f, 0.f, "Wave morph " + n);
            configParam(PW_PARAM + i, 0.05f, 0.95f, 0.5f, "Pulse width " + n);
            configParam(WARP_PARAM + i, -1.f, 1.f, 0.f, "Phase warp " + n);
            configParam(FOLD_PARAM + i, 0.f, 1.f, 0.f, "Wavefold " + n);
            configParam(FM_PARAM + i, 0.f, 1.f, 0.f, "FM depth " + n);
            configParam(FEEDBACK_PARAM + i, 0.f, 1.f, 0.f, "Feedback FM " + n);
            configParam(CROSS_PARAM + i, -1.f, 1.f, 0.f, "Cross FM " + n);
            configParam(LEVEL_PARAM + i, 0.f, 1.f, 1.f, "Level " + n);
            configInput(PITCH_INPUT + i, "V/Oct " + n);
            configInput(SYNC_INPUT + i, "Sync " + n);
            configInput(TUNE_CV + i, "Tune CV " + n);
            configInput(FINE_CV + i, "Fine CV " + n);
            configInput(TYPE_CV + i, "Morph CV " + n);
            configInput(PW_CV + i, "PW CV " + n);
            configInput(WARP_CV + i, "Warp CV " + n);
            configInput(FOLD_CV + i, "Fold CV " + n);
            configInput(FM_CV + i, "FM " + n);
            configInput(FEEDBACK_CV + i, "Feedback CV " + n);
            configInput(CROSS_CV + i, "Cross CV " + n);
            configInput(LEVEL_CV + i, "Level CV " + n);
            configOutput(OSC_OUTPUT + i, "Osc " + n);
            configLight(OSC_LIGHT + i, "Osc " + n);
            for (int v = 0; v < rack::engine::PORT_MAX_CHANNELS; ++v)
                lastPitchState[v][i] = std::numeric_limits<float>::quiet_NaN();
        }
        // Outer rings: bipolar attenuverters, CENTRED (zero) by default, which
        // is the normal modular convention -- a patched jack does nothing until
        // you dial its ring in, so nothing moves behind your back.
        // NOTE: patches saved before the rings existed stored only CVDEPTH_BASE
        // params, so they load with every ring at 0 and their CV jacks silent
        // until turned up.  Deliberate: predictable beats surprising.
        for (int p = 0; p < CVDEPTH_BASE; ++p)
            configParam(CVDEPTH_BASE + p, -1.f, 1.f, 0.f, "CV depth");
        configOutput(MIX_OUTPUT, "Mix");
    }

    void onReset() override {
        for (int v = 0; v < rack::engine::PORT_MAX_CHANNELS; ++v)
            for (int i = 0; i < kOsc; ++i) {
                phaseState[v][i] = incrementState[v][i] = lastOutputState[v][i] = 0.f;
                dcX[v][i] = dcY[v][i] = 0.f;
                syncHighState[v][i] = false;
                lastPitchState[v][i] = std::numeric_limits<float>::quiet_NaN();
            }
        ctrlCount = 0; cachedVoices = 0; lastSr = -1.f;
    }

    void process(const ProcessArgs& args) override {
        // Nothing patched out?  Do no work, so idle instances cost nothing.
        bool anyOut = outputs[MIX_OUTPUT].isConnected();
        for (int i = 0; i < kOsc && !anyOut; ++i)
            anyOut = outputs[OSC_OUTPUT + i].isConnected();
        if (!anyOut) return;

        int voices = 1;
        for (int i = 0; i < kOsc; ++i) {
            voices = std::max(voices, inputs[PITCH_INPUT + i].getChannels());
            voices = std::max(voices, inputs[FM_CV + i].getChannels());
            voices = std::max(voices, inputs[SYNC_INPUT + i].getChannels());
        }
        voices = std::min(voices, rack::engine::PORT_MAX_CHANNELS);

        if (--ctrlCount <= 0 || voices != cachedVoices) {
            ctrlCount = kCtrlDiv;
            cachedVoices = voices;
            anyShapeCv = false;
            for (int i = 0; i < kOsc; ++i) {
                fmConnected[i]    = inputs[FM_CV + i].isConnected();
                syncConnected[i]  = inputs[SYNC_INPUT + i].isConnected();
                morphConnected[i] = inputs[TYPE_CV + i].isConnected();
                pwConnected[i]    = inputs[PW_CV + i].isConnected();
                warpConnected[i]  = inputs[WARP_CV + i].isConnected();
                foldConnected[i]  = inputs[FOLD_CV + i].isConnected();
                levelConnected[i] = inputs[LEVEL_CV + i].isConnected();
                anyShapeCv = anyShapeCv || morphConnected[i] || pwConnected[i]
                          || warpConnected[i] || foldConnected[i] || levelConnected[i];
            }
            for (int v = 0; v < voices; ++v)
                for (int i = 0; i < kOsc; ++i) {
                    const float tune = cvSum(params[TUNE_PARAM + i].getValue(), params[TUNE_PARAM + i + CVDEPTH_BASE].getValue(),
                                             inputs[TUNE_CV + i], -54.f, 54.f, v);
                    const float fine = cvSum(params[FINE_PARAM + i].getValue(), params[FINE_PARAM + i + CVDEPTH_BASE].getValue(),
                                             inputs[FINE_CV + i], -1.f, 1.f, v);
                    const float pitch = inputs[PITCH_INPUT + i].getPolyVoltage(v)
                                      + tune / 12.f + fine / 12.f;
                    if (pitch != lastPitchState[v][i] || args.sampleRate != lastSr) {
                        lastPitchState[v][i] = pitch;
                        const float hz = rack::clamp(rack::FREQ_C4 * std::exp2(pitch),
                                                     1.f, 20000.f);
                        incrementState[v][i] = std::min(hz * args.sampleTime, 0.49f);
                    }
                    cMorph[v][i]   = cvSum(params[TYPE_PARAM + i].getValue(), params[TYPE_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[TYPE_CV + i], 0.f, 3.f, v);
                    cPW[v][i]      = cvSum(params[PW_PARAM + i].getValue(), params[PW_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[PW_CV + i], 0.05f, 0.95f, v);
                    cWarp[v][i]    = cvSum(params[WARP_PARAM + i].getValue(), params[WARP_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[WARP_CV + i], -1.f, 1.f, v);
                    cFold[v][i]    = cvSum(params[FOLD_PARAM + i].getValue(), params[FOLD_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[FOLD_CV + i], 0.f, 1.f, v);
                    cLevel[v][i]   = cvSum(params[LEVEL_PARAM + i].getValue(), params[LEVEL_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[LEVEL_CV + i], 0.f, 1.f, v);
                    cFmDepth[v][i] = params[FM_PARAM + i].getValue();
                    cFb[v][i]      = cvSum(params[FEEDBACK_PARAM + i].getValue(), params[FEEDBACK_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[FEEDBACK_CV + i], 0.f, 1.f, v);
                    cCross[v][i]   = cvSum(params[CROSS_PARAM + i].getValue(), params[CROSS_PARAM + i + CVDEPTH_BASE].getValue(),
                                           inputs[CROSS_CV + i], -1.f, 1.f, v);
                }
            lastSr = args.sampleRate;
        }

        for (int voice = 0; voice < voices; ++voice) {
            float* phase      = phaseState[voice];
            float* increment  = incrementState[voice];
            float* lastOutput = lastOutputState[voice];
            bool*  syncHigh   = syncHighState[voice];
            float* dx = dcX[voice];
            float* dy = dcY[voice];
            // AUDIO-RATE CV.  The control-rate divider above is right for a
            // KNOB, but these five controls also have CV jacks, and an LFO (or
            // an envelope, or another oscillator) into one of them is a signal,
            // not a knob: holding it flat for 16 samples turns it into a 3 kHz
            // staircase -- audible zipper on modulated PWM, morph and level.
            // Re-read every PATCHED jack every sample, exactly as ZDFX does.
            // An unpatched control is just its knob, so it keeps the cached
            // value and an unmodulated instance costs what it always did.
            if (anyShapeCv) {
                for (int i = 0; i < kOsc; ++i) {
                    if (morphConnected[i])
                        cMorph[voice][i] = cvSum(params[TYPE_PARAM + i].getValue(), params[TYPE_PARAM + i + CVDEPTH_BASE].getValue(),
                                                 inputs[TYPE_CV + i], 0.f, 3.f, voice);
                    if (pwConnected[i])
                        cPW[voice][i]    = cvSum(params[PW_PARAM + i].getValue(), params[PW_PARAM + i + CVDEPTH_BASE].getValue(),
                                                 inputs[PW_CV + i], 0.05f, 0.95f, voice);
                    if (warpConnected[i])
                        cWarp[voice][i]  = cvSum(params[WARP_PARAM + i].getValue(), params[WARP_PARAM + i + CVDEPTH_BASE].getValue(),
                                                 inputs[WARP_CV + i], -1.f, 1.f, voice);
                    if (foldConnected[i])
                        cFold[voice][i]  = cvSum(params[FOLD_PARAM + i].getValue(), params[FOLD_PARAM + i + CVDEPTH_BASE].getValue(),
                                                 inputs[FOLD_CV + i], 0.f, 1.f, voice);
                    if (levelConnected[i])
                        cLevel[voice][i] = cvSum(params[LEVEL_PARAM + i].getValue(), params[LEVEL_PARAM + i + CVDEPTH_BASE].getValue(),
                                                 inputs[LEVEL_CV + i], 0.f, 1.f, voice);
                }
            }

            const float* morphs = cMorph[voice];
            const float* pws    = cPW[voice];
            const float* warps  = cWarp[voice];
            const float* folds  = cFold[voice];

            alignas(16) float steps[kOsc];
            for (int i = 0; i < kOsc; ++i) {
                float mod = 0.f;
                if (fmConnected[i])
                    mod += inputs[FM_CV + i].getPolyVoltage(voice) * 0.2f * cFmDepth[voice][i];
                mod += lastOutput[i] * 0.2f * cFb[voice][i];
                mod += lastOutput[(i + kOsc - 1) % kOsc] * 0.2f * cCross[voice][i];

                // PITCH STABILITY.  Two things make FM drift sharp with depth:
                //  1. DC in the modulator -- a folded, warped or narrow-pulse
                //     wave is not zero-mean, and that offset biases the phase
                //     increment.  One-pole blocker (~5 Hz) removes it.
                //  2. Clamping the step at 0 RECTIFIES the negative half of the
                //     modulation, raising the mean frequency.  A symmetric limit
                //     lets the excursions cancel, so the average increment stays
                //     exactly increment[i]; negative steps run the phase
                //     backwards, which is what through-zero FM is.
                dy[i] = mod - dx[i] + 0.9995f * dy[i];
                dx[i] = mod;
                steps[i] = rack::clamp(increment[i] * (1.f + dy[i]), -0.49f, 0.49f);

                if (syncConnected[i]) {
                    const bool high = inputs[SYNC_INPUT + i].getPolyVoltage(voice) >= 1.f;
                    if (high && !syncHigh[i]) phase[i] = 0.f;
                    syncHigh[i] = high;
                }
            }

            alignas(16) float values[kOsc];
#ifdef PK_VCO4_SSE
            // Four oscillators == one lane: every stage below is a single op
            // for the whole module, with no split or remainder handling.
            const __m128 one = _mm_set1_ps(1.f),  half = _mm_set1_ps(0.5f);
            const __m128 sign = _mm_set1_ps(-0.f), two = _mm_set1_ps(2.f);
            const __m128 four = _mm_set1_ps(4.f), quart = _mm_set1_ps(0.25f);

            __m128 p = _mm_add_ps(_mm_load_ps(phase), _mm_load_ps(steps));
            p = _mm_sub_ps(p, sse_floor(p));
            _mm_store_ps(phase, p);

            // Periodic phase distortion: both endpoints move by the same amount,
            // so the wrap stays continuous.
            __m128 wp = _mm_add_ps(p, _mm_mul_ps(_mm_load_ps(warps),
                        _mm_sub_ps(_mm_mul_ps(p, _mm_sub_ps(one, p)), quart)));
            wp = _mm_sub_ps(wp, sse_floor(wp));

            const __m128 centered = _mm_sub_ps(wp, half);
            const __m128 x = _mm_sub_ps(one, _mm_mul_ps(two, wp));
            __m128 sine = _mm_mul_ps(four,
                          _mm_mul_ps(x, _mm_sub_ps(one, _mm_andnot_ps(sign, x))));
            sine = _mm_add_ps(sine, _mm_mul_ps(_mm_set1_ps(0.225f),
                   _mm_sub_ps(_mm_mul_ps(sine, _mm_andnot_ps(sign, sine)), sine)));
            const __m128 tri = _mm_sub_ps(one,
                               _mm_mul_ps(four, _mm_andnot_ps(sign, centered)));
            const __m128 saw = _mm_sub_ps(_mm_mul_ps(two, wp), one);
            const __m128 square = sse_sel(_mm_cmpge_ps(wp, _mm_load_ps(pws)),
                                          _mm_set1_ps(-1.f), one);

            const __m128 morph = _mm_load_ps(morphs);
            const __m128 kind  = sse_floor(morph);
            const __m128 frac  = _mm_sub_ps(morph, kind);
            const __m128 ge05 = _mm_cmpge_ps(kind, _mm_set1_ps(0.5f));
            const __m128 ge15 = _mm_cmpge_ps(kind, _mm_set1_ps(1.5f));
            const __m128 ge25 = _mm_cmpge_ps(kind, _mm_set1_ps(2.5f));
            __m128 first  = sse_sel(ge25, square, sse_sel(ge15, saw, sse_sel(ge05, tri, sine)));
            __m128 second = sse_sel(ge15, square, sse_sel(ge05, saw, tri));
            __m128 out = _mm_add_ps(first, _mm_mul_ps(_mm_sub_ps(second, first), frac));

            // Symmetric triangle wavefolder, one pass to eight folds.
            const __m128 gain = _mm_add_ps(one,
                                _mm_mul_ps(_mm_load_ps(folds), _mm_set1_ps(7.f)));
            __m128 fp = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(out, gain), quart), quart);
            fp = _mm_sub_ps(fp, sse_floor(fp));
            out = _mm_sub_ps(one, _mm_mul_ps(four,
                  _mm_andnot_ps(sign, _mm_sub_ps(fp, half))));
            _mm_store_ps(values, _mm_mul_ps(out, _mm_set1_ps(5.f)));
#else
            for (int i = 0; i < kOsc; ++i) {
                phase[i] += steps[i];
                phase[i] -= std::floor(phase[i]);
                float warped = phase[i] + warps[i] * (phase[i] * (1.f - phase[i]) - 0.25f);
                warped -= std::floor(warped);
                const int kind = (int)std::floor(morphs[i]);
                const float frac = morphs[i] - kind;
                const float a = waveform(warped, kind);
                const float b = waveform(warped, std::min(kind + 1, 3));
                float raw = a + (b - a) * frac;
                float folded = raw * (1.f + 7.f * folds[i]) * 0.25f + 0.25f;
                folded -= std::floor(folded);
                values[i] = 5.f * (1.f - 4.f * std::fabs(folded - 0.5f));
            }
#endif
            float mix = 0.f;
            const float* levels = cLevel[voice];
            for (int i = 0; i < kOsc; ++i) {
                lastOutput[i] = values[i];
                mix += values[i] * levels[i];
                if (outputs[OSC_OUTPUT + i].isConnected())
                    outputs[OSC_OUTPUT + i].setVoltage(values[i], voice);
            }
            // sqrt(n), not n: four summed oscillators are not four times louder,
            // and dividing by 4 makes the mix useless with one patched.
            outputs[MIX_OUTPUT].setVoltage(rack::clamp(mix * 0.5f, -10.f, 10.f), voice);
        }

        for (int i = 0; i < kOsc; ++i) {
            outputs[OSC_OUTPUT + i].setChannels(voices);
            const float lit = rack::clamp(std::fabs(lastOutputState[0][i]) * 0.2f, 0.f, 1.f);
            lights[OSC_LIGHT + i].setBrightnessRGB(lit * 0.2f, lit * 0.85f, lit);
        }
        outputs[MIX_OUTPUT].setChannels(voices);
    }
};

//============================================================================
//  Faceplate -- four strips, everything on one page.
//============================================================================
rackx::PanelElement el(int id, float x, float y, float r,
                       rackx::PanelControlStyle style, const std::string& label) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = r;
    v.style = style; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

//! Section decor is drawn CENTRED on x,y; take top-left and convert.
rackx::PanelElement sect(float x, float y, float w, float h, const std::string& label) {
    rackx::PanelElement v;
    v.style = rackx::PanelControlStyle::Section;
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h;
    v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Above;
    return v;
}

rackx::PanelSpec vco4ssePanel() {
    using S = rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(44);
    panel.height = 700.f;
    panel.headerHeight = 24.f;
    const float W = panel.width;

    const float margin = 12.f;
    const float stripW = (W - margin * 2.f) / (float)kOsc;

    // Row geometry is DERIVED from the control unit, not hand-picked: the rows
    // used to be hard-coded at 76 px while a unit was ~98 px tall, so they
    // overlapped.  Ask the kit how tall a unit is at this scale and space the
    // rows by that plus a gutter, and the two can no longer drift apart.
    // FULL-SIZE knobs, jacks and segment displays.  The jack no longer draws a
    // caption of its own (the editor used to ignore labelPlacement::None and
    // print the port name beside it, duplicating the display underneath), so the
    // unit is knob -> jack -> name exactly once.
    //
    // A full unit is ~98 px tall and there are five rows, so the strip needs
    // ~500 px of control area.  The panel was 560 px tall with a 434 px strip --
    // not enough for five, which is what made the rows collide.  Grow the panel
    // instead of shrinking the controls.
    constexpr float kUnitScale = 1.0f;
    const float rowH  = rackx::kit::unitHeight(kUnitScale, true) + 6.f;
    const float row0  = 40.f + 26.f + rackx::kit::kKnobR;
    auto rowY = [&](int r) { return row0 + rowH * (float)r; };

    for (int i = 0; i < kOsc; ++i) {
        const float x0 = margin + stripW * (float)i;
        const float cL = x0 + stripW * 0.28f;      // left knob column
        const float cR = x0 + stripW * 0.72f;      // right knob column

        panel.decor.push_back(sect(x0 + 3.f, 40.f, stripW - 6.f, 580.f,
                                   "OSC " + std::to_string(i + 1)));

        // knob with its CV jack directly beneath it
        // Concentric: inner disc = the value, outer ring = how much of the CV
        // jack beneath it gets in.  One control instead of a knob+atten pair.
        // The house control unit: knob (inner value / outer CV depth), its
        // jack, then a segment display naming it -- see rack_panel_kit.h.
        auto pair = [&](int pid, int cvid, float x, float y,
                        const std::string& lab, float r = 15.f) {
            (void)r;
            // Two knob columns per oscillator strip, at 0.28 and 0.72 of its
            // width, so the space one unit owns is the 0.44*stripW between them.
            // Handing that to the kit keeps the readouts from running together
            // on a strip narrower than the nominal 72 px display.
            //
            // HALF SCALE.  Four oscillators x five stacked rows does not fit at
            // full size: a full unit is ~98 px tall against this panel's 76 px
            // row pitch, so every row overlapped the one below it.  kUnitScale
            // shrinks the whole unit together (knob, jack, readout and the gaps
            // between them) rather than just the knob, which would have left the
            // reading order looking wrong.
            rackx::kit::addControl(panel, pid, VCO4SSE::CVDEPTH_BASE + pid, cvid,
                                   VCO4SSE::READOUT_BASE + pid, x, y, lab,
                                   stripW * 0.44f, kUnitScale);
        };

        pair(VCO4SSE::TUNE_PARAM + i, VCO4SSE::TUNE_CV + i, cL, rowY(0), "TUNE", 15.f);
        pair(VCO4SSE::FINE_PARAM + i, VCO4SSE::FINE_CV + i, cR, rowY(0), "FINE");

        pair(VCO4SSE::TYPE_PARAM + i, VCO4SSE::TYPE_CV + i, cL, rowY(1), "MORPH");
        pair(VCO4SSE::PW_PARAM + i,   VCO4SSE::PW_CV + i,   cR, rowY(1), "PW");

        pair(VCO4SSE::WARP_PARAM + i, VCO4SSE::WARP_CV + i, cL, rowY(2), "WARP");
        pair(VCO4SSE::FOLD_PARAM + i, VCO4SSE::FOLD_CV + i, cR, rowY(2), "FOLD");

        pair(VCO4SSE::FM_PARAM + i,       VCO4SSE::FM_CV + i,       cL, rowY(3), "FM");
        pair(VCO4SSE::FEEDBACK_PARAM + i, VCO4SSE::FEEDBACK_CV + i, cR, rowY(3), "FBACK");

        pair(VCO4SSE::CROSS_PARAM + i, VCO4SSE::CROSS_CV + i, cL, rowY(4), "XFM");
        pair(VCO4SSE::LEVEL_PARAM + i, VCO4SSE::LEVEL_CV + i, cR, rowY(4), "LEVEL");

        // per-oscillator I/O along the bottom of its own strip
        panel.inputs.push_back(el(VCO4SSE::PITCH_INPUT + i, x0 + stripW * 0.22f,
                                  640.f, 8.f, S::Knob, "V/OCT"));
        panel.inputs.push_back(el(VCO4SSE::SYNC_INPUT + i, x0 + stripW * 0.5f,
                                  640.f, 8.f, S::Knob, "SYNC"));
        panel.outputs.push_back(el(VCO4SSE::OSC_OUTPUT + i, x0 + stripW * 0.78f,
                                   640.f, 8.f, S::Knob, "OUT"));
        panel.lights.push_back(el(VCO4SSE::OSC_LIGHT + i, x0 + stripW * 0.78f + 18.f,
                                  640.f, 4.f, S::Lamp, ""));
    }

    panel.outputs.push_back(el(VCO4SSE::MIX_OUTPUT, W * 0.5f, 676.f, 10.f, S::Knob, "MIX"));
    return panel;
}

} // namespace

namespace rackx {
void registerVco4SseModule() {
    addType("VCO-4-SSE", "VCO-4 SSE", "Oscillator", Role::Normal,
            [] { return std::make_unique<VCO4SSE>(); }, vco4ssePanel());
}
} // namespace rackx
