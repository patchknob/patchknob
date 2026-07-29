//----------------------------------------------------------------------------
//  sdlui/plugin_editor_window.cpp -- native container for plugin GUIs, embedded
//  as a child of the SDL main window.  Windows (HWND) + Linux/X11 (XID); no-op
//  on windowless targets (KMSDRM) where the SDL parameter panel is used instead.
//----------------------------------------------------------------------------
#include "plugin_editor_window.h"
#include "engine/plugin_api.h"

#include <SDL.h>
#include <SDL_syswm.h>

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
    HWND                            hwnd = nullptr;
    PatchKnob::engine::IPluginInstance* inst = nullptr;
    int                             nw = 0, nh = 0;
    // Last applied bounds + show-state.  The UI loop calls set_bounds/show EVERY
    // frame; re-issuing MoveWindow/ShowWindow when nothing changed repaints the
    // child and DISMISSES the plugin's own popups (combobox dropdowns close on
    // the owner moving).  So skip the WinAPI call unless the value actually moved.
    int                             lx = INT_MIN, ly = INT_MIN, lw = INT_MIN, lh = INT_MIN;
    int                             shown = -1;   // -1 unknown, 0 hidden, 1 shown
};
const wchar_t* kClass = L"PatchKnobPluginEditor";
bool           g_reg  = false;

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    EditorWin* e = reinterpret_cast<EditorWin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_CLOSE) { if (e && e->inst) e->inst->closeEditor(); DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { if (e) e->hwnd = nullptr; return 0; }
    return DefWindowProcW(h, m, wp, lp);
}
void ensure_class() {
    if (g_reg) return;
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = kClass;
    RegisterClassExW(&wc); g_reg = true;
}
HWND sdl_hwnd(void* sdlWindow) {
    if (!sdlWindow) return nullptr;
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(sdlWindow), &wm)) return nullptr;
    return wm.subsystem == SDL_SYSWM_WINDOWS ? wm.info.win.window : nullptr;
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
    DWORD style = parent ? (WS_CHILD | WS_CLIPSIBLINGS)
                         : (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);
    HWND hwnd = CreateWindowExW(0, kClass, wt.c_str(), style, 0,0,480,320,
                                parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { delete e; return nullptr; }
    e->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(e));
    if (!inst->openEditor(reinterpret_cast<PatchKnob::engine::NativeWindowHandle>(hwnd))) {
        DestroyWindow(hwnd); delete e; return nullptr;
    }
    int ew=480, eh=320; inst->getEditorSize(ew, eh);
    if (ew<60) ew=480; if (eh<60) eh=320;
    e->nw=ew; e->nh=eh;
    if (parent) SetWindowPos(hwnd, HWND_TOP, 0,0, ew,eh, SWP_NOMOVE|SWP_SHOWWINDOW);
    else { RECT rc={0,0,ew,eh}; AdjustWindowRect(&rc, style, FALSE);
           SetWindowPos(hwnd,nullptr,0,0,rc.right-rc.left,rc.bottom-rc.top,SWP_NOMOVE|SWP_NOZORDER);
           ShowWindow(hwnd, SW_SHOW); }
    UpdateWindow(hwnd);
    return e;
}
void editor_close(void* h){ EditorWin* e=(EditorWin*)h; if(!e)return; if(e->inst)e->inst->closeEditor(); if(e->hwnd)DestroyWindow(e->hwnd); delete e; }
void editor_idle(void* h){ EditorWin* e=(EditorWin*)h; if(e&&e->inst&&e->hwnd&&IsWindowVisible(e->hwnd)) e->inst->idleEditor(); }
bool editor_alive(void* h){ EditorWin* e=(EditorWin*)h; return e&&e->hwnd&&IsWindow(e->hwnd); }
void editor_set_bounds(void* h,int x,int y,int w,int hh){
    EditorWin* e=(EditorWin*)h; if(!e||!e->hwnd) return;
    if(x==e->lx && y==e->ly && w==e->lw && hh==e->lh) return;   // unchanged -> don't repaint (keeps popups open)
    e->lx=x; e->ly=y; e->lw=w; e->lh=hh;
    MoveWindow(e->hwnd,x,y,w,hh,TRUE);
}
void editor_show(void* h,bool s){
    EditorWin* e=(EditorWin*)h; if(!e||!e->hwnd) return;
    const int want = s?1:0;
    if(e->shown==want) return;                                  // unchanged -> skip
    e->shown=want;
    ShowWindow(e->hwnd, s?SW_SHOW:SW_HIDE);
}
bool editor_native_size(void* h,int* w,int* hh){ EditorWin* e=(EditorWin*)h; if(!e||e->nw<=0)return false; if(w)*w=e->nw; if(hh)*hh=e->nh; return true; }
}} // namespace

// ============================================================================
//  Linux / X11  (parent the plugin's X11 window as a child of SDL's X11 window)
// ============================================================================
#elif defined(__linux__) && defined(SDL_VIDEO_DRIVER_X11)
#include <X11/Xlib.h>

namespace PatchKnob { namespace hostwin {
namespace {
struct EditorWin {
    Display*                        dpy   = nullptr;
    Window                          child = 0;
    PatchKnob::engine::IPluginInstance* inst  = nullptr;
    int                             nw = 0, nh = 0;
};
bool sdl_x11(void* sdlWindow, Display** dpy, Window* win) {
    if (!sdlWindow) return false;
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(reinterpret_cast<SDL_Window*>(sdlWindow), &wm)) return false;
    if (wm.subsystem != SDL_SYSWM_X11) return false;
    *dpy = wm.info.x11.display; *win = wm.info.x11.window; return true;
}
} // namespace

void* editor_open(PatchKnob::engine::IPluginInstance* inst, const char* /*title*/, void* sdlWindow) {
    if (!inst || !inst->hasEditor()) return nullptr;
    Display* dpy=nullptr; Window parent=0;
    if (!sdl_x11(sdlWindow, &dpy, &parent) || !dpy) return nullptr;  // no X11 (e.g. KMSDRM)

    EditorWin* e = new EditorWin(); e->inst = inst; e->dpy = dpy;
    int scr = DefaultScreen(dpy);
    e->child = XCreateSimpleWindow(dpy, parent, 0,0, 480,320, 0,
                                   BlackPixel(dpy,scr), BlackPixel(dpy,scr));
    XMapWindow(dpy, e->child);
    XFlush(dpy);
    // On X11 the VST3 platform type is kPlatformTypeX11EmbedWindowID; the handle
    // passed to attached() IS the X11 window id (as a void*-sized value).
    if (!inst->openEditor(reinterpret_cast<PatchKnob::engine::NativeWindowHandle>(e->child))) {
        XDestroyWindow(dpy, e->child); delete e; return nullptr;
    }
    int ew=480, eh=320; inst->getEditorSize(ew, eh);
    if (ew<60) ew=480; if (eh<60) eh=320; e->nw=ew; e->nh=eh;
    XResizeWindow(dpy, e->child, ew, eh); XFlush(dpy);
    return e;
}
void editor_close(void* h){ EditorWin* e=(EditorWin*)h; if(!e)return; if(e->inst)e->inst->closeEditor();
    if(e->dpy&&e->child){ XDestroyWindow(e->dpy,e->child); XFlush(e->dpy);} delete e; }
void editor_idle(void* h){ EditorWin* e=(EditorWin*)h; if(e&&e->inst){ e->inst->idleEditor(); if(e->dpy) XFlush(e->dpy);} }
bool editor_alive(void* h){ EditorWin* e=(EditorWin*)h; return e&&e->child; }
void editor_set_bounds(void* h,int x,int y,int w,int hh){ EditorWin* e=(EditorWin*)h;
    if(e&&e->dpy&&e->child){ XMoveResizeWindow(e->dpy,e->child,x,y,(unsigned)w,(unsigned)hh); XFlush(e->dpy);} }
void editor_show(void* h,bool s){ EditorWin* e=(EditorWin*)h; if(e&&e->dpy&&e->child){
    if(s) XMapWindow(e->dpy,e->child); else XUnmapWindow(e->dpy,e->child); XFlush(e->dpy);} }
bool editor_native_size(void* h,int* w,int* hh){ EditorWin* e=(EditorWin*)h; if(!e||e->nw<=0)return false;
    if(w)*w=e->nw; if(hh)*hh=e->nh; return true; }
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
void  editor_show(void*, bool) {}
bool  editor_native_size(void*, int*, int*) { return false; }
}}
#endif
