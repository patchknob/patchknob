//----------------------------------------------------------------------------
//  src/quantize.h -- THE quantize algorithm for PatchKnob.
//
//  There used to be three of these, and they disagreed:
//
//    * sequence::quantize_events()  rounded to the NEAREST line, strength as an
//      integer divisor.
//    * the record quantiser (sdlui/main.cpp) always FLOORED to the previous
//      line, strength as a float.
//    * PianoRoll::snap_tick() always FLOORED, no strength at all.
//
//  Flooring is what made record quantise feel rigid and "move things to the
//  wrong 16th": a note played a hair AHEAD of the beat -- which is what human
//  players do constantly -- is nearer the NEXT line, but floor drags it back a
//  full grid step.  Everything here rounds to the nearest line instead.
//
//  The parameters follow the semantics every major DAW shares (Logic's
//  Q-Strength / Q-Range / Q-Swing, Cubase's quantise panel, Ardour's
//  ARDOUR::Quantize):
//
//    grid       ticks per quantise line.  <= 0 disables (returns the input).
//    strength   0..1, the fraction of the way to the line the event travels.
//               1.0 is dead on the grid; ~0.9 tightens without mechanising.
//    threshold  events ALREADY within this many ticks of a line are left
//               exactly as played.  This is the anti-rigidity knob (Ardour's
//               _threshold, Logic's Q-Range): correct what is noticeably off,
//               leave the micro-timing that carries the feel alone.
//    swing      0.5 == straight.  Above 0.5 delays every other line, up to
//               2/3 of a grid step at 1.0 (the same ceiling Ardour uses).
//    leftward   legacy escape hatch: never move an event later than played.
//               Off by default -- it is the behaviour this header replaced.
//
//  Header-only and dependency-free so both the engine (src/) and the SDL shell
//  (sdlui/) can share one implementation.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_QUANTIZE_H
#define PATCHKNOB_QUANTIZE_H

#include <cmath>
#include <cstdlib>

namespace PatchKnob {
namespace quantize {

//! Floor division that stays correct for negative ticks (C++ / truncates
//! toward zero, which would snap negative positions the wrong way).
inline long floor_div( long a, long b )
{
    if ( b == 0 ) return 0;
    long d = a / b;
    if ( ( a % b != 0 ) && ( ( a < 0 ) != ( b < 0 ) ) ) --d;
    return d;
}

struct Params
{
    long  grid      = 0;        //!< ticks per line; <= 0 disables
    float strength  = 1.0f;     //!< 0..1 pull toward the line
    long  threshold = 0;        //!< leave events within this many ticks alone
    float swing     = 0.5f;     //!< 0.5 straight; > 0.5 delays odd lines
    bool  leftward  = false;    //!< never move an event later than played
};

//! Tick offset applied to grid line `index` for the current swing setting.
//! Even lines (the down-beats) never move; odd lines slide later.
inline long swing_offset( long index, const Params& p )
{
    if ( p.grid <= 0 ) return 0;
    if ( ( index & 1L ) == 0L ) return 0;          // down-beat: never swung
    const double amount = ( (double) p.swing - 0.5 ) * 2.0;   // -1..1
    if ( amount == 0.0 ) return 0;
    // 2/3 of a grid step at full swing -- a straight 8th becomes a triplet 8th.
    return (long) std::lround( amount * ( 2.0 / 3.0 ) * (double) p.grid );
}

//! Absolute tick of grid line `index`, swing included.
inline long line_tick( long index, const Params& p )
{
    return index * p.grid + swing_offset( index, p );
}

//! The quantise line nearest to `tick`.  Swing moves lines around, so the
//! nearest one is not always the nearest INDEX -- test both neighbours.
inline long nearest_line( long tick, const Params& p )
{
    if ( p.grid <= 0 ) return tick;
    const long idx = floor_div( tick, p.grid );
    long best = line_tick( idx, p );
    long bestDist = std::labs( tick - best );
    // The swung neighbours on either side can both end up closer.
    for ( long k = idx - 1; k <= idx + 2; ++k )
    {
        const long cand = line_tick( k, p );
        const long dist = std::labs( tick - cand );
        if ( dist < bestDist ) { bestDist = dist; best = cand; }
    }
    return best;
}

//! Quantise one absolute tick.  This is the single entry point -- every
//! quantiser and grid snap in PatchKnob goes through it.
inline long apply( long tick, const Params& p )
{
    if ( p.grid <= 0 ) return tick;

    const long target = nearest_line( tick, p );
    long delta = target - tick;

    // Already tight enough: leave the performance exactly as it was played.
    if ( p.threshold > 0 && std::labs( delta ) < p.threshold ) return tick;

    // Legacy leftward mode: never push an event later than it was played.
    if ( p.leftward && delta > 0 )
    {
        const long prev = target - p.grid;
        delta = prev - tick;
    }

    if ( p.strength < 1.0f )
        delta = (long) std::lround( (double) delta * (double) p.strength );

    return tick + delta;
}

//! Convenience for plain editor grid snapping (full strength, no threshold).
inline long snap( long tick, long grid )
{
    Params p; p.grid = grid; return apply( tick, p );
}

} // namespace quantize
} // namespace PatchKnob

#endif // PATCHKNOB_QUANTIZE_H
