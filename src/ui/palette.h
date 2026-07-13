//----------------------------------------------------------------------------
//
//  This file is part of seq24.
//
//  seq24 is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//-----------------------------------------------------------------------------
//
//  ui/palette.h
//
//  Custom-draw palette for the seq24 pattern-editor / tracker reskin.
//
//  RUNTIME-SWITCHABLE.  The 13 color names below used to be enum constants;
//  they are now plain `extern unsigned int` variables (ARGB) DEFINED in
//  src/palette.cpp.  Reading e.g. `synth::cBg` therefore yields the CURRENTLY
//  ACTIVE theme mode's value.  Call sites are unchanged: every name is still
//  just an `unsigned int` argb handed to synth::set_source()/synth::gdk_color().
//
//  Switch the active table with synth::set_palette(mode):
//      0 == ANCIENT  (grayscale black & white -- the original values)
//      1 == MIDNIGHT (green-phosphor CRT terminal)
//  (kept numerically in sync with seq24::theme::Mode).  The theme coordinator
//  drives this through seq24::theme::set_mode(), which also reloads the GTK
//  chrome RC -- see apptheme.h.
//
//  Two accessor flavours share one palette so the legacy GDK and the newer
//  Cairo draw paths agree:
//    * gdk_color(hex)          -> Gdk::Color (alpha dropped; for set_foreground)
//    * set_source(cr, hex[,a]) -> sets a Cairo source (optionally translucent)
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_PALETTE_H
#define SEQ24_UI_PALETTE_H

#include <gdkmm/color.h>
#include <cairomm/context.h>

namespace synth
{
    /* Active palette entries, ARGB.  Names mirror the original enum so every
       existing draw call site is unchanged; the values now track the live
       theme mode (see src/palette.cpp).  Strictly monochrome per mode: neutral
       greys in ANCIENT, phosphor greens in MIDNIGHT -- never a foreign hue. */
    extern unsigned int cBg;        // window background
    extern unsigned int cPanel;     // panel fill / even rows
    extern unsigned int cAccent;    // root rows, selection chrome
    extern unsigned int cSel;       // lasso, paste cursor
    extern unsigned int cActive;    // playhead, "active/playing"
    extern unsigned int cHi;        // bright text / light chrome
    extern unsigned int cDim;       // inactive / grid lines
    extern unsigned int cWhite;     // brightest text
    extern unsigned int cBlk;       // key strip background
    extern unsigned int cNote;      // note body
    extern unsigned int cNoteSel;   // selected note
    extern unsigned int cScale;     // scale tint
    extern unsigned int cChordBg;   // chord timeline background

    /* Palette table ids -- kept numerically identical to seq24::theme::Mode. */
    enum PaletteMode { PALETTE_ANCIENT = 0, PALETTE_MIDNIGHT = 1 };

    /* Overwrite all 13 live colors above with the chosen mode's table.
       Defined in src/palette.cpp.  Safe to call at any time; widgets that read
       the colors in their draw handlers pick up the change on next redraw. */
    void set_palette( int mode );

    /* extract 8-bit channels from an ARGB value */
    inline double r_of( unsigned int argb ) { return ((argb >> 16) & 0xFF) / 255.0; }
    inline double g_of( unsigned int argb ) { return ((argb >>  8) & 0xFF) / 255.0; }
    inline double b_of( unsigned int argb ) { return ((argb      ) & 0xFF) / 255.0; }
    inline double a_of( unsigned int argb ) { return ((argb >> 24) & 0xFF) / 255.0; }

    /* allocate-free Gdk::Color from an ARGB constant (alpha ignored) */
    inline Gdk::Color gdk_color( unsigned int argb )
    {
        Gdk::Color c;
        c.set_rgb_p( r_of(argb), g_of(argb), b_of(argb) );
        return c;
    }

    /* Cairo source helpers */
    inline void set_source( const Cairo::RefPtr<Cairo::Context>& cr,
                            unsigned int argb )
    {
        cr->set_source_rgba( r_of(argb), g_of(argb), b_of(argb), a_of(argb) );
    }

    inline void set_source( const Cairo::RefPtr<Cairo::Context>& cr,
                            unsigned int argb, double alpha )
    {
        cr->set_source_rgba( r_of(argb), g_of(argb), b_of(argb), alpha );
    }

    /* rounded rectangle path (radius clamped to half the smaller side) */
    inline void rounded_rect( const Cairo::RefPtr<Cairo::Context>& cr,
                              double x, double y, double w, double h,
                              double radius )
    {
        if ( w <= 0 || h <= 0 ) return;
        double r = radius;
        if ( r > w * 0.5 ) r = w * 0.5;
        if ( r > h * 0.5 ) r = h * 0.5;
        if ( r < 0.5 )
        {
            cr->rectangle( x, y, w, h );
            return;
        }
        const double deg = 3.14159265358979323846 / 180.0;
        cr->begin_new_sub_path();
        cr->arc( x + w - r, y + r,     r, -90 * deg,   0 * deg );
        cr->arc( x + w - r, y + h - r, r,   0 * deg,  90 * deg );
        cr->arc( x + r,     y + h - r, r,  90 * deg, 180 * deg );
        cr->arc( x + r,     y + r,     r, 180 * deg, 270 * deg );
        cr->close_path();
    }
}

#endif // SEQ24_UI_PALETTE_H
