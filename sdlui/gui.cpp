//----------------------------------------------------------------------------
//  sdlui/gui.cpp -- implementation of the lean SDL2 grayscale/green toolkit.
//----------------------------------------------------------------------------
#include "gui.h"
#include "meter.h"
#include "platform/platform_ui.h"
#include <fstream>
#include <SDL_ttf.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#ifdef PATCHKNOB_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

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
// Pointer in LOGICAL coordinates.  SDL_GetMouseState() answers in raw window
// pixels; mouse EVENTS are pushed through the renderer's logical size before a
// widget sees them.  Mixing the two makes every polled hover highlight drift by
// the display scale factor.  SDL_RenderWindowToLogical applies exactly the same
// mapping SDL uses for events, so the two agree at any scale.
void mouse_logical(App& app, int& mx, int& my) {
    int wx = 0, wy = 0;
    SDL_GetMouseState(&wx, &wy);
    if (app.ren) {
        float lx = 0.f, ly = 0.f;
        SDL_RenderWindowToLogical(app.ren, wx, wy, &lx, &ly);
        mx = (int)lx; my = (int)ly;
    } else {
        const float s = app.scale > 0.f ? app.scale : 1.f;
        mx = (int)(wx / s); my = (int)(wy / s);
    }
}

void set_color(SDL_Renderer* r, Color c) { if(r) SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a); }
void fill_rect(SDL_Renderer* r, const SDL_Rect& q, Color c) {
    if(!r||q.w<=0||q.h<=0||c.a==0)return;
    set_color(r,c);SDL_RenderFillRect(r,&q);
}
void frame_rect(SDL_Renderer* r, const SDL_Rect& q, Color c) {
    if(!r||q.w<=0||q.h<=0||c.a==0)return;
    set_color(r,c);SDL_RenderDrawRect(r,&q);
}
void hline(SDL_Renderer* r, int x0, int x1, int y, Color c) {
    if(!r||c.a==0)return;
    if(x1<x0)std::swap(x0,x1);set_color(r,c);SDL_RenderDrawLine(r,x0,y,x1,y);
}
void vline(SDL_Renderer* r, int x, int y0, int y1, Color c) {
    if(!r||c.a==0)return;
    if(y1<y0)std::swap(y0,y1);set_color(r,c);SDL_RenderDrawLine(r,x,y0,x,y1);
}

// ---- font atlas ------------------------------------------------------------
Font::~Font()
{
    // Everything is already gone if App::shutdown() ran first (it must -- see
    // Font::release), and release() is safe to run twice.
    release();
}

void Font::release()
{
    if (m_atlas) { SDL_DestroyTexture(m_atlas); m_atlas = nullptr; }
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
    SDL_FillRect(surface,nullptr,SDL_MapRGBA(surface->format,0,0,0,0));
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
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/system/fonts/RobotoMono-Regular.ttf",
        "/system/fonts/DroidSansMono.ttf", nullptr
    };
    TTF_Font* f = nullptr;
    m_face.clear();
#ifdef PATCHKNOB_USE_FONTCONFIG
    // The hardcoded paths below assume Debian/Ubuntu's font tree; every other
    // desktop distro (Arch, Fedora, NixOS, ...) lays out monospace fonts
    // differently, so ask fontconfig -- the thing that actually knows where
    // fonts live on THIS system -- for its pick of "monospace" first.
    {
        std::string fcPath;
        if (FcInit()) {
            if (FcPattern* pat = FcNameParse((const FcChar8*)"monospace")) {
                FcConfigSubstitute(nullptr, pat, FcMatchPattern);
                FcDefaultSubstitute(pat);
                FcResult result;
                if (FcPattern* match = FcFontMatch(nullptr, pat, &result)) {
                    FcChar8* file = nullptr;
                    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file)
                        fcPath = (const char*)file;
                    FcPatternDestroy(match);
                }
                FcPatternDestroy(pat);
            }
        }
        if (!fcPath.empty()) {
            f = TTF_OpenFont(fcPath.c_str(), pt);
            if (f) m_face = fcPath;
        }
    }
#endif
    for (int i = 0; !f && candidates[i]; ++i) {
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
    if(!atlas){TTF_CloseFont(f);return false;}
    SDL_FillRect(atlas,nullptr,SDL_MapRGBA(atlas->format,0,0,0,0));
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
    if(m_atlas)SDL_SetTextureBlendMode(m_atlas, SDL_BLENDMODE_BLEND);
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
        return glyph_count(s) *
               std::max(1, (int)std::lround(float(m_cw) * point / m_logical_point));
    }
    return (int)std::lround(text_w(s) * scale);
}

int Font::glyph_count(const std::string& s)
{
    int n=0;
    for(size_t i=0;i<s.size();) {
        const unsigned char c=(unsigned char)s[i];
        size_t bytes=c<0x80?1:(c<0xE0?2:(c<0xF0?3:4));
        if(i+bytes>s.size())bytes=1;
        i+=bytes;++n;
    }
    return n;
}

int Font::text_w(const std::string& s) const { return glyph_count(s)*m_cw; }

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

    // SDL_RenderGeometry can fail transiently (a lost/reset device, a momentary
    // allocation failure).  Latching that failure for the life of the process
    // downgraded EVERY string in the app to one SDL_RenderCopy per glyph for
    // good, so re-arm on a timer: one bad frame costs a second of slow text
    // instead of the whole session.
    static SDL_Renderer* geometry_renderer = nullptr;
    static bool geometry_available = true;
    static Uint32 geometry_failed_at = 0;
    if (geometry_renderer != r) {
        geometry_renderer = r;
        geometry_available = true;
    } else if (!geometry_available && SDL_GetTicks() - geometry_failed_at >= 1000) {
        geometry_available = true;
    }

    // Reject/clamp before building geometry. Long source/Csound/Pd lines used
    // to allocate and submit thousands of invisible vertices every repaint.
    SDL_Rect clip{};SDL_RenderGetClipRect(r,&clip);
    int rw=0,rh=0;SDL_RenderGetLogicalSize(r,&rw,&rh);
    if(rw<=0||rh<=0)SDL_GetRendererOutputSize(r,&rw,&rh);
    if(clip.w<=0||clip.h<=0)clip=SDL_Rect{0,0,rw,rh};
    if(y+cellH<=clip.y||y>=clip.y+clip.h)return;

    static thread_local std::vector<SDL_Vertex> vertices;
    vertices.clear();
    vertices.reserve(std::min<size_t>(s.size(),(size_t)std::max(1,clip.w/cellW+2))*6);

    const float atlas_w = float(cellWP * (m_last - m_first + 1));
    const auto color = pk_vertex_color(c.r, c.g, c.b, c.a);
    int column=0;
    for (size_t i=0;i<s.size();) {
        const unsigned char lead=(unsigned char)s[i];
        size_t bytes=lead<0x80?1:(lead<0xE0?2:(lead<0xF0?3:4));
        if(i+bytes>s.size())bytes=1;
        int ch=(bytes==1&&lead>=m_first&&lead<=m_last)?lead:'?';
        i+=bytes;
        float x0=float(x+column*cellW);++column;
        float x1 = x0 + float(cellW);
        if(x1<=clip.x)continue;
        if(x0>=clip.x+clip.w)break;
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
    geometry_failed_at = SDL_GetTicks();
    SDL_SetTextureColorMod(atlasTexture, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(atlasTexture, c.a);
    int fallbackColumn=0;
    for (size_t i=0;i<s.size();) {
        const unsigned char lead=(unsigned char)s[i];
        size_t bytes=lead<0x80?1:(lead<0xE0?2:(lead<0xF0?3:4));
        if(i+bytes>s.size())bytes=1;
        int ch=(bytes==1&&lead>=m_first&&lead<=m_last)?lead:'?';i+=bytes;
        const int dx=x+fallbackColumn++*cellW;
        if(dx+cellW<=clip.x)continue;if(dx>=clip.x+clip.w)break;
        SDL_Rect src { (ch - m_first)*cellWP, 0, cellWP, cellHP };
        SDL_Rect dst { dx, y, cellW, cellH };
        SDL_RenderCopy(r, atlasTexture, &src, &dst);
    }
    // The mods are texture state, not draw state: leaving them set tints every
    // later use of this atlas (including the SDL_RenderGeometry path, which
    // multiplies its vertex colour by them) with whatever the last fallback
    // string happened to be.
    SDL_SetTextureColorMod(atlasTexture, 255, 255, 255);
    SDL_SetTextureAlphaMod(atlasTexture, 255);
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

    // BATCHING: a clip-rect change is a render command, and SDL cannot merge
    // draws across one.  Setting the clip to `box` and restoring it around every
    // single string therefore ends the batch twice per label, which on a GCN
    // card (high per-draw-call driver cost) is the expensive part of drawing a
    // dense panel -- not the pixels.
    //
    // The clip is only actually needed when the text can escape `box`.  Above,
    // this function has already shrunk the scale to fit `box.h` and ellipsised
    // to fit `box.w`, so in the overwhelmingly common case the glyphs are
    // already inside it and the clip would discard nothing.  Detect that and
    // skip both commands; the ambient clip still applies, so a label inside a
    // scrolled panel is still contained by the panel exactly as before.
    //
    // `minScale` can stop the shrink before the text fits, so the narrow path
    // is kept verbatim for that case.
    if (tw <= box.w && th <= box.h) {
        draw_scaled(r, x, y, shown, c, scale);
        return;
    }

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
    // Wheel events carry no position, so the target has to be polled -- and
    // widget rects are LOGICAL, while SDL_GetMouseState() answers in raw window
    // pixels.  See mouse_logical() in gui.h: at any scale != 1 the raw pixels
    // hit-test against the wrong widget (or none).
    int mx = 0, my = 0; mouse_logical(app, mx, my);
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
        const int want = app.font.text_w(m.title) + pad*2;
        // A title that does not FIT is given no strip at all.  Clamping it to a
        // minimum width instead (the old behaviour) let neighbouring titles
        // overlap, so their hit rects overlapped too and clicking one opened
        // another; a zero-width title is simply not hit-testable.
        if (x + want > rect.x + rect.w) { m._x = x; m._w = 0; continue; }
        m._x = x;
        m._w = want;
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
        int rowh=platform::menu_row_height(app.font.ch());
        int sepH=platform::menu_separator_height();
        int h = 6; for (auto& it : m.items) h += it.separator ? sepH : rowh;
        m_ddx = std::max(rect.x, std::min(m._x, rect.x + rect.w - w));
        m_ddy = rect.y + bar_h;
        // A menu taller than the bar's own rect used to be truncated with the
        // overflow items simply unreachable.  Let it run to the bottom of the
        // WINDOW instead (the bar is an overlay root and paints last), and only
        // then clamp -- so ordinary menus are always complete.
        const int avail = std::max(rect.h - bar_h, app.h - m_ddy - 2);
        if (avail > 0) h = std::min(h, avail);
        m_ddw = w; m_ddh = h;
        SDL_Rect dd{ m_ddx, m_ddy, w, h };
        fill_rect(app.ren, dd, t.panel);
        frame_rect(app.ren, dd, t.dim);
        int mx = 0, my = 0; mouse_logical(app, mx, my);
        int yy = m_ddy + 3;
        for (auto& it : m.items) {
            if (it.separator) {
                if (yy + sepH > m_ddy + m_ddh) break;
                SDL_Rect ln{ m_ddx+4, yy+sepH/2, w-8, 1 }; fill_rect(app.ren, ln, t.dim); yy += sepH; continue;
            }
            if (yy + rowh > m_ddy + m_ddh) break;
            // Hover / armed highlight: without it the dropdown gave no feedback
            // at all about which row a click would land on.
            SDL_Rect row{ m_ddx+1, yy, w-2, rowh };
            const bool hot = it.enabled && mx >= row.x && mx < row.x + row.w &&
                                           my >= row.y && my < row.y + row.h;
            if (hot) fill_rect(app.ren, row, t.accent);
            bool ck = it.check && it.checked && it.checked();
            Color fg = it.enabled ? (hot ? t.bg : t.text) : t.dim;
            if (ck) { app.font.draw(app.ren, m_ddx+6, yy+4, "*", hot ? t.bg : t.accent); }
            app.font.draw(app.ren, m_ddx+20, yy+4, ellipsize(app.font, it.label, w - 26), fg);
            yy += rowh;
        }
    }
}
bool MenuBar::on_mouse(App& app, const MouseEv& e) {
    auto item_at = [&](int x, int y)->int {
        if (open < 0 || open >= (int)menus.size() ||
            x<m_ddx || x>=m_ddx+m_ddw || y<m_ddy || y>=m_ddy+m_ddh)
            return -1;
        Menu& m = menus[open];
        const int rowh=platform::menu_row_height(app.font.ch());
        const int sepH=platform::menu_separator_height();
        int yy=m_ddy+3;
        for (int i=0;i<(int)m.items.size();++i) {
            int rh=m.items[(size_t)i].separator ? sepH : rowh;
            if (yy+rh>m_ddy+m_ddh) break;
            // Skip separators AND disabled rows: arming a disabled item made the
            // menu look like it had accepted a click it was never going to act on.
            if (!m.items[(size_t)i].separator && m.items[(size_t)i].enabled &&
                y>=yy && y<yy+rh) return i;
            yy+=rh;
        }
        return -1;
    };
    if (!e.pressed) {
        const int armedTitle=m_armed_title, armedItem=m_armed_item;
        m_armed_title=m_armed_item=-1;
        if (armedTitle>=0) {
            // The menu list can be rebuilt between the press and the release
            // (an action may add/remove menus), which used to index out of range.
            if (armedTitle < (int)menus.size() &&
                e.y>=rect.y && e.y<rect.y+bar_h &&
                menus[(size_t)armedTitle]._w > 0 &&
                e.x>=menus[(size_t)armedTitle]._x &&
                e.x<menus[(size_t)armedTitle]._x+menus[(size_t)armedTitle]._w) {
                open=(open==armedTitle) ? -1 : armedTitle;
                app.request_redraw(); return true;
            }
            // Press-on-title, drag down, release on a row: the conventional
            // menu gesture, which previously did nothing at all.
            const int i = item_at(e.x, e.y);
            if (i >= 0 && open >= 0 && open < (int)menus.size()) {
                MenuItem item = menus[(size_t)open].items[(size_t)i];
                open=-1; app.request_redraw();
                if (item.enabled && item.action) item.action();
                return true;
            }
            app.request_redraw(); return true;
        }
        if (armedItem>=0 && open>=0 && open<(int)menus.size() &&
            item_at(e.x,e.y)==armedItem &&
            armedItem<(int)menus[(size_t)open].items.size()) {
            MenuItem item=menus[(size_t)open].items[(size_t)armedItem];
            open=-1; app.request_redraw();
            if (item.enabled && item.action) item.action();
            return true;
        }
        return open>=0;
    }
    // Arm a title on touch/mouse down; opening on release avoids Android's
    // synthesized release immediately dismissing the dropdown.
    if (e.y >= rect.y && e.y < rect.y + bar_h) {
        for (size_t i=0;i<menus.size();++i) {
            if (e.x >= menus[i]._x && e.x < menus[i]._x + menus[i]._w) {
                m_armed_title=(int)i; m_armed_item=-1; return true;
            }
        }
        m_armed_title=m_armed_item=-1;
        open=-1; app.request_redraw(); return true;
    }
    if (open>=0) {
        int i=item_at(e.x,e.y);
        if (i>=0) { m_armed_item=i; m_armed_title=-1; return true; }
    }
    if (open >= 0) { open = -1; app.request_redraw(); return true; }
    return false;
}
// An open dropdown was mouse-only: no key dismissed it, so Esc fell through to
// the view underneath while the menu stayed up over it.
bool MenuBar::on_key(App& app, SDL_Keycode k) {
    if (open < 0) return false;
    if (k == SDLK_ESCAPE) {
        open = -1; m_armed_title = m_armed_item = -1;
        app.request_redraw();
        return true;
    }
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
// Watches, across one event dispatched into a window's content, whether that
// content STARTED (or ended) an inline text edit.  App::text_target is a bare
// std::string* into view-owned storage -- pd_editor_view points it at a string
// living inside a vector -- and nothing in the toolkit knew who it belonged to,
// so closing or minimising the window left it live and invisible: every
// keystroke was swallowed (the main loop tests text_target before
// text_input_sink, so the Csound/tracker editors went deaf too) and the next
// push_back/erase on that vector turned it into a write-after-free.  Observing
// at scope exit covers every return path in Window::on_mouse.
namespace {
struct TextOwnerWatch {
    App& app; std::string*& owned; std::string* before;
    TextOwnerWatch(App& a, std::string*& o) : app(a), owned(o), before(a.text_target) {}
    ~TextOwnerWatch() {
        if (!app.text_target) owned = nullptr;              // edit finished
        else if (app.text_target != before) owned = app.text_target;
    }
    TextOwnerWatch(const TextOwnerWatch&) = delete;
    TextOwnerWatch& operator=(const TextOwnerWatch&) = delete;
};
}

void Window::commit_owned_text(App& app) {
    if (!m_text_owned || app.text_target != m_text_owned) { m_text_owned = nullptr; return; }
    // Give the content a chance to finish on its own terms first -- several
    // views commit the typed value in cancel_interaction() and end the edit
    // themselves; commit_text_if() is then a no-op.
    if (content) content->cancel_interaction(app);
    app.commit_text_if(m_text_owned);
    m_text_owned = nullptr;
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
    //  title_h == 0 means NO TITLE BAR: a full-workspace background pane
    //  (Arrange) has no use for one, and the strip only ate vertical space and
    //  repeated a label the view already makes obvious.  The whole bar is
    //  skipped rather than drawn zero-height -- the text baseline is derived
    //  from title_h, so a 0 there would paint the title ABOVE the window, on
    //  top of the transport.  Input needs no guard: the drag test
    //  (e.y < rect.y + title_h) and the button rects both collapse on their
    //  own, and body() already starts at rect.y.
    if (title_h > 0) {
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
    }
    if (content) {
        SDL_Rect b = safe_body(*this);
        content->rect = b; content->visible = true;
        // INTERSECT, never replace: the manager clips to the workspace, and a
        // window dragged half off it would otherwise widen the clip back out and
        // paint its content over the transport bar / whatever else lives there.
        // (ScopedClip is the rule this file has to follow too -- gui.h:95.)
        ScopedClip clip(app.ren, b);
        content->draw(app);
    }
    frame_rect(app.ren, rect, focused ? t.accent : t.dim);
    if (resizable && !maximized) {
        int gx = rect.x + rect.w, gy = rect.y + rect.h;
        for (int i=1;i<=3;++i) hline(app.ren, gx-4*i, gx-2, gy-2*i-1, t.dim);
    }
}
bool Window::on_mouse(App& app, const MouseEv& e) {
    TextOwnerWatch textWatch(app, m_text_owned);
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
    if (closable && pt_in(cB,e.x,e.y)) {
        if(content) content->cancel_interaction(app);
        // The view is going away; an inline edit pointing into its storage must
        // not outlive it.  end_text_if() only fires when this really is the
        // field still being edited, so closing window A can never cancel a
        // rename in progress in window B.
        app.end_text_if(m_text_owned); m_text_owned = nullptr;
        if(on_close) on_close();
        visible=false; app.request_redraw(); return true;
    }
    if (resizable   && pt_in(maxB, e.x, e.y)) { toggle_maximize(); app.request_redraw(); return true; }
    if(minimizable&&pt_in(minB,e.x,e.y)) {
        if(content)content->cancel_interaction(app);
        app.end_text_if(m_text_owned); m_text_owned = nullptr;   // as for close
        minimized=true; app.request_redraw(); return true;
    }

    // title bar -> move, or double-click -> maximize
    if (e.y >= rect.y && e.y < rect.y + title_h) {
        unsigned now = SDL_GetTicks();
        // `resizable` is what gates the maximize BUTTON (and the resize grip);
        // a double-click must respect the same flag, or a window deliberately
        // built at a fixed size gets maximized with no way to undo it but a
        // second double-click.
        if (now - m_last_title_click < 350) {
            m_last_title_click = 0;
            if (resizable) { toggle_maximize(); app.request_redraw(); }
            return true;
        }
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
    // Wheel events carry no position, so the target has to be polled -- and
    // widget rects are LOGICAL, while SDL_GetMouseState() answers in raw window
    // pixels.  See mouse_logical() in gui.h: at any scale != 1 the raw pixels
    // hit-test against the wrong widget (or none).
    int mx = 0, my = 0; mouse_logical(app, mx, my);
    SDL_Rect b = safe_body(*this);
    return content && pt_in(b, mx, my) ? content->on_wheel(app, dx, dy) : false;
}
bool Window::on_key(App& app, SDL_Keycode k) {
    TextOwnerWatch textWatch(app, m_text_owned);   // F2-style renames start here
    return content ? content->on_key(app, k) : false;
}
bool Window::on_key_up(App& app, SDL_Keycode k) {
    return content ? content->on_key_up(app, k) : false;
}
bool Window::on_undo(App& app, bool redo) {
    return content ? content->on_undo(app, redo) : false;
}
void Window::cancel_interaction(App& app) {
    m_moving=false; m_resizing=false; m_content=false;
    if(content)content->cancel_interaction(app);
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
    if (w->alwaysBack) {
        // Stays clustered at the back with any other background panes,
        // never in front of a real floating window -- see the field comment.
        size_t pos = 0;
        while (pos < windows.size() && windows[pos]->alwaysBack) ++pos;
        windows.insert(windows.begin() + (long)pos, w);
    } else {
        windows.push_back(w);
    }
    for (auto* x : windows) x->focused = (x == w);
}
void WindowManager::cancel_all_interactions(App& app)
{
    for(Window* w:windows) if(w) w->cancel_interaction(app);
    m_active=nullptr; m_drawer_open=false; m_drawer_drag=false;
    m_drawer_width=0; m_drawer_armed=-1;
}
void WindowManager::cancel_interaction(App& app){cancel_all_interactions(app);}
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
    {
        // The workspace is the whole world a floating window lives in, and
        // keep_window_reachable() deliberately lets one hang off its edge with
        // only a sliver left inside.  Bound the paint to it (intersected with
        // whatever ambient clip the shell already has) so a dragged window can
        // never draw over the menu bar / transport / dock outside it.
        ScopedClip clip(app.ren, ws);
        for (auto* w : windows) {
            w->workspace = SDL_Rect{ ws.x, ws.y, ws.w, ws.h - reserve };
            if (!w->visible) w->focused = false;
            if (w->maximized) w->rect = w->workspace;
            if (w->visible && !w->minimized) w->draw(app);
        }
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
    if (platform::mobile() && (m_drawer_open || m_drawer_drag)) {
        const int dw=m_drawer_drag ? m_drawer_width
                                   : std::min(360,std::max(260,app.w*2/5));
        SDL_Rect shade{dw,ws.y,std::max(0,app.w-dw),ws.h};
        fill_rect(app.ren,shade,Color{0,0,0,120});
        SDL_Rect drawer{0,ws.y,dw,ws.h};
        fill_rect(app.ren,drawer,t.panel); frame_rect(app.ren,drawer,t.accent);
        SDL_Rect head{8,ws.y+8,std::max(1,dw-16),44};
        app.font.draw_fitted(app.ren,head,"OPEN WINDOWS",t.accent,false);
        int y=head.y+head.h+6, index=0;
        for (auto it=windows.rbegin();it!=windows.rend();++it) {
            Window* win=*it;
            if (!win->visible) continue;
            SDL_Rect card{8,y,std::max(1,dw-16),54};
            fill_rect(app.ren,card,index==m_drawer_armed?t.sel:t.bg);
            frame_rect(app.ren,card,win->focused?t.accent:t.dim);
            app.font.draw_fitted(app.ren,SDL_Rect{card.x+12,card.y+6,card.w-24,card.h-12},
                                 win->title+(win->minimized?"  [hidden]":""),t.text,false);
            ++index; y+=60; if(y+54>ws.y+ws.h) break;
        }
        SDL_Rect grip{std::max(0,dw-5),ws.y,5,ws.h}; fill_rect(app.ren,grip,t.accent);
    }
}
bool WindowManager::on_mouse(App& app, const MouseEv& e) {
  if(platform::mobile()) {
    const int drawerW=std::min(360,std::max(260,app.w*2/5));
    if (e.pressed && !m_drawer_open && !m_drawer_drag && e.x<=24) {
        m_drawer_drag=true; m_drawer_start_x=e.x; m_drawer_width=1;
        m_drawer_armed=-1; app.request_redraw(); return true;
    }
    if (m_drawer_drag) {
        if (e.pressed) {
            m_drawer_width=std::clamp(e.x-m_drawer_start_x,1,drawerW);
        } else {
            m_drawer_open=m_drawer_width>=96;
            m_drawer_drag=false; m_drawer_width=m_drawer_open?drawerW:0;
        }
        app.request_redraw(); return true;
    }
    if (m_drawer_open) {
        if (!e.pressed) {
            int chosen=m_drawer_armed; m_drawer_armed=-1;
            if (chosen>=0) {
                int index=0;
                for (auto it=windows.rbegin();it!=windows.rend();++it) {
                    Window* win=*it; if(!win->visible) continue;
                    if(index++==chosen) {
                        win->minimized=false; raise(win); m_drawer_open=false; break;
                    }
                }
            }
            app.request_redraw(); return true;
        }
        if (e.x>=drawerW) {
            m_drawer_open=false; m_drawer_armed=-1; app.request_redraw(); return true;
        }
        const int firstY=workspace.y+58;
        int row=(e.y-firstY)/60;
        if(e.y>=firstY && row>=0) m_drawer_armed=row;
        return true;
    }
  }
    if (!e.pressed) {
        if (m_active) { m_active->on_mouse(app, e); m_active = nullptr; app.request_redraw(); return true; }
        return false;
    }
    if (m_active) { m_active->on_mouse(app, e); app.request_redraw(); return true; }

    // tray click -> restore + focus a minimized window
    std::vector<SDL_Rect> trects; std::vector<Window*> tray = taskbar_hits(app, trects);
    for (size_t i = 0; i < tray.size(); ++i)
        if (pt_in(trects[i], e.x, e.y)) {
            // `rect` is untouched while a window sits minimized, so it is
            // already the right place to come back to.  restore_rect belongs to
            // toggle_maximize() and is written NOWHERE on the minimise path:
            // consuming it here teleported a window back to wherever it was
            // before a maximize that may have been un-done long ago.  (A window
            // minimized while maximized keeps maximized = true and is re-fitted
            // to the workspace by draw(), so that case needs nothing either.)
            tray[i]->minimized = false;
            raise(tray[i]); app.request_redraw(); return true;
        }

    // topmost non-minimized hit window grabs the press (and becomes drag target)
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        Window* w = *it;
        if (w->visible && !w->minimized && w->hit(e.x, e.y)) {
            // Click-away: an inline edit belongs to the window whose content
            // started it, and clicking into a DIFFERENT window left it live.
            // The keyboard then stayed captured by a field the user had walked
            // away from (the BPM box, a mixer strip rename) -- Space no longer
            // played and the app read as frozen.
            if (app.editing_text())
                for (Window* other : windows)
                    if (other && other != w) other->commit_owned_text(app);
            raise(w); m_active=w; w->on_mouse(app,e);
            if(!w->visible||w->minimized)m_active=nullptr;
            app.request_redraw(); return true;
        }
    }
    // Missed every window: the press belongs to the workspace behind them, so
    // that is a click-away too.
    if (app.editing_text())
        for (Window* other : windows) if (other) other->commit_owned_text(app);
    return false;   // missed -> fall through to the workspace
}
bool WindowManager::on_wheel(App& app, int dx, int dy) {
    // Wheel events carry no position, so the target has to be polled -- and
    // widget rects are LOGICAL, while SDL_GetMouseState() answers in raw window
    // pixels.  See mouse_logical() in gui.h: at any scale != 1 the raw pixels
    // hit-test against the wrong widget (or none).
    int mx = 0, my = 0; mouse_logical(app, mx, my);
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
bool WindowManager::on_undo(App& app, bool redo) {
    // Same route keys take: the focused (front-most, visible, non-minimized)
    // window gets first refusal on Ctrl+Z/Y.  false => project-wide undo.
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if ((*it)->visible && !(*it)->minimized) return (*it)->on_undo(app, redo);
    return false;
}
bool WindowManager::on_key_up(App& app, SDL_Keycode k) {
    // A key may have gone down in an editor that was then covered, minimized or
    // closed. Broadcast releases so that editor can emit its matching MIDI-off.
    bool handled=false;
    for(auto it=windows.rbegin();it!=windows.rend();++it)
        if(*it&&(*it)->content) handled=(*it)->on_key_up(app,k)||handled;
    return handled;
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
    platform::prepare_sdl();
    ui_scale=platform::load_ui_scale();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr,"SDL_Init: %s\n", SDL_GetError()); return false; }
    if (TTF_Init() != 0) { fprintf(stderr,"TTF_Init: %s\n", TTF_GetError()); return false; }
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    const Uint32 windowFlags=platform::window_flags();
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w, h, windowFlags);
    if (!window) { fprintf(stderr,"CreateWindow: %s\n", SDL_GetError()); return false; }
#ifdef PATCHKNOB_SDL3
    // SDL3 scopes text input to a window, so the shim's no-argument
    // SDL_StartTextInput()/SDL_StopTextInput() need to know which one.  Without
    // this they silently did nothing and no SDL_EVENT_TEXT_INPUT ever arrived.
    pk_sdl3_set_main_window(window);
#endif
    // PATCHKNOB_NOVSYNC=1 is a measurement aid: with vsync on, the time inside
    // SDL_RenderPresent is mostly *waiting* for the vblank and tells you nothing
    // about how hard the GPU is working.  Turning it off uncaps the loop so the
    // real per-frame GPU cost becomes visible.  Not for normal use -- it tears.
    Uint32 renFlags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    if (SDL_getenv("PATCHKNOB_NOVSYNC")) renFlags = SDL_RENDERER_ACCELERATED;
    // BACKEND: left to SDL.  D3D11 returns from Present faster than SDL's
    // default D3D9 pick on this machine, but that is a CPU-side number and says
    // nothing about how busy the GPU is -- and on an old GCN part D3D11 can cost
    // MORE GPU while returning sooner.  Forcing it was not justified by anything
    // that measures the thing being complained about.  SDL_RENDER_DRIVER in the
    // environment still overrides, for when there is a real measurement to make.
    ren = SDL_CreateRenderer(window, -1, renFlags);
    if (!ren) ren = SDL_CreateRenderer(window, -1, 0);   // software fallback (e.g. headless)
    if (!ren) { fprintf(stderr,"CreateRenderer: %s\n", SDL_GetError()); return false; }
    {
        // SDL3 dropped SDL_RendererInfo (the flags it carried are no longer
        // meaningful -- every SDL3 renderer is accelerated and supports render
        // targets); the backend name is the part that was ever read here.
#ifdef PATCHKNOB_SDL3
        const char* rn = SDL_GetRendererName(ren);
        fprintf(stderr, "[gfx] renderer=%s\n", rn ? rn : "?");
#else
        SDL_RendererInfo ri;
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "[gfx] renderer=%s vsync=%d accel=%d target_tex=%d\n",
                    ri.name ? ri.name : "?",
                    (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? 1 : 0,
                    (ri.flags & SDL_RENDERER_ACCELERATED) ? 1 : 0,
                    (ri.flags & SDL_RENDERER_TARGETTEXTURE) ? 1 : 0);
#endif
    }

    platform::configure_display(ren,w,h,scale);
    // logical size scales rendering to the physical output AND maps mouse coords
    // into logical space automatically.
    SDL_RenderSetLogicalSize(ren, w, h);

    if (!font.load(ren, (int)std::lround(15.f*ui_scale), scale)) return false;
    mono.load(ren, (int)std::lround(13.f*ui_scale), scale);
    return true;
}

void App::set_ui_scale(float value) {
    if(platform::mobile())ui_scale=std::clamp(value,1.0f,1.75f);
    else ui_scale=1.f;
    font.load(ren,(int)std::lround(15.f*ui_scale),scale);
    mono.load(ren,(int)std::lround(13.f*ui_scale),scale);
    platform::save_ui_scale(ui_scale);
    dirty=true;
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
    // on_edit_begin()/on_edit_end() MUST be balanced.  The shell latches an open
    // gesture on begin and every later begin returns early while it is set, so a
    // single leaked begin -- a press whose release never arrives -- disables
    // undo for the whole session and leaks the pre-edit snapshot file.  Releases
    // do go missing: the mobile long-press path consumed its own mouse-up, and
    // losing window focus mid-drag or quitting mid-drag never released either.
    // Route every begin/end through these so the pairing lives in one place.
    bool editOpen = false;
    auto edit_begin = [&] { if (!editOpen) { editOpen = true; if (on_edit_begin) on_edit_begin(); } };
    auto edit_end   = [&] { if (editOpen) { editOpen = false; if (on_edit_end)   on_edit_end();   } };
    // Mobile-only gesture state. SDL's touch-to-mouse synthesis continues to
    // provide ordinary taps and drags; these augment it without changing PC.
    Uint32 holdStarted = 0;
    int holdX = 0, holdY = 0;
    bool holdPending = false, holdConsumed = false;
    float gestureX = 0.5f, gestureY = 0.5f;
    // This UI is mostly static DAW chrome with a one-pixel playhead/VU update.
    // Cap continuous playback redraw while keeping playhead, meters and live
    // recording previews aligned with modern displays.
    // TWO caps, deliberately.  `frameInterval` is a hard ceiling on ALL frames;
    // `animInterval` is a lower ceiling for frames nobody asked for.
    //
    // The hard ceiling is not redundant with vsync.  Vsync is a property of the
    // renderer we happen to get: the software fallback below has none, and a
    // driver or a remote session can ignore PRESENTVSYNC entirely.  Without a
    // cap of our own, any of those turns this loop into an unbounded spin that
    // renders hundreds of frames a second and pins the GPU -- the single most
    // common cause of a 2D app burning a GCN card.  Keep the cap even though
    // vsync normally makes it a no-op.
    constexpr Uint64 targetFrameRate = 60;
    // PATCHKNOB_ANIMHZ overrides the playback-animation ceiling.  It exists so
    // the 60-vs-30 trade can be measured on the machine that has to run it,
    // rather than argued about; 60 reproduces the old behaviour exactly.
    if (const char* hz = SDL_getenv("PATCHKNOB_ANIMHZ")) {
        const int v = SDL_atoi(hz);
        if (v >= 1 && v <= 240) anim_hz = v;
    }
    if (SDL_getenv("PATCHKNOB_DAMAGE")) damage_clipping = true;
    if (SDL_getenv("PATCHKNOB_TRACEREDRAW")) trace_redraw = true;
    Uint32 lastRedrawDump = 0;
    const Uint64 performanceFrequency = SDL_GetPerformanceFrequency();
    const Uint64 frameInterval = (performanceFrequency + targetFrameRate - 1) / targetFrameRate;
    Uint64 lastFrameStart = 0;
    bool drawable=true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // Window events first: SDL2 packs them all into one event type with a
            // sub-code, SDL3 gives each its own type, so neither switch shape
            // works for both.  pk_window_event() flattens the difference.
            if (const unsigned we = pk_window_event(ev)) {
                if (we & PK_WIN_SIZE) {
                    int pw,ph; SDL_GetRendererOutputSize(ren,&pw,&ph);
                    platform::resize_display(ren,w,h,scale);
                    SDL_RenderSetLogicalSize(ren, w, h); dirty = true;
                }
                if (we & PK_WIN_DEACTIVATE) {
                    for(Widget* root:roots)if(root)root->cancel_interaction(*this);
                    // The press that opened this gesture will never be released
                    // here -- the mouse-up goes to whoever took the focus.
                    edit_end();
                    captured=nullptr; holdPending=false; holdConsumed=false; dirty=true;
                    if (we & PK_WIN_UNDRAWABLE) drawable=false;
                }
                if (we & PK_WIN_ACTIVATE) { drawable=true;dirty=true;lastFrameStart=0; }
                if (we & PK_WIN_EXPOSED)  dirty = true;
                continue;
            }
            switch (ev.type) {
            case SDL_QUIT:
                for(Widget* root:roots)if(root)root->cancel_interaction(*this);
                edit_end();
                captured=nullptr; running=false; break;
            // Window events are classified before the switch (see below); this
            // label only exists so SDL2's single SDL_WINDOWEVENT does not fall
            // through to the default case.  The SDL3 build never reaches it.
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                MouseEv m { ev.button.x, ev.button.y, ev.button.button,
                            ev.type==SDL_MOUSEBUTTONDOWN };
                if (m.pressed) {
                    edit_begin();
                    captured = nullptr;
                    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->hit(m.x,m.y) && (*it)->on_mouse(*this, m)) {
                            captured = *it; break;
                        }
                    // Some Android SDL builds report the synthesized pointer as
                    // device 0 instead of SDL_TOUCH_MOUSEID.  On the mobile
                    // build every primary-button press is eligible for a hold;
                    // this also keeps an attached mouse usable.
                    if (platform::mobile()&&ev.button.button == SDL_BUTTON_LEFT) {
                        holdStarted = SDL_GetTicks();
                        holdX = m.x; holdY = m.y;
                        holdPending = captured != nullptr;
                        holdConsumed = false;
                    }
                } else {
                    if (platform::mobile()&&ev.button.button == SDL_BUTTON_LEFT) {
                        holdPending = false;
                        // The long-press below already delivered its own
                        // press/release pair and cleared `captured`; this stray
                        // real mouse-up is dropped -- but the gesture the
                        // original press opened still has to be closed, or the
                        // FIRST long press on Android silently disables undo for
                        // the rest of the session.
                        if (holdConsumed) { holdConsumed = false; captured = nullptr; edit_end(); break; }
                    }
                    // release -> the capturing root (if any), else topmost visible
                    if (captured) captured->on_mouse(*this, m);
                    else for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->on_mouse(*this, m)) break;
                    captured = nullptr;
                    edit_end();
                }
                break; }
            case SDL_MOUSEMOTION:
                if (ev.motion.state & SDL_BUTTON_LMASK) {
                    if (platform::mobile()&&(std::abs(ev.motion.x-holdX) > 14 ||
                        std::abs(ev.motion.y-holdY) > 14))
                        holdPending = false;
                    MouseEv m { ev.motion.x, ev.motion.y, SDL_BUTTON_LEFT, true };
                    // ONLY the root that claimed the press sees the drag.  With
                    // nothing captured the press was claimed by nobody, and
                    // re-hit-testing each motion delivered it as a synthetic
                    // PRESS -- indistinguishable, to a widget, from "the button
                    // went down here".  A drag begun over dead space therefore
                    // wrote any Fader it crossed (m_drag + changed()) and raised
                    // and clicked into any floating window it passed over.
                    if (captured) captured->on_mouse(*this, m);
                    else dirty = true;          // still repaint hover highlights
                } else dirty = true;
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
                if(platform::mobile()){gestureX = ev.tfinger.x; gestureY = ev.tfinger.y;}
                break;
#ifndef PATCHKNOB_SDL3
            // SDL3 removed multi-finger gesture events -- an app is expected to
            // derive pinch from raw touch points itself.  This block is
            // mobile-only, so it is compiled out of the SDL3 build until it is
            // rewritten rather than blocking the port on a platform this build
            // does not target.
            case SDL_MULTIGESTURE: {
                // Pinch zoom maps to the wheel API already implemented by the
                // arrange, piano, sample, Pd, Csound and rack views.
                const float mag = ev.mgesture.dDist;
                if (platform::mobile()&&std::fabs(mag) > 0.0015f) {
                    const int gx = (int)(gestureX * w), gy = (int)(gestureY * h);
                    const int step = mag > 0.f ? 1 : -1;
                    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                        if ((*it)->visible && (*it)->hit(gx,gy) &&
                            (*it)->on_wheel(*this, 0, step)) break;
                    dirty = true;
                }
                break; }
#endif
            case SDL_MOUSEWHEEL: {
                // Route the wheel to the window/view UNDER the pointer, so each
                // window's own zoom (arrange ticks/px, sample editor samples/px)
                // activates based on where you're hovering.
                // LOGICAL coords: root rects live in logical space and
                // SDL_GetMouseState() reports raw window pixels (gui.h:41).
                int wmx = 0, wmy = 0; mouse_logical(*this, wmx, wmy);
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->hit(wmx, wmy) &&
                        (*it)->on_wheel(*this, ev.wheel.x, ev.wheel.y)) break;
                break; }
            case SDL_TEXTINPUT: {
                edit_begin();
                // BOUNDED.  Nothing typed into a name / path / value field is
                // legitimately longer than this, and an unbounded += grows a
                // string that views measure and index into every frame -- an
                // autorepeating key or an IME burst had no ceiling at all.
                constexpr size_t kMaxTextField = 4096;
                if (text_target) {
                    const size_t add = std::strlen(ev.text.text);
                    if (text_target->size() + add <= kMaxTextField) {
                        *text_target += ev.text.text;
                        if (text_changed) text_changed();
                    }
                    dirty = true;
                }
                else if (text_input_sink) { text_input_sink(ev.text.text); dirty = true; }
                edit_end();
                break; }
            case SDL_KEYDOWN:
                // F11 before anything else: it must work even while a text field
                // has focus, and no widget has a legitimate claim on it.
                if (ev.key.PK_KEYSYM_SYM == SDLK_F11) { toggle_fullscreen(); break; }
                if ((ev.key.PK_KEYSYM_MOD & KMOD_CTRL) &&
                    (ev.key.PK_KEYSYM_SYM == SDLK_z || ev.key.PK_KEYSYM_SYM == SDLK_y)) {
                    const bool redo = (ev.key.PK_KEYSYM_SYM == SDLK_y) ||
                                      ((ev.key.PK_KEYSYM_MOD & KMOD_SHIFT) != 0);
                    // The FOCUSED view gets first refusal (Widget::on_undo, which
                    // only the key routes -- Window -> content, WindowManager ->
                    // focused window -- forward).  This used to fire the
                    // project-wide undo unconditionally, so the four editors that
                    // advertise Ctrl+Z in their own tooltips could never see it
                    // and pressing it rolled the whole PROJECT back instead.  A
                    // view that does not override on_undo returns false and
                    // Ctrl+Z still means project undo, exactly as before.
                    // While a single-field edit is live nothing view-local can
                    // sensibly claim the key, so that case keeps the old path.
                    bool consumed = false;
                    if (!text_target)
                        for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                            if ((*it)->visible && (*it)->on_undo(*this, redo)) { consumed = true; break; }
                    if (!consumed) {
                        if (redo) { if (on_global_redo) on_global_redo(); }
                        else if (on_global_undo) on_global_undo();
                    }
                    dirty = true;
                    break;
                }
                edit_begin();
                if (text_target) {              // editing a field: keys edit it, not widgets
                    SDL_Keycode k = ev.key.PK_KEYSYM_SYM;
                    SDL_Keymod mod = (SDL_Keymod)ev.key.PK_KEYSYM_MOD;
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
                    edit_end();
                    break;
                }
                // ESC does NOT quit the app; it is dispatched to views below so
                // it can clear selections / close palettes.  Quit is via the menu.
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->on_key(*this, ev.key.PK_KEYSYM_SYM)) break;
                edit_end();
                break;
            case SDL_KEYUP:
                // Gated exactly like SDL_KEYDOWN above: while a field is being
                // edited its keystrokes are not view input.  Only the keydown
                // was gated before, so view shortcuts bound to a key RELEASE
                // still fired for every character typed into a rename box.
                if (text_target) break;
                for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                    if ((*it)->visible && (*it)->on_key_up(*this, ev.key.PK_KEYSYM_SYM)) break;
                break;
            }
        }
        // A stationary hold is the mobile equivalent of a context click.  First
        // end the synthesized left press (it may have armed a node move/wire),
        // then offer the captured view a right-button press.  Views without a
        // context action return false, in which case retain the older hold-as-
        // double-click behaviour used by clips, browser rows and editor objects.
        if (platform::mobile()&&holdPending&&captured&&SDL_GetTicks()-holdStarted>=550) {
            MouseEv up{holdX,holdY,SDL_BUTTON_LEFT,false};
            MouseEv down{holdX,holdY,SDL_BUTTON_LEFT,true};
            captured->on_mouse(*this, up);
            MouseEv contextDown{holdX,holdY,SDL_BUTTON_RIGHT,true};
            const bool openedContext = captured->on_mouse(*this, contextDown);
            if (openedContext) {
                MouseEv contextUp{holdX,holdY,SDL_BUTTON_RIGHT,false};
                captured->on_mouse(*this, contextUp);
            } else {
                captured->on_mouse(*this, down); captured->on_mouse(*this, up);
                captured->on_mouse(*this, down); captured->on_mouse(*this, up);
            }
            holdPending = false; holdConsumed = true; captured = nullptr; dirty = true;
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
        if (trace_redraw) {
            const Uint32 t = SDL_GetTicks();
            if (t - lastRedrawDump >= 2000) { lastRedrawDump = t; dump_redraw_callers(); }
        }
        if(!drawable){SDL_Delay(25);continue;}
        // Benchmark mode: force continuous full repaints so every sample measures
        // the same thing.  Without this the loop drops to the 4 Hz idle refresh
        // whenever the transport is not rolling, and idle frames average in with
        // animating ones to produce numbers that compare nothing to nothing.
        // PATCHKNOB_FRAMESTATS=anim forces the continuous case; =1 reports what
        // the loop is ACTUALLY doing, idle refresh included.  Forcing on both
        // made the idle case unmeasurable -- and comparing a forced 30 Hz run
        // against a 4 Hz one as though they were the same workload is how you
        // conclude a frame costs eight times what it does.
        {
            static const char* fs = SDL_getenv("PATCHKNOB_FRAMESTATS");
            static const bool benchAnim = fs && std::strcmp(fs, "anim") == 0;
            if (benchAnim) animating = true;
        }
        if (dirty || animating) {
            // PACING.  Two separate concerns, previously conflated into one
            // 60 Hz cap:
            //
            //  * A frame with `dirty` set is a frame the user is waiting on (a
            //    click, a drag, a scroll).  Present it as soon as the display
            //    will take it -- vsync inside SDL_RenderPresent already paces
            //    it, and sleeping first only adds latency.  The old cap slept
            //    a whole millisecond (SDL_Delay granularity) when a frame was
            //    ready just under the interval, missed that vblank, and waited
            //    for the next: that is why the loop measured 57-58 fps against
            //    a 60 Hz display instead of a solid 60, with visible jitter.
            //
            //  * A frame that is ONLY `animating` is a frame nobody asked for:
            //    the playhead advanced. Repainting the whole window 60 times a
            //    second for that keeps the GPU at load continuously, which is
            //    the reported "uses a shit ton of GPU".  Those get their own,
            //    much slower cap -- a playhead updating at 30 Hz is smooth, and
            //    it halves the idle-playback GPU cost.
            const Uint64 now = SDL_GetPerformanceCounter();
            static const bool uncapped = SDL_getenv("PATCHKNOB_NOVSYNC") != nullptr;
            // A frame the user is waiting on gets the 60 Hz ceiling; a frame that
            // is only the playhead moving gets the 30 Hz one.
            // anim_hz is live (the A/V menu changes it), so derive its interval
            // per frame rather than once at startup.
            const Uint64 hz = (Uint64) (anim_hz < 1 ? 1 : (anim_hz > 240 ? 240 : anim_hz));
            const Uint64 animInterval = (performanceFrequency + hz - 1) / hz;
            const Uint64 cap = (!dirty && animating) ? animInterval : frameInterval;
            if (!uncapped && lastFrameStart != 0 && now - lastFrameStart < cap) {
                const Uint64 remaining = cap - (now - lastFrameStart);
                const Uint32 sleepMs = static_cast<Uint32>((remaining * 1000) / performanceFrequency);
                SDL_Delay(std::max<Uint32>(1,sleepMs));
                continue;
            }
            lastFrameStart = SDL_GetPerformanceCounter();
            // Layout is state/geometry work, not animation work. Playback used
            // to rebuild every window and child rectangle at 60 Hz merely to
            // move a one-pixel playhead. Preserve redraw requests made DURING
            // drawing by clearing the consumed dirty flag before callbacks.
            const bool layoutNeeded=dirty;
            dirty=false;
            // PATCHKNOB_FRAMESTATS=1 reports what a frame actually costs on the
            // CPU side (building and submitting the command queue), separately
            // from what SDL_RenderPresent then blocks on for vsync.  Sluggish
            // full-screen drawing shows up in `build`; a GPU that cannot keep up
            // shows up in `present`.  Zero cost when the variable is unset.
            static const bool frameStats = SDL_getenv("PATCHKNOB_FRAMESTATS") != nullptr;
            const Uint64 tBuild0 = frameStats ? SDL_GetPerformanceCounter() : 0;
            if (layoutNeeded&&on_layout) on_layout(*this);
            if (on_frame) on_frame(*this);

            // DAMAGE-CLIPPED ANIMATION.  On a frame nobody asked for -- the
            // playhead advanced, nothing else changed -- clip to the union of
            // what the previous frame said would move.  The draw code below is
            // unchanged and still runs in full (it is ~0.7 ms of CPU); the win
            // is that the GPU rasterises a narrow strip instead of the whole
            // window, which is the difference between a moving playhead costing
            // most of the card and costing almost nothing.
            //
            // A periodic full repaint is still forced a few times a second so
            // anything that animates WITHOUT registering damage (VU meters, the
            // CPU readout) cannot sit stale indefinitely.
            static Uint32 lastFullPaint = 0;
            const Uint32 nowMs = SDL_GetTicks();
            const bool animOnly = !layoutNeeded && animating;
            const bool forceFull = (nowMs - lastFullPaint) >= 250;
            // The render target only earns its cost when frames are actually
            // being clipped, so both are gated together: with damage clipping
            // off this is exactly the original straight-to-screen draw path.
            const bool haveTarget = damage_clipping && ensure_frame_target();
            if (haveTarget) SDL_SetRenderTarget(ren, frameTarget);
            bool clipped = false;
            if (haveTarget && animOnly && !forceFull && !lastDamage.empty()) {
                SDL_Rect u = lastDamage[0];
                for (size_t i = 1; i < lastDamage.size(); ++i)
                    SDL_UnionRect(&u, &lastDamage[i], &u);
                const SDL_Rect screen{0,0,w,h};
                SDL_Rect vis;
                if (SDL_IntersectRect(&u, &screen, &vis)) {
                    SDL_RenderSetClipRect(ren, &vis);
                    clipped = true;
                }
            }
            if (!clipped) lastFullPaint = nowMs;

            damage.clear();
            fill_rect(ren, SDL_Rect{0,0,w,h}, theme().bg);
            for (auto* rt : roots) if (rt->visible) rt->draw(*this);
            if (draw_extra) draw_extra(*this);
            if (clipped) SDL_RenderSetClipRect(ren, nullptr);
            lastDamage.swap(damage);
            if (haveTarget) {
                // Back to the screen and blit the completed frame.  The target
                // keeps its contents, so the areas this frame skipped are last
                // frame's pixels rather than a swapped-out buffer's.
                SDL_SetRenderTarget(ren, nullptr);
                SDL_RenderCopy(ren, frameTarget, nullptr, nullptr);
            }
            const Uint64 tBuild1 = frameStats ? SDL_GetPerformanceCounter() : 0;
            // SDL batches: the draw calls above only QUEUE commands, and the
            // queue is executed inside SDL_RenderPresent.  Flushing separately
            // splits "issue the GL work" from "swap the buffers", which is the
            // difference between being draw-call bound and being vsync bound.
            if (frameStats) SDL_RenderFlush(ren);
            const Uint64 tFlush = frameStats ? SDL_GetPerformanceCounter() : 0;
            SDL_RenderPresent(ren);
            if (frameStats) {
                const Uint64 tEnd = SDL_GetPerformanceCounter();
                static std::vector<float> build, flush, present;
                static Uint64 lastReport = 0;
                const double toMs = 1000.0 / (double) performanceFrequency;
                build.push_back(  (float)((double)(tBuild1 - tBuild0) * toMs) );
                flush.push_back(  (float)((double)(tFlush  - tBuild1) * toMs) );
                present.push_back((float)((double)(tEnd    - tFlush ) * toMs) );
                if (lastReport == 0) lastReport = tEnd;
                if (tEnd - lastReport > performanceFrequency * 2) {
                    auto stat = [](std::vector<float> v, float& mean, float& p95) {
                        std::sort(v.begin(), v.end());
                        double s = 0.0; for (float f : v) s += f;
                        mean = v.empty() ? 0.f : (float)(s / v.size());
                        p95  = v.empty() ? 0.f : v[(size_t)((v.size() - 1) * 0.95)];
                    };
                    float bm, bp, fm, fp, pm, pp;
                    stat(build, bm, bp); stat(flush, fm, fp); stat(present, pm, pp);
                    fprintf(stderr,
                        "[frame] %dx%d  %zu frames/2s (%.1f fps)  queue %.2f ms (p95 %.2f)"
                        "  flush %.2f ms (p95 %.2f)  swap %.2f ms (p95 %.2f)\n",
                        w, h, build.size(), build.size() / 2.0,
                        bm, bp, fm, fp, pm, pp);
                    fflush(stderr);
                    build.clear(); flush.clear(); present.clear(); lastReport = tEnd;
                }
            }
        } else {
            SDL_Delay(8);   // idle: don't spin
            // Idle refresh.  ~4 Hz is enough for a CPU readout, and far too
            // slow for a meter: at 20 dB/s falloff each frame moves the bar
            // ~4.8 dB, about a tenth of its height, four times a second.  That
            // staircase IS the reported chop.  While the engine is audible,
            // refresh at ~31 Hz (4 x 8 ms) instead -- Ardour's ballistics, which
            // sdlui/meter.cpp ports, are written against a 40 ms GUI tick.
            static int idleTicks = 0;
            const bool hot = meters_hot && meters_hot();
            if (++idleTicks >= (hot ? 4 : 30)) { idleTicks = 0; dirty = true; }
        }
    }
}

// EXCLUSIVE fullscreen, not borderless-desktop.  The distinction is the whole
// point: borderless still hands every frame to the desktop compositor, which is
// the extra composition step that costs a windowed app real GPU time on older
// Windows/AMD stacks.  Taking a real display MODE gives the window the scanout
// buffer and takes DWM out of the present path.
//
// Both APIs are spelled here because the tree builds against SDL2 and SDL3:
// SDL2 sets a display mode then a fullscreen FLAG; SDL3 sets a fullscreen MODE
// then a boolean, and passing a null mode there means borderless -- which is
// exactly what we are trying not to get.
void App::toggle_fullscreen() {
    if (!window) return;
    fullscreen = !fullscreen;
    // Wayland has no client-facing equivalent of "take the scanout buffer" --
    // every fullscreen window is composited (a compositor MAY still give an
    // exactly-output-sized, undecorated surface a hardware-plane fast path,
    // but only for a plain borderless-fullscreen request). Explicitly setting
    // a display MODE the way the exclusive-fullscreen path below does is an
    // X11/Windows concept SDL's Wayland backend cannot honour the same way;
    // asking for one anyway risks a buffer-scale mismatch that disables that
    // fast path and forces full composition every frame for as long as
    // fullscreen is active -- exactly the "high GPU at fullscreen, worse
    // while playing" symptom this was reported as. Skip the mode-setting
    // call there and just go borderless, which is what Wayland fullscreen
    // always effectively is anyway.
    const char* driver = SDL_GetCurrentVideoDriver();
    const bool wayland = driver && std::strcmp(driver, "wayland") == 0;
#ifdef PATCHKNOB_SDL3
    if (fullscreen) {
        if (!wayland) {
            const SDL_DisplayID disp = SDL_GetDisplayForWindow(window);
            // Closest mode to the current desktop resolution: switching
            // resolution as a side effect of going fullscreen would be a
            // nasty surprise.
            const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(disp);
            SDL_DisplayMode want;
            if (desk && SDL_GetClosestFullscreenDisplayMode(
                            disp, desk->w, desk->h, desk->refresh_rate, false, &want))
                SDL_SetWindowFullscreenMode(window, &want);
        }
        SDL_SetWindowFullscreen(window, true);
    } else {
        SDL_SetWindowFullscreen(window, false);
    }
#else
    if (fullscreen) {
        if (!wayland) {
            SDL_DisplayMode desk;
            if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &desk) == 0)
                SDL_SetWindowDisplayMode(window, &desk);
        }
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
    } else {
        SDL_SetWindowFullscreen(window, 0);
    }
#endif
    // The size-changed event that follows re-derives the logical size; force a
    // repaint regardless so the first fullscreen frame is not the stale one.
    dirty = true;
    lastDamage.clear();          // damage rects refer to the OLD geometry
}

// A narrower, single-widget cousin of run(): pumps SDL events and redraws
// `dlg` full-window each frame, until it hides itself (dlg.visible = false)
// or the app quits. `running` is untouched on the way out (only SDL_QUIT
// clears it) so the OUTER run() loop resumes exactly where it left off --
// this is not a nested App::run(), just its event pump narrowed to one
// widget, reentering the same SDL_Window/SDL_Renderer (there is no second OS
// window to host a "real" modal under KMSDRM).
void App::run_modal(Widget& dlg) {
    dlg.visible = true;
    // SAVE/RESTORE, don't just turn it on and off.  Text input is global SDL
    // state that a view may already have armed for itself (the Csound / tracker
    // code editors keep their own latch and only re-arm when they believe it is
    // off).  Calling SDL_StopTextInput() on the way out of a dialog opened while
    // one of those was focused left the editor deaf until it was unfocused and
    // refocused again -- the "sometimes won't let you type" symptom.
#ifdef PATCHKNOB_SDL3
    const bool textWasActive = pk_sdl3_main_window && SDL_TextInputActive(pk_sdl3_main_window);
#else
    const bool textWasActive = SDL_IsTextInputActive() == SDL_TRUE;
#endif
    SDL_StartTextInput();
    const Uint64 performanceFrequency = SDL_GetPerformanceFrequency();
    const Uint64 frameInterval = (performanceFrequency + 59) / 60;
    Uint64 lastFrameStart = 0;
    while (running && dlg.visible) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (const unsigned we = pk_window_event(ev)) {
                if (we & PK_WIN_SIZE) {
                    platform::resize_display(ren, w, h, scale);
                    SDL_RenderSetLogicalSize(ren, w, h);
                }
                dirty = true;
                continue;
            }
            switch (ev.type) {
            case SDL_QUIT:
                dlg.cancel_interaction(*this);
                running = false; dlg.visible = false; break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
MouseEv m{ (int)ev.button.x, (int)ev.button.y, ev.button.button,
                           ev.type == SDL_MOUSEBUTTONDOWN };
                dlg.on_mouse(*this, m);
                break; }
            case SDL_MOUSEMOTION: {
                MouseEv m{ (int)ev.motion.x, (int)ev.motion.y, SDL_BUTTON_LEFT,
                          (ev.motion.state & SDL_BUTTON_LMASK) != 0 };
                dlg.on_mouse(*this, m);
                break; }
            case SDL_MOUSEWHEEL:
                dlg.on_wheel(*this, ev.wheel.x, ev.wheel.y);
                break;
            case SDL_TEXTINPUT:
                dlg.on_text(*this, ev.text.text);
                break;
            case SDL_KEYDOWN:
                dlg.on_key(*this, ev.key.PK_KEYSYM_SYM);
                break;
            case SDL_KEYUP:
                dlg.on_key_up(*this, ev.key.PK_KEYSYM_SYM);
                break;
            default: break;
            }
            dirty = true;
        }
        if (!running) break;
        const Uint64 now = SDL_GetPerformanceCounter();
        if (lastFrameStart != 0 && now - lastFrameStart < frameInterval) {
            const Uint64 remaining = frameInterval - (now - lastFrameStart);
            SDL_Delay((Uint32)(remaining * 1000 / performanceFrequency));
        }
        lastFrameStart = SDL_GetPerformanceCounter();
        set_color(ren, theme().bg);
        SDL_RenderClear(ren);
        // Redraw the frozen app underneath for context. Safe without a fresh
        // on_layout/on_frame pass: every root's `rect` is still whatever the
        // outer run() loop last computed, and animation just holds still for
        // the (brief, user-driven) life of the dialog.
        for (Widget* root : roots) if (root && root->visible) root->draw(*this);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        set_color(ren, Color{0,0,0,120});
        SDL_Rect full{0,0,w,h};
        SDL_RenderFillRect(ren, &full);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        if (dlg.visible) dlg.draw(*this);
        SDL_RenderPresent(ren);
        dirty = false;
    }
    if (textWasActive) SDL_StartTextInput(); else SDL_StopTextInput();
    dirty = true;
}

// Sized to the LOGICAL size, so drawing into it is 1:1 and no coordinate in any
// view has to change.  Recreated when that size changes (resize, fullscreen).
// ---- redraw-caller tracing -------------------------------------------------
bool App::trace_redraw = false;
namespace {
std::vector<std::pair<void*, long>> g_redrawCallers;   // few distinct sites
}
void App::note_redraw(void* ra) {
    for (auto& e : g_redrawCallers) if (e.first == ra) { ++e.second; return; }
    if (g_redrawCallers.size() < 256) g_redrawCallers.emplace_back(ra, 1);
}
// Raw runtime addresses, deliberately: attach gdb to the LIVE process and
// `info symbol 0x...` resolves them with no image-base arithmetic.
void App::dump_redraw_callers() {
    if (g_redrawCallers.empty()) return;
    std::sort(g_redrawCallers.begin(), g_redrawCallers.end(),
              [](const std::pair<void*,long>& a, const std::pair<void*,long>& b) {
                  return a.second > b.second; });
    fprintf(stderr, "[redraw] top callers over the last window:\n");
    for (size_t i = 0; i < g_redrawCallers.size() && i < 6; ++i)
        fprintf(stderr, "[redraw]   %6ld  %p\n",
                g_redrawCallers[i].second, g_redrawCallers[i].first);
    fflush(stderr);
    g_redrawCallers.clear();
}

bool App::ensure_frame_target() {
    if (!ren || w <= 0 || h <= 0) return false;
    if (frameTarget && frameTargetW == w && frameTargetH == h) return true;
    if (frameTarget) { SDL_DestroyTexture(frameTarget); frameTarget = nullptr; }
    frameTarget = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                    SDL_TEXTUREACCESS_TARGET, w, h);
    if (!frameTarget) { frameTargetW = frameTargetH = 0; return false; }
    // Opaque blit: the target holds a complete frame, so blending it over the
    // previous screen contents would only cost fill rate and risk ghosting.
    SDL_SetTextureBlendMode(frameTarget, SDL_BLENDMODE_NONE);
    frameTargetW = w; frameTargetH = h;
    lastDamage.clear();          // nothing valid in a brand-new target yet
    return true;
}

void App::set_vsync(bool on) {
    vsync_on = on;
    if (!ren) return;
#ifdef PATCHKNOB_SDL3
    SDL_SetRenderVSync(ren, on ? 1 : 0);
#else
    // SDL2 gained a runtime toggle in 2.0.18; older headers only have the
    // creation flag, in which case the setting takes effect on next start.
#if SDL_VERSION_ATLEAST(2,0,18)
    SDL_RenderSetVSync(ren, on ? 1 : 0);
#endif
#endif
    dirty = true;
}

std::string App::video_backend() const {
    if (!ren) return "none";
#ifdef PATCHKNOB_SDL3
    const char* n = SDL_GetRendererName(ren);
    return n ? n : "?";
#else
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(ren, &ri) == 0 && ri.name) return ri.name;
    return "?";
#endif
}

std::string App::sdl_version() const {
    char buf[48];
#ifdef PATCHKNOB_SDL3
    const int v = SDL_GetVersion();
    std::snprintf(buf, sizeof(buf), "SDL %d.%d.%d",
                  SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                  SDL_VERSIONNUM_MICRO(v));
#else
    SDL_version v; SDL_GetVersion(&v);
    std::snprintf(buf, sizeof(buf), "SDL %d.%d.%d", v.major, v.minor, v.patch);
#endif
    return buf;
}

void App::shutdown() {
    if (frameTarget) { SDL_DestroyTexture(frameTarget); frameTarget = nullptr; }
    // The fonts own atlas textures created from `ren`.  `App` is a stack local
    // in main(), so ~Font() runs LONG after this function -- after
    // SDL_DestroyRenderer() has already freed those textures and after
    // SDL_Quit().  SDL_DestroyTexture() dereferences texture->renderer, so that
    // ordering crashed on every one of the ~14 exit paths.  Free them here,
    // while the renderer is still alive; ~Font() then has nothing left to do.
    font.release();
    mono.release();
    // Same ordering problem, same fix: the meter gradient cache holds textures
    // made from `ren` and is keyed on the renderer pointer.  A new renderer
    // landing on the freed address of this one would be indistinguishable from
    // the same renderer, so drop the table while `ren` is still valid.
    ui::meter::flush_cache();
    if (ren) { SDL_DestroyRenderer(ren); ren = nullptr; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }
    TTF_Quit();
    SDL_Quit();
}

} // namespace ui
