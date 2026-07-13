//----------------------------------------------------------------------------
//
//  ui/rack/plugin_editor.h  --  seq24 Windows port, hosted-plugin editor window.
//
//  Opens the *native* editor GUI of an already-instantiated IPluginInstance by
//  embedding it into a plain Gtk::Window.  On the gtkmm-2.4 / GDK-Win32 stack
//  the plugin editor is a Win32 child window, so the embedding recipe is:
//
//      1. create a Gtk::Window and realize() it so GDK creates its HWND,
//      2. GDK_WINDOW_HWND( gtk_widget_get_window(...) )  ->  the parent HWND,
//      3. IPluginInstance::getEditorSize()   ->  size the window,
//      4. IPluginInstance::openEditor( (void*)hwnd )  ->  plugin parents its
//         editor into our window,
//      5. a Glib timer calls IPluginInstance::idleEditor() at ~60 Hz so VST2
//         GUIs (which need effEditIdle pumped) animate / repaint,
//      6. IPluginInstance::closeEditor() when the window is closed.
//
//  Header-only so the single seq24-side translation unit (src/rackapp.cpp) that
//  the top-level glob compiles carries it directly; <windows.h> is confined to
//  TUs that actually include this header (rackapp.cpp) and never reaches the
//  engine-free rack_test build.
//
//  Public entry point:
//      void seq24::rack::open_plugin_editor( IPluginInstance* );
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_RACK_PLUGIN_EDITOR_H
#define SEQ24_UI_RACK_PLUGIN_EDITOR_H

#include <gtkmm/window.h>
#include <glibmm/main.h>

// Keep the Win32 surface minimal and avoid the min()/max() macros clashing with
// the C++ standard library.  gdkwin32.h pulls in <windows.h> and defines the
// GDK_WINDOW_HWND accessor used below.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <gdk/gdkwin32.h>

#include "../../engine/plugin_api.h"    // seq24::engine::IPluginInstance

namespace seq24 {
namespace rack {

//! A self-owning top-level window that hosts one plugin's native editor.
//! Created via open_plugin_editor(); it deletes itself when closed.
class PluginEditorWindow : public Gtk::Window
{
public:
    explicit PluginEditorWindow( seq24::engine::IPluginInstance* a_inst )
        : m_inst( a_inst ), m_opened( false )
    {
        set_title( m_inst ? ( m_inst->descriptor().name + "  -  Editor" )
                          : std::string( "Plugin Editor" ) );
        // Plugin editors are fixed-size Win32 children; don't let GTK stretch us.
        set_resizable( false );

        int w = 0, h = 0;
        if ( m_inst )
            m_inst->getEditorSize( w, h );
        if ( w <= 0 ) w = 400;
        if ( h <= 0 ) h = 300;
        set_default_size( w, h );
        set_size_request( w, h );

        // Delete ourselves (deferred, off the signal stack) when closed.
        signal_delete_event().connect(
            sigc::mem_fun( *this, &PluginEditorWindow::on_delete_event ) );

        // Realize now so GDK creates the underlying HWND before we embed.
        realize();
        embed();
    }

    virtual ~PluginEditorWindow() { stop(); }

private:
    // Parent the plugin's editor into our realized window and start the pump.
    void embed()
    {
        if ( !m_inst || m_opened )
            return;
        if ( !m_inst->hasEditor() )
            return;

        GdkWindow* gw = gtk_widget_get_window( GTK_WIDGET( gobj() ) );
        if ( !gw )
            return;

        HWND hwnd = (HWND) GDK_WINDOW_HWND( gw );
        if ( !hwnd )
            return;

        m_inst->openEditor( (void*) hwnd );
        m_opened = true;

        // Some VST2 editors only report a valid size after effEditOpen; adopt it.
        int w = 0, h = 0;
        m_inst->getEditorSize( w, h );
        if ( w > 0 && h > 0 )
        {
            set_size_request( w, h );
            resize( w, h );
        }

        // ~60 Hz idle pump for VST2 GUIs (effEditIdle).  VST3 idleEditor() is a
        // no-op (driven by the Win32 loop) so this is harmless there too.
        m_timer = Glib::signal_timeout().connect(
            sigc::mem_fun( *this, &PluginEditorWindow::on_idle ), 16 );
    }

    bool on_idle()
    {
        if ( m_inst )
            m_inst->idleEditor();
        return true;   // keep pumping
    }

    // Stop the pump and tell the plugin to un-parent its editor.  Must run while
    // our HWND is still alive, i.e. before the window object is destroyed.
    void stop()
    {
        if ( m_timer.connected() )
            m_timer.disconnect();
        if ( m_opened && m_inst )
        {
            m_inst->closeEditor();
            m_opened = false;
        }
    }

    bool on_delete_event( GdkEventAny* )
    {
        stop();
        hide();
        // Defer the delete off the current signal emission for safety.
        Glib::signal_idle().connect(
            sigc::bind( sigc::ptr_fun( &PluginEditorWindow::deferred_delete ),
                        this ) );
        return true;   // we handled the close; don't run the default destroy
    }

    static bool deferred_delete( PluginEditorWindow* self )
    {
        delete self;
        return false;  // one-shot
    }

    seq24::engine::IPluginInstance* m_inst;
    bool                            m_opened;
    sigc::connection                m_timer;
};

//! Open (or no-op) the native editor for `inst`.  The window owns itself.
inline void open_plugin_editor( seq24::engine::IPluginInstance* inst )
{
    if ( !inst )
        return;
    if ( !inst->hasEditor() )
        return;
    // Heap-allocated + self-deleting on close (see on_delete_event).
    PluginEditorWindow* win = new PluginEditorWindow( inst );
    win->show();
}

} // namespace rack
} // namespace seq24

#endif // SEQ24_UI_RACK_PLUGIN_EDITOR_H
