//----------------------------------------------------------------------------
//  sdlui/views/piano_roll/test_main.cpp
//
//  Stand-alone harness for ui::PianoRoll: builds a sample PatchKnob `sequence`
//  (melody + chord + velocity ramp), mounts the piano roll as a full-window
//  root widget, animates the playhead, and runs the toolkit event loop.
//
//  Env:
//     PATCHKNOB_RUN_MS  -- auto-quit after N ms (screenshot / CI).  0 = run forever.
//     PATCHKNOB_LIGHT   -- start in Light theme instead of Midnight.
//----------------------------------------------------------------------------
#include "gui.h"
#include "pianoroll.h"
#include "sequence.h"
#include "event.h"
#include "globals.h"

#include <atomic>
#include <thread>
#include <cstdlib>
#include <cstdio>

using namespace ui;

static void build_sample(sequence& seq)
{
    seq.set_bpm(4);          // 4 beats / measure
    seq.set_bw(4);           // quarter-note beat
    int tpbar = 4 * (4 * c_ppqn) / 4;      // 768 ticks / bar
    seq.set_length(4 * tpbar);             // 4 bars
    seq.set_name("SDL piano-roll demo");

    // a rising melody, one note per 8th, with a rising/falling velocity ramp
    const int scale[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    for (int i = 0; i < 16; ++i) {
        long tk  = i * (c_ppqn / 2);                 // every 8th note (96)
        int  n   = scale[i % 8] + (i >= 8 ? 12 : 0);
        int  vel = 45 + (i * 7) % 80;
        seq.add_event(tk,      EVENT_NOTE_ON,  n, vel);
        seq.add_event(tk + 84, EVENT_NOTE_OFF, n, 0);
    }

    // a held C-major triad across the 3rd bar
    long ct = 2 * tpbar;
    const int chord[3] = { 48, 52, 55 };
    for (int j = 0; j < 3; ++j) {
        seq.add_event(ct,       EVENT_NOTE_ON,  chord[j], 95);
        seq.add_event(ct + 700, EVENT_NOTE_OFF, chord[j], 0);
    }

    seq.verify_and_link();
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    set_mode(getenv("PATCHKNOB_LIGHT") ? Mode::Light : Mode::Midnight);

    App app;
    app.w = 1120; app.h = 720;
    if (!app.init("PatchKnob -- Piano Roll")) { app.shutdown(); return 1; }

    sequence seq;
    build_sample(seq);
    printf("[pianoroll] sample sequence built: length=%ld ticks\n", seq.get_length());
    fflush(stdout);

    PianoRoll roll(&seq);
    roll.set_snap(c_ppqn / 4);
    roll.set_note_length(c_ppqn / 4);
    roll.set_zoom(6);

    app.roots = { &roll };
    app.on_layout = [&](App& a) {
        roll.rect = { 6, 6, a.w - 12, a.h - 12 };
    };

    // ---- animate the playhead in a side thread; nudge redraws --------------
    long run_ms = 0;
    if (const char* rm = getenv("PATCHKNOB_RUN_MS")) run_ms = atol(rm);
    std::atomic<bool> go{ true };
    Uint32 t0 = SDL_GetTicks();
    std::thread anim([&] {
        while (go.load() && app.running) {
            Uint32 now = SDL_GetTicks();
            double sec = (now - t0) / 1000.0;
            long tick = (long)(sec * 384.0) % (seq.get_length() > 0 ? seq.get_length() : 1);
            seq.set_orig_tick(tick);
            app.request_redraw();
            if (run_ms > 0 && (long)(now - t0) >= run_ms) app.running = false;
            SDL_Delay(33);
        }
    });

    app.run();

    go.store(false);
    if (anim.joinable()) anim.join();
    app.shutdown();
    printf("[pianoroll] clean exit\n");
    return 0;
}
