//----------------------------------------------------------------------------
//  sdlui/views/mixer/mixer_test.cpp
//
//  Stand-alone harness for MixerView. Builds a real MixerGraph (8 tracks +
//  master), attaches fake LFO-driven "instruments" so every strip's VU meter
//  bounces, spins a background render thread that behaves like the audio
//  thread (renderBlock() -> per-track + master VU push), mounts the MixerView
//  as an ui::Widget root, and runs the app loop at a ~30 Hz redraw cadence.
//
//  Run headless / windowed for a few seconds: the process staying alive proves
//  the window renders and the view + engine bind without crashing. Interact
//  with the faders / pan / mute / solo with the mouse; Esc quits.
//----------------------------------------------------------------------------
#include "gui.h"
#include "mixer_view.h"

#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"
#include "engine/plugin_api.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace seq24::engine;

// ---------------------------------------------------------------------------
// Minimal fake IPluginInstance scaffolding (no real VSTs needed).
// ---------------------------------------------------------------------------
struct FakeBase : public IPluginInstance {
    PluginDescriptor desc;
    const PluginDescriptor& descriptor() const override { return desc; }
    bool prepare(double, int) override { return true; }
    void setActive(bool) override {}
    void release() override {}
    int       paramCount() const override { return 0; }
    ParamInfo paramInfo(int) const override { return ParamInfo{}; }
    float     getParamNormalized(uint32_t) const override { return 0.0f; }
    void      setParamNormalized(uint32_t, float) override {}
    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}
    std::vector<uint8_t> saveState() const override { return {}; }
    void loadState(const std::vector<uint8_t>&) override {}
};

// An "instrument" that continuously emits a sine tone whose amplitude follows a
// slow per-track LFO -> lively, independent VU meters with no MIDI required.
struct LfoSynth : public FakeBase {
    double sr = 48000.0;
    double phase = 0.0, freq = 220.0;
    double lfoPhase = 0.0, lfoHz = 0.5;
    float  base = 0.5f;

    LfoSynth(const char* name, double f, double lfo, float b)
        : freq(f), lfoHz(lfo), base(b) {
        desc.name = name; desc.isInstrument = true;
        desc.numAudioIn = 0; desc.numAudioOut = 2;
    }
    bool prepare(double sampleRate, int) override { sr = sampleRate; return true; }
    void process(const ProcessBlock& blk) override {
        const double TWO_PI = 6.28318530717958647692;
        for (int i = 0; i < blk.nframes; ++i) {
            float env = base * (0.35f + 0.65f * float(0.5 + 0.5 * std::sin(lfoPhase)));
            float s = env * float(std::sin(phase));
            blk.audioOut[0][i] = s;
            blk.audioOut[1][i] = s;
            phase += TWO_PI * freq / sr;    if (phase   > TWO_PI) phase   -= TWO_PI;
            lfoPhase += TWO_PI * lfoHz / sr; if (lfoPhase > TWO_PI) lfoPhase -= TWO_PI;
        }
    }
};

// A trivial insert FX just so the FX list has names to show.
struct FakeFx : public FakeBase {
    float g;
    FakeFx(const char* name, float gain) : g(gain) {
        desc.name = name; desc.numAudioIn = 2; desc.numAudioOut = 2;
    }
    void process(const ProcessBlock& blk) override {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < blk.nframes; ++i)
                blk.audioOut[c][i] = blk.audioIn[c][i] * g;
    }
};

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // ---- build the engine model -------------------------------------------
    const double kSr = 48000.0;
    const int    kBlock = 512;

    MixerGraph graph;
    graph.setTrackCount(8);

    static const char* names[8] = {
        "Bass Synth", "Lead", "Pad", "Pluck", "Keys", "Arp", "Brass", "Strings"
    };
    static const double freqs[8] = { 110, 220, 165, 330, 262, 392, 147, 294 };
    static const double lfos[8]  = { 0.4, 0.9, 0.25, 1.3, 0.6, 1.7, 0.35, 0.8 };

    std::vector<LfoSynth*> synths;
    std::vector<FakeFx*>   fx;
    for (int i = 0; i < 8; ++i) {
        LfoSynth* s = new LfoSynth(names[i], freqs[i], lfos[i], 0.55f);
        synths.push_back(s);
        graph.track(i)->setInstrument(s);
    }
    // a few insert FX so the FX lists render
    fx.push_back(new FakeFx("EQ-3",   1.0f)); graph.track(0)->addFx(fx.back());
    fx.push_back(new FakeFx("Comp",   1.0f)); graph.track(0)->addFx(fx.back());
    fx.push_back(new FakeFx("Reverb", 1.0f)); graph.track(1)->addFx(fx.back());
    fx.push_back(new FakeFx("Chorus", 1.0f)); graph.track(2)->addFx(fx.back());
    fx.push_back(new FakeFx("Delay",  1.0f)); graph.track(2)->addFx(fx.back());
    fx.push_back(new FakeFx("Drive",  1.0f)); graph.track(4)->addFx(fx.back());

    // varied initial mix state so the view is visually rich
    graph.track(0)->setGain(1.10f); graph.track(0)->setPan(-0.4f);
    graph.track(1)->setGain(0.90f); graph.track(1)->setPan(0.3f);
    graph.track(2)->setGain(0.70f); graph.track(2)->setPan(-0.7f);
    graph.track(3)->setGain(1.00f); graph.track(3)->setPan(0.6f);
    graph.track(4)->setGain(0.55f); graph.track(4)->setMute(true);
    graph.track(5)->setGain(0.80f);
    graph.track(6)->setGain(1.25f); graph.track(6)->setPan(0.15f);
    graph.track(7)->setGain(0.65f); graph.track(7)->setPan(-0.2f);
    graph.setMasterGain(0.9f);

    if (!graph.prepare(kSr, kBlock)) {
        std::fprintf(stderr, "graph.prepare failed\n");
        return 2;
    }
    graph.setTransport(120.0, 0, true);

    // ---- SDL app ----------------------------------------------------------
    ui::App app;
    app.w = 1040; app.h = 600;
    ui::set_mode(ui::Mode::Midnight);            // show the phosphor-green theme
    if (!app.init("seq24 / SDL mixer")) { app.shutdown(); return 1; }
    // Pin to the top-left work area so the full strip (incl. pan / mute-solo /
    // readout at the bottom) sits above the taskbar for the demo/screenshot.
    SDL_SetWindowPosition(app.window, 0, 0);

    mixer::MixerView view(&graph);
    app.roots = { &view };
    app.on_layout = [&](ui::App& a) {
        view.rect = { 0, 0, a.w, a.h };
    };

    // ---- fake audio-thread render loop + ~30 Hz redraw --------------------
    std::atomic<bool> running{true};
    std::thread render_thread([&] {
        std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
        float* out[2] = { L.data(), R.data() };
        int redraw = 0;
        while (running.load(std::memory_order_relaxed)) {
            graph.renderBlock(out, 2, kBlock, nullptr, 0);   // updates all VUs
            if ((++redraw % 3) == 0) app.request_redraw();   // ~30 Hz
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Optional: auto-quit after N seconds (headless CI). SEQ24_MIXER_SECONDS.
    std::thread* quit_thread = nullptr;
    if (const char* sec = getenv("SEQ24_MIXER_SECONDS")) {
        int s = atoi(sec);
        if (s > 0) {
            quit_thread = new std::thread([&app, s] {
                std::this_thread::sleep_for(std::chrono::seconds(s));
                app.running = false;
                app.request_redraw();
            });
        }
    }

    app.run();

    running.store(false, std::memory_order_relaxed);
    render_thread.join();
    if (quit_thread) { quit_thread->join(); delete quit_thread; }

    graph.release();
    app.shutdown();

    for (auto* s : synths) delete s;
    for (auto* f : fx)     delete f;
    return 0;
}
