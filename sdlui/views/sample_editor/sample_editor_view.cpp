//----------------------------------------------------------------------------
//  sdlui/views/sample_editor/sample_editor_view.cpp -- see the header.
//----------------------------------------------------------------------------
#include "sample_editor_view.h"

#include "engine/audioclip/audio_clip.h"
#include "audio_app.h"                 // one-shot audition (space / click-to-play)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ui::App;
using ui::Theme;
using ui::Color;
using ui::MouseEv;
using ui::theme;
using PatchKnob::engine::AudioClip;
using PatchKnob::engine::WarpMarker;

namespace samped {

namespace {

// Measured truncation.  `s.size() * cw` is a BYTE count times a nominal advance:
// it disagrees with the real advance at any fractional UI scale and mis-sizes
// every box it feeds, so every width here goes through Font::text_w() instead.
// (Same helper as arrange_view.cpp's fit_text.)
std::string fit_text(const ui::Font& f, std::string s, int maxw)
{
    if (maxw <= 0) return std::string();
    if (f.text_w(s) <= maxw) return s;
    const std::string ell = "...";
    if (f.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && f.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!s.empty() && f.text_w(s + ell) > maxw) s.pop_back();
    return s.empty() ? ell : s + ell;
}

// This codebase draws OPAQUE by default; a leaked blend mode tints everything
// drawn after it, so translucency is always scoped.
struct BlendScope {
    SDL_Renderer* r;
    explicit BlendScope(SDL_Renderer* ren) : r(ren) {
        if (r) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    }
    ~BlendScope() { if (r) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE); }
    BlendScope(const BlendScope&) = delete;
    BlendScope& operator=(const BlendScope&) = delete;
};

inline Color fade(Color c, int a) { c.a = (Uint8)std::max(0, std::min(255, a)); return c; }
inline bool in_rect(const SDL_Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Smallest power-of-two multiple of `unitPx` that is at least `minPx` wide --
// the arrange ruler's thinning rule.  A division either has room for itself (and
// its label) or is not drawn at all, so nothing ever overprints.
long pow2_stride(double unitPx, double minPx)
{
    if (unitPx <= 0.0) return 1L << 28;
    long n = 1;
    while ((double)n * unitPx < minPx && n < (1L << 27)) n <<= 1;
    return n;
}

// 1 / 2 / 5 ladder for the SECONDS ruler: ms when zoomed in, minutes when zoomed
// far out, and never two labels closer than `minPx`.
double nice_time_step(double pxPerSec, double minPx)
{
    static const double ladder[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05,
                                     0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0,
                                     30.0, 60.0, 120.0, 300.0, 600.0, 1800.0 };
    if (pxPerSec <= 0.0) return 0.0;
    for (double s : ladder) if (s * pxPerSec >= minPx) return s;
    return 3600.0;
}

// Marker context-menu / keyboard command ids.
enum {
    MENU_ADD = 1, MENU_DEL, MENU_SEL_ALL, MENU_SEL_NONE, MENU_ZOOM_SEL,
    MENU_ZOOM_FIT, MENU_TRANSIENTS, MENU_TRANS_TICKS, MENU_RESET, MENU_PLAY,
    MENU_APPLY, MENU_UNDO, MENU_REDO
};

// ---- pointer shapes --------------------------------------------------------
//  Hover feedback the arrange view already sets the precedent for: the cursor
//  says what the press will do BEFORE you commit to it.
SDL_Cursor* g_arrow = nullptr;
SDL_Cursor* g_sizewe = nullptr;
int         g_active = 0;
void want_cursor(int want)
{
    if (!g_arrow) {
        g_arrow  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        g_sizewe = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    }
    if (want == g_active) return;
    SDL_Cursor* c = want == 1 ? g_sizewe : g_arrow;
    if (c) { SDL_SetCursor(c); g_active = want; }
}

} // namespace

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
    m_down = false;
    m_sb_drag = false;
    m_sel_drag = false;
    m_has_sel = false;
    m_sel_marker = -1;
    m_menu_open = false;
    m_cursor = 0.0;
    stop_audition();
    m_undo.clear(); m_redo.clear();
    m_markers.clear();
    ensure_default_markers();
    detect_transients();
    rebuild_peaks();
    publish_warp();                 // identity -> clears any prior live warp
    // Fit the clip horizontally on bind.  When the view has not been laid out
    // yet (a clip bound from the arrange window before the dock has a width)
    // rect.w is 0 and the fit is impossible -- it used to be skipped silently
    // and the clip appeared at whatever zoom the PREVIOUS clip left behind, so
    // a short sample opened scrolled off the end of the view.  Defer instead.
    m_scroll = 0.0;
    m_fit_pending = true;
    if (m_clip && m_clip->numFrames() > 0 && rect.w > 8) {
        double dstLen = (double)m_markers.back().dstSample;
        m_spp = std::max(0.02, dstLen / std::max(1, rect.w - 4));
        m_fit_pending = false;
    }
}

// Simple energy-onset transient detector (source time).  Short-time RMS
// envelope, then mark a rising edge that jumps above a fraction of the recent
// average -- the raw material for auto warp markers + snap-to-transient.
void SampleEditorView::detect_transients()
{
    m_transients.clear();
    if (!m_clip) return;
    // safeFrames(), not numFrames(): a clip mid-edit can have a SHORTER right
    // channel than left, and reading R[j] to the left length walked off the end
    // of the heap block.  Both readers below bound against the shorter one.
    const int64_t n = m_clip->safeFrames();
    if (n <= 0) return;
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

//----------------------------------------------------------------------------
//  cached peak table
//
//  Built ONCE per bind.  Without it every repaint reduced every visible sample
//  again: at 512 samples/px a five-minute clip re-read 14 million floats just
//  because the pointer moved over a toolbar button.
//----------------------------------------------------------------------------
void SampleEditorView::rebuild_peaks()
{
    m_peaks.clip = nullptr;
    m_peaks.frames = 0;
    m_peaks.mn.clear(); m_peaks.mx.clear(); m_peaks.rms.clear();
    if (!m_clip) return;
    const int64_t n = m_clip->safeFrames();
    if (n <= 0) return;
    const int b = 256;
    const size_t nb = (size_t)((n + b - 1) / b);
    m_peaks.mn.resize(nb); m_peaks.mx.resize(nb); m_peaks.rms.resize(nb);
    const float* L = m_clip->ch[0].data();
    const float* R = m_clip->ch[1].empty() ? L : m_clip->ch[1].data();
    for (size_t k = 0; k < nb; ++k) {
        const int64_t a = (int64_t)k * b, e = std::min<int64_t>(n, a + b);
        float mn = 1.f, mx = -1.f; double sq = 0.0;
        for (int64_t i = a; i < e; ++i) {
            const float l = L[i], r = R[i];
            if (l < mn) mn = l;
            if (l > mx) mx = l;
            if (r < mn) mn = r;
            if (r > mx) mx = r;
            const double m = 0.5 * ((double)l + (double)r);
            sq += m * m;
        }
        const int64_t cnt = std::max<int64_t>(1, e - a);
        m_peaks.mn[k] = mn; m_peaks.mx[k] = mx;
        m_peaks.rms[k] = (float)std::sqrt(sq / (double)cnt);
    }
    m_peaks.clip = m_clip;
    m_peaks.frames = n;
    m_peaks.bucket = b;
}

// min / max / rms over source frames [s0,s1) -- from the mip when the span is
// wide enough for the bucket edges to be sub-pixel, from the samples otherwise.
void SampleEditorView::column_peaks(int64_t s0, int64_t s1,
                                    float& mn, float& mx, float& rms) const
{
    mn = 0.f; mx = 0.f; rms = 0.f;
    if (!m_clip) return;
    const int64_t n = m_clip->safeFrames();
    if (n <= 0) return;
    if (s0 < 0) s0 = 0;
    if (s1 > n) s1 = n;
    if (s0 >= n) return;
    if (s1 <= s0) s1 = s0 + 1;

    const int b = m_peaks.bucket > 0 ? m_peaks.bucket : 256;
    const bool mipOk = (m_peaks.clip == m_clip && m_peaks.frames == n && !m_peaks.mn.empty());
    if (mipOk && (s1 - s0) >= 2 * b) {
        const size_t k0 = (size_t)(s0 / b);
        const size_t k1 = std::min(m_peaks.mn.size(), (size_t)((s1 + b - 1) / b));
        float lo = 1.f, hi = -1.f; double sq = 0.0; size_t cnt = 0;
        for (size_t k = k0; k < k1; ++k) {
            if (m_peaks.mn[k] < lo) lo = m_peaks.mn[k];
            if (m_peaks.mx[k] > hi) hi = m_peaks.mx[k];
            sq += (double)m_peaks.rms[k] * m_peaks.rms[k];
            ++cnt;
        }
        if (cnt) { mn = lo; mx = hi; rms = (float)std::sqrt(sq / (double)cnt); }
        return;
    }
    const float* L = m_clip->ch[0].data();
    const float* R = m_clip->ch[1].empty() ? L : m_clip->ch[1].data();
    float lo = 1.f, hi = -1.f; double sq = 0.0;
    for (int64_t i = s0; i < s1; ++i) {
        const float l = L[i], r = R[i];
        if (l < lo) lo = l;
        if (l > hi) hi = l;
        if (r < lo) lo = r;
        if (r > hi) hi = r;
        const double m = 0.5 * ((double)l + (double)r);
        sq += m * m;
    }
    mn = lo; mx = hi;
    rms = (float)std::sqrt(sq / (double)std::max<int64_t>(1, s1 - s0));
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

// One snapshot per user gesture.  Dragging, adding, deleting or auto-marking
// used to be permanent: the only way back from a mis-drag was RESET, which threw
// away every other marker as well.
void SampleEditorView::push_undo_state(std::vector<PatchKnob::engine::WarpMarker> state)
{
    m_undo.push_back(std::move(state));
    if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

void SampleEditorView::push_undo() { push_undo_state(m_markers); }

// Move one step along the marker-map history.  False when that end is empty,
// which is what lets the shell's project-wide undo take the key instead.
bool SampleEditorView::apply_history(bool redo)
{
    std::vector<std::vector<PatchKnob::engine::WarpMarker>>& from = redo ? m_redo : m_undo;
    std::vector<std::vector<PatchKnob::engine::WarpMarker>>& to   = redo ? m_undo : m_redo;
    if (from.empty()) return false;
    to.push_back(m_markers);
    if (to.size() > 64) to.erase(to.begin());
    m_markers = std::move(from.back());
    from.pop_back();
    ensure_default_markers();
    m_sel_marker = -1;
    m_drag = -1;                 // a live drag would write into the restored map
    m_down = false;
    publish_warp();
    return true;
}

// Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y from the toolkit's undo route.  The warp map is
// this view's OWN edit history and the context menu advertises Ctrl+Z for it;
// before this hook existed the shell's project undo fired instead and rolled the
// entire project back.  Returning false with an empty stack keeps Ctrl+Z meaning
// project undo whenever this view has nothing of its own to undo.
bool SampleEditorView::on_undo(App& app, bool redo)
{
    if (!apply_history(redo)) return false;
    app.request_redraw();
    return true;
}

double SampleEditorView::smart_snap_dst(double d) const
{
    if (m_bpm <= 0) return d;
    // ALT holds the marker exactly where the pointer is -- snapping you cannot
    // switch off is snapping that fights you on material that is not on the grid.
    if (SDL_GetModState() & KMOD_ALT) return d;
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
        m_markers = { { 0, 0 }, { n > 0 ? n : 1, n > 0 ? n : 1 } };   // identity = no warp
}

int SampleEditorView::dst_to_x(double d) const
{
    return rect.x + 2 + (int)std::lround((d - m_scroll) / (m_spp > 0 ? m_spp : 1.0));
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

//----------------------------------------------------------------------------
//  layout / view maths
//----------------------------------------------------------------------------
void SampleEditorView::layout_metrics(App& app)
{
    const int ch = app.mono.ch() > 0 ? app.mono.ch() : 12;
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
    // Every strip is derived from the FONT cell, so a fractional UI scale grows
    // the chrome with the text instead of clipping labels inside fixed boxes.
    toolbar_h   = ch + 8;
    ruler_h     = ch * 2 + 5;
    info_h      = ch + 5;
    scrollbar_h = std::max(9, ch - 1);
    m_grab      = std::max(5, cw);
    m_flag_w    = std::max(7, cw + 3) | 1;      // odd: centres on the hairline
    m_flag_h    = std::max(6, ch - 2);
    // In a short dock the waveform is what matters; shed chrome before shedding
    // wave height (a 40 px dock used to leave a negative-height wave area).
    if (rect.h - (toolbar_h + ruler_h + info_h + scrollbar_h) < ch * 2) info_h = 0;
    if (rect.h - (toolbar_h + ruler_h + info_h + scrollbar_h) < ch * 2) ruler_h = ch + 4;
    if (rect.h - (toolbar_h + ruler_h + info_h + scrollbar_h) < ch)     scrollbar_h = 0;
}

SDL_Rect SampleEditorView::wave_rect() const
{
    return SDL_Rect{ rect.x + 2, wave_y(), std::max(0, rect.w - 4), std::max(0, wave_h()) };
}
SDL_Rect SampleEditorView::ruler_rect() const
{
    return SDL_Rect{ rect.x, rect.y + toolbar_h, rect.w, ruler_h };
}
SDL_Rect SampleEditorView::info_rect() const
{
    return SDL_Rect{ rect.x, rect.y + rect.h - scrollbar_h - info_h, rect.w, info_h };
}

// Total warped timeline length (dst samples) that the view scrolls over.
double SampleEditorView::content_len() const
{
    double len = m_markers.empty() ? 0.0 : (double)m_markers.back().dstSample;
    if (m_clip && len < (double)m_clip->numFrames()) len = (double)m_clip->numFrames();
    return len > 1.0 ? len : 1.0;
}

double SampleEditorView::visible_dst() const
{
    return (double)std::max(1, rect.w - 4) * m_spp;
}

// Scrolling used to clamp at 0 only, so the wheel or a page-click could push the
// view arbitrarily far PAST the end of the clip and leave you staring at empty
// background with no way back except zooming out.
void SampleEditorView::clamp_scroll()
{
    const double maxScroll = std::max(0.0, content_len() - visible_dst());
    if (m_scroll > maxScroll) m_scroll = maxScroll;
    if (m_scroll < 0.0) m_scroll = 0.0;
}

// ONE zoom path for the wheel, the keys and the buttons: they used to clamp to
// different limits, so a wheel could reach a zoom the keyboard could not leave.
void SampleEditorView::set_zoom(double spp, int anchorX)
{
    const int ax = (anchorX >= rect.x && anchorX < rect.x + rect.w)
                 ? anchorX : rect.x + rect.w / 2;
    const double anchor = x_to_dst(ax);
    // Never let the whole clip shrink below the view width: past that point you
    // are scrolling emptiness.  Never go below ~1/50 sample per pixel either.
    const double maxSpp = std::max(1.0, content_len() / std::max(1, rect.w - 4)) * 1.5;
    m_spp = std::max(0.02, std::min(maxSpp, spp));
    m_scroll = anchor - (double)(ax - rect.x - 2) * m_spp;
    clamp_scroll();
}

void SampleEditorView::zoom_to(double a, double b)
{
    if (b < a) std::swap(a, b);
    if (b - a < 8.0) return;
    m_spp = std::max(0.02, (b - a) / std::max(1, rect.w - 4));
    m_scroll = a;
    clamp_scroll();
}

SDL_Rect SampleEditorView::scrollbar_track() const
{
    return SDL_Rect{ rect.x, rect.y + rect.h - scrollbar_h, rect.w, scrollbar_h };
}

SDL_Rect SampleEditorView::scrollbar_thumb() const
{
    const SDL_Rect tr = scrollbar_track();
    const int span = std::max(1, tr.w - 4);           // never divide by a 0-width bar
    const double total = content_len();
    const double visible = visible_dst();
    int tw = (int)std::lround((visible / total) * (double)span);
    if (tw < 16) tw = 16;
    if (tw > span) tw = span;
    int tx = tr.x + 2 + (int)std::lround((m_scroll / total) * (double)span);
    if (tx + tw > tr.x + tr.w - 2) tx = tr.x + tr.w - 2 - tw;
    if (tx < tr.x + 2) tx = tr.x + 2;
    return SDL_Rect{ tx, tr.y + 2, tw, std::max(1, tr.h - 4) };
}

//----------------------------------------------------------------------------
//  audition
//----------------------------------------------------------------------------
double SampleEditorView::play_cursor_dst() const
{
    if (!m_playing) return -1.0;
    const double secs = (double)((Uint32)SDL_GetTicks() - m_play_ms0) / 1000.0;
    const double d = m_play_from + secs * m_sr;
    return d;
}

void SampleEditorView::stop_audition()
{
    m_playing = false;
    m_last_play_x = -1;
}

// The engine preview plays a clip from its first frame, so auditioning from the
// cursor means handing it the tail.  Capped: a click in a ten-minute freeze
// should preview the passage you clicked, not commit the whole file to a copy.
void SampleEditorView::audition(App& app, double fromDst)
{
    if (!m_clip || m_clip->safeFrames() <= 0) return;
    const int64_t n = m_clip->safeFrames();
    int64_t from = (int64_t)std::llround(dst_to_src(fromDst));
    if (from < 0) from = 0;
    if (from >= n) return;
    int64_t to = n;
    if (m_has_sel && sel_hi() - sel_lo() > 1.0) {
        const int64_t s1 = (int64_t)std::llround(dst_to_src(sel_hi()));
        if (s1 > from) to = std::min(n, s1);
    }
    to = std::min(to, from + (int64_t)(20.0 * m_sr));      // 20 s of audition
    if (to <= from) return;
    AudioClip tail;
    tail.name = m_clip->name;
    tail.sampleRate = m_clip->sampleRate;
    tail.sourceSampleRate = m_clip->sourceSampleRate;
    tail.ch[0].assign(m_clip->ch[0].begin() + (size_t)from, m_clip->ch[0].begin() + (size_t)to);
    if ((int64_t)m_clip->ch[1].size() >= to)
        tail.ch[1].assign(m_clip->ch[1].begin() + (size_t)from, m_clip->ch[1].begin() + (size_t)to);
    else
        tail.ch[1] = tail.ch[0];
    PatchKnob::app::audio_app_preview_clip(tail, 1.0f);
    m_playing   = true;
    m_play_ms0  = (Uint32)SDL_GetTicks();
    m_play_from = fromDst;
    m_last_play_x = -1;
    app.request_redraw();
}

//----------------------------------------------------------------------------
//  marker geometry -- ONE definition, used by draw() and by the hit test
//----------------------------------------------------------------------------
SDL_Rect SampleEditorView::marker_flag(size_t i) const
{
    const int x = (i < m_markers.size()) ? dst_to_x((double)m_markers[i].dstSample) : 0;
    const SDL_Rect rl = ruler_rect();
    return SDL_Rect{ x - m_flag_w / 2, rl.y + rl.h - m_flag_h - 1, m_flag_w, m_flag_h };
}

int SampleEditorView::marker_at(int x, int y) const
{
    // Grabbable on the flag AND anywhere down the hairline: the handle used to
    // be drawn at the top of the ruler but hit-tested in a band that stopped
    // above the wave, so clicking the line you can see spawned a NEW marker
    // instead of picking up the one you were aiming at.
    const SDL_Rect rl = ruler_rect();
    const SDL_Rect w  = wave_rect();
    const bool inRuler = (y >= rl.y && y < rl.y + rl.h);
    const bool inWave  = (y >= w.y && y < w.y + w.h);
    if (!inRuler && !inWave) return -1;
    const int radius = inRuler ? m_grab + m_flag_w / 2 : m_grab;
    int best = -1, bestd = radius + 1;
    for (size_t i = 0; i < m_markers.size(); ++i) {
        const int mx = dst_to_x((double)m_markers[i].dstSample);
        const int d = std::abs(x - mx);
        if (d <= radius && d < bestd) { best = (int)i; bestd = d; }
    }
    return best;
}

// Insert (src -> dst) keeping BOTH axes strictly increasing.
//
// The map is only meaningful if src and dst both rise monotonically: every
// lookup (src_to_dst / dst_to_src) and the renderer itself interpolate between
// consecutive markers, so a marker whose dst sits at or before its predecessor's
// gives a zero-length or NEGATIVE segment -- a division by zero, then audio
// played backwards through that span.  Insertion used to order by srcSample
// ALONE, while dst came from smart_snap_dst(), which quantises to the tempo grid
// and can easily land on the wrong side of a neighbour.  The drag path already
// clamped into the neighbours' interior; this is the same rule for inserts.
//
// Returns the index inserted at, or -1 when the gap has no room for another
// marker (no snapshot is taken in that case).
int SampleEditorView::insert_marker_ordered(int64_t src, int64_t dst)
{
    if (m_markers.size() < 2) return -1;        // endpoints must already exist
    const WarpMarker probe{ src, dst };
    size_t i = (size_t)(std::lower_bound(m_markers.begin(), m_markers.end(), probe,
        [](const WarpMarker& a, const WarpMarker& b){ return a.srcSample < b.srcSample; })
        - m_markers.begin());
    // Never outside the two endpoints: they define the clip's extent.
    if (i == 0) i = 1;
    if (i >= m_markers.size()) i = m_markers.size() - 1;
    const WarpMarker& lo = m_markers[i - 1];
    const WarpMarker& hi = m_markers[i];
    if (hi.srcSample - lo.srcSample < 2) return -1;
    if (hi.dstSample - lo.dstSample < 2) return -1;
    src = std::max(lo.srcSample + 1, std::min(hi.srcSample - 1, src));
    dst = std::max(lo.dstSample + 1, std::min(hi.dstSample - 1, dst));
    m_markers.insert(m_markers.begin() + (long)i, WarpMarker{ src, dst });
    return (int)i;
}

void SampleEditorView::add_marker(double dstPos, bool snap)
{
    if (!m_clip) return;
    int64_t s = (int64_t)std::llround(dst_to_src(dstPos));
    // snap the SOURCE to a nearby transient (within ~8 px worth of samples)
    const int64_t tS = nearest_transient_src(s, (int64_t)(m_spp * 8.0));
    if (tS >= 0 && snap) s = tS;
    const int64_t d = (int64_t)std::llround(snap ? smart_snap_dst(dstPos) : dstPos);
    // Snapshot only once the insert is known to have happened, so a click in a
    // gap too small for a marker cannot push a no-op entry onto the history.
    std::vector<WarpMarker> before = m_markers;
    const int at = insert_marker_ordered(s, d);
    if (at < 0) return;                         // m_markers untouched
    push_undo_state(std::move(before));
    m_sel_marker = at;
    publish_warp();
}

void SampleEditorView::delete_marker(int index)
{
    // The two endpoints ARE the map: deleting one leaves src_to_dst() with a
    // single point and no gradient, which reads as "the clip has no length".
    if (index <= 0 || index + 1 >= (int)m_markers.size()) return;
    push_undo();
    m_markers.erase(m_markers.begin() + index);
    if (m_sel_marker >= (int)m_markers.size()) m_sel_marker = (int)m_markers.size() - 1;
    publish_warp();
}

//----------------------------------------------------------------------------
//  readouts
//----------------------------------------------------------------------------
std::string SampleEditorView::time_str(double dstSamples) const
{
    double secs = dstSamples / (m_sr > 0 ? m_sr : 48000.0);
    const bool neg = secs < 0;
    if (neg) secs = -secs;
    const int mins = (int)(secs / 60.0);
    const double rem = secs - mins * 60.0;
    char b[32];
    std::snprintf(b, sizeof(b), "%s%d:%06.3f", neg ? "-" : "", mins, rem);
    return b;
}

std::string SampleEditorView::bars_str(double dstSamples) const
{
    const double beat = beat_samples();
    if (beat <= 0) return "-";
    const double beats = dstSamples / beat;
    const long bar = (long)std::floor(beats / 4.0) + 1;
    const long bt  = (long)std::floor(beats) % 4 + 1;
    const long tick = (long)std::floor((beats - std::floor(beats)) * m_ppqn);
    char b[40];
    std::snprintf(b, sizeof(b), "%ld.%ld.%03ld", bar, bt, tick);
    return b;
}

//----------------------------------------------------------------------------
//  drawing
//----------------------------------------------------------------------------
void SampleEditorView::draw_toolbar(App& app)
{
    const Theme& t = theme();
    SDL_Rect tb{ rect.x, rect.y, rect.w, toolbar_h };
    fill_rect(app.ren, tb, t.panel);
    hline(app.ren, tb.x, tb.x + tb.w, tb.y + tb.h - 1, t.dim);

    const int pad = std::max(4, app.mono.cw());
    const int bh = toolbar_h - 6;
    int bx = rect.x + 4, by = rect.y + 3;

    auto button = [&](SDL_Rect& r, const std::string& lbl, const char* tip, bool on) {
        // Width comes from the MEASURED label, not strlen * cw: the latter is a
        // byte count times a nominal advance and clipped every label the moment
        // the UI scale stopped being 1.0.
        const int w = app.mono.text_w(lbl) + pad;
        r = SDL_Rect{ bx, by, w, bh };
        const bool hot = m_hover_in && in_rect(r, m_mx, m_my);
        fill_rect(app.ren, r, hot ? t.accent : (on ? t.panel : t.bg));
        frame_rect(app.ren, r, hot ? t.hi : t.dim);
        app.mono.draw(app.ren, r.x + pad / 2, r.y + (bh - app.mono.ch()) / 2, lbl,
                      hot ? t.bg : (on ? t.accent : t.text));
        if (hot && tip) { m_tip = tip; m_tip_anchor = r; }
        bx += w + 4;
        return hot;
    };

    button(m_btn_apply, "APPLY WARP", "Render the warp into the clip (Ctrl+Enter)", false);
    button(m_btn_reset, "RESET", "Drop every marker back to no-warp", false);
    button(m_btn_play, m_playing ? "STOP" : "PLAY",
           "Audition from the cursor (Space)", m_playing);
    button(m_btn_fit, "FIT", "Zoom the whole clip into view (F)", false);
    // transpose stepper: -  PITCH +n  +
    button(m_btn_trDn, "-", "Transpose down a semitone", false);
    {
        char tl[24]; std::snprintf(tl, sizeof(tl), "PITCH %+.0f", m_transpose);
        const int w = app.mono.text_w(tl);
        app.mono.draw(app.ren, bx, by + (bh - app.mono.ch()) / 2, tl,
                      m_transpose != 0.0 ? t.accent : t.dim);
        bx += w + 4;
    }
    button(m_btn_trUp, "+", "Transpose up a semitone", false);
    button(m_btn_trans, "MARK TRANSIENTS", "Put a warp marker on every detected onset", false);
    {
        static const char* MN[] = { "Complex","Tones","Texture","Beats","ComplexPro","RePitch" };
        const int mi = (int)m_mode;
        char ml[32];
        std::snprintf(ml, sizeof(ml), "MODE:%s",
                      (mi >= 0 && mi < 6) ? MN[mi] : "Complex");
        button(m_btn_mode, ml, "Stretch algorithm used by APPLY", true);
    }
    // A hint about the menu, in the space left over -- the actions live there.
    const int hintX = bx + 8;
    const int hintW = rect.x + rect.w - hintX - 4;
    if (hintW > 8 * app.mono.cw())
        app.mono.draw(app.ren, hintX, by + (bh - app.mono.ch()) / 2,
                      fit_text(app.mono, "right-click for actions   dbl-click adds a marker",
                               hintW), t.dim);
}

void SampleEditorView::draw_ruler(App& app)
{
    const Theme& t = theme();
    const SDL_Rect rl = ruler_rect();
    const SDL_Rect w  = wave_rect();
    fill_rect(app.ren, rl, t.panel);
    hline(app.ren, rl.x, rl.x + rl.w, rl.y + rl.h - 1, t.dim);
    if (!m_clip) return;

    const int ch = app.mono.ch();
    const int rowTop = rl.y + 1;                       // musical row (bars/beats)
    const int rowBot = rl.y + rl.h - ch - 1;           // seconds row
    const double right = x_to_dst(rect.x + rect.w);

    BlendScope blend(app.ren);

    // --- bars / beats -------------------------------------------------------
    const double beat = beat_samples();
    const double bar  = beat * 4.0;
    if (bar > 0 && m_spp > 0) {
        const double barPx = bar / m_spp;
        const long barN = pow2_stride(barPx, 7.0);
        // Bar NUMBERS get their own, coarser stride from how wide the widest
        // number actually is -- otherwise they overprint into a smear on
        // zoom-out instead of thinning 1 -> 2 -> 4 -> 8.
        char probe[16];
        std::snprintf(probe, sizeof(probe), "%ld", (long)(right / bar) + 2);
        const long labN = std::max(barN, pow2_stride(barPx, app.mono.text_w(probe) + 3.0 * app.mono.cw()));
        const double barStep = bar * (double)barN;
        // Label every Nth DRAWN line by index.  Testing fmod(position, labelStep)
        // instead looked equivalent but is not: at 127 BPM a bar is 22677.16...
        // samples, so no drawn position is ever an exact multiple and every
        // single bar number silently vanished.
        const long labEvery = std::max(1L, labN / std::max(1L, barN));
        long i0 = (long)std::floor(m_scroll / barStep);
        for (double d = i0 * barStep; d <= right; d += barStep, ++i0) {
            const int x = dst_to_x(d);
            if (x > rect.x + rect.w) break;
            if (x < rect.x) continue;
            const bool labelled = (((i0 % labEvery) + labEvery) % labEvery) == 0;
            vline(app.ren, x, rowTop, rl.y + rl.h - 2, fade(t.accent, labelled ? 200 : 120));
            vline(app.ren, x, w.y, w.y + w.h - 1, fade(t.accent, labelled ? 70 : 40));
            if (labelled) {
                char b[16]; std::snprintf(b, sizeof(b), "%ld", (long)std::llround(d / bar) + 1);
                app.mono.draw(app.ren, x + 3, rowTop, b, t.text);
            }
        }
        // Beat subdivisions only once every bar already has room for itself.
        const double beatPx = beat / m_spp;
        if (barN == 1 && beatPx >= 7.0) {
            long j0 = (long)std::floor(m_scroll / beat);
            for (double d = j0 * beat; d <= right; d += beat, ++j0) {
                if (j0 % 4 == 0) continue;
                const int x = dst_to_x(d);
                if (x > rect.x + rect.w) break;
                if (x < rect.x) continue;
                vline(app.ren, x, rowTop + 2, rowTop + 2 + ch / 2, fade(t.dim, 200));
                vline(app.ren, x, w.y, w.y + w.h - 1, fade(t.dim, 45));
            }
        }
    }

    // --- seconds / milliseconds --------------------------------------------
    //  Density adapts: ms when zoomed in, seconds mid-way, minutes far out, so
    //  the labels never collide and there is always SOME absolute time to read.
    const double pxPerSec = (m_spp > 0) ? m_sr / m_spp : 0.0;
    const double lblW = app.mono.text_w("0:00.000") + 2.0 * app.mono.cw();
    const double tickStep = nice_time_step(pxPerSec, 6.0);
    const double labStepS = nice_time_step(pxPerSec, lblW);
    if (tickStep > 0.0 && pxPerSec > 0.0 && ruler_h >= ch * 2 && rowBot > rowTop) {
        const double stepD = tickStep * m_sr;
        const long labEvery = std::max(1L, (long)std::llround(labStepS / tickStep));
        long k0 = (long)std::floor(m_scroll / stepD);
        int lastLabelEnd = rect.x - 1000;
        for (double d = k0 * stepD; d <= right; d += stepD, ++k0) {
            const int x = dst_to_x(d);
            if (x > rect.x + rect.w) break;
            if (x < rect.x) continue;
            const bool major = (((k0 % labEvery) + labEvery) % labEvery) == 0;
            vline(app.ren, x, rowBot - (major ? 4 : 2), rowBot - 1, fade(t.dim, major ? 230 : 150));
            if (major && x > lastLabelEnd) {
                const std::string s = time_str(d);
                app.mono.draw(app.ren, x + 2, rowBot, s, t.dim);
                lastLabelEnd = x + 2 + app.mono.text_w(s) + app.mono.cw();
            }
        }
    }
}

void SampleEditorView::draw_wave(App& app, const SDL_Rect& w)
{
    const Theme& t = theme();
    if (w.w < 2 || w.h < 4) return;
    const int mid = w.y + w.h / 2;
    const int amp = std::max(1, w.h / 2 - 2);      // never 0 or negative: a short
                                                   // dock used to invert the wave
    // Zero line UNDER the audio: visible through silence, hidden by loud
    // material, which is exactly where you want to see it.
    {
        BlendScope blend(app.ren);
        hline(app.ren, w.x, w.x + w.w - 1, mid, fade(t.dim, 170));
    }
    if (!m_clip || m_clip->safeFrames() <= 0) return;
    const int64_t n = m_clip->safeFrames();

    // Selection wash goes under the audio too, so the waveform stays crisp.
    const bool haveSel = m_has_sel && sel_hi() - sel_lo() > 0.5;
    int selX0 = 0, selX1 = 0;
    if (haveSel) {
        selX0 = std::max(w.x, dst_to_x(sel_lo()));
        selX1 = std::min(w.x + w.w, dst_to_x(sel_hi()));
        if (selX1 > selX0) {
            BlendScope blend(app.ren);
            fill_rect(app.ren, SDL_Rect{ selX0, w.y, selX1 - selX0, w.h }, fade(t.accent, 46));
        }
    }

    // SAMPLE-ACCURATE mode: when fewer than ~1 source sample maps to each pixel,
    // draw the actual samples as a connected polyline with dots (zoom to samples).
    const double srcPerPx = std::fabs(dst_to_src(x_to_dst(w.x + 1)) - dst_to_src(x_to_dst(w.x)));
    if (srcPerPx < 0.5) {
        const float* L = m_clip->ch[0].data();
        const float* R = m_clip->ch[1].empty() ? L : m_clip->ch[1].data();
        int64_t sa = (int64_t)std::floor(dst_to_src(x_to_dst(w.x))) - 1;
        int64_t sb = (int64_t)std::ceil (dst_to_src(x_to_dst(w.x + w.w))) + 1;
        if (sa < 0) sa = 0;
        if (sb > n) sb = n;
        set_color(app.ren, t.text);
        int prevX = -1, prevY = 0;
        for (int64_t si = sa; si < sb; ++si) {
            int px = dst_to_x(src_to_dst((double)si));
            float v = 0.5f * (L[si] + R[si]);
            int py = mid - (int)(v * amp);
            if (py < w.y) py = w.y;
            if (py > w.y + w.h - 1) py = w.y + w.h - 1;
            if (prevX >= 0) SDL_RenderDrawLine(app.ren, prevX, prevY, px, py);
            fill_rect(app.ren, SDL_Rect{ px - 1, py - 1, 3, 3 }, t.text);   // sample dot
            prevX = px; prevY = py;
        }
        return;
    }

    // Column reduction: peak envelope + RMS body, batched into four
    // RenderFillRects calls instead of one draw call per pixel column.
    m_col_peak.clear(); m_col_peak_dim.clear();
    m_col_rms.clear();  m_col_rms_dim.clear();
    m_col_clip.clear();
    m_col_peak.reserve((size_t)w.w);
    m_col_rms.reserve((size_t)w.w);
    for (int px = 0; px < w.w; ++px) {
        int64_t s0 = (int64_t)dst_to_src(x_to_dst(w.x + px));
        int64_t s1 = (int64_t)dst_to_src(x_to_dst(w.x + px + 1));
        if (s1 < s0) std::swap(s0, s1);
        if (s0 >= n || s1 <= 0) continue;
        float mn, mx, rms;
        column_peaks(s0, s1, mn, mx, rms);
        int y0 = mid - (int)(mx * amp);
        int y1 = mid - (int)(mn * amp);
        if (y1 < y0) std::swap(y0, y1);
        const bool clipped = (mx > 0.999f || mn < -0.999f);
        if (y0 < w.y) y0 = w.y;
        if (y1 > w.y + w.h - 1) y1 = w.y + w.h - 1;
        if (y1 < y0) continue;
        const int rh = (int)(rms * amp);
        const int b0 = std::max(w.y, mid - rh), b1 = std::min(w.y + w.h - 1, mid + rh);
        const SDL_Rect peak{ w.x + px, y0, 1, (y1 - y0) + 1 };
        const SDL_Rect body{ w.x + px, b0, 1, std::max(1, b1 - b0 + 1) };
        const bool inSel = haveSel && (w.x + px) >= selX0 && (w.x + px) < selX1;
        if (haveSel && !inSel) { m_col_peak_dim.push_back(peak); m_col_rms_dim.push_back(body); }
        else                   { m_col_peak.push_back(peak);     m_col_rms.push_back(body); }
        if (clipped) m_col_clip.push_back(SDL_Rect{ w.x + px, w.y, 1, 2 });
    }
    {
        // Outside the selection the audio is drawn at reduced alpha so the
        // selected range reads as the foreground rather than as an outline.
        BlendScope blend(app.ren);
        if (!m_col_peak_dim.empty()) {
            set_color(app.ren, fade(t.dim, 105));
            SDL_RenderFillRects(app.ren, m_col_peak_dim.data(), (int)m_col_peak_dim.size());
            set_color(app.ren, fade(t.text, 105));
            SDL_RenderFillRects(app.ren, m_col_rms_dim.data(), (int)m_col_rms_dim.size());
        }
    }
    if (!m_col_peak.empty()) {
        set_color(app.ren, t.dim);                       // soft peak envelope
        SDL_RenderFillRects(app.ren, m_col_peak.data(), (int)m_col_peak.size());
        set_color(app.ren, t.text);                      // solid RMS body inside it
        SDL_RenderFillRects(app.ren, m_col_rms.data(), (int)m_col_rms.size());
    }
    if (!m_col_clip.empty()) {                           // over-0 dBFS columns
        set_color(app.ren, t.sel);
        SDL_RenderFillRects(app.ren, m_col_clip.data(), (int)m_col_clip.size());
    }

    // Firm selection edges on top of the audio.
    if (haveSel && selX1 > selX0) {
        vline(app.ren, selX0, w.y, w.y + w.h - 1, t.accent);
        vline(app.ren, selX1 - 1, w.y, w.y + w.h - 1, t.accent);
    }
}

void SampleEditorView::draw_markers(App& app, const SDL_Rect& w)
{
    const Theme& t = theme();
    if (!m_clip) return;
    const SDL_Rect rl = ruler_rect();

    // transient ticks (source onsets mapped through the warp map)
    if (m_show_transients) {
        BlendScope blend(app.ren);
        for (int64_t tS : m_transients) {
            const int x = dst_to_x(src_to_dst((double)tS));
            if (x < w.x || x >= w.x + w.w) continue;
            vline(app.ren, x, w.y, w.y + std::max(3, w.h / 12), fade(t.hi, 140));
        }
    }

    for (size_t i = 0; i < m_markers.size(); ++i) {
        const int x = dst_to_x((double)m_markers[i].dstSample);
        if (x < rect.x - m_flag_w || x > rect.x + rect.w + m_flag_w) continue;
        const bool end = (i == 0 || i + 1 == m_markers.size());
        const bool hot = ((int)i == m_hover_marker) || ((int)i == m_drag);
        const bool sel = ((int)i == m_sel_marker);
        const Color mc = end ? t.dim : t.hi;
        {   // full-height hairline, translucent so it never buries the audio
            BlendScope blend(app.ren);
            vline(app.ren, x, w.y, w.y + w.h - 1, fade(mc, hot ? 235 : 150));
        }
        // A grabbable FLAG: filled box on the ruler with a small tail, exactly
        // the rect marker_at() tests, so what you can see is what you can grab.
        const SDL_Rect f = marker_flag(i);
        fill_rect(app.ren, f, hot ? t.accent : mc);
        frame_rect(app.ren, f, sel ? t.sel : t.bg);
        for (int k = 0; k < m_flag_h / 3; ++k)          // little downward point
            hline(app.ren, x - (m_flag_h / 3 - k), x + (m_flag_h / 3 - k),
                  f.y + f.h + k, hot ? t.accent : mc);
        if (!end && f.w >= app.mono.cw()) {
            char b[8]; std::snprintf(b, sizeof(b), "%d", (int)i);
            app.mono.draw_fitted(app.ren, SDL_Rect{ f.x + 1, f.y, f.w - 2, f.h }, b,
                                 t.bg, true, 0.75f, 0.4f, false);
        }
        if (hot) {
            // Position readout parked above the flag while you are on it.
            const std::string s = bars_str((double)m_markers[i].dstSample);
            const int tw = app.mono.text_w(s) + app.mono.cw();
            SDL_Rect box{ std::min(x + 6, rect.x + rect.w - tw - 2), rl.y + 1,
                          tw, app.mono.ch() + 2 };
            fill_rect(app.ren, box, t.bg);
            frame_rect(app.ren, box, t.accent);
            app.mono.draw(app.ren, box.x + app.mono.cw() / 2, box.y + 1, s, t.accent);
        }
    }

    // --- play cursor: drawn LAST so it is never buried ----------------------
    const double p = play_cursor_dst();
    if (p >= 0) {
        const int x = dst_to_x(p);
        if (x >= w.x && x < w.x + w.w) {
            vline(app.ren, x, w.y, w.y + w.h - 1, t.sel);
            for (int k = 0; k < 4; ++k)                 // arrow head at the top
                hline(app.ren, x - (3 - k), x + (3 - k), w.y + k, t.sel);
            // Pad the damage generously: this frame is clipped to the previous
            // frame's damage, so the padding covers the distance travelled.
            app.add_damage(SDL_Rect{ x - 24, w.y, 48, w.h });
        }
    }
    // The edit cursor (where an audition would start from).
    if (!m_playing) {
        const int x = dst_to_x(m_cursor);
        if (x >= w.x && x < w.x + w.w) {
            BlendScope blend(app.ren);
            vline(app.ren, x, w.y, w.y + w.h - 1, fade(t.sel, 190));
        }
    }
}

void SampleEditorView::draw_info(App& app)
{
    if (info_h <= 0) return;
    const Theme& t = theme();
    const SDL_Rect r = info_rect();
    fill_rect(app.ren, r, t.panel);
    hline(app.ren, r.x, r.x + r.w, r.y, t.dim);
    if (!m_clip) return;
    const int ty = r.y + (r.h - app.mono.ch()) / 2;
    const int cw = app.mono.cw();
    int x = r.x + 4;

    auto field = [&](const char* label, const std::string& value, Color vc) {
        const int lw = app.mono.text_w(label);
        const int vw = app.mono.text_w(value);
        if (x + lw + vw + cw * 2 > r.x + r.w - 4) return false;
        app.mono.draw(app.ren, x, ty, label, t.dim);
        app.mono.draw(app.ren, x + lw + cw / 2, ty, value, vc);
        x += lw + vw + cw * 2;
        return true;
    };

    const double len = content_len();
    field("LEN", time_str(len), t.text);
    field("POS", (m_hover_in && m_mx >= 0) ? time_str(x_to_dst(m_mx)) : time_str(m_cursor), t.text);
    field("BAR", (m_hover_in && m_mx >= 0) ? bars_str(x_to_dst(m_mx)) : bars_str(m_cursor), t.text);
    if (m_has_sel && sel_hi() - sel_lo() > 0.5) {
        const double span = sel_hi() - sel_lo();
        char smp[32]; std::snprintf(smp, sizeof(smp), "%lld", (long long)std::llround(span));
        field("SEL", smp, t.accent);
        field("=", time_str(span), t.accent);
        char bars[32];
        std::snprintf(bars, sizeof(bars), "%.3f bars", span / std::max(1.0, beat_samples() * 4.0));
        field("=", bars, t.accent);
    } else {
        field("SEL", "none (drag the wave)", t.dim);
    }

    // Right-aligned numerics so the digits line up as the zoom changes.
    char z[48];
    std::snprintf(z, sizeof(z), "%u mk   %.2f smp/px",
                  (unsigned)m_markers.size(), m_spp);
    const int zw = app.mono.text_w(z);
    if (r.x + r.w - 4 - zw > x)
        app.mono.draw(app.ren, r.x + r.w - 4 - zw, ty, z, t.dim);
}

void SampleEditorView::draw(App& app)
{
    const Theme& t = theme();
    layout_metrics(app);

    // Hover state has to be POLLED: SDL only delivers motion events while a
    // button is held, so a view that tracks the pointer from on_mouse alone
    // highlights whatever was last clicked instead of what is under the mouse.
    int mx = -1, my = -1;
    ui::mouse_logical(app, mx, my);
    m_hover_in = hit(mx, my);
    if (m_hover_in) { m_mx = mx; m_my = my; }
    m_tip = nullptr;

    fill_rect(app.ren, rect, t.bg);
    draw_toolbar(app);

    if (!m_clip) {
        // Empty state that says what to do, rather than a blank panel.  The
        // toolbar tooltips below still paint, so hovering a button explains it
        // even before a clip is bound.
        const SDL_Rect w = wave_rect();
        const int ch = app.mono.ch();
        const char* l1 = "NO CLIP BOUND";
        const char* l2 = "select an audio clip in the arrange window to warp it";
        const int w1 = app.mono.text_w(l1), w2 = app.mono.text_w(l2);
        const int bw = std::min(w.w - 8, std::max(w1, w2) + 6 * app.mono.cw());
        SDL_Rect box{ w.x + (w.w - bw) / 2, w.y + w.h / 2 - ch * 2, bw, ch * 4 };
        if (bw > 0 && w.h > ch * 4) {
            fill_rect(app.ren, box, t.panel);
            frame_rect(app.ren, box, t.dim);
            app.mono.draw(app.ren, box.x + (bw - w1) / 2, box.y + ch / 2, l1, t.accent);
            app.mono.draw(app.ren, box.x + (bw - w2) / 2, box.y + ch * 2, fit_text(app.mono, l2, bw - 4), t.dim);
        } else if (w.h > ch) {
            app.mono.draw(app.ren, w.x + 4, w.y + std::max(0, (w.h - ch) / 2),
                          fit_text(app.mono, l2, w.w - 8), t.dim);
        }
    } else {
        // Deferred fit (see set_clip): the first draw with a real width owns it.
        if (m_fit_pending && rect.w > 8) {
            m_spp = std::max(0.02, content_len() / std::max(1, rect.w - 4));
            m_scroll = 0.0;
            m_fit_pending = false;
        }
        clamp_scroll();
        if (m_peaks.clip != m_clip || m_peaks.frames != m_clip->safeFrames()) rebuild_peaks();

        m_hover_marker = m_hover_in ? marker_at(m_mx, m_my) : -1;

        const SDL_Rect w = wave_rect();
        draw_ruler(app);
        draw_wave(app, w);
        draw_markers(app, w);
        draw_info(app);

        // --- horizontal scrollbar (bottom) ----------------------------------
        if (scrollbar_h > 0) {
            SDL_Rect tr = scrollbar_track();
            fill_rect(app.ren, tr, t.panel);
            hline(app.ren, tr.x, tr.x + tr.w, tr.y, t.dim);
            SDL_Rect th = scrollbar_thumb();
            bool hot = (m_hover_in && in_rect(th, m_mx, m_my)) || m_sb_drag;
            fill_rect(app.ren, th, hot ? t.accent : t.hi);
            frame_rect(app.ren, th, t.dim);
        }
    }

    if (m_menu_open) draw_menu(app);

    // Tooltip last, so it floats over everything.
    if (m_tip && !m_menu_open) {
        const int cw = app.mono.cw(), ch = app.mono.ch();
        const int tw = app.mono.text_w(m_tip) + cw * 2;
        SDL_Rect box{ std::min(m_tip_anchor.x, rect.x + rect.w - tw - 2),
                      m_tip_anchor.y + m_tip_anchor.h + 2, tw, ch + 4 };
        if (box.y + box.h > rect.y + rect.h) box.y = m_tip_anchor.y - box.h - 2;
        if (box.x < rect.x) box.x = rect.x;
        fill_rect(app.ren, box, t.panel);
        frame_rect(app.ren, box, t.accent);
        app.mono.draw(app.ren, box.x + cw, box.y + 2, m_tip, t.text);
    }

    // Keep the play cursor moving without asking for a repaint on every single
    // frame: only when it has actually reached a new pixel.
    if (m_playing) {
        const double p = play_cursor_dst();
        if (p > content_len() + m_sr * 0.25) stop_audition();
        else {
            const int px = dst_to_x(p);
            if (px != m_last_play_x) { m_last_play_x = px; app.request_redraw(); }
        }
    }
    apply_cursor();
}

//----------------------------------------------------------------------------
//  right-click menu
//----------------------------------------------------------------------------
void SampleEditorView::open_menu(App& app, int x, int y)
{
    m_menu.clear();
    m_menu_marker = marker_at(x, y);
    m_menu_dst = x_to_dst(x);
    const bool interior = m_menu_marker > 0 && m_menu_marker + 1 < (int)m_markers.size();
    m_menu.push_back({ "Add warp marker here", MENU_ADD, m_clip != nullptr, false });
    m_menu.push_back({ "Delete this marker", MENU_DEL, interior, false });
    m_menu.push_back({ "", 0, false, true });
    m_menu.push_back({ "Play from here            Space", MENU_PLAY, m_clip != nullptr, false });
    m_menu.push_back({ "Select all                Ctrl+A", MENU_SEL_ALL, true, false });
    m_menu.push_back({ "Clear selection           Esc", MENU_SEL_NONE, m_has_sel, false });
    m_menu.push_back({ "Zoom to selection         Z", MENU_ZOOM_SEL, m_has_sel, false });
    m_menu.push_back({ "Zoom to fit               F", MENU_ZOOM_FIT, true, false });
    m_menu.push_back({ "", 0, false, true });
    m_menu.push_back({ "Mark every transient", MENU_TRANSIENTS, !m_transients.empty(), false });
    m_menu.push_back({ m_show_transients ? "Hide transient ticks" : "Show transient ticks",
                       MENU_TRANS_TICKS, true, false });
    m_menu.push_back({ "Undo                      Ctrl+Z", MENU_UNDO, !m_undo.empty(), false });
    m_menu.push_back({ "Redo                      Ctrl+Shift+Z", MENU_REDO, !m_redo.empty(), false });
    m_menu.push_back({ "Reset warp map", MENU_RESET, true, false });
    m_menu.push_back({ "", 0, false, true });
    m_menu.push_back({ "Apply warp                Ctrl+Enter", MENU_APPLY, m_clip != nullptr, false });

    const int ch = app.mono.ch();
    const int rowh = ch + 5, seph = 5;
    int wpx = 0, hpx = 4;
    for (const MenuRow& r : m_menu) {
        wpx = std::max(wpx, app.mono.text_w(r.label));
        hpx += r.separator ? seph : rowh;
    }
    wpx += 4 * app.mono.cw();
    SDL_Rect box{ x, y, wpx, hpx };
    if (box.x + box.w > rect.x + rect.w) box.x = rect.x + rect.w - box.w;
    if (box.y + box.h > rect.y + rect.h) box.y = std::max(rect.y, y - box.h);
    if (box.x < rect.x) box.x = rect.x;
    if (box.y < rect.y) box.y = rect.y;
    m_menu_box = box;
    m_menu_rows.clear();
    int yy = box.y + 2;
    for (const MenuRow& r : m_menu) {
        if (r.separator) { m_menu_rows.push_back(SDL_Rect{ box.x, yy, box.w, seph }); yy += seph; }
        else             { m_menu_rows.push_back(SDL_Rect{ box.x, yy, box.w, rowh }); yy += rowh; }
    }
    m_menu_open = true;
    app.request_redraw();
}

void SampleEditorView::draw_menu(App& app)
{
    const Theme& t = theme();
    fill_rect(app.ren, m_menu_box, t.panel);
    frame_rect(app.ren, m_menu_box, t.accent);
    for (size_t i = 0; i < m_menu.size() && i < m_menu_rows.size(); ++i) {
        const MenuRow& r = m_menu[i];
        const SDL_Rect& q = m_menu_rows[i];
        if (r.separator) {
            hline(app.ren, q.x + 3, q.x + q.w - 4, q.y + q.h / 2, t.dim);
            continue;
        }
        const bool hot = r.enabled && m_hover_in && in_rect(q, m_mx, m_my);
        if (hot) fill_rect(app.ren, q, t.accent);
        app.mono.draw(app.ren, q.x + app.mono.cw(), q.y + 2,
                      fit_text(app.mono, r.label, q.w - 2 * app.mono.cw()),
                      hot ? t.bg : (r.enabled ? t.text : t.dim));
    }
}

bool SampleEditorView::menu_click(App& app, int x, int y)
{
    if (!in_rect(m_menu_box, x, y)) { m_menu_open = false; app.request_redraw(); return true; }
    for (size_t i = 0; i < m_menu.size() && i < m_menu_rows.size(); ++i) {
        if (!in_rect(m_menu_rows[i], x, y)) continue;
        if (m_menu[i].separator || !m_menu[i].enabled) return true;
        const int id = m_menu[i].id;
        m_menu_open = false;
        run_menu(app, id);
        return true;
    }
    return true;
}

void SampleEditorView::run_menu(App& app, int id)
{
    switch (id) {
    case MENU_ADD:  add_marker(m_menu_dst, true); break;
    case MENU_DEL:  delete_marker(m_menu_marker); break;
    case MENU_PLAY: m_cursor = m_menu_dst; audition(app, m_cursor); break;
    case MENU_SEL_ALL:  m_selA = 0; m_selB = content_len(); m_has_sel = true; break;
    case MENU_SEL_NONE: m_has_sel = false; break;
    case MENU_ZOOM_SEL: if (m_has_sel) zoom_to(sel_lo(), sel_hi()); break;
    case MENU_ZOOM_FIT: zoom_to(0.0, content_len()); break;
    case MENU_TRANS_TICKS: m_show_transients = !m_show_transients; break;
    case MENU_TRANSIENTS: {
        // Insert a warp marker at every detected transient (pinned to its current
        // warped position); keeps the endpoints, dedups near existing markers.
        push_undo();
        for (int64_t tS : m_transients) {
            bool dup = false;
            for (const auto& mk : m_markers)
                if (std::llabs(mk.srcSample - tS) < (int64_t)(0.02 * m_sr)) { dup = true; break; }
            if (dup) continue;
            // Same ordered insert as add_marker: src_to_dst() is only monotone
            // while the map is well formed, so each new marker has to be
            // clamped into its neighbours' interior rather than trusted.
            insert_marker_ordered(tS, (int64_t)std::llround(src_to_dst((double)tS)));
        }
        publish_warp();
        break;
    }
    case MENU_UNDO: apply_history(false); break;
    case MENU_REDO: apply_history(true);  break;
    case MENU_RESET:
        push_undo();
        m_markers.clear(); ensure_default_markers(); m_transpose = 0; publish_warp();
        break;
    case MENU_APPLY:
        if (m_clip && on_apply_warp && m_markers.size() >= 2)
            on_apply_warp(m_seq, m_markers, m_transpose, m_mode, 0.0);
        break;
    default: break;
    }
    app.request_redraw();
}

void SampleEditorView::apply_cursor()
{
    if (!m_hover_in) { if (g_active) want_cursor(0); return; }
    want_cursor((m_hover_marker >= 0 || m_drag >= 0) ? 1 : 0);
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
void SampleEditorView::cancel_interaction(App& app)
{
    m_down = false; m_drag = -1; m_sb_drag = false; m_sel_drag = false;
    m_menu_open = false;
    app.request_redraw();
}

bool SampleEditorView::on_mouse(App& app, const MouseEv& e)
{
    m_mx = e.x; m_my = e.y;
    if (!e.pressed) { m_down = false; m_drag = -1; m_sb_drag = false; m_sel_drag = false; return true; }

    if (m_menu_open && !m_down) { m_down = true; return menu_click(app, e.x, e.y); }

    // motion during a scrollbar-thumb drag.
    if (m_down && m_sb_drag) {
        SDL_Rect tr = scrollbar_track();
        const double total = content_len();
        const double dx = (double)(e.x - m_sb_ref_x);
        m_scroll = m_sb_ref_scroll + dx / (double)std::max(1, tr.w - 4) * total;
        clamp_scroll();
        app.request_redraw();
        return true;
    }

    // motion while dragging out a selection
    if (m_down && m_sel_drag) {
        m_selB = std::max(0.0, std::min(content_len(), x_to_dst(e.x)));
        m_has_sel = true;
        // Auto-scroll when the drag leaves the view, otherwise a selection can
        // never be longer than one screen.
        if (e.x < rect.x + 8)              { m_scroll -= 8 * m_spp; clamp_scroll(); }
        else if (e.x > rect.x + rect.w - 8) { m_scroll += 8 * m_spp; clamp_scroll(); }
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

    // Right-click anywhere: the actions menu (house style -- one-shot commands
    // belong in the menu, not spread across a toolbar that has no room for them).
    if (e.button == SDL_BUTTON_RIGHT) {
        open_menu(app, e.x, e.y);
        return true;
    }

    // scrollbar: press the thumb to drag; press the track to page toward the click
    if (scrollbar_h > 0) {
        SDL_Rect tr = scrollbar_track();
        if (e.y >= tr.y && e.y < tr.y + tr.h) {
            SDL_Rect th = scrollbar_thumb();
            if (e.x >= th.x && e.x < th.x + th.w) {
                m_sb_drag = true; m_sb_ref_x = e.x; m_sb_ref_scroll = m_scroll;
            } else {
                m_scroll += (e.x < th.x ? -visible_dst() * 0.9 : visible_dst() * 0.9);
                clamp_scroll();
            }
            app.request_redraw();
            return true;
        }
    }

    // toolbar buttons
    auto in = [&](const SDL_Rect& r){ return in_rect(r, e.x, e.y); };
    if (in(m_btn_reset)) { run_menu(app, MENU_RESET); return true; }
    if (in(m_btn_trUp))  { m_transpose = std::min(24.0, m_transpose + 1); app.request_redraw(); return true; }
    if (in(m_btn_trDn))  { m_transpose = std::max(-24.0, m_transpose - 1); app.request_redraw(); return true; }
    if (in(m_btn_mode))  { m_mode = (PatchKnob::engine::WarpMode)(((int)m_mode + 1) % 6); app.request_redraw(); return true; }
    if (in(m_btn_fit))   { zoom_to(0.0, content_len()); app.request_redraw(); return true; }
    if (in(m_btn_play)) {
        if (m_playing) { PatchKnob::app::audio_app_preview_clip(AudioClip(), 1.f); stop_audition(); }
        else audition(app, m_cursor);
        app.request_redraw();
        return true;
    }
    if (in(m_btn_trans)) { run_menu(app, MENU_TRANSIENTS); return true; }
    if (in(m_btn_apply)) { run_menu(app, MENU_APPLY); return true; }
    if (!m_clip) return true;

    // grab an existing marker handle?
    const int mi = marker_at(e.x, e.y);
    if (mi >= 0) {
        m_drag = mi;
        m_sel_marker = mi;
        m_drag_src = (SDL_GetModState() & KMOD_SHIFT) != 0;
        push_undo();                      // one snapshot for the whole drag
        app.request_redraw();
        return true;
    }

    // Click the ruler to park the cursor (and hence where Space auditions from)
    // without disturbing the selection -- the DAW convention.
    const SDL_Rect rl = ruler_rect();
    if (e.button == SDL_BUTTON_LEFT && in_rect(rl, e.x, e.y)) {
        m_cursor = std::max(0.0, std::min(content_len(), x_to_dst(e.x)));
        app.request_redraw();
        return true;
    }

    const SDL_Rect w = wave_rect();
    if (e.button == SDL_BUTTON_LEFT && e.y >= wave_y() && e.y < w.y + w.h) {
        const Uint32 now = (Uint32)SDL_GetTicks();
        const bool dbl = (now - m_last_click_ms) < 400 && std::abs(e.x - m_last_click_x) <= 4;
        m_last_click_ms = now; m_last_click_x = e.x;
        if (dbl) {
            // DOUBLE-click adds a marker.  A single click used to add one, so
            // simply clicking the waveform to look at a position littered the
            // map with markers you then had to hunt down and delete.
            add_marker(x_to_dst(e.x), true);
            app.request_redraw();
            return true;
        }
        const double d = std::max(0.0, std::min(content_len(), x_to_dst(e.x)));
        if (SDL_GetModState() & KMOD_SHIFT) {       // shift-click extends
            if (!m_has_sel) { m_selA = m_cursor; m_has_sel = true; }
            m_selB = d;
        } else {
            m_cursor = d;
            m_selA = m_selB = d;
            m_has_sel = false;
        }
        m_sel_drag = true;
        app.request_redraw();
        return true;
    }
    return true;
}

bool SampleEditorView::on_wheel(App& app, int dx, int dy)
{
    if (dy == 0) return dx != 0;
    // The anchor must be the LIVE pointer: m_mx is only updated by mouse events
    // and SDL sends motion only while a button is held, so wheel-zoom used to
    // anchor on wherever you last clicked -- the view lurched sideways.
    int mx = -1, my = -1;
    ui::mouse_logical(app, mx, my);
    if (!hit(mx, my)) { mx = m_mx; my = m_my; }
    const SDL_Keymod mod = SDL_GetModState();
    if (mod & KMOD_SHIFT) {                        // shift+wheel = scroll
        m_scroll += (dy > 0 ? -1 : 1) * visible_dst() * 0.15;
        clamp_scroll();
    } else {
        set_zoom(m_spp * (dy > 0 ? 0.85 : 1.18), mx);
    }
    app.request_redraw();
    return true;
}

bool SampleEditorView::on_key(App& app, SDL_Keycode k)
{
    const SDL_Keymod mod = SDL_GetModState();
    const bool ctrl = (mod & KMOD_CTRL) != 0;
    const double page = visible_dst();
    const double step = (mod & KMOD_SHIFT) ? m_spp * 20.0 : m_spp * 2.0;

    switch (k) {
    case SDLK_ESCAPE:
        // One key that always gets you out: cancel a drag, close the menu, then
        // drop the selection.
        if (m_menu_open) { m_menu_open = false; }
        else if (m_down || m_drag >= 0 || m_sel_drag) { cancel_interaction(app); }
        else if (m_has_sel) { m_has_sel = false; }
        else if (m_playing) { PatchKnob::app::audio_app_preview_clip(AudioClip(), 1.f); stop_audition(); }
        else return false;
        app.request_redraw();
        return true;
    case SDLK_SPACE:
        if (m_playing) { PatchKnob::app::audio_app_preview_clip(AudioClip(), 1.f); stop_audition(); }
        else audition(app, m_has_sel ? sel_lo() : m_cursor);
        app.request_redraw();
        return true;
    case SDLK_LEFT:
    case SDLK_RIGHT: {
        const double dir = (k == SDLK_LEFT) ? -1.0 : 1.0;
        if (ctrl && m_sel_marker > 0 && m_sel_marker + 1 < (int)m_markers.size()) {
            // Nudge the selected marker by a pixel (shift = 20) -- fine placement
            // that a mouse drag simply cannot reach at a coarse zoom.
            push_undo();
            WarpMarker& mk = m_markers[m_sel_marker];
            const int64_t lo = m_markers[m_sel_marker - 1].dstSample + 1;
            const int64_t hi = m_markers[m_sel_marker + 1].dstSample - 1;
            mk.dstSample = std::max(lo, std::min(hi, mk.dstSample + (int64_t)(dir * step)));
            publish_warp();
        } else {
            m_cursor = std::max(0.0, std::min(content_len(), m_cursor + dir * step));
            if (dst_to_x(m_cursor) < rect.x + 8 || dst_to_x(m_cursor) > rect.x + rect.w - 8) {
                m_scroll = m_cursor - page * 0.5;
                clamp_scroll();
            }
        }
        app.request_redraw();
        return true;
    }
    case SDLK_HOME:
        m_cursor = 0; m_scroll = 0; clamp_scroll(); app.request_redraw(); return true;
    case SDLK_END:
        m_cursor = content_len(); m_scroll = content_len(); clamp_scroll();
        app.request_redraw(); return true;
    case SDLK_PAGEUP:
        m_scroll -= page * 0.9; clamp_scroll(); app.request_redraw(); return true;
    case SDLK_PAGEDOWN:
        m_scroll += page * 0.9; clamp_scroll(); app.request_redraw(); return true;
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        if (m_sel_marker > 0 && m_sel_marker + 1 < (int)m_markers.size()) {
            delete_marker(m_sel_marker); app.request_redraw(); return true;
        }
        return false;
    case SDLK_z:
        // Ctrl+Z never actually arrives here -- the toolkit routes it to
        // on_undo() below before on_key runs -- but keep the two paths on the
        // same implementation so they can never drift.
        if (ctrl) return on_undo(app, (SDL_GetModState() & KMOD_SHIFT) != 0);
        if (m_has_sel) { zoom_to(sel_lo(), sel_hi()); app.request_redraw(); return true; }
        return false;
    case SDLK_a:
        if (ctrl) { m_selA = 0; m_selB = content_len(); m_has_sel = true; app.request_redraw(); return true; }
        return false;                 // bare 'a' used to RENDER the warp
    case SDLK_f:
        zoom_to(0.0, content_len()); app.request_redraw(); return true;
    case SDLK_EQUALS:
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
        set_zoom(m_spp * 0.7, dst_to_x(m_cursor)); app.request_redraw(); return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        set_zoom(m_spp * 1.4, dst_to_x(m_cursor)); app.request_redraw(); return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        // APPLY is destructive (it re-renders the region's audio), so it takes a
        // deliberate chord.  It used to run on a bare 'a'.
        if (ctrl) { run_menu(app, MENU_APPLY); return true; }
        return false;
    default: break;
    }
    return false;
}

} // namespace samped
