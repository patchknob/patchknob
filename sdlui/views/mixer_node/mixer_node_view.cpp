//----------------------------------------------------------------------------
//  sdlui/views/mixer_node/mixer_node_view.cpp
//----------------------------------------------------------------------------
#include "mixer_node_view.h"
#include "audio_app.h"

#include <cstdio>

using namespace ui;

namespace mixnode {

namespace {
struct Layout {
    SDL_Rect minus, plus;
    int stripsY, faderTop, faderH, muteY, muteH, sw;
};
Layout compute(const SDL_Rect& rect, App& app) {
    Layout L;
    int th = app.font.ch() + 8;
    L.minus = SDL_Rect{ rect.x + 6,  rect.y + 5, 22, th };
    L.plus  = SDL_Rect{ rect.x + 32, rect.y + 5, 22, th };
    L.stripsY  = rect.y + th + 12;
    int labelH = app.font.ch() + 4;
    L.muteY    = L.stripsY + labelH;
    L.muteH    = app.font.ch() + 4;
    L.faderTop = L.muteY + L.muteH + 6;
    L.faderH   = (rect.y + rect.h - 8) - L.faderTop;
    if (L.faderH < 20) L.faderH = 20;
    L.sw = MixerNodeView::STRIP_W;
    return L;
}
} // namespace

void MixerNodeView::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);
    if (m_node < 0) { app.font.draw(app.ren, rect.x+8, rect.y+8, "(no mixer)", t.dim);
                      frame_rect(app.ren, rect, t.dim); return; }
    const int nch = PatchKnob::app::audio_app_mixer_channels(m_node);
    Layout L = compute(rect, app);

    fill_rect(app.ren, L.minus, t.panel); frame_rect(app.ren, L.minus, t.dim); app.font.draw_centered(app.ren, L.minus, "-", t.text);
    fill_rect(app.ren, L.plus,  t.panel); frame_rect(app.ren, L.plus,  t.dim); app.font.draw_centered(app.ren, L.plus,  "+", t.text);
    { char lbl[24]; std::snprintf(lbl,sizeof(lbl),"%d ch", nch); app.font.draw(app.ren, L.plus.x+30, rect.y+9, lbl, t.text); }

    for (int ch = 0; ch < nch; ++ch) {
        int sx = rect.x + 8 + ch * L.sw;
        if (sx + L.sw > rect.x + rect.w) break;   // clip to the window
        char cl[8]; std::snprintf(cl,sizeof(cl),"%d", ch+1);
        app.font.draw(app.ren, sx + (L.sw-4-(int)std::string(cl).size()*app.font.cw())/2, L.stripsY, cl, t.text);
        // mute
        SDL_Rect mb{ sx+2, L.muteY, L.sw-8, L.muteH };
        bool mute = PatchKnob::app::audio_app_mixer_mute(m_node, ch);
        fill_rect(app.ren, mb, mute ? t.accent : t.panel); frame_rect(app.ren, mb, t.dim);
        app.font.draw_centered(app.ren, mb, "M", mute ? t.bg : t.dim);
        // VU
        SDL_Rect vu{ sx+2, L.faderTop, 6, L.faderH };
        fill_rect(app.ren, vu, t.keybg); frame_rect(app.ren, vu, t.dim);
        float v = PatchKnob::app::audio_app_mixer_vu(m_node, ch); if (v>1) v=1; if (v<0) v=0;
        SDL_Rect vf{ vu.x, vu.y + (int)((1.f-v)*L.faderH), vu.w, (int)(v*L.faderH) };
        fill_rect(app.ren, vf, t.active);
        // fader (gain 0..2, unity at mid)
        SDL_Rect fr{ sx+12, L.faderTop, L.sw-18, L.faderH };
        fill_rect(app.ren, fr, t.keybg); frame_rect(app.ren, fr, t.dim);
        hline(app.ren, fr.x, fr.x+fr.w, fr.y+L.faderH/2, t.dim);   // unity tick
        float g = PatchKnob::app::audio_app_mixer_gain(m_node, ch); float fv = g/2.f; if (fv>1) fv=1; if (fv<0) fv=0;
        int knobY = fr.y + (int)((1.f-fv)*L.faderH);
        SDL_Rect knob{ fr.x, knobY-2, fr.w, 4 };
        fill_rect(app.ren, knob, (ch==m_drag) ? t.hi : t.accent);
    }
    frame_rect(app.ren, rect, t.dim);
    app.request_redraw();   // keep the VU meters live while the mixer is open
}

bool MixerNodeView::on_mouse(App& app, const MouseEv& e) {
    if (m_node < 0) return true;
    if (!e.pressed) { m_drag = -1; return true; }
    const int nch = PatchKnob::app::audio_app_mixer_channels(m_node);
    Layout L = compute(rect, app);
    auto inr = [&](const SDL_Rect& r){ return e.x>=r.x && e.x<r.x+r.w && e.y>=r.y && e.y<r.y+r.h; };

    if (m_drag >= 0) {   // continue a fader drag
        float fv = 1.f - (float)(e.y - L.faderTop) / (L.faderH > 0 ? L.faderH : 1);
        if (fv<0) fv=0; if (fv>1) fv=1;
        PatchKnob::app::audio_app_mixer_set_gain(m_node, m_drag, fv*2.f);
        app.request_redraw(); return true;
    }
    if (inr(L.minus)) { PatchKnob::app::audio_app_mixer_set_channels(m_node, nch-1);
                        if (on_changed) on_changed(m_node); app.request_redraw(); return true; }
    if (inr(L.plus))  { PatchKnob::app::audio_app_mixer_set_channels(m_node, nch+1);
                        if (on_changed) on_changed(m_node); app.request_redraw(); return true; }
    for (int ch = 0; ch < nch; ++ch) {
        int sx = rect.x + 8 + ch * L.sw;
        SDL_Rect mb{ sx+2, L.muteY, L.sw-8, L.muteH };
        if (inr(mb)) { PatchKnob::app::audio_app_mixer_set_mute(m_node, ch, !PatchKnob::app::audio_app_mixer_mute(m_node,ch));
                       app.request_redraw(); return true; }
        SDL_Rect fr{ sx+12, L.faderTop, L.sw-18, L.faderH };
        if (e.x >= fr.x-2 && e.x <= fr.x+fr.w+2 && e.y >= L.faderTop && e.y <= L.faderTop+L.faderH) {
            m_drag = ch;
            float fv = 1.f - (float)(e.y - L.faderTop) / (L.faderH>0?L.faderH:1);
            if (fv<0) fv=0; if (fv>1) fv=1;
            PatchKnob::app::audio_app_mixer_set_gain(m_node, ch, fv*2.f);
            app.request_redraw(); return true;
        }
    }
    return true;
}

} // namespace mixnode
