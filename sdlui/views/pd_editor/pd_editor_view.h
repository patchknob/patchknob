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
    // Bang flash deadline (SDL ticks); >0 while lit.  Per-object so several bangs can
    // flash at once when a message fans out through the running patch.
    unsigned    flash_until = 0;
    // Faithful round-trip.  kind 'x' is a PASSTHROUGH gobj (a subpatch/graph/array/
    // scalar the editor doesn't edit but must preserve): `raw` holds its verbatim
    // .pd record(s), emitted unchanged on save.  `post` holds any NON-gobj records
    // (#A data, #X coords/#X f/#X declare/#X struct, ...) that followed this gobj in
    // the source, so they survive the round-trip in place.  Both empty for normal
    // editable objects.
    std::string raw;
    std::string post;
    // If this passthrough gobj is a graph containing an array, `arr` holds its data
    // so draw() can render the waveform (parsed once, not re-scanned per frame).
    // A data-structure SCALAR ('x' gobj) sets `tmpl` to its template name and reuses
    // `arr` for its float field values, so draw() can render it from the template.
    std::vector<float> arr;
    std::string        tmpl;
};

// --- data structures --------------------------------------------------------
// A value source in a template's drawing instruction: a constant, or a scalar field.
struct PdFieldDesc { bool isConst = true; float val = 0.f; std::string field; };
// One drawing instruction attached to a template (drawpolygon/drawcurve/drawnumber).
struct PdDraw
{
    enum Kind { Polygon, Curve, Number } kind = Polygon;
    bool                     closed = false;    // filled* -> closed path
    std::vector<PdFieldDesc> coords;            // Polygon/Curve: x0,y0,x1,y1,...
    PdFieldDesc              field, x, y;        // Number: draw field value at (x,y)
};
// A data-structure template: named fields (in order) + the drawing instructions
// that render each scalar of this type.
struct PdTemplate
{
    std::string              name;
    std::vector<std::string> fields;
    std::vector<PdDraw>      draws;
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

    //! Load the patch from in-memory .pd TEXT (the canonical storage now -- the patch
    //! is held on the node and saved with the project, no file on disk).  Empty text
    //! => empty patch.
    void set_patch_text( const std::string& text );

    //! Serialise the current model to .pd text (what the host stores on the node).
    std::string patch_text() const;

    //! Point the editor at a .pd file to IMPORT (external interop).  Parses it into
    //! the model; the file is not the source of truth.
    void set_patch_path( const std::string& path );
    void set_export_path( const std::string& path ) { m_path = path; }

    //! Re-read the current import .pd from disk into the model.
    void reload_from_disk();

    //! EXPORT the model to the .pd file at set_patch_path() (external interop only).
    //! Does NOT fire on_changed.  Returns false if no path is set / cannot write.
    bool save();

    //! Fired after a STRUCTURAL edit, so the host can pull patch_text() and reload
    //! libpd (adc~/dac~ ports may have changed).  May be left null.
    std::function<void()> on_changed;
    std::function<void()> on_open_file, on_save_file, on_save_file_as;

    //! Fired after a GUI value edit in RUN mode: the host should STORE patch_text() on
    //! the node WITHOUT reloading (the live value already went out via on_gui_send),
    //! so the project keeps the latest value without restarting the patch.  May be null.
    std::function<void(const std::string& text)> on_store_text;

    //! Fired when a GUI atom is driven in RUN mode, so the host can inject the value
    //! into the RUNNING libpd instance (recv = the atom's receive symbol).  Unlike
    //! on_changed (a file reload), this drives the patch LIVE -- a toggle can start a
    //! [metro] the instant you click it.  May be left null.
    std::function<void(const std::string& recv, float val)> on_gui_send;
    std::function<void(const std::string& recv)>            on_gui_bang;

    //! Every GUI atom's SEND symbol, so the host can libpd_bind them for feedback.
    std::vector<std::string> gui_send_symbols() const;

    //! Reflect a message that reached a GUI atom in the running patch onto its widget
    //! (bang -> flash; float -> set value), keyed by the atom's SEND symbol.  This is
    //! what makes data VISIBLY flow through the wires (a banged bang lights up).
    void apply_gui_feedback( const std::string& sendSym, bool bang, float val );

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

    enum EditMode { Mode_None = 0, Mode_Pick, Mode_Edit, Mode_Props };

    // ---- IEMGUI Properties dialog (right-click a GUI atom) ------------------
    // One editable field maps to a RAW token in the object's .pd record: `sym`
    // fields are symbols (send/receive/label), the rest are numbers.
    struct PropField { std::string name; std::string val; int tok = -1; bool sym = false; };
    int                     m_prop_obj  = -1;   // object whose properties are open
    int                     m_prop_edit = -1;   // field row being text-edited, -1 none
    std::vector<PropField>  m_props;
    void open_props ( ui::App& app, int oi );
    void close_props( ui::App& app );
    void apply_prop ( ui::App& app, int fieldIdx );   // write one field back + commit
    void draw_props ( ui::App& app );
    bool props_mouse( ui::App& app, const ui::MouseEv& e, bool downEdge );
    SDL_Rect props_rect( ui::App& app ) const;

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
    int  wire_at  ( ui::App& app, int mx, int my ) const;   // hit-test a wire -> conn index

    // ---- live control ------------------------------------------------------
    void live_send_gui( ui::App& app, int oi );            // push a GUI atom's value live
    std::string gui_recv_symbol( const PdObj& o ) const;   // the atom's receive symbol ("" none)
    std::string gui_send_symbol( const PdObj& o ) const;   // the atom's send symbol ("" none)

    // ---- model mutation ----------------------------------------------------
    bool add_conn( int from, int outlet, int to, int inlet );
    void delete_obj( ui::App& app, int idx );
    void create_object( ui::App& app, const std::string& text, int px, int py );
    void commit( ui::App& app );          // save() + on_changed() + redraw
    void reset_interaction();
    void release_inline_edit();      // drop any live edit pointing into objs_
    void parse();                          // read m_path file -> parse_text()
    void parse_text( const std::string& content );   // .pd text -> objs_/conns_
    std::string serialize() const;         // objs_/conns_ -> .pd text
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
    std::vector<PdObj>     objs_;
    std::vector<PdConn>    conns_;
    std::vector<PdTemplate> m_templates;   // data-structure templates (name -> draws)
    void build_templates();                // (re)scan the patch for struct + draw objects
    const PdTemplate* find_template( const std::string& name ) const;
    void draw_scalar( ui::App& app, int i );   // render a scalar via its template

    // ---- file --------------------------------------------------------------
    std::string m_path;
    int         m_canvas_w = 600;
    int         m_canvas_h = 400;
    // Faithful round-trip: the verbatim top #N canvas header, and any non-gobj
    // records that appeared before the first gobj (preserved on save).
    std::string m_header;
    std::string m_preamble;

    // ---- view --------------------------------------------------------------
    int   m_ox   = 0;        // pan offset x (screen px)
    int   m_oy   = 0;        // pan offset y
    float m_zoom = 1.0f;     // canvas zoom (mousewheel)
    int   m_sel  = -1;       // selected object index, -1 = none
    int   m_sel_wire = -1;   // selected connection index (Delete removes it), -1 = none
    unsigned m_recv_serial = 0;  // next auto receive-symbol id (pkui<N>) for live GUI atoms
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

    // Number-box keyboard entry: click an nbx (RUN mode) to TYPE a value, Enter to
    // commit (Escape cancels).  Mirrors the object-text edit, but parses a number.
    int         m_num_edit = -1;          // nbx index being typed into, -1 = none
    std::string m_num_buf;                // digits typed so far
    void begin_num_edit( ui::App& app, int oi );
    void finalize_num_edit( ui::App& app, bool commit );

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
    // begin_text() hands App a std::string* that lives INSIDE objs_, so any
    // push_back/erase/clear on that vector would leave App::text_target
    // dangling.  Remember the exact pointer and the App so every mutation
    // path can hand it back first (App::end_text_if only compares, never
    // dereferences).
    std::string* m_edit_ptr = nullptr;
    ui::App*     m_edit_app = nullptr;

    // ---- module picker (graphical card grid; drag a card onto the canvas) --
    std::string m_search;            // live-edited search string
    int         m_pick_cat = 0;      // active category filter (0 = All; see PICK_CATS)
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
