//----------------------------------------------------------------------------
//
//  MixerWindow implementation.  See mixer_window.h.
//
//-----------------------------------------------------------------------------

#include "mixer_window.h"

namespace seq24 {
namespace mixer {

MixerWindow::MixerWindow()
    : m_root(0), m_scroll(0), m_strip_row(0), m_master(0)
{
    set_title( "seq24 - Mixer" );
    set_default_size( 640, 420 );

    m_root = Gtk::manage( new Gtk::HBox( false, 2 ) );
    m_root->set_border_width( 4 );

    // Scrolling area holding the channel strips.
    m_scroll = Gtk::manage( new Gtk::ScrolledWindow() );
    m_scroll->set_policy( Gtk::POLICY_AUTOMATIC, Gtk::POLICY_NEVER );
    m_scroll->set_shadow_type( Gtk::SHADOW_IN );

    m_strip_row = Gtk::manage( new Gtk::HBox( false, 0 ) );
    // Pack the strip row into the scrolled window without forcing a viewport
    // policy that would also scroll vertically.
    m_scroll->add( *m_strip_row );

    m_root->pack_start( *m_scroll, Gtk::PACK_EXPAND_WIDGET );

    // Vertical separator between channels and the pinned master.
    m_root->pack_start( *Gtk::manage( new Gtk::VSeparator() ),
                        Gtk::PACK_SHRINK );

    // Master strip pinned on the right (outside the scroll).
    m_master = Gtk::manage( new ChannelStrip( "Master", true ) );
    m_root->pack_start( *m_master, Gtk::PACK_SHRINK );

    add( *m_root );
    show_all_children();
}

MixerWindow::~MixerWindow()
{
}

int
MixerWindow::add_channel( const std::string& a_name )
{
    ChannelStrip* strip = Gtk::manage( new ChannelStrip( a_name, false ) );
    m_strip_row->pack_start( *strip, Gtk::PACK_SHRINK );

    // Thin separator between strips for that crisp seq24 grid look.
    m_strip_row->pack_start( *Gtk::manage( new Gtk::VSeparator() ),
                             Gtk::PACK_SHRINK );

    strip->show_all();
    m_channels.push_back( strip );
    return (int) m_channels.size() - 1;
}

ChannelStrip*
MixerWindow::channel( int a_i )
{
    if ( a_i < 0 || a_i >= (int) m_channels.size() )
        return 0;
    return m_channels[a_i];
}

void
MixerWindow::set_channel_levels( int a_i,
                                 float a_peakL, float a_peakR,
                                 float a_rmsL,  float a_rmsR )
{
    ChannelStrip* s = channel( a_i );
    if ( s )
        s->set_levels( a_peakL, a_peakR, a_rmsL, a_rmsR );
}

void
MixerWindow::set_channel_fx( int a_i, const std::vector<std::string>& a_fx )
{
    ChannelStrip* s = channel( a_i );
    if ( s )
        s->set_fx_list( a_fx );
}

void
MixerWindow::set_master_levels( float a_peakL, float a_peakR,
                                float a_rmsL,  float a_rmsR )
{
    if ( m_master )
        m_master->set_levels( a_peakL, a_peakR, a_rmsL, a_rmsR );
}

} // namespace mixer
} // namespace seq24
