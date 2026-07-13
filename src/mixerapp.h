//----------------------------------------------------------------------------
//
//  mixerapp.h  -- binds the seq24 mixer UI to the live engine MixerGraph.
//
//  MixerController is the glue between the gtkmm mixer widgets
//  (seq24::mixer::MixerWindow / ChannelStrip / VuWidget) and the realtime
//  audio graph (seq24::engine::MixerGraph / Track).  It:
//
//    * builds one ChannelStrip per engine Track (plus the master strip),
//    * runs a ~30 Hz Glib timer that reads each Track's stereo VU (peak/rms)
//      and the master VU and pushes them into the strips  -> LIVE meters,
//    * routes each strip's sigc control signals to the Track setters
//      (gain->setGain, pan->setPan, mute->setMute, solo->setSolo),
//    * reflects each Track's FX-chain names into the strip's FX list.
//
//  The FX add/remove buttons are surfaced as MixerController signals
//  (signal_add_fx_request / signal_remove_fx_request) so the plugin-rack UI /
//  coordinator can wire the actual add/remove later.  MixerController itself
//  only logs them -- a clean hook, no engine mutation.
//
//  show_mixer() is the single entry point the app menu / coordinator calls:
//  it create-or-raises one singleton mixer window bound to the given graph.
//
//  This file is flat in src/ so the app's src/*.cpp glob compiles mixerapp.cpp.
//
//----------------------------------------------------------------------------
#ifndef SEQ24_MIXERAPP_H
#define SEQ24_MIXERAPP_H

#include <sigc++/signal.h>
#include <sigc++/connection.h>

#include <vector>

namespace seq24 { namespace engine { class MixerGraph; } }

namespace seq24 {
namespace mixer {

class MixerWindow;   // forward decl (defined in ui/mixer/mixer_window.h)

//----------------------------------------------------------------------------
//  MixerController -- owns a MixerWindow and binds it to a live MixerGraph.
//----------------------------------------------------------------------------
class MixerController
{
public:
    // Build a window with one strip per track of `a_graph`, wire the control
    // signals, and start the live-meter timer.  Does not show the window; the
    // caller (or show_mixer) calls window()->show_all() / present().
    explicit MixerController( seq24::engine::MixerGraph* a_graph );
    ~MixerController();

    MixerController( const MixerController& )            = delete;
    MixerController& operator=( const MixerController& ) = delete;

    MixerWindow*               window() { return m_window; }
    seq24::engine::MixerGraph* graph()  { return m_graph; }

    // Rebuild all strips from the graph's current topology (e.g. after the
    // track count changed).  Re-wires signals and restarts the meter timer.
    void rebuild();

    // Force a re-read of a track's FX chain names into its strip (message
    // thread).  Called automatically by the meter timer when fxCount changes.
    void refresh_fx( int a_track );

    // ---- clean hooks for the FX rack / coordinator -----------------------
    // Emitted when the strip's "+" is pressed (arg: track index) and when "-"
    // is pressed (args: track index, selected fx index or -1).  MixerController
    // does not itself add/remove FX (it has no plugin factory); connect these
    // to the plugin-chooser flow to perform the actual Track::addFx/removeFx.
    sigc::signal<void, int>&      signal_add_fx_request()    { return m_sig_add_fx; }
    sigc::signal<void, int, int>& signal_remove_fx_request() { return m_sig_remove_fx; }

private:
    void build_strips();
    void connect_strip( int a_track );
    void sync_strip_from_track( int a_track );

    // Per-strip UI -> engine handlers (bound with the track index).
    void on_gain( int a_track, double a_gain01 );
    void on_pan ( int a_track, double a_pan );
    void on_mute( int a_track, bool a_on );
    void on_solo( int a_track, bool a_on );
    void on_add_fx   ( int a_track );
    void on_remove_fx( int a_track, int a_idx );

    // ~30 Hz live-meter pump; returns true to stay connected.
    bool on_meter_tick();

    seq24::engine::MixerGraph* m_graph;
    MixerWindow*               m_window;
    sigc::connection           m_timer;

    // Last-seen FX count per track, so the meter timer only rebuilds the FX
    // list widget when the chain actually changed.
    std::vector<int>           m_fx_counts;

    sigc::signal<void, int>      m_sig_add_fx;
    sigc::signal<void, int, int> m_sig_remove_fx;
};

//----------------------------------------------------------------------------
//  Create-or-raise the singleton mixer window bound to `a_graph`.
//  The coordinator calls this from a menu item.  Passing a different graph
//  than a previously-shown one rebuilds the window for the new graph.
//----------------------------------------------------------------------------
void show_mixer( seq24::engine::MixerGraph* a_graph );

} // namespace mixer
} // namespace seq24

#endif // SEQ24_MIXERAPP_H
