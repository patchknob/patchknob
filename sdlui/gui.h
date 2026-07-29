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
#ifndef PATCHKNOB_SDLUI_GUI_H
#define PATCHKNOB_SDLUI_GUI_H

#include <SDL.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace ui {

// ---- color -----------------------------------------------------------------
struct Color { Uint8 r, g, b, a; };
inline Color rgb(Uint32 x) { return { Uint8(x>>16), Uint8(x>>8), Uint8(x), 255 }; }

// The named theme roles shared by the SDL widgets.
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

// ---- monospace glyph atlas -------------------------------------------------
class Font {
public:
    Font() = default;
    // pt is LOGICAL; the atlas is rasterized at pt*scale physical px so text is
    // crisp under SDL_RenderSetScale(scale).  cw()/ch() are LOGICAL units.
    bool  load(SDL_Renderer* r, int pt, float scale = 1.0f);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    int   cw() const { return m_cw; }             // logical cell (advance) width
    int   ch() const { return m_ch; }             // logical cell height
    int   text_w(const std::string& s) const { return int(s.size()) * m_cw; }
    int   text_w_scaled(const std::string& s, float scale) const;
    int   text_h_scaled(float scale) const;
    void  draw(SDL_Renderer* r, int x, int y, const std::string& s, Color c) const;
    void  draw_scaled(SDL_Renderer* r, int x, int y, const std::string& s, Color c,
                      float scale) const;
    void  draw_fitted(SDL_Renderer* r, const SDL_Rect& box, const std::string& s,
                      Color c, bool center = false, float maxScale = 1.0f,
                      float minScale = 0.25f, bool ellipsis = true) const;
    void  draw_centered(SDL_Renderer* r, const SDL_Rect& box, const std::string& s, Color c) const;
private:
    struct ZoomAtlas {
        SDL_Texture* texture = nullptr;
        int point = 0, cw = 0, ch = 0, cwP = 0, chP = 0;
    };
    bool build_atlas(SDL_Renderer* r, int logicalPoint, float deviceScale,
                     SDL_Texture*& texture, int& cw, int& ch, int& cwP, int& chP) const;
    const ZoomAtlas* zoom_atlas(SDL_Renderer* r, float scale) const;
    void clear_zoom_atlases();
    SDL_Texture* m_atlas = nullptr;
    int m_cw = 8, m_ch = 14;            // logical cell (for layout + dst rects)
    int m_cwP = 8, m_chP = 14;          // physical atlas cell (src rects)
    int m_first = 32, m_last = 126;
    int m_logical_point = 0;
    float m_device_scale = 1.f;
    std::string m_face;
    mutable std::vector<ZoomAtlas> m_zoom_atlases;
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
    virtual bool on_key_up(App& app, SDL_Keycode k) { (void)app;(void)k; return false; }  // key RELEASE
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

// ---- menu bar (File / View / Audio ... dropdowns) --------------------------
struct MenuItem {
    std::string           label;
    std::function<void()> action;
    bool                  separator = false;
    bool                  enabled   = true;
    bool                  check     = false;   // draw a check mark when checked()
    std::function<bool()> checked;
};
struct Menu {
    std::string           title;
    std::vector<MenuItem> items;
    int _x = 0, _w = 0;                         // computed each draw
};
// Full-window-width bar: paints only the title strip + the open dropdown, so it
// can sit as the LAST root (draws on top, gets mouse first) yet lets clicks fall
// through to the view below when nothing menu-related was hit.
class MenuBar : public Widget {
public:
    std::vector<Menu> menus;
    int  bar_h = 24;
    int  open  = -1;                            // open menu index, -1 = closed
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
private:
    int m_ddx=0, m_ddy=0, m_ddw=0, m_ddh=0;     // last dropdown rect (hit-test)
};

// ---- floating windows (MDI-style) ------------------------------------------
// A draggable / resizable frame with a title bar + close box that hosts ONE
// content Widget (non-owning).  Managed by a WindowManager which handles
// z-order + focus.  Two-tone; the focused window's title bar uses the accent.
class Window : public Widget {
public:
    std::string           title;
    Widget*               content   = nullptr;   // hosted view (non-owning)
    bool                  closable  = true;
    bool                  resizable = true;
    bool                  minimizable = true;
    bool                  focused   = false;
    bool                  minimized = false;      // collapsed to the taskbar
    bool                  maximized = false;      // filling the workspace
    int                   title_h   = 22;
    std::function<void()> on_close;               // fired when the close box is hit

    SDL_Rect              workspace{0,0,0,0};      // max bounds (set by the manager)
    SDL_Rect              restore_rect{0,0,0,0};   // rect to return to from max/min

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;   // -> content
    bool on_key_up(App& app, SDL_Keycode k) override;// -> content
    SDL_Rect body() const {                       // content rect (inside the frame)
        return SDL_Rect{ rect.x+1, rect.y+title_h, rect.w-2, rect.h-title_h-1 };
    }
    void toggle_maximize();
private:
    bool m_moving = false, m_resizing = false, m_content = false;
    int  m_dx = 0, m_dy = 0;
    unsigned m_last_title_click = 0;              // for double-click maximize
};

// A full in-app window manager (for framebuffer / KMSDRM where there is no OS WM):
// move / resize / focus + z-order / minimize / maximize+restore / close, and a
// taskbar listing every window (click to focus or un-minimize).  Mount as a root.
class WindowManager : public Widget {
public:
    std::vector<Window*> windows;                 // back (index 0) -> front
    SDL_Rect             workspace{0,0,0,0};       // area windows live in (set by shell)
    int                  taskbar_h = 22;
    void add(Window* w);
    void remove(Window* w);
    void raise(Window* w);
    bool any_visible() const;
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;   // -> focused window's content
    bool on_key_up(App& app, SDL_Keycode k) override;// -> focused window's content
private:
    Window* m_active = nullptr;                    // window receiving an active drag
    std::vector<Window*> taskbar_hits(App& app, std::vector<SDL_Rect>& rectsOut) const;
};

// Vertical fader (mixer / value control), value 0..1.  A live level METER is
// drawn as the backdrop behind the handle: `level` returns the current peak
// amplitude (0..~1+, 1.0 == 0 dBFS) and the fader paints it on a dB scale with a
// decaying peak-hold line and dB tick marks.  Leave `level` null for a plain fader.
class Fader : public Widget {
public:
    float value = 0.75f;
    std::function<void(float)> changed;
    std::function<float()>     level;      // live peak amplitude for the meter backdrop
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool  m_drag = false;
    float m_peak = 0.f;                     // decaying peak-hold (amplitude)
};

// ---- pan-canvas scroll bars (modular / Pd editors) -------------------------
// Draggable HORIZONTAL + VERTICAL scroll bars for an infinite pan canvas.  The
// host passes its widget rect, the content bounding box in CANVAS PIXELS (world
// coords already multiplied by zoom, BEFORE the pan offset) and the pan (ox,oy)
// -- where a content pixel draws at screen = rect + (ox,oy) + canvasPx.  draw()
// overlays the bars in a `sb`-px strip along the bottom + right edges;
// on_mouse() drags the thumbs (and click-jumps on the track), writing ox/oy.
// Stateful across events; each editor owns its own instance (independent scroll).
struct CanvasScroll {
    int  sb   = 12;      // bar thickness (px)
    int  drag = 0;       // 0 none / 1 horizontal / 2 vertical
    int  grab = 0;       // pointer offset within the thumb at grab-time
    void draw(App& app, const SDL_Rect& rect,
              int cl, int cr, int ct, int cb, int ox, int oy);
    bool on_mouse(const SDL_Rect& rect, const MouseEv& e, bool downEdge,
                  int cl, int cr, int ct, int cb, int& ox, int& oy);
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
    std::function<void(App&)> on_tick;           // ~4x/s housekeeping (device-loss
                                                 // polls etc); fires while idle AND
                                                 // while animating

    // --- text input (SDL_TEXTINPUT) --------------------------------------
    // While a target is set, typing edits *text_target; Enter commits, Esc
    // cancels (text_commit(bool)); keys are NOT dispatched to widgets.
    std::string*              text_target = nullptr;
    std::function<void()>     text_changed;
    std::function<void(bool)> text_commit;
    void begin_text(std::string* t, std::function<void()> onChange = nullptr,
                    std::function<void(bool)> onCommit = nullptr);
    void end_text();
    bool editing_text() const { return text_target != nullptr; }

    // General character sink for widgets that manage their own multi-line editing
    // (e.g. the Csound CSD editor): when no single-field text_target is active,
    // SDL_TEXTINPUT characters are delivered here.  Special keys (arrows, enter,
    // backspace, Ctrl+E, ...) still arrive via the widget's on_key.  Set it when
    // the editor is focused; clear it (nullptr) when it loses focus / closes.
    std::function<void(const char*)> text_input_sink;

    bool init(const char* title);
    void request_redraw() { dirty = true; }
    void run(std::function<void(App&)> draw_extra = nullptr);
    void shutdown();
};

} // namespace ui
#endif
