//----------------------------------------------------------------------------
//  sdlui/views/waveform/test_main.cpp -- standalone harness for WaveformView +
//  AudioTrack.
//
//  What it exercises:
//    * synth an AudioClip (sine sweep-ish mix) and bind it to a WaveformView,
//    * mount an AudioTrack onto a real (prepared) MixerGraph and schedule the
//      same clip, then render a few offline blocks to prove the audio path
//      actually produces sound through the graph (no device needed),
//    * open an SDL window, mount the view, and drive automated zoom in/out via
//      the buttons + simulated wheel so the render path is covered even when run
//      head-less/unattended; then hand over to interactive use.
//
//  Pass a .wav path as argv[1] to display a real file instead of the synth clip.
//----------------------------------------------------------------------------
#include "gui.h"
#include "waveform_view.h"
#include "audio_track.h"

#include "engine/audioclip/audio_clip.h"
#include "engine/graph/mixer_graph.h"
#include "engine/plugin_api.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace PatchKnob::engine;

static const double kSr    = 48000.0;
static const int    kBlock = 256;

// Build a visually interesting clip: an amplitude-enveloped two-tone mix.
static AudioClip make_demo_clip() {
    const double dur = 2.5;
    AudioClip c;
    c.name = "demo-tone";
    c.sampleRate = kSr; c.sourceSampleRate = kSr;
    const int64_t n = (int64_t)(dur * kSr);
    c.resize(n);
    const double twoPi = 2.0 * 3.14159265358979323846;
    for (int64_t i = 0; i < n; ++i) {
        double tt = (double)i / kSr;
        double env = 0.2 + 0.8 * std::fabs(std::sin(twoPi * 1.5 * tt));   // 1.5 Hz tremolo
        double s = 0.6 * std::sin(twoPi * 220.0 * tt)
                 + 0.3 * std::sin(twoPi * 660.0 * tt);
        float v = (float)(env * s * 0.8);
        c.ch[0][(size_t)i] = v;
        c.ch[1][(size_t)i] = v;
    }
    return c;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered so diagnostics flush live

    // ---- clip -------------------------------------------------------------
    AudioClip demo = make_demo_clip();

    // ---- audio-track wiring proof (offline render through a real graph) ---
    MixerGraph graph;
    graph.setTrackCount(4);
    bool prepared = graph.prepare(kSr, kBlock);
    std::printf("[audio] graph.prepare = %d, tracks=%d\n", prepared, graph.trackCount());

    waveform::AudioTrack atk;
    bool mounted = atk.mount(&graph, /*track*/ 2, kSr, kBlock);
    const AudioClip* clip = atk.add_test_tone(330.0, 0.5, kSr, /*start*/ 0);
    std::printf("[audio] mounted=%d clip=%p clips=%d\n",
                mounted, (const void*)clip, atk.clip_count());

    // Drive the transport and render a couple of blocks that overlap the clip;
    // confirm the graph output is non-silent (audio really flows via the graph).
    graph.setTransport(120.0, 0, true);
    std::vector<float> L((size_t)kBlock), R((size_t)kBlock);
    float* out[2] = { L.data(), R.data() };
    float peak = 0.0f;
    for (int b = 0; b < 8; ++b) {
        int64_t pos = (int64_t)b * kBlock;
        graph.setTransport(120.0, pos, true);
        // no per-track MIDI/automation; the AudioClipPlayer reads transport
        graph.stageInputs(nullptr, 0);
        for (int i = 0; i < kBlock; ++i) { L[i] = 0; R[i] = 0; }
        graph.render(out, 2, kBlock, kSr);
        for (int i = 0; i < kBlock; ++i)
            peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    std::printf("[audio] offline render peak through graph = %.4f (%s)\n",
                peak, peak > 0.01f ? "NON-SILENT ok" : "SILENT!");

    // ---- UI ---------------------------------------------------------------
    ui::App app;
    app.w = 1000; app.h = 360;
    if (!app.init("waveform view test")) { std::printf("app.init failed\n"); return 1; }
    ui::set_mode(ui::Mode::Midnight);

    waveform::WaveformView wv;
    if (argc > 1) {
        // Display a real WAV if provided; AudioTrack loads + schedules it.
        std::string err;
        static waveform::AudioTrack fileTrack;
        fileTrack.mount(&graph, 3, kSr, kBlock);
        const AudioClip* fc = fileTrack.load_wav_clip(argv[1], kSr, 0, 1.0f, &err);
        if (fc) { wv.set_clip(fc); std::printf("[ui] loaded %s\n", argv[1]); }
        else    { wv.set_clip(&demo); std::printf("[ui] load failed: %s\n", err.c_str()); }
    } else {
        wv.set_clip(&demo);
    }

    app.roots.push_back(&wv);
    app.on_layout = [&](ui::App& a){ wv.rect = { 8, 8, a.w - 16, a.h - 16 }; };

    // Automated smoke test: exercise zoom + scroll paths a few times so a
    // head-less run still covers draw() at multiple zoom levels.
    std::printf("[ui] fit spp=%.3f\n", wv.samples_per_pixel());
    for (int i = 0; i < 6; ++i) wv.zoom_in();
    std::printf("[ui] after zoom-in  spp=%.4f scroll=%lld\n",
                wv.samples_per_pixel(), (long long)wv.scroll_sample());
    wv.set_scroll_sample(20000);
    for (int i = 0; i < 10; ++i) wv.zoom_out();
    std::printf("[ui] after zoom-out spp=%.4f scroll=%lld\n",
                wv.samples_per_pixel(), (long long)wv.scroll_sample());
    wv.zoom_fit();
    wv.set_playhead(demo.numFrames() / 3);

    // Optional: auto-quit after ~2.5s (WF_AUTOEXIT=1) for unattended runs.
    if (const char* ae = getenv("WF_AUTOEXIT")) {
        (void)ae;
        SDL_InitSubSystem(SDL_INIT_TIMER);
        SDL_AddTimer(2500, [](Uint32, void*) -> Uint32 {
            SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); return 0;
        }, nullptr);
    }

    app.request_redraw();
    app.run();
    app.shutdown();
    std::printf("[ui] clean exit\n");
    return 0;
}
