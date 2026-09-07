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
    // Submit the complete disc in one renderer call.  The old implementation
    // issued one DrawLine call per scanline, which made dense modules (and in
    // particular oscillator banks) spend most of their GPU time in command
    // submission rather than rasterisation.
    static thread_local std::vector<SDL_Rect> rows;
    rows.clear();
    rows.reserve( size_t( rad * 2 + 1 ) );
    for ( int dy = -rad; dy <= rad; ++dy )
    {
        int dx = int( std::floor( std::sqrt( double( rad * rad - dy * dy ) ) ) );
        rows.push_back( SDL_Rect { cx - dx, cy + dy, dx * 2 + 1, 1 } );
    }
    SDL_RenderFillRects( r, rows.data(), int( rows.size() ) );
}

// ---- circle outline (midpoint), drawn at rad and rad-1 for ~2px ------------
void circle_ring( SDL_Renderer* r, int cx, int cy, int rad, Color c )
{
    ui::set_color( r, c );
    // A ring used to submit eight DrawPoint calls for every midpoint step.
    // Accumulate both radii and hand them to SDL as a single batch instead.
    static thread_local std::vector<SDL_Point> points;
    points.clear();
    points.reserve( size_t( std::max( 1, rad ) * 24 ) );
    for ( int rr = rad; rr >= rad - 1 && rr > 0; --rr )
    {
        int x = rr, y = 0, err = 1 - rr;
        while ( x >= y )
        {
            points.push_back( SDL_Point { cx + x, cy + y } );
            points.push_back( SDL_Point { cx + y, cy + x } );
            points.push_back( SDL_Point { cx - y, cy + x } );
            points.push_back( SDL_Point { cx - x, cy + y } );
            points.push_back( SDL_Point { cx - x, cy - y } );
            points.push_back( SDL_Point { cx - y, cy - x } );
            points.push_back( SDL_Point { cx + y, cy - x } );
            points.push_back( SDL_Point { cx + x, cy - y } );
            ++y;
            if ( err < 0 ) err += 2 * y + 1;
            else { --x; err += 2 * ( y - x ) + 1; }
        }
    }
    if ( !points.empty() )
        SDL_RenderDrawPoints( r, points.data(), int( points.size() ) );
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

// Stable golden-ratio hue assignment: adjacent cable IDs remain visually
// distinct while saturation/value stay in the readable mid-to-bright range.
Color cable_color( int id )
{
    float h = std::fmod( float( id < 0 ? -id : id ) * 0.61803398875f, 1.f );
    const float s = 0.72f;
    const float v = 0.94f;
    const float sector = h * 6.f;
    const int i = (int) std::floor( sector );
    const float f = sector - i;
    const float p = v * ( 1.f - s );
    const float q = v * ( 1.f - s * f );
    const float t = v * ( 1.f - s * ( 1.f - f ) );
    float r = v, g = t, b = p;
    switch ( i % 6 )
    {
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default: break;
    }
    return Color{ (Uint8)( r * 255.f ), (Uint8)( g * 255.f ),
                  (Uint8)( b * 255.f ), 255 };
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
            Color{0, 0, 0, 255}, Color{20, 20, 20, 255},   // rack canvas is BLACK in both themes
            Color{31, 31, 31, 255}, Color{19, 19, 19, 255},
            Color{48, 48, 48, 255}, Color{90, 90, 90, 255}, Color{176, 176, 176, 255},
            Color{238, 238, 238, 255}, Color{150, 150, 150, 255},
            Color{7, 7, 7, 255}, Color{68, 68, 68, 255}, Color{244, 244, 244, 255},
            Color{3, 3, 3, 255}, Color{102, 102, 102, 255}, Color{210, 210, 210, 255},
            Color{255, 255, 255, 255}
        };
    }
    return RackColors{
        Color{0, 0, 0, 255}, Color{20, 20, 20, 255},   // rack canvas is BLACK even in light mode
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
    if ( !m_mobile_zoom_initialized )
    {
        m_zoom = ui::platform::rack_default_zoom();
        m_mobile_zoom_initialized = true;
    }
    rackx::registerRackExtModules();     // populate the palette lazily (idempotent)
    m_sel = -1;
    m_move = m_knob = m_button = m_wire = false;
}

// ============================================================================
//  Layout -- one place computes every module's metrics, so drawing and
//  hit-testing agree to the pixel.
// ============================================================================
const rackx::PanelSpec* RackEditorView::panel_of( rackx::RackModule* m ) const
{
    // A scripting module (Pd/Csound) carries its OWN per-instance panel; the rack
    // draws from it, so the panel editor's drag/resize shows live here (Reaktor-style)
    // and the layout persists with the project.
    if ( m && m->panel.valid() ) return &m->panel;
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

bool RackEditorView::port_tab_visible( rackx::RackModule* m, bool isInput, int port ) const
{
    if ( !m ) return false;
    const rackx::PanelSpec* panel = panel_of( m );
    if ( !panel || panel->tabs.empty() ) return true;
    const std::vector<rackx::PanelElement>& elements = isInput
        ? panel->inputs : panel->outputs;
    return tab_visible( find_element( elements, port ), active_tab( m ) );
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
        const float pw = L.panel->width  > 0.f ? L.panel->width  : 1.f;
        const float ph = L.panel->height > 0.f ? L.panel->height : 1.f;
        cx = ox + (int) std::lround( element->x / pw * L.w );   // design units -> actual box
        cy = oy + (int) std::lround( element->y / ph * L.h );
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
        const float pw = L.panel->width  > 0.f ? L.panel->width  : 1.f;
        const float ph = L.panel->height > 0.f ? L.panel->height : 1.f;
        cx = ox + (int) std::lround( element->x / pw * L.w );   // design units -> actual box
        cy = oy + (int) std::lround( element->y / ph * L.h );
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
        // Map the manifest position (design units) onto the ACTUAL drawn box so the
        // plug tracks the faceplate texture even when its aspect != panel w/h -- a
        // textured panel sets L.w = L.h*aspect, which diverges from zpx(panel width)
        // and otherwise slides every jack horizontally off its printed socket.
        const float pw = L.panel->width  > 0.f ? L.panel->width  : 1.f;
        const float ph = L.panel->height > 0.f ? L.panel->height : 1.f;
        cx = ox + (int) std::lround( element->x / pw * L.w );
        cy = oy + (int) std::lround( element->y / ph * L.h );
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
        // A sharp overlaps the top of the two naturals it sits between, and the
        // naturals are drawn first (lower param index) so the sharps paint on
        // top.  Hit-testing therefore has to run the other way round: hold a
        // natural aside and let an overlapping sharp win it.
        int naturalKey = -1;
        for ( int p = 0; p < L.nP; ++p )
        {
            int cx, cy, r;
            if ( !knob_pos( m, p, cx, cy, r ) ) continue;
            const rackx::PanelElement* element = L.panel
                ? find_element( L.panel->params, p ) : nullptr;
            if ( L.panel && !L.panel->tabs.empty() &&
                 !tab_visible( element, active_tab( m ) ) ) continue;
            bool hit = near_pt(sx,sy,cx,cy,ui::platform::rack_knob_hit_radius(r));
            if ( element && element->style == rackx::PanelControlStyle::Curve )
            {
                const int halfW = imax( 20, zpx( element->width  > 0.f ? element->width  : 120.f ) / 2 );
                const int halfH = imax( 12, zpx( element->height > 0.f ? element->height :  60.f ) / 2 );
                hit = std::abs( sx - cx ) <= halfW && std::abs( sy - cy ) <= halfH;
            }
            else if ( element && element->style == rackx::PanelControlStyle::Slider )
            {
                const int halfW = element->width > 0.f ? imax( 3, zpx( element->width ) / 2 ) : r;
                const int halfH = element->height > 0.f ? imax( 12, zpx( element->height ) / 2 ) : r * 3;
                hit = std::abs( sx - cx ) <= halfW + 3 && std::abs( sy - cy ) <= halfH + 3;
            }
            else if ( element && element->style == rackx::PanelControlStyle::KnobCV )
            {
                // Concentric: the outer annulus edits the CV depth, the inner
                // disc the base value.  Report the ring's param so the generic
                // drag path drives it without knowing about concentric knobs.
                const int rr = ui::platform::rack_knob_hit_radius( r );
                const int dx = sx - cx, dy = sy - cy;
                const int d2 = dx * dx + dy * dy;
                const int inner = imax( 2, r - 4 );
                if ( d2 <= rr * rr )
                {
                    hit = true;
                    if ( d2 > inner * inner && element->cvParamId >= 0 )
                    {
                        modId = m->id; paramIdx = element->cvParamId; return true;
                    }
                }
                else hit = false;
            }
            else if ( element && ( element->style == rackx::PanelControlStyle::Switch ||
                                   element->style == rackx::PanelControlStyle::Gate ) )
                hit = std::abs( sx - cx ) <= r + 2 && std::abs( sy - cy ) <= r + 2;
            else if ( element && ( element->style == rackx::PanelControlStyle::SegmentDisplay ||
                                   element->style == rackx::PanelControlStyle::StepPad ) )
            {
                const int halfW = element->width > 0.f ? imax( 5, zpx( element->width ) / 2 ) : r + 2;
                const int halfH = element->height > 0.f ? imax( 4, zpx( element->height ) / 2 ) : r + 2;
                hit = std::abs( sx - cx ) <= halfW + 2 && std::abs( sy - cy ) <= halfH + 2;
            }
            else if ( element && element->style == rackx::PanelControlStyle::PianoKey )
            {
                // Exact key rectangle, no slop: naturals and sharps interleave,
                // so a generous pad would make the sharps unpickable.
                const int halfW = imax( 2, zpx( element->width  > 0.f ? element->width  : 12.f ) / 2 );
                const int halfH = imax( 4, zpx( element->height > 0.f ? element->height : 48.f ) / 2 );
                hit = std::abs( sx - cx ) <= halfW && std::abs( sy - cy ) <= halfH;
            }
            else if ( element && element->style == rackx::PanelControlStyle::Lamp )
                hit = false;                          // indicator only, never grabs
            if ( hit )
            {
                if ( element && element->style == rackx::PanelControlStyle::PianoKey &&
                     element->widget != "black" )
                {
                    if ( naturalKey < 0 ) naturalKey = p;
                    continue;                         // a sharp on top still wins
                }
                modId = m->id; paramIdx = p; return true;
            }
        }
        if ( naturalKey >= 0 ) { modId = m->id; paramIdx = naturalKey; return true; }
    }
    return false;
}

bool RackEditorView::jack_at( int sx, int sy, bool wantInput, int& modId, int& portIdx ) const
{
    if ( !m_engine ) return false;
    // Nearest jack within a generous hit radius (so it never mis-picks a
    // neighbour).  Scale it with zoom, but never below the base HIT_R, so that
    // zooming OUT can never make the (now tiny) jacks unclickable.
    const int hitR=ui::platform::rack_jack_hit_radius(zpx(HIT_R),HIT_R);
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
    double best=ui::platform::rack_cable_hit_radius(zpx(HIT_R),HIT_R);
    bool   got  = false;
    for ( int i = 0; i < m_engine->cableCount(); ++i )
    {
        rackx::RackCable* c = m_engine->cableAt( i );
        if ( !c ) continue;
        rackx::RackModule* a = m_engine->moduleById( c->fromMod );
        rackx::RackModule* b = m_engine->moduleById( c->toMod );
        if ( !a || !b ) continue;
        if ( !port_tab_visible( a, false, c->outPort ) ||
             !port_tab_visible( b, true, c->inPort ) ) continue;
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
    {
    ui::ScopedClip clipScope(r,clip);

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
        ui::mouse_logical( app, mouseX, mouseY );   // logical, not window px
        const bool retest=mouseX!=m_hover_test_x||mouseY!=m_hover_test_y||
            m_hover_test_cables!=m_engine->cableCount()||
            m_hover_test_modules!=m_engine->moduleCount()||m_move||m_wire;
        if(retest)
        {
            m_hover_test_x=mouseX;m_hover_test_y=mouseY;
            m_hover_test_cables=m_engine->cableCount();
            m_hover_test_modules=m_engine->moduleCount();
            m_hover_cable = in_rect(rect,mouseX,mouseY)&&cable_at(mouseX,mouseY,cableId)
                ? cableId : -1;
            m_hover_cable_endpoints = false;
            if(m_hover_cable>=0)
            {
                for(int index=0;index<m_engine->cableCount();++index) {
                    rackx::RackCable* hovered=m_engine->cableAt(index);
                    if(!hovered||hovered->id!=m_hover_cable)continue;
                    rackx::RackModule* hoverOut=m_engine->moduleById(hovered->fromMod);
                    rackx::RackModule* hoverIn=m_engine->moduleById(hovered->toMod);
                    int radius=0;
                    m_hover_cable_endpoints=hoverOut&&hoverIn&&
                        jack_pos(hoverOut,false,hovered->outPort,m_hover_out_x,m_hover_out_y,radius)&&
                        jack_pos(hoverIn,true,hovered->inPort,m_hover_in_x,m_hover_in_y,radius);
                    break;
                }
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

    }

    // draggable H/V scroll bars for navigating large patches
    { int cl, cr, ct, cb; content_bounds( cl, cr, ct, cb );
      m_scroll.draw( app, rect, cl, cr, ct, cb, m_ox, m_oy ); }

    if ( m_pal ) draw_palette( app );
    draw_ctx_menu( app );

    // Standalone rack-patch file actions (the same rack is still embedded in
    // the project file). Keep these above the canvas and below modal overlays.
    if(!m_pal&&m_ctx_mod<0){
        const RackColors colors=rack_colors(); const int h=m_ch+10;
        SDL_Rect b[3]={{rect.x+5,rect.y+5,58,h},{rect.x+67,rect.y+5,58,h},
                       {rect.x+129,rect.y+5,78,h}};
        const char* l[3]={"OPEN","SAVE","SAVE AS"};
        for(int i=0;i<3;++i){ui::fill_rect(app.ren,b[i],colors.header);
            ui::frame_rect(app.ren,b[i],colors.edgeStrong);
            app.mono.draw_centered(app.ren,b[i],l[i],colors.text);}
    }
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

            // Fold cables from inactive pages into the tab itself. Each small
            // colored segment represents one connection owned by that page;
            // selecting the page unfolds those cables into the rack canvas.
            int marker = 0;
            for ( int ci = 0; m_engine && ci < m_engine->cableCount(); ++ci )
            {
                rackx::RackCable* cable = m_engine->cableAt( ci );
                if ( !cable ) continue;
                int endpointTab = -2;
                if ( cable->fromMod == m->id )
                {
                    const rackx::PanelElement* endpoint = find_element( L.panel->outputs,
                                                                         cable->outPort );
                    endpointTab = endpoint ? endpoint->tab : -1;
                }
                if ( cable->toMod == m->id )
                {
                    const rackx::PanelElement* endpoint = find_element( L.panel->inputs,
                                                                         cable->inPort );
                    endpointTab = endpoint ? endpoint->tab : -1;
                }
                if ( endpointTab != t ) continue;
                const int markerW = imax( 3, zpx( 5.f ) );
                const int markerGap = imax( 1, zpx( 2.f ) );
                const int mx = tabR.x + zpx( 4.f ) + marker * ( markerW + markerGap );
                if ( mx + markerW >= tabR.x + tabR.w - zpx( 3.f ) ) break;
                SDL_Rect cableMark { mx, tabR.y + tabR.h - imax( 2, zpx( 3.f ) ),
                                     markerW, imax( 1, zpx( 2.f ) ) };
                ui::fill_rect( r, cableMark, cable_color( cable->id ) );
                ++marker;
            }
        }
    }

    // Decoration first: Section frames group the controls drawn over them.
    if ( L.panel && !textured && L.panel->width > 0.f && L.panel->height > 0.f )
        for ( const rackx::PanelElement& d : L.panel->decor )
        {
            if ( tabbed && !tab_visible( &d, active ) ) continue;
            if ( d.style != rackx::PanelControlStyle::Section ) continue;
            // Same design-units -> box mapping knob_pos() uses.
            const float pw = L.panel->width, ph = L.panel->height;
            const int dx = box.x + (int) std::lround( d.x / pw * L.w );
            const int dy = box.y + (int) std::lround( d.y / ph * L.h );
            const int dw = imax( 4, (int) std::lround( d.width  / pw * L.w ) );
            const int dh = imax( 4, (int) std::lround( d.height / ph * L.h ) );
            SDL_Rect frame { dx - dw / 2, dy - dh / 2, dw, dh };
            ui::frame_rect( r, frame, colors.edge );
            if ( !d.label.empty() )
            {
                const int th = zpx( m_ch );
                const std::string txt = clip_text_scaled( app.mono, d.label,
                                                          dw - zpx( 16 ), m_zoom );
                if ( !txt.empty() )
                {
                    const int tw = app.mono.text_w_scaled( txt, m_zoom );
                    // Break the frame line so the caption sits in the gap, the
                    // way a silk-screened section label does.
                    SDL_Rect gap { frame.x + zpx( 8 ) - zpx( 3 ), frame.y - th / 2,
                                   tw + zpx( 6 ), th };
                    ui::fill_rect( r, gap, colors.panel );
                    app.mono.draw_scaled( r, frame.x + zpx( 8 ), frame.y - th / 2,
                                          txt, colors.text, m_zoom );
                }
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
        if ( m_engine && m_engine->isScriptModule( m->id ) )
        {
            // Pd/Csound channel labels use the same dark, bright-text segment
            // display treatment as the Audio In/Out channel readout.
            const int padX = zpx( 4.f ), padY = zpx( 2.f );
            SDL_Rect disp { x - padX, y - padY,
                            textW + 2 * padX, textH + 2 * padY };
            ui::fill_rect( rn, disp, colors.well );
            ui::frame_rect( rn, disp, colors.edgeStrong );
            app.mono.draw_scaled( rn, x, y, text, colors.selection, textScale );
        }
        else
            app.mono.draw_scaled( rn, x, y, text, colors.text, textScale );
    };

    // ---- generic breakpoint-curve editor (rackx::ICurveSource) --------------
    // Reusable: any module exposing ICurveSource gets this widget by declaring a
    // PanelElement with style == Curve whose `id` is the CURVE INDEX.  The shape
    // is sampled through the module's own evaluator, so what is drawn is exactly
    // what the DSP plays rather than a second interpolation that can disagree.
    if ( element && element->style == rackx::PanelControlStyle::Curve )
    {
        const int halfW = imax( 20, zpx( element->width  > 0.f ? element->width  : 120.f ) / 2 );
        const int halfH = imax( 12, zpx( element->height > 0.f ? element->height :  60.f ) / 2 );
        SDL_Rect box { cx - halfW, cy - halfH, 2 * halfW + 1, 2 * halfH + 1 };
        ui::fill_rect( rn, box, colors.well );
        ui::frame_rect( rn, box, colors.edgeStrong );

        rackx::ICurveSource* src =
            m->mod ? dynamic_cast<rackx::ICurveSource*>( m->mod.get() ) : nullptr;
        const int ci = element->curveIndex;
        if ( !src || ci < 0 || ci >= src->curveCount() ) return;
        rackx::CurveInfo info;
        src->curveGetInfo( ci, info );

        auto cl01 = []( float f ) { return f < 0.f ? 0.f : ( f > 1.f ? 1.f : f ); };
        auto px = [&]( float t ) { return box.x + (int) std::lround( cl01( t ) * ( box.w - 1 ) ); };
        auto py = [&]( float v ) { return box.y + box.h - 1
                                        - (int) std::lround( cl01( v ) * ( box.h - 1 ) ); };

        if ( info.gridStep > 0.0005f )                 // musical grid
        {
            SDL_SetRenderDrawColor( rn, colors.panel.r, colors.panel.g, colors.panel.b, 255 );
            for ( float g = 0.f; g <= 1.0001f; g += info.gridStep )
                SDL_RenderDrawLine( rn, px( g ), box.y + 1, px( g ), box.y + box.h - 2 );
        }
        if ( info.loopOn )                             // loop span
        {
            const int lx = px( info.loopStart ), rx = px( info.loopEnd );
            SDL_Rect lp { lx, box.y + 1, imax( 1, rx - lx ), box.h - 2 };
            SDL_SetRenderDrawBlendMode( rn, SDL_BLENDMODE_BLEND );
            SDL_SetRenderDrawColor( rn, colors.selection.r, colors.selection.g,
                                    colors.selection.b, 46 );
            SDL_RenderFillRect( rn, &lp );
        }
        SDL_SetRenderDrawColor( rn, colors.marker.r, colors.marker.g,   // gate window
                                colors.marker.b, 255 );
        SDL_RenderDrawLine( rn, px( info.gateStart ), box.y + 1,
                                px( info.gateStart ), box.y + box.h - 2 );
        SDL_RenderDrawLine( rn, px( info.gateEnd ),   box.y + 1,
                                px( info.gateEnd ),   box.y + box.h - 2 );

        const int steps = imax( 16, box.w - 2 );       // the curve
        SDL_SetRenderDrawColor( rn, colors.text.r, colors.text.g, colors.text.b, 255 );
        int prevX = px( 0.f ), prevY = py( src->curveValueAt( ci, 0.f ) );
        for ( int s = 1; s <= steps; ++s )
        {
            const float t = (float) s / (float) steps;
            const int X = px( t ), Y = py( src->curveValueAt( ci, t ) );
            SDL_RenderDrawLine( rn, prevX, prevY, X, Y );
            prevX = X; prevY = Y;
        }

        const int nPoints = src->curvePointCount( ci );

        // Segment bend handles: a small marker at each segment's midpoint ON the
        // curve.  Dragging one vertically bends that segment -- this is what
        // replaces a "curve amount" knob.
        for ( int i = 0; i + 1 < nPoints; ++i )
        {
            rackx::CurvePoint a, b;
            if ( !src->curveGetPoint( ci, i, a ) || !src->curveGetPoint( ci, i + 1, b ) )
                continue;
            if ( std::fabs( b.v - a.v ) < 1e-4f ) continue;   // flat: nothing to bend
            const float mt = ( a.t + b.t ) * 0.5f;
            const int hx = px( mt ), hy = py( src->curveValueAt( ci, mt ) );
            SDL_Rect hbox { hx - 2, hy - 2, 5, 5 };
            ui::fill_rect( rn, hbox, colors.muted );
        }

        for ( int i = 0; i < nPoints; ++i )               // breakpoints
        {
            rackx::CurvePoint p;
            if ( !src->curveGetPoint( ci, i, p ) ) continue;
            const int hr = ( i == info.selected ) ? 4 : 3;
            SDL_Rect h { px( p.t ) - hr, py( p.v ) - hr, 2 * hr + 1, 2 * hr + 1 };
            ui::fill_rect( rn, h, i == info.selected ? colors.selection : colors.marker );
            ui::frame_rect( rn, h, colors.edgeStrong );
        }

        // Step-number ruler along the top: bar lines get "1", "2"..., and beats
        // inside a bar get ".2" ".3" ".4" while there is room for them.
        if ( info.spanBars > 0.f && info.beatsPerBar > 0 )
        {
            const int totalBeats = imax( 1, (int) std::lround( info.spanBars
                                                    * (float) info.beatsPerBar ) );
            const float tickScale = m_zoom * 0.75f;
            const int labelW = app.mono.text_w_scaled( "8.4", tickScale );
            const bool room = ( box.w / imax( 1, totalBeats ) ) > labelW + 3;
            SDL_SetRenderDrawColor( rn, colors.muted.r, colors.muted.g, colors.muted.b, 255 );
            for ( int bt = 0; bt < totalBeats; ++bt )
            {
                const float t = (float) bt / (float) totalBeats;
                const bool onBar = ( bt % info.beatsPerBar ) == 0;
                const int lx = px( t );
                SDL_RenderDrawLine( rn, lx, box.y + 1, lx, box.y + ( onBar ? 7 : 4 ) );
                if ( !onBar && !room ) continue;
                char lab[16];
                if ( onBar ) std::snprintf( lab, sizeof lab, "%d",
                                            bt / info.beatsPerBar + 1 );
                else         std::snprintf( lab, sizeof lab, ".%d",
                                            bt % info.beatsPerBar + 1 );
                app.mono.draw_scaled( rn, lx + 2, box.y + 2, lab,
                                      onBar ? colors.text : colors.muted, tickScale );
            }
        }
        if ( info.playhead >= 0.f )                    // playhead
        {
            SDL_SetRenderDrawColor( rn, colors.selection.r, colors.selection.g,
                                    colors.selection.b, 255 );
            SDL_RenderDrawLine( rn, px( info.playhead ), box.y + 1,
                                    px( info.playhead ), box.y + box.h - 2 );
        }
        return;
    }

    // ---- inline sample display (rackx::ISampleSlot) -------------------------
    if ( element && element->style == rackx::PanelControlStyle::Waveform )
    {
        const int halfW = imax( 20, zpx( element->width  > 0.f ? element->width  : 120.f ) / 2 );
        const int halfH = imax( 12, zpx( element->height > 0.f ? element->height :  60.f ) / 2 );
        SDL_Rect box { cx - halfW, cy - halfH, 2 * halfW + 1, 2 * halfH + 1 };
        ui::fill_rect( rn, box, colors.well );
        ui::frame_rect( rn, box, colors.edgeStrong );

        rackx::ISampleSlot* slot =
            m->mod ? dynamic_cast<rackx::ISampleSlot*>( m->mod.get() ) : nullptr;
        const int frames = slot ? slot->sampleFrames() : 0;
        if ( !slot || frames <= 0 )
        {
            const std::string msg = clip_text_scaled( app.mono,
                "right-click -> Edit Sample", box.w - zpx( 12 ), m_zoom );
            if ( !msg.empty() )
                app.mono.draw_scaled( rn, box.x + zpx( 6 ),
                                      box.y + box.h / 2 - zpx( m_ch ) / 2,
                                      msg, colors.muted, m_zoom );
            return;
        }

        const int buckets = imax( 8, box.w - 2 );
        WaveCache& wc = m_waveCache[m->id];
        if ( wc.frames != frames || wc.buckets != buckets )
        {
            wc.mn.assign( (size_t)buckets, 0.f );
            wc.mx.assign( (size_t)buckets, 0.f );
            slot->samplePeaks( 0, frames, wc.mn.data(), wc.mx.data(), buckets );
            wc.frames = frames; wc.buckets = buckets;
        }

        const int midY = box.y + box.h / 2;
        const int half = box.h / 2 - 2;
        SDL_SetRenderDrawColor( rn, colors.muted.r, colors.muted.g, colors.muted.b, 255 );
        SDL_RenderDrawLine( rn, box.x + 1, midY, box.x + box.w - 2, midY );
        SDL_SetRenderDrawColor( rn, colors.text.r, colors.text.g, colors.text.b, 255 );
        for ( int i = 0; i < buckets && i < box.w - 2; ++i )
            SDL_RenderDrawLine( rn, box.x + 1 + i,
                                midY - (int) std::lround( wc.mx[(size_t)i] * half ),
                                box.x + 1 + i,
                                midY - (int) std::lround( wc.mn[(size_t)i] * half ) );

        auto mx_of = [&]( float n ) {
            return box.x + 1 + (int) std::lround( n * (float)( box.w - 3 ) );
        };
        // dim what falls outside the playback region
        const int sxp = mx_of( slot->sampleMarker( PatchKnob::engine::SM_START ) );
        const int exp_ = mx_of( slot->sampleMarker( PatchKnob::engine::SM_END ) );
        SDL_SetRenderDrawBlendMode( rn, SDL_BLENDMODE_BLEND );
        SDL_SetRenderDrawColor( rn, colors.well.r, colors.well.g, colors.well.b, 175 );
        if ( sxp > box.x + 1 ) { SDL_Rect a{ box.x + 1, box.y + 1, sxp - box.x - 1, box.h - 2 };
                                 SDL_RenderFillRect( rn, &a ); }
        if ( exp_ < box.x + box.w - 2 ) { SDL_Rect b{ exp_, box.y + 1,
                                                      box.x + box.w - 1 - exp_, box.h - 2 };
                                          SDL_RenderFillRect( rn, &b ); }
        if ( slot->sampleMarkerActive( PatchKnob::engine::SM_LOOP_START ) )
        {
            const int l0 = mx_of( slot->sampleMarker( PatchKnob::engine::SM_LOOP_START ) );
            const int l1 = mx_of( slot->sampleMarker( PatchKnob::engine::SM_LOOP_END ) );
            SDL_SetRenderDrawColor( rn, colors.selection.r, colors.selection.g,
                                    colors.selection.b, 46 );
            SDL_Rect lp{ l0, box.y + 1, imax( 1, l1 - l0 ), box.h - 2 };
            SDL_RenderFillRect( rn, &lp );
        }
        SDL_SetRenderDrawColor( rn, colors.marker.r, colors.marker.g, colors.marker.b, 255 );
        SDL_RenderDrawLine( rn, sxp,  box.y + 1, sxp,  box.y + box.h - 2 );
        SDL_RenderDrawLine( rn, exp_, box.y + 1, exp_, box.y + box.h - 2 );

        const std::string nm = clip_text_scaled( app.mono, slot->sampleName(),
                                                 box.w - zpx( 10 ), m_zoom );
        if ( !nm.empty() )
            app.mono.draw_scaled( rn, box.x + zpx( 5 ), box.y + zpx( 3 ),
                                  nm, colors.selection, m_zoom );
        return;
    }

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

    // Piano key: the one-octave note-entry keyboard on the acid sequencer.
    // Naturals are tall and pale; sharps are shorter, narrower and dark, and are
    // drawn after the naturals so they sit on top of them.
    if ( element && element->style == rackx::PanelControlStyle::PianoKey )
    {
        const bool black = element->widget == "black";
        const int kw = imax( 3, zpx( element->width  > 0.f ? element->width  : 12.f ) );
        const int kh = imax( 8, zpx( element->height > 0.f ? element->height : 48.f ) );
        SDL_Rect key { cx - kw / 2, cy - kh / 2, kw, kh };
        const bool held = norm >= 0.5f;

        // A sharp must be BLACK.  This drew them in edgeStrong -- a light grey
        // (176,176,176) -- so the sharps came out paler than the naturals were
        // dark, and the whole keyboard read as undifferentiated mush.  The 303
        // faceplate uses near-black (#0a0a0a) against light keys; match it.
        const Color ivory  { 232, 230, 225, 255 };
        const Color ebony  {  10,  10,  10, 255 };
        const Color litW   = colors.selection;
        const Color litB   = colors.marker;
        ui::fill_rect( rn, key, held ? ( black ? litB : litW )
                                     : ( black ? ebony : ivory ) );
        ui::frame_rect( rn, key, colors.canvas );

        if ( !black )
        {
            // Naturals get a front edge and a soft shadow down their left side,
            // which is what separates adjacent white keys from one slab.
            const int lip = imax( 1, zpx( 3.f ) );
            ui::fill_rect( rn, SDL_Rect{ key.x, key.y + key.h - lip, key.w, lip },
                           colors.muted );
            ui::fill_rect( rn, SDL_Rect{ key.x, key.y, imax( 1, zpx( 1.f ) ), key.h },
                           colors.canvas );
        }
        else
        {
            // A highlight along the sharp's top edge lifts it off the naturals
            // it overlaps, so it reads as sitting ON them rather than cut into.
            ui::fill_rect( rn, SDL_Rect{ key.x + 1, key.y + 1,
                                         key.w - 2, imax( 1, zpx( 1.f ) ) },
                           colors.edge );
        }

        if ( !element->label.empty() && !black )
        {
            const std::string txt = clip_text_scaled( app.mono, element->label,
                                                      kw - zpx( 2 ), textScale );
            if ( !txt.empty() )
                app.mono.draw_scaled( rn, cx - app.mono.text_w_scaled( txt, textScale ) / 2,
                                      key.y + key.h - textH - zpx( 6 ), txt,
                                      ebony, textScale );   // was panel either way
        }
        return;
    }

    // Step pad: the lit rectangular buttons of the step grid.  Bigger and more
    // legible than Gate, with its caption inside the pad.
    if ( element && element->style == rackx::PanelControlStyle::StepPad )
    {
        const bool on = norm >= 0.5f;
        const int pw = imax( 6, zpx( element->width  > 0.f ? element->width  : 2.f * element->radius ) );
        const int ph = imax( 6, zpx( element->height > 0.f ? element->height : 2.f * element->radius ) );
        SDL_Rect pad { cx - pw / 2, cy - ph / 2, pw, ph };
        ui::fill_rect( rn, pad, colors.well );
        if ( on )
            ui::fill_rect( rn, SDL_Rect{ pad.x + imax( 1, zpx( 2 ) ), pad.y + imax( 1, zpx( 2 ) ),
                                         pad.w - 2 * imax( 1, zpx( 2 ) ),
                                         pad.h - 2 * imax( 1, zpx( 2 ) ) }, colors.marker );
        ui::frame_rect( rn, pad, colors.edgeStrong );
        if ( !element->label.empty() )
        {
            const std::string txt = clip_text_scaled( app.mono, element->label,
                                                      pw - zpx( 4 ), textScale );
            if ( !txt.empty() )
                app.mono.draw_scaled( rn, cx - app.mono.text_w_scaled( txt, textScale ) / 2,
                                      cy - textH / 2, txt,
                                      on ? colors.panel : colors.text, textScale );
        }
        return;
    }

    // Lamp: a read-only round indicator (the param carries the brightness).
    if ( element && element->style == rackx::PanelControlStyle::Lamp )
    {
        fill_disc( rn, cx, cy, r + 1, colors.well );
        if ( norm > 0.02f ) fill_disc( rn, cx, cy, r, colors.marker );
        circle_ring( rn, cx, cy, r + 1, colors.edgeStrong );
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
    if ( element && element->style == rackx::PanelControlStyle::SegmentDisplay )
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
        // A segment readout, not a bare number: it shows the PARAMETER NAME at
        // rest and the SELECTED VALUE while you are turning it, then falls back
        // to the name -- the way a hardware multi-switch display behaves.  This
        // branch used to return before drawLabel(), so a SegmentDisplay carried no
        // caption at all and read as an unexplained digit.
        char numbuf[24];
        std::snprintf( numbuf, sizeof( numbuf ), "%d", (int) std::lround( disp ) );
        // A display belongs to ONE knob: the one named by cvParamId (falling
        // back to its own id).  It lights up only when THAT knob -- or its CV
        // ring -- is being turned, so the cutoff display reports cutoff and
        // nothing else.
        const int shown = ( element->cvParamId >= 0 ) ? element->cvParamId : i;
        int ring = -1;
        if ( L.panel )
            for ( const rackx::PanelElement& e : L.panel->params )
                if ( e.style == rackx::PanelControlStyle::KnobCV && e.id == shown )
                    { ring = e.cvParamId; break; }
        const bool live = m_knob && m_knob_mod == m->id && m->mod &&
                          ( m_knob_param == shown ||
                            ( ring >= 0 && m_knob_param == ring ) ) &&
                          m_knob_param >= 0 &&
                          m_knob_param < (int) m->mod->paramQuantities.size();
        const int which = live ? m_knob_param : shown;
        std::string txt;
        if ( live )
        {
            const rack::engine::ParamQuantity& q = m->mod->paramQuantities[which];
            const float pv = m->mod->params[(size_t) which].value;
            const int idx = (int) std::lround( pv );
            std::string val2;
            // configSwitch() names its positions; surface them rather than an
            // index nobody can decode ("Ge OA90" beats "1").
            if ( !q.labels.empty() && idx >= 0 && idx < (int) q.labels.size() )
                val2 = q.labels[(size_t) idx];
            else
            {
                float d2 = pv;
                if ( q.displayBase > 0.f ) d2 = std::pow( q.displayBase, pv );
                d2 = d2 * ( q.displayMultiplier != 0.f ? q.displayMultiplier : 1.f )
                   + q.displayOffset;
                char b2[32];
                if ( q.snapEnabled ) std::snprintf( b2, sizeof b2, "%d", (int) std::lround( d2 ) );
                else                 std::snprintf( b2, sizeof b2, "%.2f", d2 );
                val2 = b2;
                if ( !q.unit.empty() ) val2 += q.unit;
            }
            txt = val2;          // name shows at rest; turning shows the value
        }
        else
        {
            txt = name.empty() ? std::string( numbuf ) : name;
        }
        const float textScale = m_zoom;
        txt = clip_text_scaled( app.mono, txt, box.w - zpx( 5 ), textScale );
        const int textW = app.mono.text_w_scaled( txt, textScale );
        const int textH = zpx( m_ch );
        app.mono.draw_scaled( rn, cx - textW / 2, cy - textH / 2, txt,
                              live ? colors.selection : colors.text, textScale );
        return;
    }

    // ---- concentric base + CV-depth control --------------------------------
    if ( element && element->style == rackx::PanelControlStyle::KnobCV )
    {
        // Outer ring: the CV attenuverter, drawn as an arc from centre-detent so
        // negative depth reads instantly as "the other way".
        float cvNorm = 0.5f;
        const int cvId = element->cvParamId;
        if ( m->mod && cvId >= 0 && cvId < (int) m->mod->params.size() &&
             cvId < (int) m->mod->paramQuantities.size() )
        {
            const rack::engine::ParamQuantity& q = m->mod->paramQuantities[cvId];
            const float span = q.maxValue - q.minValue;
            if ( span > 0.f )
                cvNorm = ( m->mod->params[(size_t)cvId].value - q.minValue ) / span;
        }
        fill_disc( rn, cx, cy, r + 2, colors.well );
        circle_ring( rn, cx, cy, r + 2, colors.edgeStrong );
        // arc of the ring, swept from the 12 o'clock detent
        const int ringR = imax( 3, r - 1 );
        const double aMid = -90.0, aCv = ( cvNorm - 0.5 ) * 270.0;
        const int steps = imax( 4, (int) std::fabs( aCv ) );
        ui::set_color( rn, colors.selection );
        for ( int s = 0; s <= steps; ++s )
        {
            const double t = (double) s / (double) steps;
            const double ang = ( aMid + aCv * t ) * PI / 180.0;
            const int px = cx + (int) std::lround( std::cos( ang ) * ringR );
            const int py = cy + (int) std::lround( std::sin( ang ) * ringR );
            SDL_RenderDrawPoint( rn, px, py );
            SDL_RenderDrawPoint( rn, px, py - 1 );
        }
        // Inner disc: the base value, with its own needle.
        const int ir = imax( 2, r - 4 );
        fill_disc( rn, cx, cy, ir, colors.knob );
        circle_ring( rn, cx, cy, ir, colors.edge );
        const double ia = ( -135.0 + norm * 270.0 ) * PI / 180.0;
        const int needle2 = imax( 1, ir - 2 );
        const int ex2 = cx + (int) std::lround( std::sin( ia ) * needle2 );
        const int ey2 = cy - (int) std::lround( std::cos( ia ) * needle2 );
        ui::set_color( rn, colors.marker );
        SDL_RenderDrawLine( rn, cx, cy, ex2, ey2 );
        SDL_RenderDrawLine( rn, cx + 1, cy, ex2 + 1, ey2 );
        drawLabel( cx, cy, r, placement );
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
    // A subtle 44px-diameter target guide makes the actual touch area visible;
    // the socket artwork itself remains faithful to the panel.
    if(ui::platform::rack_draw_touch_halos())
        circle_ring(rn,cx,cy,imax(r+7,22),hi||armed?colors.selection:colors.edge);
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
    auto drawJackLabel = [&]( int x, int y ) {
        // Every rack jack uses the Audio In/Out segmented-display language:
        // dark bezel, hard border, and bright mono text. Keeping this here
        // makes the rule apply to built-ins, Cardinal, scripts, and new types.
        const int padX = zpx( 3.f ), padY = zpx( 2.f );
        SDL_Rect disp { x - padX, y - padY,
                        labelW + 2 * padX, labelH + 2 * padY };
        ui::fill_rect( rn, disp, colors.well );
        ui::frame_rect( rn, disp, colors.edgeStrong );
        app.mono.draw_scaled( rn, x, y, s, colors.selection, m_zoom );
    };
    if ( compact )
    {
        const int minX = box.x + zpx( 3 );
        const int maxX = box.x + box.w - zpx( 3 ) - labelW;
        const int x = std::max( minX, std::min( cx - labelW / 2, maxX ) );
        const int titleH = zpx( L.panel->headerHeight );
        int y = cy - r - labelH - zpx( 2 );
        if ( y < box.y + titleH + zpx( 2 ) ) y = cy + r + zpx( 2 );
        drawJackLabel( x, y );
        return;
    }
    // A panel may OVERRIDE the side convention.  This path used to hardcode
    // "input right, output left" and ignore PanelElement::labelPlacement
    // entirely, so a module that asked for its caption under the jack -- the
    // knob / jack / name reading order -- silently got it beside the jack
    // instead, and no amount of setting the placement changed anything.
    const rackx::PanelLabelPlacement place =
        element ? element->labelPlacement : rackx::PanelLabelPlacement::None;
    if ( place == rackx::PanelLabelPlacement::Below )
        drawJackLabel( cx - labelW / 2, cy + r + zpx( 4 ) );
    else if ( place == rackx::PanelLabelPlacement::Above )
        drawJackLabel( cx - labelW / 2, cy - r - zpx( 4 ) - labelH );
    else if ( place == rackx::PanelLabelPlacement::Left )
        drawJackLabel( cx - r - zpx( 3 ) - labelW, cy - labelH / 2 );
    else if ( place == rackx::PanelLabelPlacement::Right )
        drawJackLabel( cx + r + zpx( 3 ), cy - labelH / 2 );
    else if ( element )
    {
        // The panel HAS an element for this jack and it asked for None.  That is
        // an explicit "this jack carries no caption of its own" -- honour it and
        // draw nothing.  It used to fall through to the legacy default below, so
        // a kit control unit drew the port name beside the jack AND the segment
        // display naming the same parameter underneath: the same word twice.
        return;
    }
    else if ( isInput )                       // legacy default: no panel element
        drawJackLabel( cx + r + zpx( 3 ), cy - labelH / 2 );
    else
        drawJackLabel( cx - r - zpx( 3 ) - labelW, cy - labelH / 2 );
}

void RackEditorView::draw_cable( ui::App& app, rackx::RackCable* c )
{
    if ( !c || !m_engine ) return;
    rackx::RackModule* a = m_engine->moduleById( c->fromMod );
    rackx::RackModule* b = m_engine->moduleById( c->toMod );
    if ( !a || !b ) return;
    if ( !port_tab_visible( a, false, c->outPort ) ||
         !port_tab_visible( b, true, c->inPort ) ) return;
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
    const Color cable = cable_color( c->id );
    const Color cableEdge = scale_color( cable, 0.34f );
    const Color highlight = c->id == m_hover_cable ? colors.selection : scale_color( cable, 1.f );
    auto stroke = [&]( Color color, int thickness ) {
        if ( std::fabs( laneOffset ) > 0.1f )
            stroke_wiggled_bezier( app.ren, x1, y1, x2, y2, color, thickness, laneOffset, phase );
        else
            stroke_bezier( app.ren, x1, y1, x2, y2, color, thickness, false );
    };
    stroke( cableEdge, imax( 4, zpx( 5 ) ) );
    stroke( cable, imax( 2, zpx( 3 ) ) );
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
    int w = imin( pal_w(), imax( 1, rect.w - 8 ) );
    int h = 3 * rowh + pal_rows() * pal_card_h() + 8;
    if ( h > rect.h - 8 ) h = imax( 1, rect.h - 8 );
    SDL_Rect q { rect.x + 4, rect.y + 4, w, h };
    return q;
}

void RackEditorView::open_palette( ui::App& app, int /*sx*/, int /*sy*/ )
{
    rackx::registerRackExtModules();
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

// ---- module right-click context menu ---------------------------------------
// Scripting modules (Pd/Csound) get "Edit Panel" + "Edit DSP" on top.
namespace {
    int ctx_row_h(){return ui::platform::rack_context_row_height();}
    int ctx_w(){return ui::platform::rack_context_width();}
    // Rows are built with an ACTION CODE each, rather than being indexed by
    // position: the menu is now variable (scripting modules add two rows, a
    // sample-holding module adds one), and position-indexing silently mis-fires
    // as soon as the set changes.
    enum CtxAction { CTX_EDIT_PANEL, CTX_EDIT_DSP, CTX_EDIT_SAMPLE,
                     CTX_LOAD_SAMPLE, CTX_CLEAR_SAMPLE, CTX_DUPLICATE, CTX_DELETE };
    void ctx_items( bool script, bool sampler,
                    std::vector<const char*>& labels, std::vector<int>& actions ) {
        labels.clear(); actions.clear();
        if ( script ) {
            labels.push_back( "Edit Panel" );     actions.push_back( CTX_EDIT_PANEL );
            labels.push_back( "Edit DSP" );       actions.push_back( CTX_EDIT_DSP );
        }
        if ( sampler ) {
            labels.push_back( "Edit Sample" );    actions.push_back( CTX_EDIT_SAMPLE );
            labels.push_back( "Load Sample..." ); actions.push_back( CTX_LOAD_SAMPLE );
            labels.push_back( "Clear Sample" );   actions.push_back( CTX_CLEAR_SAMPLE );
        }
        labels.push_back( "Duplicate" );          actions.push_back( CTX_DUPLICATE );
        labels.push_back( "Delete" );             actions.push_back( CTX_DELETE );
    } }

// One definition of the popup box, used by BOTH draw and hit-test.  It also
// clamps to the LEFT/TOP edges: only the right/bottom were clamped, so a
// right-click near the left edge of a narrow rack pushed the menu to a negative
// x where it was drawn (and clicked) partly outside the view.
SDL_Rect RackEditorView::ctx_menu_box( int count ) const
{
    const int rowH = ctx_row_h(), w = ctx_w(), h = rowH * count;
    int x = m_ctx_x, y = m_ctx_y;
    if ( x + w > rect.x + rect.w ) x = rect.x + rect.w - w;
    if ( y + h > rect.y + rect.h ) y = rect.y + rect.h - h;
    if ( x < rect.x ) x = rect.x;
    if ( y < rect.y ) y = rect.y;
    return SDL_Rect{ x, y, w, h };
}

void RackEditorView::draw_ctx_menu( ui::App& app )
{
    if ( m_ctx_mod < 0 ) return;
    // The module can be destroyed (undo, project load, another view) while its
    // menu is still up; drawing a menu for a module that no longer exists left
    // it acting on a dead id.
    if ( !m_engine || !m_engine->moduleById( m_ctx_mod ) ) { m_ctx_mod = -1; return; }
    const ui::Theme& t = theme();
    const bool script = m_engine->isScriptModule( m_ctx_mod );
    std::vector<const char*> items; std::vector<int> acts;
    ctx_items( script, sample_slot_of( m_ctx_mod ) != nullptr, items, acts );
    const int count = (int) items.size();
    const int rowH = ctx_row_h();
    const SDL_Rect box = ctx_menu_box( count );
    int mx = 0, my = 0; ui::mouse_logical( app, mx, my );   // logical, not window px
    fill_rect( app.ren, box, t.panel );
    frame_rect( app.ren, box, t.dim );
    for ( int i = 0; i < count; ++i )
    {
        SDL_Rect rr{ box.x, box.y + i * rowH, box.w, rowH };
        const bool hot = mx >= rr.x && mx < rr.x + rr.w && my >= rr.y && my < rr.y + rr.h;
        if ( hot ) fill_rect( app.ren, rr, t.accent );
        app.mono.draw( app.ren, rr.x + 6, rr.y + ( rowH - app.mono.ch() ) / 2,
                       items[i], hot ? t.bg : t.text );
    }
}

bool RackEditorView::ctx_menu_mouse( ui::App& app, const ui::MouseEv& e )
{
    if ( !e.pressed ) { app.request_redraw(); return true; }   // swallow the release
    const int id = m_ctx_mod;
    m_ctx_mod = -1;                                            // any click closes it
    // Re-validate: between opening the menu and picking a row the module may have
    // been removed, in which case every action below would run on a dead id.
    if ( !m_engine || id < 0 || !m_engine->moduleById( id ) ) {
        app.request_redraw();
        return true;
    }
    const bool script = m_engine->isScriptModule( id );
    std::vector<const char*> items; std::vector<int> acts;
    ctx_items( script, sample_slot_of( id ) != nullptr, items, acts );
    const int count = (int) items.size();
    const int rowH = ctx_row_h();
    const SDL_Rect box = ctx_menu_box( count );
    if ( e.x >= box.x && e.x < box.x + box.w &&
         e.y >= box.y && e.y < box.y + box.h )
    {
        const int idx = ( e.y - box.y ) / rowH;
        if ( idx < 0 || idx >= count ) { app.request_redraw(); return true; }
        switch ( acts[(size_t)idx] )
        {
            case CTX_EDIT_PANEL:   if ( on_edit_panel ) on_edit_panel( id ); break;
            case CTX_EDIT_DSP:     if ( on_edit_dsp )   on_edit_dsp( id );   break;
            case CTX_EDIT_SAMPLE:  if ( on_edit_sample ) on_edit_sample( id ); break;
            case CTX_LOAD_SAMPLE:  if ( on_load_sample ) on_load_sample( id ); break;
            case CTX_CLEAR_SAMPLE: if ( rackx::ISampleSlot* ss = sample_slot_of( id ) )
                                       ss->sampleClear();
                                   break;
            case CTX_DUPLICATE:  { int nid = duplicate_module( id );
                                   if ( nid >= 0 ) m_sel = nid; } break;
            case CTX_DELETE:       m_engine->removeModule( id );
                                   forget_module( id ); break;
            default: break;
        }
    }
    app.request_redraw();
    return true;
}

rackx::ISampleSlot* RackEditorView::sample_slot_of( int id ) const
{
    if ( !m_engine ) return nullptr;
    rackx::RackModule* m = m_engine->moduleById( id );
    if ( !m || !m->mod ) return nullptr;
    return dynamic_cast<rackx::ISampleSlot*>( m->mod.get() );
}

// A module id just stopped existing.  Every piece of interaction state that
// names a module has to let go of it.
//
// Deleting a module used to clear only m_sel.  The click-to-connect ARM
// (m_link_out_mod) survived: the jack stayed lit on a module that was no longer
// drawn, and the next click on any input jack called addCable() with a dead
// source id -- a cable from nothing, or from whatever module later inherited the
// id.  The title-bar move drag, the knob/button drags and the context menu all
// held the same kind of reference.
void RackEditorView::forget_module( int id )
{
    if ( id < 0 ) return;
    if ( m_sel        == id ) m_sel = -1;
    if ( m_ctx_mod    == id ) m_ctx_mod = -1;
    if ( m_link_out_mod == id ) { m_link_out_mod = -1; m_link_out_port = -1; }
    if ( m_wire   && m_wire_mod   == id ) { m_wire = false; m_wire_ok = false; }
    if ( m_move   && m_move_id    == id )   m_move = false;
    if ( m_knob   && m_knob_mod   == id )   m_knob = false;
    if ( m_button && m_button_mod == id )   m_button = false;
    if ( m_curve_index >= 0 && m_curve_mod == id ) { m_curve_index = -1; m_curve_point = -1; }
    if ( m_hover_mod == id ) { m_hover_mod = 0; m_hover_in = 0; }
    if ( m_last_knob_mod == id ) { m_last_knob_mod = -1; m_last_knob_param = -1; }
    // Cached hover/hit-test results are indexed by geometry that just changed.
    m_hover_cable = -1;
    m_hover_test_x = m_hover_test_y = -100000;
    m_hover_test_cables = m_hover_test_modules = -1;
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
    int typeMaxScroll = (int) types.size() + 1 - pal_rows();
    if ( typeMaxScroll < 0 ) typeMaxScroll = 0;
    if ( m_pal_type_scroll > typeMaxScroll ) m_pal_type_scroll = typeMaxScroll;
    if ( m_pal_type_scroll < 0 ) m_pal_type_scroll = 0;

    // selected type and search matching modules
    std::vector<int> fl = filtered();
    int maxscroll = ( (int) fl.size() + 1 ) / 2 - pal_rows(); if ( maxscroll < 0 ) maxscroll = 0;
    if ( m_pal_scroll > maxscroll ) m_pal_scroll = maxscroll;
    if ( m_pal_scroll < 0 )         m_pal_scroll = 0;

    const std::vector<rackx::ModuleType>& reg = rackx::registry();
    int mx, my; ui::mouse_logical( app, mx, my );   // logical, not window px
    const int listTop = p.y + 2 + 3 * rowh;
    const int cardGap = 4;
    const int cardWidth = ( p.w - 4 - cardGap ) / 2;
    for ( int row = 0; row < pal_rows(); ++row )
    {
        for ( int column = 0; column < 2; ++column )
        {
            const int moduleIndex = 2 * ( m_pal_scroll + row ) + column;
            if ( moduleIndex >= (int) fl.size() ) continue;
            const rackx::ModuleType& mt = reg[ fl[moduleIndex] ];
            SDL_Rect card { p.x + 2 + column * ( cardWidth + cardGap ),
                            listTop + row * pal_card_h(), cardWidth, pal_card_h() - 3 };
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
                        imin( pal_rows(), (int) types.size() + 1 ) * rowh };
        ui::fill_rect( r, menu, colors.panel );
        ui::frame_rect( r, menu, colors.selection );
        for ( int row = 0; row < pal_rows(); ++row )
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
                        imin( pal_rows(), (int) types.size() + 1 ) * rowh };
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
        const int row = ( e.y - listTop ) / pal_card_h();
        if ( row >= 0 && row < pal_rows() )
        {
            std::vector<int> fl = filtered();
            const int column = e.x >= p.x + 2 + cardWidth + cardGap ? 1 : 0;
            SDL_Rect card { p.x + 2 + column * ( cardWidth + cardGap ),
                            listTop + row * pal_card_h(), cardWidth, pal_card_h() - 3 };
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
    if(downEdge&&!m_pal&&m_ctx_mod<0){
        const int h=m_ch+10;
        SDL_Rect b[3]={{rect.x+5,rect.y+5,58,h},{rect.x+67,rect.y+5,58,h},
                       {rect.x+129,rect.y+5,78,h}};
        for(int i=0;i<3;++i)if(in_rect(b[i],e.x,e.y)){
            if(i==0&&on_open_file)on_open_file();
            if(i==1&&on_save_file)on_save_file();
            if(i==2&&on_save_file_as)on_save_file_as();
            app.request_redraw();return true;
        }
    }

    // A breakpoint drag owns the mouse until the button comes up.
    if ( m_curve_point >= 0 )
    {
        if ( !m_ldown ) { m_curve_index = -1; m_curve_point = -1; m_curve_bend = false; }
        else
        {
            rackx::RackModule* rm = m_engine ? m_engine->moduleById( m_curve_mod ) : nullptr;
            const rackx::ModuleType* ty = rm ? rackx::findType( rm->slug ) : nullptr;
            const rackx::PanelElement* el = ty
                ? find_element( ty->panel.params, m_curve_param ) : nullptr;
            rackx::ICurveSource* src = ( rm && rm->mod )
                ? dynamic_cast<rackx::ICurveSource*>( rm->mod.get() ) : nullptr;
            int kx, ky, kr;
            if ( src && el && knob_pos( rm, m_curve_param, kx, ky, kr ) )
            {
                const int halfW = imax( 20, zpx( el->width  > 0.f ? el->width  : 120.f ) / 2 );
                const int halfH = imax( 12, zpx( el->height > 0.f ? el->height :  60.f ) / 2 );
                const int bw = 2 * halfW + 1, bh = 2 * halfH + 1;
                float t = (float)( e.x - ( kx - halfW ) ) / (float) imax( 1, bw - 1 );
                float v = 1.f - (float)( e.y - ( ky - halfH ) ) / (float) imax( 1, bh - 1 );
                t = t < 0.f ? 0.f : ( t > 1.f ? 1.f : t );
                v = v < 0.f ? 0.f : ( v > 1.f ? 1.f : v );
                if ( m_curve_bend )
                {
                    // Vertical drag of a segment handle bends that segment; the
                    // module solves for its own curve amount so the midpoint
                    // lands exactly under the pointer.
                    src->curveSetSegmentMid( m_curve_index, m_curve_point, v );
                }
                else
                {
                    rackx::CurvePoint p;
                    if ( src->curveGetPoint( m_curve_index, m_curve_point, p ) )
                    {
                        p.t = src->curveSnapTime( m_curve_index, t );
                        p.v = v;
                        src->curveSetPoint( m_curve_index, m_curve_point, p );
                    }
                }
            }
            app.request_redraw();
            return true;
        }
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
        // The armed jack must still exist.  Belt and braces on top of
        // forget_module(): an arm can also be invalidated by a module the
        // ENGINE dropped (a script recompile that pruned it, a project load),
        // and connecting from a dead id would either do nothing or wire the
        // module that inherited the id.
        if ( m_engine && m_link_out_mod >= 0 && m_link_out_mod != mod &&
             m_engine->moduleById( m_link_out_mod ) )
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
            // Momentary: held while the pointer is down, released on the way up.
            // Piano keys are always momentary (a key press enters a note); a
            // StepPad opts in with widget == "momentary" (step select, octave
            // shift) and otherwise latches like a gate.
            // Breakpoint-curve editor: pick the nearest point, or add one on
            // empty space.  Right-click deletes.  Dragging is handled by the
            // motion path via m_curve_*.
            if ( element->style == rackx::PanelControlStyle::Curve )
            {
                rackx::ICurveSource* src =
                    rm->mod ? dynamic_cast<rackx::ICurveSource*>( rm->mod.get() ) : nullptr;
                const int ci = element->curveIndex;
                if ( src && ci >= 0 && ci < src->curveCount() )
                {
                    int kx, ky, kr;
                    knob_pos( rm, port, kx, ky, kr );
                    const int halfW = imax( 20, zpx( element->width  > 0.f ? element->width  : 120.f ) / 2 );
                    const int halfH = imax( 12, zpx( element->height > 0.f ? element->height :  60.f ) / 2 );
                    const int bx = kx - halfW, by = ky - halfH;
                    const int bw = 2 * halfW + 1, bh = 2 * halfH + 1;
                    const float t = (float)( mx - bx ) / (float) imax( 1, bw - 1 );
                    const float v = 1.f - (float)( my - by ) / (float) imax( 1, bh - 1 );

                    // A segment handle wins over the points it sits between:
                    // it is the smaller target and the one under the pointer.
                    int bend = -1, bendDist = 0;
                    const int nSeg = src->curvePointCount( ci );
                    for ( int i = 0; i + 1 < nSeg; ++i )
                    {
                        rackx::CurvePoint a, b;
                        if ( !src->curveGetPoint( ci, i, a ) ||
                             !src->curveGetPoint( ci, i + 1, b ) ) continue;
                        if ( std::fabs( b.v - a.v ) < 1e-4f ) continue;
                        const float mt = ( a.t + b.t ) * 0.5f;
                        const int hx = bx + (int) std::lround( mt * ( bw - 1 ) );
                        const int hy = by + bh - 1
                            - (int) std::lround( src->curveValueAt( ci, mt ) * ( bh - 1 ) );
                        const int d = std::abs( mx - hx ) + std::abs( my - hy );
                        if ( bend < 0 || d < bendDist ) { bend = i; bendDist = d; }
                    }
                    if ( bend >= 0 && bendDist <= zpx( 6.f ) )
                    {
                        m_curve_mod = mod; m_curve_param = port;
                        m_curve_index = ci; m_curve_point = bend;
                        m_curve_bend = true;
                        app.request_redraw();
                        return true;
                    }

                    int best = -1, bestDist = 0;
                    const int n = src->curvePointCount( ci );
                    for ( int i = 0; i < n; ++i )
                    {
                        rackx::CurvePoint p;
                        if ( !src->curveGetPoint( ci, i, p ) ) continue;
                        const int hx = bx + (int) std::lround( p.t * ( bw - 1 ) );
                        const int hy = by + bh - 1 - (int) std::lround( p.v * ( bh - 1 ) );
                        const int d = std::abs( mx - hx ) + std::abs( my - hy );
                        if ( best < 0 || d < bestDist ) { best = i; bestDist = d; }
                    }
                    const bool onPoint = ( best >= 0 && bestDist <= zpx( 8.f ) );
                    if ( onPoint )
                    {
                        src->curveSetSelected( ci, best );
                        m_curve_mod = mod; m_curve_param = port;
                        m_curve_index = ci; m_curve_point = best;
                        m_curve_bend = false;
                    }
                    else
                    {
                        const int added = src->curveAddPoint( ci, t, v );
                        if ( added >= 0 )
                        {
                            src->curveSetSelected( ci, added );
                            m_curve_mod = mod; m_curve_param = port;
                            m_curve_index = ci; m_curve_point = added;
                            m_curve_bend = false;
                        }
                    }
                    app.request_redraw();
                    return true;
                }
            }
            const bool momentary =
                element->style == rackx::PanelControlStyle::Button ||
                element->style == rackx::PanelControlStyle::PianoKey ||
                ( element->style == rackx::PanelControlStyle::StepPad &&
                  element->widget == "momentary" );
            if ( momentary )
            {
                m_engine->setParam( mod, port, q.maxValue );
                if ( q.name == "Reset" )
                    m_engine->resetModule( mod );
                m_button = true; m_button_mod = mod; m_button_param = port;
                app.request_redraw();
                return true;
            }
            if ( element->style == rackx::PanelControlStyle::Switch ||
                 element->style == rackx::PanelControlStyle::Gate ||
                 element->style == rackx::PanelControlStyle::StepPad )
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

    // (3) a module: select it; desktop moves from the title bar.  On mobile all
    // unused faceplate space is a move handle: controls and jacks already had
    // first refusal above, so this remains unambiguous while vastly increasing
    // the draggable area.
    rackx::RackModule* m = module_at( mx, my );
    if ( m )
    {
        m_sel = m->id;
        const bool moveHit=ui::platform::rack_drag_whole_faceplate()||title_at(m,mx,my);
        if ( moveHit )
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
        if ( m_wire_ok )
        {
            m_hover_mod = hm; m_hover_in = hp;
            if(ui::platform::mobile()) {
            // Magnetically snap the cable preview to the target socket so a
            // finger cannot obscure whether the impending connection is valid.
            rackx::RackModule* target = m_engine ? m_engine->moduleById(hm) : nullptr;
            int radius=0;
            jack_pos(target,true,hp,m_wire_x,m_wire_y,radius);
            }
        }
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
            int maxscroll = (int) palette_types().size() + 1 - pal_rows();
            if ( maxscroll < 0 ) maxscroll = 0;
            if ( m_pal_type_scroll > maxscroll ) m_pal_type_scroll = maxscroll;
            if ( m_pal_type_scroll < 0 )         m_pal_type_scroll = 0;
        }
        else
        {
            m_pal_scroll -= dy;
            int maxscroll = ( (int) filtered().size() + 1 ) / 2 - pal_rows(); if ( maxscroll < 0 ) maxscroll = 0;
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
            ui::mouse_logical( app, mx, my );   // logical, not window px
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
        const int gone = m_sel;
        m_engine->removeModule( gone );
        forget_module( gone );
        app.request_redraw();
        return true;
    }
    return false;
}

} // namespace rackui
