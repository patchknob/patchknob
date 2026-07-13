//----------------------------------------------------------------------------
//  sdlui/views/arrange/test_main.cpp
//
//  Self-contained harness for ArrangeView: builds a `perform` with a handful of
//  active sequences (names, lengths, a few notes, several clip triggers),
//  mounts the view as a single ui::Widget root sized to the window, and runs the
//  app loop.  A background thread animates the playhead so the moving playhead
//  can be seen / screenshotted without the real transport running.
//----------------------------------------------------------------------------
#include "gui.h"
#include "arrange_view.h"
#include "perform.h"
#include "sequence.h"
#include "globals.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>

using namespace ui;

static const long BAR = c_ppqn * 4;   // 768 ticks == one 4/4 bar

// add one clip trigger spanning [bar_from, bar_to) bars on a sequence
static void clip(perform& p, int seq, int bar_from, int bar_to)
{
    long start = (long)bar_from * BAR;
    long len   = (long)(bar_to - bar_from) * BAR;
    p.get_sequence(seq)->add_trigger(start, len);
}

// give a sequence a short note motif so the clip note-preview renders
static void motif(perform& p, int seq, const int* notes, int n, long step)
{
    sequence* s = p.get_sequence(seq);
    for (int i = 0; i < n; ++i)
        s->add_note(i * step, step * 3 / 4, notes[i]);
    s->verify_and_link();
}

static void build_song(perform& p)
{
    struct Trk { const char* name; long bars; int ch; } trk[] = {
        { "Kick",  1, 0 }, { "Snare", 1, 1 }, { "HiHat", 1, 2 },
        { "Bass",  2, 3 }, { "Lead",  4, 4 }, { "Pad",   4, 5 },
        { "Arp",   2, 6 }, { "FX",    2, 7 }, { "Vox",   4, 8 },
    };
    int n = (int)(sizeof(trk) / sizeof(trk[0]));

    for (int i = 0; i < n; ++i) {
        p.new_sequence(i);
        sequence* s = p.get_sequence(i);
        s->set_name(std::string(trk[i].name));
        s->set_midi_channel((unsigned char)trk[i].ch);
        s->set_length(trk[i].bars * BAR);
    }

    // note motifs (scale over the pattern length)
    int bass[]  = { 36, 43, 39, 41 };            motif(p, 3, bass, 4, BAR / 2);
    int lead[]  = { 72, 74, 76, 79, 76, 74 };    motif(p, 4, lead, 6, BAR / 2);
    int pad[]   = { 60, 64, 67 };                motif(p, 5, pad,  3, BAR);
    int arp[]   = { 60, 63, 67, 70 };            motif(p, 6, arp,  4, BAR / 4);
    int vox[]   = { 67, 69, 71, 72 };            motif(p, 8, vox,  4, BAR);

    // arrangement (clip triggers laid out along the timeline)
    clip(p, 0, 0, 1); clip(p, 0, 1, 2); clip(p, 0, 2, 3); clip(p, 0, 4, 5);
    clip(p, 0, 5, 6); clip(p, 0, 6, 7); clip(p, 0, 8, 9); clip(p, 0, 9,10);
    clip(p, 1, 2, 3); clip(p, 1, 3, 4); clip(p, 1, 6, 7); clip(p, 1, 7, 8);
    clip(p, 2, 0, 4); clip(p, 2, 4, 8); clip(p, 2, 8,12);
    clip(p, 3, 0, 2); clip(p, 3, 4, 6); clip(p, 3, 8,10);
    clip(p, 4, 4, 8); clip(p, 4, 12,16);
    clip(p, 5, 0, 4); clip(p, 5, 8,12);
    clip(p, 6, 2, 4); clip(p, 6, 6, 8); clip(p, 6, 10,12);
    clip(p, 7, 5, 7); clip(p, 7, 13,15);
    clip(p, 8, 4, 8); clip(p, 8, 12,16);

    // a couple of pre-muted / soloable states + loop markers for the header
    p.get_sequence(7)->set_song_mute(true);
    p.set_left_tick(0);
    p.set_right_tick(8 * BAR);
}

int main(int argc, char** argv)
{
    // Midnight (phosphor green) shows the DAW look best; pass "light" to flip.
    set_mode(Mode::Midnight);
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "light") set_mode(Mode::Light);

    App app;
    app.w = 1280; app.h = 720;
    if (!app.init("seq24 / SDL2 -- Arrangement (song) view")) { app.shutdown(); return 1; }

    perform perf;
    build_song(perf);

    arrange::ArrangeView view(&perf);
    app.roots = { &view };
    app.on_layout = [&](App& a) {
        view.rect = { 0, 0, a.w, a.h };
    };

    // animate the playhead on a background thread (main thread renders only)
    std::atomic<bool> alive{ true };
    std::thread ticker([&]{
        long t = 0;
        while (alive.load()) {
            t += c_ppqn / 4;                 // ~a 16th note per step
            if (t > 20 * BAR) t = 0;         // loop over 20 bars
            view.playhead_tick = t;
            app.request_redraw();
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });

    app.run();

    alive.store(false);
    ticker.join();
    app.shutdown();
    return 0;
}
