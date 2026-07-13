//----------------------------------------------------------------------------
//
//  ui/rack/plugin_browser.h  --  seq24 Windows port, VST "rack" plugin browser.
//
//  A top-level, searchable/filterable list of scanned PluginDescriptors, shown
//  in a monochrome (black & white), utilitarian TreeView with columns:
//
//      NAME | VENDOR | FORMAT | INSTR/FX | I/O
//
//  This class is PURE UI: it depends only on the header-only engine contract
//  (seq24::engine::PluginDescriptor) and gtkmm -- it never touches the audio
//  engine / audio_app glue.  That keeps it linkable into the standalone
//  rack_test executable with no engine symbols, and lets the controller
//  (src/rackapp.cpp) drive it: run the (slow, out-of-process) PluginHost::scan
//  on a background thread, then hand the results to populate().
//
//  Header-only on purpose: the whole rack UI is implemented inline so the ONE
//  translation unit that the top-level glob compiles into seq24 (rackapp.cpp)
//  carries it directly, with no extra static library to wire into the (frozen)
//  top-level CMakeLists.  rack_test.cpp includes the same header.
//
//  Controller -> view:
//      void set_scanning( bool )                 show / clear "scanning..." state
//      void populate( vector<PluginDescriptor> ) fill the list, apply the filter
//
//  View -> controller (sigc signals, each carrying the chosen descriptor):
//      signal_load_instrument()   double-click / "Load as Instrument"
//      signal_add_fx()            "Add as FX"
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_RACK_PLUGIN_BROWSER_H
#define SEQ24_UI_RACK_PLUGIN_BROWSER_H

#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/button.h>
#include <gtkmm/treeview.h>
#include <gtkmm/treeselection.h>
#include <gtkmm/liststore.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>

#include <sigc++/sigc++.h>

#include <string>
#include <vector>
#include <cctype>

#include "../palette.h"                 // synth:: monochrome palette
#include "../../engine/plugin_api.h"    // seq24::engine::PluginDescriptor

namespace seq24 {
namespace rack {

class PluginBrowser : public Gtk::Window
{
public:
    typedef seq24::engine::PluginDescriptor PluginDescriptor;

    explicit PluginBrowser( int a_track = -1 )
        : m_track( a_track ),
          m_root( 0 ), m_search( 0 ), m_view( 0 ), m_scroll( 0 ),
          m_status( 0 ), m_btn_instr( 0 ), m_btn_fx( 0 ),
          m_scanning( false )
    {
        build_ui();
        update_title();
    }

    virtual ~PluginBrowser() {}

    //! Which mixer track this browser assigns to (display + caller context).
    void set_track( int a_track ) { m_track = a_track; update_title(); }
    int  track() const            { return m_track; }

    // ---- controller -> view -------------------------------------------------

    //! Show / clear the "scanning..." placeholder.  While scanning the assign
    //! buttons are disabled (there is nothing valid to assign yet).
    void set_scanning( bool a_on )
    {
        m_scanning = a_on;
        if ( a_on )
        {
            if ( m_status )
                m_status->set_text( "scanning plugins (out-of-process, "
                                    "may take minutes)..." );
            set_buttons_sensitive( false );
        }
        else
        {
            update_status();
            set_buttons_sensitive( true );
        }
    }

    //! Replace the descriptor set and (re)apply the current search filter.
    void populate( const std::vector<PluginDescriptor>& a_list )
    {
        m_all = a_list;
        m_scanning = false;
        rebuild_rows();
        set_buttons_sensitive( true );
    }

    // ---- view -> controller signals -----------------------------------------

    sigc::signal<void, const PluginDescriptor&>& signal_load_instrument()
    { return m_sig_instr; }

    sigc::signal<void, const PluginDescriptor&>& signal_add_fx()
    { return m_sig_fx; }

private:
    // TreeView model columns (visible text + a hidden index into m_all).
    struct Columns : public Gtk::TreeModel::ColumnRecord
    {
        Gtk::TreeModelColumn<Glib::ustring> name;
        Gtk::TreeModelColumn<Glib::ustring> vendor;
        Gtk::TreeModelColumn<Glib::ustring> format;
        Gtk::TreeModelColumn<Glib::ustring> kind;    // "INSTR" / "FX"
        Gtk::TreeModelColumn<Glib::ustring> io;      // "in/out"
        Gtk::TreeModelColumn<int>           index;   // -> m_all[index]
        Columns()
        { add( name ); add( vendor ); add( format ); add( kind ); add( io );
          add( index ); }
    };

    // ---- construction -------------------------------------------------------

    void build_ui()
    {
        set_default_size( 640, 420 );
        set_border_width( 6 );

        m_root = Gtk::manage( new Gtk::VBox( false, 6 ) );
        add( *m_root );

        // Search / filter row.
        Gtk::HBox* srow = Gtk::manage( new Gtk::HBox( false, 6 ) );
        Gtk::Label* slbl = Gtk::manage( new Gtk::Label( "Filter" ) );
        m_search = Gtk::manage( new Gtk::Entry() );
        m_search->signal_changed().connect(
            sigc::mem_fun( *this, &PluginBrowser::on_search_changed ) );
        srow->pack_start( *slbl,     Gtk::PACK_SHRINK );
        srow->pack_start( *m_search, Gtk::PACK_EXPAND_WIDGET );
        m_root->pack_start( *srow, Gtk::PACK_SHRINK );

        // The plugin list.
        m_store = Gtk::ListStore::create( m_cols );
        m_view  = Gtk::manage( new Gtk::TreeView( m_store ) );
        m_view->set_rules_hint( true );
        m_view->append_column( "Name",   m_cols.name );
        m_view->append_column( "Vendor", m_cols.vendor );
        m_view->append_column( "Format", m_cols.format );
        m_view->append_column( "Kind",   m_cols.kind );
        m_view->append_column( "I/O",    m_cols.io );
        for ( int i = 0; i < 5; ++i )
        {
            Gtk::TreeViewColumn* c = m_view->get_column( i );
            if ( c ) { c->set_resizable( true ); c->set_expand( i == 0 ); }
        }
        m_view->signal_row_activated().connect(
            sigc::mem_fun( *this, &PluginBrowser::on_row_activated ) );

        m_scroll = Gtk::manage( new Gtk::ScrolledWindow() );
        m_scroll->set_policy( Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC );
        m_scroll->set_shadow_type( Gtk::SHADOW_IN );
        m_scroll->add( *m_view );
        m_root->pack_start( *m_scroll, Gtk::PACK_EXPAND_WIDGET );

        // Action row.
        Gtk::HBox* arow = Gtk::manage( new Gtk::HBox( false, 6 ) );
        m_status    = Gtk::manage( new Gtk::Label( "" ) );
        m_status->set_alignment( 0.0f, 0.5f );
        m_btn_instr = Gtk::manage( new Gtk::Button( "Load as Instrument" ) );
        m_btn_fx    = Gtk::manage( new Gtk::Button( "Add as FX" ) );
        m_btn_instr->signal_clicked().connect(
            sigc::mem_fun( *this, &PluginBrowser::on_load_instrument ) );
        m_btn_fx->signal_clicked().connect(
            sigc::mem_fun( *this, &PluginBrowser::on_add_fx ) );
        arow->pack_start( *m_status,    Gtk::PACK_EXPAND_WIDGET );
        arow->pack_start( *m_btn_instr, Gtk::PACK_SHRINK );
        arow->pack_start( *m_btn_fx,    Gtk::PACK_SHRINK );
        m_root->pack_start( *arow, Gtk::PACK_SHRINK );

        apply_monochrome();
        update_status();
    }

    // Light-touch black & white styling so the browser reads as monochrome even
    // with no system theme (as in the standalone rack_test).  Custom drawing is
    // reserved for palette.h; here we only recolour stock widgets.
    void apply_monochrome()
    {
        const Gdk::Color bg   = synth::gdk_color( synth::cBg );     // black
        const Gdk::Color pan  = synth::gdk_color( synth::cPanel );  // dark grey
        const Gdk::Color fg   = synth::gdk_color( synth::cHi );     // near-white
        const Gdk::Color selB = synth::gdk_color( synth::cAccent ); // light grey
        const Gdk::Color selF = synth::gdk_color( synth::cBg );     // black

        modify_bg( Gtk::STATE_NORMAL, bg );
        if ( m_view )
        {
            m_view->modify_base( Gtk::STATE_NORMAL,   bg );
            m_view->modify_text( Gtk::STATE_NORMAL,   fg );
            m_view->modify_base( Gtk::STATE_SELECTED, selB );
            m_view->modify_text( Gtk::STATE_SELECTED, selF );
            m_view->modify_base( Gtk::STATE_ACTIVE,   selB );
            m_view->modify_text( Gtk::STATE_ACTIVE,   selF );
        }
        if ( m_search )
        {
            m_search->modify_base( Gtk::STATE_NORMAL, pan );
            m_search->modify_text( Gtk::STATE_NORMAL, fg );
        }
        if ( m_status ) m_status->modify_fg( Gtk::STATE_NORMAL, fg );
    }

    // ---- model helpers ------------------------------------------------------

    static Glib::ustring format_str( const PluginDescriptor& d )
    {
        return d.format == seq24::engine::PluginFormat::VST3 ? "VST3" : "VST2";
    }

    static bool matches( const PluginDescriptor& d, const std::string& needle )
    {
        if ( needle.empty() )
            return true;
        return contains_ci( d.name, needle )
            || contains_ci( d.vendor, needle );
    }

    static bool contains_ci( const std::string& hay, const std::string& needle )
    {
        if ( needle.empty() ) return true;
        std::string h = lower( hay ), n = lower( needle );
        return h.find( n ) != std::string::npos;
    }

    static std::string lower( const std::string& s )
    {
        std::string o( s );
        for ( size_t i = 0; i < o.size(); ++i )
            o[i] = (char) std::tolower( (unsigned char) o[i] );
        return o;
    }

    void rebuild_rows()
    {
        if ( !m_store ) return;
        m_store->clear();

        const std::string needle =
            m_search ? std::string( m_search->get_text() ) : std::string();

        int shown = 0;
        for ( size_t i = 0; i < m_all.size(); ++i )
        {
            const PluginDescriptor& d = m_all[i];
            if ( !matches( d, needle ) )
                continue;

            Gtk::TreeModel::Row row = *( m_store->append() );
            row[ m_cols.name ]   = d.name;
            row[ m_cols.vendor ] = d.vendor;
            row[ m_cols.format ] = format_str( d );
            row[ m_cols.kind ]   = d.isInstrument ? "INSTR" : "FX";
            char io[32];
            std::snprintf( io, sizeof io, "%d/%d", d.numAudioIn, d.numAudioOut );
            row[ m_cols.io ]     = io;
            row[ m_cols.index ]  = (int) i;
            ++shown;
        }
        update_status( shown );
    }

    bool selected_descriptor( PluginDescriptor& out ) const
    {
        if ( !m_view ) return false;
        Glib::RefPtr<Gtk::TreeSelection> sel = m_view->get_selection();
        if ( !sel ) return false;
        Gtk::TreeModel::iterator it = sel->get_selected();
        if ( !it ) return false;
        int idx = (*it)[ m_cols.index ];
        if ( idx < 0 || idx >= (int) m_all.size() ) return false;
        out = m_all[ idx ];
        return true;
    }

    // ---- status / title -----------------------------------------------------

    void update_status( int shown = -1 )
    {
        if ( !m_status || m_scanning ) return;
        char buf[96];
        if ( shown < 0 ) shown = (int) m_all.size();
        std::snprintf( buf, sizeof buf, "%d shown / %d scanned",
                       shown, (int) m_all.size() );
        m_status->set_text( buf );
    }

    void update_title()
    {
        if ( m_track >= 0 )
        {
            char t[64];
            std::snprintf( t, sizeof t, "VST Rack  -  Track %d", m_track );
            set_title( t );
        }
        else
            set_title( "VST Rack" );
    }

    void set_buttons_sensitive( bool s )
    {
        if ( m_btn_instr ) m_btn_instr->set_sensitive( s );
        if ( m_btn_fx )    m_btn_fx->set_sensitive( s );
    }

    // ---- handlers -----------------------------------------------------------

    void on_search_changed() { rebuild_rows(); }

    void on_row_activated( const Gtk::TreeModel::Path&, Gtk::TreeViewColumn* )
    {
        // Double-click / Enter == "Load as Instrument".
        PluginDescriptor d;
        if ( selected_descriptor( d ) )
            m_sig_instr.emit( d );
    }

    void on_load_instrument()
    {
        PluginDescriptor d;
        if ( selected_descriptor( d ) )
            m_sig_instr.emit( d );
    }

    void on_add_fx()
    {
        PluginDescriptor d;
        if ( selected_descriptor( d ) )
            m_sig_fx.emit( d );
    }

    // ---- state --------------------------------------------------------------

    int  m_track;

    Gtk::VBox*           m_root;
    Gtk::Entry*          m_search;
    Gtk::TreeView*       m_view;
    Gtk::ScrolledWindow* m_scroll;
    Gtk::Label*          m_status;
    Gtk::Button*         m_btn_instr;
    Gtk::Button*         m_btn_fx;

    Columns                      m_cols;
    Glib::RefPtr<Gtk::ListStore> m_store;

    std::vector<PluginDescriptor> m_all;   // full scanned set (unfiltered)
    bool                          m_scanning;

    sigc::signal<void, const PluginDescriptor&> m_sig_instr;
    sigc::signal<void, const PluginDescriptor&> m_sig_fx;
};

} // namespace rack
} // namespace seq24

#endif // SEQ24_UI_RACK_PLUGIN_BROWSER_H
