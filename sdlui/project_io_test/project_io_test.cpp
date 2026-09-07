//----------------------------------------------------------------------------
//  project_io_test.cpp -- head-less round-trip test for save_project /
//  load_project.  Builds a perform with several sequences (notes + a CC event +
//  scale-follow flags + per-clip LOOP windows), saves it, loads it into a FRESH
//  perform, and asserts the sequences / notes / bus / flags / loop state survive
//  the round trip.
//
//  It then DOWNGRADES the saved file in place -- rewriting each "SEQ " payload
//  the way v15 / v14 / v13 wrote it -- to prove every backward-compatibility
//  path still loads and still lands on the historical defaults.
//
//  No audio device and no window are created: this exercises the sequence /
//  event / scale-flag path.  The engine-owned sections ("TRAK" / "PTCH" /
//  "AUDI") cannot be GENERATED without a running engine -- but that is exactly
//  the state in which a save used to silently strip them out of an existing
//  project and still report success, so the checks at the bottom of this file
//  splice real sections into a saved file and assert that a head-less save
//  carries them through byte for byte (or refuses).
//
//  The bottom half also covers the two "one bad file eats the session" defects:
//  a rejected project load must leave the open perform untouched, and a
//  rejected .pkr must leave the open rack untouched.
//----------------------------------------------------------------------------
#include "project_io.h"
#include "engine/graph/track.h"
#include "engine/rack/rack_engine.h"
#include "perform.h"
#include "sequence.h"
#include "event.h"
#include "globals.h"
#include "gui.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// The SDL widget toolkit is NOT linked into this head-less test.  project_io
// reaches into it for exactly one thing -- the persisted theme mode -- so those
// two symbols live here instead of dragging gui.cpp + SDL_ttf + libpng +
// librsvg into a test that never opens a window.
namespace ui {
static Mode g_mode = Mode::Light;
void set_mode(Mode m) { g_mode = m; }
Mode mode() { return g_mode; }
}

// audio_app.cpp references the metronome click sample, which the real app build
// generates from Click.wav at configure time.  Nothing here starts the audio
// engine, so an empty blob is all the linker needs.
extern const unsigned char g_click_wav[1];
extern const std::size_t   g_click_wav_size;
const unsigned char g_click_wav[1] = { 0 };
const std::size_t   g_click_wav_size = 0;

// ---------------------------------------------------------------------------
//  raw-file helpers (the engine-section / corruption checks work on bytes)
// ---------------------------------------------------------------------------
static std::string read_all(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}
static bool write_all(const std::string& p, const std::string& d) {
    std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc);
    f.write(d.data(), (std::streamsize)d.size());
    return f.good();
}
static bool file_exists(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    return (bool)f;
}

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

// ---------------------------------------------------------------------------
//  Rewrite a saved project as an OLDER format version, so the compatibility
//  paths are tested against real bytes instead of a hand-typed fixture that
//  would rot the moment an unrelated field is added.
//
//  Only the "SEQ " payload changed across v13..v16: v16 appends loop_start +
//  loop_end + loop_enabled, v15 the first two, v14 only loop_start, v13 and
//  older none of them.  Every other section is copied through byte for byte.
// ---------------------------------------------------------------------------
static uint32_t rd_u32(const std::string& b, size_t o) {
    return  (uint32_t)(unsigned char)b[o]           |
           ((uint32_t)(unsigned char)b[o+1] <<  8)  |
           ((uint32_t)(unsigned char)b[o+2] << 16)  |
           ((uint32_t)(unsigned char)b[o+3] << 24);
}
static void wr_u32(std::string& b, size_t o, uint32_t v) {
    b[o+0] = (char)( v        & 0xFF);  b[o+1] = (char)((v >>  8) & 0xFF);
    b[o+2] = (char)((v >> 16) & 0xFF);  b[o+3] = (char)((v >> 24) & 0xFF);
}

static void ap_u32(std::string& b, uint32_t v) {
    b += (char)( v        & 0xFF);  b += (char)((v >>  8) & 0xFF);
    b += (char)((v >> 16) & 0xFF);  b += (char)((v >> 24) & 0xFF);
}
// tag + u32 payloadLen + payload -- one container section, ready to splice.
static std::string make_section(const char* tag, const std::string& payload) {
    std::string s(tag, 4); ap_u32(s, (uint32_t)payload.size()); return s + payload;
}
// Insert raw sections immediately before the trailing "END " section.
static std::string splice_before_end(const std::string& file, const std::string& extra) {
    if (file.size() < 8 || file.compare(file.size() - 8, 4, "END ") != 0) return std::string();
    return file.substr(0, file.size() - 8) + extra + file.substr(file.size() - 8);
}

struct SectionAt { size_t off; std::string tag; uint32_t len; };
static std::vector<SectionAt> sections_of(const std::string& d) {
    std::vector<SectionAt> v;
    size_t p = 12;
    while (p + 8 <= d.size()) {
        SectionAt s; s.off = p; s.tag = d.substr(p, 4); s.len = rd_u32(d, p + 4);
        if (p + 8 + (size_t)s.len > d.size()) break;
        v.push_back(s);
        p += 8 + (size_t)s.len;
        if (s.tag == "END ") break;
    }
    return v;
}
// Byte offset of a v16 "SEQ " payload's numEvents field (same walk
// downgrade_project() uses to find the trailing loop fields).
static size_t seq_event_count_offset(const std::string& d, size_t sectionOff) {
    size_t q = sectionOff + 8;
    q += 4;                                   // slot
    q += 4 + rd_u32(d, q);                    // name
    q += 1+1 + 4+4+4 + 1+1+1 + 4+4 + 1;       // bus .. songMute
    q += 4 + rd_u32(d, q);                    // fx blob
    q += 1 + 1 + 4;                           // thru, kind, laneId
    q += 12;                                  // loopStart, loopEnd, loopEnabled
    return q;
}

static bool downgrade_project(const std::string& inPath,
                              const std::string& outPath, unsigned version)
{
    std::ifstream f(inPath.c_str(), std::ios::binary);
    if (!f) return false;
    const std::string d((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    if (d.size() < 12) return false;

    // how many of the three trailing loop i32s this version did NOT have
    const size_t dropped = version >= 15 ? 1 : (version == 14 ? 2 : 3);

    std::string o = d.substr(0, 8);              // magic
    o.resize(12); wr_u32(o, 8, version);         // restamped version

    size_t p = 12;
    while (p + 8 <= d.size()) {
        const std::string tag = d.substr(p, 4);
        const uint32_t len = rd_u32(d, p + 4);
        if (p + 8 + (size_t)len > d.size()) return false;
        std::string payload = d.substr(p + 8, len);
        p += 8 + len;

        if (tag == "SEQ ") {
            size_t q = 4;                                    // slot
            if (q + 4 > payload.size()) return false;
            q += 4 + rd_u32(payload, q);                     // name
            q += 1+1 + 4+4+4 + 1+1+1 + 4+4 + 1;              // bus .. songMute
            if (q + 4 > payload.size()) return false;
            q += 4 + rd_u32(payload, q);                     // fx blob
            q += 1 + 1 + 4;                                  // thru, kind, laneId
            if (q + 12 > payload.size()) return false;       // the three loop i32s
            payload.erase(q + (3 - dropped) * 4, dropped * 4);
        }

        o += tag;
        const size_t at = o.size();
        o.resize(at + 4); wr_u32(o, at, (uint32_t)payload.size());
        o += payload;
        if (tag == "END ") break;
    }

    std::ofstream g(outPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!g) return false;
    g.write(o.data(), (std::streamsize)o.size());
    return g.good();
}


// ---------------------------------------------------------------------------
//  A hand-built "PTCH" payload, so the modular-patch layout -- and the v17->v18
//  aux-bus migration -- can be pinned down with no audio engine.  load_project
//  only parses PTCH when a graph is up (that is the whole point of the
//  carried-sections behaviour above), so these go through the dry-parse hook.
// ---------------------------------------------------------------------------
static void ap_u8 (std::string& b, uint8_t v) { b += (char)v; }
static void ap_f32(std::string& b, float v) {
    char t[4]; std::memcpy(t, &v, 4); b.append(t, 4);
}
static void ap_str(std::string& b, const std::string& v) {
    ap_u32(b, (uint32_t)v.size()); b += v;
}

struct PtchAux { std::string name; float gain; bool mute; std::vector<uint32_t> inserts; };

// `auxes` empty + v17 == exactly the bytes a pre-aux release wrote.
static std::string make_ptch(unsigned version, int tracks,
                             const std::vector<PtchAux>& auxes,
                             const std::vector<uint8_t>& solo = {},
                             const std::vector<float>& sendLevels = {}) {
    std::string b;
    ap_u8 (b, 1);                 // modular
    ap_u8 (b, 0);                 // multithreaded
    ap_u32(b, 1); ap_u32(b, 2); ap_u32(b, 3); ap_u32(b, 4); ap_u32(b, 5);
    ap_u32(b, 0);                 // no virtual MIDI outputs
    ap_u32(b, (uint32_t)tracks);
    ap_f32(b, 1.0f);              // master gain
    for (int i = 0; i < tracks; ++i) {
        ap_u8 (b, 0);             // audio track
        ap_f32(b, 0.8f); ap_f32(b, 0.f); ap_u8(b, 0);
    }
    if (version >= 18) {
        ap_u32(b, (uint32_t)auxes.size());
        for (size_t a = 0; a < auxes.size(); ++a) {
            ap_u32(b, (uint32_t)(100 + a));            // the bus's saved node id
            ap_str(b, auxes[a].name);
            ap_f32(b, auxes[a].gain);
            ap_u8 (b, auxes[a].mute ? 1 : 0);
            ap_u32(b, (uint32_t)auxes[a].inserts.size());
            for (size_t k = 0; k < auxes[a].inserts.size(); ++k) {
                ap_u32(b, auxes[a].inserts[k]); ap_u8(b, 1); ap_u8(b, 1);
            }
        }
        for (int i = 0; i < tracks; ++i)
            ap_u8(b, (size_t)i < solo.size() ? solo[(size_t)i] : 0);
        for (int i = 0; i < tracks; ++i)
            for (size_t a = 0; a < auxes.size(); ++a) {
                const size_t ix = (size_t)i * auxes.size() + a;
                ap_f32(b, ix < sendLevels.size() ? sendLevels[ix] : 0.f);
                ap_u8 (b, 0);      // post-fader
                ap_u8 (b, 1);      // enabled
            }
    }
    ap_u32(b, 0);                 // no user nodes
    ap_u32(b, 0);                 // no connections
    return b;
}

static void check_aux_section_format() {
    std::printf("--- PTCH aux-bus section (v17 -> v18 migration) ---\n");

    // A file written before aux buses existed still parses, at its own version.
    CHECK(project_io_parse_patch_section(make_ptch(17, 3, {}).data(),
                                         make_ptch(17, 3, {}).size(), 17),
          "a v17 PTCH section (no aux block at all) still parses");

    // ...and the SAME bytes read as v18 must NOT parse: proof the aux block is
    // really gated on the version rather than being silently optional, which is
    // what would let a v17 file be misread as a v18 one.
    CHECK(!project_io_parse_patch_section(make_ptch(17, 3, {}).data(),
                                          make_ptch(17, 3, {}).size(), 18),
          "v17 bytes read as v18 are REJECTED (the aux block is version-gated)");

    // A v18 section with no buses is the same shape plus three zeros.
    const std::string none = make_ptch(18, 3, {});
    CHECK(project_io_parse_patch_section(none.data(), none.size(), 18),
          "a v18 PTCH section with zero aux buses parses");

    // Two buses, one carrying an insert chain, plus solo and a send matrix.
    std::vector<PtchAux> auxes;
    PtchAux a0; a0.name = "Reverb"; a0.gain = 0.7f; a0.mute = false; a0.inserts.push_back(42);
    PtchAux a1; a1.name = "Delay";  a1.gain = 1.0f; a1.mute = true;
    auxes.push_back(a0); auxes.push_back(a1);
    const std::vector<uint8_t> solo = { 0, 1, 0 };
    const std::vector<float> levels = { 0.5f, 0.f, 0.f, 0.25f, 1.f, 1.f };
    const std::string full = make_ptch(18, 3, auxes, solo, levels);
    CHECK(project_io_parse_patch_section(full.data(), full.size(), 18),
          "a v18 PTCH section with buses, inserts, solo and a send matrix parses");
    CHECK(full.size() > none.size(),
          "the aux block really is in the bytes (v18 payload is longer)");

    // Truncation anywhere inside the aux block must FAIL, never be read as a
    // shorter-but-valid section -- a corrupt count used to be able to wander.
    bool allRejected = true;
    for (size_t cut = none.size(); cut < full.size(); ++cut)
        if (project_io_parse_patch_section(full.data(), cut, 18)) { allRejected = false; break; }
    CHECK(allRejected, "every truncation inside the aux block is rejected");

    // An impossible bus count is refused rather than trusted.
    {
        std::string bad = make_ptch(18, 1, {});
        // the aux count is the u32 immediately after the per-track records
        const size_t auxCountAt = bad.size() - (4 /*conns*/ + 4 /*nodes*/
                                                + 1 /*solo*/ + 4 /*aux count*/);
        wr_u32(bad, auxCountAt, 0xFFFFFFFFu);
        CHECK(!project_io_parse_patch_section(bad.data(), bad.size(), 18),
              "an absurd aux-bus count is rejected, not trusted");
    }
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
        // A loop window that does NOT span the pattern: beats 1..3 of 4.
        s->set_loop_start(c_ppqn);
        s->set_loop_end(c_ppqn * 3);
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
        // ONE-SHOT: plays once per clip instead of repeating at its length.
        // Its window still spans the whole pattern ("no loop set").
        s->set_loop_enabled(false);
    }

    // seq 9 : "Odd" -- a loop window that OVERHANGS the END marker.  This is
    // the standard way to build an odd-length loop (a 3-beat loop over a 2-beat
    // phrase); sequence::set_length() hides such a window rather than destroying
    // it, so the round trip has to bring it back at its full extent.
    src.new_sequence(9);
    {
        sequence* s = src.get_sequence(9);
        s->set_name(std::string("Odd"));
        s->set_length(c_ppqn * 3, false);
        s->set_loop_start(c_ppqn / 2);
        s->set_loop_end(c_ppqn * 3);
        s->set_length(c_ppqn * 2, false);   // END marker pulled in under the window
        s->add_event(0,  EVENT_NOTE_ON,  48, 111);
        s->add_event(12, EVENT_NOTE_OFF, 48, 0);
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
        // Bump with PROJ_VERSION.  v20 == automation region loopStart (the loop
        // WINDOW's start, which v19 dropped, so a window beginning later than
        // tick 0 replayed its curve from the top after a reload);
        // v19 == per-region fade shape/slope; v18 == aux buses + the send matrix
        // + solo; v17 == per-region audio loop PERIOD; v16 == per-sequence loop
        // ENABLE.
        CHECK(formatVersion == 20, "project uses the V20 container format");
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
        CHECK(a->get_loop_start() == c_ppqn,     "seq 0 loop_start round-tripped");
        CHECK(a->get_loop_end()   == c_ppqn * 3, "seq 0 loop_end round-tripped");
        CHECK(a->get_loop_enabled(),             "seq 0 loop stays ENABLED");
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
        CHECK(!b->get_loop_enabled(),          "seq 5 stays a ONE-SHOT clip");
        CHECK(b->get_loop_start() == 0,        "seq 5 loop_start == 0 (spans pattern)");
        CHECK(b->get_loop_end() == c_ppqn * 8, "seq 5 loop_end == length (spans pattern)");
    }

    if (sequence* c = dst.get_sequence(9)) {
        CHECK(std::string(c->get_name()) == "Odd", "seq 9 name == Odd");
        CHECK(c->get_length() == c_ppqn * 2,     "seq 9 END marker == ppqn*2");
        CHECK(c->get_loop_start() == c_ppqn / 2, "seq 9 loop_start round-tripped");
        // The window OVERHANGS the END marker and must come back whole: the
        // restore used to clamp it to the length, quietly turning a 3-beat loop
        // into a 1.5-beat one on every File > Open AND on every undo.
        CHECK(c->get_loop_end() == c_ppqn * 3,
              "seq 9 loop_end past the END marker survives the round trip");
        CHECK(c->get_loop_enabled(),             "seq 9 loop enabled");
    } else {
        CHECK(false, "seq 9 fetchable");
    }

    CHECK(dst.get_scale_master() == 0, "perform scale-master index == 0");
    const auto& restoredLayout = project_io_loaded_patch_layout();
    CHECK(restoredLayout.size() == 2, "patch node layout round-tripped");
    CHECK(restoredLayout.size() == 2 && restoredLayout[0].nodeId == 11 &&
          restoredLayout[0].x == 123.5 && restoredLayout[0].y == 45.25,
          "first patch node coordinates round-tripped");

    // ---- undo / redo --------------------------------------------------------
    // main.cpp's project-wide history ring IS save_project + load_project (see
    // snapshot_state() / restore_state()): every Undo re-loads a serialized
    // whole-project state.  A round trip that degrades by one step per pass
    // would therefore erode the loop window a little on EVERY undo, of any
    // edit, anywhere in the project.  Re-serialize what we just loaded and load
    // it again: the file must be a fixed point, byte for byte.
    {
        const std::string again = path + ".again";
        CHECK(save_project(dst, again, patchLayout), "re-saving a loaded project works");
        std::ifstream f1(path.c_str(), std::ios::binary), f2(again.c_str(), std::ios::binary);
        const std::string d1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        const std::string d2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        CHECK(d1 == d2, "save -> load -> save is a fixed point (undo cannot erode state)");

        perform third;
        if (load_project(third, again)) {
            sequence* t0 = third.get_sequence(0);
            sequence* t5 = third.get_sequence(5);
            sequence* t9 = third.get_sequence(9);
            CHECK(t0 && t0->get_loop_start() == c_ppqn && t0->get_loop_end() == c_ppqn * 3,
                  "loop window survives a second undo-style restore");
            CHECK(t5 && !t5->get_loop_enabled(),
                  "one-shot flag survives a second undo-style restore");
            CHECK(t9 && t9->get_loop_end() == c_ppqn * 3 && t9->get_length() == c_ppqn * 2,
                  "overhanging window survives a second undo-style restore");
        } else {
            CHECK(false, "re-saved project loads");
        }
        std::remove(again.c_str());
    }

    // ---- backward compatibility -------------------------------------------
    // Rewrite the saved file the way v15 / v14 / v13 wrote it and re-load it.
    // Only the "SEQ " payloads differ across those versions (this head-less
    // save emits GLOB / SEQ / PUI / END and nothing else), so every other
    // section is copied through byte for byte.
    for (int target = 15; target >= 13; --target) {
        char tail[8];
        std::snprintf(tail, sizeof tail, ".v%d", target);
        const std::string downPath = path + tail;
        CHECK(downgrade_project(path, downPath, (unsigned)target),
              std::string("wrote a v" + std::to_string(target) + " project").c_str());

        perform old;
        const bool ok = load_project(old, downPath);
        CHECK(ok, std::string("v" + std::to_string(target) +
                              " project still loads").c_str());
        if (ok) {
            sequence* oa = old.get_sequence(0);
            sequence* ob = old.get_sequence(5);
            CHECK(oa && ob, "old project sequences fetchable");
            if (oa && ob) {
                CHECK(notes_of(oa).size() == 2, "old project keeps seq 0's notes");
                CHECK(oa->get_length() == c_ppqn * 4, "old project keeps seq 0's length");
                // A pre-v16 pattern had no choice but to repeat: the historical
                // behaviour is loop ENABLED, whatever the window says.
                CHECK(oa->get_loop_enabled() && ob->get_loop_enabled(),
                      "pre-v16 patterns load with the loop ENABLED");
                if (target >= 15) {
                    CHECK(oa->get_loop_start() == c_ppqn &&
                          oa->get_loop_end() == c_ppqn * 3,
                          "v15 loop bounds survive intact");
                } else if (target == 14) {
                    // v14 stored only the start; the end defaults to the length.
                    CHECK(oa->get_loop_start() == c_ppqn &&
                          oa->get_loop_end() == c_ppqn * 4,
                          "v14 loop_start survives, loop_end defaults to length");
                } else {
                    // v13 and older have no loop fields at all -> full span.
                    CHECK(oa->get_loop_start() == 0 &&
                          oa->get_loop_end() == c_ppqn * 4,
                          "v13 pattern loads with a full-span loop window");
                }
            }
        }
        std::remove(downPath.c_str());
    }

    // There is deliberately no compatibility parser for v1-v7. A valid current
    // file carrying any older version number must be rejected before state
    // changes.
    const std::string oldPath=path+".v7";
    {
        std::ifstream in(path,std::ios::binary);
        std::ofstream out(oldPath,std::ios::binary|std::ios::trunc);
        out<<in.rdbuf();
    }
    {
        std::fstream old(oldPath,std::ios::binary|std::ios::in|std::ios::out);
        const unsigned char v7[4]={7,0,0,0};
        old.seekp(8); old.write(reinterpret_cast<const char*>(v7),4);
    }
    perform rejected;
    CHECK(!load_project(rejected,oldPath),"v1-v7 projects are rejected");
    std::remove(oldPath.c_str());

    // =======================================================================
    //  A save with the audio engine DOWN must not strip the project.
    //
    //  main.cpp does not exit when audio_app_init() fails (busy device,
    //  exclusive-mode ASIO), so the whole GUI runs with no engine -- and in that
    //  state save_project used to omit "TRAK", "PTCH" and "AUDI" entirely,
    //  return true, and let the status bar say "Saved".  Open a project, save,
    //  and the modular patch, the rack, every embedded audio clip and all
    //  plugin state were gone from the file.  Undo/redo round-trips whole
    //  project files, so ONE edit in that state destroyed them irrecoverably.
    //
    //  There is no engine here either, which is exactly the condition under
    //  test: splice real sections into a saved file and re-save over it.
    // =======================================================================
    const std::string carried = path + ".carried";
    // A well-formed TRAK payload (the loader parses TRAK even with no graph):
    //   u32 track, f32 gain, f32 pan, u8 mute, u8 solo, u8 disabled,
    //   u8 hasInstrument = 0, u32 fxCount = 0
    std::string trakPayload;
    ap_u32(trakPayload, 3);
    { const float g = 0.5f, pn = -0.25f; char b[4];
      std::memcpy(b, &g,  4); trakPayload.append(b, 4);
      std::memcpy(b, &pn, 4); trakPayload.append(b, 4); }
    trakPayload += (char)1;                    // mute
    trakPayload += (char)0;                    // solo
    trakPayload += (char)1;                    // disabled (a FROZEN track)
    trakPayload += (char)0;                    // no instrument
    ap_u32(trakPayload, 0);                    // no inserts
    const std::string trakSec = make_section("TRAK", trakPayload);
    // PTCH / AUDI are opaque to a head-less load, so their exact bytes are
    // irrelevant -- what matters is that the saver keeps them without needing
    // to understand them.
    const std::string ptchSec = make_section("PTCH", std::string(96, '\xA5'));
    const std::string audiSec = make_section("AUDI", std::string(48, '\x5A'));
    {
        const std::string spliced =
            splice_before_end(read_all(path), trakSec + ptchSec + audiSec);
        CHECK(!spliced.empty() && write_all(carried, spliced),
              "built a project carrying TRAK / PTCH / AUDI sections");

        perform q;
        CHECK(load_project(q, carried),
              "a project with engine sections loads with no audio engine");

        const std::string before = read_all(carried);
        const bool resaved = save_project(q, carried, patchLayout);
        CHECK(resaved, "head-less save over that project succeeds");
        if (!resaved) std::printf("  err: %s\n", project_io_last_error());

        const std::string after = read_all(carried);
        CHECK(after.find(trakSec) != std::string::npos,
              "TRAK survives a save with the audio engine down");
        CHECK(after.find(ptchSec) != std::string::npos,
              "PTCH (modular patch + rack) survives a save with the engine down");
        CHECK(after.find(audiSec) != std::string::npos,
              "AUDI (embedded audio clips) survives a save with the engine down");

        // ...and the result is still a project, not just a file with the old
        // bytes stapled on.
        perform q2;
        CHECK(load_project(q2, carried), "the re-saved project still loads");
        CHECK(q2.is_active(0) && q2.is_active(5) && q2.is_active(9),
              "the re-saved project still has its sequences");

        // Fix 2's observable half: the previous contents are kept in a .bak and
        // no .tmp is left lying around.
        CHECK(read_all(carried + ".bak") == before,
              "saving over a project leaves the previous bytes in a .bak");
        CHECK(!file_exists(carried + ".tmp"), "no .tmp is left behind by a save");
    }

    // An OLDER container cannot have its engine sections re-stamped as current
    // ones (their layout is gated on the version), so the save is REFUSED --
    // never silently completed without them.
    {
        const std::string oldCarried = path + ".carried15";
        std::string d = read_all(carried);
        wr_u32(d, 8, 15);                       // restamp the container version
        CHECK(write_all(oldCarried, d), "built an older-format project with engine sections");
        const std::string before = read_all(oldCarried);

        perform q;
        CHECK(load_project(q, carried), "reloaded a session to save from");
        CHECK(!save_project(q, oldCarried, patchLayout),
              "engine down + engine sections in an older format: the save is REFUSED");
        CHECK(read_all(oldCarried) == before,
              "the refused save left the file on disk byte-identical");
        CHECK(!file_exists(oldCarried + ".tmp"), "the refused save left no .tmp");
        CHECK(!file_exists(oldCarried + ".bak"),
              "the refused save did not even start (no .bak)");
        std::remove(oldCarried.c_str());
        std::remove((oldCarried + ".bak").c_str());
    }

    // A stale .bak next to a path that does not exist must never be resurrected.
    {
        const std::string fresh = path + ".fresh";
        std::remove(fresh.c_str());
        CHECK(write_all(fresh + ".bak", std::string("NOT-A-PROJECT")),
              "planted a stale .bak beside a non-existent project");
        perform q;
        CHECK(load_project(q, path) && save_project(q, fresh, patchLayout),
              "saving to a fresh path succeeds");
        perform q2;
        CHECK(load_project(q2, fresh) && q2.is_active(0),
              "the fresh save is a real project, not the stale .bak");
        std::remove((fresh + ".bak").c_str());
        std::remove(fresh.c_str());
    }

    // =======================================================================
    //  A REJECTED load must leave the open session exactly as it was.
    //
    //  The loader used to delete every sequence and reset every track BEFORE it
    //  parsed a single section, so any later failure returned false on a
    //  project that no longer existed in memory.  Undo/redo IS load_project
    //  (main.cpp restore_state), so one damaged history file wiped the session.
    // =======================================================================
    {
        const std::string good = read_all(carried);
        std::vector<SectionAt> secs = sections_of(good);
        size_t seqOff = 0;
        for (size_t i = 0; i < secs.size(); ++i)
            if (secs[i].tag == "SEQ ") { seqOff = secs[i].off; break; }
        CHECK(seqOff != 0, "found a SEQ section to damage");

        // (a) a section header that claims more payload than the file holds
        std::string truncated = good;
        wr_u32(truncated, seqOff + 4, 0x7000000u);
        // (b) an event count that runs past the END of its own section -- this
        //     one only fails DEEP into the parse, after earlier sections have
        //     already been staged
        std::string blownCount = good;
        wr_u32(blownCount, seq_event_count_offset(good, seqOff), 0x00100000u);
        // (c) a file cut in half
        std::string chopped = good.substr(0, 12 + (good.size() - 12) / 2);

        struct Damaged { const char* what; const std::string* bytes; };
        const Damaged cases[] = {
            { "a section claiming more payload than the file has", &truncated },
            { "a SEQ event count running past its own section",    &blownCount },
            { "a file cut in half",                                &chopped },
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const std::string badPath = path + ".bad";
            CHECK(write_all(badPath, *cases[i].bytes),
                  std::string("wrote a project with ").append(cases[i].what).c_str());

            // A live session with real work in it.
            perform live;
            CHECK(load_project(live, carried), "opened a session to protect");

            const bool ok = load_project(live, badPath);
            CHECK(!ok, std::string("load rejects ").append(cases[i].what).c_str());
            CHECK(live.is_active(0) && live.is_active(5) && live.is_active(9),
                  "the OPEN session survives the rejected load");
            sequence* s0 = live.get_sequence(0);
            CHECK(s0 && std::string(s0->get_name()) == "Bass" &&
                  notes_of(s0).size() == 2 && s0->get_loop_end() == c_ppqn * 3,
                  "...with its name, notes and loop window intact");
            std::remove(badPath.c_str());
        }
    }
    std::remove((carried + ".bak").c_str());
    std::remove(carried.c_str());

    // =======================================================================
    //  Track defaults applied by a load.
    //
    //  "TRAK" writes and restores `disabled`, but the load-time reset cleared
    //  everything EXCEPT that flag.  Freeze a track, then open a project with
    //  no TRAK for that index: the track looked completely normal in the mixer
    //  and produced no sound.
    // =======================================================================
    {
        PatchKnob::engine::Track trk;
        trk.setGain(0.2f); trk.setPan(0.7f);
        trk.setMute(true); trk.setSolo(true); trk.setDisabled(true);
        project_io_reset_track_defaults(trk);
        CHECK(trk.gain() == 1.0f && trk.pan() == 0.0f,
              "load-time track reset restores unity gain / centre pan");
        CHECK(!trk.mute() && !trk.solo(), "load-time track reset clears mute + solo");
        CHECK(!trk.disabled(),
              "load-time track reset clears the FREEZE (disabled) flag");
        CHECK(trk.instrument() == nullptr && trk.fxCount() == 0,
              "load-time track reset drops the instrument and inserts");
    }

    // =======================================================================
    //  Stand-alone rack patches (.pkr): the save must not be able to destroy
    //  the previous file, and a damaged one must not destroy the open rack.
    // =======================================================================
    {
        const std::string rp = path + ".pkr";
        rackx::RackEngine rack;
        rack.ensureDefaultIO();
        const int vco = rack.addModule("VCO", 10.0f, 20.0f);
        const int vca = rack.addModule("VCA", 200.0f, 20.0f);
        rack.setPolyphony(4);
        const int modules = rack.moduleCount();
        CHECK(vco >= 0 && vca >= 0 && modules >= 2, "built a rack to save");

        CHECK(save_rack_patch(rack, rp), "rack patch saved");
        CHECK(!file_exists(rp + ".tmp"), "the rack patch save left no .tmp");
        const std::string savedBytes = read_all(rp);
        CHECK(savedBytes.size() > 12 && savedBytes.compare(0, 8, "PKRACK01") == 0,
              "the rack patch on disk is complete");

        // Overwriting keeps the previous file in a .bak instead of truncating
        // the target in place.
        CHECK(save_rack_patch(rack, rp), "rack patch re-saved over itself");
        CHECK(read_all(rp + ".bak") == savedBytes,
              "re-saving a rack patch keeps the previous one in a .bak");

        rackx::RackEngine rack2;
        rack2.ensureDefaultIO();
        CHECK(load_rack_patch(rack2, rp), "rack patch loads");
        CHECK(rack2.moduleCount() == modules && rack2.polyphony() == 4,
              "rack modules + polyphony round-tripped");

        const std::string damagedPath = rp + ".damaged";
        CHECK(write_all(damagedPath, savedBytes.substr(0, 12 + (savedBytes.size() - 12) / 2)),
              "wrote a truncated rack patch");
        CHECK(!load_rack_patch(rack2, damagedPath), "a damaged rack patch is rejected");
        CHECK(rack2.moduleCount() == modules,
              "the OPEN rack survives a rejected .pkr load");

        std::remove(damagedPath.c_str());
        std::remove((rp + ".bak").c_str());
        std::remove(rp.c_str());
    }

    check_aux_section_format();

    std::printf("\n%s  (%d failures)\n", g_fail == 0 ? "ALL PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
