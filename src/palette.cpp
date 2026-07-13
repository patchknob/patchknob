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
//  palette.cpp
//
//  Runtime storage + tables for the switchable custom-draw palette declared in
//  ui/palette.h.  Every seq24 custom-drawn widget (seqroll, seqkeys, seqdata,
//  seqtime, trackeredit, the mixer / rack UIs) reads the synth::cXxx variables
//  live in its draw handler, so overwriting them here and forcing a redraw
//  re-skins all custom drawing to the active theme mode.
//
//  Two full 13-color tables, both strictly monochrome (one hue family each):
//    ANCIENT  -- neutral greys, black & white (the original ScaleJammer values)
//    MIDNIGHT -- green-phosphor CRT: near-black green bg, phosphor-green fg
//
//  This file lives flat in src/ so the top-level `src/*.cpp` glob compiles it.
//
//-----------------------------------------------------------------------------

#include "ui/palette.h"

namespace synth
{
    //  Live palette entries.  Initialized to the LIGHT (ancient) table so any
    //  static/early draw before set_palette() runs is already sensible.
    unsigned int cBg      = 0xFFFFFFFFu;
    unsigned int cPanel   = 0xFFECECECu;
    unsigned int cAccent  = 0xFF404040u;
    unsigned int cSel     = 0xFF000000u;
    unsigned int cActive  = 0xFF000000u;
    unsigned int cHi      = 0xFF101010u;
    unsigned int cDim     = 0xFF9A9A9Au;
    unsigned int cWhite   = 0xFF000000u;
    unsigned int cBlk     = 0xFFF0F0F0u;
    unsigned int cNote    = 0xFF383838u;
    unsigned int cNoteSel = 0xFF000000u;
    unsigned int cScale   = 0xFFF7F7F7u;
    unsigned int cChordBg = 0xFFF0F0F0u;

    namespace
    {
        //  Ordering matches the assignment order in set_palette() below.
        //  Index: 0 Bg  1 Panel  2 Accent  3 Sel  4 Active  5 Hi  6 Dim
        //         7 White  8 Blk  9 Note  10 NoteSel  11 Scale  12 ChordBg

        // ----- LIGHT ("ancient"): WHITE background, black/greyscale fg -------
        //  A clean light theme: white canvas, dark-grey/black notes, text and
        //  playhead; mid-grey grid lines.  Strictly greyscale (R==G==B).
        const unsigned int k_ancient[13] =
        {
            0xFFFFFFFFu,  // cBg      white window background
            0xFFECECECu,  // cPanel   light grey panel / even rows
            0xFF404040u,  // cAccent  dark grey root rows / selection chrome
            0xFF000000u,  // cSel     black lasso / paste cursor
            0xFF000000u,  // cActive  black playhead
            0xFF101010u,  // cHi      near-black text
            0xFF9A9A9Au,  // cDim     mid grey grid / inactive
            0xFF000000u,  // cWhite   black text (darkest)
            0xFFF0F0F0u,  // cBlk     light key strip background
            0xFF383838u,  // cNote    dark grey note body (on white)
            0xFF000000u,  // cNoteSel black selected note
            0xFFF7F7F7u,  // cScale   very light scale tint
            0xFFF0F0F0u   // cChordBg light chord timeline bg
        };

        // ----- MIDNIGHT: green-phosphor CRT terminal ------------------------
        //  DEEP BLACK backgrounds (pure/near-pure black) with bright, HIGH-
        //  CONTRAST phosphor green foregrounds -- so the green pops off the
        //  black and reads clearly.  R and B stay well below G (green-mono,
        //  no foreign hue); grid/dim lines are kept just bright enough to be
        //  visible against the black.
        const unsigned int k_midnight[13] =
        {
            0xFF000000u,  // cBg      pure black background
            0xFF041405u,  // cPanel   very dark green row-stripe (subtle on black)
            0xFF3BFF74u,  // cAccent  bright phosphor green (root / sel chrome)
            0xFF9FFFC0u,  // cSel     brightest green lasso / paste cursor
            0xFF00FF5Fu,  // cActive  vivid green playhead
            0xFF48FF80u,  // cHi      bright green text
            0xFF1C8F42u,  // cDim     dim-but-visible green grid lines
            0xFFB6FFCEu,  // cWhite   brightest green text
            0xFF000000u,  // cBlk     pure black key strip
            0xFF34E066u,  // cNote    bright green note body (pops on black)
            0xFFAFFFD0u,  // cNoteSel brightest green selected note
            0xFF00140Au,  // cScale   subtle green scale tint
            0xFF010A03u   // cChordBg near-black green chord timeline bg
        };
    } // anonymous namespace

    void set_palette( int mode )
    {
        const unsigned int * t = ( mode == PALETTE_MIDNIGHT ) ? k_midnight
                                                              : k_ancient;
        cBg      = t[0];
        cPanel   = t[1];
        cAccent  = t[2];
        cSel     = t[3];
        cActive  = t[4];
        cHi      = t[5];
        cDim     = t[6];
        cWhite   = t[7];
        cBlk     = t[8];
        cNote    = t[9];
        cNoteSel = t[10];
        cScale   = t[11];
        cChordBg = t[12];
    }
}
