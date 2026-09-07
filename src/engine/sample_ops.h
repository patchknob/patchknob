//----------------------------------------------------------------------------
//  src/engine/sample_ops.h
//
//  The destructive sample operations, as a free function over two float
//  channels.  Header-only and engine-neutral so BOTH ISampleSlot implementers
//  share one implementation:
//      * the SMPL-1 rack module (owns its buffers directly)
//      * the Buzz Sampler instrument adapter (owns a working copy of a zone)
//
//  Keeping the edits here rather than in either sampler is what makes "the same
//  system" true rather than aspirational -- a fix to the crossfade or the
//  auto-trim threshold lands in both at once.
//
//  MESSAGE THREAD ONLY.  The caller is responsible for whatever lock protects
//  the buffers from the render thread.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_SAMPLE_OPS_H
#define PATCHKNOB_ENGINE_SAMPLE_OPS_H

#include "sample_slot.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

namespace PatchKnob { namespace engine {

//! Apply `op` to l/r over [from,to).  `clipL`/`clipR` are the caller's
//! clipboard (cut/copy/paste use it).  Returns false when the op did nothing,
//! so the caller can drop the undo snapshot it took.
inline bool apply_sample_op( int op,
                             std::vector<float>& l, std::vector<float>& r,
                             double sampleRate,
                             int64_t from, int64_t to, float arg,
                             std::vector<float>& clipL, std::vector<float>& clipR )
{
    const int64_t n = (int64_t)l.size();
    if ( n <= 0 || r.size() != l.size() ) return false;
    from = from < 0 ? 0 : ( from > n ? n : from );
    to   = to   < 0 ? 0 : ( to   > n ? n : to );
    if ( to < from ) std::swap( from, to );
    const int64_t len = to - from;
    if ( len <= 0 && op != SOP_INSERT_SILENCE && op != SOP_PASTE_INSERT &&
         op != SOP_PASTE_MIX ) return false;

    switch ( op ) {
        case SOP_NORMALIZE: {
            float peak = 0.f;
            for ( int64_t i = from; i < to; ++i )
                peak = std::max( peak, std::max( std::fabs( l[(size_t)i] ),
                                                 std::fabs( r[(size_t)i] ) ) );
            if ( peak <= 1e-9f ) return false;
            const float g = ( arg > 0.f ? arg : 1.f ) / peak;
            for ( int64_t i = from; i < to; ++i ) { l[(size_t)i] *= g; r[(size_t)i] *= g; }
            break;
        }
        case SOP_REVERSE:
            for ( int64_t a = from, b = to - 1; a < b; ++a, --b ) {
                std::swap( l[(size_t)a], l[(size_t)b] );
                std::swap( r[(size_t)a], r[(size_t)b] );
            }
            break;
        case SOP_FADE_IN:
        case SOP_FADE_OUT:
            for ( int64_t i = from; i < to; ++i ) {
                float x = (float)( i - from ) / (float)len;
                if ( op == SOP_FADE_OUT ) x = 1.f - x;
                const float k = arg * 4.f;                 // -1 log .. 0 lin .. +1 exp
                const float g = ( std::fabs( k ) < 1e-4f )
                              ? x : ( std::exp( k * x ) - 1.f ) / ( std::exp( k ) - 1.f );
                l[(size_t)i] *= g; r[(size_t)i] *= g;
            }
            break;
        case SOP_SILENCE:
            std::fill( l.begin() + (size_t)from, l.begin() + (size_t)to, 0.f );
            std::fill( r.begin() + (size_t)from, r.begin() + (size_t)to, 0.f );
            break;
        case SOP_GAIN:
            for ( int64_t i = from; i < to; ++i ) { l[(size_t)i] *= arg; r[(size_t)i] *= arg; }
            break;
        case SOP_DC_REMOVE: {
            double sl = 0.0, sr = 0.0;
            for ( int64_t i = from; i < to; ++i ) { sl += l[(size_t)i]; sr += r[(size_t)i]; }
            const float ml = (float)( sl / (double)len ), mr = (float)( sr / (double)len );
            for ( int64_t i = from; i < to; ++i ) { l[(size_t)i] -= ml; r[(size_t)i] -= mr; }
            break;
        }
        case SOP_TRIM: {
            std::vector<float> nl( l.begin() + (size_t)from, l.begin() + (size_t)to );
            std::vector<float> nr( r.begin() + (size_t)from, r.begin() + (size_t)to );
            l.swap( nl ); r.swap( nr );
            break;
        }
        case SOP_INSERT_SILENCE: {
            const int64_t count = std::max<int64_t>( 1, len );
            l.insert( l.begin() + (size_t)from, (size_t)count, 0.f );
            r.insert( r.begin() + (size_t)from, (size_t)count, 0.f );
            break;
        }
        case SOP_CROSSFADE_LOOP: {
            // Make the loop-end -> loop-start splice continuous: fade the audio
            // just BEFORE the loop start into the tail before the loop end.
            // Equal-power (sin/cos), so the crossfade does not dip in level.
            int64_t xf = (int64_t)std::max( 1.f, arg );
            xf = std::min( xf, len );
            xf = std::min( xf, from );          // needs run-in before the loop
            if ( xf <= 1 ) return false;
            for ( int64_t i = 0; i < xf; ++i ) {
                const float t = (float)i / (float)( xf - 1 );
                const float a = std::cos( t * 1.57079633f );
                const float b = std::sin( t * 1.57079633f );
                const int64_t tail = to - xf + i, pre = from - xf + i;
                if ( tail < 0 || tail >= n || pre < 0 || pre >= n ) continue;
                l[(size_t)tail] = l[(size_t)tail] * a + l[(size_t)pre] * b;
                r[(size_t)tail] = r[(size_t)tail] * a + r[(size_t)pre] * b;
            }
            break;
        }
        case SOP_COPY:
            clipL.assign( l.begin() + (size_t)from, l.begin() + (size_t)to );
            clipR.assign( r.begin() + (size_t)from, r.begin() + (size_t)to );
            return false;                       // nothing changed; no undo entry
        case SOP_CUT:
            clipL.assign( l.begin() + (size_t)from, l.begin() + (size_t)to );
            clipR.assign( r.begin() + (size_t)from, r.begin() + (size_t)to );
            // fall through
        case SOP_DELETE:
            if ( len >= n ) { l.clear(); r.clear(); break; }
            l.erase( l.begin() + (size_t)from, l.begin() + (size_t)to );
            r.erase( r.begin() + (size_t)from, r.begin() + (size_t)to );
            break;
        case SOP_PASTE_INSERT:
            if ( clipL.empty() ) return false;
            l.insert( l.begin() + (size_t)from, clipL.begin(), clipL.end() );
            r.insert( r.begin() + (size_t)from, clipR.begin(), clipR.end() );
            break;
        case SOP_PASTE_MIX: {
            if ( clipL.empty() ) return false;
            const int64_t cn = (int64_t)clipL.size();
            for ( int64_t i = 0; i < cn && from + i < (int64_t)l.size(); ++i ) {
                l[(size_t)( from + i )] += clipL[(size_t)i];
                r[(size_t)( from + i )] += clipR[(size_t)i];
            }
            break;
        }
        case SOP_DUPLICATE: {
            std::vector<float> dl( l.begin() + (size_t)from, l.begin() + (size_t)to );
            std::vector<float> dr( r.begin() + (size_t)from, r.begin() + (size_t)to );
            l.insert( l.begin() + (size_t)to, dl.begin(), dl.end() );
            r.insert( r.begin() + (size_t)to, dr.begin(), dr.end() );
            break;
        }
        case SOP_INVERT:
            for ( int64_t i = from; i < to; ++i )
            { l[(size_t)i] = -l[(size_t)i]; r[(size_t)i] = -r[(size_t)i]; }
            break;
        case SOP_SWAP_LR:
            for ( int64_t i = from; i < to; ++i ) std::swap( l[(size_t)i], r[(size_t)i] );
            break;
        case SOP_MONO:
            for ( int64_t i = from; i < to; ++i ) {
                const float m = 0.5f * ( l[(size_t)i] + r[(size_t)i] );
                l[(size_t)i] = r[(size_t)i] = m;
            }
            break;
        case SOP_DECLICK: {
            const int64_t f = std::min<int64_t>( len / 2, (int64_t)( sampleRate * 0.002 ) );
            if ( f <= 0 ) return false;
            for ( int64_t i = 0; i < f; ++i ) {
                const float g = (float)i / (float)f;
                l[(size_t)( from + i )] *= g;     r[(size_t)( from + i )] *= g;
                l[(size_t)( to - 1 - i )] *= g;   r[(size_t)( to - 1 - i )] *= g;
            }
            break;
        }
        case SOP_AUTOTRIM: {
            const float thr = arg > 0.f ? arg : 0.002f;
            int64_t a = 0, b = n;
            while ( a < n && std::fabs( l[(size_t)a] ) < thr &&
                             std::fabs( r[(size_t)a] ) < thr ) ++a;
            while ( b > a && std::fabs( l[(size_t)( b - 1 )] ) < thr &&
                             std::fabs( r[(size_t)( b - 1 )] ) < thr ) --b;
            if ( b <= a ) return false;
            std::vector<float> nl( l.begin() + (size_t)a, l.begin() + (size_t)b );
            std::vector<float> nr( r.begin() + (size_t)a, r.begin() + (size_t)b );
            l.swap( nl ); r.swap( nr );
            break;
        }
        case SOP_RESAMPLE: {
            const double f = arg > 0.01f ? (double)arg : 1.0;
            const int64_t m = (int64_t)( (double)n / f );
            if ( m < 2 ) return false;
            std::vector<float> nl( (size_t)m ), nr( (size_t)m );
            for ( int64_t i = 0; i < m; ++i ) {
                const double sp = (double)i * f;
                const int64_t s0 = (int64_t)sp, s1 = std::min( s0 + 1, n - 1 );
                const float fr = (float)( sp - (double)s0 );
                nl[(size_t)i] = l[(size_t)s0] + ( l[(size_t)s1] - l[(size_t)s0] ) * fr;
                nr[(size_t)i] = r[(size_t)s0] + ( r[(size_t)s1] - r[(size_t)s0] ) * fr;
            }
            l.swap( nl ); r.swap( nr );
            break;
        }
        case SOP_BITCRUSH: {
            const float steps = arg >= 2.f ? arg : 16.f;
            for ( int64_t i = from; i < to; ++i ) {
                l[(size_t)i] = std::round( l[(size_t)i] * steps ) / steps;
                r[(size_t)i] = std::round( r[(size_t)i] * steps ) / steps;
            }
            break;
        }
        default: return false;
    }
    return true;
}

//! Nearest zero crossing to `frame` in `l` (dir: -1 back, +1 fwd, 0 either).
inline int64_t find_zero_cross( const std::vector<float>& l, int64_t frame, int dir )
{
    const int64_t n = (int64_t)l.size();
    if ( n < 2 ) return frame;
    frame = frame < 0 ? 0 : ( frame >= n ? n - 1 : frame );
    auto cross = [&]( int64_t i ) {
        if ( i < 1 || i >= n ) return false;
        const float a = l[(size_t)( i - 1 )], b = l[(size_t)i];
        return ( a <= 0.f && b > 0.f ) || ( a >= 0.f && b < 0.f );
    };
    for ( int64_t d = 0; d < 4096; ++d ) {
        if ( dir >= 0 && cross( frame + d ) ) return frame + d;
        if ( dir <= 0 && cross( frame - d ) ) return frame - d;
    }
    return frame;
}

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_SAMPLE_OPS_H
