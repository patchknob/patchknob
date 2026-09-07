//----------------------------------------------------------------------------
//  sdlui/plugin_editor_window.cpp -- native container for plugin GUIs, embedded
//  as a child of the SDL main window.  Windows (HWND) + Linux/X11 (XID); no-op
//  on windowless targets (KMSDRM) where the SDL parameter panel is used instead.
//----------------------------------------------------------------------------
#include "plugin_editor_window.h"
#include "engine/plugin_api.h"
// gui.h FIRST: the X11 headers below leak macros (None, Status, ...) that must
// not land on the widget declarations; and both platform branches read
// ui::theme() so the container's background is two-tone like everything else.
#include "gui.h"

#include <SDL.h>
#ifndef PATCHKNOB_SDL3
#include <SDL_syswm.h>
#endif

#include <string>

// ============================================================================
//  Windows
// ============================================================================
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <climits>

namespace PatchKnob { namespace hostwin {
namespace {
struct EditorWin {
    // TWO windows, not one: `clip` is a plain child of the SDL window sized to
    // the VISIBLE part of the frame body, and `hwnd` -- the container the
    // plugin actually renders into -- is a child of `clip`, always at the FULL
    // body size.  Win32 clips a child to its parent, so shrinking `clip` crops
    // the GUI without ever lying to the plugin about its window's size, and
    // offsetting `hwnd` inside `clip` keeps the surviving region aligned with
    // the frame.  (`clip` is null for the no-parent top-level fallback.)
    HWND                            clip = nullptr;
    HWND                            hwnd = nullptr;
    PatchKnob::engine::IPluginInstance* inst = nullptr;
    int                             nw = 0, nh = 0;
    // Requested geometry (physical client px of the SDL window):
    int                             bx = INT_MIN, by = 0, bw = 0, bh = 0; // frame body
    int                             cx = INT_MIN, cy = 0, cw = 0, ck = 0; // clip rect (INT_MIN = none)
    // Last APPLIED geometry + show-state.  The UI loop calls set_bounds/show
    // EVERY frame; re-issuing MoveWindow/ShowWindow when nothing changed
    // repaints the child and DISMISSES the plugin's own popups (combobox
    // dropdowns close on the owner moving).  So skip the WinAPI call unless the
    // computed value actually moved.
    int                             avx = INT_MIN, avy = 0, avw = 0, avh = 0; // clip window
    int                             aox = INT_MIN, aoy = 0, aow = 0, aoh = 0; // container in clip
    int                             shown = -1;   // -1 unknown, 0 hidden, 1 shown
};
const wchar_t* kClass = L"PatchKnobPluginEditor";
bool           g_reg  = false;

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    EditorWin* e = reinterpret_cast<EditorWin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_CLOSE) { if (e && e->inst) e->inst->closeEditor(); DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) {
        if (e) { if (h == e->hwnd) e->hwnd = nullptr; if (h == e->clip) e->clip = nullptr; }
        return 0;
    }
    if (m == WM_ERASEBKGND) {
        // Theme-matched background (two-tone rule): visible only in the moment
        // between the frame appearing and the plugin's first paint, or when the
        // plugin shrinks its GUI below the frame body.
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc; GetClientRect(h, &rc);
        const ui::Color c = ui::theme().panel;
        HBRUSH b = CreateSolidBrush(RGB(c.r, c.g, c.b));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, m, wp, lp);
}
void ensure_class() {
    if (g_reg) return;
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;                    // WM_ERASEBKGND paints the theme
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); g_reg = true;
}
// Apply the requested body+clip rects to the two windows (embedded mode only).
void apply_geometry(EditorWin* e) {
    if (!e || !e->clip || !e->hwnd || e->bx == INT_MIN) return;
    int vx = e->bx, vy = e->by, vw = e->bw, vh = e->bh;
    if (e->cx != INT_MIN) {                        // intersect body with the clip
        const int x2 = (vx + vw < e->cx + e->cw) ? vx + vw : e->cx + e->cw;
        const int y2 = (vy + vh < e->cy + e->ck) ? vy + vh : e->cy + e->ck;
        if (vx < e->cx) vx = e->cx;
        if (vy < e->cy) vy = e->cy;
        vw = x2 - vx; vh = y2 - vy;
    }
    if (vw < 1 || vh < 1) { vx = -32000; vy = -32000; vw = 1; vh = 1; } // clipped away entirely
    const int ox = e->bx - vx, oy = e->by - vy;    // keep the GUI aligned to the frame
    if (vx != e->avx || vy != e->avy || vw != e->avw || vh != e->avh) {
        e->avx = vx; e->avy = vy; e->avw = vw; e->avh = vh;
        MoveWindow(e->clip, vx, vy, vw, vh, TRUE);
    }
    if (ox != e->aox || oy != e->aoy || e->bw != e->aow || e->bh != e->aoh) {
        e->aox = ox; e->aoy = oy; e->aow = e->bw; e->aoh = e->bh;
        MoveWindow(e->hwnd, ox, oy, e->bw, e->bh, TRUE);
    }
}
HWND sdl_hwnd(void* sdlWindow) {
    if (!sdlWindow) return nullptr;
#ifdef PATCHKNOB_SDL3
    // SDL3 replaced the SysWM struct with the per-window property store; the
    // HWND is published under a well-known key.
    SDL_PropertiesID props =
        SDL_GetWindowProperties(reinterpret_cast<SDL_Window*>(sdlWindow));
    return (HWND) SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(sdlWindow), &wm)) return nullptr;
    return wm.subsystem == SDL_SYSWM_WINDOWS ? wm.info.win.window : nullptr;
#endif
}
} // namespace

void* editor_open(PatchKnob::engine::IPluginInstance* inst, const char* title, void* sdlWindow) {
    if (!inst || !inst->hasEditor()) return nullptr;
    HWND parent = sdl_hwnd(sdlWindow);
    ensure_class();
    std::wstring wt = L"Plugin";
    if (title && *title) { int n = MultiByteToWideChar(CP_UTF8,0,title,-1,nullptr,0);
        if (n>1) { wt.resize(n-1); MultiByteToWideChar(CP_UTF8,0,title,-1,&wt[0],n); } }

    EditorWin* e = new EditorWin(); e->inst = inst;
    if (parent) {
        // Embedded: clip frame (child of SDL) + container (child of the clip).
        // WS_CLIPCHILDREN on both: without it every background erase painted
        // OVER the plugin's child window -- the white flashes / ghost frames
        // whenever the ui::Window was moved or resized.
        const DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        e->clip = CreateWindowExW(0, kClass, L"", style, 0,0,480,320,
                                  parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!e->clip) { delete e; return nullptr; }
        SetWindowLongPtrW(e->clip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
        e->hwnd = CreateWindowExW(0, kClass, wt.c_str(), style, 0,0,480,320,
                                  e->clip, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!e->hwnd) { DestroyWindow(e->clip); delete e; return nullptr; }
        SetWindowLongPtrW(e->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
        ShowWindow(e->hwnd, SW_SHOW);         // inner is always shown; visibility = clip
        if (!inst->openEditor(reinterpret_cast<PatchKnob::engine::NativeWindowHandle>(e->hwnd))) {
            DestroyWindow(e->clip); delete e; return nullptr;
        }
        int ew=480, eh=320; inst->getEditorSize(ew, eh);
        if (ew<60) ew=480;
        if (eh<60) eh=320;
        e->nw=ew; e->nh=eh;
        SetWindowPos(e->clip, HWND_TOP, 0,0, ew,eh, SWP_NOMOVE|SWP_SHOWWINDOW);
        MoveWindow(e->hwnd, 0,0, ew,eh, TRUE);
        UpdateWindow(e->clip);
    } else {
        // No SDL HWND to embed into: plain top-level fallback (the OS window
        // manager owns its geometry, so set_bounds is a no-op for it).
        const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX)
                          | WS_CLIPCHILDREN;
        HWND hwnd = CreateWindowExW(0, kClass, wt.c_str(), style, 0,0,480,320,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) { delete e; return nullptr; }
        e->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
        if (!inst->openEditor(reinterpret_cast<PatchKnob::engine::NativeWindowHandle>(hwnd))) {
            DestroyWindow(hwnd); delete e; return nullptr;
        }
        int ew=480, eh=320; inst->getEditorSize(ew, eh);
        if (ew<60) ew=480;
        if (eh<60) eh=320;
        e->nw=ew; e->nh=eh;
        RECT rc={0,0,ew,eh}; AdjustWindowRect(&rc, style, FALSE);
        SetWindowPos(hwnd,nullptr,0,0,rc.right-rc.left,rc.bottom-rc.top,SWP_NOMOVE|SWP_NOZORDER);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return e;
}
void editor_close(void* h){ EditorWin* e=(EditorWin*)h; if(!e)return; if(e->inst)e->inst->closeEditor();
    if(e->clip)DestroyWindow(e->clip);           // destroys the container with it
    else if(e->hwnd)DestroyWindow(e->hwnd);
    delete e; }
void editor_idle(void* h){
    EditorWin* e=(EditorWin*)h; if(!e||!e->inst) return;
    HWND outer = e->clip ? e->clip : e->hwnd;
    if(!outer || !IsWindowVisible(outer)) return;
    e->inst->idleEditor();
    // Live size: a plugin resizes its own GUI (VST2 audioMasterSizeWindow, a
    // VST3 resizeView, a scale menu) and the container/frame must follow or the
    // editor draws cropped inside a stale frame.  effEditGetRect/IPlugView::
    // getSize are cheap message-thread calls; the shell re-reads
    // editor_native_size() per frame and resizes the ui::Window to match.
    int w=0, hh=0; e->inst->getEditorSize(w, hh);
    if (w>=60 && hh>=60 && (w!=e->nw || hh!=e->nh)) {
        e->nw=w; e->nh=hh;
        if (!e->clip && e->hwnd) {               // top-level fallback: refit the frame
            RECT rc={0,0,w,hh};
            AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrW(e->hwnd, GWL_STYLE), FALSE);
            SetWindowPos(e->hwnd, nullptr, 0,0, rc.right-rc.left, rc.bottom-rc.top,
                         SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
    }
}
bool editor_alive(void* h){ EditorWin* e=(EditorWin*)h; if(!e) return false;
    HWND outer = e->clip ? e->clip : e->hwnd;
    return outer && IsWindow(outer); }
void editor_set_bounds(void* h,int x,int y,int w,int hh){
    EditorWin* e=(EditorWin*)h; if(!e||!e->hwnd) return;
    if(!e->clip) return;   // top-level fallback: the OS window manager owns it
    if(w<1) w=1;
    if(hh<1) hh=1;
    if(x==e->bx && y==e->by && w==e->bw && hh==e->bh) return;   // unchanged
    e->bx=x; e->by=y; e->bw=w; e->bh=hh;
    apply_geometry(e);
}
void editor_set_clip(void* h,int x,int y,int w,int hh){
    EditorWin* e=(EditorWin*)h; if(!e||!e->clip) return;
    if(w<0) w=0;
    if(hh<0) hh=0;
    if(x==e->cx && y==e->cy && w==e->cw && hh==e->ck) return;   // unchanged
    e->cx=x; e->cy=y; e->cw=w; e->ck=hh;
    apply_geometry(e);
}
void editor_show(void* h,bool s){
    EditorWin* e=(EditorWin*)h; if(!e) return;
    HWND outer = e->clip ? e->clip : e->hwnd;
    if(!outer) return;
    const int want = s?1:0;
    if(e->shown==want) return;                                  // unchanged -> skip
    e->shown=want;
    ShowWindow(outer, s?SW_SHOW:SW_HIDE);
}
bool editor_native_size(void* h,int* w,int* hh){ EditorWin* e=(EditorWin*)h; if(!e||e->nw<=0)return false; if(w)*w=e->nw; if(hh)*hh=e->nh; return true; }
bool editor_is_detached(void*){ return false; }   // Win32 editors always embed
}} // namespace

// ============================================================================
//  Linux / X11
//
//  Two modes, chosen at RUNTIME:
//
//   * EMBEDDED  -- SDL is on its X11 backend, so the SDL window has a real XID.
//                  The plugin gets a child X11 window of it, moved/resized to
//                  track the ui::Window frame.  (The original behaviour.)
//
//   * DETACHED  -- SDL is NOT on X11 (Wayland, and in principle KMSDRM+X, ...).
//                  There is no XID to parent to, so we open our OWN connection
//                  to the X server (which under a Wayland session is XWayland,
//                  reached through $DISPLAY) and give the plugin a normal
//                  TOP-LEVEL X11 window managed by the compositor.
//
//  Why detached rather than "make SDL use X11": every VST2/VST3 editor on Linux
//  is an X11 window (VST3 has exactly one Linux platform type,
//  kPlatformTypeX11EmbedWindowID; there is no Wayland platform type at all, and
//  no plugin standard can embed a foreign surface into a Wayland window).  So
//  the plugin needs an X server either way -- but only the PLUGIN needs it.
//  Forcing SDL_VIDEODRIVER=x11 would push the whole app through XWayland
//  (blurrier scaling, extra copy, worse input latency) and cannot be undone
//  after SDL_Init.  A top-level XWayland window costs nothing to the rest of
//  the app and works whichever backend SDL picked.
//
//  NOTE: the old guard here was `defined(SDL_VIDEO_DRIVER_X11)`.  SDL3 does not
//  install its build config, so that macro is NEVER defined in an SDL3 build and
//  this entire file collapsed to the no-op stub below -- native plugin GUIs
//  could not open on Linux at all, X11 session or not.  The guard is now
//  PATCHKNOB_HAVE_X11, set by CMake when libX11 is found and linked.
// ============================================================================
#elif defined(__linux__) && defined(PATCHKNOB_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace PatchKnob { namespace hostwin {
namespace {
//----------------------------------------------------------------------------
//  Why the next two helpers exist (the Dragonfly Reverb crash, 2026-08-30):
//
//  SDL3 on Wayland renders through EGL and leaves its EGL context CURRENT on
//  the UI thread.  A GL-based plugin editor (DPF/Dragonfly, JUCE-GL, ...) then
//  calls glXMakeCurrent on that same thread from inside openEditor()/its idle
//  timers -- and libglvnd refuses to hand the thread's GL dispatch from EGL to
//  GLX: CommonMakeCurrent synthesizes a BadAccess X error (X_GLXMakeCurrent)
//  client-side (__glXSendError).  The plugin never installed an X error
//  handler on its private Display, so libX11's DEFAULT handler ran -- which
//  calls exit(), which destroyed a still-joinable std::thread, which called
//  std::terminate: the whole DAW aborted from one menu click.
//
//  Two independent defences, both needed:
//   * ScopedGlRelease -- release the thread's current SDL GL/EGL context
//     around EVERY call into the plugin's editor (open, idle, close), and
//     rebind it afterwards.  This is the functional fix: with the dispatch
//     free, the plugin's glXMakeCurrent succeeds and its GUI actually draws.
//     A no-op when SDL has no GL context current (X11/GLX SDL, Vulkan/"gpu"
//     renderer, software) -- glvnd allows GLX->GLX switches.
//   * install_x_error_guard -- a process-wide X error handler that LOGS and
//     returns instead of exiting.  X errors on the plugin's own connections
//     surface inside plugin code where no handler of ours is on the stack; the
//     handler is the only thing standing between any future plugin X bug and
//     losing the user's project.  Installed lazily at first editor open so it
//     also wins over any handler SDL installed at init (we swallow-and-log for
//     the whole process; a DAW must never die on an X protocol error).
//----------------------------------------------------------------------------
struct ScopedGlRelease {
    SDL_Window*   w = nullptr;
    SDL_GLContext c = nullptr;
    ScopedGlRelease() {
        c = SDL_GL_GetCurrentContext();
        if (c) {
            w = SDL_GL_GetCurrentWindow();
            SDL_GL_MakeCurrent(nullptr, nullptr);   // release: frees glvnd dispatch
        }
    }
    ~ScopedGlRelease() {
        if (c) SDL_GL_MakeCurrent(w, c);            // rebind for SDL rendering
    }
    ScopedGlRelease(const ScopedGlRelease&) = delete;
    ScopedGlRelease& operator=(const ScopedGlRelease&) = delete;
};

int x_error_logger(Display* d, XErrorEvent* ev) {
    char text[256] = {0};
    XGetErrorText(d, ev->error_code, text, sizeof(text));
    std::fprintf(stderr,
                 "[PatchKnob] X error caught (app continues): %s "
                 "(request %d.%d, resource 0x%lx)\n",
                 text, ev->request_code, ev->minor_code,
                 (unsigned long)ev->resourceid);
    return 0;   // never exit(): that is what killed the app before
}
void install_x_error_guard() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    XSetErrorHandler(&x_error_logger);
}

struct EditorWin {
    Display*                        dpy   = nullptr;
    // EMBEDDED mode uses TWO windows, exactly like the Win32 branch above:
    // `clip` (child of the SDL window) is sized to the VISIBLE part of the
    // frame body, and `win` -- handed to the plugin -- is a child of `clip` at
    // the FULL body size.  X11 clips a child to its parent, so shrinking `clip`
    // crops the GUI without resizing the plugin's window, and offsetting `win`
    // inside `clip` keeps the surviving region aligned with the frame.
    // DETACHED mode has no `clip`; `win` is the top-level itself.
    Window                          clip  = 0;
    Window                          win   = 0;   // the window handed to the plugin
    PatchKnob::engine::IPluginInstance* inst  = nullptr;
    bool                            detached = false; // top-level, not a child of SDL
    bool                            closed   = false; // WM close / window destroyed
    Atom                            wmDelete = 0;
    int                             nw = 0, nh = 0;
    int                             tw = 0, th = 0;   // detached: current outer size
    // Requested geometry (physical client px of the SDL window):
    int                             bx = INT_MIN, by = 0, bw = 0, bh = 0; // frame body
    int                             cx = INT_MIN, cy = 0, cw = 0, ck = 0; // clip rect (INT_MIN = none)
    // Last APPLIED geometry + show-state, for the same reason the Win32 branch
    // above keeps them: the UI loop calls set_bounds/show EVERY frame, and
    // re-issuing XMoveResizeWindow when nothing moved floods the plugin with
    // ConfigureNotify and closes its own popups (menus reparent to the root
    // and go away when their owner reconfigures). Skip the call unless the
    // computed value actually changed.
    int                             avx = INT_MIN, avy = 0, avw = 0, avh = 0; // clip window
    int                             aox = INT_MIN, aoy = 0, aow = 0, aoh = 0; // container in clip
    int                             shown = -1;   // -1 unknown, 0 hidden, 1 shown
};
// Theme-matched background pixel (two-tone rule): visible only before the
// plugin's first paint or when its GUI is smaller than the frame body.
unsigned long theme_pixel(Display* dpy, int scr) {
    const ui::Color c = ui::theme().panel;
    XColor xc; xc.red = (unsigned short)(c.r * 257); xc.green = (unsigned short)(c.g * 257);
    xc.blue = (unsigned short)(c.b * 257); xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, DefaultColormap(dpy, scr), &xc)) return xc.pixel;
    return BlackPixel(dpy, scr);
}
// Apply the requested body+clip rects to the two windows (embedded mode only).
void apply_geometry(EditorWin* e) {
    if (!e || !e->dpy || !e->clip || !e->win || e->bx == INT_MIN) return;
    int vx = e->bx, vy = e->by, vw = e->bw, vh = e->bh;
    if (e->cx != INT_MIN) {                        // intersect body with the clip
        const int x2 = (vx + vw < e->cx + e->cw) ? vx + vw : e->cx + e->cw;
        const int y2 = (vy + vh < e->cy + e->ck) ? vy + vh : e->cy + e->ck;
        if (vx < e->cx) vx = e->cx;
        if (vy < e->cy) vy = e->cy;
        vw = x2 - vx; vh = y2 - vy;
    }
    if (vw < 1 || vh < 1) { vx = -32000; vy = -32000; vw = 1; vh = 1; } // clipped away entirely
    const int ox = e->bx - vx, oy = e->by - vy;    // keep the GUI aligned to the frame
    bool flush = false;
    if (vx != e->avx || vy != e->avy || vw != e->avw || vh != e->avh) {
        e->avx = vx; e->avy = vy; e->avw = vw; e->avh = vh;
        XMoveResizeWindow(e->dpy, e->clip, vx, vy, (unsigned)vw, (unsigned)vh);
        flush = true;
    }
    if (ox != e->aox || oy != e->aoy || e->bw != e->aow || e->bh != e->aoh) {
        e->aox = ox; e->aoy = oy; e->aow = e->bw; e->aoh = e->bh;
        XMoveResizeWindow(e->dpy, e->win, ox, oy, (unsigned)e->bw, (unsigned)e->bh);
        flush = true;
    }
    if (flush) XFlush(e->dpy);
}
bool sdl_x11(void* sdlWindow, Display** dpy, Window* win) {
    if (!sdlWindow) return false;
#ifdef PATCHKNOB_SDL3
    // As with the Win32 branch above: SDL3 publishes native handles through the
    // window property store instead of SDL_SysWMinfo.  On the Wayland backend
    // these two X11 keys are absent, which is exactly how we detect "no XID".
    SDL_PropertiesID props =
        SDL_GetWindowProperties(reinterpret_cast<SDL_Window*>(sdlWindow));
    Display* d = (Display*) SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    const Sint64 xid = SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!d || !xid) return false;
    *dpy = d; *win = (Window) xid; return true;
#elif defined(SDL_VIDEO_DRIVER_X11)
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(sdlWindow), &wm)) return false;
    if (wm.subsystem != SDL_SYSWM_X11) return false;
    *dpy = wm.info.x11.display; *win = wm.info.x11.window; return true;
#else
    // SDL2 built without its X11 backend: SDL_SysWMinfo has no x11 member at
    // all, so there is never an XID to embed into -> always detached.
    (void)dpy; (void)win; return false;
#endif
}

// Our own X connection for detached editors.  Opened once and kept for the life
// of the process: it must NOT be SDL's connection (we pump its event queue, and
// draining SDL's queue behind SDL's back would eat the app's input), and
// re-connecting per editor buys nothing while plugin toolkits are still tearing
// down asynchronously.  Under Wayland this lands on XWayland via $DISPLAY.
Display* detached_display() {
    static Display* d     = nullptr;
    static bool     tried = false;
    if (!tried) { tried = true; d = XOpenDisplay(nullptr); }
    return d;
}

// Embedded when SDL gave us an XID, detached otherwise.  PATCHKNOB_PLUGIN_GUI
// (=detached|embedded) forces either mode, mostly for testing the path the
// current session would not pick.
bool want_detached(bool haveSdlX11) {
    if (const char* v = getenv("PATCHKNOB_PLUGIN_GUI")) {
        if (!strcmp(v,"detached") || !strcmp(v,"toplevel") || !strcmp(v,"window")) return true;
        if (!strcmp(v,"embedded") || !strcmp(v,"embed")    || !strcmp(v,"child"))  return false;
    }
    return !haveSdlX11;
}

// Detached only: drain OUR connection.  Watches for the user closing the
// window, and follows the plugin resizing its own window inside ours (a VST3
// resizeView, a "GUI size" menu, ...) so the top-level keeps fitting the editor.
void pump_detached(EditorWin* e) {
    if (!e->dpy || !e->win) return;
    while (XPending(e->dpy)) {
        XEvent ev; XNextEvent(e->dpy, &ev);
        switch (ev.type) {
        case ClientMessage:
            if (ev.xclient.window == e->win && e->wmDelete &&
                (Atom)ev.xclient.data.l[0] == e->wmDelete)
                e->closed = true;
            break;
        case DestroyNotify:
            if (ev.xdestroywindow.window == e->win) { e->closed = true; e->win = 0; }
            break;
        case ConfigureNotify:
            // SubstructureNotify: a CHILD of our window (the plugin's own) moved
            // or resized.  event==our window, window==the child.
            if (ev.xconfigure.event == e->win && ev.xconfigure.window != e->win) {
                const int w = ev.xconfigure.width, h = ev.xconfigure.height;
                if (w >= 60 && h >= 60 && (w != e->tw || h != e->th)) {
                    e->tw = w; e->th = h; e->nw = w; e->nh = h;
                    XResizeWindow(e->dpy, e->win, (unsigned)w, (unsigned)h);
                }
            }
            break;
        default: break;
        }
    }
}
} // namespace

void* editor_open(PatchKnob::engine::IPluginInstance* inst, const char* title, void* sdlWindow) {
    if (!inst) return nullptr;
    // BEFORE any plugin-editor code runs: stray X errors must log, not exit()
    // the DAW, and the thread's GL dispatch must be free for the plugin's
    // glXMakeCurrent (see the two helpers' comment above -- this is the
    // Dragonfly Reverb crash fix, and it applies to every GL-based editor).
    install_x_error_guard();
    ScopedGlRelease glRelease;
    if (!inst->hasEditor()) return nullptr;
    Display* dpy=nullptr; Window parent=0;
    const bool haveSdlX11 = sdl_x11(sdlWindow, &dpy, &parent) && dpy && parent;
    const bool detached   = want_detached(haveSdlX11);
    if (detached) {
        dpy = detached_display();
        if (!dpy) {                       // no X server at all (pure Wayland w/o
            fprintf(stderr,               //  XWayland, KMSDRM, headless): the
                    "[PatchKnob] plugin GUI: SDL is not on X11 and no X display "
                    "is reachable ($DISPLAY=%s). Linux plugin editors are X11-only; "
                    "falling back to the parameter panel.\n",
                    getenv("DISPLAY") ? getenv("DISPLAY") : "<unset>");
            return nullptr;               //  caller opens the SDL param panel
        }
        parent = DefaultRootWindow(dpy);
    }
    if (!dpy || !parent) return nullptr;  // embedding forced but SDL has no XID

    EditorWin* e = new EditorWin(); e->inst = inst; e->dpy = dpy; e->detached = detached;
    int scr = DefaultScreen(dpy);
    const unsigned long bg = theme_pixel(dpy, scr);
    if (!detached) {
        // Embedded: clip frame under the SDL window, container inside it.
        e->clip = XCreateSimpleWindow(dpy, parent, 0,0, 480,320, 0, bg, bg);
        if (!e->clip) { delete e; return nullptr; }
        parent = e->clip;
    }
    e->win = XCreateSimpleWindow(dpy, parent, 0,0, 480,320, 0, bg, bg);
    if (!e->win) {
        if (e->clip) { XDestroyWindow(dpy, e->clip); XFlush(dpy); }
        delete e; return nullptr;
    }
    if (detached) {
        // A real top-level: give the WM a title, a class (so users can write a
        // float rule for it), and the close-button protocol.
        const char* t = (title && *title) ? title : "Plugin";
        XStoreName(dpy, e->win, t);
        Atom netName = XInternAtom(dpy, "_NET_WM_NAME", False);
        Atom utf8    = XInternAtom(dpy, "UTF8_STRING",  False);
        XChangeProperty(dpy, e->win, netName, utf8, 8, PropModeReplace,
                        (const unsigned char*)t, (int)strlen(t));
        char resName[] = "patchknob-plugin", resClass[] = "PatchKnob";
        XClassHint ch; ch.res_name = resName; ch.res_class = resClass;
        XSetClassHint(dpy, e->win, &ch);
        // _NET_WM_WINDOW_TYPE_DIALOG.  Not cosmetic: measured on Hyprland, a
        // plain top-level gets TILED (a 480x320 editor came back as 941x508 and
        // the plugin's fixed-size GUI would sit in the corner of an oversized
        // window).  Tiling WMs float dialogs and honour the requested size, and
        // a plugin editor genuinely is a utility dialog of the app.
        Atom wtype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom wdlg  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
        XChangeProperty(dpy, e->win, wtype, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char*)&wdlg, 1);
        e->wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, e->win, &e->wmDelete, 1);
        // StructureNotify: our window destroyed.  SubstructureNotify: the
        // plugin's window resizing itself inside ours.  (NOT SubstructureRedirect
        // -- that would make us a window manager for it.)
        XSelectInput(dpy, e->win, StructureNotifyMask | SubstructureNotifyMask);
    }
    XMapWindow(dpy, e->win);
    if (e->clip) XMapWindow(dpy, e->clip);
    XSync(dpy, False);                    // the plugin must find a realized window
    // On X11 the VST3 platform type is kPlatformTypeX11EmbedWindowID; the handle
    // passed to attached() IS the X11 window id (as a void*-sized value).
    if (!inst->openEditor(reinterpret_cast<PatchKnob::engine::NativeWindowHandle>(
                              (uintptr_t)e->win))) {
        XDestroyWindow(dpy, e->clip ? e->clip : e->win); XFlush(dpy);
        delete e; return nullptr;
    }
    int ew=480, eh=320; inst->getEditorSize(ew, eh);
    if (ew<60) ew=480;
    if (eh<60) eh=320;
    e->nw=ew; e->nh=eh;
    XResizeWindow(dpy, e->win, (unsigned)ew, (unsigned)eh);
    if (e->clip) XResizeWindow(dpy, e->clip, (unsigned)ew, (unsigned)eh);
    if (detached) {
        e->tw = ew; e->th = eh; e->shown = 1;
        XSizeHints hints; memset(&hints, 0, sizeof(hints));
        hints.flags = PSize; hints.width = ew; hints.height = eh;
        XSetWMNormalHints(dpy, e->win, &hints);
        XRaiseWindow(dpy, e->win);
    }
    XSync(dpy, False);
    return e;
}
void editor_close(void* h){ EditorWin* e=(EditorWin*)h; if(!e)return;
    { ScopedGlRelease glRelease;                 // plugin may enter GL to destroy
      if(e->inst)e->inst->closeEditor(); }
    if(e->dpy){ Window outer = e->clip ? e->clip : e->win;   // clip destroys its subtree
        if(outer){ XDestroyWindow(e->dpy,outer); XFlush(e->dpy); } }
    delete e; }
void editor_idle(void* h){ EditorWin* e=(EditorWin*)h; if(!e) return;
    // idleEditor() drives the plugin's timers, i.e. its GL REPAINTS: the same
    // EGL-vs-GLX dispatch conflict as editor_open, every frame.
    ScopedGlRelease glRelease;
    if(e->detached) pump_detached(e);
    if(e->inst && !e->closed) {
        e->inst->idleEditor();
        // Live size: a plugin resizes its own GUI (VST3 resizeView, VST2
        // audioMasterSizeWindow, a scale menu).  effEditGetRect / IPlugView::
        // getSize are cheap message-thread calls; embedded mode reports the new
        // size through editor_native_size() (the shell re-reads it per frame
        // and resizes the ui::Window to match), detached mode refits its own
        // top-level here.
        int w=0, hh=0; e->inst->getEditorSize(w, hh);
        if (w>=60 && hh>=60 && (w!=e->nw || hh!=e->nh)) {
            e->nw=w; e->nh=hh;
            if (e->detached && e->dpy && e->win && (w!=e->tw || hh!=e->th)) {
                e->tw=w; e->th=hh;
                XSizeHints hints; memset(&hints, 0, sizeof(hints));
                hints.flags = PSize; hints.width = w; hints.height = hh;
                XSetWMNormalHints(e->dpy, e->win, &hints);
                XResizeWindow(e->dpy, e->win, (unsigned)w, (unsigned)hh);
            }
        }
    }
    if(e->dpy) XFlush(e->dpy); }
bool editor_alive(void* h){ EditorWin* e=(EditorWin*)h; return e && e->win && !e->closed; }
void editor_set_bounds(void* h,int x,int y,int w,int hh){ EditorWin* e=(EditorWin*)h;
    if(!e||!e->dpy||!e->win) return;
    if(e->detached) return;   // top-level: the WM owns its geometry, and the
                              // caller's rect is relative to the SDL window
    if(w<1) w=1;
    if(hh<1) hh=1;                                              // X11 rejects a 0 extent
    if(x==e->bx && y==e->by && w==e->bw && hh==e->bh) return;   // unchanged -> don't reconfigure
    e->bx=x; e->by=y; e->bw=w; e->bh=hh;
    apply_geometry(e); }
void editor_set_clip(void* h,int x,int y,int w,int hh){ EditorWin* e=(EditorWin*)h;
    if(!e||!e->dpy||!e->clip) return;             // detached: the WM clips for us
    if(w<0) w=0;
    if(hh<0) hh=0;
    if(x==e->cx && y==e->cy && w==e->cw && hh==e->ck) return;   // unchanged
    e->cx=x; e->cy=y; e->cw=w; e->ck=hh;
    apply_geometry(e); }
void editor_show(void* h,bool s){ EditorWin* e=(EditorWin*)h; if(!e||!e->dpy) return;
    Window outer = e->clip ? e->clip : e->win;
    if(!outer) return;
    const int want = s?1:0;
    if(e->shown==want) return;                                  // unchanged -> skip
    e->shown=want;
    if(s){ XMapWindow(e->dpy,outer); if(e->detached) XRaiseWindow(e->dpy,outer); }
    else   XUnmapWindow(e->dpy,outer);
    XFlush(e->dpy); }
bool editor_native_size(void* h,int* w,int* hh){ EditorWin* e=(EditorWin*)h; if(!e||e->nw<=0)return false;
    if(e->detached){
        // The editor is its own OS window, so the in-app frame holds none of
        // its pixels -- report a small fixed body sized for the explanatory
        // label the shell puts there (see main.cpp on_open_gui), not the
        // editor's size, so the caller never opens a large empty box.  The
        // frame still titles / hides / closes the detached editor.
        if(w)*w=430;
        if(hh)*hh=46;
        return true;
    }
    if(w)*w=e->nw;
    if(hh)*hh=e->nh;
    return true; }
bool editor_is_detached(void* h){ EditorWin* e=(EditorWin*)h; return e && e->detached; }
}}

// ============================================================================
//  Windowless / other  (no native embedding; use the SDL parameter panel)
// ============================================================================
#else
namespace PatchKnob { namespace hostwin {
void* editor_open(PatchKnob::engine::IPluginInstance*, const char*, void*) { return nullptr; }
void  editor_close(void*) {}
void  editor_idle(void*)  {}
bool  editor_alive(void*) { return false; }
void  editor_set_bounds(void*, int, int, int, int) {}
void  editor_set_clip(void*, int, int, int, int) {}
void  editor_show(void*, bool) {}
bool  editor_native_size(void*, int*, int*) { return false; }
bool  editor_is_detached(void*) { return false; }
}}
#endif
