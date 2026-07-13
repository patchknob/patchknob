//----------------------------------------------------------------------------
//
//  rackapp.h  --  seq24 Windows port, VST "rack" coordinator entry points.
//
//  This is the seam the main-window coordinator calls from its menus.  It lives
//  flat in src/ so the top-level CMake glob (src/*.cpp) compiles rackapp.cpp
//  straight into seq24 -- no change to the (frozen) top-level CMakeLists and no
//  extra library to link.  rackapp.cpp is self-contained: it only references
//  symbols already in seq24's link (audio_app glue, the engine libs, gtkmm) and
//  the header-only rack UI in src/ui/rack/.
//
//      seq24::rack::show_plugin_browser( track )
//          Open the plugin browser for `track`.  The scan is SLOW (out of
//          process), so it runs once on a background std::thread; the window
//          shows "scanning..." and is populated when the scan completes.  The
//          result is cached in a static so re-opening is instant.
//
//      seq24::rack::open_track_editor( track )
//          Open the native editor of the instrument currently assigned to
//          `track` (audio_app_graph()->track(track)->instrument()).
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_RACKAPP_H
#define SEQ24_RACKAPP_H

namespace seq24 {
namespace rack {

//! Open the plugin browser assigning to mixer `track`.  Double-click / "Load as
//! Instrument" calls audio_app_set_track_instrument(); "Add as FX" calls
//! audio_app_add_track_fx().
void show_plugin_browser( int track );

//! Open the native editor window for `track`'s current instrument (if any).
void open_track_editor( int track );

} // namespace rack
} // namespace seq24

#endif // SEQ24_RACKAPP_H
