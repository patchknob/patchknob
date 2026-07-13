//----------------------------------------------------------------------------
//  sdlui/gui.h  -- lean, cross-platform, grayscale/green widget toolkit on SDL2.
//
//  Design goals (matches the appliance target: Windows + Linux + KMSDRM):
//    * SDL_Renderer (GPU-batched) drawing, retained widgets, dirty-rect redraw
//      so the GPU idles when nothing changes.
//    * Strictly two-tone themes: LIGHT (white bg / black-grey) and MIDNIGHT
//      (black bg / phosphor green).  No other hues.
//    * Text via a one-time monospace glyph ATLAS (SDL2_ttf renders it once;
//      draw_text() then blits atlas cells -- no per-frame TTF work).
//
//  This is the frontend shell.  The DAW engine (audio/VST/patch graph/etc.) is
//  UI-agnostic and linked unchanged.
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_GUI_H
#define SEQ24_SDLUI_GUI_H

#include <SDL.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace ui {

// ---- color -----------------------------------------------------------------
struct Color { Uint8 r, g, b, a; };
inline Color rgb(Uint32 x) { return { Uint8(x>>16), Uint8(x>>8), Uint8(x), 255 }; }

// The named theme roles (mirror the GTK palette names so porting is 1:1).
struct Theme {
    Color bg, panel, accent, sel, active, hi, dim, text, keybg, note, notesel, scale, chord;
};
enum class Mode { Light = 0, Midnight = 1 };
void   set_mode(Mode m);
Mode   mode();
const  Theme& theme();

// ---- low-level draw (SDL_Renderer) -----------------------------------------
void set_color(SDL_Renderer* r, Color c);
void fill_rect(SDL_Renderer* r, const SDL_Rect& q, Color c);
void frame_rect(SDL_Renderer* r, const SDL_Rect& q, Color c);
void hline(SDL_Renderer* r, int x0, int x1, int y, Color c);
void vline(SDL_Renderer* r, int x, int y0, int y1, Color c);

// ---- monospace glyph atlas (one texture, built once) -----------------------
class Font {
public:
    // pt is LOGICAL; the atlas is rasterized at pt*scale physical px so text is
    // crisp under SDL_RenderSetScale(scale).  cw()/ch() are LOGICAL units.
    bool  load(SDL_Renderer* r, int pt, float scale = 1.0f);
    int   cw() const { return m_cw; }             // logical cell (advance) width
    int   ch() const { return m_ch; }             // logical cell height
    int   text_w(const std::string& s) const { return int(s.size()) * m_cw; }
    void  draw(SDL_Renderer* r, int x, int y, const std::string& s, Color c) const;
    void  draw_centered(SDL_Renderer* r, const SDL_Rect& box, const std::string& s, Color c) const;
private:
    SDL_Texture* m_atlas = nullptr;
    int m_cw = 8, m_ch = 14;            // logical cell (for layout + dst rects)
    int m_cwP = 8, m_chP = 14;          // physical atlas cell (src rects)
    int m_first = 32, m_last = 126;
};

// ---- widget base -----------------------------------------------------------
struct MouseEv { int x, y, button; bool pressed; };
struct App;   // fwd

class Widget {
public:
    virtual ~Widget() {}
    SDL_Rect rect { 0,0,0,0 };
    bool visible = true;
    virtual void draw(App& app) = 0;
    virtual bool on_mouse(App& app, const MouseEv& e) { (void)app;(void)e; return false; }
    virtual bool on_wheel(App& app, int dx, int dy) { (void)app;(void)dx;(void)dy; return false; }
    virtual bool on_key(App& app, SDL_Keycode k) { (void)app;(void)k; return false; }
    bool hit(int px, int py) const {
        return px>=rect.x && px<rect.x+rect.w && py>=rect.y && py<rect.y+rect.h;
    }
};

// A container that lays out / dispatches to children (non-owning by default).
class Panel : public Widget {
public:
    std::vector<Widget*> children;
    Color* bg = nullptr;            // null => theme().panel
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
};

class Label : public Widget {
public:
    std::string text;
    void draw(App& app) override;
};

class Button : public Widget {
public:
    std::string text;
    bool toggle = false, on = false, down = false;
    std::function<void()> clicked;
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
};

// Vertical fader (mixer / value control), value 0..1.
class Fader : public Widget {
public:
    float value = 0.75f;
    std::function<void(float)> changed;
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool m_drag = false;
};

// ---- application shell ------------------------------------------------------
struct App {
    SDL_Window*   window = nullptr;
    SDL_Renderer* ren = nullptr;
    Font          font;      // UI font
    Font          mono;      // smaller monospace for grids
    int           w = 1280, h = 800;   // LOGICAL size (physical / scale)
    float         scale = 1.0f;         // HiDPI / high-res render scale
    bool          running = true;
    bool          dirty = true;                  // redraw requested
    bool          animating = false;             // continuous redraw (playback)
    std::vector<Widget*> roots;                  // top-level widgets
    std::function<void(App&)> on_layout;         // called before each redraw

    bool init(const char* title);
    void request_redraw() { dirty = true; }
    void run(std::function<void(App&)> draw_extra = nullptr);
    void shutdown();
};

} // namespace ui
#endif
