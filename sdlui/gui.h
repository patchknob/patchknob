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

// ---- pointer position ------------------------------------------------------
struct App;   // fwd (defined at the bottom of this header)
// Current pointer in LOGICAL coordinates -- the space widget `rect`s and the
// MouseEv x/y live in.  SDL routes mouse EVENTS through the renderer's logical
// size, but SDL_GetMouseState() reports raw window pixels, so polling it
// directly (which every hover highlight has to do, because motion events are
// only delivered while a button is held) puts the highlight on the wrong row
// as soon as the display scale is not 1.  Always use this instead.
void mouse_logical(App& app, int& mx, int& my);

#ifndef PATCHKNOB_SDL3
// SDL_Vertex carries a byte colour in SDL2 and a normalised float colour in
// SDL3.  The glyph-atlas geometry builder in gui.cpp compiles under both, so it
// goes through this; the SDL3 build gets its version from sdl3_compat.h.
static inline SDL_Color pk_vertex_color(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return SDL_Color{ r, g, b, a };
}
// SDL3 flattened SDL_Keysym into SDL_KeyboardEvent; key-event member accesses
// are spelled through these so one source compiles against both.
#define PK_KEYSYM_SYM      keysym.sym
#define PK_KEYSYM_MOD      keysym.mod
#define PK_KEYSYM_SCANCODE keysym.scancode

// Window-event classification -- see the SDL3 half in sdl3_compat.h for why the
// event loop goes through this rather than switching on the event directly.
enum {
    PK_WIN_NONE       = 0,
    PK_WIN_SIZE       = 1u << 0,
    PK_WIN_DEACTIVATE = 1u << 1,
    PK_WIN_UNDRAWABLE = 1u << 2,
    PK_WIN_ACTIVATE   = 1u << 3,
    PK_WIN_EXPOSED    = 1u << 4
};
static inline unsigned pk_window_event(const SDL_Event& ev) {
    if (ev.type != SDL_WINDOWEVENT) return PK_WIN_NONE;
    switch (ev.window.event) {
    case SDL_WINDOWEVENT_SIZE_CHANGED: return PK_WIN_SIZE;
    case SDL_WINDOWEVENT_FOCUS_LOST:   return PK_WIN_DEACTIVATE;
    case SDL_WINDOWEVENT_HIDDEN:
    case SDL_WINDOWEVENT_MINIMIZED:    return PK_WIN_DEACTIVATE | PK_WIN_UNDRAWABLE;
    case SDL_WINDOWEVENT_SHOWN:
    case SDL_WINDOWEVENT_RESTORED:
    case SDL_WINDOWEVENT_MAXIMIZED:    return PK_WIN_ACTIVATE;
    case SDL_WINDOWEVENT_EXPOSED:      return PK_WIN_EXPOSED;
    default:                           return PK_WIN_NONE;
    }
}
#endif

// ---- low-level draw (SDL_Renderer) -----------------------------------------
void set_color(SDL_Renderer* r, Color c);
void fill_rect(SDL_Renderer* r, const SDL_Rect& q, Color c);
void frame_rect(SDL_Renderer* r, const SDL_Rect& q, Color c);
void hline(SDL_Renderer* r, int x0, int x1, int y, Color c);
void vline(SDL_Renderer* r, int x, int y0, int y1, Color c);

// Intersects a child clip with its ambient window clip and restores the exact
// previous SDL state on scope exit. Views must never reset a parent's clip.
class ScopedClip {
public:
    ScopedClip(SDL_Renderer* renderer,const SDL_Rect& requested):r(renderer) {
        if(!r)return;
        had=SDL_RenderIsClipEnabled(r)==SDL_TRUE;
        if(had)SDL_RenderGetClipRect(r,&old);
        SDL_Rect use=requested;
        if(had&&!SDL_IntersectRect(&old,&requested,&use))use=SDL_Rect{0,0,0,0};
        SDL_RenderSetClipRect(r,&use);
    }
    ~ScopedClip(){if(r)SDL_RenderSetClipRect(r,had?&old:nullptr);}
    ScopedClip(const ScopedClip&)=delete;ScopedClip& operator=(const ScopedClip&)=delete;
private: SDL_Renderer* r=nullptr;SDL_Rect old{0,0,0,0};bool had=false;
};

// ---- monospace glyph atlas -------------------------------------------------
class Font {
public:
    Font() = default;
    // pt is LOGICAL; the atlas is rasterized at pt*scale physical px so text is
    // crisp under SDL_RenderSetScale(scale).  cw()/ch() are LOGICAL units.
    bool  load(SDL_Renderer* r, int pt, float scale = 1.0f);
    // Frees every GPU texture this font owns.  MUST run while the renderer that
    // built them is still alive: SDL_DestroyTexture() dereferences
    // texture->renderer, so letting ~Font() do it after SDL_DestroyRenderer() /
    // SDL_Quit() is a use-after-free.  App::shutdown() calls this; ~Font() then
    // finds nothing left to free.  Idempotent.
    void  release();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    int   cw() const { return m_cw; }             // logical cell (advance) width
    int   ch() const { return m_ch; }             // logical cell height
    int   text_w(const std::string& s) const;
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
    static int glyph_count(const std::string& s);
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
    // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y reach the FOCUSED view here BEFORE the
    // shell's project-wide undo runs (App::on_global_undo/redo).  Return true to
    // consume it: a view with its own edit history (sample editor, sampler zone
    // editors) must, otherwise the global undo rolls the entire project back
    // instead of undoing the edit the view advertises in its own tooltip.
    // Returning false -- the default, and what every view without a local
    // history wants -- leaves Ctrl+Z meaning project undo exactly as before.
    // Only the routes keys take (Window -> content, WindowManager -> focused
    // window) forward this; it is deliberately NOT the generic on_key path, so a
    // view that binds a bare 'z' cannot accidentally swallow project undo.
    virtual bool on_undo(App& app, bool redo) { (void)app;(void)redo; return false; }
    // SDL_TEXTINPUT text. Only App::run_modal() delivers this (the main loop
    // instead routes typed text through App::text_target); a modal dialog with
    // its own text field (FileDialog's filename box) overrides it.
    virtual bool on_text(App& app, const char* utf8) { (void)app;(void)utf8; return false; }
    virtual void cancel_interaction(App& app) { (void)app; }
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
    bool on_key(App& app, SDL_Keycode k) override;   // Esc closes an open dropdown
private:
    int m_ddx=0, m_ddy=0, m_ddw=0, m_ddh=0;     // last dropdown rect (hit-test)
    int m_armed_title=-1, m_armed_item=-1;        // touch: activate on release
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
    // A full-workspace background pane (Arrange, the bottom dock) rather than
    // a real floating window. WindowManager::raise() never lets one of these
    // in front of an actual floating window -- otherwise clicking anywhere in
    // Arrange (a near-universal target, since it covers the whole workspace)
    // raises it to the very front and visually buries every open floating
    // window (rack editor, sampler, ...) behind it.
    bool                  alwaysBack = false;
    //  A window can opt OUT of its title bar entirely.  Arrange is the
    //  full-workspace background pane: the strip only ate vertical space above
    //  the edit-mode buttons and repeated a label the view already makes
    //  obvious.  Kept as a flag rather than "just set title_h = 0", because
    //  on_layout re-derives title_h from the UI scale every pass and would
    //  stomp a bare zero straight back to 22.
    bool                  titlebar  = true;
    int                   title_h   = 22;
    std::function<void()> on_close;               // fired when the close box is hit

    SDL_Rect              workspace{0,0,0,0};      // max bounds (set by the manager)
    SDL_Rect              restore_rect{0,0,0,0};   // rect to return to from max/min

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;   // -> content
    bool on_key_up(App& app, SDL_Keycode k) override;// -> content
    bool on_undo(App& app, bool redo) override;      // -> content
    void cancel_interaction(App& app) override;
    SDL_Rect body() const {                       // content rect (inside the frame)
        return SDL_Rect{ rect.x+1, rect.y+title_h, rect.w-2, rect.h-title_h-1 };
    }
    void toggle_maximize();
private:
    bool m_moving = false, m_resizing = false, m_content = false;
    int  m_dx = 0, m_dy = 0;
    unsigned m_last_title_click = 0;              // for double-click maximize
    // App::text_target as this window's content last set it, so the close box
    // and the minimise box can end an inline edit that points into storage the
    // view is about to hide.  Compared, never dereferenced.
    std::string* m_text_owned = nullptr;
public:
    //! Commit-and-release an inline edit this window's content started, if it
    //! is still the live one.  The window manager calls this on every OTHER
    //! window when a press lands somewhere else: an inline edit that keeps
    //! App::text_target after the user has clicked away captures the whole
    //! keyboard (the main loop tests text_target before every shortcut), so
    //! Space stops playing and the app looks frozen.
    void commit_owned_text(App& app);
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
    void cancel_all_interactions(App& app);
    void cancel_interaction(App& app) override;
    bool any_visible() const;
    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;   // -> focused window's content
    bool on_key_up(App& app, SDL_Keycode k) override;// -> focused window's content
    bool on_undo(App& app, bool redo) override;      // -> focused window's content
private:
    Window* m_active = nullptr;                    // window receiving an active drag
    std::vector<Window*> taskbar_hits(App& app, std::vector<SDL_Rect>& rectsOut) const;
    bool m_drawer_open=false, m_drawer_drag=false;
    int m_drawer_width=0, m_drawer_start_x=0, m_drawer_armed=-1;
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
    float         ui_scale = 1.0f;      // mobile chrome/font scale (persistent)
    bool          running = true;
    bool          dirty = true;                  // redraw requested
    bool          animating = false;             // continuous redraw (playback)
    std::vector<Widget*> roots;                  // top-level widgets
    std::function<void(App&)> on_layout;         // called before each redraw
    std::function<void(App&)> on_frame;          // called exactly once before every painted frame
    // Polled on the IDLE path only.  Returns true while the engine is making
    // sound, which raises the idle refresh from ~4 Hz to ~30 Hz so VU meters
    // stay readable when audio is audible with the transport STOPPED -- a live
    // note, an audition, input monitoring, a free-running rack.  `animating`
    // cannot cover those: it is tied to the transport rolling.
    //
    // This is deliberately the IDLE path and not a new render gate.  Idle
    // frames set `dirty`, so on_layout -- and with it the VST2 idle pump and
    // drain_record -- runs on every one of them.  Raising this rate runs that
    // pump MORE often, never less.  (An earlier attempt added a third gate
    // that rendered with dirty=false; that skipped on_layout and stopped the
    // pump, which hung MIDI notes.  Do not reintroduce that shape.)
    std::function<bool()> meters_hot;
    std::function<void(App&)> on_tick;           // ~4x/s housekeeping (device-loss
                                                 // polls etc); fires while idle AND
                                                 // while animating
    // Project-wide edit history hooks. The shell snapshots at transaction
    // boundaries; the toolkit groups a pointer drag into one edit and routes
    // global undo/redo before a focused editor can consume Ctrl+Z/Ctrl+Y.
    std::function<void()> on_edit_begin;
    std::function<void()> on_edit_end;
    std::function<void()> on_global_undo;
    std::function<void()> on_global_redo;

    // --- text input (SDL_TEXTINPUT) --------------------------------------
    // While a target is set, typing edits *text_target; Enter commits, Esc
    // cancels (text_commit(bool)); keys are NOT dispatched to widgets.
    std::string*              text_target = nullptr;
    std::function<void()>     text_changed;
    std::function<void(bool)> text_commit;
    void begin_text(std::string* t, std::function<void()> onChange = nullptr,
                    std::function<void(bool)> onCommit = nullptr);
    void end_text();
    // Ends the edit ONLY if `t` is the field currently being edited; a no-op
    // otherwise, so it can never cancel some other view's rename.  A view whose
    // inline editor points at storage it is about to invalidate -- a window
    // closing or minimising (Widget::cancel_interaction is called on both), a
    // std::string inside a vector that is about to be push_back'd/erased -- MUST
    // call this for that storage.  text_target would otherwise stay live on
    // freed memory: every keystroke is swallowed (the main loop tests
    // text_target before text_input_sink, so the Csound/tracker editors go deaf
    // too) and the next append is a write-after-free.
    void end_text_if(const std::string* t) { if (t && text_target == t) end_text(); }
    // Like end_text_if, but COMMITS: the field's on-commit callback runs with
    // true, exactly as if the user had pressed Enter.  This is the click-away
    // path -- clicking out of a field is "accept", never "discard", everywhere
    // else -- and it is what the window manager uses when a press lands in a
    // different window than the one holding the edit.
    void commit_text_if(const std::string* t) {
        if (!t || text_target != t) return;
        auto cb = text_commit;          // end_text() clears it
        end_text();
        if (cb) cb(true);
    }
    bool editing_text() const { return text_target != nullptr; }

    // General character sink for widgets that manage their own multi-line editing
    // (e.g. the Csound CSD editor): when no single-field text_target is active,
    // SDL_TEXTINPUT characters are delivered here.  Special keys (arrows, enter,
    // backspace, Ctrl+E, ...) still arrive via the widget's on_key.  Set it when
    // the editor is focused; clear it (nullptr) when it loses focus / closes.
    std::function<void(const char*)> text_input_sink;

    bool init(const char* title);
    void set_ui_scale(float value);
    // PATCHKNOB_TRACEREDRAW=1 records who asks for repaints.  A frame marked
    // dirty is treated as user-requested and bypasses the playback frame cap, so
    // a single caller that fires every frame silently defeats the cap -- which is
    // exactly what the frame counter said was happening (56 fps against a 30 Hz
    // ceiling).  Guessing from grep was hopeless: there are 400+ call sites.
    void request_redraw() {
        dirty = true;
        if (trace_redraw) note_redraw(__builtin_return_address(0));
    }
    static bool trace_redraw;
    static void note_redraw(void* returnAddress);
    static void dump_redraw_callers();

    // ---- damage-clipped animation ------------------------------------------
    // A frame where only the playhead moved does not need 2 million pixels
    // rasterised.  Anything that animates registers the screen area it will
    // change; on an animation-only frame the loop sets the clip to the union of
    // those areas and runs the SAME draw code, so the GPU only touches that
    // strip.  Nothing else can go stale, because everything else is exactly the
    // pixels already on screen.
    //
    // Register the area GENEROUSLY (pad it): a frame is clipped to the damage
    // reported by the PREVIOUS frame, so the padding is what covers the
    // distance the playhead travels between the two.
    void add_damage(const SDL_Rect& q) {
        if (q.w > 0 && q.h > 0) damage.push_back(q);
    }
    std::vector<SDL_Rect> damage;      // registered during the current draw
    std::vector<SDL_Rect> lastDamage;  // what the previous draw registered

    // Damage clipping only works if the pixels OUTSIDE the damaged area survive
    // to the next frame, and a presented backbuffer does not: SDL_RenderPresent
    // swaps, so the buffer handed back holds the frame before last.  Clipping
    // straight to the screen therefore alternates two frames -- visible as
    // flicker on everything that is not being redrawn.  So the scene is drawn
    // into this persistent texture, which IS preserved, and the texture is
    // blitted to the screen each frame (one full-screen quad, far cheaper than
    // re-rasterising every widget).
    SDL_Texture* frameTarget = nullptr;
    int frameTargetW = 0, frameTargetH = 0;
    bool ensure_frame_target();        // false => fall back to full repaints

    // OFF BY DEFAULT.  Clipping an animation frame to the damaged area is a big
    // GPU saving in principle, but it only produces a correct image if EVERY
    // pixel that changes has been registered as damage.  Anything that animates
    // without registering (in-app windows, meters, readouts) is left showing
    // stale pixels, which reads as flicker.  Until every animated element
    // registers damage this is not correct enough to be on: a full repaint is
    // slower but always right.  PATCHKNOB_DAMAGE=1 enables it for development.
    bool damage_clipping = false;
    void run(std::function<void(App&)> draw_extra = nullptr);
    void shutdown();

    // Blocking-looking modal pump for a single overlay widget (file dialogs,
    // confirmation prompts): pumps SDL events and redraws `w` full-window each
    // frame, routing mouse/key/text to it exclusively, until `w.visible` goes
    // false. Reenters the SAME SDL_Window/SDL_Renderer as run() (there is no
    // second OS window under KMSDRM to host a "real" modal), so it is just a
    // narrower, single-widget version of the main loop rather than a nested
    // App::run() -- `running` stays under the outer loop's control throughout.
    void run_modal(Widget& dlg);

    // F11: EXCLUSIVE fullscreen -- the window owns the display's framebuffer and
    // the desktop compositor is out of the present path entirely.  That is a
    // different thing from borderless "fullscreen desktop", which still composits
    // through DWM and keeps its overhead.  Toggling back restores the window.
    void toggle_fullscreen();
    bool fullscreen = false;

    // Display settings, driven from the A/V menu.
    // `anim_hz` caps only frames nobody asked for (the playhead advancing).
    // Lower = less GPU during playback; user-driven repaints are unaffected.
    // Defaults to 30, not 60: matching frameInterval here made the animation
    // cap a no-op (see its use in run()) and defeated the whole point of
    // pacing playhead-only frames separately from user-driven ones -- a
    // playhead updating at 30 Hz reads as smooth and roughly halves GPU
    // load during playback, which is exactly the reported high-GPU-while-
    // playing complaint this mechanism exists to address.
    int  anim_hz = 30;
    bool vsync_on = true;
    void set_vsync(bool on);
    // Backend name + SDL runtime version, for the A/V menu to display.
    std::string video_backend() const;
    std::string sdl_version() const;
};

} // namespace ui
#endif
