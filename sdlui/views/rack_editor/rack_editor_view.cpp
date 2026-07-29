//----------------------------------------------------------------------------
//  sdlui/views/rack_editor/rack_editor_view.cpp -- generic, data-driven SDL2
//  editor for a rackx::RackEngine modular patch.  See the header for the full
//  layout / interaction contract.  Everything two-tone via ui::theme().
//----------------------------------------------------------------------------
#include "rack_editor_view.h"

#include "engine/rack/rack_factory.h"    // rackx::registry / ModuleType / registerBuiltins

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using ui::Color;
using ui::theme;

namespace {

const double PI = 3.14159265358979323846;

inline float clamp01( float x ) { return x < 0.f ? 0.f : ( x > 1.f ? 1.f : x ); }
inline int   imax( int a, int b ) { return a > b ? a : b; }
inline int   imin( int a, int b ) { return a < b ? a : b; }

inline bool in_rect( const SDL_Rect& q, int x, int y )
{
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

// squared-distance point test
inline bool near_pt( int x, int y, int cx, int cy, int rad )
{
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

// ---- filled disc -----------------------------------------------------------
void fill_disc( SDL_Renderer* r, int cx, int cy, int rad, Color c )
{
    if ( rad < 1 ) rad = 1;
    ui::set_color( r, c );
    for ( int dy = -rad; dy <= rad; ++dy )
    {
        int dx = int( std::floor( std::sqrt( double( rad * rad - dy * dy ) ) ) );
        SDL_RenderDrawLine( r, cx - dx, cy + dy, cx + dx, cy + dy );
    }
}

// ---- circle outline (midpoint), drawn at rad and rad-1 for ~2px ------------
void circle_ring( SDL_Renderer* r, int cx, int cy, int rad, Color c )
{
    ui::set_color( r, c );
    for ( int rr = rad; rr >= rad - 1 && rr > 0; --rr )
    {
        int x = rr, y = 0, err = 1 - rr;
        while ( x >= y )
        {
            SDL_RenderDrawPoint( r, cx + x, cy + y );
            SDL_RenderDrawPoint( r, cx + y, cy + x );
            SDL_RenderDrawPoint( r, cx - y, cy + x );
            SDL_RenderDrawPoint( r, cx - x, cy + y );
            SDL_RenderDrawPoint( r, cx - x, cy - y );
            SDL_RenderDrawPoint( r, cx - y, cy - x );
            SDL_RenderDrawPoint( r, cx + y, cy - x );
            SDL_RenderDrawPoint( r, cx + x, cy - y );
            ++y;
            if ( err < 0 ) err += 2 * y + 1;
            else { --x; err += 2 * ( y - x ) + 1; }
        }
    }
}

// ---- bezier (sampled to line segments; SDL has no native curves) -----------
void control_points( int x1, int y1, int x2, int y2,
                     double& c1x, double& c1y, double& c2x, double& c2y )
{
    double dx = std::fabs( double( x2 - x1 ) ) * 0.5;
    if ( dx < 40.0 ) dx = 40.0;
    c1x = x1 + dx; c1y = y1;
    c2x = x2 - dx; c2y = y2;
}

void bezier_point( double t, double x1, double y1, double c1x, double c1y,
                   double c2x, double c2y, double x2, double y2,
                   double& bx, double& by )
{
    double u = 1.0 - t;
    double a = u * u * u, b = 3.0 * u * u * t, c = 3.0 * u * t * t, d = t * t * t;
    bx = a * x1 + b * c1x + c * c2x + d * x2;
    by = a * y1 + b * c1y + c * c2y + d * y2;
}

void stroke_bezier( SDL_Renderer* r, int x1, int y1, int x2, int y2,
                    Color col, int thickness, bool dashed )
{
    double c1x, c1y, c2x, c2y;
    control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );

    const int STEPS = 40;
    SDL_Point pts[STEPS + 1];
    for ( int s = 0; s <= STEPS; ++s )
    {
        double t = double( s ) / STEPS, bx, by;
        bezier_point( t, x1, y1, c1x, c1y, c2x, c2y, x2, y2, bx, by );
        pts[s].x = int( std::round( bx ) );
        pts[s].y = int( std::round( by ) );
    }

    ui::set_color( r, col );
    int half = thickness / 2;
    if ( !dashed )
    {
        for ( int off = -half; off <= thickness - 1 - half; ++off )
        {
            if ( off != 0 )
                for ( SDL_Point& point : pts ) point.y += off;
            SDL_RenderDrawLines( r, pts, STEPS + 1 );
            if ( off != 0 )
                for ( SDL_Point& point : pts ) point.y -= off;
        }
        return;
    }
    for ( int off = -half; off <= thickness - 1 - half; ++off )
    {
        for ( int s = 0; s < STEPS; ++s )
        {
            if ( ( s / 3 ) % 2 ) continue;
            SDL_RenderDrawLine( r, pts[s].x,   pts[s].y   + off,
                                   pts[s+1].x, pts[s+1].y + off );
        }
    }
}

void stroke_wiggled_bezier( SDL_Renderer* r, int x1, int y1, int x2, int y2,
                            Color col, int thickness, float laneOffset, float phase )
{
    double c1x, c1y, c2x, c2y;
    control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );
    const double length = std::hypot( double( x2 - x1 ), double( y2 - y1 ) );
    if ( length < 1.0 ) return;
    const double nx = -double( y2 - y1 ) / length;
    const double ny = double( x2 - x1 ) / length;
    const int steps = 40;
    SDL_Point points[steps + 1];
    for ( int step = 0; step <= steps; ++step )
    {
        const double t = double( step ) / steps;
        double bx, by;
        bezier_point( t, x1, y1, c1x, c1y, c2x, c2y, x2, y2, bx, by );
        const double envelope = std::sin( PI * t );
        const double ripple = std::sin( phase + t * PI * 5.0 ) * 1.5;
        const double offset = envelope * ( laneOffset + ripple );
        points[step].x = (int) std::lround( bx + nx * offset );
        points[step].y = (int) std::lround( by + ny * offset );
    }

    ui::set_color( r, col );
    const int half = thickness / 2;
    for ( int offset = -half; offset <= thickness - 1 - half; ++offset )
    {
        if ( offset != 0 )
            for ( SDL_Point& point : points ) point.y += offset;
        SDL_RenderDrawLines( r, points, steps + 1 );
        if ( offset != 0 )
            for ( SDL_Point& point : points ) point.y -= offset;
    }
}

// nearest distance from (x,y) to the cable's sampled bezier
double bezier_dist( int x1, int y1, int x2, int y2, int x, int y )
{
    double c1x, c1y, c2x, c2y;
    control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );
    const int STEPS = 24;
    double best = 1e18;
    for ( int s = 0; s <= STEPS; ++s )
    {
        double t = double( s ) / STEPS, bx, by;
        bezier_point( t, x1, y1, c1x, c1y, c2x, c2y, x2, y2, bx, by );
        double dd = std::hypot( bx - x, by - y );
        if ( dd < best ) best = dd;
    }
    return best;
}

// ellipsis-truncate `s` so its monospace pixel width fits `maxw`.
std::string clip_text( const ui::Font& f, const std::string& s, int maxw )
{
    if ( maxw <= 0 ) return std::string();
    if ( f.text_w( s ) <= maxw ) return s;
    std::string t = s;
    while ( t.size() > 1 )
    {
        t.erase( t.size() - 1 );
        if ( f.text_w( t + "..." ) <= maxw ) return t + "...";
    }
    return std::string();
}

// case-insensitive substring test (empty needle matches everything)
bool ci_contains( const std::string& hay, const std::string& needle )
{
    if ( needle.empty() ) return true;
    if ( needle.size() > hay.size() ) return false;
    for ( std::size_t i = 0; i + needle.size() <= hay.size(); ++i )
    {
        std::size_t k = 0;
        for ( ; k < needle.size(); ++k )
        {
            char a = (char) std::tolower( (unsigned char) hay[i + k] );
            char b = (char) std::tolower( (unsigned char) needle[k] );
            if ( a != b ) break;
        }
        if ( k == needle.size() ) return true;
    }
    return false;
}

// scale a colour by a 0..1 brightness (LED tint)
Color scale_color( Color c, float b )
{
    b = clamp01( b );
    return Color{ (Uint8)( c.r * b ), (Uint8)( c.g * b ), (Uint8)( c.b * b ), 255 };
}

struct RackColors {
    Color canvas, grid, panel, inset, header, edge, edgeStrong;
    Color text, muted, well, knob, marker, cableOuter, cableInner, cableHighlight;
    Color selection;
};

RackColors rack_colors()
{
    const bool dark = theme().bg.r < 128;
    if ( dark )
    {
        return RackColors{
            Color{8, 8, 8, 255}, Color{18, 18, 18, 255},
            Color{31, 31, 31, 255}, Color{19, 19, 19, 255},
            Color{48, 48, 48, 255}, Color{90, 90, 90, 255}, Color{176, 176, 176, 255},
            Color{238, 238, 238, 255}, Color{150, 150, 150, 255},
            Color{7, 7, 7, 255}, Color{68, 68, 68, 255}, Color{244, 244, 244, 255},
            Color{3, 3, 3, 255}, Color{102, 102, 102, 255}, Color{210, 210, 210, 255},
            Color{255, 255, 255, 255}
        };
    }
    return RackColors{
        Color{230, 230, 230, 255}, Color{210, 210, 210, 255},
        Color{248, 248, 248, 255}, Color{235, 235, 235, 255},
        Color{218, 218, 218, 255}, Color{156, 156, 156, 255}, Color{52, 52, 52, 255},
        Color{22, 22, 22, 255}, Color{92, 92, 92, 255},
        Color{222, 222, 222, 255}, Color{246, 246, 246, 255}, Color{25, 25, 25, 255},
        Color{74, 74, 74, 255}, Color{182, 182, 182, 255}, Color{244, 244, 244, 255},
        Color{0, 0, 0, 255}
    };
}

Color rgb_led_color( const rack::engine::Light& light, const RackColors& colors )
{
    if ( !light.isColor() ) return scale_color( Color{ 58, 255, 126, 255 }, light.getBrightness() );
    auto byte = []( float value ) { return (Uint8) std::lround( 255.f * clamp01( value ) ); };
    return Color{ byte( light.getRed() ), byte( light.getGreen() ), byte( light.getBlue() ), 255 };
}

std::string clip_text_scaled( const ui::Font& font, const std::string& text,
                              int maxWidth, float scale )
{
    if ( maxWidth <= 0 ) return std::string();
    if ( font.text_w_scaled( text, scale ) <= maxWidth ) return text;
    std::string clipped = text;
    while ( clipped.size() > 1 )
    {
        clipped.erase( clipped.size() - 1 );
        if ( font.text_w_scaled( clipped + "...", scale ) <= maxWidth ) return clipped + "...";
    }
    return std::string();
}

const rackx::PanelElement* find_element( const std::vector<rackx::PanelElement>& elements,
                                         int id )
{
    for ( const rackx::PanelElement& element : elements )
        if ( element.id == id ) return &element;
    return nullptr;
}

} // anonymous namespace

namespace rackui {

// ============================================================================
//  Model access
// ============================================================================
void RackEditorView::set_engine( rackx::RackEngine* eng )
{
    m_engine = eng;
    rackx::registerBuiltins();     // populate the palette lazily (idempotent)
    m_sel = -1;
    m_move = m_knob = m_button = m_wire = false;
}

// ============================================================================
//  Layout -- one place computes every module's metrics, so drawing and
//  hit-testing agree to the pixel.
// ============================================================================
const rackx::PanelSpec* RackEditorView::panel_of( rackx::RackModule* m ) const
{
    const rackx::ModuleType* type = m ? rackx::findType( m->slug ) : nullptr;
    return type && type->panel.valid() ? &type->panel : nullptr;
}

RackEditorView::Layout RackEditorView::layout_of( rackx::RackModule* m ) const
{
    Layout L;
    rack::engine::Module* d = ( m && m->mod ) ? m->mod.get() : nullptr;
    L.nP = d ? (int) d->params.size()  : 0;
    L.nI = d ? (int) d->inputs.size()  : 0;
    L.nO = d ? (int) d->outputs.size() : 0;
    L.nL = d ? (int) d->lights.size()  : 0;

    L.panel = panel_of( m );
    if ( L.panel )
    {
        L.w = zpx( L.panel->width );
        L.h = zpx( L.panel->height );
        if ( !L.panel->textureAsset.empty() )
        {
            const float aspect = m_cardinalTextures.aspect( L.panel->textureAsset,
                                                              L.panel->texturePack );
            if ( aspect > 0.f ) L.w = imax( 1, (int) std::lround( L.h * aspect ) );
        }
        return L;
    }

    // knobs (wrap into rows) -- knob geometry scales with zoom; the label line
    // (m_ch, drawn at the fixed font size) is reserved inside the scaled cell.
    L.knobCellH = zpx( 2 * KNOB_R + 10 + m_ch );
    if ( L.nP > 0 )
    {
        L.knobsPerRow = imin( L.nP, KNOBS_PER_ROW );
        L.knobRows    = ( L.nP + L.knobsPerRow - 1 ) / L.knobsPerRow;
    }
    L.knobAreaH = L.knobRows * L.knobCellH;

    // panel inner width = widest of {knob grid, two jack columns, title, min}
    int knobsW = L.knobsPerRow * zpx( KNOB_CELL_W + 8 );
    int titleW = zpx( ( m ? (int) m->name.size() : 0 ) * m_cw + 12 );
    int jacksW = zpx( JACK_COL_MIN ) * 2;
    L.innerW = knobsW;
    L.innerW = imax( L.innerW, jacksW );
    L.innerW = imax( L.innerW, titleW );
    L.innerW = imax( L.innerW, zpx( PANEL_MIN_W - 2 * PAD ) );

    // lights (wrap into rows across the inner width)
    if ( L.nL > 0 )
    {
        L.lightsPerRow = imax( 1, L.innerW / zpx( LIGHT_CELL ) );
        L.lightsPerRow = imin( L.lightsPerRow, L.nL );
        L.lightRows    = ( L.nL + L.lightsPerRow - 1 ) / L.lightsPerRow;
        L.lightAreaH   = L.lightRows * zpx( LIGHT_ROW_H ) + 2;
    }

    // jacks: two columns (inputs left, outputs right), stacked to max(nI,nO)
    L.jackRowH = zpx( imax( m_ch + 6, 2 * JACK_R + 4 ) );
    L.jackRows = imax( L.nI, L.nO );
    L.jackAreaH = L.jackRows > 0 ? ( 2 + L.jackRows * L.jackRowH ) : 0;

    L.w = L.innerW + 2 * zpx( PAD );
    const int requiredH = zpx( TITLE_H ) + L.knobAreaH + L.lightAreaH + L.jackAreaH + zpx( BOTTOM_PAD );
    L.h = imax( zpx( rackx::RACK_PANEL_HEIGHT ), requiredH );
    return L;
}

bool RackEditorView::module_rect( rackx::RackModule* m, SDL_Rect& out ) const
{
    if ( !m ) return false;
    Layout L = layout_of( m );
    out.x = rect.x + m_ox + (int) std::lround( m->x * m_zoom );
    out.y = rect.y + m_oy + (int) std::lround( m->y * m_zoom );
    out.w = L.w;
    out.h = L.h;
    return true;
}

int RackEditorView::active_tab( rackx::RackModule* m ) const
{
    const rackx::PanelSpec* panel = panel_of( m );
    if ( !m || !panel || panel->tabs.empty() ) return 0;
    auto it = m_module_tab.find( m->id );
    int tab = it != m_module_tab.end() ? it->second : 0;
    if ( tab < 0 ) tab = 0;
    if ( tab >= (int) panel->tabs.size() ) tab = (int) panel->tabs.size() - 1;
    return tab;
}

bool RackEditorView::tab_bar_at( rackx::RackModule* m, int sx, int sy, int& tabIndex ) const
{
    const rackx::PanelSpec* panel = panel_of( m );
    if ( !m || !panel || panel->tabs.empty() ) return false;
    SDL_Rect box;
    if ( !module_rect( m, box ) ) return false;
    const int titleH = zpx( panel->headerHeight > 0.f ? panel->headerHeight : float( TITLE_H ) );
    const int barY = box.y + titleH;
    const int barH = zpx( TAB_BAR_H );
    if ( sx < box.x || sx >= box.x + box.w || sy < barY || sy >= barY + barH ) return false;
    const int n = (int) panel->tabs.size();
    int idx = ( sx - box.x ) * n / imax( 1, box.w );
    tabIndex = idx < 0 ? 0 : ( idx >= n ? n - 1 : idx );
    return true;
}

bool RackEditorView::knob_pos( rackx::RackModule* m, int i, int& cx, int& cy, int& r ) const
{
    if ( !m ) return false;
    Layout L = layout_of( m );
    if ( i < 0 || i >= L.nP ) return false;

    int ox = rect.x + m_ox + (int) std::lround( m->x * m_zoom );
    int oy = rect.y + m_oy + (int) std::lround( m->y * m_zoom );
    if ( L.panel )
    {
        const rackx::PanelElement* element = find_element( L.panel->params, i );
        if ( !element ) return false;
        cx = ox + zpx( element->x );
        cy = oy + zpx( element->y );
        r = imax( 3, zpx( element->radius ) );
        return true;
    }
    if ( L.knobsPerRow <= 0 ) return false;
    int row = i / L.knobsPerRow;
    int col = i % L.knobsPerRow;
    int cellW  = zpx( KNOB_CELL_W + 8 );
    int gridW  = L.knobsPerRow * cellW;
    int startX = ox + zpx( PAD ) + ( L.innerW - gridW ) / 2;
    cx = startX + col * cellW + cellW / 2;
    cy = oy + zpx( TITLE_H ) + row * L.knobCellH + zpx( KNOB_R + 6 );
    r  = zpx( KNOB_R );
    return true;
}

bool RackEditorView::light_pos( rackx::RackModule* m, int i, int& cx, int& cy, int& r ) const
{
    if ( !m ) return false;
    Layout L = layout_of( m );
    if ( i < 0 || i >= L.nL ) return false;

    int ox = rect.x + m_ox + (int) std::lround( m->x * m_zoom );
    int oy = rect.y + m_oy + (int) std::lround( m->y * m_zoom );
    if ( L.panel )
    {
        const rackx::PanelElement* element = find_element( L.panel->lights, i );
        if ( !element ) return false;
        cx = ox + zpx( element->x );
        cy = oy + zpx( element->y );
        r = imax( 2, zpx( element->radius ) );
        return true;
    }
    if ( L.lightsPerRow <= 0 ) return false;
    int row = i / L.lightsPerRow;
    int col = i % L.lightsPerRow;
    int cellW  = zpx( LIGHT_CELL );
    int rowH   = zpx( LIGHT_ROW_H );
    int gridW  = L.lightsPerRow * cellW;
    int startX = ox + zpx( PAD ) + ( L.innerW - gridW ) / 2;
    cx = startX + col * cellW + cellW / 2;
    cy = oy + zpx( TITLE_H ) + L.knobAreaH + row * rowH + rowH / 2 + 1;
    r  = zpx( LIGHT_R );
    return true;
}

bool RackEditorView::jack_pos( rackx::RackModule* m, bool isInput, int i,
                               int& cx, int& cy, int& r ) const
{
    if ( !m ) return false;
    Layout L = layout_of( m );
    int n = isInput ? L.nI : L.nO;
    if ( i < 0 || i >= n ) return false;

    int ox = rect.x + m_ox + (int) std::lround( m->x * m_zoom );
    int oy = rect.y + m_oy + (int) std::lround( m->y * m_zoom );
    if ( L.panel )
    {
        const std::vector<rackx::PanelElement>& elements = isInput
            ? L.panel->inputs : L.panel->outputs;
        const rackx::PanelElement* element = find_element( elements, i );
        if ( !element ) return false;
        cx = ox + zpx( element->x );
        cy = oy + zpx( element->y );
        r = imax( 3, zpx( element->radius ) );
        return true;
    }
    int jackTop = oy + L.h - zpx( BOTTOM_PAD ) - L.jackAreaH + 2;
    cy = jackTop + i * L.jackRowH + L.jackRowH / 2;
    cx = isInput ? ( ox + zpx( PAD ) + zpx( JACK_R ) + 1 )
                 : ( ox + L.w - zpx( PAD ) - zpx( JACK_R ) - 1 );
    r  = zpx( JACK_R );
    return true;
}

// ============================================================================
//  Hit testing (SCREEN coords; iterate topmost-first == last drawn first)
// ============================================================================
rackx::RackModule* RackEditorView::module_at( int sx, int sy ) const
{
    if ( !m_engine ) return nullptr;
    for ( int i = m_engine->moduleCount() - 1; i >= 0; --i )
    {
        rackx::RackModule* m = m_engine->moduleAt( i );
        SDL_Rect box;
        if ( module_rect( m, box ) && in_rect( box, sx, sy ) ) return m;
    }
    return nullptr;
}

bool RackEditorView::title_at( rackx::RackModule* m, int sx, int sy ) const
{
    SDL_Rect box;
    if ( !module_rect( m, box ) ) return false;
    Layout L = layout_of( m );
    const int titleH = zpx( L.panel ? L.panel->headerHeight : float( TITLE_H ) );
    return sx >= box.x && sx < box.x + box.w && sy >= box.y && sy < box.y + titleH;
}

bool RackEditorView::knob_at( int sx, int sy, int& modId, int& paramIdx ) const
{
    if ( !m_engine ) return false;
    for ( int i = m_engine->moduleCount() - 1; i >= 0; --i )
    {
        rackx::RackModule* m = m_engine->moduleAt( i );
        Layout L = layout_of( m );
        for ( int p = 0; p < L.nP; ++p )
        {
            int cx, cy, r;
            if ( !knob_pos( m, p, cx, cy, r ) ) continue;
            const rackx::PanelElement* element = L.panel
                ? find_element( L.panel->params, p ) : nullptr;
            if ( L.panel && !L.panel->tabs.empty() &&
                 !tab_visible( element, active_tab( m ) ) ) continue;
            bool hit = near_pt( sx, sy, cx, cy, r + 2 );
            if ( element && element->style == rackx::PanelControlStyle::Slider )
            {
                const int halfW = element->width > 0.f ? imax( 3, zpx( element->width ) / 2 ) : r;
                const int halfH = element->height > 0.f ? imax( 12, zpx( element->height ) / 2 ) : r * 3;
                hit = std::abs( sx - cx ) <= halfW + 3 && std::abs( sy - cy ) <= halfH + 3;
            }
            else if ( element && ( element->style == rackx::PanelControlStyle::Switch ||
                                   element->style == rackx::PanelControlStyle::Gate ) )
                hit = std::abs( sx - cx ) <= r + 2 && std::abs( sy - cy ) <= r + 2;
            else if ( element && element->style == rackx::PanelControlStyle::NumberBox )
            {
                const int halfW = element->width > 0.f ? imax( 5, zpx( element->width ) / 2 ) : r + 2;
                const int halfH = element->height > 0.f ? imax( 4, zpx( element->height ) / 2 ) : r + 2;
                hit = std::abs( sx - cx ) <= halfW + 2 && std::abs( sy - cy ) <= halfH + 2;
            }
            if ( hit )
            {
                modId = m->id; paramIdx = p; return true;
            }
        }
    }
    return false;
}

bool RackEditorView::jack_at( int sx, int sy, bool wantInput, int& modId, int& portIdx ) const
{
    if ( !m_engine ) return false;
    // Nearest jack within a generous hit radius (so it never mis-picks a
    // neighbour).  Scale it with zoom, but never below the base HIT_R, so that
    // zooming OUT can never make the (now tiny) jacks unclickable.
    const int hitR = imax( zpx( HIT_R ), HIT_R );
    long bestD = (long) hitR * hitR + 1;
    bool got = false;
    for ( int i = m_engine->moduleCount() - 1; i >= 0; --i )
    {
        rackx::RackModule* m = m_engine->moduleAt( i );
        Layout L = layout_of( m );
        int n = wantInput ? L.nI : L.nO;
        const bool tabbed = L.panel && !L.panel->tabs.empty();
        const int active = tabbed ? active_tab( m ) : 0;
        for ( int p = 0; p < n; ++p )
        {
            int cx, cy, r;
            if ( !jack_pos( m, wantInput, p, cx, cy, r ) ) continue;
            if ( tabbed && !tab_visible( find_element( wantInput ? L.panel->inputs
                                                                 : L.panel->outputs, p ), active ) )
                continue;
            const long dx = sx - cx, dy = sy - cy, d = dx * dx + dy * dy;
            if ( d <= (long) hitR * hitR && d < bestD )
            { bestD = d; modId = m->id; portIdx = p; got = true; }
        }
    }
    return got;
}

bool RackEditorView::cable_at( int sx, int sy, int& cableId ) const
{
    if ( !m_engine ) return false;
    double best = imax( zpx( HIT_R ), HIT_R );   // generous, scales with zoom
    bool   got  = false;
    for ( int i = 0; i < m_engine->cableCount(); ++i )
    {
        rackx::RackCable* c = m_engine->cableAt( i );
        if ( !c ) continue;
        rackx::RackModule* a = m_engine->moduleById( c->fromMod );
        rackx::RackModule* b = m_engine->moduleById( c->toMod );
        if ( !a || !b ) continue;
        int x1, y1, x2, y2, rr;
        if ( !jack_pos( a, false, c->outPort, x1, y1, rr ) ) continue;
        if ( !jack_pos( b, true,  c->inPort,  x2, y2, rr ) ) continue;
        double dd = bezier_dist( x1, y1, x2, y2, sx, sy );
        if ( dd < best ) { best = dd; cableId = c->id; got = true; }
    }
    return got;
}

// ============================================================================
//  Drawing
// ============================================================================
void RackEditorView::draw( ui::App& app )
{
    if ( !visible ) return;
    m_cw = app.mono.cw();
    m_ch = app.mono.ch();

    const RackColors colors = rack_colors();
    SDL_Renderer* r = app.ren;

    ui::fill_rect( r, rect, colors.canvas );
    ui::frame_rect( r, rect, colors.edge );

    SDL_Rect clip = rect;
    SDL_RenderSetClipRect( r, &clip );

    const int grid = imax( 8, zpx( rackx::RACK_HP_WIDTH ) );
    int gridX = rect.x + ( ( m_ox % grid ) + grid ) % grid - grid;
    int gridY = rect.y + ( ( m_oy % grid ) + grid ) % grid - grid;
    for ( int x = gridX; x < rect.x + rect.w; x += grid )
        ui::vline( r, x, rect.y, rect.y + rect.h, colors.grid );
    for ( int y = gridY; y < rect.y + rect.h; y += grid )
        ui::hline( r, rect.x, rect.x + rect.w, y, colors.grid );

    if ( m_engine )
    {
        int mouseX = 0, mouseY = 0, cableId = 0;
        SDL_GetMouseState( &mouseX, &mouseY );
        m_hover_cable = in_rect( rect, mouseX, mouseY ) && cable_at( mouseX, mouseY, cableId )
            ? cableId : -1;
        m_hover_cable_endpoints = false;
        if ( m_hover_cable >= 0 )
        {
            for ( int index = 0; index < m_engine->cableCount(); ++index )
            {
                rackx::RackCable* hovered = m_engine->cableAt( index );
                if ( !hovered || hovered->id != m_hover_cable ) continue;
                rackx::RackModule* hoverOut = m_engine->moduleById( hovered->fromMod );
                rackx::RackModule* hoverIn = m_engine->moduleById( hovered->toMod );
                int radius = 0;
                m_hover_cable_endpoints = hoverOut && hoverIn &&
                    jack_pos( hoverOut, false, hovered->outPort, m_hover_out_x, m_hover_out_y, radius ) &&
                    jack_pos( hoverIn, true, hovered->inPort, m_hover_in_x, m_hover_in_y, radius );
                break;
            }
        }

        for ( int i = 0; i < m_engine->moduleCount(); ++i )
            draw_module( app, m_engine->moduleAt( i ) );

        for ( int i = 0; i < m_engine->cableCount(); ++i )
            draw_cable( app, m_engine->cableAt( i ) );

        if ( m_wire ) draw_drag_wire( app );

        if ( m_engine->moduleCount() == 0 )
            app.mono.draw_centered( r, rect, "click to add a module", colors.muted );
    }
    else
    {
        app.mono.draw_centered( r, rect, "(no rack engine)", colors.muted );
    }

    SDL_RenderSetClipRect( r, nullptr );

    // draggable H/V scroll bars for navigating large patches
    { int cl, cr, ct, cb; content_bounds( cl, cr, ct, cb );
      m_scroll.draw( app, rect, cl, cr, ct, cb, m_ox, m_oy ); }

    if ( m_pal ) draw_palette( app );
    draw_ctx_menu( app );
}

void RackEditorView::draw_module( ui::App& app, rackx::RackModule* m )
{
    if ( !m ) return;
    SDL_Rect box;
    if ( !module_rect( m, box ) ) return;

    // cull fully off-canvas panels
    if ( box.x + box.w < rect.x || box.x > rect.x + rect.w ||
         box.y + box.h < rect.y || box.y > rect.y + rect.h )
        return;

    SDL_Renderer* r = app.ren;
    const RackColors colors = rack_colors();
    Layout L = layout_of( m );
    bool sel = ( m->id == m_sel );

    bool textured = false;
    ui::fill_rect( r, box, colors.panel );
    if ( L.panel && !L.panel->textureAsset.empty() )
        textured = m_cardinalTextures.draw( r, L.panel->textureAsset, box,
                                            L.panel->texturePack );
    ui::frame_rect( r, box, sel ? colors.selection : colors.edgeStrong );
    SDL_Rect inset { box.x + 2, box.y + 2, box.w - 4, box.h - 4 };
    if ( inset.w > 0 && inset.h > 0 ) ui::frame_rect( r, inset, colors.inset );

    if ( !textured )
    {
        int titleH = zpx( L.panel ? L.panel->headerHeight : float( TITLE_H ) );
        SDL_Rect tb { box.x, box.y, box.w, titleH };
        ui::fill_rect( r, tb, colors.header );
        ui::hline( r, tb.x, tb.x + tb.w - 1, tb.y + tb.h - 1, colors.edgeStrong );
        const float textScale = m_zoom;
        const int textH = zpx( m_ch );
        std::string title = clip_text_scaled( app.mono, m->name, box.w - zpx( 10 ), textScale );
        const int titleW = app.mono.text_w_scaled( title, textScale );
        app.mono.draw_scaled( r, box.x + ( box.w - titleW ) / 2,
                              box.y + imax( 0, ( titleH - textH ) / 2 ),
                              title, colors.text, textScale );
    }

    if ( L.panel && !textured )
    {
        const int screw = imax( 2, zpx( 3.f ) );
        fill_disc( r, box.x + zpx( 8.f ), box.y + zpx( 8.f ), screw, colors.well );
        fill_disc( r, box.x + box.w - zpx( 8.f ), box.y + zpx( 8.f ), screw, colors.well );
        fill_disc( r, box.x + zpx( 8.f ), box.y + box.h - zpx( 8.f ), screw, colors.well );
        fill_disc( r, box.x + box.w - zpx( 8.f ), box.y + box.h - zpx( 8.f ), screw, colors.well );
        circle_ring( r, box.x + zpx( 8.f ), box.y + zpx( 8.f ), screw, colors.edge );
        circle_ring( r, box.x + box.w - zpx( 8.f ), box.y + zpx( 8.f ), screw, colors.edge );
        circle_ring( r, box.x + zpx( 8.f ), box.y + box.h - zpx( 8.f ), screw, colors.edge );
        circle_ring( r, box.x + box.w - zpx( 8.f ), box.y + box.h - zpx( 8.f ), screw, colors.edge );
    }

    const bool tabbed = L.panel && !L.panel->tabs.empty();
    const int  active = tabbed ? active_tab( m ) : 0;

    // Tab bar under the title strip: one button per page, active highlighted.
    if ( tabbed )
    {
        const int titleH = zpx( L.panel->headerHeight > 0.f ? L.panel->headerHeight : float( TITLE_H ) );
        const int barY = box.y + titleH;
        const int barH = zpx( TAB_BAR_H );
        const int n = (int) L.panel->tabs.size();
        for ( int t = 0; t < n; ++t )
        {
            const int x0 = box.x + t * box.w / n;
            const int x1 = box.x + ( t + 1 ) * box.w / n;
            SDL_Rect tabR { x0, barY, imax( 1, x1 - x0 ), barH };
            const bool on = ( t == active );
            ui::fill_rect( r, tabR, on ? colors.header : colors.well );
            ui::frame_rect( r, tabR, on ? colors.selection : colors.edgeStrong );
            const std::string label = clip_text_scaled( app.mono, L.panel->tabs[t],
                                                        tabR.w - zpx( 6 ), m_zoom );
            const int textW = app.mono.text_w_scaled( label, m_zoom );
            app.mono.draw_scaled( r, tabR.x + ( tabR.w - textW ) / 2,
                                  tabR.y + imax( 0, ( barH - zpx( m_ch ) ) / 2 ),
                                  label, on ? colors.selection : colors.text, m_zoom );
        }
    }

    for ( int i = 0; i < L.nP; ++i )
        if ( !tabbed || tab_visible( find_element( L.panel->params, i ), active ) )
            draw_knob( app, m, i );
    for ( int i = 0; i < L.nL; ++i )
        if ( !tabbed || tab_visible( find_element( L.panel->lights, i ), active ) )
            draw_light( app, m, i );
    for ( int i = 0; i < L.nI; ++i )
        if ( !tabbed || tab_visible( find_element( L.panel->inputs, i ), active ) )
            draw_jack( app, m, true,  i );
    for ( int i = 0; i < L.nO; ++i )
        if ( !tabbed || tab_visible( find_element( L.panel->outputs, i ), active ) )
            draw_jack( app, m, false, i );

    // LED-style segment readout for Audio-In / Audio-Out modules: "OUT n" / "IN n",
    // numbered top-to-bottom to MATCH this module's port on the host patcher node.
    const int ch = m_engine ? m_engine->audioChannelIndex( m->id ) : 0;
    if ( ch > 0 )
    {
        const bool isOut = ( m->role == rackx::Role::AudioOut );
        char txt[16]; std::snprintf( txt, sizeof(txt), "%s %d", isOut ? "OUT" : "IN", ch );
        const int titleH = zpx( L.panel ? L.panel->headerHeight : float( TITLE_H ) );
        const int dh = imax( zpx( m_ch ) + zpx( 4.f ), zpx( 12.f ) );
        SDL_Rect disp { box.x + zpx( 4.f ), box.y + titleH + zpx( 3.f ),
                        box.w - zpx( 8.f ), dh };
        if ( disp.w > 6 && disp.h > 4 )
        {
            ui::fill_rect( r, disp, colors.well );            // dark LED bezel
            ui::frame_rect( r, disp, colors.edgeStrong );
            const int tw = app.mono.text_w_scaled( txt, m_zoom );
            app.mono.draw_scaled( r, disp.x + imax( 2, ( disp.w - tw ) / 2 ),
                                  disp.y + imax( 0, ( dh - zpx( m_ch ) ) / 2 ),
                                  txt, colors.selection, m_zoom );   // bright readout
        }
    }
}

void RackEditorView::draw_knob( ui::App& app, rackx::RackModule* m, int i )
{
    int cx, cy, r;
    if ( !knob_pos( m, i, cx, cy, r ) ) return;
    if ( !m->mod || i >= (int) m->mod->params.size() ) return;

    SDL_Renderer* rn = app.ren;
    const RackColors colors = rack_colors();

    // normalise the live value over its declared range
    float val = m->mod->params[i].value;
    float mn = 0.f, mx = 1.f;
    std::string name;
    if ( i < (int) m->mod->paramQuantities.size() )
    {
        const rack::engine::ParamQuantity& q = m->mod->paramQuantities[i];
        mn = q.minValue; mx = q.maxValue; name = q.name;
    }
    float norm = ( mx != mn ) ? ( val - mn ) / ( mx - mn ) : 0.5f;
    norm = clamp01( norm );

    Layout L = layout_of( m );
    const rackx::PanelElement* element = L.panel ? find_element( L.panel->params, i ) : nullptr;
    if ( element && !element->label.empty() ) name = element->label;
    if ( L.panel && m_cardinalTextures.aspect( L.panel->textureAsset,
                                                L.panel->texturePack ) > 0.f ) name.clear();
    const rackx::PanelLabelPlacement placement = element
        ? element->labelPlacement : rackx::PanelLabelPlacement::Below;
    const float textScale = m_zoom;
    const int textH = zpx( m_ch );
    const int labelWidth = element ? imax( zpx( 42 ), 2 * r + zpx( 14 ) )
                                   : zpx( KNOB_CELL_W + 8 );
    auto drawLabel = [&]( int anchorX, int anchorY, int controlRadius,
                          rackx::PanelLabelPlacement where ) {
        if ( name.empty() || where == rackx::PanelLabelPlacement::None ) return;
        std::string text = clip_text_scaled( app.mono, name, labelWidth, textScale );
        if ( text.empty() ) return;
        const int textW = app.mono.text_w_scaled( text, textScale );
        int x = anchorX - textW / 2;
        int y = anchorY + controlRadius + zpx( 6 );
        if ( where == rackx::PanelLabelPlacement::Above )
            y = anchorY - controlRadius - textH - zpx( 6 );
        else if ( where == rackx::PanelLabelPlacement::Left )
        {
            x = anchorX - controlRadius - zpx( 6 ) - textW;
            y = anchorY - textH / 2;
        }
        else if ( where == rackx::PanelLabelPlacement::Right )
        {
            x = anchorX + controlRadius + zpx( 6 );
            y = anchorY - textH / 2;
        }
        app.mono.draw_scaled( rn, x, y, text, colors.text, textScale );
    };

    if ( element && element->style == rackx::PanelControlStyle::Slider )
    {
        const int halfW = element->width > 0.f ? imax( 3, zpx( element->width ) / 2 )
                                               : imax( 4, r / 2 );
        const int halfH = element->height > 0.f ? imax( 12, zpx( element->height ) / 2 )
                                                : imax( 24, r * 3 );
        SDL_Rect track { cx - halfW, cy - halfH, 2 * halfW + 1, 2 * halfH + 1 };
        ui::fill_rect( rn, track, colors.well );
        ui::frame_rect( rn, track, colors.edgeStrong );
        const int slotW = imax( 2, halfW / 3 );
        ui::fill_rect( rn, SDL_Rect{ cx - slotW / 2, cy - halfH + 4,
                                     imax( 1, slotW ), 2 * halfH - 7 }, colors.panel );
        const int markerY = cy + halfH - (int) std::lround( norm * ( 2 * halfH ) );
        SDL_Rect marker { cx - halfW - 3, markerY - imax( 3, halfW / 2 ),
                          2 * halfW + 7, imax( 6, halfW ) };
        ui::fill_rect( rn, marker, colors.marker );
        ui::frame_rect( rn, marker, colors.edgeStrong );
        drawLabel( cx, cy + halfH - r, r, placement );
        return;
    }

    if ( element && element->style == rackx::PanelControlStyle::Switch )
    {
        SDL_Rect toggle { cx - r, cy - r, 2 * r + 1, 2 * r + 1 };
        ui::fill_rect( rn, toggle, colors.well );
        ui::frame_rect( rn, toggle, colors.edgeStrong );
        SDL_Rect handle { cx - r + 2, cy - r + 2, r - 1, 2 * r - 3 };
        if ( norm >= 0.5f ) handle.x = cx + 1;
        ui::fill_rect( rn, handle, norm >= 0.5f ? colors.marker : colors.muted );
        drawLabel( cx, cy, r, placement );
        return;
    }

    if ( element && element->style == rackx::PanelControlStyle::Button )
    {
        fill_disc( rn, cx, cy, r + 2, colors.well );
        fill_disc( rn, cx, cy, r, norm > 0.f ? colors.marker : colors.knob );
        circle_ring( rn, cx, cy, r + 2, colors.edgeStrong );
        drawLabel( cx, cy, r, placement );
        return;
    }

    // Gate: a latching step-gate LED.  Square well, glows when on.
    if ( element && element->style == rackx::PanelControlStyle::Gate )
    {
        const bool on = norm >= 0.5f;
        SDL_Rect well { cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3 };
        ui::fill_rect( rn, well, colors.well );
        ui::frame_rect( rn, well, colors.edgeStrong );
        if ( on )
        {
            SDL_Rect lit { cx - r + 1, cy - r + 1, 2 * r - 1, 2 * r - 1 };
            ui::fill_rect( rn, lit, colors.marker );
        }
        drawLabel( cx, cy, r, placement );
        return;
    }

    // Number box: a small square showing the param's display value (drag to
    // edit, same vertical-drag gesture as a knob).
    if ( element && element->style == rackx::PanelControlStyle::NumberBox )
    {
        const int bw = imax( 10, zpx( element->width > 0.f ? element->width : 2.f * element->radius ) );
        const int bh = imax( 8, zpx( element->height > 0.f ? element->height : 2.f * element->radius ) );
        SDL_Rect box { cx - bw / 2, cy - bh / 2, bw, bh };
        ui::fill_rect( rn, box, colors.well );
        ui::frame_rect( rn, box, colors.edgeStrong );
        // Display value: value * displayMultiplier + displayOffset (or an
        // exp mapping when displayBase > 0), matching ParamQuantity semantics.
        float disp = val;
        if ( i < (int) m->mod->paramQuantities.size() )
        {
            const rack::engine::ParamQuantity& q = m->mod->paramQuantities[i];
            if ( q.displayBase > 0.f ) disp = std::pow( q.displayBase, val );
            disp = disp * ( q.displayMultiplier != 0.f ? q.displayMultiplier : 1.f ) + q.displayOffset;
        }
        char numbuf[16];
        std::snprintf( numbuf, sizeof( numbuf ), "%d", (int) std::lround( disp ) );
        const float textScale = m_zoom;
        const int textW = app.mono.text_w_scaled( numbuf, textScale );
        const int textH = zpx( m_ch );
        app.mono.draw_scaled( rn, cx - textW / 2, cy - textH / 2, numbuf, colors.text, textScale );
        return;
    }

    fill_disc( rn, cx, cy, r + 2, colors.well );
    circle_ring( rn, cx, cy, r + 2, colors.edgeStrong );
    fill_disc( rn, cx, cy, imax( 2, r - 2 ), colors.knob );
    circle_ring( rn, cx, cy, imax( 2, r - 2 ), colors.edge );
    for ( int tick = 0; tick < 5; ++tick )
    {
        const double tickAngle = ( -135.0 + tick * 67.5 ) * PI / 180.0;
        const int ox = cx + (int) std::lround( std::sin( tickAngle ) * ( r + 4 ) );
        const int oy = cy - (int) std::lround( std::cos( tickAngle ) * ( r + 4 ) );
        const int ix = cx + (int) std::lround( std::sin( tickAngle ) * ( r + 1 ) );
        const int iy = cy - (int) std::lround( std::cos( tickAngle ) * ( r + 1 ) );
        ui::set_color( rn, colors.muted );
        SDL_RenderDrawLine( rn, ix, iy, ox, oy );
    }

    double a  = ( -135.0 + norm * 270.0 ) * PI / 180.0;
    const int needle = imax( 1, r - 4 );
    int    ex = cx + (int) std::lround( std::sin( a ) * needle );
    int    ey = cy - (int) std::lround( std::cos( a ) * needle );
    ui::set_color( rn, colors.marker );
    SDL_RenderDrawLine( rn, cx, cy, ex, ey );
    SDL_RenderDrawLine( rn, cx + 1, cy, ex + 1, ey );
    SDL_RenderDrawLine( rn, cx, cy + 1, ex, ey + 1 );

    drawLabel( cx, cy, r, placement );
}

void RackEditorView::draw_light( ui::App& app, rackx::RackModule* m, int i )
{
    int cx, cy, r;
    if ( !light_pos( m, i, cx, cy, r ) ) return;
    if ( !m->mod || i >= (int) m->mod->lights.size() ) return;

    SDL_Renderer* rn = app.ren;
    const RackColors colors = rack_colors();
    float b = clamp01( m->mod->lights[i].getBrightness() );
    if ( b <= 0.f ) return;
    const Color led = rgb_led_color( m->mod->lights[i], colors );
    fill_disc( rn, cx, cy, r, led );
}

void RackEditorView::draw_jack( ui::App& app, rackx::RackModule* m, bool isInput, int i )
{
    int cx, cy, r;
    if ( !jack_pos( m, isInput, i, cx, cy, r ) ) return;

    SDL_Renderer* rn = app.ren;
    const RackColors colors = rack_colors();

    // valid drop-target halo while wiring
    bool hi = m_wire && m_wire_ok && isInput &&
              m_hover_mod == m->id && m_hover_in == i;
    // armed click-to-connect SOURCE (an output jack clicked, awaiting an input)
    bool armed = ( !isInput && m->id == m_link_out_mod && i == m_link_out_port );
    if ( hi || armed ) circle_ring( rn, cx, cy, r + 4, colors.selection );
    fill_disc( rn, cx, cy, r + 2, colors.well );
    circle_ring( rn, cx, cy, r + 2, colors.edgeStrong );
    if ( isInput )
    {
        fill_disc( rn, cx, cy, imax( 2, r - 2 ), colors.muted );
        circle_ring( rn, cx, cy, r, colors.text );
    }
    else if ( armed )
    {
        fill_disc( rn, cx, cy, imax( 2, r - 2 ), colors.marker );
        circle_ring( rn, cx, cy, r, colors.selection );
    }
    else
    {
        fill_disc( rn, cx, cy, imax( 2, r - 3 ), colors.well );
        circle_ring( rn, cx, cy, r, colors.marker );
    }

    // label: input to the right of its jack, output to the left of its jack
    if ( !m->mod ) return;
    std::string lbl;
    Layout L = layout_of( m );
    const rackx::PanelElement* element = nullptr;
    if ( L.panel )
        element = find_element( isInput ? L.panel->inputs : L.panel->outputs, i );
    if ( element && !element->label.empty() ) lbl = element->label;
    if ( lbl.empty() )
    {
        if ( isInput ) { if ( i < (int) m->mod->inputInfos.size() )  lbl = m->mod->inputInfos[i]; }
        else           { if ( i < (int) m->mod->outputInfos.size() ) lbl = m->mod->outputInfos[i]; }
    }
    if ( lbl.empty() ) return;
    if ( L.panel && m_cardinalTextures.aspect( L.panel->textureAsset,
                                                L.panel->texturePack ) > 0.f ) return;

    SDL_Rect box; module_rect( m, box );
    const bool compact = L.panel && box.w < zpx( 6.f * rackx::RACK_HP_WIDTH );
    const int room = compact ? box.w - zpx( 6 )
                             : box.w / 2 - zpx( PAD ) - 2 * zpx( JACK_R ) - zpx( 6 );
    std::string s = clip_text_scaled( app.mono, lbl, room, m_zoom );
    if ( s.empty() ) return;
    const int labelW = app.mono.text_w_scaled( s, m_zoom );
    const int labelH = zpx( m_ch );
    if ( compact )
    {
        const int minX = box.x + zpx( 3 );
        const int maxX = box.x + box.w - zpx( 3 ) - labelW;
        const int x = std::max( minX, std::min( cx - labelW / 2, maxX ) );
        const int titleH = zpx( L.panel->headerHeight );
        int y = cy - r - labelH - zpx( 2 );
        if ( y < box.y + titleH + zpx( 2 ) ) y = cy + r + zpx( 2 );
        app.mono.draw_scaled( rn, x, y, s, colors.text, m_zoom );
        return;
    }
    if ( isInput )
        app.mono.draw_scaled( rn, cx + r + zpx( 3 ), cy - labelH / 2, s, colors.text, m_zoom );
    else
        app.mono.draw_scaled( rn, cx - r - zpx( 3 ) - labelW, cy - labelH / 2,
                              s, colors.text, m_zoom );
}

void RackEditorView::draw_cable( ui::App& app, rackx::RackCable* c )
{
    if ( !c || !m_engine ) return;
    rackx::RackModule* a = m_engine->moduleById( c->fromMod );
    rackx::RackModule* b = m_engine->moduleById( c->toMod );
    if ( !a || !b ) return;
    int x1, y1, x2, y2, rr;
    if ( !jack_pos( a, false, c->outPort, x1, y1, rr ) ) return;
    if ( !jack_pos( b, true,  c->inPort,  x2, y2, rr ) ) return;
    const RackColors colors = rack_colors();
    float laneOffset = 0.f;
    if ( m_hover_cable_endpoints && c->id != m_hover_cable )
    {
        const double nearby = imax( zpx( 72 ), 36 );
        auto distance = []( int ax, int ay, int bx, int by ) {
            return std::hypot( double( ax - bx ), double( ay - by ) );
        };
        const double nearest = std::min(
            std::min( distance( x1, y1, m_hover_out_x, m_hover_out_y ),
                      distance( x1, y1, m_hover_in_x, m_hover_in_y ) ),
            std::min( distance( x2, y2, m_hover_out_x, m_hover_out_y ),
                      distance( x2, y2, m_hover_in_x, m_hover_in_y ) ) );
        if ( nearest < nearby )
        {
            int lane = c->id % 5 - 2;
            if ( lane >= 0 ) ++lane;
            laneOffset = lane * imax( 2, zpx( 3 ) )
                       * float( 1.0 - nearest / nearby );
        }
    }

    const float phase = SDL_GetTicks() * 0.018f + c->id * 1.7f;
    const Color highlight = c->id == m_hover_cable ? colors.selection : colors.cableHighlight;
    auto stroke = [&]( Color color, int thickness ) {
        if ( std::fabs( laneOffset ) > 0.1f )
            stroke_wiggled_bezier( app.ren, x1, y1, x2, y2, color, thickness, laneOffset, phase );
        else
            stroke_bezier( app.ren, x1, y1, x2, y2, color, thickness, false );
    };
    stroke( colors.cableOuter, imax( 4, zpx( 5 ) ) );
    stroke( colors.cableInner, imax( 2, zpx( 3 ) ) );
    stroke( highlight, 1 );
}

void RackEditorView::draw_drag_wire( ui::App& app )
{
    if ( !m_engine ) return;
    rackx::RackModule* a = m_engine->moduleById( m_wire_mod );
    if ( !a ) return;
    int x1, y1, rr;
    if ( !jack_pos( a, false, m_wire_out, x1, y1, rr ) ) return;
    const RackColors colors = rack_colors();
    stroke_bezier( app.ren, x1, y1, m_wire_x, m_wire_y, colors.cableOuter,
                   imax( 4, zpx( 5 ) ), true );
    stroke_bezier( app.ren, x1, y1, m_wire_x, m_wire_y,
                   m_wire_ok ? colors.selection : colors.cableHighlight,
                   imax( 2, zpx( 3 ) ), true );
}

// ============================================================================
//  Module palette overlay (SDL has no native popups -- draw a rectangle)
// ============================================================================
std::vector<std::string> RackEditorView::palette_types() const
{
    std::vector<std::string> types;
    for ( const rackx::ModuleType& type : rackx::registry() )
    {
        if ( !type.paletteVisible || type.category.empty() ) continue;
        if ( std::find( types.begin(), types.end(), type.category ) == types.end() )
            types.push_back( type.category );
    }
    std::sort( types.begin(), types.end() );
    return types;
}

std::vector<int> RackEditorView::filtered() const
{
    std::vector<int> out;
    const std::vector<rackx::ModuleType>& reg = rackx::registry();
    for ( int i = 0; i < (int) reg.size(); ++i )
        if ( reg[i].paletteVisible &&
             ( m_pal_type.empty() || reg[i].category == m_pal_type ) &&
             ( ci_contains( reg[i].name, m_search ) ||
             ci_contains( reg[i].slug, m_search ) ||
             ci_contains( reg[i].category, m_search ) ) )
            out.push_back( i );
    return out;
}

SDL_Rect RackEditorView::palette_rect( ui::App& app ) const
{
    int rowh = m_ch + 6;
    int w = imin( PAL_W, imax( 1, rect.w - 8 ) );
    int h = 3 * rowh + PAL_ROWS * PAL_CARD_H + 8;
    if ( h > rect.h - 8 ) h = imax( 1, rect.h - 8 );
    SDL_Rect q { rect.x + 4, rect.y + 4, w, h };
    return q;
}

void RackEditorView::open_palette( ui::App& app, int /*sx*/, int /*sy*/ )
{
    rackx::registerBuiltins();
    m_pal = true;
    m_pal_scroll = 0;
    m_pal_type_scroll = 0;
    m_pal_type.clear();
    m_pal_type_menu = false;
    m_pal_drag = m_pal_dragging = false;
    m_pal_drag_index = -1;
    m_search.clear();
    app.begin_text( &m_search,
                    [&app]() { app.request_redraw(); },
                    [this, &app]( bool /*ok*/ ) { close_palette( app ); } );
    app.request_redraw();
}

void RackEditorView::close_palette( ui::App& app )
{
    m_pal = false;
    m_pal_scroll = 0;
    m_pal_type_scroll = 0;
    m_pal_type.clear();
    m_pal_type_menu = false;
    m_pal_drag = m_pal_dragging = false;
    m_pal_drag_index = -1;
    m_search.clear();
    if ( app.editing_text() ) app.end_text();
    app.request_redraw();
}

// ---- module right-click context menu (Duplicate / Delete) ------------------
namespace { const char* kCtxItems[2] = { "Duplicate", "Delete" };
            const int kCtxCount = 2, kCtxRowH = 20, kCtxW = 110; }

void RackEditorView::draw_ctx_menu( ui::App& app )
{
    if ( m_ctx_mod < 0 ) return;
    const ui::Theme& t = theme();
    const int w = kCtxW, h = kCtxRowH * kCtxCount;
    int x = m_ctx_x, y = m_ctx_y;
    if ( x + w > rect.x + rect.w ) x = rect.x + rect.w - w;
    if ( y + h > rect.y + rect.h ) y = rect.y + rect.h - h;
    int mx = 0, my = 0; SDL_GetMouseState( &mx, &my );
    SDL_Rect box{ x, y, w, h };
    fill_rect( app.ren, box, t.panel );
    frame_rect( app.ren, box, t.dim );
    for ( int i = 0; i < kCtxCount; ++i )
    {
        SDL_Rect rr{ x, y + i * kCtxRowH, w, kCtxRowH };
        const bool hot = mx >= rr.x && mx < rr.x + rr.w && my >= rr.y && my < rr.y + rr.h;
        if ( hot ) fill_rect( app.ren, rr, t.accent );
        app.mono.draw( app.ren, rr.x + 6, rr.y + ( kCtxRowH - app.mono.ch() ) / 2,
                       kCtxItems[i], hot ? t.bg : t.text );
    }
}

bool RackEditorView::ctx_menu_mouse( ui::App& app, const ui::MouseEv& e )
{
    if ( !e.pressed ) { app.request_redraw(); return true; }   // swallow the release
    const int w = kCtxW, h = kCtxRowH * kCtxCount;
    int x = m_ctx_x, y = m_ctx_y;
    if ( x + w > rect.x + rect.w ) x = rect.x + rect.w - w;
    if ( y + h > rect.y + rect.h ) y = rect.y + rect.h - h;
    const int id = m_ctx_mod;
    m_ctx_mod = -1;                                            // any click closes it
    if ( m_engine && e.x >= x && e.x < x + w && e.y >= y && e.y < y + h )
    {
        const int idx = ( e.y - y ) / kCtxRowH;
        if ( idx == 0 ) { int nid = duplicate_module( id ); if ( nid >= 0 ) m_sel = nid; }
        else if ( idx == 1 ) { m_engine->removeModule( id ); if ( m_sel == id ) m_sel = -1; }
    }
    app.request_redraw();
    return true;
}

int RackEditorView::duplicate_module( int id )
{
    if ( !m_engine ) return -1;
    rackx::RackModule* m = m_engine->moduleById( id );
    if ( !m ) return -1;
    const std::string slug = m->slug;
    const float nx = m->x + 24.f, ny = m->y + 24.f;
    const int pc = m_engine->moduleParamCount( id );
    std::vector<float> vals( (size_t)( pc > 0 ? pc : 0 ) );
    for ( int p = 0; p < pc; ++p ) vals[(size_t)p] = m_engine->getParam( id, p );
    const int nid = m_engine->addModule( slug, nx, ny );       // may reallocate mods_ -> m invalid now
    if ( nid < 0 ) return -1;
    for ( int p = 0; p < pc; ++p ) m_engine->setParam( nid, p, vals[(size_t)p] );
    return nid;
}

void RackEditorView::draw_palette( ui::App& app )
{
    SDL_Renderer* r = app.ren;
    const RackColors colors = rack_colors();
    const int rowh = m_ch + 6;

    SDL_Rect p = palette_rect( app );
    ui::fill_rect( r, p, colors.panel );
    ui::frame_rect( r, p, colors.edgeStrong );

    SDL_Rect header { p.x + 2, p.y + 2, p.w - 4, rowh };
    ui::fill_rect( r, header, colors.header );
    app.mono.draw( r, header.x + 7, header.y + 3, "MODULE BROWSER", colors.text );
    SDL_Rect collapse { header.x + header.w - rowh, header.y, rowh, rowh };
    ui::frame_rect( r, collapse, colors.edgeStrong );
    app.mono.draw( r, collapse.x + 7, collapse.y + 3, "<", colors.text );

    SDL_Rect typeBox { p.x + 2, p.y + 2 + rowh, p.w - 4, rowh };
    ui::fill_rect( r, typeBox, colors.well );
    ui::frame_rect( r, typeBox, colors.edge );
    std::string typeLabel = "TYPE: " + ( m_pal_type.empty() ? std::string( "ALL" ) : m_pal_type ) + " v";
    typeLabel = clip_text( app.mono, typeLabel, typeBox.w - 12 );
    app.mono.draw( r, typeBox.x + 7, typeBox.y + 3, typeLabel, colors.text );

    // search box
    SDL_Rect sb { p.x + 2, p.y + 2 + 2 * rowh, p.w - 4, rowh };
    ui::fill_rect( r, sb, colors.well );
    ui::frame_rect( r, sb, colors.edge );
    if ( m_search.empty() )
        app.mono.draw( r, sb.x + 4, sb.y + 3, "search modules...", colors.muted );
    else
        app.mono.draw( r, sb.x + 4, sb.y + 3, m_search, colors.text );
    if ( app.editing_text() )
    {
        int cx = sb.x + 4 + app.mono.text_w( m_search );
        ui::vline( r, cx, sb.y + 3, sb.y + rowh - 3, colors.text );
    }

    std::vector<std::string> types = palette_types();
    int typeMaxScroll = (int) types.size() + 1 - PAL_ROWS;
    if ( typeMaxScroll < 0 ) typeMaxScroll = 0;
    if ( m_pal_type_scroll > typeMaxScroll ) m_pal_type_scroll = typeMaxScroll;
    if ( m_pal_type_scroll < 0 ) m_pal_type_scroll = 0;

    // selected type and search matching modules
    std::vector<int> fl = filtered();
    int maxscroll = ( (int) fl.size() + 1 ) / 2 - PAL_ROWS; if ( maxscroll < 0 ) maxscroll = 0;
    if ( m_pal_scroll > maxscroll ) m_pal_scroll = maxscroll;
    if ( m_pal_scroll < 0 )         m_pal_scroll = 0;

    const std::vector<rackx::ModuleType>& reg = rackx::registry();
    int mx, my; SDL_GetMouseState( &mx, &my );
    const int listTop = p.y + 2 + 3 * rowh;
    const int cardGap = 4;
    const int cardWidth = ( p.w - 4 - cardGap ) / 2;
    for ( int row = 0; row < PAL_ROWS; ++row )
    {
        for ( int column = 0; column < 2; ++column )
        {
            const int moduleIndex = 2 * ( m_pal_scroll + row ) + column;
            if ( moduleIndex >= (int) fl.size() ) continue;
            const rackx::ModuleType& mt = reg[ fl[moduleIndex] ];
            SDL_Rect card { p.x + 2 + column * ( cardWidth + cardGap ),
                            listTop + row * PAL_CARD_H, cardWidth, PAL_CARD_H - 3 };
            const bool hover = in_rect( card, mx, my );
            ui::fill_rect( r, card, hover ? colors.header : colors.well );
            ui::frame_rect( r, card, hover ? colors.selection : colors.edgeStrong );
            SDL_Rect image { card.x + 3, card.y + 3, card.w - 6, card.h - rowh - 6 };
            if ( !mt.panel.textureAsset.empty() )
                m_cardinalTextures.draw( r, mt.panel.textureAsset, image,
                                         mt.panel.texturePack );
            else
            {
                ui::fill_rect( r, image, colors.panel );
                ui::frame_rect( r, image, colors.edge );
                const int previewHeader = imax( 7, image.h / 7 );
                SDL_Rect previewTitle { image.x + 2, image.y + 2, image.w - 4, previewHeader };
                ui::fill_rect( r, previewTitle, colors.header );
                const int knobRadius = imax( 3, imin( image.w, image.h ) / 12 );
                const int knobY = image.y + image.h / 2;
                fill_disc( r, image.x + image.w / 3, knobY, knobRadius, colors.well );
                fill_disc( r, image.x + (2 * image.w) / 3, knobY, knobRadius, colors.well );
                circle_ring( r, image.x + image.w / 3, knobY, knobRadius, colors.edgeStrong );
                circle_ring( r, image.x + (2 * image.w) / 3, knobY, knobRadius, colors.edgeStrong );
                const int jackRadius = imax( 2, knobRadius / 2 );
                const int jackY = image.y + image.h - jackRadius - 4;
                fill_disc( r, image.x + image.w / 4, jackY, jackRadius, colors.inset );
                circle_ring( r, image.x + (3 * image.w) / 4, jackY, jackRadius, colors.edgeStrong );
            }
            std::string label = clip_text( app.mono, mt.name, card.w - 10 );
            app.mono.draw( r, card.x + 5, card.y + card.h - rowh + 2, label,
                           hover ? colors.selection : colors.text );
        }
    }

    if ( m_pal_type_menu )
    {
        SDL_Rect menu { typeBox.x, typeBox.y + typeBox.h, typeBox.w,
                        imin( PAL_ROWS, (int) types.size() + 1 ) * rowh };
        ui::fill_rect( r, menu, colors.panel );
        ui::frame_rect( r, menu, colors.selection );
        for ( int row = 0; row < PAL_ROWS; ++row )
        {
            const int typeIndex = m_pal_type_scroll + row;
            if ( typeIndex > (int) types.size() ) break;
            const std::string& type = typeIndex == 0 ? std::string() : types[typeIndex - 1];
            SDL_Rect rr { menu.x + 1, menu.y + row * rowh, menu.w - 2, rowh };
            const bool selected = type == m_pal_type;
            if ( selected || in_rect( rr, mx, my ) ) ui::fill_rect( r, rr, colors.header );
            std::string label = type.empty() ? "ALL TYPES" : type;
            label = clip_text( app.mono, label, rr.w - 12 );
            app.mono.draw( r, rr.x + 7, rr.y + 3, label, selected ? colors.selection : colors.text );
        }
    }

    if ( m_pal_dragging && m_pal_drag_index >= 0 &&
         m_pal_drag_index < (int) reg.size() )
    {
        const rackx::ModuleType& dragged = reg[m_pal_drag_index];
        SDL_Rect ghost { m_pal_drag_x - 72, m_pal_drag_y - 48, 144, 96 };
        ui::fill_rect( r, ghost, colors.panel );
        ui::frame_rect( r, ghost, colors.selection );
        if ( !dragged.panel.textureAsset.empty() )
            m_cardinalTextures.draw( r, dragged.panel.textureAsset, ghost,
                                     dragged.panel.texturePack );
        else
        {
            std::string title = clip_text( app.mono, dragged.name, ghost.w - 12 );
            app.mono.draw( r, ghost.x + 6, ghost.y + 6, title, colors.selection );
        }
    }
}

bool RackEditorView::palette_mouse( ui::App& app, const ui::MouseEv& e, bool downEdge )
{
    if ( !e.pressed )
    {
        if ( m_pal_drag && m_pal_drag_index >= 0 &&
             m_pal_drag_index < (int) rackx::registry().size() )
        {
            SDL_Rect p = palette_rect( app );
            const bool dropOnPatcher = m_pal_dragging && !in_rect( p, e.x, e.y ) &&
                                       in_rect( rect, e.x, e.y );
            if ( dropOnPatcher )
            {
                const std::string slug = rackx::registry()[m_pal_drag_index].slug;
                const float cx = float( e.x - rect.x - m_ox ) / m_zoom;
                const float cy = float( e.y - rect.y - m_oy ) / m_zoom;
                close_palette( app );
                if ( m_engine )
                {
                    const int id = m_engine->addModule( slug, cx, cy );
                    if ( id > 0 ) m_sel = id;
                }
                app.request_redraw();
                return true;
            }
        }
        m_pal_drag = m_pal_dragging = false;
        m_pal_drag_index = -1;
        app.request_redraw();
        return true;
    }
    if ( e.button == SDL_BUTTON_RIGHT ) { close_palette( app ); return true; }
    if ( e.button != SDL_BUTTON_LEFT )  return true;
    if ( !downEdge )
    {
        if ( m_pal_drag )
        {
            m_pal_drag_x = e.x; m_pal_drag_y = e.y;
            const int distance = std::abs( e.x - m_pal_drag_start_x ) +
                                 std::abs( e.y - m_pal_drag_start_y );
            if ( distance > 4 ) m_pal_dragging = true;
            app.request_redraw();
        }
        return true;
    }

    SDL_Rect p = palette_rect( app );
    if ( !in_rect( p, e.x, e.y ) ) { close_palette( app ); return true; }

    const int rowh = m_ch + 6;
    SDL_Rect header { p.x + 2, p.y + 2, p.w - 4, rowh };
    SDL_Rect collapse { header.x + header.w - rowh, header.y, rowh, rowh };
    SDL_Rect typeBox { p.x + 2, p.y + 2 + rowh, p.w - 4, rowh };

    if ( in_rect( collapse, e.x, e.y ) ) { close_palette( app ); return true; }
    if ( m_pal_type_menu )
    {
        std::vector<std::string> types = palette_types();
        SDL_Rect menu { typeBox.x, typeBox.y + typeBox.h, typeBox.w,
                        imin( PAL_ROWS, (int) types.size() + 1 ) * rowh };
        if ( in_rect( menu, e.x, e.y ) )
        {
            const int row = ( e.y - menu.y ) / rowh;
            const int typeIndex = m_pal_type_scroll + row;
            if ( typeIndex <= (int) types.size() )
            {
                m_pal_type = typeIndex == 0 ? "" : types[typeIndex - 1];
                m_pal_scroll = 0;
            }
        }
        m_pal_type_menu = false;
        app.request_redraw();
        return true;
    }
    if ( in_rect( typeBox, e.x, e.y ) )
    {
        m_pal_type_menu = true;
        app.request_redraw();
        return true;
    }
    const int listTop = p.y + 2 + 3 * rowh;
    const int cardGap = 4;
    const int cardWidth = ( p.w - 4 - cardGap ) / 2;
    if ( e.y >= listTop )
    {
        const int row = ( e.y - listTop ) / PAL_CARD_H;
        if ( row >= 0 && row < PAL_ROWS )
        {
            std::vector<int> fl = filtered();
            const int column = e.x >= p.x + 2 + cardWidth + cardGap ? 1 : 0;
            SDL_Rect card { p.x + 2 + column * ( cardWidth + cardGap ),
                            listTop + row * PAL_CARD_H, cardWidth, PAL_CARD_H - 3 };
            const int moduleIndex = 2 * ( m_pal_scroll + row ) + column;
            if ( in_rect( card, e.x, e.y ) && moduleIndex >= 0 && moduleIndex < (int) fl.size() )
            {
                m_pal_drag = true;
                m_pal_dragging = false;
                m_pal_drag_index = fl[moduleIndex];
                m_pal_drag_x = m_pal_drag_start_x = e.x;
                m_pal_drag_y = m_pal_drag_start_y = e.y;
            }
        }
        app.request_redraw();
        return true;
    }
    // click landed on the search box or padding: keep the browser open
    app.request_redraw();
    return true;
}

// ============================================================================
//  Input
// ============================================================================
// Bounding box of all modules in CANVAS PIXELS (world*zoom, before pan) -- the
// scrollable content extent for the scroll bars.
void RackEditorView::content_bounds( int& cl, int& cr, int& ct, int& cb ) const
{
    cl = cr = ct = cb = 0;
    if ( !m_engine ) return;
    bool first = true;
    for ( int i = 0; i < m_engine->moduleCount(); ++i )
    {
        rackx::RackModule* m = m_engine->moduleAt( i );
        if ( !m ) continue;
        Layout L = layout_of( m );
        int x0 = (int) std::lround( m->x * m_zoom );
        int y0 = (int) std::lround( m->y * m_zoom );
        int x1 = x0 + L.w, y1 = y0 + L.h;
        if ( first ) { cl = x0; cr = x1; ct = y0; cb = y1; first = false; }
        else { if ( x0 < cl ) cl = x0; if ( x1 > cr ) cr = x1;
               if ( y0 < ct ) ct = y0; if ( y1 > cb ) cb = y1; }
    }
}

bool RackEditorView::on_mouse( ui::App& app, const ui::MouseEv& e )
{
    m_cw = app.mono.cw();
    m_ch = app.mono.ch();

    // left-button held-state / down-edge for the whole handler
    bool downEdge = false;
    if ( e.button == SDL_BUTTON_LEFT )
    {
        if ( e.pressed ) { downEdge = !m_ldown; m_ldown = true; }
        else             m_ldown = false;
    }

    // palette open: it eats all mouse
    if ( m_pal ) return palette_mouse( app, e, downEdge );
    // context menu open: it eats all mouse
    if ( m_ctx_mod >= 0 ) return ctx_menu_mouse( app, e );

    // scroll bars (bottom + right strips) get first crack for large patches
    { int cl, cr, ct, cb; content_bounds( cl, cr, ct, cb );
      if ( m_scroll.on_mouse( rect, e, downEdge, cl, cr, ct, cb, m_ox, m_oy ) )
      { app.request_redraw(); return true; } }

    // right-click: on a module -> remove it; on a cable -> remove it;
    // on empty canvas -> open the module palette (add), like the patchbay.
    if ( e.pressed && e.button == SDL_BUTTON_RIGHT )
    {
        m_link_out_mod = m_link_out_port = -1;     // cancel any armed jack
        if ( !m_engine ) return true;
        rackx::RackModule* m = module_at( e.x, e.y );
        if ( m )
        {
            // Open the module context menu (Duplicate / Delete) at the pointer.
            m_ctx_mod = m->id;
            m_ctx_x = e.x; m_ctx_y = e.y;
            app.request_redraw();
            return true;
        }
        int cid = 0;
        if ( cable_at( e.x, e.y, cid ) )
        {
            m_engine->removeCable( cid );
            app.request_redraw();
            return true;
        }
        open_palette( app, e.x, e.y );      // empty canvas -> add module
        app.request_redraw();
        return true;
    }

    // left button (press == down-edge or drag-motion; release == pressed==false)
    if ( e.button == SDL_BUTTON_LEFT )
    {
        if ( e.pressed )
            return downEdge ? press_left( app, e.x, e.y )
                            : drag_left ( app, e.x, e.y );
        return release_left( app, e.x, e.y );
    }
    return false;
}

bool RackEditorView::press_left( ui::App& app, int mx, int my )
{
    if ( m_button && m_engine )
    {
        rackx::RackModule* buttonModule = m_engine->moduleById( m_button_mod );
        if ( buttonModule && buttonModule->mod &&
             m_button_param < (int) buttonModule->mod->paramQuantities.size() )
            m_engine->setParam( m_button_mod, m_button_param,
                                buttonModule->mod->paramQuantities[m_button_param].minValue );
    }
    m_move = m_knob = m_button = m_wire = false;      // fresh gesture: clean slate
    m_press_x = mx; m_press_y = my;

    // (0) a TAB button -> switch the module's active page (before knobs/jacks so
    //     the bar always wins over anything drawn beneath it).
    if ( m_engine )
    {
        for ( int i = m_engine->moduleCount() - 1; i >= 0; --i )
        {
            rackx::RackModule* tm = m_engine->moduleAt( i );
            int tabIndex = 0;
            if ( tab_bar_at( tm, mx, my, tabIndex ) )
            {
                m_module_tab[tm->id] = tabIndex;
                m_sel = tm->id;
                app.request_redraw();
                return true;
            }
        }
    }

    // (1) an OUTPUT jack -> arm it (lit) for click-to-connect AND start a drag.
    int mod = 0, port = 0;
    if ( jack_at( mx, my, false, mod, port ) )
    {
        m_link_out_mod = mod; m_link_out_port = port;   // armed source jack
        m_wire = true; m_wire_mod = mod; m_wire_out = port;
        m_wire_x = mx; m_wire_y = my; m_wire_ok = false;
        m_sel = mod;
        app.request_redraw();
        return true;
    }
    // (1b) an INPUT jack -> if an output jack is armed, complete the wire here
    //      (connect on press: robust, no release-timing / click-jitter issue).
    if ( jack_at( mx, my, true, mod, port ) )
    {
        if ( m_engine && m_link_out_mod >= 0 && m_link_out_mod != mod )
            m_engine->addCable( m_link_out_mod, m_link_out_port, mod, port );
        m_link_out_mod = m_link_out_port = -1;          // consume the arm
        m_sel = mod;
        app.request_redraw();
        return true;
    }

    // (2) a KNOB -> drag to change, or double-click to reset to default
    if ( knob_at( mx, my, mod, port ) )
    {
        m_sel = mod;
        rackx::RackModule* rm = m_engine ? m_engine->moduleById( mod ) : nullptr;
        const rackx::ModuleType* type = rm ? rackx::findType( rm->slug ) : nullptr;
        const rackx::PanelElement* element = type
            ? find_element( type->panel.params, port ) : nullptr;
        if ( rm && rm->mod && port < (int) rm->mod->paramQuantities.size() && element )
        {
            const rack::engine::ParamQuantity& q = rm->mod->paramQuantities[port];
            if ( element->style == rackx::PanelControlStyle::Button )
            {
                m_engine->setParam( mod, port, q.maxValue );
                if ( q.name == "Reset" )
                    m_engine->resetModule( mod );
                m_button = true; m_button_mod = mod; m_button_param = port;
                app.request_redraw();
                return true;
            }
            if ( element->style == rackx::PanelControlStyle::Switch ||
                 element->style == rackx::PanelControlStyle::Gate )
            {
                const float midpoint = ( q.minValue + q.maxValue ) * 0.5f;
                const float value = m_engine->getParam( mod, port );
                m_engine->setParam( mod, port, value < midpoint ? q.maxValue : q.minValue );
                app.request_redraw();
                return true;
            }
        }
        Uint32 now = SDL_GetTicks();
        bool dbl = ( now - m_last_click_ms < (Uint32) DBLCLK_MS ) &&
                   m_last_knob_mod == mod && m_last_knob_param == port;
        m_last_click_ms = now; m_last_knob_mod = mod; m_last_knob_param = port;
        if ( dbl )
        {
            rackx::RackModule* rm = m_engine ? m_engine->moduleById( mod ) : nullptr;
            if ( rm && rm->mod && port < (int) rm->mod->paramQuantities.size() )
                m_engine->setParam( mod, port, rm->mod->paramQuantities[port].defaultValue );
            app.request_redraw();
            return true;
        }
        m_knob = true; m_knob_mod = mod; m_knob_param = port;
        m_knob_start = m_engine ? m_engine->getParam( mod, port ) : 0.f;
        m_knob_y0 = my;
        app.request_redraw();
        return true;
    }

    // (3) a module: select it; if the TITLE BAR was hit, begin moving it
    rackx::RackModule* m = module_at( mx, my );
    if ( m )
    {
        m_sel = m->id;
        if ( title_at( m, mx, my ) )
        {
            m_move = true; m_move_id = m->id;
            m_move_dx = mx - ( rect.x + m_ox + (int) std::lround( m->x * m_zoom ) );
            m_move_dy = my - ( rect.y + m_oy + (int) std::lround( m->y * m_zoom ) );
        }
        app.request_redraw();
        return true;
    }

    // (4) empty canvas -> deselect + cancel any armed jack (right-click here
    //     opens the add palette).
    m_sel = -1;
    m_link_out_mod = m_link_out_port = -1;
    app.request_redraw();
    return true;
}

bool RackEditorView::drag_left( ui::App& app, int mx, int my )
{
    if ( m_wire )
    {
        m_wire_x = mx; m_wire_y = my;
        int hm = 0, hp = 0;
        m_wire_ok = jack_at( mx, my, true, hm, hp ) && hm != m_wire_mod;
        if ( m_wire_ok ) { m_hover_mod = hm; m_hover_in = hp; }
        app.request_redraw();
        return true;
    }
    if ( m_knob )
    {
        rackx::RackModule* rm = m_engine ? m_engine->moduleById( m_knob_mod ) : nullptr;
        if ( rm && rm->mod && m_knob_param < (int) rm->mod->paramQuantities.size() )
        {
            const rack::engine::ParamQuantity& q = rm->mod->paramQuantities[m_knob_param];
            float lo = q.minValue, hi = q.maxValue;
            float range = hi - lo;
            if ( range == 0.f ) range = 1.f;
            float nv = m_knob_start + float( m_knob_y0 - my ) * range / float( KNOB_DRAG_PX );
            if ( q.snapEnabled ) nv = std::round( nv );
            float clo = lo < hi ? lo : hi;
            float chi = lo < hi ? hi : lo;
            nv = nv < clo ? clo : ( nv > chi ? chi : nv );
            m_engine->setParam( m_knob_mod, m_knob_param, nv );
        }
        app.request_redraw();
        return true;
    }
    if ( m_move )
    {
        rackx::RackModule* rm = m_engine ? m_engine->moduleById( m_move_id ) : nullptr;
        if ( rm )
        {
            // screen delta -> world delta: divide the un-panned offset by zoom
            // so the module tracks the cursor 1:1 on screen at any zoom.
            float nx = float( mx - rect.x - m_ox - m_move_dx ) / m_zoom;
            float ny = float( my - rect.y - m_oy - m_move_dy ) / m_zoom;
            m_engine->moveModule( m_move_id, nx, ny );
        }
        app.request_redraw();
        return true;
    }
    return true;
}

bool RackEditorView::release_left( ui::App& app, int mx, int my )
{
    if ( m_wire )
    {
        m_wire = false;
        m_wire_ok = false;
        int hm = 0, hp = 0;
        const int dx = mx - m_press_x, dy = my - m_press_y;
        const int moved = ( dx < 0 ? -dx : dx ) + ( dy < 0 ? -dy : dy );
        if ( m_engine && jack_at( mx, my, true, hm, hp ) )
        {
            m_engine->addCable( m_wire_mod, m_wire_out, hm, hp );  // drag completed
            m_link_out_mod = m_link_out_port = -1;                 // arm consumed
        }
        else if ( moved > 4 )
        {
            m_link_out_mod = m_link_out_port = -1;   // dragged to empty: cancel arm
        }
        // else: a CLICK on the output jack -> leave it ARMED (lit) for the next
        // input-jack click to complete the wire.
        app.request_redraw();
        return true;
    }
    if ( m_button )
    {
        if ( m_engine )
        {
            rackx::RackModule* buttonModule = m_engine->moduleById( m_button_mod );
            if ( buttonModule && buttonModule->mod &&
                 m_button_param < (int) buttonModule->mod->paramQuantities.size() )
                m_engine->setParam( m_button_mod, m_button_param,
                                    buttonModule->mod->paramQuantities[m_button_param].minValue );
        }
        m_button = false;
        app.request_redraw();
        return true;
    }
    if ( m_knob ) { m_knob = false; app.request_redraw(); return true; }
    if ( m_move ) { m_move = false; app.request_redraw(); return true; }
    return false;
}

bool RackEditorView::on_wheel( ui::App& app, int dx, int dy )
{
    m_cw = app.mono.cw();
    m_ch = app.mono.ch();

    if ( m_pal )                                   // scroll the palette list
    {
        if ( m_pal_type_menu )
        {
            m_pal_type_scroll -= dy;
            int maxscroll = (int) palette_types().size() + 1 - PAL_ROWS;
            if ( maxscroll < 0 ) maxscroll = 0;
            if ( m_pal_type_scroll > maxscroll ) m_pal_type_scroll = maxscroll;
            if ( m_pal_type_scroll < 0 )         m_pal_type_scroll = 0;
        }
        else
        {
            m_pal_scroll -= dy;
            int maxscroll = ( (int) filtered().size() + 1 ) / 2 - PAL_ROWS; if ( maxscroll < 0 ) maxscroll = 0;
            if ( m_pal_scroll > maxscroll ) m_pal_scroll = maxscroll;
            if ( m_pal_scroll < 0 )         m_pal_scroll = 0;
        }
        app.request_redraw();
        return true;
    }

    // Horizontal wheel always pans X.  Vertical wheel: with Shift/Ctrl held it
    // PANS (Shift -> horizontal, Ctrl -> vertical, as the old wheel did); a
    // PLAIN vertical wheel ZOOMS, anchored so the world point under the cursor
    // stays fixed on screen.
    SDL_Keymod mod = SDL_GetModState();
    if ( dx != 0 ) m_ox += dx * PAN_STEP;
    if ( dy != 0 )
    {
        if ( mod & ( KMOD_SHIFT | KMOD_CTRL ) )
        {
            // ternaries (not two one-line ifs) to avoid -Wmisleading-indentation
            m_ox += ( mod & KMOD_SHIFT ) ? dy * PAN_STEP : 0;
            m_oy += ( mod & KMOD_SHIFT ) ? 0 : dy * PAN_STEP;
        }
        else
        {
            int mx = 0, my = 0;
            SDL_GetMouseState( &mx, &my );
            float wx = float( mx - rect.x - m_ox ) / m_zoom;   // world pt under
            float wy = float( my - rect.y - m_oy ) / m_zoom;   // the cursor
            float nz = m_zoom * ( dy > 0 ? 1.15f : ( 1.0f / 1.15f ) );
            m_zoom = nz < 0.5f ? 0.5f : ( nz > 2.5f ? 2.5f : nz );
            m_ox = mx - rect.x - (int) std::lround( wx * m_zoom );
            m_oy = my - rect.y - (int) std::lround( wy * m_zoom );
        }
    }
    app.request_redraw();
    return true;
}

bool RackEditorView::on_key( ui::App& app, SDL_Keycode k )
{
    if ( app.editing_text() ) return false;        // palette search owns keys

    if ( m_pal && k == SDLK_ESCAPE ) { close_palette( app ); return true; }
    if ( m_ctx_mod >= 0 && k == SDLK_ESCAPE ) { m_ctx_mod = -1; app.request_redraw(); return true; }

    if ( ( k == SDLK_DELETE || k == SDLK_BACKSPACE ) && m_sel >= 0 && m_engine )
    {
        m_engine->removeModule( m_sel );
        m_sel = -1;
        app.request_redraw();
        return true;
    }
    return false;
}

} // namespace rackui
