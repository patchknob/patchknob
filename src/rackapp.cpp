//----------------------------------------------------------------------------
//
//  rackapp.cpp  --  seq24 Windows port, VST "rack" coordinator (impl).
//
//  Engine-coupled controller for the header-only rack UI.  See rackapp.h.
//
//  Threading model for the (slow, out-of-process) plugin scan:
//    * The very first show_plugin_browser() kicks a single detached std::thread
//      that runs PluginHost::scan().  The window opens immediately in a
//      "scanning..." state -- the UI thread is never blocked.
//    * A short Glib timeout polls a std::atomic "scan done" flag on the UI
//      thread; when set it copies the cached descriptors into the browser.
//    * The scan result is cached in a file-static, so a second open (or any
//      later open) populates instantly with no re-scan.
//
//  Window / session lifetime is owned here (the browser and editor are opened
//  on the heap and self-delete on close), so these calls are fire-and-forget.
//
//-----------------------------------------------------------------------------

#include "rackapp.h"

#include "ui/rack/plugin_browser.h"
#include "ui/rack/plugin_editor.h"

#include "audio_app.h"
#include "engine/host/plugin_host.h"
#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"
#include "engine/plugin_api.h"

#include <glibmm/main.h>
#include <sigc++/sigc++.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace seq24 {
namespace rack {

using seq24::engine::PluginDescriptor;

namespace {

// ---- shared scan cache (populated once, reused for every re-open) -----------
std::mutex                    g_cacheMtx;
std::vector<PluginDescriptor> g_cache;
std::atomic<bool>             g_cacheValid { false };  // scan finished at least once
std::atomic<bool>             g_scanning   { false };  // a scan thread is running

// Background scan worker: runs PluginHost::scan() off the UI thread and stores
// the result in the cache.  One at a time (guarded by g_scanning).
void scan_worker()
{
    std::vector<PluginDescriptor> found;
    if ( seq24::engine::PluginHost* host = seq24::app::audio_app_host() )
        found = host->scan( std::vector<std::string>() );

    {
        std::lock_guard<std::mutex> lk( g_cacheMtx );
        g_cache.swap( found );
    }
    g_cacheValid.store( true, std::memory_order_release );
    g_scanning.store( false, std::memory_order_release );
}

// ---- assign wiring (browser signals -> audio_app) ---------------------------
void do_load_instrument( const PluginDescriptor& d, int track )
{
    seq24::app::audio_app_set_track_instrument( track, d );
}

void do_add_fx( const PluginDescriptor& d, int track )
{
    seq24::app::audio_app_add_track_fx( track, d );
}

// ---- one open browser + its scan poller -------------------------------------
//
// Derives sigc::trackable so that when we delete the session (on window close)
// the poll timeout auto-disconnects -- no dangling timer touching a freed
// browser.  The browser is a heap Gtk::Window that we own and free here.
struct BrowserSession : public sigc::trackable
{
    PluginBrowser* browser;

    explicit BrowserSession( int track )
        : browser( new PluginBrowser( track ) )
    {
        browser->signal_load_instrument().connect(
            sigc::bind( sigc::ptr_fun( &do_load_instrument ), track ) );
        browser->signal_add_fx().connect(
            sigc::bind( sigc::ptr_fun( &do_add_fx ), track ) );
        browser->signal_hide().connect(
            sigc::mem_fun( *this, &BrowserSession::on_hide ) );
    }

    void start()
    {
        if ( g_cacheValid.load( std::memory_order_acquire ) )
        {
            populate_from_cache();
        }
        else
        {
            browser->set_scanning( true );
            // Kick exactly one background scan.
            if ( !g_scanning.exchange( true ) )
                std::thread( &scan_worker ).detach();
            // Poll the "done" flag on the UI thread.
            Glib::signal_timeout().connect(
                sigc::mem_fun( *this, &BrowserSession::on_poll ), 150 );
        }
        browser->show_all();
    }

    void populate_from_cache()
    {
        std::lock_guard<std::mutex> lk( g_cacheMtx );
        browser->populate( g_cache );
    }

    bool on_poll()
    {
        if ( g_cacheValid.load( std::memory_order_acquire ) )
        {
            populate_from_cache();
            return false;   // stop polling
        }
        return true;        // keep waiting
    }

    void on_hide()
    {
        // Defer teardown off the signal emission; deleting the session drops
        // the trackable and auto-disconnects any live poll timeout.
        Glib::signal_idle().connect(
            sigc::bind( sigc::ptr_fun( &BrowserSession::deferred_delete ),
                        this ) );
    }

    static bool deferred_delete( BrowserSession* self )
    {
        delete self->browser;   // destroy the Gtk::Window
        delete self;            // and this session (disconnects its timers)
        return false;           // one-shot
    }
};

} // namespace

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------

void show_plugin_browser( int track )
{
    BrowserSession* s = new BrowserSession( track );
    s->start();
}

void open_track_editor( int track )
{
    seq24::engine::MixerGraph* g = seq24::app::audio_app_graph();
    if ( !g )
        return;
    seq24::engine::Track* t = g->track( track );
    if ( !t )
        return;
    seq24::engine::IPluginInstance* inst = t->instrument();
    if ( !inst )
        return;
    open_plugin_editor( inst );
}

} // namespace rack
} // namespace seq24
