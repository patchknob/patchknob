//----------------------------------------------------------------------------
//  sdlui/views/plugin_param/plugin_param_view.cpp
//----------------------------------------------------------------------------
#include "plugin_param_view.h"
#include "engine/plugin_api.h"

#include <algorithm>
#include <cstdio>

using namespace ui;

namespace paramui {

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

int PluginParamView::row_h(App& app) const { return app.font.ch() + 12; }

SDL_Rect PluginParamView::slider_rect(int rowY, App& app) const {
    // left ~45% = label, right = slider
    int lx = rect.x + (int)(rect.w * 0.45);
    int rw = std::max(1, rect.w - (lx - rect.x) - 12);
    return SDL_Rect{ lx, rowY + 3, rw, app.font.ch() + 2 };
}

void PluginParamView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    if (!m_inst) { app.font.draw(app.ren, rect.x+8, rect.y+8, "(no plugin)", t.dim);
                   frame_rect(app.ren, rect, t.dim); return; }

    const int rh = row_h(app);
    const int np = m_inst->paramCount();
    if (np <= 0) app.font.draw(app.ren, rect.x+8, rect.y+8, "(plugin exposes no parameters)", t.dim);

    int y = rect.y + 4 - m_scroll;
    for (int i = 0; i < np; ++i) {
        if (y + rh > rect.y && y < rect.y + rect.h) {          // clip to body
            PatchKnob::engine::ParamInfo pi = m_inst->paramInfo(i);
            float v = m_inst->getParamNormalized(pi.id);
            if (v < 0) v = 0; if (v > 1) v = 1;
            app.font.draw(app.ren, rect.x + 8, y + 4,
                          fit_text(app.font, pi.name, (int)(rect.w * 0.45) - 14), t.text);
            // slider
            SDL_Rect sr = slider_rect(y, app);
            fill_rect(app.ren, sr, t.keybg);
            SDL_Rect fillq{ sr.x, sr.y, (int)(sr.w * v), sr.h };
            fill_rect(app.ren, fillq, (i==m_drag) ? t.hi : t.accent);
            frame_rect(app.ren, sr, t.dim);
            // value knob line
            int kx = sr.x + (int)(sr.w * v);
            vline(app.ren, kx, sr.y-1, sr.y + sr.h + 1, t.text);
        }
        y += rh;
    }
    frame_rect(app.ren, rect, t.dim);
}

void PluginParamView::apply_from_x(int paramIndex, int mouseX) {
    if (!m_inst) return;
    // slider x/width are row-independent (same layout as slider_rect()).
    int lx = rect.x + (int)(rect.w * 0.45);
    int rw = std::max(1, rect.w - (lx - rect.x) - 12);
    float v = (rw > 0) ? (float)(mouseX - lx) / (float)rw : 0.f;
    if (v < 0) v = 0; if (v > 1) v = 1;
    PatchKnob::engine::ParamInfo pi = m_inst->paramInfo(paramIndex);
    m_inst->setParamNormalized(pi.id, v);
}

bool PluginParamView::on_mouse(App& app, const MouseEv& e) {
    if (!m_inst) return true;
    if (!e.pressed) { m_drag = -1; return true; }
    const int rh = row_h(app);
    if (m_drag >= 0) { apply_from_x(m_drag, e.x); app.request_redraw(); return true; }
    const int np = m_inst->paramCount();
    int y = rect.y + 4 - m_scroll;
    for (int i = 0; i < np; ++i) {
        if (e.y >= y && e.y < y + rh) {
            SDL_Rect sr = slider_rect(y, app);
            if (e.x >= sr.x - 4 && e.x <= sr.x + sr.w + 4) {
                m_drag = i; apply_from_x(i, e.x); app.request_redraw();
            }
            return true;
        }
        y += rh;
    }
    return true;
}

bool PluginParamView::on_wheel(App& app, int /*dx*/, int dy) {
    m_scroll -= dy * (row_h(app) * 3);
    if (m_scroll < 0) m_scroll = 0;
    int maxScroll = std::max(0, (m_inst ? m_inst->paramCount() : 0) * row_h(app) + 8 - rect.h);
    if (m_scroll > maxScroll) m_scroll = maxScroll;
    app.request_redraw();
    return true;
}

} // namespace paramui
