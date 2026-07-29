//----------------------------------------------------------------------------
//  sdlui/plugin_editor_window.h
//
//  Hosts a plugin's NATIVE editor GUI as a CHILD window of the SDL main window,
//  so it appears embedded inside the app (framed by an ui::Window widget which
//  positions it).  The plugin renders with its own toolkit into the child HWND;
//  input reaches it natively.  This is the "translation layer": the plugin
//  believes it is in an ordinary parent window, and we place that window inside
//  our SDL canvas and move/resize it to track a ui::Window.
//
//  Pass parentHwnd = the SDL window's HWND (SDL_GetWindowWMInfo) to embed; pass
//  null to open a separate top-level window instead.  Windows-only; the calls
//  are no-ops elsewhere (use the SDL parameter panel on a framebuffer target).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PLUGIN_EDITOR_WINDOW_H
#define PATCHKNOB_SDLUI_PLUGIN_EDITOR_WINDOW_H

namespace PatchKnob { namespace engine { class IPluginInstance; } }

namespace PatchKnob { namespace hostwin {

//! Open `inst`'s editor embedded in `sdlWindow` (an SDL_Window*, passed as void*
//! so this header needs no SDL dependency).  On Windows the editor is a child
//! HWND of the SDL window; on Linux/X11 a child X11 window.  Returns an opaque
//! handle, or nullptr (e.g. no editor, or a windowless target like KMSDRM).
void* editor_open(PatchKnob::engine::IPluginInstance* inst, const char* title, void* sdlWindow);

//! Close + destroy (idempotent).
void  editor_close(void* handle);

//! VST2 idle pump; call periodically from the UI loop.
void  editor_idle(void* handle);

//! False once destroyed (host reaps the handle).
bool  editor_alive(void* handle);

//! Move/resize the embedded child window (physical client px of the parent).
void  editor_set_bounds(void* handle, int x, int y, int w, int h);

//! Show / hide the child window (follows the ui::Window's visibility).
void  editor_show(void* handle, bool show);

//! The editor's native pixel size (from the plugin).  False if unknown.
bool  editor_native_size(void* handle, int* w, int* h);

}} // namespace PatchKnob::hostwin

#endif