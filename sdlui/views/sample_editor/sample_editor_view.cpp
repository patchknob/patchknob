//----------------------------------------------------------------------------
//  sdlui/views/sample_editor/sample_editor_view.cpp
//----------------------------------------------------------------------------
#include "sample_editor_view.h"

#include "engine/audioclip/audio_clip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using ui::App;
using ui::Theme;
using ui::Color;
using ui::MouseEv;
using ui::theme;
using PatchKnob::engine::AudioClip;
using PatchKnob::engine::WarpMarker;

namespace samped {

void SampleEditorView::set_clip(int seq, const AudioClip* clip,
                                double sampleRate, double bpm, int ppqn)
{
    m_seq = seq;
    m_clip = clip;
    m_sr  = sampleRate > 0 ? sampleRate : 48000.0;
    m_bpm = bpm > 1 ? bpm : 120.0;
    m_ppqn = ppqn > 0 ? ppqn : 192;
    m_transpose = 0.0;
    m_drag = -1;
    m_markers.clear();
    ensure_default_markers();
    detect_transients();
    publish_warp();                 // identity -> clears any prior live warp
    // Fit the clip horizontally on bind.
    if (m_clip && m_clip->numFrames() > 0 && rect.w > 8) {
        double dstLen = (double)m_markers.back().dstSample;
        m_spp = std::max(1.0, dstLen / std::max(1, rect.w - 4));
        m_scroll = 0.0;
    }
}

// Simple energy-onset transient detector (source time).  Short-time RMS
// envelope, then mark a rising edge that jumps above a fraction of the recent
// average -- the raw material for auto warp markers + snap-to-transient.
void SampleEditorView::detect_transients()
{
    m_transients.clear();
    if (!m_clip || m_clip->numFrames() <= 0) return;
    const int64_t n = m_clip->numFrames();
    const float* L = m_clip->ch[0].data();
    const float* R = m_clip->ch[1].empty() ? m_clip->ch[0].data() : m_clip->ch[1].data();
    const int hop = 256;
    std::vector<float> env;
    env.reserve((size_t)(n / hop + 1));
    for (int64_t i = 0; i < n; i += hop) {
        double e = 0; int64_t j = i, jn = std::min(n, i + hop);
        for (; j < jn; ++j) { float s = 0.5f * (L[j] + R[j]); e += (double)s * s; }
        env.push_back((float)std::sqrt(e / (double)hop));
    }
    // adaptive: onset where env jumps and exceeds a fraction of the running mean.
    double mean = 0; for (float v : env) mean += v; mean /= std::max<size_t>(1, env.size());
    const int64_t minGap = (int64_t)(0.04 * m_sr);   // >= 40 ms apart
    int64_t last = -minGap;
    for (size_t i = 2; i < env.size(); ++i) {
        const float prev = 0.5f * (env[i-1] + env[i-2]);
        if (env[i] > prev * 1.6f + 1e-4f && env[i] > (float)mean * 0.9f) {
            int64_t pos = (int64_t)i * hop;
            if (pos - last >= minGap) { m_transients.push_back(pos); last = pos; }
        }
    }
}

int64_t SampleEditorView::nearest_transient_src(int64_t s, int64_t tol) const
{
    int64_t best = -1, bestd = tol + 1;
    for (int64_t tS : m_transients) { int64_t d = std::llabs(tS - s); if (d <= tol && d < bestd) { best = tS; bestd = d; } }
    return best;
}

// Push the current warp map to the engine for realtime preview.  An identity
// map (2 markers, src==dst==clip length) is sent empty so the engine drops warp.
void SampleEditorView::publish_warp()
{
    if (!on_warp_live || m_seq < 0 || !m_clip) return;
    const int64_t n = m_clip->numFrames();
    const bool identity = (m_markers.size() == 2 &&
                           m_markers[0].srcSample == 0 && m_markers[0].dstSample == 0 &&
                           m_markers[1].srcSample == n && m_markers[1].dstSample == n);
    static const std::vector<PatchKnob::engine::WarpMarker> empty;
    on_warp_live(m_seq, identity ? empty : m_markers);
}

double SampleEditorView::smart_snap_dst(double d) const
{
    if (m_bpm <= 0) return d;
    const double beat = beat_samples();
    // Candidate grid lines: 1/16 (beat/4), the beat, and the bar -- snap to the
    // nearest of whichever is within the magnetic zone (finer grids win ties).
    const double grids[3] = { beat / 4.0, beat, beat * 4.0 };
    const double snapPx = 7.0;                 // magnetic radius in screen pixels
    double best = d; double bestDist = snapPx * m_spp + 1.0;
    for (double g : grids) {
        if (g <= 0) continue;
        double nearest = std::round(d / g) * g;
        double dist = std::fabs(d - nearest);
        if (dist <= snapPx * m_spp && dist < bestDist) { best = nearest; bestDist = dist; }
    }
    return best;                                // free placement if nothing was close
}

void SampleEditorView::ensure_default_markers()
{
    if (!m_clip) { m_markers = { {0,0}, {1,1} }; return; }
    const int64_t n = m_clip->numFrames();
    if (m_markers.size() < 2)
        m_markers = { { 0, 0 }, { n, n } };   // identity = no warp
}

int SampleEditorView::dst_to_x(double d) const
{
    return rect.x + 2 + (int)std::lround((d - m_scroll) / m_spp);
}
double SampleEditorView::x_to_dst(int x) const
{
    return m_scroll + (double)(x - rect.x - 2) * m_spp;
}

// Piecewise-linear warp map through the markers (extrapolating the end segments).
double SampleEditorView::src_to_dst(double s) const
{
    const size_t m = m_markers.size();
    if (m < 2) return s;
    auto interp = [](double v, double a0, double a1, double b0, double b1) {
        return (a1 > a0) ? b0 + (v - a0) * (b1 - b0) / (a1 - a0) : b0;
    };
    if (s <= (double)m_markers[1].srcSample)
        return interp(s, m_markers[0].srcSample, m_markers[1].srcSample,
                         m_markers[0].dstSample, m_markers[1].dstSample);
    for (size_t i = 1; i + 1 < m; ++i)
        if (s <= (double)m_markers[i + 1].srcSample)
            return interp(s, m_markers[i].srcSample, m_markers[i + 1].srcSample,
                             m_markers[i].dstSample, m_markers[i + 1].dstSample);
    return interp(s, m_markers[m - 2].srcSample, m_markers[m - 1].srcSample,
                     m_markers[m - 2].dstSample, m_markers[m - 1].dstSample);
}
double SampleEditorView::dst_to_src(double d) const
{
    const size_t m = m_markers.size();
    if (m < 2) return d;
    auto interp = [](double v, double a0, double a1, double b0, double b1) {
        return (a1 > a0) ? b0 + (v - a0) * (b1 - b0) / (a1 - a0) : b0;
    };
    if (d <= (double)m_markers[1].dstSample)
        return interp(d, m_markers[0].dstSample, m_markers[1].dstSample,
                         m_markers[0].srcSample, m_markers[1].srcSample);
    for (size_t i = 1; i + 1 < m; ++i)
        if (d <= (double)m_markers[i + 1].dstSample)
            return interp(d, m_markers[i].dstSample, m_markers[i + 1].dstSample,
                             m_markers[i].srcSample, m_markers[i + 1].srcSample);
    return interp(d, m_markers[m - 2].dstSample, m_markers[m - 1].dstSample,
                     m_markers[m - 2].srcSample, m_markers[m - 1].srcSample);
}

void SampleEditorView::draw_wave(App& app, int bx, int y, int bw, int h)
{
    if (!m_clip || m_clip->numFrames() <= 0 || bw < 2 || h < 4) return;
    const int64_t n = m_clip->numFrames();
    const float* L = m_clip->ch[0].data();
    const float* R = m_clip->ch[1].empty() ? m_clip->ch[0].data() : m_clip->ch[1].data();
    const int mid  = y + h / 2;
    const int amp  = h / 2 - 2;
    // Draw the wave in the theme FOREGROUND so it contrasts with the editor's
    // background in BOTH modes (was hardcoded black -> invisible on the midnight
    // black background).  The centre line uses the dim role.
    const Theme& t = theme();
    const Color black  = t.text;
    const Color center = t.dim;

    // SAMPLE-ACCURATE mode: when fewer than ~1 source sample maps to each pixel,
    // draw the actual samples as a connected polyline with dots (zoom to samples).
    const double srcPerPx = std::fabs(dst_to_src(x_to_dst(bx + 1)) - dst_to_src(x_to_dst(bx)));
    if (srcPerPx < 0.5) {
        int64_t sa = (int64_t)std::floor(dst_to_src(x_to_dst(bx))) - 1;
        int64_t sb = (int64_t)std::ceil (dst_to_src(x_to_dst(bx + bw))) + 1;
        if (sa < 0) sa = 0; if (sb > n) sb = n;
        set_color(app.ren, black);
        int prevX = -1, prevY = 0;
        for (int64_t si = sa; si < sb; ++si) {
            int px = dst_to_x(src_to_dst((double)si));
            float v = 0.5f * (L[si] + R[si]);
            int py = mid - (int)(v * amp);
            if (py < y) py = y; if (py > y + h - 1) py = y + h - 1;
            if (prevX >= 0) SDL_RenderDrawLine(app.ren, prevX, prevY, px, py);
            fill_rect(app.ren, SDL_Rect{ px - 1, py - 1, 3, 3 }, black);   // sample dot
            prevX = px; prevY = py;
        }
        hline(app.ren, bx, bx + bw - 1, mid, center);   // centre line
        return;
    }

    static std::vector<SDL_Rect> cols;
    cols.clear(); cols.reserve((size_t)bw);
    for (int px = 0; px < bw; ++px) {
        // dst pixel -> source sample window (warped)
        int64_t s0 = (int64_t)dst_to_src(x_to_dst(bx + px));
        int64_t s1 = (int64_t)dst_to_src(x_to_dst(bx + px + 1));
        if (s1 < s0) std::swap(s0, s1);
        if (s1 <= s0) s1 = s0 + 1;
        if (s0 < 0) s0 = 0; if (s1 > n) s1 = n;
        if (s0 >= n) continue;
        float mn = 1.f, mx = -1.f;
        for (int64_t i = s0; i < s1; ++i) {
            const float l = L[i], r = R[i];
            if (l < mn) mn = l; if (l > mx) mx = l;
            if (r < mn) mn = r; if (r > mx) mx = r;
        }
        int y0 = mid - (int)(mx * amp);
        int y1 = mid - (int)(mn * amp);
        if (y0 < y) y0 = y; if (y1 > y + h - 1) y1 = y + h - 1;
        if (y1 < y0) std::swap(y0, y1);
        cols.push_back(SDL_Rect{ bx + px, y0, 1, (y1 - y0) + 1 });
    }
    set_color(app.ren, black);
    if (!cols.empty()) SDL_RenderFillRects(app.ren, cols.data(), (int)cols.size());
}

// Total warped timeline length (dst samples) that the view scrolls over.
double SampleEditorView::content_len() const
{
    double len = m_markers.empty() ? 0.0 : (double)m_markers.back().dstSample;
    if (m_clip && len < (double)m_clip->numFrames()) len = (double)m_clip->numFrames();
    return len > 1.0 ? len : 1.0;
}

SDL_Rect SampleEditorView::scrollbar_track() const
{
    return SDL_Rect{ rect.x, rect.y + rect.h - scrollbar_h, rect.w, scrollbar_h };
}

SDL_Rect SampleEditorView::scrollbar_thumb() const
{
    const SDL_Rect tr = scrollbar_track();
    const double total = content_len();
    const double visible = (double)(rect.w - 4) * m_spp;     // dst samples on screen
    int tw = (int)std::lround((visible / total) * (double)(tr.w - 4));
    if (tw < 16) tw = 16;
    if (tw > tr.w - 4) tw = tr.w - 4;
    int tx = tr.x + 2 + (int)std::lround((m_scroll / total) * (double)(tr.w - 4));
    if (tx < tr.x + 2) tx = tr.x + 2;
    if (tx + tw > tr.x + tr.w - 2) tx = tr.x + tr.w - 2 - tw;
    return SDL_Rect{ tx, tr.y + 2, tw, tr.h - 4 };
}

void SampleEditorView::draw(App& app)
{
    const Theme& t = theme();
    fill_rect(app.ren, rect, t.bg);

    // --- toolbar ------------------------------------------------------------
    SDL_Rect tb{ rect.x, rect.y, rect.w, toolbar_h };
    fill_rect(app.ren, tb, t.panel);
    hline(app.ren, tb.x, tb.x + tb.w, tb.y + tb.h - 1, t.dim);
    int bx = rect.x + 4, by = rect.y + 2, bh = toolbar_h - 4;
    auto button = [&](SDL_Rect& r, const char* lbl, int w) {
        r = SDL_Rect{ bx, by, w, bh };
        bool hot = (m_mx >= r.x && m_mx < r.x + r.w && m_my >= r.y && m_my < r.y + r.h);
        fill_rect(app.ren, r, hot ? t.accent : t.bg);
        frame_rect(app.ren, r, t.dim);
        app.mono.draw_fitted(app.ren, SDL_Rect{ r.x + 3, r.y + 1, r.w - 6, r.h - 2 },
                             lbl, hot ? t.bg : t.text, true);
        bx += w + 4;
    };
    button(m_btn_apply, "APPLY WARP", 78);
    button(m_btn_reset, "RESET", 42);
    button(m_btn_trDn,  "-", 14);
    { char tl[24]; std::snprintf(tl, sizeof(tl), "PITCH %+.0f", m_transpose);
      app.mono.draw(app.ren, bx, by + (bh - app.mono.ch()) / 2, tl, t.dim);
      bx += (int)std::strlen(tl) * (app.mono.cw()?app.mono.cw():6) + 4; }
    button(m_btn_trUp,  "+", 14);
    button(m_btn_trans, "MARK TRANSIENTS", 118);
    { static const char* MN[] = { "Complex","Tones","Texture","Beats","ComplexPro","RePitch" };
      char ml[24]; std::snprintf(ml, sizeof(ml), "MODE:%s", MN[(int)m_mode]);
      button(m_btn_mode, ml, (int)std::strlen(ml) * (app.mono.cw()?app.mono.cw():6) + 10); }
    if (!m_clip) {
        app.mono.draw(app.ren, rect.x + 6, wave_y() + wave_h()/2,
                      "select an audio clip to warp", t.dim);
        return;
    }

    // --- beat / bar grid (timeline is even; drawn over the warped waveform) --
    SDL_Rect rl{ rect.x, rect.y + toolbar_h, rect.w, ruler_h };
    fill_rect(app.ren, rl, t.panel);
    hline(app.ren, rl.x, rl.x + rl.w, rl.y + rl.h - 1, t.dim);
    const double beat = beat_samples();
    const double dstEnd = (double)m_markers.back().dstSample;
    int beatIdx = (int)std::floor(m_scroll / beat);
    for (double d = beatIdx * beat; d <= x_to_dst(rect.x + rect.w); d += beat, ++beatIdx) {
        int x = dst_to_x(d);
        if (x < rect.x) continue;
        if (x > rect.x + rect.w) break;
        const bool bar = (beatIdx % 4) == 0;
        vline(app.ren, x, rl.y, wave_y() + wave_h(), bar ? t.accent : t.dim);
        if (bar) { char b[8]; std::snprintf(b, sizeof(b), "%d", beatIdx / 4 + 1);
                   app.mono.draw(app.ren, x + 2, rl.y + 1, b, t.text); }
    }

    // --- transient ticks (source onsets, mapped through the warp map) -------
    if (m_show_transients) {
        for (int64_t tS : m_transients) {
            int x = dst_to_x(src_to_dst((double)tS));
            if (x < rect.x || x > rect.x + rect.w) continue;
            vline(app.ren, x, wave_y(), wave_y() + 5, t.dim);
        }
    }

    // --- waveform (warped) --------------------------------------------------
    draw_wave(app, rect.x + 2, wave_y(), rect.w - 4, wave_h());

    // --- warp markers -------------------------------------------------------
    for (size_t i = 0; i < m_markers.size(); ++i) {
        int x = dst_to_x((double)m_markers[i].dstSample);
        if (x < rect.x - 2 || x > rect.x + rect.w + 2) continue;
        const bool end = (i == 0 || i + 1 == m_markers.size());
        Color mc = end ? t.dim : t.hi;
        vline(app.ren, x, wave_y(), wave_y() + wave_h(), mc);
        fill_rect(app.ren, SDL_Rect{ x - 3, rl.y + 1, 6, 6 }, mc);   // top handle
    }
    (void)dstEnd;

    // --- horizontal scrollbar (bottom) --------------------------------------
    {
        SDL_Rect tr = scrollbar_track();
        fill_rect(app.ren, tr, t.panel);
        hline(app.ren, tr.x, tr.x + tr.w, tr.y, t.dim);
        SDL_Rect th = scrollbar_thumb();
        bool hot = (m_mx >= th.x && m_mx < th.x + th.w && m_my >= th.y && m_my < th.y + th.h) || m_sb_drag;
        fill_rect(app.ren, th, hot ? t.accent : t.hi);
        frame_rect(app.ren, th, t.dim);
    }
}

int SampleEditorView::marker_at(int x, int y) const
{
    // Grabbable along the ruler strip (the handle row) -- a generous zone.
    if (y < rect.y + toolbar_h || y > rect.y + toolbar_h + ruler_h + 2) return -1;
    for (size_t i = 0; i < m_markers.size(); ++i) {
        int mx = dst_to_x((double)m_markers[i].dstSample);
        if (std::abs(x - mx) <= 6) return (int)i;
    }
    return -1;
}

bool SampleEditorView::on_mouse(App& app, const MouseEv& e)
{
    m_mx = e.x; m_my = e.y;
    if (!e.pressed) { m_down = false; m_drag = -1; m_sb_drag = false; return true; }   // release

    // motion during a scrollbar-thumb drag.
    if (m_down && m_sb_drag) {
        SDL_Rect tr = scrollbar_track();
        const double total = content_len();
        const double dx = (double)(e.x - m_sb_ref_x);
        double sc = m_sb_ref_scroll + dx / (double)std::max(1, tr.w - 4) * total;
        const double visible = (double)(rect.w - 4) * m_spp;
        if (sc < 0) sc = 0;
        if (sc > total - visible) sc = std::max(0.0, total - visible);
        m_scroll = sc;
        app.request_redraw();
        return true;
    }

    // motion during a marker drag: warp (move dst) or slide source (Shift).
    if (m_down) {
        if (m_drag >= 0 && m_drag < (int)m_markers.size()) {
            WarpMarker& mk = m_markers[m_drag];
            const int last = (int)m_markers.size() - 1;
            if (m_drag_src) {
                // Shift-drag: change which SOURCE sample sits at this marker.
                double s = dst_to_src(x_to_dst(e.x));
                int64_t lo = (m_drag > 0) ? m_markers[m_drag - 1].srcSample + 1 : 0;
                int64_t hi = (m_drag + 1 < (int)m_markers.size())
                             ? m_markers[m_drag + 1].srcSample - 1
                             : (m_clip ? m_clip->numFrames() : (int64_t)s);
                mk.srcSample = std::max(lo, std::min(hi, (int64_t)std::llround(s)));
            } else {
                // Warp: move the marker's TIMELINE position (SMART-snapped to the
                // grid only when close).  The LAST real marker isn't clamped to the
                // end marker -- the end follows it (below) so the un-warped tail
                // EXTENDS the clip instead of being compressed.
                double d = smart_snap_dst(x_to_dst(e.x));
                int64_t lo = (m_drag > 0) ? m_markers[m_drag - 1].dstSample + 1 : 0;
                int64_t hi = (m_drag < last - 1) ? m_markers[m_drag + 1].dstSample - 1
                                                 : (int64_t)d + 1;
                if (m_drag == 0) { mk.dstSample = std::max<int64_t>(0, (int64_t)std::llround(d)); }
                else mk.dstSample = std::max(lo, std::min(hi, (int64_t)std::llround(d)));
            }
            // EXTEND-not-stretch: keep the FINAL segment (last real marker -> the
            // source end) at NATURAL 1:1 rate.  So warping a bar of a 2-bar sample
            // out to 2 bars makes the clip 3 bars (the second bar rides along
            // un-warped) rather than squashing the tail into a fixed length.  Only
            // when dragging an INTERIOR marker -- dragging the end marker itself is
            // an explicit whole-clip length change.
            if (m_drag != last && last >= 1) {
                int64_t tail = m_markers[last].srcSample - m_markers[last - 1].srcSample;
                if (tail < 1) tail = 1;
                m_markers[last].dstSample = m_markers[last - 1].dstSample + tail;
            }
            publish_warp();          // realtime: hear the stretch as you drag
            app.request_redraw();
        }
        return true;
    }
    m_down = true;   // fresh press below

    // scrollbar: press the thumb to drag; press the track to page toward the click
    {
        SDL_Rect tr = scrollbar_track();
        if (e.y >= tr.y && e.y < tr.y + tr.h) {
            SDL_Rect th = scrollbar_thumb();
            if (e.x >= th.x && e.x < th.x + th.w) {
                m_sb_drag = true; m_sb_ref_x = e.x; m_sb_ref_scroll = m_scroll;
            } else {
                const double total = content_len();
                const double visible = (double)(rect.w - 4) * m_spp;
                m_scroll += (e.x < th.x ? -visible * 0.9 : visible * 0.9);   // page
                if (m_scroll < 0) m_scroll = 0;
                if (m_scroll > total - visible) m_scroll = std::max(0.0, total - visible);
            }
            app.request_redraw();
            return true;
        }
    }

    // toolbar buttons
    auto in = [&](const SDL_Rect& r){ return e.x >= r.x && e.x < r.x + r.w && e.y >= r.y && e.y < r.y + r.h; };
    if (in(m_btn_reset)) { m_markers.clear(); ensure_default_markers(); m_transpose = 0; publish_warp(); app.request_redraw(); return true; }
    if (in(m_btn_trUp))  { m_transpose = std::min(24.0, m_transpose + 1); app.request_redraw(); return true; }
    if (in(m_btn_trDn))  { m_transpose = std::max(-24.0, m_transpose - 1); app.request_redraw(); return true; }
    if (in(m_btn_mode))  { m_mode = (PatchKnob::engine::WarpMode)(((int)m_mode + 1) % 6); app.request_redraw(); return true; }
    if (in(m_btn_trans)) {
        // Insert a warp marker at every detected transient (pinned to its current
        // warped position); keeps the endpoints, dedups near existing markers.
        for (int64_t tS : m_transients) {
            bool dup = false;
            for (const auto& mk : m_markers) if (std::llabs(mk.srcSample - tS) < (int64_t)(0.02 * m_sr)) { dup = true; break; }
            if (dup) continue;
            WarpMarker nm{ tS, (int64_t)std::llround(src_to_dst((double)tS)) };
            auto it = std::lower_bound(m_markers.begin(), m_markers.end(), nm,
                [](const WarpMarker& a, const WarpMarker& b){ return a.srcSample < b.srcSample; });
            m_markers.insert(it, nm);
        }
        publish_warp();
        app.request_redraw();
        return true;
    }
    if (in(m_btn_apply)) {
        if (m_clip && on_apply_warp && m_markers.size() >= 2)
            on_apply_warp(m_seq, m_markers, m_transpose, m_mode, 0.0);
        return true;
    }
    if (!m_clip) return true;

    // grab an existing marker handle?
    int mi = marker_at(e.x, e.y);
    if (mi >= 0) {
        // right-click deletes an interior marker; else start dragging it.
        if (e.button == SDL_BUTTON_RIGHT) {
            if (mi != 0 && mi + 1 != (int)m_markers.size()) { m_markers.erase(m_markers.begin() + mi); publish_warp(); app.request_redraw(); }
            return true;
        }
        m_drag = mi;
        m_drag_src = (SDL_GetModState() & KMOD_SHIFT) != 0;
        return true;
    }

    // double-click / click on the wave adds a warp marker pinned to the source
    // at that timeline point (snapped to the beat grid on the dst axis).
    if (e.button == SDL_BUTTON_LEFT && e.y >= wave_y()) {
        double d = x_to_dst(e.x);
        int64_t s = (int64_t)std::llround(dst_to_src(d));
        // snap the SOURCE to a nearby transient (within ~8 px worth of samples)
        int64_t tS = nearest_transient_src(s, (int64_t)(m_spp * 8.0));
        if (tS >= 0) s = tS;
        double snapped = smart_snap_dst(d);         // smart snap dst (free unless near grid)
        WarpMarker nm{ s, (int64_t)std::llround(snapped) };
        // insert sorted by src
        auto it = std::lower_bound(m_markers.begin(), m_markers.end(), nm,
            [](const WarpMarker& a, const WarpMarker& b){ return a.srcSample < b.srcSample; });
        m_markers.insert(it, nm);
        publish_warp();
        app.request_redraw();
        return true;
    }
    return true;
}

bool SampleEditorView::on_wheel(App& app, int dx, int dy)
{
    (void)dx;
    if (dy != 0) {                      // zoom about the pointer
        double anchor = x_to_dst(m_mx >= 0 ? m_mx : rect.x + rect.w / 2);
        m_spp *= (dy > 0) ? 0.85 : 1.18;
        if (m_spp < 0.02) m_spp = 0.02;        // < 1 sample/px -> see individual samples
        if (m_spp > 65536.0) m_spp = 65536.0;
        m_scroll = anchor - (double)((m_mx >= 0 ? m_mx : rect.x + rect.w / 2) - rect.x - 2) * m_spp;
        if (m_scroll < 0) m_scroll = 0;
        app.request_redraw();
        return true;
    }
    return false;
}

bool SampleEditorView::on_key(App& app, SDL_Keycode k)
{
    if (k == SDLK_a && m_clip && on_apply_warp && m_markers.size() >= 2) {
        on_apply_warp(m_seq, m_markers, m_transpose, m_mode, 0.0); return true;
    }
    (void)app; return false;
}

} // namespace samped
