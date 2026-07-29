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
//  min/max), the bright clip colour is used for markers/handles.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_SAMPLE_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_SAMPLE_EDITOR_VIEW_H

#include "gui.h"
#include "engine/audioclip/warp_stretch.h"

#include <functional>
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

    // Warp markers: (srcSample -> dstSample) on the TIMELINE.  Always >= 2,
    // sorted.  The default {0->0, N->N} means "no warp".
    std::vector<PatchKnob::engine::WarpMarker> m_markers;
    // Transient (onset) positions in SOURCE samples -- detected on bind; shown as
    // ticks; used for auto-markers + snapping a new marker's source to a hit.
    std::vector<int64_t> m_transients;
    bool   m_show_transients = true;
    bool   m_down = false;                     // a press is in progress (drag)
    int    m_drag = -1;                       // marker index being dragged (-1 none)
    bool   m_drag_src = false;                // Shift-drag: slide source under marker

    // layout
    int    ruler_h = 16;
    int    toolbar_h = 18;
    int    scrollbar_h = 12;                   // bottom horizontal scrollbar
    // Scrollbar drag state.
    bool   m_sb_drag = false;
    int    m_sb_ref_x = 0;
    double m_sb_ref_scroll = 0.0;
    double content_len() const;                // total warped length in dst samples
    SDL_Rect scrollbar_track() const;
    SDL_Rect scrollbar_thumb() const;
    SDL_Rect m_btn_apply { 0,0,0,0 };
    SDL_Rect m_btn_reset { 0,0,0,0 };
    SDL_Rect m_btn_trUp  { 0,0,0,0 };
    SDL_Rect m_btn_trDn  { 0,0,0,0 };
    SDL_Rect m_btn_trans { 0,0,0,0 };   // "mark transients" button
    SDL_Rect m_btn_mode  { 0,0,0,0 };   // warp-mode cycle button
    int    m_mx = -1, m_my = -1;

    int  wave_y() const { return rect.y + toolbar_h + ruler_h; }
    int  wave_h() const { return rect.h - toolbar_h - ruler_h - scrollbar_h; }
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
    void   draw_wave(ui::App& app, int bx, int y, int bw, int h);
    int    marker_at(int x, int y) const;     // index of a marker handle near (x,y)
};

} // namespace samped

#endif // PATCHKNOB_SDLUI_SAMPLE_EDITOR_VIEW_H
