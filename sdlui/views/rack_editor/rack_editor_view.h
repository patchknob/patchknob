//----------------------------------------------------------------------------
//
//  sdlui/views/rack_editor/rack_editor_view.h
//
//  A GENERIC, DATA-DRIVEN SDL editor for a VCV-Rack-style modular patch
//  (rackx::RackEngine).  It renders and edits EVERY module type with zero
//  per-module code: it reads each module's declared params / ports / lights
//  (rack::engine::Module) and draws knobs / jacks / lights generically.
//
//      * MODULE  -- a vertical panel: a title strip (module->name), a grid of
//        small KNOBS (one per param, wrapping to rows), an optional strip of
//        LIGHTS, then a two-column band of JACKS -- INPUTS down the left edge,
//        OUTPUTS down the right edge.
//      * KNOB    -- a filled disc with an indicator line whose angle maps the
//        param's normalised value over ~-135deg..+135deg; label underneath.
//      * JACK    -- a small disc; inputs FILLED, outputs RINGED (distinct).
//      * LIGHT   -- a tiny disc tinted by its live brightness (0..1).
//      * CABLE   -- a bezier from a source OUTPUT jack to a dest INPUT jack.
//
//  Interaction:
//      * Drag a module's TITLE BAR  -> move it (engine->moveModule).
//      * Drag a KNOB (vertical)     -> change the param (engine->setParam);
//        double-click a knob        -> reset it to its default value.
//      * Drag from an OUTPUT jack to an INPUT jack -> engine->addCable.
//      * Left-click empty canvas    -> searchable MODULE PALETTE overlay
//        (adds a module at the click position).
//      * Right-click a module       -> engine->removeModule.
//      * Right-click near a cable   -> engine->removeCable.
//      * Delete/Backspace           -> remove the selected module.
//      * Wheel                      -> pan (Shift / horizontal wheel -> x);
//        while the palette is open, wheel scrolls its list.
//
//  Layout is deterministic and SHARED between drawing and hit-testing via the
//  module_rect() / knob_pos() / jack_pos() helpers, so a jack drawn at pixel P
//  is hit-tested at P and cables line up exactly.
//
//  Strictly two-tone: every colour comes from ui::theme() roles, so it tracks
//  the live LIGHT / MIDNIGHT theme.  Runs on the GUI thread; reads module
//  params / light brightness live each frame (the audio thread only reads them,
//  structural edits happen on this same thread).  Never crashes on a null /
//  empty engine.
//
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_RACK_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_RACK_EDITOR_VIEW_H

#include "gui.h"
#include "engine/rack/rack_engine.h"     // rackx::RackEngine / RackModule / RackCable
#include "cardinal_svg_textures.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace rackx { struct PanelSpec; }

namespace rackui {

//! The generic SDL rack editor.  Host it in a ui::Window (or mount as a root
//! sized to a rect) and hand it a RackNode's engine via set_engine(); see the
//! shell notes at the bottom of this header.
class RackEditorView : public ui::Widget
{
public:
    RackEditorView() {}
    virtual ~RackEditorView() {}

    // ========================================================================
    //  Public API the shell uses
    // ========================================================================

    //! Point the editor at the RackNode's engine (may be null until wired).
    //! All edits / queries go through it.  Populates the palette lazily.
    void               set_engine( rackx::RackEngine* eng );
    rackx::RackEngine* engine() const { return m_engine; }

    // ========================================================================
    //  ui::Widget overrides
    // ========================================================================
    void draw( ui::App& app ) override;
    bool on_mouse( ui::App& app, const ui::MouseEv& e ) override;
    bool on_wheel( ui::App& app, int dx, int dy ) override;
    bool on_key( ui::App& app, SDL_Keycode k ) override;

    // ========================================================================
    //  Deterministic layout (SCREEN coords).  draw() AND on_mouse() share these
    //  so what is drawn at pixel P is hit-tested at pixel P.  Each returns false
    //  when the module / index is invalid.
    // ========================================================================
    bool module_rect( rackx::RackModule* m, SDL_Rect& out ) const;
    bool knob_pos( rackx::RackModule* m, int paramIdx, int& cx, int& cy, int& r ) const;
    bool jack_pos( rackx::RackModule* m, bool isInput, int portIdx,
                   int& cx, int& cy, int& r ) const;

private:
    // ---- layout constants (canvas units == pixels) -------------------------
    static const int PAD           = 8;    // panel inner horizontal padding
    static const int PANEL_MIN_W   = 100;  // minimum panel width
    static const int TITLE_H       = 20;   // title strip height
    static const int KNOB_R        = 9;    // knob radius
    static const int KNOB_CELL_W   = 32;   // horizontal pitch of a knob cell
    static const int KNOBS_PER_ROW = 4;    // max knobs before wrapping
    static const int LIGHT_R       = 3;    // light radius
    static const int LIGHT_CELL    = 12;   // horizontal pitch of a light
    static const int LIGHT_ROW_H   = 12;   // vertical pitch of a light row
    static const int JACK_R        = 6;    // jack radius (visual)
    static const int JACK_COL_MIN  = 54;   // min width reserved per jack column
    static const int BOTTOM_PAD    = 8;    // slack below the last jack row
    static const int HIT_R         = 13;   // click tolerance (jacks are small -> generous)
    static const int KNOB_DRAG_PX  = 150;  // px of drag == full param range
    static const int DBLCLK_MS     = 350;  // knob double-click (reset) window
    static const int PAN_STEP      = 24;   // wheel pan / list scroll step
    static const int PAL_W         = 292;  // docked module browser width
    static const int PAL_ROWS      = 5;    // visible module card rows
    static const int PAL_CARD_H    = 104;  // thumbnail card height

    // ---- per-view zoom ------------------------------------------------------
    // m_zoom scales all rack geometry and labels.  Because m_zoom is a member,
    // every editor window zooms independently.
    // zpx() scales an integer pixel SIZE by the current zoom (rounded).
    int zpx( int v ) const { return (int) std::lround( v * m_zoom ); }
    int zpx( float v ) const { return (int) std::lround( v * m_zoom ); }

    // ---- shared per-module metrics (drawing == hit-testing) ----------------
    struct Layout {
        const rackx::PanelSpec* panel = nullptr;
        int nP = 0, nI = 0, nO = 0, nL = 0;             // declared counts
        int innerW = 0, w = 0, h = 0;                   // panel dims
        int knobsPerRow = 0, knobRows = 0, knobCellH = 0, knobAreaH = 0;
        int lightsPerRow = 0, lightRows = 0, lightAreaH = 0;
        int jackRows = 0, jackRowH = 0, jackAreaH = 0;
    };
    const rackx::PanelSpec* panel_of( rackx::RackModule* m ) const;
    Layout layout_of( rackx::RackModule* m ) const;
    bool   light_pos( rackx::RackModule* m, int lightIdx,
                      int& cx, int& cy, int& r ) const;

    // ---- tabbed panels ------------------------------------------------------
    static const int TAB_BAR_H = 18;    // tab strip height (panel px, pre-zoom)
    int  active_tab( rackx::RackModule* m ) const;    // 0 when untabbed
    bool tab_visible( const rackx::PanelElement* e, int active ) const {
        return !e || e->tab < 0 || e->tab == active;
    }
    bool tab_bar_at( rackx::RackModule* m, int sx, int sy, int& tabIndex ) const;

    // ---- hit testing (SCREEN coords) ---------------------------------------
    rackx::RackModule* module_at( int sx, int sy ) const;   // topmost
    bool title_at( rackx::RackModule* m, int sx, int sy ) const;
    bool knob_at( int sx, int sy, int& modId, int& paramIdx ) const;
    bool jack_at( int sx, int sy, bool wantInput, int& modId, int& portIdx ) const;
    bool cable_at( int sx, int sy, int& cableId ) const;

    // ---- drawing -----------------------------------------------------------
    void draw_module( ui::App& app, rackx::RackModule* m );
    void draw_knob( ui::App& app, rackx::RackModule* m, int i );
    void draw_light( ui::App& app, rackx::RackModule* m, int i );
    void draw_jack( ui::App& app, rackx::RackModule* m, bool isInput, int i );
    void draw_cable( ui::App& app, rackx::RackCable* c );
    void draw_drag_wire( ui::App& app );

    // ---- module palette overlay --------------------------------------------
    void             draw_palette( ui::App& app );
    SDL_Rect         palette_rect( ui::App& app ) const;
    std::vector<std::string> palette_types() const;
    std::vector<int> filtered() const;      // selected type + search matching registry entries
    void             open_palette( ui::App& app, int sx, int sy );
    void             close_palette( ui::App& app );
    bool             palette_mouse( ui::App& app, const ui::MouseEv& e, bool downEdge );

    // ---- left-button sub-handlers ------------------------------------------
    bool press_left( ui::App& app, int mx, int my );
    bool drag_left( ui::App& app, int mx, int my );
    bool release_left( ui::App& app, int mx, int my );

    // ---- model -------------------------------------------------------------
    rackx::RackEngine* m_engine = nullptr;   // non-owning (the RackNode owns it)

    // ---- view --------------------------------------------------------------
    int   m_ox = 0, m_oy = 0;    // pan offset (screen px)
    float m_zoom = 0.8f;         // geometry scale, clamped [0.5, 2.5] (see zpx)
    int   m_cw = 8, m_ch = 14;   // cached mono cell metrics (updated each event)
    int   m_sel = -1;            // selected module id (-1 == none)
    mutable std::unordered_map<int, int> m_module_tab;  // module id -> active tab page
    int   m_hover_cable = -1;    // cable id under the pointer (-1 == none)
    bool  m_hover_cable_endpoints = false;
    int   m_hover_out_x = 0, m_hover_out_y = 0;
    int   m_hover_in_x = 0, m_hover_in_y = 0;
    mutable CardinalSvgTextures m_cardinalTextures;
    ui::CanvasScroll m_scroll;   // draggable H/V scroll bars for large patches
    void  content_bounds( int& cl, int& cr, int& ct, int& cb ) const;  // canvas px

    // Right-click MODULE context menu (Duplicate / Delete).
    int   m_ctx_mod = -1;        // module id the menu targets (-1 == closed)
    int   m_ctx_x = 0, m_ctx_y = 0;
    void  draw_ctx_menu( ui::App& app );
    bool  ctx_menu_mouse( ui::App& app, const ui::MouseEv& e );  // true == consumed
    int   duplicate_module( int id );   // clone slug + params, offset; -> new id

    // ---- interaction state -------------------------------------------------
    bool m_ldown = false;        // left button held (edge detect)

    bool  m_move = false;        // dragging a module by its title bar
    int   m_move_id = 0, m_move_dx = 0, m_move_dy = 0;

    bool  m_knob = false;        // dragging a knob
    int   m_knob_mod = 0, m_knob_param = 0;
    float m_knob_start = 0.f;
    int   m_knob_y0 = 0;

    bool  m_button = false;      // holding a momentary panel button
    int   m_button_mod = 0, m_button_param = 0;

    bool  m_wire = false;        // dragging a cable from an output jack
    int   m_wire_mod = 0, m_wire_out = 0;
    int   m_wire_x = 0, m_wire_y = 0;
    bool  m_wire_ok = false;     // hovering a valid input target
    int   m_hover_mod = 0, m_hover_in = 0;

    // Click-to-connect: click an OUTPUT jack (it lights up / stays armed), then
    // click an INPUT jack to wire them -- alternative to drag-wiring.
    int   m_link_out_mod = -1, m_link_out_port = -1;  // armed output jack (-1 none)
    int   m_press_x = 0, m_press_y = 0;               // press pos (click-vs-drag)

    Uint32 m_last_click_ms = 0;  // knob double-click detection
    int    m_last_knob_mod = -1, m_last_knob_param = -1;

    // ---- palette -----------------------------------------------------------
    bool        m_pal = false;   // palette open
    int         m_pal_scroll = 0;              // list scroll (rows)
    int         m_pal_type_scroll = 0;         // type list scroll (rows)
    std::string m_pal_type;                     // empty == all module types
    bool        m_pal_type_menu = false;       // type dropdown is visible
    bool        m_pal_drag = false;            // module row pressed
    bool        m_pal_dragging = false;        // module row moved beyond click slop
    int         m_pal_drag_index = -1;
    int         m_pal_drag_x = 0, m_pal_drag_y = 0;
    int         m_pal_drag_start_x = 0, m_pal_drag_start_y = 0;
    std::string m_search;                      // live-edited search string
};

} // namespace rackui

//----------------------------------------------------------------------------
//  HOW THE SHELL MOUNTS IT
//
//    using rackui::RackEditorView;
//    RackEditorView rv;
//    rv.rect = { 0, 0, app.w, app.h };            // or a ui::Window body()
//    rv.set_engine( rackNode->engine() );         // may be re-set later
//    app.roots.push_back( &rv );
//    // Re-set rv.rect in app.on_layout to track window resizes.
//
//  Host it inside a ui::Window: window.content = &rv; and the window forwards
//  draw / on_mouse / on_wheel / on_key to it, offset to its body() rect.
//----------------------------------------------------------------------------
#endif // PATCHKNOB_SDLUI_RACK_EDITOR_VIEW_H
