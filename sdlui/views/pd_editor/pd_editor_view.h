//----------------------------------------------------------------------------
//
//  sdlui/views/pd_editor/pd_editor_view.h
//
//  A basic Pure Data patch editor as an SDL widget (ui::Widget).  You visually
//  build a Pd patch -- object boxes with inlets/outlets, wires between them --
//  and it round-trips to a .pd file which libpd loads elsewhere.  This view is
//  PURE UI over a .pd file: it never touches libpd or the audio engine.  After
//  every structural change it save()s the .pd and fires on_changed() so the host
//  can tell libpd to reload the patch.
//
//      * OBJECTS   -- bordered boxes sized to their text.  Inlets draw as small
//        filled squares along the TOP edge, outlets along the BOTTOM edge.
//        obj / msg / floatatom / text records are all modelled as objects.
//      * WIRES     -- a straight line from a source outlet to a dest inlet.
//
//  Interaction:
//      * Drag an object body to move it.
//      * Drag from an OUTLET to an INLET to connect.
//      * Left-click empty canvas -> searchable MODULE PICKER (create an object).
//      * Double-click an object -> edit its text inline.
//      * Right-click an object (or select + Delete/Backspace) -> delete it and
//        every wire touching it.
//      * Wheel -> pan (Shift or horizontal wheel -> pan horizontally).
//
//  Strictly two-tone: every colour comes from ui::theme() roles, so it tracks
//  the live LIGHT / MIDNIGHT theme.
//
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_PD_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_PD_EDITOR_VIEW_H

#include "gui.h"

#include <functional>
#include <string>
#include <vector>

namespace pdui {

// ---- model -----------------------------------------------------------------
// One Pd record that carries a box on the canvas.  `kind` selects the record
// type so we regenerate it verbatim: 'o' obj, 'm' msg, 'f' floatatom, 't' text.
// (x,y) are Pd canvas coords.  nin/nout are the inlet/outlet counts (the .pd
// file does not store them; io_for() derives them from the object text).
struct PdObj
{
    int         x    = 0;
    int         y    = 0;
    std::string text;
    int         nin  = 1;
    int         nout = 1;
    char        kind = 'o';
    // bpatcher: an embedded abstraction window.  bp_w/bp_h are its pixel size
    // (from the referenced patch's graph-on-parent area); 0 => not a bpatcher.
    int         bp_w = 0;
    int         bp_h = 0;
    // Live GUI-atom value (rendered directly, so dragging never re-parses the .pd
    // text every frame).  Same convention as gui_info().val: sliders 0..1, tgl 0/1,
    // nbx/knob natural, radio cell index.  Flushed to `text` only on release.
    double      gui_val = 0.0;
    bool        gui_val_set = false;
};

// A wire.  Indices are into the model vector, in file (record) order -- exactly
// how a .pd "#X connect" line addresses objects.
struct PdConn
{
    int from   = 0;
    int outlet = 0;
    int to     = 0;
    int inlet  = 0;
};

//! The SDL Pure Data patch editor.  Host it in a ui::Window (or mount as a root
//! sized to a rect); see the shell notes at the bottom of this header.
class PdEditorView : public ui::Widget
{
public:
    PdEditorView() {}
    virtual ~PdEditorView() {}

    // ========================================================================
    //  Public API the shell uses
    // ========================================================================

    //! Point the editor at a .pd file.  If it exists it is PARSED into the
    //! model; otherwise the editor starts empty (save() will create it).
    void set_patch_path( const std::string& path );

    //! Re-read the current .pd from disk into the model (e.g. changed on disk).
    void reload_from_disk();

    //! Serialise the model back to the .pd file.  Does NOT fire on_changed --
    //! the caller (or an interactive edit) decides.  Returns false on failure
    //! (no path set / cannot open the file).
    bool save();

    //! Fired AFTER a successful save() following an interactive edit, so the
    //! host can ask libpd to reload the patch.  May be left null.
    std::function<void()> on_changed;

    //! Inlet/outlet counts for an object, keyed by the FIRST word of its text.
    //! Falls back to {1,1}.  Static so it can be reused / unit-tested.
    static void io_for( const std::string& text, int& nin, int& nout );

    // ========================================================================
    //  ui::Widget overrides
    // ========================================================================
    void draw( ui::App& app ) override;
    bool on_mouse( ui::App& app, const ui::MouseEv& e ) override;
    bool on_wheel( ui::App& app, int dx, int dy ) override;
    bool on_key( ui::App& app, SDL_Keycode k ) override;

private:
    // ---- layout constants (canvas units == pixels) -------------------------
    static const int PADX      = 6;    // text inset inside a box, x
    static const int PADY      = 4;    // text inset inside a box, y (h = ch+2*PADY)
    static const int MINW      = 28;   // minimum box width
    static const int IOW       = 7;    // inlet/outlet marker width (visual)
    static const int IOH       = 3;    // inlet/outlet marker height (visual)
    static const int HITPAD    = 9;    // click tolerance around a marker (small -> generous)
    static const int FLAG      = 6;    // msg-box right-edge notch depth
    static const int DBLCLK_MS = 400;  // double-click window
    static const int PAN_STEP  = 24;   // wheel pan / list scroll step
    static const int PICK_W    = 190;  // module-picker panel width
    static const int PICK_ROWS = 12;   // visible rows in the picker list

    enum EditMode { Mode_None = 0, Mode_Pick, Mode_Edit };

    // ---- coordinate + geometry helpers -------------------------------------
    void obj_screen( const PdObj& o, int& sx, int& sy ) const;
    int  box_w( const ui::Font& f, const PdObj& o ) const;
    int  box_h( const ui::Font& f, const PdObj& o ) const;
    // Inlet/outlet marker size, scaled with the zoom (draw + hit-test + wire ends
    // all use these, so they stay consistent -- and the marks track the box size).
    int  iow() const { int v = (int)( IOW * m_zoom + 0.5f ); return v < 3 ? 3 : v; }
    int  ioh() const { int v = (int)( IOH * m_zoom + 0.5f ); return v < 2 ? 2 : v; }
    bool outlet_pos( ui::App& app, int idx, int outlet, int& x, int& y ) const;
    bool inlet_pos ( ui::App& app, int idx, int inlet,  int& x, int& y ) const;

    // ---- hit testing (screen coords) ---------------------------------------
    int  obj_at   ( ui::App& app, int mx, int my ) const;
    bool outlet_at( ui::App& app, int mx, int my, int& idx, int& outlet ) const;
    bool inlet_at ( ui::App& app, int mx, int my, int& idx, int& inlet  ) const;

    // ---- model mutation ----------------------------------------------------
    bool add_conn( int from, int outlet, int to, int inlet );
    void delete_obj( ui::App& app, int idx );
    void create_object( ui::App& app, const std::string& text, int px, int py );
    void commit( ui::App& app );          // save() + on_changed() + redraw
    void reset_interaction();
    void parse();                          // read m_path -> objs_/conns_
    void sanitize_conns();

    // ---- drawing -----------------------------------------------------------
    void draw_obj( ui::App& app, int i );
    void draw_picker( ui::App& app );
    SDL_Rect picker_rect( ui::App& app ) const;
    std::vector<std::string> filtered() const;

    // ---- input sub-handlers ------------------------------------------------
    bool press_left  ( ui::App& app, int mx, int my );
    bool drag_left   ( ui::App& app, int mx, int my );
    bool release_left( ui::App& app, int mx, int my );
    bool picker_mouse( ui::App& app, const ui::MouseEv& e, bool downEdge );
    void open_picker ( ui::App& app, int mx, int my );
    void close_picker( ui::App& app );
    void picker_commit( ui::App& app, bool ok );
    void begin_obj_edit( ui::App& app, int idx );
    void finalize_obj_edit( ui::App& app );

    // ---- model -------------------------------------------------------------
    std::vector<PdObj>  objs_;
    std::vector<PdConn> conns_;

    // ---- file --------------------------------------------------------------
    std::string m_path;
    int         m_canvas_w = 600;
    int         m_canvas_h = 400;

    // ---- view --------------------------------------------------------------
    int   m_ox   = 0;        // pan offset x (screen px)
    int   m_oy   = 0;        // pan offset y
    float m_zoom = 1.0f;     // canvas zoom (mousewheel)
    int   m_sel  = -1;       // selected object index, -1 = none
    ui::CanvasScroll m_scroll;   // draggable H/V scroll bars for large patches
    void  content_bounds( ui::App& app, int& cl, int& cr, int& ct, int& cb ) const;

    // ---- interaction state -------------------------------------------------
    bool     m_ldown = false;   // left button currently held (edge detect)

    // Edit vs run mode (Ctrl+E toggles).  EDIT: move / wire / edit-text, GUI atoms
    // are inert.  RUN: GUI atoms are live (drag sliders, click toggles), objects
    // don't move -- exactly like Pd.
    bool     m_edit_mode = true;

    bool     m_move  = false;   // moving an object body
    int      m_move_obj = -1;
    int      m_move_dx = 0, m_move_dy = 0;
    bool     m_moved = false;

    // GUI-atom interaction (slider drag / toggle / knob / nbx / radio): plain drag
    // sets the value; hold Ctrl to MOVE the widget instead.
    bool     m_gui_drag = false;
    int      m_gui_obj  = -1;
    double   m_gui_press_val = 0.0;   // value at press (for relative nbx/knob drags)
    int      m_gui_press_y   = 0;
    Uint32   m_bng_flash_ms  = 0;     // bang: reset its lit flash after this tick
    int      m_bng_obj       = -1;
    bool     gui_interact( ui::App& app, int oi, int mx, int my, bool press );
    void     flush_gui_value( int oi );   // write the live gui_val into the .pd text

    bool     m_wire  = false;   // dragging a wire from an outlet
    int      m_wire_from = -1, m_wire_outlet = 0;
    int      m_wire_to = -1,   m_wire_inlet  = 0;
    int      m_wire_x = 0, m_wire_y = 0;
    bool     m_wire_ok = false;

    // Click-to-connect: click an OUTLET (it lights up / stays armed), then click
    // an INLET to wire them -- alternative to dragging.
    int      m_link_from = -1, m_link_outlet = 0;  // armed outlet (obj idx, -1 none)
    int      m_press_x = 0, m_press_y = 0;         // press pos (click-vs-drag)

    Uint32   m_last_click_ms  = 0;   // double-click detection
    int      m_last_click_obj = -1;

    // ---- overlay mode ------------------------------------------------------
    EditMode m_mode = Mode_None;
    int      m_edit_obj = -1;        // object whose text is being edited

    // ---- module picker (graphical card grid; drag a card onto the canvas) --
    std::string m_search;            // live-edited search string
    int         m_pick_x = 0, m_pick_y = 0;    // panel anchor (screen coords)
    int         m_pick_cx = 0, m_pick_cy = 0;  // new-object position (Pd coords)
    int         m_pick_scroll = 0;             // grid scroll offset (rows)
    bool        m_pick_drag = false;           // dragging a card out of the palette
    std::string m_pick_drag_name;              // the object being dragged
    int         m_pick_drag_x = 0, m_pick_drag_y = 0;   // ghost position (screen)
    void        draw_obj_preview( ui::App& app, const std::string& name, const SDL_Rect& box );
};

} // namespace pdui

//----------------------------------------------------------------------------
//  HOW THE SHELL MOUNTS IT
//
//    using pdui::PdEditorView;
//    PdEditorView pd;
//    pd.rect = { 0, 0, app.w, app.h };            // or a ui::Window body()
//    pd.set_patch_path( "patches/synth.pd" );     // parses if the file exists
//    pd.on_changed = []{ libpd_reload(...); };    // save() already happened
//    app.roots.push_back( &pd );
//    // Re-set pd.rect in app.on_layout to track window resizes.
//----------------------------------------------------------------------------
#endif // PATCHKNOB_SDLUI_PD_EDITOR_VIEW_H
