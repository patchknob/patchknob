// Nonlinear zero-delay-feedback model of the TB-303 four-stage transistor
// ladder. The first stage uses the original circuit's unequal capacitor ratio;
// feedback is solved implicitly and the nonlinear core runs at 2x rate.
#include "rack_factory.h"

#include <algorithm>
#include <cmath>
#include <memory>
#if defined(__AVX__)
# include <immintrin.h>
#endif

namespace {

using rack::engine::Module;
constexpr int kVoices = rack::engine::PORT_MAX_CHANNELS;
constexpr float kPi = 3.14159265358979323846f;

struct Acid303Filter final : Module {
    enum ParamIds { CUTOFF_PARAM, RES_PARAM, DRIVE_PARAM, ENV_PARAM, NUM_PARAMS };
    enum InputIds { IN_INPUT, CUTOFF_INPUT, RES_INPUT, DRIVE_INPUT, ACCENT_INPUT, NUM_INPUTS };
    enum OutputIds { LP_OUTPUT, NUM_OUTPUTS };
    enum LightIds { NUM_LIGHTS };

    alignas(32) float state[4][kVoices] = {};
    alignas(32) float previousInput[kVoices] = {};

    Acid303Filter() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(CUTOFF_PARAM, 0.f, 1.f, 0.42f, "Cutoff");
        configParam(RES_PARAM, 0.f, 1.f, 0.35f, "Resonance");
        configParam(DRIVE_PARAM, 0.f, 1.f, 0.22f, "Drive");
        configParam(ENV_PARAM, 0.f, 1.f, 0.5f, "Envelope amount");
        configInput(IN_INPUT, "Audio");
        configInput(CUTOFF_INPUT, "Cutoff CV");
        configInput(RES_INPUT, "Resonance CV");
        configInput(DRIVE_INPUT, "Drive CV");
        configInput(ACCENT_INPUT, "Accent");
        configOutput(LP_OUTPUT, "303 LP");
    }

    float tick(float input, float cutoff, float resonance, float drive, int voice,
               float internalSampleRate) {
        // 303 capacitor ratio: the bottom 18 nF stage is faster than the three
        // 33 nF stages. Bilinear/TPT coefficients retain that asymmetry.
        const float g = std::tan(kPi * cutoff / internalSampleRate);
        const float stageScale[4] = { 33.f / 18.f, 1.f, 1.f, 1.f };
        float G[4];
        for (int stage = 0; stage < 4; ++stage) {
            const float gs = std::min(g * stageScale[stage], 12.f);
            G[stage] = gs / (1.f + gs);
        }

        const float k = 3.82f * resonance;
        const float driven = input * drive;
        float u = std::tanh(driven - k * state[3][voice]);
        // Newton solve through all four nonlinear stages. The state remains
        // fixed during iteration; only the converged solution advances it.
        for (int iteration = 0; iteration < 3; ++iteration) {
            float ladder = u;
            float ladderDerivative = 1.f;
            for (int stage = 0; stage < 4; ++stage) {
                const float linear = G[stage] * ladder + state[stage][voice];
                const float saturated = std::tanh(linear * 1.12f);
                ladder = saturated / 1.12f;
                ladderDerivative *= G[stage] * (1.f - saturated * saturated);
            }
            const float argument = driven - k * ladder;
            const float nonlinear = std::tanh(argument);
            const float derivative = 1.f + k * ladderDerivative
                                           * (1.f - nonlinear * nonlinear);
            u -= (u - nonlinear) / std::max(derivative, 1e-5f);
        }

        float value = u;
        for (int stage = 0; stage < 4; ++stage) {
            const float output = G[stage] * value + state[stage][voice];
            state[stage][voice] = 2.f * output - state[stage][voice];
            // Tiny stage saturation approximates finite transistor-pair swing
            // while leaving the ZDF solution dominated by the input pair.
            value = std::tanh(output * 1.12f) / 1.12f;
            if (!std::isfinite(state[stage][voice])) state[stage][voice] = 0.f;
        }
        return value;
    }

#if defined(__AVX__)
    static __m256 tanh8(__m256 value) {
        const __m256 limit = _mm256_set1_ps(3.f);
        value = _mm256_min_ps(limit, _mm256_max_ps(_mm256_sub_ps(_mm256_setzero_ps(), limit), value));
        const __m256 square = _mm256_mul_ps(value, value);
        return _mm256_div_ps(_mm256_mul_ps(value, _mm256_add_ps(_mm256_set1_ps(27.f), square)),
                             _mm256_add_ps(_mm256_set1_ps(27.f),
                                           _mm256_mul_ps(_mm256_set1_ps(9.f), square)));
    }

    __m256 tick8(__m256 input, const __m256 G[4], __m256 resonance,
                 __m256 drive, int baseVoice) {
        const __m256 one = _mm256_set1_ps(1.f);
        const __m256 k = _mm256_mul_ps(_mm256_set1_ps(3.82f), resonance);
        const __m256 driven = _mm256_mul_ps(input, drive);
        __m256 states[4];
        for (int stage = 0; stage < 4; ++stage)
            states[stage] = _mm256_loadu_ps(state[stage] + baseVoice);
        __m256 u = tanh8(_mm256_sub_ps(driven, _mm256_mul_ps(k, states[3])));
        for (int iteration = 0; iteration < 3; ++iteration) {
            __m256 ladder = u;
            __m256 ladderDerivative = one;
            for (int stage = 0; stage < 4; ++stage) {
                const __m256 linear = _mm256_add_ps(_mm256_mul_ps(G[stage], ladder), states[stage]);
                const __m256 saturated = tanh8(_mm256_mul_ps(linear, _mm256_set1_ps(1.12f)));
                ladder = _mm256_div_ps(saturated, _mm256_set1_ps(1.12f));
                ladderDerivative = _mm256_mul_ps(ladderDerivative,
                    _mm256_mul_ps(G[stage], _mm256_sub_ps(one,
                                           _mm256_mul_ps(saturated, saturated))));
            }
            const __m256 nonlinear = tanh8(_mm256_sub_ps(driven, _mm256_mul_ps(k, ladder)));
            __m256 derivative = _mm256_add_ps(one, _mm256_mul_ps(k,
                _mm256_mul_ps(ladderDerivative,
                              _mm256_sub_ps(one, _mm256_mul_ps(nonlinear, nonlinear)))));
            derivative = _mm256_max_ps(derivative, _mm256_set1_ps(1e-5f));
            u = _mm256_sub_ps(u, _mm256_div_ps(_mm256_sub_ps(u, nonlinear), derivative));
        }
        __m256 value = u;
        for (int stage = 0; stage < 4; ++stage) {
            const __m256 output = _mm256_add_ps(_mm256_mul_ps(G[stage], value), states[stage]);
            states[stage] = _mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(2.f), output), states[stage]);
            value = _mm256_div_ps(tanh8(_mm256_mul_ps(output, _mm256_set1_ps(1.12f))),
                                  _mm256_set1_ps(1.12f));
            _mm256_storeu_ps(state[stage] + baseVoice, states[stage]);
        }
        return value;
    }
#endif

    void process(const ProcessArgs& args) override {
        int voices = std::max(1, inputs[IN_INPUT].getChannels());
        voices = std::max(voices, inputs[CUTOFF_INPUT].getChannels());
        voices = std::max(voices, inputs[ACCENT_INPUT].getChannels());
        voices = std::min(voices, kVoices);
        const float internalRate = args.sampleRate * 2.f;

#if defined(__AVX__)
        alignas(32) float signal[kVoices] = {};
        alignas(32) float resonanceValues[kVoices] = {};
        alignas(32) float driveValues[kVoices] = {};
        alignas(32) float coefficients[4][kVoices] = {};
        alignas(32) float rendered[kVoices] = {};
        for (int voice = 0; voice < voices; ++voice) {
            signal[voice] = inputs[IN_INPUT].getPolyVoltage(voice) * 0.2f;
            const float accent = rack::clamp(inputs[ACCENT_INPUT].getPolyVoltage(voice) * 0.1f, 0.f, 1.f);
            float cutoffControl = params[CUTOFF_PARAM].getValue()
                                + inputs[CUTOFF_INPUT].getPolyVoltage(voice) * 0.1f
                                  * params[ENV_PARAM].getValue()
                                + accent * 0.12f;
            cutoffControl = rack::clamp(cutoffControl, 0.f, 1.f);
            const float cutoff = std::min(20.f * std::pow(900.f, cutoffControl),
                                          internalRate * 0.225f);
            resonanceValues[voice] = rack::clamp(params[RES_PARAM].getValue()
                    + inputs[RES_INPUT].getPolyVoltage(voice) * 0.1f + accent * 0.08f, 0.f, 1.f);
            const float driveControl = rack::clamp(params[DRIVE_PARAM].getValue()
                    + inputs[DRIVE_INPUT].getPolyVoltage(voice) * 0.1f + accent * 0.18f, 0.f, 1.f);
            driveValues[voice] = std::pow(10.f, driveControl * 24.f / 20.f);
            const float g = std::tan(kPi * cutoff / internalRate);
            const float scale[4] = { 33.f / 18.f, 1.f, 1.f, 1.f };
            for (int stage = 0; stage < 4; ++stage) {
                const float gs = std::min(g * scale[stage], 12.f);
                coefficients[stage][voice] = gs / (1.f + gs);
            }
        }
        for (int base = 0; base < voices; base += 8) {
            __m256 G[4];
            for (int stage = 0; stage < 4; ++stage)
                G[stage] = _mm256_loadu_ps(coefficients[stage] + base);
            const __m256 input = _mm256_loadu_ps(signal + base);
            const __m256 previous = _mm256_loadu_ps(previousInput + base);
            const __m256 first = _mm256_mul_ps(_mm256_add_ps(previous, input), _mm256_set1_ps(0.5f));
            const __m256 resonance = _mm256_loadu_ps(resonanceValues + base);
            const __m256 drive = _mm256_loadu_ps(driveValues + base);
            tick8(first, G, resonance, drive, base);
            __m256 output = tick8(input, G, resonance, drive, base);
            _mm256_storeu_ps(previousInput + base, input);
            output = _mm256_mul_ps(output, _mm256_mul_ps(_mm256_set1_ps(5.f),
                    _mm256_add_ps(_mm256_set1_ps(1.f),
                                  _mm256_mul_ps(resonance, _mm256_set1_ps(0.32f)))));
            output = _mm256_min_ps(_mm256_set1_ps(10.f),
                                   _mm256_max_ps(_mm256_set1_ps(-10.f), output));
            _mm256_storeu_ps(rendered + base, output);
        }
        for (int voice = 0; voice < voices; ++voice)
            outputs[LP_OUTPUT].setVoltage(rendered[voice], voice);
#else
        for (int voice = 0; voice < voices; ++voice) {
            const float input = inputs[IN_INPUT].getPolyVoltage(voice) * 0.2f;
            const float accent = rack::clamp(inputs[ACCENT_INPUT].getPolyVoltage(voice) * 0.1f, 0.f, 1.f);
            float cutoffControl = rack::clamp(params[CUTOFF_PARAM].getValue()
                + inputs[CUTOFF_INPUT].getPolyVoltage(voice) * 0.1f * params[ENV_PARAM].getValue()
                + accent * 0.12f, 0.f, 1.f);
            const float cutoff = std::min(20.f * std::pow(900.f, cutoffControl), internalRate * 0.225f);
            const float resonance = rack::clamp(params[RES_PARAM].getValue()
                + inputs[RES_INPUT].getPolyVoltage(voice) * 0.1f + accent * 0.08f, 0.f, 1.f);
            const float driveControl = rack::clamp(params[DRIVE_PARAM].getValue()
                + inputs[DRIVE_INPUT].getPolyVoltage(voice) * 0.1f + accent * 0.18f, 0.f, 1.f);
            const float drive = std::pow(10.f, driveControl * 24.f / 20.f);
            tick(0.5f * (previousInput[voice] + input), cutoff, resonance, drive, voice, internalRate);
            float output = tick(input, cutoff, resonance, drive, voice, internalRate);
            previousInput[voice] = input;
            outputs[LP_OUTPUT].setVoltage(rack::clamp(output * 5.f * (1.f + resonance * 0.32f),
                                                       -10.f, 10.f), voice);
        }
#endif
        outputs[LP_OUTPUT].setChannels(voices);
    }
};

rackx::PanelElement control(int id, float x, float y, float radius,
                            rackx::PanelControlStyle style, const char* label) {
    rackx::PanelElement element;
    element.id = id; element.x = x; element.y = y; element.radius = radius;
    element.style = style; element.label = label;
    return element;
}

rackx::PanelSpec acidPanel() {
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(12);
    panel.params = {
        control(Acid303Filter::CUTOFF_PARAM, 48.f, 92.f, 20.f, rackx::PanelControlStyle::Knob, "CUTOFF"),
        control(Acid303Filter::RES_PARAM, 135.f, 92.f, 20.f, rackx::PanelControlStyle::Knob, "RESONANCE"),
        control(Acid303Filter::DRIVE_PARAM, 48.f, 177.f, 17.f, rackx::PanelControlStyle::Knob, "DRIVE"),
        control(Acid303Filter::ENV_PARAM, 135.f, 177.f, 17.f, rackx::PanelControlStyle::Knob, "ENV MOD")
    };
    panel.inputs = {
        control(Acid303Filter::IN_INPUT, 25.f, 278.f, 9.f, rackx::PanelControlStyle::Knob, "IN"),
        control(Acid303Filter::CUTOFF_INPUT, 58.f, 278.f, 9.f, rackx::PanelControlStyle::Knob, "CUT CV"),
        control(Acid303Filter::RES_INPUT, 91.f, 278.f, 9.f, rackx::PanelControlStyle::Knob, "RES CV"),
        control(Acid303Filter::DRIVE_INPUT, 124.f, 278.f, 9.f, rackx::PanelControlStyle::Knob, "DRV CV"),
        control(Acid303Filter::ACCENT_INPUT, 157.f, 278.f, 9.f, rackx::PanelControlStyle::Knob, "ACCENT")
    };
    panel.outputs = {
        control(Acid303Filter::LP_OUTPUT, 91.f, 346.f, 11.f, rackx::PanelControlStyle::Knob, "303 LP")
    };
    return panel;
}

} // namespace

namespace rackx {
void registerTb303FilterModule() {
    addType("Acid303-ZDF", "Acid 303 ZDF", "Filter", Role::Normal,
            [] { return std::make_unique<Acid303Filter>(); }, acidPanel());
}
} // namespace rackx
