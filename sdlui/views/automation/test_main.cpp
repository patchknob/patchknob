//----------------------------------------------------------------------------
//  sdlui/views/automation/test_main.cpp
//
//  Self-contained harness for AutomationView + KeyFollowPanel.  Builds a
//  `perform` with two active sequences, an AutomationPlayer seeded with a couple
//  of pre-populated lanes on track 0, mounts the two views, and (when Play is on)
//  drives player->advance() from the UI thread so the automation emit path is
//  exercised race-free.  Prints the emit counts so the advance() hook is proven.
//
//    export PATH="/c/msys64/mingw64/bin:$PATH"
//    cmake -S sdlui/views/automation -B sdlui/views/automation/build -G "MinGW Makefiles"
//    cmake --build sdlui/views/automation/build -j
//    ./sdlui/views/automation/build/automation_test
//----------------------------------------------------------------------------
#include "gui.h"
#include "automation_view.h"

#include "perform.h"
#include "sequence.h"
#include "automation/automation_player.h"

#include <atomic>
#include <cstdio>

using namespace ui;
using seq24::engine::AutomationPlayer;
using seq24::engine::AutomationTrack;
using seq24::engine::LaneTarget;
using seq24::engine::LaneTargetKind;
using seq24::engine::Interpolation;

int main(int argc, char** argv) {
    set_mode(Mode::Midnight);
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "light") set_mode(Mode::Light);

    App app;
    app.w = 1100; app.h = 760;
    if (!app.init("seq24 / SDL2 -- Automation editor + Keyfollow")) { app.shutdown(); return 1; }

    // ---- model: a perform with two active sequences -------------------------
    perform perf;
    perf.new_sequence(0);
    perf.new_sequence(1);
    sequence* s0 = perf.get_sequence(0);
    sequence* s1 = perf.get_sequence(1);
    s0->set_name(std::string("Lead")); s0->set_midi_bus(0); s0->set_length(4 * 4 * c_ppqn, false);
    s1->set_name(std::string("Bass")); s1->set_midi_bus(1); s1->set_length(4 * 4 * c_ppqn, false);
    const long LEN = s0->get_length();

    // ---- automation data model owned by the player --------------------------
    AutomationPlayer player(32);
    {
        AutomationTrack& at = player.track(0);
        // a linear CC74 (filter cutoff) sweep
        auto& cutoff = at.addLane(LaneTarget{ LaneTargetKind::MidiCC, 74 }, Interpolation::Linear);
        cutoff.add(0,           0.10f);
        cutoff.add(LEN/4,       0.90f);
        cutoff.add(LEN/2,       0.30f);
        cutoff.add(3*LEN/4,     0.80f);
        cutoff.add(LEN,         0.20f);
        // a step CC7 (volume) staircase
        auto& vol = at.addLane(LaneTarget{ LaneTargetKind::MidiCC, 7 }, Interpolation::Step);
        vol.add(0,      0.50f);
        vol.add(LEN/2,  1.00f);
        vol.add(LEN,    0.25f);
    }

    // ---- the views under test ----------------------------------------------
    automation::AutomationView av(s0, /*track=*/0, &player);
    automation::KeyFollowPanel kf(&perf);
    kf.add_track(0);
    kf.add_track(1);

    // ---- tiny top toolbar ---------------------------------------------------
    Panel bar; static Color barbg; barbg = theme().panel; bar.bg = &barbg;
    Label title; title.text = "AUTOMATION";

    static bool play = true;                 // start rolling so advance() runs live
    static std::atomic<long> emitCount{0};
    Button bPlay; bPlay.text = "Play"; bPlay.toggle = true; bPlay.on = true;
    bPlay.clicked = [&]{ play = bPlay.on; app.animating = play; app.request_redraw(); };
    app.animating = true;

    Button bTheme; bTheme.text = "Light";
    bTheme.clicked = [&]{
        bool toLight = (mode() == Mode::Midnight);
        set_mode(toLight ? Mode::Light : Mode::Midnight);
        bTheme.text = toLight ? "Midnight" : "Light";
        barbg = theme().panel; app.request_redraw();
    };

    bar.children = { &title, &bPlay, &bTheme };
    app.roots = { &bar, &kf, &av };   // av last -> keyboard focus

    // ---- transport: advance() driven from the UI thread (race-free) ---------
    static long lastTick = 0;
    static Uint32 t0 = SDL_GetTicks();
    // emit callbacks -- the shell would route these to audio_app_route_param /
    // a CC MIDI event; here we count (and could forward, harmless if no engine).
    AutomationPlayer::EmitParam emitParam =
        [&](int trk, unsigned id, float v){ (void)trk;(void)id;(void)v; emitCount++; };
    AutomationPlayer::EmitCC emitCC =
        [&](int trk, int cc, int v){ (void)trk;(void)cc;(void)v; emitCount++; };

    app.on_layout = [&](App& a) {
        bar.rect    = { 0, 0, a.w, 30 };
        title.rect  = { 8, 0, 120, 30 };
        bPlay.rect  = { 130, 4, 60, 22 };
        bTheme.rect = { a.w - 96, 4, 88, 22 };

        int splitY = 34;
        int kfH = 190;
        av.rect = { 6, splitY, a.w - 12, a.h - splitY - kfH - 12 };
        kf.rect = { 6, a.h - kfH - 6, a.w - 12, kfH };

        if (play) {
            long tempo = 480;                       // ticks/sec (~150 bpm @ 192)
            long now = (long)((SDL_GetTicks() - t0) / 1000.0 * tempo);
            long cur = now % LEN;
            if (cur < lastTick) {                   // wrapped -> flush + relocate
                player.advance(lastTick, LEN, emitParam, emitCC);
                player.emitAt(0, emitParam, emitCC);
                lastTick = 0;
            }
            player.advance(lastTick, cur, emitParam, emitCC);
            lastTick = cur;
        }
    };

    // ---- headless self-check (proves advance() emits) BEFORE the GUI loop ----
    {
        long before = emitCount.load();
        player.emitAt(0, emitParam, emitCC);
        player.advance(0, LEN, emitParam, emitCC);
        long got = emitCount.load() - before;
        std::fprintf(stderr, "[selftest] advance(0..%ld) emitted %ld changes "
                             "across %d lanes\n", LEN, got, player.track(0).laneCount());
        if (got <= 0) { std::fprintf(stderr, "[selftest] FAIL: no emits\n"); return 2; }
    }

    app.run();
    std::fprintf(stderr, "[automation_test] clean exit, total emits=%ld\n", emitCount.load());
    app.shutdown();
    return 0;
}
