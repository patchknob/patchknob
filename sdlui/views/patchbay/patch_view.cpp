//----------------------------------------------------------------------------
//  sdlui/views/patchbay/patch_view.cpp -- SDL2 patchbay node editor.
//----------------------------------------------------------------------------
#include "patch_view.h"

#include <algorithm>
#include <cmath>

using ui::Color;
using ui::theme;

namespace {

// ---- filled disc -----------------------------------------------------------
void fill_disc( SDL_Renderer* r, int cx, int cy, int rad, Color c )
{
    ui::set_color( r, c );
    for ( int dy = -rad; dy <= rad; ++dy )
    {
        int dx = int( std::floor( std::sqrt( double(rad*rad - dy*dy) ) ) );
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

// horizontal (or vertical) inset of a rounded-rect edge at position `pos`
// along an axis of length `len`, for corner radius `rad`.
int corner_inset( int pos, int len, int rad )
{
    int d = 0;
    if ( pos < rad )                 d = rad - pos;
    else if ( pos >= len - rad )     d = rad - ( len - 1 - pos );
    if ( d <= 0 ) return 0;
    return rad - int( std::round( std::sqrt( double( rad*rad - (rad-d)*(rad-d) ) ) ) );
}

void fill_round_rect( SDL_Renderer* r, const SDL_Rect& q, int rad, Color c )
{
    if ( rad > q.w/2 ) rad = q.w/2;
    if ( rad > q.h/2 ) rad = q.h/2;
    ui::set_color( r, c );
    for ( int yy = 0; yy < q.h; ++yy )
    {
        int ins = corner_inset( yy, q.h, rad );
        SDL_RenderDrawLine( r, q.x + ins, q.y + yy, q.x + q.w - 1 - ins, q.y + yy );
    }
}

void frame_round_rect( SDL_Renderer* r, const SDL_Rect& q, int rad, Color c )
{
    if ( rad > q.w/2 ) rad = q.w/2;
    if ( rad > q.h/2 ) rad = q.h/2;
    ui::set_color( r, c );
    for ( int yy = 0; yy < q.h; ++yy )
    {
        int ins = corner_inset( yy, q.h, rad );
        SDL_RenderDrawPoint( r, q.x + ins,            q.y + yy );
        SDL_RenderDrawPoint( r, q.x + q.w - 1 - ins,  q.y + yy );
    }
    for ( int xx = 0; xx < q.w; ++xx )
    {
        int ins = corner_inset( xx, q.w, rad );
        SDL_RenderDrawPoint( r, q.x + xx, q.y + ins );
        SDL_RenderDrawPoint( r, q.x + xx, q.y + q.h - 1 - ins );
    }
}

// ---- bezier ----------------------------------------------------------------
void control_points( int x1, int y1, int x2, int y2,
                     double& c1x, double& c1y, double& c2x, double& c2y )
{
    double dx = std::fabs( double(x2 - x1) ) * 0.5;
    if ( dx < 40.0 ) dx = 40.0;
    c1x = x1 + dx; c1y = y1;
    c2x = x2 - dx; c2y = y2;
}

void bezier_point( double t, double x1, double y1, double c1x, double c1y,
                   double c2x, double c2y, double x2, double y2,
                   double& bx, double& by )
{
    double u = 1.0 - t;
    double a = u*u*u, b = 3.0*u*u*t, c = 3.0*u*t*t, d = t*t*t;
    bx = a*x1 + b*c1x + c*c2x + d*x2;
    by = a*y1 + b*c1y + c*c2y + d*y2;
}

// Stroke a bezier as sampled line segments.  thickness fattens vertically;
// dashed skips alternate spans (for MIDI wires).
void stroke_bezier( SDL_Renderer* r, int x1, int y1, int x2, int y2,
                    Color col, int thickness, bool dashed )
{
    double c1x, c1y, c2x, c2y;
    control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );

    const int STEPS = 40;
    SDL_Point pts[STEPS + 1];
    for ( int s = 0; s <= STEPS; ++s )
    {
        double t = double(s) / STEPS, bx, by;
        bezier_point( t, x1, y1, c1x, c1y, c2x, c2y, x2, y2, bx, by );
        pts[s].x = int( std::round( bx ) );
        pts[s].y = int( std::round( by ) );
    }

    ui::set_color( r, col );
    int half = thickness / 2;
    for ( int off = -half; off <= thickness - 1 - half; ++off )
    {
        for ( int s = 0; s < STEPS; ++s )
        {
            if ( dashed && ( ( s / 3 ) % 2 ) ) continue;   // ~3-on 3-off pattern
            SDL_RenderDrawLine( r, pts[s].x,   pts[s].y   + off,
                                   pts[s+1].x, pts[s+1].y + off );
        }
    }
}

// squared-distance point test
bool near_pt( int x, int y, int cx, int cy, int rad )
{
    int dx = x - cx, dy = y - cy;
    return dx*dx + dy*dy <= rad*rad;
}

// ellipsis-truncate `s` so its monospace pixel width fits `maxw`.
std::string clip_text( const ui::Font& f, const std::string& s, int maxw )
{
    if ( f.text_w( s ) <= maxw ) return s;
    std::string t = s;
    while ( t.size() > 1 )
    {
        t.erase( t.size() - 1 );
        if ( f.text_w( t + "..." ) <= maxw ) return t + "...";
    }
    return t;
}

} // anonymous namespace

namespace seq24 {
namespace patchbay {

PatchView::PatchView()
    : m_next_id( 1 )
    , m_dragging_wire( false ), m_moving_node( false )
    , m_drag_kind( PortKind::Audio )
    , m_drag_x( 0 ), m_drag_y( 0 ), m_hover_ok( false )
    , m_move_id( 0 ), m_move_dx( 0 ), m_move_dy( 0 )
    , m_menu_open( false )
    , m_menu_x( 0 ), m_menu_y( 0 ), m_menu_w( 0 ), m_menu_h( 0 )
    , m_menu_cx( 0 ), m_menu_cy( 0 )
{
}

// ============================================================================
//  Model access
// ============================================================================
void PatchView::set_nodes( const std::vector<Node>& a_nodes )
{
    m_nodes = a_nodes;
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        if ( m_nodes[i].id >= m_next_id )
            m_next_id = m_nodes[i].id + 1;
}

void PatchView::set_connections( const std::vector<Connection>& a_conns )
{
    m_conns = a_conns;
}

NodeId PatchView::add_node( Node n )
{
    if ( n.id == 0 )              n.id = m_next_id++;
    else if ( n.id >= m_next_id ) m_next_id = n.id + 1;
    m_nodes.push_back( n );
    return n.id;
}

Node* PatchView::find_node( NodeId a_id )
{
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        if ( m_nodes[i].id == a_id )
            return &m_nodes[i];
    return 0;
}

bool PatchView::add_connection( const Connection& c )
{
    PortKind ka, kb;
    if ( !kind_of( c.from.node, c.from.port, PortDir::Out, ka ) ) return false;
    if ( !kind_of( c.to.node,   c.to.port,   PortDir::In,  kb ) ) return false;
    if ( ka != kb )                 return false;   // audio<->midi refused
    if ( c.from.node == c.to.node ) return false;   // no self-patch
    for ( std::size_t i = 0; i < m_conns.size(); ++i )
        if ( same_connection( m_conns[i], c ) )
            return false;                            // already wired

    m_conns.push_back( c );
    if ( on_connect ) on_connect( c.from.node, c.from.port, c.to.node, c.to.port );
    return true;
}

void PatchView::remove_connection( const Connection& c )
{
    for ( std::size_t i = 0; i < m_conns.size(); ++i )
        if ( same_connection( m_conns[i], c ) )
        {
            m_conns.erase( m_conns.begin() + i );
            if ( on_disconnect )
                on_disconnect( c.from.node, c.from.port, c.to.node, c.to.port );
            return;
        }
}

void PatchView::remove_node( NodeId a_id )
{
    bool found = false;
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        if ( m_nodes[i].id == a_id )
        {
            m_nodes.erase( m_nodes.begin() + i );
            found = true;
            break;
        }
    if ( !found ) return;

    for ( std::size_t i = m_conns.size(); i-- > 0; )
        if ( m_conns[i].from.node == a_id || m_conns[i].to.node == a_id )
            m_conns.erase( m_conns.begin() + i );

    if ( on_remove_node ) on_remove_node( a_id );
}

// ============================================================================
//  Geometry helpers (all in canvas-local coords)
// ============================================================================
int PatchView::node_height( const Node& n ) const
{
    std::size_t rows = std::max( n.inPorts.size(), n.outPorts.size() );
    int h = TITLE_H + int(rows) * PORT_ROW + BODY_PAD;
    int minh = TITLE_H + PORT_ROW;
    return h > minh ? h : minh;
}

void PatchView::port_center( const Node& n, PortDir dir, std::size_t idx,
                             int& cx, int& cy ) const
{
    cy = int( n.y ) + TITLE_H + int( PORT_ROW * ( double(idx) + 0.5 ) );
    cx = ( dir == PortDir::In ) ? int( n.x ) : int( n.x ) + NODE_W;
}

bool PatchView::ref_center( const PortRef& r, PortDir dir, int& cx, int& cy ) const
{
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
    {
        if ( m_nodes[i].id != r.node ) continue;
        const std::vector<Port>& v =
            ( dir == PortDir::In ) ? m_nodes[i].inPorts : m_nodes[i].outPorts;
        for ( std::size_t k = 0; k < v.size(); ++k )
            if ( v[k].id == r.port )
            {
                port_center( m_nodes[i], dir, k, cx, cy );
                return true;
            }
    }
    return false;
}

bool PatchView::kind_of( NodeId nid, PortId pid, PortDir dir, PortKind& out ) const
{
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
    {
        if ( m_nodes[i].id != nid ) continue;
        const std::vector<Port>& v =
            ( dir == PortDir::In ) ? m_nodes[i].inPorts : m_nodes[i].outPorts;
        for ( std::size_t k = 0; k < v.size(); ++k )
            if ( v[k].id == pid ) { out = v[k].kind; return true; }
    }
    return false;
}

bool PatchView::port_at( int x, int y, PortHit& out ) const
{
    for ( std::size_t i = m_nodes.size(); i-- > 0; )
    {
        const Node& n = m_nodes[i];
        for ( std::size_t k = 0; k < n.inPorts.size(); ++k )
        {
            int cx, cy; port_center( n, PortDir::In, k, cx, cy );
            if ( near_pt( x, y, cx, cy, HIT_R ) )
            {
                out.node = n.id; out.port = n.inPorts[k].id;
                out.kind = n.inPorts[k].kind; out.dir = PortDir::In;
                return true;
            }
        }
        for ( std::size_t k = 0; k < n.outPorts.size(); ++k )
        {
            int cx, cy; port_center( n, PortDir::Out, k, cx, cy );
            if ( near_pt( x, y, cx, cy, HIT_R ) )
            {
                out.node = n.id; out.port = n.outPorts[k].id;
                out.kind = n.outPorts[k].kind; out.dir = PortDir::Out;
                return true;
            }
        }
    }
    return false;
}

NodeId PatchView::node_at( int x, int y ) const
{
    for ( std::size_t i = m_nodes.size(); i-- > 0; )
    {
        const Node& n = m_nodes[i];
        int h = node_height( n );
        if ( x >= int(n.x) && x <= int(n.x) + NODE_W &&
             y >= int(n.y) && y <= int(n.y) + h )
            return n.id;
    }
    return 0;
}

bool PatchView::connection_at( int x, int y, Connection& out ) const
{
    double best = HIT_R;
    bool   got  = false;
    for ( std::size_t i = 0; i < m_conns.size(); ++i )
    {
        int x1, y1, x2, y2;
        if ( !ref_center( m_conns[i].from, PortDir::Out, x1, y1 ) ) continue;
        if ( !ref_center( m_conns[i].to,   PortDir::In,  x2, y2 ) ) continue;

        double c1x, c1y, c2x, c2y;
        control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );
        const int STEPS = 24;
        for ( int s = 0; s <= STEPS; ++s )
        {
            double t = double(s) / STEPS, bx, by;
            bezier_point( t, x1, y1, c1x, c1y, c2x, c2y, x2, y2, bx, by );
            double dd = std::hypot( bx - x, by - y );
            if ( dd < best ) { best = dd; out = m_conns[i]; got = true; }
        }
    }
    return got;
}

void PatchView::raise_node( NodeId id )
{
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        if ( m_nodes[i].id == id )
        {
            Node n = m_nodes[i];
            m_nodes.erase( m_nodes.begin() + i );
            m_nodes.push_back( n );
            return;
        }
}

// ============================================================================
//  Drawing
// ============================================================================
void PatchView::draw( ui::App& app )
{
    if ( !visible ) return;
    const ui::Theme& t = theme();
    SDL_Renderer* r = app.ren;

    // Canvas background + a subtle frame so the bounds are visible.
    ui::fill_rect( r, rect, t.bg );
    ui::frame_rect( r, rect, t.dim );

    // Clip to the widget so nodes can't bleed past the canvas edges.
    SDL_Rect clip = rect;
    SDL_RenderSetClipRect( r, &clip );

    // Committed connections (behind the nodes).
    for ( std::size_t i = 0; i < m_conns.size(); ++i )
        draw_connection( app, m_conns[i] );

    // In-flight drag wire.
    if ( m_dragging_wire )
        draw_drag_wire( app );

    // Nodes on top (painter's order == vector order; last == topmost).
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        draw_node( app, m_nodes[i] );

    SDL_RenderSetClipRect( r, nullptr );

    if ( m_menu_open )
        draw_menu( app );
}

void PatchView::draw_node( ui::App& app, const Node& n )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int ox = rect.x, oy = rect.y;
    const int h  = node_height( n );

    SDL_Rect box { ox + int(n.x), oy + int(n.y), NODE_W, h };

    // Body + outline.
    fill_round_rect( r, box, CORNER, t.panel );
    frame_round_rect( r, box, CORNER, t.hi );

    // Title strip (top rounded; clip a rounded rect and paint the top band).
    SDL_Rect titleClip { box.x, box.y, NODE_W, TITLE_H };
    SDL_RenderSetClipRect( r, &titleClip );
    SDL_Rect titleFull { box.x, box.y, NODE_W, TITLE_H + CORNER };
    fill_round_rect( r, titleFull, CORNER, t.accent );
    SDL_RenderSetClipRect( r, &rect );   // restore canvas clip

    // Title text (bg colour on the accent strip), ellipsis-truncated.
    std::string title = clip_text( app.mono, n.name, NODE_W - 12 );
    app.mono.draw( r, box.x + 7,
                   box.y + ( TITLE_H - app.mono.ch() ) / 2, title, t.bg );

    // Input ports + left-aligned labels.
    for ( std::size_t k = 0; k < n.inPorts.size(); ++k )
    {
        int cx, cy; port_center( n, PortDir::In, k, cx, cy );
        cx += ox; cy += oy;
        bool hi = m_dragging_wire && m_hover_ok &&
                  m_hover_pt.node == n.id && m_hover_pt.port == n.inPorts[k].id;
        draw_port( app, cx, cy, n.inPorts[k].kind, hi );
        app.mono.draw( r, cx + PORT_R + 4, cy - app.mono.ch()/2,
                       n.inPorts[k].name, t.hi );
    }
    // Output ports + right-aligned labels.
    for ( std::size_t k = 0; k < n.outPorts.size(); ++k )
    {
        int cx, cy; port_center( n, PortDir::Out, k, cx, cy );
        cx += ox; cy += oy;
        draw_port( app, cx, cy, n.outPorts[k].kind, false );
        int tw = app.mono.text_w( n.outPorts[k].name );
        app.mono.draw( r, cx - PORT_R - 4 - tw, cy - app.mono.ch()/2,
                       n.outPorts[k].name, t.hi );
    }
}

void PatchView::draw_port( ui::App& app, int cx, int cy, PortKind kind, bool hi )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();

    if ( hi )                                   // valid drop target halo
        circle_ring( r, cx, cy, PORT_R + 3, t.active );

    if ( kind == PortKind::Audio )              // FILLED
    {
        fill_disc( r, cx, cy, PORT_R, t.hi );
    }
    else                                        // MIDI: HOLLOW
    {
        fill_disc( r, cx, cy, PORT_R, t.panel );
        circle_ring( r, cx, cy, PORT_R, t.hi );
    }
}

void PatchView::draw_connection( ui::App& app, const Connection& c )
{
    int x1, y1, x2, y2;
    if ( !ref_center( c.from, PortDir::Out, x1, y1 ) ) return;
    if ( !ref_center( c.to,   PortDir::In,  x2, y2 ) ) return;

    PortKind k;
    bool midi = kind_of( c.from.node, c.from.port, PortDir::Out, k ) &&
                k == PortKind::Midi;

    stroke_bezier( app.ren, x1 + rect.x, y1 + rect.y, x2 + rect.x, y2 + rect.y,
                   theme().dim, midi ? 1 : 2, midi );
}

void PatchView::draw_drag_wire( ui::App& app )
{
    int x1, y1;
    if ( !ref_center( m_drag_src, PortDir::Out, x1, y1 ) ) return;

    stroke_bezier( app.ren, x1 + rect.x, y1 + rect.y,
                   m_drag_x + rect.x, m_drag_y + rect.y,
                   m_hover_ok ? theme().active : theme().sel, 2, true );
}

// ============================================================================
//  Context menu (SDL has no native popups -- draw an in-canvas overlay)
// ============================================================================
void PatchView::layout_menu( ui::App& app )
{
    int wmax = 0;
    for ( std::size_t i = 0; i < m_menu_items.size(); ++i )
        wmax = std::max( wmax, app.mono.text_w( m_menu_items[i].label ) );
    m_menu_w = wmax + 20;
    m_menu_h = int( m_menu_items.size() ) * ( app.mono.ch() + 6 ) + 4;

    // keep the menu on-screen
    if ( m_menu_x + m_menu_w > app.w ) m_menu_x = app.w - m_menu_w;
    if ( m_menu_y + m_menu_h > app.h ) m_menu_y = app.h - m_menu_h;
    if ( m_menu_x < 0 ) m_menu_x = 0;
    if ( m_menu_y < 0 ) m_menu_y = 0;
}

void PatchView::open_add_menu( ui::App& app, int sx, int sy, double cx, double cy )
{
    m_menu_items.clear();
    m_menu_cx = cx; m_menu_cy = cy;

    static const char* cats[] =
        { "Instrument", "Effect", "MIDI", "Audio I/O", "Mixer" };
    for ( unsigned i = 0; i < sizeof(cats)/sizeof(cats[0]); ++i )
    {
        std::string cat = cats[i];
        MenuItem mi;
        mi.label = std::string( "Add " ) + cat;
        mi.enabled = true; mi.separator = false;
        mi.action = [this, cat]() {
            if ( on_add_module ) on_add_module( m_menu_cx, m_menu_cy, cat );
        };
        m_menu_items.push_back( mi );
    }
    { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
    {
        MenuItem mi;
        mi.label = "Add Module..."; mi.enabled = true; mi.separator = false;
        mi.action = [this]() {
            if ( on_add_module ) on_add_module( m_menu_cx, m_menu_cy, std::string() );
        };
        m_menu_items.push_back( mi );
    }

    m_menu_x = sx; m_menu_y = sy;
    m_menu_open = true;
    layout_menu( app );
}

void PatchView::open_node_menu( ui::App& app, int sx, int sy, NodeId id )
{
    m_menu_items.clear();
    Node* nd = find_node( id );

    { MenuItem ti; ti.label = nd ? nd->name : std::string( "Node" );
      ti.enabled = false; ti.separator = false; m_menu_items.push_back( ti ); }
    { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
    {
        MenuItem mi; mi.label = "Open Editor"; mi.enabled = true; mi.separator = false;
        mi.action = [this, id]() { if ( on_open_editor ) on_open_editor( id ); };
        m_menu_items.push_back( mi );
    }
    {
        MenuItem mi; mi.label = "Remove"; mi.enabled = true; mi.separator = false;
        mi.action = [this, id]() { remove_node( id ); };   // fires on_remove_node
        m_menu_items.push_back( mi );
    }

    m_menu_x = sx; m_menu_y = sy;
    m_menu_open = true;
    layout_menu( app );
}

void PatchView::close_menu()
{
    m_menu_open = false;
    m_menu_items.clear();
}

void PatchView::draw_menu( ui::App& app )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int rowh = app.mono.ch() + 6;

    SDL_Rect frame { m_menu_x, m_menu_y, m_menu_w, m_menu_h };
    ui::fill_rect( r, frame, t.panel );
    ui::frame_rect( r, frame, t.hi );

    // hover row (from current mouse position)
    int mx, my; SDL_GetMouseState( &mx, &my );

    for ( std::size_t i = 0; i < m_menu_items.size(); ++i )
    {
        const MenuItem& mi = m_menu_items[i];
        int ry = m_menu_y + 2 + int(i) * rowh;
        if ( mi.separator )
        {
            ui::hline( r, m_menu_x + 4, m_menu_x + m_menu_w - 4,
                       ry + rowh/2, t.dim );
            continue;
        }
        SDL_Rect row { m_menu_x + 1, ry, m_menu_w - 2, rowh };
        bool hover = mi.enabled &&
                     mx >= row.x && mx < row.x + row.w &&
                     my >= row.y && my < row.y + row.h;
        if ( hover ) ui::fill_rect( r, row, t.accent );
        Color fg = !mi.enabled ? t.dim : ( hover ? t.bg : t.hi );
        app.mono.draw( r, m_menu_x + 10, ry + 3, mi.label, fg );
    }
}

// ============================================================================
//  Input
// ============================================================================
bool PatchView::on_mouse( ui::App& app, const ui::MouseEv& e )
{
    const int lx = e.x - rect.x;   // canvas-local coords
    const int ly = e.y - rect.y;

    // ---- menu is open: intercept everything ----
    if ( m_menu_open )
    {
        if ( e.pressed )
        {
            const int rowh = app.mono.ch() + 6;
            bool inside = e.x >= m_menu_x && e.x < m_menu_x + m_menu_w &&
                          e.y >= m_menu_y && e.y < m_menu_y + m_menu_h;
            if ( inside )
            {
                int idx = ( e.y - ( m_menu_y + 2 ) ) / rowh;
                if ( idx >= 0 && idx < int( m_menu_items.size() ) )
                {
                    MenuItem mi = m_menu_items[idx];   // copy: action may mutate list
                    if ( mi.enabled && !mi.separator )
                    {
                        close_menu();
                        if ( mi.action ) mi.action();
                        app.request_redraw();
                        return true;
                    }
                }
            }
            close_menu();          // click elsewhere dismisses
            app.request_redraw();
            return true;
        }
        app.request_redraw();      // repaint hover on move
        return true;
    }

    // ---- right-click: open a context menu ----
    if ( e.pressed && e.button == SDL_BUTTON_RIGHT )
    {
        NodeId n = node_at( lx, ly );
        if ( n != 0 ) open_node_menu( app, e.x, e.y, n );
        else          open_add_menu( app, e.x, e.y, lx, ly );
        app.request_redraw();
        return true;
    }

    // ---- left button pressed==true.  The App shell collapses the initial
    //      press AND every drag-motion tick into the same event (button==LEFT,
    //      pressed==true), so an in-progress gesture must be updated, not
    //      restarted. ----
    if ( e.pressed && e.button == SDL_BUTTON_LEFT )
    {
        // (a) a wire drag is already running -> track the pointer + hover
        if ( m_dragging_wire )
        {
            m_drag_x = lx; m_drag_y = ly;
            PortHit ph;
            m_hover_ok = ( port_at( lx, ly, ph ) &&
                           ph.dir  == PortDir::In &&
                           ph.kind == m_drag_kind &&
                           ph.node != m_drag_src.node );
            if ( m_hover_ok ) m_hover_pt = PortRef( ph.node, ph.port );
            app.request_redraw();
            return true;
        }
        // (b) a node move is already running -> reposition it
        if ( m_moving_node )
        {
            Node* nd = find_node( m_move_id );
            if ( nd ) { nd->x = lx - m_move_dx; nd->y = ly - m_move_dy;
                        app.request_redraw(); }
            return true;
        }

        // (c) fresh press: start a wire from an OUT port
        PortHit ph;
        if ( port_at( lx, ly, ph ) && ph.dir == PortDir::Out )
        {
            m_dragging_wire = true;
            m_drag_src  = PortRef( ph.node, ph.port );
            m_drag_kind = ph.kind;
            m_drag_x = lx; m_drag_y = ly;
            m_hover_ok = false;
            app.request_redraw();
            return true;
        }
        // fresh press: move a node body (raise to top)
        NodeId n = node_at( lx, ly );
        if ( n != 0 )
        {
            Node* nd = find_node( n );
            if ( nd )
            {
                m_moving_node = true;
                m_move_id = n;
                m_move_dx = lx - int(nd->x);
                m_move_dy = ly - int(nd->y);
                raise_node( n );
                app.request_redraw();
            }
            return true;
        }
        // fresh press on empty space over a wire: delete it
        Connection hit;
        if ( connection_at( lx, ly, hit ) )
        {
            remove_connection( hit );
            app.request_redraw();
            return true;
        }
        return true;
    }

    // ---- releases arrive as pressed==false ----
    if ( !e.pressed )
    {
        // ---- left release: finish a wire ----
        if ( m_dragging_wire )
        {
            m_dragging_wire = false;
            PortHit ph;
            if ( port_at( lx, ly, ph ) && ph.dir == PortDir::In )
                add_connection( Connection( m_drag_src,
                                            PortRef( ph.node, ph.port ) ) );
            m_hover_ok = false;
            app.request_redraw();
            return true;
        }
        if ( m_moving_node )
        {
            m_moving_node = false;
            app.request_redraw();
            return true;
        }
        return false;
    }

    return false;
}

} // namespace patchbay
} // namespace seq24
