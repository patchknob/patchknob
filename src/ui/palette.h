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
//  The "1111" (ScaleJammer) synthwave palette, ported for the seq24 pattern
//  editor reskin.  Source of truth: 1111/PianoRoll.h:235-248 (ARGB uint32).
//
//  Two flavours of accessor are provided so the legacy GDK draw paths and the
//  new Cairo draw paths can share one palette:
//
//    * gdk_color(hex)  -> Gdk::Color   (alpha dropped; for set_foreground)
//    * a Cairo context helper set_source_rgba(cr, hex, alpha) for the Cairo
//      paths that want translucency / grading.
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_PALETTE_H
#define SEQ24_UI_PALETTE_H

#include <gdkmm/color.h>
#include <cairomm/context.h>

namespace synth
{
    /* Monochrome (black & white) palette, ARGB.  Names mirror the original so
       all draw code is unchanged; only neutral greys here -- NO hues. */
    enum Color : unsigned int
    {
        cBg      = 0xFF000000u,   // pure black window background
        cPanel   = 0xFF161616u,   // panel fill / even rows (dark grey)
        cAccent  = 0xFFB4B4B4u,   // light grey - root rows, selection chrome
        cSel     = 0xFFFFFFFFu,   // white - lasso, paste cursor
        cActive  = 0xFFFFFFFFu,   // white - playhead, "active/playing"
        cHi      = 0xFFEDEDEDu,   // near-white text
        cDim     = 0xFF6E6E6Eu,   // mid grey - inactive
        cWhite   = 0xFFF2F2F2u,   // white text
        cBlk     = 0xFF0C0C0Cu,   // key strip background (near-black)
        cNote    = 0xFFC8C8C8u,   // note body (light grey on black)
        cNoteSel = 0xFFFFFFFFu,   // selected note (pure white)
        cScale   = 0xFF050505u,   // scale tint (barely-there)
        cChordBg = 0xFF0D0D0Du    // chord timeline bg
    };

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
