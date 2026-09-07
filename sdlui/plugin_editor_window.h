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
//  Windows (HWND) and Linux/X11 (XID) embed as a child of the SDL window.
//
//  On Linux the mode is decided at RUNTIME, because a plugin editor is always an
//  X11 window (VST3's only Linux platform type is kPlatformTypeX11EmbedWindowID;
//  no plugin standard can embed into a Wayland surface) while SDL may be on any
//  backend:
//    * SDL on X11      -> embedded child window, as on Windows.
//    * SDL on Wayland  -> the editor opens as its own TOP-LEVEL X11 window
//                         through XWayland, on a private X connection.  The
//                         in-app frame then only titles / hides / closes it.
//    * no X server     -> editor_open() returns null and the caller falls back
//                         to the portable SDL parameter panel.
//  PATCHKNOB_PLUGIN_GUI=detached|embedded forces either Linux mode.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PLUGIN_EDITOR_WINDOW_H
#define PATCHKNOB_SDLUI_PLUGIN_EDITOR_WINDOW_H

namespace PatchKnob { namespace engine { class IPluginInstance; } }

namespace PatchKnob { namespace hostwin {

//! Open `inst`'s editor embedded in `sdlWindow` (an SDL_Window*, passed as void*
//! so this header needs no SDL dependency).  On Windows the editor is a child
//! HWND of the SDL window; on Linux/X11 a child X11 window.  Returns an opaque
//! handle, or nullptr (e.g. no editor, or a windowless target like KMSDRM).
//! On Linux under Wayland the editor is a separate top-level XWayland window
//! instead of a child (see the file header); every call below still applies,
//! but set_bounds is ignored for it (the window manager owns its geometry).
void* editor_open(PatchKnob::engine::IPluginInstance* inst, const char* title, void* sdlWindow);

//! Close + destroy (idempotent).
void  editor_close(void* handle);

//! Idle pump; call periodically from the UI loop.  Drives effEditIdle (VST2) /
//! the X11 IRunLoop (VST3), and re-polls the plugin's CURRENT editor size so a
//! plugin-initiated resize (VST3 resizeView, VST2 audioMasterSizeWindow, a
//! "GUI scale" menu) shows up in editor_native_size() -- the shell re-reads it
//! every frame and lets the ui::Window frame follow.
void  editor_idle(void* handle);

//! False once destroyed (host reaps the handle).
bool  editor_alive(void* handle);

//! Move/resize the embedded child window (physical client px of the parent).
void  editor_set_bounds(void* handle, int x, int y, int w, int h);

//! Restrict where the embedded editor may draw (same space as set_bounds:
//! physical client px of the parent SDL window).  The native GUI is clipped to
//! the intersection of this rect and the bounds, with the plugin's content kept
//! aligned to the bounds -- so a frame dragged half off the workspace no longer
//! paints its GUI over the menu bar / transport / dock (SDL-drawn chrome always
//! loses to a native child otherwise).  Never calling this leaves only the
//! implicit parent-window clipping.  Ignored for detached (top-level) editors.
void  editor_set_clip(void* handle, int x, int y, int w, int h);

//! Show / hide the child window (follows the ui::Window's visibility).
void  editor_show(void* handle, bool show);

//! The editor's CURRENT native pixel size (live: editor_idle re-polls the
//! plugin, so this follows plugin-initiated resizes).  False if unknown.
bool  editor_native_size(void* handle, int* w, int* h);

//! True when this editor runs DETACHED (its own top-level OS window -- the
//! Linux/Wayland mode described in the file header) rather than embedded in
//! the SDL window.  Lets the shell present the in-app frame honestly: for a
//! detached editor the frame holds no pixels of the GUI, so it should say so
//! instead of showing an empty body.  False on Windows and for embedded X11.
bool  editor_is_detached(void* handle);

}} // namespace PatchKnob::hostwin

#endif