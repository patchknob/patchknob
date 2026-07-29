//----------------------------------------------------------------------------
//  sdlui/gui.cpp -- implementation of the lean SDL2 grayscale/green toolkit.
//----------------------------------------------------------------------------
#include "gui.h"
#include <SDL_ttf.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

namespace ui {

namespace {
std::string ellipsize(const Font& font, std::string s, int maxw) {
    if (maxw <= 0) return "";
    if (font.text_w(s) <= maxw) return s;
    const std::string ell = "...";
    if (font.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && font.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!s.empty() && font.text_w(s + ell) > maxw) s.pop_back();
    return s.empty() ? ell : s + ell;
}

void pop_utf8_char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    s.erase(i);
}

int font_point_for_scale(int logicalPoint, float scale) {
    if (logicalPoint <= 0) return 0;
    if (scale <= 0.f) scale = 1.f;
    return std::max(4, (int)std::lround(float(logicalPoint) * scale));
}
}

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
Font::~Font()
{
    if (m_atlas) SDL_DestroyTexture(m_atlas);
    clear_zoom_atlases();
}

void Font::clear_zoom_atlases()
{
    for (ZoomAtlas& atlas : m_zoom_atlases)
        if (atlas.texture) SDL_DestroyTexture(atlas.texture);
    m_zoom_atlases.clear();
}

bool Font::build_atlas(SDL_Renderer* r, int logicalPoint, float deviceScale,
                       SDL_Texture*& texture, int& cw, int& ch, int& cwP, int& chP) const
{
    if (m_face.empty()) return false;
    if (deviceScale <= 0.f) deviceScale = 1.f;
    const int physicalPoint = (int)std::lround(logicalPoint * deviceScale);
    TTF_Font* font = TTF_OpenFont(m_face.c_str(), physicalPoint);
    if (!font) return false;

    int advance = 0;
    TTF_GlyphMetrics(font, 'M', nullptr, nullptr, nullptr, nullptr, &advance);
    cwP = advance > 0 ? advance : TTF_FontHeight(font) / 2;
    chP = TTF_FontHeight(font);
    cw = (int)std::lround(cwP / deviceScale);
    ch = (int)std::lround(chP / deviceScale);
    const int count = m_last - m_first + 1;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, cwP * count, chP, 32,
                                                            SDL_PIXELFORMAT_RGBA32);
    if (!surface) { TTF_CloseFont(font); return false; }
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    const SDL_Color white { 255, 255, 255, 255 };
    for (int code = m_first; code <= m_last; ++code) {
        char text[2] = { (char)code, 0 };
        SDL_Surface* glyph = TTF_RenderText_Blended(font, text, white);
        if (glyph) {
            SDL_Rect dst { (code - m_first) * cwP, 0, glyph->w, glyph->h };
            SDL_SetSurfaceBlendMode(glyph, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(glyph, nullptr, surface, &dst);
            SDL_FreeSurface(glyph);
        }
    }
    texture = SDL_CreateTextureFromSurface(r, surface);
    if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surface);
    TTF_CloseFont(font);
    return texture != nullptr;
}

bool Font::load(SDL_Renderer* r, int ptLogical, float scale)
{
    if (scale <= 0.f) scale = 1.0f;
    int pt = (int)(ptLogical * scale + 0.5f);
    static const char* candidates[] = {
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf", nullptr
    };
    TTF_Font* f = nullptr;
    m_face.clear();
    for (int i = 0; candidates[i]; ++i) {
        f = TTF_OpenFont(candidates[i], pt);
        if (f) { m_face = candidates[i]; break; }
    }
    if (!f) { fprintf(stderr, "[sdlui] no monospace TTF found\n"); return false; }

    if (m_atlas) { SDL_DestroyTexture(m_atlas); m_atlas = nullptr; }
    clear_zoom_atlases();
    m_logical_point = ptLogical;
    m_device_scale = scale;

    int adv = 0; TTF_GlyphMetrics(f, 'M', nullptr,nullptr,nullptr,nullptr, &adv);
    m_cwP = adv > 0 ? adv : TTF_FontHeight(f)/2;   // physical atlas cell
    m_chP = TTF_FontHeight(f);
    m_cw  = (int)(m_cwP / scale + 0.5f);           // logical cell (layout/dst)
    m_ch  = (int)(m_chP / scale + 0.5f);
    int n = m_last - m_first + 1;

    SDL_Surface* atlas = SDL_CreateRGBSurfaceWithFormat(0, m_cwP*n, m_chP, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_SetSurfaceBlendMode(atlas, SDL_BLENDMODE_NONE);
    SDL_Color white { 255,255,255,255 };
    for (int c = m_first; c <= m_last; ++c) {
        char s[2] = { (char)c, 0 };
        SDL_Surface* g = TTF_RenderText_Blended(f, s, white);
        if (g) {
            SDL_Rect dst { (c - m_first)*m_cwP, 0, g->w, g->h };
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

const Font::ZoomAtlas* Font::zoom_atlas(SDL_Renderer* r, float scale) const
{
    if (m_logical_point <= 0) return nullptr;
    const int point = font_point_for_scale(m_logical_point, scale);
    if (point == m_logical_point) return nullptr;
    for (const ZoomAtlas& atlas : m_zoom_atlases)
        if (atlas.point == point) return &atlas;

    ZoomAtlas atlas;
    atlas.point = point;
    if (!build_atlas(r, point, m_device_scale, atlas.texture, atlas.cw, atlas.ch,
                     atlas.cwP, atlas.chP)) return nullptr;
    if (m_zoom_atlases.size() >= 12) {
        SDL_DestroyTexture(m_zoom_atlases.front().texture);
        m_zoom_atlases.erase(m_zoom_atlases.begin());
    }
    m_zoom_atlases.push_back(atlas);
    return &m_zoom_atlases.back();
}

int Font::text_w_scaled(const std::string& s, float scale) const
{
    if (scale <= 0.f) scale = 1.f;
    if (m_logical_point > 0) {
        const int point = font_point_for_scale(m_logical_point, scale);
        return (int)s.size() *
               std::max(1, (int)std::lround(float(m_cw) * point / m_logical_point));
    }
    return (int)std::lround(text_w(s) * scale);
}

int Font::text_h_scaled(float scale) const
{
    if (scale <= 0.f) scale = 1.f;
    if (m_logical_point > 0) {
        const int point = font_point_for_scale(m_logical_point, scale);
        return std::max(1, (int)std::lround(float(m_ch) * point / m_logical_point));
    }
    return std::max(1, (int)std::lround(float(m_ch) * scale));
}

void Font::draw(SDL_Renderer* r, int x, int y, const std::string& s, Color c) const
{
    draw_scaled(r, x, y, s, c, 1.f);
}

void Font::draw_scaled(SDL_Renderer* r, int x, int y, const std::string& s, Color c,
                       float scale) const
{
    if (!m_atlas) return;
    if (scale <= 0.f) scale = 1.f;

    SDL_Texture* atlasTexture = m_atlas;
    int cellW = m_cw, cellH = m_ch, cellWP = m_cwP, cellHP = m_chP;
    if (const ZoomAtlas* atlas = zoom_atlas(r, scale)) {
        atlasTexture = atlas->texture;
        cellW = atlas->cw; cellH = atlas->ch;
        cellWP = atlas->cwP; cellHP = atlas->chP;
    }

    static SDL_Renderer* geometry_renderer = nullptr;
    static bool geometry_available = true;
    if (geometry_renderer != r) {
        geometry_renderer = r;
        geometry_available = true;
    }

    static thread_local std::vector<SDL_Vertex> vertices;
    vertices.clear();
    vertices.reserve(s.size() * 6);

    const float atlas_w = float(cellWP * (m_last - m_first + 1));
    const SDL_Color color { c.r, c.g, c.b, c.a };
    for (size_t i = 0; i < s.size(); ++i) {
        int ch = (unsigned char)s[i];
        if (ch < m_first || ch > m_last) continue;
        float x0 = float(x + int(i) * cellW);
        float x1 = x0 + float(cellW);
        float y0 = float(y);
        float y1 = y0 + float(cellH);
        float u0 = float((ch - m_first) * cellWP) / atlas_w;
        float u1 = float((ch - m_first + 1) * cellWP) / atlas_w;
        SDL_Vertex tl { SDL_FPoint { x0, y0 }, color, SDL_FPoint { u0, 0.0f } };
        SDL_Vertex tr { SDL_FPoint { x1, y0 }, color, SDL_FPoint { u1, 0.0f } };
        SDL_Vertex br { SDL_FPoint { x1, y1 }, color, SDL_FPoint { u1, 1.0f } };
        SDL_Vertex bl { SDL_FPoint { x0, y1 }, color, SDL_FPoint { u0, 1.0f } };
        vertices.push_back(tl);
        vertices.push_back(tr);
        vertices.push_back(br);
        vertices.push_back(tl);
        vertices.push_back(br);
        vertices.push_back(bl);
    }

    if (vertices.empty()) return;
    if (geometry_available &&
        SDL_RenderGeometry(r, atlasTexture, vertices.data(), (int)vertices.size(), nullptr, 0) == 0)
        return;

    geometry_available = false;
    SDL_SetTextureColorMod(atlasTexture, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(atlasTexture, c.a);
    for (size_t i = 0; i < s.size(); ++i) {
        int ch = (unsigned char)s[i];
        if (ch < m_first || ch > m_last) continue;
        SDL_Rect src { (ch - m_first)*cellWP, 0, cellWP, cellHP };
        SDL_Rect dst { x + int(i)*cellW, y, cellW, cellH };
        SDL_RenderCopy(r, atlasTexture, &src, &dst);
    }
}

void Font::draw_fitted(SDL_Renderer* r, const SDL_Rect& box, const std::string& s,
                       Color c, bool center, float maxScale, float minScale,
                       bool useEllipsis) const
{
    if (!m_atlas || box.w <= 0 || box.h <= 0) return;
    if (maxScale <= 0.f) maxScale = 1.f;
    if (minScale <= 0.f) minScale = 0.25f;
    if (minScale > maxScale) std::swap(minScale, maxScale);

    float scale = std::min(maxScale, 1.0f);
    const float heightScale = float(box.h) / float(std::max(1, m_ch));
    scale = std::min(scale, heightScale);
    if (!s.empty())
        scale = std::min(scale, float(box.w) / float(std::max(1, text_w(s))));
    scale = std::max(minScale, std::min(maxScale, scale));

    if (m_logical_point > 0) {
        int point = font_point_for_scale(m_logical_point, scale);
        while (point > 4) {
            float qscale = float(point) / float(m_logical_point);
            if ((s.empty() || text_w_scaled(s, qscale) <= box.w) &&
                text_h_scaled(qscale) <= box.h)
            {
                scale = qscale;
                break;
            }
            --point;
        }
        if (point <= 4)
            scale = float(point) / float(m_logical_point);
    }

    std::string shown = s;
    if (useEllipsis && text_w_scaled(shown, scale) > box.w)
    {
        const std::string ell = "...";
        if (text_w_scaled(ell, scale) <= box.w)
        {
            while (!shown.empty() && text_w_scaled(shown + ell, scale) > box.w)
                pop_utf8_char(shown);
            shown = shown.empty() ? ell : shown + ell;
        }
        else
        {
            shown = ell;
            while (!shown.empty() && text_w_scaled(shown, scale) > box.w)
                shown.pop_back();
        }
    }

    const int tw = text_w_scaled(shown, scale);
    const int th = text_h_scaled(scale);
    int x = center ? box.x + (box.w - tw) / 2 : box.x;
    int y = box.y + (box.h - th) / 2;

    SDL_Rect oldClip;
    SDL_RenderGetClipRect(r, &oldClip);
    // Clip glyphs to `box` INTERSECTED with the ambient clip -- replacing it would
    // let a box straddling a scrolled panel's edge spill text out of the panel (and
    // out of the window).  When there's no ambient clip, just use `box`.
    const bool hadClip = (oldClip.w > 0 && oldClip.h > 0);
    SDL_Rect clip = box;
    bool visible = true;
    if (hadClip) visible = (SDL_IntersectRect(&box, &oldClip, &clip) == SDL_TRUE);
    if (visible) {
        SDL_RenderSetClipRect(r, &clip);
        draw_scaled(r, x, y, shown, c, scale);
    }
    SDL_RenderSetClipRect(r, hadClip ? &oldClip : nullptr);
}

void Font::draw_centered(SDL_Renderer* r, const SDL_Rect& box, const std::string& s, Color c) const
{
    draw_fitted(r, box, s, c, true);
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
    int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
    for (auto it = children.rbegin(); it != children.rend(); ++it)
        if ((*it)->visible && (*it)->hit(mx, my) && (*it)->on_wheel(app, dx, dy)) return true;
    return false;
}

void Label::draw(App& app) {
    SDL_Rect inset{ rect.x, rect.y, rect.w, rect.h };
    app.font.draw_fitted(app.ren, inset, text, theme().text, false);
}

void Button::draw(App& app) {
    const Theme& t = theme();
    bool active = (toggle && on) || down;
    fill_rect(app.ren, rect, active ? t.accent : t.panel);
    frame_rect(app.ren, rect, t.dim);
    SDL_Rect inset{ rect.x + 4, rect.y + 2, rect.w - 8, rect.h - 4 };
    app.font.draw_fitted(app.ren, inset, text, active ? t.bg : t.text, true);
}
bool Button::on_mouse(App& app, const MouseEv& e) {
    if (e.pressed) { down = true; app.request_redraw(); return true; }
    // release
    down = false;
    if (hit(e.x, e.y)) { if (toggle) on = !on; if (clicked) clicked(); }
    app.request_redraw();
    return true;
}

// ---- MenuBar ---------------------------------------------------------------
void MenuBar::draw(App& app) {
    const Theme& t = theme();
    m_ddx = m_ddy = m_ddw = m_ddh = 0;
    // title strip
    SDL_Rect bar{ rect.x, rect.y, rect.w, bar_h };
    fill_rect(app.ren, bar, t.panel);
    frame_rect(app.ren, bar, t.dim);
    int x = rect.x + 4;
    const int pad = 10;
    for (size_t i=0;i<menus.size();++i) {
        Menu& m = menus[i];
        m._x = x;
        m._w = std::min(app.font.text_w(m.title) + pad*2,
                        std::max(28, rect.x + rect.w - x));
        if (m._w <= 0) { m._w = 0; continue; }
        SDL_Rect tr{ x, rect.y, m._w, bar_h };
        if ((int)i == open) fill_rect(app.ren, tr, t.accent);
        SDL_Rect inset{ tr.x + 4, tr.y + 1, tr.w - 8, tr.h - 2 };
        app.font.draw_fitted(app.ren, inset, m.title, (int)i==open ? t.bg : t.text, true);
        x += m._w;
    }
    // dropdown
    if (open >= 0 && open < (int)menus.size()) {
        Menu& m = menus[open];
        int iw = 0;
        for (auto& it : m.items) iw = std::max(iw, app.font.text_w(it.label));
        int w = std::min(iw + 40, std::max(80, rect.w - 8));
        int rowh = app.font.ch() + 8;
        int h = 6; for (auto& it : m.items) h += it.separator ? 6 : rowh;
        m_ddx = std::max(rect.x, std::min(m._x, rect.x + rect.w - w));
        m_ddy = rect.y + bar_h;
        if (rect.h > bar_h) h = std::min(h, rect.h - bar_h);
        m_ddw = w; m_ddh = h;
        SDL_Rect dd{ m_ddx, m_ddy, w, h };
        fill_rect(app.ren, dd, t.panel);
        frame_rect(app.ren, dd, t.dim);
        int yy = m_ddy + 3;
        for (auto& it : m.items) {
            if (it.separator) {
                SDL_Rect ln{ m_ddx+4, yy+3, w-8, 1 }; fill_rect(app.ren, ln, t.dim); yy += 6; continue;
            }
            if (yy + rowh > m_ddy + m_ddh) break;
            bool ck = it.check && it.checked && it.checked();
            Color fg = it.enabled ? t.text : t.dim;
            if (ck) { app.font.draw(app.ren, m_ddx+6, yy+4, "*", t.accent); }
            app.font.draw(app.ren, m_ddx+20, yy+4, ellipsize(app.font, it.label, w - 26), fg);
            yy += rowh;
        }
    }
}
bool MenuBar::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) return (open >= 0);   // swallow the release that follows a menu press
    // click on a title
    if (e.y >= rect.y && e.y < rect.y + bar_h) {
        for (size_t i=0;i<menus.size();++i) {
            if (e.x >= menus[i]._x && e.x < menus[i]._x + menus[i]._w) {
                open = (open==(int)i) ? -1 : (int)i;
                app.request_redraw();
                return true;
            }
        }
        // clicked bar background
        open = -1; app.request_redraw(); return true;
    }
    // click inside the open dropdown
    if (open >= 0 && e.x>=m_ddx && e.x<m_ddx+m_ddw && e.y>=m_ddy && e.y<m_ddy+m_ddh) {
        Menu& m = menus[open];
        int rowh = app.font.ch() + 8;
        int yy = m_ddy + 3;
        for (auto& it : m.items) {
            int rh = it.separator ? 6 : rowh;
            if (yy + rh > m_ddy + m_ddh) break;
            if (!it.separator && e.y>=yy && e.y<yy+rowh) {
                if (it.enabled && it.action) { open=-1; app.request_redraw(); it.action(); return true; }
            }
            yy += rh;
        }
        open = -1; app.request_redraw(); return true;
    }
    // click anywhere else: if a menu was open, close it and consume; else pass through
    if (open >= 0) { open = -1; app.request_redraw(); return true; }
    return false;
}

// ---- Window ----------------------------------------------------------------
// Title-bar buttons laid right-to-left: close, maximize/restore, minimize.
static void win_buttons(const Window& w, SDL_Rect& closeB, SDL_Rect& maxB, SDL_Rect& minB) {
    const int th = w.title_h;
    int x = w.rect.x + w.rect.w - th;
    closeB = maxB = minB = SDL_Rect{0,0,0,0};
    if (w.closable)    { closeB = SDL_Rect{ x, w.rect.y, th, th }; x -= th; }
    if (w.resizable)   { maxB   = SDL_Rect{ x, w.rect.y, th, th }; x -= th; }
    if (w.minimizable) { minB   = SDL_Rect{ x, w.rect.y, th, th }; }
}
static bool pt_in(const SDL_Rect& r, int x, int y) {
    return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
static SDL_Rect safe_body(const Window& w) {
    return SDL_Rect{ w.rect.x + 1, w.rect.y + w.title_h,
                     std::max(1, w.rect.w - 2),
                     std::max(1, w.rect.h - w.title_h - 1) };
}
static void keep_window_reachable(Window& w) {
    SDL_Rect ws = w.workspace.w > 0 ? w.workspace : SDL_Rect{ 0, 0, 100000, 100000 };
    const int minw = 160, minh = 80;
    if (w.rect.w < minw) w.rect.w = minw;
    if (w.rect.h < minh) w.rect.h = minh;
    if (ws.w <= 0 || ws.h <= 0) return;
    const int visible = 32;
    if (w.rect.w > ws.w) w.rect.w = std::max(minw, ws.w);
    if (w.rect.h > ws.h) w.rect.h = std::max(minh, ws.h);
    const int minX = ws.x - w.rect.w + visible;
    const int maxX = ws.x + ws.w - visible;
    const int minY = ws.y;
    const int maxY = ws.y + std::max(0, ws.h - w.title_h);
    w.rect.x = std::max(minX, std::min(maxX, w.rect.x));
    w.rect.y = std::max(minY, std::min(maxY, w.rect.y));
}

void Window::toggle_maximize() {
    if (!maximized) {
        restore_rect = rect;
        if (workspace.w > 0) rect = SDL_Rect{ workspace.x, workspace.y, workspace.w, workspace.h };
        maximized = true;
    } else {
        if (restore_rect.w > 0) rect = restore_rect;
        maximized = false;
    }
}

void Window::draw(App& app) {
    const Theme& t = theme();
    keep_window_reachable(*this);
    fill_rect(app.ren, rect, t.bg);
    SDL_Rect tb{ rect.x, rect.y, rect.w, title_h };
    fill_rect(app.ren, tb, focused ? t.accent : t.panel);
    Color tfg = focused ? t.bg : t.text;
    Color sep = focused ? t.bg : t.dim;
    SDL_Rect cB, maxB, minB; win_buttons(*this, cB, maxB, minB);
    const int buttonsLeft = (minimizable ? minB.x : (resizable ? maxB.x : (closable ? cB.x : rect.x + rect.w)));
    app.font.draw(app.ren, rect.x+7, rect.y + (title_h-app.font.ch())/2,
                  ellipsize(app.font, title, buttonsLeft - rect.x - 12), tfg);
    if (minimizable) { app.font.draw_centered(app.ren, minB, "_",  tfg); vline(app.ren, minB.x, minB.y, minB.y+title_h, sep); }
    if (resizable)   { app.font.draw_centered(app.ren, maxB, maximized?"=":"[]", tfg); vline(app.ren, maxB.x, maxB.y, maxB.y+title_h, sep); }
    if (closable)    { app.font.draw_centered(app.ren, cB,  "x",  tfg); vline(app.ren, cB.x, cB.y, cB.y+title_h, sep); }
    if (content) {
        SDL_Rect b = safe_body(*this);
        content->rect = b; content->visible = true;
        SDL_Rect oldClip; SDL_RenderGetClipRect(app.ren, &oldClip);
        SDL_RenderSetClipRect(app.ren, &b);
        content->draw(app);
        SDL_RenderSetClipRect(app.ren, oldClip.w || oldClip.h ? &oldClip : nullptr);
    }
    frame_rect(app.ren, rect, focused ? t.accent : t.dim);
    if (resizable && !maximized) {
        int gx = rect.x + rect.w, gy = rect.y + rect.h;
        for (int i=1;i<=3;++i) hline(app.ren, gx-4*i, gx-2, gy-2*i-1, t.dim);
    }
}
bool Window::on_mouse(App& app, const MouseEv& e) {
    const int grip = 16;
    if (!e.pressed) {
        m_moving = m_resizing = false;
        if (m_content && content) content->on_mouse(app, e);
        m_content = false;
        return true;
    }
    if (m_moving)   { rect.x = e.x - m_dx; rect.y = e.y - m_dy;
                      keep_window_reachable(*this); app.request_redraw(); return true; }
    if (m_resizing) { int nw = (e.x + m_dx) - rect.x, nh = (e.y + m_dy) - rect.y;
                      rect.w = nw < 160 ? 160 : nw; rect.h = nh < 80 ? 80 : nh;
                      keep_window_reachable(*this);
                      app.request_redraw(); return true; }
    if (m_content)  { if (content) content->on_mouse(app, e); return true; }

    if (!maximized && e.button == SDL_BUTTON_LEFT && (SDL_GetModState() & KMOD_ALT)) {
        m_moving = true;
        m_dx = e.x - rect.x;
        m_dy = e.y - rect.y;
        return true;
    }

    // title-bar buttons
    SDL_Rect cB, maxB, minB; win_buttons(*this, cB, maxB, minB);
    if (closable    && pt_in(cB,   e.x, e.y)) { if (on_close) on_close(); visible = false; app.request_redraw(); return true; }
    if (resizable   && pt_in(maxB, e.x, e.y)) { toggle_maximize(); app.request_redraw(); return true; }
    if (minimizable && pt_in(minB, e.x, e.y)) { minimized = true; app.request_redraw(); return true; }

    // title bar -> move, or double-click -> maximize
    if (e.y >= rect.y && e.y < rect.y + title_h) {
        unsigned now = SDL_GetTicks();
        if (now - m_last_title_click < 350) { m_last_title_click = 0; toggle_maximize(); app.request_redraw(); return true; }
        m_last_title_click = now;
        if (!maximized) { m_moving = true; m_dx = e.x - rect.x; m_dy = e.y - rect.y; }
        return true;
    }
    if (resizable && !maximized && e.x >= rect.x + rect.w - grip && e.y >= rect.y + rect.h - grip) {
        m_resizing = true; m_dx = rect.x + rect.w - e.x; m_dy = rect.y + rect.h - e.y; return true;
    }
    SDL_Rect b = safe_body(*this);
    if (content && pt_in(b, e.x, e.y)) { m_content = true; return content->on_mouse(app, e); }
    return true;
}
bool Window::on_wheel(App& app, int dx, int dy) {
    int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
    SDL_Rect b = safe_body(*this);
    return content && pt_in(b, mx, my) ? content->on_wheel(app, dx, dy) : false;
}
bool Window::on_key(App& app, SDL_Keycode k) {
    return content ? content->on_key(app, k) : false;
}
bool Window::on_key_up(App& app, SDL_Keycode k) {
    return content ? content->on_key_up(app, k) : false;
}

// ---- WindowManager ---------------------------------------------------------
void WindowManager::add(Window* w) {
    if (!w) return;
    remove(w);
    w->visible = true;
    w->minimized = false;
    windows.push_back(w);
    raise(w);
}
void WindowManager::remove(Window* w) {
    if (m_active == w) m_active = nullptr;
    for (auto it = windows.begin(); it != windows.end(); ++it)
        if (*it == w) { windows.erase(it); break; }
    for (auto* x : windows) x->focused = false;
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if ((*it)->visible && !(*it)->minimized) { (*it)->focused = true; break; }
}
void WindowManager::raise(Window* w) {
    if (!w) return;
    for (auto it = windows.begin(); it != windows.end(); ++it)
        if (*it == w) { windows.erase(it); break; }
    windows.push_back(w);
    for (auto* x : windows) x->focused = (x == w);
}
bool WindowManager::any_visible() const {
    for (auto* w : windows) if (w->visible) return true;
    return false;
}
// Minimized windows form a tray at the bottom of the workspace (click to restore).
std::vector<Window*> WindowManager::taskbar_hits(App& app, std::vector<SDL_Rect>& rectsOut) const {
    std::vector<Window*> list; rectsOut.clear();
    SDL_Rect ws = (workspace.w > 0) ? workspace : SDL_Rect{ rect.x, rect.y, rect.w, rect.h };
    int x = ws.x + 4, bw = 130;
    for (auto* w : windows)
        if (w->visible && w->minimized) {
            if (x + 40 > ws.x + ws.w) break;
            int actualW = std::min(bw, std::max(40, ws.x + ws.w - x - 4));
            rectsOut.push_back(SDL_Rect{ x, ws.y + ws.h - taskbar_h + 2, actualW, taskbar_h - 4 });
            list.push_back(w); x += actualW + 3;
        }
    return list;
}
void WindowManager::draw(App& app) {
    const Theme& t = theme();
    SDL_Rect ws = (workspace.w > 0) ? workspace : SDL_Rect{ rect.x, rect.y, rect.w, rect.h };
    // maximize bounds = workspace (leave room for the tray only when it shows)
    std::vector<SDL_Rect> trects; std::vector<Window*> tray = taskbar_hits(app, trects);
    int reserve = tray.empty() ? 0 : taskbar_h;
    for (auto* w : windows) {
        w->workspace = SDL_Rect{ ws.x, ws.y, ws.w, ws.h - reserve };
        if (!w->visible) w->focused = false;
        if (w->maximized) w->rect = w->workspace;
        if (w->visible && !w->minimized) w->draw(app);
    }
    if (!tray.empty()) {
        SDL_Rect bar{ ws.x, ws.y + ws.h - taskbar_h, ws.w, taskbar_h };
        fill_rect(app.ren, bar, t.panel);
        hline(app.ren, bar.x, bar.x + bar.w, bar.y, t.accent);
        for (size_t i = 0; i < tray.size(); ++i) {
            SDL_Rect it = trects[i];
            fill_rect(app.ren, it, t.bg); frame_rect(app.ren, it, t.dim);
            std::string s = ellipsize(app.font, tray[i]->title, it.w - 10);
            app.font.draw(app.ren, it.x + 5, it.y + (it.h-app.font.ch())/2, s, t.text);
        }
    }
}
bool WindowManager::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed) {
        if (m_active) { m_active->on_mouse(app, e); m_active = nullptr; app.request_redraw(); return true; }
        return false;
    }
    if (m_active) { m_active->on_mouse(app, e); app.request_redraw(); return true; }

    // tray click -> restore + focus a minimized window
    std::vector<SDL_Rect> trects; std::vector<Window*> tray = taskbar_hits(app, trects);
    for (size_t i = 0; i < tray.size(); ++i)
        if (pt_in(trects[i], e.x, e.y)) {
            tray[i]->minimized = false;
            if (tray[i]->restore_rect.w > 0 && !tray[i]->maximized) tray[i]->rect = tray[i]->restore_rect;
            raise(tray[i]); app.request_redraw(); return true;
        }

    // topmost non-minimized hit window grabs the press (and becomes drag target)
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        Window* w = *it;
        if (w->visible && !w->minimized && w->hit(e.x, e.y)) {
            raise(w); m_active = w; w->on_mouse(app, e); app.request_redraw(); return true;
        }
    }
    return false;   // missed -> fall through to the workspace
}
bool WindowManager::on_wheel(App& app, int dx, int dy) {
    int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if ((*it)->visible && !(*it)->minimized && (*it)->hit(mx, my) &&
            (*it)->on_wheel(app, dx, dy)) return true;
    return false;
}
bool WindowManager::on_key(App& app, SDL_Keycode k) {
    // Route keys to the focused (front-most, visible, non-minimized) window so
    // clip editors hosted in windows receive note entry / copy-paste keys.
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if ((*it)->visible && !(*it)->minimized) return (*it)->on_key(app, k);
    return false;
}
bool WindowManager::on_key_up(App& app, SDL_Keycode k) {
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if ((*it)->visible && !(*it)->minimized) return (*it)->on_key_up(app, k);
    return false;
}

// amplitude (0..~1, 1==0 dBFS) -> 0..1 fraction over a [-60 .. +6] dB scale.
static float fader_db_frac(float amp) {
    if (amp <= 1e-4f) return 0.f;
    float db = 20.0f * std::log10(amp);
    float f = (db + 60.0f) / 66.0f;                 // -60 dB -> 0, +6 dB -> 1
    return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
}

void Fader::draw(App& app) {
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.keybg);
    frame_rect(app.ren, rect, t.dim);
    const int top = rect.y + 4, bot = rect.y + rect.h - 4;
    int track = bot - top; if (track < 1) track = 1;

    if (level) {
        // --- live level meter backdrop (dB-scaled) ---
        float lin = level(); if (lin < 0.f) lin = 0.f; if (lin > 2.f) lin = 2.f;
        if (lin >= m_peak) m_peak = lin; else m_peak *= 0.93f;   // decaying peak hold
        int mh = int(fader_db_frac(lin) * track);
        if (mh > 0) {
            SDL_Rect mq { rect.x + 3, bot - mh, rect.w - 6, mh };
            fill_rect(app.ren, mq, lin < 0.891f ? t.accent : t.note);  // >-1dB -> hot color
        }
        // dB tick marks (0, -6, -12, -24, -48 dB) on the right edge
        const float dbs[] = { 0.f, -6.f, -12.f, -24.f, -48.f };
        for (float db : dbs) {
            int y = bot - int(((db + 60.0f) / 66.0f) * track);
            hline(app.ren, rect.x + rect.w - 5, rect.x + rect.w - 2, y, t.dim);
        }
        // peak-hold line
        int py = bot - int(fader_db_frac(m_peak) * track);
        hline(app.ren, rect.x + 3, rect.x + rect.w - 3, py, t.hi);
    } else {
        // plain fader: filled portion below the handle
        int knobY = top + int((1.0f - value) * track);
        SDL_Rect fillq { rect.x + 3, knobY, rect.w - 6, bot - knobY };
        fill_rect(app.ren, fillq, t.accent);
    }

    // --- fader handle (the gain value) drawn on top of the meter ---
    int knobY = top + int((1.0f - value) * track);
    SDL_Rect knob { rect.x, knobY - 2, rect.w, 5 };
    fill_rect(app.ren, knob, t.hi);
    frame_rect(app.ren, knob, t.text);
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

// ---- pan-canvas scroll bars ------------------------------------------------
namespace {
// One axis: compute the track (given by trackPos/trackLen) thumb for a viewport
// of `viewLen` px showing content [c0,c1] (canvas px) at pan `off`.  Returns the
// thumb pos+len, the range start (canvas px), and the scrollable amount `avail`.
void sb_axis(int trackPos, int trackLen, int viewLen, int c0, int c1, int off,
             int& thumbPos, int& thumbLen, double& rangeStart, double& avail) {
    if (viewLen < 1) viewLen = 1;
    const double pad = 40.0;
    double viewStart = -(double)off;
    double viewEnd   = -(double)off + viewLen;
    double rs = (c0 < viewStart ? (double)c0 : viewStart) - pad;   // union with view
    double re = (c1 > viewEnd   ? (double)c1 : viewEnd)   + pad;
    double rangeLen = re - rs; if (rangeLen < 1.0) rangeLen = 1.0;
    int tl = (int)((double)viewLen / rangeLen * trackLen);
    tl = tl < 20 ? 20 : (tl > trackLen ? trackLen : tl);
    avail = rangeLen - viewLen; if (avail < 0) avail = 0;
    double scroll = viewStart - rs; scroll = scroll < 0 ? 0 : (scroll > avail ? avail : scroll);
    thumbPos = trackPos + (int)(avail > 0 ? (scroll / avail) * (trackLen - tl) : 0);
    thumbLen = tl; rangeStart = rs;
}
} // namespace

void CanvasScroll::draw(App& app, const SDL_Rect& r,
                        int cl, int cr, int ct, int cb, int ox, int oy) {
    const Theme& t = theme();
    int viewW = r.w - sb, viewH = r.h - sb;
    int tp, tl; double rs, av;
    sb_axis(r.x, viewW, viewW, cl, cr, ox, tp, tl, rs, av);
    SDL_Rect trkH{ r.x, r.y + r.h - sb, viewW, sb };
    SDL_Rect thmH{ tp, trkH.y + 2, tl, sb - 4 };
    fill_rect(app.ren, trkH, t.keybg); frame_rect(app.ren, trkH, t.dim);
    fill_rect(app.ren, thmH, t.dim);   frame_rect(app.ren, thmH, t.hi);
    sb_axis(r.y, viewH, viewH, ct, cb, oy, tp, tl, rs, av);
    SDL_Rect trkV{ r.x + r.w - sb, r.y, sb, viewH };
    SDL_Rect thmV{ trkV.x + 2, tp, sb - 4, tl };
    fill_rect(app.ren, trkV, t.keybg); frame_rect(app.ren, trkV, t.dim);
    fill_rect(app.ren, thmV, t.dim);   frame_rect(app.ren, thmV, t.hi);
}

bool CanvasScroll::on_mouse(const SDL_Rect& r, const MouseEv& e, bool downEdge,
                            int cl, int cr, int ct, int cb, int& ox, int& oy) {
    int viewW = r.w - sb, viewH = r.h - sb;
    int tpH, tlH, tpV, tlV; double rsH, avH, rsV, avV;
    sb_axis(r.x, viewW, viewW, cl, cr, ox, tpH, tlH, rsH, avH);
    sb_axis(r.y, viewH, viewH, ct, cb, oy, tpV, tlV, rsV, avV);
    SDL_Rect trkH{ r.x, r.y + r.h - sb, viewW, sb };
    SDL_Rect trkV{ r.x + r.w - sb, r.y, sb, viewH };

    if (!e.pressed) { bool was = (drag != 0); drag = 0; return was; }
    if (e.button != SDL_BUTTON_LEFT) return false;

    if (drag == 1) {
        int denom = trkH.w - tlH;
        double frac = denom > 0 ? (double)((e.x - grab) - trkH.x) / denom : 0.0;
        frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
        ox = (int)(-(rsH + frac * avH) + (rsH + frac * avH < 0 ? -0.5 : 0.5));
        return true;
    }
    if (drag == 2) {
        int denom = trkV.h - tlV;
        double frac = denom > 0 ? (double)((e.y - grab) - trkV.y) / denom : 0.0;
        frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
        oy = (int)(-(rsV + frac * avV) + (rsV + frac * avV < 0 ? -0.5 : 0.5));
        return true;
    }
    if (!downEdge) return false;
    // fresh press on the horizontal bar
    if (e.x >= trkH.x && e.x < trkH.x + trkH.w && e.y >= trkH.y && e.y < trkH.y + trkH.h) {
        if (e.x >= tpH && e.x < tpH + tlH) grab = e.x - tpH;         // grabbed the thumb
        else {                                                       // clicked the track -> jump
            grab = tlH / 2;
            int denom = trkH.w - tlH;
            double frac = denom > 0 ? (double)((e.x - grab) - trkH.x) / denom : 0.0;
            frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
            ox = (int)(-(rsH + frac * avH));
        }
        drag = 1; return true;
    }
    // fresh press on the vertical bar
    if (e.x >= trkV.x && e.x < trkV.x + trkV.w && e.y >= trkV.y && e.y < trkV.y + trkV.h) {
        if (e.y >= tpV && e.y < tpV + tlV) grab = e.y - tpV;
        else {
            grab = tlV / 2;
            int denom = trkV.h - tlV;
            double frac = denom > 0 ? (double)((e.y - grab) - trkV.y) / denom : 0.0;
            frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
            oy = (int)(-(rsV + frac * avV));
        }
        drag = 2; return true;
    }
    return false;
}

// ---- app shell -------------------------------------------------------------
bool App::init(const char* title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr,"SDL_Init: %s\n", SDL_GetError()); return false; }
    if (TTF_Init() != 0) { fprintf(stderr,"TTF_Init: %s\n", TTF_GetError()); return false; }
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { fprintf(stderr,"CreateWindow: %s\n", SDL_GetError()); return false; }
    ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(window, -1, 0);   // software fallback (e.g. headless)
    if (!ren) { fprintf(stderr,"CreateRenderer: %s\n", SDL_GetError()); return false; }

    // HiDPI / high-res: env override, else auto from the physical/logical ratio.
    scale = 1.0f;
    if (const char* s = getenv("PATCHKNOBSDL_SCALE")) {
        float v = (float)atof(s); if (v >= 0.5f && v <= 4.0f) scale = v;
    } else {
        int pw,ph,ww,wh; SDL_GetRendererOutputSize(ren,&pw,&ph); SDL_GetWindowSize(window,&ww,&wh);
        if (ww > 0) { float a = (float)pw/ww; if (a >= 1.0f && a <= 4.0f) scale = a; }
    }
    { int pw,ph; SDL_GetRendererOutputSize(ren,&pw,&ph);
      w = (int)(pw/scale + 0.5f); h = (int)(ph/scale + 0.5f); }
    // logical size scales rendering to the physical output AND maps mouse coords
    // into logical space automatically.
    SDL_RenderSetLogicalSize(ren, w, h);

    if (!font.load(ren, 15, scale)) return false;
    mono.load(ren, 13, scale);
    return true;
}

void App::begin_text(std::string* t, std::function<void()> onChange,
                     std::function<void(bool)> onCommit) {
    text_target = t; text_changed = std::move(onChange); text_commit = std::move(onCommit);
    SDL_StartTextInput(); dirty = true;
}
void App::end_text() {
    text_target = nullptr; text_changed = nullptr; text_commit = nullptr;
    SDL_StopTextInput(); dirty = true;
}

void App::run(std::function<void(App&)> draw_extra) {
    // Mouse capture: the root that handles a press receives all motion until the
    // release, and the release itself -- so a button gets its click, a drag stays
    // with its widget, and a fullscreen view can't swallow another widget's
    // release.  Without this, on release (which skips hit-testing) the topmost
    // visible root eats the event before the pressed widget sees it.
    Widget* captured = nullptr;
    constexpr Uint64 targetFrameRate = 60;
    const Uint64 performanceFrequency = SDL_GetPerformanceFrequency();
    const Uint64 frameInterval = (performanceFrequency + targetFrameRate - 1) / targetFrameRate;
    Uint64 lastFrameStart = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    int pw,ph; SDL_GetRendererOutputSize(ren,&pw,&ph);
                    w = (int)(pw/scale + 0.5f); h = (int)(ph/scale + 0.5f);
                    SDL_RenderSetLogicalSize(ren, w, h); dirty = true;
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) dirty = true;
                break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                MouseEv m { ev.button.x, ev.button.y, ev.button.button,
                            ev.type==SDL_MOUSEBUTTONDOWN };
                if (m.pressed) {
                    captured = nullptr;
                    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->hit(m.x,m.y) && (*it)->on_mouse(*this, m)) {
                            captured = *it; break;
                        }
                } else {
                    // release -> the capturing root (if any), else topmost visible
                    if (captured) captured->on_mouse(*this, m);
                    else for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->on_mouse(*this, m)) break;
                    captured = nullptr;
                }
                break; }
            case SDL_MOUSEMOTION:
                if (ev.motion.state & SDL_BUTTON_LMASK) {
                    MouseEv m { ev.motion.x, ev.motion.y, SDL_BUTTON_LEFT, true };
                    if (captured) captured->on_mouse(*this, m);
                    else for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->hit(m.x,m.y) && (*it)->on_mouse(*this, m)) break;
                } else dirty = true;
                break;
            case SDL_MOUSEWHEEL: {
                // Route the wheel to the window/view UNDER the pointer, so each
                // window's own zoom (arrange ticks/px, sample editor samples/px)
                // activates based on where you're hovering.
                int wmx = 0, wmy = 0; SDL_GetMouseState(&wmx, &wmy);
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->hit(wmx, wmy) &&
                        (*it)->on_wheel(*this, ev.wheel.x, ev.wheel.y)) break;
                break; }
            case SDL_TEXTINPUT:
                if (text_target) { *text_target += ev.text.text; if (text_changed) text_changed(); dirty = true; }
                else if (text_input_sink) { text_input_sink(ev.text.text); dirty = true; }
                break;
            case SDL_KEYDOWN:
                if (text_target) {              // editing a field: keys edit it, not widgets
                    SDL_Keycode k = ev.key.keysym.sym;
                    SDL_Keymod mod = (SDL_Keymod)ev.key.keysym.mod;
                    if ((mod & KMOD_CTRL) && k == SDLK_a) {
                        text_target->clear(); if (text_changed) text_changed(); dirty = true;
                    }
                    else if (k == SDLK_BACKSPACE) {
                        pop_utf8_char(*text_target);
                        if (text_changed) text_changed();
                        dirty = true;
                    }
                    else if (k == SDLK_DELETE) {
                        text_target->clear();
                        if (text_changed) text_changed();
                        dirty = true;
                    }
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { auto c=text_commit; end_text(); if (c) c(true); }
                    else if (k == SDLK_ESCAPE) { auto c=text_commit; end_text(); if (c) c(false); }
                    break;
                }
                // ESC does NOT quit the app; it is dispatched to views below so
                // it can clear selections / close palettes.  Quit is via the menu.
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->on_key(*this, ev.key.keysym.sym)) break;
                break;
            case SDL_KEYUP:
                if (text_target) break;         // editing a field: ignore releases
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->on_key_up(*this, ev.key.keysym.sym)) break;
                break;
            }
        }
        // Low-rate housekeeping (~4 Hz) regardless of render state -- device-loss
        // polls must fire during playback (animating) too, not just when idle.
        {
            static Uint32 lastTick = 0;
            Uint32 now = SDL_GetTicks();
            if (now - lastTick >= 250) { lastTick = now; if (on_tick) on_tick(*this); }
        }
        // Retained + dirty: render on change, or continuously while animating
        // (playback playhead).  Idle otherwise so the CPU stays free for audio.
        if (dirty || animating) {
            const Uint64 now = SDL_GetPerformanceCounter();
            if (lastFrameStart != 0 && now - lastFrameStart < frameInterval) {
                const Uint64 remaining = frameInterval - (now - lastFrameStart);
                const Uint32 sleepMs = static_cast<Uint32>((remaining * 1000) / performanceFrequency);
                if (sleepMs > 0) {
                    SDL_Delay(sleepMs);
                    continue;
                }
            }
            lastFrameStart = SDL_GetPerformanceCounter();
            if (on_layout) on_layout(*this);
            fill_rect(ren, SDL_Rect{0,0,w,h}, theme().bg);
            for (auto* rt : roots) if (rt->visible) rt->draw(*this);
            if (draw_extra) draw_extra(*this);
            SDL_RenderPresent(ren);
            dirty = false;
        } else {
            SDL_Delay(8);   // idle: don't spin
            // Low-rate refresh (~4 Hz) so live readouts (CPU meter, VU) update
            // even when nothing else is dirty -- still nearly idle.
            static int idleTicks = 0;
            if (++idleTicks >= 30) { idleTicks = 0; dirty = true; }
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
