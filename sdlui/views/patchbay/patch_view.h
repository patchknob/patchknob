//----------------------------------------------------------------------------
//
//  sdlui/views/patchbay/patch_view.h
//
//  SDL2 port of the modular PATCHBAY node-editor (mirrors the GTK
//  src/ui/patchbay/patch_canvas.h).  A ui::Widget that renders and edits the
//  engine-free VIEW-MODEL from src/ui/patchbay/patch_view_model.h:
//
//      * NODES  -- rounded boxes with a title strip; audio ports draw FILLED
//        dots, MIDI ports draw HOLLOW dots.  Inputs down the left edge,
//        outputs down the right.
//      * CONNECTIONS -- bezier curves (sampled to line segments in SDL) from an
//        OUT port to an IN port.  Audio wires are solid, MIDI wires dashed.
//
//  Interaction (same as the GTK canvas):
//      * Drag from an OUT port to an IN port to connect.  Only like-kinds may
//        join (audio->audio, midi->midi); mismatches are refused mid-drag.
//      * Left-click a wire to delete it.
//      * Drag a node body to move it (raises to top).
//      * Right-click empty canvas -> "Add Module" (category list).
//      * Right-click a node -> Open Editor / Remove.
//
//  It applies edits to its own copy optimistically AND emits a std::function
//  callback so a coordinator can mirror the change into the real engine graph
//  (and, if the engine rejects it, push a corrected model back via
//  set_nodes() / set_connections()).
//
//  Strictly two-tone: every colour comes from ui::theme() roles, so it tracks
//  the live LIGHT / MIDNIGHT theme.
//
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_PATCHBAY_PATCH_VIEW_H
#define SEQ24_SDLUI_PATCHBAY_PATCH_VIEW_H

#include "gui.h"
#include "ui/patchbay/patch_view_model.h"   // the SHARED engine-free view-model

#include <functional>
#include <string>
#include <vector>

namespace seq24 {
namespace patchbay {

//! The SDL patchbay canvas.  Mount it as a top-level ui::Widget root sized to a
//! rect (see the shell notes at the bottom of this header).
class PatchView : public ui::Widget
{
public:
    // ---- geometry (canvas units == pixels), mirrors PatchCanvas ------------
    static const int    NODE_W   = 154;   // node box width
    static const int    TITLE_H  = 22;    // title strip height
    static const int    PORT_ROW = 22;    // vertical pitch of ports
    static const int    PORT_R   = 5;     // port marker radius
    static const int    BODY_PAD = 12;    // slack below the last port row
    static const int    HIT_R    = 9;     // click tolerance (ports / wires)
    static const int    CORNER   = 6;     // node corner radius

    PatchView();
    virtual ~PatchView() {}

    // ========================================================================
    //  Model access (call from the UI thread; each refreshes the widget)
    // ========================================================================
    void set_nodes( const std::vector<Node>& a_nodes );
    void set_connections( const std::vector<Connection>& a_conns );

    const std::vector<Node>&       nodes()       const { return m_nodes; }
    const std::vector<Connection>& connections() const { return m_conns; }

    //! Add a node.  If n.id is 0 a fresh id is assigned.  Returns the final id.
    NodeId add_node( Node n );
    Node*  find_node( NodeId a_id );

    //! Validate + apply a connection, then fire on_connect.  Refuses unknown
    //! ports, kind mismatch, same-node self-patch, and duplicates.
    bool add_connection( const Connection& c );
    //! Remove one connection (if present) and fire on_disconnect.
    void remove_connection( const Connection& c );
    //! Remove a node + every wire touching it, then fire on_remove_node.
    void remove_node( NodeId a_id );

    // ========================================================================
    //  Callbacks (the coordinator seam).  Any may be left null.
    // ========================================================================
    std::function<void(NodeId,PortId,NodeId,PortId)> on_connect;
    std::function<void(NodeId,PortId,NodeId,PortId)> on_disconnect;
    std::function<void(double,double,const std::string&)> on_add_module;   // x, y, category ("" = generic)
    std::function<void(NodeId)>                      on_remove_node;
    std::function<void(NodeId)>                      on_open_editor;

    // ========================================================================
    //  ui::Widget overrides
    // ========================================================================
    void draw( ui::App& app ) override;
    bool on_mouse( ui::App& app, const ui::MouseEv& e ) override;

private:
    // ---- geometry helpers (canvas-local coords) ----------------------------
    int  node_height( const Node& n ) const;
    void port_center( const Node& n, PortDir dir, std::size_t idx,
                      int& cx, int& cy ) const;
    bool ref_center( const PortRef& r, PortDir dir, int& cx, int& cy ) const;
    bool kind_of( NodeId nid, PortId pid, PortDir dir, PortKind& out ) const;

    struct PortHit { NodeId node; PortId port; PortKind kind; PortDir dir; };
    bool   port_at( int x, int y, PortHit& out ) const;
    NodeId node_at( int x, int y ) const;
    bool   connection_at( int x, int y, Connection& out ) const;
    void   raise_node( NodeId id );

    // ---- drawing (canvas-local coords are offset by rect.x/rect.y) ---------
    void draw_node( ui::App& app, const Node& n );
    void draw_port( ui::App& app, int cx, int cy, PortKind kind, bool hi );
    void draw_connection( ui::App& app, const Connection& c );
    void draw_drag_wire( ui::App& app );
    void draw_menu( ui::App& app );

    // ---- context menu ------------------------------------------------------
    struct MenuItem
    {
        std::string           label;
        bool                  enabled;
        bool                  separator;
        std::function<void()> action;
    };
    void open_add_menu( ui::App& app, int sx, int sy, double cx, double cy );
    void open_node_menu( ui::App& app, int sx, int sy, NodeId id );
    void layout_menu( ui::App& app );
    void close_menu();

    // ---- model -------------------------------------------------------------
    std::vector<Node>       m_nodes;
    std::vector<Connection> m_conns;
    NodeId                  m_next_id;

    // ---- interaction state -------------------------------------------------
    bool     m_dragging_wire;
    bool     m_moving_node;
    PortRef  m_drag_src;        // OUT port a wire is being pulled from
    PortKind m_drag_kind;
    int      m_drag_x, m_drag_y;   // current pointer (canvas-local)
    bool     m_hover_ok;           // pointer over a valid IN target
    PortRef  m_hover_pt;
    NodeId   m_move_id;
    int      m_move_dx, m_move_dy;

    // ---- menu state --------------------------------------------------------
    bool                  m_menu_open;
    int                   m_menu_x, m_menu_y;   // top-left, SCREEN coords
    int                   m_menu_w, m_menu_h;
    std::vector<MenuItem> m_menu_items;
    double                m_menu_cx, m_menu_cy; // canvas coords for Add Module
};

} // namespace patchbay
} // namespace seq24

//----------------------------------------------------------------------------
//  HOW THE SHELL MOUNTS IT
//
//    using seq24::patchbay::PatchView;
//    PatchView pv;
//    pv.rect = { 0, 0, app.w, app.h };          // size to any rect you want
//    pv.set_nodes( myNodes );
//    pv.set_connections( myConns );
//    pv.on_connect     = [](auto fn,auto fp,auto tn,auto tp){ ...engine... };
//    pv.on_add_module  = [](double x,double y,const std::string& cat){ ... };
//    pv.on_remove_node = [](auto id){ ... };
//    pv.on_open_editor = [](auto id){ ... };
//    app.roots.push_back( &pv );
//    // re-set pv.rect in app.on_layout to track window resizes.
//----------------------------------------------------------------------------
#endif // SEQ24_SDLUI_PATCHBAY_PATCH_VIEW_H
