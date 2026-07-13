//----------------------------------------------------------------------------
//
//  mixerapp.cpp  -- MixerController + show_mixer().  See mixerapp.h.
//
//  Binds the gtkmm mixer UI (seq24::mixer) to the live engine MixerGraph
//  (seq24::engine): live VU meters, control routing, FX-chain reflection.
//
//----------------------------------------------------------------------------

#include "mixerapp.h"

#include "ui/mixer/mixer_window.h"
#include "ui/mixer/channel_strip.h"

#include "engine/graph/mixer_graph.h"
#include "engine/graph/track.h"
#include "engine/plugin_api.h"

#include <glibmm/main.h>

#include <cstdio>
#include <string>
#include <vector>

namespace seq24 {
namespace mixer {

namespace {

// Strip title: the track's instrument name if it has one, else "Track N".
std::string track_title( seq24::engine::Track* a_t, int a_i )
{
    if ( a_t && a_t->instrument() )
        return a_t->instrument()->descriptor().name;
    char buf[32];
    std::snprintf( buf, sizeof buf, "Track %d", a_i + 1 );
    return std::string( buf );
}

// delete-event handler for the singleton window: hide instead of destroy so a
// later show_mixer() can raise the same window.  (Bound arg carries the window;
// keeps the GdkEventAny type out of the header.)
bool on_window_delete( GdkEventAny*, MixerWindow* a_win )
{
    if ( a_win )
        a_win->hide();
    return true;   // stop the default destroy
}

} // namespace

//----------------------------------------------------------------------------

MixerController::MixerController( seq24::engine::MixerGraph* a_graph )
    : m_graph( a_graph ),
      m_window( new MixerWindow() )
{
    m_window->signal_delete_event().connect(
        sigc::bind( sigc::ptr_fun( &on_window_delete ), m_window ) );

    build_strips();

    // ~30 Hz live-meter pump.
    m_timer = Glib::signal_timeout().connect(
        sigc::mem_fun( *this, &MixerController::on_meter_tick ), 33 );
}

MixerController::~MixerController()
{
    m_timer.disconnect();
    delete m_window;
}

void
MixerController::build_strips()
{
    const int n = m_graph ? m_graph->trackCount() : 0;
    m_fx_counts.assign( (size_t) n, -1 );

    for ( int i = 0; i < n; ++i )
    {
        seq24::engine::Track* t = m_graph->track( i );
        int idx = m_window->add_channel( track_title( t, i ) );
        connect_strip( idx );
        sync_strip_from_track( idx );
        refresh_fx( idx );
        m_fx_counts[idx] = t ? t->fxCount() : 0;
    }
}

void
MixerController::rebuild()
{
    m_timer.disconnect();

    const int have = m_window->channel_count();
    const int want = m_graph ? m_graph->trackCount() : 0;
    if ( (int) m_fx_counts.size() < want )
        m_fx_counts.resize( (size_t) want, -1 );

    // MixerWindow can only grow (no strip-removal API); add any new tracks.
    for ( int i = have; i < want; ++i )
    {
        seq24::engine::Track* t = m_graph->track( i );
        m_window->add_channel( track_title( t, i ) );
        connect_strip( i );
        sync_strip_from_track( i );
        refresh_fx( i );
        m_fx_counts[i] = t ? t->fxCount() : 0;
    }

    m_timer = Glib::signal_timeout().connect(
        sigc::mem_fun( *this, &MixerController::on_meter_tick ), 33 );
}

void
MixerController::connect_strip( int a_track )
{
    ChannelStrip* s = m_window->channel( a_track );
    if ( !s )
        return;

    // Bind the track index into each handler (bind<0> fixes the first arg,
    // leaving the value the signal carries).
    s->signal_gain_changed().connect(
        sigc::bind<0>( sigc::mem_fun( *this, &MixerController::on_gain ), a_track ) );
    s->signal_pan_changed().connect(
        sigc::bind<0>( sigc::mem_fun( *this, &MixerController::on_pan ), a_track ) );
    s->signal_mute_toggled().connect(
        sigc::bind<0>( sigc::mem_fun( *this, &MixerController::on_mute ), a_track ) );
    s->signal_solo_toggled().connect(
        sigc::bind<0>( sigc::mem_fun( *this, &MixerController::on_solo ), a_track ) );
    s->signal_add_fx().connect(
        sigc::bind( sigc::mem_fun( *this, &MixerController::on_add_fx ), a_track ) );
    s->signal_remove_fx().connect(
        sigc::bind<0>( sigc::mem_fun( *this, &MixerController::on_remove_fx ), a_track ) );
}

void
MixerController::sync_strip_from_track( int a_track )
{
    ChannelStrip*         s = m_window->channel( a_track );
    seq24::engine::Track* t = m_graph ? m_graph->track( a_track ) : 0;
    if ( !s || !t )
        return;

    // set_*() are guarded (m_updating) so this engine->UI sync does not
    // re-emit the changed signals back into the engine.
    double g = t->gain();
    if ( g < 0.0 ) g = 0.0;
    if ( g > 1.0 ) g = 1.0;
    s->set_gain( g );
    s->set_pan ( t->pan() );
    s->set_mute( t->mute() );
    s->set_solo( t->solo() );
}

void
MixerController::refresh_fx( int a_track )
{
    seq24::engine::Track* t = m_graph ? m_graph->track( a_track ) : 0;
    if ( !t )
        return;

    std::vector<std::string> names;
    const int fc = t->fxCount();
    names.reserve( (size_t) fc );
    for ( int f = 0; f < fc; ++f )
    {
        seq24::engine::IPluginInstance* fx = t->fxAt( f );
        names.push_back( fx ? fx->descriptor().name : std::string( "FX" ) );
    }
    m_window->set_channel_fx( a_track, names );
}

// ---- UI -> engine control routing -----------------------------------------

void
MixerController::on_gain( int a_track, double a_gain01 )
{
    if ( !m_graph ) return;
    seq24::engine::Track* t = m_graph->track( a_track );
    if ( t )
        t->setGain( (float) a_gain01 );
}

void
MixerController::on_pan( int a_track, double a_pan )
{
    if ( !m_graph ) return;
    seq24::engine::Track* t = m_graph->track( a_track );
    if ( t )
        t->setPan( (float) a_pan );
}

void
MixerController::on_mute( int a_track, bool a_on )
{
    if ( !m_graph ) return;
    seq24::engine::Track* t = m_graph->track( a_track );
    if ( t )
        t->setMute( a_on );
}

void
MixerController::on_solo( int a_track, bool a_on )
{
    if ( !m_graph ) return;
    seq24::engine::Track* t = m_graph->track( a_track );
    if ( t )
        t->setSolo( a_on );
}

void
MixerController::on_add_fx( int a_track )
{
    // Clean hook: MixerController owns no plugin factory, so it just logs and
    // re-emits.  The plugin-rack UI / coordinator connects to
    // signal_add_fx_request() to pop a chooser, call Track::addFx(), then
    // refresh_fx(track) to update the strip.
    std::printf( "[mixer] add-FX requested on track %d\n", a_track );
    std::fflush( stdout );
    m_sig_add_fx.emit( a_track );
}

void
MixerController::on_remove_fx( int a_track, int a_idx )
{
    std::printf( "[mixer] remove-FX requested on track %d (fx %d)\n",
                 a_track, a_idx );
    std::fflush( stdout );
    m_sig_remove_fx.emit( a_track, a_idx );
}

// ---- live meters -----------------------------------------------------------

bool
MixerController::on_meter_tick()
{
    if ( !m_graph || !m_window )
        return false;

    const int n = m_graph->trackCount();
    const int nc = m_window->channel_count();
    for ( int i = 0; i < n && i < nc; ++i )
    {
        seq24::engine::Track* t = m_graph->track( i );
        if ( !t )
            continue;

        m_window->set_channel_levels(
            i,
            t->vuLeft().peak(),  t->vuRight().peak(),
            t->vuLeft().rms(),   t->vuRight().rms() );

        // Reflect FX-chain changes lazily (only when the count changed).
        const int fc = t->fxCount();
        if ( i < (int) m_fx_counts.size() && fc != m_fx_counts[i] )
        {
            m_fx_counts[i] = fc;
            refresh_fx( i );
        }
    }

    m_window->set_master_levels(
        m_graph->masterVuLeft().peak(),  m_graph->masterVuRight().peak(),
        m_graph->masterVuLeft().rms(),   m_graph->masterVuRight().rms() );

    return true;   // keep the timer alive
}

//----------------------------------------------------------------------------
//  show_mixer -- create-or-raise the singleton mixer window.
//----------------------------------------------------------------------------

void
show_mixer( seq24::engine::MixerGraph* a_graph )
{
    static MixerController* s_ctrl = 0;

    // If bound to a different graph, drop the old controller/window.
    if ( s_ctrl && s_ctrl->graph() != a_graph )
    {
        delete s_ctrl;
        s_ctrl = 0;
    }
    if ( !s_ctrl )
        s_ctrl = new MixerController( a_graph );

    if ( s_ctrl->window() )
    {
        s_ctrl->window()->show_all();
        s_ctrl->window()->present();
    }
}

} // namespace mixer
} // namespace seq24
