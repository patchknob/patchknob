//----------------------------------------------------------------------------
//  sdlui/views/sample_editor/sample_editor_view.h
//
//  Ableton-style SAMPLE EDITOR / warp view.  Docked (tabbed with the patchbay)
//  at the bottom.  Shows the selected audio clip's waveform in SOURCE-sample
//  time, a beat/bar grid mapped through the warp map, and draggable WARP MARKERS
//  that pin a source position to a timeline (beat) position.  Dragging a marker
//  restretches the audio between it and its neighbours; "Apply" renders the warp
//  (via the vendored signalsmith-stretch) and replaces the region's audio.
//
//  Two-tone toolkit; the waveform is drawn like the arrange lane (bipolar
//  min/max) with an RMS body inside the peak envelope, the bright clip colour is
//  used for markers/handles.
//
//  GEOMETRY RULE.  Everything the user can grab (marker flags, the scrollbar
//  thumb, toolbar buttons, the selection edges) is produced by ONE function that
//  draw() and on_mouse() both call.  The two used to compute their own rects
//  from the same magic numbers, which is how markers ended up drawn on the ruler
//  but grabbable only in a band that did not include their own flag.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SAMPLE_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_SAMPLE_EDITOR_VIEW_H

#include "gui.h"
#include "engine/audioclip/warp_stretch.h"

#include <functional>
#include <string>
#include <vector>

namespace PatchKnob { namespace engine { struct AudioClip; } }

namespace samped {

class SampleEditorView : public ui::Widget {
public:
    // Bind the clip to edit.  `seq` is the arrange region it came from; sr is the
    // engine sample rate; bpm/ppqn set the beat grid.  Pass nullptr to clear.
    void set_clip(int seq, const PatchKnob::engine::AudioClip* clip,
                  double sampleRate, double bpm, int ppqn);

    //! Which arrange seq the editor is currently bound to (-1 if none).
    int  bound_seq() const { return m_seq; }
    //! If the editor is showing `seq`, unbind it -- its AudioClip is about to be
    //! freed (unfreeze / delete), so drawing it afterwards would be a UAF.
    void forget_seq(int seq) { if (m_seq == seq) set_clip(-1, nullptr, m_sr, m_bpm, m_ppqn); }

    // Apply the current warp map: the shell renders warp_render(clip, markers,
    // transpose, mode, formant) and swaps the region's audio + updates its length.
    std::function<void(int seq, const std::vector<PatchKnob::engine::WarpMarker>& markers,
                       double transposeSemitones, PatchKnob::engine::WarpMode mode,
                       double formantSemitones)> on_apply_warp;

    // Fired whenever the warp map changes (drag / add / delete), so the shell can
    // push it to the engine for a REALTIME preview -- you HEAR the stretch as you
    // edit.  An identity map arrives empty (clears the live warp).
    std::function<void(int seq, const std::vector<PatchKnob::engine::WarpMarker>& markers)> on_warp_live;

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;
    //! Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, from the toolkit's undo route.  Consumes
    //! it for the LOCAL warp-marker history this view advertises in its own
    //! context menu; returns false (-> project-wide undo) when that is empty.
    bool on_undo(ui::App& app, bool redo) override;
    //! Esc / focus loss must abandon a drag rather than leave the view convinced
    //! a button is still held (a released-outside button never comes back).
    void cancel_interaction(ui::App& app) override;

private:
    const PatchKnob::engine::AudioClip* m_clip = nullptr;
    int    m_seq = -1;
    double m_sr  = 48000.0;
    double m_bpm = 120.0;
    int    m_ppqn = 192;
    double m_transpose = 0.0;                 // semitones (Ableton Transpose)
    PatchKnob::engine::WarpMode m_mode = PatchKnob::engine::WarpMode::Complex;

    // View: the x-axis is TIMELINE (beat/dst) time -- the grid is even and the
    // waveform is drawn WARPED (each dst pixel samples its source via the map),
    // Ableton-style.  Zoom = dst samples per pixel; scroll = leftmost dst sample.
    double m_spp = 512.0;
    double m_scroll = 0.0;
    //! set_clip() cannot fit the clip to the width before the shell has laid the
    //! view out (rect.w is still 0), so the fit is deferred to the first draw
    //! that has a real width instead of silently keeping the previous zoom.
    bool   m_fit_pending = false;

    // Warp markers: (srcSample -> dstSample) on the TIMELINE.  Always >= 2,
    // sorted.  The default {0->0, N->N} means "no warp".
    std::vector<PatchKnob::engine::WarpMarker> m_markers;
    int    m_sel_marker = -1;                  // selected (keyboard-nudgeable)
    // Undo history for the marker map only -- the audio is never touched until
    // APPLY, so a few dozen marker snapshots cost nothing.
    std::vector<std::vector<PatchKnob::engine::WarpMarker>> m_undo, m_redo;
    void   push_undo();
    //! push_undo(), but for a snapshot taken BEFORE the edit was applied.
    void   push_undo_state(std::vector<PatchKnob::engine::WarpMarker> state);
    //! One step along the local marker-map history; false when it is empty.
    bool   apply_history(bool redo);
    //! Insert a marker keeping BOTH src and dst strictly increasing (clamped
    //! into the neighbours' interior).  Returns the index, or -1 when the gap
    //! is too small.  Does NOT snapshot -- the caller owns the undo entry.
    int    insert_marker_ordered(int64_t src, int64_t dst);
    // Transient (onset) positions in SOURCE samples -- detected on bind; shown as
    // ticks; used for auto-markers + snapping a new marker's source to a hit.
    std::vector<int64_t> m_transients;
    bool   m_show_transients = true;
    bool   m_down = false;                     // a press is in progress (drag)
    int    m_drag = -1;                       // marker index being dragged (-1 none)
    bool   m_drag_src = false;                // Shift-drag: slide source under marker
    Uint32 m_last_click_ms = 0;               // double-click -> add a marker
    int    m_last_click_x = -9999;

    // ---- selection (dst samples) -------------------------------------------
    //  A range you can hear, zoom to and read out in samples / seconds / bars.
    bool   m_has_sel = false, m_sel_drag = false;
    double m_selA = 0.0, m_selB = 0.0;
    double sel_lo() const { return m_selA < m_selB ? m_selA : m_selB; }
    double sel_hi() const { return m_selA < m_selB ? m_selB : m_selA; }

    // ---- audition ----------------------------------------------------------
    //  The engine's one-shot preview has no position query, so the play cursor
    //  is driven from the wall clock started at the moment the audition was
    //  handed over.  It is only ever a few ms out and it makes playback visible.
    double m_cursor = 0.0;                    // edit cursor / audition origin (dst)
    bool   m_playing = false;
    Uint32 m_play_ms0 = 0;
    double m_play_from = 0.0;
    int    m_last_play_x = -1;                // pixel the cursor was last drawn at
    double play_cursor_dst() const;           // < 0 when nothing is auditioning
    void   audition(ui::App& app, double fromDst);
    void   stop_audition();

    // ---- cached peak table --------------------------------------------------
    //  Reducing every visible sample once per FRAME is what made a long clip
    //  crawl: at 512 samples/px a 5-minute file re-scanned 14 million floats for
    //  every hover repaint.  The mip is built once per bind and the columns read
    //  from it whenever a pixel spans more than one bucket.
    struct PeakMip {
        const PatchKnob::engine::AudioClip* clip = nullptr;
        int64_t frames = 0;
        int     bucket = 256;
        std::vector<float> mn, mx, rms;
    };
    PeakMip m_peaks;
    void rebuild_peaks();
    void column_peaks(int64_t s0, int64_t s1, float& mn, float& mx, float& rms) const;
    // scratch for the batched column fills (kept to avoid a per-frame malloc)
    std::vector<SDL_Rect> m_col_peak, m_col_peak_dim, m_col_rms, m_col_rms_dim, m_col_clip;

    // ---- layout (recomputed each draw from the FONT, never fixed pixels) ----
    int    toolbar_h = 18;
    int    ruler_h = 16;
    int    info_h = 14;
    int    scrollbar_h = 12;
    int    m_grab = 6;                        // marker grab radius (px)
    int    m_flag_w = 9, m_flag_h = 9;        // marker handle size
    void   layout_metrics(ui::App& app);
    // Scrollbar drag state.
    bool   m_sb_drag = false;
    int    m_sb_ref_x = 0;
    double m_sb_ref_scroll = 0.0;
    double content_len() const;                // total warped length in dst samples
    double visible_dst() const;                // dst samples across the wave area
    void   clamp_scroll();
    //! One zoom entry point for the wheel, the keys and the buttons, so the three
    //! can no longer disagree about the limits or about what stays put.
    void   set_zoom(double spp, int anchorX);
    void   zoom_to(double a, double b);
    SDL_Rect scrollbar_track() const;
    SDL_Rect scrollbar_thumb() const;
    SDL_Rect wave_rect() const;
    SDL_Rect ruler_rect() const;
    SDL_Rect info_rect() const;
    SDL_Rect m_btn_apply { 0,0,0,0 };
    SDL_Rect m_btn_reset { 0,0,0,0 };
    SDL_Rect m_btn_trUp  { 0,0,0,0 };
    SDL_Rect m_btn_trDn  { 0,0,0,0 };
    SDL_Rect m_btn_trans { 0,0,0,0 };   // "mark transients" button
    SDL_Rect m_btn_mode  { 0,0,0,0 };   // warp-mode cycle button
    SDL_Rect m_btn_fit   { 0,0,0,0 };   // zoom-to-fit
    SDL_Rect m_btn_play  { 0,0,0,0 };   // audition from the cursor
    int    m_mx = -1, m_my = -1;
    bool   m_hover_in = false;          // pointer is inside our rect
    int    m_hover_marker = -1;
    const char* m_tip = nullptr;        // tooltip to paint on top this frame
    SDL_Rect m_tip_anchor{0,0,0,0};

    // ---- right-click menu ---------------------------------------------------
    //  House style: one-shot ACTIONS belong here, not on the toolbar.  Only the
    //  controls whose STATE has to be readable at a glance stay as buttons.
    struct MenuRow { std::string label; int id; bool enabled; bool separator; };
    std::vector<MenuRow> m_menu;
    std::vector<SDL_Rect> m_menu_rows;
    bool   m_menu_open = false;
    SDL_Rect m_menu_box{0,0,0,0};
    double m_menu_dst = 0.0;
    int    m_menu_marker = -1;
    void   open_menu(ui::App& app, int x, int y);
    void   draw_menu(ui::App& app);
    bool   menu_click(ui::App& app, int x, int y);
    void   run_menu(ui::App& app, int id);

    int  wave_y() const { return rect.y + toolbar_h + ruler_h; }
    int  wave_h() const { return rect.h - toolbar_h - ruler_h - info_h - scrollbar_h; }
    int    dst_to_x(double d) const;          // timeline sample -> screen x
    double x_to_dst(int x) const;             // screen x -> timeline sample
    // Warp map: a source sample -> its warped TIMELINE (dst) sample, and inverse.
    double src_to_dst(double s) const;
    double dst_to_src(double d) const;
    double beat_samples() const { return 60.0 / m_bpm * m_sr; }   // one beat in dst samples
    // SMART SNAP: a marker can sit anywhere between grid lines, but it magnetises
    // to the nearest grid line when the pointer gets within a few screen pixels
    // of it (so you only snap when ~99% of the way there).
    double smart_snap_dst(double d) const;
    void   publish_warp();                    // push the live map (realtime preview)
    void   detect_transients();               // energy-onset detection (source time)
    int64_t nearest_transient_src(int64_t s, int64_t tolSamples) const;
    void   ensure_default_markers();
    void   add_marker(double dstPos, bool snap);
    void   delete_marker(int index);
    void   draw_toolbar(ui::App& app);
    void   draw_ruler(ui::App& app);
    void   draw_wave(ui::App& app, const SDL_Rect& w);
    void   draw_markers(ui::App& app, const SDL_Rect& w);
    void   draw_info(ui::App& app);
    void   apply_cursor();                    // pointer shape for what is under it
    SDL_Rect marker_flag(size_t i) const;     // the DRAWN handle == the hit target
    int    marker_at(int x, int y) const;     // index of a marker handle near (x,y)
    std::string time_str(double dstSamples) const;
    std::string bars_str(double dstSamples) const;
};

} // namespace samped

#endif // PATCHKNOB_SDLUI_SAMPLE_EDITOR_VIEW_H
