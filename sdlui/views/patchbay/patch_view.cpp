//----------------------------------------------------------------------------
//  sdlui/views/patchbay/patch_view.cpp -- SDL2 patchbay node editor.
//----------------------------------------------------------------------------
#include "patch_view.h"
#include "platform/platform_ui.h"

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

namespace PatchKnob {
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

// Click-to-connect: wire the first matching out->in port pair FOR EACH kind
// (audio + midi), so one click-click links whatever the two nodes share -- e.g.
// MIDI-In -> Instrument (midi), Instrument -> Out (audio), or an instrument into
// a node that takes both.  add_connection() validates kind/self/duplicate.
void PatchView::auto_connect_nodes( NodeId x, NodeId y )
{
    if ( x == y ) return;
    Node* a = find_node( x );
    Node* b = find_node( y );
    if ( !a || !b ) return;

    auto first = []( const std::vector<Port>& ports, PortKind k ) -> const Port* {
        for ( const Port& p : ports ) if ( p.kind == k ) return &p;
        return nullptr;
    };
    // For each kind, connect whichever DIRECTION has a valid out->in pair, so it
    // doesn't matter whether the user clicked the source or the sink first.
    const PortKind kinds[2] = { PortKind::Audio, PortKind::Midi };
    for ( PortKind k : kinds )
    {
        const Port* aOut = first( a->outPorts, k );
        const Port* aIn  = first( a->inPorts,  k );
        const Port* bOut = first( b->outPorts, k );
        const Port* bIn  = first( b->inPorts,  k );
        if ( aOut && bIn )
            add_connection( Connection( PortRef( x, aOut->id ), PortRef( y, bIn->id ) ) );
        else if ( bOut && aIn )
            add_connection( Connection( PortRef( y, bOut->id ), PortRef( x, aIn->id ) ) );
    }
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

    clamp_pan();   // keep the camera within the graph (node moves / resizes / zoom)

    // Clip to the widget so nodes can't bleed past the canvas edges.
    SDL_Rect clip = rect;
    {
    ui::ScopedClip clipScope(r,clip);

    // Committed connections (behind the nodes).
    for ( std::size_t i = 0; i < m_conns.size(); ++i )
        draw_connection( app, m_conns[i] );

    // In-flight drag wire.
    if ( m_dragging_wire )
        draw_drag_wire( app );

    // Nodes on top (painter's order == vector order; last == topmost).
    for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        draw_node( app, m_nodes[i] );

    draw_scrollbars( app );          // overlay pan bars + zoom read-out
    }

    if ( m_menu_open )
        draw_menu( app );
}

void PatchView::draw_node( ui::App& app, const Node& n )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int h  = node_height( n );

    SDL_Rect box { wsx( n.x ), wsy( n.y ), wsc( NODE_W ), wsc( h ) };

    // Body + outline.  No selection/move highlight -- a node looks the same whether
    // or not it's selected / being dragged (per user request).
    fill_round_rect( r, box, CORNER, t.panel );
    frame_round_rect( r, box, CORNER, t.hi );

    // Title strip (top rounded; clip a rounded rect and paint the top band).
    const int th = wsc( TITLE_H );
    SDL_Rect titleClip { box.x, box.y, box.w, th };
    {
        ui::ScopedClip titleScope(r,titleClip);
        SDL_Rect titleFull { box.x, box.y, box.w, th + wsc( CORNER ) };
        fill_round_rect( r, titleFull, wsc( CORNER ), t.accent );
    }

    // Title text -- fitted so it scales / clips with the zoomed box.
    app.mono.draw_fitted( r, SDL_Rect{ box.x + wsc(7), box.y, box.w - wsc(14), th },
                          n.name, t.bg, false );

    const int pr = wsc( PORT_R );
    // The virtual MIDI node routes in-N directly to out-N internally.  Show those
    // invisible routes as compact, colour-coded paths inside the node instead of
    // requiring misleading external patch cables.
    if ( n.name == "Instrument Virtual MIDI Ports" )
    {
        static const ui::Color pathColors[] = {
            {239, 83, 80, 255}, {255, 183, 77, 255}, {102, 187, 106, 255},
            {38, 198, 218, 255}, {66, 165, 245, 255}, {171, 71, 188, 255}
        };
        const std::vector<std::pair<int,int>> routes =
            virtual_midi_routes ? virtual_midi_routes(n.id)
                                : std::vector<std::pair<int,int>>();
        for ( std::size_t k = 0; k < routes.size(); ++k )
        {
            const int inIndex=routes[k].first, outIndex=routes[k].second;
            if(inIndex<0||outIndex<0||inIndex>=(int)n.inPorts.size()||
               outIndex>=(int)n.outPorts.size()) continue;
            int ix, iy, ox, oy;
            port_center(n, PortDir::In, (size_t)inIndex, ix, iy);
            port_center(n, PortDir::Out, (size_t)outIndex, ox, oy);
            const ui::Color c = pathColors[outIndex % (sizeof(pathColors)/sizeof(pathColors[0]))];
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
            const int leftText = app.mono.text_w(n.inPorts[(size_t)inIndex].name);
            const int rightText = app.mono.text_w(n.outPorts[(size_t)outIndex].name);
            const int x1=wsx(ix)+pr+6+leftText;
            const int x2=wsx(ox)-pr-6-rightText;
            const int y1=wsy(iy), y2=wsy(oy), xm=(x1+x2)/2;
            if (x2>x1) {
                SDL_RenderDrawLine(r,x1,y1,xm,y1);
                SDL_RenderDrawLine(r,xm,y1,xm,y2);
                SDL_RenderDrawLine(r,xm,y2,x2,y2);
            }
        }
    }
    // Input ports + left-aligned labels.
    for ( std::size_t k = 0; k < n.inPorts.size(); ++k )
    {
        int wx, wy; port_center( n, PortDir::In, k, wx, wy );
        const int cx = wsx( wx ), cy = wsy( wy );
        bool hi = m_dragging_wire && m_hover_ok &&
                  m_hover_pt.node == n.id && m_hover_pt.port == n.inPorts[k].id;
        draw_port( app, cx, cy, n.inPorts[k].kind, hi );
        app.mono.draw( r, cx + pr + 4, cy - app.mono.ch()/2,
                       n.inPorts[k].name, t.hi );
    }
    // Output ports + right-aligned labels.
    for ( std::size_t k = 0; k < n.outPorts.size(); ++k )
    {
        int wx, wy; port_center( n, PortDir::Out, k, wx, wy );
        const int cx = wsx( wx ), cy = wsy( wy );
        draw_port( app, cx, cy, n.outPorts[k].kind, false );
        int tw = app.mono.text_w( n.outPorts[k].name );
        app.mono.draw( r, cx - pr - 4 - tw, cy - app.mono.ch()/2,
                       n.outPorts[k].name, t.hi );
    }
}

void PatchView::draw_port( ui::App& app, int cx, int cy, PortKind kind, bool hi )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int pr = wsc( PORT_R );               // scale the marker with the zoom

    if ( hi )                                   // valid drop target halo
        circle_ring( r, cx, cy, pr + 3, t.active );

    if ( kind == PortKind::Audio )              // FILLED
    {
        fill_disc( r, cx, cy, pr, t.hi );
    }
    else                                        // MIDI: HOLLOW
    {
        fill_disc( r, cx, cy, pr, t.panel );
        circle_ring( r, cx, cy, pr, t.hi );
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

    stroke_bezier( app.ren, wsx( x1 ), wsy( y1 ), wsx( x2 ), wsy( y2 ),
                   theme().dim, midi ? 1 : 2, midi );
}

void PatchView::draw_drag_wire( ui::App& app )
{
    int x1, y1;
    if ( !ref_center( m_drag_src, PortDir::Out, x1, y1 ) ) return;

    // m_drag_x/y are world coords (pointer mapped through the camera in on_mouse).
    stroke_bezier( app.ren, wsx( x1 ), wsy( y1 ),
                   wsx( m_drag_x ), wsy( m_drag_y ),
                   m_hover_ok ? theme().active : theme().sel, 2, true );
}

// ============================================================================
//  Camera: pan + zoom + scrollbars
// ============================================================================
void PatchView::content_bounds( double& minx, double& miny,
                                double& maxx, double& maxy ) const
{
    if ( m_nodes.empty() ) { minx = miny = 0; maxx = rect.w; maxy = rect.h; return; }
    minx = miny = 1e9; maxx = maxy = -1e9;
    for ( const Node& n : m_nodes ) {
        minx = std::min( minx, (double)n.x );
        miny = std::min( miny, (double)n.y );
        maxx = std::max( maxx, (double)n.x + NODE_W );
        maxy = std::max( maxy, (double)n.y + node_height( n ) );
    }
    minx -= 48; miny -= 48; maxx += 48; maxy += 48;   // slack around the graph
}

void PatchView::clamp_pan()
{
    double minx, miny, maxx, maxy; content_bounds( minx, miny, maxx, maxy );
    const double visW = rect.w / m_zoom, visH = rect.h / m_zoom;
    const double maxpx = std::max( minx, maxx - visW );
    const double maxpy = std::max( miny, maxy - visH );
    if ( m_pan_x < minx ) m_pan_x = minx;   else if ( m_pan_x > maxpx ) m_pan_x = maxpx;
    if ( m_pan_y < miny ) m_pan_y = miny;   else if ( m_pan_y > maxpy ) m_pan_y = maxpy;
}

void PatchView::draw_scrollbars( ui::App& app )
{
    SDL_Renderer* r = app.ren; const ui::Theme& t = theme();
    double minx, miny, maxx, maxy; content_bounds( minx, miny, maxx, maxy );
    const double contentW = maxx - minx, contentH = maxy - miny;
    const double visW = rect.w / m_zoom, visH = rect.h / m_zoom;
    m_hbar = SDL_Rect{0,0,0,0}; m_vbar = SDL_Rect{0,0,0,0};

    if ( contentW > visW + 1.0 ) {
        const int trackY = rect.y + rect.h - SCROLLBAR;
        const int trackW = rect.w - SCROLLBAR;
        ui::fill_rect( r, SDL_Rect{ rect.x, trackY, trackW, SCROLLBAR }, t.panel );
        int thumbW = std::max( 24, (int)( trackW * visW / contentW ) );
        if ( thumbW > trackW ) thumbW = trackW;
        double f = ( m_pan_x - minx ) / std::max( 1.0, contentW - visW );
        f = f < 0 ? 0 : ( f > 1 ? 1 : f );
        m_hbar = SDL_Rect{ rect.x + (int)( f * ( trackW - thumbW ) ), trackY, thumbW, SCROLLBAR };
        ui::fill_rect( r, m_hbar, t.hi ); ui::frame_rect( r, m_hbar, t.dim );
    }
    if ( contentH > visH + 1.0 ) {
        const int trackX = rect.x + rect.w - SCROLLBAR;
        const int trackH = rect.h - SCROLLBAR;
        ui::fill_rect( r, SDL_Rect{ trackX, rect.y, SCROLLBAR, trackH }, t.panel );
        int thumbH = std::max( 24, (int)( trackH * visH / contentH ) );
        if ( thumbH > trackH ) thumbH = trackH;
        double f = ( m_pan_y - miny ) / std::max( 1.0, contentH - visH );
        f = f < 0 ? 0 : ( f > 1 ? 1 : f );
        m_vbar = SDL_Rect{ trackX, rect.y + (int)( f * ( trackH - thumbH ) ), SCROLLBAR, thumbH };
        ui::fill_rect( r, m_vbar, t.hi ); ui::frame_rect( r, m_vbar, t.dim );
    }

    // zoom read-out (bottom-left)
    char z[16]; std::snprintf( z, sizeof(z), "%d%%", (int)std::lround( m_zoom * 100 ) );
    app.mono.draw( r, rect.x + 5, rect.y + rect.h - SCROLLBAR - app.mono.ch() - 3, z, t.dim );
}

bool PatchView::on_wheel( ui::App& app, int dx, int dy )
{
    // With a menu up the wheel used to zoom/pan the canvas underneath it, which
    // left the popup hanging over unrelated nodes.  Scroll the MENU instead --
    // which is also how the (now capped) long MIDI-port list is reached.
    if ( m_menu_open ) {
        m_menu_scroll -= dy;
        const int visible = menu_visible_rows( app, m_menu_h );
        const int max_scroll = std::max( 0, int( m_menu_items.size() ) - visible );
        if ( m_menu_scroll > max_scroll ) m_menu_scroll = max_scroll;
        if ( m_menu_scroll < 0 ) m_menu_scroll = 0;
        app.request_redraw();
        return true;
    }

    const SDL_Keymod mod = (SDL_Keymod)SDL_GetModState();
    if((mod&KMOD_CTRL)||(ui::platform::patcher_plain_wheel_zooms()&&dy!=0)) {
        int mx = 0, my = 0; ui::mouse_logical( app, mx, my );   // logical px
        const double wx = sw_x( mx ), wy = sw_y( my );
        double z = m_zoom * ( dy > 0 ? 1.1 : ( dy < 0 ? 1.0 / 1.1 : 1.0 ) );
        if ( z < 0.35 ) z = 0.35;
        if ( z > 2.5 )  z = 2.5;
        m_zoom = z;
        m_pan_x = wx - ( mx - rect.x ) / m_zoom;    // keep the world point under the cursor
        m_pan_y = wy - ( my - rect.y ) / m_zoom;
    } else if ( mod & KMOD_SHIFT ) {
        m_pan_x -= dy * 40.0 / m_zoom;              // shift-wheel = horizontal pan
    } else {
        m_pan_y -= dy * 40.0 / m_zoom;
        m_pan_x += dx * 40.0 / m_zoom;
    }
    clamp_pan();
    app.request_redraw();
    return true;
}

// ============================================================================
//  Context menu (SDL has no native popups -- draw an in-canvas overlay)
// ============================================================================
void PatchView::layout_menu( ui::App& app )
{
    const int rowh = app.mono.ch() + 6;
    int wmax = 0;
    for ( std::size_t i = 0; i < m_menu_items.size(); ++i )
        wmax = std::max( wmax, app.mono.text_w( m_menu_items[i].label ) );

    // Clamp to the VIEW, not the window: the menu used to be positioned against
    // app.w/app.h and could therefore be painted over neighbouring panels.
    const int availW = std::max( 80, rect.w - 8 );
    const int availH = std::max( rowh + 4, rect.h - 8 );

    // The width was unbounded -- a long hardware MIDI port name pushed the menu
    // clean off the right edge (m_menu_x went negative, then snapped to 0 and
    // the box overflowed instead).
    m_menu_w = std::min( wmax + 20, availW );

    // The height was unbounded too, so a machine with many MIDI ports drew a
    // menu taller than the window whose lower entries could never be clicked.
    // Cap it and scroll instead.
    const int total_h = int( m_menu_items.size() ) * rowh + 4;
    m_menu_h = std::min( total_h, availH );

    if ( m_menu_x + m_menu_w > rect.x + rect.w ) m_menu_x = rect.x + rect.w - m_menu_w;
    if ( m_menu_y + m_menu_h > rect.y + rect.h ) m_menu_y = rect.y + rect.h - m_menu_h;
    if ( m_menu_x < rect.x ) m_menu_x = rect.x;
    if ( m_menu_y < rect.y ) m_menu_y = rect.y;

    const int visible = menu_visible_rows( app, m_menu_h );
    const int max_scroll = std::max( 0, int( m_menu_items.size() ) - visible );
    if ( m_menu_scroll > max_scroll ) m_menu_scroll = max_scroll;
    if ( m_menu_scroll < 0 ) m_menu_scroll = 0;
}

// Rows the box can actually show -- draw, hit-test and scroll clamping all use
// this one definition so they cannot disagree.
int PatchView::menu_visible_rows( ui::App& app, int h ) const
{
    const int rowh = app.mono.ch() + 6;
    if ( rowh <= 0 ) return 0;
    return std::max( 0, ( h - 4 ) / rowh );
}

void PatchView::open_add_menu( ui::App& app, int sx, int sy, double cx, double cy )
{
    m_menu_items.clear();
    m_menu_cx = cx; m_menu_cy = cy;

    static const char* cats[] =
        { "Instrument", "Sampler", "Effect", "MIDI", "Audio I/O", "Mixer", "Pure Data",
          "Modular (Rack)", "Csound" };
    for ( unsigned i = 0; i < sizeof(cats)/sizeof(cats[0]); ++i )
    {
        std::string cat = cats[i];
        MenuItem mi;
        mi.enabled = true; mi.separator = false;
        if ( cat == "Audio I/O" ) {
            // Audio I/O is two different nodes; picking the category alone can
            // only ever mean one of them, and it always meant Out.
            mi.label = "Audio I/O";
            const int msx = sx, msy = sy; const double mcx = cx, mcy = cy;
            mi.submenu = [this, msx, msy, mcx, mcy](ui::App& a) {
                open_audio_io_menu( a, msx, msy, mcx, mcy );
            };
        } else if ( cat == "MIDI" ) {
            // MIDI opens a submenu of virtual + hardware in/out ports.
            mi.label = "MIDI Ports";        // opens a persistent child menu
            // Open the submenu where THIS menu is (m_menu_x/y aren't assigned until
            // after this loop, so capturing them here gave a stale/zero position that
            // scattered the submenu across the canvas).
            const int msx = sx, msy = sy; const double mcx = cx, mcy = cy;
            mi.submenu = [this, msx, msy, mcx, mcy](ui::App& a) {
                open_midi_menu( a, msx, msy, mcx, mcy );
            };
        } else {
            mi.label = std::string( "Add " ) + cat;
            mi.action = [this, cat]() {
                if ( on_add_module ) on_add_module( m_menu_cx, m_menu_cy, cat );
            };
        }
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
    m_menu_scroll = 0;
    m_menu_open = true;
    layout_menu( app );
}

// Audio device in / out.  Both are plain builtin nodes; the shell decides which
// engine node each label maps to (see on_add_module).
void PatchView::open_audio_io_menu( ui::App& app, int sx, int sy, double cx, double cy )
{
    m_menu_items.clear();
    m_menu_cx = cx; m_menu_cy = cy;
    auto add = [this]( const std::string& label, const std::string& cat ) {
        MenuItem mi; mi.label = label; mi.enabled = true; mi.separator = false;
        mi.action = [this, cat]() {
            if ( on_add_module ) on_add_module( m_menu_cx, m_menu_cy, cat );
        };
        m_menu_items.push_back( mi );
    };
    add( "Add Audio In",  "Audio In"  );
    add( "Add Audio Out", "Audio Out" );
    add( "Add Mono to Stereo", "Mono to Stereo" );

    m_menu_x = sx; m_menu_y = sy;
    m_menu_scroll = 0;
    m_menu_open = true;
    layout_menu( app );
}

// PipeWire-style MIDI port picker: virtual in/out + every hardware in/out port.
void PatchView::open_midi_menu( ui::App& app, int sx, int sy, double cx, double cy )
{
    m_menu_items.clear();
    m_menu_cx = cx; m_menu_cy = cy;
    auto add_port = [this]( const std::string& label, int dir, int hw ) {
        MenuItem mi; mi.label = label; mi.enabled = true; mi.separator = false;
        mi.action = [this, dir, hw]() {
            if ( on_add_midi_port ) on_add_midi_port( m_menu_cx, m_menu_cy, dir, hw );
        };
        m_menu_items.push_back( mi );
    };
    auto sep = [this](){ MenuItem s; s.enabled=false; s.separator=true; m_menu_items.push_back(s); };
    auto hdr = [this]( const char* t ){ MenuItem h; h.label=t; h.enabled=false; m_menu_items.push_back(h); };

    hdr( "Virtual" );
    add_port( "  MIDI In (virtual)",  0, -1 );
    add_port( "  MIDI Out (virtual)", 1, -1 );

    std::vector<std::string> ins  = midi_devices     ? midi_devices()     : std::vector<std::string>();
    std::vector<std::string> outs = midi_out_devices ? midi_out_devices() : std::vector<std::string>();
    if ( !ins.empty() ) {
        sep(); hdr( "Hardware In" );
        for ( int i = 0; i < (int) ins.size(); ++i )
            add_port( std::string("  ") + ins[i], 0, i );
    }
    if ( !outs.empty() ) {
        sep(); hdr( "Hardware Out" );
        for ( int i = 0; i < (int) outs.size(); ++i )
            add_port( std::string("  ") + outs[i], 1, i );
    }

    m_menu_x = sx; m_menu_y = sy;
    m_menu_scroll = 0;
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

    const bool isMidiSource = nd && nd->category == "MIDI";
    const bool isPlugin     = nd && ( nd->category == "Instrument" || nd->category == "Effect" );
    const bool isMixer      = nd && nd->category == "Mixer";
    const bool isPd         = nd && nd->category == "Pure Data";
    const bool isRack       = nd && nd->category == "Modular (Rack)";
    const bool isSampler    = nd && nd->category == "Sampler";
    const bool isCsound     = nd && nd->category == "Csound";
    const bool isVirtualMidi = nd && nd->name == "Instrument Virtual MIDI Ports";
    if ( isCsound ) {
        MenuItem mi; mi.label = "Edit CSD";
        mi.action = [this, id]() { if ( on_open_csound ) on_open_csound( id ); };
        m_menu_items.push_back( mi );
    } else if ( isSampler ) {
        { MenuItem mi; mi.label = "Open Sampler Editor";
          mi.action = [this, id]() { if ( on_open_sampler ) on_open_sampler( id ); };
          m_menu_items.push_back( mi ); }
        { MenuItem mi; mi.label = "Parameters";
          mi.action = [this, id]() { if ( on_open_params ) on_open_params( id ); };
          m_menu_items.push_back( mi ); }
    } else if ( isPd ) {
        MenuItem mi; mi.label = "Open Pd Editor";
        mi.action = [this, id]() { if ( on_open_pd ) on_open_pd( id ); };
        m_menu_items.push_back( mi );
    } else if ( isRack ) {
        { MenuItem mi; mi.label = "Open Rack Editor";
          mi.action = [this, id]() { if ( on_open_rack ) on_open_rack( id ); };
          m_menu_items.push_back( mi ); }
        { MenuItem sep; sep.enabled = false; sep.separator = true; m_menu_items.push_back( sep ); }
        { MenuItem hdr; hdr.label = "Polyphony:"; hdr.enabled = false; m_menu_items.push_back( hdr ); }
        const int polys[] = { 1, 2, 4, 8, 16 };
        for ( int p : polys )
        {
            MenuItem mi;
            mi.label = std::string( "  " ) + std::to_string( p ) + ( p == 1 ? " (mono)" : " voices" );
            mi.action = [this, id, p]() { if ( on_set_rack_poly ) on_set_rack_poly( id, p ); };
            m_menu_items.push_back( mi );
        }
    } else if ( isMixer ) {
        MenuItem mi; mi.label = "Open Mixer";
        mi.action = [this, id]() { if ( on_open_mixer ) on_open_mixer( id ); };
        m_menu_items.push_back( mi );
    } else if ( isVirtualMidi ) {
        MenuItem mi; mi.label = "Configure MIDI Ports...";
        mi.action = [this,id](){ if(on_config_virtual_midi) on_config_virtual_midi(id); };
        m_menu_items.push_back(mi);
    } else if ( isMidiSource ) {
        { MenuItem hdr; hdr.label = "Virtual ports:"; hdr.enabled = false; m_menu_items.push_back(hdr); }
        const int counts[] = { 1, 2, 4, 8, 16 };
        for (int n : counts) {
            MenuItem mi; mi.label = "  " + std::to_string(n);
            mi.action = [this, id, n]() { if (on_set_midi_ports) on_set_midi_ports(id, n); };
            m_menu_items.push_back(mi);
        }
        { MenuItem sep; sep.separator = true; sep.enabled = false; m_menu_items.push_back(sep); }
        // MIDI-In node: pick a hardware MIDI input device (no instrument options).
        std::vector<std::string> devs = midi_devices ? midi_devices() : std::vector<std::string>();
        { MenuItem mi; mi.label = "MIDI Input:"; mi.enabled = false; m_menu_items.push_back( mi ); }
        { MenuItem mi; mi.label = "  None"; mi.enabled = true;
          mi.action = [this]() { if ( on_select_midi ) on_select_midi( -1 ); };
          m_menu_items.push_back( mi ); }
        for ( int i = 0; i < (int) devs.size(); ++i ) {
            MenuItem mi; mi.label = std::string( "  " ) + devs[i]; mi.enabled = true;
            int di = i;
            mi.action = [this, di]() { if ( on_select_midi ) on_select_midi( di ); };
            m_menu_items.push_back( mi );
        }
        { MenuItem sep; sep.separator = true; sep.enabled = false; m_menu_items.push_back( sep ); }
    } else if ( isPlugin ) {
        { MenuItem mi; mi.label = "Load Plugin from Disk...";
          mi.action = [this, id]() { if ( on_load_plugin_file ) on_load_plugin_file( id ); };
          m_menu_items.push_back( mi ); }
        // Instrument / Effect node: plugin chooser + editor.
        { MenuItem mi; mi.label = "Choose Plugin...";
          mi.action = [this, id]() { if ( on_open_editor ) on_open_editor( id ); };
          m_menu_items.push_back( mi ); }
        { MenuItem mi; mi.label = "Open GUI";
          mi.action = [this, id]() { if ( on_open_gui ) on_open_gui( id ); };
          m_menu_items.push_back( mi ); }
        { MenuItem mi; mi.label = "Parameters";
          mi.action = [this, id]() { if ( on_open_params ) on_open_params( id ); };
          m_menu_items.push_back( mi ); }
    }
    // (other node kinds: only Remove, below)
    {
        MenuItem mi; mi.label = "Remove"; mi.enabled = true; mi.separator = false;
        mi.action = [this, id]() { remove_node( id ); };   // fires on_remove_node
        m_menu_items.push_back( mi );
    }

    m_menu_x = sx; m_menu_y = sy;
    m_menu_scroll = 0;
    m_menu_open = true;
    layout_menu( app );
}

void PatchView::close_menu()
{
    m_menu_open = false;
    m_menu_items.clear();
    m_menu_scroll = 0;
    m_parent_menu_open = false;
    m_parent_menu_items.clear();
}

// The context menu was mouse-only: nothing on the keyboard dismissed it, so Esc
// went to the canvas (clearing a pending link) while the popup stayed up.
bool PatchView::on_key( ui::App& app, SDL_Keycode k )
{
    if ( m_menu_open ) {
        if ( k == SDLK_ESCAPE ) { close_menu(); app.request_redraw(); return true; }
        return true;                       // a popup is modal; swallow the rest
    }
    if ( k == SDLK_ESCAPE && m_link_src != 0 ) {
        m_link_src = 0; app.request_redraw(); return true;
    }
    return false;
}

void PatchView::draw_menu( ui::App& app )
{
    SDL_Renderer* r = app.ren;
    const ui::Theme& t = theme();
    const int rowh = app.mono.ch() + 6;

    // Hover must be tested in LOGICAL coords: SDL_GetMouseState() answers in raw
    // window pixels, so on any scaled display the highlight sat on a different
    // row than the one a click would take.
    int mx = 0, my = 0; ui::mouse_logical( app, mx, my );

    auto draw_level=[&](const std::vector<MenuItem>& items,int x,int y,int w,int h,
                        int scroll) {
        SDL_Rect f{x,y,w,h}; ui::fill_rect(r,f,t.panel); ui::frame_rect(r,f,t.hi);
        const int visible = menu_visible_rows( app, h );
        for(int vr=0;vr<visible;++vr) {
            const int idx=scroll+vr;
            if(idx<0||idx>=(int)items.size()) break;
            const MenuItem& mi=items[(std::size_t)idx]; const int ry=y+2+vr*rowh;
            if(mi.separator) { ui::hline(r,x+4,x+w-4,ry+rowh/2,t.dim); continue; }
            SDL_Rect row{x+1,ry,w-2,rowh};
            const bool hover=mi.enabled&&mx>=row.x&&mx<row.x+row.w&&my>=row.y&&my<row.y+row.h;
            if(hover) ui::fill_rect(r,row,t.accent);
            const Color fg=!mi.enabled?t.dim:(hover?t.bg:t.hi);
            SDL_Rect tx{x+10,ry+3,w-20-(mi.submenu?app.mono.cw()+8:0),rowh-3};
            app.mono.draw_fitted(r,tx,mi.label,fg,false);
            if(mi.submenu) app.mono.draw(r,x+w-app.mono.cw()-8,ry+3,">",fg);
        }
        if(scroll>0) app.mono.draw(r,x+w-12,y+2,"^",t.dim);
        if(scroll+visible<(int)items.size())
            app.mono.draw(r,x+w-12,y+h-app.mono.ch()-2,"v",t.dim);
    };
    if(m_parent_menu_open)
        draw_level(m_parent_menu_items,m_parent_menu_x,m_parent_menu_y,
                   m_parent_menu_w,m_parent_menu_h,0);

    draw_level(m_menu_items,m_menu_x,m_menu_y,m_menu_w,m_menu_h,m_menu_scroll);
}

// ============================================================================
//  Input
// ============================================================================
bool PatchView::on_mouse( ui::App& app, const ui::MouseEv& e )
{
    // Pointer -> WORLD coords through the camera (pan + zoom); all hit-testing and
    // node geometry below is in world units.
    const int lx = (int)std::lround( sw_x( e.x ) );
    const int ly = (int)std::lround( sw_y( e.y ) );

    // ---- scrollbars (screen space; before world hit-testing) ---------------
    if ( e.pressed && e.button == SDL_BUTTON_LEFT && !m_menu_open &&
         !m_dragging_wire && !m_moving_node )
    {
        auto panFromBar = [&]{
            double minx, miny, maxx, maxy; content_bounds( minx, miny, maxx, maxy );
            if ( m_drag_hbar && m_hbar.w > 0 ) {
                const int trackW = rect.w - SCROLLBAR;
                double f = double( e.x - m_bar_grab - rect.x ) / std::max( 1, trackW - m_hbar.w );
                f = f < 0 ? 0 : ( f > 1 ? 1 : f );
                const double visW = rect.w / m_zoom;
                m_pan_x = minx + f * std::max( 0.0, ( maxx - minx ) - visW );
            }
            if ( m_drag_vbar && m_vbar.h > 0 ) {
                const int trackH = rect.h - SCROLLBAR;
                double f = double( e.y - m_bar_grab - rect.y ) / std::max( 1, trackH - m_vbar.h );
                f = f < 0 ? 0 : ( f > 1 ? 1 : f );
                const double visH = rect.h / m_zoom;
                m_pan_y = miny + f * std::max( 0.0, ( maxy - miny ) - visH );
            }
            clamp_pan(); app.request_redraw();
        };
        auto inR = []( const SDL_Rect& q, int px, int py ){
            return px >= q.x && px < q.x + q.w && py >= q.y && py < q.y + q.h; };
        if ( m_drag_hbar || m_drag_vbar ) { panFromBar(); return true; }
        if ( m_hbar.w > 0 && inR( m_hbar, e.x, e.y ) ) {
            m_drag_hbar = true; m_bar_grab = e.x - m_hbar.x; return true; }
        if ( m_vbar.h > 0 && inR( m_vbar, e.x, e.y ) ) {
            m_drag_vbar = true; m_bar_grab = e.y - m_vbar.y; return true; }
    }
    if ( !e.pressed ) { m_drag_hbar = false; m_drag_vbar = false; }

    // ---- menu is open: intercept everything ----
    if ( m_menu_open )
    {
        if ( e.pressed )
        {
            const int rowh = app.mono.ch() + 6;
            const int visible = menu_visible_rows( app, m_menu_h );
            bool inside = e.x >= m_menu_x && e.x < m_menu_x + m_menu_w &&
                          e.y >= m_menu_y + 2 && e.y < m_menu_y + 2 + visible * rowh;
            if ( inside )
            {
                int idx = m_menu_scroll + ( e.y - ( m_menu_y + 2 ) ) / rowh;
                if ( idx >= 0 && idx < int( m_menu_items.size() ) )
                {
                    MenuItem mi = m_menu_items[idx];   // copy: action may mutate list
                    if ( mi.enabled && !mi.separator )
                    {
                        if(mi.submenu) {
                            m_parent_menu_items=m_menu_items;
                            m_parent_menu_x=m_menu_x; m_parent_menu_y=m_menu_y;
                            m_parent_menu_w=m_menu_w; m_parent_menu_h=m_menu_h;
                            m_parent_menu_open=true;
                            mi.submenu(app);
                            m_menu_scroll=0;
                            // Size the CHILD first: the side-placement test used
                            // to run against the parent's width (m_menu_w still
                            // held the old menu), so a wider submenu was pushed
                            // off the edge or straight over its parent.
                            m_menu_x=m_parent_menu_x; m_menu_y=m_parent_menu_y;
                            layout_menu(app);
                            m_menu_x=(m_parent_menu_x+m_parent_menu_w+4+m_menu_w
                                      <=rect.x+rect.w)
                                ?m_parent_menu_x+m_parent_menu_w+4
                                :m_parent_menu_x-m_menu_w-4;
                            m_menu_y=m_parent_menu_y;
                            layout_menu(app);
                            app.request_redraw();
                            return true;
                        }
                        close_menu();
                        if ( mi.action ) mi.action();
                        app.request_redraw();
                        return true;
                    }
                }
            }
            if(m_parent_menu_open) {
                const bool pin=e.x>=m_parent_menu_x&&e.x<m_parent_menu_x+m_parent_menu_w&&
                    e.y>=m_parent_menu_y&&e.y<m_parent_menu_y+m_parent_menu_h;
                if(pin) {
                    const int pvis=menu_visible_rows(app,m_parent_menu_h);
                    const int prow=(e.y-(m_parent_menu_y+2))/rowh;
                    const int idx=(prow>=0&&prow<pvis)?prow:-1;
                    if(idx>=0&&idx<(int)m_parent_menu_items.size()) {
                        MenuItem mi=m_parent_menu_items[(size_t)idx];
                        if(mi.enabled&&!mi.separator) {
                            if(mi.submenu) {
                                mi.submenu(app);
                                m_menu_scroll=0;
                                m_menu_x=m_parent_menu_x; m_menu_y=m_parent_menu_y;
                                layout_menu(app);        // size the child FIRST
                                m_menu_x=(m_parent_menu_x+m_parent_menu_w+4+m_menu_w
                                          <=rect.x+rect.w)
                                    ?m_parent_menu_x+m_parent_menu_w+4
                                    :m_parent_menu_x-m_menu_w-4;
                                m_menu_y=m_parent_menu_y; layout_menu(app);
                                app.request_redraw(); return true;
                            }
                            close_menu(); if(mi.action) mi.action();
                            app.request_redraw(); return true;
                        }
                    }
                }
            }
            close_menu();          // click outside both levels dismisses
            app.request_redraw();
            return true;
        }
        app.request_redraw();      // repaint hover on move
        return true;
    }

    // ---- right-click: open a context menu (also cancels a pending link) ----
    if ( e.pressed && e.button == SDL_BUTTON_RIGHT )
    {
        m_link_src = 0;
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
            m_link_src = 0;                 // starting a wire drag: cancel any arm
            m_dragging_wire = true;
            m_drag_src  = PortRef( ph.node, ph.port );
            m_drag_kind = ph.kind;
            m_drag_x = lx; m_drag_y = ly;
            m_hover_ok = false;
            app.request_redraw();
            return true;
        }
        // fresh press on a node body.
        NodeId n = node_at( lx, ly );
        if ( n != 0 )
        {
            // Click-to-connect: if another node is already armed (lit), pressing
            // a DIFFERENT node completes the wire right here -- connecting on the
            // press is robust (no dependence on release timing or click jitter).
            if ( m_link_src != 0 && m_link_src != n )
            {
                auto_connect_nodes( m_link_src, n );
                m_link_src = 0;
                app.request_redraw();
                return true;
            }
            // Otherwise SELECT + light up this node immediately, and allow a drag
            // to move it.  A no-move release leaves it armed for the next click.
            Node* nd = find_node( n );
            if ( nd )
            {
                m_link_src = n;              // lit on click (immediate feedback)
                m_moving_node = true;
                m_move_id = n;
                m_move_dx = lx - int(nd->x);
                m_move_dy = ly - int(nd->y);
                m_press_lx = lx; m_press_ly = ly;
                raise_node( n );
                app.request_redraw();
            }
            return true;
        }
        // fresh press on empty space: cancel any armed click-to-connect, and if
        // it landed on a wire, delete it.
        m_link_src = 0;
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
            // If the node was actually dragged, it was a MOVE (not a select), so
            // drop the arm.  A no-move release keeps it armed/lit for the next
            // node click to connect to.
            const int moved = std::abs( lx - m_press_lx ) + std::abs( ly - m_press_ly );
            if ( moved > 4 ) m_link_src = 0;
            app.request_redraw();
            return true;
        }
        return false;
    }

    return false;
}

} // namespace patchbay
} // namespace PatchKnob
