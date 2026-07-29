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
#include <cstring>
#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace patch = PatchKnob::engine::patch;

std::string g_last_error;
void set_err(const std::string& s) { g_last_error = s; }

const char  MAGIC[8] = { 'S','2','4','D','A','W','P','J' };
const uint32_t PROJ_VERSION = 5;   // v5: fractional tempo + loop/thru/track-disabled + safer audio
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
struct RawEv { long tick; uint8_t status, d0, d1; };
struct RawTrigger { long start, length, offset; };

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
    PATCH_MIDI_OUT, PATCH_RECORD, PATCH_CSOUND
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
    if (dynamic_cast<RecordNode*>(n))          return PATCH_RECORD;
    return 0;
}

std::vector<uint8_t> read_file_blob(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
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
        const int params = m && m->mod ? (int)m->mod->params.size() : 0;
        put_u32(b, (uint32_t)params);
        for (int p = 0; p < params; ++p) put_f32(b, m->mod->params[(size_t)p].value);
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

    std::vector<NodeId> ids = graph->nodeIds();
    std::vector<NodeId> saved;
    for (size_t i = 0; i < ids.size(); ++i)
        if ((int)ids[i] != audio_app_patch_out_node() &&
            (int)ids[i] != audio_app_patch_midi_in_node() &&
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
            PdNode* pd = static_cast<PdNode*>(n);
            put_str(out, pd->patchPath());
            put_blob(out, read_file_blob(pd->patchPath()));
        } else if (kind == PATCH_CSOUND) {
            put_str(out, static_cast<CsoundNode*>(n)->csdText());   // the .csd source
        } else if (kind == PATCH_RACK) {
            put_rack(out, static_cast<RackNode*>(n)->engine());
        } else if (kind == PATCH_SINE) {
            SineSourceNode* s = static_cast<SineSourceNode*>(n);
            put_f32(out, s->freq()); put_f32(out, s->amp());
        } else if (kind == PATCH_GAIN) {
            put_f32(out, static_cast<GainNode*>(n)->gain());
        } else if (kind == PATCH_RECORD) {
            put_u8(out, static_cast<RecordNode*>(n)->recording() ? 1 : 0);
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
            if (sc.clip && !sc.clip->empty()) entries.push_back(AudioEntry{t, sc});
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
        put_u64(out, (uint64_t)c->numFrames());
        for (int ch = 0; ch < 2; ++ch)
            for (int64_t f = 0; f < c->numFrames(); ++f) put_f32(out, c->ch[ch][(size_t)f]);
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
        put_i64(out, sc.fadeInFrames);
        put_i64(out, sc.fadeOutFrames);
        put_f32(out, sc.fadeInTension);
        put_f32(out, sc.fadeOutTension);
    }
}

bool write_pd_extract(const std::string& projectPath, uint32_t savedNode,
                      const std::vector<uint8_t>& source, std::string& outPath) {
    if (source.empty()) return false;
    outPath = projectPath + ".pdnode-" + std::to_string(savedNode) + ".pd";
    std::ofstream f(outPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)source.data(), (std::streamsize)source.size());
    return (bool)f;
}

bool restore_rack(Reader& r, rackx::RackEngine* rack) {
    const int poly = r.i32();
    const uint32_t modules = r.u32();
    if (!r.ok || modules > 1024) { r.ok = false; return false; }
    if (!rack) { r.ok = false; return false; }

    rack->clear();
    std::map<int,int> moduleMap;
    for (uint32_t i = 0; i < modules && r.ok; ++i) {
        const int oldId = r.i32();
        const std::string slug = r.str();
        const float x = r.f32(), y = r.f32();
        const uint32_t params = r.u32();
        if (!r.ok || params > 4096) { r.ok = false; return false; }
        const int id = rack->addModule(slug, x, y);
        if (id >= 0) moduleMap[oldId] = id;
        for (uint32_t p = 0; p < params; ++p) {
            const float value = r.f32();
            if (id >= 0) rack->setParam(id, (int)p, value);
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
    rack->setPolyphony(poly);
    rack->ensureDefaultIO();
    return r.ok;
}

bool restore_patch(Reader& r, const std::string& projectPath) {
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;

    const bool modular = r.u8() != 0;
    const bool multithreaded = r.u8() != 0;
    const uint32_t savedOut = r.u32();
    const uint32_t savedMidi = r.u32();
    const uint32_t savedMaster = r.u32();
    const uint32_t tracks = r.u32();
    const float masterGain = r.f32();
    if (!r.ok || tracks > 256) { r.ok = false; return false; }

    struct MasterTrack { bool midi; float gain, pan; bool mute; };
    std::vector<MasterTrack> masterTracks;
    masterTracks.reserve(tracks);
    for (uint32_t i = 0; i < tracks; ++i)
        masterTracks.push_back(MasterTrack{r.u8() != 0, r.f32(), r.f32(), r.u8() != 0});
    if (!r.ok) return false;

    g_loadedPatchNodeIds.clear();
    audio_app_project_reset_patch();
    const int outNode = audio_app_patch_out_node();
    const int midiNode = audio_app_patch_midi_in_node();
    const int masterNode = audio_app_master_mixer_node();
    std::map<uint32_t,int> nodeMap;
    nodeMap[savedOut] = outNode;
    nodeMap[savedMidi] = midiNode;
    nodeMap[savedMaster] = masterNode;

    for (size_t i = 0; i < masterTracks.size(); ++i) {
        int idx = audio_app_master_add_track(masterTracks[i].midi ? 1 : 0);
        if (idx >= 0) {
            audio_app_mixer_set_gain(masterNode, idx, masterTracks[i].gain);
            audio_app_mixer_set_pan(masterNode, idx, masterTracks[i].pan);
            audio_app_mixer_set_mute(masterNode, idx, masterTracks[i].mute);
        }
    }
    audio_app_mixer_set_master_gain(masterNode, masterGain);

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
            node = stored_plugin_is_sampler(plugin) ? audio_app_patch_add_sampler()
                 : plugin.present ? audio_app_patch_add_plugin(plugin.desc)
                                  : audio_app_patch_add_empty_plugin();
            if (node < 0 && plugin.present)
                node = audio_app_patch_add_empty_plugin();
            if (node >= 0 && plugin.present) {
                IPluginInstance* inst = audio_app_patch_node_instance(node);
                if (inst && !plugin.state.empty()) inst->loadState(plugin.state);
            }
        } else if (kind == PATCH_MIXER) {
            const int channels = r.i32(); const float gain = r.f32();
            if (channels < 1 || channels > 256) { r.ok = false; return false; }
            node = audio_app_patch_add_mixer(channels);
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
            const std::string oldPath = r.str();
            const std::vector<uint8_t> source = r.blob();
            node = audio_app_patch_add_pd();
            if (node >= 0) {
                std::string extracted;
                if (write_pd_extract(projectPath, oldId, source, extracted))
                    audio_app_pd_load(node, extracted.c_str());
                else if (!oldPath.empty()) audio_app_pd_load(node, oldPath.c_str());
            }
        } else if (kind == PATCH_CSOUND) {
            const std::string csd = r.str();
            node = audio_app_patch_add_csound();
            if (node >= 0) {
                audio_app_patch_csound_set_text(node, csd.c_str());
                audio_app_patch_csound_recompile(node);   // sets nchnls/nchnls_i ports before connections restore
            }
        } else if (kind == PATCH_RACK) {
            node = audio_app_patch_add_rack();
            if (!restore_rack(r, audio_app_rack_engine(node))) return false;
        } else if (kind == PATCH_SINE) {
            const float freq = r.f32(), amp = r.f32();
            node = audio_app_patch_add_builtin("sine");
            if (auto* n = dynamic_cast<patch::SineSourceNode*>(audio_app_patch_graph()->node((patch::NodeId)node))) {
                n->setFreq(freq); n->setAmp(amp);
            }
        } else if (kind == PATCH_GAIN) {
            const float gain = r.f32();
            node = audio_app_patch_add_builtin("gain");
            if (auto* n = dynamic_cast<patch::GainNode*>(audio_app_patch_graph()->node((patch::NodeId)node))) n->setGain(gain);
        } else if (kind == PATCH_SUM) {
            node = audio_app_patch_add_builtin("sum");
        } else if (kind == PATCH_AUDIO_OUT) {
            node = audio_app_patch_add_builtin("out");
        } else if (kind == PATCH_AUDIO_IN) {
            node = audio_app_patch_add_builtin("in");
        } else if (kind == PATCH_MIDI_IN) {
            node = audio_app_patch_add_midi_in(-1);
        } else if (kind == PATCH_MIDI_OUT) {
            node = audio_app_patch_add_midi_out(-1);
        } else if (kind == PATCH_RECORD) {
            const bool armed = r.u8() != 0;
            node = audio_app_patch_add_record();
            if (node >= 0) audio_app_record_set(node, armed);
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
    for (std::map<uint32_t,int>::const_iterator it = nodeMap.begin(); it != nodeMap.end(); ++it)
        g_loadedPatchNodeIds[it->first] = (uint32_t)it->second;
    audio_app_set_multithreaded(multithreaded);
    audio_app_set_modular(modular);
    return r.ok;
}

// Read one embedded AudioClip (name + rates + interleaved-by-channel samples).
bool read_audio_clip(Reader& r, PatchKnob::engine::AudioClip& clip) {
    clip.name = r.str(); clip.sampleRate = r.f64(); clip.sourceSampleRate = r.f64();
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
    clip.resize((int64_t)frames);
    for (int ch = 0; ch < 2; ++ch)
        for (uint64_t f = 0; f < frames; ++f) clip.ch[ch][(size_t)f] = r.f32();
    return r.ok;
}

bool restore_audio(Reader& r, uint32_t ver) {
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    audio_app_project_clear_audio_clips();

    if (ver < 4) {
        // Legacy: one full source buffer per entry, start + gain only (no region).
        const uint32_t count = r.u32();
        if (!r.ok || count > 256) { r.ok = false; return false; }
        for (uint32_t i = 0; i < count && r.ok; ++i) {
            const int track = r.i32();
            const int64_t start = r.i64();
            const float gain = r.f32();
            AudioClip clip;
            if (!read_audio_clip(r, clip)) return false;
            if (r.ok) audio_app_project_add_audio_clip(track, clip, start, gain);
        }
        return r.ok;
    }

    // v4: deduped source table + region entries.
    const uint32_t nsrc = r.u32();
    if (!r.ok || nsrc > 4096) { r.ok = false; return false; }
    std::vector<AudioClip> sources((size_t)nsrc);
    for (uint32_t i = 0; i < nsrc && r.ok; ++i)
        if (!read_audio_clip(r, sources[i])) return false;
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
        const int64_t  fIn   = r.i64();
        const int64_t  fOut  = r.i64();
        const float    fInK  = r.f32();
        const float    fOutK = r.f32();
        if (!r.ok || sidx >= nsrc) { r.ok = false; return false; }
        audio_app_project_add_audio_region(track, sources[sidx], start, soff, len,
                                           gain, muted, loop, fIn, fOut, fInK, fOutK);
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
    const int nt = ap ? ap->trackCount() : 0;
    put_u32(out, (uint32_t)nt);
    for (int t = 0; t < nt; ++t) {
        const AutomationTrack& at = ap->track(t);
        put_u32(out, (uint32_t)at.laneCount());
        for (int li = 0; li < at.laneCount(); ++li) {
            const AutomationLane& ln = at.lane(li);
            put_u8 (out, (uint8_t)(ln.target().kind == LaneTargetKind::MidiCC ? 1 : 0));
            put_u32(out, (uint32_t)ln.target().id);
            put_u8 (out, (uint8_t)ln.interpolation());        // 0 Linear,1 Step,2 Hold
            const std::vector<Breakpoint>& bps = ln.breakpoints();
            put_u32(out, (uint32_t)bps.size());
            for (const Breakpoint& b : bps) { put_i64(out, b.tick); put_f32(out, b.value); }
        }
    }
}
void restore_automation(Reader& r, PatchKnob::engine::AutomationPlayer* ap) {
    using namespace PatchKnob::engine;
    const uint32_t nt = r.u32();
    for (uint32_t t = 0; t < nt && r.ok; ++t) {
        const uint32_t nl = r.u32();
        for (uint32_t li = 0; li < nl && r.ok; ++li) {
            const uint8_t kind = r.u8();
            const uint32_t id  = r.u32();
            const uint8_t interp = r.u8();
            const uint32_t nb  = r.u32();
            if (!r.ok || nb > 1000000u) { r.ok = false; return; }
            LaneTarget tgt{ kind ? LaneTargetKind::MidiCC : LaneTargetKind::VstParam, id };
            Interpolation ip = interp == 1 ? Interpolation::Step
                             : interp == 2 ? Interpolation::Hold : Interpolation::Linear;
            if (ap && (int)t < ap->trackCount()) {
                AutomationLane& ln = ap->track((int)t).addLane(tgt, ip);
                for (uint32_t bi = 0; bi < nb && r.ok; ++bi) {
                    int64_t tick = r.i64(); float val = r.f32();
                    ln.add(tick, val);
                }
            } else {
                for (uint32_t bi = 0; bi < nb && r.ok; ++bi) { r.i64(); r.f32(); }  // skip
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

        std::vector<RawEv> evs;
        collect_events(s, evs);
        put_u32(sq, (uint32_t)evs.size());
        for (size_t e = 0; e < evs.size(); ++e) {
            put_i32(sq, (int32_t)evs[e].tick);
            put_u8 (sq, evs[e].status);
            put_u8 (sq, evs[e].d0);
            put_u8 (sq, evs[e].d1);
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

    // --- TRAK (only when the live audio graph exists) -----------------------
    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;
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
    }

    // --- PTCH / AUDI -------------------------------------------------------
    // The patch graph owns the modular instruments, Pd and Rack configurations;
    // audio is stored as raw stereo samples, not a path to an external WAV.
    if (audio_app_running()) {
        Buf patch; put_patch(patch); put_section(out, "PTCH", patch);
        Buf audio; put_audio(audio); put_section(out, "AUDI", audio);
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

    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) { set_err("cannot open '" + path + "' for writing"); return false; }
    f.write((const char*)out.data(), (std::streamsize)out.size());
    if (!f) { set_err("write failed for '" + path + "'"); return false; }
    return true;
}

// ===========================================================================
//  LOAD
// ===========================================================================
bool load_project(perform& p, const std::string& path,
                  PatchKnob::engine::AutomationPlayer* autoPlayer)
{
    g_last_error.clear();
    if (autoPlayer) autoPlayer->clear();   // start from a clean slate; AUTO refills it

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
    if (ver < 1 || ver > PROJ_VERSION) { set_err("unsupported project version"); return false; }

    g_loadedPatchLayout.clear();
    g_loadedFreezes.clear();
    g_loadedPatchNodeIds.clear();

    // wipe current project
    for (int i = 0; i < c_max_sequence; ++i)
        if (p.is_active(i)) p.delete_sequence(i);
    p.set_scale_master(-1);

    int scale_master_slot = -1;

    using namespace PatchKnob::app;
    using namespace PatchKnob::engine;
    MixerGraph* graph = audio_app_running() ? audio_app_graph() : nullptr;

    // Reset every track to a clean default first, so loading over a live
    // session doesn't leave stale instruments / mix state on tracks the file
    // doesn't mention (verify P1).
    if (graph) {
        for (int t = 0; t < graph->trackCount(); ++t) {
            if (Track* trk = graph->track(t)) {
                trk->setInstrument(nullptr);
                while (trk->fxCount() > 0) trk->removeFx(trk->fxCount() - 1);
                trk->setGain(1.0f); trk->setPan(0.0f);
                trk->setMute(false); trk->setSolo(false);
            }
        }
        audio_app_project_clear_audio_clips();
    }

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
            double bpm = 0.0;
            uint8_t looping = 0;
            int64_t leftTick = 0, rightTick = c_ppqn * 16;
            if (ver >= 5) {
                bpm = r.f64();
                looping = r.u8();
                leftTick = r.i64();
                rightTick = r.i64();
            } else {
                bpm = (double)r.i32();
            }
            ui::set_mode(themeMode ? ui::Mode::Midnight : ui::Mode::Light);
            if (bpm > 0) p.set_bpm(bpm);
            if (ver >= 5 && r.ok) {
                p.set_left_tick((long)leftTick);
                p.set_right_tick((long)rightTick);
                p.set_looping(looping != 0);
            }
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
            uint8_t songMute = (ver >= 4) ? r.u8() : 0;    // v4: freeze/song mute
            std::string fxBlob = (ver >= 4) ? r.str() : std::string();  // v4: tracker FX
            uint8_t thru = (ver >= 5) ? r.u8() : 0;
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
                    if (ver >= 3) {
                        const uint32_t nTriggers = r.u32();
                        if (!r.ok || nTriggers > 4096) { r.ok = false; break; }
                        for (uint32_t t = 0; t < nTriggers && r.ok; ++t) {
                            const int32_t start = r.i32();
                            const int32_t length = r.i32();
                            const int32_t offset = r.i32();
                            if (length > 0) s->add_trigger(start, length, offset, false);
                        }
                    }
                    s->verify_and_link();
                    s->set_playing(playing != 0);
                    s->set_thru(thru != 0);
                    if (songMute) s->set_song_mute(true);   // keep frozen source silent
                    if (!fxBlob.empty()) s->set_fx_blob(fxBlob);   // per-pattern tracker FX
                }
            }
        }
        else if (std::memcmp(tag, "TRAK", 4) == 0) {
            uint32_t track = r.u32();
            float gain     = r.f32();
            float pan      = r.f32();
            uint8_t mute   = r.u8();
            uint8_t solo   = r.u8();
            uint8_t disabled = (ver >= 5) ? r.u8() : 0;
            uint8_t hasInst= r.u8();

            StoredPlugin instrument;
            std::vector<StoredPlugin> fx;
            if (hasInst) {
                if (ver == 1) {
                    instrument.present = true;
                    instrument.desc.format = r.u8() ? PluginFormat::VST3 : PluginFormat::VST2;
                    instrument.desc.path = r.str();
                    instrument.desc.uid = r.str();
                    instrument.desc.name = instrument.desc.path;
                    instrument.desc.isInstrument = true;
                    instrument.desc.numAudioIn = 0;
                    instrument.desc.numAudioOut = 2;
                    instrument.state = r.blob();
                } else {
                    instrument = get_plugin(r);
                }
            }
            if (ver >= 2) {
                uint32_t nfx = r.u32();
                if (nfx > (uint32_t)Track::kMaxFx) { r.ok = false; break; }
                for (uint32_t i = 0; i < nfx; ++i) fx.push_back(get_plugin(r));
            }

            if (graph && r.ok) {
                Track* trk = graph->track((int)track);
                if (trk) {
                    if (instrument.present) {
                        if (audio_app_set_track_instrument((int)track, instrument.desc)) {
                            IPluginInstance* inst = trk->instrument();
                            if (inst && !instrument.state.empty()) inst->loadState(instrument.state);
                        }
                    }
                    for (size_t i = 0; i < fx.size(); ++i)
                        if (fx[i].present && audio_app_add_track_fx((int)track, fx[i].desc)) {
                            IPluginInstance* inst = trk->fxAt(trk->fxCount() - 1);
                            if (inst && !fx[i].state.empty()) inst->loadState(fx[i].state);
                        }
                    trk->setGain(gain);
                    trk->setPan(pan);
                    trk->setMute(mute != 0);
                    trk->setSolo(solo != 0);
                    trk->setDisabled(disabled != 0);
                }
            }
        }
        else if (std::memcmp(tag, "PTCH", 4) == 0) {
            if (graph && !restore_patch(r, path)) {
                set_err("invalid modular patch section"); return false;
            }
        }
        else if (std::memcmp(tag, "AUDI", 4) == 0) {
            if (graph && !restore_audio(r, ver)) {
                set_err("invalid embedded audio section"); return false;
            }
        }
        else if (std::memcmp(tag, "PUI ", 4) == 0) {
            const uint32_t count = r.u32();
            if (!r.ok || count > 512) { set_err("invalid patch layout section"); return false; }
            g_loadedPatchLayout.reserve(count);
            for (uint32_t i = 0; i < count && r.ok; ++i)
                g_loadedPatchLayout.push_back(ProjectPatchNodePosition{r.u32(), r.f64(), r.f64()});
        }
        else if (std::memcmp(tag, "FRZ ", 4) == 0) {
            const uint32_t count = r.u32();
            if (!r.ok || count > 4096) { set_err("invalid freeze section"); return false; }
            g_loadedFreezes.reserve(count);
            for (uint32_t i = 0; i < count && r.ok; ++i) {
                ProjectFreezeRecord fr;
                fr.srcSeq     = r.i32();
                fr.isTrack    = r.u8();
                fr.newSeq     = r.i32();
                fr.srcTrack   = r.i32();
                fr.wasMuted   = r.u8();
                fr.audioTrack = r.i32();
                g_loadedFreezes.push_back(fr);
            }
        }
        else if (std::memcmp(tag, "AUTO", 4) == 0) {
            restore_automation(r, autoPlayer);   // populates the passed player (nullptr -> skip)
        }
        // else: unknown section -> skip

        r.pos = sectionEnd;   // resync to declared section length
    }

    if (scale_master_slot >= 0)
        p.set_scale_master(scale_master_slot);

    for (size_t i = 0; i < g_loadedPatchLayout.size(); ++i) {
        std::map<uint32_t,uint32_t>::const_iterator id =
            g_loadedPatchNodeIds.find(g_loadedPatchLayout[i].nodeId);
        if (id != g_loadedPatchNodeIds.end()) g_loadedPatchLayout[i].nodeId = id->second;
    }

    if (!r.ok) { set_err("parse error / truncated file"); return false; }
    return true;
}

const char* project_io_last_error() { return g_last_error.c_str(); }
const std::vector<ProjectPatchNodePosition>& project_io_loaded_patch_layout()
{
    return g_loadedPatchLayout;
}
const std::vector<ProjectFreezeRecord>& project_io_loaded_freezes()
{
    return g_loadedFreezes;
}
