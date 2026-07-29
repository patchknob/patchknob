//----------------------------------------------------------------------------
//  sdlui/views/sampler_editor/sampler_editor_view.cpp -- see the header.
//----------------------------------------------------------------------------
#include "sampler_editor_view.h"
#include "engine/plugin_api.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
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
const char* kEnvNames[ENV_COUNT] = { "AMP", "PIT", "CUT", "RES", "PAN" };
bool is_wav_path(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return e == ".wav";
}

int key_left_x(const SDL_Rect& r, int key) {
    key = std::max(0, std::min(127, key));
    return r.x + (int)std::floor((double)key * (double)r.w / 128.0);
}

int key_right_x(const SDL_Rect& r, int key) {
    key = std::max(0, std::min(127, key));
    return r.x + (int)std::ceil((double)(key + 1) * (double)r.w / 128.0);
}

int key_center_x(const SDL_Rect& r, int key) {
    key = std::max(0, std::min(127, key));
    return r.x + (int)std::lround(((double)key + 0.5) * (double)r.w / 128.0);
}

void clamp_zone_keys(SamplerZone& z) {
    z.loKey = std::max(0, std::min(127, z.loKey));
    z.hiKey = std::max(0, std::min(127, z.hiKey));
    if (z.loKey > z.hiKey) z.loKey = z.hiKey;
    z.root = std::max(z.loKey, std::min(z.hiKey, z.root));
}

// Tension curve: t,0..1 -> shaped 0..1.  c=0 linear; c>0 convex (fast rise);
// c<0 concave (slow rise).  Matches the clip-fade tension feel.
float env_shape(float t, float c) {
    if (t <= 0.f) return 0.f; if (t >= 1.f) return 1.f;
    if (c > -0.02f && c < 0.02f) return t;
    return std::pow(t, std::pow(2.0f, -c * 3.0f));
}
} // namespace

void SamplerEditorView::bind(int node, std::vector<SamplerZone>* zones, SamplerEnvSet* envs, IPluginInstance* inst) {
    m_node = node; m_zones = zones; m_envs = envs; m_inst = inst;
    m_sel = 0; m_cur_env = 0; m_drag = Drag::None; m_param_drag = -1; m_env_node = -1; m_env_seg = -1;
    if (m_browser_dir.empty()) {
        try { m_browser_dir = std::filesystem::current_path().string(); }
        catch (...) { m_browser_dir = "."; }
        scan_browser();
    }
}

void SamplerEditorView::scan_browser() {
    m_browser.clear();
    m_browser_rows.clear();
    m_browser_sel = -1;
    m_preview_ok = false;
    try {
        std::filesystem::path dir(m_browser_dir.empty() ? "." : m_browser_dir);
        if (dir.has_parent_path()) {
            BrowserItem up; up.name = ".."; up.path = dir.parent_path().string(); up.dir = true;
            m_browser.push_back(up);
        }
        std::vector<BrowserItem> dirs, wavs;
        for (const auto& ent : std::filesystem::directory_iterator(dir)) {
            BrowserItem it;
            it.path = ent.path().string();
            it.name = ent.path().filename().string();
            it.dir = ent.is_directory();
            if (it.dir) dirs.push_back(it);
            else if (is_wav_path(ent.path())) wavs.push_back(it);
        }
        auto byName = [](const BrowserItem& a, const BrowserItem& b){ return a.name < b.name; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(wavs.begin(), wavs.end(), byName);
        m_browser.insert(m_browser.end(), dirs.begin(), dirs.end());
        m_browser.insert(m_browser.end(), wavs.begin(), wavs.end());
    } catch (...) {}
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
    m_browser_sel = index;
    m_preview_ok = false;
    if (on_preview_path)
        m_preview_ok = on_preview_path(it.path, m_preview_clip) && !m_preview_clip.empty();
    if (audition && m_preview_ok && on_audition_clip)
        on_audition_clip(m_preview_clip);
}

bool SamplerEditorView::step_browser(int delta, bool audition) {
    if (m_browser.empty() || delta == 0) return false;
    int idx = m_browser_sel;
    if (idx < 0) idx = delta > 0 ? -1 : (int)m_browser.size();
    const int start = idx;
    do {
        idx += delta;
        if (idx < 0) idx = 0;
        if (idx >= (int)m_browser.size()) idx = (int)m_browser.size() - 1;
        if (idx >= 0 && idx < (int)m_browser.size() && !m_browser[idx].dir) {
            select_browser(idx, audition);
            const int rowH = 18;
            if (idx < m_browser_scroll) m_browser_scroll = idx;
            int visible = m_browser_rect.h > 0 ? std::max(1, (m_browser_rect.h - 70) / rowH) : 8;
            if (idx >= m_browser_scroll + visible) m_browser_scroll = idx - visible + 1;
            if (m_browser_scroll < 0) m_browser_scroll = 0;
            return true;
        }
    } while (idx != start && idx > 0 && idx + 1 < (int)m_browser.size());
    return false;
}

SDL_Point SamplerEditorView::env_pt(const SDL_Rect& r, const EnvNode& n) const {
    return SDL_Point{ r.x + (int)(n.x * r.w), r.y + (int)((1.f - n.y) * r.h) };
}
int SamplerEditorView::env_node_at(int x, int y, const SDL_Rect& r) const {
    if (!m_envs) return -1;
    const SamplerEnv& e = m_envs->env[m_cur_env];
    for (size_t i = 0; i < e.nodes.size(); ++i) {
        SDL_Point p = env_pt(r, e.nodes[i]);
        if (std::abs(x - p.x) <= 5 && std::abs(y - p.y) <= 5) return (int)i;
    }
    return -1;
}

void SamplerEditorView::draw_env(App& app, const SDL_Rect& r) {
    const Theme& t = theme();
    // env selector tabs
    const int cw = app.mono.cw() ? app.mono.cw() : 6, ch = app.mono.ch();
    int tx = r.x;
    for (int i = 0; i < ENV_COUNT; ++i) {
        SDL_Rect tab{ tx, r.y, 5 * cw, ch + 4 };
        m_env_tabs[i] = tab;
        const bool cur = i == m_cur_env;
        const bool on = m_envs && m_envs->env[i].enabled;
        fill_rect(app.ren, tab, cur ? t.accent : t.panel);
        frame_rect(app.ren, tab, t.dim);
        app.mono.draw(app.ren, tab.x + 4, tab.y + 2, kEnvNames[i], cur ? t.bg : (on ? t.text : t.dim));
        tx += tab.w + 2;
    }
    SDL_Rect canvas{ r.x, r.y + ch + 6, r.w, r.h - ch - 6 };
    m_env_rect = canvas;
    fill_rect(app.ren, canvas, t.bg);
    frame_rect(app.ren, canvas, t.dim);
    hline(app.ren, canvas.x, canvas.x + canvas.w - 1, canvas.y + canvas.h / 2, t.dim);
    if (!m_envs) return;
    SamplerEnv& e = m_envs->env[m_cur_env];
    if (!e.enabled) {
        app.mono.draw(app.ren, canvas.x + 6, canvas.y + 3, "(disabled - click to add a point / enable)", t.dim);
    }
    // curved segments
    set_color(app.ren, e.enabled ? t.accent : t.dim);
    for (size_t i = 0; i + 1 < e.nodes.size(); ++i) {
        SDL_Point a = env_pt(canvas, e.nodes[i]), b = env_pt(canvas, e.nodes[i + 1]);
        int px = a.x, py = a.y;
        const int steps = std::max(2, (b.x - a.x) / 4);
        for (int s = 1; s <= steps; ++s) {
            float tt = (float)s / steps;
            float yy = e.nodes[i].y + (e.nodes[i + 1].y - e.nodes[i].y) * env_shape(tt, e.nodes[i].curve);
            int nx = a.x + (int)((b.x - a.x) * tt);
            int ny = canvas.y + (int)((1.f - yy) * canvas.h);
            SDL_RenderDrawLine(app.ren, px, py, nx, ny);
            // curve handle at the segment midpoint
            if (s == steps / 2) fill_rect(app.ren, SDL_Rect{ nx - 2, ny - 2, 4, 4 }, t.hi);
            px = nx; py = ny;
        }
    }
    // nodes (sustain node ringed)
    for (size_t i = 0; i < e.nodes.size(); ++i) {
        SDL_Point p = env_pt(canvas, e.nodes[i]);
        fill_rect(app.ren, SDL_Rect{ p.x - 3, p.y - 3, 6, 6 }, t.text);
        if ((int)i == e.sustain) frame_rect(app.ren, SDL_Rect{ p.x - 5, p.y - 5, 10, 10 }, t.hi);
    }
}

int SamplerEditorView::key_at_x(int x, const SDL_Rect& s) const {
    if (s.w <= 0) return 0;
    int k = (int)std::floor((double)(x - s.x) * 128.0 / (double)s.w);
    return std::max(0, std::min(127, k));
}
int SamplerEditorView::zone_at_key(int key) const {
    if (!m_zones) return -1;
    for (int i = (int)m_zones->size() - 1; i >= 0; --i)
        if (key >= (*m_zones)[i].loKey && key <= (*m_zones)[i].hiKey) return i;
    return -1;
}

void SamplerEditorView::draw_wave(App& app, const SDL_Rect& r, const AudioClip& c) {
    const Theme& t = theme();
    fill_rect(app.ren, r, t.bg);
    frame_rect(app.ren, r, t.dim);
    const int64_t n = c.numFrames();
    if (n <= 0 || r.w < 2) {
        app.mono.draw(app.ren, r.x + 6, r.y + r.h/2 - app.mono.ch()/2, "(no sample in zone)", t.dim);
        return;
    }
    const float* L = c.ch[0].data();
    const int mid = r.y + r.h / 2, amp = r.h / 2 - 2;
    set_color(app.ren, t.text);
    for (int px = 0; px < r.w; ++px) {
        int64_t a = (int64_t)((double)px       / r.w * n);
        int64_t b = (int64_t)((double)(px + 1) / r.w * n);
        if (b <= a) b = a + 1; if (b > n) b = n;
        float mn = 1.f, mx = -1.f;
        for (int64_t i = a; i < b; ++i) { float v = L[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
        int y0 = mid - (int)(mx * amp), y1 = mid - (int)(mn * amp);
        SDL_RenderDrawLine(app.ren, r.x + px, y0, r.x + px, y1);
    }
    hline(app.ren, r.x, r.x + r.w - 1, mid, t.dim);
}

void SamplerEditorView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    const int cw = app.mono.cw() ? app.mono.cw() : 6, ch = app.mono.ch();
    char buf[64];

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
    SDL_Rect bar{ rect.x, rect.y, rect.w, 26 };
    fill_rect(app.ren, bar, t.panel);
    m_btn_load = SDL_Rect{ rect.x + 6,   rect.y + 3, 11 * cw, 20 };
    m_btn_add  = SDL_Rect{ rect.x + 12 + 11 * cw, rect.y + 3, 9 * cw, 20 };
    m_btn_del  = SDL_Rect{ rect.x + 18 + 20 * cw, rect.y + 3, 9 * cw, 20 };
    button(m_btn_load, "LOAD", in_rect(m_btn_load, m_mx, m_my));
    button(m_btn_add,  "ADD",  in_rect(m_btn_add,  m_mx, m_my));
    button(m_btn_del,  "DEL",  in_rect(m_btn_del,  m_mx, m_my));

    const SDL_Rect view{ rect.x, rect.y + 28, rect.w - 12, std::max(1, rect.h - 28) };
    if (m_scroll_y < 0) m_scroll_y = 0;
    if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;
    SDL_Rect oldClip; SDL_RenderGetClipRect(app.ren, &oldClip);
    SDL_RenderSetClipRect(app.ren, &view);

    const int topY = rect.y + 32 - m_scroll_y;
    // --- zone list (left) ---------------------------------------------------
    const int listW = std::max(24 * cw, std::min(34 * cw, rect.w / 4));
    const int gap = 8;
    SDL_Rect list{ rect.x + 4, topY, listW, std::max(118, (rect.h - 40) / 3) };
    pane(list, "ZONES");
    m_zone_rows.clear();
    if (m_zones) {
        int y = list.y + ch + 12;
        for (size_t i = 0; i < m_zones->size(); ++i) {
            SDL_Rect row{ list.x + 2, y, list.w - 4, ch + 4 };
            m_zone_rows.push_back(row);
            const SamplerZone& z = (*m_zones)[i];
            const bool sel = (int)i == m_sel;
            if (sel) fill_rect(app.ren, row, t.sel);
            char lo[8], hi[8], rt[8]; note_name(z.loKey, lo, 8); note_name(z.hiKey, hi, 8); note_name(z.root, rt, 8);
            std::snprintf(buf, sizeof(buf), "%02d  %s..%s  r%s", (int)i + 1, lo, hi, rt);
            SDL_Rect tx{ row.x + 3, row.y + 1, row.w - 6, row.h - 2 };
            app.mono.draw_fitted(app.ren, tx, buf, sel ? t.bg : t.text, false);
            y += ch + 4;
        }
        if (m_zones->empty())
            app.mono.draw(app.ren, list.x + 6, list.y + ch + 14, "drop WAVs here", t.dim);
    }

    // --- disk browser + sample preview (left lower pane) -------------------
    m_browser_rect = SDL_Rect{ list.x, list.y + list.h + gap, list.w,
                               std::max(180, view.h - list.h - gap - 4) };
    pane(m_browser_rect, "DISK BROWSER");
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
    const int previewH = std::max(54, std::min(82, m_browser_rect.h / 4));
    const int rowsH = std::max(20, m_browser_rect.h - (browY - m_browser_rect.y) - previewH - 8);
    SDL_Rect browserRows{ m_browser_rect.x + 2, browY, m_browser_rect.w - 4, rowsH };
    fill_rect(app.ren, browserRows, t.bg);
    m_browser_rows.clear();
    int first = std::max(0, m_browser_scroll);
    int rowH = ch + 4;
    int visibleRows = rowsH / rowH;
    for (int i = 0; i < visibleRows && first + i < (int)m_browser.size(); ++i) {
        int idx = first + i;
        SDL_Rect row{ browserRows.x + 1, browserRows.y + i * rowH, browserRows.w - 2, rowH };
        m_browser_rows.push_back(row);
        const BrowserItem& bi = m_browser[idx];
        bool sel = idx == m_browser_sel;
        if (sel) fill_rect(app.ren, row, t.sel);
        std::string nm = bi.dir ? ("[" + bi.name + "]") : bi.name;
        SDL_Rect tx{ row.x + 3, row.y + 1, row.w - 6, row.h - 2 };
        app.mono.draw_fitted(app.ren, tx, nm, sel ? t.bg : (bi.dir ? t.accent : t.text), false);
    }
    frame_rect(app.ren, browserRows, t.dim);
    m_drive_rows.clear();
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
            bool hot = in_rect(row, m_mx, m_my);
            if (hot) fill_rect(app.ren, row, t.accent);
            SDL_Rect tx{ row.x + 4, row.y + 1, row.w - 8, row.h - 2 };
            app.mono.draw_fitted(app.ren, tx, drives[i], hot ? t.bg : t.text, false);
        }
    }
    m_preview_rect = SDL_Rect{ m_browser_rect.x + 2, browserRows.y + browserRows.h + 4,
                               m_browser_rect.w - 4, previewH };
    if (m_preview_ok) draw_wave(app, m_preview_rect, m_preview_clip);
    else {
        fill_rect(app.ren, m_preview_rect, t.bg);
        frame_rect(app.ren, m_preview_rect, t.dim);
        SDL_Rect tx{ m_preview_rect.x + 5, m_preview_rect.y + 2,
                     m_preview_rect.w - 10, m_preview_rect.h - 4 };
        app.mono.draw_fitted(app.ren, tx, "preview WAVs; drag to zones/map", t.dim, true);
    }

    const int rightX = list.x + list.w + gap;
    const int rightW = rect.x + rect.w - rightX - 16;

    // --- Renoise-style keyzone grid: notes X, velocity Y --------------------
    m_keyzone_grid = SDL_Rect{ rightX, topY, rightW, std::max(126, std::min(160, rect.h / 4)) };
    pane(m_keyzone_grid, "KEYZONES");
    app.mono.draw(app.ren, m_keyzone_grid.x + 14 * cw, m_keyzone_grid.y + 4,
                  "notes ->    velocity ^    drag edges/root/blocks", t.dim);
    SDL_Rect grid{ m_keyzone_grid.x + 6, m_keyzone_grid.y + ch + 12,
                   m_keyzone_grid.w - 12, m_keyzone_grid.h - ch - 34 };
    fill_rect(app.ren, grid, t.bg);
    frame_rect(app.ren, grid, t.dim);
    for (int oct = 0; oct <= 10; ++oct) {
        int x = key_left_x(grid, oct * 12);
        vline(app.ren, x, grid.y, grid.y + grid.h - 1, t.dim);
    }
    for (int vv = 0; vv <= 4; ++vv) {
        int y = grid.y + grid.h - (int)((double)vv / 4.0 * grid.h);
        hline(app.ren, grid.x, grid.x + grid.w - 1, y, t.dim);
    }
    if (m_zones) {
        for (size_t i = 0; i < m_zones->size(); ++i) {
            const SamplerZone& z = (*m_zones)[i];
            int x0 = key_left_x(grid, z.loKey);
            int x1 = key_right_x(grid, z.hiKey);
            int y0 = grid.y + grid.h - (int)((double)(z.hiVel + 1) / 128.0 * grid.h);
            int y1 = grid.y + grid.h - (int)((double)z.loVel / 128.0 * grid.h);
            SDL_Rect zr{ x0, y0, std::max(3, x1 - x0), std::max(3, y1 - y0) };
            const bool sel = (int)i == m_sel;
            fill_rect(app.ren, zr, z.noteOffLayer ? t.chord : (sel ? t.accent : t.active));
            frame_rect(app.ren, zr, sel ? t.hi : t.dim);
            int rx = key_center_x(grid, z.root);
            vline(app.ren, rx, zr.y, zr.y + zr.h - 1, sel ? t.bg : t.hi);
        }
    }
    if (m_drag == Drag::BrowserSample && m_drag_hover_key >= 0) {
        int hx0 = key_left_x(grid, m_drag_hover_key);
        int hx1 = key_right_x(grid, m_drag_hover_key);
        SDL_Rect hover{ hx0, grid.y, std::max(3, hx1 - hx0), grid.h };
        fill_rect(app.ren, hover, t.sel);
        frame_rect(app.ren, hover, t.hi);
        char kn[8]; note_name(m_drag_hover_key, kn, 8);
        SDL_Rect tag{ std::max(grid.x, std::min(hx0 - 18, grid.x + grid.w - 42)),
                      grid.y + 4, 42, ch + 6 };
        fill_rect(app.ren, tag, t.accent);
        frame_rect(app.ren, tag, t.hi);
        app.mono.draw_fitted(app.ren, SDL_Rect{ tag.x + 3, tag.y + 1, tag.w - 6, tag.h - 2 },
                             kn, t.bg, true);
    }
    m_strip = SDL_Rect{ grid.x, m_keyzone_grid.y + m_keyzone_grid.h - 17, grid.w, 14 };
    fill_rect(app.ren, m_strip, t.bg);
    for (int oct = 0; oct <= 10; ++oct) {
        int x = key_left_x(m_strip, oct * 12);
        vline(app.ren, x, m_strip.y, m_strip.y + m_strip.h - 1, t.dim);
    }

    // --- selected zone waveform + controls ---------------------------------
    const int editTop = m_keyzone_grid.y + m_keyzone_grid.h + gap;
    const int editH = std::max(240, std::min(280, rect.h / 3));
    const int zoneCtlW = std::max(38 * cw, std::min(56 * cw, rightW / 3));
    SDL_Rect wavePane{ rightX, editTop, std::max(160, rightW - zoneCtlW - gap), editH };
    SDL_Rect zonePane{ wavePane.x + wavePane.w + gap, editTop, zoneCtlW, editH };
    pane(wavePane, "WAVEFORM");
    pane(zonePane, "ZONE EDIT");
    m_wave = SDL_Rect{ wavePane.x + 6, wavePane.y + ch + 12, wavePane.w - 12, wavePane.h - ch - 18 };
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        SamplerZone& z = (*m_zones)[m_sel];
        draw_wave(app, m_wave, z.clip);
        int sx = m_wave.x + (int)(std::max(0.f, std::min(1.f, z.start)) * m_wave.w);
        int ex = m_wave.x + (int)(std::max(0.f, std::min(1.f, z.end)) * m_wave.w);
        vline(app.ren, sx, m_wave.y, m_wave.y + m_wave.h - 1, t.hi);
        vline(app.ren, ex, m_wave.y, m_wave.y + m_wave.h - 1, t.hi);

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
        m_btn_crop = SDL_Rect{ m_btn_loop.x + m_btn_loop.w + 4, py0, 6 * cw, 20 };
        m_btn_norm = SDL_Rect{ zonePane.x + 6, py0 + 24, 8 * cw, 20 };
        m_btn_dc = SDL_Rect{ m_btn_norm.x + m_btn_norm.w + 4, py0 + 24, 9 * cw, 20 };
        m_btn_zerotrim = SDL_Rect{ m_btn_dc.x + m_btn_dc.w + 4, py0 + 24, 10 * cw, 20 };
        m_btn_fadein = SDL_Rect{ zonePane.x + 6 + 24 * cw, py0, 8 * cw, 20 };
        m_btn_fadeout = SDL_Rect{ zonePane.x + 6 + 24 * cw, py0 + 24, 9 * cw, 20 };
        button(m_btn_rev,  z.reverse ? "REV ON" : "REV", in_rect(m_btn_rev,  m_mx, m_my));
        button(m_btn_loop, z.loop ? "LOOP ON" : "LOOP", in_rect(m_btn_loop, m_mx, m_my));
        button(m_btn_crop, "CROP", in_rect(m_btn_crop, m_mx, m_my));
        button(m_btn_norm, "NORM", in_rect(m_btn_norm, m_mx, m_my));
        button(m_btn_dc, "DC", in_rect(m_btn_dc, m_mx, m_my));
        button(m_btn_zerotrim, "ZERO", in_rect(m_btn_zerotrim, m_mx, m_my));
        button(m_btn_fadein, "FADE IN", in_rect(m_btn_fadein, m_mx, m_my));
        button(m_btn_fadeout, "FADE OUT", in_rect(m_btn_fadeout, m_mx, m_my));
    } else { fill_rect(app.ren, m_wave, t.bg); frame_rect(app.ren, m_wave, t.dim); }

    // --- envelope editor (amp/pitch/cutoff/res/pan, per-stage curves) ------
    SDL_Rect envR{ rightX, editTop + editH + gap, rightW, 118 };
    draw_env(app, envR);

    // --- parameter sliders (the machine's exposed params) ------------------
    m_param_rows.clear();
    int py = envR.y + envR.h + 8;
    if (m_inst) {
        app.mono.draw(app.ren, rightX, py, "PARAMETERS (tracker FX)", t.accent); py += ch + 4;
        const int pc = m_inst->paramCount();
        const int rowH = ch + 6, labelW = 12 * cw;
        for (int i = 0; i < pc; ++i) {
            PatchKnob::engine::ParamInfo pi = m_inst->paramInfo(i);
            SDL_Rect track{ rightX + labelW, py + 2, rightW - labelW - 8, rowH - 4 };
            m_param_rows.push_back(track);
            if (py + rowH >= view.y && py <= view.y + view.h) {
                char nm[24]; std::snprintf(nm, sizeof(nm), "%.11s", pi.name.c_str());
                app.mono.draw_fitted(app.ren,
                                     SDL_Rect{ rightX, py, std::max(10, track.x - rightX - 6), rowH },
                                     nm, t.text, false);
                fill_rect(app.ren, track, t.panel);
                float v = m_inst->getParamNormalized(pi.id);
                SDL_Rect fill{ track.x, track.y, (int)(track.w * std::max(0.f, std::min(1.f, v))), track.h };
                fill_rect(app.ren, fill, t.accent);
                frame_rect(app.ren, track, t.dim);
            }
            py += rowH;
        }
    }

    const int contentBottom = py + 12 + m_scroll_y;
    m_scroll_max = std::max(0, contentBottom - (view.y + view.h));
    if (m_scroll_y > m_scroll_max) m_scroll_y = m_scroll_max;
    SDL_RenderSetClipRect(app.ren, oldClip.w || oldClip.h ? &oldClip : nullptr);

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
}

bool SamplerEditorView::on_mouse(App& app, const MouseEv& e) {
    m_mx = e.x; m_my = e.y;
    const int ch = app.mono.ch();
    const SDL_Rect view{ rect.x, rect.y + 28, rect.w - 12, std::max(1, rect.h - 28) };
    if (!e.pressed) {
        if (m_scroll_drag) {
            m_scroll_drag = false;
            app.request_redraw();
            return true;
        }
        if (m_drag == Drag::BrowserSample && !m_drag_path.empty() && on_load_path) {
            int targetLevel = -1;
            int targetKey = -1;
            for (size_t i = 0; i < m_zone_rows.size(); ++i)
                if (in_rect(m_zone_rows[i], e.x, e.y)) { targetLevel = (int)i; break; }
            SDL_Rect grid{ m_keyzone_grid.x + 6, m_keyzone_grid.y + ch + 12,
                           m_keyzone_grid.w - 12, m_keyzone_grid.h - ch - 34 };
            if (in_rect(grid, e.x, e.y)) targetKey = key_at_x(e.x, grid);
            else if (in_rect(m_strip, e.x, e.y)) targetKey = key_at_x(e.x, m_strip);
            if (targetLevel >= 0 || targetKey >= 0)
                on_load_path(m_drag_path, targetLevel, targetKey);
        }
        // On release, push edits to the engine ONCE: envelope drags -> on_env,
        // keyrange/zone drags -> on_apply.
        if ((m_drag == Drag::EnvNode || m_drag == Drag::EnvCurve) && on_env) on_env(m_cur_env);
        else if (m_drag != Drag::None && m_drag != Drag::BrowserSample && on_apply) on_apply();
        m_drag = Drag::None; m_param_drag = -1; m_env_node = -1; m_env_seg = -1;
        m_drag_path.clear();
        m_drag_hover_key = -1;
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

    if (m_drive_menu) {
        const std::vector<std::string> drives = drive_roots();
        for (size_t i = 0; i < m_drive_rows.size() && i < drives.size(); ++i) {
            if (in_rect(m_drive_rows[i], e.x, e.y)) {
                m_browser_dir = drives[i];
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

    if (in_rect(m_drive_rect, e.x, e.y)) {
        m_drive_menu = !m_drive_menu;
        m_browser_active = true;
        app.request_redraw();
        return true;
    }

    // motion while a drag is active (held-button events)
    if (m_drag != Drag::None || m_param_drag >= 0) {
        if (m_drag == Drag::BrowserSample) {
            SDL_Rect grid{ m_keyzone_grid.x + 6, m_keyzone_grid.y + ch + 12,
                           m_keyzone_grid.w - 12, m_keyzone_grid.h - ch - 34 };
            if (in_rect(grid, e.x, e.y)) m_drag_hover_key = key_at_x(e.x, grid);
            else if (in_rect(m_strip, e.x, e.y)) m_drag_hover_key = key_at_x(e.x, m_strip);
            else m_drag_hover_key = -1;
            app.request_redraw();
            return true;
        }
        if (m_param_drag >= 0 && m_inst && m_param_drag < (int)m_param_rows.size()) {
            const SDL_Rect& tr = m_param_rows[m_param_drag];
            float v = (float)(e.x - tr.x) / (float)std::max(1, tr.w);
            m_inst->setParamNormalized(m_inst->paramInfo(m_param_drag).id, std::max(0.f, std::min(1.f, v)));
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
            SDL_Rect grid{ m_keyzone_grid.x + 6, m_keyzone_grid.y + ch + 12,
                           m_keyzone_grid.w - 12, m_keyzone_grid.h - ch - 34 };
            int key = key_at_x(e.x, grid);
            int vel = 127 - std::max(0, std::min(127,
                (int)std::lround((double)(e.y - grid.y) / std::max(1, grid.h) * 127.0)));
            if      (m_drag == Drag::ZoneLo)   z.loKey = std::min(key, z.hiKey);
            else if (m_drag == Drag::ZoneHi)   z.hiKey = std::max(key, z.loKey);
            else if (m_drag == Drag::ZoneVelLo) z.loVel = std::min(vel, z.hiVel);
            else if (m_drag == Drag::ZoneVelHi) z.hiVel = std::max(vel, z.loVel);
            else if (m_drag == Drag::ZoneRoot) z.root  = key;
            else if (m_drag == Drag::ZoneMove) {
                int width = z.hiKey - z.loKey;
                int center = std::max(0, std::min(127, key));
                z.loKey = std::max(0, std::min(127 - width, center - width / 2));
                z.hiKey = std::min(127, z.loKey + width);
            }
            else if (m_drag == Drag::ZoneStart) {
                z.start = std::max(0.f, std::min(z.end - 0.001f, (float)(e.x - m_wave.x) / std::max(1, m_wave.w)));
            } else if (m_drag == Drag::ZoneEnd) {
                z.end = std::min(1.f, std::max(z.start + 0.001f, (float)(e.x - m_wave.x) / std::max(1, m_wave.w)));
            }
            clamp_zone_keys(z);
        }
        app.request_redraw(); return true;
    }

    // toolbar buttons
    if (in_rect(m_btn_load, e.x, e.y)) { if (on_load) on_load(m_sel); return true; }
    if (in_rect(m_btn_add,  e.x, e.y)) { if (on_load) on_load(-1); return true; }   // append a new zone
    if (in_rect(m_btn_del,  e.x, e.y)) {
        if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
            m_zones->erase(m_zones->begin() + m_sel);
            if (m_sel >= (int)m_zones->size()) m_sel = (int)m_zones->size() - 1;
            if (on_apply) on_apply();
        }
        app.request_redraw(); return true;
    }
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size()) {
        SamplerZone& z = (*m_zones)[m_sel];
        if (in_rect(m_btn_rev, e.x, e.y)) {
            z.reverse = !z.reverse;
            if (on_apply) on_apply();
            app.request_redraw(); return true;
        }
        if (in_rect(m_btn_loop, e.x, e.y)) {
            z.loop = !z.loop;
            if (on_apply) on_apply();
            app.request_redraw(); return true;
        }
        if (in_rect(m_btn_note_on, e.x, e.y)) {
            z.noteOffLayer = false; if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_note_off, e.x, e.y)) {
            z.noteOffLayer = true; if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_keypitch, e.x, e.y)) {
            z.keyToPitch = !z.keyToPitch; if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_velvol, e.x, e.y)) {
            z.velToVol = !z.velToVol; if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_overlap, e.x, e.y)) {
            z.overlapMode = (z.overlapMode + 1) % 3; if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_lo_dec, e.x, e.y)) {
            z.loKey = std::max(0, z.loKey - 1); clamp_zone_keys(z);
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_lo_inc, e.x, e.y)) {
            z.loKey = std::min(z.hiKey, z.loKey + 1); clamp_zone_keys(z);
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_hi_dec, e.x, e.y)) {
            z.hiKey = std::max(z.loKey, z.hiKey - 1); clamp_zone_keys(z);
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_hi_inc, e.x, e.y)) {
            z.hiKey = std::min(127, z.hiKey + 1); clamp_zone_keys(z);
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_drumkit, e.x, e.y) && m_zones) {
            for (size_t i = 0; i < m_zones->size(); ++i) {
                SamplerZone& dz = (*m_zones)[i];
                int key = std::max(0, std::min(127, dz.root));
                dz.loKey = dz.hiKey = key;
            }
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_distribute, e.x, e.y) && m_zones && !m_zones->empty()) {
            const int count = (int)m_zones->size();
            for (int i = 0; i < count; ++i) {
                SamplerZone& dz = (*m_zones)[(size_t)i];
                dz.loKey = (i * 128) / count;
                dz.hiKey = ((i + 1) * 128) / count - 1;
                dz.root = (dz.loKey + dz.hiKey) / 2;
            }
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_layer, e.x, e.y) && m_zones && !m_zones->empty()) {
            const int count = (int)m_zones->size();
            for (int i = 0; i < count; ++i) {
                SamplerZone& dz = (*m_zones)[(size_t)i];
                dz.loVel = (i * 128) / count;
                dz.hiVel = ((i + 1) * 128) / count - 1;
            }
            if (on_apply) on_apply(); app.request_redraw(); return true;
        }
        if (in_rect(m_btn_norm, e.x, e.y)) {
            float peak = 0.f;
            const int64_t n = z.clip.numFrames();
            int64_t a = (int64_t)(std::max(0.f, std::min(1.f, z.start)) * n);
            int64_t b = (int64_t)(std::max(0.f, std::min(1.f, z.end)) * n);
            if (b <= a) { a = 0; b = n; }
            for (int chn = 0; chn < 2; ++chn)
                for (int64_t i = a; i < b && i < z.clip.numFrames(); ++i)
                    peak = std::max(peak, std::fabs(z.clip.ch[chn][(size_t)i]));
            if (peak > 1e-6f) z.gain = 0.98f / peak;
            if (on_apply) on_apply();
            app.request_redraw(); return true;
        }
        if (in_rect(m_btn_crop, e.x, e.y)) {
            const int64_t n = z.clip.numFrames();
            int64_t a = (int64_t)(std::max(0.f, std::min(1.f, z.start)) * n);
            int64_t b = (int64_t)(std::max(0.f, std::min(1.f, z.end)) * n);
            if (b > a && b <= n) {
                for (int chn = 0; chn < 2; ++chn) {
                    std::vector<float> v(z.clip.ch[chn].begin() + (size_t)a,
                                         z.clip.ch[chn].begin() + (size_t)b);
                    z.clip.ch[chn].swap(v);
                }
                z.start = 0.f; z.end = 1.f;
                if (on_apply) on_apply();
            }
            app.request_redraw(); return true;
        }
        if (in_rect(m_btn_fadein, e.x, e.y) || in_rect(m_btn_fadeout, e.x, e.y)) {
            const bool fadeIn = in_rect(m_btn_fadein, e.x, e.y);
            const int64_t n = z.clip.numFrames();
            int64_t a = (int64_t)(std::max(0.f, std::min(1.f, z.start)) * n);
            int64_t b = (int64_t)(std::max(0.f, std::min(1.f, z.end)) * n);
            if (b <= a) { a = 0; b = n; }
            const int64_t len = std::max<int64_t>(1, b - a);
            for (int chn = 0; chn < 2; ++chn)
                for (int64_t i = a; i < b && i < n; ++i) {
                    float u = (float)(i - a) / (float)len;
                    float g = fadeIn ? u : (1.f - u);
                    z.clip.ch[chn][(size_t)i] *= std::max(0.f, std::min(1.f, g));
                }
            if (on_apply) on_apply();
            app.request_redraw(); return true;
        }
        if (in_rect(m_btn_dc, e.x, e.y)) {
            const int64_t n = z.clip.numFrames();
            if (n > 0) {
                for (int chn = 0; chn < 2; ++chn) {
                    double mean = 0.0;
                    for (float v : z.clip.ch[chn]) mean += v;
                    mean /= (double)n;
                    for (float& v : z.clip.ch[chn]) v -= (float)mean;
                }
                if (on_apply) on_apply();
            }
            app.request_redraw(); return true;
        }
        if (in_rect(m_btn_zerotrim, e.x, e.y)) {
            const int64_t n = z.clip.numFrames();
            int64_t a = 0, b = n;
            const float th = 1e-4f;
            while (a < b && std::fabs(z.clip.ch[0][(size_t)a]) < th && std::fabs(z.clip.ch[1][(size_t)a]) < th) ++a;
            while (b > a && std::fabs(z.clip.ch[0][(size_t)b - 1]) < th && std::fabs(z.clip.ch[1][(size_t)b - 1]) < th) --b;
            if (b > a && (a > 0 || b < n)) {
                for (int chn = 0; chn < 2; ++chn) {
                    std::vector<float> v(z.clip.ch[chn].begin() + (size_t)a,
                                         z.clip.ch[chn].begin() + (size_t)b);
                    z.clip.ch[chn].swap(v);
                }
                z.start = 0.f; z.end = 1.f;
                if (on_apply) on_apply();
            }
            app.request_redraw(); return true;
        }
        if (in_rect(m_wave, e.x, e.y)) {
            int sx = m_wave.x + (int)(z.start * m_wave.w);
            int ex = m_wave.x + (int)(z.end * m_wave.w);
            if (std::abs(e.x - sx) <= 6) m_drag = Drag::ZoneStart;
            else if (std::abs(e.x - ex) <= 6) m_drag = Drag::ZoneEnd;
            app.request_redraw(); return true;
        }
    }
    // disk browser: dirs enter, WAV rows preview and become draggable.
    for (size_t i = 0; i < m_browser_rows.size(); ++i)
        if (in_rect(m_browser_rows[i], e.x, e.y)) {
            int idx = m_browser_scroll + (int)i;
            if (idx >= 0 && idx < (int)m_browser.size()) {
                m_browser_active = true;
                select_browser(idx, true);
                if (idx >= 0 && idx < (int)m_browser.size() && !m_browser[idx].dir) {
                    m_drag = Drag::BrowserSample;
                    m_drag_path = m_browser[idx].path;
                    m_drag_hover_key = -1;
                }
            }
            app.request_redraw(); return true;
        }
    // zone-list selection
    for (size_t i = 0; i < m_zone_rows.size(); ++i)
        if (in_rect(m_zone_rows[i], e.x, e.y)) { m_browser_active = false; m_sel = (int)i; app.request_redraw(); return true; }

    // keyzone grid: select / drag note edges, velocity edges, body, or root marker
    SDL_Rect grid{ m_keyzone_grid.x + 6, m_keyzone_grid.y + ch + 12,
                   m_keyzone_grid.w - 12, m_keyzone_grid.h - ch - 34 };
    if (in_rect(grid, e.x, e.y) && m_zones) {
        int key = key_at_x(e.x, grid);
        int zi = zone_at_key(key);
        if (m_sel >= 0 && m_sel < (int)m_zones->size()) {
            const SamplerZone& selected = (*m_zones)[m_sel];
            int sxlo = key_left_x(grid, selected.loKey);
            int sxhi = key_right_x(grid, selected.hiKey);
            int sr = key_center_x(grid, selected.root);
            if (key >= selected.loKey && key <= selected.hiKey &&
                (std::abs(e.x - sxlo) <= 8 || std::abs(e.x - sxhi) <= 8 || std::abs(e.x - sr) <= 6))
                zi = m_sel;
        }
        if (zi >= 0) {
            m_sel = zi;
            const SamplerZone& z = (*m_zones)[zi];
            int xlo = key_left_x(grid, z.loKey);
            int xhi = key_right_x(grid, z.hiKey);
            int yhi = grid.y + grid.h - (int)((double)(z.hiVel + 1) / 128.0 * grid.h);
            int ylo = grid.y + grid.h - (int)((double)z.loVel / 128.0 * grid.h);
            int rx = key_center_x(grid, z.root);
            if (std::abs(e.x - xlo) <= 6)      m_drag = Drag::ZoneLo;
            else if (std::abs(e.x - xhi) <= 6) m_drag = Drag::ZoneHi;
            else if (std::abs(e.y - ylo) <= 6) m_drag = Drag::ZoneVelLo;
            else if (std::abs(e.y - yhi) <= 6) m_drag = Drag::ZoneVelHi;
            else if (std::abs(e.x - rx) <= 5)  m_drag = Drag::ZoneRoot;
            else                               m_drag = Drag::ZoneMove;
        }
        app.request_redraw(); return true;
    }
    // envelope: tabs, then canvas (node drag / curve handle / add / delete / sustain)
    for (int i = 0; i < ENV_COUNT; ++i)
        if (in_rect(m_env_tabs[i], e.x, e.y)) { m_cur_env = i; app.request_redraw(); return true; }
    if (m_envs && in_rect(m_env_rect, e.x, e.y)) {
        SamplerEnv& en = m_envs->env[m_cur_env];
        const int ni = env_node_at(e.x, e.y, m_env_rect);
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if (e.button == SDL_BUTTON_RIGHT) {                       // delete an interior node
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
        // curve handle on a segment? (drawn at the shaped midpoint)
        for (int s = 0; s + 1 < (int)en.nodes.size(); ++s) {
            SDL_Point a = env_pt(m_env_rect, en.nodes[s]), b = env_pt(m_env_rect, en.nodes[s + 1]);
            float yy = en.nodes[s].y + (en.nodes[s + 1].y - en.nodes[s].y) * env_shape(0.5f, en.nodes[s].curve);
            int hx = (a.x + b.x) / 2, hy = m_env_rect.y + (int)((1.f - yy) * m_env_rect.h);
            if (std::abs(e.x - hx) <= 5 && std::abs(e.y - hy) <= 5) {
                m_drag = Drag::EnvCurve; m_env_seg = s; app.request_redraw(); return true;
            }
        }
        // empty space -> add a node here (and enable the envelope), then drag it
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
    for (size_t i = 0; i < m_param_rows.size(); ++i)
        if (in_rect(m_param_rows[i], e.x, e.y)) {
            m_param_drag = (int)i;
            if (m_inst) {
                float v = (float)(e.x - m_param_rows[i].x) / (float)std::max(1, m_param_rows[i].w);
                m_inst->setParamNormalized(m_inst->paramInfo((int)i).id, std::max(0.f, std::min(1.f, v)));
            }
            app.request_redraw(); return true;
        }
    return true;
}

bool SamplerEditorView::on_wheel(App& app, int, int dy) {
    int mx = m_mx, my = m_my;
    SDL_GetMouseState(&mx, &my);
    if (in_rect(m_browser_rect, mx, my)) {
        m_browser_scroll += (dy > 0 ? -3 : 3);
        int rowH = app.mono.ch() + 4;
        int visibleRows = rowH > 0 ? std::max(1, (m_browser_rect.h - 70) / rowH) : 1;
        int maxScroll = std::max(0, (int)m_browser.size() - visibleRows);
        if (m_browser_scroll < 0) m_browser_scroll = 0;
        if (m_browser_scroll > maxScroll) m_browser_scroll = maxScroll;
        app.request_redraw();
        return true;
    }
    // wheel over the strip nudges the selected zone's root note.
    if (m_zones && m_sel >= 0 && m_sel < (int)m_zones->size() && in_rect(m_strip, mx, my)) {
        SamplerZone& z = (*m_zones)[m_sel];
        z.root = std::max(0, std::min(127, z.root + (dy > 0 ? 1 : -1)));
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

bool SamplerEditorView::on_key(App& app, SDL_Keycode k) {
    if (!m_browser_active)
        return false;

    if (k == SDLK_UP || k == SDLK_DOWN) {
        if (step_browser(k == SDLK_UP ? -1 : 1, true)) {
            app.request_redraw();
            return true;
        }
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
