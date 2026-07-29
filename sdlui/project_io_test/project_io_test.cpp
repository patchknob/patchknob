//----------------------------------------------------------------------------
//  project_io_test.cpp -- head-less round-trip test for save_project /
//  load_project.  Builds a perform with 2 sequences (notes + a CC event +
//  scale-follow flags), saves it, loads it into a FRESH perform, and asserts
//  the sequences / notes / bus / flags survive the round trip.
//
//  No audio device and no window are created: this exercises the sequence /
//  event / scale-flag path.  Track / instrument sections are naturally skipped
//  because the audio engine is not running.
//----------------------------------------------------------------------------
#include "project_io.h"
#include "perform.h"
#include "sequence.h"
#include "event.h"
#include "globals.h"
#include "gui.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; } \
    else         { std::printf("  ok  : %s\n", msg); } \
} while (0)

struct Note { long s, f; int note, vel; };
struct Trigger { long start, length, offset; };

static std::vector<Note> notes_of(sequence* seq) {
    std::vector<Note> v;
    seq->reset_draw_marker();
    long ts, tf; int note, vel; bool sel;
    while (seq->get_next_note_event(&ts, &tf, &note, &sel, &vel) != DRAW_FIN) {
        Note n; n.s = ts; n.f = tf; n.note = note; n.vel = vel;
        v.push_back(n);
    }
    return v;
}

static int count_cc(sequence* seq, unsigned char cc) {
    seq->reset_draw_marker();
    long tick; unsigned char d0, d1; bool sel; int n = 0;
    while (seq->get_next_event(EVENT_CONTROL_CHANGE, cc, &tick, &d0, &d1, &sel)) {
        if (d0 == cc) ++n;
    }
    return n;
}

static std::vector<Trigger> triggers_of(sequence* seq) {
    std::vector<Trigger> v;
    seq->reset_draw_trigger_marker();
    long start, end, offset; bool selected;
    while (seq->get_next_trigger(&start, &end, &selected, &offset))
        v.push_back(Trigger{start, end - start + 1, offset});
    return v;
}

int main(int argc, char** argv)
{
    std::string path = (argc > 1) ? argv[1] : "project_io_roundtrip.s24proj";

    // ---- build a source project -------------------------------------------
    perform src;

    // seq 0 : "Bass" -- bus 2, ch 5, scale MASTER (key D=2, minor=2)
    src.new_sequence(0);
    {
        sequence* s = src.get_sequence(0);
        s->set_name(std::string("Bass"));
        s->set_midi_bus(2);
        s->set_midi_channel(5);
        s->set_length(c_ppqn * 4, false);
        // two notes
        s->add_event(0,          EVENT_NOTE_ON,  36, 100);
        s->add_event(48,         EVENT_NOTE_OFF, 36, 0);
        s->add_event(96,         EVENT_NOTE_ON,  38, 90);
        s->add_event(96 + 48,    EVENT_NOTE_OFF, 38, 0);
        // a CC7 (volume) event
        s->add_event(24, EVENT_CONTROL_CHANGE, 7, 64);
        s->set_master_key(2);
        s->set_master_scale(2);
    }
    src.set_scale_master(0);        // designates seq 0 as the scale master

    // seq 5 : "Lead" -- bus 1, ch 0, FOLLOWS master
    src.new_sequence(5);
    {
        sequence* s = src.get_sequence(5);
        s->set_name(std::string("Lead"));
        s->set_midi_bus(1);
        s->set_midi_channel(0);
        s->set_length(c_ppqn * 8, false);
        s->add_event(0,  EVENT_NOTE_ON,  60, 127);
        s->add_event(24, EVENT_NOTE_OFF, 60, 0);
    }
    src.set_follows_master(5, true);
    src.get_sequence(0)->add_trigger(0, c_ppqn * 4, 0, false);
    src.get_sequence(0)->add_trigger(c_ppqn * 8, c_ppqn * 2, c_ppqn / 2, false);

    src.set_bpm(140);
    ui::set_mode(ui::Mode::Midnight);

    // ---- save --------------------------------------------------------------
    std::vector<ProjectPatchNodePosition> patchLayout = {
        { 11, 123.5, 45.25 }, { 27, 640.0, 310.0 }
    };
    bool saved = save_project(src, path, patchLayout);
    CHECK(saved, "save_project returned true");
    if (!saved) { std::printf("  err: %s\n", project_io_last_error()); return 1; }
    {
        char magic[8] = {};
        unsigned char version[4] = {};
        std::ifstream file(path.c_str(), std::ios::binary);
        file.read(magic, sizeof(magic));
        file.read(reinterpret_cast<char*>(version), sizeof(version));
        const unsigned int formatVersion = (unsigned int)version[0] |
            ((unsigned int)version[1] << 8) |
            ((unsigned int)version[2] << 16) |
            ((unsigned int)version[3] << 24);
        CHECK(file.good() && std::string(magic, sizeof(magic)) == "S24DAWPJ",
              "project header magic is present");
        CHECK(formatVersion == 3, "project uses the V3 container format");
    }

    // ---- load into a fresh perform ----------------------------------------
    ui::set_mode(ui::Mode::Light);   // clobber, load must restore Midnight
    perform dst;
    bool loaded = load_project(dst, path);
    CHECK(loaded, "load_project returned true");
    if (!loaded) { std::printf("  err: %s\n", project_io_last_error()); return 1; }

    // ---- assert ------------------------------------------------------------
    CHECK(dst.is_active(0),  "seq 0 active after load");
    CHECK(dst.is_active(5),  "seq 5 active after load");
    CHECK(!dst.is_active(1), "seq 1 inactive (never saved)");
    CHECK(dst.get_bpm() == 140, "perform bpm == 140");
    CHECK(ui::mode() == ui::Mode::Midnight, "theme mode restored to Midnight");

    sequence* a = dst.get_sequence(0);
    sequence* b = dst.get_sequence(5);
    CHECK(a != nullptr && b != nullptr, "both sequences fetchable");

    if (a) {
        CHECK(std::string(a->get_name()) == "Bass", "seq 0 name == Bass");
        CHECK(a->get_midi_bus() == 2,       "seq 0 bus == 2");
        CHECK(a->get_midi_channel() == 5,   "seq 0 channel == 5");
        CHECK(a->get_length() == c_ppqn*4,  "seq 0 length == ppqn*4");
        CHECK(a->get_scale_master(),        "seq 0 is scale master");
        CHECK(a->get_master_key() == 2,     "seq 0 master key == 2");
        CHECK(a->get_master_scale() == 2,   "seq 0 master scale == 2");

        std::vector<Note> ns = notes_of(a);
        CHECK(ns.size() == 2, "seq 0 has 2 notes");
        bool found36 = false, found38 = false;
        for (size_t i = 0; i < ns.size(); ++i) {
            if (ns[i].note == 36 && ns[i].s == 0  && ns[i].f == 48     && ns[i].vel == 100) found36 = true;
            if (ns[i].note == 38 && ns[i].s == 96 && ns[i].f == 96+48  && ns[i].vel == 90)  found38 = true;
        }
        CHECK(found36, "seq 0 note 36 (tick 0..48 vel100) round-tripped");
        CHECK(found38, "seq 0 note 38 (tick 96..144 vel90) round-tripped");
        CHECK(count_cc(a, 7) == 1, "seq 0 CC7 event round-tripped");
        std::vector<Trigger> triggers = triggers_of(a);
        CHECK(triggers.size() == 2, "seq 0 timeline clips round-tripped");
        bool foundFirst = false, foundSecond = false;
        for (const auto& trigger : triggers) {
            if (trigger.start == 0 && trigger.length == c_ppqn * 4 && trigger.offset == 0) foundFirst = true;
            if (trigger.start == c_ppqn * 8 && trigger.length == c_ppqn * 2 && trigger.offset == c_ppqn / 2) foundSecond = true;
        }
        CHECK(foundFirst && foundSecond, "timeline clip positions and offsets round-tripped");
    }

    if (b) {
        CHECK(std::string(b->get_name()) == "Lead", "seq 5 name == Lead");
        CHECK(b->get_midi_bus() == 1,     "seq 5 bus == 1");
        CHECK(b->get_midi_channel() == 0, "seq 5 channel == 0");
        CHECK(b->get_length() == c_ppqn*8,"seq 5 length == ppqn*8");
        CHECK(b->get_follows_master(),    "seq 5 follows master");
        std::vector<Note> ns = notes_of(b);
        CHECK(ns.size() == 1, "seq 5 has 1 note");
        if (ns.size() == 1)
            CHECK(ns[0].note == 60 && ns[0].vel == 127, "seq 5 note 60 vel127 round-tripped");
    }

    CHECK(dst.get_scale_master() == 0, "perform scale-master index == 0");
    const auto& restoredLayout = project_io_loaded_patch_layout();
    CHECK(restoredLayout.size() == 2, "patch node layout round-tripped");
    CHECK(restoredLayout.size() == 2 && restoredLayout[0].nodeId == 11 &&
          restoredLayout[0].x == 123.5 && restoredLayout[0].y == 45.25,
          "first patch node coordinates round-tripped");

    std::printf("\n%s  (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
