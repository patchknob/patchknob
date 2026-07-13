//----------------------------------------------------------------------------
//  sdlui/gui.cpp -- implementation of the lean SDL2 grayscale/green toolkit.
//----------------------------------------------------------------------------
#include "gui.h"
#include <SDL_ttf.h>
#include <cstdio>

namespace ui {

// ---- themes ----------------------------------------------------------------
//  LIGHT  = white background, black/greyscale.
//  MIDNIGHT = pure-black background, high-contrast phosphor green.
static const Theme k_light = {
    rgb(0xFFFFFF), rgb(0xECECEC), rgb(0x404040), rgb(0x000000), rgb(0x000000),
    rgb(0x101010), rgb(0x9A9A9A), rgb(0x000000), rgb(0xF0F0F0), rgb(0x383838),
    rgb(0x000000), rgb(0xF7F7F7), rgb(0xF0F0F0)
};
static const Theme k_midnight = {
    rgb(0x000000), rgb(0x041405), rgb(0x3BFF74), rgb(0x9FFFC0), rgb(0x00FF5F),
    rgb(0x48FF80), rgb(0x1C8F42), rgb(0x48FF80), rgb(0x000000), rgb(0x34E066),
    rgb(0xAFFFD0), rgb(0x00140A), rgb(0x010A03)
};
static Mode  g_mode = Mode::Light;
static Theme g_theme = k_light;

void set_mode(Mode m) { g_mode = m; g_theme = (m==Mode::Midnight) ? k_midnight : k_light; }
Mode mode() { return g_mode; }
const Theme& theme() { return g_theme; }

// ---- draw helpers ----------------------------------------------------------
void set_color(SDL_Renderer* r, Color c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }
void fill_rect(SDL_Renderer* r, const SDL_Rect& q, Color c) { set_color(r,c); SDL_RenderFillRect(r,&q); }
void frame_rect(SDL_Renderer* r, const SDL_Rect& q, Color c) { set_color(r,c); SDL_RenderDrawRect(r,&q); }
void hline(SDL_Renderer* r, int x0, int x1, int y, Color c) { set_color(r,c); SDL_RenderDrawLine(r,x0,y,x1,y); }
void vline(SDL_Renderer* r, int x, int y0, int y1, Color c) { set_color(r,c); SDL_RenderDrawLine(r,x,y0,x,y1); }

// ---- font atlas ------------------------------------------------------------
bool Font::load(SDL_Renderer* r, int pt)
{
    static const char* candidates[] = {
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf", nullptr
    };
    TTF_Font* f = nullptr;
    for (int i = 0; candidates[i]; ++i) { f = TTF_OpenFont(candidates[i], pt); if (f) break; }
    if (!f) { fprintf(stderr, "[sdlui] no monospace TTF found\n"); return false; }

    int adv = 0; TTF_GlyphMetrics(f, 'M', nullptr,nullptr,nullptr,nullptr, &adv);
    m_cw = adv > 0 ? adv : TTF_FontHeight(f)/2;
    m_ch = TTF_FontHeight(f);
    int n = m_last - m_first + 1;

    SDL_Surface* atlas = SDL_CreateRGBSurfaceWithFormat(0, m_cw*n, m_ch, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_SetSurfaceBlendMode(atlas, SDL_BLENDMODE_NONE);
    SDL_Color white { 255,255,255,255 };
    for (int c = m_first; c <= m_last; ++c) {
        char s[2] = { (char)c, 0 };
        SDL_Surface* g = TTF_RenderText_Blended(f, s, white);
        if (g) {
            SDL_Rect dst { (c - m_first)*m_cw, 0, g->w, g->h };
            SDL_SetSurfaceBlendMode(g, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(g, nullptr, atlas, &dst);
            SDL_FreeSurface(g);
        }
    }
    m_atlas = SDL_CreateTextureFromSurface(r, atlas);
    SDL_SetTextureBlendMode(m_atlas, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(atlas);
    TTF_CloseFont(f);
    return m_atlas != nullptr;
}

void Font::draw(SDL_Renderer* r, int x, int y, const std::string& s, Color c) const
{
    if (!m_atlas) return;
    SDL_SetTextureColorMod(m_atlas, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(m_atlas, c.a);
    for (size_t i = 0; i < s.size(); ++i) {
        int ch = (unsigned char)s[i];
        if (ch < m_first || ch > m_last) continue;
        SDL_Rect src { (ch - m_first)*m_cw, 0, m_cw, m_ch };
        SDL_Rect dst { x + int(i)*m_cw, y, m_cw, m_ch };
        SDL_RenderCopy(r, m_atlas, &src, &dst);
    }
}

void Font::draw_centered(SDL_Renderer* r, const SDL_Rect& box, const std::string& s, Color c) const
{
    int tw = text_w(s);
    draw(r, box.x + (box.w - tw)/2, box.y + (box.h - m_ch)/2, s, c);
}

// ---- widgets ---------------------------------------------------------------
void Panel::draw(App& app) {
    if (!visible) return;
    fill_rect(app.ren, rect, bg ? *bg : theme().panel);
    for (auto* c : children) if (c->visible) c->draw(app);
}
bool Panel::on_mouse(App& app, const MouseEv& e) {
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        if ((*it)->visible && (*it)->hit(e.x, e.y) && (*it)->on_mouse(app, e)) return true;
    return false;
}
bool Panel::on_wheel(App& app, int dx, int dy) {
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        if ((*it)->visible && (*it)->on_wheel(app, dx, dy)) return true;
    return false;
}

void Label::draw(App& app) {
    app.font.draw(app.ren, rect.x, rect.y + (rect.h-app.font.ch())/2, text, theme().text);
}

void Button::draw(App& app) {
    const Theme& t = theme();
    bool active = (toggle && on) || down;
    fill_rect(app.ren, rect, active ? t.accent : t.panel);
    frame_rect(app.ren, rect, t.dim);
    app.font.draw_centered(app.ren, rect, text, active ? t.bg : t.text);
}
bool Button::on_mouse(App& app, const MouseEv& e) {
    if (e.pressed) { down = true; app.request_redraw(); return true; }
    // release
    down = false;
    if (hit(e.x, e.y)) { if (toggle) on = !on; if (clicked) clicked(); }
    app.request_redraw();
    return true;
}

void Fader::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.keybg);
    frame_rect(app.ren, rect, t.dim);
    int track = rect.h - 8;
    int knobY = rect.y + 4 + int((1.0f - value) * track);
    // filled portion below the knob
    SDL_Rect fillq { rect.x+3, knobY, rect.w-6, rect.y+rect.h-4-knobY };
    fill_rect(app.ren, fillq, t.accent);
    SDL_Rect knob { rect.x+1, knobY-2, rect.w-2, 4 };
    fill_rect(app.ren, knob, t.hi);
}
bool Fader::on_mouse(App& app, const MouseEv& e) {
    if (e.pressed) m_drag = true;
    if (!e.pressed) m_drag = false;
    if (m_drag || e.pressed) {
        int track = rect.h - 8;
        float v = 1.0f - float(e.y - (rect.y+4)) / (track>0?track:1);
        value = v < 0 ? 0 : (v > 1 ? 1 : v);
        if (changed) changed(value);
        app.request_redraw();
    }
    return true;
}

// ---- app shell -------------------------------------------------------------
bool App::init(const char* title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr,"SDL_Init: %s\n", SDL_GetError()); return false; }
    if (TTF_Init() != 0) { fprintf(stderr,"TTF_Init: %s\n", TTF_GetError()); return false; }
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w, h, SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr,"CreateWindow: %s\n", SDL_GetError()); return false; }
    ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(window, -1, 0);   // software fallback (e.g. headless)
    if (!ren) { fprintf(stderr,"CreateRenderer: %s\n", SDL_GetError()); return false; }
    if (!font.load(ren, 15)) return false;
    mono.load(ren, 13);
    return true;
}

void App::run(std::function<void(App&)> draw_extra) {
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    w = ev.window.data1; h = ev.window.data2; dirty = true;
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) dirty = true;
                break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                MouseEv m { ev.button.x, ev.button.y, ev.button.button,
                            ev.type==SDL_MOUSEBUTTONDOWN };
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && ((*it)->hit(m.x,m.y) || !m.pressed) && (*it)->on_mouse(*this, m)) break;
                break; }
            case SDL_MOUSEMOTION:
                if (ev.motion.state & SDL_BUTTON_LMASK) {
                    MouseEv m { ev.motion.x, ev.motion.y, SDL_BUTTON_LEFT, true };
                    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->on_mouse(*this, m)) break;
                }
                break;
            case SDL_MOUSEWHEEL:
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->on_wheel(*this, ev.wheel.x, ev.wheel.y)) break;
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = false;
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->on_key(*this, ev.key.keysym.sym)) break;
                break;
            }
        }
        // Retained + dirty: render on change, or continuously while animating
        // (playback playhead).  Idle otherwise so the CPU stays free for audio.
        if (dirty || animating) {
            if (on_layout) on_layout(*this);
            fill_rect(ren, SDL_Rect{0,0,w,h}, theme().bg);
            for (auto* rt : roots) if (rt->visible) rt->draw(*this);
            if (draw_extra) draw_extra(*this);
            SDL_RenderPresent(ren);
            dirty = false;
        } else {
            SDL_Delay(8);   // idle: don't spin
        }
    }
}

void App::shutdown() {
    if (ren) SDL_DestroyRenderer(ren);
    if (window) SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
}

} // namespace ui
