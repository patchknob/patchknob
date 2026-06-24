//----------------------------------------------------------------------------
//
//  VuWidget implementation.  See vu_widget.h.
//
//  Drawing style mirrors seq24's maintime.cpp / seqdata.cpp: allocate
//  black/white/grey from the default colormap in the constructor, grab the
//  Gdk::Window and create a Gdk::GC in on_realize(), then paint with
//  draw_rectangle / draw_line in on_expose_event.
//
//-----------------------------------------------------------------------------

#include "vu_widget.h"

#include <gdkmm/general.h>
#include <cmath>
#include <algorithm>

namespace seq24 {
namespace mixer {

// Display geometry.
static const int c_vu_width   = 34;   // total widget width
static const int c_vu_height  = 140;  // total widget height
static const int c_vu_margin  = 2;    // border inset

// dB range mapped onto the meter.  -60 dB at the bottom, 0 dB (amp 1.0) at top.
static const float c_db_floor = -60.0f;

// Peak-hold decay per repaint (linear amplitude subtracted).  Tuned for a
// ~30 Hz timer so the tick falls in roughly a second.
static const float c_peak_decay = 0.012f;

VuWidget::VuWidget()
    : m_rmsL(0.0f), m_rmsR(0.0f),
      m_peakL(0.0f), m_peakR(0.0f),
      m_holdL(0.0f), m_holdR(0.0f),
      m_clipL(false), m_clipR(false)
{
    // Colors can be allocated in the constructor (get_window() is not yet
    // valid, but the default colormap is).  Same pattern as seq24 widgets.
    Glib::RefPtr<Gdk::Colormap> colormap = get_default_colormap();

    m_black      = Gdk::Color( "black" );
    m_white      = Gdk::Color( "white" );
    m_grey       = Gdk::Color( "grey" );
    m_dark_grey  = Gdk::Color( "gray40" );
    m_light_grey = Gdk::Color( "gray85" );

    colormap->alloc_color( m_black );
    colormap->alloc_color( m_white );
    colormap->alloc_color( m_grey );
    colormap->alloc_color( m_dark_grey );
    colormap->alloc_color( m_light_grey );

    set_size_request( c_vu_width, c_vu_height );
}

VuWidget::~VuWidget()
{
}

void
VuWidget::on_realize()
{
    Gtk::DrawingArea::on_realize();

    m_window = get_window();
    m_gc = Gdk::GC::create( m_window );
    m_window->clear();
}

void
VuWidget::reset()
{
    m_rmsL = m_rmsR = 0.0f;
    m_peakL = m_peakR = 0.0f;
    m_holdL = m_holdR = 0.0f;
    m_clipL = m_clipR = false;
    if ( is_realized() )
        queue_draw();
}

void
VuWidget::decay_peaks()
{
    m_holdL = std::max( 0.0f, m_holdL - c_peak_decay );
    m_holdR = std::max( 0.0f, m_holdR - c_peak_decay );
}

void
VuWidget::set_levels( float a_peakL, float a_peakR,
                      float a_rmsL,  float a_rmsR )
{
    m_clipL = ( a_peakL >= 1.0f );
    m_clipR = ( a_peakR >= 1.0f );

    m_peakL = std::min( 1.0f, std::max( 0.0f, a_peakL ) );
    m_peakR = std::min( 1.0f, std::max( 0.0f, a_peakR ) );
    m_rmsL  = std::min( 1.0f, std::max( 0.0f, a_rmsL  ) );
    m_rmsR  = std::min( 1.0f, std::max( 0.0f, a_rmsR  ) );

    // Decay the hold, then raise it if the new peak is higher.
    decay_peaks();
    if ( m_peakL > m_holdL ) m_holdL = m_peakL;
    if ( m_peakR > m_holdR ) m_holdR = m_peakR;

    if ( is_realized() )
        queue_draw();
}

int
VuWidget::amp_to_y( float a_amp, int a_top, int a_bottom )
{
    // Convert linear amplitude to a dB-scaled fraction [0..1] of the meter.
    float frac;
    if ( a_amp <= 0.0f )
    {
        frac = 0.0f;
    }
    else
    {
        float db = 20.0f * std::log10( a_amp );
        if ( db < c_db_floor ) db = c_db_floor;
        if ( db > 0.0f )       db = 0.0f;
        frac = 1.0f - ( db / c_db_floor );   // 0 at floor, 1 at 0 dB
    }
    int h = a_bottom - a_top;
    return a_bottom - (int)( frac * h );
}

void
VuWidget::draw_column( int a_x, int a_w, float a_rms, float a_peak )
{
    Gtk::Allocation alloc = get_allocation();
    int top    = c_vu_margin + 1;
    int bottom = alloc.get_height() - c_vu_margin - 1;

    // Recessed channel background (light grey trough).
    m_gc->set_foreground( m_light_grey );
    m_window->draw_rectangle( m_gc, true, a_x, top, a_w, bottom - top );

    // RMS fill (mid grey body).
    int rms_y = amp_to_y( a_rms, top, bottom );
    if ( rms_y < bottom )
    {
        m_gc->set_foreground( m_grey );
        m_window->draw_rectangle( m_gc, true,
                                  a_x, rms_y, a_w, bottom - rms_y );
    }

    // Peak instantaneous level (dark grey thin cap above the RMS body).
    int peak_y = amp_to_y( a_peak, top, bottom );
    if ( peak_y < rms_y )
    {
        m_gc->set_foreground( m_dark_grey );
        m_window->draw_rectangle( m_gc, true,
                                  a_x, peak_y, a_w, rms_y - peak_y );
    }
}

void
VuWidget::draw_meter()
{
    if ( !m_window )
        return;

    Gtk::Allocation alloc = get_allocation();
    int w = alloc.get_width();
    int h = alloc.get_height();

    m_window->clear();

    // Outer border (black rectangle, like maintime).
    m_gc->set_foreground( m_black );
    m_window->draw_rectangle( m_gc, false, 0, 0, w - 1, h - 1 );

    int top    = c_vu_margin + 1;
    int bottom = h - c_vu_margin - 1;

    // Two columns (L / R) leaving a slim gutter on the right for dB ticks.
    int tick_area = 7;
    int usable_w  = w - 2 * c_vu_margin - tick_area;
    int gap       = 2;
    int col_w     = ( usable_w - gap ) / 2;
    int lx        = c_vu_margin + 1;
    int rx        = lx + col_w + gap;

    draw_column( lx, col_w, m_rmsL, m_peakL );
    draw_column( rx, col_w, m_rmsR, m_peakR );

    // Peak-hold ticks (solid black lines spanning each column).
    m_gc->set_foreground( m_black );
    m_gc->set_line_attributes( 1, Gdk::LINE_SOLID,
                               Gdk::CAP_NOT_LAST, Gdk::JOIN_MITER );
    if ( m_holdL > 0.0f )
    {
        int y = amp_to_y( m_holdL, top, bottom );
        m_window->draw_line( m_gc, lx, y, lx + col_w - 1, y );
    }
    if ( m_holdR > 0.0f )
    {
        int y = amp_to_y( m_holdR, top, bottom );
        m_window->draw_line( m_gc, rx, y, rx + col_w - 1, y );
    }

    // dB scale ticks down the right gutter: 0, -6, -12, -24, -48.
    static const float marks[] = { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f };
    int tick_x = rx + col_w + 1;
    m_gc->set_foreground( m_dark_grey );
    for ( unsigned i = 0; i < sizeof(marks)/sizeof(marks[0]); ++i )
    {
        float frac = 1.0f - ( marks[i] / c_db_floor );
        int y = bottom - (int)( frac * ( bottom - top ) );
        m_window->draw_line( m_gc, tick_x, y, w - c_vu_margin - 1, y );
    }

    // Clip indicators: a filled black cap at the very top of a column.
    if ( m_clipL )
    {
        m_gc->set_foreground( m_black );
        m_window->draw_rectangle( m_gc, true, lx, top, col_w, 3 );
    }
    if ( m_clipR )
    {
        m_gc->set_foreground( m_black );
        m_window->draw_rectangle( m_gc, true, rx, top, col_w, 3 );
    }
}

bool
VuWidget::on_expose_event( GdkEventExpose* /*a_ev*/ )
{
    draw_meter();
    return true;
}

} // namespace mixer
} // namespace seq24
