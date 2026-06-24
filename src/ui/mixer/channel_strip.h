//----------------------------------------------------------------------------
//
//  ChannelStrip - one mixer channel for the seq24 mixer.
//
//  Layout (top to bottom), a Gtk::VBox:
//    - name label (plugin / track name)
//    - VU meter (VuWidget) beside a vertical gain fader
//    - pan control (horizontal slider)
//    - Mute / Solo toggle buttons
//    - "FX" insert chain: a scrolled list of effect names with +/- buttons
//
//  All user actions are exposed as sigc++ signals so the engine's MixerGraph
//  can connect to them later.  The widget itself only manipulates the UI; it
//  never assumes a backend.
//
//  This file is part of the seq24 Windows port mixer module.
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_MIXER_CHANNEL_STRIP_H
#define SEQ24_UI_MIXER_CHANNEL_STRIP_H

#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/button.h>
#include <gtkmm/treeview.h>
#include <gtkmm/liststore.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/frame.h>
#include <gtkmm/separator.h>

#include <sigc++/signal.h>
#include <string>
#include <vector>

#include "vu_widget.h"

namespace seq24 {
namespace mixer {

class ChannelStrip : public Gtk::VBox
{
public:
    // a_is_master gives the master strip a slightly distinct look and no
    // FX add/remove (you can still show its chain).  Default is a normal strip.
    explicit ChannelStrip( const std::string& a_name,
                           bool a_is_master = false );
    virtual ~ChannelStrip();

    // ---- Engine -> UI : push state in ------------------------------------

    void set_name( const std::string& a_name );
    std::string get_name() const { return m_name; }

    // Linear amplitude [0..1]; forwarded to the VU widget.
    void set_levels( float a_peakL, float a_peakR,
                     float a_rmsL,  float a_rmsR );

    // Replace the visible insert-FX list.
    void set_fx_list( const std::vector<std::string>& a_fx );

    // Set fader / pan without emitting the changed signals (engine sync).
    void set_gain( double a_gain01 );   // 0..1
    void set_pan( double a_pan );        // -1..+1
    void set_mute( bool a_on );
    void set_solo( bool a_on );

    double get_gain() const;             // 0..1
    double get_pan()  const;             // -1..+1
    bool   get_mute() const;
    bool   get_solo() const;

    // ---- UI -> Engine : sigc signals ------------------------------------

    // Fader moved.  Argument: new gain in 0..1.
    sigc::signal<void, double>& signal_gain_changed() { return m_sig_gain; }
    // Pan moved.  Argument: new pan in -1..+1.
    sigc::signal<void, double>& signal_pan_changed()  { return m_sig_pan; }
    // Mute toggled.  Argument: new state.
    sigc::signal<void, bool>&   signal_mute_toggled() { return m_sig_mute; }
    // Solo toggled.  Argument: new state.
    sigc::signal<void, bool>&   signal_solo_toggled() { return m_sig_solo; }
    // "+" pressed in the FX section.  Engine should pop a chooser, then call
    // set_fx_list() with the updated chain.
    sigc::signal<void>&         signal_add_fx()       { return m_sig_add_fx; }
    // "-" pressed.  Argument: index of the selected FX (or -1 if none).
    sigc::signal<void, int>&    signal_remove_fx()    { return m_sig_remove_fx; }

protected:
    // Internal GTK handlers translate widget events into the sigc signals.
    void on_gain_changed();
    void on_pan_changed();
    void on_mute_clicked();
    void on_solo_clicked();
    void on_add_clicked();
    void on_remove_clicked();

private:
    void build_ui();

    std::string m_name;
    bool        m_is_master;

    // Guard so set_*() (engine sync) does not re-emit changed signals.
    bool m_updating;

    // ---- Widgets ----
    Gtk::Label*       m_label;
    VuWidget*         m_vu;
    Gtk::VScale*      m_fader;        // gain 0..1
    Gtk::HScale*      m_pan;          // pan -1..+1
    Gtk::ToggleButton* m_mute_btn;
    Gtk::ToggleButton* m_solo_btn;

    Gtk::Frame*          m_fx_frame;
    Gtk::TreeView*       m_fx_view;
    Gtk::ScrolledWindow* m_fx_scroll;
    Glib::RefPtr<Gtk::ListStore> m_fx_store;
    Gtk::Button*         m_add_btn;
    Gtk::Button*         m_remove_btn;

    // TreeView column model for the FX list.
    struct FxColumns : public Gtk::TreeModel::ColumnRecord
    {
        FxColumns() { add( m_col_name ); }
        Gtk::TreeModelColumn<Glib::ustring> m_col_name;
    };
    FxColumns m_fx_cols;

    // ---- Signals ----
    sigc::signal<void, double> m_sig_gain;
    sigc::signal<void, double> m_sig_pan;
    sigc::signal<void, bool>   m_sig_mute;
    sigc::signal<void, bool>   m_sig_solo;
    sigc::signal<void>         m_sig_add_fx;
    sigc::signal<void, int>    m_sig_remove_fx;
};

} // namespace mixer
} // namespace seq24

#endif // SEQ24_UI_MIXER_CHANNEL_STRIP_H
