//----------------------------------------------------------------------------
//
//  patchbayapp.h  --  seq24 Windows port, modular patchbay coordinator seam.
//
//  This is the seam the main-window coordinator calls from its menus to open the
//  modular node-editor / patchbay.  It lives flat in src/ so the top-level CMake
//  glob (src/*.cpp) compiles patchbayapp.cpp straight into seq24 -- no change to
//  the (frozen) top-level CMakeLists and no extra library to link.
//
//  patchbayapp.cpp is self-contained: it references only symbols already in
//  seq24's link (the VST "rack" coordinator in rackapp.h, the runtime palette,
//  gtkmm) and the HEADER-ONLY patchbay UI in src/ui/patchbay/.  It owns the
//  window lifetime (heap window, self-deletes on close), so the call is
//  fire-and-forget.
//
//      seq24::patchbay::show_patchbay()
//          Open the node-editor window populated with a representative default
//          patch.  Right-click "Add Module" is wired to the plugin browser
//          (seq24::rack::show_plugin_browser); right-click "Open Editor" is
//          wired to seq24::rack::open_track_editor.  The connect / disconnect /
//          remove request signals are the seam the engine graph binding (built
//          in parallel) hooks into to mutate the real PatchGraph.
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_PATCHBAYAPP_H
#define SEQ24_PATCHBAYAPP_H

namespace seq24 {
namespace patchbay {

//! Open (or focus) the modular node-editor / patchbay window.
void show_patchbay();

} // namespace patchbay
} // namespace seq24

#endif // SEQ24_PATCHBAYAPP_H
