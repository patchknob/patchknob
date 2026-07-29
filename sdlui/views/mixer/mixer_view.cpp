//----------------------------------------------------------------------------
//  sdlui/views/mixer/mixer_view.cpp -- see mixer_view.h.
//----------------------------------------------------------------------------
#include "mixer_view.h"

#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"
#include "engine/graph/vu_meter.h"
#include "engine/plugin_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ui;
using PatchKnob::engine::MixerGraph;
using PatchKnob::engine::Track;

namespace mixer {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float gain_to_value(float g) { return clampf(g / kMaxGain, 0.0f, 1.0f); }
static inline float value_to_gain(float v) { return v * kMaxGain; }

// Clip a string to at most n glyph cells, adding no ellipsis (cheap grid font).
static std::string clip_text(const std::string& s, int cells) {
    if (cells <= 0) return std::string();
    if ((int)s.size() <= cells) return s;
    return s.substr(0, (size_t)cells);
}

static std::string track_name(Track* t, int idx) {
    if (t && t->instrument())
        return t->instrument()->descriptor().name;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Track %d", idx + 1);
    return buf;
}

// ===========================================================================
// PanSlider
// ===========================================================================
void PanSlider::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.keybg);
    frame_rect(app.ren, rect, t.dim);
    // centre detent
    int cx = rect.x + rect.w / 2;
    vline(app.ren, cx, rect.y + 2, rect.y + rect.h - 3, t.dim);
    // knob
    float norm = value * 0.5f + 0.5f;          // -1..1 -> 0..1
    int kx = rect.x + 2 + int(clampf(norm, 0.f, 1.f) * (rect.w - 6));
    SDL_Rect knob { kx, rect.y + 2, 4, rect.h - 4 };
    fill_rect(app.ren, knob, t.accent);
    // L / R hint text is omitted to keep the strip terse; the knob position and
    // centre detent read the pan at a glance.
}
void PanSlider::apply(App& app, int px) {
    float n = float(px - (rect.x + 2)) / float(rect.w - 6 > 0 ? rect.w - 6 : 1);
    value = clampf(n, 0.f, 1.f) * 2.0f - 1.0f;      // 0..1 -> -1..1
    if (std::fabs(value) < 0.06f) value = 0.0f;      // snap to centre
    if (changed) changed(value);
    app.request_redraw();
}
bool PanSlider::on_mouse(App& app, const MouseEv& e) {
    if (e.pressed) apply(app, e.x);
    return true;
}

// ===========================================================================
// MixerView
// ===========================================================================
MixerView::MixerView(MixerGraph* graph) : graph_(graph) {
    build();
}

void MixerView::set_graph(MixerGraph* graph) {
    graph_ = graph;
    build();
}

void MixerView::build() {
    strips_.clear();
    children_.clear();
    capture_ = nullptr;

    const int n = graph_ ? graph_->trackCount() : 0;
    strips_.resize((size_t)n);

    for (int i = 0; i < n; ++i) {
        Track* t = graph_->track(i);
        Strip& s = strips_[(size_t)i];

        s.fader.reset(new Fader());
        s.fader->value = gain_to_value(t ? t->gain() : 1.0f);
        s.fader->changed = [t](float v) { if (t) t->setGain(value_to_gain(v)); };

        s.pan.reset(new PanSlider());
        s.pan->value = t ? t->pan() : 0.0f;
        s.pan->changed = [t](float p) { if (t) t->setPan(p); };

        s.mute.reset(new Button());
        s.mute->text = "M";
        s.mute->toggle = true;
        s.mute->on = t ? t->mute() : false;
        {
            Button* b = s.mute.get();
            s.mute->clicked = [t, b] { if (t) t->setMute(b->on); };
        }

        s.solo.reset(new Button());
        s.solo->text = "S";
        s.solo->toggle = true;
        s.solo->on = t ? t->solo() : false;
        {
            Button* b = s.solo.get();
            s.solo->clicked = [t, b] { if (t) t->setSolo(b->on); };
        }

        children_.push_back(s.fader.get());
        children_.push_back(s.pan.get());
        children_.push_back(s.mute.get());
        children_.push_back(s.solo.get());
    }

    // master fader
    masterFader_.reset(new Fader());
    masterFader_->value = gain_to_value(graph_ ? graph_->masterGain() : 1.0f);
    {
        MixerGraph* g = graph_;
        masterFader_->changed = [g](float v) { if (g) g->setMasterGain(value_to_gain(v)); };
    }
    children_.push_back(masterFader_.get());
}

// ---- geometry --------------------------------------------------------------
// A strip is a vertical stack:  header(name+instr) / FX list / [VU + Fader tall]
//                               / pan / Mute|Solo / gain readout.
void MixerView::layout(App& app) {
    (void)app;
    const int pad = 6;
    const int n   = (int)strips_.size();

    auto strip_area = [&](int col) -> SDL_Rect {
        int x = rect.x + pad + col * (strip_w_ + gap_);
        return SDL_Rect{ x, rect.y + pad, strip_w_, rect.h - 2 * pad };
    };

    // Sub-rects that need widget positions live inside draw_strip via the same
    // arithmetic; here we only need to place the interactive children so mouse
    // hit-testing matches what's drawn. Keep the two in lock-step.
    for (int i = 0; i < n; ++i) {
        SDL_Rect a = strip_area(i);
        Strip& s = strips_[(size_t)i];

        const int ip = 6;                       // inner pad
        int ix = a.x + ip, iw = a.w - 2 * ip;
        int y  = a.y + 4;
        y += app.font.ch() + 2;                 // name
        y += app.mono.ch() + 4;                 // instrument
        y += 2;                                 // fx title baseline
        y += app.mono.ch() + 2;                 // "FX" header
        y += 3 * (app.mono.ch() + 1) + 4;       // up to 3 fx rows

        // gain readout + buttons + pan sit at the bottom, meter fills the gap.
        int bottom = a.y + a.h - 4;
        int readoutH = app.mono.ch() + 2;
        int btnH = 22;
        int panH = 16;

        int readoutY = bottom - readoutH;
        int btnY     = readoutY - 4 - btnH;
        int panY     = btnY - 4 - panH;
        int meterTop = y;
        int meterBot = panY - 4;
        int meterH   = std::max(20, meterBot - meterTop);

        int vuW    = 18;
        int faderW = std::max(28, iw - vuW - 6);

        s.fader->rect = { ix + vuW + 6, meterTop, faderW, meterH };
        s.pan->rect   = { ix, panY, iw, panH };
        int halfW = (iw - 4) / 2;
        s.mute->rect  = { ix, btnY, halfW, btnH };
        s.solo->rect  = { ix + halfW + 4, btnY, iw - halfW - 4, btnH };
    }

    // master strip (last column)
    if (masterFader_) {
        SDL_Rect a = strip_area(n);
        const int ip = 6;
        int ix = a.x + ip, iw = a.w - 2 * ip;
        int y = a.y + 4;
        y += app.font.ch() + 2;                 // "MASTER"
        y += app.mono.ch() + 6;                 // subtitle gap
        int bottom = a.y + a.h - 4;
        int readoutH = app.mono.ch() + 2;
        int meterTop = y;
        int meterBot = bottom - readoutH - 4;
        int meterH = std::max(20, meterBot - meterTop);
        int vuW = 24;                            // stereo master meter a touch wider
        int faderW = std::max(28, iw - vuW - 8);
        masterFader_->rect = { ix + vuW + 8, meterTop, faderW, meterH };
    }
}

// ---- VU meter --------------------------------------------------------------
void MixerView::draw_vu(App& app, const SDL_Rect& bar, float level, float& hold) {
    const Theme& t = theme();
    fill_rect(app.ren, bar, t.keybg);
    frame_rect(app.ren, bar, t.dim);

    float lc = clampf(level, 0.f, 1.f);
    int inner = bar.h - 2;
    int filled = int(lc * inner);
    if (filled > 0) {
        SDL_Rect f { bar.x + 1, bar.y + bar.h - 1 - filled, bar.w - 2, filled };
        fill_rect(app.ren, f, t.accent);
    }
    // brighter cap near the top of the fill for a little life
    if (filled > 2) {
        SDL_Rect cap { bar.x + 1, bar.y + bar.h - 1 - filled, bar.w - 2, 2 };
        fill_rect(app.ren, cap, t.hi);
    }

    // peak-hold marker
    if (level > hold) hold = level; else hold *= 0.93f;
    float hc = clampf(hold, 0.f, 1.f);
    if (hc > 0.01f) {
        int hy = bar.y + bar.h - 1 - int(hc * inner);
        hline(app.ren, bar.x + 1, bar.x + bar.w - 2, hy, t.text);
    }
    // clip indicator: fill the very top when the level is at/over full scale
    if (level >= 0.999f) {
        SDL_Rect clip { bar.x + 1, bar.y + 1, bar.w - 2, 3 };
        fill_rect(app.ren, clip, t.active);
    }
}

// ---- one channel strip -----------------------------------------------------
void MixerView::draw_strip(App& app, int i, const SDL_Rect& a) {
    const Theme& t = theme();
    Track* tk = graph_ ? graph_->track(i) : nullptr;
    Strip& s = strips_[(size_t)i];

    fill_rect(app.ren, a, t.panel);
    frame_rect(app.ren, a, t.dim);

    const int ip = 6;
    int ix = a.x + ip, iw = a.w - 2 * ip;
    int cells = iw / (app.mono.cw() > 0 ? app.mono.cw() : 8);
    int y = a.y + 4;

    // name (channel number + instrument/track name)
    {
        char num[8]; std::snprintf(num, sizeof(num), "%d", i + 1);
        app.font.draw(app.ren, ix, y, num, t.accent);
        std::string nm = track_name(tk, i);
        int nx = ix + app.font.cw() * 2;
        int ncells = (a.x + a.w - ip - nx) / (app.font.cw() > 0 ? app.font.cw() : 8);
        app.font.draw(app.ren, nx, y, clip_text(nm, ncells), t.text);
        y += app.font.ch() + 2;
    }
    // instrument descriptor (dim)
    {
        std::string instr = (tk && tk->instrument())
            ? tk->instrument()->descriptor().name : std::string("<no inst>");
        app.mono.draw(app.ren, ix, y, clip_text(instr, cells), t.dim);
        y += app.mono.ch() + 4;
    }

    hline(app.ren, ix, ix + iw, y, t.dim);
    y += 2;

    // FX list
    {
        app.mono.draw(app.ren, ix, y, "FX", t.dim);
        y += app.mono.ch() + 2;
        int nfx = tk ? tk->fxCount() : 0;
        for (int k = 0; k < 3; ++k) {
            if (k < nfx && tk->fxAt(k)) {
                std::string fn = tk->fxAt(k)->descriptor().name;
                app.mono.draw(app.ren, ix + 2, y, clip_text(fn, cells - 1), t.text);
            } else if (k == nfx) {
                app.mono.draw(app.ren, ix + 2, y, "--", t.dim);
            }
            y += app.mono.ch() + 1;
        }
        y += 4;
    }

    // meter: stereo VU on the left, fader on the right (rects from layout())
    SDL_Rect fr = s.fader->rect;
    int vuX = ix, vuTop = fr.y, vuH = fr.h;
    int barW = 8;
    SDL_Rect barL { vuX, vuTop, barW, vuH };
    SDL_Rect barR { vuX + barW + 2, vuTop, barW, vuH };
    float pkL = tk ? tk->vuLeft().rms()  : 0.0f;
    float pkR = tk ? tk->vuRight().rms() : 0.0f;
    draw_vu(app, barL, pkL, s.holdL);
    draw_vu(app, barR, pkR, s.holdR);

    // Sync every interactive widget to the live model before drawing so the
    // strip reflects external changes (project load / automation), not just its
    // own edits.  Skip the fader while it is being dragged so the drag wins.
    if (tk) {
        s.mute->on = tk->mute();
        s.solo->on = tk->solo();
        if (!s.fader->m_drag) s.fader->value = gain_to_value(tk->gain());
        s.pan->value = tk->pan();
    }
    s.fader->draw(app);
    s.pan->draw(app);
    s.mute->draw(app);
    s.solo->draw(app);

    // gain readout at the very bottom
    {
        float g = tk ? tk->gain() : 1.0f;
        float db = g > 0.0001f ? 20.0f * std::log10(g) : -60.0f;
        char buf[24];
        if (db <= -59.9f) std::snprintf(buf, sizeof(buf), "-inf");
        else              std::snprintf(buf, sizeof(buf), "%+.1fdB", db);
        SDL_Rect rr { ix, a.y + a.h - 4 - (app.mono.ch() + 2), iw, app.mono.ch() + 2 };
        app.mono.draw_centered(app.ren, rr, buf, t.text);
    }
}

// ---- master strip ----------------------------------------------------------
void MixerView::draw_master(App& app, const SDL_Rect& a) {
    const Theme& t = theme();

    // a slightly stronger frame so the master reads as separate
    fill_rect(app.ren, a, t.panel);
    frame_rect(app.ren, a, t.accent);

    const int ip = 6;
    int ix = a.x + ip, iw = a.w - 2 * ip;
    int y = a.y + 4;

    app.font.draw(app.ren, ix, y, "MASTER", t.accent);
    y += app.font.ch() + 2;
    app.mono.draw(app.ren, ix, y, "stereo bus", t.dim);

    SDL_Rect fr = masterFader_->rect;
    int vuTop = fr.y, vuH = fr.h;
    int barW = 10;
    SDL_Rect barL { ix, vuTop, barW, vuH };
    SDL_Rect barR { ix + barW + 3, vuTop, barW, vuH };
    float pkL = graph_ ? graph_->masterVuLeft().rms()  : 0.0f;
    float pkR = graph_ ? graph_->masterVuRight().rms() : 0.0f;
    draw_vu(app, barL, pkL, masterHoldL_);
    draw_vu(app, barR, pkR, masterHoldR_);

    if (graph_ && !masterFader_->m_drag)
        masterFader_->value = gain_to_value(graph_->masterGain());
    masterFader_->draw(app);

    // master gain readout
    {
        float g = graph_ ? graph_->masterGain() : 1.0f;
        float db = g > 0.0001f ? 20.0f * std::log10(g) : -60.0f;
        char buf[24];
        if (db <= -59.9f) std::snprintf(buf, sizeof(buf), "-inf");
        else              std::snprintf(buf, sizeof(buf), "%+.1fdB", db);
        SDL_Rect rr { ix, a.y + a.h - 4 - (app.mono.ch() + 2), iw, app.mono.ch() + 2 };
        app.mono.draw_centered(app.ren, rr, buf, t.accent);
    }
}

// ---- draw ------------------------------------------------------------------
void MixerView::draw(App& app) {
    if (!visible) return;
    layout(app);                                // keep sub-rects in sync

    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);

    const int pad = 6;
    const int n = (int)strips_.size();
    auto strip_area = [&](int col) -> SDL_Rect {
        int x = rect.x + pad + col * (strip_w_ + gap_);
        return SDL_Rect{ x, rect.y + pad, strip_w_, rect.h - 2 * pad };
    };

    for (int i = 0; i < n; ++i) {
        SDL_Rect a = strip_area(i);
        if (a.x >= rect.x + rect.w) break;      // off the right edge -> stop
        draw_strip(app, i, a);
    }
    if (masterFader_)
        draw_master(app, strip_area(n));
}

// ---- input dispatch --------------------------------------------------------
bool MixerView::on_mouse(App& app, const MouseEv& e) {
    // rects were set on the last draw(); dispatch to the captured / hit child.
    if (e.pressed) {
        if (!capture_) {
            for (auto* c : children_)
                if (c->visible && c->hit(e.x, e.y)) { capture_ = c; break; }
        }
        if (capture_) return capture_->on_mouse(app, e);
        return true;                            // swallow clicks inside the mixer
    }
    // release
    bool handled = false;
    if (capture_) { handled = capture_->on_mouse(app, e); capture_ = nullptr; }
    return handled;
}

bool MixerView::on_wheel(App& app, int dx, int dy) {
    (void)dx;
    // Nudge the fader under the cursor (App::on_wheel carries no coordinates, so
    // ask SDL for the pointer position).
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const float step = 0.03f * float(dy);
    for (auto* c : children_) {
        Fader* f = dynamic_cast<Fader*>(c);
        if (f && f->hit(mx, my)) {
            f->value = clampf(f->value + step, 0.f, 1.f);
            if (f->changed) f->changed(f->value);
            app.request_redraw();
            return true;
        }
    }
    return false;
}

} // namespace mixer
