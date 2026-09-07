//----------------------------------------------------------------------------
//  sdl3_compat.h -- SDL2 spellings on top of SDL3.
//
//  The UI is ~1900 SDL call sites across 71 files.  Rewriting all of them in
//  one commit would leave the tree unbuildable for the whole migration and make
//  every unrelated bug impossible to bisect.  Instead this header provides the
//  SDL2 names the code already uses, implemented with SDL3, so the port lands
//  in three stages:
//
//    1. build against SDL3 THROUGH this shim (tree stays green, no view edits)
//    2. move call sites onto native SDL3 spellings a file at a time
//    3. delete this header
//
//  WHAT THIS SHIM CANNOT COVER, and therefore still needs hand edits:
//
//    * Window events.  SDL2 had one SDL_WINDOWEVENT carrying a sub-code in
//      `ev.window.event`; SDL3 promoted each to its own event type.  A macro
//      cannot turn one switch label into several.
//    * `ev.motion.x/y` are floats now.  Struct members, not names.
//    * SDL_GetWindowWMInfo / SDL_SysWMinfo are gone; native handles come from
//      the window property store.
//    * The audio API was redesigned (SDL_OpenAudioDevice -> audio streams).
//      Only the fallback audio path uses it; PortAudio is the real backend.
//
//  Everything below is a rename or a rect-type adaptation, which is the bulk.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDL3_COMPAT_H
#define PATCHKNOB_SDL3_COMPAT_H

#include <SDL3/SDL.h>

// ---- booleans --------------------------------------------------------------
// SDL_bool is plain bool in SDL3; the old enumerators are gone.
#define SDL_TRUE  true
#define SDL_FALSE false
typedef bool SDL_bool;

// ---- RETURN CONVENTION -----------------------------------------------------
// The nastiest part of this migration, because it does not produce a compile
// error.  SDL2 returned int, 0 for success and negative for failure; SDL3
// returns bool, TRUE for success.  Existing code spelled `if (SDL_Init(..) != 0)`
// which under SDL3 reads a SUCCESSFUL init as a failure and quits with an empty
// error string.  Every such call has to keep the SDL2 convention until its call
// site is converted, so wrap it and hide the real one behind a macro.  Each
// inline is defined BEFORE its macro so the body still reaches the real symbol.
static inline int pk_sdl_init(SDL_InitFlags f)      { return SDL_Init(f) ? 0 : -1; }
#define SDL_Init pk_sdl_init
static inline int pk_sdl_init_sub(SDL_InitFlags f)  { return SDL_InitSubSystem(f) ? 0 : -1; }
#define SDL_InitSubSystem pk_sdl_init_sub
static inline int pk_sdl_save_bmp(SDL_Surface* s, const char* f) {
    return SDL_SaveBMP(s, f) ? 0 : -1;
}
#define SDL_SaveBMP pk_sdl_save_bmp
static inline int pk_sdl_set_clip(const char* t) { return SDL_SetClipboardText(t) ? 0 : -1; }
#define SDL_SetClipboardText pk_sdl_set_clip

// ---- surfaces / cursors ----------------------------------------------------
#define SDL_FreeSurface   SDL_DestroySurface
#define SDL_FreeCursor    SDL_DestroyCursor
#define SDL_FillRect      SDL_FillSurfaceRect

// SDL3 dropped the width/height/depth/mask forms: format alone now describes
// the layout, and pitch is derived unless supplied.
static inline SDL_Surface* SDL_CreateRGBSurfaceWithFormat(
        Uint32 /*flags*/, int w, int h, int /*depth*/, SDL_PixelFormat fmt) {
    return SDL_CreateSurface(w, h, fmt);
}
static inline SDL_Surface* SDL_CreateRGBSurfaceWithFormatFrom(
        void* pixels, int w, int h, int /*depth*/, int pitch, SDL_PixelFormat fmt) {
    return SDL_CreateSurfaceFrom(w, h, fmt, pixels, pitch);
}

// ---- rect helpers ----------------------------------------------------------
#define SDL_IntersectRect SDL_GetRectIntersection
#define SDL_UnionRect     SDL_GetRectUnion

// ---- renderer state --------------------------------------------------------
#define SDL_RenderSetClipRect   SDL_SetRenderClipRect
#define SDL_RenderGetClipRect   SDL_GetRenderClipRect
#define SDL_RenderIsClipEnabled SDL_RenderClipEnabled
#define SDL_RenderSetScale      SDL_SetRenderScale
#define SDL_RenderGetWindow     SDL_GetRenderWindow

// SDL2's logical size is SDL3's "logical presentation".  LETTERBOX reproduces
// SDL2's behaviour of scaling to fit and keeping the aspect ratio, which is
// what the mouse-coordinate mapping in this UI assumes.
static inline int SDL_RenderSetLogicalSize(SDL_Renderer* r, int w, int h) {
    return SDL_SetRenderLogicalPresentation(r, w, h,
               SDL_LOGICAL_PRESENTATION_LETTERBOX) ? 0 : -1;
}
static inline int SDL_RenderGetLogicalSize(SDL_Renderer* r, int* w, int* h) {
    SDL_RendererLogicalPresentation mode;
    return SDL_GetRenderLogicalPresentation(r, w, h, &mode) ? 0 : -1;
}
// NOT SDL_GetCurrentRenderOutputSize: that reports the size AFTER logical
// presentation is applied, i.e. the logical size we ourselves just set, so the
// resize handler fed its own output back in and the window never grew to the
// display (a maximised window reported 1627x1017, then 1279x800).  SDL2's
// SDL_GetRendererOutputSize meant the raw pixel size, which is SDL3's
// SDL_GetRenderOutputSize.
static inline int SDL_GetRendererOutputSize(SDL_Renderer* r, int* w, int* h) {
    return SDL_GetRenderOutputSize(r, w, h) ? 0 : -1;
}
static inline int SDL_RenderWindowToLogical(SDL_Renderer* r,
                                            int wx, int wy, float* lx, float* ly) {
    return SDL_RenderCoordinatesFromWindow(r, (float) wx, (float) wy, lx, ly) ? 0 : -1;
}

// ---- drawing: SDL3 takes float rects/points --------------------------------
// The UI is laid out in integers, so adapt at the boundary rather than churn
// every rect in every view.  These are inline and fold to a few stores.
static inline SDL_FRect pk_frect(const SDL_Rect& q) {
    return SDL_FRect{ (float) q.x, (float) q.y, (float) q.w, (float) q.h };
}

static inline int SDL_RenderFillRect(SDL_Renderer* r, const SDL_Rect* q) {
    if (!q) return SDL_RenderFillRect(r, (const SDL_FRect*) nullptr) ? 0 : -1;
    const SDL_FRect f = pk_frect(*q);
    return SDL_RenderFillRect(r, &f) ? 0 : -1;
}
static inline int SDL_RenderDrawRect(SDL_Renderer* r, const SDL_Rect* q) {
    if (!q) return SDL_RenderRect(r, nullptr) ? 0 : -1;
    const SDL_FRect f = pk_frect(*q);
    return SDL_RenderRect(r, &f) ? 0 : -1;
}
static inline int SDL_RenderDrawLine(SDL_Renderer* r, int x1, int y1, int x2, int y2) {
    return SDL_RenderLine(r, (float) x1, (float) y1, (float) x2, (float) y2) ? 0 : -1;
}
static inline int SDL_RenderDrawPoint(SDL_Renderer* r, int x, int y) {
    return SDL_RenderPoint(r, (float) x, (float) y) ? 0 : -1;
}
static inline int SDL_RenderCopy(SDL_Renderer* r, SDL_Texture* t,
                                 const SDL_Rect* src, const SDL_Rect* dst) {
    SDL_FRect s, d;
    if (src) s = pk_frect(*src);
    if (dst) d = pk_frect(*dst);
    return SDL_RenderTexture(r, t, src ? &s : nullptr, dst ? &d : nullptr) ? 0 : -1;
}

// The plural forms are the batching-friendly ones, so keep them cheap: convert
// into a reusable scratch buffer instead of allocating per call.
int SDL_RenderFillRects(SDL_Renderer* r, const SDL_Rect* q, int count);
int SDL_RenderDrawLines(SDL_Renderer* r, const SDL_Point* p, int count);
int SDL_RenderDrawPoints(SDL_Renderer* r, const SDL_Point* p, int count);

// ---- renderer creation -----------------------------------------------------
// SDL3 takes a driver NAME instead of an index, and vsync is no longer a
// creation flag -- it is set afterwards.  This is exactly the SDL3 spelling the
// user's notes referred to (SDL_SetRenderVSync), which does not exist in SDL2.
#define SDL_RENDERER_ACCELERATED   0x1u
#define SDL_RENDERER_PRESENTVSYNC  0x2u
#define SDL_RENDERER_SOFTWARE      0x4u
#define SDL_RENDERER_TARGETTEXTURE 0x8u

static inline SDL_Renderer* SDL_CreateRenderer(SDL_Window* win, int /*index*/,
                                               Uint32 flags) {
    SDL_Renderer* r = SDL_CreateRenderer(win, nullptr);
    if (r) SDL_SetRenderVSync(r, (flags & SDL_RENDERER_PRESENTVSYNC) ? 1 : 0);
    return r;
}

// ---- window creation -------------------------------------------------------
// SDL3 dropped the x/y arguments; a centred window is the default.
#define SDL_WINDOW_ALLOW_HIGHDPI     SDL_WINDOW_HIGH_PIXEL_DENSITY
#define SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
static inline SDL_Window* SDL_CreateWindow(const char* title, int /*x*/, int /*y*/,
                                           int w, int h, Uint64 flags) {
    return SDL_CreateWindow(title, w, h, flags);
}

// ---- event type names ------------------------------------------------------
// Value renames only.  SDL_WINDOWEVENT is deliberately NOT defined here: it has
// no single SDL3 equivalent, so any remaining use must fail to compile and be
// converted by hand rather than silently mis-dispatch.
#define SDL_QUIT             SDL_EVENT_QUIT
#define SDL_KEYDOWN          SDL_EVENT_KEY_DOWN
#define SDL_KEYUP            SDL_EVENT_KEY_UP
#define SDL_TEXTINPUT        SDL_EVENT_TEXT_INPUT
#define SDL_MOUSEMOTION      SDL_EVENT_MOUSE_MOTION
#define SDL_MOUSEBUTTONDOWN  SDL_EVENT_MOUSE_BUTTON_DOWN
#define SDL_MOUSEBUTTONUP    SDL_EVENT_MOUSE_BUTTON_UP
#define SDL_MOUSEWHEEL       SDL_EVENT_MOUSE_WHEEL
#define SDL_FINGERDOWN       SDL_EVENT_FINGER_DOWN
#define SDL_FINGERMOTION     SDL_EVENT_FINGER_MOTION

// ---- text input ------------------------------------------------------------
// SDL3 scopes text input to a window.  The UI has exactly one, tracked here so
// the SDL2-shaped no-argument calls keep working.
extern SDL_Window* pk_sdl3_main_window;
static inline void pk_sdl3_set_main_window(SDL_Window* w) { pk_sdl3_main_window = w; }
static inline int SDL_StartTextInput(void) {
    return (pk_sdl3_main_window && SDL_StartTextInput(pk_sdl3_main_window)) ? 0 : -1;
}
static inline int SDL_StopTextInput(void) {
    return (pk_sdl3_main_window && SDL_StopTextInput(pk_sdl3_main_window)) ? 0 : -1;
}

// ---- keyboard --------------------------------------------------------------
// SDL3 namespaced the modifier flags and made the letter keycodes uppercase.
#define KMOD_NONE   SDL_KMOD_NONE
#define KMOD_CTRL   SDL_KMOD_CTRL
#define KMOD_SHIFT  SDL_KMOD_SHIFT
#define KMOD_ALT    SDL_KMOD_ALT
#define KMOD_GUI    SDL_KMOD_GUI
#define KMOD_LCTRL  SDL_KMOD_LCTRL
#define KMOD_RCTRL  SDL_KMOD_RCTRL

#define SDLK_a SDLK_A
#define SDLK_b SDLK_B
#define SDLK_c SDLK_C
#define SDLK_d SDLK_D
#define SDLK_e SDLK_E
#define SDLK_f SDLK_F
#define SDLK_g SDLK_G
#define SDLK_h SDLK_H
#define SDLK_i SDLK_I
#define SDLK_j SDLK_J
#define SDLK_k SDLK_K
#define SDLK_l SDLK_L
#define SDLK_m SDLK_M
#define SDLK_n SDLK_N
#define SDLK_o SDLK_O
#define SDLK_p SDLK_P
#define SDLK_q SDLK_Q
#define SDLK_r SDLK_R
#define SDLK_s SDLK_S
#define SDLK_t SDLK_T
#define SDLK_u SDLK_U
#define SDLK_v SDLK_V
#define SDLK_w SDLK_W
#define SDLK_x SDLK_X
#define SDLK_y SDLK_Y
#define SDLK_z SDLK_Z

// ---- window events ---------------------------------------------------------
// SDL2 delivered one SDL_WINDOWEVENT carrying a sub-code; SDL3 gave each its own
// event type.  Neither shape can be macro'd into the other, so both builds
// classify the event through this instead and the caller switches on the result.
enum {
    PK_WIN_NONE       = 0,
    PK_WIN_SIZE       = 1u << 0,   // size changed: relayout + logical size
    PK_WIN_DEACTIVATE = 1u << 1,   // focus lost / hidden / minimised
    PK_WIN_UNDRAWABLE = 1u << 2,   // hidden / minimised: stop drawing entirely
    PK_WIN_ACTIVATE   = 1u << 3,   // shown / restored / maximised
    PK_WIN_EXPOSED    = 1u << 4
};
static inline unsigned pk_window_event(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return PK_WIN_SIZE;
    case SDL_EVENT_WINDOW_FOCUS_LOST:         return PK_WIN_DEACTIVATE;
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:          return PK_WIN_DEACTIVATE | PK_WIN_UNDRAWABLE;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:          return PK_WIN_ACTIVATE;
    case SDL_EVENT_WINDOW_EXPOSED:            return PK_WIN_EXPOSED;
    default:                                  return PK_WIN_NONE;
    }
}

// SDL3 removed multi-finger gesture events; apps derive them from raw touches.
// The gesture block is mobile-only, so it is compiled out until it is rewritten.

// SDL3 flattened SDL_Keysym into SDL_KeyboardEvent.  A macro cannot rewrite
// `ev.key.keysym.sym` (it is two member accesses, not a name), so the accesses
// themselves are spelled through these and resolve per build.
#define PK_KEYSYM_SYM      key
#define PK_KEYSYM_MOD      mod
#define PK_KEYSYM_SCANCODE scancode

#define SDLK_BACKQUOTE SDLK_GRAVE

// ---- cursors ---------------------------------------------------------------
#define SDL_SYSTEM_CURSOR_ARROW  SDL_SYSTEM_CURSOR_DEFAULT
#define SDL_SYSTEM_CURSOR_SIZENS SDL_SYSTEM_CURSOR_NS_RESIZE
#define SDL_SYSTEM_CURSOR_SIZEWE SDL_SYSTEM_CURSOR_EW_RESIZE

// ---- renderer misc ---------------------------------------------------------
#define SDL_RenderFlush SDL_FlushRenderer

// SDL3 always batches -- the hint is gone.  Keep the name pointing at a string
// SDL will simply not recognise, so the existing SDL_SetHint call is a no-op
// rather than a compile error.  (Setting it was never load-bearing: it asked
// for the behaviour SDL3 now guarantees.)
#ifndef SDL_HINT_RENDER_BATCHING
#define SDL_HINT_RENDER_BATCHING "SDL_RENDER_BATCHING"
#endif

// ---- mouse -----------------------------------------------------------------
// SDL3 reports sub-pixel positions.  The UI works in integer logical pixels, so
// take the float result and truncate at the boundary.
static inline SDL_MouseButtonFlags SDL_GetMouseState(int* x, int* y) {
    float fx = 0.f, fy = 0.f;
    const SDL_MouseButtonFlags b = SDL_GetMouseState(&fx, &fy);
    if (x) *x = (int) fx;
    if (y) *y = (int) fy;
    return b;
}

// ---- pixel formats ---------------------------------------------------------
// SDL3 split the format ENUM from the format DETAILS table that MapRGBA needs.
static inline Uint32 SDL_MapRGBA(SDL_PixelFormat fmt, Uint8 r, Uint8 g, Uint8 b,
                                 Uint8 a) {
    return SDL_MapRGBA(SDL_GetPixelFormatDetails(fmt), nullptr, r, g, b, a);
}

// ---- geometry vertex colour ------------------------------------------------
// SDL_Vertex carries a normalised float colour in SDL3 and a byte colour in
// SDL2.  gui.cpp builds vertices for the glyph atlas and has to compile under
// both, so it goes through this; the SDL2 build defines the same name in gui.h.
static inline SDL_FColor pk_vertex_color(Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return SDL_FColor{ r / 255.f, g / 255.f, b / 255.f, a / 255.f };
}

// ---- readback --------------------------------------------------------------
// SDL3 returns a freshly allocated surface instead of filling a caller buffer.
// Convert into the caller's pitch-packed buffer so the recorder's downscaler,
// which owns its own layout, does not have to change.
static inline int SDL_RenderReadPixels(SDL_Renderer* r, const SDL_Rect* rect,
                                       SDL_PixelFormat fmt, void* pixels,
                                       int pitch) {
    SDL_Surface* got = SDL_RenderReadPixels(r, rect);
    if (!got) return -1;
    SDL_Surface* want = (got->format == fmt) ? got
                                             : SDL_ConvertSurface(got, fmt);
    int rc = -1;
    if (want) {
        const int bpp  = SDL_BYTESPERPIXEL(fmt);
        const int rows = want->h;
        const int span = want->w * bpp;
        for (int y = 0; y < rows; ++y)
            SDL_memcpy((Uint8*) pixels + (size_t) y * pitch,
                       (const Uint8*) want->pixels + (size_t) y * want->pitch,
                       (size_t) span);
        rc = 0;
        if (want != got) SDL_DestroySurface(want);
    }
    SDL_DestroySurface(got);
    return rc;
}

// ---- misc ------------------------------------------------------------------
// SDL_GetRendererInfo is gone; only the backend NAME was ever read here.
static inline const char* SDL_GetRendererNameCompat(SDL_Renderer* r) {
    return SDL_GetRendererName(r);
}

#endif
