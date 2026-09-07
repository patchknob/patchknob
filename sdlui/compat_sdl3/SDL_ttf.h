// SDL2_ttf spellings on top of SDL3_ttf.  See sdl3_compat.h for why the port
// goes through a shim rather than one enormous rename commit.
#pragma once
#include "../sdl3_compat.h"
#include <SDL3_ttf/SDL_ttf.h>

// SDL3_ttf takes an explicit byte length (0 = NUL-terminated) so it can render
// slices of a buffer without copying.  Every call here passes whole C strings.
static inline SDL_Surface* TTF_RenderText_Blended(TTF_Font* f, const char* text,
                                                  SDL_Color fg) {
    return TTF_RenderText_Blended(f, text, 0, fg);
}

// Renamed to the Get* convention in SDL3_ttf.
#define TTF_FontHeight  TTF_GetFontHeight
#define TTF_FontAscent  TTF_GetFontAscent
#define TTF_FontDescent TTF_GetFontDescent
#define TTF_GetError    SDL_GetError

// SDL2_ttf returned (w,h) via an int-pair out-params call that took the whole
// NUL-terminated string; SDL3_ttf folded that into the same explicit-length
// TTF_GetStringSize used by TTF_RenderText_Blended above, and flipped the
// success convention from 0 to bool true (see sdl3_compat.h).
static inline int TTF_SizeUTF8(TTF_Font* f, const char* text, int* w, int* h) {
    return TTF_GetStringSize(f, text, 0, w, h) ? 0 : -1;
}

// Same int-vs-bool return flip as SDL_Init -- see sdl3_compat.h.  Silent at
// compile time, fatal at startup.
static inline int pk_ttf_init(void) { return TTF_Init() ? 0 : -1; }
#define TTF_Init pk_ttf_init

// SDL3_ttf reports metrics for a full codepoint rather than a Latin-1 byte.
static inline int TTF_GlyphMetrics(TTF_Font* f, Uint16 ch, int* minx, int* maxx,
                                   int* miny, int* maxy, int* advance) {
    return TTF_GetGlyphMetrics(f, (Uint32) ch, minx, maxx, miny, maxy, advance)
           ? 0 : -1;
}
