//----------------------------------------------------------------------------
//  Native translations of Cardinal module DSP.  The original source remains
//  vendored under vendor/Cardinal; these implementations use our GUI-free Rack
//  API and PanelSpec metadata instead of Cardinal's SVG widget layer.
//----------------------------------------------------------------------------
#include "rack_factory.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

using rack::engine::Module;
using rackx::PanelControlStyle;
using rackx::PanelElement;
using rackx::PanelSpec;

PanelElement element(int id, float x, float y, float radius,
                     PanelControlStyle style, const char* label)
{
    return PanelElement{ id, x, y, radius, style, label };
}

struct BefacoAtte : Module {
    enum ParamIds {
        GAIN_A_PARAM, GAIN_B_PARAM, GAIN_C_PARAM, GAIN_D_PARAM,
        MODE_A_PARAM, MODE_B_PARAM, MODE_C_PARAM, MODE_D_PARAM,
        NUM_PARAMS
    };
    enum InputIds { A_INPUT, B_INPUT, C_INPUT, D_INPUT, NUM_INPUTS };
    enum OutputIds { A_OUTPUT, B_OUTPUT, C_OUTPUT, D_OUTPUT, NUM_OUTPUTS };
    enum LightIds { A_LIGHT, B_LIGHT, C_LIGHT, D_LIGHT, NUM_LIGHTS };

    BefacoAtte() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(GAIN_A_PARAM, 0.f, 1.f, 1.f, "Gain A");
        configParam(GAIN_B_PARAM, 0.f, 1.f, 1.f, "Gain B");
        configParam(GAIN_C_PARAM, 0.f, 1.f, 1.f, "Gain C");
        configParam(GAIN_D_PARAM, 0.f, 1.f, 1.f, "Gain D");
        configSwitch(MODE_A_PARAM, 0.f, 1.f, 1.f, "Mode A", {"Invert", "Attenuate"});
        configSwitch(MODE_B_PARAM, 0.f, 1.f, 1.f, "Mode B", {"Invert", "Attenuate"});
        configSwitch(MODE_C_PARAM, 0.f, 1.f, 1.f, "Mode C", {"Invert", "Attenuate"});
        configSwitch(MODE_D_PARAM, 0.f, 1.f, 1.f, "Mode D", {"Invert", "Attenuate"});
        configInput(A_INPUT, "A"); configInput(B_INPUT, "B");
        configInput(C_INPUT, "C"); configInput(D_INPUT, "D");
        configOutput(A_OUTPUT, "A"); configOutput(B_OUTPUT, "B");
        configOutput(C_OUTPUT, "C"); configOutput(D_OUTPUT, "D");
        configLight(A_LIGHT, "A"); configLight(B_LIGHT, "B");
        configLight(C_LIGHT, "C"); configLight(D_LIGHT, "D");
    }

    void process(const ProcessArgs&) override {
        float normal[rack::engine::PORT_MAX_CHANNELS];
        for (float& value : normal) value = 10.f;
        int normalChannels = 1;
        for (int section = 0; section < 4; ++section) {
            const int inputId = A_INPUT + section;
            const int outputId = A_OUTPUT + section;
            const int channels = inputs[inputId].isConnected()
                ? std::max(1, inputs[inputId].getChannels()) : normalChannels;
            const float polarity = params[MODE_A_PARAM + section].getValue() >= 0.5f ? 1.f : -1.f;
            const float gain = polarity * params[GAIN_A_PARAM + section].getValue();
            float sumSquares = 0.f;
            float firstOutput = 0.f;
            for (int channel = 0; channel < channels; ++channel) {
                const float normalVoltage = normal[normalChannels == 1 ? 0 : channel];
                const float input = inputs[inputId].getNormalPolyVoltage(normalVoltage, channel);
                const float output = input * gain;
                outputs[outputId].setVoltage(output, channel);
                normal[channel] = input;
                sumSquares += output * output;
                if (channel == 0) firstOutput = output;
            }
            outputs[outputId].setChannels(channels);
            normalChannels = channels;
            if (channels > 1)
                lights[A_LIGHT + section].setBrightnessRGB(0.f, 0.f,
                    std::min(1.f, std::sqrt(sumSquares / channels) / 10.f));
            else if (firstOutput < 0.f)
                lights[A_LIGHT + section].setBrightnessRGB(
                    std::min(1.f, -firstOutput / 10.f), 0.f, 0.f);
            else
                lights[A_LIGHT + section].setBrightnessRGB(0.f,
                    std::min(1.f, firstOutput / 10.f), 0.f);
        }
    }
};

struct BefacoABC : Module {
    enum ParamIds { B1_LEVEL_PARAM, C1_LEVEL_PARAM, B2_LEVEL_PARAM, C2_LEVEL_PARAM, NUM_PARAMS };
    enum InputIds { A1_INPUT, B1_INPUT, C1_INPUT, A2_INPUT, B2_INPUT, C2_INPUT, NUM_INPUTS };
    enum OutputIds { OUT1_OUTPUT, OUT2_OUTPUT, NUM_OUTPUTS };
    enum LightIds { OUT1_LIGHT, OUT2_LIGHT, NUM_LIGHTS };

    BefacoABC() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(B1_LEVEL_PARAM, -1.f, 1.f, 0.f, "B1 Level");
        configParam(C1_LEVEL_PARAM, -1.f, 1.f, 0.f, "C1 Level");
        configParam(B2_LEVEL_PARAM, -1.f, 1.f, 0.f, "B2 Level");
        configParam(C2_LEVEL_PARAM, -1.f, 1.f, 0.f, "C2 Level");
        configInput(A1_INPUT, "A1"); configInput(B1_INPUT, "B1"); configInput(C1_INPUT, "C1");
        configInput(A2_INPUT, "A2"); configInput(B2_INPUT, "B2"); configInput(C2_INPUT, "C2");
        configOutput(OUT1_OUTPUT, "Out 1"); configOutput(OUT2_OUTPUT, "Out 2");
        configLight(OUT1_LIGHT, "Out 1"); configLight(OUT2_LIGHT, "Out 2");
    }

    void process(const ProcessArgs&) override {
        processSection(B1_LEVEL_PARAM, C1_LEVEL_PARAM, A1_INPUT, B1_INPUT, C1_INPUT, OUT1_OUTPUT, OUT1_LIGHT);
        processSection(B2_LEVEL_PARAM, C2_LEVEL_PARAM, A2_INPUT, B2_INPUT, C2_INPUT, OUT2_OUTPUT, OUT2_LIGHT);
    }

    void processSection(int levelB, int levelC, int inputA, int inputB, int inputC,
                        int output, int light)
    {
        int channels = std::max(1, inputs[inputA].getChannels());
        channels = std::max(channels, inputs[inputB].getChannels());
        channels = std::max(channels, inputs[inputC].getChannels());
        const float gainB = 2.f * params[levelB].getValue();
        const float gainC = params[levelC].getValue();
        float sumSquares = 0.f;
        float firstOutput = 0.f;
        for (int channel = 0; channel < channels; ++channel) {
            const float value = inputs[inputA].getPolyVoltage(channel)
                * inputs[inputB].getNormalPolyVoltage(5.f, channel) * gainB / 5.f
                + inputs[inputC].getNormalPolyVoltage(10.f, channel) * gainC;
            const float clipped = rack::clamp(value, -10.f, 10.f);
            outputs[output].setVoltage(clipped, channel);
            sumSquares += clipped * clipped;
            if (channel == 0) firstOutput = clipped;
        }
        outputs[output].setChannels(channels);
        if (channels > 1)
            lights[light].setBrightnessRGB(0.f, 0.f,
                std::min(1.f, std::sqrt(sumSquares / channels) / 10.f));
        else if (firstOutput < 0.f)
            lights[light].setBrightnessRGB(std::min(1.f, -firstOutput / 10.f), 0.f, 0.f);
        else
            lights[light].setBrightnessRGB(0.f, std::min(1.f, firstOutput / 10.f), 0.f);
    }
};

PanelSpec attePanel()
{
    PanelSpec panel = PanelSpec::fromHp(4);
    panel.textureAsset = "Befaco/res/panels/Atte.svg";
    panel.params = {
        element(0, 37.f, 41.f, 10.f, PanelControlStyle::Knob, "A"),
        element(1, 37.f, 89.f, 10.f, PanelControlStyle::Knob, "B"),
        element(2, 37.f, 137.f, 10.f, PanelControlStyle::Knob, "C"),
        element(3, 37.f, 185.f, 10.f, PanelControlStyle::Knob, "D"),
        element(4, 8.f, 35.f, 6.f, PanelControlStyle::Switch, ""),
        element(5, 8.f, 83.f, 6.f, PanelControlStyle::Switch, ""),
        element(6, 8.f, 131.f, 6.f, PanelControlStyle::Switch, ""),
        element(7, 8.f, 179.f, 6.f, PanelControlStyle::Switch, "")
    };
    panel.inputs = {
        element(0, 15.f, 230.f, 7.f, PanelControlStyle::Knob, "A"),
        element(1, 15.f, 267.f, 7.f, PanelControlStyle::Knob, "B"),
        element(2, 15.f, 304.f, 7.f, PanelControlStyle::Knob, "C"),
        element(3, 15.f, 341.f, 7.f, PanelControlStyle::Knob, "D")
    };
    panel.outputs = {
        element(0, 45.f, 230.f, 7.f, PanelControlStyle::Knob, "A"),
        element(1, 45.f, 267.f, 7.f, PanelControlStyle::Knob, "B"),
        element(2, 45.f, 304.f, 7.f, PanelControlStyle::Knob, "C"),
        element(3, 45.f, 341.f, 7.f, PanelControlStyle::Knob, "D")
    };
    panel.lights = {
        element(0, 9.f, 62.f, 3.f, PanelControlStyle::Knob, ""),
        element(1, 9.f, 110.f, 3.f, PanelControlStyle::Knob, ""),
        element(2, 9.f, 158.f, 3.f, PanelControlStyle::Knob, ""),
        element(3, 9.f, 206.f, 3.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

PanelSpec abcPanel()
{
    PanelSpec panel = PanelSpec::fromHp(5);
    panel.textureAsset = "Befaco/res/panels/ABC.svg";
    panel.params = {
        element(0, 55.f, 47.f, 12.f, PanelControlStyle::Knob, "B1"),
        element(1, 55.f, 117.f, 12.f, PanelControlStyle::Knob, "C1"),
        element(2, 55.f, 214.f, 12.f, PanelControlStyle::Knob, "B2"),
        element(3, 55.f, 284.f, 12.f, PanelControlStyle::Knob, "C2")
    };
    panel.inputs = {
        element(0, 15.f, 38.f, 7.f, PanelControlStyle::Knob, "A1"),
        element(1, 15.f, 80.f, 7.f, PanelControlStyle::Knob, "B1"),
        element(2, 15.f, 122.f, 7.f, PanelControlStyle::Knob, "C1"),
        element(3, 15.f, 205.f, 7.f, PanelControlStyle::Knob, "A2"),
        element(4, 15.f, 247.f, 7.f, PanelControlStyle::Knob, "B2"),
        element(5, 15.f, 289.f, 7.f, PanelControlStyle::Knob, "C2")
    };
    panel.outputs = {
        element(0, 15.f, 164.f, 7.f, PanelControlStyle::Knob, "OUT 1"),
        element(1, 15.f, 331.f, 7.f, PanelControlStyle::Knob, "OUT 2")
    };
    panel.lights = {
        element(0, 45.f, 172.f, 5.f, PanelControlStyle::Knob, ""),
        element(1, 45.f, 339.f, 5.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

} // namespace

namespace rackx {

void registerCardinalModules()
{
    addType("Befaco.Atte", "ATTE", "Befaco / Utility", Role::Normal,
            [] { return std::make_unique<BefacoAtte>(); }, attePanel());
    addType("Befaco.ABC", "ABC", "Befaco / Utility", Role::Normal,
            [] { return std::make_unique<BefacoABC>(); }, abcPanel());
}

} // namespace rackx
