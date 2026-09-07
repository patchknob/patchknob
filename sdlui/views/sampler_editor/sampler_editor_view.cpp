//----------------------------------------------------------------------------
//  sdlui/views/sampler_editor/sampler_editor_view.cpp -- see the header.
//----------------------------------------------------------------------------
#include "sampler_editor_view.h"
#include "engine/plugin_api.h"
#include "engine/sample_slot.h"
#include "audio_app.h"                 // one-shot preview audition (space / click)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ui {

using PatchKnob::engine::AudioClip;
using PatchKnob::engine::IPluginInstance;

namespace {
const char* kNoteNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
void note_name(int midi, char* out, int cap) {
    if (midi < 0) midi = 0; if (midi > 127) midi = 127;
    std::snprintf(out, cap, "%s%d", kNoteNames[midi % 12], midi / 12 - 1);
}
bool in_rect(const SDL_Rect& r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
// Two extra tabs after the 5 instrument-global envelopes: the SELECTED ZONE's
// amp and mod envelope overrides (six-stage DAHDSR, engine-side per zone).
enum { ZENV_AMP = ENV_COUNT, ZENV_MOD, ENV_TABS };
const char* kEnvNames[ENV_TABS] = { "AMP", "PIT", "CUT", "RES", "PAN", "Z-AMP", "Z-MOD" };
const char* kZEnvStage[6] = { "DELAY", "ATTACK", "HOLD", "DECAY", "SUSTAIN", "RELEASE" };

// Per-zone inspector fields.  ONE table drives layout, hit test and mapping.
enum {
    ZF_PAN = 0, ZF_ATT, ZF_CUT, ZF_RES, ZF_COARSE, ZF_FINE, ZF_SCALE, ZF_EXCL,
    ZF_MODPITCH, ZF_MODCUT, ZF_LSTART, ZF_LEND, ZF_COUNT
};
const char* kZFieldName[ZF_COUNT] = {
    "PAN", "ATTEN", "CUTOFF", "RESO", "COARSE", "FINE", "KEYTRACK", "CHOKE",
    "MOD>PITCH", "MOD>CUTOFF", "LOOP START", "LOOP END"
};

// Stage slider <-> seconds.  Quadratic taper: fine control on short times,
// still reaches 20 s at the right edge.
// Envelope stage times span five orders of magnitude in real material: a 1 ms
// percussive attack and a ~100 s piano decay are both ordinary.  A quadratic
// taper over 0..20 s could represent neither end -- it clamped anything longer
// than 20 s (so an imported soundfont decay was truncated the moment the editor
// pushed a stage back) and gave sub-millisecond resolution no room at all.  A
// log taper over 1 ms .. 120 s covers SoundFont's full legal range (8000
// timecents = 101.6 s) with usable resolution everywhere.
constexpr float kZEnvMinSec = 0.001f;
constexpr float kZEnvMaxSec = 120.0f;
float zenv_slider_to_secs(float v) {
    v = std::max(0.f, std::min(1.f, v));
    if (v <= 0.f) return 0.f;
    return kZEnvMinSec * std::pow(kZEnvMaxSec / kZEnvMinSec, v);
}
float zenv_secs_to_slider(float s) {
    if (s <= kZEnvMinSec) return 0.f;
    const float v = std::log(std::min(s, kZEnvMaxSec) / kZEnvMinSec) /
                    std::log(kZEnvMaxSec / kZEnvMinSec);
    return std::max(0.f, std::min(1.f, v));
}
void  zenv_fmt_secs(float s, char* out, int cap) {
    if (s < 0.9995f) std::snprintf(out, cap, "%.0fms", s * 1000.f);
    else             std::snprintf(out, cap, "%.2fs", s);
}
bool is_wav_path(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return e == ".wav";
}

//! Extension gate first; the RIFF signature check only runs on plausible names.
bool is_soundfont_path(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return e == ".sf2" || e == ".sf3";
}

void clamp_zone_keys(SamplerZone& z) {
    z.loKey = std::max(0, std::min(127, z.loKey));
    z.hiKey = std::max(0, std::min(127, z.hiKey));
    if (z.loKey > z.hiKey) z.loKey = z.hiKey;
    z.root = std::max(0, std::min(127, z.root));
}

// Tension curve: t,0..1 -> shaped 0..1.  c=0 linear; c>0 convex (fast rise);
// c<0 concave (slow rise).  Matches the clip-fade tension feel.
float env_shape(float t, float c) {
    if (t <= 0.f) return 0.f; if (t >= 1.f) return 1.f;
    if (c > -0.02f && c < 0.02f) return t;
    return std::pow(t, std::pow(2.0f, -c * 3.0f));
}

// Measured truncation.  `text.size() * cw` is a BYTE count times a NOMINAL
// advance: the two stop agreeing the moment the UI scale is not 1.0, so every
// box sized that way clips its own label.  Same helper as arrange_view.cpp.
std::string fit_text(const Font& f, std::string s, int maxw) {
    if (maxw <= 0) return std::string();
    if (f.text_w(s) <= maxw) return s;
    const std::string ell = "...";
    if (f.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && f.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!s.empty() && f.text_w(s + ell) > maxw) s.pop_back();
    return s.empty() ? ell : s + ell;
}

// This codebase draws OPAQUE by default; a leaked blend mode tints everything
// drawn afterwards, so translucency is always scoped.
struct BlendScope {
    SDL_Renderer* r;
    explicit BlendScope(SDL_Renderer* ren) : r(ren) {
        if (r) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    }
    ~BlendScope() { if (r) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE); }
    BlendScope(const BlendScope&) = delete;
    BlendScope& operator=(const BlendScope&) = delete;
};
inline Color fade(Color c, int a) { c.a = (Uint8)std::max(0, std::min(255, a)); return c; }

// Menu command ids (see SamplerEditorView::run_menu).
enum {
    MZ_RENAME = 1, MZ_DUPLICATE, MZ_DELETE, MZ_EDITWAV, MZ_ROOT_HERE, MZ_ONE_KEY,
    MZ_SPREAD_SEL, MZ_LAYER_SEL, MZ_FILL_SEL, MZ_SELECT_ALL, MZ_SELECT_NONE,
    MZ_LOCK, MZ_MOVE_ROOT, MZ_SOLO, MZ_OVERLAPS, MZ_UNDO, MZ_REDO,
    MB_LOAD_SEL, MB_ADD_ZONE, MB_AUDITION, MB_UP, MB_REFRESH
};

// ---- pointer shapes --------------------------------------------------------
//  The cursor says what the press will do before you commit to it (same shape
//  the arrange view already establishes).
SDL_Cursor* g_arrow = nullptr;
SDL_Cursor* g_sizewe = nullptr;
SDL_Cursor* g_sizens = nullptr;
int         g_active = 0;
void want_cursor(int want) {
    if (!g_arrow) {
        g_arrow  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        g_sizewe = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
        g_sizens = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
    }
    if (want == g_active) return;
    SDL_Cursor* c = want == 1 ? g_sizewe : (want == 2 ? g_sizens : g_arrow);
    if (c) { SDL_SetCursor(c); g_active = want; }
}
} // namespace

void SamplerEditorView::bind(int node, std::vector<SamplerZone>* zones, SamplerEnvSet* envs, IPluginInstance* inst) {
    m_node = node; m_zones = zones; m_envs = envs; m_inst = inst;
    m_sel = 0; m_cur_env = 0; m_drag = Drag::None; m_param_drag = -1; m_env_node = -1; m_env_seg = -1;
    m_insp_drag = -1; m_zenv_drag = -1;
    m_leftDown = m_rightDown = false; m_scroll_drag = false;
    m_drag_path.clear(); m_drag_name.clear();
    m_drag_armed = false; m_drag_target = DropTarget{};
    m_drop_status.clear();
    m_browser_scroll = 0; m_zone_scroll = 0;
    m_selected.clear(); if (zones && !zones->empty()) m_selected.insert(0);
    m_zone_undo.clear(); m_zone_redo.clear(); m_zone_clipboard.clear();
    if (m_browser_dir.empty()) {
        try { m_browser_dir = std::filesystem::current_path().string(); }
        catch (...) { m_browser_dir = "."; }
        scan_browser();
    }
}

//----------------------------------------------------------------------------
//  per-zone inspector: normalized value <-> zone field, in ONE place, so the
//  slider you drag, the number you read and the default you right-click back
//  to can never disagree.
//----------------------------------------------------------------------------
float SamplerEditorView::insp_get(const SamplerZone& z, int field) const {
    auto clamp01 = [](float v){ return std::max(0.f, std::min(1.f, v)); };
    switch (field) {
    case ZF_PAN:      return clamp01((std::max(-1.f, std::min(1.f, z.pan)) + 1.f) * 0.5f);
    case ZF_ATT:      return clamp01(z.attenuationDb / 96.f);
    case ZF_CUT:      // 0 = filter off at the far left; 20 Hz..20 kHz log above it
        if (z.cutoffHz <= 0.f) return 0.f;
        return clamp01(0.04f + 0.96f * std::log(std::max(20.f, std::min(20000.f, z.cutoffHz)) / 20.f)
                                     / std::log(1000.f));
    case ZF_RES:      return clamp01(z.resonanceDb / 30.f);
    case ZF_COARSE:   return clamp01(((float)z.coarseTune + 48.f) / 96.f);
    case ZF_FINE:     return clamp01(((float)z.fineTune + 99.f) / 198.f);
    case ZF_SCALE:    return clamp01((float)z.scaleTuning / 1200.f);
    case ZF_EXCL:     return clamp01((float)z.exclusiveClass / 16.f);
    case ZF_MODPITCH: return clamp01((z.modEnvToPitchCents + 12000.f) / 24000.f);
    case ZF_MODCUT:   return clamp01((z.modEnvToFilterCents + 12000.f) / 24000.f);
    case ZF_LSTART:   return clamp01(z.loopStart);
    case ZF_LEND:     return clamp01(z.loopEnd);
    default:          return 0.f;
    }
}

void SamplerEditorView::insp_set(SamplerZone& z, int field, float v) const {
    v = std::max(0.f, std::min(1.f, v));
    switch (field) {
    case ZF_PAN:      z.pan = v * 2.f - 1.f; break;
    case ZF_ATT:      z.attenuationDb = v * 96.f; break;
    case ZF_CUT:      // the leftmost sliver means OFF, everything else is log Hz
        z.cutoffHz = v <= 0.04f ? 0.f
                   : 20.f * std::pow(1000.f, (v - 0.04f) / 0.96f);
        break;
    case ZF_RES:      z.resonanceDb = v * 30.f; break;
    case ZF_COARSE:   z.coarseTune = (int)std::lround(v * 96.f) - 48; break;
    case ZF_FINE:     z.fineTune = (int)std::lround(v * 198.f) - 99; break;
    case ZF_SCALE:    // steps of 5 so the SF2 landmarks (0 fixed, 100 normal) land exactly
        z.scaleTuning = 5 * (int)std::lround(v * 1200.f / 5.f); break;
    case ZF_EXCL:     z.exclusiveClass = (int)std::lround(v * 16.f); break;
    case ZF_MODPITCH: z.modEnvToPitchCents = 10.f * std::lround((v * 24000.f - 12000.f) / 10.f); break;
    case ZF_MODCUT:   z.modEnvToFilterCents = 10.f * std::lround((v * 24000.f - 12000.f) / 10.f); break;
    case ZF_LSTART:   z.loopStart = std::min(v, z.loopEnd); break;
    case ZF_LEND:     z.loopEnd = std::max(v, z.loopStart); break;
    default: break;
    }
}

void SamplerEditorView::insp_default(SamplerZone& z, int field) const {
    switch (field) {
    case ZF_PAN:      z.pan = 0.f; break;
    case ZF_ATT:      z.attenuationDb = 0.f; break;
    case ZF_CUT:      z.cutoffHz = 0.f; break;
    case ZF_RES:      z.resonanceDb = 0.f; break;
    case ZF_COARSE:   z.coarseTune = 0; break;
    case ZF_FINE:     z.fineTune = 0; break;
    case ZF_SCALE:    z.scaleTuning = 100; break;
    case ZF_EXCL:     z.exclusiveClass = 0; break;
    case ZF_MODPITCH: z.modEnvToPitchCents = 0.f; break;
    case ZF_MODCUT:   z.modEnvToFilterCents = 0.f; break;
    case ZF_LSTART:   z.loopStart = 0.f; break;
    case ZF_LEND:     z.loopEnd = 1.f; break;
    default: break;
    }
}

void SamplerEditorView::insp_format(const SamplerZone& z, int field, char* out, int cap) const {
    switch (field) {
    case ZF_PAN:
        if (z.pan < -0.005f)     std::snprintf(out, cap, "L%d", (int)std::lround(-z.pan * 100.f));
        else if (z.pan > 0.005f) std::snprintf(out, cap, "R%d", (int)std::lround(z.pan * 100.f));
        else                     std::snprintf(out, cap, "C");
        break;
    case ZF_ATT:      std::snprintf(out, cap, "-%.1fdB", z.attenuationDb); break;
    case ZF_CUT:
        if (z.cutoffHz <= 0.f)        std::snprintf(out, cap, "OFF");
        else if (z.cutoffHz >= 1000.f) std::snprintf(out, cap, "%.1fkHz", z.cutoffHz / 1000.f);
        else                           std::snprintf(out, cap, "%.0fHz", z.cutoffHz);
        break;
    case ZF_RES:      std::snprintf(out, cap, "%.1fdB", z.resonanceDb); break;
    case ZF_COARSE:   std::snprintf(out, cap, "%+dst", z.coarseTune); break;
    case ZF_FINE:     std::snprintf(out, cap, "%+dct", z.fineTune); break;
    case ZF_SCALE:
        if (z.scaleTuning == 0) std::snprintf(out, cap, "FIXED");
        else                    std::snprintf(out, cap, "%dct", z.scaleTuning);
        break;
    case ZF_EXCL:
        if (z.exclusiveClass == 0) std::snprintf(out, cap, "OFF");
        else                       std::snprintf(out, cap, "%d", z.exclusiveClass);
        break;
    case ZF_MODPITCH: std::snprintf(out, cap, "%+.0fct", z.modEnvToPitchCents); break;
    case ZF_MODCUT:   std::snprintf(out, cap, "%+.0fct", z.modEnvToFilterCents); break;
    case ZF_LSTART:   std::snprintf(out, cap, "%.1f%%", z.loopStart * 100.f); break;
    case ZF_LEND:     std::snprintf(out, cap, "%.1f%%", z.loopEnd * 100.f); break;
    default:          if (cap > 0) out[0] = 0; break;
    }
}

void SamplerEditorView::select_only(int index) {
    m_selected.clear();
    if (m_zones && index >= 0 && index < (int)m_zones->size()) {
        m_sel = index; m_selected.insert(index);
    } else m_sel = -1;
}

void SamplerEditorView::sanitize_selection() {
    if (!m_zones) { m_selected.clear(); m_sel = -1; return; }
    for (auto it=m_selected.begin(); it!=m_selected.end(); )
        if (*it < 0 || *it >= (int)m_zones->size()) it=m_selected.erase(it); else ++it;
    if (m_sel < 0 || m_sel >= (int)m_zones->size()) m_sel=m_zones->empty()?-1:0;
    if (m_sel >= 0 && m_selected.empty()) m_selected.insert(m_sel);
}

std::vector<int> SamplerEditorView::selected_indices() const {
    std::vector<int> out;
    if (!m_zones) return out;
    for (int i : m_selected) if (i >= 0 && i < (int)m_zones->size()) out.push_back(i);
    if (out.empty() && m_sel >= 0 && m_sel < (int)m_zones->size()) out.push_back(m_sel);
    return out;
}

// All the fields, none of the samples.  Swapping the clip out for the copy
// and back is O(1) vector moves, so this stays cheap at any sample length.
SamplerZone SamplerEditorView::zone_meta_copy(SamplerZone& z) {
    PatchKnob::engine::AudioClip keep;
    std::swap(keep.ch[0], z.clip.ch[0]);
    std::swap(keep.ch[1], z.clip.ch[1]);
    SamplerZone meta = z;                       // clip vectors are empty here
    std::swap(z.clip.ch[0], keep.ch[0]);
    std::swap(z.clip.ch[1], keep.ch[1]);
    return meta;
}

void SamplerEditorView::zone_snapshot(bool metaOnly) {
    if (!m_zones) return;
    ZoneHistEntry e;
    e.metaOnly = metaOnly;
    if (metaOnly) {
        // Mapping/parameter edits: every field except the PCM.  Copying the
        // clips too measured ~250 ms per key-map press on a 2976-zone
        // soundfont; the metadata alone is a few hundred KB.
        e.zones.reserve(m_zones->size());
        for (SamplerZone& z : *m_zones) e.zones.push_back(zone_meta_copy(z));
    } else {
        e.zones = *m_zones;
    }
    m_zone_undo.push_back(std::move(e));
    // Count-only history was catastrophic for long recordings: 32 snapshots
    // of one five-minute stereo float WAV can retain several gigabytes and put
    // the entire DAW into paging. Keep the newest undo, then trim older entries
    // to a firm PCM budget. Metadata is negligible compared with clip samples.
    constexpr size_t kUndoPcmBudget=256u*1024u*1024u;
    auto bytes=[](const ZoneHistEntry& s){size_t n=0;
        for(const SamplerZone& z:s.zones)
            n+=(z.clip.ch[0].size()+z.clip.ch[1].size())*sizeof(float);
        return n;};
    size_t used=0;for(const auto& s:m_zone_undo)used+=bytes(s);
    while(m_zone_undo.size()>1&&(m_zone_undo.size()>32||used>kUndoPcmBudget)){
        used-=bytes(m_zone_undo.front());m_zone_undo.erase(m_zone_undo.begin());
    }
    m_zone_redo.clear();
}

void SamplerEditorView::zone_restore(ZoneHistEntry entry) {
    if (!m_zones) return;
    if (entry.metaOnly) {
        // The edit being unwound could not have changed the zone count or any
        // PCM, and the LIFO order of the history means every structural edit
        // between then and now has already been unwound through its own FULL
        // entry -- so the counts match and each zone keeps its live audio.
        const size_t n = std::min(entry.zones.size(), m_zones->size());
        for (size_t i = 0; i < n; ++i) {
            SamplerZone& dst = (*m_zones)[i];
            PatchKnob::engine::AudioClip keep = std::move(dst.clip);
            dst = std::move(entry.zones[i]);    // all fields, empty clip
            dst.clip = std::move(keep);         // the audio never moved
        }
    } else {
        *m_zones = std::move(entry.zones);
    }
    sanitize_selection();
    if (on_apply) on_apply();
    if (m_sel >= 0 && on_zone_selected) on_zone_selected(m_sel);
}

void SamplerEditorView::edit_zone_inline(PatchKnob::engine::ISampleSlot* slot,
                                         const std::string& title, double sampleRate) {
    m_inlineEditor.set_sample_rate(sampleRate);
    m_inlineEditor.set_slot(slot, title);
    m_inlineEdit = false; // editor is fused into the keyzone workspace
    m_scroll_y = 0;
}

void SamplerEditorView::toggle_sf2(const std::string& path) {
    if (m_sf2Open.count(path)) { m_sf2Open.erase(path); scan_browser(); return; }
    if (!m_sf2Cache.count(path)) {
        PatchKnob::engine::sf2::SoundFont font; std::string err;
        // headers only: presets and zones resolve, no sample data is read
        if (!PatchKnob::engine::sf2::read(path, font, err, /*loadPcm=*/false)) {
            m_browser_msg = "cannot read soundfont: " + err;
            return;                       // degrade quietly, stay collapsed
        }
        m_sf2Cache.emplace(path, std::move(font));
    }
    m_sf2Open.insert(path);
    scan_browser();
}

void SamplerEditorView::scan_browser() {
    m_browser.clear();
    m_browser_rows.clear();
    m_browser_sel = -1;
    m_preview_ok = false;
    m_preview_path.clear();
    m_prev_playing = false;
    m_browser_msg.clear();
    try {
        std::filesystem::path dir(m_browser_dir.empty() ? "." : m_browser_dir);
        if (dir.has_parent_path()) {
            BrowserItem up; up.name = ".."; up.path = dir.parent_path().string(); up.dir = true;
            m_browser.push_back(up);
        }
        std::vector<BrowserItem> dirs, wavs;
        std::error_code ec;
        for (std::filesystem::directory_iterator di(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && di != end; di.increment(ec)) {
            const auto& ent = *di;
            BrowserItem it;
            it.path = ent.path().string();
            it.name = ent.path().filename().string();
            it.dir = ent.is_directory();
            if (it.dir) dirs.push_back(it);
            else if (is_wav_path(ent.path())) wavs.push_back(it);
            else if (is_soundfont_path(ent.path()) &&
                     PatchKnob::engine::sf2::looksLikeSoundFont(it.path)) {
                it.isSoundFont = true;
                wavs.push_back(it);
            }
        }
        auto byName = [](const BrowserItem& a, const BrowserItem& b){
            std::string aa=a.name,bb=b.name;
            std::transform(aa.begin(),aa.end(),aa.begin(),[](unsigned char c){return(char)std::tolower(c);});
            std::transform(bb.begin(),bb.end(),bb.begin(),[](unsigned char c){return(char)std::tolower(c);});
            return aa < bb;
        };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(wavs.begin(), wavs.end(), byName);
        m_browser.insert(m_browser.end(), dirs.begin(), dirs.end());
        // Splice each expanded soundfont's presets in directly under it, so the
        // font reads as a folder that happens to live inside a file.
        for (BrowserItem& f : wavs) {
            if (f.isSoundFont) {
                const bool open = m_sf2Open.count(f.path) != 0;
                f.name = (open ? "- " : "+ ") + f.name;
                m_browser.push_back(f);
                if (!open) continue;
                auto it = m_sf2Cache.find(f.path);
                if (it == m_sf2Cache.end()) continue;
                int pi = 0;
                for (const auto& pr : it->second.presets) {
                    BrowserItem r;
                    char lbl[160];
                    std::snprintf(lbl, sizeof lbl, "    [%03d:%03d] %s",
                                  pr.bank, pr.program, pr.name.c_str());
                    r.name = lbl; r.path = f.path; r.isPreset = true;
                    r.bank = pr.bank; r.program = pr.program;
                    m_browser.push_back(r);
                    ++pi;
                }
                (void)pi;
            } else {
                m_browser.push_back(f);
            }
        }
        // Say WHY the list is empty.  A folder you cannot read and a folder with
        // no WAVs in it produced exactly the same blank pane, so an unreadable
        // drive looked like an empty one.
        if (ec) m_browser_msg = "cannot read this folder";
        else if (wavs.empty()) m_browser_msg = dirs.empty() ? "empty folder" : "no WAV files here";
    } catch (const std::exception& ex) {
        m_browser_msg = ex.what();
    } catch (...) {
        m_browser_msg = "cannot read this folder";
    }
}

std::vector<std::string> SamplerEditorView::drive_roots() const {
    std::vector<std::string> roots;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (mask & (1u << i)) {
            char root[4] = { (char)('A' + i), ':', '\\', 0 };
            roots.emplace_back(root);
        }
    }
#else
    roots.emplace_back("/");
#endif
    if (roots.empty()) roots.emplace_back(".");
    return roots;
}

void SamplerEditorView::select_browser(int index, bool audition) {
    if (index < 0 || index >= (int)m_browser.size()) return;
    const BrowserItem& it = m_browser[index];
    if (it.dir) {
        m_browser_dir = it.path;
        m_browser_scroll = 0;
        scan_browser();
        return;
    }
    // A soundfont is not audio: clicking it EXPANDS it rather than trying to
    // decode an 800 MB bank as a WAV.
    if (it.isSoundFont) { m_browser_sel = index; toggle_sf2(it.path); return; }
    if (it.isPreset) {
        m_browser_sel = index;
        m_browser_msg = "importing " + it.name + " ...";
        if (on_load_sf2_preset) on_load_sf2_preset(it.path, it.bank, it.program);
        else m_browser_msg = "no importer bound";
        return;
    }
    m_browser_sel = index;
    m_preview_ok = false;
    m_prev_playing = false;
    m_preview_path.clear();
    if (on_preview_path)
        m_preview_ok = on_preview_path(it.path, m_preview_clip) && !m_preview_clip.empty();
    // Remember WHICH file the decoded audio came from: a drop of that same file
    // then reuses it instead of decoding the WAV a second time on the UI thread.
    if (m_preview_ok) m_preview_path = it.path;
    // The peak table follows the file, not the frame: rebuilt here, once.
    rebuild_preview_peaks();
    if (!m_preview_ok)
        m_browser_msg = "could not read " + it.name;
    else
        m_browser_msg.clear();
    if (audition && m_preview_ok) {
        if (on_audition_clip) on_audition_clip(m_preview_clip);
        else PatchKnob::app::audio_app_preview_clip(m_preview_clip, 1.0f);
        m_prev_playing = true;
        m_prev_ms0 = (Uint32)SDL_GetTicks();
        m_prev_from = 0;
        m_prev_last_x = -1;
    }
}

// Move the selection `delta` FILE rows (skipping folders), clamping at the ends.
// The old do/while gave up whenever the walk touched index 0 or the last row --
// so Up from the second file, or Down towards a trailing folder, silently did
// nothing -- and it scrolled using a hardcoded 18 px row height and a "- 70"
// fudge for the pane chrome, neither of which matched what draw() laid out, so
// the keyboard selection could sit outside the visible rows.
bool SamplerEditorView::step_browser(int delta, bool audition) {
    const int n = (int)m_browser.size();
    if (n == 0 || delta == 0) return false;
    const int step = delta > 0 ? 1 : -1;
    int remaining = std::abs(delta);
    int cur = m_browser_sel;
    if (cur < 0) { cur = step > 0 ? -1 : n; remaining = 1; }
    while (remaining > 0) {
        int probe = cur + step;
        while (probe >= 0 && probe < n && m_browser[(size_t)probe].dir) probe += step;
        if (probe < 0 || probe >= n) break;
        cur = probe;
        --remaining;
    }
    if (cur < 0 || cur >= n || m_browser[(size_t)cur].dir || cur == m_browser_sel) return false;
    if (m_browser[(size_t)cur].isSoundFont || m_browser[(size_t)cur].isPreset) return false;
    select_browser(cur, audition);
    if (cur < m_browser_scroll) m_browser_scroll = cur;
    if (cur >= m_browser_scroll + m_browser_visible)
        m_browser_scroll = cur - m_browser_visible + 1;
    if (m_browser_scroll < 0) m_browser_scroll = 0;
    return true;
}

// Which handle of zone `zi` a press should grab.  NEAREST wins: checking the
// edges in a fixed order meant that on a zone narrower than the two 6 px grab
// zones (every drum-kit zone, where loKey == hiKey) the low-edge test matched
// first and matched always -- the velocity edges, the root marker and dragging
// the zone itself were all unreachable.
SamplerEditorView::Drag SamplerEditorView::handle_at(const SDL_Rect& g, int zi,
                                                     int x, int y) const {
    if (!m_zones || zi < 0 || zi >= (int)m_zones->size()) return Drag::None;
    const SamplerZone& z = (*m_zones)[(size_t)zi];
    const int xlo = key_left_x(g, z.loKey), xhi = key_right_x(g, z.hiKey);
    const int rx  = key_center_x(g, z.root);
    const int ylo = vel_to_y(g, z.loVel), yhi = vel_to_y(g, z.hiVel + 1);
    // Edges only count when the pointer is inside the OTHER axis of the zone:
    // grabbing "the top of a zone" 200 px to its right was never intended.
    const bool inX = x >= xlo - 6 && x <= xhi + 6;
    const bool inY = y >= yhi - 6 && y <= ylo + 6;
    const int grab = 6;
    Drag best = Drag::ZoneMove;
    int bestd = grab + 1;
    auto consider = [&](int d, Drag which) { if (d < bestd) { bestd = d; best = which; } };
    if (inY) {
        consider(std::abs(x - xlo), Drag::ZoneLo);
        consider(std::abs(x - xhi), Drag::ZoneHi);
    }
    if (inX) {
        consider(std::abs(y - ylo), Drag::ZoneVelLo);
        consider(std::abs(y - yhi), Drag::ZoneVelHi);
    }
    // The root marker is INSIDE the zone body, so it only wins if it is strictly
    // closer than an edge -- otherwise a one-key zone could never be resized.
    if (inY && std::abs(x - rx) < bestd) { bestd = std::abs(x - rx); best = Drag::ZoneRoot; }
    // A zone too narrow to have distinguishable edges is a MOVE target: you can
    // still resize it from the keyboard (arrows) or the context menu.
    if ((xhi - xlo) <= 2 * grab && (best == Drag::ZoneLo || best == Drag::ZoneHi))
        best = Drag::ZoneMove;
    return best;
}

SDL_Point SamplerEditorView::env_pt(const SDL_Rect& r, const EnvNode& n) const {
    return SDL_Point{ r.x + (int)(n.x * r.w), r.y + (int)((1.f - n.y) * r.h) };
}

// The curve handle's DRAWN position.  draw_env() used to plot it at the stepped
// polyline sample nearest the middle and the hit test computed the analytic
// midpoint: on a wide segment those are different pixels, so the handle you saw
// was not the handle that responded.
SDL_Point SamplerEditorView::env_curve_handle(const SDL_Rect& r, int seg) const {
    if (!m_envs || m_cur_env >= ENV_COUNT) return SDL_Point{ 0, 0 };
    const SamplerEnv& e = m_envs->env[m_cur_env];
    if (seg < 0 || seg + 1 >= (int)e.nodes.size()) return SDL_Point{ 0, 0 };
    const SDL_Point a = env_pt(r, e.nodes[(size_t)seg]);
    const SDL_Point b = env_pt(r, e.nodes[(size_t)seg + 1]);
    const float yy = e.nodes[(size_t)seg].y +
                     (e.nodes[(size_t)seg + 1].y - e.nodes[(size_t)seg].y) *
                     env_shape(0.5f, e.nodes[(size_t)seg].curve);
    (void)b;
    return SDL_Point{ (a.x + b.x) / 2, r.y + (int)((1.f - yy) * r.h) };
}
int SamplerEditorView::env_node_at(int x, int y, const SDL_Rect& r) const {
    if (!m_envs || m_cur_env >= ENV_COUNT) return -1;
    const SamplerEnv& e = m_envs->env[m_cur_env];
    // Keep the dot compact, but give it a forgiving invisible target.  Pick
    // the nearest node so tightly spaced points do not depend on vector order.
    constexpr int kGrabRadius = 11;
    int nearest = -1;
    int nearestD2 = kGrabRadius * kGrabRadius + 1;
    for (size_t i = 0; i < e.nodes.size(); ++i) {
        SDL_Point p = env_pt(r, e.nodes[i]);
        const int dx = x - p.x, dy = y - p.y;
        const int d2 = dx * dx + dy * dy;
        if (d2 <= kGrabRadius * kGrabRadius && d2 < nearestD2) {
            nearest = (int)i;
            nearestD2 = d2;
        }
    }
    return nearest;
}

void SamplerEditorView::draw_env(App& app, const SDL_Rect& r) {
    const Theme& t = theme();
    // env selector tabs: the 5 instrument-global envelopes, then the SELECTED
    // ZONE's amp/mod overrides.  A zone tab lights its label when that zone is
    // actually overriding (not inheriting) the instrument envelope.
    const int cw = app.mono.cw() ? app.mono.cw() : 6, ch = app.mono.ch();
    const SamplerZone* selz = (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size())
                            ? &(*m_zones)[(size_t)m_sel] : nullptr;
    int tx = r.x;
    for (int i = 0; i < ENV_TABS; ++i) {
        SDL_Rect tab{ tx, r.y, app.mono.text_w(kEnvNames[i]) + cw + 4, ch + 4 };
        m_env_tabs[i] = tab;
        const bool cur = i == m_cur_env;
        bool on = false;
        if (i < ENV_COUNT)       on = m_envs && m_envs->env[i].enabled;
        else if (i == ZENV_AMP)  on = selz && selz->ampEnv.enabled;
        else                     on = selz && selz->modEnv.enabled;
        fill_rect(app.ren, tab, cur ? t.accent : t.panel);
        frame_rect(app.ren, tab, t.dim);
        app.mono.draw(app.ren, tab.x + 4, tab.y + 2, kEnvNames[i], cur ? t.bg : (on ? t.text : t.dim));
        tx += tab.w + 2;
    }
    // Zone-env controls exist only while a zone tab is up; stale rects must not
    // keep eating clicks after switching back to an instrument envelope.
    m_zenv_toggle = SDL_Rect{0,0,0,0};
    for (int s = 0; s < 6; ++s) m_zenv_rows[s] = SDL_Rect{0,0,0,0};
    SDL_Rect canvas{ r.x, r.y + ch + 8, r.w, r.h - ch - 8 };
    // A canvas with no area divides by zero in every normalise below (and the
    // add-a-node path fed the NaN straight into a node position, which then drew
    // nowhere and could never be grabbed again).  Publish an empty rect instead:
    // in_rect() rejects it, so the whole editor is simply inert until it fits.
    if (canvas.w < 8 || canvas.h < 8) { m_env_rect = SDL_Rect{0,0,0,0}; return; }
    if (m_cur_env >= ENV_COUNT) {
        // Per-zone DAHDSR editor.  The node canvas is retired for these tabs;
        // publishing an empty m_env_rect keeps every node/curve hit test inert.
        m_env_rect = SDL_Rect{0,0,0,0};
        draw_zone_env(app, canvas);
        return;
    }
    m_env_rect = canvas;
    fill_rect(app.ren, canvas, t.bg);
    frame_rect(app.ren, canvas, t.dim);
    for (int g = 1; g < 4; ++g) {
        hline(app.ren, canvas.x, canvas.x + canvas.w - 1,
              canvas.y + canvas.h * g / 4, t.panel);
        vline(app.ren, canvas.x + canvas.w * g / 4,
              canvas.y, canvas.y + canvas.h - 1, t.panel);
    }
    if (!m_envs) return;
    SamplerEnv& e = m_envs->env[m_cur_env];
    if (!e.enabled) {
        app.mono.draw(app.ren, canvas.x + 8, canvas.y + 6,
                      "Disabled - click to add a point; Shift-click a point to set sustain", t.dim);
    }
    // curved segments
    for (size_t i = 0; i + 1 < e.nodes.size(); ++i) {
        set_color(app.ren, e.enabled ? t.accent : t.dim);
        SDL_Point a = env_pt(canvas, e.nodes[i]), b = env_pt(canvas, e.nodes[i + 1]);
        int px = a.x, py = a.y;
        const int steps = std::max(2, (b.x - a.x) / 4);
        for (int s = 1; s <= steps; ++s) {
            float tt = (float)s / steps;
            float yy = e.nodes[i].y + (e.nodes[i + 1].y - e.nodes[i].y) * env_shape(tt, e.nodes[i].curve);
            int nx = a.x + (int)((b.x - a.x) * tt);
            int ny = canvas.y + (int)((1.f - yy) * canvas.h);
            SDL_RenderDrawLine(app.ren, px, py, nx, ny);
            px = nx; py = ny;
        }
        // Curve handle at the ONE place the hit test looks for it, highlighted
        // when the pointer is on it so it is discoverable at all.
        const SDL_Point h = env_curve_handle(canvas, (int)i);
        const bool hot = m_hover_in && std::abs(m_mx - h.x) <= 5 && std::abs(m_my - h.y) <= 5;
        fill_rect(app.ren, SDL_Rect{ h.x - 3, h.y - 3, 7, 7 }, hot ? t.sel : t.hi);
        if (hot) { m_tip = "drag: curve of this segment"; m_tip_anchor = SDL_Rect{ h.x - 3, h.y - 3, 7, 7 }; }
    }
    // nodes (sustain node ringed)
    for (size_t i = 0; i < e.nodes.size(); ++i) {
        SDL_Point p = env_pt(canvas, e.nodes[i]);
        fill_rect(app.ren, SDL_Rect{ p.x - 4, p.y - 4, 9, 9 },
                  (int)i == e.sustain ? t.accent : t.text);
        if ((int)i == e.sustain) frame_rect(app.ren, SDL_Rect{ p.x - 6, p.y - 6, 13, 13 }, t.hi);
    }
}

//----------------------------------------------------------------------------
//  per-zone DAHDSR editor (Z-AMP / Z-MOD tabs)
//
//  Right column: the override toggle and one slider per stage.  Left: the
//  curve those stages produce.  Falling back to the instrument envelope is the
//  DEFAULT, so that state is spelled out in words -- an inherited envelope must
//  never read as an empty one.
//----------------------------------------------------------------------------
void SamplerEditorView::draw_zone_env(App& app, const SDL_Rect& canvas) {
    const Theme& t = theme();
    const int cw = app.mono.cw() ? app.mono.cw() : 6, ch = app.mono.ch();
    fill_rect(app.ren, canvas, t.bg);
    frame_rect(app.ren, canvas, t.dim);
    if (!m_zones || m_sel < 0 || m_sel >= (int)m_zones->size()) {
        app.mono.draw(app.ren, canvas.x + 8, canvas.y + 6,
                      fit_text(app.mono, "no zone selected", canvas.w - 16), t.dim);
        return;
    }
    SamplerZone& z = (*m_zones)[(size_t)m_sel];
    const bool isMod = m_cur_env == ZENV_MOD;
    const SamplerZoneEnvUI& en = isMod ? z.modEnv : z.ampEnv;
    const bool over = en.enabled != 0;
    const Color line = over ? t.accent : t.dim;

    // ---- right column: override toggle + stage sliders ---------------------
    const int colW = std::max(20 * cw, canvas.w * 2 / 5);
    const SDL_Rect col{ canvas.x + canvas.w - colW, canvas.y, colW, canvas.h };
    vline(app.ren, col.x, col.y, col.y + col.h - 1, t.dim);
    m_zenv_toggle = SDL_Rect{ col.x + 5, col.y + 4, col.w - 10, ch + 5 };
    {
        const bool hot = m_hover_in && in_rect(m_zenv_toggle, m_mx, m_my);
        fill_rect(app.ren, m_zenv_toggle, over ? t.accent : t.panel);
        frame_rect(app.ren, m_zenv_toggle, hot ? t.hi : t.dim);
        SDL_Rect tx{ m_zenv_toggle.x + 4, m_zenv_toggle.y + 1,
                     m_zenv_toggle.w - 8, m_zenv_toggle.h - 2 };
        app.mono.draw_fitted(app.ren, tx,
                             over ? "ZONE OVERRIDE  (click: back to default)"
                                  : "INSTRUMENT DEFAULT  (click: override)",
                             over ? t.bg : t.text, false, 1.f, .6f, true);
        if (hot) {
            m_tip = over ? "release the override: this zone falls back to the instrument envelope"
                         : "give this zone its own envelope (stages keep their values)";
            m_tip_anchor = m_zenv_toggle;
        }
    }
    const int rowsTop = m_zenv_toggle.y + m_zenv_toggle.h + 3;
    const int rowH = std::max(ch + 4, (col.y + col.h - rowsTop - 3) / 6);
    const int labelW = app.mono.text_w("RELEASE") + cw;
    for (int s = 0; s < 6; ++s) {
        const int ry = rowsTop + s * rowH;
        app.mono.draw(app.ren, col.x + 5, ry + (rowH - ch) / 2, kZEnvStage[s],
                      over ? t.text : t.dim);
        SDL_Rect track{ col.x + 5 + labelW, ry + 2,
                        std::max(8, col.w - labelW - 12), rowH - 4 };
        m_zenv_rows[s] = track;
        const float sv[6] = { en.delay, en.attack, en.hold, en.decay, en.sustain, en.release };
        const float v = s == 4 ? std::max(0.f, std::min(1.f, sv[4]))
                               : zenv_secs_to_slider(sv[s]);
        fill_rect(app.ren, track, t.panel);
        fill_rect(app.ren, SDL_Rect{ track.x, track.y, (int)(track.w * v), track.h },
                  over ? t.accent : t.dim);
        frame_rect(app.ren, track, t.dim);
        char vs[16];
        if (s == 4) std::snprintf(vs, sizeof(vs), "%d%%", (int)std::lround(sv[4] * 100.f));
        else        zenv_fmt_secs(sv[s], vs, sizeof(vs));
        const int vw = app.mono.text_w(vs);
        if (track.w > vw + 2 * cw)
            app.mono.draw(app.ren, track.x + track.w - vw - 3,
                          track.y + (track.h - ch) / 2, vs, v > 0.55f && over ? t.bg : t.text);
        if (m_hover_in && in_rect(track, m_mx, m_my)) {
            m_tip = over ? "drag to set   shift: fine   right-click: default"
                         : "drag to set (turns the zone override ON)";
            m_tip_anchor = track;
        }
    }

    // ---- left area: the curve the stages produce ---------------------------
    const SDL_Rect plot{ canvas.x + 1, canvas.y + 1, canvas.w - colW - 2, canvas.h - 2 };
    if (plot.w > 24 && plot.h > 16) {
        for (int g = 1; g < 4; ++g)
            hline(app.ren, plot.x, plot.x + plot.w - 1, plot.y + plot.h * g / 4, t.panel);
        const float susFrac = 0.18f;                    // fixed on-screen sustain hold
        const float total = std::max(0.02f, en.delay + en.attack + en.hold + en.decay + en.release);
        const float px_per_s = (float)plot.w * (1.f - susFrac) / total;
        const int pad = 4;
        const int y0 = plot.y + plot.h - pad, y1 = plot.y + pad;
        auto ly = [&](float lvl){ return y0 - (int)((float)(y0 - y1) * std::max(0.f, std::min(1.f, lvl))); };
        int x = plot.x;
        auto seg = [&](float secs, float lvlFrom, float lvlTo){
            const int nx = x + (int)(secs * px_per_s);
            set_color(app.ren, line);
            SDL_RenderDrawLine(app.ren, x, ly(lvlFrom), nx, ly(lvlTo));
            x = nx;
        };
        const float sus = std::max(0.f, std::min(1.f, en.sustain));
        seg(en.delay, 0.f, 0.f);
        seg(en.attack, 0.f, 1.f);
        seg(en.hold, 1.f, 1.f);
        seg(en.decay, 1.f, sus);
        const int susEnd = x + (int)((float)plot.w * susFrac);
        set_color(app.ren, line);
        SDL_RenderDrawLine(app.ren, x, ly(sus), susEnd, ly(sus));
        {   // the held stretch is marked so it reads as "until note-off"
            BlendScope blend(app.ren);
            fill_rect(app.ren, SDL_Rect{ x, ly(sus), std::max(1, susEnd - x), y0 - ly(sus) + 1 },
                      fade(line, 40));
        }
        x = susEnd;
        seg(en.release, sus, 0.f);
        // The inherit state, in words, over the dimmed preview curve.
        if (!over) {
            const char* l1 = "USING INSTRUMENT DEFAULT";
            const char* l2 = isMod ? "drag a stage to give this zone its own mod envelope"
                                   : "drag a stage to give this zone its own amp envelope";
            app.mono.draw(app.ren, plot.x + 8, plot.y + 5,
                          fit_text(app.mono, l1, plot.w - 16), t.text);
            app.mono.draw(app.ren, plot.x + 8, plot.y + 7 + ch,
                          fit_text(app.mono, l2, plot.w - 16), t.dim);
        } else if (isMod) {
            app.mono.draw(app.ren, plot.x + 8, plot.y + 5,
                          fit_text(app.mono, "depth: MOD>PITCH / MOD>CUTOFF in ZONE PARAMETERS",
                                   plot.w - 16), t.dim);
        }
    }
}

int SamplerEditorView::key_at_x(int x, const SDL_Rect& s) const {
    if (s.w <= 0) return 0;
    int k = m_key_first+(int)std::floor((double)(x-s.x)*m_key_visible/(double)s.w);
    return std::max(0, std::min(127, k));
}
int SamplerEditorView::key_left_x(const SDL_Rect& r,int key) const {
    return r.x+(int)std::floor((double)(key-m_key_first)*r.w/m_key_visible);
}
int SamplerEditorView::key_right_x(const SDL_Rect& r,int key) const {
    return r.x+(int)std::ceil((double)(key+1-m_key_first)*r.w/m_key_visible);
}
int SamplerEditorView::key_center_x(const SDL_Rect& r,int key) const {
    return r.x+(int)std::lround(((double)(key-m_key_first)+.5)*r.w/m_key_visible);
}
int SamplerEditorView::zone_at_key(int key) const {
    if (!m_zones) return -1;
    for (int i = (int)m_zones->size() - 1; i >= 0; --i)
        if (key >= (*m_zones)[i].loKey && key <= (*m_zones)[i].hiKey) return i;
    return -1;
}
int SamplerEditorView::zone_at(int key, int vel) const {
    if (!m_zones) return -1;
    for (int i = (int)m_zones->size() - 1; i >= 0; --i) {
        const SamplerZone& z = (*m_zones)[(size_t)i];
        if (key >= z.loKey && key <= z.hiKey && vel >= z.loVel && vel <= z.hiVel) return i;
    }
    return -1;
}

//----------------------------------------------------------------------------
//  drag & drop from the sample library
//
//  What the pointer is over is worked out ONCE, here, and both the highlight
//  and the release read it.  While a file is in flight the zone underneath is
//  lit, so the drop target is never a guess, and a drop that lands on nothing
//  droppable simply cancels instead of loading into whatever was selected.
//----------------------------------------------------------------------------
SamplerEditorView::DropTarget SamplerEditorView::drop_target_at(int x, int y) const {
    DropTarget d;
    if (!m_zones) return d;

    // ---- key map / keyboard strip -----------------------------------------
    const SDL_Rect grid = keymap_grid();
    const bool onGrid  = in_rect(grid, x, y);
    const bool onStrip = !onGrid && in_rect(m_strip, x, y);
    if (onGrid || onStrip) {
        const SDL_Rect& axis = onGrid ? grid : m_strip;
        const int key = key_at_x(x, axis);
        // On the grid the velocity axis is meaningful, so a velocity LAYER can
        // be targeted individually; the strip has no velocity, so it addresses
        // the topmost zone on that key.
        const int zi = onGrid ? zone_at(key, y_to_vel(grid, y)) : zone_at_key(key);
        if (zi >= 0 && zi < (int)m_zones->size()) {
            const SamplerZone& z = (*m_zones)[(size_t)zi];
            const int x0 = key_left_x(grid, z.loKey), x1 = key_right_x(grid, z.hiKey);
            const int y0 = vel_to_y(grid, z.hiVel + 1), y1 = vel_to_y(grid, z.loVel);
            d.kind = DropTarget::ReplaceZone; d.zone = zi; d.key = key;
            d.hi = SDL_Rect{ x0, y0, std::max(3, x1 - x0), std::max(3, y1 - y0) };
            return d;
        }
        const int x0 = key_left_x(grid, key), x1 = key_right_x(grid, key);
        d.kind = DropTarget::NewAtKey; d.key = key;
        d.hi = SDL_Rect{ x0, grid.y, std::max(3, x1 - x0), grid.h };
        return d;
    }

    // ---- zone list ---------------------------------------------------------
    for (size_t i = 0; i < m_zone_rows.size(); ++i)
        if (in_rect(m_zone_rows[i], x, y)) {
            d.kind = DropTarget::ReplaceZone;
            d.zone = i < m_zone_row_indices.size() ? m_zone_row_indices[i] : (int)i;
            d.hi = m_zone_rows[i];
            return d;
        }
    if (in_rect(m_zone_list_rect, x, y)) {
        // Empty space under the last row: a new zone, over the whole keyboard.
        const int top = m_zone_rows.empty()
                      ? m_zone_list_rect.y + m_keymap_ch + 12
                      : m_zone_rows.back().y + m_zone_rows.back().h;
        d.kind = DropTarget::AppendZone;
        d.hi = SDL_Rect{ m_zone_list_rect.x + 3, top, m_zone_list_rect.w - 6,
                         std::max(4, m_zone_list_rect.y + m_zone_list_rect.h - 3 - top) };
        return d;
    }

    // ---- the waveform editor shows the SELECTED zone -----------------------
    // Dropping on the sample you are looking at replaces exactly that sample.
    if (in_rect(m_wave, x, y) && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        d.kind = DropTarget::ReplaceZone; d.zone = m_sel; d.hi = m_wave;
        return d;
    }
    return d;
}

void SamplerEditorView::set_drop_status(const std::string& msg) {
    m_drop_status = msg;
    m_drop_status_ms = (Uint32)SDL_GetTicks();
}

void SamplerEditorView::drop_sample(App& app, const std::string& path,
                                    const DropTarget& tgt) {
    if (!m_zones || tgt.kind == DropTarget::None || path.empty()) return;
    const size_t slash = path.find_last_of("/\\");
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    if (tgt.kind == DropTarget::ReplaceZone &&
        (tgt.zone < 0 || tgt.zone >= (int)m_zones->size())) return;

    // Undoable BEFORE anything is replaced.  A drop that ate the sample under
    // it with no way back is the one failure this must not have; the whole zone
    // set is the unit the rest of this view undoes through (Ctrl+Z).
    zone_snapshot();

    // The dragged file was decoded into the preview the moment the drag STARTED
    // (the press auditions it), so the drop reuses that decode instead of
    // stalling the UI thread reading the same WAV a second time.  Only a file
    // the preview could not read falls back to the shell's blocking loader.
    const bool decoded = m_preview_ok && !m_preview_clip.empty() &&
                         m_preview_path == path;
    if (!decoded) {
        if (!on_load_path) { m_zone_undo.pop_back(); return; }
        set_drop_status("loading " + leaf + " ...");
        app.request_redraw();
        const int before = (int)m_zones->size();
        on_load_path(path,
                     tgt.kind == DropTarget::ReplaceZone ? tgt.zone : -1,
                     tgt.kind == DropTarget::NewAtKey ? tgt.key : -1);
        // The loader reports failure through the shell's status line, not to us.
        // Detect it from the model so a failed drop does not leave a phantom
        // entry on the undo stack that silently reverts an unrelated edit.
        const bool ok = tgt.kind == DropTarget::ReplaceZone
                      ? (tgt.zone < (int)m_zones->size() &&
                         (*m_zones)[(size_t)tgt.zone].name == leaf)
                      : (int)m_zones->size() > before;
        if (!ok) { if (!m_zone_undo.empty()) m_zone_undo.pop_back();
                   set_drop_status("could not load " + leaf); }
        else set_drop_status("loaded " + leaf);
        app.request_redraw();
        return;
    }

    if (tgt.kind == DropTarget::ReplaceZone) {
        SamplerZone& z = (*m_zones)[(size_t)tgt.zone];
        // ONLY the audio and the display name.  The key range, root note,
        // velocity band, loop/envelope/gain/pan settings ARE the mapping: a
        // replacement that reset them would silently unmap the zone, which is
        // the whole reason for dropping onto a specific zone in the first place.
        z.clip = std::move(m_preview_clip);
        z.name = leaf;
        select_only(tgt.zone);
        char note[8]; note_name(z.root, note, 8);
        set_drop_status("replaced zone " + std::to_string(tgt.zone + 1) +
                        " with " + leaf + " (root " + note + " kept)");
    } else {
        SamplerZone z;
        z.clip = std::move(m_preview_clip);
        z.name = leaf;
        if (tgt.kind == DropTarget::NewAtKey) {
            // Dropped ON a key: map it there, drum-kit style, matching what the
            // shell's own loader does for a key-targeted load.
            z.root = z.loKey = z.hiKey = std::max(0, std::min(127, tgt.key));
            char note[8]; note_name(z.root, note, 8);
            set_drop_status("new zone on " + std::string(note) + ": " + leaf);
        } else {
            // Dropped in open space: the sensible default is the whole keyboard
            // at concert pitch, which is what SamplerZone already carries.
            set_drop_status("new zone: " + leaf);
        }
        m_zones->push_back(std::move(z));
        select_only((int)m_zones->size() - 1);
    }
    // Ownership moved into the zone. Do not retain another full decoded copy
    // (or a peak cache keyed to its released vectors) for a long recording.
    m_preview_ok=false;m_preview_path.clear();m_preview_peaks.key=nullptr;
    m_preview_peaks.frames=0;m_preview_peaks.mn.clear();
    m_preview_peaks.mx.clear();m_preview_peaks.rms.clear();
    if (on_apply) on_apply();
    // Rebind the waveform editor to what was just dropped.  This is not
    // cosmetic: growing the zone vector can move it, and the editor's slot
    // holds a pointer INTO it, so it has to be re-pointed before anything
    // draws again.
    if (m_sel >= 0 && on_zone_selected) on_zone_selected(m_sel);
    else if (!on_zone_selected) m_inlineEditor.set_slot(nullptr);
    app.request_redraw();
}

// Painted LAST, over everything else.  The highlight used to be drawn with the
// zones -- i.e. before the waveform editor -- so it disappeared under the very
// pane you were dropping onto, and there was no ghost at all: nothing on screen
// said WHICH file was in flight.
void SamplerEditorView::draw_drag_ghost(App& app) {
    if (m_drag != Drag::BrowserSample || !m_drag_armed || m_drag_path.empty()) return;
    const Theme& t = theme();
    const int ch = app.mono.ch();
    const int pad = std::max(3, ch / 4);
    const DropTarget& d = m_drag_target;

    // ---- the target ---------------------------------------------------------
    // Exactly the rect drop_target_at() produced: what lights up IS what the
    // release will hit.
    if (d.kind != DropTarget::None && d.hi.w > 0 && d.hi.h > 0) {
        const Color key = d.kind == DropTarget::ReplaceZone ? t.hi : t.accent;
        {
            BlendScope blend(app.ren);
            fill_rect(app.ren, d.hi, fade(key, 90));
        }
        frame_rect(app.ren, d.hi, key);
        SDL_Rect inner{ d.hi.x + 2, d.hi.y + 2, d.hi.w - 4, d.hi.h - 4 };
        if (inner.w > 2 && inner.h > 2) frame_rect(app.ren, inner, key);
    }

    // ---- the ghost ----------------------------------------------------------
    int mx = m_mx, my = m_my;
    ui::mouse_logical(app, mx, my);
    std::string what;
    switch (d.kind) {
        case DropTarget::ReplaceZone: {
            char lo[8], hi[8];
            const bool live = m_zones && d.zone >= 0 && d.zone < (int)m_zones->size();
            if (live) {
                const SamplerZone& z = (*m_zones)[(size_t)d.zone];
                note_name(z.loKey, lo, 8); note_name(z.hiKey, hi, 8);
                char buf[96];
                std::snprintf(buf, sizeof(buf), "replace zone %02d   %s-%s kept",
                              d.zone + 1, lo, hi);
                what = buf;
            } else what = "replace zone";
            break;
        }
        case DropTarget::NewAtKey: {
            char kn[8]; note_name(d.key, kn, 8);
            what = std::string("new zone on ") + kn;
            break;
        }
        case DropTarget::AppendZone: what = "new zone   full keyboard"; break;
        default:                     what = "release to cancel";        break;
    }
    const std::string leaf = m_drag_name.empty() ? m_drag_path : m_drag_name;
    // Measured, never a byte count times a nominal advance -- a name with any
    // multi-byte character in it would spill straight out of the box.
    const int textW = std::max(app.mono.text_w(leaf), app.mono.text_w(what));
    SDL_Rect g{ mx + 14, my - ch, textW + 4 * pad, 2 * ch + 3 * pad };
    // Keep it on screen: at the right or bottom edge the ghost would otherwise
    // be clipped away exactly when you need to read it.
    if (g.x + g.w > rect.x + rect.w - 2) g.x = std::max(rect.x + 2, mx - 14 - g.w);
    if (g.y + g.h > rect.y + rect.h - 2) g.y = rect.y + rect.h - 2 - g.h;
    if (g.y < rect.y + 2) g.y = rect.y + 2;
    sampleslot::skin::fill_round(app.ren, SDL_Rect{ g.x + 2, g.y + 2, g.w, g.h }, pad,
                                 Color{ 0, 0, 0, 70 });
    sampleslot::skin::fill_round(app.ren, g, pad, t.panel);
    sampleslot::skin::frame_round(app.ren, g, pad,
                                  d.kind == DropTarget::None ? t.dim : t.accent);
    app.mono.draw(app.ren, g.x + 2 * pad, g.y + pad, leaf, t.text);
    app.mono.draw(app.ren, g.x + 2 * pad, g.y + pad + ch, what,
                  d.kind == DropTarget::None ? t.dim : t.accent);
}

//----------------------------------------------------------------------------
//  key map geometry -- the single source draw() and every hit test share
//----------------------------------------------------------------------------
SDL_Rect SamplerEditorView::keymap_grid() const {
    // m_keyzone_grid is the pane; the plot area is inset for the title bar and
    // the keyboard strip along the bottom.  m_keymap_ch is whatever font cell the
    // last draw used, so a hit test can never be laid out for a different one.
    const int ch = m_keymap_ch > 0 ? m_keymap_ch : 12;
    return SDL_Rect{ m_keyzone_grid.x + 6, m_keyzone_grid.y + ch + 12,
                     std::max(1, m_keyzone_grid.w - 12),
                     std::max(1, m_keyzone_grid.h - ch - 34) };
}

// Velocity 0 sits on the BOTTOM edge and 127 on the top; a zone's band covers
// [loVel, hiVel+1) so a single-velocity zone is still one row tall.
int SamplerEditorView::vel_to_y(const SDL_Rect& g, int vel) const {
    vel = std::max(0, std::min(128, vel));
    return g.y + g.h - (int)std::lround((double)vel / 128.0 * (double)g.h);
}
int SamplerEditorView::y_to_vel(const SDL_Rect& g, int y) const {
    if (g.h <= 0) return 0;
    const double v = (double)(g.y + g.h - y) / (double)g.h * 128.0;
    return std::max(0, std::min(127, (int)std::floor(v)));
}

//----------------------------------------------------------------------------
//  browser preview waveform
//
//  Peak/RMS reduction from a cached mip, batched into two RenderFillRects.  The
//  old version point-scanned the LEFT channel only, once per pixel column, on
//  every repaint: transients on the right channel were invisible, a hard-panned
//  file looked silent, and hovering a button re-read the whole file.
//----------------------------------------------------------------------------
void SamplerEditorView::rebuild_preview_peaks() {
    m_preview_peaks.key = nullptr;
    m_preview_peaks.frames = 0;
    m_preview_peaks.mn.clear(); m_preview_peaks.mx.clear(); m_preview_peaks.rms.clear();
    m_preview_mono = true;
    const int64_t n = m_preview_clip.safeFrames();
    if (n <= 0) return;
    const int b = 256;
    const size_t nb = (size_t)((n + b - 1) / b);
    m_preview_peaks.mn.resize(nb); m_preview_peaks.mx.resize(nb); m_preview_peaks.rms.resize(nb);
    const float* L = m_preview_clip.ch[0].data();
    const float* R = m_preview_clip.ch[1].empty() ? L : m_preview_clip.ch[1].data();
    // Mono-ness is decided IN this walk (per-bucket memcmp while the data is
    // already in cache) -- a separate whole-file vector compare added ~25 ms
    // to every file selection on a 10-minute WAV.
    bool same = true;
    for (size_t k = 0; k < nb; ++k) {
        const int64_t a = (int64_t)k * b, e = std::min<int64_t>(n, a + b);
        float mn = 1.f, mx = -1.f; double sq = 0.0;
        for (int64_t i = a; i < e; ++i) {
            const float l = L[i], r = R[i];
            if (l < mn) mn = l;
            if (l > mx) mx = l;
            if (r < mn) mn = r;
            if (r > mx) mx = r;
            const double m = 0.5 * ((double)l + (double)r);
            sq += m * m;
        }
        if (same && R != L)
            same = 0 == std::memcmp(L + a, R + a, (size_t)(e - a) * sizeof(float));
        m_preview_peaks.mn[k] = mn;
        m_preview_peaks.mx[k] = mx;
        m_preview_peaks.rms[k] = (float)std::sqrt(sq / (double)std::max<int64_t>(1, e - a));
    }
    m_preview_mono = same;
    m_preview_peaks.key = L;
    m_preview_peaks.frames = n;
    m_preview_peaks.bucket = b;
}

void SamplerEditorView::preview_column(const AudioClip& c, int64_t s0, int64_t s1,
                                       float& mn, float& mx, float& rms) const {
    // Reduces the clip it is HANDED.  It used to read m_preview_clip no matter
    // what draw_wave() was passed -- a trap where any new call site would have
    // silently drawn the preview's audio under the caller's clip's name.
    mn = 0.f; mx = 0.f; rms = 0.f;
    const int64_t n = c.safeFrames();
    if (n <= 0) return;
    if (s0 < 0) s0 = 0;
    if (s1 > n) s1 = n;
    if (s0 >= n) return;
    if (s1 <= s0) s1 = s0 + 1;
    const int b = m_preview_peaks.bucket > 0 ? m_preview_peaks.bucket : 256;
    const float* L = c.ch[0].data();
    const float* R = c.ch[1].empty() ? L : c.ch[1].data();
    if (m_preview_peaks.key == L && m_preview_peaks.frames == n && (s1 - s0) >= 2 * b) {
        const size_t k0 = (size_t)(s0 / b);
        const size_t k1 = std::min(m_preview_peaks.mn.size(), (size_t)((s1 + b - 1) / b));
        float lo = 1.f, hi = -1.f; double sq = 0.0; size_t cnt = 0;
        for (size_t k = k0; k < k1; ++k) {
            if (m_preview_peaks.mn[k] < lo) lo = m_preview_peaks.mn[k];
            if (m_preview_peaks.mx[k] > hi) hi = m_preview_peaks.mx[k];
            sq += (double)m_preview_peaks.rms[k] * m_preview_peaks.rms[k];
            ++cnt;
        }
        if (cnt) { mn = lo; mx = hi; rms = (float)std::sqrt(sq / (double)cnt); }
        return;
    }
    float lo = 1.f, hi = -1.f; double sq = 0.0;
    for (int64_t i = s0; i < s1; ++i) {
        const float l = L[i], r = R[i];
        if (l < lo) lo = l;
        if (l > hi) hi = l;
        if (r < lo) lo = r;
        if (r > hi) hi = r;
        const double m = 0.5 * ((double)l + (double)r);
        sq += m * m;
    }
    mn = lo; mx = hi;
    rms = (float)std::sqrt(sq / (double)std::max<int64_t>(1, s1 - s0));
}

void SamplerEditorView::draw_wave(App& app, const SDL_Rect& r, const AudioClip& c) {
    const Theme& t = theme();
    fill_rect(app.ren, r, t.bg);
    frame_rect(app.ren, r, t.dim);
    const int64_t n = c.safeFrames();
    if (n <= 0 || r.w < 4 || r.h < 6) {
        if (r.h >= app.mono.ch())
            app.mono.draw(app.ren, r.x + 6, r.y + r.h / 2 - app.mono.ch() / 2,
                          fit_text(app.mono, "(no audio)", r.w - 12), t.dim);
        return;
    }
    // Plot INSIDE the frame: the old loop started at r.x and ran r.w columns, so
    // the first and last columns painted over the pane's own border.
    const SDL_Rect w{ r.x + 1, r.y + 1, r.w - 2, r.h - 2 };
    const int mid = w.y + w.h / 2;
    const int amp = std::max(1, w.h / 2 - 2);     // never 0 or negative: a short
                                                  // pane used to mirror the wave
    {   // zero line UNDER the audio -- visible through silence, hidden by loud
        // material, which is exactly where it is useful
        BlendScope blend(app.ren);
        hline(app.ren, w.x, w.x + w.w - 1, mid, fade(t.dim, 170));
    }
    m_col_peak.clear(); m_col_rms.clear();
    m_col_peak.reserve((size_t)w.w); m_col_rms.reserve((size_t)w.w);
    for (int px = 0; px < w.w; ++px) {
        const int64_t a = (int64_t)((double)px       / (double)w.w * (double)n);
        int64_t b       = (int64_t)((double)(px + 1) / (double)w.w * (double)n);
        if (b <= a) b = a + 1;
        float mn, mx, rms;
        preview_column(c, a, b, mn, mx, rms);
        int y0 = mid - (int)(mx * amp), y1 = mid - (int)(mn * amp);
        if (y1 < y0) std::swap(y0, y1);
        // Clamp INSIDE the pane: a normalised-hot file (samples past +-1) used to
        // paint over the frame and into the neighbouring widget.
        if (y0 < w.y) y0 = w.y;
        if (y1 > w.y + w.h - 1) y1 = w.y + w.h - 1;
        if (y1 < y0) continue;
        const int rh = (int)(rms * amp);
        const int b0 = std::max(w.y, mid - rh), b1 = std::min(w.y + w.h - 1, mid + rh);
        m_col_peak.push_back(SDL_Rect{ w.x + px, y0, 1, (y1 - y0) + 1 });
        m_col_rms.push_back(SDL_Rect{ w.x + px, b0, 1, std::max(1, b1 - b0 + 1) });
    }
    if (!m_col_peak.empty()) {
        set_color(app.ren, t.dim);                 // soft peak envelope
        SDL_RenderFillRects(app.ren, m_col_peak.data(), (int)m_col_peak.size());
        set_color(app.ren, t.text);                // solid RMS body inside it
        SDL_RenderFillRects(app.ren, m_col_rms.data(), (int)m_col_rms.size());
    }
    // --- play cursor, drawn last so nothing buries it ----------------------
    if (m_prev_playing && &c == &m_preview_clip) {
        const double secs = (double)((Uint32)SDL_GetTicks() - m_prev_ms0) / 1000.0;
        const double sr = c.sampleRate > 0 ? c.sampleRate : 48000.0;
        const int64_t pos = m_prev_from + (int64_t)(secs * sr);
        if (pos >= n) { m_prev_playing = false; m_prev_last_x = -1; }
        else {
            const int x = w.x + (int)((double)pos / (double)n * (double)w.w);
            vline(app.ren, x, w.y, w.y + w.h - 1, t.sel);
            for (int k = 0; k < 3; ++k)
                hline(app.ren, x - (2 - k), x + (2 - k), w.y + k, t.sel);
            app.add_damage(SDL_Rect{ x - 16, w.y, 32, w.h });
            if (x != m_prev_last_x) { m_prev_last_x = x; app.request_redraw(); }
        }
    }
}

// The engine preview always starts at frame 0, so auditioning from a click means
// handing it the tail.  Capped at 20 s: a click in a long field recording should
// preview the passage you clicked, not copy the whole file.
void SamplerEditorView::audition_preview(App& app, int64_t fromFrame) {
    const int64_t n = m_preview_clip.safeFrames();
    if (n <= 0) return;
    if (fromFrame < 0) fromFrame = 0;
    if (fromFrame >= n) return;
    const double sr = m_preview_clip.sampleRate > 0 ? m_preview_clip.sampleRate : 48000.0;
    const int64_t to = std::min(n, fromFrame + (int64_t)(20.0 * sr));
    AudioClip tail;
    tail.name = m_preview_clip.name;
    tail.sampleRate = m_preview_clip.sampleRate;
    tail.sourceSampleRate = m_preview_clip.sourceSampleRate;
    tail.ch[0].assign(m_preview_clip.ch[0].begin() + (size_t)fromFrame,
                      m_preview_clip.ch[0].begin() + (size_t)to);
    if ((int64_t)m_preview_clip.ch[1].size() >= to)
        tail.ch[1].assign(m_preview_clip.ch[1].begin() + (size_t)fromFrame,
                          m_preview_clip.ch[1].begin() + (size_t)to);
    else
        tail.ch[1] = tail.ch[0];
    if (on_audition_clip) on_audition_clip(tail);
    else PatchKnob::app::audio_app_preview_clip(tail, 1.0f);
    m_prev_playing = true;
    m_prev_ms0 = (Uint32)SDL_GetTicks();
    m_prev_from = fromFrame;
    m_prev_last_x = -1;
    app.request_redraw();
}

void SamplerEditorView::draw_keymap_zones(App& app, const SDL_Rect& grid) {
    if (!m_zones) return;
    const Theme& t = theme();
    const int ch = app.mono.ch();

        // O(zones) per frame with a big soundfont loaded (2976 zones for one
        // concert grand), so this loop is written to stay cheap:
        //   * zones entirely OUTSIDE the visible key window are culled -- with
        //     the map zoomed to an octave that is ~95% of them;
        //   * the unselected majority is batched into a handful of
        //     SDL_Render*Rects calls instead of 4-5 renderer calls per zone
        //     (measured ~7.5 ms/frame of the old per-zone calls at 2976 zones);
        //   * selected/focused zones and labels still paint individually, on
        //     top, where their highlights belong anyway.
        const int keyLo = m_key_first, keyHi = m_key_first + m_key_visible;
        // Selected zones pulled out once: the "hide overlaps" filter used to
        // re-walk the selection set per zone (O(zones x selected) per frame).
        std::vector<const SamplerZone*> selZones;
        if (!m_show_overlaps)
            for (int si : m_selected)
                if (si >= 0 && si < (int)m_zones->size())
                    selZones.push_back(&(*m_zones)[(size_t)si]);
        std::vector<SDL_Rect> bodies, bodiesOff, bodiesSel, frames, framesSel, roots;
        std::vector<size_t> special, labeled;
        bodies.reserve(m_zones->size());
        for (size_t i = 0; i < m_zones->size(); ++i) {
            const bool sel = m_selected.count((int)i) != 0;
            if (m_solo_selection && !sel) continue;
            const SamplerZone& z = (*m_zones)[i];
            if (z.hiKey < keyLo || z.loKey >= keyHi) continue;   // off-window
            if (!m_show_overlaps && !sel) {
                bool overlaps=false;
                for(const SamplerZone* s:selZones)
                    if(z.loKey<=s->hiKey&&z.hiKey>=s->loKey&&z.loVel<=s->hiVel&&z.hiVel>=s->loVel){overlaps=true;break;}
                if(overlaps) continue;
            }
            const bool focused = (int)i == m_sel;
            if (focused) { special.push_back(i); continue; }
            int x0 = key_left_x(grid, z.loKey);
            int x1 = key_right_x(grid, z.hiKey);
            int y0 = vel_to_y(grid, z.hiVel + 1);
            int y1 = vel_to_y(grid, z.loVel);
            SDL_Rect zr{ x0, y0, std::max(3, x1 - x0), std::max(3, y1 - y0) };
            // Selected zones batch too (fill t.sel, frame t.accent): a
            // select-all on 2976 zones used to fall back to per-zone calls.
            (z.noteOffLayer ? bodiesOff : (sel ? bodiesSel : bodies)).push_back(zr);
            // Frame as four 1-px strips: SDL_RenderFillRects exists in both the
            // SDL2 build and the SDL3 compat shim; SDL_RenderDrawRects does not.
            std::vector<SDL_Rect>& fr = sel ? framesSel : frames;
            fr.push_back(SDL_Rect{ zr.x, zr.y, zr.w, 1 });
            fr.push_back(SDL_Rect{ zr.x, zr.y + zr.h - 1, zr.w, 1 });
            fr.push_back(SDL_Rect{ zr.x, zr.y + 1, 1, zr.h - 2 });
            fr.push_back(SDL_Rect{ zr.x + zr.w - 1, zr.y + 1, 1, zr.h - 2 });
            roots.push_back(SDL_Rect{ key_center_x(grid, z.root), zr.y, 1, zr.h });
            if (zr.w > 54 && zr.h > ch + 4) labeled.push_back(i);
        }
        if (!bodies.empty()) {
            set_color(app.ren, t.active);
            SDL_RenderFillRects(app.ren, bodies.data(), (int)bodies.size());
        }
        if (!bodiesOff.empty()) {
            set_color(app.ren, t.chord);
            SDL_RenderFillRects(app.ren, bodiesOff.data(), (int)bodiesOff.size());
        }
        if (!bodiesSel.empty()) {
            set_color(app.ren, t.sel);
            SDL_RenderFillRects(app.ren, bodiesSel.data(), (int)bodiesSel.size());
        }
        if (!frames.empty()) {
            set_color(app.ren, t.dim);
            SDL_RenderFillRects(app.ren, frames.data(), (int)frames.size());
        }
        if (!framesSel.empty()) {
            set_color(app.ren, t.accent);
            SDL_RenderFillRects(app.ren, framesSel.data(), (int)framesSel.size());
        }
        if (!roots.empty()) {
            set_color(app.ren, t.hi);
            SDL_RenderFillRects(app.ren, roots.data(), (int)roots.size());
        }
        for (size_t i : labeled) {
            const SamplerZone& z = (*m_zones)[i];
            const int x0 = key_left_x(grid, z.loKey), x1 = key_right_x(grid, z.hiKey);
            const int y0 = vel_to_y(grid, z.hiVel + 1);
            SDL_Rect tx{ x0 + 5, y0 + 3, std::max(3, x1 - x0) - 10, ch + 2 };
            app.mono.draw_fitted(app.ren, tx, z.name.empty()?"zone":z.name, t.bg, false, 1.f, .55f, true);
        }
        for (size_t i : special) {
            const SamplerZone& z = (*m_zones)[i];
            int x0 = key_left_x(grid, z.loKey);
            int x1 = key_right_x(grid, z.hiKey);
            int y0 = vel_to_y(grid, z.hiVel + 1);
            int y1 = vel_to_y(grid, z.loVel);
            SDL_Rect zr{ x0, y0, std::max(3, x1 - x0), std::max(3, y1 - y0) };
            const bool focused = true;   // only the focused zone is special now
            fill_rect(app.ren, zr, z.noteOffLayer ? t.chord : t.accent);
            frame_rect(app.ren, zr, t.hi);
            int rx = key_center_x(grid, z.root);
            vline(app.ren, rx, zr.y, zr.y + zr.h - 1, t.bg);
            if (focused) {
                fill_rect(app.ren, SDL_Rect{zr.x, zr.y, 4, zr.h}, t.hi);
                fill_rect(app.ren, SDL_Rect{zr.x+zr.w-4, zr.y, 4, zr.h}, t.hi);
                fill_rect(app.ren, SDL_Rect{zr.x, zr.y, zr.w, 3}, t.hi);
                fill_rect(app.ren, SDL_Rect{zr.x, zr.y+zr.h-3, zr.w, 3}, t.hi);
            }
            if (zr.w > 54 && zr.h > ch + 4) {
                SDL_Rect tx{zr.x+5,zr.y+3,zr.w-10,ch+2};
                app.mono.draw_fitted(app.ren,tx,z.name.empty()?"zone":z.name,t.bg,false,1.f,.55f,true);
            }
        }
    }

void SamplerEditorView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    const int cw = app.mono.cw() ? app.mono.cw() : 6, ch = app.mono.ch();
    m_keymap_ch = ch;
    m_tip = nullptr;

    // Hover state must be POLLED.  SDL only delivers motion events while a
    // button is HELD, so every "hot" highlight in this view used to light up
    // whatever was last clicked and stay there until the next click.
    {
        int hx = -1, hy = -1;
        ui::mouse_logical(app, hx, hy);
        m_hover_in = hit(hx, hy);
        if (m_hover_in) { m_mx = hx; m_my = hy; }
    }

    auto button = [&](SDL_Rect r, const char* label, bool hot) {
        fill_rect(app.ren, r, hot ? t.accent : t.panel);
        frame_rect(app.ren, r, hot ? t.hi : t.dim);
        SDL_Rect tx{ r.x + 5, r.y + 2, r.w - 10, r.h - 4 };
        app.mono.draw_fitted(app.ren, tx, label, hot ? t.bg : t.text,
                             true, 0.72f, 0.30f, true);
    };
    auto pane = [&](SDL_Rect r, const char* title) {
        fill_rect(app.ren, r, t.panel);
        frame_rect(app.ren, r, t.dim);
        SDL_Rect head{ r.x, r.y, r.w, ch + 8 };
        fill_rect(app.ren, head, t.bg);
        hline(app.ren, r.x, r.x + r.w - 1, head.y + head.h - 1, t.dim);
        SDL_Rect tx{ r.x + 6, r.y + 1, r.w - 12, head.h - 2 };
        app.mono.draw_fitted(app.ren, tx, title, t.accent, false);
    };
    // --- toolbar ------------------------------------------------------------
    SDL_Rect bar{ rect.x, rect.y, rect.w, 30 };
    fill_rect(app.ren, bar, t.panel);
    // Button widths come from the MEASURED label + padding.  `12 * cw` is a
    // guess at 12 nominal advances: at a fractional UI scale the real text is
    // wider and "DELETE ZONE" lost its last characters inside its own box.
    auto tbtn = [&](SDL_Rect& r, int x, const char* label, const char* tip) {
        r = SDL_Rect{ x, rect.y + 4, app.mono.text_w(label) + 2 * cw, 22 };
        const bool hot = m_hover_in && in_rect(r, m_mx, m_my);
        button(r, label, hot);
        if (hot) { m_tip = tip; m_tip_anchor = r; }
        return r.x + r.w + 5;
    };
    int tbx = rect.x + 8;
    tbx = tbtn(m_btn_load, tbx, "REPLACE WAV", "Load a WAV into the selected zone");
    tbx = tbtn(m_btn_add,  tbx, "ADD ZONE",    "Load a WAV as a new zone");
    tbx = tbtn(m_btn_del,  tbx, "DELETE ZONE", "Delete the selected zones (Del, undoable with Ctrl+Z)");
    char summary[160];
    const int zoneCount = m_zones ? (int)m_zones->size() : 0;
    std::snprintf(summary, sizeof(summary),
                  "%d zones  %d selected  %s  root:%s   right-click for actions",
                  zoneCount, (int)m_selected.size(), m_move_lock ? "LOCKED" : "EDIT",
                  m_move_root ? "moves" : "fixed");
    // A drop reports what it did (and a slow load says it is working) in the
    // one place that is always visible.  It expires: it is a status, not a log.
    if (!m_drop_status.empty() &&
        (Uint32)SDL_GetTicks() - m_drop_status_ms > 4000u) m_drop_status.clear();
    const SDL_Rect statusBox{ tbx + 9, rect.y + 4,
                              std::max(20, rect.x + rect.w - tbx - 17), 22 };
    if (m_drop_status.empty())
        app.mono.draw_fitted(app.ren, statusBox, summary, t.accent, false);
    else
        app.mono.draw_fitted(app.ren, statusBox, m_drop_status, t.hi, false);

    const SDL_Rect view{ rect.x, rect.y + 32, rect.w - 12, std::max(1, rect.h - 32) };
    if (m_scroll_y < 0) m_scroll_y = 0;
    if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;
    ui::ScopedClip clipScope(app.ren,view);

    const int topY = rect.y + 36 - m_scroll_y;
    // --- zone list (left) ---------------------------------------------------
    const int listW = std::max(250, std::min(340, rect.w * 30 / 100));
    const int gap = 10;
    SDL_Rect list{ rect.x + 6, topY, listW, std::max(150, std::min(220, rect.h / 3)) };
    pane(list, "KEYZONE LAYERS");
    m_zone_list_rect = list;              // drop target for "append a new zone"
    m_zone_rows.clear();
    m_zone_row_indices.clear();
    if (m_zones) {
        const int zrH = std::max(22, ch + 8);
        int y = list.y + ch + 12;
        const int visible = std::max(1, (list.y + list.h - y - 3) / zrH);
        const int maxStart = std::max(0, (int)m_zones->size() - visible);
        m_zone_scroll = std::max(0, std::min(m_zone_scroll, maxStart));
        for (int vi = 0; vi < visible && m_zone_scroll + vi < (int)m_zones->size(); ++vi) {
            const size_t i = (size_t)(m_zone_scroll + vi);
            SDL_Rect row{ list.x + 3, y, list.w - 6, zrH };
            m_zone_rows.push_back(row);
            m_zone_row_indices.push_back((int)i);
            const SamplerZone& z = (*m_zones)[i];
            const bool focused = (int)i == m_sel;
            const bool sel = m_selected.count((int)i) != 0;
            const bool hot = m_hover_in && in_rect(row, m_mx, m_my);
            if (focused) fill_rect(app.ren, row, t.accent);
            else if (sel) fill_rect(app.ren, row, t.sel);
            else if (hot) fill_rect(app.ren, row, t.panel);
            else if ((i & 1u) != 0u) fill_rect(app.ren, row, t.bg);
            char lo[8], hi[8], rt[8]; note_name(z.loKey, lo, 8); note_name(z.hiKey, hi, 8); note_name(z.root, rt, 8);
            // The facts are laid out from the RIGHT at their measured width and
            // the name takes what is left, fitted by measurement.  The old
            // "%02d %-14.14s ..." truncated the name at 14 BYTES (mangling any
            // non-ASCII name) and still overflowed the row at a fractional scale.
            char facts[80], vel[16];
            // The velocity band is part of the mapping; a full-range band is
            // the default and stays quiet so drum rows do not drown in noise.
            vel[0] = 0;
            if (z.loVel > 0 || z.hiVel < 127)
                std::snprintf(vel, sizeof(vel), " v%d-%d", z.loVel, z.hiVel);
            std::snprintf(facts, sizeof(facts), "%s-%s%s root %s%s%s", lo, hi, vel, rt,
                          z.loop ? " LOOP" : "", z.reverse ? " REV" : "");
            char idx[8]; std::snprintf(idx, sizeof(idx), "%02d ", (int)i + 1);
            const int factW = app.mono.text_w(facts), idxW = app.mono.text_w(idx);
            const Color fg = focused ? t.bg : (sel ? t.hi : t.text);
            const int ty = row.y + (row.h - ch) / 2;
            app.mono.draw(app.ren, row.x + 6, ty, idx, fg);
            app.mono.draw(app.ren, row.x + 6 + idxW, ty,
                          fit_text(app.mono, z.name.empty() ? "(unnamed)" : z.name,
                                   std::max(0, row.w - 12 - idxW - factW - cw)), fg);
            app.mono.draw(app.ren, row.x + row.w - 6 - factW, ty, facts,
                          focused ? t.bg : t.dim);
            y += zrH;
        }
        if (maxStart > 0) {
            SDL_Rect tr{list.x + list.w - 5, list.y + ch + 12, 3, list.h - ch - 15};
            fill_rect(app.ren, tr, t.bg);
            const int th = std::max(12, tr.h * visible / std::max(1, (int)m_zones->size()));
            SDL_Rect tb{tr.x, tr.y + (tr.h - th) * m_zone_scroll / maxStart, tr.w, th};
            fill_rect(app.ren, tb, t.accent);
        }
        if (m_zones->empty())
            app.mono.draw(app.ren, list.x + 6, list.y + ch + 14, "drop WAVs here", t.dim);
    }

    // --- disk browser + sample preview (left lower pane) -------------------
    m_browser_rect = SDL_Rect{ list.x, list.y + list.h + gap, list.w,
                               std::max(180, view.h - list.h - gap - 4) };
    char browserTitle[80];
    std::snprintf(browserTitle, sizeof(browserTitle), "SAMPLE LIBRARY   %d items", (int)m_browser.size());
    pane(m_browser_rect, browserTitle);
    m_drive_rect = SDL_Rect{ m_browser_rect.x + 4, m_browser_rect.y + ch + 7,
                             std::min(8 * cw, m_browser_rect.w / 3), ch + 6 };
    fill_rect(app.ren, m_drive_rect, m_drive_menu ? t.accent : t.bg);
    frame_rect(app.ren, m_drive_rect, t.dim);
    std::filesystem::path browserPath(m_browser_dir.empty() ? "." : m_browser_dir);
    std::string driveLabel = browserPath.root_name().string();
    if (driveLabel.empty()) driveLabel = browserPath.root_path().string();
    if (driveLabel.empty()) driveLabel = "root";
    SDL_Rect driveTx{ m_drive_rect.x + 4, m_drive_rect.y + 1,
                      m_drive_rect.w - cw - 8, m_drive_rect.h - 2 };
    app.mono.draw_fitted(app.ren, driveTx, driveLabel, m_drive_menu ? t.bg : t.text, false);
    app.mono.draw(app.ren, m_drive_rect.x + m_drive_rect.w - cw - 3, m_drive_rect.y + 3,
                  "v", m_drive_menu ? t.bg : t.accent);
    SDL_Rect dirTx{ m_drive_rect.x + m_drive_rect.w + 6, m_browser_rect.y + ch + 5,
                    m_browser_rect.w - m_drive_rect.w - 14, ch + 6 };
    app.mono.draw_fitted(app.ren, dirTx, m_browser_dir, t.dim, false, 1.f, 0.75f, true);

    const int browY = m_browser_rect.y + 18 + 2 * ch;
    const int previewH = std::max(72, std::min(100, m_browser_rect.h / 4));
    const int rowsH = std::max(20, m_browser_rect.h - (browY - m_browser_rect.y) - previewH - 8);
    SDL_Rect browserRows{ m_browser_rect.x + 2, browY, m_browser_rect.w - 4, rowsH };
    m_browser_list_rect = browserRows;
    fill_rect(app.ren, browserRows, t.bg);
    m_browser_rows.clear();
    int first = std::max(0, m_browser_scroll);
    int rowH = ch + 4;
    int visibleRows = rowsH / rowH;
    m_browser_visible = std::max(1, visibleRows);
    m_browser_scroll = std::max(0, std::min(m_browser_scroll,
        std::max(0, (int)m_browser.size() - m_browser_visible)));
    for (int i = 0; i < visibleRows && first + i < (int)m_browser.size(); ++i) {
        int idx = first + i;
        SDL_Rect row{ browserRows.x + 1, browserRows.y + i * rowH, browserRows.w - 10, rowH };
        m_browser_rows.push_back(row);
        const BrowserItem& bi = m_browser[idx];
        bool sel = idx == m_browser_sel;
        const bool hot = in_rect(row, m_mx, m_my);
        if (sel) fill_rect(app.ren, row, t.accent);
        else if (hot) fill_rect(app.ren, row, t.panel);
        std::string nm = bi.dir ? ("+  " + bi.name) : ("~  " + bi.name);
        SDL_Rect tx{ row.x + 3, row.y + 1, row.w - 6, row.h - 2 };
        app.mono.draw_fitted(app.ren, tx, nm, sel ? t.bg : (bi.dir ? t.accent : t.text), false,
                             1.f, 0.65f, true);
    }
    if (m_browser.empty() || (!m_browser_msg.empty() && m_browser.size() <= 1))
        app.mono.draw(app.ren, browserRows.x + 6, browserRows.y + 4,
                      fit_text(app.mono, m_browser_msg.empty() ? "empty folder" : m_browser_msg,
                               browserRows.w - 12), t.dim);
    frame_rect(app.ren, browserRows, t.dim);
    const int browserMaxScroll=std::max(0,(int)m_browser.size()-m_browser_visible);
    m_browser_scroll_track=SDL_Rect{browserRows.x+browserRows.w-7,browserRows.y+2,5,browserRows.h-4};
    m_browser_scroll_thumb=SDL_Rect{0,0,0,0};
    if(browserMaxScroll>0){
        fill_rect(app.ren,m_browser_scroll_track,t.panel);
        const int th=std::max(14,m_browser_scroll_track.h*m_browser_visible/std::max(1,(int)m_browser.size()));
        m_browser_scroll_thumb=SDL_Rect{m_browser_scroll_track.x,
            m_browser_scroll_track.y+(m_browser_scroll_track.h-th)*m_browser_scroll/browserMaxScroll,
            m_browser_scroll_track.w,th};
        fill_rect(app.ren,m_browser_scroll_thumb,t.accent);
    }
    m_drive_rows.clear();
    m_drive_labels.clear();
    if (m_drive_menu) {
        const std::vector<std::string> drives = drive_roots();
        const int dh = ch + 6;
        SDL_Rect menu{ m_drive_rect.x, m_drive_rect.y + m_drive_rect.h, m_drive_rect.w,
                       std::min((int)drives.size() * dh + 2, std::max(dh + 2, browserRows.h)) };
        fill_rect(app.ren, menu, t.panel);
        frame_rect(app.ren, menu, t.accent);
        for (size_t i = 0; i < drives.size(); ++i) {
            SDL_Rect row{ menu.x + 1, menu.y + 1 + (int)i * dh, menu.w - 2, dh };
            if (row.y + row.h > menu.y + menu.h) break;
            m_drive_rows.push_back(row);
            m_drive_labels.push_back(drives[i]);
            bool hot = in_rect(row, m_mx, m_my);
            if (hot) fill_rect(app.ren, row, t.accent);
            SDL_Rect tx{ row.x + 4, row.y + 1, row.w - 8, row.h - 2 };
            app.mono.draw_fitted(app.ren, tx, drives[i], hot ? t.bg : t.text, false);
        }
    }
    m_preview_rect = SDL_Rect{ m_browser_rect.x + 2, browserRows.y + browserRows.h + 4,
                               m_browser_rect.w - 4, previewH };
    if (m_preview_ok) {
        draw_wave(app, m_preview_rect, m_preview_clip);
        // What you are about to load, in words: length, rate and channel count
        // were nowhere on screen, so two takes of the same name were
        // indistinguishable until after you had loaded one.
        const int64_t pn = m_preview_clip.safeFrames();
        const double psr = m_preview_clip.sampleRate > 0 ? m_preview_clip.sampleRate : 48000.0;
        const bool mono = m_preview_mono;   // decided once in rebuild_preview_peaks()
        char info[96];
        std::snprintf(info, sizeof(info), "%.2fs  %.0f Hz  %s",
                      (double)pn / psr, psr, mono ? "mono" : "stereo");
        const int iw = app.mono.text_w(info);
        SDL_Rect ib{ m_preview_rect.x + m_preview_rect.w - iw - 6, m_preview_rect.y + 2,
                     iw + 4, ch + 2 };
        if (ib.w < m_preview_rect.w - 8) {
            BlendScope blend(app.ren);
            fill_rect(app.ren, ib, fade(t.bg, 200));
            app.mono.draw(app.ren, ib.x + 2, ib.y + 1, info, t.accent);
        }
        if (m_hover_in && in_rect(m_preview_rect, m_mx, m_my)) {
            m_tip = "click to play from here   space replays   drag to the key map to load";
            m_tip_anchor = m_preview_rect;
        }
    } else {
        fill_rect(app.ren, m_preview_rect, t.bg);
        frame_rect(app.ren, m_preview_rect, t.dim);
        SDL_Rect tx{ m_preview_rect.x + 5, m_preview_rect.y + 2,
                     m_preview_rect.w - 10, m_preview_rect.h - 4 };
        app.mono.draw_fitted(app.ren, tx, "Select a WAV to preview - drag it onto the key map", t.dim, true);
    }

    const int rightX = list.x + list.w + gap;
    const int rightW = rect.x + rect.w - rightX - 16;

    if (m_inlineEdit) {
        m_inlineRect = SDL_Rect{rightX, topY, rightW, std::max(80, view.h - 6)};
        m_inlineEditor.rect = m_inlineRect;
        m_inlineEditor.draw(app);
        m_scroll_max = 0;
        m_scroll_track = m_scroll_thumb = SDL_Rect{0,0,0,0};
        draw_drag_ghost(app);          // still on top in the full-editor layout
        return;
    }
    m_inlineRect = SDL_Rect{0,0,0,0};

    // --- Renoise-style keyzone grid: notes X, velocity Y --------------------
    m_keyzone_grid = SDL_Rect{ rightX, topY, rightW, std::max(170, std::min(210, rect.h / 3)) };
    pane(m_keyzone_grid, "KEY MAP   horizontal: note range / root    vertical: velocity range");
    const SDL_Rect grid = keymap_grid();          // the ONE rect hit tests use
    fill_rect(app.ren, grid, t.bg);
    frame_rect(app.ren, grid, t.dim);
    for (int oct = 0; oct <= 10; ++oct) {
        int x = key_left_x(grid, oct * 12);
        vline(app.ren, x, grid.y, grid.y + grid.h - 1, t.dim);
        char octName[8]; std::snprintf(octName, sizeof(octName), "C%d", oct - 1);
        app.mono.draw(app.ren, x + 3, grid.y + grid.h - ch - 1, octName, t.dim);
    }
    for (int vv = 0; vv <= 4; ++vv) {
        // Through vel_to_y so the gridlines land exactly where the zone bands
        // and the drag maths put the same velocity.
        int y = vel_to_y(grid, vv * 32);
        hline(app.ren, grid.x, grid.x + grid.w - 1, y, t.dim);
        char vel[8]; std::snprintf(vel, sizeof(vel), "%d", vv * 32);
        app.mono.draw(app.ren, grid.x + 3, std::max(grid.y, y - ch), vel, t.dim);
    }
    // Zone layer: culled to the visible key window and batched into a handful
    // of SDL_RenderFillRects calls (see draw_keymap_zones).  Measured 1.9-3.6
    // ms/frame at 2976 zones on the SOFTWARE renderer, the slowest backend.  A
    // cached-texture layer was tried and measured: steady frames dropped to
    // ~1.1 ms, but rendering the layer INTO a target texture cost ~40 ms
    // full-view and multiple SECONDS zoomed-in (SDL's software backend
    // converts per glyph for the zone labels), so the cache lost everywhere
    // it mattered and was removed.
    if (m_zones) draw_keymap_zones(app, grid);
    // The drag highlight is NOT painted here: it belongs on top of the waveform
    // editor too, so it is drawn with the ghost at the very end of draw().
    m_strip = SDL_Rect{grid.x,m_keyzone_grid.y+m_keyzone_grid.h-28,grid.w,14};
    fill_rect(app.ren, m_strip, t.bg);
    const int labelEvery=m_key_visible<=48?1:(m_key_visible<=80?2:12);
    for(int key=m_key_first;key<std::min(128,m_key_first+m_key_visible);++key) {
        const int x=key_left_x(m_strip,key),x1=key_right_x(m_strip,key);
        if(key%12==0)vline(app.ren,x,m_strip.y,m_strip.y+m_strip.h-1,t.dim);
        if((key-m_key_first)%labelEvery==0) {
            char nn[8];note_name(key,nn,8);
            SDL_Rect nr{x+1,m_strip.y,std::max(2,x1-x-2),m_strip.h};
            app.mono.draw_fitted(app.ren,nr,nn,t.text,true,1.f,.45f,true);
        }
    }
    m_key_scroll_track=SDL_Rect{grid.x,m_strip.y+m_strip.h+2,grid.w,8};
    fill_rect(app.ren,m_key_scroll_track,t.panel);frame_rect(app.ren,m_key_scroll_track,t.dim);
    const int thumbW=std::max(18,m_key_scroll_track.w*m_key_visible/128);
    const int travel=std::max(1,m_key_scroll_track.w-thumbW);
    const int maxFirst=std::max(1,128-m_key_visible);
    m_key_scroll_thumb=SDL_Rect{m_key_scroll_track.x+travel*m_key_first/maxFirst,
                               m_key_scroll_track.y,thumbW,m_key_scroll_track.h};
    fill_rect(app.ren,m_key_scroll_thumb,t.accent);
    if(m_zone_marquee){fill_rect(app.ren,m_marquee_rect,t.sel);frame_rect(app.ren,m_marquee_rect,t.hi);}

    // --- selected-zone mapping controls ------------------------------------
    // Keep the numerical mapping immediately under the graphical key map, so
    // precise one-step edits do not compete with its small drag handles.
    m_zone_mapping_strip = SDL_Rect{rightX, m_keyzone_grid.y + m_keyzone_grid.h + gap,
                                    rightW, 58};
    pane(m_zone_mapping_strip, "SELECTED ZONE MAPPING");
    m_btn_lo_dec=m_btn_lo_inc=m_btn_hi_dec=m_btn_hi_inc=
    m_btn_root_dec=m_btn_root_inc=m_btn_vel_lo_dec=m_btn_vel_lo_inc=
    m_btn_vel_hi_dec=m_btn_vel_hi_inc=SDL_Rect{0,0,0,0};
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        const SamplerZone& z=(*m_zones)[m_sel];
        const int y=m_zone_mapping_strip.y+ch+11;
        const int unit=std::max(62,(rightW-12)/5), bw=22;
        auto mapping_control=[&](int column,const char* label,const char* value,
                                 SDL_Rect& dec,SDL_Rect& inc) {
            const int x=m_zone_mapping_strip.x+6+column*unit;
            dec=SDL_Rect{x,y,bw,22}; inc=SDL_Rect{x+unit-bw-6,y,bw,22};
            button(dec,"<",in_rect(dec,m_mx,m_my));
            button(inc,">",in_rect(inc,m_mx,m_my));
            SDL_Rect tx{x+bw+3,y,unit-2*bw-12,22};
            std::string text=std::string(label)+" "+value;
            app.mono.draw_fitted(app.ren,tx,text,t.text,true,1.f,.65f,true);
        };
        char lo[8],hi[8],root[8],vlo[8],vhi[8];
        note_name(z.loKey,lo,8);note_name(z.hiKey,hi,8);note_name(z.root,root,8);
        std::snprintf(vlo,sizeof(vlo),"%d",z.loVel);
        std::snprintf(vhi,sizeof(vhi),"%d",z.hiVel);
        mapping_control(0,"LOW",lo,m_btn_lo_dec,m_btn_lo_inc);
        mapping_control(1,"ROOT",root,m_btn_root_dec,m_btn_root_inc);
        mapping_control(2,"HIGH",hi,m_btn_hi_dec,m_btn_hi_inc);
        mapping_control(3,"VEL LOW",vlo,m_btn_vel_lo_dec,m_btn_vel_lo_inc);
        mapping_control(4,"VEL HIGH",vhi,m_btn_vel_hi_dec,m_btn_vel_hi_inc);
    }

    // --- per-zone parameter inspector ---------------------------------------
    // Engine-side zone parameters (SF2 parity): filter, tuning, level, choke,
    // mod-envelope routing, loop.  Slider rows in the same idiom as the machine
    // parameter sliders below; right-click restores the per-field default.
    m_insp_rows.assign(ZF_COUNT, SDL_Rect{0,0,0,0});
    m_btn_zloop = SDL_Rect{0,0,0,0};
    const int inspRowH = std::max(22, ch + 8);
    const int inspCells = ZF_COUNT + 1;              // fields + the loop toggle
    const int inspRowsN = (inspCells + 1) / 2;
    SDL_Rect insp{ rightX, m_zone_mapping_strip.y + m_zone_mapping_strip.h + gap,
                   rightW, ch + 14 + inspRowsN * inspRowH };
    pane(insp, "ZONE PARAMETERS   filter / tuning / level / choke / mod routing / loop");
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        const SamplerZone& z = (*m_zones)[(size_t)m_sel];
        const int colW = (insp.w - 12) / 2;
        const int labelW = app.mono.text_w("MOD>CUTOFF") + cw;
        for (int f = 0; f < inspCells; ++f) {
            const int col = f % 2, row = f / 2;
            const int bx = insp.x + 6 + col * colW;
            const int by = insp.y + ch + 11 + row * inspRowH;
            if (f == ZF_COUNT) {
                // loop mode: the one on/off among the sliders is a button
                m_btn_zloop = SDL_Rect{ bx, by + 2, labelW + 6 * cw, inspRowH - 5 };
                button(m_btn_zloop, z.loop ? "LOOP: ON" : "LOOP: OFF",
                       m_hover_in && in_rect(m_btn_zloop, m_mx, m_my));
                if (m_hover_in && in_rect(m_btn_zloop, m_mx, m_my)) {
                    m_tip = "loop the region between LOOP START and LOOP END";
                    m_tip_anchor = m_btn_zloop;
                }
                continue;
            }
            SDL_Rect track{ bx + labelW, by + 3, std::max(8, colW - labelW - 10), inspRowH - 6 };
            m_insp_rows[f] = track;
            app.mono.draw_fitted(app.ren, SDL_Rect{ bx, by, std::max(10, track.x - bx - 6), inspRowH },
                                 fit_text(app.mono, kZFieldName[f], std::max(10, track.x - bx - 6)),
                                 t.text, false);
            const float v = insp_get(z, f);
            fill_rect(app.ren, track, t.panel);
            fill_rect(app.ren, SDL_Rect{ track.x, track.y, (int)(track.w * v), track.h }, t.accent);
            frame_rect(app.ren, track, t.dim);
            char vs[24];
            insp_format(z, f, vs, sizeof(vs));
            const int vw = app.mono.text_w(vs);
            if (track.w > vw + 2 * cw)
                app.mono.draw(app.ren, track.x + track.w - vw - 3,
                              track.y + (track.h - ch) / 2, vs, v > 0.55f ? t.bg : t.text);
            if (m_hover_in && in_rect(track, m_mx, m_my)) {
                m_tip = "drag to set   shift: fine   right-click: default";
                m_tip_anchor = track;
            }
        }
    } else {
        app.mono.draw(app.ren, insp.x + 6, insp.y + ch + 14,
                      fit_text(app.mono, "no zone selected", insp.w - 12), t.dim);
    }

    // --- selected zone waveform + controls ---------------------------------
    const int editTop = insp.y + insp.h + gap;
    const int editH = std::max(280, std::min(360, rect.h * 40 / 100));
    SDL_Rect wavePane{ rightX, editTop, rightW, editH };
    // Retired zone button panel is placed outside the clipped view; those
    // commands are supplied to the waveform editor's context menu instead.
    SDL_Rect zonePane{ rect.x + rect.w + 64, editTop, 1, editH };
    m_wave = wavePane;
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        SamplerZone& z = (*m_zones)[m_sel];
        m_inlineEditor.rect = m_wave;
        m_inlineEditor.draw(app);
        // Zone actions are fused into the waveform context menu.  Do not paint
        // or hit-test the retired off-screen button bank.
        if (false) {
        char zoneInfo[128];
        char loName[8], hiName[8];
        note_name(z.loKey, loName, 8);
        note_name(z.hiKey, hiName, 8);
        int cy = zonePane.y + ch + 12;
        app.mono.draw(app.ren, zonePane.x + 6, cy, "KEY RANGE", t.accent);
        std::snprintf(zoneInfo, sizeof(zoneInfo), "%s .. %s", loName, hiName);
        app.mono.draw_fitted(app.ren,
                             SDL_Rect{ zonePane.x + 16 * cw, cy - 1,
                                       zonePane.w - 16 * cw - 8, ch + 4 },
                             zoneInfo, t.text, false);
        cy += ch + 6;
        m_btn_lo_dec = SDL_Rect{ zonePane.x + 6, cy, 5 * cw, 20 };
        m_btn_lo_inc = SDL_Rect{ m_btn_lo_dec.x + m_btn_lo_dec.w + 4, cy, 5 * cw, 20 };
        m_btn_hi_dec = SDL_Rect{ m_btn_lo_inc.x + m_btn_lo_inc.w + 8, cy, 5 * cw, 20 };
        m_btn_hi_inc = SDL_Rect{ m_btn_hi_dec.x + m_btn_hi_dec.w + 4, cy, 5 * cw, 20 };
        button(m_btn_lo_dec, "LO-", in_rect(m_btn_lo_dec, m_mx, m_my));
        button(m_btn_lo_inc, "LO+", in_rect(m_btn_lo_inc, m_mx, m_my));
        button(m_btn_hi_dec, "HI-", in_rect(m_btn_hi_dec, m_mx, m_my));
        button(m_btn_hi_inc, "HI+", in_rect(m_btn_hi_inc, m_mx, m_my));

        cy += 28;
        std::snprintf(zoneInfo, sizeof(zoneInfo), "gain %.2f  pan %.2f  trim %.2f..%.2f",
                      z.gain, z.pan, z.start, z.end);
        app.mono.draw(app.ren, zonePane.x + 6, cy, zoneInfo, t.dim);

        cy += ch + 6;
        m_btn_note_on  = SDL_Rect{ zonePane.x + 6, cy, 7 * cw, 20 };
        m_btn_note_off = SDL_Rect{ m_btn_note_on.x + m_btn_note_on.w + 4, cy, 8 * cw, 20 };
        m_btn_drumkit  = SDL_Rect{ m_btn_note_off.x + m_btn_note_off.w + 4, cy, 8 * cw, 20 };
        m_btn_distribute = SDL_Rect{ m_btn_drumkit.x + m_btn_drumkit.w + 4, cy, 10 * cw, 20 };
        m_btn_layer    = SDL_Rect{ zonePane.x + 6, cy + 24, 8 * cw, 20 };
        m_btn_keypitch = SDL_Rect{ m_btn_layer.x + m_btn_layer.w + 4, cy + 24, 10 * cw, 20 };
        m_btn_velvol   = SDL_Rect{ m_btn_keypitch.x + m_btn_keypitch.w + 4, cy + 24, 9 * cw, 20 };
        m_btn_overlap  = SDL_Rect{ zonePane.x + 6, cy + 48, 11 * cw, 20 };
        button(m_btn_note_on,  z.noteOffLayer ? "ON" : "ON LYR", in_rect(m_btn_note_on, m_mx, m_my));
        button(m_btn_note_off, z.noteOffLayer ? "OFF LYR" : "OFF", in_rect(m_btn_note_off, m_mx, m_my));
        button(m_btn_drumkit, "DRUM", in_rect(m_btn_drumkit, m_mx, m_my));
        button(m_btn_distribute, "SPREAD", in_rect(m_btn_distribute, m_mx, m_my));
        button(m_btn_layer, "VEL LYR", in_rect(m_btn_layer, m_mx, m_my));
        button(m_btn_keypitch, z.keyToPitch ? "PITCH" : "FIXED", in_rect(m_btn_keypitch, m_mx, m_my));
        button(m_btn_velvol, z.velToVol ? "VEL VOL" : "FIX VOL", in_rect(m_btn_velvol, m_mx, m_my));
        const char* om = z.overlapMode == 1 ? "CYCLE" : (z.overlapMode == 2 ? "RANDOM" : "ALL");
        button(m_btn_overlap, om, in_rect(m_btn_overlap, m_mx, m_my));

        const int py0 = zonePane.y + zonePane.h - 48;
        m_btn_rev  = SDL_Rect{ zonePane.x + 6, py0, 7 * cw, 20 };
        m_btn_loop = SDL_Rect{ m_btn_rev.x + m_btn_rev.w + 4, py0, 7 * cw, 20 };
        // Destructive WAV tools live in the shared editor's contextual menu.
        // Keep playback switches here and replace six cramped duplicate buttons
        // with one obvious entry point to the complete editor.
        m_btn_crop = SDL_Rect{ m_btn_loop.x + m_btn_loop.w + 4, py0, 12 * cw, 20 };
        m_btn_norm = m_btn_dc = m_btn_zerotrim = m_btn_fadein = m_btn_fadeout = SDL_Rect{0,0,0,0};
        button(m_btn_rev,  z.reverse ? "REV ON" : "REV", in_rect(m_btn_rev,  m_mx, m_my));
        button(m_btn_loop, z.loop ? "LOOP ON" : "LOOP", in_rect(m_btn_loop, m_mx, m_my));
        button(m_btn_crop, "EDIT WAV...", in_rect(m_btn_crop, m_mx, m_my));
        }
    } else { fill_rect(app.ren, m_wave, t.bg); frame_rect(app.ren, m_wave, t.dim); }

    // --- envelope editor (amp/pitch/cutoff/res/pan, per-stage curves) ------
    SDL_Rect envR{ rightX, editTop + editH + gap, rightW, 150 };
    draw_env(app, envR);

    // --- parameter sliders (the machine's exposed params) ------------------
    m_param_rows.clear();
    int py = envR.y + envR.h + 8;
    if (m_inst) {
        app.mono.draw(app.ren, rightX, py, "PARAMETERS (tracker FX)", t.accent); py += ch + 4;
        const int pc = m_inst->paramCount();
        const int rowH = std::max(20, ch + 8), cols = rightW >= 620 ? 2 : 1;
        const int colW = rightW / cols;
        const int labelW = std::min(colW / 2, app.mono.text_w("MMMMMMMMMMMMMM") + cw);
        for (int i = 0; i < pc; ++i) {
            PatchKnob::engine::ParamInfo pi = m_inst->paramInfo(i);
            const int col = i % cols, row = i / cols;
            const int bx = rightX + col * colW;
            const int by = py + row * rowH;
            SDL_Rect track{ bx + labelW, by + 3, std::max(8, colW - labelW - 10), rowH - 6 };
            // Hit rects are registered ONLY for rows that were actually painted.
            // Registering them all meant the sliders scrolled out of the viewport
            // were still grabbable through whatever was drawn on top of them.
            const bool visible = (by + rowH >= view.y && by <= view.y + view.h);
            m_param_rows.push_back(visible ? track : SDL_Rect{0,0,0,0});
            if (!visible) continue;
            app.mono.draw_fitted(app.ren,
                                 SDL_Rect{ bx, by, std::max(10, track.x - bx - 6), rowH },
                                 fit_text(app.mono, pi.name, std::max(10, track.x - bx - 6)),
                                 t.text, false);
            fill_rect(app.ren, track, t.panel);
            float v = m_inst->getParamNormalized(pi.id);
            v = std::max(0.f, std::min(1.f, v));
            SDL_Rect fill{ track.x, track.y, (int)(track.w * v), track.h };
            fill_rect(app.ren, fill, t.accent);
            frame_rect(app.ren, track, t.dim);
            // The VALUE, which the bar alone never told you.  Right-aligned so
            // the digits stay in one column as you drag.
            char vs[16]; std::snprintf(vs, sizeof(vs), "%3d%%", (int)std::lround(v * 100.f));
            const int vw = app.mono.text_w(vs);
            if (track.w > vw + 2 * cw)
                app.mono.draw(app.ren, track.x + track.w - vw - 3, track.y + (track.h - ch) / 2,
                              vs, v > 0.55f ? t.bg : t.text);
            if (m_hover_in && in_rect(track, m_mx, m_my)) {
                m_tip = "drag to set   shift: fine   right-click: default";
                m_tip_anchor = track;
            }
        }
        py += ((pc + cols - 1) / cols) * rowH;
    }

    const int contentBottom = py + 12 + m_scroll_y;
    m_scroll_max = std::max(0, contentBottom - (view.y + view.h));
    if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;

    m_scroll_track = SDL_Rect{ rect.x + rect.w - 10, view.y + 2, 7, view.h - 4 };
    m_scroll_thumb = SDL_Rect{0,0,0,0};
    if (m_scroll_max > 0 && m_scroll_track.h > 12) {
        fill_rect(app.ren, m_scroll_track, t.panel);
        frame_rect(app.ren, m_scroll_track, t.dim);
        const int contentH = view.h + m_scroll_max;
        int thumbH = std::max(18, (view.h * m_scroll_track.h) / std::max(1, contentH));
        if (thumbH > m_scroll_track.h) thumbH = m_scroll_track.h;
        int travel = std::max(1, m_scroll_track.h - thumbH);
        int thumbY = m_scroll_track.y + (m_scroll_y * travel) / std::max(1, m_scroll_max);
        m_scroll_thumb = SDL_Rect{ m_scroll_track.x + 1, thumbY, m_scroll_track.w - 2, thumbH };
        fill_rect(app.ren, m_scroll_thumb, in_rect(m_scroll_thumb, m_mx, m_my) ? t.hi : t.accent);
    }

    if (m_menu_open) draw_menu(app);

    // Tooltip last so it floats over everything, and only when no menu is up.
    if (m_tip && !m_menu_open) {
        const int tw = app.mono.text_w(m_tip) + 2 * cw;
        SDL_Rect box{ std::min(m_tip_anchor.x, view.x + view.w - tw - 2),
                      m_tip_anchor.y + m_tip_anchor.h + 2, tw, ch + 4 };
        if (box.y + box.h > view.y + view.h) box.y = m_tip_anchor.y - box.h - 2;
        if (box.x < view.x) box.x = view.x;
        fill_rect(app.ren, box, t.panel);
        frame_rect(app.ren, box, t.accent);
        app.mono.draw(app.ren, box.x + cw, box.y + 2, m_tip, t.text);
    }

    // Drop target + carried file, over every pane including the waveform editor.
    draw_drag_ghost(app);

    // Pointer shape says what a press will do before you commit to it.
    if (!m_hover_in) { if (g_active) want_cursor(0); }
    else {
        int want = 0;
        switch (m_drag) {
        case Drag::ZoneLo: case Drag::ZoneHi: case Drag::ZoneRoot: want = 1; break;
        case Drag::ZoneVelLo: case Drag::ZoneVelHi: want = 2; break;
        default: break;
        }
        if (!want && m_zones && in_rect(grid, m_mx, m_my)) {
            const int hk = key_at_x(m_mx, grid), hv = y_to_vel(grid, m_my);
            const int hz = zone_at(hk, hv);
            if (hz >= 0) switch (handle_at(grid, hz, m_mx, m_my)) {
                case Drag::ZoneLo: case Drag::ZoneHi: case Drag::ZoneRoot: want = 1; break;
                case Drag::ZoneVelLo: case Drag::ZoneVelHi: want = 2; break;
                default: break;
            }
        }
        want_cursor(want);
    }
}

//----------------------------------------------------------------------------
//  right-click menus
//
//  The key map, the zone list and the disk browser had NO context menu, and the
//  commands that used to be buttons were laid out at rect.x + rect.w + 64 --
//  outside the clipped viewport, so they were painted nowhere and could not be
//  clicked at all.  The view toggles were reachable only through undocumented
//  single letters (L/M/S/O).  Everything one-shot now lives here, where the
//  rest of this project puts it.
//----------------------------------------------------------------------------
void SamplerEditorView::open_menu(App& app, int x, int y, int zone, int key, int browserRow) {
    m_menu.clear();
    m_menu_zone = zone; m_menu_key = key; m_menu_browser = browserRow;
    const bool haveZone = m_zones && zone >= 0 && zone < (int)m_zones->size();
    const int nsel = (int)selected_indices().size();
    if (browserRow >= 0) {
        const bool isFile = browserRow < (int)m_browser.size() && !m_browser[browserRow].dir;
        m_menu.push_back({ "Audition", MB_AUDITION, isFile, false });
        m_menu.push_back({ "Load into selected zone", MB_LOAD_SEL,
                           isFile && m_zones && m_sel >= 0, false });
        m_menu.push_back({ "Add as new zone", MB_ADD_ZONE, isFile, false });
        m_menu.push_back({ "", 0, false, true });
        m_menu.push_back({ "Parent folder", MB_UP, true, false });
        m_menu.push_back({ "Rescan folder", MB_REFRESH, true, false });
    } else {
        m_menu.push_back({ "Rename zone...", MZ_RENAME, haveZone, false });
        m_menu.push_back({ "Duplicate zone            Ctrl+D", MZ_DUPLICATE, haveZone, false });
        m_menu.push_back({ "Delete zone               Del", MZ_DELETE, haveZone, false });
        m_menu.push_back({ "Edit waveform...          W", MZ_EDITWAV, haveZone, false });
        m_menu.push_back({ "", 0, false, true });
        m_menu.push_back({ "Set root to this key", MZ_ROOT_HERE, haveZone && key >= 0, false });
        m_menu.push_back({ "Shrink to one key (drum)", MZ_ONE_KEY, haveZone, false });
        m_menu.push_back({ "Spread selection over keys", MZ_SPREAD_SEL, nsel > 1, false });
        m_menu.push_back({ "Fill gaps between roots   F", MZ_FILL_SEL, nsel > 1, false });
        m_menu.push_back({ "Layer selection by velocity  V", MZ_LAYER_SEL, nsel > 1, false });
        m_menu.push_back({ "", 0, false, true });
        m_menu.push_back({ "Select all zones          Ctrl+A", MZ_SELECT_ALL, m_zones && !m_zones->empty(), false });
        m_menu.push_back({ "Select none", MZ_SELECT_NONE, nsel > 0, false });
        m_menu.push_back({ m_move_lock ? "Unlock editing        L" : "Lock editing          L",
                           MZ_LOCK, true, false });
        m_menu.push_back({ m_move_root ? "Root follows move     M" : "Root stays put        M",
                           MZ_MOVE_ROOT, true, false });
        m_menu.push_back({ m_solo_selection ? "Show all zones        S" : "Solo selected zones   S",
                           MZ_SOLO, true, false });
        m_menu.push_back({ m_show_overlaps ? "Hide overlapping      O" : "Show overlapping      O",
                           MZ_OVERLAPS, true, false });
        m_menu.push_back({ "", 0, false, true });
        m_menu.push_back({ "Undo                      Ctrl+Z", MZ_UNDO, !m_zone_undo.empty(), false });
        m_menu.push_back({ "Redo                      Ctrl+Y", MZ_REDO, !m_zone_redo.empty(), false });
    }
    const int ch = app.mono.ch();
    const int rowh = ch + 5, seph = 5;
    int wpx = 0, hpx = 4;
    for (const MenuRow& r : m_menu) {
        wpx = std::max(wpx, app.mono.text_w(r.label));
        hpx += r.separator ? seph : rowh;
    }
    wpx += 4 * (app.mono.cw() ? app.mono.cw() : 6);
    SDL_Rect box{ x, y, wpx, hpx };
    if (box.x + box.w > rect.x + rect.w - 12) box.x = rect.x + rect.w - 12 - box.w;
    if (box.y + box.h > rect.y + rect.h) box.y = std::max(rect.y, y - box.h);
    if (box.x < rect.x) box.x = rect.x;
    if (box.y < rect.y) box.y = rect.y;
    m_menu_box = box;
    m_menu_rows.clear();
    int yy = box.y + 2;
    for (const MenuRow& r : m_menu) {
        const int h = r.separator ? seph : rowh;
        m_menu_rows.push_back(SDL_Rect{ box.x, yy, box.w, h });
        yy += h;
    }
    m_menu_open = true;
    app.request_redraw();
}

void SamplerEditorView::draw_menu(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, m_menu_box, t.panel);
    frame_rect(app.ren, m_menu_box, t.accent);
    const int cw = app.mono.cw() ? app.mono.cw() : 6;
    for (size_t i = 0; i < m_menu.size() && i < m_menu_rows.size(); ++i) {
        const MenuRow& r = m_menu[i];
        const SDL_Rect& q = m_menu_rows[i];
        if (r.separator) { hline(app.ren, q.x + 3, q.x + q.w - 4, q.y + q.h / 2, t.dim); continue; }
        const bool hot = r.enabled && m_hover_in && in_rect(q, m_mx, m_my);
        if (hot) fill_rect(app.ren, q, t.accent);
        app.mono.draw(app.ren, q.x + cw, q.y + 2,
                      fit_text(app.mono, r.label, q.w - 2 * cw),
                      hot ? t.bg : (r.enabled ? t.text : t.dim));
    }
}

bool SamplerEditorView::menu_click(App& app, int x, int y) {
    if (!in_rect(m_menu_box, x, y)) { m_menu_open = false; app.request_redraw(); return true; }
    for (size_t i = 0; i < m_menu.size() && i < m_menu_rows.size(); ++i) {
        if (!in_rect(m_menu_rows[i], x, y)) continue;
        if (m_menu[i].separator || !m_menu[i].enabled) return true;
        const int id = m_menu[i].id;
        m_menu_open = false;
        run_menu(app, id);
        return true;
    }
    return true;
}

void SamplerEditorView::run_menu(App& app, int id) {
    auto changed = [&] {
        sanitize_selection();
        if (on_apply) on_apply();
        if (m_sel >= 0 && on_zone_selected) on_zone_selected(m_sel);
        app.request_redraw();
    };
    switch (id) {
    case MZ_RENAME:
        if (m_zones && m_menu_zone >= 0 && m_menu_zone < (int)m_zones->size()) {
            // Inline text entry, the toolkit's own mechanism -- a zone could be
            // named only by the file it came from, so four takes of "kick" were
            // four rows of "kick".
            zone_snapshot(true);
            m_rename_zone = m_menu_zone;
            m_rename_buf = (*m_zones)[(size_t)m_menu_zone].name;
            App* ap = &app;          // by VALUE: the reference parameter dies here
            app.begin_text(&m_rename_buf, nullptr, [this, ap](bool ok) {
                if (ok && m_zones && m_rename_zone >= 0 && m_rename_zone < (int)m_zones->size())
                    (*m_zones)[(size_t)m_rename_zone].name = m_rename_buf;
                m_rename_zone = -1;
                if (ok && on_apply) on_apply();
                if (ap) ap->request_redraw();
            });
        }
        break;
    case MZ_DUPLICATE:
        if (m_zones && m_menu_zone >= 0 && m_menu_zone < (int)m_zones->size()) {
            zone_snapshot();
            SamplerZone copy = (*m_zones)[(size_t)m_menu_zone];
            m_zones->push_back(std::move(copy));
            select_only((int)m_zones->size() - 1);
            changed();
        }
        break;
    case MZ_DELETE:
        if (m_zones && !selected_indices().empty()) {
            zone_snapshot();
            auto ids = selected_indices();
            for (auto it = ids.rbegin(); it != ids.rend(); ++it) m_zones->erase(m_zones->begin() + *it);
            select_only(std::min(m_sel, (int)m_zones->size() - 1));
            if (m_sel < 0) m_inlineEditor.set_slot(nullptr);
            changed();
        }
        break;
    case MZ_EDITWAV:
        if (on_edit_zone && m_menu_zone >= 0) { on_edit_zone(m_menu_zone); app.request_redraw(); }
        break;
    case MZ_ROOT_HERE:
        if (m_zones && m_menu_zone >= 0 && m_menu_zone < (int)m_zones->size() && m_menu_key >= 0) {
            zone_snapshot(true);
            SamplerZone& z = (*m_zones)[(size_t)m_menu_zone];
            z.root = std::max(z.loKey, std::min(z.hiKey, m_menu_key));
            changed();
        }
        break;
    case MZ_ONE_KEY:
        if (m_zones) {
            zone_snapshot(true);
            for (int i : selected_indices()) {
                SamplerZone& z = (*m_zones)[(size_t)i];
                z.loKey = z.hiKey = std::max(0, std::min(127, z.root));
            }
            changed();
        }
        break;
    case MZ_SPREAD_SEL:
        if (m_zones) {
            zone_snapshot(true);
            auto ids = selected_indices();
            const int n = (int)ids.size();
            for (int j = 0; j < n; ++j) {
                SamplerZone& z = (*m_zones)[(size_t)ids[j]];
                z.loKey = (j * 128) / n;
                z.hiKey = ((j + 1) * 128) / n - 1;
                z.root = (z.loKey + z.hiKey) / 2;
                clamp_zone_keys(z);
            }
            changed();
        }
        break;
    case MZ_FILL_SEL:
        if (m_zones) {
            zone_snapshot(true);
            auto ids = selected_indices();
            std::sort(ids.begin(), ids.end(),
                      [&](int a, int b){ return (*m_zones)[(size_t)a].root < (*m_zones)[(size_t)b].root; });
            for (size_t j = 0; j < ids.size(); ++j) {
                SamplerZone& z = (*m_zones)[(size_t)ids[j]];
                z.loKey = j ? (((*m_zones)[(size_t)ids[j-1]].root + z.root) / 2 + 1) : 0;
                z.hiKey = j + 1 < ids.size() ? ((z.root + (*m_zones)[(size_t)ids[j+1]].root) / 2) : 127;
                clamp_zone_keys(z);
            }
            changed();
        }
        break;
    case MZ_LAYER_SEL:
        if (m_zones) {
            zone_snapshot(true);
            auto ids = selected_indices();
            const int n = (int)ids.size();
            for (int j = 0; j < n; ++j) {
                SamplerZone& z = (*m_zones)[(size_t)ids[j]];
                z.loVel = j * 128 / n;
                z.hiVel = (j + 1) * 128 / n - 1;
            }
            changed();
        }
        break;
    case MZ_SELECT_ALL:
        if (m_zones) {
            m_selected.clear();
            for (int i = 0; i < (int)m_zones->size(); ++i) m_selected.insert(i);
            if (m_sel < 0 && !m_zones->empty()) m_sel = 0;
            app.request_redraw();
        }
        break;
    case MZ_SELECT_NONE: m_selected.clear(); sanitize_selection(); app.request_redraw(); break;
    case MZ_LOCK:      m_move_lock = !m_move_lock; app.request_redraw(); break;
    case MZ_MOVE_ROOT: m_move_root = !m_move_root; app.request_redraw(); break;
    case MZ_SOLO:      m_solo_selection = !m_solo_selection; app.request_redraw(); break;
    case MZ_OVERLAPS:  m_show_overlaps = !m_show_overlaps; app.request_redraw(); break;
    // Menu and Ctrl+Z share ONE implementation (on_undo), so the menu entry and
    // the shortcut it advertises can never disagree.  zone_history() skips the
    // waveform-pane branch: the menu item is explicitly the ZONE history.
    case MZ_UNDO: zone_history(app, false); break;
    case MZ_REDO: zone_history(app, true);  break;
    case MB_AUDITION:
        if (m_menu_browser >= 0) { select_browser(m_menu_browser, false); audition_preview(app, 0); }
        break;
    case MB_LOAD_SEL:
        if (on_load_path && m_menu_browser >= 0 && m_menu_browser < (int)m_browser.size())
            on_load_path(m_browser[(size_t)m_menu_browser].path, m_sel, -1);
        break;
    case MB_ADD_ZONE:
        if (on_load_path && m_menu_browser >= 0 && m_menu_browser < (int)m_browser.size())
            on_load_path(m_browser[(size_t)m_menu_browser].path, -1, -1);
        break;
    case MB_UP: {
        std::error_code ec;
        std::filesystem::path p(m_browser_dir.empty() ? "." : m_browser_dir);
        if (p.has_parent_path() && p.parent_path() != p) {
            m_browser_dir = p.parent_path().string();
            m_browser_scroll = 0;
            scan_browser();
        }
        (void)ec;
        app.request_redraw();
        break;
    }
    case MB_REFRESH: scan_browser(); app.request_redraw(); break;
    default: break;
    }
}

void SamplerEditorView::cancel_interaction(App& app) {
    // Abandon whatever was in flight and restore the pre-drag zone state: a drag
    // that ends because the window lost focus used to leave the zone wherever the
    // last motion event put it, with no way to tell that it had been moved.
    if (m_zones && !m_drag_origin.empty() &&
        (m_drag == Drag::ZoneMove || m_drag == Drag::ZoneLo || m_drag == Drag::ZoneHi ||
         m_drag == Drag::ZoneVelLo || m_drag == Drag::ZoneVelHi || m_drag == Drag::ZoneRoot)) {
        // The origin is metadata-only (a zone drag cannot touch PCM), so put
        // the fields back and leave each zone's audio where it already is.
        const size_t n = std::min(m_drag_origin.size(), m_zones->size());
        for (size_t i = 0; i < n; ++i) {
            SamplerZone& dst = (*m_zones)[i];
            PatchKnob::engine::AudioClip keep = std::move(dst.clip);
            dst = std::move(m_drag_origin[i]);
            dst.clip = std::move(keep);
        }
        if (!m_zone_undo.empty()) m_zone_undo.pop_back();   // the snapshot it took
        if (on_apply) on_apply();
    }
    m_drag = Drag::None;
    m_drag_origin.clear();
    m_param_drag = -1; m_env_node = -1; m_env_seg = -1;
    m_insp_drag = -1; m_zenv_drag = -1;
    // A library drag is abandoned WITHOUT loading anything: Esc, a release
    // delivered somewhere else, or the window losing focus all land here, and
    // leaving m_drag latched left the view convinced a file was still in flight
    // with no button down -- the next pointer move dropped it.
    m_drag_path.clear(); m_drag_name.clear();
    m_drag_target = DropTarget{}; m_drag_armed = false;
    m_leftDown = m_rightDown = false;
    m_scroll_drag = false; m_browser_scroll_drag = false;
    m_menu_open = false; m_drive_menu = false;
    app.request_redraw();
}

bool SamplerEditorView::on_mouse(App& app, const MouseEv& e) {
    m_mx = e.x; m_my = e.y;
    const bool leftNow = e.pressed && e.button == SDL_BUTTON_LEFT;
    const bool rightNow = e.pressed && e.button == SDL_BUTTON_RIGHT;
    const bool leftEdge = leftNow && !m_leftDown;
    const bool rightEdge = rightNow && !m_rightDown;
    if (!e.pressed) { m_leftDown = false; m_rightDown = false; }
    else if (e.button == SDL_BUTTON_LEFT) m_leftDown = true;
    else if (e.button == SDL_BUTTON_RIGHT) m_rightDown = true;
    // 32, matching draw().  on_mouse used 28 and draw used 32, so the four-pixel
    // band between them counted as "inside the scrolling view" for clicks but was
    // painted as toolbar -- presses there hit whatever the view had at y=28.
    const SDL_Rect view{ rect.x, rect.y + 32, rect.w - 12, std::max(1, rect.h - 32) };
    // A menu is modal-ish: the next press either picks a row or dismisses it.
    if (m_menu_open) {
        if (e.pressed && (leftEdge || rightEdge)) return menu_click(app, e.x, e.y);
        if (e.pressed) return true;
    }
    // An interaction belongs to the pane where it STARTED.  In particular a
    // keyzone drag crossing the waveform must never become a wave selection.
    if (m_drag == Drag::None && m_param_drag < 0 && !m_scroll_drag &&
        m_inlineEditor.slot() && (in_rect(m_wave, e.x, e.y) || m_inlineEditor.mouse_capture_active())) {
        if (leftEdge || rightEdge) { m_wave_focus = true; m_browser_active = false; }
        return m_inlineEditor.on_mouse(app, e);
    }
    if (leftEdge || rightEdge) m_wave_focus = false;
    if (m_inlineEdit && e.pressed && in_rect(m_btn_load, e.x, e.y)) {
        m_inlineEdit = false; m_inlineEditor.set_slot(nullptr); app.request_redraw(); return true;
    }
    if (m_inlineEdit && in_rect(m_inlineRect, e.x, e.y))
        return m_inlineEditor.on_mouse(app, e);
    if (!e.pressed) {
        m_browser_scroll_drag=false;
        m_key_scroll_drag=false;
        if(m_zone_marquee) {
            m_zone_marquee=false;m_selected.clear();
            const SDL_Rect g=keymap_grid();
            for(int i=0;m_zones&&i<(int)m_zones->size();++i) {
                const SamplerZone& z=(*m_zones)[i];
                SDL_Rect zr{key_left_x(g,z.loKey),vel_to_y(g,z.hiVel+1),
                            key_right_x(g,z.hiKey)-key_left_x(g,z.loKey),
                            vel_to_y(g,z.loVel)-vel_to_y(g,z.hiVel+1)};
                SDL_Rect hit;
                if(SDL_IntersectRect(&zr,&m_marquee_rect,&hit))m_selected.insert(i);
            }
            if(!m_selected.empty()){m_sel=*m_selected.begin();if(on_zone_selected)on_zone_selected(m_sel);}
            app.request_redraw();return true;
        }
        if (m_scroll_drag) {
            m_scroll_drag = false;
            app.request_redraw();
            return true;
        }
        if (m_drag == Drag::BrowserSample && m_drag_armed && !m_drag_path.empty()) {
            // Resolved at the RELEASE position through the same function that
            // painted the highlight, so the two can never disagree -- and a
            // release over nothing droppable (or outside the view entirely)
            // reports None and simply cancels.
            const DropTarget tgt = drop_target_at(e.x, e.y);
            const std::string path = m_drag_path;
            m_drag = Drag::None; m_drag_path.clear(); m_drag_name.clear();
            m_drag_armed = false; m_drag_target = DropTarget{};
            drop_sample(app, path, tgt);
            app.request_redraw();
            return true;
        }
        // Inspector / zone-env drags: the light per-zone setters already ran on
        // every motion; only the LOOP POINTS need the heavy full re-push (they
        // are baked into the uploaded region), and only once, here.
        if (m_insp_drag >= 0) {
            if ((m_insp_drag == ZF_LSTART || m_insp_drag == ZF_LEND) && on_apply) on_apply();
            m_insp_drag = -1;
            app.request_redraw(); return true;
        }
        if (m_zenv_drag >= 0) { m_zenv_drag = -1; app.request_redraw(); return true; }
        // On release, push edits to the engine ONCE: envelope drags -> on_env,
        // keyrange/zone drags -> on_apply.
        if ((m_drag == Drag::EnvNode || m_drag == Drag::EnvCurve) && on_env) on_env(m_cur_env);
        else if (m_drag != Drag::None && m_drag != Drag::BrowserSample && on_apply) on_apply();
        m_drag = Drag::None; m_param_drag = -1; m_env_node = -1; m_env_seg = -1;
        m_drag_path.clear(); m_drag_name.clear();
        m_drag_armed = false; m_drag_target = DropTarget{};
        app.request_redraw(); return true;
    }

    // ---- a file from the library is in flight -------------------------------
    // Handled ABOVE the viewport gate below: a drag that wanders outside the
    // view still has to hear about it, or the highlight freezes on the last
    // thing it touched inside and the drop looks like it will land there.
    if (m_drag == Drag::BrowserSample) {
        if (!m_drag_armed &&
            (std::abs(e.x - m_drag_press_x) > 4 || std::abs(e.y - m_drag_press_y) > 4))
            m_drag_armed = true;
        m_drag_target = m_drag_armed ? drop_target_at(e.x, e.y) : DropTarget{};
        app.request_redraw();
        return true;
    }

    if(m_key_scroll_drag) {
        const int travel=std::max(1,m_key_scroll_track.w-m_key_scroll_thumb.w);
        const int maxFirst=std::max(0,128-m_key_visible);
        m_key_first=std::max(0,std::min(maxFirst,m_key_scroll_drag_first+
            (e.x-m_key_scroll_drag_x)*maxFirst/travel));
        app.request_redraw();return true;
    }
    if(leftEdge&&in_rect(m_key_scroll_track,e.x,e.y)) {
        if(in_rect(m_key_scroll_thumb,e.x,e.y)) {
            m_key_scroll_drag=true;m_key_scroll_drag_x=e.x;m_key_scroll_drag_first=m_key_first;
        } else {
            const int centered=key_at_x(e.x,m_key_scroll_track)-m_key_visible/2;
            m_key_first=std::max(0,std::min(128-m_key_visible,centered));
        }
        app.request_redraw();return true;
    }
    if(m_zone_marquee) {
        m_marquee_rect=SDL_Rect{std::min(m_marquee_start.x,e.x),std::min(m_marquee_start.y,e.y),
            std::abs(e.x-m_marquee_start.x),std::abs(e.y-m_marquee_start.y)};
        app.request_redraw();return true;
    }

    if(m_browser_scroll_drag){
        const int maxScroll=std::max(0,(int)m_browser.size()-m_browser_visible);
        const int travel=std::max(1,m_browser_scroll_track.h-m_browser_scroll_thumb.h);
        m_browser_scroll=std::max(0,std::min(maxScroll,m_browser_scroll_drag_start+
            (e.y-m_browser_scroll_drag_y)*maxScroll/travel));
        app.request_redraw(); return true;
    }
    if(leftEdge && m_browser_scroll_thumb.w>0 && in_rect(m_browser_scroll_track,e.x,e.y)){
        if(in_rect(m_browser_scroll_thumb,e.x,e.y)){
            m_browser_scroll_drag=true;m_browser_scroll_drag_y=e.y;m_browser_scroll_drag_start=m_browser_scroll;
        }else{
            m_browser_scroll += e.y<m_browser_scroll_thumb.y ? -m_browser_visible : m_browser_visible;
            m_browser_scroll=std::max(0,std::min(std::max(0,(int)m_browser.size()-m_browser_visible),m_browser_scroll));
        }
        app.request_redraw(); return true;
    }

    if (m_scroll_drag) {
        int travel = std::max(1, m_scroll_track.h - m_scroll_thumb.h);
        int delta = e.y - m_scroll_drag_y;
        m_scroll_y = m_scroll_drag_start + (delta * std::max(1, m_scroll_max)) / travel;
        if (m_scroll_y < 0) m_scroll_y = 0;
        if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;
        app.request_redraw();
        return true;
    }

    if (m_scroll_max > 0 && in_rect(m_scroll_track, e.x, e.y)) {
        if (in_rect(m_scroll_thumb, e.x, e.y)) {
            m_scroll_drag = true;
            m_scroll_drag_y = e.y;
            m_scroll_drag_start = m_scroll_y;
        } else {
            m_scroll_y += (e.y < m_scroll_thumb.y) ? -view.h : view.h;
            if (m_scroll_y < 0) m_scroll_y = 0;
            if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;
        }
        app.request_redraw();
        return true;
    }

    const bool inToolbar = (e.y >= rect.y && e.y < rect.y + 28);
    if (!inToolbar && !in_rect(view, e.x, e.y))
        return true;

    if (m_drive_menu && (leftEdge || rightEdge)) {
        // Use the list that was DRAWN, not a fresh drive_roots() query: the two
        // can disagree (a volume mounts or unmounts between the frame and the
        // click) and the row you aimed at would then select a different drive.
        for (size_t i = 0; i < m_drive_rows.size() && i < m_drive_labels.size(); ++i) {
            if (in_rect(m_drive_rows[i], e.x, e.y)) {
                m_browser_dir = m_drive_labels[i];
                m_browser_scroll = 0;
                m_drive_menu = false;
                scan_browser();
                app.request_redraw();
                return true;
            }
        }
        if (!in_rect(m_drive_rect, e.x, e.y)) {
            m_drive_menu = false;
            app.request_redraw();
            return true;
        }
    }

    if (leftEdge && in_rect(m_drive_rect, e.x, e.y)) {
        m_drive_menu = !m_drive_menu;
        m_browser_active = true;
        app.request_redraw();
        return true;
    }

    // motion while a drag is active (held-button events).  A library drag is
    // NOT handled here -- it is resolved above, before the viewport gate.
    if (m_drag != Drag::None || m_param_drag >= 0 || m_insp_drag >= 0 || m_zenv_drag >= 0) {
        if (m_insp_drag >= 0 && m_zones && m_sel >= 0 && m_sel < (int)m_zones->size() &&
            m_insp_drag < (int)m_insp_rows.size()) {
            SamplerZone& z = (*m_zones)[(size_t)m_sel];
            const SDL_Rect& tr = m_insp_rows[m_insp_drag];
            float v = (float)(e.x - tr.x) / (float)std::max(1, tr.w);
            v = std::max(0.f, std::min(1.f, v));
            if (SDL_GetModState() & KMOD_SHIFT) {   // fine, same feel as the param sliders
                const float cur = insp_get(z, m_insp_drag);
                v = cur + (v - cur) * 0.15f;
            }
            insp_set(z, m_insp_drag, v);
            // Loop points are pushed once on release (full re-upload); every
            // other field goes through the cheap per-zone setter LIVE.
            if (m_insp_drag != ZF_LSTART && m_insp_drag != ZF_LEND && on_zone_params)
                on_zone_params(m_sel);
        } else if (m_zenv_drag >= 0 && m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
            SamplerZone& z = (*m_zones)[(size_t)m_sel];
            SamplerZoneEnvUI& en = m_cur_env == ZENV_MOD ? z.modEnv : z.ampEnv;
            const SDL_Rect& tr = m_zenv_rows[m_zenv_drag];
            float v = (float)(e.x - tr.x) / (float)std::max(1, tr.w);
            v = std::max(0.f, std::min(1.f, v));
            float* stage[6] = { &en.delay, &en.attack, &en.hold, &en.decay, &en.sustain, &en.release };
            *stage[m_zenv_drag] = m_zenv_drag == 4 ? v : zenv_slider_to_secs(v);
            en.enabled = 1;                        // editing a stage IS overriding
            if (on_zone_params) on_zone_params(m_sel);
        } else if (m_param_drag >= 0 && m_inst && m_param_drag < (int)m_param_rows.size()) {
            const SDL_Rect& tr = m_param_rows[m_param_drag];
            float v = (float)(e.x - tr.x) / (float)std::max(1, tr.w);
            v = std::max(0.f, std::min(1.f, v));
            // Shift = fine: the track is ~200 px for the whole range, so without
            // it a filter cutoff moves in 0.5% steps at best and small values are
            // simply not reachable with a mouse.
            if (SDL_GetModState() & KMOD_SHIFT) {
                const float cur = m_inst->getParamNormalized(m_inst->paramInfo(m_param_drag).id);
                v = cur + (v - cur) * 0.15f;
            }
            m_inst->setParamNormalized(m_inst->paramInfo(m_param_drag).id, v);
        } else if (m_drag == Drag::EnvNode && m_envs && m_env_node >= 0) {
            SamplerEnv& en = m_envs->env[m_cur_env];
            if (m_env_node < (int)en.nodes.size() && m_env_rect.w > 0) {
                EnvNode& nd = en.nodes[m_env_node];
                nd.x = std::max(0.f, std::min(1.f, (float)(e.x - m_env_rect.x) / m_env_rect.w));
                nd.y = std::max(0.f, std::min(1.f, 1.f - (float)(e.y - m_env_rect.y) / m_env_rect.h));
                const int last = (int)en.nodes.size() - 1;
                if (m_env_node == 0)    nd.x = 0.f;
                if (m_env_node == last) nd.x = 1.f;
                if (m_env_node > 0)     nd.x = std::max(nd.x, en.nodes[m_env_node - 1].x + 0.002f);
                if (m_env_node < last)  nd.x = std::min(nd.x, en.nodes[m_env_node + 1].x - 0.002f);
            }
        } else if (m_drag == Drag::EnvCurve && m_envs && m_env_seg >= 0) {
            SamplerEnv& en = m_envs->env[m_cur_env];
            if (m_env_seg + 1 < (int)en.nodes.size() && m_env_rect.h > 0) {
                float midY = (en.nodes[m_env_seg].y + en.nodes[m_env_seg + 1].y) * 0.5f;
                int midScreenY = m_env_rect.y + (int)((1.f - midY) * m_env_rect.h);
                en.nodes[m_env_seg].curve = std::max(-1.f, std::min(1.f,
                    2.f * (float)(midScreenY - e.y) / m_env_rect.h));
            }
        } else if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
            SamplerZone& z = (*m_zones)[m_sel];
            const SDL_Rect grid = keymap_grid();
            int key = key_at_x(e.x, grid);
            // Through y_to_vel: this used to divide by 127 while the drawing
            // divided by 128, so the band you dragged settled a couple of
            // velocity steps away from where the pointer was.
            int vel = y_to_vel(grid, e.y);
            if      (m_drag == Drag::ZoneLo)   z.loKey = std::min(key, z.hiKey);
            else if (m_drag == Drag::ZoneHi)   z.hiKey = std::max(key, z.loKey);
            else if (m_drag == Drag::ZoneVelLo) z.loVel = std::min(vel, z.hiVel);
            else if (m_drag == Drag::ZoneVelHi) z.hiVel = std::max(vel, z.loVel);
            else if (m_drag == Drag::ZoneRoot) z.root  = key;
            else if (m_drag == Drag::ZoneMove) {
                const int desired=key-m_zoneDragKeyOffset;
                const int originLo=(m_sel<(int)m_drag_origin.size()?m_drag_origin[m_sel].loKey:z.loKey);
                int delta=desired-originLo;
                int minLo=127,maxHi=0;
                for(int i:selected_indices()) if(i<(int)m_drag_origin.size()) {
                    minLo=std::min(minLo,m_drag_origin[i].loKey); maxHi=std::max(maxHi,m_drag_origin[i].hiKey);
                }
                delta=std::max(-minLo,std::min(127-maxHi,delta));
                for(int i:selected_indices()) if(i<(int)m_drag_origin.size()) {
                    SamplerZone& dz=(*m_zones)[i]; const SamplerZone& oz=m_drag_origin[i];
                    dz.loKey=oz.loKey+delta; dz.hiKey=oz.hiKey+delta;
                    dz.root=m_move_root?oz.root+delta:std::max(dz.loKey,std::min(dz.hiKey,oz.root));
                }
            }
            clamp_zone_keys(z);
        }
        app.request_redraw(); return true;
    }

    // toolbar buttons
    if (leftEdge && in_rect(m_btn_load, e.x, e.y)) { if (on_load) on_load(m_sel); return true; }
    if (leftEdge && in_rect(m_btn_add,  e.x, e.y)) { if (on_load) on_load(-1); return true; }
    if (leftEdge && in_rect(m_btn_del,  e.x, e.y)) {
        if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
            zone_snapshot(); auto ids=selected_indices();
            for(auto it=ids.rbegin();it!=ids.rend();++it)m_zones->erase(m_zones->begin()+*it);
            select_only(std::min(m_sel,(int)m_zones->size()-1));
            if (on_apply) on_apply();
            if (m_sel >= 0 && on_zone_selected) on_zone_selected(m_sel);
            else m_inlineEditor.set_slot(nullptr);
        }
        app.request_redraw(); return true;
    }
    if (leftEdge && m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        SamplerZone& z=(*m_zones)[m_sel];
        enum MapStep { MS_NONE,MS_LO_DEC,MS_LO_INC,MS_ROOT_DEC,MS_ROOT_INC,
                       MS_HI_DEC,MS_HI_INC,MS_VLO_DEC,MS_VLO_INC,MS_VHI_DEC,MS_VHI_INC };
        MapStep step=MS_NONE;
        if(in_rect(m_btn_lo_dec,e.x,e.y))step=MS_LO_DEC;
        else if(in_rect(m_btn_lo_inc,e.x,e.y))step=MS_LO_INC;
        else if(in_rect(m_btn_root_dec,e.x,e.y))step=MS_ROOT_DEC;
        else if(in_rect(m_btn_root_inc,e.x,e.y))step=MS_ROOT_INC;
        else if(in_rect(m_btn_hi_dec,e.x,e.y))step=MS_HI_DEC;
        else if(in_rect(m_btn_hi_inc,e.x,e.y))step=MS_HI_INC;
        else if(in_rect(m_btn_vel_lo_dec,e.x,e.y))step=MS_VLO_DEC;
        else if(in_rect(m_btn_vel_lo_inc,e.x,e.y))step=MS_VLO_INC;
        else if(in_rect(m_btn_vel_hi_dec,e.x,e.y))step=MS_VHI_DEC;
        else if(in_rect(m_btn_vel_hi_inc,e.x,e.y))step=MS_VHI_INC;
        if(step!=MS_NONE) {
            zone_snapshot(true);
            switch(step) {
            case MS_LO_DEC:  z.loKey=std::max(0,z.loKey-1); break;
            case MS_LO_INC:  z.loKey=std::min(z.hiKey,z.loKey+1); break;
            case MS_ROOT_DEC:z.root=std::max(0,z.root-1); break;
            case MS_ROOT_INC:z.root=std::min(127,z.root+1); break;
            case MS_HI_DEC:  z.hiKey=std::max(z.loKey,z.hiKey-1); break;
            case MS_HI_INC:  z.hiKey=std::min(127,z.hiKey+1); break;
            case MS_VLO_DEC: z.loVel=std::max(0,z.loVel-1); break;
            case MS_VLO_INC: z.loVel=std::min(z.hiVel,z.loVel+1); break;
            case MS_VHI_DEC: z.hiVel=std::max(z.loVel,z.hiVel-1); break;
            case MS_VHI_INC: z.hiVel=std::min(127,z.hiVel+1); break;
            default: break;
            }
            clamp_zone_keys(z);
            if(on_apply)on_apply();
            app.request_redraw(); return true;
        }
    }
    // per-zone parameter inspector: slider grab / right-click default / loop toggle
    if ((leftEdge || rightEdge) && m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        SamplerZone& z = (*m_zones)[(size_t)m_sel];
        if (leftEdge && in_rect(m_btn_zloop, e.x, e.y)) {
            zone_snapshot(true);
            z.loop = !z.loop;
            if (on_apply) on_apply();              // loop flag is baked into the upload
            app.request_redraw(); return true;
        }
        for (int f = 0; f < (int)m_insp_rows.size(); ++f) {
            if (m_insp_rows[f].w <= 0 || !in_rect(m_insp_rows[f], e.x, e.y)) continue;
            zone_snapshot(true);
            const bool loopField = f == ZF_LSTART || f == ZF_LEND;
            if (rightEdge) {                       // back to the field's default
                insp_default(z, f);
                if (loopField) { if (on_apply) on_apply(); }
                else if (on_zone_params) on_zone_params(m_sel);
                app.request_redraw(); return true;
            }
            m_insp_drag = f;
            const float v = (float)(e.x - m_insp_rows[f].x) / (float)std::max(1, m_insp_rows[f].w);
            insp_set(z, f, std::max(0.f, std::min(1.f, v)));
            if (!loopField && on_zone_params) on_zone_params(m_sel);
            app.request_redraw(); return true;
        }
    }
    // The zone command buttons that used to live here were laid out at
    // rect.x + rect.w + 64 -- past the right edge of the clipped viewport, so
    // they were painted nowhere and could never be pressed, and the raw
    // buffer edits behind them (fade / DC / auto-trim / normalise) wrote
    // straight into the zone's AudioClip with no undo entry and no bounds check
    // on the right channel.  Destructive audio editing belongs to the shared
    // editor over the wave pane, which performs every edit through
    // ISampleSlot::sampleOp() and can therefore undo it (sampleUndo/sampleRedo);
    // the zone-mapping commands live in the right-click menu below.
    // Preview pane: click positions AND auditions from there, so you can hear
    // the tail of a long take without loading it.  It used to be inert -- a
    // waveform you could look at and nothing else.
    if (leftEdge && m_preview_ok && in_rect(m_preview_rect, e.x, e.y)) {
        const SDL_Rect w{ m_preview_rect.x + 1, m_preview_rect.y + 1,
                          std::max(1, m_preview_rect.w - 2), std::max(1, m_preview_rect.h - 2) };
        const int64_t n = m_preview_clip.safeFrames();
        const int64_t from = (int64_t)((double)std::max(0, e.x - w.x) / (double)w.w * (double)n);
        m_browser_active = true;
        audition_preview(app, from);
        return true;
    }
    // disk browser: dirs enter, WAV rows preview and become draggable.
    if (leftEdge || rightEdge) for (size_t i = 0; i < m_browser_rows.size(); ++i)
        if (in_rect(m_browser_rows[i], e.x, e.y)) {
            int idx = m_browser_scroll + (int)i;
            if (idx >= 0 && idx < (int)m_browser.size()) {
                m_browser_active = true;
                if (rightEdge) { open_menu(app, e.x, e.y, -1, -1, idx); return true; }
                select_browser(idx, true);
                if (idx >= 0 && idx < (int)m_browser.size() && !m_browser[idx].dir) {
                    m_prev_playing = true;                 // select_browser auditioned it
                    m_prev_ms0 = (Uint32)SDL_GetTicks();
                    m_prev_from = 0; m_prev_last_x = -1;
                    if (m_preview_ok) rebuild_preview_peaks();
                    // The press only BECOMES a drag past a threshold (see the
                    // motion handler), so clicking a row to audition it does
                    // not flash a ghost or drop anything.
                    m_drag = Drag::BrowserSample;
                    m_drag_path = m_browser[idx].path;
                    m_drag_name = m_browser[idx].name;
                    m_drag_armed = false;
                    m_drag_press_x = e.x; m_drag_press_y = e.y;
                    m_drag_target = DropTarget{};
                }
            }
            app.request_redraw(); return true;
        }
    // Right-click anywhere else in the browser pane: folder-level commands.
    if (rightEdge && in_rect(m_browser_rect, e.x, e.y)) {
        m_browser_active = true;
        open_menu(app, e.x, e.y, -1, -1, m_browser_sel >= 0 ? m_browser_sel : 0);
        return true;
    }
    // zone-list selection
    if (rightEdge) for (size_t i = 0; i < m_zone_rows.size(); ++i)
        if (in_rect(m_zone_rows[i], e.x, e.y)) {
            const int hit = i < m_zone_row_indices.size() ? m_zone_row_indices[i] : (int)i;
            if (!m_selected.count(hit)) select_only(hit);   // right-click targets what you point at
            else m_sel = hit;
            m_browser_active = false;
            open_menu(app, e.x, e.y, hit, -1, -1);
            return true;
        }
    if (leftEdge) for (size_t i = 0; i < m_zone_rows.size(); ++i)
        if (in_rect(m_zone_rows[i], e.x, e.y)) { m_browser_active = false;
            const int hit = i < m_zone_row_indices.size() ? m_zone_row_indices[i] : (int)i;
            const SDL_Keymod mods=SDL_GetModState();
            if (mods & KMOD_CTRL) {
                if (m_selected.count(hit)) m_selected.erase(hit); else m_selected.insert(hit);
                m_sel=hit; sanitize_selection();
            } else if ((mods & KMOD_SHIFT) && m_sel >= 0) {
                const int a=std::min(m_sel,hit), b=std::max(m_sel,hit);
                for(int z=a;z<=b;++z) m_selected.insert(z); m_sel=hit;
            } else select_only(hit);
            if (on_zone_selected) on_zone_selected(m_sel); app.request_redraw(); return true; }

    // keyzone grid: select / drag note edges, velocity edges, body, or root marker
    const SDL_Rect grid = keymap_grid();          // same rect draw() painted
    if ((leftEdge || rightEdge) && in_rect(grid, e.x, e.y) && m_zones) {
        const int key = key_at_x(e.x, grid);
        const int hitVel = y_to_vel(grid, e.y);
        int zi = zone_at(key, hitVel);
        // Shift-drag is an explicit marquee gesture even when it starts over a
        // zone; plain dragging a zone still moves/resizes it.
        if(leftEdge&&(SDL_GetModState()&KMOD_SHIFT)) {
            m_zone_marquee=true;m_marquee_start=SDL_Point{e.x,e.y};
            m_marquee_rect=SDL_Rect{e.x,e.y,1,1};
            app.request_redraw();return true;
        }
        // Keep the SELECTED zone grabbable by its edges even when the press
        // lands a few pixels outside its velocity band -- otherwise resizing a
        // one-row-tall layer is a pixel-hunt.
        if (zi < 0 && m_sel >= 0 && m_sel < (int)m_zones->size() &&
            handle_at(grid, m_sel, e.x, e.y) != Drag::ZoneMove)
            zi = m_sel;
        if (rightEdge) {
            if (zi >= 0 && !m_selected.count(zi)) select_only(zi);
            else if (zi >= 0) m_sel = zi;
            open_menu(app, e.x, e.y, zi, key, -1);
            return true;
        }
        if (zi >= 0) {
            const SDL_Keymod mods=SDL_GetModState();
            if (mods & KMOD_CTRL) {
                if (m_selected.count(zi)) m_selected.erase(zi); else m_selected.insert(zi);
                m_sel=zi; sanitize_selection();
            } else if (!(mods & KMOD_SHIFT) && !m_selected.count(zi)) select_only(zi);
            else { m_sel=zi; m_selected.insert(zi); }
            if (on_zone_selected) on_zone_selected(m_sel);
            if (m_move_lock) { app.request_redraw(); return true; }
            zone_snapshot(true);           // a drag moves mapping, never PCM
            m_drag_origin.clear(); m_drag_origin.reserve(m_zones->size());
            for (SamplerZone& oz : *m_zones) m_drag_origin.push_back(zone_meta_copy(oz));
            // ONE handle picker, shared with the hover cursor: nearest handle
            // wins, so a narrow zone is movable and its velocity edges are
            // reachable instead of every press becoming "drag the low edge".
            m_drag = handle_at(grid, zi, e.x, e.y);
            if (m_drag == Drag::ZoneMove) {
                m_zoneDragKeyOffset = key - (*m_zones)[(size_t)zi].loKey;
                m_zoneDragRootOffset = (*m_zones)[(size_t)zi].root - (*m_zones)[(size_t)zi].loKey;
            }
        } else if(leftEdge) {
            if(!(SDL_GetModState()&KMOD_CTRL))m_selected.clear();
            m_zone_marquee=true;m_marquee_start=SDL_Point{e.x,e.y};
            m_marquee_rect=SDL_Rect{e.x,e.y,1,1};
        }
        app.request_redraw(); return true;
    }
    // envelope: tabs, then canvas (node drag / curve handle / add / delete / sustain)
    if (leftEdge) for (int i = 0; i < ENV_TABS; ++i)
        if (in_rect(m_env_tabs[i], e.x, e.y)) { m_cur_env = i; app.request_redraw(); return true; }
    // per-zone envelope (Z-AMP / Z-MOD tabs): override toggle + stage sliders
    if ((leftEdge || rightEdge) && m_cur_env >= ENV_COUNT &&
        m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        SamplerZone& z = (*m_zones)[(size_t)m_sel];
        SamplerZoneEnvUI& en = m_cur_env == ZENV_MOD ? z.modEnv : z.ampEnv;
        if (leftEdge && in_rect(m_zenv_toggle, e.x, e.y)) {
            zone_snapshot(true);
            en.enabled = en.enabled ? 0 : 1;
            // First-ever override on an untouched envelope: seed an audible ADSR
            // instead of the do-nothing flat default, so turning the override on
            // is immediately visible AND immediately editable.
            if (en.enabled && en.delay == 0.f && en.attack == 0.f && en.hold == 0.f &&
                en.decay == 0.f && en.release == 0.f && en.sustain >= 1.f) {
                en.attack = 0.005f; en.decay = 0.25f; en.sustain = 0.75f; en.release = 0.3f;
            }
            if (on_zone_params) on_zone_params(m_sel);
            app.request_redraw(); return true;
        }
        for (int s = 0; s < 6; ++s) {
            if (m_zenv_rows[s].w <= 0 || !in_rect(m_zenv_rows[s], e.x, e.y)) continue;
            zone_snapshot(true);
            float* stage[6] = { &en.delay, &en.attack, &en.hold, &en.decay, &en.sustain, &en.release };
            if (rightEdge) {                       // stage default (sustain 100%, times 0)
                *stage[s] = s == 4 ? 1.f : 0.f;
                if (on_zone_params) on_zone_params(m_sel);
                app.request_redraw(); return true;
            }
            m_zenv_drag = s;
            const float v = std::max(0.f, std::min(1.f,
                (float)(e.x - m_zenv_rows[s].x) / (float)std::max(1, m_zenv_rows[s].w)));
            *stage[s] = s == 4 ? v : zenv_slider_to_secs(v);
            en.enabled = 1;                        // editing a stage IS overriding
            if (on_zone_params) on_zone_params(m_sel);
            app.request_redraw(); return true;
        }
    }
    if ((leftEdge || rightEdge) && m_envs && m_cur_env < ENV_COUNT && in_rect(m_env_rect, e.x, e.y)) {
        SamplerEnv& en = m_envs->env[m_cur_env];
        const int ni = env_node_at(e.x, e.y, m_env_rect);
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if (rightEdge) {                       // delete an interior node
            if (ni > 0 && ni < (int)en.nodes.size() - 1) {
                en.nodes.erase(en.nodes.begin() + ni);
                if (en.sustain == ni) en.sustain = -1; else if (en.sustain > ni) --en.sustain;
                if (on_env) on_env(m_cur_env);
            }
            app.request_redraw(); return true;
        }
        if (ni >= 0) {
            if (shift) { en.sustain = (en.sustain == ni ? -1 : ni); if (on_env) on_env(m_cur_env); }
            else { m_drag = Drag::EnvNode; m_env_node = ni; }
            app.request_redraw(); return true;
        }
        // curve handle on a segment? -- asks the SAME function that drew it
        for (int s = 0; s + 1 < (int)en.nodes.size(); ++s) {
            const SDL_Point h = env_curve_handle(m_env_rect, s);
            if (std::abs(e.x - h.x) <= 5 && std::abs(e.y - h.y) <= 5) {
                m_drag = Drag::EnvCurve; m_env_seg = s; app.request_redraw(); return true;
            }
        }
        // Empty space adds a node only on a real double-click.  A single click
        // is now harmless, which makes selecting/approaching nearby points far
        // less likely to create accidental nodes.
        if (!leftEdge) { app.request_redraw(); return true; }
        const Uint64 now = SDL_GetTicks();
        const int dcx = e.x - m_env_last_click.x;
        const int dcy = e.y - m_env_last_click.y;
        const bool doubleClick = m_env_last_click_ms != 0 &&
                                 now - m_env_last_click_ms <= 450 &&
                                 dcx * dcx + dcy * dcy <= 12 * 12;
        m_env_last_click_ms = now;
        m_env_last_click = SDL_Point{e.x, e.y};
        if (!doubleClick) { app.request_redraw(); return true; }
        m_env_last_click_ms = 0; // a triple-click must not add two points
        // Guarded: a zero-width canvas divided by 0 here and stored a NaN
        // position, and a NaN node draws nowhere and can never be grabbed again.
        if (m_env_rect.w <= 0 || m_env_rect.h <= 0) { app.request_redraw(); return true; }
        float fx = std::max(0.f, std::min(1.f, (float)(e.x - m_env_rect.x) / m_env_rect.w));
        float fy = std::max(0.f, std::min(1.f, 1.f - (float)(e.y - m_env_rect.y) / m_env_rect.h));
        int ins = (int)en.nodes.size();
        for (int j = 0; j < (int)en.nodes.size(); ++j) if (en.nodes[j].x > fx) { ins = j; break; }
        EnvNode nn; nn.x = fx; nn.y = fy;
        en.nodes.insert(en.nodes.begin() + ins, nn);
        if (en.sustain >= ins) ++en.sustain;
        en.enabled = true;
        m_drag = Drag::EnvNode; m_env_node = ins;
        app.request_redraw(); return true;
    }

    // param slider grab
    if (leftEdge || rightEdge) for (size_t i = 0; i < m_param_rows.size(); ++i)
        if (m_param_rows[i].w > 0 && in_rect(m_param_rows[i], e.x, e.y)) {
            if (!m_inst) return true;
            const PatchKnob::engine::ParamInfo pi = m_inst->paramInfo((int)i);
            if (rightEdge) {
                // Right-click restores the DEFAULT.  There was no way back from
                // an accidental drag except remembering the old number.
                m_inst->setParamNormalized(pi.id, std::max(0.f, std::min(1.f, pi.defaultValue)));
                app.request_redraw(); return true;
            }
            m_param_drag = (int)i;
            float v = (float)(e.x - m_param_rows[i].x) / (float)std::max(1, m_param_rows[i].w);
            m_inst->setParamNormalized(pi.id, std::max(0.f, std::min(1.f, v)));
            app.request_redraw(); return true;
        }
    return true;
}

bool SamplerEditorView::on_wheel(App& app, int, int dy) {
    int mx = m_mx, my = m_my;
    ui::mouse_logical(app, mx, my);   // logical, not window px
    const SDL_Rect kg=keymap_grid();
    if((in_rect(kg,mx,my)||in_rect(m_strip,mx,my))&&(SDL_GetModState()&KMOD_CTRL)) {
        const int anchor=key_at_x(mx,kg);
        const int old=m_key_visible;
        m_key_visible=dy>0?std::max(12,m_key_visible/2):std::min(128,m_key_visible*2);
        const double u=kg.w>0?(double)(mx-kg.x)/kg.w:.5;
        m_key_first=(int)std::lround(anchor-u*m_key_visible);
        m_key_first=std::max(0,std::min(128-m_key_visible,m_key_first));
        if(old!=m_key_visible)app.request_redraw();return true;
    }
    if (!m_zone_rows.empty()) {
        SDL_Rect zr = m_zone_rows.front();
        zr.y = m_zone_rows.front().y - app.mono.ch() - 12;
        zr.h = m_zone_rows.back().y + m_zone_rows.back().h - zr.y;
        if (in_rect(zr, mx, my)) {
            m_zone_scroll = std::max(0, m_zone_scroll - dy * 2);
            app.request_redraw(); return true;
        }
    }
    if (m_inlineEditor.slot() && in_rect(m_wave, mx, my))
        return m_inlineEditor.on_wheel(app, 0, dy);
    if (m_inlineEdit && in_rect(m_inlineRect, mx, my))
        return m_inlineEditor.on_wheel(app, 0, dy);
    if (in_rect(m_browser_rect, mx, my)) {
        m_browser_scroll += (dy > 0 ? -3 : 3);
        const int visibleRows = std::max(1, m_browser_visible);
        int maxScroll = std::max(0, (int)m_browser.size() - visibleRows);
        if (m_browser_scroll < 0) m_browser_scroll = 0;
        if (m_browser_scroll > maxScroll) m_browser_scroll = maxScroll;
        app.request_redraw();
        return true;
    }
    // wheel over the strip nudges the selected zone's root note.
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size() && in_rect(m_strip, mx, my)) {
        SamplerZone& z = (*m_zones)[m_sel];
        z.root = std::max(z.loKey, std::min(z.hiKey, z.root + (dy > 0 ? 1 : -1)));
        if (on_apply) on_apply();
        app.request_redraw();
    } else if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size() && in_rect(m_wave, mx, my)) {
        SamplerZone& z = (*m_zones)[m_sel];
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if (shift) z.pan = std::max(-1.f, std::min(1.f, z.pan + (dy > 0 ? 0.05f : -0.05f)));
        else       z.gain = std::max(0.f, std::min(8.f, z.gain * (dy > 0 ? 1.08f : 0.925f)));
        if (on_apply) on_apply();
        app.request_redraw();
    } else {
        m_scroll_y += (dy > 0 ? -48 : 48);
        if (m_scroll_y < 0) m_scroll_y = 0;
        if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;
        app.request_redraw();
    }
    return true;
}


// Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y from the toolkit's undo route.
//
// Two histories live under this key and the split follows the FOCUS the view
// already tracks: when the shared waveform editor owns the keyboard the key
// means "undo the sample edit" (the slot's own PCM history), otherwise it means
// "undo the zone edit" (m_zone_undo/m_zone_redo).  Both are advertised in this
// view's own tooltips.  Before this hook existed neither could ever run --
// Ctrl+Z was consumed by the shell and rolled the whole PROJECT back -- so
// returning false only when BOTH are empty is what keeps project undo working
// while making the advertised behaviour real.
bool SamplerEditorView::zone_history(App& app, bool redo) {
    if (!m_zones) return false;
    std::vector<ZoneHistEntry>& from = redo ? m_zone_redo : m_zone_undo;
    std::vector<ZoneHistEntry>& to   = redo ? m_zone_undo : m_zone_redo;
    if (from.empty()) return false;
    ZoneHistEntry st = std::move(from.back());
    from.pop_back();
    // The state pushed onto the OPPOSITE stack mirrors the entry being
    // restored: unwinding a metaOnly edit means the counter-entry can be
    // metaOnly too (nothing structural lies between the two states), while a
    // full entry needs the current audio captured so it can come back.
    ZoneHistEntry cur;
    cur.metaOnly = st.metaOnly;
    if (st.metaOnly) {
        cur.zones.reserve(m_zones->size());
        for (SamplerZone& z : *m_zones) cur.zones.push_back(zone_meta_copy(z));
    } else {
        cur.zones = *m_zones;
    }
    to.push_back(std::move(cur));
    zone_restore(std::move(st));
    app.request_redraw();
    return true;
}

bool SamplerEditorView::on_undo(App& app, bool redo) {
    if ((m_inlineEdit || m_wave_focus) && m_inlineEditor.slot() &&
        m_inlineEditor.on_undo(app, redo))
        return true;
    return zone_history(app, redo);
}

bool SamplerEditorView::on_key(App& app, SDL_Keycode k) {
    if (m_inlineEdit) {
        if (k == SDLK_ESCAPE) { m_inlineEdit = false; m_inlineEditor.set_slot(nullptr); app.request_redraw(); return true; }
        return m_inlineEditor.on_key(app, k);
    }
    if (m_menu_open) {                       // Esc closes the menu first
        if (k == SDLK_ESCAPE) { m_menu_open = false; app.request_redraw(); return true; }
    }
    if (k == SDLK_ESCAPE &&
        (m_drag != Drag::None || m_param_drag >= 0 || m_insp_drag >= 0 ||
         m_zenv_drag >= 0 || m_drive_menu || m_scroll_drag)) {
        cancel_interaction(app);
        return true;
    }
    // In the FUSED layout the shared waveform editor is drawn inside this view
    // but never received keys, so its Ctrl+Z -- the undo for every destructive
    // sample op, which it performs through ISampleSlot::sampleOp -- was
    // unreachable from the keyboard.  Route keys to it while it has the focus.
    if (m_wave_focus && m_inlineEditor.slot() && m_inlineEditor.on_key(app, k))
        return true;
    // 'W' opens the SHARED waveform editor on the selected zone.  Checked before
    // the browser gate below, so it works whether or not the list has focus.
    if (k == SDLK_w && on_edit_zone && m_zones && !m_zones->empty()) {
        const int z = (m_sel >= 0 && m_sel < (int)m_zones->size()) ? m_sel : 0;
        on_edit_zone(z);
        app.request_redraw();
        return true;
    }
    const SDL_Keymod mods=SDL_GetModState();
    const bool ctrl=(mods & KMOD_CTRL)!=0, shift=(mods & KMOD_SHIFT)!=0, alt=(mods & KMOD_ALT)!=0;
    auto changed=[&]{ sanitize_selection(); if(on_apply)on_apply();
        if(m_sel>=0&&on_zone_selected)on_zone_selected(m_sel); app.request_redraw(); return true; };
    if (m_zones && ctrl && k==SDLK_a) {
        m_selected.clear(); for(int i=0;i<(int)m_zones->size();++i)m_selected.insert(i);
        if(m_sel<0&&!m_zones->empty())m_sel=0; app.request_redraw(); return true;
    }
    if (m_zones && ctrl && k==SDLK_i) {
        std::set<int> inv; for(int i=0;i<(int)m_zones->size();++i)if(!m_selected.count(i))inv.insert(i);
        m_selected=std::move(inv); sanitize_selection(); app.request_redraw(); return true;
    }
    if (m_zones && ctrl && k==SDLK_c) {
        m_zone_clipboard.clear(); for(int i:selected_indices())m_zone_clipboard.push_back((*m_zones)[i]); return true;
    }
    if (m_zones && ctrl && k==SDLK_v && !m_zone_clipboard.empty()) {
        // Paste ADDS zones, so the snapshot must be structural (full) or an
        // undo could not remove what redo re-adds; the clipboard is iterated
        // by const ref -- by value copied every zone's PCM twice per paste.
        zone_snapshot(); const int delta=shift?12:1; m_selected.clear();
        for(const SamplerZone& src:m_zone_clipboard){SamplerZone z=src;int d=std::min(delta,127-z.hiKey);z.loKey+=d;z.hiKey+=d;z.root+=d;
            m_zones->push_back(std::move(z));m_selected.insert((int)m_zones->size()-1);}
        m_sel=(int)m_zones->size()-1; return changed();
    }
    if (m_zones && ctrl && k==SDLK_d) {
        m_zone_clipboard.clear();for(int i:selected_indices())m_zone_clipboard.push_back((*m_zones)[i]);
        zone_snapshot();m_selected.clear();for(const SamplerZone& src:m_zone_clipboard){SamplerZone z=src;int d=std::min(1,127-z.hiKey);
            z.loKey+=d;z.hiKey+=d;z.root+=d;m_zones->push_back(std::move(z));m_selected.insert((int)m_zones->size()-1);}
        m_sel=(int)m_zones->size()-1;return changed();
    }
    // Ctrl+Z/Y never reach on_key -- the toolkit routes them to on_undo() --
    // but keep both paths on one implementation so they cannot drift.
    if (ctrl && (k==SDLK_z || k==SDLK_y)) return on_undo(app, k==SDLK_y || shift);
    if (m_zones && ctrl && k==SDLK_x && !selected_indices().empty()) {
        zone_snapshot();m_zone_clipboard.clear();auto ids=selected_indices();
        for(int i:ids)m_zone_clipboard.push_back((*m_zones)[i]);
        for(auto it=ids.rbegin();it!=ids.rend();++it)m_zones->erase(m_zones->begin()+*it);
        select_only(std::min(m_sel,(int)m_zones->size()-1));return changed();
    }
    if (m_zones && !ctrl && k==SDLK_DELETE && !selected_indices().empty()) {
        zone_snapshot();auto ids=selected_indices();for(auto it=ids.rbegin();it!=ids.rend();++it)m_zones->erase(m_zones->begin()+*it);
        select_only(std::min(m_sel,(int)m_zones->size()-1));return changed();
    }
    if (m_zones && !m_browser_active && (k==SDLK_UP || k==SDLK_DOWN) && !alt) {
        int n=(int)m_zones->size();if(n){select_only(std::max(0,std::min(n-1,m_sel+(k==SDLK_UP?-1:1))));
            if(on_zone_selected)on_zone_selected(m_sel);app.request_redraw();return true;}
    }
    if (m_zones && !m_browser_active && (k==SDLK_LEFT || k==SDLK_RIGHT || (alt&&(k==SDLK_UP||k==SDLK_DOWN)))) {
        zone_snapshot(true);int step=(k==SDLK_LEFT||k==SDLK_DOWN)?-1:1;if(shift)step*=12;
        for(int i:selected_indices()){SamplerZone& z=(*m_zones)[i];
            if(alt){int d=std::max(-z.loVel,std::min(127-z.hiVel,step));z.loVel+=d;z.hiVel+=d;}
            else {int d=std::max(-z.loKey,std::min(127-z.hiKey,step));z.loKey+=d;z.hiKey+=d;if(m_move_root)z.root+=d;else z.root=std::max(z.loKey,std::min(z.hiKey,z.root));}}
        return changed();
    }
    if (m_zones && !m_browser_active && (k==SDLK_r || k==SDLK_HOME || k==SDLK_END)) {
        zone_snapshot(true);for(int i:selected_indices()){SamplerZone& z=(*m_zones)[i];z.root=k==SDLK_HOME?z.loKey:(k==SDLK_END?z.hiKey:(z.loKey+z.hiKey)/2);}return changed();
    }
    if (m_zones && !m_browser_active && k==SDLK_f) {
        zone_snapshot(true);auto ids=selected_indices();std::sort(ids.begin(),ids.end(),[&](int a,int b){return(*m_zones)[a].root<(*m_zones)[b].root;});
        for(size_t j=0;j<ids.size();++j){SamplerZone& z=(*m_zones)[ids[j]];z.loKey=j?(((*m_zones)[ids[j-1]].root+z.root)/2+1):0;
            z.hiKey=j+1<ids.size()?((z.root+(*m_zones)[ids[j+1]].root)/2):127;clamp_zone_keys(z);}return changed();
    }
    if (m_zones && !m_browser_active && k==SDLK_v) {
        auto ids=selected_indices();if(ids.empty())return true;zone_snapshot(true);int n=(int)ids.size();for(int j=0;j<n;++j){auto&z=(*m_zones)[ids[j]];z.loVel=j*128/n;z.hiVel=(j+1)*128/n-1;}return changed();
    }
    if (!m_browser_active && k==SDLK_l) {m_move_lock=!m_move_lock;app.request_redraw();return true;}
    if (!m_browser_active && k==SDLK_m) {m_move_root=!m_move_root;app.request_redraw();return true;}
    if (!m_browser_active && k==SDLK_s) {m_solo_selection=!m_solo_selection;app.request_redraw();return true;}
    if (!m_browser_active && k==SDLK_o) {m_show_overlaps=!m_show_overlaps;app.request_redraw();return true;}
    if (!m_browser_active)
        return false;

    if (k == SDLK_UP || k == SDLK_DOWN) {
        if (step_browser(k == SDLK_UP ? -1 : 1, true)) {
            app.request_redraw();
            return true;
        }
        return false;
    }

    // A file list you can only walk one row at a time is unusable at 400 files.
    if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
        const int page = std::max(1, m_browser_visible - 1);
        step_browser(k == SDLK_PAGEUP ? -page : page, false);
        app.request_redraw();
        return true;
    }
    if (k == SDLK_HOME || k == SDLK_END) {
        m_browser_sel = k == SDLK_HOME ? -1 : (int)m_browser.size();
        step_browser(k == SDLK_HOME ? 1 : -1, false);
        app.request_redraw();
        return true;
    }
    if (k == SDLK_BACKSPACE) {                 // up one folder, like every browser
        run_menu(app, MB_UP);
        return true;
    }
    if (k == SDLK_F5) { scan_browser(); app.request_redraw(); return true; }
    if (k == SDLK_SPACE) {                     // audition the selection again
        if (m_preview_ok) { audition_preview(app, 0); return true; }
        return false;
    }

    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        if (m_browser_sel >= 0 && m_browser_sel < (int)m_browser.size()) {
            select_browser(m_browser_sel, true);
            app.request_redraw();
            return true;
        }
    }

    if (k == SDLK_ESCAPE) {
        m_browser_active = false;
        app.request_redraw();
        return true;
    }

    return false;
}

} // namespace ui
