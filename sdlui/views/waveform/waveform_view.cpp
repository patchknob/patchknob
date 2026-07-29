//----------------------------------------------------------------------------
//  sdlui/views/waveform/waveform_view.cpp -- see waveform_view.h.
//----------------------------------------------------------------------------
#include "waveform_view.h"

#include "engine/audioclip/audio_clip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace ui;
using PatchKnob::engine::AudioClip;

namespace waveform {

// ---------------------------------------------------------------------------
// tuning constants
// ---------------------------------------------------------------------------
static constexpr int64_t kBinSamples = 256;   // samples summarised per cache bin
static constexpr double  kMinSpp     = 1.0/16.0;  // most zoomed-in: 16 px/sample
static constexpr double  kZoomStep   = 1.35;      // per button / wheel notch
static constexpr int     kToolbarH   = 24;        // toolbar strip height (logical)
static constexpr int     kScrollH    = 12;        // bottom scrollbar height

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int64_t clampi64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static std::string clip_text(const std::string& s, int cells) {
    if (cells <= 0) return std::string();
    if ((int)s.size() <= cells) return s;
    return s.substr(0, (size_t)cells);
}

// ===========================================================================
// construction
// ===========================================================================
WaveformView::WaveformView() {
    zin_.reset(new Button());  zin_->text  = "+";
    zout_.reset(new Button()); zout_->text = "-";
    zfit_.reset(new Button()); zfit_->text = "Fit";
    zin_->clicked  = [this]{ zoom_in();  };
    zout_->clicked = [this]{ zoom_out(); };
    zfit_->clicked = [this]{ zoom_fit(); };
    children_ = { zin_.get(), zout_.get(), zfit_.get() };
}

WaveformView::WaveformView(const AudioClip* clip) : WaveformView() {
    set_clip(clip);
}

// ===========================================================================
// clip binding + peak cache
// ===========================================================================
void WaveformView::set_clip(const AudioClip* clip) {
    clip_ = clip;
    numFrames_  = clip ? clip->numFrames() : 0;
    sampleRate_ = (clip && clip->sampleRate > 0.0) ? clip->sampleRate : 48000.0;
    scroll_   = 0;
    playhead_ = -1;
    build_peaks();
    zoom_fit();
}

void WaveformView::build_peaks() {
    bins_.clear();
    if (!clip_ || numFrames_ <= 0) return;

    const int64_t nbins = (numFrames_ + kBinSamples - 1) / kBinSamples;
    bins_.resize((size_t)nbins);
    const float* L = clip_->ch[0].data();
    const float* R = clip_->ch[1].empty() ? L : clip_->ch[1].data();

    for (int64_t b = 0; b < nbins; ++b) {
        const int64_t s0 = b * kBinSamples;
        const int64_t s1 = std::min<int64_t>(s0 + kBinSamples, numFrames_);
        float mn = 1.0f, mx = -1.0f;
        for (int64_t i = s0; i < s1; ++i) {
            const float v = 0.5f * (L[(size_t)i] + R[(size_t)i]);   // mono mix
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (mx < mn) { mn = 0.0f; mx = 0.0f; }
        bins_[(size_t)b] = { mn, mx };
    }
}

// Compute the mono min/max over [s0, s1). Uses the bin summary for wide spans
// and reads raw samples when zoomed in past one bin, so detail is exact.
void WaveformView::column_peak(int64_t s0, int64_t s1, float& mn, float& mx) const {
    mn = 0.0f; mx = 0.0f;
    if (!clip_ || numFrames_ <= 0) return;
    s0 = clampi64(s0, 0, numFrames_);
    s1 = clampi64(s1, 0, numFrames_);
    if (s1 <= s0) return;

    if ((s1 - s0) >= kBinSamples && !bins_.empty()) {
        int64_t b0 = s0 / kBinSamples;
        int64_t b1 = (s1 + kBinSamples - 1) / kBinSamples;
        b1 = std::min<int64_t>(b1, (int64_t)bins_.size());
        float lo = 1.0f, hi = -1.0f;
        for (int64_t b = b0; b < b1; ++b) {
            lo = std::min(lo, bins_[(size_t)b].mn);
            hi = std::max(hi, bins_[(size_t)b].mx);
        }
        if (hi < lo) { lo = 0.0f; hi = 0.0f; }
        mn = lo; mx = hi;
    } else {
        const float* L = clip_->ch[0].data();
        const float* R = clip_->ch[1].empty() ? L : clip_->ch[1].data();
        float lo = 1.0f, hi = -1.0f;
        for (int64_t i = s0; i < s1; ++i) {
            const float v = 0.5f * (L[(size_t)i] + R[(size_t)i]);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (hi < lo) { lo = 0.0f; hi = 0.0f; }
        mn = lo; mx = hi;
    }
}

// ===========================================================================
// zoom / scroll
// ===========================================================================
void WaveformView::set_samples_per_pixel(double spp) {
    double maxSpp = std::max(kMinSpp,
                             (double)std::max<int64_t>(1, numFrames_) /
                             (double)std::max(1, lastLaneW_));
    maxSpp = std::max(maxSpp, kMinSpp);
    spp_ = std::min(std::max(spp, kMinSpp), maxSpp);
    clamp_scroll();
}

void WaveformView::zoom_about(int anchorPx, double factor) {
    SDL_Rect L = lane_rect();
    int rel = anchorPx - L.x;
    if (rel < 0) rel = 0;
    if (rel > L.w) rel = L.w;
    int64_t anchorSample = scroll_ + (int64_t)llround(rel * spp_);
    set_samples_per_pixel(spp_ * factor);
    scroll_ = anchorSample - (int64_t)llround(rel * spp_);
    clamp_scroll();
}

void WaveformView::zoom_in()  { zoom_about(lane_rect().x + lane_rect().w / 2, 1.0 / kZoomStep); }
void WaveformView::zoom_out() { zoom_about(lane_rect().x + lane_rect().w / 2, kZoomStep); }

void WaveformView::zoom_fit() {
    int w = std::max(1, lastLaneW_);
    set_samples_per_pixel((double)std::max<int64_t>(1, numFrames_) / (double)w);
    scroll_ = 0;
    clamp_scroll();
}

void WaveformView::set_scroll_sample(int64_t s) { scroll_ = s; clamp_scroll(); }

void WaveformView::clamp_scroll() {
    int64_t visible = (int64_t)llround(spp_ * std::max(1, lastLaneW_));
    int64_t maxScroll = std::max<int64_t>(0, numFrames_ - visible);
    scroll_ = clampi64(scroll_, 0, maxScroll);
}

// ===========================================================================
// geometry
// ===========================================================================
SDL_Rect WaveformView::toolbar_rect() const {
    return SDL_Rect{ rect.x, rect.y, rect.w, kToolbarH };
}
SDL_Rect WaveformView::lane_rect() const {
    int y = rect.y + kToolbarH;
    int h = rect.h - kToolbarH - kScrollH;
    if (h < 8) h = 8;
    return SDL_Rect{ rect.x + 2, y, std::max(1, rect.w - 4), h };
}
SDL_Rect WaveformView::scrollbar_rect() const {
    return SDL_Rect{ rect.x + 2, rect.y + rect.h - kScrollH,
                     std::max(1, rect.w - 4), kScrollH - 2 };
}

void WaveformView::layout_buttons(App& app) {
    (void)app;
    int bx = rect.x + 4;
    int by = rect.y + 2;
    int bh = kToolbarH - 4;
    int bw = 26;
    zout_->rect = { bx,               by, bw, bh };
    zin_->rect  = { bx + bw + 3,      by, bw, bh };
    zfit_->rect = { bx + 2*(bw + 3),  by, 40, bh };
}

// ===========================================================================
// draw
// ===========================================================================
void WaveformView::draw(App& app) {
    if (!visible) return;
    const Theme& t = theme();

    SDL_Rect lane = lane_rect();
    if (lane.w != lastLaneW_) {
        int prevW = lastLaneW_;
        lastLaneW_ = lane.w;
        // Keep the whole clip visible if we were fit-to-width when resized.
        if (prevW <= 1) zoom_fit(); else clamp_scroll();
    }
    layout_buttons(app);

    // background
    fill_rect(app.ren, rect, t.bg);

    // ---- toolbar -----------------------------------------------------------
    SDL_Rect tb = toolbar_rect();
    fill_rect(app.ren, tb, t.panel);
    hline(app.ren, tb.x, tb.x + tb.w, tb.y + tb.h - 1, t.dim);
    zout_->draw(app); zin_->draw(app); zfit_->draw(app);

    // toolbar readout: name / duration / zoom
    {
        int infoX = zfit_->rect.x + zfit_->rect.w + 12;
        int infoY = tb.y + (tb.h - app.mono.ch()) / 2;
        int cells = (tb.x + tb.w - 6 - infoX) / (app.mono.cw() > 0 ? app.mono.cw() : 8);
        char buf[128];
        if (clip_ && numFrames_ > 0) {
            double dur = (double)numFrames_ / sampleRate_;
            double pxPerSec = sampleRate_ / (spp_ > 0 ? spp_ : 1.0);
            std::snprintf(buf, sizeof(buf), "%s  %.2fs  %.0fpx/s",
                          clip_->name.c_str(), dur, pxPerSec);
        } else {
            std::snprintf(buf, sizeof(buf), "<no clip>");
        }
        app.mono.draw(app.ren, infoX, infoY, clip_text(buf, cells), t.text);
    }

    // ---- lane --------------------------------------------------------------
    fill_rect(app.ren, lane, t.keybg);
    frame_rect(app.ren, lane, t.dim);

    const int cy = lane.y + lane.h / 2;
    const int h2 = (lane.h / 2) - 2;
    hline(app.ren, lane.x + 1, lane.x + lane.w - 2, cy, t.dim);   // zero line

    if (clip_ && numFrames_ > 0 && h2 > 0) {
        set_color(app.ren, t.accent);
        for (int px = 0; px < lane.w - 2; ++px) {
            int64_t s0 = scroll_ + (int64_t)llround((double)px       * spp_);
            int64_t s1 = scroll_ + (int64_t)llround((double)(px + 1) * spp_);
            if (s1 <= s0) s1 = s0 + 1;
            if (s0 >= numFrames_) break;                 // past clip end
            float mn, mx;
            column_peak(s0, s1, mn, mx);
            int yTop = cy - (int)llround(clampf(mx, -1.f, 1.f) * h2);
            int yBot = cy - (int)llround(clampf(mn, -1.f, 1.f) * h2);
            if (yTop == yBot) { if (yTop > lane.y) --yTop; else ++yBot; }
            SDL_RenderDrawLine(app.ren, lane.x + 1 + px, yTop,
                                        lane.x + 1 + px, yBot);
        }
        // bright cap on the zero line where audio exists, for a little life
    } else if (h2 > 0) {
        app.mono.draw_centered(app.ren, lane, "no audio clip", t.dim);
    }

    // ---- playhead ----------------------------------------------------------
    if (playhead_ >= 0 && clip_ && numFrames_ > 0) {
        double relPx = (double)(playhead_ - scroll_) / (spp_ > 0 ? spp_ : 1.0);
        if (relPx >= 0 && relPx < lane.w - 1) {
            int x = lane.x + 1 + (int)llround(relPx);
            vline(app.ren, x, lane.y + 1, lane.y + lane.h - 2, t.active);
        }
    }

    // ---- scrollbar ---------------------------------------------------------
    SDL_Rect sb = scrollbar_rect();
    fill_rect(app.ren, sb, t.panel);
    frame_rect(app.ren, sb, t.dim);
    if (numFrames_ > 0) {
        int64_t visible = (int64_t)llround(spp_ * std::max(1, lastLaneW_));
        visible = clampi64(visible, 1, numFrames_);
        double f0 = (double)scroll_ / (double)numFrames_;
        double f1 = (double)(scroll_ + visible) / (double)numFrames_;
        int tx = sb.x + 1 + (int)llround(f0 * (sb.w - 2));
        int tw = std::max(6, (int)llround((f1 - f0) * (sb.w - 2)));
        if (tx + tw > sb.x + sb.w - 1) tw = sb.x + sb.w - 1 - tx;
        SDL_Rect thumb{ tx, sb.y + 1, tw, sb.h - 2 };
        fill_rect(app.ren, thumb, t.accent);
    }
}

// ===========================================================================
// input
// ===========================================================================
bool WaveformView::on_mouse(App& app, const MouseEv& e) {
    if (e.pressed) {
        // an ongoing drag takes priority (motion arrives as pressed=true)
        if (capture_) return capture_->on_mouse(app, e);
        if (scrollDrag_) {
            SDL_Rect sb = scrollbar_rect();
            int64_t visible = (int64_t)llround(spp_ * std::max(1, lastLaneW_));
            double f = (double)(e.x - sb.x) / (double)std::max(1, sb.w);
            scroll_ = (int64_t)llround(f * numFrames_) - visible / 2;
            clamp_scroll();
            app.request_redraw();
            return true;
        }
        if (panDrag_) {
            scroll_ = panAnchorScroll_ - (int64_t)llround((e.x - panAnchorX_) * spp_);
            clamp_scroll();
            app.request_redraw();
            return true;
        }
        // fresh press: toolbar buttons first
        for (auto* c : children_)
            if (c->visible && c->hit(e.x, e.y)) { capture_ = c; return c->on_mouse(app, e); }
        // scrollbar?
        SDL_Rect sb = scrollbar_rect();
        if (e.x >= sb.x && e.x < sb.x + sb.w && e.y >= sb.y && e.y < sb.y + sb.h) {
            scrollDrag_ = true;
            int64_t visible = (int64_t)llround(spp_ * std::max(1, lastLaneW_));
            double f = (double)(e.x - sb.x) / (double)std::max(1, sb.w);
            scroll_ = (int64_t)llround(f * numFrames_) - visible / 2;
            clamp_scroll();
            app.request_redraw();
            return true;
        }
        // lane grab-scroll
        SDL_Rect lane = lane_rect();
        if (e.x >= lane.x && e.x < lane.x + lane.w && e.y >= lane.y && e.y < lane.y + lane.h) {
            panDrag_ = true;
            panAnchorX_ = e.x;
            panAnchorScroll_ = scroll_;
            return true;
        }
        return true;   // swallow clicks inside the view
    }

    // release
    bool handled = false;
    if (capture_) { handled = capture_->on_mouse(app, e); capture_ = nullptr; }
    panDrag_ = false;
    scrollDrag_ = false;
    return handled || true;
}

bool WaveformView::on_wheel(App& app, int dx, int dy) {
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    if (!hit(mx, my)) return false;

    SDL_Keymod mod = SDL_GetModState();
    const bool shift = (mod & KMOD_SHIFT) != 0;

    if (dx != 0 || shift) {
        // horizontal scroll: prefer real horizontal wheel, else Shift+vertical
        int amt = dx != 0 ? dx : dy;
        scroll_ -= (int64_t)llround(amt * spp_ * 40.0);
        clamp_scroll();
        app.request_redraw();
        return true;
    }
    if (dy != 0) {
        // zoom anchored under the cursor
        zoom_about(mx, dy > 0 ? (1.0 / kZoomStep) : kZoomStep);
        app.request_redraw();
        return true;
    }
    return false;
}

} // namespace waveform
