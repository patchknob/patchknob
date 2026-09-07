//----------------------------------------------------------------------------
//  sdlui/views/plugin_picker/plugin_picker_view.cpp
//----------------------------------------------------------------------------
#include "plugin_picker_view.h"
#include <algorithm>
#include <string>

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

// Compact right-hand I/O column.  "2>2" for the ordinary stereo effect, but
// "0>2x8" for an 8-bus multi-out instrument -- the bus COUNT is the part that
// tells a user this plugin has more than one output pair to patch, which a flat
// "0>16" total does not.
std::string PluginPickerView::io_summary(const PatchKnob::engine::PluginDescriptor& d) {
    auto side = [](const std::vector<PatchKnob::engine::PluginBusInfo>& buses,
                   int flatTotal) {
        if (buses.size() > 1) {
            // Uniform bus widths (the common multi-out shape) read as WxN.
            bool uniform = true;
            for (const auto& b : buses)
                if (b.channelCount != buses[0].channelCount) { uniform = false; break; }
            if (uniform)
                return std::to_string(buses[0].channelCount) + "x" +
                       std::to_string((int)buses.size());
            return std::to_string(flatTotal) + "/" +
                   std::to_string((int)buses.size()) + "bus";
        }
        return std::to_string(flatTotal);
    };
    return side(d.audioInBuses, d.numAudioIn) + ">" +
           side(d.audioOutBuses, d.numAudioOut);
}

void PluginPickerView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    const int rh = row_h(app);
    int y = rect.y + 4 - m_scroll;

    if (!status.empty()) {
        app.font.draw(app.ren, rect.x + 8, y + 3, fit_text(app.font, status, rect.w - 16), t.dim);
        y += rh;
    }
    // An EMPTY plugin set is a valid state (a fresh install, or nothing on this
    // machine's VST paths) -- explain it instead of presenting a blank box, and
    // say which of the two empties this is: no inventory at all, or an
    // inventory with nothing that passes the instrument filter.
    std::vector<int> vis = visible_indices();
    if (vis.empty() && status.empty()) {
        const char* what = plugins.empty()
            ? "No plugins found."
            : "No instruments among the scanned plugins.";
        app.font.draw(app.ren, rect.x + 8, y + 3, what, t.dim);
        y += rh;
        app.font.draw(app.ren, rect.x + 8, y + 3,
                      fit_text(app.font, "Install VST2/VST3 plugins, then View > Rescan Plugins.",
                               rect.w - 16), t.dim);
    }

    for (int idx : vis) {
        if (y + rh > rect.y && y < rect.y + rect.h) {
            SDL_Rect rr{ rect.x + 3, y, rect.w - 6, rh - 2 };
            std::string name = plugins[(size_t)idx].name.empty()
                ? base_name(plugins[(size_t)idx].path)
                : base_name(plugins[(size_t)idx].name);
            const char* fmt = plugins[(size_t)idx].format == PatchKnob::engine::PluginFormat::VST3 ? "[VST3] " : "[VST2] ";
            const std::string io = io_summary(plugins[(size_t)idx]);
            const int iow = app.mono.text_w(io) + 10;
            app.font.draw(app.ren, rr.x + 6, y + 3,
                          fit_text(app.font, std::string(fmt) + name, rr.w - 12 - iow), t.text);
            app.mono.draw(app.ren, rr.x + rr.w - iow + 4, y + 3, io, t.dim);
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
