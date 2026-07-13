//----------------------------------------------------------------------------
//
//  This file is part of seq24 (Windows port).
//
//  seq24 is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//-----------------------------------------------------------------------------
//
//  ui/patchbay/patch_canvas.h
//
//  The modular NODE-EDITOR / PATCHBAY canvas -- a Gtk::DrawingArea subclass.
//
//  HEADER-ONLY (every method inline) so, like the rack UI, the engine-coupled
//  coordinator (src/patchbayapp.cpp) can be compiled straight into seq24 by the
//  top-level src/*.cpp glob with no extra library and no edit to the frozen
//  top-level CMakeLists.  The standalone demo (patchbay_test) includes the same
//  header directly.
//
//  What it draws (strictly BLACK & WHITE, via ui/palette.h so it tracks the
//  live runtime theme):
//      * NODES as rounded boxes with a title strip; audio IN/OUT ports as
//        FILLED dots down the edges, MIDI ports as HOLLOW dots.
//      * CONNECTIONS as bezier curves from an OUT port to an IN port.
//
//  What it lets you do:
//      * Drag from an OUT port to an IN port to connect.  Only like-kinds may
//        join (audio->audio, midi->midi); mismatches are refused mid-drag.
//      * Click a connection to delete it.
//      * Drag a node body to move it.
//      * Right-click empty canvas -> "Add Module" (+ category submenu).
//      * Right-click a node -> Open Editor / Remove.
//
//  It renders from, and edits, a small engine-free VIEW-MODEL (patch_view_model
//  .h): a list of Nodes and a list of Connections.  It applies edits to its own
//  copy optimistically AND emits a request signal so a coordinator can mirror
//  the change into the real engine graph (and, if the engine rejects it, push a
//  corrected model back via set_nodes()/set_connections()).
//
//  Signals (the seam to the engine coordinator):
//      signal_connect_request( srcNode, srcPort, dstNode, dstPort )
//      signal_disconnect_request( srcNode, srcPort, dstNode, dstPort )
//      signal_add_module_request( x, y )
//      signal_add_module_in_category( category, x, y )   // finer-grained bonus
//      signal_remove_node( node )
//      signal_open_node_editor( node )
//
//-----------------------------------------------------------------------------

#ifndef SEQ24_UI_PATCHBAY_PATCH_CANVAS_H
#define SEQ24_UI_PATCHBAY_PATCH_CANVAS_H

#include <gtkmm/drawingarea.h>
#include <gtkmm/menu.h>

#include <sigc++/sigc++.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../palette.h"          // synth::cBg etc. + set_source / rounded_rect
#include "patch_view_model.h"

namespace seq24 {
namespace patchbay {

class PatchCanvas : public Gtk::DrawingArea
{
public:
    // ---- geometry (canvas units == pixels) ---------------------------------
    static constexpr double NODE_W    = 154.0;  // node box width
    static constexpr double TITLE_H   = 22.0;   // title strip height
    static constexpr double PORT_ROW  = 22.0;   // vertical pitch of ports
    static constexpr double PORT_R    = 5.0;    // port marker radius
    static constexpr double BODY_PAD  = 12.0;   // slack below the last port row
    static constexpr double HIT_R     = 9.0;    // click tolerance (ports / wires)
    static constexpr double CORNER    = 6.0;    // node corner radius
    static constexpr double TWO_PI    = 6.28318530717958647692;  // 2*pi (no M_PI)

    PatchCanvas()
        : m_dragging_wire( false )
        , m_moving_node( false )
        , m_drag_kind( PortKind::Audio )
        , m_drag_x( 0.0 ), m_drag_y( 0.0 )
        , m_move_id( 0 ), m_move_dx( 0.0 ), m_move_dy( 0.0 )
        , m_hover_ok( false )
        , m_menu_x( 0.0 ), m_menu_y( 0.0 )
        , m_ctx_node( 0 )
        , m_menu( 0 ), m_submenu( 0 )
        , m_next_id( 1 )
    {
        add_events( Gdk::BUTTON_PRESS_MASK   |
                    Gdk::BUTTON_RELEASE_MASK |
                    Gdk::POINTER_MOTION_MASK |
                    Gdk::LEAVE_NOTIFY_MASK );
        set_size_request( 720, 460 );
    }

    virtual ~PatchCanvas() {}

    // ========================================================================
    //  Model access (message thread / UI thread)
    // ========================================================================

    //! Replace the whole node list (e.g. an engine snapshot pushed by the
    //! coordinator).  Ids >= m_next_id bump the auto-id counter so later
    //! add_node() calls never collide.
    void set_nodes( const std::vector<Node>& a_nodes )
    {
        m_nodes = a_nodes;
        for ( std::size_t i = 0; i < m_nodes.size(); ++i )
            if ( m_nodes[i].id >= m_next_id )
                m_next_id = m_nodes[i].id + 1;
        refresh();
    }

    void set_connections( const std::vector<Connection>& a_conns )
    {
        m_conns = a_conns;
        refresh();
    }

    const std::vector<Node>&        nodes()       const { return m_nodes; }
    const std::vector<Connection>&  connections() const { return m_conns; }

    //! Add a node to the model.  If `n.id` is 0 a fresh id is assigned.
    //! Returns the final id.  Purely local (no signal) -- adding modules is
    //! driven from the engine side via signal_add_module_request().
    NodeId add_node( Node n )
    {
        if ( n.id == 0 )
            n.id = m_next_id++;
        else if ( n.id >= m_next_id )
            m_next_id = n.id + 1;
        m_nodes.push_back( n );
        refresh();
        return n.id;
    }

    Node* find_node( NodeId a_id )
    {
        for ( std::size_t i = 0; i < m_nodes.size(); ++i )
            if ( m_nodes[i].id == a_id )
                return &m_nodes[i];
        return 0;
    }

    //! Validate + apply a connection, then emit signal_connect_request.
    //! Refuses: unknown ports, kind mismatch, same-node self-patch, duplicates.
    bool add_connection( const Connection& c )
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
        refresh();
        m_sig_connect.emit( c.from.node, c.from.port, c.to.node, c.to.port );
        return true;
    }

    //! Remove one connection (if present) and emit signal_disconnect_request.
    void remove_connection( const Connection& c )
    {
        for ( std::size_t i = 0; i < m_conns.size(); ++i )
        {
            if ( same_connection( m_conns[i], c ) )
            {
                m_conns.erase( m_conns.begin() + i );
                refresh();
                m_sig_disconnect.emit( c.from.node, c.from.port,
                                       c.to.node,   c.to.port );
                return;
            }
        }
    }

    //! Remove a node, drop every connection that touches it, emit
    //! signal_remove_node.
    void remove_node( NodeId a_id )
    {
        bool found = false;
        for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        {
            if ( m_nodes[i].id == a_id )
            {
                m_nodes.erase( m_nodes.begin() + i );
                found = true;
                break;
            }
        }
        if ( !found )
            return;

        for ( std::size_t i = m_conns.size(); i-- > 0; )
            if ( m_conns[i].from.node == a_id || m_conns[i].to.node == a_id )
                m_conns.erase( m_conns.begin() + i );

        refresh();
        m_sig_remove.emit( a_id );
    }

    // ========================================================================
    //  Signals (the coordinator seam)
    // ========================================================================
    sigc::signal<void, NodeId, PortId, NodeId, PortId>& signal_connect_request()
        { return m_sig_connect; }
    sigc::signal<void, NodeId, PortId, NodeId, PortId>& signal_disconnect_request()
        { return m_sig_disconnect; }
    sigc::signal<void, double, double>& signal_add_module_request()
        { return m_sig_add; }
    sigc::signal<void, const std::string&, double, double>&
        signal_add_module_in_category() { return m_sig_add_cat; }
    sigc::signal<void, NodeId>& signal_remove_node()      { return m_sig_remove; }
    sigc::signal<void, NodeId>& signal_open_node_editor() { return m_sig_editor; }

protected:
    // ========================================================================
    //  GTK plumbing
    // ========================================================================

    // Chain the base realize FIRST -- required on the Windows GTK backend to
    // avoid the DrawingArea realize crash (same fix as src/seqroll.cpp).
    virtual void on_realize()
    {
        Gtk::DrawingArea::on_realize();
        set_flags( Gtk::CAN_FOCUS );
        m_window = get_window();
    }

    virtual bool on_expose_event( GdkEventExpose* /*ev*/ )
    {
        Glib::RefPtr<Gdk::Window> win = get_window();
        if ( !win )
            return true;

        Cairo::RefPtr<Cairo::Context> cr = win->create_cairo_context();
        Gtk::Allocation a = get_allocation();

        // Background.
        synth::set_source( cr, synth::cBg );
        cr->rectangle( 0, 0, a.get_width(), a.get_height() );
        cr->fill();

        // Committed connections (behind the nodes).
        for ( std::size_t i = 0; i < m_conns.size(); ++i )
            draw_connection( cr, m_conns[i], false );

        // In-flight drag wire.
        if ( m_dragging_wire )
            draw_drag_wire( cr );

        // Nodes on top.
        for ( std::size_t i = 0; i < m_nodes.size(); ++i )
            draw_node( cr, m_nodes[i] );

        return true;
    }

    virtual bool on_button_press_event( GdkEventButton* ev )
    {
        grab_focus();
        const double x = ev->x, y = ev->y;

        // ---- right-click: context menus ----
        if ( ev->button == 3 )
        {
            NodeId n = node_at( x, y );
            if ( n != 0 )
                popup_node_menu( n, ev );
            else
                popup_add_menu( x, y, ev );
            return true;
        }

        if ( ev->button != 1 )
            return true;

        // ---- left-click: start a wire from an OUT port ----
        PortHit ph;
        if ( port_at( x, y, ph ) && ph.dir == PortDir::Out )
        {
            m_dragging_wire = true;
            m_drag_src      = PortRef( ph.node, ph.port );
            m_drag_kind     = ph.kind;
            m_drag_x        = x;
            m_drag_y        = y;
            m_hover_ok      = false;
            refresh();
            return true;
        }

        // ---- left-click a node body: move it (raise to top) ----
        NodeId n = node_at( x, y );
        if ( n != 0 )
        {
            Node* nd = find_node( n );
            if ( nd )
            {
                m_moving_node = true;
                m_move_id     = n;
                m_move_dx     = x - nd->x;
                m_move_dy     = y - nd->y;
                raise_node( n );
                refresh();
            }
            return true;
        }

        // ---- left-click a connection: delete it ----
        Connection hit;
        if ( connection_at( x, y, hit ) )
        {
            remove_connection( hit );
            return true;
        }

        return true;
    }

    virtual bool on_motion_notify_event( GdkEventMotion* ev )
    {
        const double x = ev->x, y = ev->y;

        if ( m_dragging_wire )
        {
            m_drag_x = x;
            m_drag_y = y;
            PortHit ph;
            m_hover_ok = ( port_at( x, y, ph ) &&
                           ph.dir == PortDir::In &&
                           ph.kind == m_drag_kind &&
                           ph.node != m_drag_src.node );
            m_hover_pt = PortRef( ph.node, ph.port );
            refresh();
            return true;
        }

        if ( m_moving_node )
        {
            Node* nd = find_node( m_move_id );
            if ( nd )
            {
                nd->x = x - m_move_dx;
                nd->y = y - m_move_dy;
                refresh();
            }
            return true;
        }

        return true;
    }

    virtual bool on_button_release_event( GdkEventButton* ev )
    {
        if ( ev->button == 1 && m_dragging_wire )
        {
            m_dragging_wire = false;
            PortHit ph;
            if ( port_at( ev->x, ev->y, ph ) && ph.dir == PortDir::In )
            {
                Connection c( m_drag_src, PortRef( ph.node, ph.port ) );
                add_connection( c );   // validates kind / self / dup, emits
            }
            m_hover_ok = false;
            refresh();
            return true;
        }

        if ( ev->button == 1 && m_moving_node )
        {
            m_moving_node = false;
            return true;
        }

        return true;
    }

    virtual bool on_leave_notify_event( GdkEventCrossing* /*ev*/ )
    {
        // Do not abandon an active drag on a brief pointer excursion; only tidy
        // the hover highlight.
        if ( m_dragging_wire )
        {
            m_hover_ok = false;
            refresh();
        }
        return true;
    }

private:
    // ========================================================================
    //  Geometry helpers
    // ========================================================================

    double node_height( const Node& n ) const
    {
        std::size_t rows = std::max( n.inPorts.size(), n.outPorts.size() );
        double h = TITLE_H + double( rows ) * PORT_ROW + BODY_PAD;
        double minh = TITLE_H + PORT_ROW;
        return h > minh ? h : minh;
    }

    // Centre of the i-th port on `dir` side of node `n`.
    void port_center( const Node& n, PortDir dir, std::size_t idx,
                      double& cx, double& cy ) const
    {
        cy = n.y + TITLE_H + PORT_ROW * ( double( idx ) + 0.5 );
        cx = ( dir == PortDir::In ) ? n.x : n.x + NODE_W;
    }

    // Look up the world position of a specific port ref.
    bool ref_center( const PortRef& r, PortDir dir,
                     double& cx, double& cy ) const
    {
        for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        {
            if ( m_nodes[i].id != r.node )
                continue;
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

    bool kind_of( NodeId nid, PortId pid, PortDir dir, PortKind& out ) const
    {
        for ( std::size_t i = 0; i < m_nodes.size(); ++i )
        {
            if ( m_nodes[i].id != nid )
                continue;
            const std::vector<Port>& v =
                ( dir == PortDir::In ) ? m_nodes[i].inPorts : m_nodes[i].outPorts;
            for ( std::size_t k = 0; k < v.size(); ++k )
                if ( v[k].id == pid )
                {
                    out = v[k].kind;
                    return true;
                }
        }
        return false;
    }

    struct PortHit
    {
        NodeId   node;
        PortId   port;
        PortKind kind;
        PortDir  dir;
        PortHit() : node( 0 ), port( 0 ), kind( PortKind::Audio ),
                    dir( PortDir::In ) {}
    };

    // Topmost port under (x,y), if any.
    bool port_at( double x, double y, PortHit& out ) const
    {
        for ( std::size_t i = m_nodes.size(); i-- > 0; )
        {
            const Node& n = m_nodes[i];
            for ( std::size_t k = 0; k < n.inPorts.size(); ++k )
            {
                double cx, cy; port_center( n, PortDir::In, k, cx, cy );
                if ( near( x, y, cx, cy, HIT_R ) )
                {
                    out.node = n.id; out.port = n.inPorts[k].id;
                    out.kind = n.inPorts[k].kind; out.dir = PortDir::In;
                    return true;
                }
            }
            for ( std::size_t k = 0; k < n.outPorts.size(); ++k )
            {
                double cx, cy; port_center( n, PortDir::Out, k, cx, cy );
                if ( near( x, y, cx, cy, HIT_R ) )
                {
                    out.node = n.id; out.port = n.outPorts[k].id;
                    out.kind = n.outPorts[k].kind; out.dir = PortDir::Out;
                    return true;
                }
            }
        }
        return false;
    }

    // Topmost node box under (x,y), or 0.
    NodeId node_at( double x, double y ) const
    {
        for ( std::size_t i = m_nodes.size(); i-- > 0; )
        {
            const Node& n = m_nodes[i];
            double h = node_height( n );
            if ( x >= n.x && x <= n.x + NODE_W && y >= n.y && y <= n.y + h )
                return n.id;
        }
        return 0;
    }

    // Nearest connection to (x,y) within HIT_R (bezier sampled), or false.
    bool connection_at( double x, double y, Connection& out ) const
    {
        double best = HIT_R;
        bool   got  = false;
        for ( std::size_t i = 0; i < m_conns.size(); ++i )
        {
            double x1, y1, x2, y2;
            if ( !ref_center( m_conns[i].from, PortDir::Out, x1, y1 ) ) continue;
            if ( !ref_center( m_conns[i].to,   PortDir::In,  x2, y2 ) ) continue;

            double c1x, c1y, c2x, c2y;
            control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );

            const int STEPS = 24;
            for ( int s = 0; s <= STEPS; ++s )
            {
                double t = double( s ) / STEPS;
                double bx, by;
                bezier_point( t, x1, y1, c1x, c1y, c2x, c2y, x2, y2, bx, by );
                double d = std::hypot( bx - x, by - y );
                if ( d < best )
                {
                    best = d;
                    out  = m_conns[i];
                    got  = true;
                }
            }
        }
        return got;
    }

    static bool near( double x, double y, double cx, double cy, double r )
    {
        double dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    }

    static void control_points( double x1, double y1, double x2, double y2,
                                double& c1x, double& c1y,
                                double& c2x, double& c2y )
    {
        double dx = std::fabs( x2 - x1 ) * 0.5;
        if ( dx < 40.0 ) dx = 40.0;
        c1x = x1 + dx; c1y = y1;
        c2x = x2 - dx; c2y = y2;
    }

    static void bezier_point( double t,
                              double x1, double y1, double c1x, double c1y,
                              double c2x, double c2y, double x2, double y2,
                              double& bx, double& by )
    {
        double u = 1.0 - t;
        double a = u * u * u;
        double b = 3.0 * u * u * t;
        double c = 3.0 * u * t * t;
        double d = t * t * t;
        bx = a * x1 + b * c1x + c * c2x + d * x2;
        by = a * y1 + b * c1y + c * c2y + d * y2;
    }

    void raise_node( NodeId id )
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

    // ========================================================================
    //  Drawing
    // ========================================================================

    void draw_node( const Cairo::RefPtr<Cairo::Context>& cr, const Node& n )
    {
        const double h = node_height( n );

        // Body.
        synth::rounded_rect( cr, n.x, n.y, NODE_W, h, CORNER );
        synth::set_source( cr, synth::cPanel );
        cr->fill_preserve();
        synth::set_source( cr, synth::cHi );
        cr->set_line_width( 1.5 );
        cr->stroke();

        // Title strip.
        cr->save();
        synth::rounded_rect( cr, n.x, n.y, NODE_W, h, CORNER );
        cr->clip();
        synth::set_source( cr, synth::cAccent );
        cr->rectangle( n.x, n.y, NODE_W, TITLE_H );
        cr->fill();
        cr->restore();

        // Title text.
        cr->select_font_face( "monospace", Cairo::FONT_SLANT_NORMAL,
                              Cairo::FONT_WEIGHT_BOLD );
        cr->set_font_size( 11.0 );
        synth::set_source( cr, synth::cBg );
        draw_clipped_text( cr, n.name, n.x + 7.0, n.y + 15.0, NODE_W - 12.0 );

        // Ports + labels.
        cr->select_font_face( "monospace", Cairo::FONT_SLANT_NORMAL,
                              Cairo::FONT_WEIGHT_NORMAL );
        cr->set_font_size( 9.5 );

        for ( std::size_t k = 0; k < n.inPorts.size(); ++k )
        {
            double cx, cy; port_center( n, PortDir::In, k, cx, cy );
            bool hi = m_dragging_wire && m_hover_ok &&
                      m_hover_pt.node == n.id && m_hover_pt.port == n.inPorts[k].id;
            draw_port( cr, cx, cy, n.inPorts[k].kind, hi );
            synth::set_source( cr, synth::cHi );
            draw_text_left( cr, n.inPorts[k].name, cx + PORT_R + 4.0, cy + 3.0 );
        }
        for ( std::size_t k = 0; k < n.outPorts.size(); ++k )
        {
            double cx, cy; port_center( n, PortDir::Out, k, cx, cy );
            draw_port( cr, cx, cy, n.outPorts[k].kind, false );
            synth::set_source( cr, synth::cHi );
            draw_text_right( cr, n.outPorts[k].name, cx - PORT_R - 4.0, cy + 3.0 );
        }
    }

    // Audio port = FILLED dot; Midi port = HOLLOW dot.
    void draw_port( const Cairo::RefPtr<Cairo::Context>& cr,
                    double cx, double cy, PortKind kind, bool highlight )
    {
        if ( highlight )
        {
            cr->begin_new_sub_path();
            cr->arc( cx, cy, PORT_R + 3.0, 0.0, TWO_PI );
            synth::set_source( cr, synth::cActive );
            cr->set_line_width( 1.5 );
            cr->stroke();
        }

        cr->begin_new_sub_path();
        cr->arc( cx, cy, PORT_R, 0.0, TWO_PI );
        if ( kind == PortKind::Audio )
        {
            synth::set_source( cr, synth::cHi );
            cr->fill();
        }
        else
        {
            synth::set_source( cr, synth::cPanel );
            cr->fill_preserve();
            synth::set_source( cr, synth::cHi );
            cr->set_line_width( 1.5 );
            cr->stroke();
        }
    }

    void draw_connection( const Cairo::RefPtr<Cairo::Context>& cr,
                          const Connection& c, bool /*sel*/ )
    {
        double x1, y1, x2, y2;
        if ( !ref_center( c.from, PortDir::Out, x1, y1 ) ) return;
        if ( !ref_center( c.to,   PortDir::In,  x2, y2 ) ) return;

        PortKind k;
        bool midi = kind_of( c.from.node, c.from.port, PortDir::Out, k ) &&
                    k == PortKind::Midi;

        double c1x, c1y, c2x, c2y;
        control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );

        cr->move_to( x1, y1 );
        cr->curve_to( c1x, c1y, c2x, c2y, x2, y2 );
        synth::set_source( cr, synth::cDim );
        cr->set_line_width( midi ? 1.4 : 2.2 );
        std::vector<double> dashes;
        if ( midi ) { dashes.push_back( 4.0 ); dashes.push_back( 3.0 ); }
        cr->set_dash( dashes, 0.0 );
        cr->stroke();
        cr->unset_dash();
    }

    void draw_drag_wire( const Cairo::RefPtr<Cairo::Context>& cr )
    {
        double x1, y1;
        if ( !ref_center( m_drag_src, PortDir::Out, x1, y1 ) )
            return;
        double x2 = m_drag_x, y2 = m_drag_y;

        double c1x, c1y, c2x, c2y;
        control_points( x1, y1, x2, y2, c1x, c1y, c2x, c2y );

        cr->move_to( x1, y1 );
        cr->curve_to( c1x, c1y, c2x, c2y, x2, y2 );
        synth::set_source( cr, synth::cActive, m_hover_ok ? 1.0 : 0.7 );
        cr->set_line_width( 2.0 );
        std::vector<double> dashes;
        dashes.push_back( 6.0 );
        dashes.push_back( 4.0 );
        cr->set_dash( dashes, 0.0 );
        cr->stroke();
        cr->unset_dash();
    }

    void draw_text_left( const Cairo::RefPtr<Cairo::Context>& cr,
                         const std::string& s, double x, double y )
    {
        cr->move_to( x, y );
        cr->show_text( s );
    }

    void draw_text_right( const Cairo::RefPtr<Cairo::Context>& cr,
                          const std::string& s, double xr, double y )
    {
        Cairo::TextExtents te;
        cr->get_text_extents( s, te );
        cr->move_to( xr - te.width, y );
        cr->show_text( s );
    }

    // Title text truncated with an ellipsis to fit `maxw`.
    void draw_clipped_text( const Cairo::RefPtr<Cairo::Context>& cr,
                            const std::string& s, double x, double y,
                            double maxw )
    {
        Cairo::TextExtents te;
        cr->get_text_extents( s, te );
        if ( te.width <= maxw )
        {
            cr->move_to( x, y );
            cr->show_text( s );
            return;
        }
        std::string t = s;
        while ( t.size() > 1 )
        {
            t.erase( t.size() - 1 );
            std::string probe = t + "\xE2\x80\xA6";   // UTF-8 ellipsis
            cr->get_text_extents( probe, te );
            if ( te.width <= maxw )
            {
                cr->move_to( x, y );
                cr->show_text( probe );
                return;
            }
        }
        cr->move_to( x, y );
        cr->show_text( t );
    }

    // ========================================================================
    //  Context menus
    // ========================================================================

    void popup_add_menu( double x, double y, GdkEventButton* ev )
    {
        using namespace Gtk::Menu_Helpers;
        m_menu_x = x;
        m_menu_y = y;

        m_menu = Gtk::manage( new Gtk::Menu() );

        // Category submenu (finer requests for the coordinator's browser).
        m_submenu = Gtk::manage( new Gtk::Menu() );
        static const char* cats[] =
            { "Instrument", "Effect", "MIDI", "Audio I/O", "Mixer" };
        for ( unsigned i = 0; i < sizeof( cats ) / sizeof( cats[0] ); ++i )
            m_submenu->items().push_back(
                MenuElem( cats[i],
                    sigc::bind( sigc::mem_fun( *this,
                                    &PatchCanvas::on_add_category ),
                                std::string( cats[i] ) ) ) );

        m_menu->items().push_back( MenuElem( "Add Module", *m_submenu ) );
        m_menu->items().push_back( SeparatorElem() );
        m_menu->items().push_back(
            MenuElem( "Add Module...",
                      sigc::mem_fun( *this, &PatchCanvas::on_add_generic ) ) );

        m_menu->show_all();
        m_menu->popup( ev->button, ev->time );
    }

    void popup_node_menu( NodeId id, GdkEventButton* ev )
    {
        using namespace Gtk::Menu_Helpers;
        m_ctx_node = id;
        m_menu = Gtk::manage( new Gtk::Menu() );

        Node* nd = find_node( id );
        Gtk::MenuItem* title =
            Gtk::manage( new Gtk::MenuItem( nd ? nd->name : std::string( "Node" ) ) );
        title->set_sensitive( false );
        m_menu->append( *title );
        m_menu->items().push_back( SeparatorElem() );

        m_menu->items().push_back(
            MenuElem( "Open Editor",
                      sigc::mem_fun( *this, &PatchCanvas::on_ctx_open_editor ) ) );
        m_menu->items().push_back(
            MenuElem( "Remove",
                      sigc::mem_fun( *this, &PatchCanvas::on_ctx_remove ) ) );

        m_menu->show_all();
        m_menu->popup( ev->button, ev->time );
    }

    void on_add_generic()
    {
        m_sig_add.emit( m_menu_x, m_menu_y );
    }

    void on_add_category( std::string cat )
    {
        m_sig_add_cat.emit( cat, m_menu_x, m_menu_y );
        m_sig_add.emit( m_menu_x, m_menu_y );
    }

    void on_ctx_open_editor()
    {
        if ( m_ctx_node != 0 )
            m_sig_editor.emit( m_ctx_node );
    }

    void on_ctx_remove()
    {
        if ( m_ctx_node != 0 )
            remove_node( m_ctx_node );   // emits signal_remove_node
    }

    // ========================================================================
    //  Misc
    // ========================================================================
    void refresh()
    {
        if ( is_realized() )
            queue_draw();
    }

    // ---- model --------------------------------------------------------------
    std::vector<Node>        m_nodes;
    std::vector<Connection>  m_conns;

    // ---- interaction state --------------------------------------------------
    bool     m_dragging_wire;
    bool     m_moving_node;
    PortRef  m_drag_src;       // OUT port a wire is being pulled from
    PortKind m_drag_kind;
    double   m_drag_x, m_drag_y;
    bool     m_hover_ok;       // pointer is over a valid IN target
    PortRef  m_hover_pt;       // that target (for highlighting)
    NodeId   m_move_id;
    double   m_move_dx, m_move_dy;

    // ---- menu scratch -------------------------------------------------------
    double   m_menu_x, m_menu_y;
    NodeId   m_ctx_node;
    Gtk::Menu* m_menu;
    Gtk::Menu* m_submenu;

    Glib::RefPtr<Gdk::Window> m_window;
    NodeId   m_next_id;

    // ---- signals ------------------------------------------------------------
    sigc::signal<void, NodeId, PortId, NodeId, PortId> m_sig_connect;
    sigc::signal<void, NodeId, PortId, NodeId, PortId> m_sig_disconnect;
    sigc::signal<void, double, double>                 m_sig_add;
    sigc::signal<void, const std::string&, double, double> m_sig_add_cat;
    sigc::signal<void, NodeId>                         m_sig_remove;
    sigc::signal<void, NodeId>                         m_sig_editor;
};

} // namespace patchbay
} // namespace seq24

#endif // SEQ24_UI_PATCHBAY_PATCH_CANVAS_H
