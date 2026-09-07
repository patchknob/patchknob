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
#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/audio_clip_player.h"
#include "audio_app.h"   // real engine transport for the song-loop stall test
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include "engine/audio/audio_engine.h"
#include <cmath>

#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>

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

//----------------------------------------------------------------------------
//  --selftest : drive the view's input paths synthetically (no display
//  needed -- run with SDL_VIDEODRIVER=dummy) and assert on the ch.29/30
//  model: mode block clicks, Shuffle Lock, tool keys, the Selector's edit
//  selection + timeline link, Tab-to-boundary, nudging and zoom presets.
//----------------------------------------------------------------------------
static int g_fails = 0;
#define CHECK(cond, name) do { \
    if (cond) std::printf("PASS  %s\n", name); \
    else      { std::printf("FAIL  %s\n", name); ++g_fails; } \
} while (0)

static void click(App& app, arrange::ArrangeView& v, int x, int y,
                  int button = SDL_BUTTON_LEFT)
{
    v.on_mouse(app, MouseEv{ x, y, button, true });
    v.on_mouse(app, MouseEv{ x, y, button, false });
}

//============================================================================
//  MIDI CLIP DATA-ALIGNMENT AUDIT
//
//  THE QUESTION.  After a clip is copied, cut, sliced, moved and pasted, does
//  every surviving note still SOUND at the same absolute timeline tick, with
//  the same pitch, velocity, length and routing as before the edit?
//
//  "Sound" is meant literally.  The schedule compared here is captured from
//  g_seq_emit_tap (sequence.h) -- the engine's own diagnostic hook, which every
//  MIDI byte a sequence produces passes through -- while
//  sequence::play_triggered() is driven over the timeline, which is the call
//  perform's output thread makes.  Nothing here re-implements play_span's
//  repetition layout: the ground truth is what the clip actually emitted BEFORE
//  it was cut, and every later comparison is against that recording.
//
//  SEEDED.  kSeed is printed at the top of the section so a failure is
//  reproducible.  The PRNG is written out longhand because std::mt19937's
//  DISTRIBUTIONS are not required to yield the same numbers on two standard
//  libraries -- a "seeded" test built on them is not reproducible across the
//  Linux and MinGW builds this project ships.  A xorshift and a raw modulo
//  are.
//
//  THE CONTRACT AT A CUT (asserted, not merely accepted -- see expect()):
//    * a note whose ONSET is inside a piece sounds at its recorded pitch,
//      velocity, bus and channel, moved by the piece's displacement;
//    * a note held ACROSS a cut is TRUNCATED at the cut: it is released on the
//      piece's last tick (one tick before the next piece's first, so the two
//      can never overlap) and is NOT re-struck by the next piece.  A cut
//      shortens a note; it never duplicates it and never drops it.
//============================================================================
namespace align {

// ------------------------------------------------------------------ RNG ---
struct Rng {
    unsigned s;
    explicit Rng(unsigned seed) : s(seed ? seed : 1u) {}
    unsigned next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int pick(int lo, int hi) { return lo + (int)(next() % (unsigned)(hi - lo + 1)); }
};

// ------------------------------------------ what the sequencer emitted -----
//  g_seq_emit_tap (sequence.h) is the engine's own diagnostic hook: every MIDI
//  byte a sequence produces -- pattern notes, retrigger releases, trigger-end
//  releases, panic offs and loop-boundary offs -- passes through it, carrying
//  the ABSOLUTE tick the audio thread will place it at.  It is the only way to
//  see what a clip SOUNDS rather than what the arrangement model claims.
struct Emit { long tick; int bus; int chan; int status; int d0; int d1; };
static std::vector<Emit>* g_cap = 0;
static void tap(sequence* sq, int bus, int chan, unsigned char st,
                unsigned char note, unsigned char vel, long tick, int kind)
{
    (void)sq; (void)kind;
    if (!g_cap) return;
    Emit e; e.tick = tick; e.bus = bus; e.chan = chan;
    e.status = (int)(st & 0xF0); e.d0 = (int)note; e.d1 = (int)vel;
    g_cap->push_back(e);
}

// ------------------------------------------------------- a sounding note ---
struct Note {
    long tick;      // ABSOLUTE timeline tick of the note-on
    int  pitch, vel;
    long dur;       // note-off tick - note-on tick (-1 == never released)
    int  bus, chan;
};
static bool operator==(const Note& a, const Note& b)
{
    return a.tick == b.tick && a.pitch == b.pitch && a.vel == b.vel &&
           a.dur == b.dur && a.bus == b.bus && a.chan == b.chan;
}
static bool operator<(const Note& a, const Note& b)
{
    if (a.tick  != b.tick ) return a.tick  < b.tick;
    if (a.pitch != b.pitch) return a.pitch < b.pitch;
    if (a.vel   != b.vel  ) return a.vel   < b.vel;
    if (a.dur   != b.dur  ) return a.dur   < b.dur;
    if (a.bus   != b.bus  ) return a.bus   < b.bus;
    return a.chan < b.chan;
}

static int g_hung = 0;      // note-ons the render never released (a hung note)

//  Play `seqs` through the REAL playback path over [t0,t1] and pair every
//  note-on with its note-off.  Pairing is done per sequence, so two clips that
//  happen to share a pitch on one bus can never cross-pair.
static std::vector<Note> render(perform& p, const std::vector<int>& seqs,
                                long t0, long t1)
{
    std::vector<Note> out;
    g_hung = 0;
    for (size_t k = 0; k < seqs.size(); ++k) {
        if (!p.is_active(seqs[k])) continue;
        sequence* s = p.get_sequence(seqs[k]);
        if (!s) continue;
        s->set_playing(false);              // flush any state a prior render left
        std::vector<Emit> ev;
        g_cap = &ev;
        g_seq_emit_tap = &tap;
        s->play_triggered(t0, t1);
        g_seq_emit_tap = 0;
        g_cap = 0;
        std::map<int, std::vector<size_t> > open;   // (bus,chan,pitch) -> pending
        std::vector<Note> mine;
        for (size_t i = 0; i < ev.size(); ++i) {
            const Emit& e = ev[i];
            const int key = (e.bus << 12) | (e.chan << 8) | e.d0;
            if (e.status == 0x90 && e.d1 > 0) {
                Note n; n.tick = e.tick; n.pitch = e.d0; n.vel = e.d1;
                n.dur = -1; n.bus = e.bus; n.chan = e.chan;
                mine.push_back(n);
                open[key].push_back(mine.size() - 1);
            } else if (e.status == 0x80 || (e.status == 0x90 && e.d1 == 0)) {
                std::vector<size_t>& q = open[key];
                if (q.empty()) continue;            // an off with no on: ignore
                mine[q.front()].dur = e.tick - mine[q.front()].tick;
                q.erase(q.begin());
            }
        }
        for (size_t i = 0; i < mine.size(); ++i) {
            if (mine[i].dur < 0) ++g_hung;
            out.push_back(mine[i]);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

//  A piece of the ORIGINAL timeline, relocated: it carries original ticks
//  [lo,hi) and now begins at `at`.  Splitting, moving and pasting are all
//  expressed as pieces, so one model covers every edit.
struct Piece {
    long lo, hi, at;
    int  bus, chan;     // -1 = unchanged.  A paste onto ANOTHER lane is built
                        // on the destination lane's pattern, so it takes that
                        // lane's routing -- the notes must not move, but they
                        // do change bus/channel.
    Piece(long l, long h, long a, int b = -1, int c = -1)
        : lo(l), hi(h), at(a), bus(b), chan(c) {}
};

//  The schedule a set of pieces MUST produce, derived from the ground-truth
//  recording of the uncut clip.  See the contract at the top of this section.
static std::vector<Note> expect(const std::vector<Note>& snap,
                                const std::vector<Piece>& pieces)
{
    std::vector<Note> out;
    for (size_t p = 0; p < pieces.size(); ++p) {
        const Piece& q = pieces[p];
        for (size_t i = 0; i < snap.size(); ++i) {
            const Note& n = snap[i];
            if (n.tick < q.lo || n.tick >= q.hi) continue;
            Note m = n;
            m.tick = n.tick + (q.at - q.lo);
            const long cap = (q.hi - 1) - n.tick;   // released on the piece's last tick
            if (m.dur > cap) m.dur = cap;
            if (q.bus  >= 0) m.bus  = q.bus;
            if (q.chan >= 0) m.chan = q.chan;
            out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

//  Split the piece containing timeline tick `t`.  Mirrors what a Separate does
//  to the arrangement, in original-timeline coordinates.
static bool split_piece(std::vector<Piece>& v, long t)
{
    for (size_t i = 0; i < v.size(); ++i) {
        const long len = v[i].hi - v[i].lo;
        if (t <= v[i].at || t >= v[i].at + len) continue;
        const long d = t - v[i].at;
        Piece right(v[i].lo + d, v[i].hi, t, v[i].bus, v[i].chan);
        v[i].hi = v[i].lo + d;
        v.insert(v.begin() + (long)i + 1, right);
        return true;
    }
    return false;
}

//  Compare, and on failure say exactly which note moved -- with the seed, so
//  the case can be reproduced.
static bool same(const std::vector<Note>& exp, const std::vector<Note>& got,
                 unsigned seed)
{
    size_t i = 0, j = 0;
    int shown = 0;
    bool ok = true;
    while (i < exp.size() || j < got.size()) {
        if (i < exp.size() && j < got.size() && exp[i] == got[j]) { ++i; ++j; continue; }
        ok = false;
        if (shown++ < 8) {
            const bool takeExp = (j >= got.size()) ||
                                 (i < exp.size() && exp[i] < got[j]);
            const Note& n = takeExp ? exp[i] : got[j];
            std::printf("      seed=0x%08x  %-8s tick=%ld pitch=%d vel=%d dur=%ld bus=%d ch=%d\n",
                        seed, takeExp ? "MISSING" : "EXTRA",
                        n.tick, n.pitch, n.vel, n.dur, n.bus, n.chan);
        }
        if (j >= got.size()) { ++i; continue; }
        if (i >= exp.size()) { ++j; continue; }
        if (exp[i] < got[j]) ++i; else ++j;
    }
    if (!ok && shown > 8)
        std::printf("      ... %d further differences\n", shown - 8);
    if (!ok)
        std::printf("      (%u notes expected, %u sounded)\n",
                    (unsigned)exp.size(), (unsigned)got.size());
    return ok;
}

//  16th notes at varying pitches with 8ths mixed in and a sustained note every
//  so often, so a cut has something to land inside.  Two notes never sound the
//  same pitch at the same time (short notes are detached and live in an octave
//  range the sustains never use), which keeps note-on/note-off pairing
//  unambiguous.  `barrier`, when > 0, is a tick no note may be held across
//  (used for the looping clip, whose loop window must not strand a note).
//  Returns the notes in PATTERN coordinates.
static std::vector<Note> build_pattern(sequence* s, long lengthTicks,
                                       Rng& rng, long barrier)
{
    const long S16 = (long)c_ppqn / 4;
    const long S8  = (long)c_ppqn / 2;
    std::vector<Note> notes;
    long t = 0, sustainEnd = -1;
    while (t < lengthTicks - 1) {
        const long step = (rng.pick(0, 9) < 3) ? S8 : S16;      // ~30% eighths
        long dur; int pitch;
        if (t >= sustainEnd && rng.pick(0, 5) == 0) {
            dur   = step * rng.pick(3, 5);                      // held across steps
            pitch = rng.pick(36, 47);
            sustainEnd = t + dur;
        } else {
            dur   = step * 3 / 4;                               // detached
            pitch = rng.pick(60, 83);
        }
        if (t + dur >= lengthTicks) dur = lengthTicks - 1 - t;
        if (barrier > 0 && t < barrier && t + dur >= barrier) dur = barrier - 1 - t;
        if (dur < 1) { t += step; continue; }
        const int vel = rng.pick(40, 120);
        s->add_note_velocity(t, dur, pitch, vel);
        Note n; n.tick = t; n.pitch = pitch; n.vel = vel; n.dur = dur;
        n.bus = s->get_midi_bus(); n.chan = s->get_midi_channel();
        notes.push_back(n);
        t += step;
    }
    s->verify_and_link();
    return notes;
}

// every active sequence in one lane's index block
static std::vector<int> block(perform& p, int base)
{
    std::vector<int> v;
    for (int i = base; i < base + 40; ++i) if (p.is_active(i)) v.push_back(i);
    return v;
}

// the sequence in a lane block whose trigger STARTS at `startTick`
static int seq_at(perform& p, int base, long startTick)
{
    for (int i = base; i < base + 40; ++i) {
        if (!p.is_active(i)) continue;
        sequence* s = p.get_sequence(i);
        s->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        while (s->get_next_trigger(&on, &off, &sel, &offs))
            if (on == startTick) return i;
    }
    return -1;
}

static int lane_clip_count(perform& p, int base)
{
    int n = 0;
    for (int i = base; i < base + 40; ++i)
        if (p.is_active(i)) n += p.get_sequence(i)->trigger_count();
    return n;
}

} // namespace align

//============================================================================
//  SONG-LOOP WRAP AUDIT (the TRANSPORT loop, not the clip loop).
//
//  THE QUESTION.  With the song loop on, does every pass of the loop sound
//  exactly the same notes?  The long-standing report is "silence for a few
//  loops, then it comes back" -- i.e. per-PASS damage that an aggregate count
//  would hide -- so every pass is compared individually against a ground-truth
//  render of [left, right).
//
//  WHAT IS DRIVEN.  The exact call sequence perform::output_func's
//  audio-paced branch makes at a wrap (perform.cpp):
//
//      play( right-1 );                    // finish the tail of this pass
//      reset_sequences( -1, true );        // queue boundary offs, zero markers
//      set_orig_ticks( left );             // rewind every sequence cursor
//      play( left + leftover );            // head of the next pass
//
//  with the between-wrap windows advanced in small jittered steps the way the
//  poll thread really does, across MANY wraps.  This exercises the sequencer
//  state a wrap must reset -- m_last_tick, m_playing, m_playing_notes[],
//  trigger cover state -- which no contiguous play_triggered() render touches.
//============================================================================
namespace songloop {

struct WEmit { sequence* sq; long tick; int status, note, vel, kind; };
static std::vector<WEmit>* cap = 0;
static void wtap(sequence* sq, int /*bus*/, int /*chan*/, unsigned char st,
                 unsigned char note, unsigned char vel, long tick, int kind)
{
    if (!cap) return;
    WEmit e; e.sq = sq; e.tick = tick; e.status = (int)(st & 0xF0);
    e.note = (int)note; e.vel = (int)vel; e.kind = kind;
    cap->push_back(e);
}

//  One note-on as compared across passes.
typedef std::pair<long, std::pair<int, int> > On;      // (tick, (note, vel))
static std::vector<On> ons_of(const std::vector<WEmit>& ev, sequence* sq)
{
    std::vector<On> v;
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].sq == sq && ev[i].kind == 0 &&
            ev[i].status == 0x90 && ev[i].vel > 0)
            v.push_back(On(ev[i].tick,
                           std::make_pair(ev[i].note, ev[i].vel)));
    return v;
}

//  Ground truth: what ONE clean uninterrupted pass over [L, R-1] sounds.
static std::vector<On> ground_truth(perform& p, sequence* sq, long L, long R)
{
    std::vector<WEmit> ev;
    p.reset_sequences();                    // pristine note bookkeeping
    cap = &ev; g_seq_emit_tap = &wtap;
    sq->play_triggered(L, R - 1);
    g_seq_emit_tap = 0; cap = 0;
    p.reset_sequences();                    // release what the pass left held
    std::vector<On> v = ons_of(ev, sq);
    std::sort(v.begin(), v.end());
    return v;
}

//  Drive perform through `wraps` passes of the song loop [L, R).  Mirrors the
//  audio-paced scheduler: a lookahead horizon advanced in jittered steps, the
//  tail+head pair fired once per pass, the wrap pulling the transport back.
//  If growRightAtWrap >= 0, the right marker is moved to newRight at that wrap
//  (through the same rebuild handshake output_func uses for a live loop-marker
//  edit) and the capture restarts, so the caller audits the passes AFTER the
//  edit.
static std::vector<WEmit> run(perform& p, long L, long R, long start,
                              int wraps, long lookahead, long step,
                              unsigned jitterSeed,
                              int growRightAtWrap = -1, long newRight = 0)
{
    std::vector<WEmit> ev;
    align::Rng rng(jitterSeed);
    p.set_left_tick(L);
    p.set_right_tick(R);
    p.set_looping(true);
    p.start(true);                          // song (playback) mode, no threads
    p.reset_sequences();
    p.set_orig_ticks(start);
    cap = &ev; g_seq_emit_tap = &wtap;

    long long t         = start;            // audible transport tick
    long long last      = start - 1;        // last scheduled horizon
    bool      tail_done = false;
    int       done      = 0;
    bool      moved     = false;

    while (done < wraps) {

        if (!moved && growRightAtWrap >= 0 && done >= growRightAtWrap) {
            //  Live loop-marker edit: output_func invalidates the queued
            //  future and rebuilds from the audible tick (loop_config_changed
            //  handshake in perform.cpp).
            moved = true;
            p.set_right_tick(newRight);
            R = p.get_right_tick(); L = p.get_left_tick();
            p.set_orig_ticks((long)t);
            last = t - 1;
            tail_done = false;
            ev.clear();                     // audit what follows the edit
        }

        const long long horizon = t + lookahead;

        if (horizon >= R) {
            if (!tail_done) {
                long long leftover = horizon - R;
                if (leftover > R - L) leftover = 0;
                p.play((long)(R - 1));
                p.reset_sequences(-1, true);
                p.set_orig_ticks(L);
                last = L + leftover;
                p.play((long)last);
                tail_done = true;
            }
            //  else: wait for the engine wrap, exactly like output_func
        } else {
            tail_done = false;
            if (horizon > last) { p.play((long)horizon); last = horizon; }
        }

        //  transport advances with poll jitter; the engine wraps sample-exact
        t += step / 2 + (long)(rng.next() % (unsigned)(step + 1));
        if (t >= R) {
            const long long over = t - R;
            //  a normal wrap keeps the sub-poll remainder; a transport that
            //  finds itself far past the loop end (loop switched on behind
            //  the playhead) relocates to the loop start exactly, which is
            //  what audio_render_device_inner's `room <= 0` branch does
            t = (over <= 2 * step) ? L + over : L;
            tail_done = false;              // wrap generation consumed
            ++done;
        }
    }

    g_seq_emit_tap = 0; cap = 0;
    p.stop();                               // inner_stop: reset_sequences()
    p.set_looping(false);
    return ev;
}

//  Split one sequence's note-ons into passes (the tick regresses exactly once
//  per wrap) and CHECK every complete pass against the ground truth.  The
//  first segment (partial: started mid-loop) and the last (truncated by the
//  end of the run) are not complete passes and are skipped.
static void audit(perform& p, sequence* sq, const std::vector<WEmit>& ev,
                  const std::vector<On>& want, int wraps, const char* name)
{
    std::vector<On> raw = ons_of(ev, sq);   // arrival order
    std::vector<std::vector<On> > passes;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (passes.empty() || (!passes.back().empty() &&
                               raw[i].first < passes.back().back().first))
            passes.push_back(std::vector<On>());
        passes.back().push_back(raw[i]);
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "song loop[%s]: %d wraps produce %d+ passes",
                  name, wraps, wraps - 1);
    CHECK((int)passes.size() >= wraps - 1, msg);

    int badPass = -1, checked = 0;
    for (size_t k = 1; k + 1 < passes.size(); ++k) {
        std::vector<On> got = passes[k];
        std::sort(got.begin(), got.end());
        ++checked;
        if (got != want && badPass < 0) {
            badPass = (int)k;
            //  say exactly what differs, so a failure names the damage
            size_t shown = 0;
            for (size_t i = 0; i < want.size() && shown < 6; ++i) {
                bool found = false;
                for (size_t j = 0; j < got.size(); ++j)
                    if (got[j] == want[i]) { found = true; break; }
                if (!found) {
                    std::printf("      pass %d MISSING tick=%ld note=%d vel=%d\n",
                                (int)k, want[i].first, want[i].second.first,
                                want[i].second.second);
                    ++shown;
                }
            }
            for (size_t j = 0; j < got.size() && shown < 12; ++j) {
                bool found = false;
                for (size_t i = 0; i < want.size(); ++i)
                    if (got[j] == want[i]) { found = true; break; }
                if (!found) {
                    std::printf("      pass %d EXTRA   tick=%ld note=%d vel=%d\n",
                                (int)k, got[j].first, got[j].second.first,
                                got[j].second.second);
                    ++shown;
                }
            }
            std::printf("      (pass %d: %u ons, ground truth %u)\n", (int)k,
                        (unsigned)got.size(), (unsigned)want.size());
        }
    }
    std::snprintf(msg, sizeof msg,
                  "song loop[%s]: every complete pass (%d audited) matches the "
                  "ground truth (first bad: %d)", name, checked, badPass);
    CHECK(badPass < 0 && checked > 0, msg);
}

} // namespace songloop

//----------------------------------------------------------------------------
//  The audit.  Every CHECK compares a RENDER of the arrangement against the
//  ground-truth recording taken before the first edit.
//----------------------------------------------------------------------------
static void align_audit(App& app, perform& perf, arrange::ArrangeView& view)
{
    using namespace align;
    //  Fixed and printed, so any failure below is reproducible verbatim.
    //  PATCHKNOB_ALIGN_SEED re-runs the whole audit on different material --
    //  the checks are all relative to the recording, so any seed must pass.
    unsigned kSeed = 0x5eed1234u;
    if (const char* sv = std::getenv("PATCHKNOB_ALIGN_SEED"))
        kSeed = (unsigned)std::strtoul(sv, 0, 0);
    Rng rng(kSeed);

    const long S16    = (long)c_ppqn / 4;         // a 16th, derived from c_ppqn
    const long S8     = (long)c_ppqn / 2;         // an 8th
    const long BARt   = (long)c_ppqn * 4;         // 4/4 bar
    const long PATLEN = 4 * BARt;                 // four bars of data
    const long TEND   = 120 * BARt;               // render window: past everything

    std::printf("---- clip data-alignment audit: seed 0x%08x, c_ppqn=%d, "
                "16th=%ld, 8th=%ld, bar=%ld ----\n",
                kSeed, c_ppqn, S16, S8, BARt);

    char nm[220];

    // NOTHING SNAPS in this section: a cut and a paste have to be able to land
    // on an odd tick, which is where a folded offset shows itself.
    view.on_key(app, SDLK_F2);                          // Slip
    if (view.dbg_snap_to_grid()) {
        SDL_SetModState(KMOD_LSHIFT);
        view.on_key(app, SDLK_F4);
        SDL_SetModState(KMOD_NONE);
    }
    CHECK(view.dbg_edit_mode() == 1 && !view.dbg_snap_to_grid(),
          "alignment audit runs UNSNAPPED (Slip mode, Snap To Grid off)");

    // five lanes, one 40-sequence block each
    view.on_track_key = [](int s) -> int {
        if (s >= 100 && s < 140) return 100000 + 90;
        if (s >= 140 && s < 180) return 100000 + 91;
        if (s >= 180 && s < 220) return 100000 + 92;
        if (s >= 220 && s < 260) return 100000 + 93;
        if (s >= 260 && s < 300) return 100000 + 94;
        return s;
    };
    view.dbg_set_focus_lane(-1);
    view.dbg_unselect_all_clips();

    //  Mirrors sdlui/main.cpp's on_create_pattern EXACTLY -- the shell's real
    //  hook.  copy_events clones through sequence::operator=; otherwise the
    //  pattern is built empty carrying the TARGET lane's shape and routing (the
    //  cross-lane paste path).  A slice is allocated inside its source's own
    //  40-sequence block so it stays on the lane it was cut from, which is what
    //  g_seqToTrack does in the shell.
    view.on_create_pattern = [&perf](int src, long start, long length,
                                     long offset, bool copyEvents) -> int {
        if (!perf.is_active(src)) return -1;
        sequence* s = perf.get_sequence(src);
        if (!s) return -1;
        const int base = (src >= 100 && src < 300) ? 100 + ((src - 100) / 40) * 40 : 0;
        int idx = -1;
        for (int i = base; i < base + 40; ++i)
            if (!perf.is_active(i)) { idx = i; break; }
        if (idx < 0) return -1;
        perf.new_sequence(idx);
        perf.set_active(idx, true);
        sequence* d = perf.get_sequence(idx);
        if (!d) return -1;
        if (copyEvents) {
            *d = *s;
        } else {
            d->set_length(s->get_length(), false);
            d->set_midi_bus(s->get_midi_bus());
            d->set_midi_channel(s->get_midi_channel());
            d->set_track_kind(s->get_track_kind());
            d->set_arrange_lane_id(s->get_arrange_lane_id());
            d->set_loop_end(s->get_loop_end());
            d->set_loop_start(s->get_loop_start());
            d->set_loop_enabled(s->get_loop_enabled());
        }
        d->clear_triggers();
        if (length < 1) length = d->get_length() > 0 ? d->get_length() : c_ppqn * 4;
        d->add_trigger(start, length, offset, false);
        d->set_name(std::string(s->get_name() ? s->get_name() : "P") +
                    (copyEvents ? " copy " : " pat ") + std::to_string(idx));
        return idx;
    };

    // -------- small drivers over the REAL input paths -----------------------
    auto select_only = [&](int seq, long tick) {
        view.dbg_unselect_all_clips();
        if (perf.is_active(seq)) perf.get_sequence(seq)->select_trigger(tick);
    };
    auto ctrl_key = [&](SDL_Keycode k) {
        SDL_SetModState(KMOD_LCTRL);
        view.on_key(app, k);
        SDL_SetModState(KMOD_NONE);
    };
    // Ctrl+V pastes at the edit insertion point; a zero-width selection IS that
    // point and is not a RANGE, so Ctrl+C stays an object copy.
    auto paste_at = [&](long at) { view.dbg_select(at, at, -1, -1); ctrl_key(SDLK_v); };

    //========================================================================
    //  A -- ONE-SHOT clip, cut into sections at every awkward point.
    //========================================================================
    const int A = 100;
    perf.new_sequence(A); perf.set_active(A, true);
    sequence* sa = perf.get_sequence(A);
    sa->set_name("align-oneshot");
    sa->set_midi_bus(2); sa->set_midi_channel(3);
    sa->set_length(PATLEN, false);
    sa->set_loop_enabled(false);                        // ONE-SHOT
    const std::vector<Note> patA = build_pattern(sa, PATLEN, rng, 0);
    const long A0 = 2 * BARt + 5 * S16;                 // clip start, off the bar line
    sa->clear_triggers();
    sa->add_trigger(A0, PATLEN, 0, false);

    std::vector<Note> snapA = render(perf, block(perf, 100), 0, TEND);
    {
        std::vector<Note> want;
        for (size_t i = 0; i < patA.size(); ++i) {
            Note n = patA[i]; n.tick += A0;
            const long cap = (A0 + PATLEN - 1) - n.tick;
            if (n.dur > cap) n.dur = cap;
            want.push_back(n);
        }
        std::sort(want.begin(), want.end());
        std::printf("      pattern: %u notes over %ld ticks, clip at %ld\n",
                    (unsigned)patA.size(), PATLEN, A0);
        CHECK(patA.size() >= 30 && g_hung == 0 && same(want, snapA, kSeed),
              "GROUND TRUTH: the uncut one-shot sounds every note it was given, "
              "at its own absolute tick");
    }

    // a cut has to be able to land INSIDE a held note and exactly ON a note-on
    size_t li = 0;
    for (size_t i = 1; i < patA.size(); ++i) if (patA[i].dur > patA[li].dur) li = i;
    const long sustainMid = A0 + patA[li].tick + patA[li].dur / 2;
    const long onNote     = A0 + patA[patA.size() / 2].tick;

    struct Cut { long t; const char* what; };
    //  Two of these ticks are derived from the RANDOM material, so on some
    //  seeds they can coincide with a fixed cut.  A cut on a tick that is
    //  already a clip boundary is a no-op (split_clip_at refuses t <= start),
    //  which would test nothing -- shift a collision by a tick instead.  Every
    //  cut still lands where its name says: one tick either way is still inside
    //  the same held note, still off the grid, still on the same bar.
    std::vector<Cut> cutsA;
    {
        const Cut want[] = {
            { 3 * BARt,           "on a bar line" },
            { A0 + 3 * S16 + 37,  "off the grid (odd tick)" },
            { onNote,             "exactly on a note-on" },
            { sustainMid,         "inside a sustained note" },
            { A0 + 1,             "one tick after the clip start" },
            { A0 + PATLEN - 1,    "on the clip's final tick" },
        };
        for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
            Cut c = want[i];
            bool clash = true;
            while (clash) {
                clash = false;
                for (size_t j = 0; j < cutsA.size(); ++j)
                    if (cutsA[j].t == c.t) { clash = true; break; }
                if (clash) --c.t;
            }
            cutsA.push_back(c);
        }
    }
    std::printf("      one-shot cuts: bar=%ld odd=%ld on-note=%ld in-sustain=%ld "
                "(held note %ld..%ld)\n",
                cutsA[0].t, cutsA[1].t, cutsA[2].t, cutsA[3].t,
                A0 + patA[li].tick, A0 + patA[li].tick + patA[li].dur);

    std::vector<Piece> pa;
    pa.push_back(Piece(A0, A0 + PATLEN, A0));
    for (size_t c = 0; c < cutsA.size(); ++c) {
        const int before = lane_clip_count(perf, 100);
        view.dbg_split_at(A, cutsA[c].t);
        const bool modelled = split_piece(pa, cutsA[c].t);
        std::snprintf(nm, sizeof nm,
                      "one-shot split %s (tick %ld) creates exactly one more clip",
                      cutsA[c].what, cutsA[c].t);
        CHECK(modelled && lane_clip_count(perf, 100) == before + 1, nm);
        const std::vector<Note> got = render(perf, block(perf, 100), 0, TEND);
        std::snprintf(nm, sizeof nm,
                      "one-shot: every note still sounds in place after the split %s",
                      cutsA[c].what);
        CHECK(g_hung == 0 && same(expect(snapA, pa), got, kSeed), nm);
    }
    {
        bool allOneShot = true;
        const std::vector<int> bl = block(perf, 100);
        for (size_t i = 0; i < bl.size(); ++i) {
            sequence* q = perf.get_sequence(bl[i]);
            if (q->trigger_count() > 0 && q->get_loop_enabled()) allOneShot = false;
        }
        CHECK(allOneShot && pa.size() == 7,
              "every slice of a ONE-SHOT clip is still a one-shot (7 pieces)");
    }

    //  MOVE a slice: the nudge path re-seats the trigger.  The content must
    //  travel WITH the clip -- an offset re-folded on the move would leave the
    //  notes behind on the old grid.
    {
        size_t last = 0;
        for (size_t i = 1; i < pa.size(); ++i) if (pa[i].at > pa[last].at) last = i;
        const int mseq = seq_at(perf, 100, pa[last].at);
        const long nd = view.dbg_nudge_ticks();
        select_only(mseq, pa[last].at);
        for (int k = 0; k < 3; ++k) view.on_key(app, SDLK_KP_PLUS);
        pa[last].at += 3 * nd;
        CHECK(mseq >= 0 && nd > 0 && seq_at(perf, 100, pa[last].at) == mseq,
              "nudging a slice moves its clip by the nudge value");
        const std::vector<Note> got = render(perf, block(perf, 100), 0, TEND);
        CHECK(g_hung == 0 && same(expect(snapA, pa), got, kSeed),
              "moving a slice carries its content with it, tick for tick");
    }

    //========================================================================
    //  B -- LOOPING clip whose loop-window period divides neither the clip
    //  length nor the cut distances.  This is the case split_clip_at's own
    //  comment says used to come back on the wrong step.
    //========================================================================
    const int B = 140;
    perf.new_sequence(B); perf.set_active(B, true);
    sequence* sb = perf.get_sequence(B);
    sb->set_name("align-loop");
    sb->set_midi_bus(5); sb->set_midi_channel(9);
    sb->set_length(PATLEN, false);
    const long LS = 3 * S16, LE = LS + 5 * S16;         // a FIVE-16th window
    sb->set_loop_start(0); sb->set_loop_end(LE); sb->set_loop_start(LS);
    sb->set_loop_enabled(true);
    build_pattern(sb, PATLEN, rng, LE);                 // nothing held across LE
    const long PERIOD = sb->repeat_period();
    const long B0   = 24 * BARt + 293;                  // clip start off every grid
    const long BLEN = 4 * PERIOD + 137;                 // NOT a whole number of periods
    sb->clear_triggers();
    sb->add_trigger(B0, BLEN, 0, false);
    CHECK(PERIOD == 5 * S16 && BLEN % PERIOD != 0 && BARt % PERIOD != 0,
          "looping clip: the window period divides neither the bar nor the clip");
    const std::vector<Note> snapB = render(perf, block(perf, 140), 0, TEND);
    CHECK(snapB.size() >= 8 && g_hung == 0,
          "GROUND TRUTH: the uncut looping clip repeats its window");

    size_t lb = 0;
    for (size_t i = 1; i < snapB.size(); ++i) if (snapB[i].dur > snapB[lb].dur) lb = i;
    std::vector<Cut> cutsB;
    {
        const Cut want[] = {
            { B0 + PERIOD,                     "exactly one loop period in" },
            { B0 + PERIOD + PERIOD / 2 + 7,    "1.5 periods + 7 ticks in" },
            { B0 + 2 * PERIOD + PERIOD / 3 + 11, "2.3 periods + 11 ticks in" },
            { snapB[lb].tick + snapB[lb].dur / 2, "inside a sustained note" },
            { B0 + BLEN - 1,                   "on the clip's final tick" },
        };
        for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
            Cut c = want[i];
            bool clash = true;
            while (clash) {
                clash = false;
                for (size_t j = 0; j < cutsB.size(); ++j)
                    if (cutsB[j].t == c.t) { clash = true; break; }
                if (clash) --c.t;
            }
            cutsB.push_back(c);
        }
    }
    std::vector<Piece> pb;
    pb.push_back(Piece(B0, B0 + BLEN, B0));
    for (size_t c = 0; c < cutsB.size(); ++c) {
        const int before = lane_clip_count(perf, 140);
        view.dbg_split_at(B, cutsB[c].t);
        const bool modelled = split_piece(pb, cutsB[c].t);
        std::snprintf(nm, sizeof nm,
                      "looping split %s (tick %ld) creates exactly one more clip",
                      cutsB[c].what, cutsB[c].t);
        CHECK(modelled && lane_clip_count(perf, 140) == before + 1, nm);
        const std::vector<Note> got = render(perf, block(perf, 140), 0, TEND);
        std::snprintf(nm, sizeof nm,
                      "looping: the repetition stays on the same step after the split %s",
                      cutsB[c].what);
        CHECK(g_hung == 0 && same(expect(snapB, pb), got, kSeed), nm);
    }
    {
        bool ok = true;
        const std::vector<int> bl = block(perf, 140);
        for (size_t i = 0; i < bl.size(); ++i) {
            sequence* q = perf.get_sequence(bl[i]);
            if (q->trigger_count() == 0) continue;
            if (q->get_loop_start() != LS || q->get_loop_end() != LE ||
                !q->get_loop_enabled()) ok = false;
        }
        CHECK(ok, "every slice of a looping clip keeps its loop window and enable flag");
    }

    //========================================================================
    //  C -- COPY / PASTE, through Ctrl+C and Ctrl+V.
    //========================================================================
    const int C = 180, D = 220;
    perf.new_sequence(C); perf.set_active(C, true);
    sequence* sc = perf.get_sequence(C);
    sc->set_name("align-copy");
    sc->set_midi_bus(1); sc->set_midi_channel(4);
    sc->set_length(PATLEN, false);
    sc->set_loop_enabled(false);
    build_pattern(sc, PATLEN, rng, 0);
    const long C0 = 40 * BARt + 7 * S16;
    sc->clear_triggers(); sc->add_trigger(C0, PATLEN, 0, false);

    //  the cross-lane paste destination: an empty pattern with its OWN routing
    perf.new_sequence(D); perf.set_active(D, true);
    sequence* sd = perf.get_sequence(D);
    sd->set_name("align-dest");
    sd->set_midi_bus(7); sd->set_midi_channel(11);
    sd->set_length(PATLEN, false);
    sd->set_loop_enabled(false);

    const std::vector<Note> snapC = render(perf, block(perf, 180), 0, TEND);
    CHECK(snapC.size() >= 30 && g_hung == 0,
          "GROUND TRUTH: the clip to be copied sounds its whole pattern");

    select_only(C, C0);
    ctrl_key(SDLK_c);
    CHECK(view.dbg_clipboard() == 1 && view.dbg_clip_span() == PATLEN,
          "Ctrl+C copies exactly the one selected clip, span == its length");

    //  ROUND TRIP: paste the copy back where it came from.  Paste OVERWRITES,
    //  so the original is removed and replaced by the copy -- and the lane must
    //  still render note-for-note identical to the ground truth.
    paste_at(C0);
    {
        const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
        CHECK(g_hung == 0 && same(snapC, got, kSeed),
              "ROUND TRIP: a copy pasted back at its own position is note-for-note "
              "the original");
    }

    std::vector<Piece> pcv;
    pcv.push_back(Piece(C0, C0 + PATLEN, C0));

    struct Pst { long at; const char* what; };
    const Pst pastes[] = {
        { 0,                            "at tick 0" },
        { 50 * BARt,                    "on a bar line" },
        { 56 * BARt + 137,              "off the grid (odd tick)" },
        { 60 * BARt + 5 * S16 + 1,      "one tick past a 16th" },
        { 64 * BARt + PATLEN / 3 + 1,   "at a distance that is no multiple of the clip length" },
    };
    for (size_t i = 0; i < sizeof(pastes) / sizeof(pastes[0]); ++i) {
        const int before = lane_clip_count(perf, 180);
        paste_at(pastes[i].at);
        pcv.push_back(Piece(C0, C0 + PATLEN, pastes[i].at));
        std::snprintf(nm, sizeof nm, "paste %s (tick %ld) adds one clip",
                      pastes[i].what, pastes[i].at);
        CHECK(lane_clip_count(perf, 180) == before + 1, nm);
        const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
        std::snprintf(nm, sizeof nm,
                      "paste %s: every note lands at originalTick - clipStart + pasteStart",
                      pastes[i].what);
        CHECK(g_hung == 0 && same(expect(snapC, pcv), got, kSeed), nm);
    }

    //  ABUT: paste sharing an edge with the copy already at tick 0.  Nothing is
    //  overwritten -- the neighbour is not INSIDE the paste range.
    {
        const long abut = PATLEN;
        paste_at(abut);
        pcv.push_back(Piece(C0, C0 + PATLEN, abut));
        const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
        CHECK(g_hung == 0 && same(expect(snapC, pcv), got, kSeed),
              "paste ABUTTING an existing clip leaves the neighbour untouched");
    }

    //  OVERLAP: paste half over an existing copy.  Paste overwrites (ch.28
    //  p668): the clip under the range is separated at the paste boundaries and
    //  whatever falls wholly inside is removed.  What SURVIVES must still be in
    //  its original alignment.
    {
        const long ov = 50 * BARt + PATLEN / 2;
        split_piece(pcv, ov);
        for (size_t i = 0; i < pcv.size(); ) {
            const long len = pcv[i].hi - pcv[i].lo;
            if (pcv[i].at >= ov && pcv[i].at + len <= ov + PATLEN)
                pcv.erase(pcv.begin() + (long)i);
            else ++i;
        }
        paste_at(ov);
        pcv.push_back(Piece(C0, C0 + PATLEN, ov));
        const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
        CHECK(g_hung == 0 && same(expect(snapC, pcv), got, kSeed),
              "paste OVERLAPPING an existing clip: the surviving material keeps "
              "its alignment");
    }

    //  CROSS-LANE paste: the focused lane is the paste target, so the clip is
    //  rebuilt on THAT lane's pattern and filled by clone_pattern_events.  The
    //  notes must not move; only the routing becomes the destination lane's.
    {
        const long dAt = 80 * BARt + 3 * S16 + 5;
        view.dbg_set_focus_lane(view.dbg_lane_key(D));
        paste_at(dAt);
        view.dbg_set_focus_lane(-1);
        std::vector<Piece> pd;
        pd.push_back(Piece(C0, C0 + PATLEN, dAt, 7, 11));
        const std::vector<Note> got = render(perf, block(perf, 220), 0, TEND);
        CHECK(lane_clip_count(perf, 220) == 1, "cross-lane paste creates the clip on the target lane");
        CHECK(g_hung == 0 && same(expect(snapC, pd), got, kSeed),
              "paste onto ANOTHER lane keeps every note's tick/pitch/velocity/"
              "length and takes the destination lane's routing");
    }

    //  PASTE A SLICE.  A slice carries a NON-ZERO content offset, so this is
    //  where an offset that was folded once can get folded twice.  The check is
    //  against the ORIGINAL ground truth, not against the slice's own state.
    {
        const int cseq = seq_at(perf, 180, C0);
        const long sliceCut = C0 + 7 * S16 + 53;        // odd, mid-pattern
        view.dbg_split_at(C, sliceCut);
        split_piece(pcv, sliceCut);
        const int rseq = seq_at(perf, 180, sliceCut);
        long roff = -1;
        if (rseq >= 0) {
            sequence* q = perf.get_sequence(rseq);
            q->reset_draw_trigger_marker();
            long on, off, offs; bool sel;
            while (q->get_next_trigger(&on, &off, &sel, &offs))
                if (on == sliceCut) roff = offs;
        }
        CHECK(cseq >= 0 && rseq >= 0 && roff == 7 * S16 + 53,
              "the right slice carries a non-zero content offset (7*16th + 53)");
        {
            const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
            CHECK(g_hung == 0 && same(expect(snapC, pcv), got, kSeed),
                  "slicing the source clip leaves both halves in alignment");
        }
        // copy that slice and paste it somewhere odd
        select_only(rseq, sliceCut);
        ctrl_key(SDLK_c);
        const long slicePaste = 96 * BARt + 3 * S16 + 91;
        paste_at(slicePaste);
        // find the slice's own piece to know the original span it carries
        Piece src(0, 0, 0);
        for (size_t i = 0; i < pcv.size(); ++i) if (pcv[i].at == sliceCut) src = pcv[i];
        pcv.push_back(Piece(src.lo, src.hi, slicePaste));
        const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
        CHECK(g_hung == 0 && same(expect(snapC, pcv), got, kSeed),
              "a pasted SLICE sounds the original's notes at the original spacing");
    }

    //  CUT + PASTE = reorder.  Ctrl+X removes the clip and fills the clipboard;
    //  Ctrl+V puts it back somewhere else.
    {
        const int mseq = seq_at(perf, 180, PATLEN);      // the abutting copy
        select_only(mseq, PATLEN);
        ctrl_key(SDLK_x);
        for (size_t i = 0; i < pcv.size(); ) {
            if (pcv[i].at == PATLEN) pcv.erase(pcv.begin() + (long)i); else ++i;
        }
        {
            const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
            CHECK(mseq >= 0 && g_hung == 0 && same(expect(snapC, pcv), got, kSeed),
                  "Ctrl+X removes exactly the cut clip and nothing else moves");
        }
        const long re = 104 * BARt + 11;
        paste_at(re);
        pcv.push_back(Piece(C0, C0 + PATLEN, re));
        const std::vector<Note> got = render(perf, block(perf, 180), 0, TEND);
        CHECK(g_hung == 0 && same(expect(snapC, pcv), got, kSeed),
              "the cut clip pastes back in alignment at a new, odd position");
    }

    //========================================================================
    //  E -- a ONE-SHOT dragged out PAST its data, then cut inside the tail.
    //
    //  A one-shot's length is free: grow_trigger just moves its end point, so a
    //  one-shot clip can legitimately be longer than the pattern it plays, with
    //  silence after the END marker.  Cutting inside that silence gives the
    //  right half a content offset PAST the end marker, which split_clip_at
    //  deliberately does NOT fold ("past the end it simply stops").  Nothing
    //  may sound in the tail, and nothing may be replayed.
    //========================================================================
    {
        const int E = 260;
        perf.new_sequence(E); perf.set_active(E, true);
        sequence* se = perf.get_sequence(E);
        se->set_name("align-tail");
        se->set_midi_bus(4); se->set_midi_channel(6);
        se->set_length(PATLEN, false);
        se->set_loop_enabled(false);
        build_pattern(se, PATLEN, rng, 0);
        const long E0 = 90 * BARt;
        se->clear_triggers();
        se->add_trigger(E0, 2 * PATLEN, 0, false);       // dragged to twice its data
        const std::vector<Note> snapE = render(perf, block(perf, 260), 0, TEND);
        long lastEnd = 0;
        for (size_t i = 0; i < snapE.size(); ++i)
            if (snapE[i].tick + snapE[i].dur > lastEnd) lastEnd = snapE[i].tick + snapE[i].dur;
        CHECK(snapE.size() >= 30 && g_hung == 0 && lastEnd < E0 + PATLEN,
              "GROUND TRUTH: an over-long one-shot falls silent at its END marker");

        const long tailCut = E0 + PATLEN + 700;          // inside the silent tail
        std::vector<Piece> pe;
        pe.push_back(Piece(E0, E0 + 2 * PATLEN, E0));
        view.dbg_split_at(E, tailCut);
        split_piece(pe, tailCut);
        const std::vector<Note> got = render(perf, block(perf, 260), 0, TEND);
        CHECK(g_hung == 0 && same(expect(snapE, pe), got, kSeed),
              "cutting an over-long one-shot INSIDE its silent tail replays nothing");
    }

    // -------- teardown ------------------------------------------------------
    view.dbg_unselect_all_clips();
    view.dbg_set_focus_lane(-1);
    view.dbg_select(-1, -1, -1, -1);
    view.on_create_pattern = 0;
    view.on_track_key = 0;
    for (int i = 100; i < 300; ++i) if (perf.is_active(i)) perf.set_active(i, false);
}

static int selftest(App& app, perform& perf, arrange::ArrangeView& view)
{
    view.rect = { 0, 0, app.w, app.h };
    view.draw(app);                       // populate the topbar hit rects

    // ---- edit-mode block (2x2 over the header column) ----------------------
    CHECK(view.dbg_edit_mode() == 3, "default edit mode is Grid");
    click(app, view, 40, 30);             // SLIP cell (row B, col 0)
    CHECK(view.dbg_edit_mode() == 1, "clicking SLIP selects Slip mode");
    view.on_key(app, SDLK_F4);
    CHECK(view.dbg_edit_mode() == 3, "F4 selects Grid mode");
    view.on_key(app, SDLK_F1);
    CHECK(view.dbg_edit_mode() == 0, "F1 selects Shuffle mode");
    view.on_key(app, SDLK_F2);
    CHECK(view.dbg_edit_mode() == 1, "F2 selects Slip mode");

    // Shuffle Lock: Ctrl-click the Shuffle button from another mode
    SDL_SetModState(KMOD_LCTRL);
    click(app, view, 40, 10);             // SHUFFLE cell (row A, col 0)
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_shuffle_lock(), "Ctrl-click Shuffle engages Shuffle Lock");
    view.on_key(app, SDLK_F1);
    CHECK(view.dbg_edit_mode() == 1, "F1 is dead while Shuffle is locked");
    SDL_SetModState(KMOD_LCTRL);
    click(app, view, 40, 10);
    SDL_SetModState(KMOD_NONE);
    CHECK(!view.dbg_shuffle_lock(), "Ctrl-click again unlocks Shuffle");

    // Snap To Grid: Shift+F4 while in Slip
    view.on_key(app, SDLK_F4);            // back to Grid...
    view.on_key(app, SDLK_F2);            // ...then Slip
    SDL_SetModState(KMOD_LSHIFT);
    view.on_key(app, SDLK_F4);
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_snap_to_grid(), "Shift+F4 enables Snap To Grid in Slip");
    SDL_SetModState(KMOD_LSHIFT);
    view.on_key(app, SDLK_F4);
    SDL_SetModState(KMOD_NONE);
    CHECK(!view.dbg_snap_to_grid(), "Shift+F4 toggles Snap To Grid back off");
    view.on_key(app, SDLK_F4);            // Grid for the rest

    // ---- tools -------------------------------------------------------------
    view.on_key(app, SDLK_F7);
    CHECK(view.dbg_tool() == 1, "F7 selects the Selector");
    view.on_key_up(app, SDLK_F7);         // release before the next tool key
    view.on_key(app, SDLK_F6);
    CHECK(view.dbg_tool() == 5, "F6 selects the Trim tool");
    view.on_key(app, SDLK_F7);            // chord: F7 while F6 still held
    CHECK(view.dbg_tool() == 7, "F6+F7 chord selects the Smart tool");
    view.on_key_up(app, SDLK_F6);
    view.on_key_up(app, SDLK_F7);
    view.on_key(app, SDLK_F5);
    CHECK(view.dbg_tool() == 4, "F5 selects the Zoomer");
    view.on_key(app, SDLK_F8);
    CHECK(view.dbg_tool() == 0, "F8 selects the Grabber");
    view.on_key_up(app, SDLK_F5);
    view.on_key_up(app, SDLK_F8);

    // ---- Selector: edit selection + timeline link --------------------------
    view.on_key(app, SDLK_F7);
    view.on_key_up(app, SDLK_F7);
    view.on_mouse(app, MouseEv{ 400, 100, SDL_BUTTON_LEFT, true });
    view.on_mouse(app, MouseEv{ 600, 100, SDL_BUTTON_LEFT, true });   // drag
    view.on_mouse(app, MouseEv{ 600, 100, SDL_BUTTON_LEFT, false });
    CHECK(view.dbg_sel_start() >= 0 && view.dbg_sel_end() > view.dbg_sel_start(),
          "Selector drag makes an edit selection");
    CHECK(perf.get_left_tick() == view.dbg_sel_start() &&
          perf.get_right_tick() == view.dbg_sel_end(),
          "linked timeline mirrors the edit selection");

    // ---- Tab to clip boundary ----------------------------------------------
    click(app, view, 200, 100);           // insertion point near tick 640
    const long ins = view.dbg_sel_start();
    view.on_key(app, SDLK_TAB);
    CHECK(view.dbg_sel_start() > ins &&
          view.dbg_sel_start() % (c_ppqn * 4) == 0,
          "Tab moves the cursor to the next clip boundary");

    // ---- nudge -------------------------------------------------------------
    const long before = view.dbg_sel_start();
    view.on_key(app, SDLK_KP_PLUS);
    CHECK(view.dbg_sel_start() > before, "numpad + nudges the selection right");
    view.on_key(app, SDLK_KP_MINUS);
    CHECK(view.dbg_sel_start() == before, "numpad - nudges it back");

    // ---- zoom presets ------------------------------------------------------
    view.draw(app);                       // refresh preset rects
    const double z0 = view.dbg_scale();
    SDL_SetModState(KMOD_LSHIFT);
    click(app, view, 189, 33);            // Shift-click preset 1 = store
    SDL_SetModState(KMOD_NONE);
    view.on_key(app, SDLK_MINUS);         // zoom out one step
    CHECK(view.dbg_scale() > z0, "minus key zooms out");
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_1);             // Ctrl+1 recalls preset 1
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_scale() == z0, "Ctrl+1 recalls the stored zoom preset");

    // ---- universe toggle ---------------------------------------------------
    SDL_SetModState(KMOD_LALT);
    view.on_key(app, SDLK_7);
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_universe(), "Alt+7 shows the Universe view");
    view.draw(app);                       // must lay out with the strip shown
    SDL_SetModState(KMOD_LALT);
    view.on_key(app, SDLK_7);             // back off: keep later canvas
    SDL_SetModState(KMOD_NONE);           // coordinates stable
    view.draw(app);

    // ---- SHUFFLE: deleting a clip closes the gap ---------------------------
    // seq 0 carries clips at bars 0-1, 1-2, 2-3, 4-5...  Select the 1-2 clip
    // with the Grabber and Delete it in Shuffle mode: the 2-3 clip must slide
    // to bar 1 and the 4-5 clip to bar 3 (gaps between clips preserved).
    view.on_key(app, SDLK_ESCAPE);        // drop any selection from above
    view.on_key(app, SDLK_F8);            // Grabber
    view.on_key_up(app, SDLK_F8);
    view.on_key(app, SDLK_F1);            // Shuffle mode
    SDL_Delay(450);                       // outrun the double-click window
    click(app, view, 216, 100);           // clip 1-2 (tick ~1152) on lane 0
    view.on_key(app, SDLK_DELETE);
    {
        bool at1 = false, at1536 = false, at2304 = false;
        sequence* s0 = perf.get_sequence(0);
        s0->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        while (s0->get_next_trigger(&on, &off, &sel, &offs)) {
            if (on == 1 * BAR) at1 = true;
            if (on == 2 * BAR) at1536 = true;
            if (on == 3 * BAR) at2304 = true;
        }
        CHECK(at1 && !at1536, "Shuffle delete slides the next clip into the gap");
        CHECK(at2304, "Shuffle delete preserves the gap before later clips");
    }

    // ---- SPOT: clicking a clip opens the dialog; typed location applies ----
    view.on_key(app, SDLK_F3);            // Spot mode
    SDL_Delay(450);                       // outrun the double-click window
    click(app, view, 190, 100);           // the bar 0-1 clip on lane 0
    CHECK(app.editing_text(), "Spot mode press opens the Spot dialog");
    if (app.editing_text() && app.text_target) {
        *app.text_target = "6";           // bar 6 == tick 5 * BAR
        app.text_commit(true);
        app.end_text();                   // the app's Enter key does both
        bool at5 = false;
        sequence* s0 = perf.get_sequence(0);
        s0->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        while (s0->get_next_trigger(&on, &off, &sel, &offs))
            if (on == 5 * BAR) at5 = true;
        CHECK(at5, "Spot dialog moves the clip to the typed bar");
    }
    view.on_key(app, SDLK_F4);            // leave things in Grid mode

    //------------------------------------------------------------------------
    //  ch.28 -- MULTIPLE UNDO queue (arrange_edit.cpp)
    //------------------------------------------------------------------------
    view.on_key(app, SDLK_ESCAPE);        // drop any selection
    view.on_key(app, SDLK_F2);            // Slip mode: no shuffle ripple below
    view.on_key(app, SDLK_F8);            // Grabber
    view.on_key_up(app, SDLK_F8);
    SDL_Delay(450);                       // outrun the double-click window
    click(app, view, 200, 500);           // Pad clip bars 0-4 on lane 5
    auto lane5_has_at = [&](long tick) {
        sequence* s5 = perf.get_sequence(5);
        long on, off, offs; bool sel;
        s5->reset_draw_trigger_marker();
        while (s5->get_next_trigger(&on, &off, &sel, &offs))
            if (on <= tick && off >= tick) return true;
        return false;
    };
    CHECK(lane5_has_at(0), "precondition: Pad clip exists at bar 1");
    const int undo0 = view.dbg_undo_len();
    view.on_key(app, SDLK_DELETE);
    CHECK(view.dbg_undo_len() == undo0 + 1, "Delete pushes ONE undo-queue entry");
    CHECK(!lane5_has_at(0), "Delete removed the clip");
    CHECK(view.on_undo(app, false), "Ctrl+Z is consumed by the view's queue");
    CHECK(lane5_has_at(0), "undo restores the deleted clip");
    CHECK(view.on_undo(app, true), "Ctrl+Shift+Z redoes through the queue");
    CHECK(!lane5_has_at(0), "redo removes the clip again");
    view.on_undo(app, false);             // leave the clip in place
    CHECK(lane5_has_at(0), "second undo restores it once more");
    view.on_key(app, SDLK_u);
    CHECK(view.dbg_undo_open(), "U opens the Undo History window");
    view.on_key(app, SDLK_u);
    CHECK(!view.dbg_undo_open(), "U closes it again");

    //------------------------------------------------------------------------
    //  ch.28 -- range Cut / Paste over the edit selection
    //------------------------------------------------------------------------
    view.on_key(app, SDLK_F7);            // Selector
    view.on_key_up(app, SDLK_F7);
    SDL_Delay(450);                       // defeat the multi-click counter
    view.on_mouse(app, MouseEv{ 276, 500, SDL_BUTTON_LEFT, true });
    view.on_mouse(app, MouseEv{ 372, 500, SDL_BUTTON_LEFT, true });   // drag
    view.on_mouse(app, MouseEv{ 372, 500, SDL_BUTTON_LEFT, false }); // 3072..6144
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_x);             // Cut
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_clipboard() > 0, "range Cut fills the clipboard");
    CHECK(!lane5_has_at(4000), "range Cut leaves a hole in the clip");
    CHECK(lane5_has_at(1000) && lane5_has_at(8000),
          "leftover clips are auto-created on either side of the cut");
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_v);             // Paste at the selection start
    SDL_SetModState(KMOD_NONE);
    CHECK(lane5_has_at(4000), "Paste at the insertion restores the material");

    //------------------------------------------------------------------------
    //  ch.31 -- Separate At Selection (Ctrl+E) + Heal Separation (Ctrl+H)
    //------------------------------------------------------------------------
    auto lane5_count_in = [&](long a, long b) {
        sequence* s5 = perf.get_sequence(5);
        long on, off, offs; bool sel; int n = 0;
        s5->reset_draw_trigger_marker();
        while (s5->get_next_trigger(&on, &off, &sel, &offs))
            if (on >= a && off < b) ++n;
        return n;
    };
    CHECK(lane5_count_in(24576, 36864) == 1, "precondition: one clip at bars 9-12");
    SDL_Delay(450);
    view.on_mouse(app, MouseEv{ 1044, 500, SDL_BUTTON_LEFT, true });
    view.on_mouse(app, MouseEv{ 1140, 500, SDL_BUTTON_LEFT, true });  // 27648..30720
    view.on_mouse(app, MouseEv{ 1140, 500, SDL_BUTTON_LEFT, false });
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_e);             // Separate At Selection
    SDL_SetModState(KMOD_NONE);
    CHECK(lane5_count_in(24576, 36864) == 3,
          "Separate At Selection splits the clip into three");
    SDL_Delay(450);
    view.on_mouse(app, MouseEv{ 1011, 500, SDL_BUTTON_LEFT, true });
    view.on_mouse(app, MouseEv{ 1073, 500, SDL_BUTTON_LEFT, true });  // across 27648
    view.on_mouse(app, MouseEv{ 1073, 500, SDL_BUTTON_LEFT, false });
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_h);             // Heal Separation
    SDL_SetModState(KMOD_NONE);
    CHECK(lane5_count_in(24576, 36864) == 2,
          "Heal Separation rejoins the adjacent unmodified clips");

    //------------------------------------------------------------------------
    //  ch.31 -- nudge clips by the Nudge value + Quantize to Grid + Rating
    //------------------------------------------------------------------------
    view.on_key(app, SDLK_ESCAPE);
    view.on_key(app, SDLK_F8);            // Grabber
    view.on_key_up(app, SDLK_F8);
    SDL_Delay(450);
    click(app, view, 700, 660);           // FX clip bars 6-8 on lane 7 (isolated)
    auto lane7_first = [&]{
        sequence* s7 = perf.get_sequence(7);
        long on, off, offs; bool sel; long best = -1;
        s7->reset_draw_trigger_marker();
        while (s7->get_next_trigger(&on, &off, &sel, &offs))
            if (best < 0 || on < best) best = on;
        return best;
    };
    const long fx0 = lane7_first();
    SDL_SetModState((SDL_Keymod)(KMOD_LSHIFT | KMOD_LALT));
    view.on_key(app, SDLK_MINUS);         // next SMALLER nudge value (1/8)
    SDL_SetModState(KMOD_NONE);
    const long nd = view.dbg_nudge_ticks();
    CHECK(nd > 0 && nd < c_ppqn, "Shift+Alt+- steps the nudge value down");
    view.on_key(app, SDLK_KP_PLUS);
    CHECK(lane7_first() == fx0 + nd, "numpad + nudges the selected clip");
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_0);             // Quantize to Grid
    SDL_SetModState(KMOD_NONE);
    CHECK(lane7_first() % (long)c_ppqn == 0 && lane7_first() != fx0 + nd,
          "Ctrl+0 snaps the clip start to the nearest grid line");
    SDL_SetModState((SDL_Keymod)(KMOD_LCTRL | KMOD_LALT));
    view.on_key(app, SDLK_3);             // Rate Clips: 3
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_rating(7) == 3, "Ctrl+Alt+3 rates the selected clip");

    //------------------------------------------------------------------------
    //  ch.28 -- Undo All / levels clamp
    //------------------------------------------------------------------------
    view.set_undo_levels(4);
    CHECK(view.dbg_undo_levels() == 4 && view.dbg_undo_len() <= 4,
          "Levels of Undo trims the queue to the new cap");
    view.set_undo_levels(64);
    view.undo_all();
    CHECK(view.dbg_undo_done() == 0, "Undo All walks the whole queue back");
    view.redo_all();
    CHECK(view.dbg_undo_done() == view.dbg_undo_len(),
          "Redo All walks it forward again");
    view.clear_undo_queue();
    CHECK(view.dbg_undo_len() == 0, "Clear Undo Queue empties the history");

    //------------------------------------------------------------------------
    //  ch.32 -- FADES & CROSSFADES
    //------------------------------------------------------------------------
    using PatchKnob::engine::ScheduledClip;
    using PatchKnob::engine::AudioClip;
    // (a) the engine fade curves: the seven presets' characters, the linear
    // default, Equal Power as the square root, and the S-curve's fixed middle.
    CHECK(ScheduledClip::fadeCurve(0.5f, 2, 0.f, 0) == 1.0f,
          "preset 1 holds full volume through the fade");
    CHECK(ScheduledClip::fadeCurve(0.5f, 8, 0.f, 0) == 0.0f,
          "preset 7 holds silence until the fade end");
    CHECK(std::fabs(ScheduledClip::fadeCurve(0.5f, 5, 0.f, 0) - 0.5f) < 1e-6f,
          "preset 4 is the linear default curve");
    {
        const float u = 0.3f;
        const float g  = ScheduledClip::fadeCurve(u, 5, 0.f, 0);
        const float p  = ScheduledClip::fadeCurve(u, 5, 0.f, 1);
        CHECK(std::fabs(p * p - g) < 1e-5f, "Equal Power plays the square root of the curve");
        const float pin  = ScheduledClip::fadeCurve(u, 5, 0.f, 1);
        const float pout = ScheduledClip::fadeCurve(1.f - u, 5, 0.f, 1);
        CHECK(std::fabs(pin * pin + pout * pout - 1.f) < 1e-5f,
              "an equal-power crossfade sums to unity POWER");
        CHECK(std::fabs(ScheduledClip::fadeCurve(0.5f, 1, 0.7f, 0) - 0.5f) < 1e-5f,
              "the S-curve pivots through its midpoint at any tension");
    }
    {   // (b) fadeGain honours the per-half shape/slope fields
        AudioClip tone = AudioClip::synth_sine(440.0, 0.1, 48000.0);
        ScheduledClip sc;
        sc.clip = &tone; sc.length = tone.numFrames();
        sc.fadeInFrames = 1000; sc.fadeInShape = 8;      // preset 7: silence
        CHECK(sc.fadeGain(500, sc.regionLength()) == 0.0f,
              "fadeGain: a preset-7 fade-in is silent mid-fade");
        sc.fadeInShape = 2;                              // preset 1: full level
        CHECK(sc.fadeGain(500, sc.regionLength()) == 1.0f,
              "fadeGain: a preset-1 fade-in is at full level mid-fade");
    }

    // (c) crossfades in the arrangement: two audio clips sharing one lane.
    static AudioClip xa = AudioClip::synth_sine(440.0, 2.0, 48000.0, 0.8f, "xa");
    static AudioClip xb = AudioClip::synth_sine(220.0, 2.0, 48000.0, 0.8f, "xb");
    perf.new_sequence(20); perf.get_sequence(20)->set_name("XA");
    perf.new_sequence(21); perf.get_sequence(21)->set_name("XB");
    view.on_track_key = [](int s){ return s == 21 ? 20 : s; };
    const long FULL = 4 * BAR;
    view.set_audio_clip(20, &xa, FULL);
    view.set_audio_clip(21, &xb, FULL);
    // A occupies [0, 2 bars) of its source (2 bars of tail material left);
    // B starts at bar 2 playing from source bar 1 (1 bar of head material).
    view.set_audio_region(20, 0, 2 * BAR, 0);
    view.set_audio_region(21, 2 * BAR, 2 * BAR, BAR);
    view.draw(app);
    const long W = 192;                       // wanted half-window
    CHECK(view.dbg_create_crossfade(20, 21, 2 * BAR - W, 2 * BAR + W),
          "create_crossfade accepts a centered window with material both sides");
    CHECK(view.dbg_region_len(20) == 2 * BAR + W,
          "crossfade extends the left clip past the splice");
    CHECK(view.dbg_region_pos(21) == 2 * BAR - W,
          "crossfade pulls the right clip ahead of the splice (regions overlap)");
    CHECK(view.dbg_fade(20).outTicks == 2 * W && view.dbg_fade(21).inTicks == 2 * W,
          "both halves span the whole crossfade window");
    CHECK(view.dbg_fade(20).outSlope == 1 && view.dbg_fade(21).inSlope == 1,
          "the default Equal Power link forces both slopes to Equal Power");
    {
        int l = -1, r = -1;
        CHECK(view.dbg_xfade_pair(20, l, r) && l == 20 && r == 21,
              "the overlap is recognised as a crossfade pair");
    }
    view.dbg_remove_crossfade(20, 21);
    CHECK(view.dbg_region_len(20) == 2 * BAR && view.dbg_region_pos(21) == 2 * BAR,
          "removing the crossfade retracts both clips to a butt joint");
    CHECK(view.dbg_fade(20).outTicks == 0 && view.dbg_fade(21).inTicks == 0,
          "removing the crossfade clears both fades");

    // insufficient material: both clips already use their whole source
    perf.new_sequence(22); perf.get_sequence(22)->set_name("YA");
    perf.new_sequence(23); perf.get_sequence(23)->set_name("YB");
    view.on_track_key = [](int s){ return s == 21 ? 20 : (s == 23 ? 22 : s); };
    view.set_audio_clip(22, &xa, FULL);
    view.set_audio_clip(23, &xb, FULL);
    view.set_audio_region(22, 0, FULL, 0);          // whole source
    view.set_audio_region(23, FULL, FULL, 0);       // whole source, no head room
    CHECK(!view.dbg_create_crossfade(22, 23, FULL - W, FULL + W),
          "a crossfade is refused when no audio exists beyond the boundary");

    // (d) Ctrl+Alt+F: default fade-in from an edit selection, through the
    // Multiple-Undo queue; Ctrl+F alone opens the Fades dialog.
    view.dbg_select(0, BAR, 9, 9);                  // row 9 == lane XA
    const int undoF = view.dbg_undo_len();
    SDL_SetModState((SDL_Keymod)(KMOD_LCTRL | KMOD_LALT));
    view.on_key(app, SDLK_f);
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_fade(20).inTicks == BAR,
          "Ctrl+Alt+F (default settings) creates the fade-in from the selection");
    CHECK(view.dbg_undo_len() == undoF + 1,
          "the fade edit lands as ONE entry on the undo queue");
    CHECK(view.on_undo(app, false) && view.dbg_fade(20).inTicks == 0,
          "Ctrl+Z removes the fade again");
    view.on_undo(app, true);                        // redo: keep the fade-in
    CHECK(view.dbg_fade(20).inTicks == BAR, "redo restores the fade-in");

    view.dbg_select(2 * BAR - W, 2 * BAR + W, 9, 9);
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_f);
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_fdlg_open() && view.dbg_fdlg_kind() == 2,
          "Ctrl+F across the splice opens the Crossfade dialog");
    view.on_key(app, SDLK_RETURN);                  // OK: keep the crossfade
    CHECK(!view.dbg_fdlg_open(), "Enter closes the Fades dialog (OK)");
    CHECK(view.dbg_fade(21).inTicks == 2 * W,
          "OK keeps the crossfade the dialog created");

    // (e) shape cycling on the selected fades (Alt+Win+Right)
    view.dbg_select(0, BAR, 9, 9);                  // the fade-in from (d)
    SDL_SetModState((SDL_Keymod)(KMOD_LALT | KMOD_LGUI));
    view.on_key(app, SDLK_RIGHT);
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_fade(20).inShape == 1,
          "Alt+Win+Right cycles the fade shape Standard -> S-Curve");

    // (f) Edit > Fades > Delete over a range
    view.dbg_select(0, BAR, 9, 9);
    view.dbg_delete_fades();
    CHECK(view.dbg_fade(20).inTicks == 0, "Delete Fades clears the fade in range");

    // (g) Batch Fades: selecting across both whole clips opens the dialog,
    // and OK creates the boundary crossfade + edge fades.
    view.dbg_remove_crossfade(20, 21);              // back to a butt joint
    view.dbg_select(0, 4 * BAR, 9, 9);
    SDL_SetModState(KMOD_LCTRL);
    view.on_key(app, SDLK_f);
    SDL_SetModState(KMOD_NONE);
    CHECK(view.dbg_bdlg_open(), "Ctrl+F across whole clips opens Batch Fades");
    view.on_key(app, SDLK_RETURN);                  // OK with the defaults
    CHECK(!view.dbg_bdlg_open(), "Enter applies and closes Batch Fades");
    CHECK(view.dbg_fade(20).inTicks > 0 && view.dbg_fade(21).outTicks > 0,
          "Batch Fades adds a fade-in to the first clip and a fade-out to the last");
    {
        int l = -1, r = -1;
        CHECK(view.dbg_xfade_pair(20, l, r) && view.dbg_fade(21).inTicks > 0,
              "Batch Fades creates a real (overlapping) crossfade at the boundary");
    }

    // (n) SPARSE lane keys.  The host's on_track_key() does NOT return a
    // sequence index: mixer-mapped tracks come back as 100000 + trackIndex and
    // automation lanes as 200000 + laneId.  Every check above uses small keys,
    // so a version of active_list()/lane_groups() that subscripted an array
    // with the key passed the whole suite while folding every mapped track
    // into ONE lane -- the arrange view showed a single track no matter how
    // many existed.  Drive the real key space here.
    {
        for (int i = 60; i < 66; ++i) { perf.new_sequence(i); perf.set_active(i, true); }
        // Three mapped tracks (two sequences each) + one automation lane.
        view.on_track_key = [](int s) -> int {
            if (s >= 60 && s <= 65) {
                if (s == 65) return 200000 + 7;          // automation lane
                return 100000 + ((s - 60) / 2);          // 60,61 -> 0; 62,63 -> 1; 64 -> 2
            }
            return s;
        };
        const std::vector<int> act = view.dbg_active_list();
        int mapped = 0, autom = 0;
        for (size_t i = 0; i < act.size(); ++i) {
            const int k = view.on_track_key(act[i]);
            if (k >= 200000) ++autom; else if (k >= 100000) ++mapped;
        }
        CHECK(mapped == 3, "sparse lane keys: three mapped tracks stay three lanes");
        CHECK(autom == 1, "sparse lane keys: the automation lane survives dedup");

        const std::vector<std::vector<int>> groups = view.dbg_lane_groups();
        CHECK(groups.size() == act.size(),
              "lane_groups returns one bucket per active lane");
        size_t bucketed = 0;
        for (size_t i = 0; i < groups.size(); ++i) bucketed += groups[i].size();
        CHECK(bucketed >= 6,
              "lane_groups buckets every sparse-keyed sequence, none dropped");
        for (size_t i = 0; i < act.size(); ++i) {
            const int k = view.on_track_key(act[i]);
            if (k != 100000 + 0) continue;
            CHECK(groups[i].size() == 2,
                  "lane_groups puts both sequences of a mapped track in one bucket");
        }
        view.on_track_key = 0;
        for (int i = 60; i < 66; ++i) perf.set_active(i, false);
    }

    //------------------------------------------------------------------------
    //  Part 1 -- an audio SPLIT must produce two REAL engine regions.
    //
    //  The view is wired to a live AudioClipPlayer through mocks that mirror
    //  the shell exactly (seq -> stable region id; a slice is a NEW region
    //  over the same shared source).  The assertions are about the PLAYER's
    //  schedule -- the thing that renders -- not about m_region, which can
    //  show a convincing cut while playback still runs the whole file.
    //------------------------------------------------------------------------
    {
        using PatchKnob::engine::AudioClipPlayer;
        using PatchKnob::engine::ScheduledClip;
        AudioClipPlayer player;
        player.prepare(48000.0, 512);
        const long SPT = 10;                       // mock samples-per-tick
        static AudioClip splitSrc =
            AudioClip::synth_sine(220.0, 4.0, 48000.0, 0.5f, "split-src");
        const long clipTicks = (long)(splitSrc.numFrames() / SPT);   // 19200

        std::map<int, unsigned long long> seqRegion;   // mirrors g_seqRegion
        const int laneSeq = 40;
        perf.new_sequence(laneSeq); perf.set_active(laneSeq, true);
        sequence* ls = perf.get_sequence(laneSeq);
        ls->set_name("acut"); ls->set_track_kind(1);
        ls->set_length(std::max<long>(2, clipTicks));
        ls->add_trigger(0, clipTicks, 0, false);
        view.set_audio_clip(laneSeq, &splitSrc, clipTicks);
        view.set_audio_region(laneSeq, 0, clipTicks, 0);
        seqRegion[laneSeq] = player.addRegion(&splitSrc, 0, 1.f);
        // Group the split pieces onto ONE lane (the shell's on_track_key maps
        // every slice to its mixer track); 50..59 is a SECOND audio lane so
        // the rubber band can be proven to reach across lanes.
        view.on_track_key = [](int sq) -> int {
            if (sq >= 40 && sq < 50) return 100000 + 90;
            if (sq >= 50 && sq < 60) return 100000 + 91;
            return sq;
        };
        const int laneB = 50;
        perf.new_sequence(laneB); perf.set_active(laneB, true);
        sequence* lb = perf.get_sequence(laneB);
        lb->set_name("acut2"); lb->set_track_kind(1);
        lb->set_length(std::max<long>(2, clipTicks));
        lb->add_trigger(0, clipTicks, 0, false);
        view.set_audio_clip(laneB, &splitSrc, clipTicks);
        view.set_audio_region(laneB, 0, clipTicks, 0);

        std::vector<int> newSeqs;
        view.on_create_pattern = [&](int srcSeq, long start, long length,
                                     long offset, bool copyEvents) -> int {
            (void)offset; (void)copyEvents;
            int idx = -1;
            for (int i = 41; i < 60; ++i) if (!perf.is_active(i)) { idx = i; break; }
            if (idx < 0) return -1;
            perf.new_sequence(idx); perf.set_active(idx, true);
            sequence* d = perf.get_sequence(idx);
            sequence* sc = perf.get_sequence(srcSeq);
            d->set_track_kind(1);
            if (sc) {
                d->set_length(sc->get_length(), false);
                d->set_arrange_lane_id(sc->get_arrange_lane_id());
                d->set_midi_bus(sc->get_midi_bus());
            }
            d->clear_triggers();
            d->add_trigger(start, length, 0, false);
            return idx;
        };
        view.on_clip_duplicated = [&](int srcSeq, int newSeq, long startTick,
                                      long srcOffTick, long lenTick){
            (void)srcSeq;
            const unsigned long long id =
                player.addRegion(&splitSrc, startTick * SPT, 1.f);
            player.setClipRegion(player.regionIndex(id), startTick * SPT,
                                 srcOffTick * SPT, lenTick * SPT);
            seqRegion[newSeq] = id;
            newSeqs.push_back(newSeq);
            view.set_audio_clip(newSeq, &splitSrc, clipTicks);
        };
        view.on_clip_region_changed = [&](int seq, long startTick,
                                          long lengthTick, long offsetTick){
            std::map<int, unsigned long long>::iterator it = seqRegion.find(seq);
            if (it == seqRegion.end()) return;
            const int i = player.regionIndex(it->second);
            if (i < 0) return;
            player.setClipRegion(i, startTick * SPT, offsetTick * SPT,
                                 lengthTick * SPT);
        };

        const long cut = (clipTicks / 2 / BAR) * BAR;          // grid-aligned
        view.dbg_split_at(laneSeq, cut);
        CHECK(newSeqs.size() == 1, "audio split creates one right-half lane");
        CHECK(player.clipCount() == 2, "audio split schedules TWO engine regions");
        const int rightSeq = newSeqs.empty() ? -1 : newSeqs[0];
        const ScheduledClip* left =
            player.scheduled(player.regionIndex(seqRegion[laneSeq]));
        const ScheduledClip* right = rightSeq >= 0
            ? player.scheduled(player.regionIndex(seqRegion[rightSeq])) : 0;
        CHECK(left && left->startSample == 0 && left->sourceOffset == 0 &&
              left->length == (long long)cut * SPT,
              "left engine region is {P, S, B}");
        CHECK(right && right->startSample == (long long)cut * SPT &&
              right->sourceOffset == (long long)cut * SPT &&
              right->length == (long long)(clipTicks - cut) * SPT,
              "right engine region is {P+B, S+B, L-B}");

        // A second cut through the LEFT piece: its own region trims, the
        // sibling's region is untouched -- the exact edit the pointer-matched
        // lookup used to mis-address.
        const long cut2 = (cut / 2 / BAR) * BAR;
        view.dbg_split_at(laneSeq, cut2);
        CHECK(player.clipCount() == 3, "second split schedules a third region");
        left = player.scheduled(player.regionIndex(seqRegion[laneSeq]));
        CHECK(left && left->length == (long long)cut2 * SPT,
              "left piece trims to the second cut");
        right = player.scheduled(player.regionIndex(seqRegion[rightSeq]));
        CHECK(right && right->startSample == (long long)cut * SPT &&
              right->length == (long long)(clipTicks - cut) * SPT,
              "first right piece is untouched by the second split");

        //--------------------------------------------------------------------
        //  Part 2 -- selection gestures + Consolidate over selected clips.
        //--------------------------------------------------------------------
        const int midSeq = newSeqs.size() >= 2 ? newSeqs[1] : -1;
        SDL_Rect ra{}, rb{}, rc{}, rd{};
        bool haveRects = false;
        // The two audio lanes are the LAST rows -- scroll until they are on
        // screen, or the synthetic clicks land outside the canvas and select
        // via the (degenerate) lasso instead of the click paths under test.
        for (int hop = 0; hop < 12; ++hop) {
            view.draw(app);                    // refresh clip rects
            haveRects = view.dbg_clip_rect(laneSeq, ra) &&
                        midSeq >= 0 && view.dbg_clip_rect(midSeq, rb) &&
                        view.dbg_clip_rect(rightSeq, rc) &&
                        view.dbg_clip_rect(laneB, rd);
            if (haveRects && ra.y > 60 && rd.y + rd.h < app.h) break;
            view.on_key(app, SDLK_PAGEDOWN);
            haveRects = false;
        }
        CHECK(haveRects, "all cut pieces + the second lane have on-screen rects");

        // plain click selects exactly one
        view.on_key(app, SDLK_g);              // Grabber
        click(app, view, ra.x + ra.w / 2, ra.y + ra.h / 2);
        CHECK(view.dbg_clip_selected(laneSeq) && view.dbg_selected_clips() == 1,
              "plain click selects just the clicked piece");

        // Shift+click toggles pieces in and out of the selection
        SDL_SetModState(KMOD_LSHIFT);
        click(app, view, rc.x + rc.w / 2, rc.y + rc.h / 2);
        SDL_SetModState(KMOD_NONE);
        CHECK(view.dbg_clip_selected(laneSeq) && view.dbg_clip_selected(rightSeq) &&
              view.dbg_selected_clips() == 2,
              "Shift+click ADDS a second piece to the selection");
        SDL_SetModState(KMOD_LSHIFT);
        click(app, view, rc.x + rc.w / 2, rc.y + rc.h / 2);
        SDL_SetModState(KMOD_NONE);
        CHECK(view.dbg_clip_selected(laneSeq) && !view.dbg_clip_selected(rightSeq),
              "Shift+click again toggles that piece back OUT");

        // rubber-band drag starting on empty audio-lane canvas: every clip
        // the rectangle intersects becomes selected, ACROSS lanes (the sweep
        // runs from beyond the last piece on lane A down into lane B).
        const int lx0 = rc.x + rc.w + 40, ly0 = ra.y + 2;
        const int lx1 = ra.x + 4,         ly1 = rd.y + rd.h - 2;
        view.on_mouse(app, MouseEv{ lx0, ly0, SDL_BUTTON_LEFT, true });
        view.on_mouse(app, MouseEv{ (lx0 + lx1) / 2, ly1, SDL_BUTTON_LEFT, true });
        view.on_mouse(app, MouseEv{ lx1, ly1, SDL_BUTTON_LEFT, true });
        view.on_mouse(app, MouseEv{ lx1, ly1, SDL_BUTTON_LEFT, false });
        CHECK(view.dbg_clip_selected(laneSeq) && view.dbg_clip_selected(midSeq) &&
              view.dbg_clip_selected(rightSeq) && view.dbg_clip_selected(laneB) &&
              view.dbg_selected_clips() == 4,
              "rubber-band drag selects every intersected clip across lanes");

        // Consolidate the SELECTED clips: the hook receives exactly the
        // selected pieces and the span from first start to last end, wrapped
        // in ONE undo entry.
        std::vector<int> gotSeqs; long gotA = -1, gotB = -1; int calls = 0;
        view.on_consolidate_clips = [&](const std::vector<int>& seqs,
                                        long aTick, long bTick){
            gotSeqs = seqs; gotA = aTick; gotB = bTick; ++calls;
        };
        // deselect the middle piece (its gap must land INSIDE the span) and
        // the lane-B clip (so exactly one lane consolidates)
        SDL_SetModState(KMOD_LSHIFT);
        click(app, view, rb.x + rb.w / 2, rb.y + rb.h / 2);
        click(app, view, rd.x + rd.w / 2, rd.y + rd.h / 2);
        SDL_SetModState(KMOD_NONE);
        view.dbg_select(-1, -1, -1, -1);       // no edit RANGE: clip selection rules
        const int undo0 = view.dbg_undo_len();
        SDL_SetModState((SDL_Keymod)(KMOD_LALT | KMOD_LSHIFT));
        view.on_key(app, SDLK_3);              // Alt+Shift+3 = Consolidate
        SDL_SetModState(KMOD_NONE);
        CHECK(calls == 1, "Consolidate fires the clip-selection hook once");
        CHECK(gotSeqs.size() == 2 && gotA == 0 && gotB == clipTicks,
              "hook gets exactly the selected pieces + the full first..last span");
        CHECK(view.dbg_undo_len() == undo0 + 1,
              "clip-selection Consolidate lands as ONE undo entry");

        // teardown: unbind mocks, drop the audio lanes
        view.on_create_pattern = 0;
        view.on_clip_duplicated = 0;
        view.on_clip_region_changed = 0;
        view.on_consolidate_clips = 0;
        view.on_track_key = 0;
        view.set_audio_clip(laneSeq, nullptr, 0);
        view.set_audio_clip(laneB, nullptr, 0);
        if (midSeq >= 0) view.set_audio_clip(midSeq, nullptr, 0);
        if (rightSeq >= 0) view.set_audio_clip(rightSeq, nullptr, 0);
        perf.set_active(laneSeq, false);
        perf.set_active(laneB, false);
        if (midSeq >= 0) perf.set_active(midSeq, false);
        if (rightSeq >= 0) perf.set_active(rightSeq, false);
    }

    align_audit(app, perf, view);

    // =====================================================================
    //  LOOPED CLIP PLAYBACK.  The alignment audit covered editing; this covers
    //  what a LOOPING clip actually sounds over many repetitions.  Everything
    //  is observed through the same emit tap driving play_triggered(), so it
    //  is what sounds, not what the model claims.
    {
        std::printf("\n[loop playback] repetition phase, drift, overhang, boundaries\n");
        using align::Note; using align::Rng; using align::build_pattern;
        using align::render; using align::g_hung;
        const long S16L = (long)c_ppqn / 4;
        const long BARL = (long)c_ppqn * 4;

        //  ---- P1: PLACEMENT INVARIANCE ---------------------------------
        //  A loop period that divides neither the bar nor the clip length.
        //  The same clip placed at different song ticks must sound the SAME
        //  pattern relative to its own left edge; a repetition grid laid out
        //  from song zero would make the first step depend on placement.
        {
            const long PL = 4 * BARL;
            const long WIN = 5 * S16L;               // 5 sixteenths: divides nothing
            std::vector<std::vector<Note> > runs;
            const long starts[4] = { 0, 293, 7 * BARL, 12 * BARL + 991 };
            for (int i = 0; i < 4; ++i) {
                const int L = 140 + i;
                perf.new_sequence(L); perf.set_active(L, true);
                sequence* sl = perf.get_sequence(L);
                sl->set_name("loopinv"); sl->set_midi_bus(3); sl->set_midi_channel(4);
                sl->set_length(PL, false);
                sl->set_loop_start(0); sl->set_loop_end(WIN); sl->set_loop_start(0);
                sl->set_loop_enabled(true);
                Rng r2(0xA11CE + 0u);                // SAME material every placement
                build_pattern(sl, PL, r2, WIN);
                sl->clear_triggers();
                sl->add_trigger(starts[i], 6 * WIN, 0, false);
                std::vector<int> one(1, L);
                std::vector<Note> got = render(perf, one, starts[i], starts[i] + 6 * WIN);
                //  Re-express relative to the clip's own start.
                for (size_t k = 0; k < got.size(); ++k) got[k].tick -= starts[i];
                std::sort(got.begin(), got.end());
                runs.push_back(got);
            }
            bool same = true;
            for (size_t i = 1; i < runs.size(); ++i)
                if (runs[i] != runs[0]) same = false;
            CHECK(!runs[0].empty(), "loop: the placement-invariance clip sounds at all");
            CHECK(same, "loop: a non-dividing period sounds identically wherever the clip sits");
            for (int i = 0; i < 4; ++i) perf.set_active(140 + i, false);
        }

        //  ---- P2: NO DRIFT OVER MANY REPETITIONS ------------------------
        //  Every repetition must be an exact translation of the first by k*P.
        //  Any accumulation shows up as a repetition that no longer matches.
        {
            const long PL = 2 * BARL;
            const long WIN = 7 * S16L;               // 7/16 against a 4/4 bar
            const int  L = 150;
            perf.new_sequence(L); perf.set_active(L, true);
            sequence* sl = perf.get_sequence(L);
            sl->set_name("loopdrift"); sl->set_midi_bus(2); sl->set_midi_channel(3);
            sl->set_length(PL, false);
            sl->set_loop_start(0); sl->set_loop_end(WIN); sl->set_loop_start(0);
            sl->set_loop_enabled(true);
            Rng r3(0xD817);
            build_pattern(sl, PL, r3, WIN);
            const long P = sl->repeat_period();
            const long T0 = 3 * BARL + 517;          // off every grid
            const int  REPS = 40;
            sl->clear_triggers();
            sl->add_trigger(T0, REPS * P, 0, false);
            std::vector<int> one(1, L);
            const std::vector<Note> got = render(perf, one, T0, T0 + REPS * P - 1);
            CHECK(g_hung == 0, "loop: 40 repetitions leave no hung note");

            //  Bucket by repetition and compare each against repetition 0.
            std::map<long, std::vector<Note> > byRep;
            for (size_t k = 0; k < got.size(); ++k) {
                const long rel = got[k].tick - T0;
                Note n = got[k]; n.tick = rel % P;
                byRep[rel / P].push_back(n);
            }
            for (std::map<long, std::vector<Note> >::iterator it = byRep.begin();
                 it != byRep.end(); ++it) std::sort(it->second.begin(), it->second.end());
            bool allMatch = !byRep.empty() && byRep.count(0) && !byRep[0].empty();
            long badRep = -1;
            for (std::map<long, std::vector<Note> >::const_iterator it = byRep.begin();
                 it != byRep.end(); ++it) {
                //  The last repetition may be clipped by the trigger end.
                if (it->first == (long)REPS - 1) continue;
                if (it->second != byRep[0]) { allMatch = false; badRep = it->first; }
            }
            char msg[128];
            std::snprintf(msg, sizeof msg,
                "loop: 40 repetitions are exact translations (first mismatch rep %ld)", badRep);
            CHECK(allMatch, msg);
            perf.set_active(L, false);
        }

        //  ---- P3: WINDOW OVERHANGING THE DATA ---------------------------
        //  loop_end past m_length is the documented way to hang an odd-length
        //  loop over a shorter phrase: the window wraps at loop_end, but the
        //  DATA stops at m_length, so each repetition ends in silence.
        {
            const long PL  = 6 * S16L;               // data: six sixteenths
            const long WIN = 8 * S16L;               // window: eight -> 2/16 silence
            const int  L = 151;
            perf.new_sequence(L); perf.set_active(L, true);
            sequence* sl = perf.get_sequence(L);
            sl->set_name("loopover"); sl->set_midi_bus(1); sl->set_midi_channel(2);
            sl->set_length(PL, false);
            sl->set_loop_start(0); sl->set_loop_end(WIN); sl->set_loop_start(0);
            sl->set_loop_enabled(true);
            Rng r4(0x0FA5);
            build_pattern(sl, PL, r4, PL);
            const long P = sl->repeat_period();
            CHECK(P == WIN, "loop: an overhanging window keeps the window's period");
            const long T0 = 5 * BARL;
            sl->clear_triggers();
            sl->add_trigger(T0, 4 * P, 0, false);
            std::vector<int> one(1, L);
            const std::vector<Note> got = render(perf, one, T0, T0 + 4 * P - 1);
            CHECK(g_hung == 0, "loop: an overhanging window leaves no hung note");
            bool inData = true;
            for (size_t k = 0; k < got.size(); ++k) {
                const long ph = (got[k].tick - T0) % P;
                if (ph >= PL) inData = false;        // sounded inside the silent tail
            }
            CHECK(inData, "loop: nothing sounds in the silent tail past the end marker");
            CHECK(!got.empty(), "loop: the overhanging window still sounds its data");
        }

        //  ---- P4: A NOTE HELD ACROSS THE LOOP BOUNDARY ------------------
        //  It must be released at the wrap, never left hanging, and never
        //  still sounding when the next repetition re-strikes it.
        {
            const long PL  = 4 * S16L;
            const int  L = 152;
            perf.new_sequence(L); perf.set_active(L, true);
            sequence* sl = perf.get_sequence(L);
            sl->set_name("loopheld"); sl->set_midi_bus(0); sl->set_midi_channel(1);
            sl->set_length(PL, false);
            //  One note that starts inside the window and runs PAST its end.
            sl->add_note_velocity(S16L, 3 * S16L + 7, 64, 100);
            sl->verify_and_link();
            sl->set_loop_start(0); sl->set_loop_end(3 * S16L); sl->set_loop_start(0);
            sl->set_loop_enabled(true);
            const long P = sl->repeat_period();
            const long T0 = 2 * BARL;
            sl->clear_triggers();
            sl->add_trigger(T0, 5 * P, 0, false);
            std::vector<int> one(1, L);
            const std::vector<Note> got = render(perf, one, T0, T0 + 5 * P - 1);
            CHECK(g_hung == 0, "loop: a note held across the wrap is released, not hung");
            bool noOverlap = true;
            for (size_t k = 0; k + 1 < got.size(); ++k)
                if (got[k].pitch == got[k+1].pitch &&
                    got[k].tick + got[k].dur > got[k+1].tick) noOverlap = false;
            CHECK(noOverlap, "loop: the held note is off before the next repetition re-strikes it");
            bool withinRep = true;
            for (size_t k = 0; k < got.size(); ++k)
                if (got[k].dur > P) withinRep = false;
            CHECK(withinRep, "loop: no sounding note outlives one repetition");
        }

        //  ---- P5: TRIGGER SHORTER THAN ONE REPETITION -------------------
        //  A clip dragged shorter than its own loop must simply stop; nothing
        //  may sound past the trigger's end.
        {
            const long PL  = 4 * BARL;
            const long WIN = 6 * S16L;
            const int  L = 153;
            perf.new_sequence(L); perf.set_active(L, true);
            sequence* sl = perf.get_sequence(L);
            sl->set_name("loopshort"); sl->set_midi_bus(6); sl->set_midi_channel(7);
            sl->set_length(PL, false);
            sl->set_loop_start(0); sl->set_loop_end(WIN); sl->set_loop_start(0);
            sl->set_loop_enabled(true);
            Rng r5(0x51073);
            build_pattern(sl, PL, r5, WIN);
            const long T0 = 9 * BARL + 61;
            const long TLEN = 3 * S16L + 11;          // less than one period
            sl->clear_triggers();
            sl->add_trigger(T0, TLEN, 0, false);
            std::vector<int> one(1, L);
            const std::vector<Note> got = render(perf, one, T0, T0 + 8 * WIN);
            bool inside = true;
            for (size_t k = 0; k < got.size(); ++k)
                if (got[k].tick >= T0 + TLEN) inside = false;
            CHECK(inside, "loop: a clip shorter than its period sounds nothing past its end");
            CHECK(g_hung == 0, "loop: the short clip leaves no hung note");
        }
    }

    // =====================================================================
    //  MULTIPLE PLACEMENTS OF ONE PATTERN.  seq24's model is one pattern, many
    //  triggers -- and m_trigger_offset / m_play_anchor are SINGLE members
    //  rewritten per trigger. If any per-trigger state leaks across placements
    //  inside one render pass, the second and third copies play the first
    //  one's content grid. Rendered in ONE call spanning all of them, which is
    //  how the output thread actually sees them.
    {
        std::printf("\n[multi-placement] one pattern, several triggers, one pass\n");
        using align::Note; using align::Rng; using align::build_pattern;
        using align::render; using align::g_hung;
        const long S16L = (long)c_ppqn / 4;
        const long BARL = (long)c_ppqn * 4;

        const long PL = 2 * BARL;
        const int  L = 160;
        perf.new_sequence(L); perf.set_active(L, true);
        sequence* sm = perf.get_sequence(L);
        sm->set_name("multiplace"); sm->set_midi_bus(4); sm->set_midi_channel(5);
        sm->set_length(PL, false);
        Rng rm(0x33AC3);
        build_pattern(sm, PL, rm, 0);

        //  One placement alone gives the reference content grid.
        const long REF = 40 * BARL;
        sm->clear_triggers();
        sm->add_trigger(REF, PL, 0, false);
        std::vector<int> one(1, L);
        std::vector<Note> ref = render(perf, one, REF, REF + PL - 1);
        for (size_t k = 0; k < ref.size(); ++k) ref[k].tick -= REF;
        std::sort(ref.begin(), ref.end());
        CHECK(!ref.empty(), "multi: the reference placement sounds");

        //  Three placements, deliberately at ugly positions, all offset 0 so
        //  each must reproduce the reference exactly.
        const long P0 = 0, P1 = 5 * BARL + 379, P2 = 11 * BARL + 1723;
        sm->clear_triggers();
        sm->add_trigger(P0, PL, 0, false);
        sm->add_trigger(P1, PL, 0, false);
        sm->add_trigger(P2, PL, 0, false);
        const std::vector<Note> got = render(perf, one, 0, P2 + PL + BARL);
        CHECK(g_hung == 0, "multi: three placements leave no hung note");

        const long bases[3] = { P0, P1, P2 };
        for (int i = 0; i < 3; ++i) {
            std::vector<Note> mine;
            for (size_t k = 0; k < got.size(); ++k)
                if (got[k].tick >= bases[i] && got[k].tick < bases[i] + PL) {
                    Note n = got[k]; n.tick -= bases[i]; mine.push_back(n);
                }
            std::sort(mine.begin(), mine.end());
            char msg[96];
            std::snprintf(msg, sizeof msg,
                "multi: placement %d plays the reference content exactly", i);
            CHECK(mine == ref, msg);
        }

        //  Now give each placement a DIFFERENT content offset. Placement i must
        //  play the pattern from offset[i] onward, anchored at its own start --
        //  this is where a leaked m_trigger_offset shows up immediately.
        const long offs[3] = { 0, 3 * S16L, 7 * S16L + 11 };
        sm->clear_triggers();
        for (int i = 0; i < 3; ++i)
            sm->add_trigger(bases[i], PL, offs[i], false);
        const std::vector<Note> got2 = render(perf, one, 0, P2 + PL + BARL);
        CHECK(g_hung == 0, "multi: offset placements leave no hung note");

        bool allOk = true; int badIdx = -1;
        for (int i = 0; i < 3; ++i) {
            //  These clips LOOP (m_loop_enabled defaults true), so an offset
            //  ROTATES the content rather than trimming it: a note at pattern
            //  position ts sounds at base + ((ts - offs + PL) mod PL), and the
            //  notes before the offset come back round at the tail. (An
            //  earlier version of this check asserted the one-shot trim-in
            //  rule here and failed -- the test was wrong, not the engine.)
            std::vector<Note> want;
            for (size_t k = 0; k < ref.size(); ++k) {
                Note n = ref[k];
                n.tick = ((ref[k].tick - offs[i]) % PL + PL) % PL;
                want.push_back(n);
            }
            std::sort(want.begin(), want.end());
            std::vector<Note> mine;
            for (size_t k = 0; k < got2.size(); ++k)
                if (got2[k].tick >= bases[i] && got2[k].tick < bases[i] + PL) {
                    Note n = got2[k]; n.tick -= bases[i]; mine.push_back(n);
                }
            std::sort(mine.begin(), mine.end());
            //  A note trimmed by the offset may have its tail clipped by the
            //  placement end; compare onsets and pitches, which cannot shift.
            std::vector<std::pair<long,int> > wa, ma;
            for (size_t k = 0; k < want.size(); ++k) wa.push_back(std::make_pair(want[k].tick, want[k].pitch));
            for (size_t k = 0; k < mine.size(); ++k) ma.push_back(std::make_pair(mine[k].tick, mine[k].pitch));
            if (wa != ma) { allOk = false; if (badIdx < 0) badIdx = i; }
        }
        char msg2[128];
        std::snprintf(msg2, sizeof msg2,
            "multi: each placement honours its OWN content offset (first bad: %d)", badIdx);
        CHECK(allOk, msg2);

        //  ONE-SHOT placements with offsets: here the offset really is a
        //  trim-IN. Placement i plays only ts >= offs[i], at base+(ts-offs[i]),
        //  and nothing wraps round.
        sm->set_loop_enabled(false);
        sm->clear_triggers();
        for (int i = 0; i < 3; ++i)
            sm->add_trigger(bases[i], PL, offs[i], false);
        const std::vector<Note> got3 = render(perf, one, 0, P2 + PL + BARL);
        CHECK(g_hung == 0, "multi: one-shot offset placements leave no hung note");
        bool osOk = true; int osBad = -1;
        for (int i = 0; i < 3; ++i) {
            std::vector<std::pair<long,int> > wa, ma;
            for (size_t k = 0; k < ref.size(); ++k)
                if (ref[k].tick >= offs[i])
                    wa.push_back(std::make_pair(ref[k].tick - offs[i], ref[k].pitch));
            for (size_t k = 0; k < got3.size(); ++k)
                if (got3[k].tick >= bases[i] && got3[k].tick < bases[i] + PL)
                    ma.push_back(std::make_pair(got3[k].tick - bases[i], got3[k].pitch));
            std::sort(wa.begin(), wa.end()); std::sort(ma.begin(), ma.end());
            if (wa != ma) { osOk = false; if (osBad < 0) osBad = i; }
        }
        char msg3[128];
        std::snprintf(msg3, sizeof msg3,
            "multi: one-shot placements TRIM at their offset, no wrap (first bad: %d)", osBad);
        CHECK(osOk, msg3);
        sm->set_loop_enabled(true);
        perf.set_active(L, false);
    }

    // =====================================================================
    //  SONG-LOOP WRAP AUDIT -- see the songloop namespace above.  Many wraps,
    //  every pass audited individually: "some notes came out" would pass while
    //  the reported bug ("silence for a few loops, then it comes back") is
    //  live, so per-pass identity is the only meaningful assertion.
    {
        std::printf("\n[song loop] transport-loop wraps: per-pass identity\n");
        using align::Note; using align::Rng; using align::build_pattern;
        using songloop::WEmit; using songloop::On;
        const long S16L = (long)c_ppqn / 4;
        const long BARL = (long)c_ppqn * 4;
        //  The audio-paced scheduler's real proportions at 120 BPM: the
        //  lookahead is ~41 ms (~64 ticks), the poll advances a few ticks.
        const long LOOKAHEAD = 64, STEP = 6;

        //  ---- W1: plain clip filling the loop exactly -------------------
        {
            const int  Q = 170;
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-plain"); s->set_midi_bus(5); s->set_midi_channel(6);
            s->set_length(4 * BARL, false);
            Rng r(0x51D01u);
            build_pattern(s, 4 * BARL, r, 0);
            s->clear_triggers();
            s->add_trigger(0, 4 * BARL, 0, false);
            const std::vector<On> want =
                songloop::ground_truth(perf, s, 0, 4 * BARL);
            CHECK(!want.empty(), "song loop[plain]: the clip sounds at all");
            const std::vector<WEmit> ev = songloop::run(
                perf, 0, 4 * BARL, 0, 25, LOOKAHEAD, STEP, 0xB007u);
            songloop::audit(perf, s, ev, want, 25, "plain");
            perf.set_active(Q, false);
        }

        //  ---- W2: loop markers mid-song, clip STRADDLING both markers,
        //  playback starting mid-loop.  The wrap's set_orig_ticks(L) lands
        //  inside the trigger, not at its start.
        {
            const int  Q = 171;
            const long L0 = 4 * BARL, R0 = 8 * BARL;
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-straddle"); s->set_midi_bus(6); s->set_midi_channel(7);
            s->set_length(2 * BARL, false);
            Rng r(0x57ADD1u);
            build_pattern(s, 2 * BARL, r, 0);
            s->clear_triggers();
            s->add_trigger(3 * BARL, 6 * BARL, 0, false);   // [3,9) bars
            const std::vector<On> want = songloop::ground_truth(perf, s, L0, R0);
            CHECK(!want.empty(), "song loop[straddle]: the clip sounds at all");
            const std::vector<WEmit> ev = songloop::run(
                perf, L0, R0, L0 + 123, 25, LOOKAHEAD, STEP, 0x57217u);
            songloop::audit(perf, s, ev, want, 25, "straddle");
            perf.set_active(Q, false);
        }

        //  ---- W3: the clip carries its own pattern-local LOOP WINDOW (the
        //  user's "clip looped") while the transport loop wraps around it.
        //  A 5/16 window divides neither the bar nor the transport loop.
        {
            const int  Q = 172;
            const long L0 = 4 * BARL, R0 = 8 * BARL;
            const long WIN = 5 * S16L;
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-cliploop"); s->set_midi_bus(7); s->set_midi_channel(8);
            s->set_length(2 * BARL, false);
            s->set_loop_start(0); s->set_loop_end(WIN); s->set_loop_start(0);
            s->set_loop_enabled(true);
            Rng r(0xC11B00u);
            build_pattern(s, 2 * BARL, r, WIN);
            s->clear_triggers();
            s->add_trigger(3 * BARL + 57, 6 * BARL, 0, false);  // ugly start
            const std::vector<On> want = songloop::ground_truth(perf, s, L0, R0);
            CHECK(!want.empty(), "song loop[clip-loop]: the looping clip sounds at all");
            const std::vector<WEmit> ev = songloop::run(
                perf, L0, R0, L0, 25, LOOKAHEAD, STEP, 0xC11B1u);
            songloop::audit(perf, s, ev, want, 25, "clip-loop");
            perf.set_active(Q, false);
        }

        //  ---- W4: a note HELD ACROSS the loop-right marker.  The wrap must
        //  cut it (boundary offs) and the next pass must strike it again --
        //  losing the re-strike is exactly the reported silence.
        {
            const int  Q = 173;
            const long L0 = 0, R0 = 2 * BARL;
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-held"); s->set_midi_bus(8); s->set_midi_channel(9);
            s->set_length(4 * BARL, false);
            //  starts inside the loop, written to sustain far past R0
            s->add_note_velocity(BARL, 2 * BARL + 400, 52, 96);
            s->add_note_velocity(2 * S16L, S16L, 76, 80);
            s->verify_and_link();
            s->clear_triggers();
            s->add_trigger(0, 4 * BARL, 0, false);
            const std::vector<On> want = songloop::ground_truth(perf, s, L0, R0);
            CHECK(want.size() == 2, "song loop[held]: both notes sound in one pass");
            const std::vector<WEmit> ev = songloop::run(
                perf, L0, R0, 0, 25, LOOKAHEAD, STEP, 0x8E1Du);
            songloop::audit(perf, s, ev, want, 25, "held");
            perf.set_active(Q, false);
        }

        //  ---- W5: SHORT loop (one beat).  Wraps come fast; run twice the
        //  wrap count.  A short loop is where a stale m_last_tick or a missed
        //  head window eats a whole pass instead of a fraction of one.
        {
            const int  Q = 174;
            const long R0 = (long)c_ppqn;                   // one beat
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-short"); s->set_midi_bus(9); s->set_midi_channel(10);
            s->set_length(R0, false);
            s->add_note_velocity(0, 120, 60, 100);
            s->add_note_velocity(R0 / 4, 120, 64, 90);
            s->add_note_velocity(R0 / 2, 120, 67, 85);
            s->add_note_velocity(3 * R0 / 4, 120, 72, 110);
            s->verify_and_link();
            s->clear_triggers();
            s->add_trigger(0, R0, 0, false);
            const std::vector<On> want = songloop::ground_truth(perf, s, 0, R0);
            CHECK(want.size() == 4, "song loop[short]: all four notes sound in one pass");
            const std::vector<WEmit> ev = songloop::run(
                perf, 0, R0, 0, 50, LOOKAHEAD, STEP, 0x5807u);
            songloop::audit(perf, s, ev, want, 50, "short");
            perf.set_active(Q, false);
        }

        //  ---- W6: loop SHORTER than the scheduler lookahead.  The horizon
        //  is past the loop end the moment the wrap lands, so the tail branch
        //  runs back-to-back and the leftover clamp (degenerate markers)
        //  engages every pass.
        {
            const int  Q = 175;
            const long R0 = S16L;                            // one sixteenth
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-tiny"); s->set_midi_bus(10); s->set_midi_channel(11);
            s->set_length(R0, false);
            s->add_note_velocity(0, 90, 48, 100);
            s->add_note_velocity(R0 / 2, 80, 55, 95);
            s->verify_and_link();
            s->clear_triggers();
            s->add_trigger(0, R0, 0, false);
            const std::vector<On> want = songloop::ground_truth(perf, s, 0, R0);
            CHECK(want.size() == 2, "song loop[tiny]: both notes sound in one pass");
            const std::vector<WEmit> ev = songloop::run(
                perf, 0, R0, 0, 50, 300 /* lookahead >> loop */, STEP, 0x71D7u);
            songloop::audit(perf, s, ev, want, 50, "tiny");
            perf.set_active(Q, false);
        }

        //  ---- W7: the RIGHT MARKER MOVED while rolling (the live loop-edit
        //  handshake).  Every pass after the edit must sound the grown span;
        //  a stranded m_last_tick or last_scheduled shows up as silence here.
        {
            const int  Q = 176;
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-edit"); s->set_midi_bus(11); s->set_midi_channel(12);
            s->set_length(8 * BARL, false);
            Rng r(0xED17u);
            build_pattern(s, 8 * BARL, r, 0);
            s->clear_triggers();
            s->add_trigger(0, 8 * BARL, 0, false);
            const std::vector<On> want = songloop::ground_truth(perf, s, 0, 6 * BARL);
            CHECK(!want.empty(), "song loop[edit]: the clip sounds at all");
            //  25 wraps total; the loop grows 4 -> 6 bars at wrap 8, capture
            //  restarts there, leaving 17 audited wraps of the new geometry.
            const std::vector<WEmit> ev = songloop::run(
                perf, 0, 4 * BARL, 0, 25, LOOKAHEAD, STEP, 0xED18u,
                8, 6 * BARL);
            songloop::audit(perf, s, ev, want, 17, "edit");
            perf.set_active(Q, false);
        }

        //  ---- W8: loop switched on with the playhead already PAST the right
        //  marker.  The engine relocates to the loop start; the scheduler must
        //  recover and play every pass whole ("sometimes it's just silence").
        {
            const int  Q = 177;
            perf.new_sequence(Q); perf.set_active(Q, true);
            sequence* s = perf.get_sequence(Q);
            s->set_name("wrap-behind"); s->set_midi_bus(12); s->set_midi_channel(13);
            s->set_length(4 * BARL, false);
            Rng r(0xBE41Du);
            build_pattern(s, 4 * BARL, r, 0);
            s->clear_triggers();
            s->add_trigger(0, 4 * BARL, 0, false);
            const std::vector<On> want = songloop::ground_truth(perf, s, 0, 4 * BARL);
            CHECK(!want.empty(), "song loop[behind]: the clip sounds at all");
            const std::vector<WEmit> ev = songloop::run(
                perf, 0, 4 * BARL, 6 * BARL /* start 2 bars past R */,
                25, LOOKAHEAD, STEP, 0xBE42u);
            songloop::audit(perf, s, ev, want, 25, "behind");
            perf.set_active(Q, false);
        }

        //  ---- W9: THE REAL SCHEDULER, REAL ENGINE WRAPS, and an induced
        //  stall across the tail window.  perform::output_func's wrap branch
        //  only runs while the horizon hangs past the loop end -- a window one
        //  lookahead (~15-40 ms) wide.  The output thread contends on every
        //  sequence's recursive mutex with GUI drawing, so a stall covering
        //  that window is routine; when it happens the engine wraps anyway and
        //  last_scheduled is left stranded near the OLD pass's right edge --
        //  the else-branch then schedules NOTHING for almost the whole new
        //  pass.  That is the long-standing "silence for a few loops, then it
        //  comes back".  This drives the REAL output thread against the REAL
        //  engine transport and takes the mutex exactly the way the GUI does,
        //  across the tail window of passes 2, 5 and 8.
        {
            if (!PatchKnob::app::audio_app_init()) {
                std::printf("SKIP  song loop[stall]: no audio device -- the "
                            "real-scheduler stall test needs the engine\n");
            } else {
                PatchKnob::app::audio_app_set_silent_output(true);
                const int  Q    = 178;
                const long LOOP = 4 * BARL;
                perf.new_sequence(Q); perf.set_active(Q, true);
                sequence* s = perf.get_sequence(Q);
                s->set_name("wrap-stall"); s->set_midi_bus(13); s->set_midi_channel(14);
                s->set_length(LOOP, false);
                //  a note on every 16th: a lost pass is unmistakable
                for (long t = 0; t < LOOP; t += S16L)
                    s->add_note_velocity(t, S16L * 3 / 4,
                                         60 + (int)((t / S16L) % 12), 100);
                s->verify_and_link();
                s->clear_triggers();
                s->add_trigger(0, LOOP, 0, false);

                const std::vector<On> want = songloop::ground_truth(perf, s, 0, LOOP);
                CHECK(!want.empty(), "song loop[stall]: the clip sounds at all");

                perf.set_bpm(480.0);                    // ~2 s per pass
                perf.set_left_tick(0);
                perf.set_right_tick(LOOP);
                perf.set_starting_tick(0);
                perf.set_looping(true);

                std::vector<WEmit> ev; ev.reserve(1 << 17);
                songloop::cap = &ev; g_seq_emit_tap = &songloop::wtap;

                perf.launch_output_thread();
                perf.start(true);

                const long long Rsamp =
                    PatchKnob::app::audio_app_tick_to_sample(LOOP);
                const long long sr =
                    (long long) PatchKnob::app::audio_app_sample_rate();
                //  take the lock well BEFORE the tail window opens (the
                //  lookahead is max(2 blocks + 5 ms, 15 ms); 120 ms is early
                //  on any device) and hold it until just after the wrap
                const long long lead =
                    2LL * (long long) PatchKnob::app::audio_app_buffer_size()
                    + sr * 120 / 1000;
                const int PASSES = 10;
                const unsigned long long gen0 =
                    PatchKnob::app::audio_app_loop_wrap_generation();
                unsigned long long gen = gen0;
                int stalls = 0, waited = 0;
                while ((int)(gen - gen0) < PASSES && waited < 60000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    ++waited;
                    const unsigned long long g =
                        PatchKnob::app::audio_app_loop_wrap_generation();
                    const int passNo = (int)(g - gen0);
                    if (g == gen &&
                        (passNo == 2 || passNo == 5 || passNo == 8) &&
                        stalls < 3) {
                        const long long pos =
                            PatchKnob::app::audio_app_transport_sample();
                        if (pos >= Rsamp - lead) {
                            s->lock();              // what GUI drawing does
                            ++stalls;
                            for (int w = 0; w < 4000; ++w) {
                                if (PatchKnob::app::audio_app_loop_wrap_generation()
                                    != g) break;
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(1));
                            }
                            //  keep the thread pinned a little past the wrap
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(15));
                            s->unlock();
                        }
                    }
                    gen = g;
                }
                perf.stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                g_seq_emit_tap = 0; songloop::cap = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                perf.set_looping(false);
                perf.set_active(Q, false);

                CHECK((int)(gen - gen0) >= PASSES,
                      "song loop[stall]: the engine wrapped all passes");
                CHECK(stalls == 3,
                      "song loop[stall]: three tail-window stalls were induced");

                //  EVERY interior pass must sound everything from one beat
                //  after the loop start to two beats before its end.  The
                //  edges are excused because music due DURING a stall and
                //  beyond the already-queued lookahead is unrecoverable by any
                //  scheduler -- its samples were rendered while the thread was
                //  blocked; the head margin covers the recovery's audible-tick
                //  anchor.  What the recovery must guarantee is the body of
                //  the pass: without it a stalled tail window silences the
                //  ENTIRE next pass (last_scheduled stranded at the old right
                //  edge), which is what this CHECK catches.
                std::vector<On> raw = songloop::ons_of(ev, s);
                std::vector<std::vector<On> > passes;
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (passes.empty() || (!passes.back().empty() &&
                                           raw[i].first < passes.back().back().first))
                        passes.push_back(std::vector<On>());
                    passes.back().push_back(raw[i]);
                }
                const long grace = (long)c_ppqn;        // one beat (~125 ms)
                int badPass = -1, checkedP = 0, worstMiss = 0;
                for (size_t k = 1; k + 1 < passes.size(); ++k) {
                    ++checkedP;
                    int miss = 0;
                    for (size_t i = 0; i < want.size(); ++i) {
                        if (want[i].first < grace ||
                            want[i].first >= LOOP - 2 * (long)c_ppqn) continue;
                        bool found = false;
                        for (size_t j = 0; j < passes[k].size(); ++j)
                            if (passes[k][j] == want[i]) { found = true; break; }
                        if (!found) ++miss;
                    }
                    if (miss > 0 && badPass < 0) badPass = (int)k;
                    if (miss > worstMiss) worstMiss = miss;
                }
                char msg[200];
                std::snprintf(msg, sizeof msg,
                    "song loop[stall]: a tail-window stall never silences a pass "
                    "(%d passes, worst missing %d of %u ons, first bad: %d)",
                    checkedP, worstMiss, (unsigned)want.size(), badPass);
                CHECK(badPass < 0 && checkedP >= PASSES - 3, msg);

                PatchKnob::app::audio_app_shutdown();
            }
        }
    }

    // =====================================================================
    //  LOOP MARKERS.  seq24 refused any right marker inside the first bar and
    //  said nothing, so a loop brace dragged short snapped back with no
    //  feedback; and moving the left marker up shoved the right one a whole BAR
    //  out, silently widening a deliberately short loop.
    {
        std::printf("\n[loop markers] short windows are honoured\n");
        const long BEAT = (long)c_ppqn;
        const long BAR  = BEAT * 4;
        const long S16  = BEAT / 4;

        //  A half-bar loop from the very top of the song.
        perf.set_left_tick(0);
        perf.set_right_tick(BAR / 2);
        CHECK(perf.get_right_tick() == BAR / 2,
              "a half-bar right marker inside the first bar is accepted");
        CHECK(perf.get_left_tick() == 0, "and the left marker stays at zero");

        //  A one-beat loop, the shape a drum fill wants.
        perf.set_left_tick(0);
        perf.set_right_tick(BEAT);
        CHECK(perf.get_right_tick() == BEAT, "a one-beat loop is accepted");

        //  Below the minimum window it CLAMPS rather than silently doing nothing.
        perf.set_left_tick(0);
        perf.set_right_tick(1);
        CHECK(perf.get_right_tick() >= S16,
              "a degenerate right marker clamps to the minimum window");
        CHECK(perf.get_right_tick() > perf.get_left_tick(),
              "the window is never inverted or zero-width");

        //  Moving the LEFT marker up must not widen a short loop to a whole bar.
        perf.set_left_tick(0);
        perf.set_right_tick(BEAT);            // one-beat window
        perf.set_left_tick(8 * BAR);          // shove it far past the right
        const long w = perf.get_right_tick() - perf.get_left_tick();
        CHECK(w >= S16 && w <= BEAT,
              "pushing the left marker past the right keeps the loop SHORT, not a bar");
        CHECK(perf.get_right_tick() > perf.get_left_tick(),
              "and still leaves a valid window");

        //  A negative left marker is clamped, not stored.
        perf.set_left_tick(-1000);
        CHECK(perf.get_left_tick() >= 0, "a negative left marker clamps to zero");

        //  Restore something sane for anything that runs after this.
        perf.set_left_tick(0);
        perf.set_right_tick(4 * BAR);
    }

    std::printf(g_fails ? "SELFTEST: %d FAILED\n" : "SELFTEST: all passed\n",
                g_fails);
    return g_fails ? 1 : 0;
}

//----------------------------------------------------------------------------
//  --bench : headless draw-time benchmark over the two axes the large-asset
//  report names: CLIP COUNT (many ordinary clips) and CLIP LENGTH (few long
//  clips).  Synthesised audio only; run with SDL_VIDEODRIVER=dummy.  Prints a
//  table of ms/frame for a STATIC viewport (same geometry every frame -- the
//  waveform-cache hit path) and a SCROLLING viewport (geometry changes every
//  frame -- the cache-miss path).
//----------------------------------------------------------------------------
static double bench_ms(ui::App& app, arrange::ArrangeView& v, int frames,
                       long scrollStep, long scroll0, double scale)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    for (int i = 0; i < frames; ++i) {
        v.dbg_set_view(scroll0 + (long)i * scrollStep, scale);
        v.draw(app);
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count()
           / (double)(frames > 0 ? frames : 1);
}

static int bench(ui::App& app)
{
    using PatchKnob::engine::AudioClip;
    const double rate = 48000.0;
    const double tps  = (double)c_ppqn * 120.0 / 60.0;      // ticks per second

    // Shared sources: distinct SOURCE WINDOWS per region make every cache key
    // distinct, which is what 200 separate files would do, without 200 buffers.
    std::printf("synthesising audio...\n");
    static AudioClip longClip  = AudioClip::synth_sine(220.0, 600.0, rate, 0.8f, "long");
    static AudioClip shortClip = AudioClip::synth_sine(330.0,   5.0, rate, 0.8f, "short");

    struct Scen { const char* name; int clips; double seconds; const AudioClip* clip;
                  int laneCap; int overlap; bool universe; };
    // laneCap 0 = auto (min(clips,10)); overlap -1 = view default, 0/1 forced.
    const Scen scen[] = {
        { "1 clip  x 10 min",   1, 600.0, &longClip,  0, -1, false },
        { "5 clips x 10 min",   5, 600.0, &longClip,  0, -1, false },
        { "50 clips x 5 s",    50,   5.0, &shortClip, 0, -1, false },
        { "200 clips x 5 s",  200,   5.0, &shortClip, 0, -1, false },
        { "200 clips x 10 min", 200, 600.0, &longClip, 0, -1, false },
        { "200x5s ovl OFF",   200,   5.0, &shortClip, 0,  0, false },
        { "200x5s 1 lane",    200,   5.0, &shortClip, 1, -1, false },
        { "200x5s 1lane oOFF",200,   5.0, &shortClip, 1,  0, false },
        { "200x5s +universe", 200,   5.0, &shortClip, 0, -1, true  },
    };

    std::printf("%-20s | %10s | %10s | %10s\n",
                "scenario", "first(ms)", "static", "scroll");
    for (const Scen& sc : scen) {
        perform perf;
        arrange::ArrangeView view(&perf);
        view.rect = { 0, 0, app.w, app.h };
        app.roots = { &view };

        const int autoLanes = sc.clips < 10 ? sc.clips : 10;
        const int lanes   = sc.laneCap > 0 ? sc.laneCap : autoLanes;
        const int perLane = (sc.clips + lanes - 1) / lanes;
        view.on_track_key = [perLane](int s) { return (s / perLane) * perLane; };
        if (sc.overlap >= 0) view.dbg_set_wf_overlap(sc.overlap != 0);
        view.dbg_set_universe(sc.universe);

        const long lenTicks = (long)(sc.seconds * tps);
        for (int i = 0; i < sc.clips; ++i) {
            perf.new_sequence(i);
            perf.get_sequence(i)->set_name("bench");
            view.set_audio_clip(i, sc.clip, lenTicks);
            const int j = i % perLane;
            // distinct source window per region (as 200 separate takes would be)
            view.set_audio_region(i, (long)j * lenTicks,
                                  lenTicks - (long)(i + 1), (long)i);
        }
        const long total = (long)perLane * lenTicks;
        const double scale = (double)total / 1200.0;         // fit the canvas
        const int heavy = (sc.clips >= 200 && sc.seconds > 60.0) ? 2 : 20;

        const double first  = bench_ms(app, view, 1, 0, 0, scale);
        const double stat   = bench_ms(app, view, heavy, 0, 0, scale);
        const double scroll = bench_ms(app, view, heavy, (long)(3.0 * scale), 0, scale);
        std::printf("%-20s | %10.2f | %10.2f | %10.2f\n",
                    sc.name, first, stat, scroll);
        for (int i = 0; i < sc.clips; ++i) {
            view.set_audio_clip(i, nullptr);
            perf.delete_sequence(i);
        }
    }
    // ---- next_transient (Tab to Transients), timed on a 10-minute clip ----
    // A sine has no onsets, so both directions walk the WHOLE region: the
    // worst case the backlog names.  The click train checks the detector
    // still lands on the same ticks after any optimisation.
    {
        perform perf;
        arrange::ArrangeView view(&perf);
        view.rect = { 0, 0, app.w, app.h };
        app.roots = { &view };
        const long lenTicks = (long)(600.0 * tps);
        perf.new_sequence(0);
        perf.get_sequence(0)->set_name("t");
        view.set_audio_clip(0, &longClip, lenTicks);
        view.set_audio_region(0, 0, lenTicks, 0);
        // click train: 1 ms bursts once a second over 30 s
        static AudioClip clicks = AudioClip::synth_sine(0.0, 30.0, 48000.0, 0.0f, "clicks");
        for (int k = 1; k < 30; ++k)
            for (int i = 0; i < 48; ++i) {
                clicks.ch[0][(size_t)(k * 48000 + i)] = 0.9f;
                clicks.ch[1][(size_t)(k * 48000 + i)] = 0.9f;
            }
        const long clen = (long)(30.0 * tps);
        perf.new_sequence(1);
        perf.get_sequence(1)->set_name("c");
        view.set_audio_clip(1, &clicks, clen);
        view.set_audio_region(1, 0, clen, 0);
        view.draw(app);
        using Clock = std::chrono::steady_clock;
        auto t0 = Clock::now();
        const long fwd = view.dbg_next_transient(1, false);
        const double fwdMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        t0 = Clock::now();
        const long back = view.dbg_next_transient(lenTicks - 1, true);
        const double backMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("next_transient fwd %.2f ms (hit %ld)   back %.2f ms (hit %ld)\n",
                    fwdMs, fwd, backMs, back);
        view.set_audio_clip(0, nullptr); view.set_audio_clip(1, nullptr);
        perf.delete_sequence(0); perf.delete_sequence(1);
    }
    app.roots.clear();
    return 0;
}

//============================================================================
//  --loopstress : a REAL AUDIO check of transport looping.
//
//  Every previous loop test in this tree asserted on MIDI -- the emit tap, or a
//  VU meter. Both lie about this bug: a stalled pass emits all its events late
//  in a burst with PERFECT ticks, and a peak meter cannot see a dropout shorter
//  than its own decay. Six fixes "passed" while the user still heard silence.
//
//  So this one looks at SAMPLES. A MIDI sequence drives a Csound sine, the
//  transport loops, the real device callback is pumped, and every pass is
//  compared against a reference pass -- for silence AND for glitches (a pass
//  that is loud but WRONG). Nothing here is inferred from engine state.
//============================================================================
static int loopstress(perform& P, int passesWanted, long loopTicks, double bpm)
{
    using namespace PatchKnob::app;

    if (!audio_app_init()) {
        std::printf("SKIP  loopstress: no audio device\n");
        return 0;
    }
    //  STOP the stream rather than muting the output.  set_silent_output()
    //  zeroes exactly the bytes this harness measures ("everything is still
    //  rendered; only the bytes handed to the device are zeroed"), so with it
    //  on every pass reads as silence and the test measures its own mute --
    //  which is how the first run of this harness reported 100 dead passes.
    //  A stopped stream reaches no hardware, and render_into still renders.
    audio_app_set_silent_output(false);
    if (audio_app_engine()) audio_app_engine()->stop();

    const double sr    = audio_app_sample_rate();
    const int    block = audio_app_buffer_size();
    PatchKnob::engine::AudioEngine* eng = audio_app_engine();
    if (!eng) { std::printf("FAIL  loopstress: no engine\n"); return 1; }

    //  ---- a MIDI-driven sine, wired the way the app wires instruments ----
    const int node = audio_app_patch_add_csound();
    if (node < 0) { std::printf("SKIP  loopstress: no Csound in this build\n"); return 0; }
    audio_app_patch_csound_set_text(node,
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "sr = 48000\nksmps = 32\nnchnls = 2\nnchnls_i = 2\n0dbfs = 1\n"
        "massign 0, 1\n"
        "instr 1\n"
        "  icps cpsmidi\n  iamp ampmidi 0.5\n"
        "  kenv madsr 0.001, 0.01, 1.0, 0.01\n"
        "  a1 oscili iamp*kenv, icps\n"
        "  outs a1, a1\n"
        "endin\n"
        "</CsInstruments>\n<CsScore>\nf 0 86400\n</CsScore>\n</CsoundSynthesizer>\n");
    if (!audio_app_patch_csound_recompile(node)) {
        std::printf("FAIL  loopstress: sine instrument did not compile: %s\n",
                    audio_app_patch_csound_error(node));
        return 1;
    }
    //  Exactly what the app's ensure_instr_wired() does: the MODULAR render
    //  path on, the node accepting every MIDI channel, an audio-inlet track,
    //  and connect_instrument to wire MidiIn -> node -> that track's inlet.
    //  (Skipping set_modular was why the first run of this harness was silent:
    //  the graph rendered nothing at all, and csound reported 0.00000 amps.)
    audio_app_set_modular(true);
    audio_app_patch_set_node_channel(node, -1);
    const int track = audio_app_master_add_track(0);
    if (track < 0 || audio_app_master_connect_instrument(track, node) < 0) {
        std::printf("FAIL  loopstress: could not route the instrument\n");
        return 1;
    }

    //  ---- the sequence: notes dense enough that a lost pass is obvious ----
    const int SQ = 191;
    P.new_sequence(SQ); P.set_active(SQ, true);
    sequence* s = P.get_sequence(SQ);
    s->set_name("loopstress"); s->set_midi_bus((char)track); s->set_midi_channel(0);
    s->set_length(loopTicks, false);
    const long step = loopTicks >= 8 ? loopTicks / 8 : 1;
    for (long t = 0; t + 1 < loopTicks; t += step)
        s->add_note_velocity(t, std::max<long>(1, step * 3 / 4),
                             60 + (int)((t / std::max(1L, step)) % 7), 100);
    s->verify_and_link();
    s->clear_triggers();
    s->add_trigger(0, loopTicks, 0, false);
    s->set_playing(true);

    P.set_bpm(bpm);
    P.set_left_tick(0);
    P.set_right_tick(loopTicks);
    P.set_starting_tick(0);
    P.set_looping(true);

    const long long loopEndS = audio_app_tick_to_sample(loopTicks);
    std::printf("      loop = %ld ticks (%lld frames, %.2f ms) @ %.0f BPM, "
                "block %d -> %.2f wraps per block\n",
                loopTicks, loopEndS, 1000.0 * (double)loopEndS / sr, bpm, block,
                (double)block / (double)std::max<long long>(1, loopEndS));

    //  Count what the SEQUENCER emits, so a note-off lost downstream can be
    //  told apart from one that was never sent.  If ons == offs here but the
    //  audio still piles up, the loss is between perform and the instrument.
    static std::atomic<long> s_ons{0}, s_offs{0};
    s_ons.store(0); s_offs.store(0);
    g_seq_emit_tap = [](sequence*, int, int, unsigned char st,
                        unsigned char, unsigned char vel, long, int) {
        const unsigned char hi = st & 0xF0;
        if (hi == 0x90 && vel > 0)                    s_ons.fetch_add(1);
        else if (hi == 0x80 || (hi == 0x90 && vel == 0)) s_offs.fetch_add(1);
    };

    //  ---- run the REAL scheduler thread and pump the REAL callback -------
    //  The REAL scheduler thread, started exactly as the app starts it.
    P.launch_output_thread();
    P.start(true);                              // true == song/playback mode
    audio_app_patch_set_playing(true);
    std::vector<std::vector<float>> devBuf(2);
    std::vector<float*> devPtr(2);
    for (int c = 0; c < 2; ++c) { devBuf[c].assign((size_t)block, 0.f); devPtr[c] = devBuf[c].data(); }

    std::vector<std::vector<float>> pass;        // one captured pass each
    std::vector<float> cur;
    cur.reserve((size_t)loopEndS + (size_t)block);
    long long prevPos = -1;
    const auto t0 = std::chrono::steady_clock::now();

    //  PACE THE PUMP TO REAL TIME.  perform's scheduler runs on a wall-clock
    //  thread; pumping render_into in a tight loop advances the transport far
    //  faster than realtime, so the scheduler cannot keep up and under-emits --
    //  an artefact of the harness that looks exactly like the bug (an earlier
    //  run of this test emitted 68 of 160 notes for precisely that reason).
    //  Sleeping to the block's real duration makes the two clocks agree.
    const auto blockDur = std::chrono::duration<double>((double)block / sr);
    auto nextDue = std::chrono::steady_clock::now();

    while ((int)pass.size() < passesWanted) {
        nextDue += std::chrono::duration_cast<std::chrono::steady_clock::duration>(blockDur);
        std::this_thread::sleep_until(nextDue);
        eng->render_into(nullptr, (void*)devPtr.data(), (unsigned long)block, 0);
        //  SAMPLE-ACCURATE pass slicing.  Reading audio_app_transport_sample()
        //  once per block can only place a wrap to the nearest block, which is
        //  useless once a loop is a few blocks long and meaningless when a loop
        //  is SHORTER than a block (several wraps land inside one callback).
        //  Reconstruct the position per frame by replaying the wrap rule the
        //  engine uses, exactly as the audio-clip harness does.
        //  Seed the reconstructed position ONCE, then advance it continuously.
        //  Re-seeding from audio_app_transport_sample() every block let the two
        //  clocks disagree whenever a wrap landed mid-block, which shifted the
        //  slice boundaries and manufactured "silent" passes that were really
        //  just misaligned windows -- a fault in the ruler, not the audio.
        if (prevPos < 0) prevPos = audio_app_transport_sample() - block;
        for (int i = 0; i < block; ++i) {
            cur.push_back(devBuf[0][(size_t)i]);
            if (++prevPos >= loopEndS) {                  // wrap at the loop end
                prevPos = 0;
                if (!cur.empty()) pass.push_back(cur);
                cur.clear();
                if ((int)pass.size() >= passesWanted) break;
            }
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(180)) {
            std::printf("FAIL  loopstress: timed out after %d passes\n", (int)pass.size());
            break;
        }
    }
    P.stop();
    audio_app_patch_set_playing(false);
    g_seq_emit_tap = 0;
    std::printf("      sequencer emitted: %ld note-ons, %ld note-offs (diff %ld)\n",
                s_ons.load(), s_offs.load(), s_ons.load() - s_offs.load());

    //  ---- analysis: silence AND glitch, against a reference pass ---------
    auto rms = [](const std::vector<float>& v) {
        if (v.empty()) return 0.0;
        double a = 0; for (float x : v) a += (double)x * x;
        return std::sqrt(a / (double)v.size());
    };
    std::vector<double> r(pass.size());
    for (size_t i = 0; i < pass.size(); ++i) r[i] = rms(pass[i]);
    std::vector<double> sorted = r;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];

    //  The reference is the median-loudest pass -- robust to a few bad ones.
    size_t refIdx = 0; double bestD = 1e9;
    for (size_t i = 0; i < r.size(); ++i) {
        const double d = std::fabs(r[i] - med);
        if (d < bestD) { bestD = d; refIdx = i; }
    }

    int silent = 0, quiet = 0, glitched = 0;
    std::string bad;
    for (size_t i = 0; i < pass.size(); ++i) {
        const bool isSilent = r[i] < med * 0.05;
        const bool isQuiet  = !isSilent && r[i] < med * 0.60;
        //  GLITCH: right level, wrong SHAPE.  Compared as a windowed-RMS
        //  ENVELOPE, not raw samples: pass boundaries are detected at block
        //  granularity, so two byte-identical passes can sit up to a block
        //  apart and raw-sample correlation reads ~0 for audio that is
        //  actually fine.  The envelope is insensitive to that shift while
        //  still catching a pass that drops out halfway or plays the wrong
        //  notes.
        double corr = 1.0;
        if (!isSilent) {
            auto envelope = [](const std::vector<float>& v, size_t win) {
                std::vector<double> e;
                for (size_t k = 0; k + win <= v.size(); k += win) {
                    double a = 0;
                    for (size_t j = 0; j < win; ++j) a += (double)v[k+j]*v[k+j];
                    e.push_back(std::sqrt(a/(double)win));
                }
                return e;
            };
            //  Note onsets land on BLOCK boundaries, so the same pass can sit a
            //  block earlier or later than the reference -- inaudible, but it
            //  slides every attack across the envelope windows and craters a
            //  fixed-alignment correlation.  Take the best correlation over a
            //  +/- one-block search, which tolerates that jitter while still
            //  failing a pass whose CONTENT differs.
            const size_t win = 128;
            const std::vector<double> eb = envelope(pass[refIdx], win);
            const int    slack = (int)(block / win) + 2;
            double best = -2.0;
            for (int sh = -slack; sh <= slack; ++sh) {
                const std::vector<double> ea = envelope(pass[i], win);
                const size_t n = std::min(ea.size(), eb.size());
                if (n <= (size_t)(2 * slack + 4)) break;
                const size_t lo = (size_t)slack, hi = n - (size_t)slack;
                double sa=0, sb=0; size_t cnt = 0;
                for (size_t k = lo; k < hi; ++k) { sa += ea[k]; sb += eb[(size_t)((long)k + sh)]; ++cnt; }
                const double ma = sa/(double)cnt, mb = sb/(double)cnt;
                double sab=0, saa=0, sbb=0;
                for (size_t k = lo; k < hi; ++k) {
                    const double da = ea[k]-ma, db = eb[(size_t)((long)k + sh)]-mb;
                    sab += da*db; saa += da*da; sbb += db*db;
                }
                const double c = (saa>0 && sbb>0) ? sab/std::sqrt(saa*sbb) : 0.0;
                if (c > best) best = c;
            }
            corr = best;
        }
        const bool isGlitch = !isSilent && corr < 0.90;
        if (isSilent) ++silent; else if (isQuiet) ++quiet;
        if (isGlitch) ++glitched;
        if (isSilent || isQuiet || isGlitch) {
            char b2[96];
            std::snprintf(b2, sizeof b2, " %zu(%s rms=%.4f r=%.3f)", i,
                          isSilent ? "SILENT" : isGlitch ? "GLITCH" : "quiet",
                          r[i], corr);
            if (bad.size() < 900) bad += b2;
        }
    }
    std::printf("      %d passes captured, median rms %.4f, reference pass %zu\n",
                (int)pass.size(), med, refIdx);
    if (!bad.empty()) std::printf("      bad passes:%s\n", bad.c_str());

    CHECK((int)pass.size() >= passesWanted, "loopstress: captured every requested pass");
    CHECK(med > 0.001, "loopstress: the instrument actually sounds");
    CHECK(silent == 0, "loopstress: NO pass is silent");
    CHECK(quiet == 0, "loopstress: no pass is partially dropped out");
    CHECK(glitched == 0, "loopstress: no pass differs from the reference waveform");

    //  HUNG NOTES.  A loop that repeats identical material must not grow
    //  louder: if it does, note-offs are not reaching the instrument and voices
    //  are piling up pass after pass -- which ends in a voice ceiling and then
    //  silence.  Compare the first quarter of the run against the last.
    if (pass.size() >= 8) {
        const size_t q = pass.size() / 4;
        double early = 0, late = 0;
        for (size_t i = 0; i < q; ++i)                    early += r[i];
        for (size_t i = pass.size() - q; i < pass.size(); ++i) late  += r[i];
        early /= (double)q; late /= (double)q;
        std::printf("      first-quarter rms %.4f vs last-quarter %.4f (ratio %.2f)\n",
                    early, late, early > 0 ? late / early : 0.0);
        CHECK(early > 0 && late < early * 1.25,
              "loopstress: the loop does not get LOUDER as it repeats (no hung notes)");
    }
    return g_fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    bool run_selftest = false;
    bool run_bench    = false;
    bool run_stress   = false;
    // Midnight (phosphor green) shows the DAW look best; pass "light" to flip.
    set_mode(Mode::Midnight);
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "light") set_mode(Mode::Light);
        if (std::string(argv[i]) == "--selftest") run_selftest = true;
        if (std::string(argv[i]) == "--bench")    run_bench = true;
        if (std::string(argv[i]) == "--loopstress") run_stress = true;
    }

    App app;
    app.w = 1280; app.h = 720;
    if (!app.init("PatchKnob -- Arrangement (song) view")) { app.shutdown(); return 1; }

    if (run_bench) {
        const int rc = bench(app);
        app.shutdown();
        return rc;
    }

    perform perf;    if (run_stress) {
        //  Defaults chosen to be HARSH: a 128th-note loop at 480 BPM puts
        //  several wraps inside a single audio block, which is the regime the
        //  earlier bar-length tests never reached.
        const long  lt = (long)(getenv("PK_STRESS_TICKS") ? atol(getenv("PK_STRESS_TICKS"))
                                                          : (long)c_ppqn / 32);
        const int   np = getenv("PK_STRESS_PASSES") ? atoi(getenv("PK_STRESS_PASSES")) : 100;
        const double bp = getenv("PK_STRESS_BPM") ? atof(getenv("PK_STRESS_BPM")) : 480.0;
        std::printf("\n[loopstress] %d passes, %ld-tick loop, %.0f BPM\n", np, lt, bp);
        const int rc = loopstress(perf, np, lt, bp);
        std::printf(g_fails ? "LOOPSTRESS: %d FAILED\n" : "LOOPSTRESS: all passed\n", g_fails);
        return rc;
    }

    build_song(perf);

    arrange::ArrangeView view(&perf);
    app.roots = { &view };
    app.on_layout = [&](App& a) {
        view.rect = { 0, 0, a.w, a.h };
    };

    if (run_selftest) {
        const int rc = selftest(app, perf, view);
        app.shutdown();
        return rc;
    }

    // animate the playhead on a background thread (main thread renders only)
    std::atomic<bool> alive{ true };
    std::thread ticker([&]{
        long t = 0;
        while (alive.load()) {
            t += c_ppqn / 4;                 // ~a 16th note per step
            if (t > 20 * BAR) t = 0;         // loop over 20 bars
            perf.set_tick(t);               // the playhead_tick override is gone
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
