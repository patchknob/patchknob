//----------------------------------------------------------------------------
//  sdlui/views/plugin_param/plugin_param_view.cpp
//----------------------------------------------------------------------------
#include "plugin_param_view.h"
#include "engine/plugin_api.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace ui;

namespace paramui {

bool PluginParamView::is_nine50() const { return m_inst && m_inst->descriptor().path=="builtin://nine50-compressor"; }
bool PluginParamView::is_limiter() const { return m_inst && m_inst->descriptor().path=="builtin://classic-master-limiter"; }
SDL_Rect PluginParamView::native_control_rect(int i) const {
    if(is_limiter()) return SDL_Rect{rect.x+28,rect.y+74,112,112};
    const int col=i%4,row=i/4,w=rect.w/4;
    return SDL_Rect{rect.x+col*w+8,rect.y+48+row*150,w-16,126};
}

static void dial(App& app,const SDL_Rect& q,float v,const char* name,bool active){
    const Theme&t=theme(); const int cx=q.x+q.w/2,cy=q.y+q.h/2-8,rad=std::max(12,std::min(q.w,q.h)/3);
    set_color(app.ren,t.keybg); for(int y=-rad;y<=rad;++y){int x=(int)std::sqrt((double)(rad*rad-y*y));SDL_RenderDrawLine(app.ren,cx-x,cy+y,cx+x,cy+y);}
    const double a=(-135.+270.*v)*3.141592653589793/180.; const int ex=cx+(int)(std::sin(a)*(rad-4)),ey=cy-(int)(std::cos(a)*(rad-4));
    set_color(app.ren,active?t.hi:t.accent); SDL_RenderDrawLine(app.ren,cx,cy,ex,ey); SDL_Rect cap{cx-2,cy-2,5,5};fill_rect(app.ren,cap,t.text);
    app.mono.draw_centered(app.ren,SDL_Rect{q.x,q.y+q.h-app.mono.ch()-2,q.w,app.mono.ch()+2},name,t.text);
}

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

// One line per direction: the ordered BUS layout, not just a channel total.
// "Main 2ch" for the ordinary effect; "Kick 2ch . Snare 2ch . ..." for a
// multi-out instrument, which is the whole point -- the panel is where a user
// finds out that a plugin has outputs beyond its main pair.
static std::string bus_summary(
        const std::vector<PatchKnob::engine::PluginBusInfo>& buses, int flatTotal)
{
    if (buses.empty())
        return flatTotal > 0 ? (std::to_string(flatTotal) + "ch") : std::string("none");
    std::string s;
    for (size_t i = 0; i < buses.size(); ++i) {
        if (i) s += "  ";
        if (!buses[i].name.empty()) { s += buses[i].name; s += ' '; }
        s += std::to_string(buses[i].channelCount);
        s += "ch";
        if (buses[i].isAux) s += " (aux)";
    }
    return s;
}

int PluginParamView::header_h(App& app) const {
    if (!m_inst || is_nine50() || is_limiter()) return 0;
    const PatchKnob::engine::PluginDescriptor& d = m_inst->descriptor();
    int lines = 0;
    if (!d.audioInBuses.empty()  || d.numAudioIn  > 0) ++lines;
    if (!d.audioOutBuses.empty() || d.numAudioOut > 0) ++lines;
    return lines ? lines * (app.mono.ch() + 2) + 6 : 0;
}

void PluginParamView::draw_header(App& app) const {
    const int hh = header_h(app);
    if (hh <= 0) return;
    const Theme& t = theme();
    const PatchKnob::engine::PluginDescriptor& d = m_inst->descriptor();
    const int lh = app.mono.ch() + 2;
    int y = rect.y + 3;
    const int labelW = app.mono.text_w("OUT ");
    auto line = [&](const char* tag,
                    const std::vector<PatchKnob::engine::PluginBusInfo>& buses,
                    int flat) {
        app.mono.draw(app.ren, rect.x + 8, y, tag, t.dim);
        app.mono.draw(app.ren, rect.x + 8 + labelW, y,
                      fit_text(app.mono, bus_summary(buses, flat),
                               rect.w - 16 - labelW), t.accent);
        y += lh;
    };
    if (!d.audioInBuses.empty()  || d.numAudioIn  > 0) line("IN ", d.audioInBuses,  d.numAudioIn);
    if (!d.audioOutBuses.empty() || d.numAudioOut > 0) line("OUT", d.audioOutBuses, d.numAudioOut);
    hline(app.ren, rect.x + 4, rect.x + rect.w - 4, rect.y + hh - 2, t.dim);
}

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

    if(is_nine50()) {
        fill_rect(app.ren,rect,Color{25,30,33,255}); frame_rect(app.ren,rect,t.dim);
        app.font.draw_centered(app.ren,SDL_Rect{rect.x,rect.y+8,rect.w,28},"NINE50  COMPRESSOR",Color{220,225,216,255});
        for(int i=0;i<8;++i){ auto pi=m_inst->paramInfo(i); dial(app,native_control_rect(i),m_inst->getParamNormalized(pi.id),pi.name.c_str(),i==m_drag); }
        // Source panel's vertical gain-reduction display.
        float gr=0; const float th=m_inst->getParamNormalized(0); (void)th;
        SDL_Rect meter{rect.x+rect.w-14,rect.y+42,7,rect.h-54}; fill_rect(app.ren,meter,Color{8,12,13,255});
        SDL_Rect lit{meter.x+1,meter.y+1,meter.w-2,(int)((meter.h-2)*gr)};fill_rect(app.ren,lit,t.hi); return;
    }
    if(is_limiter()) {
        fill_rect(app.ren,rect,Color{78,77,49,255}); frame_rect(app.ren,rect,Color{28,29,22,255});
        app.font.draw(app.ren,rect.x+20,rect.y+18,"CLASSIC MASTER LIMITER",Color{225,220,177,255});
        dial(app,native_control_rect(0),m_inst->getParamNormalized(0),"THRESHOLD",m_drag==0);
        app.mono.draw(app.ren,rect.x+168,rect.y+62,"COMPRESSION",Color{225,220,177,255});
        for(int c=0;c<2;++c){ float v=m_inst->getParamNormalized(1+c); int y=rect.y+92+c*52;
            app.mono.draw(app.ren,rect.x+168,y,c?"RIGHT":"LEFT",Color{225,220,177,255});
            for(int k=0;k<11;++k){ SDL_Rect led{rect.x+224+k*17,y,12,18}; fill_rect(app.ren,led,k<=(int)(v*10)?(k>7?t.active:t.hi):Color{35,36,27,255}); }
        }
        app.font.draw(app.ren,rect.x+170,rect.y+205,"RE01",Color{215,210,165,255}); return;
    }
    draw_header(app);
    const int hh = header_h(app);
    const int rh = row_h(app);
    const int np = m_inst->paramCount();
    if (np <= 0) app.font.draw(app.ren, rect.x+8, rect.y+hh+8, "(plugin exposes no parameters)", t.dim);

    int y = rect.y + 4 + hh - m_scroll;
    for (int i = 0; i < np; ++i) {
        if (y + rh > rect.y + hh && y < rect.y + rect.h) {     // clip below header
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
    if(is_nine50()||is_limiter()) {
        if(m_drag>=0){ const SDL_Rect q=native_control_rect(m_drag); float v=1.f-(float)(e.y-q.y)/(float)std::max(1,q.h); m_inst->setParamNormalized(m_inst->paramInfo(m_drag).id,std::max(0.f,std::min(1.f,v)));app.request_redraw();return true; }
        const int n=is_limiter()?1:8; for(int i=0;i<n;++i){const SDL_Rect q=native_control_rect(i);if(e.x>=q.x&&e.x<q.x+q.w&&e.y>=q.y&&e.y<q.y+q.h){m_drag=i;app.request_redraw();return true;}}
        return true;
    }
    const int rh = row_h(app);
    if (m_drag >= 0) { apply_from_x(m_drag, e.x); app.request_redraw(); return true; }
    const int np = m_inst->paramCount();
    const int hh = header_h(app);
    if (e.y < rect.y + hh) return true;          // the I/O header is not clickable
    int y = rect.y + 4 + hh - m_scroll;
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
    // The header does not scroll, so it shrinks the viewport rather than the
    // content: same arithmetic, one term added.
    int maxScroll = std::max(0, (m_inst ? m_inst->paramCount() : 0) * row_h(app)
                                + 8 + header_h(app) - rect.h);
    if (m_scroll > maxScroll) m_scroll = maxScroll;
    app.request_redraw();
    return true;
}

} // namespace paramui
