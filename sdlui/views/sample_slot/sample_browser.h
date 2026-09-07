//----------------------------------------------------------------------------
//  sdlui/views/sample_slot/sample_browser.h
//
//  The SHARED disk browser for samples.  Both samplers mount it:
//      * the SMPL-1 rack module's editor window
//      * the Buzz Sampler instrument's editor
//
//  A plain directory list -- folders first, then audio files -- with keyboard
//  navigation, a type-ahead filter, a clickable path breadcrumb, and a DRAG
//  SOURCE: press on a row and drag it onto a drop target to load it.  The
//  browser never touches audio or the sampler engine; it only hands over a
//  DragPayload and lets whoever is under the pointer decide what to do.
//
//  A .sf2/.sf3 file gets a "+"/"-" affordance and expands IN PLACE like a
//  folder: double-clicking it (or hitting the affordance) splices its presets
//  in as rows directly below it, "[bank:program] Name".  Dragging one of those
//  rows out hands over an Sf2Preset DragPayload (bank/program, not a path
//  alone -- a preset is not a file).  The parse that lists them is cached per
//  path, so collapsing and re-expanding costs nothing.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SAMPLE_BROWSER_H
#define PATCHKNOB_SDLUI_SAMPLE_BROWSER_H

#include "gui.h"
#include "engine/sf2/sf2_reader.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sampleslot {

//! What a completed drag out of the browser hands to whoever is under the
//! pointer at release.  A plain file just needs its path; a soundfont PRESET
//! is not a file -- the shell needs bank/program to route the drop into
//! importPresetIntoSampler(), so the payload carries both plus a display name.
struct DragPayload {
    enum Kind { File, Sf2Preset };
    Kind        kind = File;
    std::string path;          //!< the file, or the .sf2 for Sf2Preset
    int         bank = 0;      //!< Sf2Preset only
    int         program = 0;   //!< Sf2Preset only
    std::string label;         //!< display name ("Overdrive Guitar")
};

//============================================================================
//  skin -- the painting + popup primitives the two views in this folder share.
//
//  They live in one place because both views kept hitting the same two bugs:
//
//    * boxes sized as `text.size() * cw` -- a BYTE count times a NOMINAL glyph
//      advance.  That is wrong twice over (multi-byte characters count more
//      than once, and the real advance is fractional once the UI scale is not
//      1.0), and the symptom was labels spilling out of their buttons and path
//      names cut mid-word.  Everything here measures with Font::text_w().
//
//    * popup menus whose geometry was computed inside draw() and then RE-derived
//      by the click handler.  The two drifted apart the moment the menu wrapped
//      into columns, so the row you clicked was not the row you saw.  Popup
//      lays out ONCE per event, through layout(), and both the painter and the
//      hit-test read the rects it produced.
//============================================================================
namespace skin {

//! The one padding unit for these views.  Derived from the font so the whole
//! layout stays proportional at any UI scale instead of drifting apart.
inline int pad( const ui::Font& f ) { return std::max( 3, f.ch() / 4 ); }
//! Standard control height: one line of text plus a padding unit above/below.
inline int row_h( const ui::Font& f ) { return f.ch() + 2 * pad( f ); }

//! Truncate `s` to `maxw` PIXELS with a measured ellipsis (never a character
//! count -- see the note above).  Returns "" when there is no room at all.
inline std::string fit( const ui::Font& f, std::string s, int maxw ) {
    if ( maxw <= 0 ) return std::string();
    if ( f.text_w( s ) <= maxw ) return s;
    const std::string ell = "...";
    if ( f.text_w( ell ) > maxw ) {
        std::string dots = ell;
        while ( !dots.empty() && f.text_w( dots ) > maxw ) dots.pop_back();
        return dots;
    }
    while ( !s.empty() && f.text_w( s + ell ) > maxw ) s.pop_back();
    return s.empty() ? ell : s + ell;
}

//! Same, but keeps the TAIL: for a filesystem path the leaf is what identifies
//! it, so "C:\...\drums\kicks" beats "C:\Users\lain\Desktop\samp...".
inline std::string fit_tail( const ui::Font& f, std::string s, int maxw ) {
    if ( maxw <= 0 ) return std::string();
    if ( f.text_w( s ) <= maxw ) return s;
    const std::string ell = "...";
    if ( f.text_w( ell ) > maxw ) return fit( f, s, maxw );
    while ( !s.empty() && f.text_w( ell + s ) > maxw ) s.erase( s.begin() );
    return ell + s;
}

//! Rounded filled block.  An alpha < 255 turns blending on for the fill and
//! puts it back to NONE afterwards -- this codebase draws opaque by default and
//! a leaked blend mode corrupts everything painted after it.
inline void fill_round( SDL_Renderer* r, SDL_Rect q, int rad, ui::Color c ) {
    if ( !r || q.w <= 0 || q.h <= 0 || c.a == 0 ) return;
    if ( rad * 2 > q.w ) rad = q.w / 2;
    if ( rad * 2 > q.h ) rad = q.h / 2;
    const bool blend = c.a < 255;
    if ( blend ) SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_BLEND );
    if ( rad < 1 ) {
        ui::set_color( r, c );
        SDL_RenderFillRect( r, &q );
    } else {
        ui::set_color( r, c );
        for ( int yy = 0; yy < q.h; ++yy ) {
            int dy = -1, dx = 0;
            if ( yy < rad )                 dy = rad - 1 - yy;
            else if ( yy >= q.h - rad )     dy = yy - ( q.h - rad );
            if ( dy >= 0 )
                dx = rad - (int)std::floor( std::sqrt( (double)( rad * rad - dy * dy ) ) );
            SDL_Rect ln{ q.x + dx, q.y + yy, q.w - 2 * dx, 1 };
            if ( ln.w > 0 ) SDL_RenderFillRect( r, &ln );
        }
    }
    if ( blend ) SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_NONE );
}

//! 1px rounded outline matching fill_round's silhouette.
inline void frame_round( SDL_Renderer* r, SDL_Rect q, int rad, ui::Color c ) {
    if ( !r || q.w <= 0 || q.h <= 0 || c.a == 0 ) return;
    if ( rad * 2 > q.w ) rad = q.w / 2;
    if ( rad * 2 > q.h ) rad = q.h / 2;
    const bool blend = c.a < 255;
    if ( blend ) SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_BLEND );
    if ( rad < 1 ) {
        ui::set_color( r, c );
        SDL_RenderDrawRect( r, &q );
    } else {
        ui::set_color( r, c );
        int prev = -1;
        for ( int yy = 0; yy < q.h; ++yy ) {
            int dy = -1, dx = 0;
            if ( yy < rad )                 dy = rad - 1 - yy;
            else if ( yy >= q.h - rad )     dy = yy - ( q.h - rad );
            if ( dy >= 0 )
                dx = rad - (int)std::floor( std::sqrt( (double)( rad * rad - dy * dy ) ) );
            const int p0 = prev < 0 ? dx : prev;
            const int lo = std::min( dx, p0 ), hi = std::max( dx, p0 );
            SDL_Rect l{ q.x + lo, q.y + yy, hi - lo + 1, 1 };
            SDL_RenderFillRect( r, &l );
            SDL_Rect rr{ q.x + q.w - 1 - hi, q.y + yy, hi - lo + 1, 1 };
            SDL_RenderFillRect( r, &rr );
            prev = dx;
        }
        SDL_Rect top{ q.x + rad, q.y, q.w - 2 * rad, 1 };
        if ( top.w > 0 ) SDL_RenderFillRect( r, &top );
        SDL_Rect bot{ q.x + rad, q.y + q.h - 1, q.w - 2 * rad, 1 };
        if ( bot.w > 0 ) SDL_RenderFillRect( r, &bot );
    }
    if ( blend ) SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_NONE );
}

//! Translucent wash.  Turning blending ON is not optional: ui::fill_rect()
//! honours the alpha byte but not the mode, so an "80% tint" drawn without
//! this paints a solid block.  The mode is always put back to NONE, because
//! everything else in this UI assumes opaque drawing.
inline void wash_blend( SDL_Renderer* r, const SDL_Rect& q, ui::Color c, Uint8 a ) {
    if ( !r || q.w <= 0 || q.h <= 0 ) return;
    SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_BLEND );
    ui::set_color( r, ui::Color{ c.r, c.g, c.b, a } );
    SDL_RenderFillRect( r, &q );
    SDL_SetRenderDrawBlendMode( r, SDL_BLENDMODE_NONE );
}

//! The four states EVERY interactive control in these views has to show.  They
//! used to be two (a disabled button and an idle one were the same rectangle,
//! so there was no way to tell a greyed-out UNDO from a live one at a glance).
enum State { StDisabled = 0, StIdle, StHover, StActive };

//! One pill painter, so every button in both views has identical metrics.
inline void pill( SDL_Renderer* r, const ui::Font& f, const SDL_Rect& q,
                  const std::string& label, State st, bool center = true ) {
    if ( q.w <= 0 || q.h <= 0 ) return;
    const ui::Theme& t = ui::theme();
    const int rad = std::max( 2, q.h / 4 );
    ui::Color fg = t.text;
    switch ( st ) {
        case StActive:
            fill_round( r, q, rad, t.accent );
            frame_round( r, q, rad, t.accent );
            fg = t.bg;
            break;
        case StHover:
            fill_round( r, q, rad, t.panel );
            fill_round( r, q, rad, ui::Color{ t.accent.r, t.accent.g, t.accent.b, 56 } );
            frame_round( r, q, rad, t.accent );
            fg = t.text;
            break;
        case StIdle:
            fill_round( r, q, rad, t.panel );
            frame_round( r, q, rad, ui::Color{ t.dim.r, t.dim.g, t.dim.b, 170 } );
            fg = t.text;
            break;
        default:                                  // StDisabled
            fill_round( r, q, rad, ui::Color{ t.panel.r, t.panel.g, t.panel.b, 130 } );
            frame_round( r, q, rad, ui::Color{ t.dim.r, t.dim.g, t.dim.b, 70 } );
            fg = ui::Color{ t.dim.r, t.dim.g, t.dim.b, 200 };
            break;
    }
    const int p = pad( f );
    const std::string s = fit( f, label, q.w - 2 * p );
    const int tw = f.text_w( s );
    const int tx = center ? q.x + ( q.w - tw ) / 2 : q.x + p;
    f.draw( r, tx, q.y + ( q.h - f.ch() ) / 2, s, fg );
}

//! A quiet framed group with a small header, so related controls read as one
//! block instead of a loose field of widgets.  Returns the inner content rect.
inline SDL_Rect section( SDL_Renderer* r, const ui::Font& f, const SDL_Rect& q,
                         const std::string& title ) {
    const ui::Theme& t = ui::theme();
    const int p = pad( f );
    fill_round( r, q, std::max( 2, p ), ui::Color{ t.panel.r, t.panel.g, t.panel.b, 210 } );
    frame_round( r, q, std::max( 2, p ), ui::Color{ t.dim.r, t.dim.g, t.dim.b, 120 } );
    int top = q.y + p;
    if ( !title.empty() && q.h > f.ch() * 2 ) {
        f.draw( r, q.x + 2 * p, top, fit( f, title, q.w - 4 * p ), t.dim );
        top += f.ch() + p;
        ui::hline( r, q.x + p, q.x + q.w - p - 1, top - p / 2,
                   ui::Color{ t.dim.r, t.dim.g, t.dim.b, 90 } );
    }
    return SDL_Rect{ q.x + 2 * p, top, q.w - 4 * p, q.y + q.h - p - top };
}

//----------------------------------------------------------------------------
//  Popup -- one right-click menu implementation for both views.
//----------------------------------------------------------------------------
struct MenuRow {
    std::string label;
    int  op        = 0;
    bool separator = false;
    bool enabled   = true;
    bool check     = false;      //!< reserve the tick gutter
    bool checked   = false;
};

class Popup {
public:
    std::vector<MenuRow> rows;

    bool open() const { return m_open; }
    void show( int x, int y ) { m_open = true; m_x = x; m_y = y; m_sel = -1;
                                m_armed = false; }
    void close() { m_open = false; m_sel = -1; m_armed = false; }
    //! A press that OPENED the menu must not also activate a row: the button is
    //! still down, and motion events carry `pressed == true`, so the old code
    //! closed the menu (running whatever was under the pointer) the instant you
    //! nudged the mouse before letting go.  The menu arms on the first release
    //! and only then accepts a press.
    void arm() { m_armed = true; }
    bool armed() const { return m_armed; }

    //! The single geometry pass.  Call before drawing AND before hit-testing.
    void layout( const ui::Font& f, const SDL_Rect& bounds );
    void draw( ui::App& app, const ui::Font& f );
    //! Row index under (x,y), or -1.  Disabled rows report -1 so a greyed row
    //! cannot fire -- the old menu drew rows grey and ran them anyway.
    int  hit( int x, int y ) const;
    bool inside( int x, int y ) const {
        return x >= m_box.x && x < m_box.x + m_box.w &&
               y >= m_box.y && y < m_box.y + m_box.h;
    }
    const SDL_Rect& box() const { return m_box; }
    //! Keyboard driving: Up/Down/Home/End move, Enter activates, Esc closes.
    //! Returns true when the key was consumed; `activated` gets the row index.
    bool key( SDL_Keycode k, int& activated );
    int  cursor() const { return m_sel; }

private:
    bool m_open = false, m_armed = false;
    int  m_x = 0, m_y = 0, m_sel = -1;
    SDL_Rect m_box{ 0, 0, 0, 0 };
    std::vector<SDL_Rect> m_rects;
    int  m_first_enabled() const;
    void m_step( int dir );
};

} // namespace skin

class SampleBrowser : public ui::Widget {
public:
    struct Item {
        std::string name;
        std::string path;
        bool        dir  = false;
        long long   size = 0;
        long long   mtime = 0;
        // ---- soundfont support ----------------------------------------------
        //! A real .sf2/.sf3 FILE row (never true for a spliced-in preset row):
        //! it passed sf2::looksLikeSoundFont() and gets the "+"/"-" affordance.
        bool        isSoundFont   = false;
        //! A synthesized row listing ONE preset of the soundfont immediately
        //! above it in the list -- spliced in while that file is expanded.
        //! `path` is the OWNING .sf2's path (not a real, independently
        //! openable file), matching DragPayload::path for Sf2Preset.
        bool        isPreset      = false;
        int         presetBank    = 0;
        int         presetProgram = 0;
    };

    //! Show `dir` (falls back to the user's home / cwd when empty or unreadable).
    void set_dir( const std::string& dir );
    const std::string& dir() const { return m_dir; }
    void refresh() { scan(); }

    //! Path of the highlighted row ("" when none / a folder).
    std::string selected_path() const;

    // ---- drag source -------------------------------------------------------
    //! True while a file is being dragged out of the list.
    bool dragging() const { return m_dragging; }
    //! True from the press on a row until the button is released, even
    //! before the drag threshold trips -- the host must keep routing mouse
    //! events here or the press can never BECOME a drag.
    bool pressing() const { return m_press; }
    //! Forget any in-flight press (the host took the gesture elsewhere).
    void cancel_press() { m_press = false; m_dragging = false;
                          m_dragPayload = DragPayload(); }
    //! The host sees EVERY mouse event; this widget does not (it is routed by
    //! pointer position).  Tracking the press edge locally therefore went stale
    //! whenever a release was delivered elsewhere, and the next click was not
    //! recognised as a new click -- which is why folders stopped opening.  The
    //! host computes the edge and hands it in.
    void set_down_edge( bool e ) { m_downEdge = e; }
    //! Called when the drag is released; the shell routes it to whatever is
    //! under the pointer (normally a SampleSlotEditor, or -- for an Sf2Preset
    //! payload -- the sampler window).
    std::function<void(const DragPayload& payload, int x, int y)> on_drag_drop;

    //! Double-click / Enter: load into the current slot directly.
    std::function<void(const std::string& path)> on_activate;
    //! Audition-on-select.  Fires ONLY when the selected row actually changes:
    //! it decodes a whole file, so firing it per keystroke (which is what
    //! happened while type-ahead filtering) stalled the UI on every letter.
    std::function<void(const std::string& path)> on_select;
    //! Human-readable notices ("nothing matches /kick") for the shell's status.
    std::function<void(const std::string& msg)> on_status;

    //! Which pane owns the keyboard, so the list can show a focus ring.
    void set_focused( bool f ) { m_focused = f; }
    bool focused() const { return m_focused; }

    void draw( ui::App& app ) override;
    //! Drag ghost + context menu, painted AFTER the sibling editor.  They used
    //! to be drawn inside draw(), which runs first, so the editor painted over
    //! the ghost exactly when the pointer was over the drop target.
    void draw_overlay( ui::App& app );
    bool on_mouse( ui::App& app, const ui::MouseEv& e ) override;
    bool on_wheel( ui::App& app, int dx, int dy ) override;
    bool on_key( ui::App& app, SDL_Keycode k ) override;
    void cancel_interaction( ui::App& app ) override;

    //! True while the context menu owns the pointer (the host must keep routing
    //! events here even when the pointer leaves the list).
    bool menu_open() const { return m_menu.open(); }

private:
    void scan();
    void scan_drives();
    //! ONE layout pass: header, breadcrumb, drive chips, list body.  draw() and
    //! on_mouse() both call it, so a chip can never be hit where it was not
    //! drawn (drive chips used to keep the rect from a wider frame and stayed
    //! clickable off the end of the strip).
    void layout( const ui::Font& f );
    int  row_at( int y ) const;
    int  visible_rows() const;
    void move_sel( int to, bool audition );
    void build_menu();
    void run_menu( ui::App& app, int op );
    void set_dir_remember( const std::string& path );
    //! (Un)expand the soundfont file at `idx` in place: parses it (lazily,
    //! cached per path so re-expanding is free) and re-scans so its presets
    //! are spliced in as rows right below it.  A file that fails to parse
    //! degrades quietly -- a status note, never a crash -- and stays collapsed.
    void toggle_sf2_expand( int idx );

    //! `ell` marks the leading chip of a trail that had to be shortened: it
    //! draws as "..." but still navigates, so a deep path stays clickable
    //! instead of being cut to a meaningless prefix.
    struct Crumb { std::string label, path; SDL_Rect r{ 0, 0, 0, 0 }; bool ell = false; };
    std::vector<Crumb> m_crumbs;
    struct Drive { std::string label, path; SDL_Rect r{ 0, 0, 0, 0 }; };
    std::vector<Drive> m_drives;

    std::string        m_dir;
    std::vector<Item>  m_items;
    std::string        m_filter;
    Uint32             m_filterMs = 0;   //!< type-ahead expiry
    int   m_sel = 0, m_scroll = 0;
    int   m_rowH = 16;
    bool  m_dragging = false;
    bool  m_press = false;
    bool  m_downEdge = false;     // set by the host each event
    bool  m_focused = true;
    bool  m_showHidden = false;
    int   m_sort = 0;             // 0 name, 1 size, 2 newest first
    //! Scroll follows the selection only when the SELECTION moved (keyboard,
    //! directory change) -- never on a redraw, or the wheel is yanked back to
    //! the selected row every frame and scrolling looks broken.
    bool  m_followSel = true;
    Uint32 m_lastClickMs = 0;   // double-click: open folder / load file
    int   m_lastIdx = -1;
    int   m_lastClickX = 0, m_lastClickY = 0;
    int   m_pressX = 0, m_pressY = 0;
    int   m_hotRow = -1;        // row under the pointer (hover feedback)
    DragPayload m_dragPayload;  // what a completed drag on this press would hand over
    std::string m_note;         //!< empty-state / no-match line
    // ---- soundfont expansion ------------------------------------------------
    //! One cached headers-only parse per path.  8 ms / 6.4 MB even for an
    //! 800 MB bank, so the cost that matters is not the parse -- it's paying it
    //! twice.  `ok == false` remembers a file that failed to parse, so a
    //! collapsed-then-reopened row does not retry (and re-log) every time.
    struct Sf2Entry {
        bool        ok = false;
        std::string error;
        PatchKnob::engine::sf2::SoundFont font;
    };
    std::map<std::string, Sf2Entry> m_sf2Cache;     // keyed by file path
    std::set<std::string>           m_sf2Expanded;  // paths currently expanded
    //! Walking up used to dump you at the top of the parent with no idea where
    //! you had been; remembering the leaf puts the cursor back on it.
    std::string m_cameFrom;

    // geometry produced by layout()
    SDL_Rect m_head{ 0, 0, 0, 0 }, m_crumbBar{ 0, 0, 0, 0 };
    SDL_Rect m_driveBar{ 0, 0, 0, 0 }, m_list{ 0, 0, 0, 0 };
    SDL_Rect m_filterChip{ 0, 0, 0, 0 };
    skin::Popup m_menu;
    int   m_menuRow = -1;       //!< row the menu was opened on
};

} // namespace sampleslot

#endif // PATCHKNOB_SDLUI_SAMPLE_BROWSER_H
