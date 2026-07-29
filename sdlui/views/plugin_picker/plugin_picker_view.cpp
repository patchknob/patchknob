//----------------------------------------------------------------------------
//  sdlui/views/plugin_picker/plugin_picker_view.cpp
//----------------------------------------------------------------------------
#include "plugin_picker_view.h"
#include <algorithm>

using namespace ui;

namespace pickerui {

static std::string fit_text(const ui::Font& font, std::string text, int maxw) {
    if (maxw <= 0) return "";
    if (font.text_w(text) <= maxw) return text;
    const std::string ell = "...";
    if (font.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && font.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!text.empty() && font.text_w(text + ell) > maxw) text.pop_back();
    return text.empty() ? ell : text + ell;
}

std::string PluginPickerView::base_name(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    std::string b = (s == std::string::npos) ? p : p.substr(s + 1);
    // strip a trailing .vst3 / .dll for readability
    size_t d = b.find_last_of('.');
    if (d != std::string::npos) b = b.substr(0, d);
    return b;
}

std::vector<int> PluginPickerView::visible_indices() const {
    std::vector<int> v;
    for (int i = 0; i < (int)plugins.size(); ++i)
        if (!instrumentsOnly || plugins[i].isInstrument) v.push_back(i);
    return v;
}

int PluginPickerView::row_h(App& app) const { return app.font.ch() + 8; }

void PluginPickerView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    const int rh = row_h(app);
    int y = rect.y + 4 - m_scroll;

    if (!status.empty()) {
        app.font.draw(app.ren, rect.x + 8, y + 3, fit_text(app.font, status, rect.w - 16), t.dim);
        y += rh;
    }
    std::vector<int> vis = visible_indices();
    if (vis.empty() && status.empty())
        app.font.draw(app.ren, rect.x + 8, y + 3, "(no plugins found)", t.dim);

    for (int idx : vis) {
        if (y + rh > rect.y && y < rect.y + rect.h) {
            SDL_Rect rr{ rect.x + 3, y, rect.w - 6, rh - 2 };
            std::string name = plugins[(size_t)idx].name.empty()
                ? base_name(plugins[(size_t)idx].path)
                : base_name(plugins[(size_t)idx].name);
            const char* fmt = plugins[(size_t)idx].format == PatchKnob::engine::PluginFormat::VST3 ? "[VST3] " : "[VST2] ";
            app.font.draw(app.ren, rr.x + 6, y + 3,
                          fit_text(app.font, std::string(fmt) + name, rr.w - 12), t.text);
        }
        y += rh;
    }
    frame_rect(app.ren, rect, t.dim);
}

bool PluginPickerView::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) return true;
    const int rh = row_h(app);
    int y = rect.y + 4 - m_scroll;
    if (!status.empty()) y += rh;
    for (int idx : visible_indices()) {
        if (e.y >= y && e.y < y + rh) {
            if (on_pick) on_pick(plugins[(size_t)idx]);
            return true;
        }
        y += rh;
    }
    return true;
}

bool PluginPickerView::on_wheel(App& app, int /*dx*/, int dy) {
    m_scroll -= dy * (row_h(app) * 3);
    if (m_scroll < 0) m_scroll = 0;
    int rows = (int)visible_indices().size() + (status.empty() ? 0 : 1);
    int maxScroll = std::max(0, rows * row_h(app) + 8 - rect.h);
    if (m_scroll > maxScroll) m_scroll = maxScroll;
    app.request_redraw();
    return true;
}

} // namespace pickerui
