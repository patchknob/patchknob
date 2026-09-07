//----------------------------------------------------------------------------
//  sdlui/project_io.cpp  --  implementation of save_project / load_project.
//  See project_io.h for the container format.
//----------------------------------------------------------------------------
#include "project_io.h"

#include "perform.h"
#include "sequence.h"
#include "event.h"
#include "globals.h"

#include "gui.h"          // ui::mode() / ui::set_mode()  (theme mode)

// --- optional: live audio graph (mixer + per-track VST instruments) ---------
#include "audio_app.h"
#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"
#include "engine/plugin_api.h"
#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/audio_clip_player.h"
#include "engine/patch/patch_graph.h"
#include "engine/patch/patch_nodes.h"
#include "engine/patch/pd_node.h"
#include "engine/patch/csound_node.h"
#include "engine/rack/rack_node.h"
#include "engine/rack/rack_engine.h"
#include "engine/automation/automation_player.h"
#include "engine/automation/automation_track.h"
#include "engine/automation/automation_lane.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Durable saves need a real flush-to-DISK barrier, which the iostreams have no
// portable spelling for.  MinGW and MSVC both expose the CRT's _commit()
// (FlushFileBuffers under the hood); POSIX has fsync().
#ifdef _WIN32
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace {

namespace patch = PatchKnob::engine::patch;

std::string g_last_error;
void set_err(const std::string& s) { g_last_error = s; }

const char  MAGIC[8] = { 'S','2','4','D','A','W','P','J' };
const uint32_t PROJ_VERSION = 20;  // v20: automation region loopStart (the
                                   //      window's START, which v19 dropped:
                                   //      a clip whose loop window began later
                                   //      than tick 0 wrapped at the right place
                                   //      but replayed from the top after a
                                   //      reload, so automation and notes drifted
                                   //      apart on every repetition.
                                   // v19: per-region fade shape/slope + the
                                   //      crossfade link (ch.32, "AUDI")
                                   // v18: aux buses + the per-track send matrix
                                   //      + real master-strip SOLO ("PTCH")
                                   // v17: per-region audio loop PERIOD (trimmed loops)
                                   // v16: per-sequence loop ENABLE (one-shot clips)

// MIGRATION, v17 -> v18.  Purely ADDITIVE, and deliberately so: the aux ports
// are keyed by a stable SLOT out of a free list (see patch_nodes.h), so they
// occupy port ids no pre-v18 project could ever have written and NO EXISTING
// PORT ID CHANGES MEANING.  A v17 file therefore restores byte-for-byte as it
// always did -- it simply has no aux block, which reads back as "no buses, no
// sends, nothing soloed", exactly the state it was saved in.

// Ticks in a project written before v13 are in the OLD 192-PPQN unit; c_ppqn is
// now 768, so every stored musical position has to be scaled on load or the
// whole project plays at the wrong rate.  Derived, not a literal 4, so a future
// PPQN change only needs kLegacyPpqn updated.
const long kLegacyPpqn = 192;
inline long migrate_tick(long t, uint32_t ver) {
    return (ver >= 13) ? t : t * (long)(c_ppqn / kLegacyPpqn);
}
// Put back a saved per-clip loop window EXACTLY as it was, including the legal
// state where it reaches past the END marker.
//
// sequence::set_length() deliberately hides rather than destroys a window that
// overhangs m_length -- that is how a 3-bar loop is laid over a 2-bar phrase --
// but sequence::set_loop_end() clamps its argument to m_length, so the plain
// "set_length, then set_loop_end, then set_loop_start" restore silently pulled
// every such window back to the END marker.  Since undo/redo restores whole
// project files (see main.cpp's project-wide history ring), that truncation hit
// on any undo of any unrelated edit, not only on File > Open.
//
// The fix is ordering, not clamping: grow the pattern far enough that the saved
// end is in range, place the window, then set the real length back.  A window
// pinned at a non-zero start is not the "spans the whole pattern" default, so
// set_length() leaves it alone on the way down -- hence the temporary non-zero
// start for windows that legitimately begin at tick 0.
//
// (The clamp in set_loop_end() is the real defect and belongs in
// src/sequence.cpp; this keeps user data intact without reaching into a file
// this module does not own.)
void restore_loop_window(sequence* s, long loopStart, long loopEnd) {
    const long realLen = s->get_length();
    if (loopEnd < 0) loopEnd = realLen;
    if (loopStart < 0) loopStart = 0;
    if (loopStart > loopEnd) loopStart = loopEnd;

    // The setters no longer clamp against m_length (a window may legitimately
    // overhang the END marker -- that is how a 3-beat loop sits over a 2-beat
    // phrase), so the old grow/place/shrink dance to smuggle one past the clamp
    // is gone.  They DO still enforce ordering against whatever window this
    // sequence currently carries, and on load that is the default the preceding
    // set_length() left behind.  Park the start at the floor first: then no
    // intermediate state can be inverted, whatever was there before.
    s->set_loop_start(0);
    s->set_loop_end(loopEnd);
    s->set_loop_start(loopStart);
}

std::vector<ProjectPatchNodePosition> g_loadedPatchLayout;
std::vector<ProjectFreezeRecord>      g_loadedFreezes;
std::map<uint32_t,uint32_t> g_loadedPatchNodeIds;

// ---------------------------------------------------------------------------
//  little-endian writers (append into a byte buffer)
// ---------------------------------------------------------------------------
typedef std::vector<uint8_t> Buf;

void put_u8 (Buf& b, uint8_t  v) { b.push_back(v); }
void put_u32(Buf& b, uint32_t v) {
    b.push_back((uint8_t)(v      & 0xFF));
    b.push_back((uint8_t)((v>>8 )& 0xFF));
    b.push_back((uint8_t)((v>>16)& 0xFF));
    b.push_back((uint8_t)((v>>24)& 0xFF));
}
void put_i32(Buf& b, int32_t v) { put_u32(b, (uint32_t)v); }
void put_u64(Buf& b, uint64_t v) {
    put_u32(b, (uint32_t)(v & 0xffffffffu));
    put_u32(b, (uint32_t)(v >> 32));
}
void put_i64(Buf& b, int64_t v) { put_u64(b, (uint64_t)v); }
void put_f32(Buf& b, float v) {
    uint32_t u; std::memcpy(&u, &v, 4); put_u32(b, u);
}
void put_f64(Buf& b, double v) {
    uint64_t u; std::memcpy(&u, &v, 8); put_u64(b, u);
}
void put_str(Buf& b, const std::string& s) {
    put_u32(b, (uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}
void put_blob(Buf& b, const std::vector<uint8_t>& d) {
    put_u32(b, (uint32_t)d.size());
    b.insert(b.end(), d.begin(), d.end());
}

// A section = 4-char tag + u32 length + payload.
void put_section(Buf& out, const char tag[4], const Buf& payload) {
    out.insert(out.end(), tag, tag + 4);
    put_u32(out, (uint32_t)payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

// ---------------------------------------------------------------------------
//  little-endian readers (cursor over a byte buffer; bounds-checked)
// ---------------------------------------------------------------------------
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool   ok  = true;

    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}

    bool need(size_t k) { if (pos + k > n) { ok = false; return false; } return true; }

    uint8_t u8() { if (!need(1)) return 0; return p[pos++]; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos+1]<<8) |
                     ((uint32_t)p[pos+2]<<16) | ((uint32_t)p[pos+3]<<24);
        pos += 4; return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    uint64_t u64() { uint64_t lo = u32(); uint64_t hi = u32(); return lo | (hi << 32); }
    int64_t i64() { return (int64_t)u64(); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    double f64() { uint64_t u = u64(); double f; std::memcpy(&f, &u, 8); return f; }
    std::string str() {
        uint32_t len = u32();
        if (!need(len)) return std::string();
        std::string s((const char*)(p + pos), len);
        pos += len; return s;
    }
    std::vector<uint8_t> blob() {
        uint32_t len = u32();
        if (!need(len)) return {};
        std::vector<uint8_t> d(p + pos, p + pos + len);
        pos += len; return d;
    }
};

// ---------------------------------------------------------------------------
//  event capture: walk EVERY event in a sequence with full fidelity.
//
//  PatchKnob has no single "give me all events" accessor, so we use the same two
//  pass idiom as the legacy editors:
//    pass 1  reset_draw_marker() + get_next_event(&status,&cc) to collect the
//            set of distinct event KINDS present.  For non-CC events a "kind" is
//            just the status (cc collapsed to 0) because get_next_event(status,
//            cc, ...) ignores cc for those; for CC events the kind is
//            (status, controller) so each controller is captured separately.
//    pass 2  for each kind, reset + get_next_event(status,cc,...) to pull every
//            matching event's (tick,status,d0,d1).
// ---------------------------------------------------------------------------
struct RawEv { long tick; uint8_t status, d0, d1; int column; };
struct RawTrigger { long start, length, offset; };

void collect_events(sequence* s, std::vector<RawEv>& out) {
    // snapshot_events() reports the tracker COLUMN as well.  The old two-pass
    // get_next_event() walk could not, so every save silently dropped the
    // per-note voice assignment and the pattern came back with its columns
    // collapsed.
    std::vector<sequence::EventSnapshot> snap;
    s->snapshot_events(snap);
    out.clear();
    out.reserve(snap.size());
    for (size_t i = 0; i < snap.size(); ++i) {
        RawEv e;
        e.tick = snap[i].tick; e.status = snap[i].status;
        e.d0 = snap[i].d0;     e.d1 = snap[i].d1;
        e.column = snap[i].column;
        out.push_back(e);
    }
}

void collect_triggers(sequence* s, std::vector<RawTrigger>& out) {
    s->reset_draw_trigger_marker();
    long start, end, offset; bool selected;
    while (s->get_next_trigger(&start, &end, &selected, &offset)) {
        if (end >= start) out.push_back(RawTrigger{start, end - start + 1, offset});
    }
}

struct StoredPlugin {
    bool present = false;
    PatchKnob::engine::PluginDescriptor desc;
    std::vector<uint8_t> state;
};

bool stored_plugin_is_sampler(const StoredPlugin& plugin) {
    return plugin.present && plugin.desc.name == "Sampler" && plugin.desc.path.empty();
}

void put_plugin(Buf& b, PatchKnob::engine::IPluginInstance* inst) {
    put_u8(b, inst ? 1 : 0);
    if (!inst) return;
    const PatchKnob::engine::PluginDescriptor& d = inst->descriptor();
    put_u8(b, d.format == PatchKnob::engine::PluginFormat::VST3 ? 1 : 0);
    put_str(b, d.path);
    put_str(b, d.uid);
    put_str(b, d.name);
    put_u8(b, d.isInstrument ? 1 : 0);
    put_i32(b, d.numAudioIn);
    put_i32(b, d.numAudioOut);
    put_blob(b, inst->saveState());
}

StoredPlugin get_plugin(Reader& r) {
    StoredPlugin s;
    s.present = r.u8() != 0;
    if (!s.present) return s;
    s.desc.format = r.u8() ? PatchKnob::engine::PluginFormat::VST3
                            : PatchKnob::engine::PluginFormat::VST2;
    s.desc.path = r.str();
    s.desc.uid = r.str();
    s.desc.name = r.str();
    s.desc.isInstrument = r.u8() != 0;
    s.desc.numAudioIn = r.i32();
    s.desc.numAudioOut = r.i32();
    s.state = r.blob();
    return s;
}

enum PatchNodeKind {
    PATCH_PLUGIN = 1, PATCH_MIXER, PATCH_PD, PATCH_RACK, PATCH_SINE,
    PATCH_GAIN, PATCH_SUM, PATCH_AUDIO_OUT, PATCH_AUDIO_IN, PATCH_MIDI_IN,
    PATCH_MIDI_OUT, PATCH_RECORD, PATCH_CSOUND, PATCH_MIDI_TRACK
};

int patch_node_kind(PatchKnob::engine::patch::Node* n) {
    using namespace PatchKnob::engine::patch;
    if (dynamic_cast<PluginNode*>(n))          return PATCH_PLUGIN;
    if (dynamic_cast<MixerNode*>(n))           return PATCH_MIXER;
    if (dynamic_cast<PdNode*>(n))              return PATCH_PD;
    if (dynamic_cast<CsoundNode*>(n))          return PATCH_CSOUND;
    if (dynamic_cast<RackNode*>(n))            return PATCH_RACK;
    if (dynamic_cast<SineSourceNode*>(n))      return PATCH_SINE;
    if (dynamic_cast<GainNode*>(n))            return PATCH_GAIN;
    if (dynamic_cast<SumNode*>(n))             return PATCH_SUM;
    if (dynamic_cast<AudioDeviceOutNode*>(n))  return PATCH_AUDIO_OUT;
    if (dynamic_cast<AudioDeviceInNode*>(n))   return PATCH_AUDIO_IN;
    if (dynamic_cast<MidiInNode*>(n))          return PATCH_MIDI_IN;
    if (dynamic_cast<MidiOutNode*>(n))         return PATCH_MIDI_OUT;
    if (dynamic_cast<MidiTrackNode*>(n))       return PATCH_MIDI_TRACK;
    return 0;
}

// ---------------------------------------------------------------------------
//  Durable, atomic file replacement (used by BOTH save_project and
//  save_rack_patch).  A DAW's project file is the user's work, so a save that
//  half-succeeds must never be able to eat the previous one.  Three things all
//  have to hold:
//
//    * the bytes must reach the DISK before the new file takes the old one's
//      name.  ostream::flush() only hands them to the kernel, so a crash or
//      power loss right after the status bar said "Saved" could leave a
//      zero-length project where a good one used to be.  fsync() / _commit()
//      is the barrier.
//    * the swap must be a RENAME over the live file, never remove-then-rename.
//      rename() replaces the destination, so the remove bought nothing except a
//      window in which the project did not exist at all.
//    * the previous contents are kept in a .bak that is only ever restored if
//      THIS save is what removed them.
// ---------------------------------------------------------------------------
bool write_all_sync(const std::string& path, const uint8_t* data, size_t len) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    if (len && std::fwrite(data, 1, len, f) != len) { std::fclose(f); return false; }
    if (std::fflush(f) != 0) { std::fclose(f); return false; }
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0) { std::fclose(f); return false; }
#else
    if (::fsync(::fileno(f)) != 0) { std::fclose(f); return false; }
#endif
    return std::fclose(f) == 0;
}

// Make the RENAME itself durable too.  Only POSIX needs (and offers) this; the
// MoveFileEx behind the Windows rename is already ordered against the metadata.
void sync_parent_dir(const std::string& path) {
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty()) dir = std::filesystem::path(".");
    const int fd = ::open(dir.string().c_str(), O_RDONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
    (void)ec;
#else
    (void)path;
#endif
}

bool install_file_atomically(const std::string& path, const std::vector<uint8_t>& bytes,
                             const char* what) {
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";

    if (!write_all_sync(tmp, bytes.data(), bytes.size())) {
        std::remove(tmp.c_str());
        set_err(std::string("write failed for '") + tmp + "'");
        return false;
    }

    std::error_code ec;
    const bool hadOriginal = std::filesystem::exists(path, ec) && !ec;
    bool madeBackup = false;
    if (hadOriginal) {
        ec.clear();
        std::filesystem::copy_file(path, bak,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::remove(tmp.c_str());
            set_err(std::string("cannot create ") + what + " backup '" + bak + "'");
            return false;
        }
        madeBackup = true;
    }

    // rename() replaces an existing destination on POSIX and in MSVC's
    // <filesystem>; libstdc++ on MinGW forwards to the CRT rename(), which does
    // NOT.  Try the atomic form first and only fall back to remove-then-rename
    // when the platform actually refuses -- that ordering is the one that can
    // lose the file, so it must never be the default path.
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (ec && hadOriginal) {
        std::error_code rmec;
        std::filesystem::remove(path, rmec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        // Restore ONLY if this save is what removed the original.  The old code
        // tested exists(bak) -- a stale .bak left by an earlier save would be
        // copied over a path that never existed in this run.
        std::error_code rec;
        if (madeBackup && !std::filesystem::exists(path, rec))
            std::filesystem::copy_file(bak, path,
                std::filesystem::copy_options::overwrite_existing, rec);
        std::remove(tmp.c_str());
        set_err(std::string("cannot install saved ") + what + " '" + path + "'");
        return false;
    }
    sync_parent_dir(path);
    return true;
}

std::vector<uint8_t> read_file_blob(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
//  The sections only a LIVE audio engine can regenerate.
//
//  TRAK, PTCH and AUDI carry the per-track instruments and inserts, the modular
//  patch, the rack, every plugin's own state blob and the embedded audio clips
//  -- and all three are serialized out of the running engine.  main.cpp does
//  NOT exit when audio_app_init() fails (busy device, exclusive-mode ASIO), so
//  the GUI runs perfectly well with no engine at all; in that state save_project
//  simply omitted all three, returned true, and let the status bar say "Saved".
//  Opening an existing project and saving it silently emptied it -- and since
//  undo/redo round-trips whole project files (main.cpp snapshot_state /
//  restore_state), one edit in that state destroyed the patch irrecoverably.
//
//  Policy: a save is never allowed to be lossy.  What cannot be regenerated is
//  carried through byte for byte from the copy already on disk.  Those bytes
//  are only meaningful under the container version they were written with (the
//  PTCH/rack layout is gated on it), so if the file on disk is an OLDER format
//  the save is REFUSED with an explanation rather than silently dropped.
// ---------------------------------------------------------------------------
struct CarriedSections {
    std::vector<uint8_t> trak, ptch, audi;   // whole sections: tag + len + payload
    bool sameVersion = false;                // ...written by THIS format version
};

CarriedSections read_carried_sections(const std::string& path) {
    CarriedSections out;
    const std::vector<uint8_t> data = read_file_blob(path);
    if (data.size() < 12 || std::memcmp(data.data(), MAGIC, 8) != 0) return out;

    Reader r(data.data(), data.size());
    for (int i = 0; i < 8; ++i) r.u8();
    out.sameVersion = (r.u32() == PROJ_VERSION);

    while (r.ok && r.pos + 8 <= r.n) {
        const size_t at = r.pos;
        char tag[4];
        for (int i = 0; i < 4; ++i) tag[i] = (char)r.u8();
        const uint32_t len = r.u32();
        if (!r.ok || r.pos + len > r.n) break;
        if (std::memcmp(tag, "END ", 4) == 0) break;
        std::vector<uint8_t>* dst = nullptr;
        if      (std::memcmp(tag, "TRAK", 4) == 0) dst = &out.trak;
        else if (std::memcmp(tag, "PTCH", 4) == 0) dst = &out.ptch;
        else if (std::memcmp(tag, "AUDI", 4) == 0) dst = &out.audi;
        if (dst) dst->insert(dst->end(), data.begin() + (long)at,
                                         data.begin() + (long)(r.pos + len));
        r.pos += len;
    }
    return out;
}

void put_rack(Buf& b, rackx::RackEngine* rack) {
    put_i32(b, rack ? rack->polyphony() : 1);
    const int modules = rack ? rack->moduleCount() : 0;
    put_u32(b, (uint32_t)modules);
    for (int i = 0; i < modules; ++i) {
        rackx::RackModule* m = rack->moduleAt(i);
        put_i32(b, m ? m->id : -1);
        put_str(b, m ? m->slug : std::string());
        put_f32(b, m ? m->x : 0.0f);
        put_f32(b, m ? m->y : 0.0f);
        // v6: scripting module (Pd/Csound) patch text + panel layout, BEFORE params
        // (so the loader rebuilds the module's ports before restoring param values).
        const bool script = m && rack->isScriptModule(m->id);
        put_u8(b, script ? 1 : 0);
        if (script) {
            put_str(b, rack->moduleScript(m->id));
            put_f32(b, m->panel.width);
            auto putEls = [&](const std::vector<rackx::PanelElement>& v) {
                put_u32(b, (uint32_t)v.size());
                for (const auto& e : v) { put_f32(b, e.x); put_f32(b, e.y); }
            };
            putEls(m->panel.params); putEls(m->panel.inputs); putEls(m->panel.outputs);
        }
        const int params = m && m->mod ? (int)m->mod->params.size() : 0;
        put_u32(b, (uint32_t)params);
        for (int p = 0; p < params; ++p) put_f32(b, m->mod->params[(size_t)p].value);
        // v7: the sample a slot-backed module (SMPL-1) has loaded.  Only param
        // VALUES were ever written, and a sample is not a param, so every rack
        // sampler came back empty on reload.
        //
        // The AUDIO is embedded, not just a path: the project stays playable if
        // the source file moves, and an edited/reversed/trimmed sample restores
        // as it was rather than being re-decoded from a file that no longer
        // matches.  Stored as float32 planar at the slot's own rate -- lossless
        // and already the in-memory format, so this costs ~8 bytes/frame.
        const rackx::ISampleSlot* slot =
            (m && m->mod) ? dynamic_cast<const rackx::ISampleSlot*>(m->mod.get()) : nullptr;
        std::vector<float> sl, sr;
        double srate = 0.0;
        const bool haveAudio = slot && slot->sampleReadAudio(sl, sr, srate);
        put_u8(b, haveAudio ? 1 : 0);
        if (haveAudio) {
            put_str(b, slot->sampleName());
            put_str(b, slot->samplePath());          // metadata / relink hint
            put_f64(b, srate);
            // ISampleSlot does NOT promise the two channel buffers are the same
            // length (sample_slot.h), and restore_rack reads `frames` floats for
            // BOTH.  Declaring sl.size() and then writing sr.size() floats made
            // the reader consume the wrong bytes from that point on -- i.e. the
            // whole rest of the FILE, not just this module.  Write exactly
            // `frames` per channel, mirroring L into a short/absent R the same
            // way sampleSetAudio() treats a mono buffer.
            const size_t frames = sl.size();
            put_u64(b, (uint64_t)frames);
            for (size_t k = 0; k < frames; ++k) put_f32(b, sl[k]);
            for (size_t k = 0; k < frames; ++k)
                put_f32(b, k < sr.size() ? sr[k] : sl[k]);
        }
    }
    const int cables = rack ? rack->cableCount() : 0;
    put_u32(b, (uint32_t)cables);
    for (int i = 0; i < cables; ++i) {
        rackx::RackCable* c = rack->cableAt(i);
        put_i32(b, c ? c->fromMod : -1); put_i32(b, c ? c->outPort : -1);
        put_i32(b, c ? c->toMod   : -1); put_i32(b, c ? c->inPort  : -1);
    }
}

void put_patch(Buf& out) {
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    using namespace PatchKnob::engine::patch;
    PatchGraph* graph = audio_app_running() ? audio_app_patch_graph() : nullptr;
    if (!graph) return;

    put_u8(out, audio_app_modular() ? 1 : 0);
    put_u8(out, audio_app_multithreaded() ? 1 : 0);
    put_u32(out, (uint32_t)audio_app_patch_out_node());
    put_u32(out, (uint32_t)audio_app_patch_midi_in_node());
    put_u32(out, (uint32_t)audio_app_master_mixer_node());
    put_u32(out, (uint32_t)audio_app_default_hw_midi_in_node());
    put_u32(out, (uint32_t)audio_app_virtual_midi_node());
    const int virtualOutputs=audio_app_virtual_midi_outputs();
    put_u32(out,(uint32_t)std::max(0,virtualOutputs));
    for(int o=0;o<virtualOutputs;++o)
        put_i32(out,audio_app_virtual_midi_route(o));
    MasterMixerNode* master = dynamic_cast<MasterMixerNode*>(
        graph->node((NodeId)audio_app_master_mixer_node()));
    const int tracks = master ? master->trackCount() : 0;
    put_u32(out, (uint32_t)tracks);
    put_f32(out, master ? master->masterGain() : 1.0f);
    for (int i = 0; i < tracks; ++i) {
        put_u8(out, master->trackIsMidi(i) ? 1 : 0);
        put_f32(out, master->gain(i)); put_f32(out, master->pan(i));
        put_u8(out, master->mute(i) ? 1 : 0);
    }

    // ---- v18: aux buses, their insert chains, the send matrix and SOLO -----
    // Written in bus INDEX order.  The bus's node id is saved like any other
    // node id so the connection table below (which stores the send/return
    // cables by port id) remaps onto the recreated bus; the SLOT is not saved,
    // because it is re-derived on load in the same order and the port ids are
    // rebuilt from it.
    const int auxes = master ? master->auxCount() : 0;
    put_u32(out, (uint32_t)auxes);
    for (int a = 0; a < auxes; ++a) {
        const int strip = audio_app_master_aux_strip(a);
        put_u32(out, (uint32_t)audio_app_master_aux_node(a));
        char nm[128]; audio_app_master_aux_name(a, nm, (int)sizeof nm);
        put_str(out, std::string(nm));
        put_f32(out, audio_app_master_aux_return_gain(a));
        put_u8 (out, audio_app_master_aux_mute(a) ? 1 : 0);
        const int inserts = audio_app_master_insert_count(strip);
        put_u32(out, (uint32_t)inserts);
        for (int k = 0; k < inserts; ++k) {
            int node = -1, active = 1, pre = 1;
            audio_app_master_insert_info(strip, k, &node, nullptr, 0, &active, &pre);
            put_u32(out, (uint32_t)node);
            put_u8 (out, (uint8_t)(active ? 1 : 0));
            put_u8 (out, (uint8_t)(pre ? 1 : 0));
        }
    }
    for (int i = 0; i < tracks; ++i) put_u8(out, master->solo(i) ? 1 : 0);
    for (int i = 0; i < tracks; ++i)
        for (int a = 0; a < auxes; ++a) {
            put_f32(out, master->sendLevel(i, a));
            put_u8 (out, master->sendPreFader(i, a) ? 1 : 0);
            put_u8 (out, master->sendEnabled(i, a) ? 1 : 0);
        }

    std::vector<NodeId> ids = graph->nodeIds();
    std::vector<NodeId> saved;
    for (size_t i = 0; i < ids.size(); ++i)
        if ((int)ids[i] != audio_app_patch_out_node() &&
            (int)ids[i] != audio_app_patch_midi_in_node() &&
            (int)ids[i] != audio_app_default_hw_midi_in_node() &&
            (int)ids[i] != audio_app_virtual_midi_node() &&
            (int)ids[i] != audio_app_master_mixer_node() &&
            patch_node_kind(graph->node(ids[i])) != 0) saved.push_back(ids[i]);
    put_u32(out, (uint32_t)saved.size());

    for (size_t i = 0; i < saved.size(); ++i) {
        Node* n = graph->node(saved[i]);
        const int kind = patch_node_kind(n);
        put_u32(out, saved[i]);
        put_u8(out, (uint8_t)kind);
        put_u8(out, n->bypass() ? 1 : 0);
        put_i32(out, n->midiChannel());
        if (kind == PATCH_PLUGIN) {
            put_plugin(out, static_cast<PluginNode*>(n)->instance());
        } else if (kind == PATCH_MIXER) {
            MixerNode* m = static_cast<MixerNode*>(n);
            const int channels = m->channels(); put_i32(out, channels); put_f32(out, m->masterGain());
            for (int c = 0; c < channels; ++c) {
                put_f32(out, m->gain(c)); put_f32(out, m->pan(c)); put_u8(out, m->mute(c) ? 1 : 0);
            }
        } else if (kind == PATCH_PD) {
            // Patch lives in memory on the node; store the TEXT with the project (no
            // on-disk .pd).  Same wire layout (str + blob) as before -> old projects,
            // whose blob was the .pd file content, still load as patch text.
            PdNode* pd = static_cast<PdNode*>(n);
            put_str(out, std::string());                 // legacy path field (unused now)
            const std::string& t = pd->patchText();
            put_blob(out, std::vector<uint8_t>(t.begin(), t.end()));
        } else if (kind == PATCH_CSOUND) {
            put_str(out, static_cast<CsoundNode*>(n)->csdText());   // the .csd source
        } else if (kind == PATCH_RACK) {
            put_rack(out, static_cast<RackNode*>(n)->engine());
        } else if (kind == PATCH_SINE) {
            SineSourceNode* s = static_cast<SineSourceNode*>(n);
            put_f32(out, s->freq()); put_f32(out, s->amp());
        } else if (kind == PATCH_GAIN) {
            put_f32(out, static_cast<GainNode*>(n)->gain());
        } else if (kind == PATCH_MIDI_TRACK) {
            MidiTrackNode* t=static_cast<MidiTrackNode*>(n);
            put_i32(out,t->input()); put_i32(out,t->output());
        }
    }

    std::vector<Connection> conns = graph->connections();
    put_u32(out, (uint32_t)conns.size());
    for (size_t i = 0; i < conns.size(); ++i) {
        put_u32(out, conns[i].from.node); put_u32(out, conns[i].from.port);
        put_u32(out, conns[i].to.node);   put_u32(out, conns[i].to.port);
    }
}

void put_audio(Buf& out) {
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;
    if (!graph) return;

    struct AudioEntry { int track; ScheduledClip scheduled; };
    std::vector<AudioEntry> entries;
    for (int t = 0; t < graph->trackCount(); ++t) {
        Track* trk = graph->track(t);
        AudioClipPlayer* player = trk ? dynamic_cast<AudioClipPlayer*>(trk->instrument()) : nullptr;
        if (!player) continue;
        for (int i = 0; i < player->clipCount(); ++i) {
            ScheduledClip sc = player->clipAt(i);
            // safeFrames(), not empty(): a half-resized clip (ch[0] filled,
            // ch[1] not yet) is "not empty" but has nothing readable on both
            // channels -- see the ragged-clip note in audio_clip.h.
            if (sc.clip && sc.clip->safeFrames() > 0) entries.push_back(AudioEntry{t, sc});
        }
    }
    // v4 format: a DEDUPED source table (each unique AudioClip written ONCE)
    // followed by region ENTRIES that reference a source by index and carry the
    // full non-destructive placement.  Split clips share one source (was: the
    // whole buffer stored per entry, so both halves reloaded full-length and
    // overlapped -- and the file was 2x the size).
    std::vector<const AudioClip*> sources;             // unique clips, in order
    std::map<const AudioClip*, uint32_t> srcIndex;
    for (const AudioEntry& e : entries) {
        const AudioClip* c = e.scheduled.clip;
        if (srcIndex.find(c) == srcIndex.end()) {
            srcIndex[c] = (uint32_t)sources.size();
            sources.push_back(c);
        }
    }
    put_u32(out, (uint32_t)sources.size());
    for (const AudioClip* c : sources) {
        put_str(out, c->name); put_f64(out, c->sampleRate); put_f64(out, c->sourceSampleRate);
        // numFrames() is ch[0].size() ONLY.  audio_clip.h documents that the
        // equal-length invariant can be broken while the message thread edits a
        // clip that is already scheduled, so declaring numFrames() and then
        // indexing ch[1] over that range was an out-of-bounds READ during save.
        // safeFrames() is the count both channels can actually supply.
        const int64_t frames = c->safeFrames();
        put_u64(out, (uint64_t)frames);
        // Bulk-write the PCM.  put_f32() is four push_back()s per sample with a
        // capacity check each; a 10-minute stereo clip is ~230 million of them,
        // which is most of a second of pure overhead per clip on save.  Size the
        // buffer once per channel and fill it directly instead.
        for (int ch = 0; ch < 2; ++ch) {
            const size_t at = out.size();
            out.resize(at + (size_t)frames * 4);
            uint8_t* dst = out.data() + at;
            const float* src = c->ch[ch].data();
            for (int64_t f = 0; f < frames; ++f) {
                uint32_t u; std::memcpy(&u, src + f, 4);
                dst[0] = (uint8_t)(u & 0xFF);         dst[1] = (uint8_t)((u >> 8) & 0xFF);
                dst[2] = (uint8_t)((u >> 16) & 0xFF); dst[3] = (uint8_t)((u >> 24) & 0xFF);
                dst += 4;
            }
        }
    }
    put_u32(out, (uint32_t)entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const ScheduledClip& sc = entries[i].scheduled;
        put_i32(out, entries[i].track);
        put_u32(out, srcIndex[sc.clip]);               // source-table index
        put_i64(out, sc.startSample);
        put_i64(out, sc.sourceOffset);
        put_i64(out, sc.length);
        put_f32(out, sc.gain);
        put_u8 (out, sc.muted ? 1 : 0);
        put_u8 (out, sc.loop  ? 1 : 0);
        put_i64(out, sc.loopLength);                   // v17: loop PERIOD (0 = whole source)
        put_i64(out, sc.fadeInFrames);
        put_i64(out, sc.fadeOutFrames);
        put_f32(out, sc.fadeInTension);
        put_f32(out, sc.fadeOutTension);
        // v19: fade shapes/slopes + the edit-time crossfade link (ch.32).
        put_u8 (out, sc.fadeInShape);
        put_u8 (out, sc.fadeOutShape);
        put_u8 (out, sc.fadeInSlope);
        put_u8 (out, sc.fadeOutSlope);
        put_u8 (out, sc.xfadeLink);
    }
}

// `dry` == parse and validate the exact same byte layout WITHOUT touching the
// engine.  It exists so load_project can prove a file is readable before it
// destroys the open session (see the two-pass loader below), and it shares this
// one parser rather than duplicating the layout in a second walker that would
// drift out of sync on the next format change.
bool restore_rack(Reader& r, rackx::RackEngine* rack, bool hasSampleState, bool dry) {
    const int poly = r.i32();
    const uint32_t modules = r.u32();
    if (!r.ok || modules > 1024) { r.ok = false; return false; }
    if (!dry && !rack) { r.ok = false; return false; }

    if (!dry) rack->clear();
    std::map<int,int> moduleMap;
    for (uint32_t i = 0; i < modules && r.ok; ++i) {
        const int oldId = r.i32();
        const std::string slug = r.str();
        const float x = r.f32(), y = r.f32();
        const int id = dry ? -1 : rack->addModule(slug, x, y);
        // A missing module type used to be silently skipped, which also dropped
        // all following params/cables and made a valid project appear "zeroed".
        // Registration now happens before RackNode creation; retain a hard
        // failure here so future registration regressions are visible.
        if (!dry && id < 0 && (slug == "PKPd" || slug == "PKCsound")) {
            r.ok = false;
            return false;
        }
        if (id >= 0) moduleMap[oldId] = id;
        const bool script = r.u8() != 0;
        if (script) {
                const std::string text = r.str();
                const float pw = r.f32();
                auto readEls = [&](std::vector<std::pair<float,float>>& out) {
                    const uint32_t n = r.u32();
                    if (r.ok && n <= 4096) for (uint32_t k = 0; k < n && r.ok; ++k) {
                        const float ex = r.f32(), ey = r.f32(); out.push_back({ ex, ey }); }
                };
                std::vector<std::pair<float,float>> ep, ei, eo;
                readEls(ep); readEls(ei); readEls(eo);
                if (id >= 0) {
                    rack->setModuleScript(id, text);          // reconfig ports/params
                    if (rackx::PanelSpec* pn = rack->modulePanel(id)) {
                        if (pw > 0.f) pn->width = pw;
                        for (size_t k = 0; k < ep.size() && k < pn->params.size();  ++k) { pn->params[k].x  = ep[k].first; pn->params[k].y  = ep[k].second; }
                        for (size_t k = 0; k < ei.size() && k < pn->inputs.size();  ++k) { pn->inputs[k].x  = ei[k].first; pn->inputs[k].y  = ei[k].second; }
                        for (size_t k = 0; k < eo.size() && k < pn->outputs.size(); ++k) { pn->outputs[k].x = eo[k].first; pn->outputs[k].y = eo[k].second; }
                    }
                }
        }
        const uint32_t params = r.u32();
        if (!r.ok || params > 4096) { r.ok = false; return false; }
        for (uint32_t p = 0; p < params; ++p) {
            const float value = r.f32();
            if (id >= 0) rack->setParam(id, (int)p, value);
        }
        // v7: reload a slot-backed module's sample.  Restored AFTER the params
        // so start/end/loop markers are already in place; a missing file is not
        // fatal (the rest of the rack must still load).
        if (hasSampleState) {
            const bool haveAudio = r.u8() != 0;
            if (haveAudio) {
                const std::string sname = r.str();
                const std::string spath = r.str();
                const double srate = r.f64();
                const uint64_t frames = r.u64();
                // Bound the allocation: a corrupt count must not ask for GBs.
                if (!r.ok || frames > (uint64_t)1 << 31) { r.ok = false; return false; }
                if (dry) {                       // validate the extent, read nothing
                    const uint64_t bytes = frames * 2ULL * sizeof(float);
                    const uint64_t left  = (r.pos <= r.n) ? (uint64_t)(r.n - r.pos) : 0ULL;
                    if (bytes > left) { r.ok = false; return false; }
                    r.pos += (size_t)bytes;
                } else {
                std::vector<float> sl((size_t)frames), sr((size_t)frames);
                for (uint64_t k = 0; k < frames && r.ok; ++k) sl[(size_t)k] = r.f32();
                for (uint64_t k = 0; k < frames && r.ok; ++k) sr[(size_t)k] = r.f32();
                if (!r.ok) return false;
                if (id >= 0) {
                    rackx::RackModule* rm = rack->moduleById(id);
                    rackx::ISampleSlot* slot = (rm && rm->mod)
                        ? dynamic_cast<rackx::ISampleSlot*>(rm->mod.get()) : nullptr;
                    // Install the embedded audio directly.  Only fall back to
                    // re-reading the source file if the slot cannot take it.
                    if (slot && !slot->sampleSetAudio(sl, sr, srate, sname, spath)
                        && !spath.empty()) {
                        std::string err;
                        slot->sampleLoad(spath, PatchKnob::app::audio_app_sample_rate(), &err);
                    }
                }
                }
            }
        }
    }
    const uint32_t cables = r.u32();
    if (!r.ok || cables > 4096) { r.ok = false; return false; }
    for (uint32_t i = 0; i < cables && r.ok; ++i) {
        const int fromOld = r.i32(), outPort = r.i32();
        const int toOld = r.i32(), inPort = r.i32();
        std::map<int,int>::const_iterator from = moduleMap.find(fromOld);
        std::map<int,int>::const_iterator to = moduleMap.find(toOld);
        if (from != moduleMap.end() && to != moduleMap.end())
            rack->addCable(from->second, outPort, to->second, inPort);
    }
    if (!dry) { rack->setPolyphony(poly); rack->ensureDefaultIO(); }
    return r.ok;
}

bool restore_patch(Reader& r, uint32_t ver, bool hasRackSamples, bool dry) {
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;

    const bool modular = r.u8() != 0;
    const bool multithreaded = r.u8() != 0;
    const uint32_t savedOut = r.u32();
    const uint32_t savedMidi = r.u32();
    const uint32_t savedMaster = r.u32();
    const uint32_t savedHwMidi = r.u32();
    const uint32_t savedVirtualMidi = r.u32();
    const uint32_t savedVirtualOutputs=r.u32();
    if(!r.ok||savedVirtualOutputs>32) { r.ok=false; return false; }
    std::vector<int> savedVirtualRoutes(savedVirtualOutputs,-1);
    for(uint32_t o=0;o<savedVirtualOutputs;++o) savedVirtualRoutes[o]=r.i32();
    const uint32_t tracks = r.u32();
    const float masterGain = r.f32();
    if (!r.ok || tracks > 256) { r.ok = false; return false; }

    struct MasterTrack { bool midi; float gain, pan; bool mute; bool solo; };
    std::vector<MasterTrack> masterTracks;
    masterTracks.reserve(tracks);
    for (uint32_t i = 0; i < tracks; ++i)
        masterTracks.push_back(MasterTrack{r.u8() != 0, r.f32(), r.f32(), r.u8() != 0, false});
    if (!r.ok) return false;

    // ---- v18: aux buses + sends + solo.  A pre-v18 file simply has none, and
    // reads back as the state it was saved in (see the migration note on
    // PROJ_VERSION) -- no port id changes meaning across the bump.
    struct AuxInsert { uint32_t node; bool active; bool pre; };
    struct AuxBus { uint32_t node; std::string name; float gain; bool mute;
                    std::vector<AuxInsert> inserts; };
    struct Send { float level; bool pre; bool on; };
    std::vector<AuxBus> auxes;
    std::vector<Send>   sends;                 // tracks * auxes, track-major
    if (ver >= 18) {
        const uint32_t nAux = r.u32();
        if (!r.ok || nAux > (uint32_t)patch::MasterMixerNode::kMaxAuxBuses) {
            r.ok = false; return false;
        }
        auxes.reserve(nAux);
        for (uint32_t a = 0; a < nAux && r.ok; ++a) {
            AuxBus b;
            b.node = r.u32();
            b.name = r.str();
            b.gain = r.f32();
            b.mute = r.u8() != 0;
            const uint32_t nIns = r.u32();
            if (!r.ok || nIns > 512) { r.ok = false; return false; }
            for (uint32_t k = 0; k < nIns && r.ok; ++k) {
                AuxInsert ins;
                ins.node   = r.u32();
                ins.active = r.u8() != 0;
                ins.pre    = r.u8() != 0;
                b.inserts.push_back(ins);
            }
            auxes.push_back(b);
        }
        if (!r.ok) return false;
        for (uint32_t i = 0; i < tracks; ++i) masterTracks[i].solo = r.u8() != 0;
        if (!r.ok) return false;
        sends.resize((size_t)tracks * auxes.size());
        for (uint32_t i = 0; i < tracks && r.ok; ++i)
            for (size_t a = 0; a < auxes.size() && r.ok; ++a) {
                Send sd; sd.level = r.f32(); sd.pre = r.u8() != 0; sd.on = r.u8() != 0;
                sends[(size_t)i * auxes.size() + a] = sd;
            }
        if (!r.ok) return false;
    }

    std::map<uint32_t,int> nodeMap;
    int masterNode = -1;
    if (!dry) {
        g_loadedPatchNodeIds.clear();
        audio_app_project_reset_patch();
        const int outNode = audio_app_patch_out_node();
        const int midiNode = audio_app_patch_midi_in_node();
        masterNode = audio_app_master_mixer_node();
        nodeMap[savedOut] = outNode;
        nodeMap[savedMidi] = midiNode;
        nodeMap[savedMaster] = masterNode;
        nodeMap[savedHwMidi]=audio_app_default_hw_midi_in_node();
        nodeMap[savedVirtualMidi]=audio_app_virtual_midi_node();
        for(uint32_t o=0;o<savedVirtualOutputs;++o)
            audio_app_virtual_midi_set_route((int)o,savedVirtualRoutes[o]);

        for (size_t i = 0; i < masterTracks.size(); ++i) {
            int idx = audio_app_master_add_track(masterTracks[i].midi ? 1 : 0);
            if (idx >= 0) {
                audio_app_mixer_set_gain(masterNode, idx, masterTracks[i].gain);
                audio_app_mixer_set_pan(masterNode, idx, masterTracks[i].pan);
                audio_app_mixer_set_mute(masterNode, idx, masterTracks[i].mute);
            }
        }
        audio_app_mixer_set_master_gain(masterNode, masterGain);
        for (size_t i = 0; i < masterTracks.size(); ++i)
            audio_app_master_set_solo((int)i, masterTracks[i].solo ? 1 : 0);

        // The buses have to exist before the node/connection tables are
        // replayed: their node ids appear in the saved connection list (the
        // send and return cables), so they need nodeMap entries.
        for (size_t a = 0; a < auxes.size(); ++a) {
            const int idx = audio_app_master_aux_add(auxes[a].name.c_str());
            if (idx < 0) continue;
            audio_app_master_aux_set_return_gain(idx, auxes[a].gain);
            audio_app_master_aux_set_mute(idx, auxes[a].mute ? 1 : 0);
            const int busNode = audio_app_master_aux_node(idx);
            if (busNode >= 0) nodeMap[auxes[a].node] = busNode;
        }
        for (size_t i = 0; i < masterTracks.size(); ++i)
            for (size_t a = 0; a < auxes.size(); ++a) {
                const size_t ix = i * auxes.size() + a;
                if (ix >= sends.size()) break;
                const Send& sd = sends[ix];
                audio_app_master_set_send_level((int)i, (int)a, sd.level);
                audio_app_master_set_send_prefader((int)i, (int)a, sd.pre ? 1 : 0);
                audio_app_master_set_send_enabled((int)i, (int)a, sd.on ? 1 : 0);
            }
    }

    const uint32_t nodes = r.u32();
    if (!r.ok || nodes > 512) { r.ok = false; return false; }
    for (uint32_t i = 0; i < nodes && r.ok; ++i) {
        const uint32_t oldId = r.u32();
        const int kind = r.u8();
        const bool bypass = r.u8() != 0;
        const int channel = r.i32();
        int node = -1;
        if (kind == PATCH_PLUGIN) {
            StoredPlugin plugin = get_plugin(r);
            if (!dry) {
                node = stored_plugin_is_sampler(plugin) ? audio_app_patch_add_sampler()
                     : plugin.present ? audio_app_patch_add_plugin(plugin.desc)
                                      : audio_app_patch_add_empty_plugin();
                if (node < 0 && plugin.present)
                    node = audio_app_patch_add_empty_plugin();
                if (node >= 0 && plugin.present) {
                    IPluginInstance* inst = audio_app_patch_node_instance(node);
                    if (inst && !plugin.state.empty()) inst->loadState(plugin.state);
                }
            }
        } else if (kind == PATCH_MIXER) {
            const int channels = r.i32(); const float gain = r.f32();
            if (channels < 1 || channels > 256) { r.ok = false; return false; }
            if (!dry) node = audio_app_patch_add_mixer(channels);
            for (int c = 0; c < channels; ++c) {
                const float cg = r.f32(), pan = r.f32(); const bool mute = r.u8() != 0;
                if (node >= 0) {
                    audio_app_mixer_set_gain(node, c, cg);
                    audio_app_mixer_set_pan(node, c, pan);
                    audio_app_mixer_set_mute(node, c, mute);
                }
            }
            if (node >= 0) audio_app_mixer_set_master_gain(node, gain);
        } else if (kind == PATCH_PD) {
            (void)r.str();                               // legacy path field (ignored)
            const std::vector<uint8_t> source = r.blob();
            if (!dry) node = audio_app_patch_add_pd();
            if (node >= 0)                               // load the patch text into memory
                audio_app_pd_set_text(node, std::string(source.begin(), source.end()).c_str());
        } else if (kind == PATCH_CSOUND) {
            const std::string csd = r.str();
            if (!dry) node = audio_app_patch_add_csound();
            if (node >= 0) {
                audio_app_patch_csound_set_text(node, csd.c_str());
                audio_app_patch_csound_recompile(node);   // sets nchnls/nchnls_i ports before connections restore
            }
        } else if (kind == PATCH_RACK) {
            if (!dry) node = audio_app_patch_add_rack();
            if (!restore_rack(r, dry ? nullptr : audio_app_rack_engine(node),
                              hasRackSamples, dry)) return false;
        } else if (kind == PATCH_SINE) {
            const float freq = r.f32(), amp = r.f32();
            if (!dry) {
                node = audio_app_patch_add_builtin("sine");
                if (auto* n = dynamic_cast<patch::SineSourceNode*>(audio_app_patch_graph()->node((patch::NodeId)node))) {
                    n->setFreq(freq); n->setAmp(amp);
                }
            }
        } else if (kind == PATCH_GAIN) {
            const float gain = r.f32();
            if (!dry) {
                node = audio_app_patch_add_builtin("gain");
                if (auto* n = dynamic_cast<patch::GainNode*>(audio_app_patch_graph()->node((patch::NodeId)node))) n->setGain(gain);
            }
        } else if (kind == PATCH_SUM) {
            if (!dry) node = audio_app_patch_add_builtin("sum");
        } else if (kind == PATCH_AUDIO_OUT) {
            if (!dry) node = audio_app_patch_add_builtin("out");
        } else if (kind == PATCH_AUDIO_IN) {
            if (!dry) node = audio_app_patch_add_builtin("in");
        } else if (kind == PATCH_MIDI_IN) {
            if (!dry) node = audio_app_patch_add_midi_in(-1);
        } else if (kind == PATCH_MIDI_OUT) {
            if (!dry) node = audio_app_patch_add_midi_out(-1);
        } else if (kind == PATCH_RECORD) {
            (void)r.u8(); // obsolete recorder arm byte; numeric kind is reserved
            node = -1;
        } else if(kind==PATCH_MIDI_TRACK) {
            if (!dry) node=audio_app_add_midi_track_node();
            const int input=r.i32(),output=r.i32();
            if(node>=0)audio_app_midi_track_set_routing(node,input,output);
        } else { r.ok = false; return false; }

        if (node >= 0) {
            nodeMap[oldId] = node;
            audio_app_patch_set_node_channel(node, channel);
            if (patch::Node* n = audio_app_patch_graph()->node((patch::NodeId)node)) n->setBypass(bypass);
        }
    }

    const uint32_t conns = r.u32();
    if (!r.ok || conns > 4096) { r.ok = false; return false; }
    for (uint32_t i = 0; i < conns; ++i) {
        const uint32_t fromNode = r.u32(), fromPort = r.u32();
        const uint32_t toNode = r.u32(), toPort = r.u32();
        std::map<uint32_t,int>::const_iterator from = nodeMap.find(fromNode);
        std::map<uint32_t,int>::const_iterator to = nodeMap.find(toNode);
        if (from != nodeMap.end() && to != nodeMap.end())
            audio_app_patch_connect(from->second, (int)fromPort, to->second, (int)toPort);
    }
    if (!dry) {
        // Bus processor boxes last: their entries are ordinary patch nodes, so
        // they only exist once the node table above has been replayed.  Adding
        // them re-wires each bus's chain from scratch (send -> inserts -> bus),
        // which also cleans up the direct send->bus cable audio_app_master_aux_add
        // drew for an empty bus.
        for (size_t a = 0; a < auxes.size(); ++a) {
            const int strip = audio_app_master_aux_strip((int)a);
            for (size_t k = 0; k < auxes[a].inserts.size(); ++k) {
                std::map<uint32_t,int>::const_iterator it = nodeMap.find(auxes[a].inserts[k].node);
                if (it == nodeMap.end()) continue;
                const int slot = audio_app_master_insert_add_node(strip, it->second,
                                                                  auxes[a].inserts[k].pre ? 1 : 0);
                if (slot >= 0)
                    audio_app_master_insert_set_active(strip, slot,
                                                       auxes[a].inserts[k].active ? 1 : 0);
            }
        }
        for (std::map<uint32_t,int>::const_iterator it = nodeMap.begin(); it != nodeMap.end(); ++it)
            g_loadedPatchNodeIds[it->first] = (uint32_t)it->second;
        audio_app_set_multithreaded(multithreaded);
        audio_app_set_modular(modular);
    }
    return r.ok;
}

// Read one embedded AudioClip (name + rates + interleaved-by-channel samples).
// `clip == nullptr` validates the record and skips its payload (dry run).
bool read_audio_clip(Reader& r, PatchKnob::engine::AudioClip* clip) {
    const std::string name = r.str();
    const double rate = r.f64(), srcRate = r.f64();
    if (clip) { clip->name = name; clip->sampleRate = rate; clip->sourceSampleRate = srcRate; }
    const uint64_t frames = r.u64();
    const uint64_t bytesPerFrame = sizeof(float) * 2ULL;
    const uint64_t kMaxEmbeddedClipBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    if (!r.ok || frames > (uint64_t)std::numeric_limits<int64_t>::max() ||
        frames > std::numeric_limits<uint64_t>::max() / bytesPerFrame) {
        r.ok = false; return false;
    }
    const uint64_t bytesNeeded = frames * bytesPerFrame;
    const uint64_t bytesLeft = (r.pos <= r.n) ? (uint64_t)(r.n - r.pos) : 0ULL;
    if (bytesNeeded > bytesLeft || bytesNeeded > kMaxEmbeddedClipBytes) {
        r.ok = false; return false;
    }
    if (!clip) { r.pos += (size_t)bytesNeeded; return r.ok; }
    clip->resize((int64_t)frames);
    // The `bytesNeeded` check above already proved every one of these bytes is
    // present, so re-checking bounds per sample (what r.f32() does) buys nothing
    // and costs ~230 million checks on a 10-minute stereo clip.
    for (int ch = 0; ch < 2; ++ch) {
        const uint8_t* src = r.p + r.pos;
        float* dst = clip->ch[ch].data();
        for (uint64_t f = 0; f < frames; ++f) {
            const uint32_t u = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                               ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
            std::memcpy(dst + f, &u, 4);
            src += 4;
        }
        r.pos += (size_t)frames * 4;
    }
    return r.ok;
}

bool restore_audio(Reader& r, uint32_t ver, bool dry) {
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    if (!dry) audio_app_project_clear_audio_clips();

    const uint32_t nsrc = r.u32();
    if (!r.ok || nsrc > 4096) { r.ok = false; return false; }
    std::vector<AudioClip> sources(dry ? 0 : (size_t)nsrc);
    for (uint32_t i = 0; i < nsrc && r.ok; ++i)
        if (!read_audio_clip(r, dry ? nullptr : &sources[i])) return false;
    const uint32_t count = r.u32();
    if (!r.ok || count > 4096) { r.ok = false; return false; }
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        const int      track = r.i32();
        const uint32_t sidx  = r.u32();
        const int64_t  start = r.i64();
        const int64_t  soff  = r.i64();
        const int64_t  len   = r.i64();
        const float    gain  = r.f32();
        const uint8_t  muted = r.u8();
        const uint8_t  loop  = r.u8();
        // v17 added the loop PERIOD.  Older files looped the whole source, and
        // 0 means exactly that, so they need no migration.
        const int64_t  loopLen = ver >= 17 ? r.i64() : 0;
        const int64_t  fIn   = r.i64();
        const int64_t  fOut  = r.i64();
        const float    fInK  = r.f32();
        const float    fOutK = r.f32();
        // v19 added the fade shapes/slopes + crossfade link (ch.32).  Older
        // files carry only the tension curve; 0/0 == Standard / Equal Gain is
        // exactly how those fades always rendered, so no migration is needed.
        const uint8_t  fInShape  = ver >= 19 ? r.u8() : 0;
        const uint8_t  fOutShape = ver >= 19 ? r.u8() : 0;
        const uint8_t  fInSlope  = ver >= 19 ? r.u8() : 0;
        const uint8_t  fOutSlope = ver >= 19 ? r.u8() : 0;
        const uint8_t  xLink     = ver >= 19 ? r.u8() : 0;
        if (!r.ok || sidx >= nsrc) { r.ok = false; return false; }
        if (!dry)
            audio_app_project_add_audio_region(track, sources[sidx], start, soff, len,
                                               gain, muted, loop, fIn, fOut, fInK, fOutK,
                                               loopLen, fInShape, fOutShape,
                                               fInSlope, fOutSlope, xLink);
    }
    return r.ok;
}

} // anonymous namespace

// ===========================================================================
//  SAVE
// ===========================================================================
// --- automation lanes (v5 "AUTO"): per track, its lanes + breakpoints ---------
void put_automation(Buf& out, const PatchKnob::engine::AutomationPlayer* ap) {
    using namespace PatchKnob::engine;
    auto putLane=[&](const AutomationLane& ln){
        put_u8(out,(uint8_t)ln.target().kind);put_u32(out,(uint32_t)ln.target().id);
        put_i32(out,ln.target().node);put_i32(out,ln.target().module);
        put_f32(out,ln.target().minValue);put_f32(out,ln.target().maxValue);
        put_u8(out,(uint8_t)ln.interpolation());
        const auto& bps=ln.breakpoints();put_u32(out,(uint32_t)bps.size());
        for(const Breakpoint& b:bps){put_i64(out,b.tick);put_f32(out,b.value);put_f32(out,b.curve);}
    };
    const int nt = ap ? ap->trackCount() : 0;
    put_u32(out, (uint32_t)nt);
    for (int t = 0; t < nt; ++t) {
        const AutomationTrack& at = ap->track(t);
        put_u32(out, (uint32_t)at.laneCount());
        for (int li = 0; li < at.laneCount(); ++li) {
            putLane(at.lane(li));
        }
    }
    const auto& regions=ap?ap->regions():std::vector<AutomationPlayer::Region>();
    put_u32(out,(uint32_t)regions.size());
    for(const auto& rg:regions){
        put_i32(out,rg.id);put_i32(out,rg.destinationTrack);put_i64(out,rg.position);
        put_i64(out,rg.length);put_i64(out,rg.loopLength);put_i64(out,rg.loopStart);
        put_i64(out,rg.source);put_u8(out,rg.muted?1:0);
        put_str(out,rg.trackerFx);put_u32(out,(uint32_t)rg.automation.laneCount());
        for(int li=0;li<rg.automation.laneCount();++li)putLane(rg.automation.lane(li));
    }
}
void restore_automation(Reader& r, PatchKnob::engine::AutomationPlayer* ap, uint32_t version) {
    using namespace PatchKnob::engine;
    const uint32_t nt = r.u32();
    for (uint32_t t = 0; t < nt && r.ok; ++t) {
        const uint32_t nl = r.u32();
        for (uint32_t li = 0; li < nl && r.ok; ++li) {
            const uint8_t kind = r.u8();
            const uint32_t id  = r.u32();
            int node=-1,module=-1;float minv=0.f,maxv=1.f;
            if(version>=11){node=r.i32();module=r.i32();minv=r.f32();maxv=r.f32();}
            const uint8_t interp = r.u8();
            const uint32_t nb  = r.u32();
            if (!r.ok || nb > 1000000u) { r.ok = false; return; }
            LaneTarget tgt; tgt.kind=version>=11?(LaneTargetKind)kind:
                (kind?LaneTargetKind::MidiCC:LaneTargetKind::VstParam);
            tgt.id=id;tgt.node=node;tgt.module=module;tgt.minValue=minv;tgt.maxValue=maxv;
            Interpolation ip = interp == 1 ? Interpolation::Step
                             : interp == 2 ? Interpolation::Hold : Interpolation::Linear;
            if (ap && (int)t < ap->trackCount()) {
                AutomationLane& ln = ap->track((int)t).addLane(tgt, ip);
                for (uint32_t bi = 0; bi < nb && r.ok; ++bi) {
                    int64_t tick = r.i64(); float val = r.f32(); float curve=version>=11?r.f32():0.f;
                    tick = migrate_tick((long)tick, version);   // 192-PPQN files -> c_ppqn
                    int pointIndex=ln.add(tick,val); if(curve!=0.f)ln.setCurveAfter(pointIndex,curve);
                }
            } else {
                for (uint32_t bi = 0; bi < nb && r.ok; ++bi) { r.i64(); r.f32();if(version>=11)r.f32(); }
            }
        }
    }
    if(version>=11&&r.ok){
        const uint32_t nr=r.u32();if(nr>100000u){r.ok=false;return;}
        for(uint32_t ri=0;ri<nr&&r.ok;++ri){
            const int id=r.i32(),dest=r.i32();const int64_t pos=r.i64(),len=r.i64();
            const int64_t loop=version>=12?r.i64():len;
            //  v20 added the window's START.  Older files have none, and 0 is
            //  exactly how they always folded, so they need no migration.
            const int64_t lstart=version>=20?r.i64():0;
            const int64_t src=r.i64();
            const bool muted=r.u8()!=0;const std::string fx=r.str();const uint32_t nl=r.u32();
            AutomationPlayer::Region* region=ap?&ap->ensureRegion(id):nullptr;
            if(region){region->destinationTrack=dest;
                       // musical positions -- migrate 192-PPQN files to c_ppqn
                       region->position=migrate_tick((long)pos,version);
                       region->length=migrate_tick((long)len,version);
                       region->loopLength=migrate_tick((long)loop,version);
                       region->loopStart=migrate_tick((long)lstart,version);
                       region->source=migrate_tick((long)src,version);
                       region->muted=muted;region->trackerFx=fx;region->automation.clear();}
            for(uint32_t li=0;li<nl&&r.ok;++li){
                LaneTarget tgt;tgt.kind=(LaneTargetKind)r.u8();tgt.id=r.u32();tgt.node=r.i32();
                tgt.module=r.i32();tgt.minValue=r.f32();tgt.maxValue=r.f32();
                const uint8_t ipb=r.u8();const uint32_t nb=r.u32();
                AutomationLane* lane=nullptr;if(region)lane=&region->automation.addLane(tgt,
                    ipb==1?Interpolation::Step:(ipb==2?Interpolation::Hold:Interpolation::Linear));
                for(uint32_t bi=0;bi<nb&&r.ok;++bi){int64_t tk=r.i64();float v=r.f32(),c=r.f32();
                    tk=migrate_tick((long)tk,version);
                    if(lane){int ix=lane->add(tk,v);if(c!=0.f)lane->setCurveAfter(ix,c);}}
            }
        }
    }
}

bool save_project(perform& p, const std::string& path,
                  const std::vector<ProjectPatchNodePosition>& patchLayout,
                  const std::vector<ProjectFreezeRecord>& freezes,
                  const PatchKnob::engine::AutomationPlayer* autoPlayer)
{
    g_last_error.clear();
    Buf out;

    // header
    out.insert(out.end(), MAGIC, MAGIC + 8);
    put_u32(out, PROJ_VERSION);

    // --- GLOB ---------------------------------------------------------------
    {
        Buf g;
        put_u8 (g, (uint8_t)(ui::mode() == ui::Mode::Midnight ? 1 : 0));
        put_f64(g, p.get_bpm());
        put_u8 (g, (uint8_t)(p.get_looping() ? 1 : 0));
        put_i64(g, (int64_t)p.get_left_tick());
        put_i64(g, (int64_t)p.get_right_tick());
        put_i32(g, PatchKnob::app::audio_app_virtual_midi_inputs());
        put_i32(g, PatchKnob::app::audio_app_virtual_midi_outputs());
        // v19: the session AutoFades length (PT p752; saved with the session)
        put_f64(g, PatchKnob::app::audio_app_auto_fade_ms());
        put_section(out, "GLOB", g);
    }

    // --- SEQ  (one per active sequence) -------------------------------------
    for (int i = 0; i < c_max_sequence; ++i) {
        if (!p.is_active(i)) continue;
        sequence* s = p.get_sequence(i);
        if (!s) continue;

        Buf sq;
        put_u32(sq, (uint32_t)i);
        put_str(sq, std::string(s->get_name() ? s->get_name() : ""));
        put_u8 (sq, (uint8_t)s->get_midi_bus());
        put_u8 (sq, (uint8_t)s->get_midi_channel());
        put_i32(sq, (int32_t)s->get_length());
        put_i32(sq, (int32_t)s->get_bpm());
        put_i32(sq, (int32_t)s->get_bw());
        put_u8 (sq, (uint8_t)(s->get_playing() ? 1 : 0));
        put_u8 (sq, (uint8_t)(s->get_scale_master()  ? 1 : 0));
        put_u8 (sq, (uint8_t)(s->get_follows_master() ? 1 : 0));
        put_i32(sq, (int32_t)s->get_master_scale());
        put_i32(sq, (int32_t)s->get_master_key());
        put_u8 (sq, (uint8_t)(s->get_song_mute() ? 1 : 0));   // v4: freeze/song mute
        put_str(sq, s->get_fx_blob());                        // v4: per-pattern tracker FX
        put_u8 (sq, (uint8_t)(s->get_thru() ? 1 : 0));         // v5: MIDI thru
        put_u8 (sq, (uint8_t)s->get_track_kind());              // v11: INS/AUDIO/AUTO
        put_i32(sq, s->get_arrange_lane_id());                  // v11: stable playlist identity
        put_i32(sq, (int32_t)s->get_loop_start());              // v14: piano-roll loop-start marker
        put_i32(sq, (int32_t)s->get_loop_end());                // v15: piano-roll loop-end marker
        put_i32(sq, s->get_loop_enabled() ? 1 : 0);             // v16: loop on/off (off = one-shot)

        std::vector<RawEv> evs;
        collect_events(s, evs);
        put_u32(sq, (uint32_t)evs.size());
        for (size_t e = 0; e < evs.size(); ++e) {
            put_i32(sq, (int32_t)evs[e].tick);
            put_u8 (sq, evs[e].status);
            put_u8 (sq, evs[e].d0);
            put_u8 (sq, evs[e].d1);
            // v9: tracker column (0xFF == untagged)
            put_u8 (sq, (uint8_t)(evs[e].column < 0 || evs[e].column > 254
                                  ? 0xFF : evs[e].column));
        }
        std::vector<RawTrigger> triggers;
        collect_triggers(s, triggers);
        put_u32(sq, (uint32_t)triggers.size());
        for (size_t t = 0; t < triggers.size(); ++t) {
            put_i32(sq, (int32_t)triggers[t].start);
            put_i32(sq, (int32_t)triggers[t].length);
            put_i32(sq, (int32_t)triggers[t].offset);
        }
        put_section(out, "SEQ ", sq);
    }

    // --- TRAK / PTCH / AUDI -------------------------------------------------
    // These three come out of the LIVE engine.  When it is down they are carried
    // over from the file on disk instead of being dropped -- see
    // read_carried_sections() for why silently omitting them was destructive.
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;
    const bool canWriteTracks = (graph != nullptr);
    const bool canWritePatch  = audio_app_running() && audio_app_patch_graph() != nullptr;
    CarriedSections carried;
    if (!canWriteTracks || !canWritePatch) {
        carried = read_carried_sections(path);
        const bool needTrak = !canWriteTracks && !carried.trak.empty();
        const bool needPtch = !canWritePatch  && !carried.ptch.empty();
        const bool needAudi = !canWriteTracks && !carried.audi.empty();
        if ((needTrak || needPtch || needAudi) && !carried.sameVersion) {
            set_err("the audio engine is not running, so the modular patch, rack, "
                    "embedded audio and plugin state already in '" + path + "' "
                    "cannot be re-saved -- and that file is an older format "
                    "version, so they cannot be carried over verbatim either.  "
                    "Start the audio engine (or save to a new file) instead of "
                    "overwriting it with an incomplete project.");
            return false;
        }
    }
    if (graph) {
        int nt = graph->trackCount();
        for (int t = 0; t < nt; ++t) {
            Track* trk = graph->track(t);
            if (!trk) continue;
            IPluginInstance* inst = trk->instrument();

            // AudioClipPlayer is serialized losslessly in the AUDI section;
            // it is not a VST descriptor and must not be written as one here.
            bool hasInst = inst && !dynamic_cast<AudioClipPlayer*>(inst);
            bool hasFx = trk->fxCount() > 0;
            bool dfltMix = (trk->gain() == 1.0f && trk->pan() == 0.0f &&
                            !trk->mute() && !trk->solo() && !trk->disabled());
            if (!hasInst && !hasFx && dfltMix) continue;   // nothing worth persisting

            Buf tk;
            put_u32(tk, (uint32_t)t);
            put_f32(tk, trk->gain());
            put_f32(tk, trk->pan());
            put_u8 (tk, (uint8_t)(trk->mute() ? 1 : 0));
            put_u8 (tk, (uint8_t)(trk->solo() ? 1 : 0));
            put_u8 (tk, (uint8_t)(trk->disabled() ? 1 : 0));   // v5
            put_u8 (tk, (uint8_t)(hasInst ? 1 : 0));
            if (hasInst) put_plugin(tk, inst);
            put_u32(tk, (uint32_t)trk->fxCount());
            for (int f = 0; f < trk->fxCount(); ++f) put_plugin(tk, trk->fxAt(f));
            put_section(out, "TRAK", tk);
        }
    } else {
        out.insert(out.end(), carried.trak.begin(), carried.trak.end());
    }

    // --- PTCH / AUDI -------------------------------------------------------
    // The patch graph owns the modular instruments, Pd and Rack configurations;
    // audio is stored as raw stereo samples, not a path to an external WAV.
    if (canWritePatch) {
        // An EMPTY payload is not a valid PTCH -- restore_patch fails the whole
        // load on one -- and put_patch only produces one if the graph vanished
        // between the check above and the call.  Prefer whatever is on disk.
        Buf patch; put_patch(patch);
        if (!patch.empty()) put_section(out, "PTCH", patch);
        else out.insert(out.end(), carried.ptch.begin(), carried.ptch.end());
    } else {
        out.insert(out.end(), carried.ptch.begin(), carried.ptch.end());
    }
    if (canWriteTracks) {
        Buf audio; put_audio(audio);
        if (!audio.empty()) put_section(out, "AUDI", audio);
        else out.insert(out.end(), carried.audi.begin(), carried.audi.end());
    } else {
        out.insert(out.end(), carried.audi.begin(), carried.audi.end());
    }

    {
        Buf layout;
        put_u32(layout, (uint32_t)patchLayout.size());
        for (size_t i = 0; i < patchLayout.size(); ++i) {
            put_u32(layout, patchLayout[i].nodeId);
            put_f64(layout, patchLayout[i].x);
            put_f64(layout, patchLayout[i].y);
        }
        put_section(out, "PUI ", layout);
    }

    // --- FRZ (freeze relationships; v4) -------------------------------------
    if (!freezes.empty()) {
        Buf fz;
        put_u32(fz, (uint32_t)freezes.size());
        for (size_t i = 0; i < freezes.size(); ++i) {
            put_i32(fz, freezes[i].srcSeq);
            put_u8 (fz, (uint8_t)(freezes[i].isTrack ? 1 : 0));
            put_i32(fz, freezes[i].newSeq);
            put_i32(fz, freezes[i].srcTrack);
            put_u8 (fz, (uint8_t)(freezes[i].wasMuted ? 1 : 0));
            put_i32(fz, freezes[i].audioTrack);
        }
        put_section(out, "FRZ ", fz);
    }

    // --- AUTO (v5: automation lanes) ---------------------------------------
    if (autoPlayer) {
        Buf a; put_automation(a, autoPlayer); put_section(out, "AUTO", a);
    }

    // --- END ----------------------------------------------------------------
    { Buf e; put_section(out, "END ", e); }

    return install_file_atomically(path, out, "project");
}

// ===========================================================================
//  LOAD
// ===========================================================================
// Every field "TRAK" can carry has to be represented here, or a track the
// incoming file happens not to mention silently keeps the OUTGOING session's
// value for it.  `disabled` was the one that got missed: TRAK writes it and
// restores it, but the reset did not clear it, so opening a project with no
// TRAK for a track that had been FROZEN left that track disabled -- perfectly
// normal-looking in the mixer, and completely silent.
void project_io_reset_track_defaults(PatchKnob::engine::Track& track) {
    track.setInstrument(nullptr);
    while (track.fxCount() > 0) track.removeFx(track.fxCount() - 1);
    track.setGain(1.0f);
    track.setPan(0.0f);
    track.setMute(false);
    track.setSolo(false);
    track.setDisabled(false);
}

bool load_project(perform& p, const std::string& path,
                  PatchKnob::engine::AutomationPlayer* autoPlayer)
{
    g_last_error.clear();
    // NOTE: autoPlayer->clear() is deliberately NOT done here -- see the
    // two-pass structure below.  Nothing that belongs to the OPEN session may be
    // touched until the file on disk has been proven readable end to end.

    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) { set_err("cannot open '" + path + "' for reading"); return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (data.size() < 12) { set_err("file too small / truncated"); return false; }

    Reader r(data.data(), data.size());
    char magic[8];
    for (int i = 0; i < 8; ++i) magic[i] = (char)r.u8();
    if (std::memcmp(magic, MAGIC, 8) != 0) { set_err("bad magic (not a S24 project)"); return false; }
    uint32_t ver = r.u32();
    // v9 added a per-event tracker column byte.  v8 files still load; their
    // notes simply come back untagged, exactly as they were saved.
    //
    // A RANGE, not an enumeration.  The enumerated form silently rotted on
    // every version bump: adding v16 left the list reading "PROJ_VERSION, 14,
    // 13, ..." and v15 -- the format the immediately preceding build wrote,
    // loop_start + loop_end and all -- became "unsupported project version",
    // i.e. unopenable.  Every version from kOldestReadableVersion up is parsed
    // by the same reader, gated field-by-field on `ver` below, so the accepted
    // set is exactly that closed interval and must be expressed as one.
    const uint32_t kOldestReadableVersion = 8;
    if (ver < kOldestReadableVersion || ver > PROJ_VERSION) {
        set_err("unsupported project version"); return false;
    }
    const bool hasColumns = (ver >= 9);
    // v10 appends each rack module's sample-slot path after its params.
    const bool hasRackSamples = (ver >= 10);

    // -----------------------------------------------------------------------
    //  PASS 1 -- PARSE.  Read the whole file into staging state and touch
    //  NOTHING that belongs to the open session.
    //
    //  The loader used to delete every sequence and reset every track before it
    //  had parsed a single section, so ANY later failure -- "truncated
    //  section", "invalid modular patch section", a plain parse error -- left
    //  the caller holding a false return on a project that no longer existed in
    //  memory.  Since undo/redo IS load_project (main.cpp restore_state), one
    //  damaged history file destroyed the whole open session.  Parse first;
    //  commit only once the file has been proven readable end to end.
    //
    //  The engine-graph sections (PTCH / AUDI) cannot be staged -- restoring
    //  them IS building the graph -- so they are validated by running the very
    //  same parser in `dry` mode, which reads the identical byte layout without
    //  calling into the engine.  One parser, two modes: a separate validator
    //  would drift out of sync with the reader on the next format change.
    // -----------------------------------------------------------------------
    struct StagedGlob {
        uint8_t themeMode = 0;
        double  bpm = 0.0;
        uint8_t looping = 0;
        int64_t leftTick = 0, rightTick = 0;
        bool    fieldsOk = false;          // GLOB's own r.ok gate, staged
        bool    hasVirtualPorts = false;
        int     midiIns = 0, midiOuts = 0;
        bool    hasAutoFade = false;       // v19+ AutoFades ms (ch.32)
        double  autoFadeMs = 0.0;
    };
    struct StagedSeq {
        uint32_t slot = 0;
        std::string name, fxBlob;
        uint8_t  midiBus = 0, midiChan = 0;
        int32_t  length = 0, seqBpm = 4, bw = 4;
        uint8_t  playing = 0, isMaster = 0, follows = 0, songMute = 0, thru = 0;
        uint8_t  trackKind = 0;
        int32_t  mScale = 0, mKey = 0, laneId = -1;
        int32_t  loopStart = 0, loopEnd = -1, loopOn = 1;
        std::vector<RawEv>      events;
        std::vector<RawTrigger> triggers;
    };
    struct StagedTrak {
        uint32_t track = 0;
        float    gain = 1.0f, pan = 0.0f;
        uint8_t  mute = 0, solo = 0, disabled = 0;
        StoredPlugin instrument;
        std::vector<StoredPlugin> fx;
    };
    struct StagedRange { size_t off = 0, len = 0; };   // payload of PTCH/AUDI/AUTO

    enum { K_GLOB, K_SEQ, K_TRAK, K_PTCH, K_AUDI, K_PUI, K_FRZ, K_AUTO };

    std::vector<StagedGlob>  globs;
    std::vector<StagedSeq>   seqs;
    std::vector<StagedTrak>  traks;
    std::vector<StagedRange> patches, audios, autos;
    std::vector<std::vector<ProjectPatchNodePosition> > layouts;
    std::vector<std::vector<ProjectFreezeRecord> >      freezeSets;
    // (kind, index) in FILE order -- the commit replays sections exactly as
    // they were written, because PTCH resets the patch graph and TRAK does not.
    std::vector<std::pair<int,size_t> > order;

    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;
    // PTCH / AUDI are only applied when there is a live graph, so they are only
    // validated then either -- pass 1 must accept exactly what pass 2 applies.
    const bool engineSections = (graph != nullptr);

    while (r.ok && r.pos + 8 <= r.n) {
        char tag[4];
        for (int i = 0; i < 4; ++i) tag[i] = (char)r.u8();
        uint32_t len = r.u32();
        if (!r.ok || r.pos + len > r.n) { set_err("truncated section"); return false; }
        const size_t sectionStart = r.pos;
        const size_t sectionEnd   = r.pos + len;
        // Every section is parsed through a reader scoped to its OWN declared
        // payload, so a corrupt count inside one section can only fail -- it can
        // never wander into the next section and come back with plausible
        // garbage.
        Reader sec(data.data() + sectionStart, len);
        if (std::memcmp(tag, "END ", 4) == 0) break;

        if (std::memcmp(tag, "GLOB", 4) == 0) {
            StagedGlob g;
            g.themeMode = sec.u8();
            g.bpm       = sec.f64();
            g.looping   = sec.u8();
            g.leftTick  = sec.i64();
            g.rightTick = sec.i64();
            g.fieldsOk  = sec.ok;
            if (sec.pos + 8 <= sec.n) {          // v7+ virtual MIDI port counts
                g.midiIns  = sec.i32();
                g.midiOuts = sec.i32();
                g.hasVirtualPorts = true;
            }
            if (sec.pos + 8 <= sec.n) {          // v19+ AutoFades ms (ch.32)
                g.autoFadeMs = sec.f64();
                g.hasAutoFade = true;
            }
            order.push_back(std::make_pair((int)K_GLOB, globs.size()));
            globs.push_back(g);
        }
        else if (std::memcmp(tag, "SEQ ", 4) == 0) {
            StagedSeq sq;
            sq.slot      = sec.u32();
            sq.name      = sec.str();
            sq.midiBus   = sec.u8();
            sq.midiChan  = sec.u8();
            sq.length    = sec.i32();
            sq.seqBpm    = sec.i32();
            sq.bw        = sec.i32();
            sq.playing   = sec.u8();
            sq.isMaster  = sec.u8();
            sq.follows   = sec.u8();
            sq.mScale    = sec.i32();
            sq.mKey      = sec.i32();
            sq.songMute  = sec.u8();
            sq.fxBlob    = sec.str();
            sq.thru      = sec.u8();
            sq.trackKind = ver >= 11 ? sec.u8()  : 0;
            sq.laneId    = ver >= 11 ? sec.i32() : -1;
            sq.loopStart = ver >= 14 ? sec.i32() : 0;
            sq.loopEnd   = ver >= 15 ? sec.i32() : -1;   // -1 = "not saved, default to length"
            // Pre-v16 projects had no choice: patterns always repeated.
            sq.loopOn    = ver >= 16 ? sec.i32() : 1;
            const uint32_t nEvents = sec.u32();

            // An out-of-range slot is skipped exactly as before: its payload is
            // left unread and the section resync below steps over it.
            if (sq.slot < (uint32_t)c_max_sequence && sec.ok) {
                sq.events.reserve(nEvents < 65536u ? nEvents : 65536u);
                for (uint32_t e = 0; e < nEvents && sec.ok; ++e) {
                    RawEv ev;
                    ev.tick   = (long)sec.i32();
                    ev.status = sec.u8();
                    ev.d0     = sec.u8();
                    ev.d1     = sec.u8();
                    ev.column = -1;
                    if (hasColumns) {
                        const uint8_t c = sec.u8();
                        if (c != 0xFF) ev.column = (int)c;
                    }
                    if (sec.ok) sq.events.push_back(ev);
                }
                const uint32_t nTriggers = sec.u32();
                if (!sec.ok || nTriggers > 4096) { set_err("invalid sequence section"); return false; }
                for (uint32_t t = 0; t < nTriggers && sec.ok; ++t) {
                    RawTrigger tg;
                    tg.start  = (long)sec.i32();
                    tg.length = (long)sec.i32();
                    tg.offset = (long)sec.i32();
                    if (sec.ok) sq.triggers.push_back(tg);
                }
                order.push_back(std::make_pair((int)K_SEQ, seqs.size()));
                seqs.push_back(sq);
            }
        }
        else if (std::memcmp(tag, "TRAK", 4) == 0) {
            StagedTrak tk;
            tk.track    = sec.u32();
            tk.gain     = sec.f32();
            tk.pan      = sec.f32();
            tk.mute     = sec.u8();
            tk.solo     = sec.u8();
            tk.disabled = sec.u8();
            const uint8_t hasInst = sec.u8();
            if (hasInst) tk.instrument = get_plugin(sec);
            const uint32_t nfx = sec.u32();
            if (!sec.ok || nfx > (uint32_t)Track::kMaxFx) { set_err("invalid track section"); return false; }
            for (uint32_t i = 0; i < nfx; ++i) tk.fx.push_back(get_plugin(sec));
            order.push_back(std::make_pair((int)K_TRAK, traks.size()));
            traks.push_back(tk);
        }
        else if (std::memcmp(tag, "PTCH", 4) == 0) {
            if (engineSections) {
                Reader sub(data.data() + sectionStart, len);
                if (!restore_patch(sub, ver, hasRackSamples, /*dry=*/true) || !sub.ok) {
                    set_err("invalid modular patch section"); return false;
                }
                order.push_back(std::make_pair((int)K_PTCH, patches.size()));
                patches.push_back(StagedRange{sectionStart, len});
            }
        }
        else if (std::memcmp(tag, "AUDI", 4) == 0) {
            if (engineSections) {
                Reader sub(data.data() + sectionStart, len);
                if (!restore_audio(sub, ver, /*dry=*/true) || !sub.ok) {
                    set_err("invalid embedded audio section"); return false;
                }
                order.push_back(std::make_pair((int)K_AUDI, audios.size()));
                audios.push_back(StagedRange{sectionStart, len});
            }
        }
        else if (std::memcmp(tag, "PUI ", 4) == 0) {
            const uint32_t count = sec.u32();
            if (!sec.ok || count > 512) { set_err("invalid patch layout section"); return false; }
            std::vector<ProjectPatchNodePosition> layout;
            layout.reserve(count);
            for (uint32_t i = 0; i < count && sec.ok; ++i)
                layout.push_back(ProjectPatchNodePosition{sec.u32(), sec.f64(), sec.f64()});
            order.push_back(std::make_pair((int)K_PUI, layouts.size()));
            layouts.push_back(layout);
        }
        else if (std::memcmp(tag, "FRZ ", 4) == 0) {
            const uint32_t count = sec.u32();
            if (!sec.ok || count > 4096) { set_err("invalid freeze section"); return false; }
            std::vector<ProjectFreezeRecord> fz;
            fz.reserve(count);
            for (uint32_t i = 0; i < count && sec.ok; ++i) {
                ProjectFreezeRecord fr;
                fr.srcSeq     = sec.i32();
                fr.isTrack    = sec.u8();
                fr.newSeq     = sec.i32();
                fr.srcTrack   = sec.i32();
                fr.wasMuted   = sec.u8();
                fr.audioTrack = sec.i32();
                fz.push_back(fr);
            }
            order.push_back(std::make_pair((int)K_FRZ, freezeSets.size()));
            freezeSets.push_back(fz);
        }
        else if (std::memcmp(tag, "AUTO", 4) == 0) {
            // A null player is already "validate but apply nothing".
            Reader sub(data.data() + sectionStart, len);
            restore_automation(sub, nullptr, ver);
            if (!sub.ok) { set_err("invalid automation section"); return false; }
            order.push_back(std::make_pair((int)K_AUTO, autos.size()));
            autos.push_back(StagedRange{sectionStart, len});
        }
        // else: unknown section -> skip

        if (!sec.ok) { set_err("truncated section"); return false; }
        r.pos = sectionEnd;   // resync to declared section length
    }

    if (!r.ok) { set_err("parse error / truncated file"); return false; }

    // -----------------------------------------------------------------------
    //  PASS 2 -- COMMIT.  The file is known good, so the open session can now
    //  be replaced.  Everything below this line is pure application of state
    //  that has already been parsed and bounds-checked.
    // -----------------------------------------------------------------------
    if (autoPlayer) autoPlayer->clear();   // start from a clean slate; AUTO refills it

    g_loadedPatchLayout.clear();
    g_loadedFreezes.clear();
    g_loadedPatchNodeIds.clear();

    // wipe current project
    for (int i = 0; i < c_max_sequence; ++i)
        if (p.is_active(i)) p.delete_sequence(i);
    p.set_scale_master(-1);

    int scale_master_slot = -1;

    // Reset every track to a clean default first, so loading over a live
    // session doesn't leave stale instruments / mix state on tracks the file
    // doesn't mention (verify P1).
    if (graph) {
        for (int t = 0; t < graph->trackCount(); ++t)
            if (Track* trk = graph->track(t)) project_io_reset_track_defaults(*trk);
        audio_app_project_clear_audio_clips();
    }

    for (size_t si = 0; si < order.size(); ++si) {
        const int kind = order[si].first;
        const size_t ix = order[si].second;

        if (kind == K_GLOB) {
            const StagedGlob& g = globs[ix];
            ui::set_mode(g.themeMode ? ui::Mode::Midnight : ui::Mode::Light);
            if (g.bpm > 0) p.set_bpm(g.bpm);
            if (g.fieldsOk) {
                p.set_left_tick(migrate_tick((long)g.leftTick, ver));
                p.set_right_tick(migrate_tick((long)g.rightTick, ver));
                p.set_looping(g.looping != 0);
            }
            if (g.hasVirtualPorts)
                PatchKnob::app::audio_app_virtual_midi_set_ports(g.midiIns, g.midiOuts);
            if (g.hasAutoFade)
                PatchKnob::app::audio_app_set_auto_fade_ms(g.autoFadeMs);
        }
        else if (kind == K_SEQ) {
            const StagedSeq& sq = seqs[ix];
            if (p.is_active((int)sq.slot)) p.delete_sequence((int)sq.slot);
            p.new_sequence((int)sq.slot);
            sequence* s = p.get_sequence((int)sq.slot);
            if (!s) continue;
            s->set_name(sq.name);
            s->set_midi_bus((char)sq.midiBus);
            s->set_midi_channel(sq.midiChan);
            s->set_bpm(sq.seqBpm > 0 ? sq.seqBpm : 4);
            s->set_bw(sq.bw > 0 ? sq.bw : 4);
            s->set_length(sq.length > 0 ? migrate_tick((long)sq.length, ver)
                                        : (c_ppqn * 4), false);
            restore_loop_window(s, (long)sq.loopStart,
                                sq.loopEnd >= 0 ? (long)sq.loopEnd : s->get_length());
            s->set_loop_enabled(sq.loopOn != 0);
            s->set_master_scale(sq.mScale);
            s->set_master_key(sq.mKey);
            if (sq.follows)  s->set_follows_master(true);
            if (sq.isMaster) scale_master_slot = (int)sq.slot;

            for (size_t e = 0; e < sq.events.size(); ++e) {
                event ev;
                ev.set_timestamp((unsigned long)migrate_tick(sq.events[e].tick, ver));
                ev.set_status((char)sq.events[e].status);
                ev.set_data((char)sq.events[e].d0, (char)sq.events[e].d1);
                ev.set_column(sq.events[e].column);
                s->add_event(&ev);
            }
            for (size_t t = 0; t < sq.triggers.size(); ++t) {
                const long start  = migrate_tick(sq.triggers[t].start,  ver);
                const long length = migrate_tick(sq.triggers[t].length, ver);
                const long offset = migrate_tick(sq.triggers[t].offset, ver);
                if (length > 0) s->add_trigger(start, length, offset, false);
            }
            s->verify_and_link();
            s->set_playing(sq.playing != 0);
            s->set_thru(sq.thru != 0);
            s->set_track_kind((int)sq.trackKind);
            s->set_arrange_lane_id(sq.laneId);
            if (sq.songMute) s->set_song_mute(true);   // keep frozen source silent
            if (!sq.fxBlob.empty()) s->set_fx_blob(sq.fxBlob);  // per-pattern tracker FX
        }
        else if (kind == K_TRAK) {
            const StagedTrak& tk = traks[ix];
            if (!graph) continue;
            Track* trk = graph->track((int)tk.track);
            if (!trk) continue;
            if (tk.instrument.present) {
                if (audio_app_set_track_instrument((int)tk.track, tk.instrument.desc)) {
                    IPluginInstance* inst = trk->instrument();
                    if (inst && !tk.instrument.state.empty()) inst->loadState(tk.instrument.state);
                }
            }
            for (size_t i = 0; i < tk.fx.size(); ++i)
                if (tk.fx[i].present && audio_app_add_track_fx((int)tk.track, tk.fx[i].desc)) {
                    IPluginInstance* inst = trk->fxAt(trk->fxCount() - 1);
                    if (inst && !tk.fx[i].state.empty()) inst->loadState(tk.fx[i].state);
                }
            trk->setGain(tk.gain);
            trk->setPan(tk.pan);
            trk->setMute(tk.mute != 0);
            trk->setSolo(tk.solo != 0);
            trk->setDisabled(tk.disabled != 0);
        }
        else if (kind == K_PTCH) {
            Reader sub(data.data() + patches[ix].off, patches[ix].len);
            if (!restore_patch(sub, ver, hasRackSamples, /*dry=*/false)) {
                set_err("invalid modular patch section"); return false;
            }
        }
        else if (kind == K_AUDI) {
            Reader sub(data.data() + audios[ix].off, audios[ix].len);
            if (!restore_audio(sub, ver, /*dry=*/false)) {
                set_err("invalid embedded audio section"); return false;
            }
        }
        else if (kind == K_PUI) {   // append: a file may carry more than one
            g_loadedPatchLayout.insert(g_loadedPatchLayout.end(),
                                       layouts[ix].begin(), layouts[ix].end());
        }
        else if (kind == K_FRZ) {
            g_loadedFreezes.insert(g_loadedFreezes.end(),
                                   freezeSets[ix].begin(), freezeSets[ix].end());
        }
        else if (kind == K_AUTO) {
            Reader sub(data.data() + autos[ix].off, autos[ix].len);
            restore_automation(sub, autoPlayer, ver);   // nullptr player -> skip
        }
    }

    if (scale_master_slot >= 0)
        p.set_scale_master(scale_master_slot);

    for (size_t i = 0; i < g_loadedPatchLayout.size(); ++i) {
        std::map<uint32_t,uint32_t>::const_iterator id =
            g_loadedPatchNodeIds.find(g_loadedPatchLayout[i].nodeId);
        if (id != g_loadedPatchNodeIds.end()) g_loadedPatchLayout[i].nodeId = id->second;
    }

    return true;
}

bool project_io_parse_patch_section(const void* payload, size_t len, unsigned version) {
    if (!payload && len) return false;
    Reader r((const uint8_t*)payload, len);
    return restore_patch(r, (uint32_t)version, /*hasRackSamples=*/false, /*dry=*/true) && r.ok;
}

const char* project_io_last_error() { return g_last_error.c_str(); }

bool save_rack_patch(rackx::RackEngine& rack, const std::string& path) {
    Buf payload; put_rack(payload,&rack);
    Buf file; const char magic[8]={'P','K','R','A','C','K','0','1'};
    file.insert(file.end(),magic,magic+8); put_u32(file,7);   // v7: rack sample-slot source path
    file.insert(file.end(),payload.begin(),payload.end());
    // Was: truncate the destination in place and return true without ever
    // checking the write.  An interrupted or failed write destroyed the
    // previous .pkr and still reported success.  Same durable temp-file +
    // backup + rename install the project file gets.
    if (!install_file_atomically(path, file, "rack patch")) return false;
    set_err(""); return true;
}

bool load_rack_patch(rackx::RackEngine& rack, const std::string& path) {
    std::vector<uint8_t> file=read_file_blob(path);
    const char magic[8]={'P','K','R','A','C','K','0','1'};
    if(file.size()<12||std::memcmp(file.data(),magic,8)!=0){
        set_err("Not a PatchKnob rack patch: "+path);return false;
    }
    Reader r(file.data()+8,file.size()-8); const uint32_t ver=r.u32();
    if(ver!=6&&ver!=7){set_err("Unsupported rack patch version");return false;}
    // restore_rack() starts by clearing the target engine, so a damaged file
    // used to wipe the rack the user had open and then report failure.  Prove
    // the whole payload parses first (same parser, dry mode), then apply.
    {
        Reader probe(file.data()+8,file.size()-8); probe.u32();
        if(!restore_rack(probe,nullptr,ver>=7,/*dry=*/true)||!probe.ok){
            set_err("Rack patch is damaged or incomplete");return false;}
    }
    if(!restore_rack(r,&rack,ver>=7,/*dry=*/false)||!r.ok){set_err("Rack patch is damaged or incomplete");return false;}
    set_err(""); return true;
}
const std::vector<ProjectPatchNodePosition>& project_io_loaded_patch_layout()
{
    return g_loadedPatchLayout;
}
const std::vector<ProjectFreezeRecord>& project_io_loaded_freezes()
{
    return g_loadedFreezes;
}
