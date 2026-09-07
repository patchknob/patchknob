//----------------------------------------------------------------------------
//  src/engine/rack/rack_modules.cpp
//
//  The built-in VCV-Rack-style module set, implemented against the GUI-free
//  rack::engine API (rack.hpp / rack_dsp.h) and registered through the factory
//  (rack_factory.h).  Every module is data-driven: its constructor declares
//  params/inputs/outputs/lights (with short on-screen labels) and process()
//  does the DSP.  process() is allocation-free and lock-free -- all filter /
//  oscillator / RNG state lives in the struct.
//
//  Signal conventions (match VCV so modules interoperate):
//    * audio  = +/-5V,  CV = +/-10V,  gate/trigger = 0/10V
//    * pitch  = 1V/octave, 0V == middle C via rack::FREQ_C4
//
//  NOTE: VCO / VCF / VCA / ADSR / LFO / Mixer are NOT implemented here.  Full
//  duplicate implementations of all six used to sit in this file, ~400 lines
//  that nothing ever constructed: registerBuiltinModules() has always built
//  those slugs from fundamental::makeVCO() and friends (the Fundamental
//  bridge).  Dead copies of live DSP only drift from the real thing and give
//  the next reader the wrong source to fix, so they are gone.  Only AudioOut,
//  AudioIn, MidiToCV and Noise are implemented in this file; the panels for
//  every slug still live here.
//
//----------------------------------------------------------------------------

// rack_dsp.h references M_PI; MinGW/MSVC gate it behind _USE_MATH_DEFINES under
// a strict -std=c++17.  Request it before <cmath> is first pulled in.  Guarded
// so it never clashes with a build that already defines it on the command line.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include "rack.hpp"
#include "rack_dsp.h"
#include "rack_factory.h"
#include "fundamental_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace {

using rack::engine::Module;

static constexpr float kPi = 3.14159265358979323846f;

//============================================================================
//  I/O modules -- FIXED port layouts the engine relies on.  process() is
//  intentionally EMPTY: the engine reads/writes these ports directly to wire
//  the module to the node's stereo audio / MIDI.
//============================================================================

// Audio Out: engine reads inputs[L], inputs[R] as the node's stereo output.
struct AudioOut : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { INPUT_L, INPUT_R, NUM_INPUTS };
    enum OutputIds { NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    AudioOut() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(INPUT_L, "L");
        configInput(INPUT_R, "R");
    }
    void process(const ProcessArgs&) override {}
};

// Audio In: engine drives outputs[L], outputs[R] from the node's stereo input.
struct AudioIn : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { NUM_INPUTS };
    enum OutputIds { OUTPUT_L, OUTPUT_R, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    AudioIn() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configOutput(OUTPUT_L, "L");
        configOutput(OUTPUT_R, "R");
    }
    void process(const ProcessArgs&) override {}
};

// MIDI-CV: engine drives outputs[0..2] from the node's MIDI stream.
struct MidiToCV : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { NUM_INPUTS };
    enum OutputIds { PITCH_OUTPUT, GATE_OUTPUT, VEL_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    MidiToCV() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configOutput(PITCH_OUTPUT, "V/Oct");
        configOutput(GATE_OUTPUT,  "Gate");
        configOutput(VEL_OUTPUT,   "Vel");
    }
    void process(const ProcessArgs&) override {}
};

//============================================================================
//  Noise -- white (per-module xorshift32 RNG) + pink (Paul Kellet filter).
//============================================================================
struct Noise : Module {
    enum ParamIds  { NUM_PARAMS };
    enum InputIds  { NUM_INPUTS };
    enum OutputIds { WHITE_OUTPUT, PINK_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    uint32_t rngState;
    float pb0 = 0.f, pb1 = 0.f, pb2 = 0.f, pb3 = 0.f, pb4 = 0.f, pb5 = 0.f, pb6 = 0.f;

    Noise() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configOutput(WHITE_OUTPUT, "Wht");
        configOutput(PINK_OUTPUT, "Pnk");
        // Per-instance seed (decorrelates multiple Noise modules).  Never 0.
        rngState = 0x9E3779B9u ^ (uint32_t)(uintptr_t)this;
        if (rngState == 0u) rngState = 1u;
    }

    inline uint32_t nextRandom() {
        uint32_t x = rngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rngState = x;
        return x;
    }

    void process(const ProcessArgs&) override {
        // Uniform white in [-1, 1).
        float w = (float)(int32_t)nextRandom() * (1.f / 2147483648.f);

        // Paul Kellet's economy pink-noise filter.
        pb0 = 0.99886f * pb0 + w * 0.0555179f;
        pb1 = 0.99332f * pb1 + w * 0.0750759f;
        pb2 = 0.96900f * pb2 + w * 0.1538520f;
        pb3 = 0.86650f * pb3 + w * 0.3104856f;
        pb4 = 0.55000f * pb4 + w * 0.5329522f;
        pb5 = -0.7616f * pb5 - w * 0.0168980f;
        float pink = pb0 + pb1 + pb2 + pb3 + pb4 + pb5 + pb6 + w * 0.5362f;
        pb6 = w * 0.115926f;

        outputs[WHITE_OUTPUT].setVoltage(5.f * w);
        outputs[PINK_OUTPUT].setVoltage(rack::clamp(2.f * pink, -5.f, 5.f));
        outputs[WHITE_OUTPUT].channels = 1;
        outputs[PINK_OUTPUT].channels = 1;
    }
};

rackx::PanelElement panelElement(int id, float x, float y, float radius,
                                 rackx::PanelControlStyle style, const char* label)
{
    return rackx::PanelElement{ id, x, y, radius, style, label };
}

rackx::PanelSpec audioOutPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(4);
    panel.inputs = {
        panelElement(0, 30.f, 150.f, 9.f, PanelControlStyle::Knob, "L"),
        panelElement(1, 30.f, 240.f, 9.f, PanelControlStyle::Knob, "R")
    };
    return panel;
}

rackx::PanelSpec audioInPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(4);
    panel.outputs = {
        panelElement(0, 30.f, 150.f, 9.f, PanelControlStyle::Knob, "L"),
        panelElement(1, 30.f, 240.f, 9.f, PanelControlStyle::Knob, "R")
    };
    return panel;
}

rackx::PanelSpec midiCvPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(4);
    panel.outputs = {
        panelElement(0, 30.f, 125.f, 9.f, PanelControlStyle::Knob, "V/OCT"),
        panelElement(1, 30.f, 220.f, 9.f, PanelControlStyle::Knob, "GATE"),
        panelElement(2, 30.f, 315.f, 9.f, PanelControlStyle::Knob, "VEL")
    };
    return panel;
}

rackx::PanelSpec vcoPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(9);
    panel.textureAsset = "Fundamental/res/VCO.svg";
    panel.params = {
        panelElement(2, 36.f, 108.f, 20.f, PanelControlStyle::Knob, "FREQ"),
        panelElement(4, 36.f, 149.f, 9.f, PanelControlStyle::Knob, "FM"),
        panelElement(5, 99.f, 108.f, 20.f, PanelControlStyle::Knob, "PW"),
        panelElement(6, 99.f, 149.f, 9.f, PanelControlStyle::Knob, "PWM"),
        panelElement(7, 36.f, 231.f, 7.f, PanelControlStyle::Button, "LINEAR"),
        panelElement(1, 85.f, 231.f, 7.f, PanelControlStyle::Button, "SYNC")
    };
    panel.inputs = {
        panelElement(0, 67.f, 62.f, 9.f, PanelControlStyle::Knob, "V/OCT"),
        panelElement(1, 36.f, 187.f, 9.f, PanelControlStyle::Knob, "FM"),
        panelElement(2, 99.f, 231.f, 9.f, PanelControlStyle::Knob, "SYNC"),
        panelElement(3, 99.f, 187.f, 9.f, PanelControlStyle::Knob, "PWM")
    };
    panel.outputs = {
        panelElement(0, 36.f, 289.f, 9.f, PanelControlStyle::Knob, "SIN"),
        panelElement(1, 99.f, 289.f, 9.f, PanelControlStyle::Knob, "TRI"),
        panelElement(2, 36.f, 343.f, 9.f, PanelControlStyle::Knob, "SAW"),
        panelElement(3, 99.f, 343.f, 9.f, PanelControlStyle::Knob, "SQR")
    };
    panel.lights = {
        panelElement(3, 36.f, 231.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(4, 85.f, 231.f, 3.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

rackx::PanelSpec vcfPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(7);
    panel.textureAsset = "Fundamental/res/VCF.svg";
    panel.params = {
        panelElement(0, 26.f, 110.f, 20.f, PanelControlStyle::Knob, "FREQ"),
        panelElement(2, 79.f, 110.f, 20.f, PanelControlStyle::Knob, "RES"),
        panelElement(3, 26.f, 151.f, 9.f, PanelControlStyle::Knob, "FREQ CV"),
        panelElement(4, 53.f, 239.f, 20.f, PanelControlStyle::Knob, "DRIVE"),
        panelElement(5, 79.f, 151.f, 9.f, PanelControlStyle::Knob, "RES CV"),
        panelElement(6, 53.f, 263.f, 9.f, PanelControlStyle::Knob, "DRIVE CV")
    };
    panel.inputs = {
        panelElement(0, 26.f, 190.f, 9.f, PanelControlStyle::Knob, "FREQ CV"),
        panelElement(1, 79.f, 190.f, 9.f, PanelControlStyle::Knob, "RES CV"),
        panelElement(2, 53.f, 292.f, 9.f, PanelControlStyle::Knob, "DRIVE CV"),
        panelElement(3, 53.f, 62.f, 9.f, PanelControlStyle::Knob, "IN")
    };
    panel.outputs = {
        panelElement(0, 31.f, 343.f, 9.f, PanelControlStyle::Knob, "LP"),
        panelElement(1, 75.f, 343.f, 9.f, PanelControlStyle::Knob, "HP")
    };
    return panel;
}

rackx::PanelSpec vcaPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/VCA-1.svg";
    panel.params = { panelElement(0, 23.f, 172.f, 12.f, PanelControlStyle::Slider, "LEVEL") };
    panel.inputs = {
        panelElement(0, 23.f, 287.f, 9.f, PanelControlStyle::Knob, "CV"),
        panelElement(1, 23.f, 62.f, 9.f, PanelControlStyle::Knob, "IN")
    };
    panel.outputs = { panelElement(0, 23.f, 341.f, 9.f, PanelControlStyle::Knob, "OUT") };
    return panel;
}

rackx::PanelSpec adsrPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(9);
    panel.textureAsset = "Fundamental/res/ADSR.svg";
    panel.params = {
        panelElement(0, 21.f, 166.f, 14.f, PanelControlStyle::Knob, "ATT"),
        panelElement(1, 52.f, 166.f, 14.f, PanelControlStyle::Knob, "DEC"),
        panelElement(2, 83.f, 166.f, 14.f, PanelControlStyle::Knob, "SUS"),
        panelElement(3, 114.f, 166.f, 14.f, PanelControlStyle::Knob, "REL"),
        panelElement(4, 21.f, 223.f, 9.f, PanelControlStyle::Knob, "ATT CV"),
        panelElement(5, 52.f, 223.f, 9.f, PanelControlStyle::Knob, "DEC CV"),
        panelElement(6, 83.f, 223.f, 9.f, PanelControlStyle::Knob, "SUS CV"),
        panelElement(7, 114.f, 223.f, 9.f, PanelControlStyle::Knob, "REL CV"),
        panelElement(8, 68.f, 62.f, 7.f, PanelControlStyle::Button, "PUSH")
    };
    panel.inputs = {
        panelElement(0, 21.f, 249.f, 9.f, PanelControlStyle::Knob, "ATT CV"),
        panelElement(1, 52.f, 249.f, 9.f, PanelControlStyle::Knob, "DEC CV"),
        panelElement(2, 83.f, 249.f, 9.f, PanelControlStyle::Knob, "SUS CV"),
        panelElement(3, 114.f, 249.f, 9.f, PanelControlStyle::Knob, "REL CV"),
        panelElement(4, 21.f, 62.f, 9.f, PanelControlStyle::Knob, "GATE"),
        panelElement(5, 68.f, 62.f, 9.f, PanelControlStyle::Knob, "RETRIG")
    };
    panel.outputs = { panelElement(0, 68.f, 343.f, 9.f, PanelControlStyle::Knob, "ENV") };
    panel.lights = { panelElement(4, 114.f, 62.f, 3.f, PanelControlStyle::Knob, "") };
    return panel;
}

rackx::PanelSpec lfoPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(9);
    panel.textureAsset = "Fundamental/res/LFO.svg";
    panel.params = {
        panelElement(0, 68.f, 191.f, 7.f, PanelControlStyle::Button, "OFFSET"),
        panelElement(1, 68.f, 147.f, 7.f, PanelControlStyle::Button, "INVERT"),
        panelElement(2, 31.f, 88.f, 20.f, PanelControlStyle::Knob, "FREQ"),
        panelElement(3, 31.f, 129.f, 9.f, PanelControlStyle::Knob, "FM"),
        panelElement(5, 105.f, 88.f, 20.f, PanelControlStyle::Knob, "PW"),
        panelElement(6, 105.f, 129.f, 9.f, PanelControlStyle::Knob, "PWM")
    };
    panel.inputs = {
        panelElement(0, 31.f, 167.f, 9.f, PanelControlStyle::Knob, "FM"),
        panelElement(2, 105.f, 230.f, 9.f, PanelControlStyle::Knob, "RESET"),
        panelElement(3, 105.f, 167.f, 9.f, PanelControlStyle::Knob, "PWM"),
        panelElement(4, 31.f, 230.f, 9.f, PanelControlStyle::Knob, "CLOCK")
    };
    panel.outputs = {
        panelElement(0, 34.f, 289.f, 9.f, PanelControlStyle::Knob, "SIN"),
        panelElement(1, 102.f, 289.f, 9.f, PanelControlStyle::Knob, "TRI"),
        panelElement(2, 34.f, 343.f, 9.f, PanelControlStyle::Knob, "SAW"),
        panelElement(3, 102.f, 343.f, 9.f, PanelControlStyle::Knob, "SQR")
    };
    panel.lights = {
        panelElement(3, 68.f, 147.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(4, 68.f, 191.f, 3.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

rackx::PanelSpec noisePanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/Noise.svg";
    panel.outputs = {
        panelElement(0, 23.f, 285.f, 9.f, PanelControlStyle::Knob, "WHITE"),
        panelElement(1, 23.f, 341.f, 9.f, PanelControlStyle::Knob, "PINK")
    };
    return panel;
}

rackx::PanelSpec mixerPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/Mixer.svg";
    panel.params = { panelElement(0, 23.f, 290.f, 15.f, PanelControlStyle::Knob, "LEVEL") };
    panel.inputs = {
        panelElement(0, 23.f, 62.f, 9.f, PanelControlStyle::Knob, "1"),
        panelElement(1, 23.f, 94.f, 9.f, PanelControlStyle::Knob, "2"),
        panelElement(2, 23.f, 126.f, 9.f, PanelControlStyle::Knob, "3"),
        panelElement(3, 23.f, 158.f, 9.f, PanelControlStyle::Knob, "4"),
        panelElement(4, 23.f, 190.f, 9.f, PanelControlStyle::Knob, "5"),
        panelElement(5, 23.f, 222.f, 9.f, PanelControlStyle::Knob, "6")
    };
    panel.outputs = { panelElement(0, 23.f, 343.f, 9.f, PanelControlStyle::Knob, "MIX") };
    return panel;
}

rackx::PanelSpec eightVertPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(8);
    panel.textureAsset = "Fundamental/res/8vert.svg";
    for (int index = 0; index < 8; ++index) {
        const float y = 71.f + 38.f * index;
        panel.params.push_back(panelElement(index, 60.f, y, 15.f, PanelControlStyle::Knob, ""));
        panel.inputs.push_back(panelElement(index, 21.f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.outputs.push_back(panelElement(index, 99.f, y, 9.f, PanelControlStyle::Knob, ""));
    }
    return panel;
}

rackx::PanelSpec mergePanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(5);
    panel.textureAsset = "Fundamental/res/Merge.svg";
    for (int row = 0; row < 8; ++row) {
        const float y = 76.f + 29.f * row;
        panel.inputs.push_back(panelElement(row, 21.25f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.inputs.push_back(panelElement(row + 8, 53.75f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(row, 21.25f, y, 3.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(row + 8, 53.75f, y, 3.f, PanelControlStyle::Knob, ""));
    }
    panel.outputs.push_back(panelElement(0, 37.5f, 343.f, 9.f, PanelControlStyle::Knob, ""));
    return panel;
}

rackx::PanelSpec midSidePanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(5);
    panel.textureAsset = "Fundamental/res/MidSide.svg";
    panel.params = {
        panelElement(0, 23.375f, 83.f, 20.f, PanelControlStyle::Knob, ""),
        panelElement(1, 23.375f, 249.f, 20.f, PanelControlStyle::Knob, "")
    };
    panel.inputs = {
        panelElement(0, 60.125f, 83.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 21.25f, 138.5f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 53.75f, 138.5f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, 60.125f, 249.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(4, 21.25f, 304.5f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(5, 53.75f, 304.5f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.outputs = {
        panelElement(0, 21.25f, 177.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 53.75f, 177.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 21.25f, 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, 53.75f, 343.f, 9.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

rackx::PanelSpec octavePanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/Octave.svg";
    panel.params = { panelElement(0, 22.5f, 185.f, 13.f, PanelControlStyle::Slider, "") };
    panel.inputs = {
        panelElement(0, 22.5f, 61.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 22.5f, 101.f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.outputs = { panelElement(0, 22.5f, 343.f, 9.f, PanelControlStyle::Knob, "") };
    return panel;
}

rackx::PanelSpec splitPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(5);
    panel.textureAsset = "Fundamental/res/Split.svg";
    panel.inputs.push_back(panelElement(0, 37.5f, 69.f, 9.f, PanelControlStyle::Knob, ""));
    for (int row = 0; row < 8; ++row) {
        const float y = 133.f + 29.f * row;
        panel.outputs.push_back(panelElement(row, 21.25f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.outputs.push_back(panelElement(row + 8, 53.75f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(row, 21.25f, y, 3.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(row + 8, 53.75f, y, 3.f, PanelControlStyle::Knob, ""));
    }
    return panel;
}

rackx::PanelSpec sumPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/Sum.svg";
    panel.params = { panelElement(0, 22.5f, 290.f, 15.f, PanelControlStyle::Knob, "") };
    panel.inputs = { panelElement(0, 22.5f, 61.f, 9.f, PanelControlStyle::Knob, "") };
    panel.outputs = { panelElement(0, 22.5f, 344.f, 9.f, PanelControlStyle::Knob, "") };
    for (int index = 0; index < 6; ++index)
        panel.lights.push_back(panelElement(index, 22.5f, 144.f + 20.f * index, 3.f, PanelControlStyle::Knob, ""));
    return panel;
}

rackx::PanelSpec dualVcaPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(5);
    panel.textureAsset = "Fundamental/res/VCA.svg";
    panel.params = {
        panelElement(0, 37.5f, 125.5f, 17.f, PanelControlStyle::Knob, ""),
        panelElement(1, 37.5f, 297.5f, 17.f, PanelControlStyle::Knob, "")
    };
    panel.inputs = {
        panelElement(0, 18.f, 89.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 57.f, 89.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 37.5f, 62.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, 18.f, 261.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(4, 57.f, 261.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(5, 37.5f, 229.f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.outputs = {
        panelElement(0, 37.5f, 172.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 37.5f, 343.f, 9.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

rackx::PanelSpec vcMixerPanel()
{
    using rackx::PanelControlStyle;
    const float positions[] = {23.5f, 52.833f, 82.167f, 111.5f};
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(9);
    panel.textureAsset = "Fundamental/res/VCMixer.svg";
    for (int channel = 0; channel < 4; ++channel) {
        const float x = positions[channel];
        panel.params.push_back(panelElement(channel + 1, x, 122.f, 13.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(channel + 6, x, 155.5f, 9.f, PanelControlStyle::Knob, ""));
        panel.inputs.push_back(panelElement(channel + 1, x, 59.5f, 9.f, PanelControlStyle::Knob, ""));
        panel.inputs.push_back(panelElement(channel + 5, x, 183.f, 9.f, PanelControlStyle::Knob, ""));
        panel.outputs.push_back(panelElement(channel + 1, x, 343.f, 9.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(channel, x, 122.f, 3.f, PanelControlStyle::Knob, ""));
    }
    panel.params.push_back(panelElement(5, 61.74f, 236.f, 9.f, PanelControlStyle::Knob, ""));
    panel.params.push_back(panelElement(0, 106.f, 236.f, 20.f, PanelControlStyle::Knob, ""));
    panel.inputs.push_back(panelElement(0, 25.925f, 236.f, 9.f, PanelControlStyle::Knob, ""));
    panel.outputs.push_back(panelElement(0, 67.5f, 305.f, 9.f, PanelControlStyle::Knob, ""));
    return panel;
}

rackx::PanelSpec mutesPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(8);
    panel.textureAsset = "Fundamental/res/Mutes.svg";
    for (int index = 0; index < 10; ++index) {
        const float y = 68.f + 30.f * index;
        panel.inputs.push_back(panelElement(index, 19.663f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(index, 60.f, y, 7.f, PanelControlStyle::Button, ""));
        panel.outputs.push_back(panelElement(index, 96.663f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(index, 60.f, y, 3.f, PanelControlStyle::Knob, ""));
    }
    return panel;
}

rackx::PanelSpec pulsesPanel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(8);
    panel.textureAsset = "Fundamental/res/Pulses.svg";
    for (int index = 0; index < 10; ++index) {
        const float y = 68.f + 30.f * index;
        panel.params.push_back(panelElement(index, 19.663f, y, 7.f, PanelControlStyle::Button, ""));
        panel.outputs.push_back(panelElement(index, 60.f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.outputs.push_back(panelElement(index + 10, 96.663f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(index, 19.663f, y, 3.f, PanelControlStyle::Knob, ""));
    }
    return panel;
}

rackx::PanelSpec randomPanel()
{
    using rackx::PanelControlStyle;
    constexpr float left = 25.825f;
    constexpr float center = 67.5f;
    constexpr float right = 109.175f;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(9);
    panel.textureAsset = "Fundamental/res/Random.svg";
    panel.params = {
        panelElement(0, left, 117.f, 15.f, PanelControlStyle::Knob, ""),
        panelElement(1, right, 117.f, 15.f, PanelControlStyle::Knob, ""),
        panelElement(2, right, 62.f, 7.f, PanelControlStyle::Button, ""),
        panelElement(4, left, 224.f, 15.f, PanelControlStyle::Knob, ""),
        panelElement(5, center, 117.f, 15.f, PanelControlStyle::Knob, ""),
        panelElement(6, left, 150.5f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(7, right, 150.5f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(8, center, 224.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(9, center, 150.5f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.inputs = {
        panelElement(0, left, 177.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, right, 177.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, left, 62.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, center, 62.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(4, right, 224.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(5, center, 177.f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.outputs = {
        panelElement(0, left, 297.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, center, 297.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, left, 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, right, 297.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(4, right, 343.f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.lights = {
        panelElement(0, left, 117.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(1, right, 117.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(2, left, 224.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(3, center, 117.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(4, right, 62.f, 3.f, PanelControlStyle::Knob, "")
    };
    return panel;
}

rackx::PanelSpec seq3Panel()
{
    using rackx::PanelControlStyle;
    const float steps[] = {31.5f, 70.f, 108.5f, 147.f, 185.5f, 224.f, 262.5f, 301.f};
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(22);
    panel.textureAsset = "Fundamental/res/SEQ3.svg";
    panel.params = {
        panelElement(0, 38.5f, 86.f, 20.f, PanelControlStyle::Knob, ""),
        panelElement(36, 77.5f, 66.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, 128.5f, 86.f, 20.f, PanelControlStyle::Knob, ""),
        panelElement(37, 165.5f, 66.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 264.5f, 66.f, 7.f, PanelControlStyle::Button, ""),
        panelElement(2, 300.5f, 66.f, 7.f, PanelControlStyle::Button, "")
    };
    panel.inputs = {
        panelElement(0, 77.5f, 91.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 228.5f, 91.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 300.5f, 91.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, 165.5f, 91.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(4, 264.5f, 91.f, 9.f, PanelControlStyle::Knob, "")
    };
    for (int step = 0; step < 8; ++step) {
        const float x = steps[step];
        panel.params.push_back(panelElement(28 + step, x, 131.f, 7.f, PanelControlStyle::Button, ""));
        panel.lights.push_back(panelElement(3 + step, x, 131.f, 3.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(4 + step, x, 165.5f, 13.5f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(12 + step, x, 205.5f, 13.5f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(20 + step, x, 245.5f, 13.5f, PanelControlStyle::Knob, ""));
        panel.outputs.push_back(panelElement(4 + step, x, 301.f, 9.f, PanelControlStyle::Knob, ""));
    }
    panel.outputs.insert(panel.outputs.end(), {
        panelElement(12, steps[0], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(13, steps[1], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(14, steps[2], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(15, steps[3], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(0, steps[4], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, steps[5], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, steps[6], 343.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, steps[7], 343.f, 9.f, PanelControlStyle::Knob, "")
    });
    panel.lights.insert(panel.lights.end(), {
        panelElement(0, 228.5f, 66.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(1, 264.5f, 66.f, 3.f, PanelControlStyle::Knob, ""),
        panelElement(2, 300.5f, 66.f, 3.f, PanelControlStyle::Knob, "")
    });
    return panel;
}

rackx::PanelSpec sequentialSwitch1Panel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/SequentialSwitch1.svg";
    panel.params = { panelElement(0, 22.5f, 56.5f, 11.f, PanelControlStyle::Switch, "") };
    panel.inputs = {
        panelElement(0, 22.5f, 99.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 22.5f, 142.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 22.5f, 185.f, 9.f, PanelControlStyle::Knob, "")
    };
    for (int index = 0; index < 4; ++index) {
        const float y = 242.f + 31.f * index;
        panel.outputs.push_back(panelElement(index, 22.5f, y, 9.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(2 * index, 22.5f, y, 3.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(2 * index + 1, 22.5f, y, 3.f, PanelControlStyle::Knob, ""));
    }
    return panel;
}

rackx::PanelSpec sequentialSwitch2Panel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(3);
    panel.textureAsset = "Fundamental/res/SequentialSwitch2.svg";
    panel.params = { panelElement(0, 22.5f, 56.5f, 11.f, PanelControlStyle::Switch, "") };
    panel.inputs = {
        panelElement(0, 22.5f, 99.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 22.5f, 142.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 22.5f, 191.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(3, 22.5f, 224.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(4, 22.5f, 257.f, 9.f, PanelControlStyle::Knob, ""),
        panelElement(5, 22.5f, 290.f, 9.f, PanelControlStyle::Knob, "")
    };
    panel.outputs = { panelElement(0, 22.5f, 343.f, 9.f, PanelControlStyle::Knob, "") };
    for (int index = 0; index < 4; ++index) {
        const float y = 191.f + 33.f * index;
        panel.lights.push_back(panelElement(2 * index, 22.5f, y, 3.f, PanelControlStyle::Knob, ""));
        panel.lights.push_back(panelElement(2 * index + 1, 22.5f, y, 3.f, PanelControlStyle::Knob, ""));
    }
    return panel;
}

rackx::PanelSpec stable16Panel()
{
    using rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(39);
    panel.textureAsset = "GoodSheperd/res/Stable16.svg";
    const float rows[] = {64.f, 104.f, 144.f, 184.f, 224.f, 264.f, 304.f, 344.f};
    for (int row = 0; row < 8; ++row) {
        for (int step = 0; step < 16; ++step) {
            const int id = 3 + step + 16 * row;
            const float x = 20.f + 20.f * step;
            panel.params.push_back(panelElement(id, x, rows[row], 6.f, PanelControlStyle::Button, ""));
            panel.lights.push_back(panelElement(step + 16 * row, x, rows[row], 3.f, PanelControlStyle::Knob, ""));
        }
        panel.outputs.push_back(panelElement(1 + row, 372.f, rows[row], 9.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(147 + row, 345.f, rows[row], 7.f, PanelControlStyle::Button, ""));
        panel.lights.push_back(panelElement(131 + row, 345.f, rows[row], 3.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(131 + row, 412.f, rows[row], 11.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(139 + row, 452.f, rows[row], 11.f, PanelControlStyle::Knob, ""));
        panel.params.push_back(panelElement(155 + row, 524.f, rows[row], 6.f, PanelControlStyle::Button, ""));
        panel.params.push_back(panelElement(163 + row, 540.f, rows[row], 6.f, PanelControlStyle::Button, ""));
    }
    panel.params.insert(panel.params.end(), {
        panelElement(0, 492.f, rows[0], 14.f, PanelControlStyle::Knob, ""),
        panelElement(1, 492.f, rows[4], 7.f, PanelControlStyle::Button, ""),
        panelElement(2, 492.f, rows[5], 7.f, PanelControlStyle::Button, ""),
        panelElement(171, 492.f, rows[7], 7.f, PanelControlStyle::Switch, "")
    });
    panel.inputs = {
        panelElement(0, 492.f, rows[1], 9.f, PanelControlStyle::Knob, ""),
        panelElement(1, 492.f, rows[2], 9.f, PanelControlStyle::Knob, ""),
        panelElement(2, 492.f, rows[6], 9.f, PanelControlStyle::Knob, "")
    };
    panel.lights.insert(panel.lights.end(), {
        panelElement(128, 492.f, rows[4], 3.f, PanelControlStyle::Knob, ""),
        panelElement(129, 492.f, rows[5], 3.f, PanelControlStyle::Knob, ""),
        panelElement(130, 492.f, rows[3], 3.f, PanelControlStyle::Knob, "")
    });
    return panel;
}

} // anonymous namespace

//============================================================================
//  Registration -- one addType() per module (stable slug, role, category).
//============================================================================
namespace rackx {

void registerVco4SseModule();
void registerTb303FilterModule();
void registerAcidSequencerModule();
void registerAcidOscillatorModule();
void registerHarmonicForgeModule();
void registerCdpModules();
void registerEnv8Module();
void registerSamplerModule();
void registerBufferRetrigModule();
void registerZdfMultiModule();
void registerZPlaneModule();
void registerAcid303Module();

void registerBuiltinModules() {
    // I/O (engine-wired) modules.
    addType("AudioOut", "Audio Out", "I/O", Role::AudioOut,
            [] { return std::make_unique<AudioOut>(); }, audioOutPanel());
    addType("AudioIn", "Audio In", "I/O", Role::AudioIn,
            [] { return std::make_unique<AudioIn>(); }, audioInPanel());
    addType("MIDI-CV", "MIDI-CV", "I/O", Role::MidiCV,
            [] { return std::make_unique<MidiToCV>(); }, midiCvPanel());

    // Normal DSP modules.
    addType("VCO", "VCO", "Oscillator", Role::Normal,
            [] { return fundamental::makeVCO(); }, vcoPanel());
    registerVco4SseModule();
    registerEnv8Module();
    registerSamplerModule();
    registerBufferRetrigModule();
    registerZdfMultiModule();
    registerZPlaneModule();
    registerAcid303Module();
    registerTb303FilterModule();
    addType("VCF", "VCF", "Filter", Role::Normal,
            [] { return fundamental::makeVCF(); }, vcfPanel());
    addType("VCA", "VCA", "Amplifier", Role::Normal,
            [] { return fundamental::makeVCA1(); }, vcaPanel());
    addType("ADSR", "ADSR", "Envelope", Role::Normal,
            [] { return fundamental::makeADSR(); }, adsrPanel());
    addType("LFO", "LFO", "Modulation", Role::Normal,
            [] { return fundamental::makeLFO(); }, lfoPanel());
    addType("Noise", "Noise", "Source", Role::Normal,
            [] { return std::make_unique<Noise>(); }, noisePanel());
    addType("Mixer", "Mixer", "Mixer", Role::Normal,
            [] { return fundamental::makeMixer(); }, mixerPanel());
    addType("8vert", "8vert", "Utility", Role::Normal,
            [] { return fundamental::make8vert(); }, eightVertPanel());
    addType("Merge", "Merge", "Utility", Role::Normal,
            [] { return fundamental::makeMerge(); }, mergePanel());
    addType("MidSide", "Mid/Side", "Utility", Role::Normal,
            [] { return fundamental::makeMidSide(); }, midSidePanel());
    addType("Octave", "Octave", "Utility", Role::Normal,
            [] { return fundamental::makeOctave(); }, octavePanel());
    addType("Split", "Split", "Utility", Role::Normal,
            [] { return fundamental::makeSplit(); }, splitPanel());
    addType("Sum", "Sum", "Utility", Role::Normal,
            [] { return fundamental::makeSum(); }, sumPanel());
    addType("VCA2", "VCA-2", "Amplifier", Role::Normal,
            [] { return fundamental::makeVCA2(); }, dualVcaPanel());
    addType("VCMixer", "VCA Mix", "Mixer", Role::Normal,
            [] { return fundamental::makeVCMixer(); }, vcMixerPanel());
    addType("Mutes", "Mutes", "Utility", Role::Normal,
            [] { return fundamental::makeMutes(); }, mutesPanel());
    addType("Pulses", "Pulses", "Utility", Role::Normal,
            [] { return fundamental::makePulses(); }, pulsesPanel());
    addType("Random", "Random", "Random", Role::Normal,
            [] { return fundamental::makeRandom(); }, randomPanel());
    addType("SEQ3", "SEQ-3", "Sequencer", Role::Normal,
            [] { return fundamental::makeSEQ3(); }, seq3Panel());
    registerAcidSequencerModule();
    registerAcidOscillatorModule();
    registerHarmonicForgeModule();
    registerCdpModules();
    addType("SequentialSwitch1", "Sequential Switch 1→4", "Utility", Role::Normal,
            [] { return fundamental::makeSequentialSwitch1(); }, sequentialSwitch1Panel());
    addType("SequentialSwitch2", "Sequential Switch 4→1", "Utility", Role::Normal,
            [] { return fundamental::makeSequentialSwitch2(); }, sequentialSwitch2Panel());
    addType("GoodSheperd.Stable16", "Stable16", "GoodSheperd / Sequencer", Role::Normal,
            [] { return fundamental::makeStable16(); }, stable16Panel());
}

} // namespace rackx
