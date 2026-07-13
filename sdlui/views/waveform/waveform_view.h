//----------------------------------------------------------------------------
//  sdlui/views/waveform/waveform_view.h -- SDL2 audio-clip waveform display.
//
//  WaveformView is a single retained ui::Widget that renders an AudioClip's
//  waveform as min/max peaks per pixel column (mono mix of L/R), themed two-tone
//  (Light / Midnight both work). It supports:
//
//      * ZOOM     : "+"/"-"/"Fit" toolbar buttons AND mouse-wheel zoom
//                   (zoom is anchored on the sample under the cursor).
//      * SCROLL   : horizontal grab-scroll by dragging the lane, a draggable
//                   bottom scrollbar, and horizontal wheel / Shift+wheel.
//      * PLAYHEAD : an optional transport playhead line (set_playhead()).
//
//  The clip is NON-OWNING (the shell / AudioTrack owns it and must outlive the
//  view or call set_clip(nullptr) first). A fixed-bin min/max summary is built
//  once per clip so drawing is O(pixels) at any zoom, never O(samples).
//
//  Mounting (shell side):
//      waveform::WaveformView wv(clip);      // clip = AudioTrack::addClip(...)
//      app.roots.push_back(&wv);
//      app.on_layout = [&](ui::App& a){ wv.rect = { x, y, w, h }; };
//      // draw()/wheel/mouse are self-contained; the view calls
//      // app.request_redraw() whenever the zoom/scroll changes.
//----------------------------------------------------------------------------
#ifndef SEQ24_SDLUI_VIEWS_WAVEFORM_VIEW_H
#define SEQ24_SDLUI_VIEWS_WAVEFORM_VIEW_H

#include "gui.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace seq24 { namespace engine { struct AudioClip; } }

namespace waveform {

class WaveformView : public ui::Widget {
public:
    WaveformView();
    explicit WaveformView(const seq24::engine::AudioClip* clip);

    // --- clip binding (message thread) ---------------------------------------

    //! Bind a (non-owning) clip to display; rebuilds the peak cache and fits it
    //! to the current width. Pass nullptr to clear.
    void set_clip(const seq24::engine::AudioClip* clip);
    const seq24::engine::AudioClip* clip() const { return clip_; }

    // --- zoom -----------------------------------------------------------------

    void   zoom_in();                        //!< zoom in around the view centre
    void   zoom_out();                       //!< zoom out around the view centre
    void   zoom_fit();                       //!< show the whole clip
    void   set_samples_per_pixel(double spp);
    double samples_per_pixel() const { return spp_; }

    // --- horizontal scroll ----------------------------------------------------

    void    set_scroll_sample(int64_t s);
    int64_t scroll_sample() const { return scroll_; }

    // --- transport ------------------------------------------------------------

    //! Absolute timeline sample of the playhead; < 0 hides it.
    void    set_playhead(int64_t sample) { playhead_ = sample; }
    int64_t playhead() const { return playhead_; }

    // --- ui::Widget -----------------------------------------------------------

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;

private:
    //! Fixed-window mono min/max summary cell.
    struct Bin { float mn = 0.0f, mx = 0.0f; };

    void     build_peaks();
    void     zoom_about(int anchorPx, double factor);
    void     clamp_scroll();
    void     column_peak(int64_t s0, int64_t s1, float& mn, float& mx) const;
    SDL_Rect toolbar_rect() const;
    SDL_Rect lane_rect() const;
    SDL_Rect scrollbar_rect() const;
    void     layout_buttons(ui::App& app);

    const seq24::engine::AudioClip* clip_ = nullptr;
    std::vector<Bin> bins_;                  //!< kBinSamples samples per bin
    int64_t          numFrames_ = 0;
    double           sampleRate_ = 48000.0;

    double  spp_    = 256.0;                  //!< samples per pixel (zoom level)
    int64_t scroll_ = 0;                      //!< leftmost visible sample
    int64_t playhead_ = -1;

    int lastLaneW_ = 1;                       //!< remembered from last draw()

    std::unique_ptr<ui::Button> zin_, zout_, zfit_;
    std::vector<ui::Widget*>    children_;    //!< non-owning dispatch list
    ui::Widget* capture_ = nullptr;

    bool    panDrag_    = false;
    bool    scrollDrag_ = false;
    int     panAnchorX_ = 0;
    int64_t panAnchorScroll_ = 0;
};

} // namespace waveform

#endif // SEQ24_SDLUI_VIEWS_WAVEFORM_VIEW_H
