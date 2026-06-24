//----------------------------------------------------------------------------
//
//  VuWidget - a reusable vertical VU meter for the seq24 mixer.
//
//  Monochrome seq24 aesthetic: black / greys on a light-grey background,
//  drawn directly with a Gdk::GC in a Gtk::DrawingArea (same approach as
//  seq24's maintime / seqdata widgets).
//
//  Draws a stereo meter (left + right column) with:
//    - filled RMS bar (mid grey)
//    - peak-hold tick (solid black line that decays slowly)
//    - dB scale ticks along the side
//
//  Driven from a timer via set_levels(); call queue_draw() implicitly.
//
//  This file is part of the seq24 Windows port mixer module.
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_MIXER_VU_WIDGET_H
#define SEQ24_UI_MIXER_VU_WIDGET_H

#include <gtkmm/drawingarea.h>
#include <gdkmm/gc.h>
#include <gdkmm/colormap.h>

namespace seq24 {
namespace mixer {

class VuWidget : public Gtk::DrawingArea
{
public:
    VuWidget();
    virtual ~VuWidget();

    // Levels are linear amplitude in [0..1] (0 = silence, 1 = 0 dBFS).
    // Values above 1.0 are clamped for display but light the clip indicator.
    // Called from a Glib timer in the engine / test harness.
    void set_levels( float a_peakL, float a_peakR,
                     float a_rmsL,  float a_rmsR );

    // Reset all state to silence (e.g. on transport stop).
    void reset();

    // Decay the peak-hold ticks; call this on the same timer as set_levels
    // if you want the peak hold to fall on its own. set_levels also decays.
    void decay_peaks();

protected:
    virtual void on_realize();
    virtual bool on_expose_event( GdkEventExpose* a_ev );

private:
    void draw_meter();
    void draw_column( int a_x, int a_w, float a_rms, float a_peak );
    // Map a linear amplitude [0..1] to a pixel height inside the meter,
    // using a dB scale so quiet signals are visible.
    int  amp_to_y( float a_amp, int a_top, int a_bottom );

    Glib::RefPtr<Gdk::Window> m_window;
    Glib::RefPtr<Gdk::GC>     m_gc;

    Gdk::Color m_black;
    Gdk::Color m_white;
    Gdk::Color m_grey;
    Gdk::Color m_dark_grey;
    Gdk::Color m_light_grey;

    // Current displayed levels (linear amplitude).
    float m_rmsL,  m_rmsR;
    float m_peakL, m_peakR;
    // Peak-hold values (decay slowly).
    float m_holdL, m_holdR;
    bool  m_clipL, m_clipR;
};

} // namespace mixer
} // namespace seq24

#endif // SEQ24_UI_MIXER_VU_WIDGET_H
