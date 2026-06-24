//----------------------------------------------------------------------------
//
//  MixerWindow - the top-level mixer window for the seq24 Windows port.
//
//  A horizontal row of ChannelStrips inside a horizontally-scrolling area,
//  with a master ChannelStrip pinned on the right (outside the scroll).
//
//  Designed to be driven by the engine's MixerGraph later: one strip per
//  plugin / track.  For now it is fed by dummy data from mixer_test.cpp.
//
//  Public API:
//    int  add_channel( name )                 -> returns channel index
//    void set_channel_levels( i, pkL,pkR,rmsL,rmsR )
//    void set_channel_fx( i, vector<string> )
//    ChannelStrip* channel(i) / master()      -> connect sigc signals
//
//  This file is part of the seq24 Windows port mixer module.
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_MIXER_MIXER_WINDOW_H
#define SEQ24_UI_MIXER_MIXER_WINDOW_H

#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>

#include <string>
#include <vector>

#include "channel_strip.h"

namespace seq24 {
namespace mixer {

class MixerWindow : public Gtk::Window
{
public:
    MixerWindow();
    virtual ~MixerWindow();

    // Add a channel strip; returns its index (0-based).
    int add_channel( const std::string& a_name );

    // Number of (non-master) channels.
    int channel_count() const { return (int) m_channels.size(); }

    // Push VU levels into channel i (linear amplitude 0..1).  Out-of-range
    // index is ignored.
    void set_channel_levels( int a_i,
                             float a_peakL, float a_peakR,
                             float a_rmsL,  float a_rmsR );

    // Replace the FX insert list shown on channel i.
    void set_channel_fx( int a_i, const std::vector<std::string>& a_fx );

    // Master strip helpers (master is index -1 conceptually).
    void set_master_levels( float a_peakL, float a_peakR,
                            float a_rmsL,  float a_rmsR );

    // Direct access so the engine can connect to a strip's sigc signals.
    ChannelStrip* channel( int a_i );
    ChannelStrip* master() { return m_master; }

private:
    Gtk::HBox*           m_root;        // strips area | sep | master
    Gtk::ScrolledWindow* m_scroll;      // wraps the strips row
    Gtk::HBox*           m_strip_row;   // holds the per-channel strips
    ChannelStrip*        m_master;

    std::vector<ChannelStrip*> m_channels;
};

} // namespace mixer
} // namespace seq24

#endif // SEQ24_UI_MIXER_MIXER_WINDOW_H
