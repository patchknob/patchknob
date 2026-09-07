//----------------------------------------------------------------------------
//  src/engine/rack/rack_acid_oscillator.cpp
//
//  "303 Oscillator" -- the bassline box's saw/square pair, ported from Open303
//  (https://github.com/RobinSchmidt/Open303) by Robin Schmidt, MIT licence:
//
//    Copyright (c) 2009 Robin Schmidt (www.rs-met.com)
//    Permission is hereby granted, free of charge, to any person obtaining a
//    copy of this software and associated documentation files (the "Software"),
//    to deal in the Software without restriction ... THE SOFTWARE IS PROVIDED
//    "AS IS", WITHOUT WARRANTY OF ANY KIND.  (full text: Open303 License.txt)
//
//  Ported from rosic::MipMappedWaveTable + rosic::BlendOscillator:
//
//    * the SAW prototype is a plain ramp (fillWithSaw303),
//    * the SQUARE prototype is that same ramp pushed through tanh with a large
//      drive (36.9 dB) and a DC offset of 4.37, then inverted and rotated 180
//      degrees so it phase-aligns with the saw when the two are mixed
//      (fillWithSquare303).  That shaped-ramp derivation -- rather than an ideal
//      pulse -- is what gives the hardware's "square" its lopsided character,
//      and it is why the two waveforms blend rather than merely crossfade.
//    * the square is scaled by 0.5 against the saw, as BlendOscillator does,
//    * mip level selection is Open303's: floor(log2(phase increment)) + 2, i.e.
//      band-limited to a quarter of Nyquist at the top note,
//    * lookup is linear interpolation between two adjacent table entries.
//
//  WHAT IS DIFFERENT: Open303 band-limits by FFT-ing the prototype, zeroing the
//  bins above each octave's cutoff and inverse-transforming.  Pulling in its
//  FFT for a table that is built once at startup is not worth it, so each mip
//  level is instead synthesised ADDITIVELY from the prototype's harmonic
//  amplitudes (obtained by one direct DFT).  Zeroing bins and re-synthesising
//  from the surviving bins are the same operation, so the tables match; only
//  the arithmetic used to get there differs.
//----------------------------------------------------------------------------
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include "rack.hpp"
#include "rack_dsp.h"
#include "rack_factory.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

using rack::engine::Module;
constexpr int V = rack::engine::PORT_MAX_CHANNELS;

constexpr int    kTableLen  = 2048;             // rosic::MipMappedWaveTable
constexpr int    kNumTables = 12;
constexpr double kPi        = 3.14159265358979323846;

// rosic::MipMappedWaveTable defaults for the 303 square.
constexpr double kTanhDriveDb = 36.9;
constexpr double kTanhOffset  = 4.37;

//----------------------------------------------------------------------------
//  Mip-mapped table set, built once and shared by every instance.
//----------------------------------------------------------------------------
struct WaveTable {
    // +1 guard sample per level so linear interpolation never reads past the end.
    std::vector<float> level[kNumTables];

    // Harmonics surviving at mip level t.  Open303 zeroes the packed spectrum
    // from bin tableLength/2^t upward, which keeps complex bins below
    // tableLength/2^(t+1) -- so harmonics 1 .. that-1.
    static int harmonicsFor(int t) {
        const int cutoff = kTableLen >> (t + 1);
        return cutoff > 1 ? cutoff - 1 : 0;
    }

    void build(const std::vector<double>& prototype) {
        // One direct DFT of the prototype -> per-harmonic sine/cosine amplitudes.
        const int H = kTableLen / 2;
        std::vector<double> re(H + 1, 0.0), im(H + 1, 0.0);
        for (int h = 1; h <= H; ++h) {
            double sr = 0.0, si = 0.0;
            const double w = 2.0 * kPi * h / kTableLen;
            for (int n = 0; n < kTableLen; ++n) {
                sr += prototype[(size_t)n] * std::cos(w * n);
                si -= prototype[(size_t)n] * std::sin(w * n);
            }
            re[(size_t)h] = 2.0 * sr / kTableLen;
            im[(size_t)h] = 2.0 * si / kTableLen;
        }

        for (int t = 0; t < kNumTables; ++t) {
            level[t].assign((size_t)kTableLen + 1, 0.f);
            if (t == 0) {
                // Level 0 is the prototype itself (Open303 copies it verbatim).
                for (int n = 0; n < kTableLen; ++n)
                    level[0][(size_t)n] = (float)prototype[(size_t)n];
            } else {
                const int nh = std::min(harmonicsFor(t), H);
                for (int n = 0; n < kTableLen; ++n) {
                    double acc = 0.0;
                    for (int h = 1; h <= nh; ++h) {
                        const double ph = 2.0 * kPi * h * n / kTableLen;
                        acc += re[(size_t)h] * std::cos(ph) - im[(size_t)h] * std::sin(ph);
                    }
                    level[t][(size_t)n] = (float)acc;
                }
            }
            level[t][(size_t)kTableLen] = level[t][0];      // wrap guard
        }
    }

    inline float lookup(int tableIndex, int intIndex, float frac) const {
        if (tableIndex < 0) tableIndex = 0;
        else if (tableIndex >= kNumTables) tableIndex = kNumTables - 1;
        const std::vector<float>& L = level[tableIndex];
        return (1.f - frac) * L[(size_t)intIndex] + frac * L[(size_t)intIndex + 1];
    }
};

// A plain rising ramp over the table, exactly as fillWithSaw303 builds it.
std::vector<double> makeRamp() {
    std::vector<double> p((size_t)kTableLen, 0.0);
    const int N = kTableLen;
    const int N1 = std::max(1, std::min(N - 1, (int)std::lround(0.5 * (N - 1))));
    const int N2 = N - N1;
    const double s1 = 1.0 / (N1 - 1);
    const double s2 = 1.0 / N2;
    for (int n = 0; n < N1; ++n)      p[(size_t)n] = s1 * n;
    for (int n = N1; n < N; ++n)      p[(size_t)n] = -1.0 + s2 * (n - N1);
    return p;
}

const WaveTable& sawTable() {
    static const WaveTable* t = [] {
        WaveTable* w = new WaveTable();
        w->build(makeRamp());
        return w;
    }();
    return *t;
}

const WaveTable& squareTable() {
    static const WaveTable* t = [] {
        std::vector<double> p = makeRamp();
        const double drive = std::pow(10.0, kTanhDriveDb / 20.0);
        for (int n = 0; n < kTableLen; ++n)
            p[(size_t)n] = -std::tanh(drive * p[(size_t)n] + kTanhOffset);
        // 180 degree circular shift so it phase-aligns with the saw.
        const int shift = kTableLen / 2;
        std::vector<double> r((size_t)kTableLen);
        for (int n = 0; n < kTableLen; ++n)
            r[(size_t)((n + shift) % kTableLen)] = p[(size_t)n];
        WaveTable* w = new WaveTable();
        w->build(r);
        return w;
    }();
    return *t;
}

//----------------------------------------------------------------------------
//  Module
//----------------------------------------------------------------------------
struct AcidOscillator final : Module {
    enum ParamIds  { TUNE_PARAM, FINE_PARAM, WAVE_PARAM, LEVEL_PARAM, NUM_PARAMS };
    enum InputIds  { PITCH_INPUT, WAVE_INPUT, FM_INPUT, RESET_INPUT, NUM_INPUTS };
    enum OutputIds { OUT_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    double phase[V] = {};
    rack::dsp::SchmittTrigger resetTrig[V];

    AcidOscillator() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        auto* tune = configParam(TUNE_PARAM, -24.f, 24.f, 0.f, "Tune", " semitones");
        if (tune) tune->snapEnabled = true;
        configParam(FINE_PARAM, -1.f, 1.f, 0.f, "Fine", " semitones");
        // 0 = saw, 1 = square.  The hardware is a switch; the blend between them
        // is Open303's BlendOscillator, and the ends are the two stock waves.
        configParam(WAVE_PARAM, 0.f, 1.f, 0.f, "Waveform", "%", 0.f, 100.f);
        configParam(LEVEL_PARAM, 0.f, 1.f, 1.f, "Level", "%", 0.f, 100.f);
        configInput(PITCH_INPUT, "V/Oct");
        configInput(WAVE_INPUT, "Wave CV");
        configInput(FM_INPUT, "FM");
        configInput(RESET_INPUT, "Sync");
        configOutput(OUT_OUTPUT, "Out");
        // Touch the tables here so the one-time build happens at patch load
        // rather than inside the first audio block.
        sawTable(); squareTable();
    }

    void onReset() override {
        for (int v = 0; v < V; ++v) { phase[v] = 0.0; resetTrig[v].reset(); }
    }

    void process(const ProcessArgs& args) override {
        int voices = std::max(1, inputs[PITCH_INPUT].getChannels());
        voices = std::max(voices, inputs[FM_INPUT].getChannels());
        voices = std::min(voices, V);

        const WaveTable& saw = sawTable();
        const WaveTable& sqr = squareTable();
        const float semis = params[TUNE_PARAM].getValue() + params[FINE_PARAM].getValue();
        const float level = params[LEVEL_PARAM].getValue();

        for (int v = 0; v < voices; ++v) {
            if (resetTrig[v].process(inputs[RESET_INPUT].getPolyVoltage(v), 0.1f, 2.f))
                phase[v] = 0.0;

            const float volts = inputs[PITCH_INPUT].getPolyVoltage(v)
                              + inputs[FM_INPUT].getPolyVoltage(v) * 0.1f
                              + semis / 12.f;
            float freq = rack::FREQ_C4 * std::pow(2.f, volts);
            if (freq < 0.f) freq = 0.f;
            if (freq > args.sampleRate * 0.45f) freq = args.sampleRate * 0.45f;

            // BlendOscillator::calculateIncrement
            const double increment = (double)kTableLen * freq / args.sampleRate;

            // ...and its mip selection: the exponent of the increment, biased so
            // the top note only reaches a quarter of Nyquist.
            int table = 0;
            if (increment > 0.0) table = std::ilogb(increment) + 2;
            if (table < 0) table = 0;
            if (table >= kNumTables) table = kNumTables - 1;

            double p = phase[v];
            while (p >= (double)kTableLen) p -= (double)kTableLen;
            while (p < 0.0) p += (double)kTableLen;
            const int   iidx = (int)p;
            const float frac = (float)(p - (double)iidx);

            const float blend = rack::clamp(params[WAVE_PARAM].getValue()
                + inputs[WAVE_INPUT].getPolyVoltage(v) * 0.1f, 0.f, 1.f);
            const float a = (1.f - blend) * saw.lookup(table, iidx, frac);
            // BlendOscillator halves the second waveform against the first.
            const float b = blend * sqr.lookup(table, iidx, frac) * 0.5f;

            phase[v] = p + increment;
            outputs[OUT_OUTPUT].setVoltage(rack::clamp((a + b) * 5.f * level,
                                                       -10.f, 10.f), v);
        }
        outputs[OUT_OUTPUT].setChannels(voices);
    }
};

//----------------------------------------------------------------------------
//  Faceplate
//----------------------------------------------------------------------------
rackx::PanelElement el(int id, float x, float y, float r,
                       rackx::PanelControlStyle style, const std::string& label,
                       rackx::PanelLabelPlacement place = rackx::PanelLabelPlacement::Below) {
    rackx::PanelElement e;
    e.id = id; e.x = x; e.y = y; e.radius = r;
    e.style = style; e.label = label; e.labelPlacement = place;
    return e;
}

rackx::PanelSpec acidOscPanel() {
    rackx::PanelSpec p = rackx::PanelSpec::fromHp(8);      // 121.9 px
    const float cx = p.width * 0.5f;
    p.params = {
        el(AcidOscillator::TUNE_PARAM,  cx, 70.f,  18.f, rackx::PanelControlStyle::Knob, "TUNE"),
        el(AcidOscillator::FINE_PARAM,  cx, 138.f, 14.f, rackx::PanelControlStyle::Knob, "FINE"),
        el(AcidOscillator::WAVE_PARAM,  cx, 202.f, 18.f, rackx::PanelControlStyle::Knob, "SAW-SQR"),
        el(AcidOscillator::LEVEL_PARAM, cx, 262.f, 14.f, rackx::PanelControlStyle::Knob, "LEVEL")
    };
    // Jack captions are drawn up to 42 px wide, so two per row at this width.
    const float lx = cx - 28.f, rx = cx + 28.f;
    p.inputs = {
        el(AcidOscillator::PITCH_INPUT, lx, 312.f, 8.f, rackx::PanelControlStyle::Knob, "V/OCT"),
        el(AcidOscillator::FM_INPUT,    rx, 312.f, 8.f, rackx::PanelControlStyle::Knob, "FM"),
        el(AcidOscillator::WAVE_INPUT,  lx, 352.f, 8.f, rackx::PanelControlStyle::Knob, "WAVE"),
        el(AcidOscillator::RESET_INPUT, rx, 352.f, 8.f, rackx::PanelControlStyle::Knob, "SYNC")
    };
    p.outputs = {
        el(AcidOscillator::OUT_OUTPUT, cx, 380.f - 32.f, 10.f,
           rackx::PanelControlStyle::Knob, "OUT", rackx::PanelLabelPlacement::Above)
    };
    return p;
}

} // namespace

namespace rackx {
void registerAcidOscillatorModule() {
    addType("Acid303-OSC", "303 Oscillator", "Oscillator", Role::Normal,
            [] { return std::make_unique<AcidOscillator>(); }, acidOscPanel());
}
} // namespace rackx
