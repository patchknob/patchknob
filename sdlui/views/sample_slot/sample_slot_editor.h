//----------------------------------------------------------------------------
//  sdlui/views/sample_slot/sample_slot_editor.h
//
//  The SHARED sample editor.  It edits anything implementing
//  PatchKnob::engine::ISampleSlot, which today means BOTH:
//      * the SMPL-1 rack module
//      * the Buzz-hosted Sampler instrument (through an adapter)
//
//  It is deliberately NOT the arrange-window warp editor (samped::SampleEditorView):
//  that one is bound to arrange region indices and is about time-stretching a
//  clip on the timeline.  This one is about a sampler's playback region --
//  waveform, start/end, loop points -- and knows nothing about the timeline.
//
//      * bipolar min/max waveform, fetched as PEAKS so the UI never holds a
//        pointer into audio the render thread may swap under it
//      * draggable START / END / LOOP START / LOOP END markers, snapped to the
//        pixel grid and clamped so they can never cross
//      * scroll + zoom (wheel = zoom AT THE POINTER, shift+wheel = scroll)
//      * DROP TARGET: a path dragged from the disk browser loads into the slot
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SAMPLE_SLOT_EDITOR_H
#define PATCHKNOB_SDLUI_SAMPLE_SLOT_EDITOR_H

#include "gui.h"
#include "engine/sample_slot.h"
#include "sample_browser.h"

#include <functional>
#include <string>
#include <vector>

namespace sampleslot {

class SampleSlotEditor : public ui::Widget {
public:
    typedef PatchKnob::engine::ISampleSlot ISampleSlot;

    //! Bind the slot being edited (null clears).  `title` is shown in the strip.
    void set_slot( ISampleSlot* slot, const std::string& title = std::string() );
    ISampleSlot* slot() const { return m_slot; }

    //! Engine sample rate, used when a dropped/loaded file must be resampled.
    void set_sample_rate( double sr ) { m_sr = sr > 0.0 ? sr : 48000.0; }

    //! A file was dropped (or double-clicked in the browser) onto this editor.
    //! Loads it into the bound slot; returns false (and sets status) on failure.
    bool load_path( const std::string& path );

    //! Live drag feedback from the browser: the path currently under the mouse
    //! while a drag is in flight, or "" when nothing is being dragged.
    void set_drag_hover( bool over ) { m_dragOver = over; }
    bool wants_drop( int x, int y ) const;

    //! Fired after a successful load, so the shell can refresh a title bar etc.
    std::function<void(const std::string& path)> on_loaded;
    //! Human-readable status (load errors) for the shell's status line.
    std::function<void(const std::string& msg)> on_status;
    //! Ask the shell for a save path (it owns the file dialog).
    std::function<bool(std::string& pathInOut)> on_pick_save_path;
    struct ContextAction { std::string label; std::function<void()> run; };
    void set_context_actions( std::vector<ContextAction> actions ) { m_contextActions = std::move(actions); }
    bool mouse_capture_active() const { return m_selDrag || m_drag >= 0 || m_dragSlice >= 0 ||
                                               m_dragXfade || m_dragXfadeField || m_menu.open(); }

    void draw( ui::App& app ) override;
    bool on_mouse( ui::App& app, const ui::MouseEv& e ) override;
    bool on_wheel( ui::App& app, int dx, int dy ) override;
    bool on_key( ui::App& app, SDL_Keycode k ) override;
    //! Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y from the toolkit's undo route.  Undoes the
    //! SAMPLE edit (ISampleSlot::sampleUndo/Redo) the toolbar advertises;
    //! returns false when the slot's history is empty so project undo still
    //! works.
    bool on_undo( ui::App& app, bool redo ) override;
    //! Drop every in-flight drag and close the menu.  Without this a release
    //! delivered somewhere else (window drag, focus loss) left the editor still
    //! "holding" a marker: the next pointer move dragged it with no button down.
    void cancel_interaction( ui::App& app ) override;

private:
    //! ONE geometry pass.  draw(), on_mouse(), on_wheel() and on_key() all call
    //! it before touching a rect, so a control can never be hit-tested where it
    //! was not painted -- the toolbar used to be laid out inside draw() only.
    void layout( const ui::Font& f );
    SDL_Rect wave_rect() const { return m_wave; }
    //! Normalised 0..1 position of a pixel x within the visible span.
    float x_to_norm( int x ) const;
    int   norm_to_x( float n ) const;
    int   marker_at( int x, int y ) const;      // -1 == none
    //! Recompute what the pointer is over.  Called from draw() as well as from
    //! on_mouse(), because this widget only RECEIVES mouse events while the
    //! pointer is inside it: relying on events alone left the last hovered
    //! button lit (and its tooltip on screen) after the pointer had left.
    bool  update_hover( int mx, int my );
    void  clamp_markers();
    void  rebuild_peaks( int buckets );
    void  set_status( const std::string& msg );
    void  clamp_view();
    //! Zoom the visible window by `factor`, holding the file position under
    //! pixel `anchorX` still (the header has always promised zoom-at-pointer).
    void  zoom_at( float factor, int anchorX );

    ISampleSlot* m_slot = nullptr;
    std::string  m_title;
    double       m_sr = 48000.0;

    // visible window over the file, normalised
    float m_viewStart = 0.f, m_viewEnd = 1.f;

    std::vector<float> m_min, m_max;   // cached peaks for the current view
    int    m_peakBuckets = 0;
    int    m_peakFrames  = 0;          // frame count the cache was built from
    float  m_peakVs = 0.f, m_peakVe = 1.f;
    // Full-file peak MIP (256 source frames per bucket), built with ONE walk of
    // the file and reused by every zoomed-out rebuild.  Without it each zoom or
    // scroll step re-walked the whole visible span through samplePeaks() --
    // measured 39.6 ms per step on a ten-minute sample.  Rebuilt only when the
    // AUDIO changes (mark_audio_changed), never for view moves.
    std::vector<float> m_mipMin, m_mipMax;
    int    m_mipFrames = -1;
    //! Audio (not just the view) changed: the visible peaks, the mip and the
    //! footer statistics are all stale.
    void mark_audio_changed() { m_peakFrames = -1; m_mipFrames = -1; }

    // ---- geometry produced by layout() -------------------------------------
    SDL_Rect m_strip{0,0,0,0};      // title + status
    SDL_Rect m_toolbar{0,0,0,0};
    SDL_Rect m_ruler{0,0,0,0};
    SDL_Rect m_wave{0,0,0,0};
    SDL_Rect m_footer{0,0,0,0};     // readout row (was drawn OVER the waveform)
    SDL_Rect m_xfadeField{0,0,0,0}; // draggable crossfade-length readout

    // ---- selection + tools -------------------------------------------------
    struct Tool { SDL_Rect r; const char* label; const char* tip; int op; };
    std::vector<Tool> m_tools;
    void  build_tools( const ui::Font& f );
    //! Shared by the painter and the click handler, so a greyed-out control is
    //! genuinely inert instead of merely looking inert.
    bool  op_enabled( int op ) const;
    // ---- right-click menu ---------------------------------------------------
    //  Everything that is a one-shot ACTION lives here.  Only controls whose
    //  STATE you need to see at a glance stay on the toolbar as buttons.
    void  build_menu();
    void  open_menu( int x, int y, bool sliceMenu );
    void  run_tool( int op );
    std::vector<ContextAction> m_contextActions;
    skin::Popup m_menu;
    int   m_menuSlice = -1;         // slice the slice-menu was opened on
    int   m_toolRows = 1;           // toolbar wraps; layout() follows it
    //! Selection as normalised positions; m_selA may be > m_selB while dragging.
    float sel_lo() const { return m_selA < m_selB ? m_selA : m_selB; }
    float sel_hi() const { return m_selA < m_selB ? m_selB : m_selA; }
    bool  has_sel() const { return m_hasSel && sel_hi() - sel_lo() > 1e-6f; }
    int64_t sel_from() const;
    int64_t sel_to() const;
    //! Selection if there is one, else the whole file.
    void  op_range( int64_t& from, int64_t& to ) const;

    float m_selA = 0.f, m_selB = 0.f;
    bool  m_hasSel = false, m_selDrag = false;
    // Cached footer statistics.  sampleStats() walks the selected span sample
    // by sample; recomputing it EVERY FRAME while a selection existed measured
    // ~45 ms/frame with ten selected minutes.  Keyed on exactly what changes
    // the answer: the span and the frame count (edits change the latter).
    int64_t m_statFrom = -1, m_statTo = -1;
    int     m_statFrames = -1;
    bool    m_statOk = false;
    float   m_statPeak = 0.f, m_statRms = 0.f;
    bool  m_zeroSnap = true;           // snap edits to zero crossings
    float m_xfadeMs = 20.f;            // crossfade length for a seamless loop
    float m_vZoom   = 1.f;             // amplitude zoom (ctrl+wheel)
    bool  m_stereo  = false;           // dual-lane L/R display
    int   m_hotTool = -1;              // toolbar button under the pointer
    int   m_curX    = -1;              // pointer x, for the position readout
    int   m_curY    = -1;
    int   m_hotMarker = -1;            // marker under the pointer

    int   m_drag = -1;                 // marker index being dragged
    int   m_dragSlice = -1;            // slice index being dragged
    bool  m_dragXfade = false;         // dragging the loop-crossfade handle
    bool  m_dragXfadeFromIn = false;   // which of the two handles was grabbed
    bool  m_dragXfadeField = false;    // dragging the numeric xfade readout
    int   m_dragOriginX = 0;           // pointer x when a drag began (fine drag)
    float m_dragOriginV = 0.f;         // value when a drag began (Esc restores)
    float m_undoSelA = 0.f, m_undoSelB = 0.f;
    bool  m_undoHasSel = false;
    Uint32 m_lastClickMs = 0;          // double-click-to-add-slice
    int   m_lastClickX = 0, m_lastClickY = 0;
    bool  m_dragOver = false;          // a browser drag is hovering us
    bool  m_leftDown = false;          // MouseEv::pressed is state; derive a true edge
    bool  m_rightDown = false;
    bool  m_confirmClearSlices = false;// destructive action, armed on first pick
    std::string m_status;
    Uint32 m_statusMs = 0;             // status auto-expires; it is not a log
};

//----------------------------------------------------------------------------
//  SampleSlotPanel -- the mountable unit: disk browser on the left, editor on
//  the right, with the browser's drag routed into the editor.  A ui::Window
//  hosts exactly one widget, so this composite is what the shell mounts.
//----------------------------------------------------------------------------
class SampleSlotPanel : public ui::Widget {
public:
    SampleSlotPanel();

    void set_slot( PatchKnob::engine::ISampleSlot* slot,
                   const std::string& title = std::string() );
    void set_sample_rate( double sr ) { m_editor.set_sample_rate( sr ); }
    void set_dir( const std::string& d ) { m_browser.set_dir( d ); }

    SampleBrowser&    browser() { return m_browser; }
    SampleSlotEditor& editor()  { return m_editor; }

    //! Status text (load errors etc.) for the shell's status line.
    std::function<void(const std::string&)> on_status;
    //! Audition a browser file without loading it (the shell plays it).
    std::function<void(const std::string& path)> on_preview;
    //! Fired after a successful load (the shell remembers the folder).
    std::function<void(const std::string& path)> on_loaded;
    //! An Sf2Preset drag landed somewhere -- this panel has nowhere to put a
    //! whole preset (its editor is one ISampleSlot, not a multi-zone
    //! sampler), so it hands the payload straight up.  The shell decides
    //! whether (x,y) actually landed on something that can import it.
    std::function<void(const DragPayload& payload, int x, int y)> on_preset_drop;

    void draw( ui::App& app ) override;
    bool on_mouse( ui::App& app, const ui::MouseEv& e ) override;
    bool on_wheel( ui::App& app, int dx, int dy ) override;
    bool on_key( ui::App& app, SDL_Keycode k ) override;
    //! Forwarded to the hosted editor: the composite is what the shell mounts,
    //! so the toolkit's undo route stops here otherwise.
    bool on_undo( ui::App& app, bool redo ) override;
    void cancel_interaction( ui::App& app ) override;

private:
    void layout();
    SampleBrowser    m_browser;
    SampleSlotEditor m_editor;
    int  m_split = 240;          // browser width in px
    bool m_ldown = false;        // button state, for the press edge
    bool m_browserFocused = true;// which pane receives keys
    //! The divider is draggable.  A fixed 240 px browser was either wasting
    //! space or cutting every filename in half, depending on the folder.
    bool m_splitDrag = false;
    int  m_splitGrabDx = 0;
    bool m_splitHot = false;
    SDL_Rect m_splitRect{0,0,0,0};
};

} // namespace sampleslot

#endif // PATCHKNOB_SDLUI_SAMPLE_SLOT_EDITOR_H
