#include "sample_slot_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sampleslot {

using PatchKnob::engine::SM_START;
using PatchKnob::engine::SM_END;
using PatchKnob::engine::SM_LOOP_START;
using PatchKnob::engine::SM_LOOP_END;
using PatchKnob::engine::SM_COUNT;

namespace {
const int kGrab = 6;         // marker grab tolerance in px
const Uint32 kStatusMs = 6000;   // a status line is a notice, not a log

const char* marker_label( int m ) {
    switch ( m ) {
        case SM_START:      return "S";
        case SM_END:        return "E";
        case SM_LOOP_START: return "LS";
        case SM_LOOP_END:   return "LE";
        default: return "?";
    }
}
//! Spelled out for the hover readout.  "LS" means nothing until someone tells
//! you once, and there was nowhere for that to be said.
const char* marker_name( int m ) {
    switch ( m ) {
        case SM_START:      return "region start";
        case SM_END:        return "region end";
        case SM_LOOP_START: return "loop start";
        case SM_LOOP_END:   return "loop end";
        default: return "marker";
    }
}
bool mod_shift() {
    const auto* keys = SDL_GetKeyboardState( nullptr );
    return keys && ( keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] );
}
bool mod_ctrl() {
    const auto* keys = SDL_GetKeyboardState( nullptr );
    return keys && ( keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL] );
}
} // namespace

//----------------------------------------------------------------------------
//  binding
//----------------------------------------------------------------------------
void SampleSlotEditor::set_slot( ISampleSlot* slot, const std::string& title ) {
    m_slot = slot;
    m_title = title;
    m_viewStart = 0.f; m_viewEnd = 1.f;
    m_peakBuckets = 0; mark_audio_changed();   // force a rebuild
    // Everything below describes the PREVIOUS sample.  Leaving it behind meant
    // a new slot opened with the old selection highlighted over unrelated
    // audio -- and the next Normalize applied to that phantom range.
    m_drag = -1; m_dragSlice = -1; m_dragXfade = false; m_dragXfadeField = false;
    m_selDrag = false; m_hasSel = false; m_selA = m_selB = 0.f;
    m_vZoom = 1.f; m_curX = m_curY = -1; m_hotTool = -1; m_hotMarker = -1;
    m_confirmClearSlices = false;
    m_menu.close();
    m_status.clear(); m_statusMs = 0;
}

void SampleSlotEditor::set_status( const std::string& msg ) {
    m_status = msg;
    m_statusMs = SDL_GetTicks();
    if ( on_status && !msg.empty() ) on_status( msg );
}

//----------------------------------------------------------------------------
//  geometry -- one pass, shared by the painter and every hit-test
//----------------------------------------------------------------------------
void SampleSlotEditor::layout( const ui::Font& f ) {
    const int p    = skin::pad( f );
    const int line = f.ch();

    int y = rect.y;
    m_strip = SDL_Rect{ rect.x, y, rect.w, line + 2 * p };
    y += m_strip.h;

    m_toolbar = SDL_Rect{ rect.x, y, rect.w, 0 };
    build_tools( f );                       // fills m_tools + m_toolbar.h
    y += m_toolbar.h;

    // The editor is also embedded in the sampler window, where it gets a short
    // rect.  Shed the optional rows rather than letting the waveform collapse
    // to nothing: readout first, then the ruler.
    int rulerH  = line + p / 2;
    int footerH = line + p;
    int remain  = rect.y + rect.h - y;
    if ( remain < 28 + rulerH + footerH ) footerH = 0;
    if ( remain < 28 + rulerH )           rulerH  = 0;

    m_ruler = SDL_Rect{ rect.x + p, y, std::max( 8, rect.w - 2 * p ), rulerH };
    y += rulerH;

    int waveH = rect.y + rect.h - y - footerH - p;
    if ( waveH < 16 ) waveH = 16;
    m_wave = SDL_Rect{ rect.x + p, y, std::max( 8, rect.w - 2 * p ), waveH };

    if ( footerH > 0 ) {
        m_footer = SDL_Rect{ rect.x + p, m_wave.y + m_wave.h + p / 2,
                             std::max( 8, rect.w - 2 * p ), footerH };
        // Draggable numeric readout, right-aligned like every other number in
        // this view so the columns line up.
        const int w = f.text_w( "xfade 1000 ms" ) + 2 * p;
        m_xfadeField = SDL_Rect{ m_footer.x + m_footer.w - w, m_footer.y, w, m_footer.h };
        if ( m_xfadeField.w > m_footer.w / 2 ) m_xfadeField = SDL_Rect{ 0, 0, 0, 0 };
    } else {
        m_footer = SDL_Rect{ 0, 0, 0, 0 };
        m_xfadeField = SDL_Rect{ 0, 0, 0, 0 };
    }
}

void SampleSlotEditor::build_tools( const ui::Font& f ) {
    // Only controls whose STATE matters at a glance are buttons.  SET LOOP and
    // SET REGION used to sit here too; they are one-shot actions, so per this
    // project's convention they belong in the right-click menu (and on [ / ]),
    // and the row is now short enough not to wrap in a normal window.
    static const struct { const char* l; const char* tip; int op; } kDefs[] = {
        { "UNDO", "Undo the last edit  (Ctrl+Z)",                    -1  },
        { "REDO", "Redo  (Ctrl+Shift+Z)",                            -2  },
        { "LOOP", "Loop playback between LS and LE",                 -19 },
        { "0X",   "Snap edits to zero crossings -- stops clicks",    -7  },
        { "ST",   "Show two stacked lanes instead of one",           -17 },
        { "FIT",  "Zoom out to the whole file  (Home)",              -12 },
    };
    const int p = skin::pad( f );
    const int h = f.ch() + 2 * p;
    m_tools.clear();
    const int x0 = m_toolbar.x + p, right = m_toolbar.x + m_toolbar.w - p;
    int x = x0, row = 0;
    for ( const auto& d : kDefs ) {
        // MEASURED width.  This was `strlen(label) * 8 + 10` -- a byte count
        // times a nominal advance -- so every label spilled out of its button
        // as soon as the UI scale stopped being exactly 1.0.
        const int w = f.text_w( d.l ) + 4 * p;
        if ( x + w > right && x > x0 ) { x = x0; ++row; }
        m_tools.push_back( Tool{ SDL_Rect{ x, m_toolbar.y + p / 2 + row * ( h + p / 2 ),
                                           w, h }, d.l, d.tip, d.op } );
        x += w + p;
    }
    m_toolRows = row + 1;
    m_toolbar.h = m_toolRows * ( h + p / 2 ) + p / 2;
}

float SampleSlotEditor::x_to_norm( int x ) const {
    float f = (float)( x - m_wave.x ) / (float)( m_wave.w > 1 ? m_wave.w - 1 : 1 );
    f = f < 0.f ? 0.f : ( f > 1.f ? 1.f : f );
    return m_viewStart + f * ( m_viewEnd - m_viewStart );
}

int SampleSlotEditor::norm_to_x( float n ) const {
    const float span = ( m_viewEnd - m_viewStart );
    const float f = span > 1e-6f ? ( n - m_viewStart ) / span : 0.f;
    return m_wave.x + (int)std::lround( f * (float)( m_wave.w - 1 ) );
}

void SampleSlotEditor::clamp_view() {
    if ( m_viewEnd - m_viewStart < 1e-4f ) m_viewEnd = m_viewStart + 1e-4f;
    if ( m_viewEnd - m_viewStart > 1.f )   { m_viewStart = 0.f; m_viewEnd = 1.f; }
    if ( m_viewStart < 0.f ) { m_viewEnd -= m_viewStart; m_viewStart = 0.f; }
    if ( m_viewEnd > 1.f )   { m_viewStart -= ( m_viewEnd - 1.f ); m_viewEnd = 1.f; }
    if ( m_viewStart < 0.f ) m_viewStart = 0.f;
}

void SampleSlotEditor::zoom_at( float factor, int anchorX ) {
    // Zoom around the file position under the POINTER.  Zooming around the
    // centre (what this did) walks whatever you were looking at off the screen
    // after two clicks of the wheel, so you zoom then hunt, zoom then hunt.
    const float at = x_to_norm( anchorX );
    const float frac = ( m_wave.w > 1 )
                     ? (float)( anchorX - m_wave.x ) / (float)( m_wave.w - 1 ) : 0.5f;
    float span = ( m_viewEnd - m_viewStart ) * factor;
    span = std::max( 1e-4f, std::min( 1.f, span ) );
    m_viewStart = at - frac * span;
    m_viewEnd   = m_viewStart + span;
    clamp_view();
    m_peakFrames = -1;
}

bool SampleSlotEditor::wants_drop( int x, int y ) const {
    return m_slot && x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
}

bool SampleSlotEditor::load_path( const std::string& path ) {
    if ( !m_slot ) return false;
    std::string err;
    if ( !m_slot->sampleLoad( path, m_sr, &err ) ) {
        set_status( "load failed: " + err );
        return false;
    }
    m_viewStart = 0.f; m_viewEnd = 1.f;
    mark_audio_changed();                    // invalidate peaks, mip and stats
    // The incoming file is a different length; a selection expressed as a
    // fraction of the OLD one points at arbitrary audio in the new one.
    m_hasSel = false; m_selA = m_selB = 0.f; m_selDrag = false;
    m_drag = -1; m_dragSlice = -1; m_dragXfade = false;
    m_vZoom = 1.f;
    m_status.clear(); m_statusMs = 0;
    if ( on_loaded ) on_loaded( path );
    return true;
}

void SampleSlotEditor::rebuild_peaks( int buckets ) {
    if ( !m_slot || buckets <= 0 ) { m_min.clear(); m_max.clear(); m_peakBuckets = 0; return; }
    const int frames = m_slot->sampleFrames();
    // Only refetch when something that changes the picture changed: the file,
    // the zoom window, or the width.  Peak extraction walks the whole visible
    // span, so doing it every frame on a long sample is needlessly expensive.
    if ( buckets == m_peakBuckets && frames == m_peakFrames &&
         m_peakVs == m_viewStart && m_peakVe == m_viewEnd )
        return;
    m_min.assign( (size_t)buckets, 0.f );
    m_max.assign( (size_t)buckets, 0.f );
    // Anything that invalidated the peak cache may have changed the audio, so
    // the footer's cached peak/RMS must be re-measured too (in-place edits keep
    // the frame count, which is why the stat cache cannot key on that alone).
    m_statFrom = m_statTo = -1;
    const int64_t from = (int64_t)( (double)frames * (double)m_viewStart );
    const int64_t to   = (int64_t)( (double)frames * (double)m_viewEnd );
    // Zoomed OUT, columns are reduced from the full-file mip (built once per
    // audio change) instead of re-walking the whole visible span every zoom or
    // scroll step.  Zoomed IN the span is small, so the exact walk stays.
    constexpr int kMipB = 256;
    const double perCol = (double)( to - from ) / (double)buckets;
    if ( frames > 0 && perCol >= 2.0 * (double)kMipB ) {
        if ( m_mipFrames != frames ) {
            const int nb = (int)( ( (int64_t)frames + kMipB - 1 ) / kMipB );
            m_mipMin.assign( (size_t)nb, 0.f );
            m_mipMax.assign( (size_t)nb, 0.f );
            if ( m_slot->samplePeaks( 0, frames, m_mipMin.data(), m_mipMax.data(), nb ) > 0 )
                m_mipFrames = frames;
            else { m_mipMin.clear(); m_mipMax.clear(); m_mipFrames = -1; }
        }
        if ( m_mipFrames == frames && !m_mipMin.empty() ) {
            const double per = (double)frames / (double)m_mipMin.size();
            for ( int b = 0; b < buckets; ++b ) {
                const int64_t s = from + (int64_t)( perCol * b );
                int64_t e = from + (int64_t)( perCol * ( b + 1 ) );
                if ( e <= s ) e = s + 1;
                size_t k0 = (size_t)std::max( 0.0, std::floor( (double)s / per ) );
                size_t k1 = (size_t)std::min( (double)m_mipMin.size(),
                                              std::ceil( (double)e / per ) );
                if ( k1 <= k0 ) k1 = std::min( m_mipMin.size(), k0 + 1 );
                float mn = 1.f, mx = -1.f;
                for ( size_t k = k0; k < k1; ++k ) {
                    if ( m_mipMin[k] < mn ) mn = m_mipMin[k];
                    if ( m_mipMax[k] > mx ) mx = m_mipMax[k];
                }
                if ( mx < mn ) mn = mx = 0.f;
                m_min[(size_t)b] = mn; m_max[(size_t)b] = mx;
            }
            m_peakBuckets = buckets; m_peakFrames = frames;
            m_peakVs = m_viewStart; m_peakVe = m_viewEnd;
            return;
        }
    }
    const int got = m_slot->samplePeaks( from, to, m_min.data(), m_max.data(), buckets );
    if ( got <= 0 ) { m_min.assign( (size_t)buckets, 0.f ); m_max = m_min; }
    m_peakBuckets = buckets; m_peakFrames = frames;
    m_peakVs = m_viewStart; m_peakVe = m_viewEnd;
}

void SampleSlotEditor::clamp_markers() {
    if ( !m_slot ) return;
    float s = m_slot->sampleMarker( SM_START );
    float e = m_slot->sampleMarker( SM_END );
    if ( e < s + 1e-4f ) { e = std::min( 1.f, s + 1e-4f ); m_slot->sampleSetMarker( SM_END, e ); }
    float ls = m_slot->sampleMarker( SM_LOOP_START );
    float le = m_slot->sampleMarker( SM_LOOP_END );
    // The loop has to live inside the playback region, or it plays audio the
    // region excludes.
    ls = std::max( s, std::min( ls, e ) );
    le = std::max( ls + 1e-4f, std::min( le, e ) );
    m_slot->sampleSetMarker( SM_LOOP_START, ls );
    m_slot->sampleSetMarker( SM_LOOP_END, le );
}

int SampleSlotEditor::marker_at( int x, int y ) const {
    if ( !m_slot ) return -1;
    if ( y < m_ruler.y || y > m_wave.y + m_wave.h ) return -1;
    int best = -1, bestD = kGrab + 1;
    for ( int m = 0; m < SM_COUNT; ++m ) {
        if ( !m_slot->sampleMarkerActive( m ) ) continue;
        const int mx = norm_to_x( m_slot->sampleMarker( m ) );
        // A marker scrolled out of the visible window still had a computed x a
        // few pixels off the edge, so clicking near the border grabbed a marker
        // that was not on screen.
        if ( mx < m_wave.x - kGrab || mx > m_wave.x + m_wave.w + kGrab ) continue;
        const int d = std::abs( x - mx );
        if ( d < bestD ) { bestD = d; best = m; }
    }
    return best;
}

bool SampleSlotEditor::update_hover( int mx, int my ) {
    // A drag owns the highlight until it ends; re-deriving it from the pointer
    // mid-drag makes the marker you are holding flicker as it passes others.
    if ( m_drag >= 0 || m_dragSlice >= 0 || m_dragXfade || m_dragXfadeField || m_selDrag )
        return false;
    const int prevTool = m_hotTool, prevMark = m_hotMarker, prevX = m_curX;
    const bool over = hit( mx, my );
    m_hotTool = -1;
    if ( over )
        for ( size_t i = 0; i < m_tools.size(); ++i ) {
            const SDL_Rect& tr = m_tools[i].r;
            if ( mx >= tr.x && mx < tr.x + tr.w && my >= tr.y && my < tr.y + tr.h )
            { m_hotTool = (int)i; break; }
        }
    m_hotMarker = over ? marker_at( mx, my ) : -1;
    const bool inWave = over && mx >= m_wave.x && mx < m_wave.x + m_wave.w &&
                        my >= m_wave.y && my < m_wave.y + m_wave.h;
    m_curX = inWave ? mx : -1;
    m_curY = my;
    return prevTool != m_hotTool || prevMark != m_hotMarker || prevX != m_curX;
}

//----------------------------------------------------------------------------
//  enablement -- ONE predicate, read by the painter and the click handler
//----------------------------------------------------------------------------
bool SampleSlotEditor::op_enabled( int op ) const {
    using namespace PatchKnob::engine;
    if ( !m_slot ) return false;
    if ( op <= -1000 ) return true;                  // host-supplied zone actions
    const bool ed  = m_slot->sampleEditable();
    const bool sel = has_sel();
    const bool any = m_slot->sampleFrames() > 0;
    switch ( op ) {
        case -1:  return m_slot->sampleCanUndo();
        case -2:  return m_slot->sampleCanRedo();
        case -3:  return sel;                         // zoom to selection
        case -4:  return any;                         // select all / none
        case -5:  case -6: return sel && any;         // region/loop := selection
        case -7:  case -17: return true;              // display toggles
        case -12: case -13: case -14: return any;     // view
        case -18: return any;                         // save as wav
        case -19: return any;                         // loop on/off
        case -20: return m_slot->slicesSupported() && m_slot->sliceCount() > 0;
        case -21: return m_menuSlice >= 0;
        case -22: return m_menuSlice >= 0;
        case SOP_COPY:         return any && sel;
        case SOP_PASTE_INSERT:
        case SOP_PASTE_MIX:    return ed && m_slot->sampleHasClipboard();
        // Range-destructive.  With no selection op_range() falls back to the
        // WHOLE file, so "Delete" on an untouched view silently wiped the
        // sample.  These now require a selection, which is the confirmation.
        case SOP_CUT: case SOP_DELETE: case SOP_TRIM: case SOP_SILENCE:
        case SOP_DUPLICATE: case SOP_INSERT_SILENCE:
            return ed && sel;
        case SOP_CROSSFADE_LOOP:
            return ed && m_slot->sampleMarkerActive( SM_LOOP_START );
        default: return ed && any;                    // whole-file processing
    }
}

void SampleSlotEditor::build_menu() {
    using namespace PatchKnob::engine;
    auto row = [&]( const char* label, int op ) {
        m_menu.rows.push_back( skin::MenuRow{ label, op, false, op_enabled( op ), false, false } );
    };
    auto sep = [&]() {
        m_menu.rows.push_back( skin::MenuRow{ "", 0, true, true, false, false } );
    };
    auto chk = [&]( const char* label, int op, bool on ) {
        m_menu.rows.push_back( skin::MenuRow{ label, op, false, op_enabled( op ), true, on } );
    };
    m_menu.rows.clear();

    if ( m_menuSlice >= 0 ) {
        // Right-clicking a slice used to DELETE it instantly -- no undo, no
        // confirmation, and no way to discover the gesture either.  It is a
        // menu now, and clearing every slice at once has to be picked twice.
        char head[48];
        std::snprintf( head, sizeof head, "Slice %d", m_menuSlice + 1 );
        m_menu.rows.push_back( skin::MenuRow{ head, 0, false, false, false, false } );
        sep();
        row( "Remove this slice", -21 );
        row( "Snap slice to zero crossing", -22 );
        row( m_confirmClearSlices ? "Clear ALL slices -- click again" : "Clear all slices", -20 );
        return;
    }

    row( "Cut",                SOP_CUT );
    row( "Copy",               SOP_COPY );
    row( "Paste (insert)",     SOP_PASTE_INSERT );
    row( "Paste (mix)",        SOP_PASTE_MIX );
    row( "Delete",             SOP_DELETE );
    row( "Duplicate",          SOP_DUPLICATE );
    sep();
    row( "Trim to selection",  SOP_TRIM );
    row( "Insert silence",     SOP_INSERT_SILENCE );
    row( "Silence",            SOP_SILENCE );
    sep();
    row( "Normalize",          SOP_NORMALIZE );
    row( "Gain +1.5 dB",       -8 );
    row( "Gain -1.5 dB",       -9 );
    row( "Reverse",            SOP_REVERSE );
    row( "Invert phase",       SOP_INVERT );
    row( "Fade in",            SOP_FADE_IN );
    row( "Fade out",           SOP_FADE_OUT );
    row( "Remove DC",          SOP_DC_REMOVE );
    row( "Declick edges",      SOP_DECLICK );
    row( "Auto-trim silence",  SOP_AUTOTRIM );
    sep();
    row( "Swap L/R",           SOP_SWAP_LR );
    row( "Sum to mono",        SOP_MONO );
    row( "Bitcrush",           SOP_BITCRUSH );
    row( "Half speed",         -10 );
    row( "Double speed",       -11 );
    sep();
    // The destructive loop crossfade had no entry anywhere in the UI: the code
    // that performs it was reachable only from a keystroke nobody bound.  The
    // length it uses is the one shown (and dragged) in the readout below.
    {
        char lab[64];
        std::snprintf( lab, sizeof lab, "Crossfade loop (%.0f ms)", (double)m_xfadeMs );
        m_menu.rows.push_back( skin::MenuRow{ lab, SOP_CROSSFADE_LOOP, false,
                                              op_enabled( SOP_CROSSFADE_LOOP ), false, false } );
    }
    row( "Region = selection", -5 );
    row( "Loop = selection",   -6 );
    row( "Select all / none",  -4 );
    row( "Zoom to selection",  -3 );
    sep();
    chk( "Snap to zero crossings", -7,  m_zeroSnap );
    chk( "Two lanes",              -17, m_stereo );
    chk( "Loop playback",          -19, m_slot && m_slot->sampleLoopEnabled() );
    sep();
    row( "Save as WAV...",     -18 );

    if ( !m_contextActions.empty() ) {
        sep();
        for ( size_t i = 0; i < m_contextActions.size(); ++i )
            m_menu.rows.push_back( skin::MenuRow{ m_contextActions[i].label,
                                                  -1000 - (int)i, false, true, false, false } );
    }
}

void SampleSlotEditor::open_menu( int x, int y, bool sliceMenu ) {
    if ( !sliceMenu ) { m_menuSlice = -1; m_confirmClearSlices = false; }
    build_menu();
    m_menu.show( x, y );
}

//----------------------------------------------------------------------------
//  draw
//----------------------------------------------------------------------------
void SampleSlotEditor::draw( ui::App& app ) {
    SDL_Renderer* r = app.ren;
    const ui::Font& f = app.mono;
    const ui::Theme& t = ui::theme();
    layout( f );
    const int p = skin::pad( f );
    {
        // Poll the pointer rather than trusting the last event: motion events
        // only reach a widget while the pointer is inside it, so a button lit
        // on the way out stayed lit (with its tooltip) until you came back.
        int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
        update_hover( mx, my );
    }

    ui::fill_rect( r, rect, t.bg );

    const int frames = m_slot ? m_slot->sampleFrames() : 0;
    const double sr = ( m_slot && m_slot->sampleRate() > 0.0 ) ? m_slot->sampleRate() : 48000.0;

    // ---- title strip -------------------------------------------------------
    ui::fill_rect( r, m_strip, t.panel );
    ui::hline( r, m_strip.x, m_strip.x + m_strip.w - 1, m_strip.y + m_strip.h - 1,
               ui::Color{ t.dim.r, t.dim.g, t.dim.b, 120 } );
    {
        // The status used to be painted one line BELOW the strip -- straight
        // under the toolbar, which is drawn afterwards and covered it.  Load
        // errors were therefore invisible.  It lives in the strip now, right
        // aligned, and it expires.
        if ( m_statusMs && SDL_GetTicks() - m_statusMs > kStatusMs ) {
            m_status.clear(); m_statusMs = 0;
        }
        std::string right = m_status;
        if ( right.empty() && m_hotTool >= 0 && m_hotTool < (int)m_tools.size() )
            right = m_tools[(size_t)m_hotTool].tip;
        const int rw = right.empty() ? 0
                     : std::min( f.text_w( right ) + 2 * p, m_strip.w / 2 );
        const std::string head = m_title.empty() ? std::string( "Sample" ) : m_title;
        int x = m_strip.x + 2 * p;
        const int avail = m_strip.w - 4 * p - rw;
        const std::string headFit = skin::fit( f, head, avail );
        f.draw( r, x, m_strip.y + p, headFit, t.text );
        x += f.text_w( headFit ) + 2 * p;
        if ( m_slot && frames > 0 ) {
            char buf[160];
            std::snprintf( buf, sizeof buf, "%s   %.2f s   %d fr   %.1f kHz",
                           m_slot->sampleName(), (double)frames / sr, frames, sr / 1000.0 );
            f.draw( r, x, m_strip.y + p,
                    skin::fit( f, buf, m_strip.x + 2 * p + avail - x ), t.dim );
        }
        if ( !right.empty() ) {
            const std::string s = skin::fit( f, right, rw - p );
            f.draw( r, m_strip.x + m_strip.w - 2 * p - f.text_w( s ), m_strip.y + p, s,
                    m_status.empty() ? t.dim : t.accent );
        }
    }

    // ---- toolbar -----------------------------------------------------------
    for ( size_t i = 0; i < m_tools.size(); ++i ) {
        const Tool& tl = m_tools[i];
        const bool on  = op_enabled( tl.op );
        const bool lit = ( tl.op == -7  && m_zeroSnap )
                      || ( tl.op == -17 && m_stereo )
                      || ( tl.op == -19 && m_slot && m_slot->sampleLoopEnabled() );
        const bool hot = ( (int)i == m_hotTool );
        skin::pill( r, f, tl.r, tl.label,
                    lit ? skin::StActive
                        : ( !on ? skin::StDisabled
                                : ( hot ? skin::StHover : skin::StIdle ) ) );
    }

    // ---- ruler -------------------------------------------------------------
    if ( m_ruler.h > 0 && m_slot && frames > 0 ) {
        const double t0 = (double)frames * m_viewStart / sr;
        const double t1 = (double)frames * m_viewEnd   / sr;
        const double vis = t1 - t0;
        if ( vis > 1e-9 ) {
            static const double kSteps[] = { 0.001,0.005,0.01,0.05,0.1,0.25,0.5,1,2,5,10,30,60 };
            const int nSteps = (int)( sizeof kSteps / sizeof kSteps[0] );
            const double want = vis * (double)( f.cw() * 8 ) / (double)std::max( 1, m_wave.w );
            double step = kSteps[nSteps - 1];
            for ( int i = 0; i < nSteps; ++i ) if ( kSteps[i] >= want ) { step = kSteps[i]; break; }
            ui::ScopedClip clip( r, m_ruler );
            for ( double tt = std::ceil( t0 / step ) * step; tt < t1; tt += step ) {
                const int tx = norm_to_x( (float)( tt * sr / (double)frames ) );
                ui::vline( r, tx, m_ruler.y + m_ruler.h - 3, m_ruler.y + m_ruler.h - 1,
                           ui::Color{ t.dim.r, t.dim.g, t.dim.b, 200 } );
                char lab[24];
                std::snprintf( lab, sizeof lab, step < 1.0 ? "%.3g" : "%.0f", tt );
                f.draw( r, tx + 2, m_ruler.y, lab, ui::Color{ t.dim.r, t.dim.g, t.dim.b, 220 } );
            }
        }
    }

    // ---- waveform panel ----------------------------------------------------
    const SDL_Rect w = m_wave;
    skin::fill_round( r, w, std::max( 2, p ), t.bg );
    skin::frame_round( r, w, std::max( 2, p ),
                       ui::Color{ t.dim.r, t.dim.g, t.dim.b, 150 } );

    if ( !m_slot || frames <= 0 ) {
        // An empty editor should say what to DO, not just that it is empty.
        const char* l0 = m_slot ? "No sample loaded" : "No sample slot selected";
        const char* l1 = m_slot ? "Drag a file from the browser onto this panel"
                                : "Open a sampler module and pick a slot";
        const char* l2 = m_slot ? "or double-click it in the list" : "";
        const char* l3 = m_slot ? "Right-click here for the edit menu" : "";
        const int lh = f.ch() + p / 2;
        int cy = w.y + w.h / 2 - lh * 2;
        auto center = [&]( const char* s, ui::Color c ) {
            if ( !s || !*s ) return;
            const std::string fs = skin::fit( f, s, w.w - 4 * p );
            f.draw( r, w.x + ( w.w - f.text_w( fs ) ) / 2, cy, fs, c );
            cy += lh;
        };
        center( l0, t.text );
        cy += p;
        center( l1, t.dim );
        center( l2, t.dim );
        center( l3, t.dim );
    } else {
        rebuild_peaks( w.w );
        ui::ScopedClip clip( r, w );

        const int midY = w.y + w.h / 2;
        const int half = std::max( 1, (int)( (float)( w.h / 2 - 2 ) * m_vZoom ) );
        // -6 / -12 dB guides, so level is readable rather than guessed at.
        for ( int gi = 0; gi < 2; ++gi ) {
            const float db = gi == 0 ? 0.5f : 0.25f;
            const int dy = (int)std::lround( db * half );
            const ui::Color g{ t.dim.r, t.dim.g, t.dim.b, 70 };
            ui::hline( r, w.x + 1, w.x + w.w - 2, midY - dy, g );
            ui::hline( r, w.x + 1, w.x + w.w - 2, midY + dy, g );
            if ( w.h > f.ch() * 5 )
                f.draw( r, w.x + 3, midY - dy - f.ch() - 1, gi == 0 ? "-6" : "-12",
                        ui::Color{ t.dim.r, t.dim.g, t.dim.b, 150 } );
        }
        ui::hline( r, w.x + 1, w.x + w.w - 2, midY,
                   ui::Color{ t.dim.r, t.dim.g, t.dim.b, 210 } );

        ui::set_color( r, t.text );
        const int lim = w.h / 2 - 1;
        auto clipY = [&]( int dy ) { return dy < -lim ? -lim : ( dy > lim ? lim : dy ); };
        if ( m_stereo ) {
            // Two stacked lanes.  samplePeaks() gives a mono sum, so the lanes
            // are the same shape at half height -- honest about what the data
            // is rather than pretending to show independent channels.
            const int q = std::max( 2, w.h / 4 );
            for ( int lane = 0; lane < 2; ++lane ) {
                const int ly = w.y + q + lane * ( w.h / 2 );
                ui::hline( r, w.x + 1, w.x + w.w - 2, ly,
                           ui::Color{ t.dim.r, t.dim.g, t.dim.b, 180 } );
                ui::set_color( r, t.text );
                for ( int i = 0; i < (int)m_min.size() && i < w.w; ++i ) {
                    const int hh = std::max( 1, (int)( (float)q * m_vZoom ) );
                    int y0 = ly - (int)std::lround( m_max[(size_t)i] * hh );
                    int y1 = ly - (int)std::lround( m_min[(size_t)i] * hh );
                    y0 = std::max( y0, ly - q ); y1 = std::min( y1, ly + q );
                    SDL_RenderDrawLine( r, w.x + i, y0, w.x + i, y1 );
                }
                f.draw( r, w.x + 4, ly - q + 1, lane ? "R" : "L",
                        ui::Color{ t.dim.r, t.dim.g, t.dim.b, 200 } );
            }
        } else {
            for ( int i = 0; i < (int)m_min.size() && i < w.w; ++i ) {
                const int y0 = midY - clipY( (int)std::lround( m_max[(size_t)i] * half ) );
                const int y1 = midY - clipY( (int)std::lround( m_min[(size_t)i] * half ) );
                SDL_RenderDrawLine( r, w.x + i, y0, w.x + i, y1 );
            }
        }

        // ---- selection -----------------------------------------------------
        if ( has_sel() ) {
            const int a = norm_to_x( sel_lo() ), b = norm_to_x( sel_hi() );
            skin::wash_blend( r, SDL_Rect{ a, w.y + 1, std::max( 1, b - a ), w.h - 2 },
                              t.accent, 60 );
            ui::vline( r, a, w.y + 1, w.y + w.h - 2, t.accent );
            ui::vline( r, b, w.y + 1, w.y + w.h - 2, t.accent );
        }

        // ---- region shading: outside START..END is not played ---------------
        const int sx = norm_to_x( m_slot->sampleMarker( SM_START ) );
        const int ex = norm_to_x( m_slot->sampleMarker( SM_END ) );
        if ( sx > w.x )
            skin::wash_blend( r, SDL_Rect{ w.x + 1, w.y + 1, sx - w.x - 1, w.h - 2 }, t.bg, 175 );
        if ( ex < w.x + w.w - 1 )
            skin::wash_blend( r, SDL_Rect{ ex, w.y + 1, w.x + w.w - ex - 1, w.h - 2 }, t.bg, 175 );
        if ( m_slot->sampleMarkerActive( SM_LOOP_START ) ) {
            const int lsx = norm_to_x( m_slot->sampleMarker( SM_LOOP_START ) );
            const int lex = norm_to_x( m_slot->sampleMarker( SM_LOOP_END ) );
            skin::wash_blend( r, SDL_Rect{ lsx, w.y + 1, std::max( 1, lex - lsx ), w.h - 2 },
                              t.accent, 42 );
        }

        // ---- slice lane -----------------------------------------------------
        if ( m_slot->slicesSupported() ) {
            const int n = m_slot->sliceCount();
            for ( int i = 0; i < n; ++i ) {
                const int sxp = norm_to_x( m_slot->sliceAt( i ) );
                if ( sxp < w.x || sxp > w.x + w.w - 1 ) continue;
                const bool hot = ( m_dragSlice == i || m_menuSlice == i );
                const ui::Color c = hot ? t.accent : ui::Color{ t.dim.r, t.dim.g, t.dim.b, 235 };
                ui::set_color( r, c );
                for ( int y = w.y + 1; y < w.y + w.h - 1; y += 4 )  // dashed: reads as
                    SDL_RenderDrawLine( r, sxp, y, sxp, y + 1 );    // a cut, not an edge
                char num[8]; std::snprintf( num, sizeof num, "%d", i + 1 );
                f.draw( r, sxp + 2, w.y + 2, num, c );
            }
        }

        // ---- loop crossfade (non-destructive) -------------------------------
        // Drawn as the ramp it actually is: the shaded wedge before the loop
        // end is the span that fades into the pre-loop-start audio at playback.
        if ( m_slot->sampleXfadeSupported() &&
             m_slot->sampleMarkerActive( SM_LOOP_START ) ) {
            const float xf = m_slot->sampleXfade();
            if ( xf > 0.f ) {
                const float le = m_slot->sampleMarker( SM_LOOP_END );
                const float ls = m_slot->sampleMarker( SM_LOOP_START );
                struct Span { int a, b; bool fadeOut; };
                const Span spans[2] = {
                    { norm_to_x( std::max( 0.f, le - xf ) ), norm_to_x( le ), true  },
                    { norm_to_x( std::max( 0.f, ls - xf ) ), norm_to_x( ls ), false }
                };
                SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_BLEND );
                for ( int si = 0; si < 2; ++si ) {
                    const Span& sp = spans[si];
                    for ( int px2 = sp.a; px2 < sp.b; ++px2 ) {
                        float fr = ( sp.b > sp.a )
                                 ? (float)( px2 - sp.a ) / (float)( sp.b - sp.a ) : 0.f;
                        if ( sp.fadeOut ) fr = 1.f - fr;      // bright where it is loud
                        ui::set_color( r, ui::Color{ t.accent.r, t.accent.g, t.accent.b,
                                                     (Uint8)( 18 + fr * 72.f ) } );
                        SDL_RenderDrawLine( r, px2, w.y + 1, px2, w.y + w.h - 2 );
                    }
                }
                SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_NONE );
                for ( int si = 0; si < 2; ++si ) {
                    SDL_Rect hb{ spans[si].a - 3, w.y + w.h / 2 - 5, 7, 11 };
                    skin::fill_round( r, hb, 2, m_dragXfade ? t.accent : t.text );
                    skin::frame_round( r, hb, 2, t.bg );
                }
                f.draw( r, spans[0].a + 6, w.y + w.h / 2 - f.ch() / 2, "XF", t.accent );
            }
        }

        // ---- markers --------------------------------------------------------
        for ( int m = 0; m < SM_COUNT; ++m ) {
            if ( !m_slot->sampleMarkerActive( m ) ) continue;
            const int mx = norm_to_x( m_slot->sampleMarker( m ) );
            const bool hot = ( m == m_drag || m == m_hotMarker );
            const ui::Color c = hot ? t.accent : ( m <= SM_END ? t.text : t.dim );
            ui::vline( r, mx, w.y + 1, w.y + w.h - 2, c );
            if ( hot ) ui::vline( r, mx + 1, w.y + 1, w.y + w.h - 2,
                                  ui::Color{ c.r, c.g, c.b, 110 } );
            const int tagW = f.text_w( marker_label( m ) ) + p;
            SDL_Rect tag{ mx, w.y + 1, tagW, f.ch() + 1 };
            skin::fill_round( r, tag, 2, c );
            f.draw( r, mx + p / 2, w.y + 1, marker_label( m ), t.bg );
        }
    }

    if ( m_dragOver ) {
        // A drop target has to look like one BEFORE the button is released.
        skin::wash_blend( r, w, t.accent, 34 );
        skin::frame_round( r, w, std::max( 2, p ), t.accent );
    }

    // ---- footer readout ----------------------------------------------------
    if ( m_footer.h > 0 ) {
        std::string left, num;
        if ( m_hotMarker >= 0 && m_slot && frames > 0 ) {
            left = marker_name( m_hotMarker );
            char b[64];
            std::snprintf( b, sizeof b, "%.4f s",
                (double)m_slot->sampleMarker( m_hotMarker ) * (double)frames / sr );
            num = b;
        } else if ( has_sel() && m_slot && frames > 0 ) {
            const int64_t a = sel_from(), b2 = sel_to();
            // Only when the selection (or the audio -- see rebuild_peaks)
            // changed: sampleStats() walks the span sample by sample, and
            // running it per frame measured ~45 ms/frame on a long selection.
            if ( a != m_statFrom || b2 != m_statTo || frames != m_statFrames ) {
                m_statOk = m_slot->sampleStats( a, b2, &m_statPeak, &m_statRms );
                m_statFrom = a; m_statTo = b2; m_statFrames = frames;
            }
            const float pk = m_statPeak, rmsv = m_statRms;
            const bool got = m_statOk;
            auto db = []( float v ) { return v > 1e-6f ? 20.f * std::log10( v ) : -99.f; };
            char b[192];
            std::snprintf( b, sizeof b,
                "%.3f - %.3f s   %lld fr   peak %.1f dB   rms %.1f dB",
                (double)a / sr, (double)b2 / sr, (long long)( b2 - a ),
                got ? db( pk ) : -99.f, got ? db( rmsv ) : -99.f );
            left = "selection"; num = b;
        } else if ( m_curX >= 0 && m_slot && frames > 0 ) {
            const double pos = (double)x_to_norm( m_curX ) * (double)frames;
            char b[96];
            std::snprintf( b, sizeof b, "%.4f s   frame %lld", pos / sr, (long long)pos );
            left = "position"; num = b;
        } else {
            left = "wheel zooms   shift+wheel scrolls   right-click for edits";
        }
        const int fieldW = m_xfadeField.w;
        int numRight = m_footer.x + m_footer.w - ( fieldW ? fieldW + 2 * p : 0 );
        if ( !num.empty() ) {
            // Numbers are right aligned so successive readings sit in the same
            // column instead of jittering as the digit count changes.
            const std::string s = skin::fit( f, num, numRight - m_footer.x - f.text_w( left ) - 4 * p );
            f.draw( r, numRight - f.text_w( s ), m_footer.y, s, t.text );
            numRight -= f.text_w( s ) + 2 * p;
        }
        f.draw( r, m_footer.x, m_footer.y,
                skin::fit( f, left, std::max( 0, numRight - m_footer.x ) ), t.dim );

        if ( fieldW > 0 ) {
            char b[32]; std::snprintf( b, sizeof b, "xfade %.0f ms", (double)m_xfadeMs );
            int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
            const bool hot = m_dragXfadeField ||
                             ( mx >= m_xfadeField.x && mx < m_xfadeField.x + m_xfadeField.w &&
                               my >= m_xfadeField.y && my < m_xfadeField.y + m_xfadeField.h );
            skin::pill( r, f, m_xfadeField, b,
                        m_dragXfadeField ? skin::StActive
                                         : ( hot ? skin::StHover : skin::StIdle ) );
        }
    }

    // ---- tooltip -----------------------------------------------------------
    // "0X" and "ST" are unguessable.  The tip appears the moment the pointer is
    // over the button, under it, clamped inside the view.
    if ( m_hotTool >= 0 && m_hotTool < (int)m_tools.size() && !m_menu.open() ) {
        const Tool& tl = m_tools[(size_t)m_hotTool];
        const std::string tip = tl.tip;
        const int tw = f.text_w( tip ) + 4 * p;
        SDL_Rect box{ tl.r.x, tl.r.y + tl.r.h + p / 2, tw, f.ch() + 2 * p };
        if ( box.x + box.w > rect.x + rect.w ) box.x = rect.x + rect.w - box.w;
        box.x = std::max( box.x, rect.x );
        if ( box.y + box.h > rect.y + rect.h ) box.y = tl.r.y - box.h - p / 2;
        skin::fill_round( r, SDL_Rect{ box.x + 2, box.y + 2, box.w, box.h }, p,
                          ui::Color{ 0, 0, 0, 70 } );
        skin::fill_round( r, box, p, t.panel );
        skin::frame_round( r, box, p, t.accent );
        f.draw( r, box.x + 2 * p, box.y + p, tip, t.text );
    }

    // The menu is drawn LAST and unconditionally.  It used to sit after an
    // early return taken whenever the slot was empty, so right-clicking an
    // empty editor opened an invisible menu that then ate the next click.
    if ( m_menu.open() ) {
        m_menu.layout( f, rect );
        m_menu.draw( app, f );
    }
}

//----------------------------------------------------------------------------
//  mouse
//----------------------------------------------------------------------------
bool SampleSlotEditor::on_mouse( ui::App& app, const ui::MouseEv& e ) {
    layout( app.mono );
    const bool leftNow  = e.pressed && e.button == SDL_BUTTON_LEFT;
    const bool leftDownEdge = leftNow && !m_leftDown;
    const bool rightNow = e.pressed && e.button == SDL_BUTTON_RIGHT;
    const bool rightDownEdge = rightNow && !m_rightDown;
    if ( !e.pressed ) { m_leftDown = false; m_rightDown = false; }
    else if ( e.button == SDL_BUTTON_LEFT )  m_leftDown = true;
    else if ( e.button == SDL_BUTTON_RIGHT ) m_rightDown = true;

    // ---- open menu owns the pointer ----------------------------------------
    if ( m_menu.open() ) {
        m_menu.layout( app.mono, rect );
        if ( !e.pressed ) { m_menu.arm(); app.request_redraw(); return true; }
        // The right button that OPENED the menu is still down, and motion
        // events carry pressed == true: without arming on the first release,
        // nudging the mouse closed the menu and fired whatever was under it.
        if ( !m_menu.armed() ) { app.request_redraw(); return true; }
        if ( leftDownEdge || rightDownEdge ) {
            const int row = m_menu.hit( e.x, e.y );
            if ( row >= 0 ) {
                const int op = m_menu.rows[(size_t)row].op;
                m_menu.close();
                run_tool( op );
            } else if ( !m_menu.inside( e.x, e.y ) ) {
                m_menu.close();
                m_menuSlice = -1; m_confirmClearSlices = false;
            }
            // Landing on a greyed row or a separator leaves the menu open: it
            // is a mis-aim, and closing it made you re-open and re-find it.
            app.request_redraw();
        }
        return true;
    }

    // ---- hover state (the SAME updater the painter uses) -------------------
    // Ahead of the empty-slot bail-out, so the toolbar still lights and still
    // explains itself on a panel that has nothing loaded yet.
    if ( update_hover( e.x, e.y ) ) app.request_redraw();

    if ( !m_slot ) {
        // Even with no slot bound the menu must be reachable, or the panel is
        // a dead rectangle with no way to discover what it wants.
        if ( rightDownEdge && hit( e.x, e.y ) ) {
            open_menu( e.x, e.y, false );
            app.request_redraw();
            return true;
        }
        return false;
    }

    const SDL_Rect w = m_wave;
    const bool inWave = e.x >= w.x && e.x < w.x + w.w && e.y >= w.y && e.y < w.y + w.h;

    // ---- toolbar -----------------------------------------------------------
    if ( leftDownEdge && m_hotTool >= 0 ) {
        const Tool& tl = m_tools[(size_t)m_hotTool];
        // A greyed-out button used to be fully clickable: the paint code asked
        // whether the op was available, the click code did not.
        if ( op_enabled( tl.op ) ) run_tool( tl.op );
        else set_status( "not available right now" );
        app.request_redraw();
        return true;
    }
    if ( rightDownEdge && m_hotTool >= 0 ) {
        open_menu( e.x, e.y, false );
        app.request_redraw();
        return true;
    }

    // ---- crossfade length field --------------------------------------------
    if ( m_xfadeField.w > 0 && leftDownEdge &&
         e.x >= m_xfadeField.x && e.x < m_xfadeField.x + m_xfadeField.w &&
         e.y >= m_xfadeField.y && e.y < m_xfadeField.y + m_xfadeField.h ) {
        m_dragXfadeField = true;
        m_dragOriginX = e.x; m_dragOriginV = m_xfadeMs;
        app.request_redraw();
        return true;
    }
    if ( m_dragXfadeField ) {
        if ( !e.pressed ) m_dragXfadeField = false;
        else {
            // Shift is a fine drag everywhere in this view; without it a
            // 20 ms crossfade could not be dialled in at all on a wide window.
            const float per = mod_shift() ? 0.25f : 2.f;
            m_xfadeMs = m_dragOriginV + (float)( e.x - m_dragOriginX ) * per;
            m_xfadeMs = std::max( 1.f, std::min( 1000.f, m_xfadeMs ) );
        }
        app.request_redraw();
        return true;
    }

    // ---- slices -------------------------------------------------------------
    // Region markers still win the hit-test, so a slice can never be grabbed
    // instead of START/END.
    if ( m_slot->slicesSupported() && ( leftDownEdge || rightDownEdge ) && inWave ) {
        int sHit = -1, sDist = kGrab + 1;
        for ( int i = 0; i < m_slot->sliceCount(); ++i ) {
            const int d = std::abs( e.x - norm_to_x( m_slot->sliceAt( i ) ) );
            if ( d < sDist ) { sDist = d; sHit = i; }
        }
        if ( rightDownEdge ) {
            m_menuSlice = ( sHit >= 0 && sDist <= kGrab ) ? sHit : -1;
            open_menu( e.x, e.y, m_menuSlice >= 0 );
            app.request_redraw();
            return true;
        }
        if ( leftDownEdge && marker_at( e.x, e.y ) < 0 ) {
            if ( sHit >= 0 && sDist <= kGrab ) {
                m_dragSlice = sHit;
                m_dragOriginX = e.x; m_dragOriginV = m_slot->sliceAt( sHit );
                app.request_redraw();
                return true;
            }
            const Uint32 now = SDL_GetTicks();
            // A double-click has to be in the same PLACE, not merely within the
            // time window: two quick clicks at opposite ends of the waveform
            // used to drop a slice wherever the second one landed.
            const bool dbl = ( now - m_lastClickMs < 350 ) &&
                             std::abs( e.x - m_lastClickX ) <= 4 &&
                             std::abs( e.y - m_lastClickY ) <= 4;
            if ( dbl ) {
                const int idx = m_slot->sliceAdd( x_to_norm( e.x ) );
                if ( idx >= 0 ) {
                    m_dragSlice = idx;
                    m_dragOriginX = e.x; m_dragOriginV = m_slot->sliceAt( idx );
                } else set_status( "slice list is full" );
                m_lastClickMs = 0;
                app.request_redraw();
                return true;
            }
            m_lastClickMs = now; m_lastClickX = e.x; m_lastClickY = e.y;
        }
    }
    if ( m_dragSlice >= 0 ) {
        if ( !e.pressed ) { m_dragSlice = -1; app.request_redraw(); return true; }
        const float scale = mod_shift() ? 0.15f : 1.f;
        float v = m_dragOriginV + ( x_to_norm( e.x ) - x_to_norm( m_dragOriginX ) ) * scale;
        v = std::max( 0.f, std::min( 1.f, v ) );
        m_slot->sliceSet( m_dragSlice, v );
        app.request_redraw();
        return true;
    }

    // ---- markers ------------------------------------------------------------
    if ( leftDownEdge ) {
        const int m = marker_at( e.x, e.y );
        if ( m >= 0 ) {
            m_drag = m;
            // Remember where the grab started so the marker keeps its offset
            // under the pointer instead of snapping to it, and so Escape can
            // put it back exactly where it was.
            m_dragOriginX = e.x;
            m_dragOriginV = m_slot->sampleMarker( m );
            app.request_redraw();
            return true;
        }
    } else if ( !e.pressed ) {
        if ( m_drag >= 0 ) { m_drag = -1; app.request_redraw(); return true; }
    }
    if ( m_drag >= 0 ) {
        const float scale = mod_shift() ? 0.15f : 1.f;
        float v = m_dragOriginV + ( x_to_norm( e.x ) - x_to_norm( m_dragOriginX ) ) * scale;
        v = std::max( 0.f, std::min( 1.f, v ) );
        // With zero-snap lit, region and loop points land on a zero crossing --
        // which is the whole point of the toggle, and it only ever applied to
        // the destructive ops before, never to the markers you drag by hand.
        const int64_t n = m_slot->sampleFrames();
        if ( m_zeroSnap && n > 0 && !mod_shift() ) {
            const int64_t fr = m_slot->sampleZeroCross( (int64_t)( v * (double)n ), 0 );
            v = (float)( (double)fr / (double)n );
            v = std::max( 0.f, std::min( 1.f, v ) );
        }
        m_slot->sampleSetMarker( m_drag, v );
        clamp_markers();
        app.request_redraw();
        return true;
    }

    // ---- right-click menu ---------------------------------------------------
    if ( rightDownEdge ) {
        open_menu( e.x, e.y, false );
        app.request_redraw();
        return true;
    }

    // ---- loop crossfade handle ----------------------------------------------
    if ( m_slot->sampleXfadeSupported() &&
         m_slot->sampleMarkerActive( SM_LOOP_START ) ) {
        const float le = m_slot->sampleMarker( SM_LOOP_END );
        const float ls = m_slot->sampleMarker( SM_LOOP_START );
        const float xf = m_slot->sampleXfade();
        const int hOut = norm_to_x( std::max( 0.f, le - xf ) );
        const int hIn  = norm_to_x( std::max( 0.f, ls - xf ) );
        if ( leftDownEdge && !m_dragXfade && inWave && xf > 0.f &&
             ( std::abs( e.x - hOut ) <= kGrab || std::abs( e.x - hIn ) <= kGrab ) ) {
            m_dragXfadeFromIn = std::abs( e.x - hIn ) < std::abs( e.x - hOut );
            m_dragXfade = true;
            m_dragOriginX = e.x; m_dragOriginV = xf;
            app.request_redraw();
            return true;
        }
    }
    if ( m_dragXfade ) {
        if ( !e.pressed ) m_dragXfade = false;
        else {
            const float le = m_slot->sampleMarker( SM_LOOP_END );
            const float ls = m_slot->sampleMarker( SM_LOOP_START );
            const float scale = mod_shift() ? 0.15f : 1.f;
            const float d = ( x_to_norm( m_dragOriginX ) - x_to_norm( e.x ) ) * scale;
            // Either handle sets the SAME length: they are two ends of one
            // crossfade, so dragging one moves both.  The fade can be at most
            // the loop length, and needs run-in before the loop start to have
            // anything to fade from.
            float want = m_dragOriginV + d;
            want = std::min( want, le - ls );
            want = std::min( want, ls );
            m_slot->sampleSetXfade( std::max( 0.f, want ) );
        }
        app.request_redraw();
        return true;
    }

    // ---- selection ----------------------------------------------------------
    if ( leftDownEdge && inWave && !m_selDrag ) {
        const Uint32 now = SDL_GetTicks();
        const bool dbl = ( now - m_lastClickMs < 350 ) &&
                         std::abs( e.x - m_lastClickX ) <= 4 &&
                         std::abs( e.y - m_lastClickY ) <= 4;
        m_lastClickMs = now; m_lastClickX = e.x; m_lastClickY = e.y;
        if ( dbl && !m_slot->slicesSupported() ) {
            // Double-click selects everything, the way every other audio editor
            // behaves.  (Slice-capable slots keep double-click for adding a
            // slice; that gesture is handled above.)
            m_selA = 0.f; m_selB = 1.f; m_hasSel = true; m_selDrag = false;
            app.request_redraw();
            return true;
        }
        m_undoSelA = m_selA; m_undoSelB = m_selB; m_undoHasSel = m_hasSel;
        m_selDrag = true; m_hasSel = true;
        // Shift extends the existing selection instead of throwing it away --
        // trimming one edge used to mean re-dragging the whole span.
        if ( !( mod_shift() && m_undoHasSel ) ) m_selA = x_to_norm( e.x );
        else m_selA = m_undoSelA;
        m_selB = x_to_norm( e.x );
        app.request_redraw();
        return true;
    }
    if ( m_selDrag ) {
        if ( !e.pressed ) {
            m_selDrag = false;
            if ( sel_hi() - sel_lo() < 1e-5f ) m_hasSel = false;   // a click clears
        } else {
            m_selB = x_to_norm( e.x );
            // Pull past the edge and the view follows, so a selection can be
            // longer than the screen without letting go of the button.
            const float span = m_viewEnd - m_viewStart;
            if ( e.x < w.x && m_viewStart > 0.f ) {
                const float d = std::min( m_viewStart, span * 0.04f );
                m_viewStart -= d; m_viewEnd -= d; m_selB = m_viewStart; m_peakFrames = -1;
            } else if ( e.x >= w.x + w.w && m_viewEnd < 1.f ) {
                const float d = std::min( 1.f - m_viewEnd, span * 0.04f );
                m_viewStart += d; m_viewEnd += d; m_selB = m_viewEnd; m_peakFrames = -1;
            }
        }
        app.request_redraw();
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------
//  wheel
//----------------------------------------------------------------------------
bool SampleSlotEditor::on_wheel( ui::App& app, int dx, int dy ) {
    layout( app.mono );
    if ( m_menu.open() ) return true;          // never scroll under an open menu
    if ( !m_slot || m_slot->sampleFrames() <= 0 ) return false;

    int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
    if ( mx < m_wave.x ) mx = m_wave.x;
    if ( mx > m_wave.x + m_wave.w - 1 ) mx = m_wave.x + m_wave.w - 1;

    if ( mod_ctrl() && dy != 0 ) {                 // amplitude zoom
        m_vZoom *= ( dy > 0 ) ? 1.25f : 0.8f;
        m_vZoom = std::max( 0.25f, std::min( 16.f, m_vZoom ) );
        app.request_redraw();
        return true;
    }
    // Hovering ANY toolbar button used to turn the wheel into a crossfade-length
    // control, so scrolling with the pointer near the top silently retuned an
    // unrelated parameter.  The length is a control of its own now (footer).
    const float span = m_viewEnd - m_viewStart;
    if ( dx != 0 || ( dy != 0 && mod_shift() ) ) {   // shift+wheel = scroll
        const int d = dx != 0 ? dx : -dy;
        const float step = span * 0.12f * (float)( d > 0 ? 1 : -1 );
        m_viewStart += step; m_viewEnd += step;
        clamp_view();
        m_peakFrames = -1;
    } else if ( dy != 0 ) {                          // wheel = zoom at pointer
        zoom_at( ( dy > 0 ) ? 0.8f : 1.25f, mx );
    } else return false;
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  keyboard
//----------------------------------------------------------------------------
bool SampleSlotEditor::on_key( ui::App& app, SDL_Keycode k ) {
    layout( app.mono );
    if ( m_menu.open() ) {
        int row = -1;
        if ( m_menu.key( k, row ) ) {
            if ( row >= 0 ) run_tool( m_menu.rows[(size_t)row].op );
            app.request_redraw();
            return true;
        }
        return true;                    // an open menu is modal over the editor
    }
    if ( !m_slot ) return false;
    using namespace PatchKnob::engine;
    const bool ctrl  = mod_ctrl();
    const bool shift = mod_shift();
    const float span = m_viewEnd - m_viewStart;
    int op = 0; bool handled = true;

    if ( k == SDLK_ESCAPE ) {
        // Escape unwinds ONE thing at a time, innermost first: an in-flight
        // drag, then the selection.  Before, a drag could not be cancelled at
        // all -- you had to release and undo.
        if ( m_drag >= 0 ) {
            m_slot->sampleSetMarker( m_drag, m_dragOriginV ); clamp_markers(); m_drag = -1;
        } else if ( m_dragSlice >= 0 ) {
            m_slot->sliceSet( m_dragSlice, m_dragOriginV ); m_dragSlice = -1;
        } else if ( m_dragXfade ) {
            m_slot->sampleSetXfade( m_dragOriginV ); m_dragXfade = false;
        } else if ( m_dragXfadeField ) {
            m_xfadeMs = m_dragOriginV; m_dragXfadeField = false;
        } else if ( m_selDrag ) {
            m_selA = m_undoSelA; m_selB = m_undoSelB; m_hasSel = m_undoHasSel;
            m_selDrag = false;
        } else if ( m_hasSel ) {
            m_hasSel = false;
        } else handled = false;
    }
    else if ( k == SDLK_HOME )  { m_viewStart = 0.f; m_viewEnd = 1.f; m_vZoom = 1.f;
                                  m_peakFrames = -1; }
    else if ( k == SDLK_END )   { m_viewStart = 1.f - span; m_viewEnd = 1.f;
                                  clamp_view(); m_peakFrames = -1; }
    else if ( k == SDLK_LEFT || k == SDLK_RIGHT ) {
        const float step = span * ( shift ? 0.02f : 0.15f ) * ( k == SDLK_LEFT ? -1.f : 1.f );
        m_viewStart += step; m_viewEnd += step; clamp_view(); m_peakFrames = -1;
    }
    else if ( k == SDLK_PAGEUP || k == SDLK_PAGEDOWN ) {
        const float step = span * ( k == SDLK_PAGEUP ? -0.9f : 0.9f );
        m_viewStart += step; m_viewEnd += step; clamp_view(); m_peakFrames = -1;
    }
    else if ( k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS )
        zoom_at( 0.7f, m_curX >= 0 ? m_curX : m_wave.x + m_wave.w / 2 );
    else if ( k == SDLK_MINUS || k == SDLK_KP_MINUS )
        zoom_at( 1.4f, m_curX >= 0 ? m_curX : m_wave.x + m_wave.w / 2 );
    // Ctrl+Z/Y never reach on_key -- the toolkit routes them to on_undo() --
    // but keep both paths on one implementation so they cannot drift.
    else if ( ctrl && ( k == SDLK_z || k == SDLK_y ) )
        return on_undo( app, k == SDLK_y || shift );
    else if ( ctrl && k == SDLK_a ) op = -4;
    else if ( ctrl && k == SDLK_x ) op = SOP_CUT;
    else if ( ctrl && k == SDLK_c ) op = SOP_COPY;
    else if ( ctrl && k == SDLK_v ) op = SOP_PASTE_INSERT;
    else if ( ctrl && k == SDLK_s ) op = -18;
    else if ( k == SDLK_DELETE || k == SDLK_BACKSPACE ) op = SOP_DELETE;
    else if ( k == SDLK_e )     op = -3;              // zoom to selection
    else if ( k == SDLK_t )     op = SOP_TRIM;
    else if ( k == SDLK_n )     op = SOP_NORMALIZE;
    else if ( k == SDLK_r )     op = SOP_REVERSE;
    else if ( k == SDLK_i )     op = SOP_FADE_IN;
    else if ( k == SDLK_o )     op = SOP_FADE_OUT;
    else if ( k == SDLK_m )     op = SOP_SILENCE;
    else if ( k == SDLK_d )     op = SOP_DC_REMOVE;
    else if ( k == SDLK_l )     op = -19;             // loop on/off
    else if ( k == SDLK_z )     op = -7;              // toggle zero snap
    else if ( k == SDLK_f )     op = -12;             // fit
    else if ( k == SDLK_LEFTBRACKET )  op = -5;       // region := selection
    else if ( k == SDLK_RIGHTBRACKET ) op = -6;       // loop := selection
    else handled = false;

    if ( !handled ) return false;
    if ( op != 0 ) {
        // Route every shortcut through the same predicate the menu uses, so a
        // key can never perform an edit the menu greys out (Delete with nothing
        // selected used to erase the entire sample).
        if ( op_enabled( op ) ) run_tool( op );
        else set_status( "nothing to do -- make a selection first" );
    }
    app.request_redraw();
    return true;
}

// Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y from the toolkit's undo route.  The slot owns
// the history (ISampleSlot::sampleUndo/sampleRedo, snapshotted per destructive
// op); the toolbar advertises Ctrl+Z for it.  Before this hook the shell's
// project undo fired instead and rolled the whole project back.  Returning
// false when the slot has nothing left keeps Ctrl+Z meaning project undo.
bool SampleSlotEditor::on_undo( ui::App& app, bool redo ) {
    if ( !m_slot ) return false;
    if ( redo ? !m_slot->sampleCanRedo() : !m_slot->sampleCanUndo() ) return false;
    const bool ok = redo ? m_slot->sampleRedo() : m_slot->sampleUndo();
    if ( !ok ) return false;
    // The restored PCM invalidates the peak mip AND any selection/marker drag
    // that was measured against the old length.
    mark_audio_changed();
    cancel_interaction( app );
    clamp_view();
    set_status( redo ? "redo" : "undo" );
    app.request_redraw();
    return true;
}

void SampleSlotEditor::cancel_interaction( ui::App& app ) {
    const bool had = m_drag >= 0 || m_dragSlice >= 0 || m_dragXfade ||
                     m_dragXfadeField || m_selDrag || m_menu.open();
    m_drag = -1; m_dragSlice = -1; m_dragXfade = false; m_dragXfadeField = false;
    m_selDrag = false;
    m_leftDown = false; m_rightDown = false;
    m_hotTool = -1; m_hotMarker = -1;
    m_menu.close(); m_menuSlice = -1; m_confirmClearSlices = false;
    m_dragOver = false;
    if ( had ) app.request_redraw();
}

//----------------------------------------------------------------------------
//  SampleSlotPanel
//----------------------------------------------------------------------------
SampleSlotPanel::SampleSlotPanel() {
    // Start the browser somewhere real.  It used to sit on an empty path, which
    // never scans -- so the list came up permanently empty and looked broken.
    // The shell can still point it elsewhere; this only guarantees it is alive.
    m_browser.set_dir( std::string() );        // "" -> current working directory
    // Drag a row out of the browser and drop it on the waveform to load it.
    // A preset is not a file -- this editor holds one ISampleSlot, not a
    // multi-zone sampler, so an Sf2Preset payload has nowhere to land HERE
    // and is handed straight up via on_preset_drop for the shell to route.
    m_browser.on_drag_drop = [this]( const DragPayload& payload, int x, int y ) {
        m_editor.set_drag_hover( false );
        if ( payload.kind == DragPayload::File ) {
            if ( m_editor.wants_drop( x, y ) ) m_editor.load_path( payload.path );
        } else if ( on_preset_drop ) {
            on_preset_drop( payload, x, y );
        }
    };
    // Double-click in the list loads straight into the slot.
    m_browser.on_activate = [this]( const std::string& path ) {
        m_editor.load_path( path );
    };
    // Single click / arrow-key move auditions the file WITHOUT loading it, so a
    // folder can be hunted through by ear.
    m_browser.on_select = [this]( const std::string& path ) {
        if ( on_preview ) on_preview( path );
    };
    m_browser.on_status = [this]( const std::string& msg ) {
        if ( on_status ) on_status( msg );
    };
    m_editor.on_status = [this]( const std::string& msg ) {
        if ( on_status ) on_status( msg );
    };
    m_editor.on_loaded = [this]( const std::string& path ) {
        if ( on_loaded ) on_loaded( path );
    };
}

void SampleSlotPanel::set_slot( PatchKnob::engine::ISampleSlot* slot,
                                const std::string& title ) {
    m_editor.set_slot( slot, title );
}

void SampleSlotPanel::layout() {
    const int bar = 6;                                   // splitter thickness
    int split = m_split;
    if ( split > rect.w - 120 ) split = rect.w - 120;    // never crowd out the wave
    if ( split > rect.w * 3 / 4 ) split = rect.w * 3 / 4;
    if ( split < 90 ) split = std::min( 90, std::max( 0, rect.w ) );
    m_browser.rect = SDL_Rect{ rect.x, rect.y, split, rect.h };
    m_splitRect    = SDL_Rect{ rect.x + split, rect.y, bar, rect.h };
    m_editor.rect  = SDL_Rect{ rect.x + split + bar, rect.y,
                               std::max( 8, rect.w - split - bar ), rect.h };
    m_browser.set_focused( m_browserFocused );
}

void SampleSlotPanel::draw( ui::App& app ) {
    layout();
    const ui::Theme& t = ui::theme();
    // While a browser drag is in flight, highlight the editor as a drop target.
    if ( m_browser.dragging() ) {
        int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
        m_editor.set_drag_hover( m_editor.wants_drop( mx, my ) );
    }
    m_browser.draw( app );
    m_editor.draw( app );

    // ---- splitter ----------------------------------------------------------
    // A fixed 240 px browser was either wasting half the window or cutting
    // every filename in half.  The divider is a grip you can drag, and it says
    // so by lighting when the pointer is on it.
    {
        int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
        m_splitHot = m_splitDrag ||
                     ( mx >= m_splitRect.x - 2 && mx < m_splitRect.x + m_splitRect.w + 2 &&
                       my >= m_splitRect.y && my < m_splitRect.y + m_splitRect.h );
        ui::fill_rect( app.ren, m_splitRect, t.panel );
        const ui::Color grip = m_splitHot ? t.accent
                                          : ui::Color{ t.dim.r, t.dim.g, t.dim.b, 160 };
        const int cy = m_splitRect.y + m_splitRect.h / 2;
        for ( int i = -2; i <= 2; ++i )
            ui::fill_rect( app.ren,
                SDL_Rect{ m_splitRect.x + 2, cy + i * 4, m_splitRect.w - 4, 2 }, grip );
    }

    // Painted last so the drag ghost and the browser's context menu land ON TOP
    // of the editor.  They used to be drawn inside the browser, which paints
    // first, so the ghost vanished exactly when it crossed the drop target.
    m_browser.draw_overlay( app );
}

bool SampleSlotPanel::on_mouse( ui::App& app, const ui::MouseEv& e ) {
    layout();
    // This panel receives every mouse event for the window, so it is the only
    // place that can compute a reliable press EDGE for its children.
    const bool downEdge = e.pressed && !m_ldown;
    m_ldown = e.pressed;
    m_browser.set_down_edge( downEdge );

    // ---- splitter drag ------------------------------------------------------
    if ( m_splitDrag ) {
        if ( !e.pressed ) { m_splitDrag = false; }
        else {
            m_split = std::max( 90, std::min( rect.w - 120, e.x - rect.x - m_splitGrabDx ) );
            layout();
        }
        app.request_redraw();
        return true;
    }
    if ( downEdge && e.button == SDL_BUTTON_LEFT &&
         e.x >= m_splitRect.x - 2 && e.x < m_splitRect.x + m_splitRect.w + 2 &&
         e.y >= m_splitRect.y && e.y < m_splitRect.y + m_splitRect.h ) {
        m_splitDrag = true;
        m_splitGrabDx = e.x - m_splitRect.x;
        app.request_redraw();
        return true;
    }

    // A press that started in the browser keeps the mouse until release --
    // including BEFORE the drag threshold trips.  Routing by pointer position
    // instead handed the motion to the editor the moment the pointer crossed
    // out of the list, so the press never became a drag and the drop never
    // happened (which is why a second sample could not replace the first).
    if ( m_browser.dragging() || m_browser.pressing() || m_browser.menu_open() )
        return m_browser.on_mouse( app, e );
    // The editor's own menu and drags capture the pointer for the same reason.
    if ( m_editor.mouse_capture_active() ) return m_editor.on_mouse( app, e );
    const bool inBrowser = e.x < m_browser.rect.x + m_browser.rect.w;
    if ( downEdge ) m_browserFocused = inBrowser;   // click decides who gets keys
    if ( inBrowser ) return m_browser.on_mouse( app, e );
    return m_editor.on_mouse( app, e );
}

bool SampleSlotPanel::on_wheel( ui::App& app, int dx, int dy ) {
    layout();
    int mx = 0, my = 0; ui::mouse_logical( app, mx, my );
    if ( mx < m_browser.rect.x + m_browser.rect.w )
        return m_browser.on_wheel( app, dx, dy );
    return m_editor.on_wheel( app, dx, dy );
}

// The composite hosts the editor, so the toolkit's undo route reaches the
// editor's local history only if it is forwarded here.  The browser has no edit
// history of its own, so this is unconditional.
bool SampleSlotPanel::on_undo( ui::App& app, bool redo ) {
    return m_editor.on_undo( app, redo );
}

bool SampleSlotPanel::on_key( ui::App& app, SDL_Keycode k ) {
    // An open menu is modal: whichever pane owns it gets the key, wherever the
    // focus nominally is.
    if ( m_browser.menu_open() ) return m_browser.on_key( app, k );

    // Enter always means "use the highlighted file", the only thing it can mean
    // in this window.  Arrows and paging, though, are routed by FOCUS: the
    // editor scrolls and zooms with them now, and sending them to the list
    // unconditionally meant the waveform could not be navigated from the
    // keyboard at all.
    if ( k == SDLK_RETURN || k == SDLK_KP_ENTER )
        return m_browser.on_key( app, k );
    // The browser treats every printable key as type-ahead input, so asking it
    // first would eat all of the editor's single-key shortcuts (T/N/R...).
    if ( m_browserFocused ) {
        if ( m_browser.on_key( app, k ) ) return true;
        return m_editor.on_key( app, k );
    }
    if ( m_editor.on_key( app, k ) ) return true;
    return m_browser.on_key( app, k );
}

void SampleSlotPanel::cancel_interaction( ui::App& app ) {
    // The host takes the gesture away (window move, focus loss).  Both children
    // have to hear about it or the next click continues a drag nobody started.
    m_splitDrag = false;
    m_ldown = false;
    m_browser.cancel_interaction( app );
    m_editor.cancel_interaction( app );
}

//----------------------------------------------------------------------------
//  Selection + destructive tools
//----------------------------------------------------------------------------
int64_t SampleSlotEditor::sel_from() const {
    if ( !m_slot ) return 0;
    return (int64_t)( (double)m_slot->sampleFrames() * (double)sel_lo() );
}
int64_t SampleSlotEditor::sel_to() const {
    if ( !m_slot ) return 0;
    return (int64_t)( (double)m_slot->sampleFrames() * (double)sel_hi() );
}

void SampleSlotEditor::op_range( int64_t& from, int64_t& to ) const {
    if ( !m_slot ) { from = to = 0; return; }
    const int64_t n = m_slot->sampleFrames();
    if ( has_sel() ) { from = sel_from(); to = sel_to(); }
    else             { from = 0; to = n; }
    // A trim or fade that starts mid-cycle clicks; snapping the edges to zero
    // crossings is what makes these edits sound clean.
    if ( m_zeroSnap ) {
        from = m_slot->sampleZeroCross( from, 0 );
        to   = m_slot->sampleZeroCross( to, 0 );
        if ( to < from ) std::swap( from, to );
    }
    from = std::max<int64_t>( 0, std::min( from, n ) );
    to   = std::max<int64_t>( 0, std::min( to, n ) );
}

void SampleSlotEditor::run_tool( int op ) {
    if ( !m_slot ) return;
    const int64_t n = m_slot->sampleFrames();
    if ( op <= -1000 ) {                          // host-supplied zone actions
        const int i = -1000 - op;
        if ( i >= 0 && i < (int)m_contextActions.size() && m_contextActions[(size_t)i].run )
            m_contextActions[(size_t)i].run();
        return;
    }
    if ( op != -20 ) m_confirmClearSlices = false;   // any other pick disarms it
    switch ( op ) {
        case -1: if ( m_slot->sampleUndo() ) mark_audio_changed(); return;
        case -2: if ( m_slot->sampleRedo() ) mark_audio_changed(); return;
        case -3:                                   // zoom to selection
            if ( has_sel() ) { m_viewStart = sel_lo(); m_viewEnd = sel_hi();
                               clamp_view(); m_peakFrames = -1; }
            return;
        case -4:                                   // select all / none
            if ( has_sel() ) { m_hasSel = false; }
            else { m_selA = 0.f; m_selB = 1.f; m_hasSel = true; }
            return;
        case -5:                                   // region := selection
            if ( has_sel() ) {
                m_slot->sampleSetMarker( SM_START, sel_lo() );
                m_slot->sampleSetMarker( SM_END,   sel_hi() );
                clamp_markers();
            }
            return;
        case -6:                                   // loop := selection
            if ( has_sel() ) {
                m_slot->sampleSetMarker( SM_LOOP_START, sel_lo() );
                m_slot->sampleSetMarker( SM_LOOP_END,   sel_hi() );
                clamp_markers();
            }
            return;
        case -7: m_zeroSnap = !m_zeroSnap; return;  // zero-crossing snap
        case -19: m_slot->sampleSetLoopEnabled( !m_slot->sampleLoopEnabled() );
                  return;                           // loop on/off
        case -12: m_viewStart = 0.f; m_viewEnd = 1.f; m_vZoom = 1.f;
                  m_peakFrames = -1; return;        // fit
        case -13: zoom_at( 0.6f, m_wave.x + m_wave.w / 2 ); return;
        case -14: zoom_at( 1.6f, m_wave.x + m_wave.w / 2 ); return;
        case -15: m_vZoom = std::min( 16.f, m_vZoom * 1.25f ); return;
        case -16: m_vZoom = std::max( 0.25f, m_vZoom * 0.8f ); return;
        case -17: m_stereo = !m_stereo; return;     // dual-lane display
        case -18: {                                 // save as WAV
            if ( !on_pick_save_path ) { set_status( "no save dialog wired" ); return; }
            std::string path;
            if ( !on_pick_save_path( path ) ) return;
            std::string err;
            set_status( m_slot->sampleSave( path, &err ) ? "saved" : ( "save failed: " + err ) );
            return;
        }
        case -20:                                   // clear ALL slices
            // Two-step, because there is no undo for the slice list: the first
            // pick arms it and relabels the row, the second one does it.
            if ( !m_confirmClearSlices ) {
                m_confirmClearSlices = true;
                set_status( "pick 'Clear ALL slices' again to confirm" );
                return;
            }
            m_confirmClearSlices = false;
            for ( int i = m_slot->sliceCount() - 1; i >= 0; --i ) m_slot->sliceRemove( i );
            set_status( "slices cleared" );
            return;
        case -21:                                   // remove the picked slice
            if ( m_menuSlice >= 0 ) {
                m_slot->sliceRemove( m_menuSlice );
                m_menuSlice = -1;
                set_status( "slice removed" );
            }
            return;
        case -22:                                   // snap the picked slice
            if ( m_menuSlice >= 0 && n > 0 ) {
                const double v = (double)m_slot->sliceAt( m_menuSlice );
                const int64_t fr = m_slot->sampleZeroCross( (int64_t)( v * (double)n ), 0 );
                m_slot->sliceSet( m_menuSlice, (float)( (double)fr / (double)n ) );
                m_menuSlice = -1;
            }
            return;
        default: break;
    }
    if ( !m_slot->sampleEditable() || n <= 0 ) {
        set_status( "sample is not editable" );
        return;
    }

    int64_t from = 0, to = 0;
    float arg = 1.f;
    using namespace PatchKnob::engine;
    int realOp = op;
    if ( op == -8 || op == -9 ) {            // +/- 1.5 dB per press
        realOp = SOP_GAIN; arg = ( op == -8 ) ? 1.1885f : 0.8414f;
        op_range( from, to );
        if ( m_slot->sampleOp( realOp, from, to, arg ) ) mark_audio_changed();
        return;
    }
    if ( op == -10 || op == -11 ) {          // half / double speed (resample)
        realOp = SOP_RESAMPLE; arg = ( op == -10 ) ? 0.5f : 2.f;
        if ( m_slot->sampleOp( realOp, 0, m_slot->sampleFrames(), arg ) ) {
            mark_audio_changed(); m_hasSel = false;
        }
        return;
    }
    if ( op == SOP_CROSSFADE_LOOP ) {
        // The crossfade acts on the LOOP, not the selection: it is the loop
        // splice we are trying to make inaudible.
        const double sr = m_slot->sampleRate() > 0.0 ? m_slot->sampleRate() : 48000.0;
        from = (int64_t)( (double)n * (double)m_slot->sampleMarker( SM_LOOP_START ) );
        to   = (int64_t)( (double)n * (double)m_slot->sampleMarker( SM_LOOP_END ) );
        arg  = (float)( m_xfadeMs * 0.001 * sr );
    } else {
        op_range( from, to );
        if ( op == SOP_NORMALIZE ) arg = 1.f;
        else if ( op == SOP_FADE_IN || op == SOP_FADE_OUT ) arg = 0.f;   // linear
    }
    if ( m_slot->sampleOp( op, from, to, arg ) ) {
        mark_audio_changed();                     // waveform changed
        if ( op == SOP_TRIM ) { m_hasSel = false; m_viewStart = 0.f; m_viewEnd = 1.f; }
        m_status.clear(); m_statusMs = 0;
    }
}

} // namespace sampleslot
