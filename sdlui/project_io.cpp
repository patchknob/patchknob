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

#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string g_last_error;
void set_err(const std::string& s) { g_last_error = s; }

const char  MAGIC[8] = { 'S','2','4','D','A','W','P','J' };
const uint32_t PROJ_VERSION = 1;

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
void put_f32(Buf& b, float v) {
    uint32_t u; std::memcpy(&u, &v, 4); put_u32(b, u);
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
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
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
//  seq24 has no single "give me all events" accessor, so we use the same two
//  pass idiom the GTK editors use:
//    pass 1  reset_draw_marker() + get_next_event(&status,&cc) to collect the
//            set of distinct event KINDS present.  For non-CC events a "kind" is
//            just the status (cc collapsed to 0) because get_next_event(status,
//            cc, ...) ignores cc for those; for CC events the kind is
//            (status, controller) so each controller is captured separately.
//    pass 2  for each kind, reset + get_next_event(status,cc,...) to pull every
//            matching event's (tick,status,d0,d1).
// ---------------------------------------------------------------------------
struct RawEv { long tick; uint8_t status, d0, d1; };

void collect_events(sequence* s, std::vector<RawEv>& out) {
    std::set<std::pair<uint8_t,uint8_t>> kinds;

    s->reset_draw_marker();
    unsigned char st, cc;
    while (s->get_next_event(&st, &cc)) {
        uint8_t key_cc = ((st & 0xF0) == EVENT_CONTROL_CHANGE) ? (uint8_t)cc : 0;
        kinds.insert(std::make_pair((uint8_t)st, key_cc));
    }

    for (std::set<std::pair<uint8_t,uint8_t>>::iterator it = kinds.begin();
         it != kinds.end(); ++it) {
        s->reset_draw_marker();
        long tick; unsigned char d0, d1; bool sel;
        while (s->get_next_event(it->first, it->second, &tick, &d0, &d1, &sel)) {
            RawEv e; e.tick = tick; e.status = it->first; e.d0 = d0; e.d1 = d1;
            out.push_back(e);
        }
    }
}

} // anonymous namespace

// ===========================================================================
//  SAVE
// ===========================================================================
bool save_project(perform& p, const std::string& path)
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
        put_i32(g, (int32_t)p.get_bpm());
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

        std::vector<RawEv> evs;
        collect_events(s, evs);
        put_u32(sq, (uint32_t)evs.size());
        for (size_t e = 0; e < evs.size(); ++e) {
            put_i32(sq, (int32_t)evs[e].tick);
            put_u8 (sq, evs[e].status);
            put_u8 (sq, evs[e].d0);
            put_u8 (sq, evs[e].d1);
        }
        put_section(out, "SEQ ", sq);
    }

    // --- TRAK (only when the live audio graph exists) -----------------------
    using namespace seq24::app;
    using namespace seq24::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;
    if (graph) {
        int nt = graph->trackCount();
        for (int t = 0; t < nt; ++t) {
            Track* trk = graph->track(t);
            if (!trk) continue;
            IPluginInstance* inst = trk->instrument();

            bool hasInst = (inst != nullptr);
            bool dfltMix = (trk->gain() == 1.0f && trk->pan() == 0.0f &&
                            !trk->mute() && !trk->solo());
            if (!hasInst && dfltMix) continue;   // nothing worth persisting

            Buf tk;
            put_u32(tk, (uint32_t)t);
            put_f32(tk, trk->gain());
            put_f32(tk, trk->pan());
            put_u8 (tk, (uint8_t)(trk->mute() ? 1 : 0));
            put_u8 (tk, (uint8_t)(trk->solo() ? 1 : 0));
            put_u8 (tk, (uint8_t)(hasInst ? 1 : 0));
            if (hasInst) {
                const PluginDescriptor& d = inst->descriptor();
                put_u8 (tk, (uint8_t)(d.format == PluginFormat::VST3 ? 1 : 0));
                put_str(tk, d.path);
                put_str(tk, d.uid);
                put_blob(tk, inst->saveState());
            }
            put_section(out, "TRAK", tk);
        }
    }

    // --- END ----------------------------------------------------------------
    { Buf e; put_section(out, "END ", e); }

    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) { set_err("cannot open '" + path + "' for writing"); return false; }
    f.write((const char*)out.data(), (std::streamsize)out.size());
    if (!f) { set_err("write failed for '" + path + "'"); return false; }
    return true;
}

// ===========================================================================
//  LOAD
// ===========================================================================
bool load_project(perform& p, const std::string& path)
{
    g_last_error.clear();

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
    if (ver != PROJ_VERSION) { set_err("unsupported project version"); return false; }

    // wipe current project
    for (int i = 0; i < c_max_sequence; ++i)
        if (p.is_active(i)) p.delete_sequence(i);
    p.set_scale_master(-1);

    int scale_master_slot = -1;

    using namespace seq24::app;
    using namespace seq24::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;

    // walk sections
    while (r.ok && r.pos + 8 <= r.n) {
        char tag[4];
        for (int i = 0; i < 4; ++i) tag[i] = (char)r.u8();
        uint32_t len = r.u32();
        if (!r.ok || r.pos + len > r.n) { set_err("truncated section"); return false; }
        size_t sectionEnd = r.pos + len;

        if (std::memcmp(tag, "END ", 4) == 0) break;

        if (std::memcmp(tag, "GLOB", 4) == 0) {
            uint8_t themeMode = r.u8();
            int32_t bpm       = r.i32();
            ui::set_mode(themeMode ? ui::Mode::Midnight : ui::Mode::Light);
            if (bpm > 0) p.set_bpm(bpm);
        }
        else if (std::memcmp(tag, "SEQ ", 4) == 0) {
            uint32_t slot = r.u32();
            std::string name = r.str();
            uint8_t midiBus  = r.u8();
            uint8_t midiChan = r.u8();
            int32_t length   = r.i32();
            int32_t bpm      = r.i32();
            int32_t bw       = r.i32();
            uint8_t playing  = r.u8();
            uint8_t isMaster = r.u8();
            uint8_t follows  = r.u8();
            int32_t mScale   = r.i32();
            int32_t mKey     = r.i32();
            uint32_t nEvents = r.u32();

            if (slot < (uint32_t)c_max_sequence && r.ok) {
                if (p.is_active((int)slot)) p.delete_sequence((int)slot);
                p.new_sequence((int)slot);
                sequence* s = p.get_sequence((int)slot);
                if (s) {
                    s->set_name(name);
                    s->set_midi_bus((char)midiBus);
                    s->set_midi_channel(midiChan);
                    s->set_bpm(bpm > 0 ? bpm : 4);
                    s->set_bw(bw > 0 ? bw : 4);
                    s->set_length(length > 0 ? length : (c_ppqn * 4), false);
                    s->set_master_scale(mScale);
                    s->set_master_key(mKey);
                    if (follows) s->set_follows_master(true);
                    if (isMaster) scale_master_slot = (int)slot;

                    for (uint32_t e = 0; e < nEvents && r.ok; ++e) {
                        int32_t tick   = r.i32();
                        uint8_t status = r.u8();
                        uint8_t d0     = r.u8();
                        uint8_t d1     = r.u8();
                        event ev;
                        ev.set_timestamp((unsigned long)tick);
                        ev.set_status((char)status);
                        ev.set_data((char)d0, (char)d1);
                        s->add_event(&ev);
                    }
                    s->verify_and_link();
                    s->set_playing(playing != 0);
                }
            }
        }
        else if (std::memcmp(tag, "TRAK", 4) == 0) {
            uint32_t track = r.u32();
            float gain     = r.f32();
            float pan      = r.f32();
            uint8_t mute   = r.u8();
            uint8_t solo   = r.u8();
            uint8_t hasInst= r.u8();

            uint8_t fmt = 0; std::string ppath, uid; std::vector<uint8_t> state;
            if (hasInst) {
                fmt   = r.u8();
                ppath = r.str();
                uid   = r.str();
                state = r.blob();
            }

            if (graph && r.ok) {
                Track* trk = graph->track((int)track);
                if (trk) {
                    if (hasInst) {
                        PluginDescriptor d;
                        d.format       = fmt ? PluginFormat::VST3 : PluginFormat::VST2;
                        d.path         = ppath;
                        d.uid          = uid;
                        d.name         = ppath;
                        d.isInstrument = true;
                        d.numAudioIn   = 0;
                        d.numAudioOut  = 2;
                        if (audio_app_set_track_instrument((int)track, d)) {
                            IPluginInstance* inst = trk->instrument();
                            if (inst && !state.empty()) inst->loadState(state);
                        }
                    }
                    trk->setGain(gain);
                    trk->setPan(pan);
                    trk->setMute(mute != 0);
                    trk->setSolo(solo != 0);
                }
            }
        }
        // else: unknown section -> skip

        r.pos = sectionEnd;   // resync to declared section length
    }

    if (scale_master_slot >= 0)
        p.set_scale_master(scale_master_slot);

    if (!r.ok) { set_err("parse error / truncated file"); return false; }
    return true;
}

const char* project_io_last_error() { return g_last_error.c_str(); }
