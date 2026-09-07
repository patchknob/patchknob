#include "file_dialog.h"
#include "views/sample_slot/sample_browser.h"   // shared skin:: paint helpers

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace ui {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string extension_of(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}
std::string home_dir() {
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE")) return p;
#else
    if (const char* p = std::getenv("HOME")) return p;
#endif
    std::error_code ec;
    return fs::current_path(ec).string();
}
} // namespace

void FileDialog::open(const Options& opt) {
    m_opt = opt;
    m_active = true;
    m_ok = false;
    m_result.clear();
    m_filename = opt.defaultName;
    m_nameFocused = false;
    std::string start = opt.startDir;
    std::error_code ec;
    if (start.empty() || !fs::is_directory(start, ec)) start = home_dir();
    navigate(start);
}

bool FileDialog::matches_filter(const std::string& name) const {
    if (m_opt.foldersOnly || m_opt.extensions.empty()) return true;
    const std::string ext = extension_of(name);
    for (const auto& e : m_opt.extensions) if (lower(e) == ext) return true;
    return false;
}

void FileDialog::navigate(const std::string& dir) {
    std::error_code ec;
    fs::path p(dir);
    if (ec || !fs::exists(p, ec) || !fs::is_directory(p, ec)) {
        if (!m_dir.empty()) return;   // stay put rather than bounce somewhere unrelated
        p = fs::current_path(ec);
    }
    m_dir = p.string();
    m_sel = -1; m_scroll = 0;
    scan();
}

void FileDialog::scan() {
    m_items.clear();
    std::error_code ec;
    const fs::path here(m_dir);
    if (here.has_parent_path() && here.parent_path() != here) {
        Item up; up.name = ".."; up.path = here.parent_path().string(); up.dir = true;
        m_items.push_back(up);
    }
    std::vector<Item> dirs, files;
    for (fs::directory_iterator it(here, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        if (!name.empty() && name[0] == '.') continue;
        std::error_code e2;
        if (fs::is_directory(p, e2)) {
            Item d; d.name = name; d.path = p.string(); d.dir = true;
            dirs.push_back(d);
        } else if (!m_opt.foldersOnly && matches_filter(name)) {
            Item f; f.name = name; f.path = p.string(); f.dir = false;
            f.size = (long long)fs::file_size(p, e2);
            files.push_back(f);
        }
    }
    auto byName = [](const Item& a, const Item& b) { return lower(a.name) < lower(b.name); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (auto& d : dirs) m_items.push_back(d);
    for (auto& f : files) m_items.push_back(f);
}

int FileDialog::visible_rows() const { return m_list.h > 0 ? m_list.h / m_rowH : 0; }

int FileDialog::row_at(int y) const {
    if (y < m_list.y || y >= m_list.y + m_list.h) return -1;
    const int idx = (y - m_list.y) / m_rowH + m_scroll;
    return (idx >= 0 && idx < (int)m_items.size()) ? idx : -1;
}

void FileDialog::cancel() { m_ok = false; m_result.clear(); m_active = false; visible = false; }

void FileDialog::try_accept() {
    if (m_opt.foldersOnly) { m_ok = true; m_result = m_dir; m_active = false; visible = false; return; }
    if (m_opt.save) {
        std::string name = m_filename;
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        if (name.empty()) return;
        if (!m_opt.extensions.empty() && extension_of(name) != lower(m_opt.extensions[0]))
            name += "." + m_opt.extensions[0];
        fs::path full = fs::path(m_dir) / name;
        m_ok = true; m_result = full.string(); m_active = false; visible = false;
        return;
    }
    if (m_sel >= 0 && m_sel < (int)m_items.size() && !m_items[(size_t)m_sel].dir) {
        m_ok = true; m_result = m_items[(size_t)m_sel].path; m_active = false; visible = false;
    }
}

void FileDialog::accept_row(int index) {
    if (index < 0 || index >= (int)m_items.size()) return;
    const Item& it = m_items[(size_t)index];
    if (it.dir) { navigate(it.path); return; }
    if (m_opt.save) { m_filename = it.name; return; }   // single click already did this too
    m_ok = true; m_result = it.path; m_active = false; visible = false;
}

void FileDialog::draw(App& app) {
    const Theme& t = theme();
    const Font& f = app.font;
    const int pad = sampleslot::skin::pad(f);
    const int rowH = sampleslot::skin::row_h(f);
    m_rowH = rowH;

    const int pw = std::min(app.w - 2 * pad * 8, 900);
    const int ph = std::min(app.h - 2 * pad * 8, 640);
    m_panel = SDL_Rect{ (app.w - pw) / 2, (app.h - ph) / 2, pw, ph };

    sampleslot::skin::fill_round(app.ren, m_panel, pad, t.panel);
    sampleslot::skin::frame_round(app.ren, m_panel, pad, t.dim);

    int y = m_panel.y + pad * 2;
    const int lx = m_panel.x + pad * 2;
    const int rx = m_panel.x + m_panel.w - pad * 2;
    const std::string title = m_opt.title.empty()
        ? (m_opt.foldersOnly ? "Choose Folder" : (m_opt.save ? "Save" : "Open")) : m_opt.title;
    f.draw(app.ren, lx, y, title, t.text);
    y += f.ch() + pad;

    // Up + current path.
    m_upBtn = SDL_Rect{ lx, y, f.text_w("Up") + 2 * pad, rowH };
    sampleslot::skin::pill(app.ren, f, m_upBtn, "Up", sampleslot::skin::StIdle);
    const int pathX = m_upBtn.x + m_upBtn.w + pad;
    f.draw(app.ren, pathX, y + (rowH - f.ch()) / 2,
           sampleslot::skin::fit_tail(f, m_dir, rx - pathX), t.dim);
    y += rowH + pad;

    // List.
    const int listBottom = m_panel.y + m_panel.h - pad * 2 -
        (m_opt.save ? (rowH + pad) : 0) - (rowH + pad);
    m_list = SDL_Rect{ lx, y, rx - lx, std::max(0, listBottom - y) };
    sampleslot::skin::fill_round(app.ren, m_list, 2, Color{ t.bg.r, t.bg.g, t.bg.b, 255 });
    {
        ScopedClip clip(app.ren, m_list);
        int ry = m_list.y;
        for (int i = m_scroll; i < (int)m_items.size() && ry < m_list.y + m_list.h; ++i, ry += rowH) {
            const Item& it = m_items[(size_t)i];
            SDL_Rect rr{ m_list.x, ry, m_list.w, rowH };
            if (i == m_sel) sampleslot::skin::fill_round(app.ren, rr, 0, Color{ t.accent.r, t.accent.g, t.accent.b, 60 });
            const Color fg = it.dir ? t.accent : t.text;
            f.draw(app.ren, rr.x + pad, rr.y + (rowH - f.ch()) / 2,
                   sampleslot::skin::fit(f, it.dir ? ("/ " + it.name) : it.name, rr.w - 2 * pad), fg);
        }
    }

    // Filename field (save mode).
    if (m_opt.save) {
        m_nameField = SDL_Rect{ lx, listBottom + pad, m_list.w, rowH };
        sampleslot::skin::fill_round(app.ren, m_nameField, 2,
            m_nameFocused ? Color{ t.bg.r, t.bg.g, t.bg.b, 255 } : t.panel);
        sampleslot::skin::frame_round(app.ren, m_nameField, 2, m_nameFocused ? t.accent : t.dim);
        f.draw(app.ren, m_nameField.x + pad, m_nameField.y + (rowH - f.ch()) / 2, m_filename, t.text);
    } else {
        m_nameField = SDL_Rect{0,0,0,0};
    }

    // OK / Cancel.
    const int btnW = f.text_w("Cancel") + 4 * pad;
    m_cancelBtn = SDL_Rect{ rx - btnW, m_panel.y + m_panel.h - pad * 2 - rowH, btnW, rowH };
    m_okBtn = SDL_Rect{ m_cancelBtn.x - pad - btnW, m_cancelBtn.y, btnW, rowH };
    const bool okEnabled = m_opt.foldersOnly || m_opt.save ||
        (m_sel >= 0 && m_sel < (int)m_items.size() && !m_items[(size_t)m_sel].dir);
    sampleslot::skin::pill(app.ren, f, m_okBtn, m_opt.save ? "Save" : "Open",
                            okEnabled ? sampleslot::skin::StIdle : sampleslot::skin::StDisabled);
    sampleslot::skin::pill(app.ren, f, m_cancelBtn, "Cancel", sampleslot::skin::StIdle);
}

bool FileDialog::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) return true;
    if (e.x >= m_upBtn.x && e.x < m_upBtn.x + m_upBtn.w &&
        e.y >= m_upBtn.y && e.y < m_upBtn.y + m_upBtn.h) {
        fs::path here(m_dir);
        if (here.has_parent_path() && here.parent_path() != here) navigate(here.parent_path().string());
        app.request_redraw(); return true;
    }
    if (e.x >= m_okBtn.x && e.x < m_okBtn.x + m_okBtn.w &&
        e.y >= m_okBtn.y && e.y < m_okBtn.y + m_okBtn.h) { try_accept(); app.request_redraw(); return true; }
    if (e.x >= m_cancelBtn.x && e.x < m_cancelBtn.x + m_cancelBtn.w &&
        e.y >= m_cancelBtn.y && e.y < m_cancelBtn.y + m_cancelBtn.h) { cancel(); app.request_redraw(); return true; }
    if (m_opt.save && e.x >= m_nameField.x && e.x < m_nameField.x + m_nameField.w &&
        e.y >= m_nameField.y && e.y < m_nameField.y + m_nameField.h) {
        m_nameFocused = true; app.request_redraw(); return true;
    }
    m_nameFocused = false;
    const int idx = row_at(e.y);
    if (idx >= 0 && e.x >= m_list.x && e.x < m_list.x + m_list.w) {
        const Uint32 now = SDL_GetTicks();
        const bool doubleClick = (idx == m_sel) && (now - m_lastClickMs) < 400;
        m_sel = idx; m_lastClickMs = now;
        if (doubleClick) accept_row(idx);
        else if (m_opt.save && !m_items[(size_t)idx].dir) m_filename = m_items[(size_t)idx].name;
        app.request_redraw();
        return true;
    }
    return true;
}

bool FileDialog::on_wheel(App& app, int, int dy) {
    m_scroll -= dy;
    const int maxScroll = std::max(0, (int)m_items.size() - visible_rows());
    m_scroll = std::max(0, std::min(m_scroll, maxScroll));
    app.request_redraw();
    return true;
}

bool FileDialog::on_key(App& app, SDL_Keycode k) {
    if (k == SDLK_ESCAPE) { cancel(); app.request_redraw(); return true; }
    if (m_nameFocused) {
        if (k == SDLK_BACKSPACE) {
            if (!m_filename.empty()) m_filename.pop_back();
            app.request_redraw(); return true;
        }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { try_accept(); app.request_redraw(); return true; }
        return true;
    }
    if (k == SDLK_UP)   { if (m_sel > 0) --m_sel; else m_sel = 0; app.request_redraw(); return true; }
    if (k == SDLK_DOWN) { if (m_sel + 1 < (int)m_items.size()) ++m_sel; app.request_redraw(); return true; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        if (m_sel >= 0 && m_sel < (int)m_items.size() && m_items[(size_t)m_sel].dir) accept_row(m_sel);
        else try_accept();
        app.request_redraw(); return true;
    }
    if (k == SDLK_BACKSPACE) {
        fs::path here(m_dir);
        if (here.has_parent_path() && here.parent_path() != here) navigate(here.parent_path().string());
        app.request_redraw(); return true;
    }
    return true;
}

bool FileDialog::on_text(App& app, const char* utf8) {
    if (!m_nameFocused || !utf8) return true;
    m_filename += utf8;
    app.request_redraw();
    return true;
}

bool choose_path(App& app, const FileDialog::Options& opt, std::string& selected) {
    static FileDialog dlg;   // one instance reused across calls, like the old native wrappers
    dlg.open(opt);
    app.run_modal(dlg);
    if (!dlg.ok()) return false;
    selected = dlg.result();
    return true;
}

} // namespace ui
