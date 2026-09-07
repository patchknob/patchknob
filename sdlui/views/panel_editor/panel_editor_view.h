//----------------------------------------------------------------------------
//  sdlui/views/panel_editor/panel_editor_view.h  (header-only)
//
//  Edits the FACEPLATE of a scripting rack module (Pd / Csound): drag the knobs
//  and jacks to lay them out, and drag the right edge to resize the panel WIDTH
//  (the height is fixed, VCV-style).  Draws on a BLACK background in any theme.
//  Layout is written straight into the module's per-instance PanelSpec via the
//  RackEngine, so the rack view + the save file pick it up.  An "Edit DSP" button
//  fires on_edit_dsp() so the host can open the module's patch (.csd / .pd).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PANEL_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_PANEL_EDITOR_VIEW_H

#include "gui.h"
#include "engine/rack/rack_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace paneled {

inline bool pe_in_rect(const SDL_Rect& q, int x, int y) {
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

class PanelEditorView : public ui::Widget {
public:
    std::function<void(int moduleId)> on_edit_dsp;   // open the DSP (patch) editor

    void set_target(rackx::RackEngine* eng, int moduleId) {
        m_eng = eng; m_mod = moduleId; m_drag = Drag::None; m_de = nullptr;
    }
    int module_id() const { return m_mod; }

    void draw(ui::App& app) override {
        if (!visible) return;
        SDL_Renderer* r = app.ren;
        const ui::Theme& t = ui::theme();
        ui::fill_rect(r, rect, ui::Color{0, 0, 0, 255});           // BLACK background (any theme)
        ui::frame_rect(r, rect, t.dim);

        rackx::PanelSpec* p = panel();
        const int th = app.mono.ch() + 8;
        SDL_Rect bar{ rect.x, rect.y, rect.w, th };
        ui::fill_rect(r, bar, t.panel);
        m_btn_dsp = SDL_Rect{ rect.x + 4, rect.y + 3, 84, th - 6 };
        m_btn_wm  = SDL_Rect{ rect.x + rect.w - 96, rect.y + 3, 28, th - 6 };
        m_btn_wp  = SDL_Rect{ rect.x + rect.w - 64, rect.y + 3, 28, th - 6 };
        auto btn = [&](const SDL_Rect& b, const char* label) {
            ui::fill_rect(r, b, t.bg); ui::frame_rect(r, b, t.dim);
            app.mono.draw_fitted(r, SDL_Rect{ b.x + 2, b.y + 2, b.w - 4, b.h - 4 }, label, t.text, true);
        };
        btn(m_btn_dsp, "Edit DSP"); btn(m_btn_wm, "W-"); btn(m_btn_wp, "W+");
        if (p) {
            char wl[32]; std::snprintf(wl, sizeof(wl), "%d HP", hp(*p));
            app.mono.draw(r, rect.x + rect.w - 150, rect.y + 4, wl, t.dim);
        }
        if (!p) { app.mono.draw(r, rect.x + 8, rect.y + th + 8, "no module selected", t.dim); return; }

        layout(app, *p);
        SDL_Rect pr = m_panelRect;
        ui::fill_rect(r, pr, ui::Color{18, 18, 18, 255});
        ui::frame_rect(r, pr, t.hi);

        // segment-style name display ABOVE the element (its Csound channel / Pd receive)
        auto seg_label = [&](int cx, int cy, int rr, const std::string& s) {
            if (s.empty()) return;
            const int tw = app.mono.text_w(s) + 8, sh = app.mono.ch() + 4;
            SDL_Rect lb{ cx - tw / 2, cy - rr - sh - 4, tw, sh };
            ui::fill_rect(r, lb, ui::Color{6, 12, 6, 255});        // dark LED backdrop
            ui::frame_rect(r, lb, t.dim);
            app.mono.draw_fitted(r, SDL_Rect{ lb.x + 2, lb.y + 2, lb.w - 4, lb.h - 4 },
                                 s, ui::Color{120, 255, 120, 255}, true);   // green LED text
        };
        for (auto& e : p->params) {                            // knobs = rings + pointer
            int cx, cy; elem_screen(e, cx, cy);
            const int rr = std::max(4, (int)std::lround(e.radius * m_scale));
            ring(r, cx, cy, rr, t.text);
            ui::set_color(r, t.text); SDL_RenderDrawLine(r, cx, cy, cx, cy - rr);
            seg_label(cx, cy, rr, e.label);
        }
        auto draw_jack = [&](rackx::PanelElement& e, ui::Color col) {
            int cx, cy; elem_screen(e, cx, cy);
            const int rr = std::max(3, (int)std::lround(e.radius * m_scale));
            ring(r, cx, cy, rr, col); ring(r, cx, cy, std::max(1, rr / 2), col);
            seg_label(cx, cy, rr, e.label);
        };
        for (auto& e : p->inputs)  draw_jack(e, t.accent);
        for (auto& e : p->outputs) draw_jack(e, t.sel);

        m_wHandle = SDL_Rect{ pr.x + pr.w - 4, pr.y, 8, pr.h };  // width drag handle
        ui::fill_rect(r, m_wHandle, t.hi);

        app.mono.draw(r, rect.x + 6, rect.y + rect.h - app.mono.ch() - 4,
                      "drag knobs/jacks to arrange - drag the right edge to resize width", t.dim);
    }

    bool on_mouse(ui::App& app, const ui::MouseEv& e) override {
        rackx::PanelSpec* p = panel();
        if (!e.pressed) { m_drag = Drag::None; m_de = nullptr; return true; }

        // continued drag (motion arrives as pressed=true too)
        if (m_drag == Drag::Width && p) {
            p->width = std::min(40.f * rackx::RACK_HP_WIDTH,
                       std::max(3.f * rackx::RACK_HP_WIDTH, (e.x - m_panelRect.x) / std::max(0.01f, m_scale)));
            app.request_redraw(); return true;
        }
        if (m_drag == Drag::Elem && m_de && p) {
            m_de->x = std::max(6.f, std::min(p->width  - 6.f, (e.x - m_panelRect.x) / std::max(0.01f, m_scale)));
            m_de->y = std::max(6.f, std::min(p->height - 6.f, (e.y - m_panelRect.y) / std::max(0.01f, m_scale)));
            app.request_redraw(); return true;
        }
        if (m_drag != Drag::None) return true;                 // drag in progress but nothing to move

        // fresh press: buttons, then width handle, then an element
        if (pe_in_rect(m_btn_dsp, e.x, e.y)) { if (on_edit_dsp && m_mod >= 0) on_edit_dsp(m_mod);
                                               app.request_redraw(); return true; }
        if (p && pe_in_rect(m_btn_wm, e.x, e.y)) { set_hp(*p, hp(*p) - 1); app.request_redraw(); return true; }
        if (p && pe_in_rect(m_btn_wp, e.x, e.y)) { set_hp(*p, hp(*p) + 1); app.request_redraw(); return true; }
        if (!p) return true;
        if (pe_in_rect(m_wHandle, e.x, e.y)) { m_drag = Drag::Width; return true; }
        if (pick(*p, e.x, e.y)) { m_drag = Drag::Elem; return true; }
        return true;
    }

private:
    enum class Drag { None, Elem, Width };
    rackx::RackEngine*   m_eng = nullptr;
    int                  m_mod = -1;
    Drag                 m_drag = Drag::None;
    rackx::PanelElement* m_de = nullptr;
    float                m_scale = 1.f;
    SDL_Rect             m_panelRect{0,0,0,0};
    SDL_Rect             m_btn_dsp{0,0,0,0}, m_btn_wm{0,0,0,0}, m_btn_wp{0,0,0,0}, m_wHandle{0,0,0,0};

    rackx::PanelSpec* panel() { return (m_eng && m_mod >= 0) ? m_eng->modulePanel(m_mod) : nullptr; }
    static int hp(const rackx::PanelSpec& p) { return (int)std::lround(p.width / rackx::RACK_HP_WIDTH); }
    void set_hp(rackx::PanelSpec& p, int h) { h = std::max(3, std::min(40, h)); p.width = h * rackx::RACK_HP_WIDTH; }

    void layout(ui::App& app, const rackx::PanelSpec& p) {
        const int th = app.mono.ch() + 8;
        const int top = rect.y + th + 10;
        const int availH = rect.h - th - 40, availW = rect.w - 40;
        m_scale = std::min(availW / std::max(1.f, p.width), availH / std::max(1.f, p.height));
        if (m_scale < 0.2f) m_scale = 0.2f;
        const int pw = (int)std::lround(p.width * m_scale), ph = (int)std::lround(p.height * m_scale);
        m_panelRect = SDL_Rect{ rect.x + (rect.w - pw) / 2, top, pw, ph };
    }
    void elem_screen(const rackx::PanelElement& e, int& cx, int& cy) const {
        cx = m_panelRect.x + (int)std::lround(e.x * m_scale);
        cy = m_panelRect.y + (int)std::lround(e.y * m_scale);
    }
    bool pick(rackx::PanelSpec& p, int mx, int my) {
        auto hit = [&](std::vector<rackx::PanelElement>& v) -> bool {
            for (int i = (int)v.size() - 1; i >= 0; --i) {
                int cx, cy; elem_screen(v[i], cx, cy);
                const int rr = std::max(6, (int)std::lround(v[i].radius * m_scale)) + 3;
                if (std::abs(mx - cx) <= rr && std::abs(my - cy) <= rr) { m_de = &v[i]; return true; }
            }
            return false;
        };
        return hit(p.outputs) || hit(p.inputs) || hit(p.params);
    }
    static void ring(SDL_Renderer* r, int cx, int cy, int rad, ui::Color col) {
        if (rad < 1) return; ui::set_color(r, col);
        const int seg = std::max(10, rad * 2); int px = 0, py = 0; bool have = false;
        for (int s = 0; s <= seg; ++s) {
            const double a = 2.0 * 3.14159265 * s / seg;
            const int x = cx + (int)std::lround(std::cos(a) * rad), y = cy + (int)std::lround(std::sin(a) * rad);
            if (have) SDL_RenderDrawLine(r, px, py, x, y);
            px = x; py = y; have = true;
        }
    }
};

} // namespace paneled

#endif
