//----------------------------------------------------------------------------
//  Out-of-line half of the SDL2-on-SDL3 shim.  See sdl3_compat.h.
//----------------------------------------------------------------------------
#include "sdl3_compat.h"

#include <vector>

SDL_Window* pk_sdl3_main_window = nullptr;

namespace {
// Converting int rects/points to float ones is the one place the shim could
// allocate per frame.  These grid/ruler batches are drawn every repaint, so
// reuse one buffer per thread instead -- the whole point of the plural calls is
// to keep the driver's draw-call count down, and an allocation per call would
// hand back on the CPU what the batching saves on the GPU.
thread_local std::vector<SDL_FRect>  g_frects;
thread_local std::vector<SDL_FPoint> g_fpoints;
}

int SDL_RenderFillRects(SDL_Renderer* r, const SDL_Rect* q, int count)
{
    if (!q || count <= 0) return 0;
    g_frects.clear();
    g_frects.reserve((size_t) count);
    for (int i = 0; i < count; ++i) g_frects.push_back(pk_frect(q[i]));
    return SDL_RenderFillRects(r, g_frects.data(), count) ? 0 : -1;
}

int SDL_RenderDrawLines(SDL_Renderer* r, const SDL_Point* p, int count)
{
    if (!p || count <= 0) return 0;
    g_fpoints.clear();
    g_fpoints.reserve((size_t) count);
    for (int i = 0; i < count; ++i)
        g_fpoints.push_back(SDL_FPoint{ (float) p[i].x, (float) p[i].y });
    return SDL_RenderLines(r, g_fpoints.data(), count) ? 0 : -1;
}

int SDL_RenderDrawPoints(SDL_Renderer* r, const SDL_Point* p, int count)
{
    if (!p || count <= 0) return 0;
    g_fpoints.clear();
    g_fpoints.reserve((size_t) count);
    for (int i = 0; i < count; ++i)
        g_fpoints.push_back(SDL_FPoint{ (float) p[i].x, (float) p[i].y });
    return SDL_RenderPoints(r, g_fpoints.data(), count) ? 0 : -1;
}
