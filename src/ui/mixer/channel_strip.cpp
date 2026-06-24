//----------------------------------------------------------------------------
//
//  ChannelStrip implementation.  See channel_strip.h.
//
//-----------------------------------------------------------------------------

#include "channel_strip.h"

#include <gtkmm/adjustment.h>
#include <gtkmm/cellrenderertext.h>

namespace seq24 {
namespace mixer {

static const int c_strip_width = 96;

ChannelStrip::ChannelStrip( const std::string& a_name, bool a_is_master )
    : Gtk::VBox( false, 2 ),
      m_name( a_name ),
      m_is_master( a_is_master ),
      m_updating( false ),
      m_label(0), m_vu(0), m_fader(0), m_pan(0),
      m_mute_btn(0), m_solo_btn(0),
      m_fx_frame(0), m_fx_view(0), m_fx_scroll(0),
      m_add_btn(0), m_remove_btn(0)
{
    build_ui();
}

ChannelStrip::~ChannelStrip()
{
}

void
ChannelStrip::build_ui()
{
    set_border_width( 3 );
    set_size_request( c_strip_width, -1 );

    // --- Name label ---------------------------------------------------------
    m_label = Gtk::manage( new Gtk::Label() );
    set_name( m_name );          // also sets markup
    m_label->set_ellipsize( Pango::ELLIPSIZE_END );
    m_label->set_size_request( c_strip_width - 6, -1 );
    pack_start( *m_label, Gtk::PACK_SHRINK );

    pack_start( *Gtk::manage( new Gtk::HSeparator() ), Gtk::PACK_SHRINK );

    // --- VU meter + gain fader side by side --------------------------------
    Gtk::HBox* meter_row = Gtk::manage( new Gtk::HBox( false, 2 ) );

    m_vu = Gtk::manage( new VuWidget() );
    meter_row->pack_start( *m_vu, Gtk::PACK_SHRINK );

    // Gain fader: 0..1, default 0.8 (~ -2 dB visual headroom).
    m_fader = Gtk::manage( new Gtk::VScale( 0.0, 1.0001, 0.01 ) );
    m_fader->set_inverted( true );          // top = loud
    m_fader->set_draw_value( false );
    m_fader->set_value( 0.8 );
    m_fader->set_size_request( -1, 140 );
    m_fader->signal_value_changed().connect(
        sigc::mem_fun( *this, &ChannelStrip::on_gain_changed ) );
    meter_row->pack_start( *m_fader, Gtk::PACK_EXPAND_WIDGET );

    pack_start( *meter_row, Gtk::PACK_SHRINK );

    // --- Pan ----------------------------------------------------------------
    m_pan = Gtk::manage( new Gtk::HScale( -1.0, 1.0001, 0.01 ) );
    m_pan->set_draw_value( false );
    m_pan->set_value( 0.0 );
    m_pan->add_mark( 0.0, Gtk::POS_BOTTOM, "" );   // centre detent marker
    m_pan->signal_value_changed().connect(
        sigc::mem_fun( *this, &ChannelStrip::on_pan_changed ) );
    Gtk::HBox* pan_row = Gtk::manage( new Gtk::HBox( false, 2 ) );
    Gtk::Label* pan_lbl = Gtk::manage( new Gtk::Label( "Pan" ) );
    pan_lbl->set_size_request( 22, -1 );
    pan_row->pack_start( *pan_lbl, Gtk::PACK_SHRINK );
    pan_row->pack_start( *m_pan, Gtk::PACK_EXPAND_WIDGET );
    pack_start( *pan_row, Gtk::PACK_SHRINK );

    // --- Mute / Solo --------------------------------------------------------
    Gtk::HBox* ms_row = Gtk::manage( new Gtk::HBox( true, 2 ) );
    m_mute_btn = Gtk::manage( new Gtk::ToggleButton( "M" ) );
    m_solo_btn = Gtk::manage( new Gtk::ToggleButton( "S" ) );
    m_mute_btn->signal_clicked().connect(
        sigc::mem_fun( *this, &ChannelStrip::on_mute_clicked ) );
    m_solo_btn->signal_clicked().connect(
        sigc::mem_fun( *this, &ChannelStrip::on_solo_clicked ) );
    ms_row->pack_start( *m_mute_btn, Gtk::PACK_EXPAND_WIDGET );
    ms_row->pack_start( *m_solo_btn, Gtk::PACK_EXPAND_WIDGET );
    pack_start( *ms_row, Gtk::PACK_SHRINK );

    // --- FX insert chain ----------------------------------------------------
    m_fx_frame = Gtk::manage( new Gtk::Frame( "FX" ) );
    m_fx_frame->set_shadow_type( Gtk::SHADOW_IN );
    Gtk::VBox* fx_box = Gtk::manage( new Gtk::VBox( false, 2 ) );
    fx_box->set_border_width( 2 );

    m_fx_store = Gtk::ListStore::create( m_fx_cols );
    m_fx_view  = Gtk::manage( new Gtk::TreeView( m_fx_store ) );
    m_fx_view->set_headers_visible( false );
    m_fx_view->append_column( "Name", m_fx_cols.m_col_name );

    m_fx_scroll = Gtk::manage( new Gtk::ScrolledWindow() );
    m_fx_scroll->set_policy( Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC );
    m_fx_scroll->set_shadow_type( Gtk::SHADOW_NONE );
    m_fx_scroll->set_size_request( -1, 70 );
    m_fx_scroll->add( *m_fx_view );
    fx_box->pack_start( *m_fx_scroll, Gtk::PACK_EXPAND_WIDGET );

    Gtk::HBox* fx_btns = Gtk::manage( new Gtk::HBox( true, 2 ) );
    m_add_btn    = Gtk::manage( new Gtk::Button( "+" ) );
    m_remove_btn = Gtk::manage( new Gtk::Button( "-" ) );
    m_add_btn->signal_clicked().connect(
        sigc::mem_fun( *this, &ChannelStrip::on_add_clicked ) );
    m_remove_btn->signal_clicked().connect(
        sigc::mem_fun( *this, &ChannelStrip::on_remove_clicked ) );
    fx_btns->pack_start( *m_add_btn, Gtk::PACK_EXPAND_WIDGET );
    fx_btns->pack_start( *m_remove_btn, Gtk::PACK_EXPAND_WIDGET );
    fx_box->pack_start( *fx_btns, Gtk::PACK_SHRINK );

    m_fx_frame->add( *fx_box );
    pack_start( *m_fx_frame, Gtk::PACK_EXPAND_WIDGET );

    // The master strip has no insert FX add/remove in this port.
    if ( m_is_master )
    {
        m_add_btn->set_sensitive( false );
        m_remove_btn->set_sensitive( false );
    }
}

// ---- Engine -> UI ----------------------------------------------------------

void
ChannelStrip::set_name( const std::string& a_name )
{
    m_name = a_name;
    if ( m_label )
    {
        Glib::ustring esc = Glib::Markup::escape_text( a_name );
        if ( m_is_master )
            m_label->set_markup( "<b>" + esc + "</b>" );
        else
            m_label->set_markup( esc );
        m_label->set_tooltip_text( a_name );
    }
}

void
ChannelStrip::set_levels( float a_peakL, float a_peakR,
                          float a_rmsL,  float a_rmsR )
{
    if ( m_vu )
        m_vu->set_levels( a_peakL, a_peakR, a_rmsL, a_rmsR );
}

void
ChannelStrip::set_fx_list( const std::vector<std::string>& a_fx )
{
    if ( !m_fx_store )
        return;
    m_fx_store->clear();
    for ( size_t i = 0; i < a_fx.size(); ++i )
    {
        Gtk::TreeModel::Row row = *( m_fx_store->append() );
        row[ m_fx_cols.m_col_name ] = a_fx[i];
    }
}

void
ChannelStrip::set_gain( double a_gain01 )
{
    m_updating = true;
    m_fader->set_value( a_gain01 );
    m_updating = false;
}

void
ChannelStrip::set_pan( double a_pan )
{
    m_updating = true;
    m_pan->set_value( a_pan );
    m_updating = false;
}

void
ChannelStrip::set_mute( bool a_on )
{
    m_updating = true;
    m_mute_btn->set_active( a_on );
    m_updating = false;
}

void
ChannelStrip::set_solo( bool a_on )
{
    m_updating = true;
    m_solo_btn->set_active( a_on );
    m_updating = false;
}

double ChannelStrip::get_gain() const { return m_fader->get_value(); }
double ChannelStrip::get_pan()  const { return m_pan->get_value(); }
bool   ChannelStrip::get_mute() const { return m_mute_btn->get_active(); }
bool   ChannelStrip::get_solo() const { return m_solo_btn->get_active(); }

// ---- UI -> Engine ----------------------------------------------------------

void
ChannelStrip::on_gain_changed()
{
    if ( m_updating ) return;
    m_sig_gain.emit( m_fader->get_value() );
}

void
ChannelStrip::on_pan_changed()
{
    if ( m_updating ) return;
    m_sig_pan.emit( m_pan->get_value() );
}

void
ChannelStrip::on_mute_clicked()
{
    if ( m_updating ) return;
    m_sig_mute.emit( m_mute_btn->get_active() );
}

void
ChannelStrip::on_solo_clicked()
{
    if ( m_updating ) return;
    m_sig_solo.emit( m_solo_btn->get_active() );
}

void
ChannelStrip::on_add_clicked()
{
    m_sig_add_fx.emit();
}

void
ChannelStrip::on_remove_clicked()
{
    int idx = -1;
    if ( m_fx_view )
    {
        Gtk::TreeModel::iterator it = m_fx_view->get_selection()->get_selected();
        if ( it )
        {
            Gtk::TreePath path( it );
            if ( !path.empty() )
                idx = path[0];
        }
    }
    m_sig_remove_fx.emit( idx );
}

} // namespace mixer
} // namespace seq24
