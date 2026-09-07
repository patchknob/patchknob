// Shim so the existing `#include <SDL.h>` in 71 files resolves to SDL3 plus the
// SDL2-name compatibility layer.  This directory is put AHEAD of the real SDL
// include path only when PATCHKNOB_SDL3 is on; the SDL2 build never sees it.
#pragma once
#include "../sdl3_compat.h"
