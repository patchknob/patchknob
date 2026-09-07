//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_view.cpp  -- implementation.  See arrange_view.h.
//
//  Ports src/perfroll.cpp + src/perfnames.cpp + src/perftime.cpp onto the SDL2
//  toolkit.  All legacy pixmap drawing becomes ui:: draw helpers; the
//  colour roles map 1:1 (m_black->bg, m_panel->panel, m_grey->accent,
//  m_lt_grey/m_dk_grey->dim, m_white->hi, m_note->note).
//----------------------------------------------------------------------------
#include "arrange_view.h"
#include "meter.h"
#include "perform.h"
#include "sequence.h"
#include "globals.h"
#include "quantize.h"
#include "engine/audioclip/audio_clip.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace ui;

namespace arrange {

// Bumped whenever the Pencil destructively rewrites clip samples
// (arrange_protools.cpp), so the reduced-envelope cache below re-reads them.
int g_wave_epoch = 0;

//----------------------------------------------------------------------------
//  small drawing helpers
//----------------------------------------------------------------------------
static std::string fit_text(const ui::Font& font, std::string text, int maxw)
{
    if (maxw <= 0) return "";
    if (font.text_w(text) <= maxw) return text;
    const std::string ell = "...";
    if (font.text_w(ell) > maxw) {
        std::string dots = ell;
        while (!dots.empty() && font.text_w(dots) > maxw) dots.pop_back();
        return dots;
    }
    while (!text.empty() && font.text_w(text + ell) > maxw) text.pop_back();
    return text.empty() ? ell : text + ell;
}

static SDL_Rect clamp_popup_rect(SDL_Rect box, const SDL_Rect& bounds)
{
    if (box.w > bounds.w) box.w = bounds.w;
    if (box.h > bounds.h) box.h = bounds.h;
    if (box.x + box.w > bounds.x + bounds.w) box.x = bounds.x + bounds.w - box.w;
    if (box.y + box.h > bounds.y + bounds.h) box.y = bounds.y + bounds.h - box.h;
    if (box.x < bounds.x) box.x = bounds.x;
    if (box.y < bounds.y) box.y = bounds.y;
    return box;
}

// Filled rounded rectangle (row-span scanline with circular corners).
static void fill_round(SDL_Renderer* r, SDL_Rect q, int rad, Color c)
{
    if (q.w <= 0 || q.h <= 0) return;
    if (rad * 2 > q.w) rad = q.w / 2;
    if (rad * 2 > q.h) rad = q.h / 2;
    if (rad < 1) { fill_rect(r, q, c); return; }
    set_color(r, c);
    for (int yy = 0; yy < q.h; ++yy) {
        int dx = 0;
        int dy = -1;
        if (yy < rad)              dy = rad - 1 - yy;
        else if (yy >= q.h - rad)  dy = yy - (q.h - rad);
        if (dy >= 0)
            dx = rad - (int)floor(sqrt((double)(rad * rad - dy * dy)));
        SDL_Rect ln{ q.x + dx, q.y + yy, q.w - 2 * dx, 1 };
        SDL_RenderFillRect(r, &ln);
    }
}

// Ring / annulus (scanline circle outline, `thick` px wide) -- the "+" enclosure.
static void stroke_ring(SDL_Renderer* r, int cx, int cy, int rad, int thick, Color c)
{
    if (rad < 1) return;
    if (thick < 1) thick = 1;
    int inner = rad - thick; if (inner < 0) inner = 0;
    set_color(r, c);
    for (int dy = -rad; dy <= rad; ++dy) {
        int oq = rad * rad - dy * dy;
        if (oq < 0) continue;
        int outer = (int)floor(sqrt((double)oq));
        int in = 0;
        if (dy > -inner && dy < inner) {          // |dy| < inner -> leave a hole
            int iq = inner * inner - dy * dy;
            in = iq > 0 ? (int)floor(sqrt((double)iq)) : 0;
        }
        int y  = cy + dy;
        int lw = outer - in + 1;
        SDL_Rect lft{ cx - outer, y, lw, 1 };  SDL_RenderFillRect(r, &lft);
        SDL_Rect rgt{ cx + in,    y, lw, 1 };  SDL_RenderFillRect(r, &rgt);
    }
}

// Small utilitarian push-button (ports perfnames::draw_button).
static void draw_button(App& app, SDL_Rect q, const char* label, bool engaged)
{
    const Theme& t = theme();
    fill_rect(app.ren, q, engaged ? t.accent : t.panel);
    frame_rect(app.ren, q, engaged ? t.accent : t.dim);
    app.mono.draw_centered(app.ren, q, label, engaged ? t.bg : t.text);
}

// Loop-offset of the trigger covering `tick` (so a drag-copy keeps the pattern
// phase).  Scans the trigger list read-only; returns 0 if none found.
static long trigger_offset_at(sequence* s, long tick)
{
    if (!s) return 0;
    s->reset_draw_trigger_marker();
    long on, off, offs; bool sel;
    while (s->get_next_trigger(&on, &off, &sel, &offs))
        if (tick >= on && tick <= off) return offs;
    return 0;
}

//  Slide a clip's CONTENT under it by `delta` ticks, leaving the block where
//  it is: the trigger at `start` is re-seated with offset + delta.  A trigger's
//  offset has no direct setter (sequence::set_trigger_offset is the playback
//  member, not the stored trigger), so the trigger is re-added -- which is
//  exactly what add_trigger() is for, and it cannot disturb a neighbour: the
//  rebuilt trigger occupies the identical span, and triggers never overlap.
//
//  LOOPING clips fold the new offset into their repetition (adjust = true,
//  the same fold play_span uses).  A ONE-SHOT must NOT fold: its offset is a
//  trim-IN into data bounded by the END marker, so wrapping it would replay
//  material the clip is already past.  It is only kept in [0, length].
static void shift_clip_content(sequence* s, long start, long delta)
{
    if (!s || delta == 0) return;
    s->reset_draw_trigger_marker();
    long on, off, offs; bool sel;
    while (s->get_next_trigger(&on, &off, &sel, &offs)) {
        if (on != start) continue;
        const bool looping = s->get_loop_enabled();
        long noff = offs + delta;
        if (!looping) {
            const long len = s->get_length();
            if (noff < 0)   noff = 0;
            if (len > 0 && noff > len) noff = len;   // == past the data: silent
        }
        s->add_trigger(on, off - on + 1, noff, looping);
        s->select_trigger(on);        // add_trigger seats it unselected, and the
        return;                       // gesture reads "the selected trigger"
    }
}

static bool rect_intersects(SDL_Rect a, SDL_Rect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// Bright, saturated clip body colour (auto-assigned per clip; the waveform /
// notes inside are drawn SOLID BLACK for contrast).  A per-clip override in
// m_clipColor wins; otherwise the seq index is spread across the palette.
ui::Color ArrangeView::clip_color(int seq) const
{
    static const unsigned char P[][3] = {
        {  80, 200, 255 }, { 255, 120, 200 }, { 170, 255,  80 }, { 255, 200,  60 },
        { 160, 140, 255 }, {  70, 255, 180 }, { 255, 140,  90 }, { 120, 220, 255 },
        { 255,  90, 130 }, { 200, 255, 110 }, { 110, 255, 240 }, { 255, 170, 220 },
    };
    int idx;
    std::map<int,int>::const_iterator it = m_clipColor.find(seq);
    if (it != m_clipColor.end()) idx = ((it->second % 12) + 12) % 12;
    else idx = (int)( ( (unsigned)seq * 5u + 3u ) % 12u );   // spread hues
    ui::Color c; c.r = P[idx][0]; c.g = P[idx][1]; c.b = P[idx][2]; c.a = 255;
    return c;
}

//----------------------------------------------------------------------------
//  construction
//----------------------------------------------------------------------------
ArrangeView::ArrangeView(perform* p)
    : m_perf(p),
      m_scale_x(c_perf_scale_x),          // 32 ticks / pixel
      m_scroll_ticks(0),
      m_v_offset(0),
      m_snap(c_ppqn),                      // 1 beat
      m_measure_len(c_ppqn * 4),           // 4/4 bar
      m_beat_len(c_ppqn)
{
    m_solo.assign(c_max_sequence, 0);
    m_mute_snapshot.assign(c_max_sequence, 0);
    m_solo_snapped.assign(c_max_sequence, 0);
    load_fade_prefs();       // ch.32 default shapes, AutoFades, presets 1-5
}

//----------------------------------------------------------------------------
//  coordinate helpers
//----------------------------------------------------------------------------
std::vector<int> ArrangeView::active_list() const
{
    std::vector<int> v;
    if (!m_perf) return v;
    // Membership by lookup, not by scanning what we have collected so far.
    // The std::find made this O(active^2), and row_top(), row_at() and
    // visible_rows() each call active_list() again -- so the cost was paid
    // several times per frame and grew quadratically with lane count.
    // NOTE: lane_key() is NOT a sequence index -- on_track_key() returns
    // 100000 + trackIndex for mixer-mapped tracks and 200000 + laneId for
    // automation lanes.  Indexing a c_max_sequence-sized array by it (an
    // earlier version of this function) folded every mapped track into one
    // bucket, so the arrange view showed a single lane no matter how many
    // tracks existed.  The key space is sparse: hash it.
    std::unordered_set<int> seen;
    seen.reserve(64);
    for (int i = 0; i < c_max_sequence; ++i) {
        if (!m_perf->is_active(i)) continue;
        if (!seen.insert(lane_key(i)).second) continue;
        v.push_back(i);
    }
    return v;
}

int ArrangeView::lane_key(int seq) const
{
    return on_track_key ? on_track_key(seq) : seq;
}

bool ArrangeView::is_automation(int seq) const
{
    sequence* s=(m_perf&&m_perf->is_active(seq))?m_perf->get_sequence(seq):nullptr;
    return s && s->get_track_kind()==2;
}

std::vector<int> ArrangeView::lane_sequences(int lane_seq) const
{
    std::vector<int> out;
    if (!m_perf || !m_perf->is_active(lane_seq)) return out;
    int key = lane_key(lane_seq);
    for (int seq = 0; seq < c_max_sequence; ++seq)
        if (m_perf->is_active(seq) && lane_key(seq) == key)
            out.push_back(seq);
    return out;
}

std::vector<std::vector<int>> ArrangeView::lane_groups(const std::vector<int>& act) const
{
    std::vector<std::vector<int>> out(act.size());
    if (!m_perf) return out;
    // key -> bucket index, then one walk of the sequence table.  lane_key() is
    // NOT a sequence index: on_track_key() returns 100000 + trackIndex for
    // mixer-mapped tracks and 200000 + laneId for automation lanes, so the key
    // space is sparse and must be hashed, never used as an array subscript.
    // (Bucketing it into a c_max_sequence-sized array dropped every mapped
    // track's clips out of the lane groups entirely.)
    std::unordered_map<int,int> keyToIdx;
    keyToIdx.reserve(act.size() * 2 + 8);
    for (size_t i = 0; i < act.size(); ++i) keyToIdx[lane_key(act[i])] = (int)i;
    for (int seq = 0; seq < c_max_sequence; ++seq) {
        if (!m_perf->is_active(seq)) continue;
        std::unordered_map<int,int>::const_iterator it = keyToIdx.find(lane_key(seq));
        if (it != keyToIdx.end()) out[(size_t)it->second].push_back(seq);
    }
    return out;
}

int ArrangeView::clip_sequence_at(int lane_seq, long tick) const
{
    std::vector<int> seqs = lane_sequences(lane_seq);
    for (int i = (int)seqs.size() - 1; i >= 0; --i) {
        std::map<int,const PatchKnob::engine::AudioClip*>::const_iterator ai=m_audio.find(seqs[i]);
        if(ai!=m_audio.end()) {
            std::map<int,AudioRegion>::const_iterator ri=m_region.find(seqs[i]);
            if(ri!=m_region.end()&&tick>=ri->second.position&&
               tick<ri->second.position+ri->second.length) return seqs[i];
        } else {
            sequence* s=m_perf->get_sequence(seqs[i]);
            if(s&&s->get_trigger_state(tick)) return seqs[i];
        }
    }
    return -1;
}

// Ruler scrub position: snapped, unless ALT asks for the exact tick.
// Floor to the bar line at or before `tick` (ctrl+arrow steps bar to bar even
// when the playhead is sitting mid-bar).
long ArrangeView::snap_down_bar(long tick) const
{
    const long bar = m_measure_len > 0 ? m_measure_len : (c_ppqn * 4);
    if (tick <= 0) return 0;
    return (tick / bar) * bar;
}

long ArrangeView::scrub_tick(int px) const
{
    // Grid-constrained only when the MODE says so (Grid, or Snap To Grid in
    // Shuffle/Slip/Spot); ALT always asks for the exact tick.
    const long t = x_to_tick(px);
    return (SDL_GetModState() & KMOD_ALT) ? t : esnap(t);
}

bool ArrangeView::pt_in_rect(const SDL_Rect& r, int x, int y)
{
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// The single source of truth for a clip's tick span.  Audio clips use their
// Ardour region (half-open [position, position+length)); MIDI clips use their
// triggers, whose end tick is INCLUSIVE, so it is converted here once instead
// of at four call sites that each remembered differently.
void ArrangeView::clip_spans(int seq, std::vector<ClipSpan>& out) const
{
    if (!m_perf || !m_perf->is_active(seq)) return;
    if (m_audio.count(seq)) {
        std::map<int,AudioRegion>::const_iterator ri = m_region.find(seq);
        long pos = 0, len = 0, src = 0; bool sel = false;
        if (ri != m_region.end()) {
            pos = ri->second.position; len = ri->second.length;
            src = ri->second.source;   sel = ri->second.selected;
        } else {
            // Same lazy import region_for() does, but const: a freshly attached
            // clip drew (via region_for) yet had no hit-test rect at all.
            sequence* s = m_perf->get_sequence(seq);
            if (s) {
                s->reset_draw_trigger_marker();
                long on, off, offs; bool tsel;
                if (s->get_next_trigger(&on, &off, &tsel, &offs)) { pos = on; len = off - on + 1; }
            }
            if (len <= 0) {
                std::map<int,long>::const_iterator lit = m_audioLen.find(seq);
                len = (lit != m_audioLen.end() && lit->second > 0) ? lit->second : 1;
            }
        }
        if (len < 1) len = 1;
        ClipSpan cs; cs.seq = seq; cs.on = pos; cs.endEx = pos + len;
        cs.selected = sel; cs.offset = src;
        out.push_back(cs);
        return;
    }
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    s->reset_draw_trigger_marker();
    long on, off, offs; bool sel;
    while (s->get_next_trigger(&on, &off, &sel, &offs)) {
        if (off <= 0) continue;
        ClipSpan cs; cs.seq = seq; cs.on = on; cs.endEx = off + 1;   // trigger end is inclusive
        cs.selected = sel; cs.offset = offs;
        out.push_back(cs);
    }
}

void ArrangeView::for_each_clip(const std::function<void(const ClipSpan&)>& fn) const
{
    if (!m_perf) return;
    std::vector<ClipSpan> spans;
    for (int seq = 0; seq < c_max_sequence; ++seq) {
        if (!m_perf->is_active(seq)) continue;
        spans.clear();
        clip_spans(seq, spans);
        for (size_t i = 0; i < spans.size(); ++i) fn(spans[i]);
    }
}

void ArrangeView::for_each_visible_clip(
    const std::function<void(const ClipSpan&, const SDL_Rect&)>& fn) const
{
    if (!m_perf) return;
    std::vector<int> act = active_list();
    std::vector<ClipSpan> spans;
    for (int row = 0; row < (int)act.size(); ++row) {
        const int lane_y = row_top(row - m_v_offset);
        const int lh = track_h(act[(size_t)row]);
        if (lane_y >= canvas_y() + canvas_h() || lane_y + lh < canvas_y()) continue;
        std::vector<int> seqs = lane_sequences(act[(size_t)row]);
        for (size_t i = 0; i < seqs.size(); ++i) {
            spans.clear();
            clip_spans(seqs[i], spans);
            for (size_t k = 0; k < spans.size(); ++k)
                fn(spans[k], span_rect(spans[k], lane_y, lh));
        }
    }
}

SDL_Rect ArrangeView::span_rect(const ClipSpan& s, int lane_y, int lane_h) const
{
    const int x0 = tick_to_x(s.on);
    const int x1 = tick_to_x(s.endEx);
    return SDL_Rect{ x0, lane_y + 3, std::max(2, x1 - x0), lane_h - 6 };
}

ArrangeView::HeaderGeom ArrangeView::header_geom() const
{
    HeaderGeom g;
    g.spine_w = 4;
    g.badge_x = g.spine_w + 6;
    g.badge_w = 22;
    g.name_x  = g.badge_x + g.badge_w + 6;
    g.rm_w = 14; g.rm_h = 14;
    g.rm_x = header_w - g.rm_w - 4;
    g.rm_y = 4;
    g.btn_w = 22; g.btn_h = 12;
    g.btn_x = g.rm_x - g.btn_w - 4;
    g.vu_w = 14;
    g.vu_x = g.btn_x - g.vu_w - 6;
    return g;
}

int ArrangeView::tick_to_x(long tick) const
{
    return canvas_x() + (int)((double)(tick - m_scroll_ticks) / m_scale_x);
}

long ArrangeView::x_to_tick(int px) const
{
    long t = m_scroll_ticks + (long)((double)(px - canvas_x()) * m_scale_x);
    return t < 0 ? 0 : t;
}

// Snap through the SHARED quantize algorithm (src/quantize.h), which rounds to
// the NEAREST line.  This used to floor, so every drag landed up to a full grid
// unit EARLIER than where it was dropped -- and it disagreed with the record
// quantiser and the piano roll, which the project had already standardised on
// one implementation to stop exactly this class of mismatch.  Flooring was also
// wrong for negative ticks, where C++ '%' truncates toward zero.
long ArrangeView::snap(long tick) const
{
    // Clips/Markers grid (ch.29 p642): free placement that snaps to clip
    // starts/ends, the timeline markers and edit-selection bounds when near.
    if (m_grid_scale == 3) return magnet_snap(tick);
    if (m_snap <= 0) return tick;
    PatchKnob::quantize::Params p;
    p.grid = m_snap;
    return PatchKnob::quantize::apply(tick, p);
}

// Effective lane height (keyed by LANE, so every clip sequence sharing a lane
// resizes together), per-track override else the default.
int ArrangeView::track_h(int seq) const
{
    std::map<int,int>::const_iterator it = m_trackH.find(lane_key(seq));
    int h = (it != m_trackH.end() && it->second > 0) ? it->second : row_h;
    if (h < 20) h = 20; if (h > 400) h = 400;
    return h;
}

// Screen-y top of on-screen row r (cumulative over the variable lane heights).
int ArrangeView::row_top(int r) const
{
    std::vector<int> act = active_list();
    int y = canvas_y();
    //  `r` is a SCREEN row: lane index minus m_v_offset, so it goes NEGATIVE
    //  for every lane scrolled off the top.  The loop below used to run zero
    //  times for those and hand back canvas_y() -- the top of the canvas --
    //  which parked every hidden lane on top of the first visible one.  So the
    //  rubber band, whose hit test walks these rects, selected clips on lanes
    //  that were scrolled out of sight (and clip_rect_of / the context menu
    //  anchored to the wrong row for the same reason).  Walk upwards instead.
    if (r >= 0) {
        for (int k = 0; k < r; ++k) {
            int idx = m_v_offset + k;
            y += (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        }
    } else {
        for (int k = -1; k >= r; --k) {
            int idx = m_v_offset + k;
            y -= (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        }
    }
    return y;
}

int ArrangeView::row_at(int py) const
{
    if (py < canvas_y()) return -1;
    std::vector<int> act = active_list();
    int y = canvas_y();
    const int bottom = canvas_y() + canvas_h() + 400;
    for (int r = 0; y < bottom; ++r) {
        int idx = m_v_offset + r;
        int h = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        if (py < y + h) return r;
        y += h;
    }
    return -1;
}

// Last tick the arrangement occupies, over BOTH models: seq24 triggers and the
// Ardour audio regions.  get_max_trigger() alone reported 0 for an audio-only
// project, which broke fit, the scrollbar range and End.
long ArrangeView::song_end() const
{
    long last = m_perf ? m_perf->get_max_trigger() : 0;
    for (std::map<int,AudioRegion>::const_iterator it = m_region.begin();
         it != m_region.end(); ++it) {
        if (m_perf && !m_perf->is_active(it->first)) continue;
        const long e = it->second.position + it->second.length;
        if (e > last) last = e;
    }
    return last;
}

void ArrangeView::clamp_scroll()
{
    const long view  = (long)((double)canvas_w() * m_scale_x);
    const long total = std::max<long>(m_measure_len, song_end() + m_measure_len);
    long maxScroll = total - view;
    if (maxScroll < 0) maxScroll = 0;
    if (m_scroll_ticks > maxScroll) m_scroll_ticks = maxScroll;
    if (m_scroll_ticks < 0)         m_scroll_ticks = 0;
}

// Rows that actually fit, summing the real per-lane heights.
int ArrangeView::visible_rows() const
{
    std::vector<int> act = active_list();
    const int ch = canvas_h();
    int y = 0, n = 0;
    for (int i = m_v_offset; i >= 0 && i < (int)act.size() && y < ch; ++i) {
        y += track_h(act[(size_t)i]);
        ++n;
    }
    return n < 1 ? 1 : n;
}

// Largest top-row index that still fills the canvas from the bottom.
int ArrangeView::max_v_offset() const
{
    std::vector<int> act = active_list();
    const int ch = canvas_h();
    int y = 0, off = (int)act.size();
    for (int i = (int)act.size() - 1; i >= 0; --i) {
        y += track_h(act[(size_t)i]);
        if (y > ch) break;
        off = i;
    }
    if (off < 0) off = 0;
    if (off > (int)act.size()) off = (int)act.size();
    return off;
}

long ArrangeView::playhead() const
{
    return m_perf ? m_perf->get_tick() : 0;
}

// Move the transport.  The shell's on_seek updates perform AND the audio engine
// locate, so this is how the playhead moves while stopped.
void ArrangeView::seek_to(long tick)
{
    if (tick < 0) tick = 0;
    if (on_seek) on_seek(tick);
}

//----------------------------------------------------------------------------
//  snap cycle + zoom-to-fit (Qtractor-inspired)
//----------------------------------------------------------------------------
long ArrangeView::snap_value(int idx) const
{
    switch (idx) {
    case 0:  return m_measure_len;        // BAR
    case 1:  return m_measure_len / 2;    // 1/2
    case 2:  return m_beat_len;           // 1/4  (one beat)
    case 3:  return m_beat_len / 2;       // 1/8
    case 4:  return m_beat_len / 4;       // 1/16
    case 5:  return m_beat_len / 8;       // 1/32
    case 6:  return m_beat_len / 16;      // 1/64
    case 7:  return m_beat_len / 32;      // 1/128
    default: return 0;                    // OFF (free)
    }
}

const char* ArrangeView::snap_label(int idx) const
{
    static const char* L[] = { "BAR", "1/2", "1/4", "1/8", "1/16",
                               "1/32", "1/64", "1/128", "OFF" };
    if (idx < 0 || idx > 8) idx = 8;
    return L[idx];
}

//----------------------------------------------------------------------------
//  adaptive metric grid
//
//  Smallest power-of-two multiplier n (1, 2, 4, 8, ...) that makes n units at
//  least `minPx` wide.  Everything the ruler draws is thinned with this, so a
//  division either has room for itself (and its label) or is not drawn at all.
//----------------------------------------------------------------------------
static long pow2_stride(double unitPx, double minPx)
{
    if (unitPx <= 0.0) return 1L << 28;
    long n = 1;
    while ((double)n * unitPx < minPx && n < (1L << 27)) n <<= 1;
    return n;
}

// Bar lines / bar NUMBERS / beats / snap subdivisions for the current zoom.
// Bar numbers use their OWN (coarser or equal) stride, computed from how wide
// the widest visible number actually is, so zooming out drops them 1 -> 2 -> 4
// -> 8 -> 16 bars instead of overprinting, and zooming in brings them back.
ArrangeView::Metric ArrangeView::metric(int cw) const
{
    Metric m;
    if (cw < 1) cw = 6;
    const long bar = m_measure_len > 0 ? m_measure_len : (c_ppqn * 4);
    const double barPx = (double)bar / (m_scale_x > 0.0 ? m_scale_x : 1.0);

    // Width of the widest bar number that can appear, plus breathing room.
    long lastBar = x_to_tick(canvas_x() + canvas_w()) / bar + 2;
    int digits = 1;
    for (long v = lastBar; v >= 10; v /= 10) ++digits;
    const double labelPx = (double)digits * cw + (double)cw * 2.0;

    // `bar * stride` must stay inside a long (32-bit under LLP64).  Unclamped,
    // an extreme zoom-out overflowed it to 0 or negative -- and barStep is then
    // both a DIVISOR and a loop increment, so the grid divided by zero and the
    // ruler/canvas loops never terminated.
    const long maxN   = std::max<long>(1, 0x3FFFFFFFL / bar);
    const long barN   = std::min(maxN, pow2_stride(barPx, 7.0));   // >= 7 px apart
    const long labelN = std::min(maxN, std::max(barN, pow2_stride(barPx, labelPx)));
    m.barStep   = bar * barN;
    m.labelStep = bar * labelN;
    if (m.barStep   < 1) m.barStep   = bar;
    if (m.labelStep < m.barStep) m.labelStep = m.barStep;

    // Beats and snap subdivisions only once EVERY bar is already drawn: below
    // that the bars themselves are the finest readable division.
    const double beatPx = (double)m_beat_len / m_scale_x;
    m.beatStep = (barN == 1 && beatPx >= 6.0) ? m_beat_len : 0;
    const long sub = m_snap > 0 ? m_snap : m_beat_len;
    m.subStep = (barN == 1 && sub < m_beat_len && (double)sub / m_scale_x >= 6.0)
                ? sub : 0;
    return m;
}

// "bar.beat.tick" -- the position readout every DAW ruler carries.
std::string ArrangeView::bbt(long tick) const
{
    if (tick < 0) tick = 0;
    const long bar  = tick / m_measure_len + 1;
    const long beat = (tick % m_measure_len) / m_beat_len + 1;
    const long sub  = tick % m_beat_len;
    char b[48];
    std::snprintf(b, sizeof(b), "%ld.%ld.%03ld", bar, beat, sub);
    return b;
}

// A DURATION in bars.beats (so a drag readout says "2.0" not "3840 ticks").
std::string ArrangeView::bars_len(long ticks) const
{
    if (ticks < 0) ticks = 0;
    char b[48];
    std::snprintf(b, sizeof(b), "%ld.%ld", ticks / m_measure_len,
                  (ticks % m_measure_len) / m_beat_len);
    return b;
}

// Wall clock at the project tempo, for the transport readout badge.
std::string ArrangeView::time_str(long tick) const
{
    double bpm = m_perf ? m_perf->get_bpm() : 120.0;
    if (bpm < 1.0) bpm = 120.0;
    double sec = (double)std::max<long>(0, tick) / (double)c_ppqn * 60.0 / bpm;
    const int mins = (int)(sec / 60.0);
    sec -= mins * 60.0;
    char b[48];
    std::snprintf(b, sizeof(b), "%d:%06.3f", mins, sec);
    return b;
}

// Fit the whole arrangement (0 .. last trigger end) to the canvas width.
void ArrangeView::zoom_to_fit()
{
    if (!m_perf) return;
    long last = song_end();                          // triggers AND audio regions
    if (last < m_measure_len) last = m_measure_len;  // show at least one bar
    int cw = canvas_w();
    if (cw < 16) cw = 16;
    double sx = (double)last * 1.04 / (double)cw;    // +4% right margin
    if (sx < kZoomMin) sx = kZoomMin;
    if (sx > kZoomMax) sx = kZoomMax;
    m_scale_x = sx;
    m_scroll_ticks = 0;
}

// Shift+F : frame the current SELECTION instead of the whole song.  Falls back
// to the loop range, then to fit-song, so the key always does something useful.
void ArrangeView::zoom_to_selection()
{
    if (!m_perf) return;
    // The ch.30 EDIT selection wins; clip selections and the loop range are
    // the fallbacks.
    if (m_sel_start >= 0 && m_sel_end > m_sel_start) {
        const long span = m_sel_end - m_sel_start;
        int cw = canvas_w(); if (cw < 16) cw = 16;
        double sx = (double)span * 1.12 / (double)cw;
        if (sx < kZoomMin) sx = kZoomMin;
        if (sx > kZoomMax) sx = kZoomMax;
        remember_zoom();
        m_scale_x = sx;
        m_scroll_ticks = std::max<long>(0, m_sel_start - (long)(span * 0.06));
        return;
    }
    long lo = -1, hi = -1;
    for (int seq = 0; seq < c_max_sequence; ++seq) {
        if (!m_perf->is_active(seq)) continue;
        std::map<int,AudioRegion>::const_iterator ri = m_region.find(seq);
        if (m_audio.count(seq) && ri != m_region.end()) {
            if (!ri->second.selected) continue;
            if (lo < 0 || ri->second.position < lo) lo = ri->second.position;
            if (ri->second.position + ri->second.length > hi)
                hi = ri->second.position + ri->second.length;
            continue;
        }
        sequence* s = m_perf->get_sequence(seq);
        if (!s) continue;
        s->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        while (s->get_next_trigger(&on, &off, &sel, &offs)) {
            if (!sel) continue;
            if (lo < 0 || on < lo) lo = on;
            if (off + 1 > hi) hi = off + 1;
        }
    }
    if (lo < 0 || hi <= lo) {                      // nothing selected -> loop range
        lo = m_perf->get_left_tick();
        hi = m_perf->get_right_tick();
    }
    if (lo < 0 || hi <= lo) { zoom_to_fit(); return; }
    const long span = hi - lo;
    int cw = canvas_w(); if (cw < 16) cw = 16;
    double sx = (double)span * 1.12 / (double)cw;   // 12% margin around it
    if (sx < kZoomMin) sx = kZoomMin;
    if (sx > kZoomMax) sx = kZoomMax;
    m_scale_x = sx;
    m_scroll_ticks = std::max<long>(0, lo - (long)(span * 0.06));
}

//----------------------------------------------------------------------------
//  edit-tool palette : vector glyphs, a hover tooltip and a right-click picker
//
//  The four modes used to be spelled out as text buttons ("GRAB RANGE CUT
//  DRAW"), which ate a third of the ruler and still needed reading.  They are
//  now icons -- an open hand, a range bracket, scissors and a pencil -- drawn
//  as line/rect strokes so they stay crisp in both themes and at any UI scale.
//----------------------------------------------------------------------------
const char* ArrangeView::tool_name(int tool) const
{
    static const char* N[8] = { "Grabber", "Selector", "Cut", "Pencil",
                                "Zoomer", "Trim", "Scrubber", "Smart" };
    return N[(tool < 0 || tool > 7) ? 0 : tool];
}

const char* ArrangeView::tool_key(int tool) const
{
    static const char* K[8] = { "F8", "F7", "C", "F10", "F5", "F6", "F9", "F6+F7" };
    return K[(tool < 0 || tool > 7) ? 0 : tool];
}

void ArrangeView::draw_tool_icon(App& app, SDL_Rect box, int tool, Color c)
{
    SDL_Renderer* r = app.ren;
    const int cx = box.x + box.w / 2;
    const int cy = box.y + box.h / 2;
    set_color(r, c);
    switch (tool) {
    case 0:                                   // GRAB : an open hand
        fill_rect(r, SDL_Rect{ cx - 4, cy, 8, 5 }, c);            // palm
        for (int i = 0; i < 3; ++i)                                // fingers
            fill_rect(r, SDL_Rect{ cx - 4 + i * 3, cy - 5, 2, 6 }, c);
        fill_rect(r, SDL_Rect{ cx + 4, cy - 2, 2, 4 }, c);         // thumb
        break;
    case 1:                                   // RANGE : |<-->| brackets
        vline(r, cx - 6, cy - 5, cy + 6, c);
        vline(r, cx + 6, cy - 5, cy + 6, c);
        hline(r, cx - 5, cx + 6, cy, c);
        SDL_RenderDrawLine(r, cx - 5, cy, cx - 2, cy - 3);
        SDL_RenderDrawLine(r, cx - 5, cy, cx - 2, cy + 3);
        SDL_RenderDrawLine(r, cx + 5, cy, cx + 2, cy - 3);
        SDL_RenderDrawLine(r, cx + 5, cy, cx + 2, cy + 3);
        break;
    case 2:                                   // CUT : scissors
        SDL_RenderDrawLine(r, cx - 4, cy - 6, cx + 3, cy + 3);
        SDL_RenderDrawLine(r, cx - 3, cy - 6, cx + 4, cy + 3);
        SDL_RenderDrawLine(r, cx + 4, cy - 6, cx - 3, cy + 3);
        SDL_RenderDrawLine(r, cx + 3, cy - 6, cx - 4, cy + 3);
        stroke_ring(r, cx - 4, cy + 5, 2, 1, c);                   // finger rings
        stroke_ring(r, cx + 4, cy + 5, 2, 1, c);
        break;
    case 3:                                   // DRAW / PENCIL
        SDL_RenderDrawLine(r, cx - 5, cy + 4, cx + 3, cy - 5);     // body
        SDL_RenderDrawLine(r, cx - 3, cy + 6, cx + 5, cy - 3);
        SDL_RenderDrawLine(r, cx + 3, cy - 5, cx + 5, cy - 3);     // cap
        SDL_RenderDrawLine(r, cx - 5, cy + 4, cx - 3, cy + 6);     // ferrule
        fill_rect(r, SDL_Rect{ cx - 7, cy + 5, 3, 3 }, c);         // graphite point
        break;
    case 4:                                   // ZOOMER : magnifier
        stroke_ring(r, cx - 1, cy - 2, 4, 1, c);
        SDL_RenderDrawLine(r, cx + 2, cy + 1, cx + 5, cy + 5);
        SDL_RenderDrawLine(r, cx + 3, cy + 1, cx + 6, cy + 5);
        break;
    case 5:                                   // TRIM : edge bracket + arrow
        vline(r, cx - 5, cy - 6, cy + 6, c);
        vline(r, cx - 4, cy - 6, cy + 6, c);
        hline(r, cx - 4, cx + 5, cy, c);
        SDL_RenderDrawLine(r, cx + 4, cy, cx + 1, cy - 3);
        SDL_RenderDrawLine(r, cx + 4, cy, cx + 1, cy + 3);
        break;
    case 6:                                   // SCRUBBER : speaker + waves
        fill_rect(r, SDL_Rect{ cx - 6, cy - 2, 3, 5 }, c);          // driver
        SDL_RenderDrawLine(r, cx - 3, cy - 2, cx - 1, cy - 5);      // cone
        SDL_RenderDrawLine(r, cx - 3, cy + 2, cx - 1, cy + 5);
        vline(r, cx - 1, cy - 5, cy + 5, c);
        SDL_RenderDrawLine(r, cx + 2, cy - 3, cx + 2, cy + 3);      // waves
        SDL_RenderDrawLine(r, cx + 5, cy - 5, cx + 5, cy + 5);
        break;
    default:                                  // SMART : sel/grab/trim triad
        vline(r, cx - 6, cy - 5, cy + 6, c);                        // trim edge
        hline(r, cx - 6, cx - 2, cy - 5, c);
        hline(r, cx - 6, cx - 2, cy + 5, c);
        vline(r, cx, cy - 5, cy + 1, c);                            // I-beam top
        hline(r, cx - 2, cx + 3, cy - 5, c);
        fill_rect(r, SDL_Rect{ cx + 2, cy + 1, 5, 4 }, c);          // hand bottom
        break;
    }
}

void ArrangeView::draw_toolmenu(App& app)
{
    if (!m_toolmenu_open) return;
    const Theme& t = theme();
    const int rowh = std::max(22, app.font.ch() + 10);
    SDL_Rect box = clamp_popup_rect(SDL_Rect{ m_toolmenu_rect.x, m_toolmenu_rect.y,
                                              150, kToolCount * rowh + 2 }, rect);
    m_toolmenu_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    for (int i = 0; i < kToolCount; ++i) {
        SDL_Rect row{ box.x + 1, box.y + i * rowh + 1, box.w - 2, rowh - 1 };
        const bool hot = (m_mx >= row.x && m_mx < row.x + row.w &&
                          m_my >= row.y && m_my < row.y + row.h);
        const bool on  = (int)m_edit_tool == i;
        if (hot || on) fill_rect(app.ren, row, hot ? t.accent : t.keybg);
        const Color fg = hot ? t.bg : (on ? t.hi : t.text);
        draw_tool_icon(app, SDL_Rect{ row.x + 4, row.y, 20, row.h }, i, fg);
        app.font.draw(app.ren, row.x + 28, row.y + (row.h - app.font.ch()) / 2,
                      tool_name(i), fg);
        app.mono.draw(app.ren, row.x + row.w - app.mono.cw() - 6,
                      row.y + (row.h - app.mono.ch()) / 2, tool_key(i),
                      hot ? t.bg : t.dim);
    }
}

bool ArrangeView::toolmenu_click(App& app, int mx, int my)
{
    const int rowh = std::max(22, app.font.ch() + 10);
    const SDL_Rect b = m_toolmenu_rect;
    if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + kToolCount * rowh) {
        const int idx = (my - b.y) / rowh;
        if (idx >= 0 && idx < kToolCount) select_tool((EditTool)idx, false);
    }
    m_toolmenu_open = false;                       // click (in or out) closes it
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  one-frame tooltip.  Hover tests call tip() while drawing; draw() paints the
//  result last so it floats over everything, then clears it for the next frame.
//----------------------------------------------------------------------------
void ArrangeView::tip(const std::string& s, int x, int y)
{
    m_tip = s; m_tip_x = x; m_tip_y = y;
}

void ArrangeView::draw_tooltip(App& app)
{
    if (m_tip.empty()) return;
    const Theme& t = theme();
    const int cw = app.mono.cw() ? app.mono.cw() : 6;
    (void)cw;
    SDL_Rect b = clamp_popup_rect(
        SDL_Rect{ m_tip_x, m_tip_y, app.mono.text_w(m_tip) + 10,
                  app.mono.ch() + 6 }, rect);
    fill_rect (app.ren, b, t.panel);
    frame_rect(app.ren, b, t.accent);
    app.mono.draw(app.ren, b.x + 5, b.y + 3, m_tip, t.text);
    m_tip.clear();
}

//----------------------------------------------------------------------------
//  keyboard-shortcut overlay ("?" or F1, and the "?" button on the tool strip)
//----------------------------------------------------------------------------
void ArrangeView::draw_help(App& app)
{
    if (!m_help_open) return;
    static const char* kHelp[] = {
        "ARRANGE  --  KEYS & GESTURES",
        "",
        "MODES      F1 shuffle  F2 slip  F3 spot  F4 grid   ` cycles",
        "           shift+F4        snap-to-grid in shuffle/slip/spot",
        "           shift+F1..F3    that mode + snap-to-grid",
        "           ctrl-click SHUFFLE   shuffle lock (from another mode)",
        "           right-click GRID     absolute / relative grid",
        "           shift+= / -     larger / smaller grid value",
        "",
        "TOOLS      F5 zoomer  F6 trim  F7 selector  F8 grabber",
        "           F9 scrubber  F10 pencil  F6+F7 smart   Esc cycles",
        "           repeat F-key / click = cycle tool modes (hold: menu)",
        "           trim: standard/TCE/scrub/loop  grabber: time/sep/object",
        "           zoomer: click in, alt-click out, drag range,",
        "           ctrl+drag continuous, dbl-click icon = fit session,",
        "           ctrl-click icon = 256 smp/px overview",
        "",
        "ZOOM       wheel h-zoom   shift+wheel pan   alt+wheel lanes",
        "           alt+shift+wheel audio v-zoom  alt+ctrl+wheel midi",
        "           ctrl+wheel lane height   + / - buttons drag=continuous",
        "           F fit  shift+F fit selection  Z previous zoom",
        "           presets 1-5: click recall (ctrl+1..5), shift/hold store",
        "           E zoom toggle  alt+shift+E cancel  alt-click Z<> clears",
        "",
        "SELECT     selector: drag range (vertical = more tracks),",
        "           dbl-click clip, triple-click track, shift-click extend",
        "           shift+/ link timeline<>edit    shift+T link track",
        "           tab / ctrl+tab   next / prev transient or boundary",
        "           shift+tab extends   ctrl+alt+tab  toggles transients",
        "           numpad + / -  nudge selection (alt=start ctrl=end)",
        "           P / ;  selection up / down a track (shift extends,",
        "           alt removes top/bottom)   ctrl+alt+Z restore last",
        "           ctrl+shift+' double   ctrl+shift+L halve",
        "           ctrl+shift+E duplicate+extend   alt+left/right move",
        "           alt+shift+5/6  timeline=edit / edit=timeline",
        "           down/up during playback mark selection start/end",
        "           type in MAIN/ST/EN/LEN fields: . next subfield,",
        "           / next field, +/- calculator, enter commits",
        "",
        "MOVE       arrows pan/lanes   ctrl+left/right playhead by bar",
        "           home/end  song start/end   drag ruler scrubs",
        "           ctrl+numpad 0-9 shuttle lock (5=1x 9=custom 0=stop)",
        "           alt+7 universe view (drag frame, drag edge resizes)",
        "           alt+up/down lane heights  ctrl+alt+up fit to window",
        "",
        "EDIT       drag move   edges trim   ctrl+drag duplicate",
        "           alt+drag slip (audio)   shift+drag marquee",
        "           dbl-click open editor   middle-click split",
        "           X split at playhead   S cycle snap   delete removes",
        "           ctrl+X/C/V cut/copy/paste (range or clips)  ctrl+B clear",
        "           ctrl+del clear all data   ctrl+A all   ctrl+D dup",
        "           ctrl+Z/Y multi-undo/redo   U undo history window",
        "           ctrl+R capture   ctrl+E separate   ctrl+H heal",
        "           ctrl+T trim to selection   alt+shift+7/8 trim to insert",
        "           ctrl+0 quantize to grid   alt+shift+3 consolidate",
        "           ctrl+shift+R rename clip   ctrl+alt+1..5 rate clips",
        "           clips selected: numpad +/- nudges them (alt=trim start,",
        "           ctrl=trim end, shift=slide contents)  / and M next-size",
        "           shift+alt+= / - step the nudge value",
        "           pencil on audio: sample-zoom repairs DESTRUCTIVELY",
        "",
        "FADES      ctrl+F create from selection (dialog; whole clips =",
        "           batch fades)   ctrl+alt+F / ctrl+win+F default fades",
        "           win+D fade to start   win+G fade to end",
        "           alt+win+left/right cycle shape (std/S/preset 1-7)",
        "           grabber-click a fade: select it -- drag/numpad moves,",
        "           delete removes   smart tool: corners pull fades,",
        "           bottom seam drags a crossfade, middle reshapes",
        "           right-click clip > Fades...: shape/slope/batch/prefs",
        "",
        "VIEW       T theme   L follow   / or ? this help",
        "           EDIT/VIEW chips: command menu, waveform+clip options",
        "           LAY chip: layered editing (overlaps kept vs trimmed)",
    };
    const int n = (int)(sizeof(kHelp) / sizeof(kHelp[0]));
    const Theme& t = theme();
    const int cw = app.mono.cw() ? app.mono.cw() : 6;
    const int lh = app.mono.ch() + 2;
    (void)cw;
    int w = 0;
    for (int i = 0; i < n; ++i)
        w = std::max(w, app.mono.text_w(kHelp[i]));
    SDL_Rect box{ 0, 0, w + 32, n * lh + 24 };
    box.x = rect.x + (rect.w - box.w) / 2;
    box.y = rect.y + (rect.h - box.h) / 2;
    box = clamp_popup_rect(box, rect);

    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
    Color veil = t.bg; veil.a = 190;
    fill_rect(app.ren, rect, veil);
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    for (int i = 0; i < n; ++i) {
        const int y = box.y + 12 + i * lh;
        if (y + lh > box.y + box.h) break;
        app.mono.draw(app.ren, box.x + 16, y, kHelp[i],
                      i == 0 ? t.hi : (kHelp[i][0] && kHelp[i][0] != ' ' ? t.accent : t.text));
    }
}

//----------------------------------------------------------------------------
//  inline clip rename : edit the underlying sequence name in place
//----------------------------------------------------------------------------
void ArrangeView::begin_rename(App& app, int seq, SDL_Rect clip_rect)
{
    if (!m_perf || !m_perf->is_active(seq)) return;
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    const char* nm = s->get_name();
    m_edit_name = nm ? nm : "";
    m_edit_seq  = seq;
    m_edit_rect = clip_rect;
    app.begin_text(&m_edit_name, nullptr, [this, &app](bool ok) {
        if (ok && m_perf && m_edit_seq >= 0 && m_perf->is_active(m_edit_seq)) {
            sequence* sq = m_perf->get_sequence(m_edit_seq);
            if (sq) sq->set_name(m_edit_name);        // clip name == pattern name
        }
        m_edit_seq = -1;
        app.request_redraw();
    });
    app.request_redraw();
}

void ArrangeView::draw_rename(App& app)
{
    if (!app.editing_text() || m_edit_seq < 0) return;
    const Theme& t = theme();
    SDL_Rect box = m_edit_rect;
    if (box.w < 60) box.w = 60;
    if (box.h < app.mono.ch() + 4) box.h = app.mono.ch() + 4;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.hi);
    int tx = box.x + 3;
    int ty = box.y + (box.h - app.mono.ch()) / 2;
    // Trim from the LEFT by measured width so the caret stays in view; the old
    // character-count estimate cut the wrong number of glyphs at any UI scale.
    std::string vis = m_edit_name;
    while (!vis.empty() && app.mono.text_w(vis) > box.w - 8)
        vis.erase(vis.begin());
    app.mono.draw(app.ren, tx, ty, vis, t.text);
    int cx = tx + app.mono.text_w(vis);               // caret
    vline(app.ren, cx, ty, ty + app.mono.ch(), t.hi);
}

//----------------------------------------------------------------------------
//  shaded L..R loop span drawn across the lanes (edit-range readout)
//----------------------------------------------------------------------------
void ArrangeView::draw_loop_band(App& app)
{
    if (!m_perf) return;
    const Theme& t = theme();
    long left  = m_perf->get_left_tick();
    long right = m_perf->get_right_tick();
    if (right <= left) return;
    int cvx = canvas_x(), cvr = canvas_x() + canvas_w();
    int lx = tick_to_x(left), rx = tick_to_x(right);
    if (rx < cvx || lx > cvr) return;
    int bx = lx < cvx ? cvx : lx;
    int br = rx > cvr ? cvr : rx;
    if (br <= bx) return;
    // subtle two-tone band: accent hue at low alpha (blended), same colour role.
    Color band = t.accent; band.a = 28;
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
    fill_rect(app.ren, SDL_Rect{ bx, canvas_y(), br - bx, canvas_h() }, band);
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    if (lx >= cvx && lx <= cvr) vline(app.ren, lx, canvas_y(), canvas_y() + canvas_h(), t.accent);
    if (rx >= cvx && rx <= cvr) vline(app.ren, rx, canvas_y(), canvas_y() + canvas_h(), t.accent);
}

//----------------------------------------------------------------------------
//  drag feedback : is a clip gesture running, auto-scroll at the canvas edge,
//  and the snapped guide line + bar.beat readout that follows the pointer
//----------------------------------------------------------------------------
bool ArrangeView::dragging() const
{
    return m_moving || m_growing || m_adding || m_copying || m_extending ||
           m_slipping || m_range_drag || m_lassoing || m_selecting ||
           m_scrubtrim || m_tandem || m_looptrim_src;
}

// Pan when a drag reaches the canvas edge, so a clip can be dragged past the
// visible span without letting go.  Called every frame while a gesture runs
// (draw() keeps requesting redraws), so it scrolls even if the pointer is held
// still beyond the edge.
void ArrangeView::edge_autoscroll()
{
    if (!dragging() || m_mx < 0) return;
    const int L = canvas_x(), R = canvas_x() + canvas_w();
    const int margin = 32;
    double px = 0.0;
    if      (m_mx < L + margin) px = -(double)(L + margin - m_mx);
    else if (m_mx > R - margin) px =  (double)(m_mx - (R - margin));
    else return;
    if (px < -margin) px = -margin;
    if (px >  margin) px =  margin;
    m_scroll_ticks += (long)(px * 0.5 * m_scale_x);
    clamp_scroll();
}

// Vertical guide at the snapped edge being dragged + a readout box by the
// pointer.  m_guide_* are filled by drag_canvas / press_canvas so the text can
// describe the actual gesture (position when moving, length when trimming).
void ArrangeView::drag_feedback(App& app)
{
    // Clip Overwrite Indicator (ch.28 p664): while a clip is dragged, a
    // smaller OFF-SCREEN clip that the drag currently covers fully lights
    // the corresponding screen edge in the warning (accent) role, blinking
    // so it cannot be mistaken for a selection edge.
    if (m_clip_overwrite && (m_moving || m_copying) && m_perf) {
        const Theme& tt = theme();
        long a = -1, b = -1; int lane = -1;
        if (m_copying && m_drop_seq >= 0) {
            a = m_ghost_tick; b = m_ghost_tick + m_copy_len; lane = m_drop_seq;
        } else if (m_moving && m_move_seq >= 0 && m_perf->is_active(m_move_seq)) {
            std::vector<ClipSpan> ms; clip_spans(m_move_seq, ms);
            for (const ClipSpan& sp : ms)
                if (sp.selected || ms.size() == 1) { a = sp.on; b = sp.endEx; break; }
            lane = m_move_seq;
        }
        if (lane >= 0 && b > a) {
            bool offL = false, offR = false;
            std::vector<ClipSpan> os;
            for (int cs : lane_sequences(lane)) {
                if (cs == lane && !m_copying) continue;
                os.clear(); clip_spans(cs, os);
                for (const ClipSpan& sp : os) {
                    if (!(sp.on >= a && sp.endEx <= b)) continue;   // not covered
                    const int x0 = tick_to_x(sp.on), x1 = tick_to_x(sp.endEx);
                    if (x1 < canvas_x()) offL = true;
                    else if (x0 > canvas_x() + canvas_w()) offR = true;
                }
            }
            if ((offL || offR) && ((SDL_GetTicks() / 300) & 1)) {
                if (offL) fill_rect(app.ren, SDL_Rect{ canvas_x(), canvas_y(),
                                                       5, canvas_h() }, tt.accent);
                if (offR) fill_rect(app.ren, SDL_Rect{ canvas_x() + canvas_w() - 5,
                                                       canvas_y(), 5, canvas_h() }, tt.accent);
                tip("Overwriting an off-screen clip", m_mx + 14, m_my - 22);
            }
            if (offL || offR) app.request_redraw();     // keep the blink alive
        }
    }
    if (!m_guide_on && m_guide_text.empty()) return;
    const Theme& t = theme();
    const int x = tick_to_x(m_guide_tick);
    if (m_guide_on && x >= canvas_x() && x <= canvas_x() + canvas_w()) {
        // dashed so it reads as a transient guide, not a grid line
        for (int y = canvas_y(); y < canvas_y() + canvas_h(); y += 6)
            vline(app.ren, x, y, std::min(y + 3, canvas_y() + canvas_h()), t.hi);
    }
    if (m_guide_text.empty()) return;
    SDL_Rect box = clamp_popup_rect(
        SDL_Rect{ m_mx + 14, m_my + 16, app.mono.text_w(m_guide_text) + 10,
                  app.mono.ch() + 6 },
        SDL_Rect{ canvas_x(), canvas_y(), canvas_w(), canvas_h() });
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.hi);
    app.mono.draw(app.ren, box.x + 5, box.y + 3, m_guide_text, t.hi);
}

// How many clips the marquee currently covers (drawn as a live badge, so a
// rubber-band selection reports what it will take before the button is let go).
int ArrangeView::count_clips_in_rect(SDL_Rect box) const
{
    int n = 0;
    for_each_visible_clip([&](const ClipSpan&, const SDL_Rect& r) {
        if (rect_intersects(box, r)) ++n;
    });
    return n;
}

//----------------------------------------------------------------------------
//  draw
//----------------------------------------------------------------------------
void ArrangeView::draw(App& app)
{
    if (!visible) return;
    const Theme& t = theme();

    // Live pointer position (logical coords) for hover highlighting.  Plain mouse
    // motion is not delivered to the view without a button held, so poll it here;
    // the highlight then refreshes whenever the view is redrawn.
    mouse_logical(app, m_mx, m_my);

    // No bound perform -> paint an empty themed frame rather than crash.
    if (!m_perf) {
        fill_rect(app.ren, rect, t.bg);
        frame_rect(app.ren, rect, t.dim);
        return;
    }

    // The top block is now the two topbar rows + the optional Universe strip +
    // the 22 px time band; the layout knob follows so canvas_y() partitions
    // the rect the same way the painters below do.
    ruler_h = toolbar_h() + (m_universe_on ? m_universe_h : 0) + 22;

    std::vector<int> act = active_list();

    m_tip.clear();            // tooltips are re-collected by this frame's hovers

    // A running drag pans the view when it reaches the canvas edge; keep the
    // frames coming so it scrolls even while the pointer is held still.
    if (dragging()) { edge_autoscroll(); app.request_redraw(); }

    // Shuttle Lock: advance the playhead by the locked speed in realtime.
    // Honest transport shuttling -- the engine has no varispeed audition path,
    // so this moves the playhead (and any playback follows normally).
    if (m_shuttle != 0.0) {
        const Uint64 now = SDL_GetTicks();
        const double dt = (now > m_shuttle_ms) ? (double)(now - m_shuttle_ms) / 1000.0 : 0.0;
        m_shuttle_ms = now;
        double bpm = m_perf->get_bpm(); if (bpm < 1.0) bpm = 120.0;
        const double tps = (double)c_ppqn * bpm / 60.0;
        long t = playhead() + (long)(m_shuttle * tps * dt);
        if (t < 0) { t = 0; m_shuttle = 0.0; }
        seek_to(t);
        app.request_redraw();
    }

    // follow-playhead: page the view so the playhead stays visible during play.
    if (m_follow) {
        long ph   = playhead();
        long span = (long)((double)canvas_w() * m_scale_x);
        if (span < 1) span = 1;
        if (ph < m_scroll_ticks || ph > m_scroll_ticks + span) {
            m_scroll_ticks = ph - span / 4;
            if (m_scroll_ticks < 0) m_scroll_ticks = 0;
        }
    }

    fill_rect(app.ren, rect, t.bg);

    m_hover_resize = false;   // recomputed by draw_headers each frame
    m_hover_loop = m_hover_extend = m_hover_trim = false;   // recomputed by draw_clips
    draw_canvas (app, act);   // lanes + grid + clips + playhead
    draw_headers(app, act);   // left track-header column
    draw_ruler  (app);        // top time ruler + L/R markers
    draw_scrollbar(app);      // proportional horizontal song navigator
    apply_resize_cursor();    // SIZENS cursor over a lane's resize edge

    // corner box: the header column's share of the Universe row + time band
    // (the topbar rows above it are painted full-width by draw_topbar).
    SDL_Rect corner{ rect.x, rect.y + toolbar_h(), header_w,
                     ruler_h - toolbar_h() };
    fill_rect(app.ren, corner, t.panel);
    hline(app.ren, corner.x, corner.x + corner.w, rect.y + ruler_h - 1, t.accent);
    vline(app.ren, corner.x + corner.w - 1, corner.y, corner.y + corner.h, t.accent);
    app.mono.draw(app.ren, corner.x + 6,
                  corner.y + (corner.h - app.mono.ch()) / 2, "ARRANGE", t.text);

    // Pencil over audio: surface the manual's destructive-edit warning as a
    // hover tip before any press happens.
    if (m_edit_tool == EditTool::Draw && m_mx >= canvas_x() && m_my >= canvas_y()) {
        const int pr = row_at(m_my);
        if (pr >= 0) {
            const int pidx = m_v_offset + pr;
            if (pidx >= 0 && pidx < (int)act.size()) {
                const int pcs = clip_sequence_at(act[(size_t)pidx], x_to_tick(m_mx));
                if (pcs >= 0 && m_audio.count(pcs)) {
                    double bpm = m_perf->get_bpm(); if (bpm < 1.0) bpm = 120.0;
                    double rate = 48000.0;
                    if (m_audio[pcs] && m_audio[pcs]->sampleRate > 0)
                        rate = m_audio[pcs]->sampleRate;
                    const double tps = (double)c_ppqn * bpm / 60.0 / rate;
                    tip(m_scale_x <= tps * 1.5
                        ? "PENCIL: drag redraws the waveform DESTRUCTIVELY"
                        : "Pencil (audio): zoom to sample level to repair the waveform",
                        m_mx + 12, m_my + 18);
                }
            }
        }
    }

    draw_fade_selection(app); // ch.32: highlight the Grabber's selected fade
    audition_poll();          // ch.32: stop a fade audition past its end
    draw_topbar(app);         // mode block, tool strip, counters, fields
    draw_universe(app);       // session overview strip (when shown)
    draw_smart_glyph(app);    // Smart-tool zone feedback by the pointer

    frame_rect(app.ren, rect, t.dim);
    draw_rename(app);         // inline clip-name editor (over the canvas)
    draw_menu(app);
    draw_addmenu(app);        // add-track chooser (drawn on top of everything)
    draw_instrmenu(app);      // track-header instrument picker (topmost)
    draw_iomenu(app);
    draw_toolmenu(app);       // right-click edit-tool picker
    draw_submode_menu(app);   // click-and-hold tool sub-mode picker
    draw_grid_menu(app);      // Grid value configuration menu
    draw_nudge_menu(app);     // Nudge value picker
    draw_zt_menu(app);        // Zoom Toggle preferences
    draw_spot(app);           // Spot-mode location dialog
    draw_view_menu(app);      // View > Waveforms / View > Clip options
    draw_edit_menu(app);      // Edit command menu (ch.28/31)
    draw_undo_window(app);    // Undo History window (non-modal)
    draw_fadesmenu(app);      // right-click Fades submenu (ch.32)
    draw_fadepref(app);       // fade & crossfade preferences (ch.32)
    draw_fade_dialog(app);    // the Fade In/Out/Crossfade dialog (modal)
    draw_batch_dialog(app);   // the Batch Fades dialog (modal)
    draw_flash(app);          // transient status line
    draw_tooltip(app);        // hover hints collected during this frame
    draw_help(app);           // "?" / "/" shortcut overlay (over everything)
}

//----------------------------------------------------------------------------
//  right-click context menu (add / open / delete clips)
//----------------------------------------------------------------------------
namespace {
    // Rows 6/7 are Freeze/Freeze-Track; their labels flip to Unfreeze when the
    // clip's sequence is frozen (see menu_label()).
    // Row 9 is the clip's own LOOP / ONE-SHOT flag (sequence::m_loop_enabled);
    // its label flips to say which way the click will take it.
    const char* kMenuClip[]  = { "Open Piano", "Open Tracker",
                                 "Split", "Duplicate", "Rename", "Delete",
                                 "Freeze", "Freeze Track", "CDP...", "Loop On",
                                 "Sync Point", "Rate +" };
    // AUDIO region context menu (Ardour region ops); row 6 flips Mute/Unmute.
    const char* kMenuAudio[] = { "Split", "Duplicate", "Rename", "Delete",
                                 "Fades...",
                                 "Normalize", "Reverse", "Mute", "Unfreeze",
                                 "CDP...", "Split Into Mono", "Compact...",
                                 "Sync Point", "Rate +" };
    // Automation regions are neither MIDI patterns nor audio.  In particular,
    // never offer Piano/Tracker here: both editors can mutate the sequence's
    // event store and corrupt an automation region that only owns lanes/curves.
    const char* kMenuAuto[]  = { "Open Automation", "Split", "Duplicate",
                                 "Rename", "Delete" };
    const char* kMenuEmpty[] = { "Add Piano Clip", "Add Tracker Clip" };
    const char* kMenuEmptyAuto = "Add Automation Clip";
    const int   kMenuW = 150;
    const int   kMenuClipN = 12;
    const int   kMenuAudioN = 14;
    const int   kMenuAutoN = 5;
}

bool ArrangeView::menu_is_audio() const
{
    return m_menu_on_clip && m_audio.count(m_menu_seq) != 0;
}

bool ArrangeView::any_menu_open() const
{
    return m_menu_open || m_addmenu_open || m_instrmenu_open || m_iomenu_open ||
           m_toolmenu_open || m_snap_menu || m_submode_menu || m_grid_menu ||
           m_nudge_menu || m_zt_menu || m_spot_open || m_view_menu ||
           m_edit_menu || m_undo_opts || m_fdlg_open || m_bdlg_open ||
           m_fadesmenu_open || m_fadepref_open;
}

void ArrangeView::close_all_menus()
{
    m_menu_open = m_addmenu_open = m_instrmenu_open = false;
    m_iomenu_open = m_toolmenu_open = m_snap_menu = false;
    m_submode_menu = m_grid_menu = m_nudge_menu = m_zt_menu = false;
    m_view_menu = m_edit_menu = m_undo_opts = false;
    m_fadesmenu_open = m_fadepref_open = false;
    m_spot_open = false; m_spot_seq = -1; m_submode_tool = -1;
    m_menu_seq = -1; m_menu_on_clip = false;
    m_instrmenu_seq = -1; m_instrmenu_items.clear();
    m_iomenu_seq = -1; m_iomenu_route = -1; m_iomenu_items.clear();
}

const char* ArrangeView::menu_label(int idx) const
{
    if (menu_is_audio()) {
        if (idx == 7) {
            std::map<int, AudioRegion>::const_iterator it = m_region.find(m_menu_seq);
            return (it != m_region.end() && it->second.muted) ? "Unmute" : "Mute";
        }
        return kMenuAudio[idx];
    }
    if (is_automation(m_menu_seq)) return kMenuAuto[idx];
    if (idx == 6) return is_frozen(m_menu_seq) ? "Unfreeze" : "Freeze";
    if (idx == 7) return is_frozen(m_menu_seq) ? "Unfreeze Track" : "Freeze Track";
    // The clip's own repeat flag.  Label names the ACTION (as Mute/Unmute
    // does on the audio menu), so it reads as what the click will do.
    if (idx == 9) {
        sequence* s = (m_menu_seq >= 0 && m_perf && m_perf->is_active(m_menu_seq))
                      ? m_perf->get_sequence(m_menu_seq) : nullptr;
        return (s && s->get_loop_enabled()) ? "Loop Off (1-Shot)" : "Loop On";
    }
    return kMenuClip[idx];
}

// The context menu opens with the edit-tool palette across its top: the same
// four icons as the ruler strip, so the tool can be switched right where the
// pointer already is instead of travelling back up to the toolbar.
static const int kMenuStripH = 26;

// Geometry of the context menu, shared by draw AND hit-test.  They used to
// derive it separately -- draw from the CLAMPED rect, the click handler from the
// unclamped kMenuW -- so in a narrow view the tool cells and the row hit areas
// sat somewhere the menu wasn't.
SDL_Rect ArrangeView::menu_box(App& app, int& rowh, int& n) const
{
    n = m_menu_on_clip ? (menu_is_audio() ? kMenuAudioN
                           : (is_automation(m_menu_seq) ? kMenuAutoN : kMenuClipN))
                       : (is_automation(m_menu_seq) ? 1 : 2);
    rowh = app.font.ch() + 8;
    return clamp_popup_rect(
        SDL_Rect{ m_menu_x, m_menu_y, kMenuW, kMenuStripH + n * rowh + 2 }, rect);
}

void ArrangeView::draw_menu(App& app)
{
    if (!m_menu_open) return;
    const Theme& t = theme();
    int n = 0, rowh = 0;
    SDL_Rect box = menu_box(app, rowh, n);
    m_menu_x = box.x; m_menu_y = box.y;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.dim);

    // --- tool palette header ------------------------------------------------
    const int tw = box.w / kToolCount;
    for (int i = 0; i < kToolCount; ++i) {
        SDL_Rect tb{ box.x + 1 + i * tw, box.y + 1, tw - 1, kMenuStripH - 2 };
        const bool hot = (m_mx >= tb.x && m_mx < tb.x + tb.w &&
                          m_my >= tb.y && m_my < tb.y + tb.h);
        const bool on  = (int)m_edit_tool == i;
        if (hot || on) fill_rect(app.ren, tb, hot ? t.accent : t.keybg);
        draw_tool_icon(app, tb, i, hot ? t.bg : (on ? t.hi : t.text));
        if (hot) tip(std::string(tool_name(i)) + " tool (" + tool_key(i) + ")",
                     m_mx + 12, m_my - 20);
    }
    hline(app.ren, box.x + 1, box.x + box.w - 1, box.y + kMenuStripH - 1, t.dim);

    // --- action rows --------------------------------------------------------
    for (int i = 0; i < n; ++i) {
        const char* label = m_menu_on_clip ? menu_label(i)
                          : (is_automation(m_menu_seq) ? kMenuEmptyAuto : kMenuEmpty[i]);
        SDL_Rect row{ box.x + 1, box.y + kMenuStripH + i * rowh, box.w - 2, rowh };
        if (row.y + rowh > rect.y + rect.h) break;
        const bool hot = (m_mx >= row.x && m_mx < row.x + row.w &&
                          m_my >= row.y && m_my < row.y + row.h);
        if (hot) fill_rect(app.ren, row, t.accent);
        app.font.draw(app.ren, row.x + 7, row.y + 4,
                      fit_text(app.font, label, box.w - 16), hot ? t.bg : t.text);
    }
}

bool ArrangeView::menu_click(App& app, int mx, int my)
{
    int n = 0, rowh = 0;
    const SDL_Rect box = menu_box(app, rowh, n);
    // tool palette strip across the menu's top
    if (mx >= box.x && mx < box.x + box.w &&
        my >= box.y && my < box.y + kMenuStripH) {
        const int i = (mx - box.x) / std::max(1, box.w / kToolCount);
        if (i >= 0 && i < kToolCount) select_tool((EditTool)i, false);
        m_menu_open = false;
        app.request_redraw();
        return true;
    }
    const int rows_y = box.y + kMenuStripH;
    // Only rows that FIT inside the box are actionable; a menu taller than the
    // view drew a subset but accepted clicks for every row.
    const int rows = std::min(n, std::max(0, (box.h - kMenuStripH - 2) / std::max(1, rowh)));
    bool inside = (mx >= box.x && mx < box.x + box.w &&
                   my >= rows_y && my < rows_y + rows * rowh);
    if (inside) {
        int idx = (my - rows_y) / rowh;
        sequence* s = (m_menu_seq >= 0 && m_perf->is_active(m_menu_seq))
                      ? m_perf->get_sequence(m_menu_seq) : nullptr;
        if (s) {
            if (menu_is_audio()) {
                // AUDIO region ops (kMenuAudio order).
                if      (idx == 0) { split_clip_at(m_menu_seq,m_menu_tick); }
                else if (idx == 1) {                 // Duplicate after
                    const AudioRegion& r=region_for(m_menu_seq);
                    create_pattern(m_menu_seq,r.position+r.length,r.length,r.source,false);
                }
                else if (idx == 2) {                 // Rename (inline)
                    const AudioRegion& r=region_for(m_menu_seq);
                    int xs=tick_to_x(r.position),xe=tick_to_x(r.position+r.length);
                    int cvx = canvas_x(), cvr = canvas_x() + canvas_w();
                    if (xs < cvx) xs = cvx;
                    if (xe > cvr) xe = cvr;
                    int w = xe - xs; if (w < 60) w = 60;
                    int lane_y = canvas_y();
                    std::vector<int> act = active_list();
                    for (size_t i = 0; i < act.size(); ++i)
                        if (act[i] == m_menu_seq) { lane_y = row_top((int)i - m_v_offset); break; }
                    m_menu_open = false;
                    begin_rename(app, m_menu_seq, SDL_Rect{ xs, lane_y + 3, w, track_h(m_menu_seq) - 6 });
                    return true;
                }
                else if (idx == 3) {                                 // Delete
                    // routes through the Multiple-Undo queue (disk-cached
                    // audio restore + history entry in one op).
                    delete_audio_clip_undoable(m_menu_seq);
                }
                else if (idx == 4) {                 // Fades submenu (ch.32)
                    // A fade under the click becomes the submenu's target so
                    // Delete / Shape / Slope act on the fade you clicked.
                    int which = -1;
                    if (fade_hit(m_menu_seq, m_menu_tick, which)) {
                        m_fade_sel_seq = m_menu_seq; m_fade_sel_which = which;
                    }
                    m_menu_open = false;
                    m_fadesmenu_open = true;
                    m_fadesmenu_rect = SDL_Rect{ m_menu_x, m_menu_y, 0, 0 };
                    app.request_redraw();
                    return true;
                }
                else if (idx == 5) { if (on_clip_normalize) on_clip_normalize(m_menu_seq); }
                else if (idx == 6) { if (on_clip_reverse)   on_clip_reverse(m_menu_seq); }
                else if (idx == 7) {                 // Mute / Unmute (toggle)
                    AudioRegion& r = region_for(m_menu_seq);
                    r.muted = !r.muted;
                    if (on_clip_mute) on_clip_mute(m_menu_seq, r.muted);
                }
                else if (idx == 8) { if (on_unfreeze) on_unfreeze(m_menu_seq); }        // Unfreeze
                else if (idx == 9) { if (on_open_cdp) on_open_cdp(m_menu_seq); }         // CDP...
                else if (idx == 10) {                // Split Into Mono (ch.31 p736)
                    if (on_clip_split_mono) on_clip_split_mono(m_menu_seq);
                    else flash("Split Into Mono: needs shell wiring");
                }
                else if (idx == 11) {                // Compact... (ch.31 p737)
                    // DESTRUCTIVE and not undoable -- the prompt says so, and
                    // Esc is the way out.  The typed number is the pad in ms.
                    const int cseq = m_menu_seq;
                    m_dlg_kind = 3;
                    m_dlg_buf = "10";
                    m_menu_open = false;
                    flash("COMPACT is destructive & CANNOT BE UNDONE - pad ms, Enter");
                    app.begin_text(&m_dlg_buf, nullptr, [this, cseq, &app](bool ok) {
                        if (ok) {
                            const long pad = std::atol(m_dlg_buf.c_str());
                            if (on_clip_compact) {
                                // Compact invalidates the disk-cached undo
                                // blobs' source geometry: clear the queue.
                                clear_undo_queue();
                                on_clip_compact(cseq, pad < 0 ? 0 : pad);
                            } else flash("Compact: needs shell wiring");
                        }
                        m_dlg_kind = -1;
                        app.request_redraw();
                    });
                    return true;
                }
                else if (idx == 12) {                // Sync Point at the click
                    AudioRegion& r = region_for(m_menu_seq);
                    const long rel = m_menu_tick - r.position;
                    std::map<int,long>::iterator it = m_sync_point.find(m_menu_seq);
                    if (it != m_sync_point.end() && std::labs(it->second - rel) <
                        (long)(6.0 * m_scale_x))
                        m_sync_point.erase(it);      // click it again = clear
                    else m_sync_point[m_menu_seq] = std::max<long>(0, rel);
                }
                else if (idx == 13) {                // Rate +: cycle 1..5, off
                    int& rr = m_rating[m_menu_seq];
                    rr = (rr % 5) + 1;
                }
                m_menu_open = false;
                app.request_redraw();
                return true;
            }
            if (m_menu_on_clip && is_automation(m_menu_seq)) {
                if      (idx == 0) { if (on_open_editor) on_open_editor(m_menu_seq, 2); }
                else if (idx == 1) split_clip_at(m_menu_seq, m_menu_tick);
                else if (idx == 2) {
                    push_undo("Duplicate Clip");
                    s->select_trigger(m_menu_tick);
                    const long st=s->get_selected_trigger_start_tick();
                    const long en=s->get_selected_trigger_end_tick();
                    const long len=en-st+1;
                    create_pattern(m_menu_seq,st+len,len,
                                   trigger_offset_at(s,m_menu_tick),true);
                }
                else if (idx == 3) {
                    s->select_trigger(m_menu_tick);
                    const long st=s->get_selected_trigger_start_tick();
                    const long en=s->get_selected_trigger_end_tick();
                    int xs=tick_to_x(st),xe=tick_to_x(en);
                    const int cvx=canvas_x(),cvr=canvas_x()+canvas_w();
                    if(xs<cvx)xs=cvx;
                    if(xe>cvr)xe=cvr;
                    int w=xe-xs;if(w<60)w=60;
                    int lane_y=canvas_y();
                    const std::vector<int> act=active_list();
                    for(size_t i=0;i<act.size();++i)
                        if(act[i]==m_menu_seq){lane_y=row_top((int)i-m_v_offset);break;}
                    m_menu_open=false;
                    begin_rename(app,m_menu_seq,SDL_Rect{xs,lane_y+3,w,track_h(m_menu_seq)-6});
                    return true;
                }
                else if (idx == 4) {
                    push_undo("Delete Clip");
                    s->del_trigger(m_menu_tick);
                    if(on_midi_clip_delete)on_midi_clip_delete(m_menu_seq);
                }
                m_menu_open=false;
                app.request_redraw();
                return true;
            }
            if (m_menu_on_clip) {
                if      (idx == 0 && on_open_editor) on_open_editor(m_menu_seq, is_automation(m_menu_seq)?2:0);
                else if (idx == 1 && on_open_editor) on_open_editor(m_menu_seq, 1);
                else if (idx == 2) {                 // Split at the click tick
                    split_clip_at(m_menu_seq,m_menu_tick);
                }
                else if (idx == 3) {                 // Duplicate (paste a copy after)
                    push_undo("Duplicate Clip");
                    s->select_trigger(m_menu_tick);
                    long st = s->get_selected_trigger_start_tick();
                    long en = s->get_selected_trigger_end_tick();
                    long len = en - st + 1;
                    create_pattern(m_menu_seq, st + len, len,
                                   trigger_offset_at(s, m_menu_tick), true);
                }
                else if (idx == 4) {                 // Rename (inline)
                    s->select_trigger(m_menu_tick);
                    long st = s->get_selected_trigger_start_tick();
                    long en = s->get_selected_trigger_end_tick();
                    int xs = tick_to_x(st), xe = tick_to_x(en);
                    int cvx = canvas_x(), cvr = canvas_x() + canvas_w();
                    if (xs < cvx) xs = cvx;
                    if (xe > cvr) xe = cvr;
                    int w = xe - xs; if (w < 60) w = 60;
                    int lane_y = canvas_y();
                    std::vector<int> act = active_list();
                    for (size_t i = 0; i < act.size(); ++i)
                        if (act[i] == m_menu_seq) { lane_y = row_top((int)i - m_v_offset); break; }
                    m_menu_open = false;
                    begin_rename(app, m_menu_seq, SDL_Rect{ xs, lane_y + 3, w, track_h(m_menu_seq) - 6 });
                    return true;
                }
                else if (idx == 5) { push_undo("Delete Clip"); s->del_trigger(m_menu_tick);
                                     if(on_midi_clip_delete)on_midi_clip_delete(m_menu_seq); }
                else if (idx == 6) {                 // Freeze / Unfreeze this clip
                    if (is_frozen(m_menu_seq)) { if (on_unfreeze) on_unfreeze(m_menu_seq); }
                    else if (on_freeze_clip) {
                        s->select_trigger(m_menu_tick);
                        long st = s->get_selected_trigger_start_tick();
                        long en = s->get_selected_trigger_end_tick();
                        on_freeze_clip(m_menu_seq, st, en);
                    }
                }
                else if (idx == 7) {                 // Freeze / Unfreeze whole lane
                    if (is_frozen(m_menu_seq)) { if (on_unfreeze) on_unfreeze(m_menu_seq); }
                    else if (on_freeze_track) on_freeze_track(m_menu_seq);
                }
                // A MIDI clip has no audio to hand CDP, so the shell freezes it
                // to a wav first -- see on_open_cdp's host binding.
                else if (idx == 8) { if (on_open_cdp) on_open_cdp(m_menu_seq); }
                else if (idx == 9) {                 // Loop / one-shot
                    // Per CLIP, per sequence: nothing global, and no other
                    // clip's flag is touched.  Not the audio region's `loop`
                    // (that menu is kMenuAudio and returns above), not the
                    // song loop (perform's left/right tick).
                    s->set_loop_enabled(!s->get_loop_enabled());
                }
                else if (idx == 10) {                // Sync Point at the click
                    s->select_trigger(m_menu_tick);
                    const long st = s->get_selected_trigger_start_tick();
                    const long rel = m_menu_tick - (st >= 0 ? st : 0);
                    std::map<int,long>::iterator it = m_sync_point.find(m_menu_seq);
                    if (it != m_sync_point.end() && std::labs(it->second - rel) <
                        (long)(6.0 * m_scale_x))
                        m_sync_point.erase(it);
                    else m_sync_point[m_menu_seq] = std::max<long>(0, rel);
                }
                else if (idx == 11) {                // Rate +: cycle 1..5
                    int& rr = m_rating[m_menu_seq];
                    rr = (rr % 5) + 1;
                }
            } else {
                push_undo("Add Clip");
                long len = s->get_length(); if (len < 1) len = c_ppqn * 4;
                long tt = m_menu_tick - (m_menu_tick % len);
                int new_seq = create_pattern(m_menu_seq, tt, len, 0, false);
                if (on_open_editor && new_seq >= 0)
                    on_open_editor(new_seq, is_automation(m_menu_seq)?2:(idx == 1 ? 1 : 0));
            }
        }
    }
    m_menu_open = false;
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  ADD-TRACK chooser popup (opened by the "+" affordance; same themed-rect
//  style as the right-click context menu).  Two items -> on_add_track(kind).
//----------------------------------------------------------------------------
namespace {
    const char* kAddMenu[] = { "Instrument Track", "Audio Track", "Automation Track" };
    const int   kAddMenuN  = 3;
}

void ArrangeView::draw_addmenu(App& app)
{
    if (!m_addmenu_open) return;
    const Theme& t = theme();
    const int rowh = app.font.ch() + 8;
    SDL_Rect box = clamp_popup_rect(m_addmenu_rect, rect);
    m_addmenu_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    for (int i = 0; i < kAddMenuN; ++i) {
        bool hot = (m_mx >= box.x && m_mx < box.x + box.w &&
                    m_my >= box.y + i * rowh && m_my < box.y + (i + 1) * rowh);
        if (hot) fill_rect(app.ren, SDL_Rect{ box.x + 1, box.y + i * rowh + 1,
                                              box.w - 2, rowh - 1 }, t.accent);
        app.font.draw(app.ren, box.x + 8, box.y + i * rowh + 5,
                      fit_text(app.font, kAddMenu[i], box.w - 16),
                      hot ? t.bg : t.text);
    }
}

bool ArrangeView::addmenu_click(App& app, int mx, int my)
{
    const int rowh = app.font.ch() + 8;
    SDL_Rect b = m_addmenu_rect;
    bool inside = (mx >= b.x && mx < b.x + b.w &&
                   my >= b.y && my < b.y + kAddMenuN * rowh);
    if (inside) {
        int idx = (my - b.y) / rowh;
        if      (idx == 0) { if (on_add_track) on_add_track(0); }  // instrument
        else if (idx == 1) { if (on_add_track) on_add_track(1); }  // audio
        else if (idx == 2) { if (on_add_track) on_add_track(2); }  // automation
    }
    m_addmenu_open = false;                       // click (in or out) closes it
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  Track-header INSTRUMENT dropdown : line-2 box + a pick popup listing every
//  instrument node.  Selecting one assigns this track to that instrument (the
//  shell sets the sequence's MIDI channel + wires the instrument).
//----------------------------------------------------------------------------
SDL_Rect ArrangeView::instr_box_rect(int row_y, int ch) const
{
    const HeaderGeom g = header_geom();
    int bx = rect.x + g.name_x;
    int bw = (rect.x + g.vu_x - 6) - bx;           // stop short of the VU strip
    if (bw < 48) bw = 48;
    int by = row_y + 5 + ch + 1;                   // line-2 band
    int bh = ch + 4;
    return SDL_Rect{ bx, by, bw, bh };
}

// Rows a clamped popup can actually show.  clamp_popup_rect caps the HEIGHT,
// but these lists used to draw (and hit-test) every entry regardless, so a long
// instrument or port list spilled out of the view and the rows past the bottom
// edge were pickable while invisible.
static int popup_visible_rows(const SDL_Rect& box, int rowh, int n)
{
    if (rowh <= 0) return 0;
    int rows = (box.h - 2) / rowh;
    if (rows < 0) rows = 0;
    return std::min(rows, n);
}

// ONE scrolling list popup, shared by the instrument and I/O pickers (they used
// to be two near-identical copies, and bug fixes only ever landed in one).
void ArrangeView::draw_list_popup(App& app, SDL_Rect& box,
                                  const std::vector<std::string>& items,
                                  int& scroll, int selected)
{
    const Theme& t = theme();
    const int rowh = app.font.ch() + 8;
    box = clamp_popup_rect(box, rect);
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    const int n = (int)items.size();
    const int rows = popup_visible_rows(box, rowh, n);
    if (scroll > n - rows) scroll = n - rows;
    if (scroll < 0) scroll = 0;
    for (int i = 0; i < rows; ++i) {
        const int idx = scroll + i;
        if (idx < 0 || idx >= n) break;
        SDL_Rect row{ box.x + 1, box.y + i * rowh + 1, box.w - 2, rowh - 1 };
        const bool hot = (m_mx >= row.x && m_mx < row.x + row.w &&
                          m_my >= row.y && m_my < row.y + row.h);
        const bool on  = (idx == selected);
        if (hot || on) fill_rect(app.ren, row, hot ? t.accent : t.keybg);
        const std::string label = (selected >= 0 ? (on ? "* " : "  ") : std::string())
                                + items[(size_t)idx];
        app.font.draw(app.ren, box.x + 5, row.y + 4,
                      fit_text(app.font, label, box.w - 10), hot ? t.bg : t.text);
    }
    if (scroll > 0)
        app.mono.draw(app.ren, box.x + box.w - 12, box.y + 2, "^", t.dim);
    if (scroll + rows < n)
        app.mono.draw(app.ren, box.x + box.w - 12,
                      box.y + box.h - app.mono.ch() - 2, "v", t.dim);
}

bool ArrangeView::list_popup_pick(App& app, const SDL_Rect& box,
                                  const std::vector<std::string>& items,
                                  int scroll, int mx, int my, int& outIdx) const
{
    const int rowh = app.font.ch() + 8;
    const int n = (int)items.size();
    const int rows = popup_visible_rows(box, rowh, n);
    if (mx < box.x || mx >= box.x + box.w ||
        my < box.y || my >= box.y + rows * rowh) return false;
    const int idx = scroll + (my - box.y) / rowh;
    if (idx < 0 || idx >= n) return false;
    outIdx = idx;
    return true;
}

void ArrangeView::draw_instrmenu(App& app)
{
    if (!m_instrmenu_open) return;
    draw_list_popup(app, m_instrmenu_rect, m_instrmenu_items, m_instrmenu_scroll, -1);
}

bool ArrangeView::instrmenu_click(App& app, int mx, int my)
{
    int idx = -1;
    if (list_popup_pick(app, m_instrmenu_rect, m_instrmenu_items,
                        m_instrmenu_scroll, mx, my, idx) && on_pick_instrument)
        on_pick_instrument(m_instrmenu_seq, idx);
    m_instrmenu_open = false;                      // click (in or out) closes it
    m_instrmenu_seq  = -1;
    m_instrmenu_scroll = 0;
    app.request_redraw();
    return true;
}

void ArrangeView::draw_iomenu(App& app)
{
    if (!m_iomenu_open) return;
    const int sel = on_track_io_index ? on_track_io_index(m_iomenu_seq, m_iomenu_route) : -1;
    draw_list_popup(app, m_iomenu_rect, m_iomenu_items, m_iomenu_scroll, sel);
}

bool ArrangeView::iomenu_click(App& app, int mx, int my)
{
    int idx = -1;
    if (list_popup_pick(app, m_iomenu_rect, m_iomenu_items,
                        m_iomenu_scroll, mx, my, idx) && on_pick_track_io)
        on_pick_track_io(m_iomenu_seq, m_iomenu_route, idx);
    m_iomenu_open = false; m_iomenu_seq = m_iomenu_route = -1; m_iomenu_scroll = 0;
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  canvas : lanes, bar/beat grid, clip blocks, playhead
//----------------------------------------------------------------------------
void ArrangeView::draw_canvas(App& app, const std::vector<int>& act)
{
    const Theme& t = theme();
    SDL_Rect cv{ canvas_x(), canvas_y(), canvas_w(), canvas_h() };
    fill_rect(app.ren, cv, t.bg);

    // lane stripes + bottom separators (variable per-track heights, cumulative).
    // A song-muted lane is washed out here too, so mute state is visible on the
    // timeline itself and not only on its header button.
    for (int r = 0, y = canvas_y(); y < canvas_y() + canvas_h(); ++r) {
        int idx = m_v_offset + r;
        int lh  = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        Color lane = (r % 2 == 0) ? t.panel : t.bg;
        fill_rect(app.ren, SDL_Rect{ cv.x, y, cv.w, lh }, lane);
        if (idx >= 0 && idx < (int)act.size()) {
            sequence* ls = m_perf->get_sequence(act[(size_t)idx]);
            if (ls && ls->get_song_mute()) {
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                Color wash = t.dim; wash.a = 46;
                fill_rect(app.ren, SDL_Rect{ cv.x, y, cv.w, lh - 1 }, wash);
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
            }
        }
        hline(app.ren, cv.x, cv.x + cv.w, y + lh - 1, t.dim);
        y += lh;
    }

    // Alternating shade over each LABELLED bar group (the exact span between two
    // ruler numbers), so bars stay countable by eye when the numbers thin out.
    const Metric mt = metric(app.mono.cw());
    if (mt.labelStep > 0) {
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
        Color shade = t.dim; shade.a = 16;
        for (long g = m_scroll_ticks / mt.labelStep; ; ++g) {
            const int x0 = tick_to_x(g * mt.labelStep);
            if (x0 > cv.x + cv.w) break;
            if (g & 1) {
                const int a = std::max(cv.x, x0);
                const int b = std::min(cv.x + cv.w, tick_to_x((g + 1) * mt.labelStep));
                if (b > a) fill_rect(app.ren, SDL_Rect{ a, cv.y, b - a, cv.h }, shade);
            }
        }
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    }

    // Growing recording overlay: visible immediately, before the final clip is
    // committed. MIDI draws note blocks; audio draws the latest waveform data.
    if(m_recPreviewSeq>=0&&m_recPreviewLength>0) {
        int row=-1;
        for(int i=0;i<(int)act.size();++i)
            if(lane_key(act[(size_t)i])==lane_key(m_recPreviewSeq)){row=i-m_v_offset;break;}
        // The armed lane can be scrolled out of view.  `row>=0` only rejected a
        // lane above the viewport; one scrolled off the BOTTOM gave a row index
        // past the last visible one, and row_top() happily returns a y below the
        // canvas -- the growing preview block was then painted over the
        // horizontal scrollbar and whatever else lives under the grid.
        if(row>=0&&row<visible_rows()) {
            // LOOP OVERDUB draws differently.  The take is not becoming a new
            // clip stretching to the right of the record point: it is being
            // folded INTO the armed pattern modulo that pattern's length (see
            // commit_recording's `overdub` path -- this mirrors its predicate).
            // Drawing the raw take meant a block that grew by a pattern length
            // on every pass, sprawling right across every clip beside it, with
            // notes drawn at ticks they will never end up on.
            sequence* rec=(m_perf&&m_perf->is_active(m_recPreviewSeq))
                         ?m_perf->get_sequence(m_recPreviewSeq):nullptr;
            const bool isAudioTake=(m_recPreviewAudio&&!m_recPreviewAudio->empty());
            const long fold=(!isAudioTake&&m_perf&&m_perf->get_looping()&&
                             rec&&rec->get_length()>0)?rec->get_length():0;
            const long previewLen=fold>0?std::min(m_recPreviewLength,fold)
                                        :m_recPreviewLength;
            const int y=row_top(row)+3,h=track_h(act[(size_t)(row+m_v_offset)])-6;
            const int x1=std::max(canvas_x(),tick_to_x(m_recPreviewStart));
            const int x2=std::min(canvas_x()+canvas_w(),tick_to_x(m_recPreviewStart+previewLen));
            if(x2>x1) {
                Color body=theme().accent; body.a=190;
                fill_round(app.ren,SDL_Rect{x1,y,x2-x1,h},4,body);
                if(isAudioTake)
                    draw_waveform(app,m_recPreviewAudio,x1,y,x2-x1,h,0,
                                  m_recPreviewAudio->numFrames());
                else for(const auto& n:m_recPreviewNotes) {
                    long ns=n.start,ne=std::max(n.start+1,n.end);
                    if(fold>0) {                      // same wrap the commit does
                        const long len=std::max<long>(1,ne-ns);
                        ns%=fold;
                        ne=ns+std::min(len,fold-ns);  // clipped at the fold point
                    }
                    int nx1=tick_to_x(m_recPreviewStart+ns);
                    int nx2=tick_to_x(m_recPreviewStart+ne);
                    if(nx2<canvas_x()||nx1>canvas_x()+canvas_w()) continue;
                    nx1=std::max(nx1,canvas_x());
                    nx2=std::min(nx2,canvas_x()+canvas_w());
                    int ny=y+2+(127-std::max(0,std::min(127,n.pitch)))*(h-5)/128;
                    fill_rect(app.ren,SDL_Rect{nx1,ny,std::max(2,nx2-nx1),3},
                              Color{0,0,0,255});
                }
                frame_rect(app.ren,SDL_Rect{x1,y,x2-x1,h},theme().hi);
            }
        }
    }

    // shaded loop / edit-range band under the grid + clips
    draw_loop_band(app);

    // Grid, drawn finest-first so heavier divisions overprint lighter ones.  It
    // uses the SAME metric as the ruler, so a line under a clip is always the
    // line whose number is printed above it -- and both thin out together as
    // you zoom out instead of collapsing into a solid wall.
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
    if (mt.subStep > 0) {                       // snap subdivisions
        Color c = t.dim; c.a = 80;
        for (long tick = m_scroll_ticks - (m_scroll_ticks % mt.subStep); ; tick += mt.subStep) {
            const int x = tick_to_x(tick);
            if (x > cv.x + cv.w) break;
            if (x >= cv.x && (tick % m_beat_len) != 0)
                vline(app.ren, x, cv.y, cv.y + cv.h, c);
        }
    }
    if (mt.beatStep > 0) {                      // beats
        Color c = t.dim; c.a = 170;
        for (long tick = m_scroll_ticks - (m_scroll_ticks % mt.beatStep); ; tick += mt.beatStep) {
            const int x = tick_to_x(tick);
            if (x > cv.x + cv.w) break;
            if (x >= cv.x && (tick % m_measure_len) != 0)
                vline(app.ren, x, cv.y, cv.y + cv.h, c);
        }
    }
    if (mt.barStep > 0) {                       // bars (labelled ones brighter)
        for (long tick = (m_scroll_ticks / mt.barStep) * mt.barStep; ; tick += mt.barStep) {
            const int x = tick_to_x(tick);
            if (x > cv.x + cv.w) break;
            if (x < cv.x) continue;
            Color c = t.accent;
            if (mt.labelStep > 0 && (tick % mt.labelStep) != 0) c.a = 110;
            vline(app.ren, x, cv.y, cv.y + cv.h, c);
        }
    }
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);

    // clip blocks per visible active track (cumulative y).  The FOCUSED lane --
    // the one the last gesture touched, and therefore where a paste or keyboard
    // edit lands -- gets a thin accent frame so that target is never a guess.
    // Lane membership is grouped ONCE here; each clip also receives its own
    // lane's bucket so the Overlapped Crossfades sibling walk needs no rescan.
    const std::vector<std::vector<int>> laneGroups = lane_groups(act);
    for (int r = 0, y = canvas_y(); y < canvas_y() + canvas_h(); ++r) {
        int idx = m_v_offset + r;
        int lh  = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        if (idx >= 0 && idx < (int)act.size()) {
            if (m_focus_lane >= 0 && lane_key(act[(size_t)idx]) == m_focus_lane) {
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                Color glow = t.accent; glow.a = 18;
                fill_rect(app.ren, SDL_Rect{ cv.x, y, cv.w, lh - 1 }, glow);
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
                hline(app.ren, cv.x, cv.x + cv.w, y, t.accent);
                hline(app.ren, cv.x, cv.x + cv.w, y + lh - 1, t.accent);
            }
            for (int clip_seq : laneGroups[(size_t)idx])
                draw_clips(app, clip_seq, y, &laneGroups[(size_t)idx]);
        }
        y += lh;
    }

    // drag-copy ghost: outline where a Ctrl+drag duplicate will land
    if (m_copying && m_drop_seq >= 0 && m_copy_len > 0) {
        int row = -1;
        for (int i = 0; i < (int)act.size(); ++i)
            if (lane_key(act[i]) == lane_key(m_drop_seq)) { row = i - m_v_offset; break; }
        if (row >= 0) {
            int y = row_top(row);
            int lh = track_h(act[(size_t)(m_v_offset + row)]);
            if (y < canvas_y() + canvas_h()) {
                int gx = tick_to_x(m_ghost_tick);
                int gr = tick_to_x(m_ghost_tick + m_copy_len);
                if (gx < cv.x) gx = cv.x;
                if (gr > cv.x + cv.w) gr = cv.x + cv.w;
                if (gr > gx) {
                    // View > Clip > Transparency: the drag overlay is a
                    // translucent fill so material beneath stays readable
                    if (m_clip_transp) {
                        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                        Color gcol = t.hi; gcol.a = 60;
                        fill_rect(app.ren, SDL_Rect{ gx, y + 3, gr - gx, lh - 6 }, gcol);
                        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
                    }
                    frame_rect(app.ren, SDL_Rect{ gx, y + 3, gr - gx, lh - 6 }, t.hi);
                }
            }
        }
    }

    // playhead (ports perfroll::draw_progress)
    int px = tick_to_x(playhead());
    if (px >= cv.x && px <= cv.x + cv.w)
        vline(app.ren, px, cv.y, cv.y + cv.h, t.hi);

    // Tell the frame loop that this column is the only thing that moves during
    // playback, so an animation-only frame can clip to it instead of repainting
    // the window.  Padded either side: the next frame is clipped to THIS rect,
    // and the playhead will have travelled by then -- the pad has to cover that
    // travel plus the wide zoom case where a frame advances several pixels.
    {
        constexpr int kPad = 96;
        SDL_Rect strip{ px - kPad, cv.y, kPad * 2, cv.h };
        app.add_damage(strip);
    }

    // Zoomer rubber band: the range that will fill the window on release
    if (m_zoomer_band) {
        const int a = std::min(m_zoomer_x0, m_zoomer_x1);
        const int b = std::max(m_zoomer_x0, m_zoomer_x1);
        if (b > a + 2) {
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            Color band = t.hi; band.a = 30;
            fill_rect(app.ren, SDL_Rect{ a, cv.y, b - a, cv.h }, band);
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
            vline(app.ren, a, cv.y, cv.y + cv.h, t.hi);
            vline(app.ren, b, cv.y, cv.y + cv.h, t.hi);
        }
    }

    if (m_lassoing) {
        int x0 = std::min(m_lasso_x0, m_lasso_x1);
        int y0 = std::min(m_lasso_y0, m_lasso_y1);
        int x1 = std::max(m_lasso_x0, m_lasso_x1);
        int y1 = std::max(m_lasso_y0, m_lasso_y1);
        SDL_Rect box{ x0, y0, x1 - x0, y1 - y0 };
        Color fill = t.accent; fill.a = 36;
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
        fill_rect(app.ren, box, fill);
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        frame_rect(app.ren, box, t.hi);
        // live count + span, so the marquee reports what it is about to take
        if (box.w > 3 || box.h > 3) {
            char badge[64];
            const int n = count_clips_in_rect(box);
            std::snprintf(badge, sizeof(badge), "%d clip%s  %s bars", n,
                          n == 1 ? "" : "s",
                          bars_len((long)(box.w * m_scale_x)).c_str());
            SDL_Rect bb = clamp_popup_rect(
                SDL_Rect{ x1 + 6, y0 - app.mono.ch() - 8,
                          app.mono.text_w(badge) + 10, app.mono.ch() + 6 }, cv);
            fill_rect (app.ren, bb, t.panel);
            frame_rect(app.ren, bb, t.hi);
            app.mono.draw(app.ren, bb.x + 5, bb.y + 3, badge, t.hi);
        }
    }

    // ch.30 edit selection: band over the selected lanes' tick span (drawn
    // over the clips, like every DAW's edit selection)
    draw_edit_selection(app);

    // snapped guide line + bar.beat readout for a running clip gesture
    drag_feedback(app);

    // Empty project: say what to do instead of showing a blank green field.
    if (act.empty()) {
        static const char* kHint[] = {
            "NO TRACKS",
            "right-click here or in the header column to add one",
            "wheel = zoom   shift+wheel = pan   ? = shortcuts"
        };
        const int lh = app.mono.ch() + 6;
        for (int i = 0; i < 3; ++i) {
            const int w = app.mono.text_w(kHint[i]);
            app.mono.draw(app.ren, cv.x + (cv.w - w) / 2,
                          cv.y + cv.h / 2 - lh + i * lh, kHint[i],
                          i == 0 ? t.accent : t.dim);
        }
    }
}

//----------------------------------------------------------------------------
//  one track's clip blocks (ports perfroll::draw_sequence_on)
//----------------------------------------------------------------------------
void ArrangeView::draw_clips(App& app, int seq, int lane_y,
                             const std::vector<int>* laneSeqs)
{
    if (!m_perf->is_active(seq)) return;
    const Theme& t = theme();
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    s->reset_draw_trigger_marker();

    int cvx = canvas_x();
    int cvr = canvas_x() + canvas_w();

    long seq_len = s->get_length();
    int  length_w = (int)(seq_len / m_scale_x);

    long tick_on, tick_off, offset;
    bool selected;
    bool audioRegionEmitted=false;
    const bool isAudioRegion=m_audio.count(seq)!=0;
    while (isAudioRegion ? !audioRegionEmitted
                         : s->get_next_trigger(&tick_on,&tick_off,&selected,&offset)) {
        if(isAudioRegion) {
            const AudioRegion& r=region_for(seq);
            tick_on=r.position; tick_off=r.position+r.length;
            offset=r.source; selected=r.selected; audioRegionEmitted=true;
        }
        if (tick_off <= 0) continue;

        int x_on  = tick_to_x(tick_on);
        const long endExclusive=isAudioRegion?tick_off:tick_off+1;
        int x_off = tick_to_x(endExclusive);
        if (x_off < cvx || x_on > cvr) continue;          // off-screen

        int y = lane_y + 3;
        int h = track_h(seq) - 6;
        int full_x = x_on;

        // clamp the drawn body to the canvas
        int bx = x_on  < cvx ? cvx : x_on;
        int br = x_off > cvr ? cvr : x_off;
        int bw = br - bx;
        if (bw < 2) bw = 2;

        // Pointer over this block?  Hovering gets an outline, and the 6 px trim
        // zones at either end flip the cursor, so "move" vs "trim" is visible
        // BEFORE the button goes down instead of being discovered by surprise.
        const bool hovered = (m_mx >= bx && m_mx < bx + bw &&
                              m_my >= y && m_my < y + h);
        if (hovered && !m_mouse_down &&
            (m_mx - bx <= 6 || (bx + bw) - m_mx <= 6))
            m_hover_trim = true;

        // body: a bright, auto-assigned clip colour; the data inside is drawn
        // SOLID BLACK for contrast. Selected clips get an expanded translucent
        // halo plus a bright outline, so a multi-selection reads as a group.
        const Color body  = clip_color(seq);
        const Color black = Color{ 0, 0, 0, 255 };
        if (selected) {
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            Color glow = t.hi; glow.a = 72;
            fill_round(app.ren, SDL_Rect{ bx - 3, y - 3, bw + 6, h + 6 }, 6, glow);
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
            frame_rect(app.ren, SDL_Rect{ bx - 2, y - 2, bw + 4, h + 4 }, t.hi);
        }
        // View > Clip > Transparency: the clip being MOVED becomes a
        // transparent overlay so clips beneath show through while aligning.
        const bool transp = m_clip_transp && m_moving && m_mouse_down &&
                            seq == m_move_seq;
        if (transp) {
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            Color tb = body; tb.a = 140;
            fill_round(app.ren, SDL_Rect{ bx, y, bw, h }, 4, tb);
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        } else {
            fill_round(app.ren, SDL_Rect{ bx, y, bw, h }, 4, body);
        }

        // ---- AUDIO track: draw the clip's waveform instead of a note preview
        std::map<int, const PatchKnob::engine::AudioClip*>::const_iterator ait = m_audio.find(seq);
        if (ait != m_audio.end() && ait->second) {
            // Map the VISIBLE (canvas-clamped, possibly trimmed) trigger window to
            // a sample sub-range of the clip, so resizing shows only that slice
            // instead of stretching the whole wave.  fullTicks = the clip's
            // natural length; the trigger's [offset .. offset+visibleLen] selects
            // the content window; canvas clamping selects within that.
            const PatchKnob::engine::AudioClip* clip = ait->second;
            const long long nfr = (long long)clip->numFrames();
            std::map<int,long>::const_iterator lit = m_audioLen.find(seq);
            long fullTicks = (lit != m_audioLen.end() && lit->second > 0)
                             ? lit->second : (endExclusive - tick_on);
            if (fullTicks < 1) fullTicks = 1;
            // Region source-offset (Ardour START): the first source tick the
            // block shows.  Tracked in m_region, NOT the trigger's wrapping
            // offset.  The visible px window maps into [srcTick, srcTick+visLen].
            const long srcTick = region_for(seq).source;
            const int full_w = (x_off - x_on) < 1 ? 1 : (x_off - x_on);
            const long visLen = endExclusive - tick_on;
            const double fracL = (double)(bx - x_on) / (double)full_w;   // 0..1 of visible
            const double fracR = (double)(br - x_on) / (double)full_w;
            const double tkL = (double)srcTick + fracL * (double)visLen;  // ticks into source
            const double tkR = (double)srcTick + fracR * (double)visLen;
            // WHAT IS DRAWN MUST BE WHAT IS PLAYED, for audio as much as for
            // notes.  A LOOPING region repeats [source, source+loopLength) for
            // as long as the block runs -- AudioClipPlayer wraps at that period
            // -- but this used to map the whole block linearly onto one pass of
            // the source, so extending a looped clip stretched the waveform
            // instead of repeating it.  Draw one waveform per repetition.
            const AudioRegion& areg = region_for(seq);
            long loopTicks = 0;
            if (areg.loop) {
                loopTicks = areg.loopLength > 0 ? areg.loopLength
                                                : (fullTicks - srcTick);
                if (loopTicks < 1) loopTicks = 0;      // nothing to repeat
            }
            auto wave_slice = [&](double tkA, double tkB, int pxA, int pxB) {
                if (pxB <= pxA) return;
                long long a = (long long)(tkA / (double)fullTicks * (double)nfr);
                long long b = (long long)(tkB / (double)fullTicks * (double)nfr);
                if (a < 0) a = 0; if (b > nfr) b = nfr; if (b <= a) b = a + 1;
                // View > Clip > Display on All Channels: a stereo clip draws
                // its L and R channels as two stacked half-height waveforms.
                if (m_clip_all_chan && !clip->ch[1].empty() && h >= 12) {
                    const int hh = h / 2;
                    draw_waveform(app, clip, pxA, y,      pxB - pxA, hh,     a, b, 0);
                    draw_waveform(app, clip, pxA, y + hh, pxB - pxA, h - hh, a, b, 1);
                } else {
                    draw_waveform(app, clip, pxA, y, pxB - pxA, h, a, b);
                }
            };
            if (loopTicks > 0 && visLen > loopTicks) {
                // px <-> tick of the FULL (unclamped) block, so the phase is
                // right even when the left edge is scrolled off-screen.
                const double pxPerTick = (double)full_w / (double)visLen;
                const long   firstRep  = (long)(((double)(bx - x_on) / pxPerTick) / (double)loopTicks);
                // A very long block at a wide zoom can be thousands of periods;
                // only the ones intersecting the visible body are drawn, and the
                // count is bounded so a 1-tick period cannot stall a frame.
                for (long k = firstRep, guard = 0; guard < 4096; ++k, ++guard) {
                    const long repStart = k * loopTicks;              // block-relative
                    if (repStart >= visLen) break;
                    const long repEnd = std::min<long>(repStart + loopTicks, visLen);
                    int pa = x_on + (int)(repStart * pxPerTick);
                    int pb = x_on + (int)(repEnd   * pxPerTick);
                    if (pa < bx) pa = bx;
                    if (pb > br) pb = br;
                    if (pb <= pa) { if (x_on + (int)(repStart * pxPerTick) > br) break; continue; }
                    // Which slice of the source this column band shows: the
                    // repetition restarts at `source` every period.
                    const double tA = (double)srcTick +
                        ((double)(pa - x_on) / pxPerTick - (double)repStart);
                    const double tB = (double)srcTick +
                        ((double)(pb - x_on) / pxPerTick - (double)repStart);
                    wave_slice(tA, tB, pa, pb);
                    // Seam marker so a repeat reads as a repeat, not one long take.
                    if (pb < br && pb > bx) vline(app.ren, pb, y + 1, y + h - 2, black);
                }
            } else {
                wave_slice(tkL, tkR, bx, br);
            }
            draw_clip_fades(app, seq, SDL_Rect{ bx, y, bw, h });
            // Overlapped Crossfades view (p657): inside a crossfade, draw the
            // OTHER contributing clip's waveform as a translucent ghost so the
            // crossfaded audio can be aligned visually.  A crossfade here is
            // the fade-in/out pair the Smart tool's tandem drag sets on two
            // ADJACENT audio clips.
            if (m_wf_overlap) {
                std::map<int, ClipFade>::const_iterator ft = m_clipFade.find(seq);
                const ClipFade fades = ft != m_clipFade.end() ? ft->second : ClipFade{};
                // Siblings come pre-grouped from draw_canvas; the fallback
                // rescan only runs for callers that did not group (none today).
                const std::vector<int> laneScan =
                    laneSeqs ? std::vector<int>() : lane_sequences(seq);
                const std::vector<int>& sibs = laneSeqs ? *laneSeqs : laneScan;
                for (int cs : sibs) {
                    if (cs == seq) continue;
                    // Region first: every ghost case needs the neighbour's
                    // region to touch this clip's span, so distant siblings
                    // (the common case on a crowded lane) cost ONE map find.
                    std::map<int, AudioRegion>::const_iterator ri = m_region.find(cs);
                    if (ri == m_region.end()) continue;
                    const AudioRegion& orr = ri->second;
                    if (orr.position > endExclusive ||
                        orr.position + orr.length < tick_on) continue;
                    std::map<int, const PatchKnob::engine::AudioClip*>::const_iterator
                        oi = m_audio.find(cs);
                    if (oi == m_audio.end() || !oi->second) continue;
                    const PatchKnob::engine::AudioClip* oc = oi->second;
                    std::map<int, long>::const_iterator ol = m_audioLen.find(cs);
                    const long oFull = (ol != m_audioLen.end() && ol->second > 0)
                                     ? ol->second : orr.length;
                    const long long onfr = (long long)oc->numFrames();
                    if (oFull < 1 || onfr < 1) continue;
                    auto ghost_slice = [&](long t0, long t1, long srcT0) {
                        int pa = std::max(bx, tick_to_x(t0));
                        int pb = std::min(br, tick_to_x(t1));
                        if (pb <= pa) return;
                        const long long a = (long long)((double)srcT0 / oFull * (double)onfr);
                        const long long b2 = (long long)((double)(srcT0 + (t1 - t0)) / oFull * (double)onfr);
                        draw_waveform(app, oc, pa, y, pb - pa, h, a,
                                      std::min(onfr, std::max(a + 1, b2)), -1, true);
                    };
                    const long oEnd = orr.position + orr.length;
                    // left neighbour ends where we start: its tail rides our fade-in
                    if (fades.inTicks > 0 && oEnd == tick_on)
                        ghost_slice(tick_on, tick_on + std::min(fades.inTicks,
                                    endExclusive - tick_on),
                                    orr.source + orr.length);
                    // right neighbour starts where we end: its head rides our fade-out
                    if (fades.outTicks > 0 && orr.position == endExclusive)
                        ghost_slice(endExclusive - std::min(fades.outTicks,
                                    endExclusive - tick_on), endExclusive,
                                    orr.source - std::min(fades.outTicks,
                                    endExclusive - tick_on));
                    // a REAL crossfade: the neighbour genuinely overlaps us --
                    // show its wave inside the overlap window (ch.32 p742)
                    if (fades.inTicks > 0 && orr.position < tick_on &&
                        oEnd > tick_on && oEnd <= endExclusive)
                        ghost_slice(tick_on, std::min(oEnd, tick_on + fades.inTicks),
                                    orr.source + (tick_on - orr.position));
                    if (fades.outTicks > 0 && orr.position > tick_on &&
                        orr.position < endExclusive && oEnd > endExclusive)
                        ghost_slice(std::max(tick_on, orr.position), endExclusive,
                                    orr.source);
                }
            }
            // Ardour gain line: a horizontal line across the region at the gain
            // height, with a centre handle; dimmed when the region is muted.
            {
                int gly = gain_line_y(seq, y, h);
                if (gly < y) gly = y; if (gly > y + h - 1) gly = y + h - 1;
                const bool rmuted = m_region.count(seq) && m_region[seq].muted;
                hline(app.ren, bx, br, gly, black);           // gain line: black
                int hx = bx + (br - bx) / 2;
                fill_rect(app.ren, SDL_Rect{ hx - 2, gly - 2, 4, 4 }, black);
                if (rmuted)   // "M" marker top-left when muted
                    app.mono.draw(app.ren, bx + 2, y + h - app.mono.ch() - 1, "M", black);
            }
            // outline (white when selected) + black title on the coloured body.
            frame_rect(app.ren, SDL_Rect{ bx, y, bw, h }, selected ? t.hi : black);
            const char* anm = s->get_name();
            // Keep the name clear of the corner chip: it used to be laid out
            // across the full body and then painted over by the chip, so the
            // last characters of a name simply vanished under the glyph.
            const int anmw = bw - 4 - (loop_chip_rect(SDL_Rect{ bx, y, bw, h }).w
                                       ? 15 : 0);
            if (anm && anmw > 8 && m_clip_show_name)
                app.mono.draw(app.ren, bx + 2, y + 1,
                              fit_text(app.mono, anm, anmw), black);
            // LOOP handle (top-right corner): a loop glyph; filled when looping.
            // AUDIO ONLY.  This is the REGION's source-wrap flag (m_region.loop)
            // -- "repeat the sample to fill the block" -- and has nothing to do
            // with sequence::get_loop_enabled(), the MIDI clip's own repeat
            // flag drawn further down.  Two different flags, one word: keep
            // each one on its own kind of clip.
            SDL_Rect lb = loop_chip_rect(SDL_Rect{ bx, y, bw, h });
            if (lb.w > 0) {
                const bool looping = m_region.count(seq) && m_region[seq].loop;
                if (m_mx >= lb.x && m_mx < lb.x + lb.w && m_my >= lb.y && m_my < lb.y + lb.h) {
                    m_hover_loop = true;
                    tip(looping ? "Region loops the sample to fill the block"
                                : "Region plays the sample once", m_mx + 12, m_my - 20);
                }
                fill_rect(app.ren, lb, looping ? t.hi : body);
                frame_rect(app.ren, lb, black);
                // two opposed arcs (a loop icon) drawn as short strokes
                set_color(app.ren, black);
                SDL_RenderDrawLine(app.ren, lb.x+3, lb.y+3, lb.x+9, lb.y+3);
                SDL_RenderDrawLine(app.ren, lb.x+9, lb.y+3, lb.x+9, lb.y+6);
                SDL_RenderDrawLine(app.ren, lb.x+9, lb.y+9, lb.x+3, lb.y+9);
                SDL_RenderDrawLine(app.ren, lb.x+3, lb.y+9, lb.x+3, lb.y+6);
            }
            // EXTEND handle: the lower of the two explicit right-side boxes.
            {
                SDL_Rect eb{ br - 14, y + h - 13, 12, 12 };
                const bool ehot = (m_mx >= eb.x && m_mx < eb.x + eb.w &&
                                   m_my >= eb.y && m_my < eb.y + eb.h);
                if (ehot) m_hover_extend = true;
                fill_rect(app.ren, eb, ehot ? t.hi : body);
                frame_rect(app.ren, eb, black);
                hline(app.ren, eb.x + 3, eb.x + 8, eb.y + 4, black);
                hline(app.ren, eb.x + 3, eb.x + 8, eb.y + 7, black);
            }
            // length in bars.beats, bottom-left, when the block has the room
            // (suppressed while a View > Clip time display owns the corner)
            if (bw > 96 && h > 22 && m_clip_time == 0 && !m_clip_gain_info) {
                const std::string len = bars_len(endExclusive - tick_on);
                app.mono.draw(app.ren, bx + 3, y + h - app.mono.ch() - 2, len, black);
            }
            // View > Clip adornments: times, sync point, gain info, rating,
            // overlap shadows (ch.28 p662-664)
            draw_clip_adornments(app, seq, SDL_Rect{ bx, y, bw, h },
                                 tick_on, endExclusive, true);
            // muted lane -> wash the block down to match its lane tint
            if (s->get_song_mute()) {
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                fill_round(app.ren, SDL_Rect{ bx, y, bw, h }, 4, Color{ 0, 0, 0, 110 });
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
            }
            if (hovered && !selected)
                frame_rect(app.ren, SDL_Rect{ bx, y, bw, h }, t.hi);
            continue;   // skip the MIDI note-preview path for audio clips
        }

        // ---- AUTOMATION clip: the envelope, its loop repeats and a live dot.
        // The block is the visible (canvas-clamped) window; auto_geom maps it
        // through the region's source/loopLength exactly as the waveform path
        // maps its block through [s0,s1), so a trimmed or slipped clip shows
        // the right slice of curve and a repeat shows as a repeat.
        bool autoMuted = false;
        if (is_automation(seq)) {
            // The rect is the CLAMPED body, but the tick span is the clip's
            // real one -- so the wrap phase still comes from the block's true
            // start when its left edge is scrolled off-screen.
            const AutoGeom g = auto_geom(seq, tick_on, endExclusive,
                                         SDL_Rect{ bx, y, bw, h }, offset);
            autoMuted = g.muted;
            draw_automation(app, seq, g, g.muted || s->get_song_mute());
        }

        // ---- tiny note preview tiled across the clip (perfroll port) -------
        // The pattern's events are walked ONCE and cached as clip-relative
        // pixel offsets, then stamped per repetition.  It used to re-walk every
        // event of the sequence for EVERY repetition, so a long clip at a wide
        // zoom (hundreds of repeats) re-read the whole event list hundreds of
        // times per frame, for every clip on screen.  Repetitions that fall
        // outside the visible body are now skipped without touching a note, and
        // the lines go out in one batched call.
        int lowest  = s->get_lowest_note_event();
        int highest = s->get_highest_note_event();
        if (!is_automation(seq) && highest >= lowest && length_w > 1 && seq_len > 0) {
            const int height = highest - lowest + 2;

            // WHAT IS DRAWN MUST BE WHAT IS PLAYED.  This used to tile the whole
            // pattern at get_length() unconditionally, anchored to song tick 0.
            // That is neither of the two things a clip can actually do:
            //   * loop ON with a window  -> sequence::play_span repeats
            //     [loop_start, loop_end) at that window's period and skips
            //     everything outside it, so a 3-step polyrhythmic loop was
            //     drawn as bar-length repeats of the entire pattern;
            //   * loop OFF (one-shot)    -> play_span walks the data ONCE from
            //     the clip's start, so dragging the clip longer changed nothing
            //     on screen even though the clip really did get longer.
            // Mirror src/sequence.cpp's loop_set/period derivation exactly.
            const long lp_s   = s->get_loop_start();
            const long lp_e   = s->get_loop_end();
            const bool lp_on  = s->get_loop_enabled();
            const bool lp_set = (lp_s > 0 || lp_e < seq_len) && lp_e > lp_s;
            const bool use_win = lp_on && lp_set;
            const long win_s  = use_win ? lp_s : 0;
            const long win_e  = use_win ? lp_e : seq_len;
            const long period = win_e - win_s;
            // Pixel width of ONE repetition (the window when looping, else the
            // pattern).  length_w is the pattern's own width, so scale by it.
            const int  win_w  = (int)(((long) period * length_w) / seq_len);

            struct PrevNote { int x0, x1, y; };
            static std::vector<PrevNote> notes;     // reused across frames
            notes.clear();
            {
                long tick_s, tick_f; int note, vel; bool nsel; draw_type dt;
                s->reset_draw_marker();
                while ((dt = s->get_next_note_event(&tick_s, &tick_f, &note,
                                                    &nsel, &vel)) != DRAW_FIN) {
                    // HIDDEN events (outside [0, length)) must not be drawn.
                    // sequence::set_length keeps them so a later re-grow can
                    // restore them, and play_span refuses to sound them -- but
                    // their pixel offset lands past length_w, and the clamp
                    // below then pinned them to the body edge (or, further out,
                    // straight into the NEXT repetition's body).  A shortened
                    // clip drew phantom notes it does not play.
                    // Outside the repetition -> never sounds, never drawn.
                    // With no loop window this is the old [0, seq_len) test.
                    if (tick_s < win_s || tick_s >= win_e) continue;
                    PrevNote p;
                    p.x0 = (int)(((long)(tick_s - win_s) * length_w) / seq_len);
                    p.x1 = (dt == DRAW_NOTE_ON || dt == DRAW_NOTE_OFF)
                           ? p.x0 + 1
                           : (int)(((long)(tick_f - win_s) * length_w) / seq_len);
                    p.y = ((h - 8) - ((h - 8) * (note - lowest)) / height) + 4;
                    // vertical MIDI zoom: spread the rows about the centre
                    if (m_midi_zoom != 1.f) {
                        int c2 = h / 2;
                        int zy = c2 + (int)((p.y - c2) * m_midi_zoom);
                        p.y = std::max(2, std::min(h - 2, zy));
                    }
                    // A note that WRAPS the pattern end has its off at a LOWER
                    // tick than its on.  That collapsed into a 1 px stub; draw
                    // the two segments it really is -- on..pattern end, and
                    // pattern start..off.
                    if (dt == DRAW_NORMAL_LINKED && p.x1 < p.x0) {
                        PrevNote head; head.x0 = 0; head.x1 = p.x1; head.y = p.y;
                        if (head.x1 <= head.x0) head.x1 = head.x0 + 1;
                        notes.push_back(head);
                        p.x1 = win_w;
                    }
                    if (p.x1 <= p.x0) p.x1 = p.x0 + 1;
                    notes.push_back(p);
                }
            }

            static std::vector<SDL_Point> pts;      // batched line endpoints
            pts.clear();
            // Phase anchors at the CLIP's start, not at song tick 0: the old
            // `tick_on % seq_len` made a clip's content depend on where it sat
            // in the song, so moving a clip changed which beat it drew first
            // whenever the period did not divide the bar.
            const long off_p = period > 0
                             ? ((((offset - win_s) % period) + period) % period)
                             : 0;
            const long first_marker = tick_on - off_p;
            for (long marker = first_marker; marker < tick_off; marker += period) {
                const int marker_x = tick_to_x(marker);
                if (marker_x > bx + bw) break;            // past the body: done
                if (marker_x + win_w < bx) {
                    if (!lp_on) break;                    // one-shot: no repeats
                    continue;                             // before it: skip pass
                }
                for (size_t n = 0; n < notes.size(); ++n) {
                    int ns_x = notes[n].x0 + marker_x;
                    int nf_x = notes[n].x1 + marker_x;
                    if (ns_x < bx) ns_x = bx;
                    if (nf_x > bx + bw) nf_x = bx + bw;
                    if (nf_x >= bx && ns_x <= bx + bw) {
                        pts.push_back(SDL_Point{ ns_x, y + notes[n].y });
                        pts.push_back(SDL_Point{ nf_x, y + notes[n].y });
                    }
                }
                // A one-shot plays its data once; the rest of the clip is the
                // empty tail you get by dragging the end out.
                if (!lp_on) break;
            }
            set_color(app.ren, black);            // notes: solid black on the colour
            for (size_t p = 0; p + 1 < pts.size(); p += 2)
                SDL_RenderDrawLine(app.ren, pts[p].x, pts[p].y,
                                   pts[p + 1].x, pts[p + 1].y);
        }

        // ---- MIDI clip: the ONE-SHOT tail ---------------------------------
        // A one-shot and a repeating clip were pixel-identical apart from where
        // the notes happened to stop -- and a clip whose notes are off screen,
        // or which has none yet, gave no clue at all.  sequence::play_span
        // walks a one-shot's data ONCE from the clip's start, bounded by the
        // END marker (m_length), so everything past start + (length - offset)
        // is dead air: hatch it.  A looping clip has no tail -- its window
        // repeats for the whole block -- so it never gets one.
        // AUTOMATION regions repeat on the region's own loopLength, not on
        // sequence::m_loop_enabled, so they are excluded here.
        const bool midiClip = !is_automation(seq);
        const bool oneShot  = midiClip && !s->get_loop_enabled();
        if (oneShot && seq_len > 0) {
            const long off = ((offset % seq_len) + seq_len) % seq_len;
            const long dataEnd = tick_on + (seq_len - off);
            int dx = tick_to_x(dataEnd);
            if (dx < bx) dx = bx;
            if (dx < bx + bw - 1) {
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                set_color(app.ren, Color{ 0, 0, 0, 70 });
                // 45-degree strokes, clipped to the tail rect analytically (the
                // renderer's clip rect is the frame's damage region and must not
                // be stolen for a decoration).
                for (int hx = dx - h; hx < bx + bw; hx += 7) {
                    int xa = hx > dx ? hx : dx;
                    int xb = hx + h;
                    if (xb > bx + bw - 1) xb = bx + bw - 1;
                    if (xb <= xa) continue;
                    SDL_RenderDrawLine(app.ren, xa, (y + h) - (xa - hx),
                                                xb, (y + h) - (xb - hx));
                }
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
                if (dx > bx) vline(app.ren, dx, y, y + h, black);
            }
        }

        // outline (white when selected, else black) + black resize handles
        frame_rect(app.ren, SDL_Rect{ bx, y, bw, h }, selected ? t.hi : black);
        if (x_on >= cvx) {
            fill_rect(app.ren, SDL_Rect{ full_x + 1, y + 1, 3, 3 }, black);
            fill_rect(app.ren, SDL_Rect{ full_x + 1, y + h - 4, 3, 3 }, black);
        }

        // pattern name in black on the coloured body (drawn last, stays legible)
        const char* nm = s->get_name();
        const SDL_Rect chip = midiClip ? loop_chip_rect(SDL_Rect{ bx, y, bw, h })
                                       : SDL_Rect{ 0, 0, 0, 0 };
        const int nmw = bw - 4 - (chip.w ? 15 : 0);
        if (nm && nmw > 8 && m_clip_show_name)
            app.mono.draw(app.ren, bx + 2, y + 1,
                          fit_text(app.mono, nm, nmw), black);
        // ---- MIDI clip: the LOOP / ONE-SHOT chip --------------------------
        // Toggles sequence::set_loop_enabled -- the CLIP's own repeat flag.
        // Deliberately NOT the audio region's `loop` (source wrap) drawn on the
        // audio path above: same word, different flag, so each is only ever
        // offered on the kind of clip it belongs to.
        if (chip.w > 0) {
            const bool lp_on = s->get_loop_enabled();
            if (m_mx >= chip.x && m_mx < chip.x + chip.w &&
                m_my >= chip.y && m_my < chip.y + chip.h) {
                m_hover_loop = true;
                tip(lp_on ? "Clip LOOPS its window - click for one-shot"
                          : "Clip plays ONCE - click to loop", m_mx + 12, m_my - 20);
            }
            fill_rect(app.ren, chip, lp_on ? t.hi : body);
            frame_rect(app.ren, chip, black);
            set_color(app.ren, black);
            if (lp_on) {
                // a closed circuit: it comes back round
                SDL_RenderDrawLine(app.ren, chip.x+3, chip.y+3, chip.x+9, chip.y+3);
                SDL_RenderDrawLine(app.ren, chip.x+9, chip.y+3, chip.x+9, chip.y+8);
                SDL_RenderDrawLine(app.ren, chip.x+9, chip.y+8, chip.x+3, chip.y+8);
                SDL_RenderDrawLine(app.ren, chip.x+3, chip.y+8, chip.x+3, chip.y+3);
                SDL_RenderDrawLine(app.ren, chip.x+5, chip.y+1, chip.x+3, chip.y+3);
                SDL_RenderDrawLine(app.ren, chip.x+5, chip.y+5, chip.x+3, chip.y+3);
            } else {
                // an arrow running into a stop bar: it plays once and ends
                SDL_RenderDrawLine(app.ren, chip.x+2, chip.y+6, chip.x+7, chip.y+6);
                SDL_RenderDrawLine(app.ren, chip.x+5, chip.y+4, chip.x+7, chip.y+6);
                SDL_RenderDrawLine(app.ren, chip.x+5, chip.y+8, chip.x+7, chip.y+6);
                SDL_RenderDrawLine(app.ren, chip.x+9, chip.y+2, chip.x+9, chip.y+10);
            }
        }
        // length in bars.beats, bottom-left, when the block has the room
        if (bw > 96 && h > 22 && m_clip_time == 0)
            app.mono.draw(app.ren, bx + 3, y + h - app.mono.ch() - 2,
                          bars_len(endExclusive - tick_on), black);
        draw_clip_adornments(app, seq, SDL_Rect{ bx, y, bw, h },
                             tick_on, endExclusive, false);
        // muted lane -- or a muted AUTOMATION region, which the player skips
        // just as it skips a muted lane -> wash the block down to its lane tint
        if (s->get_song_mute() || autoMuted) {
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            fill_round(app.ren, SDL_Rect{ bx, y, bw, h }, 4, Color{ 0, 0, 0, 110 });
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        }
        if (hovered && !selected)
            frame_rect(app.ren, SDL_Rect{ bx, y, bw, h }, t.hi);
    }
}

void ArrangeView::set_audio_clip(int seq, const PatchKnob::engine::AudioClip* clip, long fullTicks)
{
    if (clip) {
        const bool same=m_audio.count(seq)&&m_audio[seq]==clip;
        m_audio[seq] = clip;
        if (fullTicks > 0) m_audioLen[seq] = fullTicks;
        if(!same) {
            m_region.erase(seq);   // a true source replacement imports fresh geometry
            m_clipFade.erase(seq); // ... and must not inherit the old clip's fades
        }
    } else {
        m_audio.erase(seq);
        m_audioLen.erase(seq);
        m_region.erase(seq);
        m_clipFade.erase(seq);         // matched to the cleared audio (was leaking)
    }
}

const PatchKnob::engine::AudioClip* ArrangeView::audio_clip(int seq) const
{
    std::map<int,const PatchKnob::engine::AudioClip*>::const_iterator it=m_audio.find(seq);
    return it==m_audio.end()?nullptr:it->second;
}

void ArrangeView::set_audio_region(int seq,long position,long length,long source,
                                   float gain,bool muted,bool loop,long loopTicks)
{
    AudioRegion r;
    r.position=std::max<long>(0,position); r.length=std::max<long>(1,length);
    r.source=std::max<long>(0,source); r.gain=gain; r.muted=muted; r.loop=loop;
    r.loopLength=std::max<long>(0,loopTicks);
    m_region[seq]=r;
}

void ArrangeView::set_audio_region_loop_length(int seq,long loopTicks)
{
    std::map<int,AudioRegion>::iterator it=m_region.find(seq);
    if(it==m_region.end())return;
    it->second.loopLength=std::max<long>(0,loopTicks);
}

void ArrangeView::set_record_preview(int seq,long timelineStart,long length,
                                     const std::vector<RecordPreviewNote>& notes,
                                     const PatchKnob::engine::AudioClip* audio) {
    m_recPreviewSeq=seq; m_recPreviewStart=timelineStart;
    m_recPreviewLength=std::max<long>(1,length);
    m_recPreviewNotes=notes; m_recPreviewAudio=audio;
}
void ArrangeView::clear_record_preview() {
    m_recPreviewSeq=-1; m_recPreviewLength=0; m_recPreviewNotes.clear();
    m_recPreviewAudio=nullptr;
}

// Purge every per-sequence entry for `seq`.  Called on delete / track removal so
// the recycled index cannot resurrect a previous clip's waveform pointer (which
// may point at a freed freeze buffer -> crash), region, fade, colour, or frozen
// tint.  The lane HEIGHT is shared by every sequence on the lane, so it is only
// dropped when `seq` was the last sequence on that lane.
void ArrangeView::forget_seq(int seq)
{
    // A popup captured this sequence index when it opened.  If the track is torn
    // down while the menu is still up (Delete from the menu itself, or the shell
    // removing the track), the next pick would act on a recycled -- or dead --
    // index.  Close anything pointing at it.
    if (m_menu_seq == seq || m_instrmenu_seq == seq || m_iomenu_seq == seq)
        close_all_menus();
    if (m_focus_lane == lane_key(seq)) m_focus_lane = -1;

    const int key = lane_key(seq);     // resolve BEFORE the caller drops routing
    m_audio.erase(seq);
    m_audioLen.erase(seq);
    m_region.erase(seq);
    m_clipFade.erase(seq);
    m_clipColor.erase(seq);
    m_frozen.erase(seq);
    m_sync_point.erase(seq);           // ch.28/31 per-clip metadata: a recycled
    m_rating.erase(seq);               // index must not inherit the dead clip's
    m_timestamp.erase(seq);            // sync point / rating / time stamps /
    m_user_stamp.erase(seq);           // user-named promotion
    m_user_named.erase(seq);
    bool laneStillUsed = false;
    if (m_perf)
        for (int s = 0; s < c_max_sequence; ++s)
            if (s != seq && m_perf->is_active(s) && lane_key(s) == key) { laneStillUsed = true; break; }
    if (!laneStillUsed) m_trackH.erase(key);
}

void ArrangeView::reset_project_state()
{
    m_audio.clear(); m_audioLen.clear(); m_region.clear(); m_clipFade.clear();
    m_clipColor.clear(); m_frozen.clear(); m_trackH.clear();
    clear_record_preview(); m_clip_clipboard.clear();
    // ch.28: a project swap invalidates every queued undo closure (they name
    // sequences of the song that is no longer loaded) -- clear the queue, the
    // per-clip metadata and the gain-special clipboard with it.
    clear_undo_queue();
    m_undo_open=false; m_undo_opts=false; m_undo_scroll=0; m_undo_compound=0;
    m_undo_pending=UndoOp{};
    m_sync_point.clear(); m_rating.clear(); m_timestamp.clear();
    m_user_stamp.clear(); m_user_named.clear();
    m_gain_clipboard=-999.f; m_capture_name.clear();
    m_view_menu=false; m_edit_menu=false; m_flash.clear();
    m_menu_open=false; m_addmenu_open=false; m_instrmenu_open=false; m_iomenu_open=false;
    m_menu_seq=-1; m_menu_tick=0; m_menu_on_clip=false;
    m_instrmenu_seq=-1; m_instrmenu_items.clear();
    m_iomenu_seq=-1; m_iomenu_route=-1; m_iomenu_items.clear();
    m_edit_name.clear(); m_edit_seq=-1; m_edit_rect=SDL_Rect{0,0,0,0};
    m_drop_seq=-1; m_move_seq=-1; m_extend_seq=-1; m_gain_seq=-1; m_fade_seq=-1;
    m_mouse_down=m_moving=m_growing=m_adding=m_copying=m_slipping=m_extending=false;
    m_gain_grab=m_lassoing=m_range_drag=m_scroll_drag=m_vscroll_drag=false;
    m_hdr_resize=m_drag_left=m_drag_right=false;
    m_hdr_resize_seq=-1; m_hdr_resize_y0=m_hdr_resize_h0=0;
    m_hover_resize=m_hover_loop=m_hover_extend=m_hover_trim=false; m_mx=m_my=-1;
    m_fade_grab=FadeGrab::None;
    m_toolmenu_open=false; m_help_open=false; m_scrubbing=false;
    m_focus_lane=-1; m_guide_on=false; m_guide_text.clear(); m_guide_tick=0;
    m_tip.clear();
    m_drop_tick=m_drop_offset=0; m_copy_len=m_copy_offset=m_ghost_tick=0;
    m_clip_span=0; m_paste_tick=-1; m_dup_origin=0;
    m_slip_ref_tick=m_slip_ref_source=0;
    m_lasso_x0=m_lasso_y0=m_lasso_x1=m_lasso_y1=0;
    m_range_anchor=0; m_scroll_drag_x0=0; m_scroll_drag_tick0=0;
    m_vscroll_drag_y0=m_vscroll_drag_offset0=0;
    // These are indexed by sequence number everywhere (draw_headers, apply_solo,
    // the solo button).  Clearing them left an EMPTY vector that every one of
    // those sites then indexed -- an out-of-bounds read/write on every project
    // load.  Re-arm them at full size instead.
    m_solo.assign(c_max_sequence, 0);
    m_mute_snapshot.assign(c_max_sequence, 0);
    m_solo_snapped.assign(c_max_sequence, 0);
    m_solo_active=false; m_ioTab.clear();
    m_edit_tool=EditTool::Grab; m_snap_menu=false;
    // ch.29/30 state back to defaults
    m_edit_mode=EditMode::Grid; m_snap_to_grid=false; m_grid_relative=false;
    m_shuffle_lock=false; m_tool_lock=false;
    m_grid_scale=0; m_grid_dotted=false; m_grid_triplet=false; m_grid_follow_main=true;
    m_trim_mode=0; m_grab_mode=0; m_zoom_mode=0; m_tool_before_zoom=EditTool::Grab;
    for (int i = 0; i < 6; ++i) m_fkey_down[i]=false;
    m_zoomer_band=m_zoomer_cont=false;
    m_trim_group.clear(); m_tce_drag=false; m_scrubtrim=false;
    m_looptrim_src=false; m_tandem=false; m_tandem_left=m_tandem_right=-1;
    m_xfade_drag=false; m_xfade_left=m_xfade_right=-1;
    m_shuttle=0.0; m_pencil_audio=false; m_pencil_seq=-1;
    m_wave_zoom=m_midi_zoom=1.f;
    for (int i = 0; i < 5; ++i) m_zoom_preset[i]=0.0;
    m_prev_scale=0.0; m_prev_scroll=0;
    m_zt_on=false; m_zt_in_valid=false; m_zt_in=ZoomToggleState{}; m_zt_out=ZoomToggleState{};
    m_zt_menu=false; m_zt_lane=-1;
    m_sel_start=m_sel_end=-1; m_sel_lo=m_sel_hi=-1;
    m_link_timeline=m_link_track=true;
    m_lastsel[0]=m_lastsel[1]=-1; m_lastsel_rows[0]=m_lastsel_rows[1]=-1;
    m_selecting=m_tl_selecting=false; m_marker_drag=0;
    m_tab_transients=false; m_click_count=0;
    m_counter_edit=-1; m_counter_scrub=false; m_counter_scrub_which=-1;
    m_grid_menu=m_nudge_menu=m_submode_menu=false; m_submode_tool=-1;
    m_nudge_idx=2; m_hold_tool=m_hold_preset=-1;
    m_universe_on=false; m_universe_h=48; m_univ_drag=m_univ_resize=false;
    m_spot_open=false; m_spot_seq=-1; m_timestamp.clear();
    m_last_click_seq=-1; m_last_click_tick=0; m_last_click_ms=0;
    m_scroll_ticks=0; m_v_offset=0;
    // View settings are part of the project's presentation too; leaving them
    // meant a new project inherited the last one's zoom, grid and lane height.
    m_scale_x=c_perf_scale_x; row_h=80;
    m_snap_idx=2; m_snap=snap_value(m_snap_idx); m_follow=false;
}

void ArrangeView::cancel_interaction(App& app)
{
    m_mouse_down=m_moving=m_growing=m_adding=m_copying=m_slipping=m_extending=false;
    m_gain_grab=m_lassoing=m_range_drag=m_scroll_drag=m_vscroll_drag=false;
    m_hdr_resize=m_drag_left=m_drag_right=false;
    m_move_seq=m_extend_seq=m_gain_seq=m_fade_seq=m_drop_seq=-1;
    m_fade_grab=FadeGrab::None;
    if (m_fdlg_open) fade_dialog_apply(false);   // window going away: revert
    m_bdlg_open=false; m_fadesmenu_open=false; m_fadepref_open=false;
    m_fade_move=false; m_fade_move_before.clear();
    m_menu_open=m_addmenu_open=m_instrmenu_open=m_iomenu_open=m_snap_menu=false;
    m_edit_name.clear(); m_edit_seq=-1;
    app.request_redraw();
}

void ArrangeView::move_lane_height(int fromKey, int toKey)
{
    if (fromKey == toKey) return;
    std::map<int,int>::iterator it = m_trackH.find(fromKey);
    if (it != m_trackH.end()) { int h = it->second; m_trackH.erase(it); m_trackH[toKey] = h; }
    else m_trackH.erase(toKey);   // source had default height -> clear any stale target
}

void ArrangeView::unfreeze_rebuild_midi(int audioLaneSeq, int sourceSeq)
{
    if (!m_perf) return;
    sequence* src = m_perf->get_sequence(sourceSeq);
    if (!src) return;

    // A track freeze places the audio at position 0 / source 0.  Only rebuild
    // when the audio was actually rearranged (split into pieces, or the single
    // piece moved / trimmed off the origin) -- a plain unfreeze must leave the
    // source MIDI exactly as it was.
    std::vector<int> seqs = lane_sequences(audioLaneSeq);
    if (seqs.empty()) return;
    bool rearranged = seqs.size() > 1;
    if (!rearranged) {
        AudioRegion r0 = region_for(seqs[0]);
        rearranged = (r0.position != 0) || (r0.source != 0);
    }
    if (!rearranged) return;

    // Everything that is NOT a note (controllers, program changes, pitch bend)
    // has to survive: the rebuild below wipes the sequence, and it used to drop
    // all of it on the floor.
    std::vector<sequence::EventSnapshot> other;
    {
        std::vector<sequence::EventSnapshot> all;
        src->snapshot_events(all);
        for (size_t i = 0; i < all.size(); ++i) {
            const unsigned char st = all[i].status & 0xF0;
            if (st != 0x90 && st != 0x80) other.push_back(all[i]);
        }
    }

    // Snapshot the source notes (start, length, pitch, velocity).
    struct N { long start, len; int note, vel; };
    std::vector<N> notes;
    src->reset_draw_marker();
    long ts = 0, tf = 0; int note = 0, vel = 0; bool sel = false;
    draw_type dt;
    while ((dt = src->get_next_note_event(&ts, &tf, &note, &sel, &vel)) != DRAW_FIN)
        if (dt == DRAW_NORMAL_LINKED && tf > ts) notes.push_back(N{ ts, tf - ts, note, vel });

    // Each region maps SOURCE ticks [source, source+length) to timeline
    // [position, ...): a note that STARTS inside that window moves by
    // (position - source).  A note landing in several regions is emitted once per
    // region (mirrors the audio being duplicated across those regions).
    std::vector<N> out;
    for (int as : seqs) {
        AudioRegion r = region_for(as);
        const long shift = r.position - r.source;
        for (size_t i = 0; i < notes.size(); ++i) {
            const N& n = notes[i];
            if (n.start >= r.source && n.start < r.source + r.length)
                out.push_back(N{ n.start + shift, n.len, n.note, n.vel });
        }
    }

    // Map the non-note events through the same region windows.
    std::vector<sequence::EventSnapshot> outOther;
    for (size_t k = 0; k < seqs.size(); ++k) {
        AudioRegion r = region_for(seqs[k]);
        const long shift = r.position - r.source;
        for (size_t i = 0; i < other.size(); ++i) {
            const sequence::EventSnapshot& ev = other[i];
            if (ev.tick >= r.source && ev.tick < r.source + r.length) {
                sequence::EventSnapshot o = ev; o.tick = ev.tick + shift;
                outOther.push_back(o);
            }
        }
    }

    // Replace the source's notes with the rearranged set (triggers are untouched).
    push_undo("Unfreeze Rebuild");
    src->select_all();
    src->mark_selected();
    src->remove_marked();
    for (size_t i = 0; i < out.size(); ++i) {
        const N& n = out[i];
        if (n.start < 0) continue;
        src->add_event(n.start,         0x90, (unsigned char)n.note, (unsigned char)n.vel);
        src->add_event(n.start + n.len, 0x80, (unsigned char)n.note, 0);
    }
    for (size_t i = 0; i < outOther.size(); ++i) {
        const sequence::EventSnapshot& o = outOther[i];
        if (o.tick < 0) continue;
        src->add_event(o.tick, o.status, o.d0, o.d1);
    }
    src->verify_and_link();
}

// The Ardour AudioRegion for `seq`, lazily initialised from its first trigger
// (position + length) with source-offset 0 (a fresh, untrimmed placement).
ArrangeView::AudioRegion& ArrangeView::region_for(int seq)
{
    std::map<int, AudioRegion>::iterator it = m_region.find(seq);
    if (it != m_region.end()) return it->second;
    AudioRegion r;
    if (sequence* s = m_perf->get_sequence(seq)) {
        s->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        if (s->get_next_trigger(&on, &off, &sel, &offs)) { r.position = on; r.length = off - on + 1; }
    }
    if (r.length <= 0) {
        std::map<int,long>::const_iterator lit = m_audioLen.find(seq);
        r.length = (lit != m_audioLen.end() && lit->second > 0) ? lit->second : 1;
    }
    return m_region[seq] = r;
}

// Apply the Ardour content law for the just-finished gesture and push the new
// region to the shell.  The trigger already carries the new position+length;
// `source` is derived here:
//   * move       (set_position)  : source FIXED  (content travels with the block)
//   * left-trim  (trim_front)    : source += (newPos - oldPos) (content anchored)
//   * right-trim (trim_end)      : source FIXED  (position fixed, length grows/shrinks)
// source is clamped to [0, sourceLen - length] (Ardour verify_start_and_length).
// The per-gesture Ardour laws are applied by the gesture itself (drag_canvas
// moves position / advances source on a left trim), so this only clamps and
// publishes.  It used to take leftTrim/rightTrim flags and (void) them both.
void ArrangeView::commit_region(int seq)
{
    AudioRegion& r = region_for(seq);
    if(r.position<0)r.position=0; if(r.length<1)r.length=1;

    // clamp source to the available source (Ardour verify_start_and_length).
    // A LOOPED region is allowed to exceed the source length (it wraps to fill),
    // so the right-edge clamp is skipped when r.loop.
    std::map<int,long>::const_iterator lit = m_audioLen.find(seq);
    const long srcLen = (lit != m_audioLen.end() && lit->second > 0) ? lit->second : r.length;
    if (r.source < 0) r.source = 0;
    if(srcLen>0&&r.source>srcLen-1)r.source=srcLen-1;
    if(!r.loop&&r.source+r.length>srcLen)r.length=std::max<long>(1,srcLen-r.source);

    if (on_clip_region_changed) on_clip_region_changed(seq, r.position, r.length, r.source);
    // Trimming either contributor of a crossfade recalculates it for the new
    // overlap (ch.32 p752); no overlap left -> its fades are dropped/clamped.
    sync_xfade_after_region_edit(seq);
}

// Y of the region's horizontal gain line: gain 0..2 maps bottom..top (unity at
// the vertical centre), so there is headroom to boost above the recorded level.
int ArrangeView::gain_line_y(int seq, int clipTop, int clipH) const
{
    float g = 1.0f;
    std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
    if (it != m_region.end()) g = it->second.gain;
    float gn = g * 0.5f;                       // 0..2 -> 0..1
    if (gn < 0.f) gn = 0.f; if (gn > 1.f) gn = 1.f;
    return clipTop + (int)((1.f - gn) * clipH);
}

// Ardour Playlist::_split_region at `splitTick`: the LEFT half keeps {P,S,B};
// the RIGHT half becomes a NEW lane-sharing sequence with {P+B, S+B, L-B} (its
// source start advances by B so the audio does not shift across the cut).
void ArrangeView::split_audio_clip(int seq, long splitTick)
{
    if (!m_audio.count(seq)) return;
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    AudioRegion r = region_for(seq);
    const long P = r.position, S = r.source, L = r.length;
    const long B = splitTick - P;
    if (B < 1 || B >= L) return;                    // no zero-length halves (Ardour)

    // RIGHT half: a new lane (create_pattern shares the mixer track / lane_key
    // and duplicates the audio); then override its source-offset to S + B.
    // create_pattern falls back to returning the SOURCE index when the shell
    // binds no on_create_pattern; writing the right half to that index and then
    // the left half over it silently corrupted the region instead of splitting.
    const int newSeq = create_pattern(seq, splitTick, L - B, 0, false);
    if (newSeq >= 0 && newSeq != seq) {
        if (sequence* rs = m_perf->get_sequence(newSeq)) {
            if (s->get_name()) rs->set_name(s->get_name());
            auto_name_clip(newSeq, seq);       // numbered variation (p727)
        }
        AudioRegion nr=r; nr.position=splitTick; nr.source=S+B; nr.length=L-B;
        nr.selected=true;
        m_region[newSeq] = nr;
        if (on_clip_region_changed) on_clip_region_changed(newSeq, nr.position, nr.length, nr.source);
        // Split the fades between halves (create_pattern copied the whole fade):
        // the LEFT keeps its fade-IN, the RIGHT keeps the fade-OUT; the cut edge
        // gets no fade on either side.  With the Preserve Fades when Editing
        // preference OFF, fades touched by the edit are dropped instead
        // (ch.32 p759-760); ON also trims a fade the cut lands inside to the
        // new clip boundary (the engine clamps the rest).
        std::map<int,ClipFade>::iterator lf = m_clipFade.find(seq);
        if (lf != m_clipFade.end()) {
            if (!m_preserve_fades) {
                m_clipFade.erase(newSeq);
                m_clipFade.erase(lf);
                commit_fade(seq); commit_fade(newSeq);
            } else {
                ClipFade rf; rf.outTicks = lf->second.outTicks; rf.outK = lf->second.outK;
                rf.outShape = lf->second.outShape; rf.outSlope = lf->second.outSlope;
                if (rf.outTicks > 0) { m_clipFade[newSeq] = rf; commit_fade(newSeq); }
                else                   m_clipFade.erase(newSeq);
                lf->second.outTicks = 0; lf->second.outK = 0.f;    // left drops its out-fade
                if (lf->second.inTicks > B) lf->second.inTicks = B;   // trimmed to the cut
                commit_fade(seq);
            }
        }
    }

    // LEFT half: trim the original's right edge back to the split point.
    r.position = P; r.source = S; r.length = B;
    m_region[seq] = r;
    if (on_clip_region_changed) on_clip_region_changed(seq, P, B, S);
}

void ArrangeView::split_clip_at(int laneSeq,long timelineTick)
{
    if(!m_perf||!m_perf->is_active(laneSeq))return;
    const long t=esnap(timelineTick);
    const int seq=clip_sequence_at(laneSeq,t);
    if(seq<0)return;
    if(m_audio.count(seq)) {
        const AudioRegion r=region_for(seq);
        if(t<=r.position||t>=r.position+r.length)return;
        push_undo("Split Clip");
        split_audio_clip(seq,t);
        return;
    }
    sequence* s=m_perf->get_sequence(seq);if(!s)return;
    // Find the clip under the cut by WALKING the triggers, not by selecting it
    // and asking for "the selected" bounds.  select_trigger only adds to the
    // selection, and get_selected_trigger_start/end_tick return the LAST
    // selected trigger in list order -- so with two clips already selected on
    // this lane, splitting the earlier one measured the later one's bounds and
    // the `t<=a||t>b` guard threw the edit away with no feedback.
    long a=-1,b=-1,aoff=0;
    {
        long on,off,offs; bool tsel;
        s->reset_draw_trigger_marker();
        while(s->get_next_trigger(&on,&off,&tsel,&offs))
            if(t>=on&&t<=off){a=on;b=off;aoff=offs;break;}
    }
    if(a<0||t<=a||t>b)return;
    push_undo("Split Clip");
    //  CONTENT MUST NOT JUMP AT THE CUT.  sequence::split_trigger() copies the
    //  trigger and only moves the edges, so the right half kept the LEFT half's
    //  offset.  play_span anchors a clip's content on its own start
    //  (anchor = trigger_start - offset), so an unchanged offset restarts the
    //  content at the cut: split a one-shot and the tail replayed the pattern
    //  from the top; split a looping clip whose window does not divide the cut
    //  distance and the second half came in on the wrong step.  The right half
    //  begins (t - a) ticks further into the content, so that is its offset.
    //
    //    LOOPING   -> fold it into the repetition (add_trigger's adjust does
    //                 exactly the fold play_span/set_trigger_offset use).
    //    ONE-SHOT  -> do NOT fold: a one-shot's offset is a trim-IN bounded by
    //                 the END marker, and wrapping it would resurrect data the
    //                 clip has already played.  Past the end it simply stops,
    //                 which is what a one-shot cut past its data should do.
    //  THE HALVES MUST BE INDEPENDENT CLIPS.  Both used to be triggers on the
    //  SAME sequence, which is seq24's model: one pattern, many placements.
    //  That makes the loop window and the loop-enabled flag shared state --
    //  toggling one half to one-shot toggled the other, and the two halves
    //  could never carry different loops.  Cutting a clip up and giving the
    //  pieces different loops is the whole point of a per-clip window, so the
    //  right half becomes its OWN sequence, copied from the source (which
    //  carries the window, the enable flag, the events and the scale-follow
    //  state through sequence::operator=) and free to diverge afterwards.
    const bool looping = s->get_loop_enabled();
    long rightOff = aoff + (t - a);
    if (looping) {
        // Fold into the repetition, the same fold play_span uses.  A one-shot
        // is NOT folded: its offset is a trim-in bounded by the END marker, and
        // wrapping would resurrect data the clip has already played.
        const long period = s->repeat_period();
        if (period > 0) rightOff = ((rightOff % period) + period) % period;
    }
    s->del_trigger(t);                        // drop the original [a, b]
    s->add_trigger(a, t - a, aoff, false);    // LEFT  [a, t-1] stays put
    const int rseq = create_pattern(seq, t, b - t + 1, rightOff, /*copy_events=*/true);
    if (rseq < 0) {
        // Out of sequence slots: fall back to the old shared-sequence split
        // rather than losing the right half entirely.
        s->add_trigger(t, b - t + 1, rightOff, false);
        s->select_trigger(t);
        return;
    }
    if (sequence* rs = m_perf->get_sequence(rseq)) {
        // create_pattern names copies "<name> copy N"; a split is not a copy.
        const char* nm = s->get_name();
        if (nm) rs->set_name(nm);
        // Auto-Name Separated Clips (ch.31 p727): the auto-created half gets
        // a numbered variation of the original's name.
        auto_name_clip(rseq, seq);
        rs->select_trigger(t);   // the new right half is selected, as before
    }
    //  NOT committed to the automation engine: it keeps ONE region per
    //  sequence, so pushing the left half's geometry would silence the right
    //  half.  The region still spans both, and auto_geom() now reads each
    //  block's OWN trigger offset, so the drawn curve matches what plays.
}

void ArrangeView::set_frozen(int seq, bool frozen)
{
    if (frozen) m_frozen.insert(seq); else m_frozen.erase(seq);
}

bool ArrangeView::is_frozen(int seq) const
{
    return m_frozen.count(seq) != 0;
}

int ArrangeView::create_pattern(int source_seq, long start, long length,
                                long offset, bool copy_events)
{
    if (!m_perf || !m_perf->is_active(source_seq)) return -1;
    if (length < 1) length = c_ppqn * 4;
    if (start < 0) start = 0;
    if (on_create_pattern) {
        const int newSeq = on_create_pattern(source_seq, start, length, offset, copy_events);
        // Original Time Stamp (ch.28 p662): where the clip was first created.
        if (newSeq >= 0 && !m_timestamp.count(newSeq)) m_timestamp[newSeq] = start;
        // Inherit the source's clip COLOUR + FADES so a copy / split reads as the
        // same clip instead of a different-hue, fade-less block (split may then
        // re-split the fades between the halves).
        if (newSeq >= 0 && newSeq != source_seq) {
            std::map<int,int>::const_iterator cit = m_clipColor.find(source_seq);
            m_clipColor[newSeq] = (cit != m_clipColor.end()) ? cit->second
                                  : (int)( ( (unsigned)source_seq * 5u + 3u ) % 12u );
            std::map<int,ClipFade>::const_iterator fit = m_clipFade.find(source_seq);
            if (fit != m_clipFade.end()) m_clipFade[newSeq] = fit->second;
        }
        // AUDIO clip: the pattern clone carries no audio -- propagate the region
        // + source clip to the copy so it plays back (not a silent block).
        if (newSeq >= 0 && newSeq != source_seq && m_audio.count(source_seq)) {
            const long copySource = region_for(source_seq).source;   // capture first
            // Shell attaches a new engine region for the SAME source audio and
            // sets m_audio(newSeq)=the copy's own clip (correct lifetime); this
            // also erases m_region[newSeq], so we set the display region AFTER.
            if (on_clip_duplicated) on_clip_duplicated(source_seq, newSeq, start, copySource, length);
            if (!m_audio.count(newSeq)) m_audio[newSeq] = m_audio[source_seq];   // fallback (no audio engine)
            std::map<int,long>::const_iterator lit = m_audioLen.find(source_seq);
            if (!m_audioLen.count(newSeq) && lit != m_audioLen.end()) m_audioLen[newSeq] = lit->second;
            AudioRegion r=region_for(source_seq);
            r.position=start; r.source=copySource; r.length=length; r.selected=true;
            m_region[newSeq] = r;               // survives the shell's set_audio_clip erase
        }
        return newSeq;
    }

    sequence* s = m_perf->get_sequence(source_seq);
    if (!s) return -1;
    push_undo("Add Clip");
    s->add_trigger(start, length, offset);
    return source_seq;
}

void ArrangeView::unselect_all_triggers()
{
    if (!m_perf) return;
    for(auto& kv:m_region) kv.second.selected=false;
    for (int seq = 0; seq < c_max_sequence; ++seq)
        if (m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq))
                s->unselect_triggers();
}

void ArrangeView::select_clips_in_rect(SDL_Rect box, bool add_to_selection)
{
    if (!m_perf) return;
    if (!add_to_selection) unselect_all_triggers();
    std::vector<ClipSpan> hits;
    for_each_visible_clip([&](const ClipSpan& s, const SDL_Rect& r) {
        if (rect_intersects(box, r)) hits.push_back(s);
    });
    for (size_t i = 0; i < hits.size(); ++i) {
        const ClipSpan& s = hits[i];
        if (m_audio.count(s.seq)) region_for(s.seq).selected = true;
        else if (sequence* sq = m_perf->get_sequence(s.seq)) sq->select_trigger(s.on);
    }
}

void ArrangeView::copy_selected_clips()
{
    m_clip_clipboard.clear();
    if (!m_perf) return;
    long min_start = -1;
    std::vector<ClipCopy> clips;
    for_each_clip([&](const ClipSpan& s) {
        if (!s.selected) return;
        ClipCopy cc;
        cc.seq = s.seq; cc.rel_start = s.on;
        cc.length = s.endEx - s.on; cc.offset = s.offset;
        clips.push_back(cc);
        if (min_start < 0 || s.on < min_start) min_start = s.on;
    });
    if (min_start < 0) return;
    long span = 0;
    for (size_t i = 0; i < clips.size(); ++i) {
        clips[i].rel_start -= min_start;
        if (clips[i].rel_start + clips[i].length > span)
            span = clips[i].rel_start + clips[i].length;
    }
    m_clip_span  = span;
    m_dup_origin = min_start;
    m_paste_tick = -1;               // a fresh copy restarts paste stepping
    m_clip_clipboard.swap(clips);
}

//  Give `dst` EXACTLY `src`'s loop window and loop-enabled flag.
//
//  A loop window is allowed to reach past its pattern's END marker: that is how
//  an odd-length loop is built (a 3-beat loop laid over a 2-beat phrase), and
//  sequence::set_length() HIDES such a window rather than destroying it,
//  precisely so it survives.  The setters no longer clamp to m_length either
//  (they used to, and the duplicate then came back with a shorter loop than the
//  clip it was copied from), so the bounds now travel verbatim.
//
//  What the setters DO still enforce is ordering, against the window already on
//  `dst` -- set_loop_end() will not accept an end below the current start, and
//  set_loop_start() will not accept a start above the current end.  `dst` is a
//  recycled sequence that arrives carrying whatever window it had before, so
//  writing the pair in either fixed order can hit that guard on the way
//  through: copying [0,1) onto a sequence sitting at [2,3) clamps the incoming
//  end UP to 2 and the copy is silently wrong again.  Park the start at the
//  floor first and no intermediate state can be inverted, whatever `dst` held.
//
//  The result is verified against the source, because "the copy is silently
//  different from its original" is precisely the failure this area keeps
//  producing and precisely the one nobody notices until playback.
static void copy_loop_window(sequence* dst, sequence* src)
{
    if (!dst || !src) return;
    const long ls = src->get_loop_start();
    const long le = src->get_loop_end();

    dst->set_loop_start(0);      // floor: the incoming end can now land anywhere
    dst->set_loop_end(le);       // verbatim, overhang past the END marker and all
    dst->set_loop_start(ls);     // ls <= le, so nothing to clamp against
    dst->set_loop_enabled(src->get_loop_enabled());

    if (dst->get_loop_start() != ls || dst->get_loop_end() != le ||
        dst->get_loop_enabled() != src->get_loop_enabled()) {
        fprintf(stderr,
                "[loop] COPY MISMATCH: source loop=[%ld,%ld) on=%d -> copy "
                "loop=[%ld,%ld) on=%d (copy length=%ld).  The copy will not "
                "play what its original plays.\n",
                ls, le, (int)src->get_loop_enabled(),
                dst->get_loop_start(), dst->get_loop_end(),
                (int)dst->get_loop_enabled(), dst->get_length());
    }
}

// Replace `toSeq`'s events with a faithful copy of `fromSeq`'s -- every status
// byte, not just notes, and the tracker column tags with them.  This is what
// lets a clip be pasted onto a DIFFERENT lane: the new pattern is created empty
// with the target lane's routing, then filled from the source.
void ArrangeView::clone_pattern_events(int fromSeq, int toSeq)
{
    if (!m_perf || fromSeq == toSeq) return;
    sequence* src = m_perf->get_sequence(fromSeq);
    sequence* dst = m_perf->get_sequence(toSeq);
    if (!src || !dst) return;

    std::vector<sequence::EventSnapshot> ev;
    src->snapshot_events(ev);

    dst->select_all();
    dst->mark_selected();
    dst->remove_marked();
    dst->set_length(src->get_length(), false);
    // Tracker parameter bindings and non-MIDI automation values live in this
    // per-pattern payload rather than in the event list.
    dst->set_fx_blob(src->get_fx_blob());
    // The clip's OWN loop points are pattern data too -- the piano roll's ruler
    // loop, one pair per sequence.  Copying only events and length reset the
    // pasted clip's loop to the default full-pattern span, so a clip pasted
    // onto another lane quietly lost the loop the user had set on it.
    // ...and whether it loops at all.  Without this a duplicated, pasted or
    // split ONE-SHOT clip silently came back as a looping one.
    copy_loop_window(dst, src);

    // set_event_column addresses an event by (tick, note, Nth occurrence), so
    // count the duplicates as they go in.
    std::map<std::pair<long,int>, int> occ;
    for (size_t i = 0; i < ev.size(); ++i) {
        const sequence::EventSnapshot& s = ev[i];
        dst->add_event(s.tick, s.status, s.d0, s.d1);
        const int n = occ[std::make_pair(s.tick, (int)s.d0)]++;
        if (s.column >= 0) dst->set_event_column(s.tick, (int)s.d0, n, s.column);
    }
    dst->verify_and_link();
}

void ArrangeView::paste_clips(long start_tick)
{
    if (!m_perf || m_clip_clipboard.empty()) return;
    start_tick = esnap(start_tick);
    if (start_tick < 0) start_tick = 0;

    // Repeated pastes at the SAME point used to stack every copy on top of the
    // previous one; step on by the clipboard's own span instead.
    if (start_tick == m_paste_tick && m_clip_span > 0) start_tick += m_clip_span;

    // Paste can retarget a lane: the focused lane is drawn as the paste target
    // (header spine + canvas frame), so it has to actually be one.  Only a
    // single-lane clipboard can be retargeted -- a multi-lane copy keeps its
    // own lane layout.
    int retarget = -1;
    if (m_focus_lane >= 0) {
        bool oneLane = true;
        const int k0 = lane_key(m_clip_clipboard[0].seq);
        for (const ClipCopy& cc : m_clip_clipboard)
            if (lane_key(cc.seq) != k0) { oneLane = false; break; }
        if (oneLane && k0 != m_focus_lane) {
            for (int seq : active_list())
                if (lane_key(seq) == m_focus_lane) { retarget = seq; break; }
        }
    }

    begin_undo_compound("Paste");
    push_undo("Paste");
    // SHUFFLE: pasting pushes all subsequent material right by the pasted
    // span on every destination lane (ch.29 p639).
    if (m_edit_mode == EditMode::Shuffle && m_clip_span > 0) {
        std::vector<int> lanes;
        for (const ClipCopy& cc : m_clip_clipboard) {
            const int lane = retarget >= 0 ? retarget : cc.seq;
            if (std::find(lanes.begin(), lanes.end(), lane_key(lane)) == lanes.end()) {
                lanes.push_back(lane_key(lane));
                ripple_lane(lane, start_tick, m_clip_span);
            }
        }
    } else if (m_clip_span > 0) {
        // PASTE OVERWRITES (ch.28 p668): outside Shuffle, the material under
        // the paste range is removed first -- separated at the boundaries so
        // the leftovers on either side become auto-created clips.
        const long pa = start_tick, pb = start_tick + m_clip_span;
        std::vector<int> lanes;
        for (const ClipCopy& cc : m_clip_clipboard) {
            const int lane = retarget >= 0 ? retarget : cc.seq;
            if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end())
                lanes.push_back(lane);
        }
        std::vector<ClipSpan> spans;
        for (int lane : lanes) {
            for (long cut : { pa, pb }) {
                const int cs = clip_sequence_at(lane, cut);
                if (cs < 0) continue;
                if (m_audio.count(cs)) {
                    const AudioRegion r = region_for(cs);
                    if (cut > r.position && cut < r.position + r.length)
                        split_audio_clip(cs, cut);
                } else if (sequence* ls = m_perf->get_sequence(cs)) {
                    long o, f, of; bool tsel; long on2 = -1, off2 = -1, offs2 = 0;
                    ls->reset_draw_trigger_marker();
                    while (ls->get_next_trigger(&o, &f, &tsel, &of))
                        if (cut > o && cut <= f) { on2 = o; off2 = f; offs2 = of; break; }
                    if (on2 >= 0) {
                        ls->del_trigger(cut);
                        ls->add_trigger(on2, cut - on2, offs2, false);
                        ls->add_trigger(cut, off2 - cut + 1, offs2 + (cut - on2), false);
                    }
                }
            }
            for (int cs : lane_sequences(lane)) {
                spans.clear();
                clip_spans(cs, spans);
                bool any = false;
                for (const ClipSpan& sp : spans)
                    if (sp.on >= pa && sp.endEx <= pb) any = true;
                if (!any) continue;
                if (m_audio.count(cs)) { delete_audio_clip_undoable(cs); continue; }
                if (sequence* ls = m_perf->get_sequence(cs)) {
                    for (const ClipSpan& sp : spans)
                        if (sp.on >= pa && sp.endEx <= pb) ls->del_trigger(sp.on);
                    if (on_midi_clip_delete) on_midi_clip_delete(cs);
                }
            }
        }
    }
    unselect_all_triggers();
    for (const ClipCopy& clip : m_clip_clipboard) {
        const long at = start_tick + clip.rel_start;
        int seq;
        if (retarget >= 0 && !m_audio.count(clip.seq)) {
            // Build on the TARGET lane (its routing), then copy the content in.
            seq = create_pattern(retarget, at, clip.length, clip.offset, false);
            if (seq >= 0) clone_pattern_events(clip.seq, seq);
        } else {
            seq = create_pattern(clip.seq, at, clip.length, clip.offset, true);
        }
        if (seq >= 0 && m_audio.count(seq)) region_for(seq).selected = true;
        else if (seq >= 0 && m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq)) s->select_trigger(at);
        // a CAPTURED clip carries the name it was captured under (ch.31 p725)
        if (seq >= 0 && !m_capture_name.empty() && m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq)) {
                s->set_name(m_capture_name);
                m_user_named.insert(seq);
            }
    }
    m_paste_tick = start_tick;
    end_undo_compound();
}

// Ctrl+X -- there was no cut at all, only copy.
void ArrangeView::cut_selected_clips()
{
    if (!any_selected_clip()) return;
    copy_selected_clips();
    delete_selected_clips();
}

// Ctrl+A -- there was no select-all either.
void ArrangeView::select_all_clips()
{
    if (!m_perf) return;
    for (int seq = 0; seq < c_max_sequence; ++seq) {
        if (!m_perf->is_active(seq)) continue;
        if (m_audio.count(seq)) { region_for(seq).selected = true; continue; }
        sequence* s = m_perf->get_sequence(seq);
        if (!s) continue;
        s->reset_draw_trigger_marker();
        long on, off, offs; bool sel;
        while (s->get_next_trigger(&on, &off, &sel, &offs)) s->select_trigger(on);
    }
}

// Ctrl+D -- duplicate the selection immediately after itself.
void ArrangeView::duplicate_selected_clips()
{
    if (!any_selected_clip()) return;
    std::vector<ClipCopy> saved = m_clip_clipboard;
    const long savedSpan = m_clip_span, savedTick = m_paste_tick;
    copy_selected_clips();
    if (!m_clip_clipboard.empty()) {
        long lo = -1;
        for (const ClipCopy& cc : m_clip_clipboard)
            if (lo < 0 || cc.rel_start < lo) lo = cc.rel_start;
        m_paste_tick = -1;                       // never auto-advance a duplicate
        paste_clips(m_dup_origin + m_clip_span);
    }
    m_clip_clipboard = saved; m_clip_span = savedSpan; m_paste_tick = savedTick;
}

void ArrangeView::delete_selected_clips()
{
    // Kept as the historic entry point; the body now routes through the
    // Multiple-Undo queue so audio teardowns and the trigger snapshot land
    // as ONE history entry.
    if (!m_perf) return;
    begin_undo_compound("Delete");
    edit_delete_guts();
    end_undo_compound();
}

bool ArrangeView::any_selected_clip() const
{
    bool any = false;
    for_each_clip([&](const ClipSpan& s) { if (s.selected) any = true; });
    return any;
}

int ArrangeView::selected_clip_count() const
{
    int n = 0;
    for_each_clip([&](const ClipSpan& s) { if (s.selected) ++n; });
    return n;
}

bool ArrangeView::clip_selected_at(int seq, long tick) const
{
    if (!m_perf) return false;
    std::vector<ClipSpan> spans;
    clip_spans(seq, spans);
    for (size_t i = 0; i < spans.size(); ++i)
        if (spans[i].selected && tick >= spans[i].on && tick < spans[i].endEx)
            return true;
    return false;
}

//  sequence::get_selected_trigger_start_tick() reports the LAST selected
//  trigger, not the earliest -- and the group delta has to be measured from the
//  earliest, exactly like the engine measures it inside
//  move_selected_triggers_to().  Compute it here rather than trust that.
long ArrangeView::selected_group_start(int seq) const
{
    if (!m_perf) return -1;
    std::vector<ClipSpan> spans;
    clip_spans(seq, spans);
    long best = -1;
    for (size_t i = 0; i < spans.size(); ++i)
        if (spans[i].selected && (best < 0 || spans[i].on < best)) best = spans[i].on;
    return best;
}

//  Move the whole selection rigidly.
//
//  Every lane-sequence in the group is asked for the SAME delta.  The engine
//  clamps per sequence (a selected clip cannot cross an unselected neighbour),
//  so a second pass re-applies the largest delta that every lane actually
//  achieved: without it the group shears, one lane sliding while another sits
//  jammed against its neighbour.
void ArrangeView::move_clip_group(long anchor_to)
{
    if (!m_perf || m_move_group.empty()) return;
    const long want = anchor_to - m_move_anchor;

    auto apply = [&](const MoveGroup& g, long delta) {
        if (g.audio) {
            AudioRegion& r = region_for(g.seq);
            long p = g.start + delta;
            if (p < 0) p = 0;
            r.position = p;
        } else if (sequence* gs = m_perf->get_sequence(g.seq)) {
            //  Absolute target measured off the ORIGINAL start, so each frame
            //  of the drag re-derives the same answer no matter what the
            //  previous frame left behind.
            gs->move_selected_triggers_to(g.start + delta, true);
        }
    };
    auto achieved = [&](const MoveGroup& g) -> long {
        if (g.audio) return region_for(g.seq).position - g.start;
        const long now = selected_group_start(g.seq);
        return now < 0 ? want : now - g.start;
    };

    for (size_t i = 0; i < m_move_group.size(); ++i) apply(m_move_group[i], want);

    long common = want;
    for (size_t i = 0; i < m_move_group.size(); ++i) {
        const long got = achieved(m_move_group[i]);
        if (want >= 0) { if (got < common) common = got; }
        else           { if (got > common) common = got; }
    }
    if (common != want)
        for (size_t i = 0; i < m_move_group.size(); ++i) apply(m_move_group[i], common);

    for (size_t i = 0; i < m_move_group.size(); ++i) {
        if (m_move_group[i].audio) commit_region(m_move_group[i].seq);
        else                       commit_auto_region(m_move_group[i].seq);
    }
}

//----------------------------------------------------------------------------
//  PEAK PYRAMID: a reduced-resolution overview (min / max / sum-of-squares per
//  fixed-size bin, in ~4x coarser levels) built ONCE per AudioClip and reused
//  by every draw_waveform() column rebuild.
//
//  Why: the column rebuild used to scan every SOURCE SAMPLE of the visible
//  window.  A 10-minute 48 kHz stereo clip is 57.6M sample reads per rebuild,
//  and a rebuild happens whenever the geometry key changes (any scroll, zoom,
//  or lane resize) -- and, with more distinct clip windows on screen than the
//  64-entry envelope cache holds, on EVERY frame for EVERY clip.  200 long
//  clips took ~30 s per frame: the reported hang.  With the pyramid a rebuild
//  reads a handful of bins per pixel instead, so it is O(clip width in px)
//  regardless of the source length.
//
//  The pyramid is APPEND-ONLY while a clip grows (the record preview grows its
//  clip every frame): new complete bins are folded in and coarser levels are
//  re-derived from level 0, so recording stays O(new material), not O(clip).
//  A pencil edit bumps g_wave_epoch, which drops and rebuilds the pyramid.
//  Message/UI thread only, like every other draw path.
//----------------------------------------------------------------------------
namespace {

struct WavePyramid {
    static const long long kBase = 256;          // frames per level-0 bin
    long long frames = 0;                        // complete-bin coverage, frames
    int       epoch  = 0;
    uint64_t  lastUse = 0;
    struct Level {
        long long bin = 0;                       // frames per bin at this level
        std::vector<float> mn[2], mx[2], ss[2];  // per channel: min, max, sum x^2
    };
    std::vector<Level> levels;

    size_t bytes() const {
        size_t b = 0;
        for (const Level& l : levels)
            for (int c = 0; c < 2; ++c)
                b += (l.mn[c].capacity() + l.mx[c].capacity() + l.ss[c].capacity())
                     * sizeof(float);
        return b;
    }
};

// Fold source frames [from, upto) of planar L/R into the level-0 bins (only
// complete bins), then re-derive every coarser level's tail from level 0.
void wave_pyramid_extend(WavePyramid& p, const float* L, const float* R,
                         long long upto)
{
    const long long kB = WavePyramid::kBase;
    if (p.levels.empty()) {
        p.levels.emplace_back();
        p.levels.back().bin = kB;
    }
    WavePyramid::Level& l0 = p.levels[0];
    const long long doneBins = (long long)l0.mn[0].size();
    const long long wantBins = upto / kB;                 // complete bins only
    if (wantBins <= doneBins) return;
    for (int c = 0; c < 2; ++c) {
        l0.mn[c].resize((size_t)wantBins);
        l0.mx[c].resize((size_t)wantBins);
        l0.ss[c].resize((size_t)wantBins);
    }
    for (long long b = doneBins; b < wantBins; ++b) {
        const float* src[2] = { L + b * kB, R + b * kB };
        for (int c = 0; c < 2; ++c) {
            float mn = src[c][0], mx = src[c][0];
            double ss = 0.0;
            for (long long i = 0; i < kB; ++i) {
                const float v = src[c][i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                ss += (double)v * v;
            }
            l0.mn[c][(size_t)b] = mn;
            l0.mx[c][(size_t)b] = mx;
            l0.ss[c][(size_t)b] = (float)ss;
        }
    }
    p.frames = wantBins * kB;
    // Coarser levels, each 4x the bin below, until a level is small enough
    // that a whole-clip draw touches only a few thousand bins.
    for (size_t li = 1; ; ++li) {
        const long long belowBins = (long long)p.levels[li - 1].mn[0].size();
        if (belowBins <= 4096) { p.levels.resize(li); break; }
        if (p.levels.size() <= li) {
            const long long belowBin = p.levels[li - 1].bin;
            p.levels.emplace_back();        // (may reallocate: no refs held)
            p.levels[li].bin = belowBin * 4;
        }
        WavePyramid::Level& below = p.levels[li - 1];
        WavePyramid::Level& lv = p.levels[li];
        const long long bins = belowBins / 4;             // complete bins only
        const long long start = (long long)lv.mn[0].size();
        for (int c = 0; c < 2; ++c) {
            lv.mn[c].resize((size_t)bins);
            lv.mx[c].resize((size_t)bins);
            lv.ss[c].resize((size_t)bins);
        }
        for (long long b = start; b < bins; ++b)
            for (int c = 0; c < 2; ++c) {
                float mn = below.mn[c][(size_t)(b * 4)];
                float mx = below.mx[c][(size_t)(b * 4)];
                double ss = 0.0;
                for (int k = 0; k < 4; ++k) {
                    const size_t j = (size_t)(b * 4 + k);
                    if (below.mn[c][j] < mn) mn = below.mn[c][j];
                    if (below.mx[c][j] > mx) mx = below.mx[c][j];
                    ss += (double)below.ss[c][j];
                }
                lv.mn[c][(size_t)b] = mn;
                lv.mx[c][(size_t)b] = mx;
                lv.ss[c][(size_t)b] = (float)ss;
            }
    }
}

// The pyramid for `clip`, built/extended to cover `n` frames.  A bounded cache:
// past ~128 MB of pyramids the least-recently-used clips are dropped (they
// rebuild in O(clip) the next time they are drawn, which is the cold cost).
WavePyramid* wave_pyramid(const PatchKnob::engine::AudioClip* clip,
                          const float* L, const float* R,
                          long long n, int epoch)
{
    static std::map<const PatchKnob::engine::AudioClip*, WavePyramid> cache;
    static uint64_t useTick = 0;
    WavePyramid& p = cache[clip];
    if (p.epoch != epoch || p.frames > n) {              // edited or shrunk
        p.levels.clear();
        p.frames = 0;
        p.epoch  = epoch;
    }
    p.lastUse = ++useTick;
    wave_pyramid_extend(p, L, R, n);
    // Evict LRU entries past the byte budget (never the one just used).
    size_t total = 0;
    for (std::map<const PatchKnob::engine::AudioClip*, WavePyramid>::iterator
             it = cache.begin(); it != cache.end(); ++it)
        total += it->second.bytes();
    const size_t kBudget = (size_t)128 * 1024 * 1024;
    while (total > kBudget && cache.size() > 1) {
        std::map<const PatchKnob::engine::AudioClip*, WavePyramid>::iterator
            oldest = cache.end();
        for (std::map<const PatchKnob::engine::AudioClip*, WavePyramid>::iterator
                 it = cache.begin(); it != cache.end(); ++it)
            if (it->first != clip &&
                (oldest == cache.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest == cache.end()) break;
        total -= oldest->second.bytes();
        cache.erase(oldest);
    }
    return &cache[clip];
}

} // namespace

//----------------------------------------------------------------------------
//  Overview access for the transient detector (arrange_protools.cpp): borrow
//  the clip's level-0 peak bins (256 frames per bin, channel 0).  Builds /
//  extends the cached pyramid exactly as draw_waveform() would, so a Tab
//  press and a redraw share one reduction.  Returns the number of bins (the
//  first bins*256 frames of the clip); 0 = nothing covered, caller scans raw.
//  The pointers alias the pyramid cache: use them immediately, do not hold
//  them across another pyramid call.  Message/UI thread only.
//----------------------------------------------------------------------------
long long wave_overview_ch0(const PatchKnob::engine::AudioClip* clip,
                            const float** mn, const float** mx)
{
    *mn = nullptr; *mx = nullptr;
    if (!clip || clip->ch[0].empty()) return 0;
    const float* tL = clip->ch[0].data();
    const float* tR = clip->ch[1].empty() ? tL : clip->ch[1].data();
    const long long n = clip->ch[1].empty()
        ? (long long)clip->ch[0].size()
        : (long long)std::min(clip->ch[0].size(), clip->ch[1].size());
    const WavePyramid* pyr = wave_pyramid(clip, tL, tR, n, g_wave_epoch);
    if (!pyr || pyr->levels.empty() || pyr->frames <= 0) return 0;
    const WavePyramid::Level& l0 = pyr->levels[0];
    *mn = l0.mn[0].data();
    *mx = l0.mx[0].data();
    return (long long)l0.mn[0].size();
}

//----------------------------------------------------------------------------
//  peak-based audio waveform inside a clip body (min/max per pixel column)
//----------------------------------------------------------------------------
void ArrangeView::draw_waveform(App& app, const PatchKnob::engine::AudioClip* clip,
                                int bx, int y, int bw, int h,
                                long long s0, long long s1,
                                int chan, bool ghost)
{
    long long n = (long long)clip->numFrames();
    if (n <= 0 || bw < 2 || h < 4) return;
    // Default (s1<0) = whole clip; else draw only the [s0,s1) sample window so a
    // trimmed trigger shows just its visible slice.
    if (s1 < 0) { s0 = 0; s1 = n; }
    if(s0<0)s0=0;
    if(s0>=n)s0=n-1;
    if(s1>n)s1=n;
    if(s1<=s0)s1=std::min(n,s0+1);
    // RAGGED CLIPS: `n` comes from channel 0, but a clip whose channels have
    // different lengths is reachable (a partly-recorded stereo take, a
    // half-restored project).  Indexing R with channel 0's length then reads
    // past the end of channel 1.  The pyramid path below already clamps; this
    // one did not, so clamp the SHARED span instead of trusting either channel.
    if (!clip->ch[1].empty()) {
        const long long nR = (long long)clip->ch[1].size();
        if (nR < n) {
            n = nR;
            if (s1 > n) s1 = n;
            if (s0 >= n) s0 = n > 0 ? n - 1 : 0;
            if (s1 <= s0) s1 = std::min(n, s0 + 1);
        }
    }
    const long long span = s1 - s0;
    const float* L = clip->ch[0].data();
    const float* R = clip->ch[1].empty() ? clip->ch[0].data() : clip->ch[1].data();
    if (chan == 0) R = L;
    else if (chan == 1 && !clip->ch[1].empty()) L = R;

    // ---- View > Waveforms (ch.28 p655-657) ---------------------------------
    // Peak view is ALWAYS shown at sample-level zoom (< ~2 samples per pixel
    // there is no meaningful RMS window) and while recording (the record
    // preview clip); rectified + outlines are dropped at sample zoom too.
    const bool sampleZoom = span < 2LL * (long long)bw;
    const bool forcedPeak = sampleZoom || waveform_forced_peak(clip);
    const bool power    = m_wf_power && !forcedPeak;
    const bool rect     = m_wf_rect  && !sampleZoom;
    const bool outlines = m_wf_outlines && !sampleZoom;

    // Ardour WaveView geometry (compute_tips, Normal shape): an even effective
    // height, amplitude mapped by y = (1 - a) * half about the centre line.
    const int H     = 2 * (int)std::floor((h - 1) * 0.5);   // even drawing height
    const int half  = H / 2;                                // == floor((h-1)/2): centre & scale
    const int zeroY = y + half;                             // zero / centre line

    // Data is SOLID BLACK on the bright clip body.  Faint centre line
    // (rectified view sits on the BOTTOM of the lane instead, p656).
    const Color waveColor{ 0, 0, 0, 255 };
    if (!ghost) {
        if (rect) hline(app.ren, bx, bx + bw - 1, y + h - 1, waveColor);
        else      hline(app.ren, bx, bx + bw - 1, zeroY, waveColor);
    }

    // Cache the reduced envelope by source window + pixel geometry. Playback
    // redraws the same clips 60 times/sec; rescanning every source sample on
    // every one-pixel playhead move was the arrange view's dominant CPU cost.
    struct CachedWave {
        const PatchKnob::engine::AudioClip* clip=nullptr;
        long long frames=0,s0=0,s1=0;int width=0,height=0;
        float wz=1.f;int epoch=0;      // vertical audio zoom + pencil edits
        int mode=0;                    // power|rect|chan (waveform view options)
        std::vector<short> top,bot;
    };
    const int mode = (power ? 1 : 0) | (rect ? 2 : 0) | ((chan + 1) << 2);
    static std::vector<CachedWave> cache;
    static size_t replace=0;
    CachedWave* hit=nullptr;
    for(CachedWave& c:cache)
        if(c.clip==clip&&c.frames==n&&c.s0==s0&&c.s1==s1&&
           c.width==bw&&c.height==h&&c.wz==m_wave_zoom&&
           c.epoch==g_wave_epoch&&c.mode==mode){hit=&c;break;}
    if(!hit) {
        // 1024 entries, not 64: a screen can easily show a few hundred clip
        // bodies (200 clips was routine in the report), and once the working
        // set exceeded the cache the round-robin replacement guaranteed a 100%
        // MISS rate -- every clip rebuilt its envelope every frame.  An entry
        // is two short-vectors of clip-width size, so even 1024 of them is a
        // couple of MB.  The linear key scan above stays cheap at this size.
        if(cache.size()<1024){cache.emplace_back();hit=&cache.back();}
        else {hit=&cache[replace++%cache.size()];}
        hit->clip=clip;hit->frames=n;hit->s0=s0;hit->s1=s1;
        hit->width=bw;hit->height=h;hit->top.resize((size_t)bw);
        hit->wz=m_wave_zoom;hit->epoch=g_wave_epoch;hit->mode=mode;
        hit->bot.resize((size_t)bw);
        // Columns come from the clip's PEAK PYRAMID whenever a pixel spans at
        // least a couple of level-0 bins; at higher zoom (or in a pyramid's
        // trailing partial bin) the raw samples are scanned, which is cheap
        // exactly because the window is then small.
        const long long spp = span / (bw > 0 ? bw : 1);   // source frames per px
        // Build the pyramid from the clip's TRUE channels (not the chan-view
        // redirected L/R above): it is cached per clip and serves every view.
        // Coverage is capped at the frames BOTH channels can supply, so a
        // ragged mid-edit clip never reads past the shorter buffer.
        const float* tL = clip->ch[0].data();
        const float* tR = clip->ch[1].empty() ? tL : clip->ch[1].data();
        const long long nPyr = clip->ch[1].empty()
            ? n : std::min<long long>(n, (long long)clip->ch[1].size());
        const WavePyramid* pyr = (spp >= 2 * WavePyramid::kBase)
            ? wave_pyramid(clip, tL, tR, nPyr, g_wave_epoch) : nullptr;
        const WavePyramid::Level* lev = nullptr;
        if (pyr && !pyr->levels.empty() && pyr->frames > 0) {
            lev = &pyr->levels[0];
            for (size_t li = 1; li < pyr->levels.size(); ++li)
                if (pyr->levels[li].bin * 2 <= spp) lev = &pyr->levels[li];
        }
        const int c0 = (chan == 1) ? 1 : 0;               // channels to fold in
        const int c1 = (chan == 0) ? 0 : 1;
        for (int px = 0; px < bw; ++px) {
            long long a0=s0+(long long)((double)px/bw*span);
            long long a1=s0+(long long)((double)(px+1)/bw*span);
            if(a1<=a0)a1=a0+1;if(a1>n)a1=n;
            float mn=1.f,mx=-1.f;
            // The pixel window as whole pyramid bins [b0,b1) + a raw tail for
            // anything past the pyramid's complete-bin coverage.
            long long rawFrom = a0;
            double acc = 0.0; long long cnt = 0;          // power accumulation
            bool any = false;
            if (lev) {
                const long long bins = (long long)lev->mn[0].size();
                long long b0 = a0 / lev->bin;
                long long b1 = (a1 + lev->bin - 1) / lev->bin;
                if (b0 < 0) b0 = 0;
                if (b1 > bins) b1 = bins;
                if (b1 > b0) {
                    if (power) {
                        for (int c = c0; c <= c1; ++c)
                            for (long long b = b0; b < b1; ++b)
                                acc += (double)lev->ss[c][(size_t)b];
                        cnt += (b1 - b0) * lev->bin * (c1 - c0 + 1);
                    } else {
                        mn = lev->mn[c0][(size_t)b0];
                        mx = lev->mx[c0][(size_t)b0];
                        for (int c = c0; c <= c1; ++c)
                            for (long long b = b0; b < b1; ++b) {
                                if (lev->mn[c][(size_t)b] < mn) mn = lev->mn[c][(size_t)b];
                                if (lev->mx[c][(size_t)b] > mx) mx = lev->mx[c][(size_t)b];
                            }
                    }
                    any = true;
                    rawFrom = b1 * lev->bin;              // bins covered to here
                }
            }
            if (rawFrom < a1) {                           // raw path / raw tail
                if (!any) { mn = 1.f; mx = -1.f; }
                if (power) {
                    for (long long i = rawFrom; i < a1; ++i) {
                        const float l = L[i], r = R[i];
                        acc += (double)l * l + (double)r * r; cnt += 2;
                    }
                } else {
                    for (long long i = rawFrom; i < a1; ++i) {
                        const float l = L[i], r = R[i];
                        if (l < mn) mn = l; if (l > mx) mx = l;
                        if (r < mn) mn = r; if (r > mx) mx = r;
                    }
                }
            }
            if (power) {
                // Power view: the column is the +-RMS of its sample window --
                // the Root-Mean-Square overview of the manual, more revealing
                // of sonic character than the raw peaks when zoomed out.
                const float rms = cnt ? (float)std::sqrt(acc / (double)cnt) : 0.f;
                mn = -rms; mx = rms;
            }
            // vertical AUDIO zoom: amplify then clamp (waveform magnifier)
            mn*=m_wave_zoom; mx*=m_wave_zoom;
            if(mn<-1.f)mn=-1.f;
            if(mn>1.f)mn=1.f;
            if(mx<-1.f)mx=-1.f;
            if(mx>1.f)mx=1.f;
            int top,bot;
            if (rect) {
                // Rectified view (p656): positive and negative excursions sum
                // to one positive-value signal rising from the lane BOTTOM.
                float a = std::max(std::fabs(mn), std::fabs(mx));
                if (a > 1.f) a = 1.f;
                top = (h - 1) - (int)std::lround((double)a * (double)(h - 1));
                bot = h - 1;
            } else {
                const double pmax=(1.0-(double)mx)*(double)half;
                const double pmin=(1.0-(double)mn)*(double)half;
                if(pmax*pmin<0.0){top=(int)std::ceil(pmax);bot=(int)std::floor(pmin);}
                else {top=(int)std::lround(pmax);bot=(int)std::lround(pmin);}
                if(top>bot)top=bot=(int)std::lround(.5*(top+bot));
            }
            hit->top[(size_t)px]=(short)std::max(0,std::min(h-1,top));
            hit->bot[(size_t)px]=(short)std::max(0,std::min(h-1,bot));
        }
    }

    // Batched into one FillRects call.
    static std::vector<SDL_Rect> cols;   // reused across frames to avoid realloc
    cols.clear(); cols.reserve((size_t)bw);
    for (int px = 0; px < bw; ++px) {
        const int ay0=y+hit->top[(size_t)px],ay1=y+hit->bot[(size_t)px];
        cols.push_back(SDL_Rect{ bx + px, ay0, 1, (ay1 - ay0) + 1 });
    }
    if (ghost) {
        // Overlapped Crossfades view: the OTHER clip's wave inside the fade,
        // translucent so both remain readable for visual alignment (p657).
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
        set_color(app.ren, Color{ 0, 0, 0, 90 });
        if (!cols.empty()) SDL_RenderFillRects(app.ren, cols.data(), (int)cols.size());
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        return;
    }
    set_color(app.ren, waveColor);
    if (!cols.empty()) SDL_RenderFillRects(app.ren, cols.data(), (int)cols.size());

    // ---- Outlines (p657): a contour along the envelope for definition ------
    // Off at sample zoom (precise editing wants the bare peaks).  Drawn in
    // the theme hi role so the contour reads against the solid black fill.
    if (outlines && bw > 2) {
        const Theme& t = theme();
        static std::vector<SDL_Point> line;   // reused across frames
        line.clear(); line.reserve((size_t)bw * 2);
        for (int px = 0; px < bw; ++px)
            line.push_back(SDL_Point{ bx + px, y + hit->top[(size_t)px] });
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
        Color oc = t.hi; oc.a = 140;
        set_color(app.ren, oc);
        SDL_RenderDrawLines(app.ren, line.data(), (int)line.size());
        if (!rect) {
            line.clear();
            for (int px = 0; px < bw; ++px)
                line.push_back(SDL_Point{ bx + px, y + hit->bot[(size_t)px] });
            SDL_RenderDrawLines(app.ren, line.data(), (int)line.size());
        }
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    }
}

//----------------------------------------------------------------------------
//  AUTOMATION clips: the region's curve, its loop repeats, and a live dot
//
//  The engine (AutomationPlayer::advanceRegions) evaluates a region at
//      local = (source + (T - position)) % loopLength
//  so a region LONGER than its loop repeats the curve, and `source` slides the
//  curve window under a trimmed / slipped block -- the automation twin of the
//  audio path's [s0,s1) source mapping.  Everything below is derived from that
//  one formula so the picture matches what is played, to the tick.
//----------------------------------------------------------------------------
long ArrangeView::AutoGeom::local_at(long tick) const
{
    const long lp = loop > 0 ? loop : 1;
    long m = (source + (tick - position)) % lp;
    if (m < 0) m += lp;             // C++ '%' truncates toward zero
    return m;
}

long ArrangeView::AutoGeom::repeat_at(long tick) const
{
    const long lp = loop > 0 ? loop : 1;
    const long raw = source + (tick - position);
    long q = raw / lp;
    if (raw % lp != 0 && raw < 0) --q;      // floor division
    return q;
}

// Geometry for ONE automation block, computed once per frame per clip and
// shared by the painter and the hover readout.
ArrangeView::AutoGeom ArrangeView::auto_geom(int seq, long tick_on, long tick_off,
                                             const SDL_Rect& body,
                                             long trig_offset) const
{
    AutoGeom g;
    if (!on_automation_info || !on_automation_sample) return g;
    if (body.w < 3 || body.h < 8) return g;
    const AutoClipInfo info = on_automation_info(seq);
    if (!info.valid || info.lanes < 1) return g;

    // The BLOCK is authoritative for where the clip sits (it is what the user
    // dragged); the REGION is authoritative for the curve window it shows.
    // commit_auto_region() keeps the engine's copy in step with the block.
    g.position = tick_on;
    g.length   = std::max<long>(1, tick_off - tick_on);
    g.loop     = std::max<long>(1, info.loopLength);
    // The curve tick under THIS block's start.  The engine keeps one region
    // per sequence, so info.source describes the sequence's FIRST block only;
    // after a split the lane carries two blocks on one sequence and the second
    // one drew the first one's window (its curve restarted instead of carrying
    // on).  The trigger's own offset is that block's source -- and for a
    // single-block sequence it is exactly info.source, so nothing else moves.
    g.source   = trig_offset >= 0 ? trig_offset
                                  : (info.source < 0 ? 0 : info.source);
    g.lanes    = info.lanes;
    g.focus    = info.focus < 0 ? 0 : (info.focus >= g.lanes ? 0 : info.focus);
    g.muted    = info.muted;

    // Inset: 1 px in from the body sides, and one text line clear of the top so
    // the curve never runs through the clip name.
    g.plot = body;
    g.plot.x += 1;  g.plot.w -= 2;
    g.plot.y += 2;  g.plot.h -= 4;
    if (g.plot.w < 2 || g.plot.h < 4) return g;

    g.tickL = x_to_tick(g.plot.x);
    g.tickR = x_to_tick(g.plot.x + g.plot.w - 1);
    if (g.tickR < g.tickL) g.tickR = g.tickL;
    g.ok = true;
    return g;
}

void ArrangeView::draw_automation(App& app, int seq, const AutoGeom& g, bool washed)
{
    if (!g.ok) return;
    const Theme& t = theme();
    const Color black{ 0, 0, 0, 255 };

    // One sample per pixel column: the x of sample i is plot.x + i, so the
    // curve uses the SAME tick->x mapping as the grid and the block edges.
    const int n = std::min(g.plot.w, 4096);
    if (n < 2) return;

    // Buffers live across frames: this runs for every visible clip, every frame.
    static std::vector<float>     vals;
    static std::vector<SDL_Point> pts;

    const int  top     = g.plot.y;
    const int  usableH = g.plot.h - 1;
    const auto y_of    = [&](float v) {
        if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
        return top + usableH - (int)((double)v * usableH + 0.5);
    };
    // The curve tick each column samples (the player's own wrap arithmetic).
    const auto col_tick = [&](int i) {
        return g.tickL + (long)(((long long)(g.tickR - g.tickL) * i) / (n - 1));
    };

    // ---- 1. the lanes, layered: focus lane solid, the rest translucent -----
    // Drawn back-to-front so the focus lane lands on top.  Every lane is one
    // sample call plus one RenderDrawLines per unbroken run.
    for (int pass = 0; pass < 2; ++pass) {
        for (int lane = 0; lane < g.lanes; ++lane) {
            const bool focus = (lane == g.focus);
            if ((pass == 0) == focus) continue;        // dim lanes first
            vals.clear();
            on_automation_sample(seq, lane, g.local_at(g.tickL),
                                 g.local_at(g.tickL) + (g.tickR - g.tickL),
                                 n, vals);
            if ((int)vals.size() < n) continue;

            if (!focus) {
                Color faint = black; faint.a = washed ? 60 : 96;
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                set_color(app.ren, faint);
            } else {
                set_color(app.ren, black);
            }
            // Break the polyline where the curve WRAPS, so a repeat reads as a
            // fresh pass instead of one long ramp joined across the seam.
            pts.clear(); pts.reserve((size_t)n);
            long prevRep = g.repeat_at(col_tick(0));
            for (int i = 0; i < n; ++i) {
                const long rep = g.repeat_at(col_tick(i));
                if (rep != prevRep && pts.size() > 1) {
                    SDL_RenderDrawLines(app.ren, pts.data(), (int)pts.size());
                    pts.clear();
                }
                prevRep = rep;
                pts.push_back(SDL_Point{ g.plot.x + i, y_of(vals[(size_t)i]) });
            }
            if (pts.size() > 1)
                SDL_RenderDrawLines(app.ren, pts.data(), (int)pts.size());
            if (!focus) SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        }
    }

    // ---- 2. the LOOP POINTS -------------------------------------------------
    // Every tick where the curve restarts, i.e. project tick
    //     position - source + k * loop
    // Drawn as a DOTTED vertical rule with a solid notch at the top edge, so it
    // is unmistakably not one of the canvas's solid bar lines.  Suppressed when
    // the repeats are too dense to read (they would become a wall).
    const double loopPx = (double)g.loop / m_scale_x;
    if (loopPx >= 6.0) {
        const long base = g.position - g.source;          // k == 0 boundary
        long k0 = (g.tickL - base) / g.loop;
        if (base + k0 * g.loop < g.tickL) ++k0;
        static std::vector<SDL_Rect> dots;
        dots.clear();
        int drawn = 0;
        for (long k = k0; drawn < 512; ++k) {
            const long bt = base + k * g.loop;
            if (bt > g.tickR) break;
            const int bxk = tick_to_x(bt);
            if (bxk <= g.plot.x || bxk >= g.plot.x + g.plot.w - 1) continue;
            for (int yy = g.plot.y; yy < g.plot.y + g.plot.h; yy += 3)
                dots.push_back(SDL_Rect{ bxk, yy, 1, 2 });
            // notch: a 3 px flag at the top of the block marks the restart
            dots.push_back(SDL_Rect{ bxk - 1, g.plot.y, 3, 2 });
            ++drawn;
        }
        if (!dots.empty()) {
            set_color(app.ren, black);
            SDL_RenderFillRects(app.ren, dots.data(), (int)dots.size());
        }
    }

    // ---- 3. LIVE: the value under the playhead ------------------------------
    // A dot riding the curve is what makes the block read as live; it moves
    // every frame the transport does, inside the playhead damage strip.
    const long ph = playhead();
    if (ph >= g.position && ph < g.position + g.length) {
        const int phx = tick_to_x(ph);
        if (phx >= g.plot.x && phx < g.plot.x + g.plot.w) {
            vals.clear();
            const long lt = g.local_at(ph);
            on_automation_sample(seq, g.focus, lt, lt, 1, vals);
            if (!vals.empty()) {
                const int dy = y_of(vals[0]);
                fill_rect(app.ren, SDL_Rect{ phx - 2, dy - 2, 5, 5 }, black);
                fill_rect(app.ren, SDL_Rect{ phx - 1, dy - 1, 3, 3 }, t.hi);
            }
        }
    }

    // ---- 4. hover readout ---------------------------------------------------
    // Shares g, so the number quoted is the value the curve is drawn at.
    if (!m_mouse_down && pt_in_rect(g.plot, m_mx, m_my)) {
        vals.clear();
        const long lt = g.local_at(x_to_tick(m_mx));
        on_automation_sample(seq, g.focus, lt, lt, 1, vals);
        if (!vals.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.3f  @ %s", (double)vals[0],
                          bbt(x_to_tick(m_mx)).c_str());
            tip(buf, m_mx + 12, m_my - app.mono.ch() - 10);
        }
    }

    // ---- 5. fade boundaries + shapes of the automated AUDIO clip (ch.32
    // p761: "Fade Boundaries and Shapes Displayed in Automation View").  The
    // shell resolves which audio lane this automation clip targets; its fade
    // spans are marked with boundary lines and a ghost of the fade curve, so
    // automation can be edited against the fades precisely.
    if (on_automation_dest_seq) {
        const int as = on_automation_dest_seq(seq);
        if (as >= 0 && m_audio.count(as)) {
            std::map<int, AudioRegion>::const_iterator ri = m_region.find(as);
            std::map<int, ClipFade>::const_iterator fi = m_clipFade.find(as);
            if (ri != m_region.end() && fi != m_clipFade.end()) {
                const AudioRegion& rr = ri->second;
                const ClipFade& f = fi->second;
                auto ghost_curve = [&](long a, long b, bool rising, int shape,
                                       float k, int slope) {
                    if (b <= a) return;
                    int xa = tick_to_x(a), xb = tick_to_x(b);
                    if (xb <= g.plot.x || xa >= g.plot.x + g.plot.w) return;
                    Color c = t.accent; c.a = 120;
                    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                    set_color(app.ren, c);
                    vline(app.ren, std::max(xa, g.plot.x), g.plot.y,
                          g.plot.y + g.plot.h - 1, c);
                    vline(app.ren, std::min(xb, g.plot.x + g.plot.w - 1),
                          g.plot.y, g.plot.y + g.plot.h - 1, c);
                    int px = -1, py = -1;
                    for (int x = std::max(xa, g.plot.x);
                         x <= std::min(xb, g.plot.x + g.plot.w - 1); ++x) {
                        const float u = (float)(x - xa) / (float)std::max(1, xb - xa);
                        const float lvl = fade_curve(rising ? u : 1.f - u, shape, k, slope);
                        const int y = g.plot.y + g.plot.h - 1 -
                                      (int)(lvl * (g.plot.h - 1));
                        if (px >= 0) SDL_RenderDrawLine(app.ren, px, py, x, y);
                        px = x; py = y;
                    }
                    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
                };
                if (f.inTicks > 0)
                    ghost_curve(rr.position, rr.position + f.inTicks, true,
                                f.inShape, f.inK, f.inSlope);
                if (f.outTicks > 0)
                    ghost_curve(rr.position + rr.length - f.outTicks,
                                rr.position + rr.length, false,
                                f.outShape, f.outK, f.outSlope);
            }
        }
    }
}

// The engine's automation region must follow the block the user just dragged,
// or the curve window (source) drawn here and the automation actually played
// drift apart.  Audio clips get this from commit_region(); automation clips are
// seq24-trigger clips, so their geometry is read back off the trigger.
void ArrangeView::commit_auto_region(int seq)
{
    if (!on_clip_region_changed || !is_automation(seq)) return;
    if (!m_perf || !m_perf->is_active(seq)) return;
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    s->reset_draw_trigger_marker();
    long on, off, offs; bool sel;
    if (!s->get_next_trigger(&on, &off, &sel, &offs)) return;
    if (on < 0) on = 0;
    on_clip_region_changed(seq, on, std::max<long>(1, off - on + 1),
                           offs < 0 ? 0 : offs);
}

//----------------------------------------------------------------------------
//  clip crossfades: draggable fade-in/out with a bendable curve point
//----------------------------------------------------------------------------
void ArrangeView::set_clip_fade(int seq, const ClipFade& in)
{
    ClipFade f = in;
    if (f.inTicks  < 0) f.inTicks  = 0;
    if (f.outTicks < 0) f.outTicks = 0;
    // Neither fade may exceed the clip; a clip whose two halves belong to
    // CROSSFADES may legitimately carry fades over its whole span (the
    // overlap windows), so the old half-clip clamp only applies when the
    // fades are free-standing (no overlapping lane-mate on that side).
    std::map<int,AudioRegion>::const_iterator ri = m_region.find(seq);
    if (ri != m_region.end() && ri->second.length > 1) {
        const long len = ri->second.length;
        if (f.inTicks  > len) f.inTicks  = len;
        if (f.outTicks > len) f.outTicks = len;
        if (f.inTicks + f.outTicks > len) {          // meet, never overlap
            const long half = len / 2;
            if (f.inTicks  > half) f.inTicks  = half;
            if (f.outTicks > len - f.inTicks) f.outTicks = len - f.inTicks;
        }
    }
    auto cl = [](float v){ return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
    f.inK = cl(f.inK); f.outK = cl(f.outK);
    auto cs = [](int v, int hi){ return v < 0 ? 0 : (v > hi ? hi : v); };
    f.inShape = cs(f.inShape, 8); f.outShape = cs(f.outShape, 8);
    f.inSlope = cs(f.inSlope, 1); f.outSlope = cs(f.outSlope, 1);
    f.link = cs(f.link, 2);
    m_clipFade[seq] = f;
}

void ArrangeView::set_clip_fade(int seq, long inTicks, long outTicks, float inK, float outK)
{
    ClipFade f = fade_of(seq);
    f.inTicks = inTicks; f.outTicks = outTicks; f.inK = inK; f.outK = outK;
    set_clip_fade(seq, f);
}


void ArrangeView::draw_clip_fades(App& app, int seq, const SDL_Rect& clip)
{
    const Theme& t = theme();
    std::map<int, ClipFade>::const_iterator it = m_clipFade.find(seq);
    ClipFade f = (it != m_clipFade.end()) ? it->second : ClipFade{};

    const int inW  = std::min(clip.w / 2, std::max(0, (int)(f.inTicks  / m_scale_x)));
    const int outW = std::min(clip.w / 2, std::max(0, (int)(f.outTicks / m_scale_x)));
    const int top = clip.y, bot = clip.y + clip.h;

    auto draw_fade = [&](int x0, int w, float k, bool rising, int shape, int slope) {
        if (w < 2) return;
        // gain(u): rising 0->1 for fade-in; for fade-out we mirror u.  The
        // curve is the ENGINE's (fadeCurve), so what is drawn is what plays.
        SDL_Point pts[33];
        const int N = 32;
        for (int i = 0; i <= N; ++i) {
            const float u = (float)i / N;
            const float g = fade_curve(rising ? u : (1.f - u), shape, k, slope);
            const int px = x0 + (int)(u * w);
            const int py = bot - (int)(g * clip.h);
            pts[i] = SDL_Point{ px, py };
        }
        set_color(app.ren, t.accent);
        SDL_RenderDrawLines(app.ren, pts, N + 1);
        // length handle (top corner) + curve point (mid)
        const int hx = rising ? x0 + w : x0;
        SDL_Rect handle{ hx - 3, top - 1, 6, 6 };
        fill_rect(app.ren, handle, t.accent);
        const float gm = fade_curve(0.5f, shape, k, slope);
        const int cx = x0 + w / 2;
        const int cy = bot - (int)(gm * clip.h);
        SDL_Rect cp{ cx - 3, cy - 3, 6, 6 };
        fill_rect(app.ren, cp, t.notesel);
        frame_rect(app.ren, cp, t.accent);
    };
    draw_fade(clip.x, inW, f.inK, true, f.inShape, f.inSlope);
    draw_fade(clip.x + clip.w - outW, outW, f.outK, false, f.outShape, f.outSlope);
    // With no fade set, draw_fade bails (w < 2) and the grab point was invisible.
    // Mark both corners so the handle can be found before it exists.
    if (inW < 2 && clip.w > 24)
        fill_rect(app.ren, SDL_Rect{ clip.x + 1, top, 4, 3 }, t.accent);
    if (outW < 2 && clip.w > 24)
        fill_rect(app.ren, SDL_Rect{ clip.x + clip.w - 5, top, 4, 3 }, t.accent);
}

// Height of the clip's top strip that belongs to the fade handles rather than
// to the trim zones.
static const int kFadeStripH = 9;

// Which fade handle (if any) is under (mx,my) for this clip.
ArrangeView::FadeGrab ArrangeView::fade_at(int seq, const SDL_Rect& clip, int mx, int my) const
{
    std::map<int, ClipFade>::const_iterator it = m_clipFade.find(seq);
    ClipFade f = (it != m_clipFade.end()) ? it->second : ClipFade{};
    const int inW  = std::min(clip.w / 2, std::max(0, (int)(f.inTicks  / m_scale_x)));
    const int outW = std::min(clip.w / 2, std::max(0, (int)(f.outTicks / m_scale_x)));
    const int top = clip.y, bot = clip.y + clip.h;
    auto near = [](int x, int y, int px, int py, int r) {
        return std::abs(x - px) <= r && std::abs(y - py) <= r;
    };
    // curve points first (they sit inside the clip, over the length handles)
    if (inW >= 8) {
        const int cx = clip.x + inW / 2;
        const int cy = bot - (int)(fade_curve(0.5f, f.inShape, f.inK, f.inSlope) * clip.h);
        if (near(mx, my, cx, cy, 6)) return FadeGrab::InCurve;
    }
    if (outW >= 8) {
        const int cx = clip.x + clip.w - outW + outW / 2;
        const int cy = bot - (int)(fade_curve(0.5f, f.outShape, f.outK, f.outSlope) * clip.h);
        if (near(mx, my, cx, cy, 6)) return FadeGrab::OutCurve;
    }
    // Length handles live in the clip's TOP STRIP only.  They used to be
    // grabbable within 7 px of the top corner, which sits inside the 6 px trim
    // zone and is checked first -- so an audio clip could never be trimmed by
    // its left edge.  Restricting them vertically keeps "pull a fade out of
    // nothing" while leaving the edges to trim.
    if (my <= top + kFadeStripH) {
        if (near(mx, my, clip.x + inW, top, 7)) return FadeGrab::InLen;
        if (near(mx, my, clip.x + clip.w - outW, top, 7)) return FadeGrab::OutLen;
    }
    return FadeGrab::None;
}

void ArrangeView::commit_fade(int seq)
{
    if (!on_clip_fade) return;
    ClipFade f = m_clipFade.count(seq) ? m_clipFade[seq] : ClipFade{};
    on_clip_fade(seq, f);
}

// Locate an audio clip's on-screen body rect (first trigger) for fade hit-test.
bool ArrangeView::clip_rect_span(int seq, long on, long endEx, SDL_Rect& out) const
{
    if (!m_perf || !m_perf->is_active(seq)) return false;
    if (!m_perf->get_sequence(seq)) return false;
    std::vector<int> act = active_list();
    int lane_y = -1;
    for (size_t i = 0; i < act.size(); ++i) {
        for (int cs : lane_sequences(act[i]))
            if (cs == seq) { lane_y = row_top((int)i - m_v_offset); break; }
        if (lane_y >= 0) break;
    }
    if (lane_y < 0) return false;
    const int cvx = canvas_x(), cvr = canvas_x() + canvas_w();
    const int x_on = tick_to_x(on), x_off = tick_to_x(endEx);
    const int bx = x_on < cvx ? cvx : x_on;
    const int br = x_off > cvr ? cvr : x_off;
    out = SDL_Rect{ bx, lane_y + 3, std::max(2, br - bx), track_h(seq) - 6 };
    return true;
}

bool ArrangeView::clip_rect_of(int seq, SDL_Rect& out) const
{
    if (!m_perf || !m_perf->is_active(seq)) return false;
    std::vector<ClipSpan> spans;
    clip_spans(seq, spans);
    if (spans.empty()) return false;
    return clip_rect_span(seq, spans[0].on, spans[0].endEx, out);
}

// The LOOP / ONE-SHOT chip in a clip's top-right corner.  ONE definition, used
// by the painter and by the press hit-test on both the audio and the MIDI path:
// they used to spell the rect out twice, with a one-pixel disagreement, so the
// glyph's left column was dead and the pixel left of it toggled.
SDL_Rect ArrangeView::loop_chip_rect(const SDL_Rect& body)
{
    if (body.w <= 20 || body.h < 14) return SDL_Rect{ body.x, body.y, 0, 0 };
    return SDL_Rect{ body.x + body.w - 14, body.y + 1, 12, 12 };
}

//----------------------------------------------------------------------------
//  ADD-TRACK "+" affordance : an extra header row below the last track.  It sits
//  directly under the last visible header; if the tracks overflow the column it
//  is pinned to the very bottom (its own opaque cell, so it never looks like it
//  overlaps a lane).  Always present -- even with 0 tracks.
//----------------------------------------------------------------------------
SDL_Rect ArrangeView::add_row_rect() const
{
    int n = m_perf ? (int)active_list().size() : 0;
    int r_add = n - m_v_offset;                    // on-screen row past last header
    if (r_add < 0) r_add = 0;
    int y = row_top(r_add);                        // cumulative bottom of all lanes
    int col_bottom = rect.y + rect.h;
    if (y + row_h > col_bottom) y = col_bottom - row_h;   // overflow -> pin bottom
    if (y < canvas_y())         y = canvas_y();
    return SDL_Rect{ rect.x, y, header_w, row_h };
}

// Per-track remove "x": a small box carrying two crossed line strokes.
void ArrangeView::draw_remove_btn(App& app, SDL_Rect box, bool hot)
{
    const Theme& t = theme();
    fill_rect (app.ren, box, hot ? t.accent : t.panel);
    frame_rect(app.ren, box, hot ? t.hi     : t.dim);
    Color xc = hot ? t.bg : t.dim;
    int pad = 3;
    int x0 = box.x + pad,              y0 = box.y + pad;
    int x1 = box.x + box.w - 1 - pad,  y1 = box.y + box.h - 1 - pad;
    set_color(app.ren, xc);
    SDL_RenderDrawLine(app.ren, x0,     y0, x1, y1);      // "\" stroke
    SDL_RenderDrawLine(app.ren, x1,     y0, x0, y1);      // "/" stroke
    SDL_RenderDrawLine(app.ren, x0 + 1, y0, x1, y1 - 1);  // 2 px thickening
    SDL_RenderDrawLine(app.ren, x1 - 1, y0, x0, y1 - 1);
}

//----------------------------------------------------------------------------
//  per-lane meter ballistics (ui::meter, the Ardour port)
//----------------------------------------------------------------------------
// One State per (track, channel).  It is kept here rather than on ArrangeView
// because arrange_view.h is not part of this change; the map is tiny (two
// entries per visible lane) and keyed by a stable sequence number.
namespace {
std::map<int, ui::meter::State> g_arrangeMeters;
double g_meter_dt      = 0.0;      // real seconds since the previous header draw
Uint64 g_meter_ticks   = 0;

ui::meter::State& arrange_meter_state(int seq, int ch) {
    return g_arrangeMeters[seq * 2 + ch];
}
void begin_meter_frame() {
    const Uint64 now = SDL_GetTicks();
    g_meter_dt = (g_meter_ticks && now > g_meter_ticks)
               ? double(now - g_meter_ticks) / 1000.0 : 0.0;
    g_meter_ticks = now;
}
} // namespace

//----------------------------------------------------------------------------
//  left track-header column (ports perfnames::draw_sequence)
//----------------------------------------------------------------------------
void ArrangeView::draw_headers(App& app, const std::vector<int>& act)
{
    const Theme& t = theme();
    begin_meter_frame();                   // one dt shared by every lane meter
    const HeaderGeom g = header_geom();
    const int spine_w = g.spine_w, badge_x = g.badge_x, badge_w = g.badge_w;
    const int name_x = g.name_x, rm_w = g.rm_w, rm_h = g.rm_h;
    const int rm_x = g.rm_x, rm_y = g.rm_y;
    const int btn_w = g.btn_w, btn_h = g.btn_h, btn_x = g.btn_x;
    const int vu_w = g.vu_w, vu_x = g.vu_x;

    for (int r = 0, y0 = canvas_y(); y0 < canvas_y() + canvas_h(); ++r) {
        int idx = m_v_offset + r;
        const int lh = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;

        SDL_Rect hdr{ rect.x, y0, header_w, lh };
        Color bg = (r % 2 == 0) ? t.panel : t.bg;
        fill_rect(app.ren, hdr, bg);
        // Hovering a header row lifts it, so the row under the pointer -- the
        // one every M / S / R / x hit will apply to -- is unambiguous.
        const bool rowHot = (m_mx >= hdr.x && m_mx < hdr.x + hdr.w &&
                             m_my >= y0 && m_my < y0 + lh);
        if (rowHot) {
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
            Color lift = t.hi; lift.a = 16;
            fill_rect(app.ren, hdr, lift);
            SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
        }
        hline(app.ren, hdr.x, hdr.x + hdr.w, y0 + lh - 1, t.dim);

        if (idx >= 0 && idx < (int)act.size()) {
        int seq = act[idx];
        sequence* s = m_perf->get_sequence(seq);
        if (s) {
        bool muted = s->get_song_mute();

        // The focused lane (last touched, and the paste target) keeps a solid
        // accent spine so the canvas highlight has a matching anchor here.
        const bool focused = (m_focus_lane >= 0 && lane_key(seq) == m_focus_lane);
        if (focused) frame_rect(app.ren, SDL_Rect{ hdr.x, y0, hdr.w, lh }, t.accent);

        // status spine
        fill_rect(app.ren, SDL_Rect{ hdr.x, y0 + 1, spine_w, lh - 2 },
                  muted ? t.dim : (focused ? t.accent : t.hi));

        // type badge: AUD for audio (waveform) tracks, INS for instrument tracks
        const bool is_audio = m_audio.count(seq) != 0;
        const bool is_auto = is_automation(seq);
        SDL_Rect badge{ hdr.x + badge_x, y0 + 4, badge_w, std::max(8, std::min(lh - 8, 20)) };
        if (badge.y + badge.h < y0 + lh) {
            frame_rect(app.ren, badge, t.dim);
            app.mono.draw_centered(app.ren, badge, is_audio ? "AUD" : (is_auto ? "AUT" : "INS"), t.accent);
        }
        SDL_Rect recBtn{ hdr.x + badge_x, y0 + 28, badge_w, 14 };
        const bool recArmed = is_track_record_armed && is_track_record_armed(seq);
        if (recBtn.y + recBtn.h < y0 + lh) {
            draw_button(app, recBtn, "R", recArmed);
            if (m_mx >= recBtn.x && m_mx < recBtn.x + recBtn.w &&
                m_my >= recBtn.y && m_my < recBtn.y + recBtn.h)
                tip(recArmed ? "Record-armed - click to disarm"
                             : "Arm this track for recording", m_mx + 12, m_my + 16);
        }

        // name (line 1)
        std::string name = std::to_string(seq + 1);
        if (seq + 1 < 10) name = "0" + name;
        name += " ";
        name += s->get_name() ? s->get_name() : "";
        app.mono.draw(app.ren, hdr.x + name_x, y0 + 5,
                      fit_text(app.mono, name, std::max(1, vu_x - name_x - 8)), t.text);

        // line 2: INSTRUMENT dropdown (instrument tracks) or bus/ch (audio tracks)
        if (!is_audio && !is_auto && on_track_instrument) {
            SDL_Rect ib = instr_box_rect(y0, app.mono.ch());
            bool hot = (m_mx >= ib.x && m_mx < ib.x + ib.w &&
                        m_my >= ib.y && m_my < ib.y + ib.h);
            fill_rect(app.ren, ib, hot ? t.accent : t.panel);
            frame_rect(app.ren, ib, t.dim);
            std::string in = on_track_instrument(seq);
            // fit the name, leaving room for the "v" chevron on the right.
            const int chev_w = app.mono.cw() + 4;
            app.mono.draw(app.ren, ib.x + 4, ib.y + 2,
                          fit_text(app.mono, in, ib.w - 8 - chev_w), hot ? t.bg : t.text);
            app.mono.draw(app.ren, ib.x + ib.w - chev_w, ib.y + 2, "v", hot ? t.bg : t.accent);
        } else {
            char info[48];
            std::snprintf(info, sizeof(info), "b%d ch%d  %ld/%ld",
                          s->get_midi_bus(), s->get_midi_channel() + 1,
                          s->get_bpm(), s->get_bw());
            app.mono.draw(app.ren, hdr.x + name_x, y0 + 6 + app.mono.ch(),
                          fit_text(app.mono, info, std::max(1, vu_x - name_x - 8)), t.dim);
        }
        if (!is_auto && (on_track_io_label || on_track_record_input) && lh >= 64) {
            // operator[] INSERTS -- this ran every frame, for every track, from
            // inside draw.  Look it up instead.
            std::map<int,int>::const_iterator tabIt = m_ioTab.find(seq);
            const int tab = (tabIt == m_ioTab.end() ? 0 : tabIt->second) & 1;
            const char* tabs[]={"MIDI","AUDIO"};
            const int ty=y0+2*app.mono.ch()+13;
            const int avail=std::max(72,vu_x-name_x-8);
            const int tabw=(avail-2)/2;
            for(int ti=0;ti<2;++ti)
                draw_button(app,SDL_Rect{hdr.x+name_x+ti*(tabw+2),ty,tabw,12},
                            tabs[ti],ti==tab);
            const int gap=3, boxw=(avail-gap)/2;
            SDL_Rect boxes[2] = {
                SDL_Rect{hdr.x+name_x,ty+14,boxw,app.mono.ch()+4},
                SDL_Rect{hdr.x+name_x+boxw+gap,ty+14,avail-boxw-gap,app.mono.ch()+4}
            };
            for(int io=0;io<2;++io) {
                fill_rect(app.ren,boxes[io],t.panel);
                frame_rect(app.ren,boxes[io],t.dim);
                const int route = tab*2+io;
                const std::string value = on_track_io_label
                    ? on_track_io_label(seq,route) : on_track_record_input(seq);
                const std::string label = std::string(io==0 ? "I:" : "O:") + value;
                app.mono.draw(app.ren,boxes[io].x+3,boxes[io].y+2,
                              fit_text(app.mono,label,boxes[io].w-app.mono.cw()-8),t.text);
                app.mono.draw(app.ren,boxes[io].x+boxes[io].w-app.mono.cw()-2,
                              boxes[io].y+2,"v",t.accent);
            }
        }

        // Live post-mixer VU for the lane.
        int vu_y = y0 + 4, vu_h = lh - 8;
        const SDL_Rect meter{ hdr.x + vu_x, vu_y, vu_w, vu_h };
        fill_rect(app.ren, meter, t.keybg);
        // The hand-rolled log-meter lambda that used to live here is now the
        // Ardour port in sdlui/meter.{h,cpp}: same log_meter() scaling, but with
        // Ardour's ballistics (instant attack, 20 dB/s falloff), its peak-hold
        // bar and its gradient instead of three flat theme colours.
        for (int ch = 0; ch < 2; ++ch) {
            ui::meter::State& ms = arrange_meter_state(seq, ch);
            ui::meter::update(ms, muted ? 0.f
                                        : (on_track_level ? on_track_level(seq, ch) : 0.f),
                              g_meter_dt);
            if (muted) continue;                        // muted lanes read blank
            // Keep the shared renderer inside the enclosing well.  It paints
            // its complete destination background, so using meter.y/meter.h
            // here used to erase the top and bottom of the widget itself.
            const SDL_Rect bar{ meter.x + 1 + ch * 6, meter.y + 1, 6,
                                std::max(1, meter.h - 2) };
            // Animates every frame -> must register, or a damage-clipped frame
            // freezes this lane's level.  Cover the enclosing well so the
            // chrome drawn around it below is repainted with it.
            app.add_damage(meter);
            ui::meter::draw(app.ren, bar, ms, ui::meter::Peak, /*vertical=*/true);
        }
        // Draw chrome last so a silent stereo meter remains an unmistakable
        // visible UI object rather than merging into the track-header panel.
        vline(app.ren, meter.x + 7, meter.y + 1, meter.y + meter.h - 2, t.dim);
        frame_rect(app.ren, meter, t.text);

        // Mute / Solo toggle buttons
        SDL_Rect mbox{ hdr.x + btn_x, y0 + 4, btn_w, btn_h };
        SDL_Rect sbox{ hdr.x + btn_x, y0 + 4 + btn_h + 2, btn_w, btn_h };
        draw_button(app, mbox, "M", muted);
        draw_button(app, sbox, "S", m_solo[seq] != 0);
        if (m_mx >= mbox.x && m_mx < mbox.x + mbox.w &&
            m_my >= mbox.y && m_my < mbox.y + mbox.h)
            tip(muted ? "Unmute track" : "Mute track", m_mx + 12, m_my + 16);
        if (m_mx >= sbox.x && m_mx < sbox.x + sbox.w &&
            m_my >= sbox.y && m_my < sbox.y + sbox.h)
            tip(m_solo[seq] ? "Un-solo track" : "Solo track", m_mx + 12, m_my + 16);

        // remove "x" button (top-right corner of the header cell)
        SDL_Rect xbox{ hdr.x + rm_x, y0 + rm_y, rm_w, rm_h };
        bool xhot = (m_mx >= xbox.x && m_mx < xbox.x + xbox.w &&
                     m_my >= xbox.y && m_my < xbox.y + xbox.h);
        draw_remove_btn(app, xbox, xhot);
        if (xhot) tip("Delete this track", m_mx + 12, m_my + 16);

        // LANE-RESIZE grip: the bottom edge of the header (drag to resize).
        const bool rhot = (m_mx >= hdr.x && m_mx < hdr.x + header_w &&
                           m_my >= y0 + lh - 4 && m_my <= y0 + lh);
        if (rhot) { m_hover_resize = true; tip("Drag to resize lane", m_mx + 12, m_my - 20); }
        Color grip = (rhot || (m_hdr_resize && m_hdr_resize_seq == seq)) ? t.hi : t.dim;
        for (int gx = hdr.x + header_w/2 - 8; gx <= hdr.x + header_w/2 + 8; gx += 4)
            fill_rect(app.ren, SDL_Rect{ gx, y0 + lh - 3, 2, 2 }, grip);
        }  // if (s)
        }  // if (idx valid)
        y0 += lh;
    }

    // The blank area below the last header is the add-track target.  It used to
    // be completely unmarked, so the only way to find it was to already know.
    {
        SDL_Rect ar = add_row_rect();
        if (ar.h > app.mono.ch() + 6 && ar.y + ar.h <= rect.y + rect.h) {
            const std::string hint = "right-click to add a track";
            const int hw = app.mono.text_w(hint);
            if (hw < ar.w - 8)
                app.mono.draw(app.ren, ar.x + (ar.w - hw) / 2,
                              ar.y + (ar.h - app.mono.ch()) / 2, hint, t.dim);
        }
    }
}

// Show the vertical-resize system cursor while over (or dragging) a lane's
// bottom edge; restore the arrow otherwise.  Cursors are created once.
// Build a 32x32 loop-cursor bitmap once: an elliptical ring (the "loop") with a
// small arrowhead, drawn white with a black outline so it reads on any backdrop.
static SDL_Cursor* make_loop_cursor()
{
    const int W = 32, H = 32;
    static Uint32 pix[32 * 32];
    for (int i = 0; i < W * H; ++i) pix[i] = 0;               // transparent
    const Uint32 white = 0xFFFFFFFFu, black = 0xFF000000u;
    const double cx = 15.5, cy = 15.5, rx = 10.0, ry = 7.0;
    auto plot = [&](int x, int y, Uint32 c) {
        if (x >= 0 && x < W && y >= 0 && y < H) pix[y * W + x] = c;
    };
    // ring: parametric ellipse; skip a gap at the top-right for the arrow mouth.
    for (int d = 0; d < 360; ++d) {
        double a = d * 3.14159265 / 180.0;
        if (d > 300 && d < 345) continue;                     // gap
        int x = (int)(cx + rx * std::cos(a));
        int y = (int)(cy + ry * std::sin(a));
        // 2px white core with a 1px black halo (drawn first, then core on top).
        for (int oy = -2; oy <= 2; ++oy) for (int ox = -2; ox <= 2; ++ox)
            plot(x + ox, y + oy, black);
    }
    for (int d = 0; d < 360; ++d) {
        double a = d * 3.14159265 / 180.0;
        if (d > 300 && d < 345) continue;
        int x = (int)(cx + rx * std::cos(a));
        int y = (int)(cy + ry * std::sin(a));
        for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox)
            plot(x + ox, y + oy, white);
    }
    // arrowhead at the ring's open mouth (top-right), pointing clockwise.
    int ax = (int)(cx + rx * std::cos(-0.35)), ay = (int)(cy + ry * std::sin(-0.35));
    for (int k = 0; k < 6; ++k) {
        for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
            plot(ax - k + ox, ay - k + oy, black);
            plot(ax - k + ox, ay + k + oy, black);
        }
    }
    for (int k = 0; k < 6; ++k) { plot(ax - k, ay - k, white); plot(ax - k, ay + k, white); }
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pix, W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;
    SDL_Cursor* c = SDL_CreateColorCursor(surf, 15, 15);
    SDL_FreeSurface(surf);
    return c;
}

namespace {
SDL_Cursor* g_sizens_cursor=nullptr;
SDL_Cursor* g_sizewe_cursor=nullptr;
SDL_Cursor* g_loop_cursor=nullptr;
SDL_Cursor* g_arrow_cursor=nullptr;
int g_active_cursor=0;
}

void shutdown_cursors() {
    if(g_loop_cursor&&g_loop_cursor!=g_arrow_cursor) SDL_FreeCursor(g_loop_cursor);
    if(g_sizens_cursor) SDL_FreeCursor(g_sizens_cursor);
    if(g_sizewe_cursor) SDL_FreeCursor(g_sizewe_cursor);
    if(g_arrow_cursor) SDL_FreeCursor(g_arrow_cursor);
    g_sizens_cursor=g_sizewe_cursor=g_loop_cursor=g_arrow_cursor=nullptr;
    g_active_cursor=0;
}

void ArrangeView::apply_resize_cursor()
{
    if (!g_arrow_cursor) {
        g_sizens_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
        g_sizewe_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
        g_arrow_cursor  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        g_loop_cursor   = make_loop_cursor();
        if (!g_loop_cursor) g_loop_cursor = g_arrow_cursor;
    }
    int want = 0;
    if (m_hover_loop)                              want = 3;   // loop corner
    else if (m_hover_extend || m_extending)        want = 2;   // extend edge (horiz)
    else if (m_hover_trim || m_growing)            want = 2;   // clip trim zone
    else if (m_hover_resize || m_hdr_resize)       want = 1;   // track-height edge
    if (want == g_active_cursor) return;
    SDL_Cursor* c = want == 3 ? g_loop_cursor : want == 2 ? g_sizewe_cursor
                  : want == 1 ? g_sizens_cursor : g_arrow_cursor;
    if (c) { SDL_SetCursor(c); g_active_cursor = want; }
}

//----------------------------------------------------------------------------
//  top time ruler (ports perftime::on_expose_event)
//----------------------------------------------------------------------------
void ArrangeView::draw_ruler(App& app)
{
    const Theme& t = theme();
    SDL_Rect rl{ canvas_x(), rect.y, canvas_w(), ruler_h };
    fill_rect(app.ren, rl, t.panel);
    hline(app.ren, rl.x, rl.x + rl.w, rl.y + rl.h - 1, t.accent);

    // The tool strip, zoom buttons, loop chip, transport readout and snap
    // field all moved to the TOPBAR (draw_topbar, arrange_protools.cpp).
    // What remains here is the actual ruler: the time band, its metric
    // hierarchy, and the timeline / edit selection markers.
    const int cw = app.mono.cw() ? app.mono.cw() : 6;

    // Chips are re-registered every frame; clear them first so a chip that is
    // no longer drawn (narrow ruler) stops swallowing clicks.
    const int timeY=rl.y+toolstrip_h();
    const int timeH=rl.h-toolstrip_h();
    SDL_Rect timeBand{rl.x,timeY,rl.w,timeH};
    fill_rect(app.ren,timeBand,t.bg);

    // Loop/edit range is a ruler lane, not just two detached letters.
    long left  = m_perf->get_left_tick();
    long right = m_perf->get_right_tick();
    int lx = tick_to_x(left);
    int rx = tick_to_x(right);
    if(rx>rl.x&&lx<rl.x+rl.w&&right>left){
        int a=std::max(rl.x,lx),b=std::min(rl.x+rl.w,rx);
        Color range=t.accent;range.a=52;
        SDL_SetRenderDrawBlendMode(app.ren,SDL_BLENDMODE_BLEND);
        fill_rect(app.ren,SDL_Rect{a,timeY,std::max(0,b-a),timeH},range);
        SDL_SetRenderDrawBlendMode(app.ren,SDL_BLENDMODE_NONE);
    }

    // ---- adaptive metric hierarchy ----------------------------------------
    // The bar NUMBERS get their own stride, sized from how wide the widest
    // visible number actually is: zoom out and they drop to every 2nd, 4th,
    // 8th, 16th ... bar instead of overprinting each other; zoom back in and
    // they fill back in, then beats appear, then the snap subdivision.  The
    // canvas grid below uses this same metric(), so a printed number always
    // sits over the line it names.
    const long visibleEnd = x_to_tick(rl.x + rl.w) + m_measure_len;
    const Metric mt = metric(cw);

    // finest divisions first, so the heavier ones overdraw them
    if (mt.subStep > 0) {
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
        Color c = t.dim; c.a = 150;
        for (long tick = m_scroll_ticks - (m_scroll_ticks % mt.subStep);
             tick <= visibleEnd; tick += mt.subStep) {
            if ((tick % m_beat_len) == 0) continue;
            const int x = tick_to_x(tick);
            if (x < rl.x || x > rl.x + rl.w) continue;
            vline(app.ren, x, timeY + timeH - timeH / 4, timeY + timeH, c);
        }
        SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    }
    if (mt.beatStep > 0) {
        const bool labelBeats = (double)m_beat_len / m_scale_x >= (double)(cw * 5 + 10);
        for (long tick = m_scroll_ticks - (m_scroll_ticks % mt.beatStep);
             tick <= visibleEnd; tick += mt.beatStep) {
            if ((tick % m_measure_len) == 0) continue;
            const int x = tick_to_x(tick);
            if (x < rl.x || x > rl.x + rl.w) continue;
            vline(app.ren, x, timeY + timeH - timeH * 2 / 5, timeY + timeH, t.text);
            if (labelBeats) {
                char label[24];
                std::snprintf(label, sizeof(label), "%ld.%ld",
                              tick / m_measure_len + 1,
                              (tick % m_measure_len) / m_beat_len + 1);
                app.mono.draw(app.ren, x + 2, timeY + timeH - app.mono.ch() - 1,
                              label, t.dim);
            }
        }
    }
    // bar lines: full height + accent when they carry a number, short + dim
    // when they are only a tick between two numbered bars.
    for (long tick = (m_scroll_ticks / mt.barStep) * mt.barStep;
         tick <= visibleEnd; tick += mt.barStep) {
        const int x = tick_to_x(tick);
        if (x < rl.x || x > rl.x + rl.w) continue;
        const bool labelled = (tick % mt.labelStep) == 0;
        if (!labelled) {
            vline(app.ren, x, timeY + timeH / 2, timeY + timeH, t.dim);
            continue;
        }
        vline(app.ren, x, timeY, timeY + timeH, t.accent);
        char label[24];
        std::snprintf(label, sizeof(label), "%ld", tick / m_measure_len + 1);
        const int lw = app.mono.text_w(label) + 6;
        SDL_Rect tab{ x + 1, timeY + 1, lw, std::max(10, app.mono.ch() + 2) };
        if (tab.x + tab.w > rl.x + rl.w) tab.w = rl.x + rl.w - tab.x;
        if (tab.w > 2) {
            fill_rect(app.ren, tab, t.panel);
            app.mono.draw(app.ren, x + 4, timeY + 2, label, t.text);
        }
    }
    hline(app.ren,rl.x,rl.x+rl.w,timeY,t.dim);
    hline(app.ren,rl.x,rl.x+rl.w,timeY+timeH-1,t.accent);

    // ---- TIMELINE SELECTION MARKERS (ch.30 p670) --------------------------
    // The play/record range as a DOWN arrow (start) and an UP arrow (end).
    // While any track is record-armed they blink between the two brightest
    // roles -- the two-tone stand-in for Pro Tools' red.  When the timeline
    // and edit selections are UNLINKED, the edit selection additionally draws
    // as black-bracket Edit Markers.  Both pairs drag; Alt-drag slides the
    // whole selection preserving its length.
    {
        bool armed = false;
        if (is_track_record_armed)
            for (int seq : active_list())
                if (is_track_record_armed(seq)) { armed = true; break; }
        Color mc = t.accent;
        if (armed) { mc = ((SDL_GetTicks() / 400) & 1) ? t.hi : t.accent;
                     app.add_damage(SDL_Rect{ rl.x, timeY, rl.w, 12 }); }
        auto down_arrow = [&](int x) {                     // start marker
            for (int i = 0; i < 6; ++i)
                hline(app.ren, x - (5 - i), x + (5 - i), timeY + 1 + i, mc);
        };
        auto up_arrow = [&](int x) {                       // end marker
            for (int i = 0; i < 6; ++i)
                hline(app.ren, x - i, x + i, timeY + 1 + i, mc);
        };
        if (lx >= rl.x && lx <= rl.x + rl.w) {
            down_arrow(lx);
            if (std::abs(m_mx - lx) <= 8 && m_my >= timeY && m_my < timeY + 12)
                tip("Timeline start  " + bbt(left) + "  (Alt-drag slides)",
                    lx, rl.y + rl.h);
        }
        if (rx >= rl.x && rx <= rl.x + rl.w) {
            up_arrow(rx);
            if (std::abs(m_mx - rx) <= 8 && m_my >= timeY && m_my < timeY + 12)
                tip("Timeline end  " + bbt(right) + "   len " + bars_len(right - left),
                    rx - 60, rl.y + rl.h);
        }
        // Edit Markers: brackets, only when unlinked (linked selections are
        // already represented by the timeline arrows, per the manual).
        if (!m_link_timeline && m_sel_start >= 0 && m_sel_end > m_sel_start) {
            const Color ec = t.hi;
            const int ex0 = tick_to_x(m_sel_start), ex1 = tick_to_x(m_sel_end);
            if (ex0 >= rl.x && ex0 <= rl.x + rl.w) {
                vline(app.ren, ex0, timeY + 1, timeY + 11, ec);
                hline(app.ren, ex0, ex0 + 4, timeY + 1, ec);
                hline(app.ren, ex0, ex0 + 4, timeY + 10, ec);
            }
            if (ex1 >= rl.x && ex1 <= rl.x + rl.w) {
                vline(app.ren, ex1, timeY + 1, timeY + 11, ec);
                hline(app.ren, ex1 - 4, ex1, timeY + 1, ec);
                hline(app.ren, ex1 - 4, ex1, timeY + 10, ec);
            }
        }
    }

    // Ardour-like edit/playhead triangle with a one-pixel stem into the ruler.
    int px = tick_to_x(playhead());
    if (px >= rl.x && px <= rl.x + rl.w) {
        for (int i = 0; i < 7; ++i)
            hline(app.ren, px-(6-i),px+(6-i),timeY+i,t.hi);
        vline(app.ren,px,timeY+6,timeY+timeH,t.hi);
    }
    // The ruler marker moves with the playhead, so it is damage too -- without
    // this it would only refresh on the periodic full repaint and visibly lag
    // the canvas playhead during playback.
    app.add_damage(SDL_Rect{ px - 96, timeY, 192, timeH + 8 });

    m_snap_rect = SDL_Rect{ 0, 0, 0, 0 };   // snap chip moved to the topbar
    m_snap_menu_rect = SDL_Rect{ 0, 0, 0, 0 };
}

void ArrangeView::draw_scrollbar(App& app)
{
    const Theme& t=theme();
    m_scrollbar_rect=SDL_Rect{canvas_x(),rect.y+rect.h-scrollbar_h,
                              canvas_w(),scrollbar_h};
    fill_rect(app.ren,m_scrollbar_rect,t.panel);
    frame_rect(app.ren,m_scrollbar_rect,t.dim);
    const long view=std::max<long>(1,(long)(canvas_w()*m_scale_x));
    const long songEnd=std::max<long>(m_measure_len,song_end()+m_measure_len);
    const long total=std::max(view,songEnd);
    const int trackW=std::max(1,m_scrollbar_rect.w-4);
    int thumbW=std::max(24,(int)((double)trackW*view/total));
    if(thumbW>trackW)thumbW=trackW;
    const long maxScroll=std::max<long>(1,total-view);
    const int travel=trackW-thumbW;
    const int thumbX=m_scrollbar_rect.x+2+
        (travel>0?(int)((double)travel*std::min(m_scroll_ticks,maxScroll)/maxScroll):0);

    // SONG OVERVIEW inside the navigator: every clip in the project as a 2 px
    // dash, so the whole arrangement is legible at a glance and you can aim a
    // jump at material instead of scrubbing blind.  Drawn under the thumb.
    {
        const int ox = m_scrollbar_rect.x + 2, ow = trackW;
        const int oy = m_scrollbar_rect.y + 3;
        const int oh = std::max(2, m_scrollbar_rect.h - 6);
        for (int seq = 0; seq < c_max_sequence; ++seq) {
            if (!m_perf->is_active(seq)) continue;
            long on = 0, off = 0;
            std::map<int,AudioRegion>::const_iterator ri = m_region.find(seq);
            sequence* s = m_perf->get_sequence(seq);
            if (m_audio.count(seq) && ri != m_region.end()) {
                on = ri->second.position; off = on + ri->second.length;
                const int a = ox + (int)((double)on  / (double)total * ow);
                const int b = ox + (int)((double)off / (double)total * ow);
                fill_rect(app.ren, SDL_Rect{ a, oy, std::max(2, b - a), oh }, t.dim);
                continue;
            }
            if (!s) continue;
            s->reset_draw_trigger_marker();
            long offs; bool sel;
            while (s->get_next_trigger(&on, &off, &sel, &offs)) {
                const int a = ox + (int)((double)on  / (double)total * ow);
                const int b = ox + (int)((double)off / (double)total * ow);
                fill_rect(app.ren, SDL_Rect{ a, oy, std::max(2, b - a), oh }, t.dim);
            }
        }
        // playhead tick over the overview
        const int phx = ox + (int)((double)playhead() / (double)total * ow);
        if (phx >= ox && phx <= ox + ow)
            vline(app.ren, phx, m_scrollbar_rect.y + 1,
                  m_scrollbar_rect.y + m_scrollbar_rect.h - 1, t.hi);
        // moves during playback: damage, same as the canvas and ruler markers
        app.add_damage(SDL_Rect{ phx - 32, m_scrollbar_rect.y,
                                 64, m_scrollbar_rect.h });
    }

    m_scroll_thumb=SDL_Rect{thumbX,m_scrollbar_rect.y+2,thumbW,
                            std::max(4,m_scrollbar_rect.h-4)};
    const bool hhot = m_scroll_drag ||
        (m_mx >= m_scroll_thumb.x && m_mx < m_scroll_thumb.x + m_scroll_thumb.w &&
         m_my >= m_scroll_thumb.y && m_my < m_scroll_thumb.y + m_scroll_thumb.h);
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
    { Color c = hhot ? t.hi : t.accent; c.a = 200; fill_rect(app.ren, m_scroll_thumb, c); }
    SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
    frame_rect(app.ren,m_scroll_thumb,t.hi);

    // Right-side track navigator. The thumb represents the visible lane count;
    // dragging scrolls by track, while clicks above/below page by one view.
    m_vscrollbar_rect=SDL_Rect{rect.x+rect.w-scrollbar_w,canvas_y(),
                               scrollbar_w,canvas_h()};
    fill_rect(app.ren,m_vscrollbar_rect,t.panel);
    frame_rect(app.ren,m_vscrollbar_rect,t.dim);
    // Lanes have individual heights, so the thumb must be sized from the rows
    // that actually fit -- canvas_h()/row_h was wrong on every resized lane.
    const int totalRows=std::max(1,(int)active_list().size());
    const int visibleRows=visible_rows();
    const int vtrack=std::max(1,m_vscrollbar_rect.h-4);
    int vthumb=std::max(24,(int)((double)vtrack*std::min(totalRows,visibleRows)/totalRows));
    if(vthumb>vtrack)vthumb=vtrack;
    const int maxOff=max_v_offset();
    const int vtravel=vtrack-vthumb;
    const int vy=m_vscrollbar_rect.y+2+
        (vtravel>0?(int)((double)vtravel*std::min(m_v_offset,maxOff)/std::max(1,maxOff)):0);
    m_vscroll_thumb=SDL_Rect{m_vscrollbar_rect.x+2,vy,
                             std::max(4,m_vscrollbar_rect.w-4),vthumb};
    const bool vhot = m_vscroll_drag ||
        (m_mx >= m_vscroll_thumb.x && m_mx < m_vscroll_thumb.x + m_vscroll_thumb.w &&
         m_my >= m_vscroll_thumb.y && m_my < m_vscroll_thumb.y + m_vscroll_thumb.h);
    fill_rect(app.ren,m_vscroll_thumb,vhot?t.hi:t.accent);
    frame_rect(app.ren,m_vscroll_thumb,t.hi);

    // Bottom-right corner closes the two reserved gutters visibly.
    SDL_Rect corner{rect.x+rect.w-scrollbar_w,rect.y+rect.h-scrollbar_h,
                    scrollbar_w,scrollbar_h};
    fill_rect(app.ren,corner,t.panel);frame_rect(app.ren,corner,t.dim);
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
bool ArrangeView::on_mouse(App& app, const MouseEv& e)
{
    if (!m_perf) return false;

    m_mx = e.x; m_my = e.y;                 // track pointer for hover highlight

    // ch.32 modal dialogs swallow everything (press, drag and release)
    if (m_fdlg_open) return fade_dialog_mouse(app, e);
    if (m_bdlg_open) return batch_dialog_mouse(app, e);

    // release : commit a pending drag-copy, then clear ALL drag state (handled
    // first so state is cleared even while a menu / rename field is up) --------
    if (!e.pressed) {
        m_hold_tool = -1; m_hold_preset = -1;    // click-and-hold disarmed
        m_vzoom_drag = -1;
        if (m_counter_scrub) { m_counter_scrub = false; m_counter_scrub_which = -1; }
        if (m_univ_drag || m_univ_resize) { m_univ_drag = m_univ_resize = false; }
        // ZOOMER release: a travelled band zooms to that range; a bare click
        // zooms one level (Alt = out) centred on the click.  Single Zoom mode
        // then returns to the previously selected tool.
        if (m_zoomer_band) {
            const int a = std::min(m_zoomer_x0, m_zoomer_x1);
            const int b = std::max(m_zoomer_x0, m_zoomer_x1);
            remember_zoom();
            if (b - a > 6) {
                const long t0 = x_to_tick(a), t1 = x_to_tick(b);
                int cw = canvas_w(); if (cw < 16) cw = 16;
                double sx = (double)std::max<long>(1, t1 - t0) / (double)cw;
                if (sx < kZoomMin) sx = kZoomMin;
                if (sx > kZoomMax) sx = kZoomMax;
                m_scale_x = sx;
                m_scroll_ticks = t0;
            } else {
                const long c = x_to_tick(m_zoomer_x0);
                const bool out = (SDL_GetModState() & KMOD_ALT) != 0;
                double ns = out ? m_scale_x * 2.0 : m_scale_x * 0.5;
                if (ns < kZoomMin) ns = kZoomMin;
                if (ns > kZoomMax) ns = kZoomMax;
                m_scale_x = ns;
                m_scroll_ticks = c - (long)(canvas_w() / 2 * m_scale_x);
            }
            clamp_scroll();
            m_zoomer_band = false;
            if (m_zoom_mode == 1) m_edit_tool = m_tool_before_zoom;  // Single
        }
        m_zoomer_cont = false;
        // SELECTOR release: settle the edit selection (link mirror, last-sel).
        if (m_selecting) { m_selecting = false; selection_changed(); }
        m_tl_selecting = false;
        m_marker_drag = 0;
        // SCRUB TRIM release: trim the pressed side to the audition point.
        if (m_scrubtrim && m_move_seq >= 0 && m_audio.count(m_move_seq)) {
            AudioRegion& r = region_for(m_move_seq);
            long t = esnap(x_to_tick(e.x));
            if (m_scrubtrim_left) {
                const long end = r.position + r.length;
                if (t >= end) t = end - 1;
                if (t < 0) t = 0;
                const long d = t - r.position;
                r.position = t; r.source += d; r.length = end - t;
            } else {
                if (t <= r.position) t = r.position + 1;
                r.length = t - r.position;
            }
            commit_region(m_move_seq);
        }
        m_scrubtrim = false;
        // TCE TRIM release: hand the pending length to the shell's stretch
        // hook; unbound, revert (never leave a region whose audio does not
        // match its drawn span -- that would be a fake stretch).
        if (m_tce_drag && m_move_seq >= 0 && m_audio.count(m_move_seq)) {
            AudioRegion& r = region_for(m_move_seq);
            const long newLen = r.length;
            if (on_clip_tce && newLen != m_tce_len0) {
                on_clip_tce(m_move_seq, newLen);
            } else {
                r.position = m_trim_start0;
                r.length   = m_tce_len0;
                commit_region(m_move_seq);
            }
        }
        m_tce_drag = false;
        m_trim_group.clear();
        m_tandem = false; m_tandem_left = m_tandem_right = -1;
        m_looptrim_src = false;
        if (m_xfade_drag) {
            if (m_xfade_left  >= 0) commit_fade(m_xfade_left);
            if (m_xfade_right >= 0) commit_fade(m_xfade_right);
            if (!m_fade_move_before.empty()) {
                std::vector<int> seqs;
                for (const FadeState& st : m_fade_move_before) seqs.push_back(st.seq);
                std::vector<FadeState> after;
                capture_fade_state(seqs, after);
                UndoOp op; op.name = "Crossfade";
                const std::vector<FadeState> before = m_fade_move_before;
                op.undo.push_back([this, before]{ restore_fade_state(before); });
                op.redo.push_back([this, after]{ restore_fade_state(after); });
                push_undo_op(std::move(op));
                m_fade_move_before.clear();
            }
            m_xfade_drag = false; m_xfade_left = m_xfade_right = -1;
        }
        m_pencil_audio = false; m_pencil_seq = -1;
        // SHUFFLE trim ripple: subsequent clips slide by however much the
        // trimmed clip's END moved (a front trim re-packs the clip first so
        // it stays abutted, which is what makes the end move).
        if (m_edit_mode == EditMode::Shuffle && m_growing && m_move_seq >= 0) {
            long newEnd = -1;
            if (m_audio.count(m_move_seq)) {
                AudioRegion& r = region_for(m_move_seq);
                if (m_grow_dir) {                     // front trim: re-pack
                    const long newLen = r.length;
                    r.position = m_trim_start0;
                    r.length   = newLen;
                    commit_region(m_move_seq);
                }
                newEnd = r.position + r.length;
            } else if (sequence* ms = m_perf->get_sequence(m_move_seq)) {
                const long en = ms->get_selected_trigger_end_tick();
                if (en >= 0) newEnd = en + 1;
            }
            if (newEnd >= 0 && newEnd != m_trim_end0)
                ripple_lane(m_move_seq, m_trim_end0, newEnd - m_trim_end0,
                            m_move_seq);
        }
        if (m_lassoing) {
            int x0 = std::min(m_lasso_x0, m_lasso_x1);
            int y0 = std::min(m_lasso_y0, m_lasso_y1);
            int x1 = std::max(m_lasso_x0, m_lasso_x1);
            int y1 = std::max(m_lasso_y0, m_lasso_y1);
            select_clips_in_rect(SDL_Rect{ x0, y0, x1 - x0, y1 - y0 },
                                 (SDL_GetModState() & (KMOD_CTRL | KMOD_SHIFT)) != 0);
        }
        if (m_copying && m_drop_seq >= 0 && m_perf->is_active(m_drop_seq)) {
            //  Only a real DRAG duplicates.  m_copy_len is always > 0, so the
            //  old guard let a bare Ctrl+CLICK -- the add-to-selection gesture
            //  -- drop a duplicate at the identical tick on the same lane:
            //  invisible, and the part played doubled from then on.  Require
            //  both that the pointer travelled and that the ghost actually
            //  landed somewhere else.
            const bool travelled = std::abs(e.x - m_press_px) > kDragSlop ||
                                   std::abs(e.y - m_press_py) > kDragSlop;
            long t = m_ghost_tick < 0 ? 0 : m_ghost_tick;
            if (m_copy_len > 0 && travelled && t != m_copy_src_tick) {
                push_undo("Duplicate Clip");
                create_pattern(m_drop_seq, t, m_copy_len, m_copy_offset, true);
            }
        }
        if (m_fade_grab != FadeGrab::None) {
            if (m_fade_seq >= 0) commit_fade(m_fade_seq);   // final push to engine
            if (!m_fade_move_before.empty()) {   // one queue entry per gesture
                std::vector<int> seqs;
                for (const FadeState& st : m_fade_move_before) seqs.push_back(st.seq);
                std::vector<FadeState> after;
                capture_fade_state(seqs, after);
                UndoOp op; op.name = "Fade";
                const std::vector<FadeState> before = m_fade_move_before;
                op.undo.push_back([this, before]{ restore_fade_state(before); });
                op.redo.push_back([this, after]{ restore_fade_state(after); });
                push_undo_op(std::move(op));
                m_fade_move_before.clear();
            }
            m_fade_grab = FadeGrab::None; m_fade_seq = -1;
        }
        // Grabber fade-move release: same one-entry-per-gesture undo law.
        if (m_fade_move) {
            if (!m_fade_move_before.empty()) {
                std::vector<int> seqs;
                for (const FadeState& st : m_fade_move_before) seqs.push_back(st.seq);
                std::vector<FadeState> after;
                capture_fade_state(seqs, after);
                UndoOp op; op.name = "Move Fade";
                const std::vector<FadeState> before = m_fade_move_before;
                op.undo.push_back([this, before]{ restore_fade_state(before); });
                op.redo.push_back([this, after]{ restore_fade_state(after); });
                push_undo_op(std::move(op));
                m_fade_move_before.clear();
            }
            m_fade_move = false;
        }
        // A moved/trimmed AUDIO clip: apply the Ardour content law for this
        // gesture and push the new region (position, span, source-offset) to the
        // engine so playback follows non-destructively.
        if ((m_moving || m_growing) && m_move_seq >= 0 && m_audio.count(m_move_seq)) {
            commit_region(m_move_seq);
            // ch.32 p758-759: moving a crossfade contributor stretches the
            // crossfade (or drops/keeps its fades per Preserve Fades).
            if (m_moving)
                stretch_xfades_after_nudge(m_move_seq, m_trim_start0, m_trim_end0);
        }
        // Layered Editing (ch.31 p734): a DROPPED move settles overlaps --
        // fully covered clips are removed; partial overlaps are trimmed to
        // the overlapper unless the Layered option keeps them intact.
        if (m_moving && m_move_seq >= 0 &&
            (std::abs(e.x - m_press_px) > kDragSlop ||
             std::abs(e.y - m_press_py) > kDragSlop))
            resolve_overlaps(m_move_seq);
        // EXTEND release: the trigger's new end sets the region length (right-trim
        // law); for a looped region commit_region keeps the over-length span.
        if (m_extending && m_extend_seq >= 0 && m_audio.count(m_extend_seq)) {
            commit_region(m_extend_seq);
        }
        // Same for an AUTOMATION clip, whose block is its region: settle the
        // final position/length/source once the gesture lets go.
        if ((m_moving || m_growing || m_adding) && m_drop_seq >= 0)
            commit_auto_region(m_drop_seq);
        m_extending = false; m_extend_seq = -1;
        m_move_seq = -1;
        m_gain_grab = false; m_gain_seq = -1;
        m_lassoing = false;
        m_copying = false;
        //  Grabbing a clip out of a multi-selection keeps the group so the drag
        //  can move all of it -- but a bare CLICK on one of them still has to
        //  collapse the selection down to that clip, or a group could never be
        //  narrowed by clicking.
        if (m_group_click_seq >= 0 &&
            std::abs(e.x - m_press_px) <= kDragSlop &&
            std::abs(e.y - m_press_py) <= kDragSlop &&
            (SDL_GetModState() & KMOD_CTRL) == 0) {
            const int cs = m_group_click_seq;
            unselect_all_triggers();
            if (m_audio.count(cs)) region_for(cs).selected = true;
            else if (m_perf->is_active(cs))
                if (sequence* gs = m_perf->get_sequence(cs))
                    gs->select_trigger(m_group_click_tick);
        }
        m_group_click_seq = -1;
        m_move_group.clear();
        m_mouse_down = m_moving = m_growing = m_adding = m_slipping = false;
        m_hdr_resize = false; m_hdr_resize_seq = -1;
        m_drag_left = m_drag_right = false;
        m_scroll_drag=false;
        m_vscroll_drag=false;
        m_range_drag=false;
        m_scrubbing=false;
        m_guide_on=false; m_guide_text.clear();
        return true;
    }

    // The shortcut overlay swallows the next click (anywhere) to dismiss.
    if (m_help_open) { m_help_open = false; app.request_redraw(); return true; }

    // context menu / add-track chooser intercept a press while open ----------
    if (m_menu_open)      return menu_click(app, e.x, e.y);
    if (m_toolmenu_open)  return toolmenu_click(app, e.x, e.y);
    if (m_addmenu_open)   return addmenu_click(app, e.x, e.y);
    if (m_instrmenu_open) return instrmenu_click(app, e.x, e.y);
    if (m_iomenu_open)    return iomenu_click(app, e.x, e.y);
    if (m_submode_menu)   return submode_click(app, e.x, e.y);
    if (m_grid_menu)      return grid_menu_click(app, e.x, e.y);
    if (m_nudge_menu)     return nudge_menu_click(app, e.x, e.y);
    if (m_zt_menu)        return zt_menu_click(app, e.x, e.y);
    if (m_view_menu)      return view_menu_click(app, e.x, e.y);
    if (m_edit_menu)      return edit_menu_click(app, e.x, e.y);
    if (m_fadesmenu_open) return fadesmenu_click(app, e.x, e.y);
    if (m_fadepref_open)  return fadepref_click(app, e.x, e.y);
    // the Undo History window is NON-modal: presses inside it act on it,
    // everything else falls through to the view as usual.
    if (undo_window_mouse(app, e)) return true;

    // while the inline rename editor is open, swallow presses (Enter/Esc ends)
    if (app.editing_text()) return true;

    if(m_scroll_drag) {
        const long view=std::max<long>(1,(long)(canvas_w()*m_scale_x));
        const long total=std::max(view,std::max<long>(m_measure_len,
            song_end()+m_measure_len));
        const int travel=std::max(1,m_scrollbar_rect.w-4-m_scroll_thumb.w);
        const long maxScroll=std::max<long>(0,total-view);
        m_scroll_ticks=m_scroll_drag_tick0+
            (long)((double)(e.x-m_scroll_drag_x0)*maxScroll/travel);
        if(m_scroll_ticks<0)m_scroll_ticks=0;
        if(m_scroll_ticks>maxScroll)m_scroll_ticks=maxScroll;
        app.request_redraw();return true;
    }
    if(m_vscroll_drag) {
        const int maxOff=max_v_offset();
        const int travel=std::max(1,m_vscrollbar_rect.h-4-m_vscroll_thumb.h);
        m_v_offset=m_vscroll_drag_offset0+
            (int)((double)(e.y-m_vscroll_drag_y0)*maxOff/travel);
        if(m_v_offset<0)m_v_offset=0;if(m_v_offset>maxOff)m_v_offset=maxOff;
        app.request_redraw();return true;
    }

    // universe navigation / resize + counter-field scrubbing continue while
    // the button is held, independent of the canvas drag machine.
    if ((m_univ_drag || m_univ_resize)) { universe_mouse(app, e); return true; }
    if (m_vzoom_drag >= 0 && e.pressed) {
        // continuous vertical zoom: drag up = in, down = out
        const float f = std::pow(1.01f, (float)(m_vzoom_y0 - e.y));
        if (m_vzoom_drag < 2)
            m_wave_zoom = std::max(0.25f, std::min(8.f, m_vzoom_v0 * f));
        else
            m_midi_zoom = std::max(0.25f, std::min(4.f, m_vzoom_v0 * f));
        app.request_redraw();
        return true;
    }
    if (m_counter_scrub && m_counter_scrub_which >= 0) {
        const bool fine = (SDL_GetModState() & KMOD_CTRL) != 0;
        const long step = fine ? 1 : m_beat_len;
        const long v = m_counter_scrub_v0 +
                       (long)(m_counter_scrub_y0 - e.y) * step;
        counter_apply(m_counter_scrub_which, v < 0 ? 0 : v);
        counter_load(m_counter_scrub_which);
        app.request_redraw();
        return true;
    }

    if (m_snap_menu) {
        const int rh = std::max(18, app.mono.ch() + 6);
        if(e.button==SDL_BUTTON_LEFT && e.x>=m_snap_menu_rect.x &&
           e.x<m_snap_menu_rect.x+m_snap_menu_rect.w &&
           e.y>=m_snap_menu_rect.y && e.y<m_snap_menu_rect.y+m_snap_menu_rect.h) {
            m_snap_idx=std::max(0,std::min(8,(e.y-m_snap_menu_rect.y)/rh));
            m_snap=snap_value(m_snap_idx);
        }
        m_snap_menu=false;
        app.request_redraw();
        return true;
    }

    // drag (continue whatever mode the press established) --------------------
    if (m_mouse_down) {
        // Lane-height resize drag (header bottom edge).
        if (m_hdr_resize && m_hdr_resize_seq >= 0) {
            int nh = m_hdr_resize_h0 + (e.y - m_hdr_resize_y0);
            if (nh < 20) nh = 20; if (nh > 400) nh = 400;
            m_trackH[lane_key(m_hdr_resize_seq)] = nh;   // key by lane -> all its clips
            app.request_redraw();
            return true;
        }
        // Grabber fade move: slide the fade / crossfade within its clips.
        if (m_fade_move && m_fade_sel_seq >= 0) {
            move_fade_to(x_to_tick(e.x));
            app.request_redraw();
            return true;
        }
        // Clip fade drag: reshape the fade length / curve live.
        if (m_fade_grab != FadeGrab::None && m_fade_seq >= 0) {
            SDL_Rect clip;
            if (clip_rect_of(m_fade_seq, clip)) {
                ClipFade& f = m_clipFade[m_fade_seq];
                const long maxTk = (long)((clip.w / 2) * m_scale_x);
                if (m_fade_grab == FadeGrab::InLen) {
                    long tk = (long)((e.x - clip.x) * m_scale_x);
                    f.inTicks = tk <= 0 ? 0 : (tk > maxTk ? maxTk : tk);
                } else if (m_fade_grab == FadeGrab::OutLen) {
                    long tk = (long)(((clip.x + clip.w) - e.x) * m_scale_x);
                    f.outTicks = tk <= 0 ? 0 : (tk > maxTk ? maxTk : tk);
                } else {
                    const int half = std::max(1, clip.h / 2);
                    float k = (float)(e.y - (clip.y + clip.h / 2)) / (float)half;
                    if (k < -1.f) k = -1.f; if (k > 1.f) k = 1.f;
                    const bool inHalf = m_fade_grab == FadeGrab::InCurve;
                    // the seven preset curves are fixed (p745): only Standard
                    // and S-Curve are hand-editable
                    if ((inHalf ? f.inShape : f.outShape) > 1) {
                        flash("Preset curves are fixed (pick Standard/S-Curve)");
                    } else if (inHalf) f.inK = k; else f.outK = k;
                    // linked crossfade: the partner's curve follows (p747)
                    int xl = -1, xr = -1;
                    if (xfade_pair(m_fade_seq, xl, xr)) {
                        const int other = m_fade_seq == xl ? xr : xl;
                        ClipFade of = fade_of(other);
                        const int lnk = fade_of(xr).link;
                        if (lnk != 2 && (inHalf ? f.inTicks : f.outTicks) > 0) {
                            if (m_fade_seq == xr && inHalf && of.outShape <= 1)
                                { of.outK = -f.inK; m_clipFade[other] = of; commit_fade(other); }
                            else if (m_fade_seq == xl && !inHalf && of.inShape <= 1)
                                { of.inK = -f.outK; m_clipFade[other] = of; commit_fade(other); }
                        }
                    }
                    // Shift: the same change on every fade in the Edit
                    // selection across tracks (p756)
                    if (SDL_GetModState() & KMOD_SHIFT) {
                        std::vector<std::pair<int,int>> sel;
                        selected_fades(sel);
                        for (const auto& pr : sel) {
                            if (pr.first == m_fade_seq) continue;
                            ClipFade g = fade_of(pr.first);
                            if (pr.second == 0 && g.inShape <= 1) g.inK = k;
                            if (pr.second == 1 && g.outShape <= 1) g.outK = k;
                            m_clipFade[pr.first] = g;
                            commit_fade(pr.first);
                        }
                    }
                }
                commit_fade(m_fade_seq);           // live-update the engine
            }
            app.request_redraw();
            return true;
        }
        // Zoomer: rubber-band range (x1 tracks) or Ctrl continuous zoom.
        if (m_zoomer_band) {
            m_zoomer_x1 = e.x;
            app.request_redraw();
            return true;
        }
        if (m_zoomer_cont) {
            // right = zoom in horizontally, up = zoom in vertically
            const int dx = e.x - m_zoomer_cx;
            const int dy = e.y - m_zoomer_cy;
            double ns = m_zoomer_s0 * std::pow(1.01, (double)-dx);
            if (ns < kZoomMin) ns = kZoomMin;
            if (ns > kZoomMax) ns = kZoomMax;
            m_scale_x = ns;
            int nh = m_zoomer_h0 - dy / 2;
            if (nh < 20) nh = 20;
            if (nh > 400) nh = 400;
            row_h = nh;
            clamp_scroll();
            app.request_redraw();
            return true;
        }
        // Timeline / edit marker drags (arrows and brackets in the ruler).
        if (m_marker_drag) {
            long t = esnap(x_to_tick(e.x)); if (t < 0) t = 0;
            switch (m_marker_drag) {
            case 1: if (t < m_perf->get_right_tick()) m_perf->set_left_tick(t); break;
            case 2: if (t > m_perf->get_left_tick())  m_perf->set_right_tick(t); break;
            case 3: if (m_sel_end >= 0 && t <= m_sel_end)
                        set_edit_selection(t, m_sel_end, m_sel_lo, m_sel_hi);
                    break;
            case 4: if (m_sel_start >= 0 && t >= m_sel_start)
                        set_edit_selection(m_sel_start, t, m_sel_lo, m_sel_hi);
                    break;
            case 5: {   // Alt-slide the whole timeline selection
                const long len = m_perf->get_right_tick() - m_perf->get_left_tick();
                long a = t - m_marker_off; if (a < 0) a = 0;
                m_perf->set_left_tick(a);
                m_perf->set_right_tick(a + len);
                break;
            }
            case 6: {   // Alt-slide the whole edit selection
                const long len = m_sel_end - m_sel_start;
                long a = t - m_marker_off; if (a < 0) a = 0;
                set_edit_selection(a, a + len, m_sel_lo, m_sel_hi);
                break;
            }
            }
            app.request_redraw();
            return true;
        }
        // Timeline selection drag in the ruler (Selector tool).
        if (m_tl_selecting) {
            long t = esnap(x_to_tick(e.x)); if (t < 0) t = 0;
            long a = std::min(m_tl_anchor, t), b = std::max(m_tl_anchor, t);
            if (b <= a) b = a + std::max<long>(1, m_snap);
            m_perf->set_left_tick(a);
            m_perf->set_right_tick(b);
            if (m_link_timeline)     // mirrored as an all-track edit selection
                set_edit_selection(a, b, 0, (int)active_list().size() - 1);
            app.request_redraw();
            return true;
        }
        // Selector: extend the edit selection over ticks and lanes.
        if (m_selecting) {
            long t = esnap(x_to_tick(e.x)); if (t < 0) t = 0;
            const int r = row_at(e.y);
            int row = m_sel_anchor_row;
            if (r >= 0) {
                const int idx = m_v_offset + r;
                if (idx >= 0 && idx < (int)active_list().size()) row = idx;
            }
            set_edit_selection(std::min(m_sel_anchor, t), std::max(m_sel_anchor, t),
                               std::min(m_sel_anchor_row, row),
                               std::max(m_sel_anchor_row, row));
            m_guide_on = true; m_guide_tick = t;
            m_guide_text = bbt(std::min(m_sel_anchor, t)) + "  " +
                           bars_len(std::labs(t - m_sel_anchor));
            app.request_redraw();
            return true;
        }
        // Scrub Trim: audition while dragging (Ctrl = finer resolution).
        if (m_scrubtrim) {
            const bool fine = (SDL_GetModState() & KMOD_CTRL) != 0;
            long t = x_to_tick(fine ? (m_press_px + (e.x - m_press_px) / 4) : e.x);
            if (t < 0) t = 0;
            if (on_scrub_audition) on_scrub_audition(m_move_seq, t);
            else seek_to(t);
            m_guide_on = true; m_guide_tick = t;
            m_guide_text = "scrub-trim " + bbt(t);
            app.request_redraw();
            return true;
        }
        // Loop Trim, bottom half: resize the SOURCE ITERATION while the
        // overall looped length stays constant.
        if (m_looptrim_src && m_move_seq >= 0 && m_audio.count(m_move_seq)) {
            AudioRegion& r = region_for(m_move_seq);
            long lp = esnap(x_to_tick(e.x)) - r.position;
            if (lp < 1) lp = 1;
            if (lp > r.length) lp = r.length;
            r.loopLength = lp;
            if (!r.loop) { r.loop = true; if (on_clip_loop) on_clip_loop(m_move_seq, true); }
            if (on_clip_loop_length) on_clip_loop_length(m_move_seq, lp);
            m_guide_on = true; m_guide_tick = r.position + lp;
            m_guide_text = "iteration " + bars_len(lp);
            app.request_redraw();
            return true;
        }
        // Tandem trim: one boundary, both clips.
        if (m_tandem && m_tandem_left >= 0 && m_tandem_right >= 0) {
            AudioRegion& lrg = region_for(m_tandem_left);
            AudioRegion& rrg = region_for(m_tandem_right);
            long t = esnap(x_to_tick(e.x));
            const long lo = lrg.position + 1;
            const long hi = rrg.position + rrg.length - 1;
            if (t < lo) t = lo;
            if (t > hi) t = hi;
            lrg.length = t - lrg.position;                    // left end trim
            const long rEnd = rrg.position + rrg.length;      // right front trim
            const long d = t - rrg.position;
            rrg.position = t; rrg.source += d; rrg.length = rEnd - t;
            commit_region(m_tandem_left);
            commit_region(m_tandem_right);
            m_guide_on = true; m_guide_tick = t;
            m_guide_text = "tandem " + bbt(t);
            app.request_redraw();
            return true;
        }
        // Smart-tool crossfade drag: a REAL centered crossfade -- the two
        // regions are overlapped symmetrically around the boundary and given
        // the default crossfade settings, so the crossfade is heard (ch.32).
        if (m_xfade_drag && m_xfade_left >= 0 && m_xfade_right >= 0) {
            long half = std::labs(x_to_tick(e.x) - m_xfade_bound);
            if (half < 1) half = 1;
            create_crossfade(m_xfade_left, m_xfade_right,
                             m_xfade_bound - half, m_xfade_bound + half,
                             m_def_xfade, false);
            m_guide_on = false;
            m_guide_text = "xfade " + bars_len(half * 2);
            app.request_redraw();
            return true;
        }
        // Pencil over audio at sample zoom: destructive waveform repair.
        if (m_pencil_audio && m_pencil_seq >= 0) {
            pencil_redraw(m_pencil_seq, e.x, e.y);
            app.request_redraw();
            return true;
        }
        // Loop markers: only move while a marker handle is actively grabbed.
        if (m_drag_left) {
            long t = snap(x_to_tick(e.x)); if (t < 0) t = 0;
            if (t < m_perf->get_right_tick()) m_perf->set_left_tick(t);
            app.request_redraw(); return true;
        }
        if (m_drag_right) {
            long t = snap(x_to_tick(e.x));
            if (t > m_perf->get_left_tick()) m_perf->set_right_tick(t);
            app.request_redraw(); return true;
        }
        if (m_scrubbing) {                       // ruler drag = continuous seek
            seek_to(scrub_tick(e.x));
            app.request_redraw();
            return true;
        }
        if (m_lassoing) {
            m_lasso_x1 = e.x;
            m_lasso_y1 = e.y;
            app.request_redraw();
            return true;
        }
        if (m_moving || m_growing || m_adding || m_slipping || m_gain_grab ||
            m_extending || m_range_drag) drag_canvas(app, e);
        return true;
    }

    // fresh press -----------------------------------------------------------
    m_mouse_down = true;

    // A live counter entry is cancelled by any press outside its field, so
    // typed-entry mode can never silently trap the keyboard.
    if (m_counter_edit >= 0) {
        bool onField = false;
        for (int i = 0; i < 4; ++i)
            if (pt_in_rect(m_counter_rect[i], e.x, e.y)) onField = true;
        if (!onField) counter_commit(false);
    }

    // topbar rows (mode block, tools, presets, counters, fields) + Universe.
    if (m_universe_on && universe_mouse(app, e)) { m_mouse_down = false; return true; }
    if (e.y < rect.y + toolbar_h()) {
        if (topbar_click(app, e)) return true;
    }

    if(e.button==SDL_BUTTON_LEFT && e.x>=m_scrollbar_rect.x &&
       e.x<m_scrollbar_rect.x+m_scrollbar_rect.w &&
       e.y>=m_scrollbar_rect.y && e.y<m_scrollbar_rect.y+m_scrollbar_rect.h) {
        if(e.x<m_scroll_thumb.x){m_scroll_ticks-=(long)(canvas_w()*m_scale_x);
              clamp_scroll();}
        else if(e.x>=m_scroll_thumb.x+m_scroll_thumb.w){
              m_scroll_ticks+=(long)(canvas_w()*m_scale_x); clamp_scroll();}
        else {m_scroll_drag=true;m_scroll_drag_x0=e.x;
              m_scroll_drag_tick0=m_scroll_ticks;}
        app.request_redraw();return true;
    }
    if(e.button==SDL_BUTTON_LEFT && e.x>=m_vscrollbar_rect.x &&
       e.x<m_vscrollbar_rect.x+m_vscrollbar_rect.w &&
       e.y>=m_vscrollbar_rect.y && e.y<m_vscrollbar_rect.y+m_vscrollbar_rect.h) {
        const int page=visible_rows();
        const int maxOff=max_v_offset();
        if(e.y<m_vscroll_thumb.y)m_v_offset=std::max(0,m_v_offset-page);
        else if(e.y>=m_vscroll_thumb.y+m_vscroll_thumb.h)
            m_v_offset=std::min(maxOff,m_v_offset+page);
        else {m_vscroll_drag=true;m_vscroll_drag_y0=e.y;
              m_vscroll_drag_offset0=m_v_offset;}
        app.request_redraw();return true;
    }

    const HeaderGeom hg = header_geom();
    const int spine_end = hg.spine_w;            // spine only (was spine+6, so
                                                 // the badge column toggled mute)
    const int rm_w = hg.rm_w, rm_h = hg.rm_h;
    const int rm_x = hg.rm_x, rm_y = hg.rm_y;
    const int btn_w = hg.btn_w, btn_h = hg.btn_h;
    const int btn_x = hg.btn_x;

    // (0) Blank header area below the tracks: right-click opens Add Track.
    // The old always-visible '+' button was removed in favour of this context
    // action and the main Track menu.
    {
        SDL_Rect ar = add_row_rect();
        if (e.x >= ar.x && e.x < ar.x + ar.w &&
            e.y >= ar.y && e.y < ar.y + ar.h &&
            e.button == SDL_BUTTON_RIGHT) {
                const int mw   = 160;
                const int rowh = app.font.ch() + 8;
                const int mh   = kAddMenuN * rowh + 2;
                int mx = e.x;
                int my = e.y;
                if (mx + mw > rect.x + rect.w) mx = rect.x + rect.w - mw;
                if (my + mh > rect.y + rect.h) my = e.y - mh;
                m_addmenu_rect = SDL_Rect{ mx, my, mw, mh };
                m_addmenu_open = true;
                app.request_redraw();
            return true;
        }
    }

    // (a) header column ------------------------------------------------------
    if (e.x < rect.x + header_w && e.y >= canvas_y()) {
        int r = row_at(e.y);
        if (r < 0) return true;                   // below every lane: not row -1
        std::vector<int> act = active_list();
        int idx = m_v_offset + r;
        if (idx < 0 || idx >= (int)act.size()) return true;
        int seq = act[idx];
        sequence* s = m_perf->get_sequence(seq);
        if (!s) return true;                      // draw guards this; input did not
        m_focus_lane = lane_key(seq);             // header click focuses the lane
        int lx = e.x - rect.x;                    // x within header
        int row_y0 = row_top(r);                  // this row's top (screen y)
        int lh = track_h(seq);
        int ly = e.y - row_y0;                    // y within row
        const int badge_x = hg.badge_x, badge_w = hg.badge_w;
        const int name_x = hg.name_x;

        // Drawn only when it fits the lane -- so it must only be CLICKABLE then
        // too, or a short lane armed recording from blank space.
        SDL_Rect recBtn{ rect.x + badge_x, row_y0 + 28, badge_w, 14 };
        if (e.button == SDL_BUTTON_LEFT && recBtn.y + recBtn.h < row_y0 + lh &&
            e.x >= recBtn.x && e.x < recBtn.x + recBtn.w &&
            e.y >= recBtn.y && e.y < recBtn.y + recBtn.h) {
            if (on_track_record_arm) on_track_record_arm(seq);
            app.request_redraw();
            return true;
        }
        if (e.button == SDL_BUTTON_LEFT && (on_cycle_track_io || on_cycle_track_record_input) && lh >= 64) {
            const int vu_x = hg.vu_x;
            const int ty=row_y0+2*app.mono.ch()+13;
            const int avail=std::max(72,vu_x-name_x-8);
            const int tabw=(avail-2)/2;
            for(int ti=0;ti<2;++ti) {
                SDL_Rect tb{rect.x+name_x+ti*(tabw+2),ty,tabw,12};
                if(e.x>=tb.x&&e.x<tb.x+tb.w&&e.y>=tb.y&&e.y<tb.y+tb.h) {
                    m_ioTab[seq]=ti; app.request_redraw(); return true;
                }
            }
            const int gap=3, boxw=(avail-gap)/2;
            SDL_Rect boxes[2] = {
                SDL_Rect{rect.x+name_x,ty+14,boxw,app.mono.ch()+4},
                SDL_Rect{rect.x+name_x+boxw+gap,ty+14,avail-boxw-gap,app.mono.ch()+4}
            };
            for(int io=0;io<2;++io) {
                const SDL_Rect& rb=boxes[io];
                if(e.x>=rb.x&&e.x<rb.x+rb.w&&e.y>=rb.y&&e.y<rb.y+rb.h) {
                    std::map<int,int>::const_iterator tIt = m_ioTab.find(seq);
                    const int route=((tIt==m_ioTab.end()?0:tIt->second)&1)*2+io;
                    if(on_list_track_io&&on_pick_track_io) {
                        m_iomenu_items=on_list_track_io(seq,route);
                        if(!m_iomenu_items.empty()) {
                            const int rowh=app.font.ch()+8;
                            const int mh=(int)m_iomenu_items.size()*rowh+2;
                            int my=rb.y+rb.h;
                            if(my+mh>rect.y+rect.h) my=rb.y-mh;
                            m_iomenu_rect=SDL_Rect{rb.x,my,std::max(150,rb.w),mh};
                            m_iomenu_seq=seq; m_iomenu_route=route; m_iomenu_open=true;
                        }
                    } else if(on_cycle_track_io) on_cycle_track_io(seq,route);
                    else on_cycle_track_record_input(seq);
                    app.request_redraw();
                    return true;
                }
            }
        }

        // LANE RESIZE: press within 4px of the header's bottom edge -> drag to
        // resize the lane height.
        if (e.button == SDL_BUTTON_LEFT && e.y >= row_y0 + lh - 4 && e.y <= row_y0 + lh) {
            m_hdr_resize = true; m_hdr_resize_seq = seq;
            m_hdr_resize_y0 = e.y; m_hdr_resize_h0 = lh;
            return true;
        }

        // instrument dropdown box (line 2) : open the picker.  Only instrument
        // (non-audio) tracks carry one -- audio tracks have no instrument node.
        if (e.button == SDL_BUTTON_LEFT && on_list_instruments && m_audio.count(seq) == 0) {
            SDL_Rect ib = instr_box_rect(row_y0, app.mono.ch());
            if (e.x >= ib.x && e.x < ib.x + ib.w && e.y >= ib.y && e.y < ib.y + ib.h) {
                m_instrmenu_items = on_list_instruments();
                if (!m_instrmenu_items.empty()) {
                    const int rowh = app.font.ch() + 8;
                    int mh = (int)m_instrmenu_items.size() * rowh + 2;
                    int my = ib.y + ib.h;
                    if (my + mh > rect.y + rect.h) my = ib.y - mh;   // flip up near bottom
                    if (my < canvas_y()) my = canvas_y();
                    m_instrmenu_rect = SDL_Rect{ ib.x, my, ib.w < 160 ? 160 : ib.w, mh };
                    m_instrmenu_seq  = seq;
                    m_instrmenu_open = true;
                    app.request_redraw();
                }
                return true;
            }
        }

        // remove "x" button (checked first so it can't fall through to a
        // track select / mute / rename)
        if (lx >= rm_x && lx < rm_x + rm_w && ly >= rm_y && ly < rm_y + rm_h) {
            if (on_remove_track) on_remove_track(seq);
            // ch.28 p666: deleting a track CLEARS the Undo Queue -- its queued
            // closures may name the dead sequence / renumbered lanes.
            clear_undo_queue();
            app.request_redraw();
            return true;
        }

        // Mute button (only where it is actually drawn -- see draw_headers)
        if (4 + 2 * btn_h + 2 < lh &&
            lx >= btn_x && lx <= btn_x + btn_w && ly >= 4 && ly <= 4 + btn_h) {
            s->set_song_mute(!s->get_song_mute());
            app.request_redraw(); return true;
        }
        // Solo button
        if (4 + 2 * btn_h + 2 < lh &&
            lx >= btn_x && lx <= btn_x + btn_w &&
            ly >= 4 + btn_h + 2 && ly <= 4 + 2 * btn_h + 2) {
            m_solo[seq] = !m_solo[seq];
            apply_solo(); app.request_redraw(); return true;
        }
        // spine / left area -> quick mute toggle
        if (lx < spine_end) {
            s->set_song_mute(!s->get_song_mute());
            app.request_redraw(); return true;
        }
        return true;
    }

    // (b) ruler : grab an EXISTING L/R marker handle and drag it.  A bare click
    // on empty ruler no longer teleports a marker (they were too easy to nudge).
    if (e.y < canvas_y() && e.x >= rect.x + header_w) {
        // Clickable SNAP readout opens a real resolution dropdown.
        if (e.x >= m_snap_rect.x && e.x < m_snap_rect.x + m_snap_rect.w &&
            e.y >= m_snap_rect.y && e.y < m_snap_rect.y + m_snap_rect.h) {
            m_snap_menu=true;
            app.request_redraw();
            return true;
        }
        // Right-click anywhere on the ruler opens the edit-tool picker: the
        // same four icons, named, without aiming at the 22 px strip.
        if (e.button == SDL_BUTTON_RIGHT) {
            m_toolmenu_rect = SDL_Rect{ e.x, e.y, 150, 0 };
            m_toolmenu_open = true;
            m_mouse_down = false;
            app.request_redraw();
            return true;
        }
        if (e.button == SDL_BUTTON_LEFT) {
            const SDL_Keymod mod = SDL_GetModState();
            const bool alt  = (mod & KMOD_ALT)  != 0;
            const bool ctrl = (mod & KMOD_CTRL) != 0;
            const int GRAB = 8;   // px hit tolerance around a marker
            int xl = tick_to_x(m_perf->get_left_tick());
            int xr = tick_to_x(m_perf->get_right_tick());
            // Edit Markers (brackets) are draggable when unlinked; checked
            // first so they stay reachable inside a timeline selection.
            const int ex0 = m_sel_start >= 0 ? tick_to_x(m_sel_start) : -9999;
            const int ex1 = m_sel_end   >= 0 ? tick_to_x(m_sel_end)   : -9999;
            // Double-click a ruler: select ALL material in all tracks (with
            // Link Timeline enabled, per ch.30 p673).
            static Uint32 s_rulerClickMs = 0;
            const Uint32 now = SDL_GetTicks();
            const bool dbl = now - s_rulerClickMs < 400;
            s_rulerClickMs = now;
            if (dbl && e.y >= rect.y + toolstrip_h()) {
                select_all_clips();
                set_edit_selection(0, std::max<long>(m_measure_len, song_end()),
                                   0, (int)active_list().size() - 1);
                m_mouse_down = false;
                app.request_redraw();
                return true;
            }
            if (ctrl && e.y >= rect.y + toolstrip_h()) {
                // Ctrl+drag in a ruler = the Zoomer appears there (zoom range)
                m_zoomer_band = true; m_zoomer_x0 = m_zoomer_x1 = e.x;
            }
            else if (!m_link_timeline && std::abs(e.x - ex0) <= GRAB) {
                if (alt) { m_marker_drag = 6;
                           m_marker_off = x_to_tick(e.x) - m_sel_start; }
                else m_marker_drag = 3;
            }
            else if (!m_link_timeline && std::abs(e.x - ex1) <= GRAB) {
                if (alt) { m_marker_drag = 6;
                           m_marker_off = x_to_tick(e.x) - m_sel_start; }
                else m_marker_drag = 4;
            }
            else if (std::abs(e.x - xl) <= GRAB) {
                if (alt) { m_marker_drag = 5;
                           m_marker_off = x_to_tick(e.x) - m_perf->get_left_tick(); }
                else m_marker_drag = 1;
            }
            else if (std::abs(e.x - xr) <= GRAB) {
                if (alt) { m_marker_drag = 5;
                           m_marker_off = x_to_tick(e.x) - m_perf->get_left_tick(); }
                else m_marker_drag = 2;
            }
            // The LOOP / transport-readout chips live on the strip; a click on
            // one must not fall through and yank the playhead.  Nor may empty
            // strip space: only the TIME BAND scrubs / selects.
            else if (e.y < rect.y + toolstrip_h()) { /* toolbar: no scrub */ }
            // Selector tool in the time band drags a TIMELINE SELECTION
            // (ch.30 p683); every other tool scrubs the playhead, snapped
            // (hold ALT for the free position).
            else if (m_edit_tool == EditTool::Range) {
                m_tl_selecting = true;
                m_tl_anchor = esnap(x_to_tick(e.x));
                if (m_tl_anchor < 0) m_tl_anchor = 0;
            }
            else { m_scrubbing = true; seek_to(scrub_tick(e.x)); }
        }
        app.request_redraw();
        return true;
    }

    // (c) canvas -------------------------------------------------------------
    if (e.x >= rect.x + header_w && e.y >= canvas_y()) {
        int r = row_at(e.y);
        if (r < 0) return true;                   // below every lane
        std::vector<int> act = active_list();
        int idx = m_v_offset + r;
        if (idx < 0 || idx >= (int)act.size()) return true;
        press_canvas(app, e, act[idx]);
    }
    return true;
}

void ArrangeView::press_canvas(App& app, const MouseEv& e, int seq)
{
    if (!m_perf->is_active(seq)) return;
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;

    m_focus_lane = lane_key(seq);      // paste / keyboard edits land here

    // clear any prior selection on the previously-touched track -- but NOT
    // while Shift/Ctrl are held: those are the add/toggle gestures, and this
    // pre-clear silently dropped the previously-clicked clip from the very
    // selection the modifier was building (Shift+click piece A, Shift+click
    // piece B, Shift+click B again -> A's lane rep press cleared B first, so
    // the "toggle out" flipped it straight back IN).
    if ((SDL_GetModState() & (KMOD_SHIFT | KMOD_CTRL)) == 0 &&
        m_drop_seq >= 0 && m_drop_seq != seq && m_perf->is_active(m_drop_seq)) {
        if (m_audio.count(m_drop_seq)) region_for(m_drop_seq).selected = false;
        else if (sequence* prev = m_perf->get_sequence(m_drop_seq))
            prev->unselect_triggers();      // was an unchecked deref
    }

    long tick = x_to_tick(e.x);
    m_drop_seq  = seq;
    m_drop_tick = tick;
    int clip_seq = clip_sequence_at(seq, tick);
    if (clip_seq >= 0) {
        seq = clip_seq;
        s = m_perf->get_sequence(seq);
        if (!s) return;
        m_drop_seq = seq;
    }

    // Left-press on an AUDIO clip's fade handle -> begin a fade drag (priority
    // over clip move/resize).  Grab/Smart only: the Trim and Selector tools
    // own the clip corners for their own gestures.
    if (e.button == SDL_BUTTON_LEFT && clip_seq >= 0 && m_audio.count(seq) &&
        (m_edit_tool == EditTool::Grab || m_edit_tool == EditTool::Smart)) {
        SDL_Rect clip;
        if (clip_rect_of(seq, clip)) {
            FadeGrab g = fade_at(seq, clip, e.x, e.y);
            if (g != FadeGrab::None) {
                m_fade_grab = g; m_fade_seq = seq;
                m_mouse_down = true;
                if (!m_clipFade.count(seq)) m_clipFade[seq] = ClipFade{};
                m_fade_move_before.clear();          // one undo entry per gesture
                capture_fade_state({ seq }, m_fade_move_before);
                app.request_redraw();
                return;
            }
            // Grabber: clicking a FADE (not its handles) selects the fade as
            // an object -- Delete removes it, +/- nudges it, dragging moves it
            // within its clips (ch.32 p757-759).
            if (m_edit_tool == EditTool::Grab) {
                int which = -1;
                const long ft = x_to_tick(e.x);
                if (fade_hit(seq, ft, which)) {
                    m_fade_sel_seq = seq; m_fade_sel_which = which;
                    m_fade_move = true; m_mouse_down = true;
                    m_fade_move_ref = ft;
                    m_fade_move_before.clear();
                    if (which == 2) {
                        int xl = -1, xr = -1;
                        long a = 0, b = 0;
                        if (xfade_pair(seq, xl, xr) && xfade_window(xl, xr, a, b)) {
                            capture_fade_state({ xl, xr }, m_fade_move_before);
                            m_fade_move_p0 = a; m_fade_move_p1 = b;
                        }
                    } else {
                        capture_fade_state({ seq }, m_fade_move_before);
                        const AudioRegion& rg = region_for(seq);
                        if (which == 0) { m_fade_move_p0 = rg.position; m_fade_move_p1 = rg.source; }
                        else            { m_fade_move_p0 = rg.position + rg.length; m_fade_move_p1 = 0; }
                    }
                    app.request_redraw();
                    return;
                }
            }
        }
    }

    // right-click opens the clip context menu (Add / Open / Delete)
    if (e.button == SDL_BUTTON_RIGHT) {
        m_menu_open    = true;
        m_menu_x       = e.x;  m_menu_y = e.y;
        m_menu_seq     = seq;  m_menu_tick = tick;
        m_menu_on_clip = clip_seq >= 0;
        app.request_redraw();
        return;
    }
    // middle-click splits a clip at the click point (Ardour split-at-edit-point)
    if (e.button == SDL_BUTTON_MIDDLE) {
        split_clip_at(seq,tick);
        app.request_redraw();
        return;
    }
    if (e.button != SDL_BUTTON_LEFT) return;

    // Row index (into active_list) of the pressed lane, for the selection model.
    const int press_row = [&]() {
        std::vector<int> act = active_list();
        for (int i = 0; i < (int)act.size(); ++i)
            if (lane_key(act[(size_t)i]) == lane_key(m_drop_seq)) return i;
        return 0;
    }();

    // ---- ZOOMER: click/drag zoom gestures on the canvas (ch.29 p645-647) --
    if (m_edit_tool == EditTool::Zoom) {
        if (SDL_GetModState() & KMOD_CTRL) {       // Ctrl+drag = continuous
            m_zoomer_cont = true; m_zoomer_cx = e.x; m_zoomer_cy = e.y;
            m_zoomer_s0 = m_scale_x; m_zoomer_h0 = row_h;
        } else {
            m_zoomer_band = true; m_zoomer_x0 = m_zoomer_x1 = e.x;
        }
        app.request_redraw();
        return;
    }

    // ---- SCRUBBER: drag scrubs the pressed track (ch.29 p663) --------------
    // Alt = Shuttle mode; motion continues through the m_scrubbing drag path.
    // The audition itself needs the shell's on_scrub_audition; unbound, the
    // playhead follows honestly with no sound while stopped.
    if (m_edit_tool == EditTool::Scrub) {
        m_scrubbing = true;
        const long t = x_to_tick(e.x);
        if (on_scrub_audition) on_scrub_audition(seq, t);
        else seek_to(t);
        app.request_redraw();
        return;
    }

    // ---- PENCIL on an audio clip: destructive repair at sample zoom --------
    if (m_edit_tool == EditTool::Draw && clip_seq >= 0 && m_audio.count(seq)) {
        double bpm = m_perf->get_bpm(); if (bpm < 1.0) bpm = 120.0;
        double rate = 48000.0;
        if (m_audio[seq] && m_audio[seq]->sampleRate > 0) rate = m_audio[seq]->sampleRate;
        const double tps = (double)c_ppqn * bpm / 60.0 / rate;
        if (m_scale_x <= tps * 1.5) {
            m_pencil_audio = true; m_pencil_seq = seq;
            pencil_redraw(seq, e.x, e.y);
        } else {
            // status line: the manual's warning, surfaced before any damage
            m_guide_on = false;
            m_guide_text = "PENCIL edits audio DESTRUCTIVELY - zoom to sample level first";
            m_mouse_down = false;
        }
        app.request_redraw();
        return;
    }

    // ---- SELECTOR (ch.29 p659, ch.30): edit cursor + range selection -------
    const auto selector_press = [&]() {
        const long t = esnap(tick) < 0 ? 0 : esnap(tick);
        const unsigned now = SDL_GetTicks();
        const bool nearLast = std::labs(tick - m_last_click_tick) <= (long)(24 * m_scale_x);
        m_click_count = (m_last_click_seq == m_drop_seq && nearLast &&
                         now - m_last_click_ms < 400) ? m_click_count + 1 : 1;
        m_last_click_seq = m_drop_seq; m_last_click_tick = tick; m_last_click_ms = now;
        if (m_click_count >= 3) {                  // triple-click: whole track
            for (int cs : lane_sequences(m_drop_seq)) {
                if (m_audio.count(cs)) { region_for(cs).selected = true; continue; }
                if (sequence* ls = m_perf->get_sequence(cs)) {
                    std::vector<ClipSpan> sp; clip_spans(cs, sp);
                    for (const ClipSpan& c : sp) ls->select_trigger(c.on);
                }
            }
            set_edit_selection(0, std::max<long>(m_measure_len, song_end()),
                               press_row, press_row);
            m_mouse_down = false;
            return;
        }
        if (m_click_count == 2 && clip_seq >= 0) { // double-click: whole clip
            std::vector<ClipSpan> sp; clip_spans(seq, sp);
            for (const ClipSpan& c : sp)
                if (tick >= c.on && tick < c.endEx) {
                    if (m_audio.count(seq)) region_for(seq).selected = true;
                    else if (sequence* cs2 = m_perf->get_sequence(seq))
                        cs2->select_trigger(c.on);
                    set_edit_selection(c.on, c.endEx, press_row, press_row);
                    break;
                }
            m_mouse_down = false;
            return;
        }
        if ((SDL_GetModState() & KMOD_SHIFT) && m_sel_start >= 0) {
            // Shift-click / Shift-drag: move the NEARER end (ch.30 p675)
            long a = m_sel_start, b = m_sel_end;
            if (std::labs(t - a) < std::labs(t - b)) a = t; else b = t;
            set_edit_selection(a, b, std::min(m_sel_lo, press_row),
                               std::max(m_sel_hi, press_row));
            m_selecting = true;
            m_sel_anchor = (a == t) ? b : a;
            m_sel_anchor_row = press_row;
            return;
        }
        m_selecting = true;
        m_sel_anchor = t;
        m_sel_anchor_row = press_row;
        set_edit_selection(t, t, press_row, press_row);
        // The edit cursor drives the play start while linked and stopped.
        if (m_link_timeline && !m_perf->running()) seek_to(t);
    };
    if (m_edit_tool == EditTool::Range) {
        // ch.32 p756: Ctrl-click with the Selector adjusts fades exactly as
        // the Smart tool's fade zone does.
        if ((SDL_GetModState() & KMOD_CTRL) && clip_seq >= 0 && m_audio.count(seq)) {
            SDL_Rect body;
            if (clip_rect_of(seq, body)) {
                FadeGrab g = fade_at(seq, body, e.x, e.y);
                if (g == FadeGrab::InCurve || g == FadeGrab::OutCurve) {
                    if (!m_clipFade.count(seq)) m_clipFade[seq] = ClipFade{};
                    m_fade_move_before.clear();
                    capture_fade_state({ seq }, m_fade_move_before);
                    m_fade_grab = g; m_fade_seq = seq;
                    m_mouse_down = true;
                    app.request_redraw();
                    return;
                }
            }
        }
        selector_press();
        app.request_redraw();
        return;
    }

    if(m_edit_tool==EditTool::Cut){
        if(clip_seq>=0)split_clip_at(seq,tick);
        m_mouse_down=false;app.request_redraw();return;
    }

    if ((SDL_GetModState() & KMOD_SHIFT) != 0) {
        // Shift-click toggles noncontiguous whole clips in and out of the
        // selection -- the Object Grabber behaviour (ch.30 p673), now for
        // EVERY grabber mode: picking the pieces of a cut for Consolidate must
        // not require discovering a grabber sub-mode first.  Disabled in
        // Shuffle and Spot, as before.
        if (m_edit_tool == EditTool::Grab && clip_seq >= 0 &&
            m_edit_mode != EditMode::Shuffle && m_edit_mode != EditMode::Spot) {
            if (m_audio.count(seq)) {
                AudioRegion& r = region_for(seq);
                r.selected = !r.selected;
            } else if (sequence* os = m_perf->get_sequence(seq)) {
                std::vector<ClipSpan> sp; clip_spans(seq, sp);
                for (const ClipSpan& c : sp)
                    if (tick >= c.on && tick < c.endEx) {
                        if (c.selected) os->unselect_triggers();
                        else            os->select_trigger(c.on);
                        break;
                    }
            }
            m_mouse_down = false;
            app.request_redraw();
            return;
        }
        // (The former Time-Grabber Shift-click range extension, ch.30 p672,
        // is superseded by the toggle above; extending a RANGE remains the
        // Selector tool's job.)
        m_lassoing = true;
        m_lasso_x0 = m_lasso_x1 = e.x;
        m_lasso_y0 = m_lasso_y1 = e.y;
        // Ctrl OR Shift keeps the existing selection so a lasso can ADD the
        // rest of the cut pieces to clips already picked by hand.
        if ((SDL_GetModState() & (KMOD_CTRL | KMOD_SHIFT)) == 0)
            unselect_all_triggers();
        app.request_redraw();
        return;
    }

    long seq_len = s->get_length();
    bool state = clip_seq >= 0;

    if (state) {
        bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;

        // MIDI clip LOOP / ONE-SHOT chip (top-right corner).  Tested FIRST:
        // before the selection is cleared, and before the double-click
        // bookkeeping -- two quick toggles on the chip would otherwise be read
        // as a double-click and open the piano roll on top of the clip.
        // Only a plain MIDI clip has this flag: an audio region's `loop` is a
        // source-wrap flag handled in the audio branch below, and an automation
        // region repeats on its own loopLength.
        if (!ctrl && !m_audio.count(seq) && !is_automation(seq)) {
            long con = -1, coff = -1;
            {
                s->reset_draw_trigger_marker();
                long on, off, offs; bool tsel;
                while (s->get_next_trigger(&on, &off, &tsel, &offs))
                    if (tick >= on && tick <= off) { con = on; coff = off; break; }
            }
            SDL_Rect cr;
            if (con >= 0 && clip_rect_span(seq, con, coff + 1, cr)) {
                const SDL_Rect chip = loop_chip_rect(cr);
                if (chip.w > 0 &&
                    e.x >= chip.x && e.x < chip.x + chip.w &&
                    e.y >= chip.y && e.y < chip.y + chip.h) {
                    s->set_loop_enabled(!s->get_loop_enabled());
                    m_last_click_seq = -1;         // never pairs into a dbl-click
                    m_mouse_down = false;          // a toggle, not a drag
                    app.request_redraw();
                    return;
                }
            }
        }

        m_press_px = e.x; m_press_py = e.y;   // click-vs-drag reference

        // Plain click is an exclusive REGION selection. The previous code only
        // cleared selection when the lane representative changed; sibling
        // slices use different sequence ids on the same lane, so old slices
        // stayed selected invisibly and Delete removed them too.
        //
        // EXCEPT when the clip under the pointer is ALREADY part of a
        // multi-selection: grabbing one clip of a group must move the group,
        // the way every DAW behaves.  Clearing here is why a multi-selection
        // could never be dragged at all -- Shift restarts a lasso and Ctrl
        // duplicates, so a plain press was the only way left in, and it threw
        // the selection away before the drag even began.
        const bool group_grab = clip_selected_at(seq, tick) && selected_clip_count() > 1;
        if(!ctrl && !group_grab)unselect_all_triggers();

        // double-click (no Ctrl) opens the clip's editor (piano roll).  Rename is
        // on the right-click menu ("Rename").
        unsigned now = SDL_GetTicks();
        // Same clip AND near the same spot: two deliberate clicks at opposite
        // ends of a long clip used to count as a double-click.
        const bool nearLast = std::labs(tick - m_last_click_tick) <= (long)(24 * m_scale_x);
        if (!ctrl && m_last_click_seq == seq && nearLast &&
            (now - m_last_click_ms) < 400) {
            m_last_click_seq = -1;
            if (m_audio.count(seq)) { if (on_open_sample_editor) on_open_sample_editor(seq); }
            else if (on_open_editor) on_open_editor(seq, is_automation(seq)?2:0);
            return;
        }
        m_last_click_seq = seq; m_last_click_tick = tick; m_last_click_ms = now;

        long start=0,end=0;
        if(m_audio.count(seq)) {
            AudioRegion& ar=region_for(seq); ar.selected=true;
            start=ar.position; end=ar.position+ar.length;
        } else {
            s->select_trigger(tick);
            //  Use the span actually UNDER THE POINTER.
            //  get_selected_trigger_start_tick() reports the LAST selected
            //  trigger in the list, not the grabbed one -- so with several clips
            //  selected on this lane `start` belonged to a different clip, and
            //  the edge test below then read a press in the middle of the
            //  grabbed clip as "you grabbed the left edge" and started a trim.
            start = -1; end = 0;
            std::vector<ClipSpan> spans;
            clip_spans(seq, spans);
            for (size_t i = 0; i < spans.size(); ++i)
                if (tick >= spans[i].on && tick < spans[i].endEx) {
                    start = spans[i].on; end = spans[i].endEx; break;
                }
            if (start < 0) {
                start = s->get_selected_trigger_start_tick();
                end   = s->get_selected_trigger_end_tick() + 1;
            }
        }

        // ---- ch.29: MODE + TOOL dispatch on a clip -------------------------
        const SDL_Keymod pmod = SDL_GetModState();
        // SPOT mode: pressing a clip (or its edges with the Trim tool) opens
        // the Spot dialog instead of starting a drag (p640, p653-655).
        if (m_edit_mode == EditMode::Spot &&
            (m_edit_tool == EditTool::Grab || m_edit_tool == EditTool::Trim ||
             m_edit_tool == EditTool::Smart)) {
            const int xs = tick_to_x(start), xe = tick_to_x(end);
            const bool nearL = e.x - xs <= 6, nearR = xe - e.x <= 6;
            const bool trimT = m_edit_tool == EditTool::Trim;
            int kind = 0;
            if (trimT || nearL || nearR) {
                bool leftEdge = trimT ? (nearL || (!nearR && e.x < (xs + xe) / 2))
                                      : nearL;
                if (pmod & KMOD_ALT) leftEdge = !leftEdge;
                kind = (trimT && m_trim_mode == 1) ? (leftEdge ? 3 : 4)
                                                   : (leftEdge ? 1 : 2);
            }
            m_mouse_down = false;
            open_spot(app, seq, kind);
            return;
        }

        // SMART TOOL: the zone under the pointer picks the function (p693).
        int smart = -1;
        if (m_edit_tool == EditTool::Smart) {
            SDL_Rect body;
            if (clip_rect_span(seq, start, end, body))
                smart = smart_zone(seq, body, e.x, e.y);
            if (smart == 8) {          // Ctrl held: temporary Scrubber
                m_scrubbing = true;
                if (on_scrub_audition) on_scrub_audition(seq, x_to_tick(e.x));
                else seek_to(x_to_tick(e.x));
                app.request_redraw();
                return;
            }
            if (smart == 0) {          // upper middle: Selector
                selector_press();
                app.request_redraw();
                return;
            }
            if ((smart == 4 || smart == 5 || smart == 7) && m_audio.count(seq)) {
                // top corners: pull a fade out; over a fade: reshape it.
                SDL_Rect body2;
                if (clip_rect_span(seq, start, end, body2)) {
                    if (!m_clipFade.count(seq)) m_clipFade[seq] = ClipFade{};
                    m_fade_move_before.clear();
                    capture_fade_state({ seq }, m_fade_move_before);
                    if (smart == 7) {
                        FadeGrab g = fade_at(seq, body2, e.x, e.y);
                        m_fade_grab = g != FadeGrab::None ? g : FadeGrab::InCurve;
                    } else {
                        m_fade_grab = smart == 4 ? FadeGrab::InLen : FadeGrab::OutLen;
                    }
                    m_fade_seq = seq;
                    app.request_redraw();
                    return;
                }
            }
            if (smart == 6 && m_audio.count(seq)) {
                // bottom edge between two adjacent clips: CROSSFADE drag,
                // routed through the existing fade machinery on both clips.
                long bestGap = -1; int other = -1; long bound = 0; bool otherLeft = false;
                std::vector<ClipSpan> os;
                for (int cs : lane_sequences(m_drop_seq)) {
                    if (cs == seq || !m_audio.count(cs)) continue;
                    os.clear(); clip_spans(cs, os);
                    for (const ClipSpan& o : os) {
                        const long g1 = std::labs(o.on - end);      // other right
                        const long g2 = std::labs(start - o.endEx); // other left
                        const long g = std::min(g1, g2);
                        if (bestGap < 0 || g < bestGap) {
                            bestGap = g; other = cs;
                            otherLeft = g2 < g1;
                            bound = otherLeft ? start : end;
                        }
                    }
                }
                if (other >= 0) {
                    m_xfade_drag = true;
                    m_xfade_left  = otherLeft ? other : seq;
                    m_xfade_right = otherLeft ? seq : other;
                    m_xfade_bound = bound;
                    if (!m_clipFade.count(m_xfade_left))  m_clipFade[m_xfade_left]  = ClipFade{};
                    if (!m_clipFade.count(m_xfade_right)) m_clipFade[m_xfade_right] = ClipFade{};
                    m_fade_move_before.clear();
                    capture_fade_state({ m_xfade_left, m_xfade_right }, m_fade_move_before);
                    app.request_redraw();
                    return;
                }
            }
            // smart 1 (grabber) falls through to the move logic below;
            // smart 2/3 (edges) go through the Trim press next.
        }

        // TRIM TOOL (and the Smart tool's edge zones): the whole body is a
        // trim zone split at the middle; Alt reverses the direction (p653).
        if (m_edit_tool == EditTool::Trim ||
            (m_edit_tool == EditTool::Smart && (smart == 2 || smart == 3))) {
            const int xs = tick_to_x(start), xe = tick_to_x(end);
            bool leftEdge = m_edit_tool == EditTool::Trim
                          ? (e.x < (xs + xe) / 2) : (smart == 2);
            if (pmod & KMOD_ALT) leftEdge = !leftEdge;
            push_undo("Trim Clip");
            m_trim_start0 = start; m_trim_end0 = end;
            // TANDEM (Ctrl): both ends of two adjacent OVERLAPPING clips move
            // together.  Standard / Scrub / Loop trim only, never TCE (p658).
            if ((pmod & KMOD_CTRL) && m_trim_mode != 1 && m_audio.count(seq)) {
                int other = -1; bool otherLeft = false;
                std::vector<ClipSpan> os;
                for (int cs : lane_sequences(m_drop_seq)) {
                    if (cs == seq || !m_audio.count(cs)) continue;
                    os.clear(); clip_spans(cs, os);
                    for (const ClipSpan& o : os)
                        if (o.on < end && o.endEx > start) {
                            other = cs; otherLeft = o.on < start;
                            break;
                        }
                    if (other >= 0) break;
                }
                if (other >= 0) {
                    m_tandem = true;
                    m_tandem_left  = otherLeft ? other : seq;
                    m_tandem_right = otherLeft ? seq : other;
                    m_tandem_bound0 = tick;
                    app.request_redraw();
                    return;
                }
            }
            if (m_edit_tool == EditTool::Trim && m_trim_mode == 2 &&
                m_audio.count(seq)) {                     // SCRUB TRIM
                m_scrubtrim = true; m_scrubtrim_left = leftEdge;
                m_move_seq = seq;
                m_press_px = e.x; m_press_py = e.y;
                app.request_redraw();
                return;
            }
            if (m_edit_tool == EditTool::Trim && m_trim_mode == 3 &&
                m_audio.count(seq)) {                     // LOOP TRIM
                SDL_Rect cr;
                if (clip_rect_span(seq, start, end, cr) &&
                    e.y > cr.y + cr.h / 2) {
                    // bottom half / loop glyph: trim the SOURCE ITERATION
                    // while the overall looped length stays constant (p657).
                    m_looptrim_src = true; m_move_seq = seq;
                    app.request_redraw();
                    return;
                }
                // top half: loop-trim = change how long the clip is looped.
                // Creating a loop out of an unlooped clip is exactly what the
                // Loop Trim tool is for (p656).
                AudioRegion& r = region_for(seq);
                if (!r.loop) {
                    r.loop = true;
                    if (r.loopLength <= 0) r.loopLength = r.length;
                    if (on_clip_loop) on_clip_loop(seq, true);
                }
            }
            m_growing = true; m_grow_dir = leftEdge; m_move_seq = seq;
            m_drop_offset = tick - (leftEdge ? start : end);
            m_tce_drag = (m_edit_tool == EditTool::Trim && m_trim_mode == 1 &&
                          m_audio.count(seq));
            m_tce_len0 = end - start;
            // Standard Trim trims the edges of ALL selected clips (p653):
            // capture the other selected audio regions' geometry at press.
            m_trim_group.clear();
            if (m_edit_tool == EditTool::Trim && m_trim_mode == 0) {
                for_each_clip([&](const ClipSpan& cs2) {
                    if (!cs2.selected || cs2.seq == seq) return;
                    if (!m_audio.count(cs2.seq)) return;
                    const AudioRegion& rr = region_for(cs2.seq);
                    m_trim_group.push_back(TrimRef{ cs2.seq, rr.position,
                                                    rr.length, rr.source });
                });
            }
            app.request_redraw();
            return;
        }

        // SEPARATION GRABBER (p660): dragging inside the edit selection
        // separates it into its own clip and moves it; Alt-drag copies it out
        // without disturbing the original.
        if (m_edit_tool == EditTool::Grab && m_grab_mode == 1 &&
            m_sel_start >= 0 && m_sel_end > m_sel_start &&
            tick >= m_sel_start && tick < m_sel_end) {
            if (pmod & KMOD_ALT) {
                m_copying = true; m_moving = true;
                m_copy_len = m_sel_end - m_sel_start;
                m_copy_offset = m_audio.count(seq)
                    ? region_for(seq).source + (m_sel_start - region_for(seq).position)
                    : trigger_offset_at(s, tick) + (m_sel_start - start);
                m_drop_offset  = tick - m_sel_start;
                m_ghost_tick   = m_sel_start;
                m_copy_src_tick= m_sel_start;
                m_press_px = e.x; m_press_py = e.y;
                app.request_redraw();
                return;
            }
            push_undo("Separate Clip");
            if (m_sel_start > start) split_clip_at(m_drop_seq, m_sel_start);
            if (m_sel_end   < end)   split_clip_at(m_drop_seq, m_sel_end);
            const int mid = clip_sequence_at(m_drop_seq, m_sel_start);
            if (mid >= 0) {
                unselect_all_triggers();
                if (m_audio.count(mid)) region_for(mid).selected = true;
                else if (sequence* msq = m_perf->get_sequence(mid))
                    msq->select_trigger(m_sel_start);
                m_moving = true; m_move_seq = mid; m_drop_seq = mid;
                m_drop_offset = tick - m_sel_start;
                m_trim_start0 = m_sel_start; m_trim_end0 = m_sel_end;
                m_press_px = e.x; m_press_py = e.y;
            }
            app.request_redraw();
            return;
        }

        // Audio-clip corner/edge affordances: LOOP toggle (top-right), EXTEND
        // (bottom edge), GAIN line.  Checked before the move/trim decision.
        if (m_audio.count(seq)) {
            SDL_Rect cr;
            if (clip_rect_of(seq, cr)) {
                // Loop handle: the top-right chip -> toggle the REGION's
                // source-wrap loop.  The rect comes from loop_chip_rect(), the
                // same call the painter uses, so the icon and the hot area are
                // the same pixels (they were spelled out twice and differed by
                // one column).
                const SDL_Rect chip = loop_chip_rect(cr);
                if (chip.w > 0 &&
                    e.x >= chip.x && e.x < chip.x + chip.w &&
                    e.y >= chip.y && e.y < chip.y + chip.h) {
                    AudioRegion& r = region_for(seq);
                    r.loop = !r.loop;
                    if (on_clip_loop) on_clip_loop(seq, r.loop);
                    m_last_click_seq = -1;   // two quick toggles != double-click
                    m_mouse_down = false;
                    app.request_redraw();
                    return;
                }
                // Extend handle: lower right-side box -> drag horizontally.
                SDL_Rect eb{ cr.x + cr.w - 15, cr.y + cr.h - 13, 12, 12 };
                if (e.x >= eb.x && e.x < eb.x + eb.w &&
                    e.y >= eb.y && e.y < eb.y + eb.h) {
                    m_extending = true; m_extend_seq = seq;
                    return;
                }
                // Gain: only the CENTRE handle grabs (not the whole width) so the
                // clip body stays grabbable for moves (the line sits at the clip's
                // vertical middle at unity gain).
                const int hx  = cr.x + cr.w / 2;
                const int gly = gain_line_y(seq, cr.y, cr.h);
                if (e.x >= hx - 12 && e.x <= hx + 12 && e.y >= gly - 4 && e.y <= gly + 4) {
                    m_gain_grab = true; m_gain_seq = seq;
                    return;
                }
            }
        }

        if (ctrl) {
            // Ctrl+DRAG duplicates: leave the original, drag a ghost, and add a
            // new trigger for the SAME sequence at the drop tick on release.
            // Ctrl+CLICK does NOT: it is the add-to-selection gesture (the
            // selection was deliberately left alone just above), and committing
            // on every release stacked a second, independent sequence on top of
            // the original -- same lane, same tick, nothing to see, the part
            // silently playing twice.  The release checks both the pointer
            // travel and the landing tick before it commits.
            m_copying      = true;
            m_moving       = true;         // route motion through drag_canvas
            m_copy_len     = end - start;
            m_copy_offset  = m_audio.count(seq)?region_for(seq).source:trigger_offset_at(s,tick);
            m_drop_offset  = tick - start;
            m_ghost_tick   = start;
            m_copy_src_tick= start;
            m_press_px     = e.x;
            m_press_py     = e.y;
        } else {
            // select + decide move vs. resize by proximity to the clip edges
            push_undo("Move Clip");
            int xs = tick_to_x(start), xe = tick_to_x(end);
            const int handle = 6;
            if (e.x - xs <= handle) {
                m_growing = true; m_grow_dir = true;  m_drop_offset = tick - start; m_move_seq = seq;
                m_trim_start0 = start; m_trim_end0 = end;
            } else if (xe - e.x <= handle) {
                m_growing = true; m_grow_dir = false; m_drop_offset = tick - end; m_move_seq = seq;
                m_trim_start0 = start; m_trim_end0 = end;
            } else if ((SDL_GetModState() & KMOD_ALT) && m_audio.count(seq)) {
                // Alt+drag an audio body = SLIP: slide the source under the block.
                m_slipping = true; m_move_seq = seq;
                m_slip_ref_tick = tick; m_slip_ref_source = region_for(seq).source;
            } else {
                m_moving = true; m_drop_offset = tick - start; m_move_seq = seq;
                m_trim_start0 = start; m_trim_end0 = end;   // Relative Grid ref
                // Snapshot the group ONCE, at the press: every selected clip's
                // lane-sequence and where its selection sat when the drag began.
                m_move_group.clear();
                m_move_anchor = start;
                if (group_grab) {
                    m_group_click_seq  = seq;
                    m_group_click_tick = tick;
                    std::vector<int> seen;
                    for_each_clip([&](const ClipSpan& cs) {
                        if (!cs.selected) return;
                        for (size_t i = 0; i < seen.size(); ++i)
                            if (seen[i] == cs.seq) return;
                        seen.push_back(cs.seq);
                        MoveGroup g;
                        g.seq   = cs.seq;
                        g.audio = m_audio.count(cs.seq) != 0;
                        g.start = g.audio ? region_for(cs.seq).position
                                          : selected_group_start(cs.seq);
                        if (g.start >= 0) m_move_group.push_back(g);
                    });
                    if (m_move_group.size() < 2) m_move_group.clear();
                }
            }
        }
    } else {
        // Empty instrument lanes insert on DOUBLE-click.  Keep audio-lane
        // placement unchanged: imported/recorded audio has its own source flow.
        // An audio lane is one carrying audio -- it used to be sniffed from the
        // track NAME ("Audio "), so renaming an audio track made double-click
        // start dropping MIDI clips onto it.
        bool audioLane = false;
        {
            std::vector<int> ls = lane_sequences(seq);
            for (size_t i = 0; i < ls.size(); ++i)
                if (m_audio.count(ls[i])) { audioLane = true; break; }
        }
        const long snappedClick = esnap(tick);
        if (!audioLane && m_edit_tool!=EditTool::Draw) {
            const unsigned now = SDL_GetTicks();
            const bool secondClick =
                m_last_click_seq == seq &&
                now - m_last_click_ms < 400 &&
                snappedClick == snap(m_last_click_tick);
            if (!secondClick) {
                m_last_click_seq = seq;
                m_last_click_tick = tick;
                m_last_click_ms = now;
                // A press on empty instrument-lane space establishes both the
                // paste anchor and a lasso origin. A click-release is harmless;
                // dragging selects every intersecting clip across lanes.
                m_lassoing = true;
                m_lasso_x0 = m_lasso_x1 = e.x;
                m_lasso_y0 = m_lasso_y1 = e.y;
                app.request_redraw();
                return;
            }
            m_last_click_seq = -1;
        }
        if (audioLane && m_edit_tool != EditTool::Draw) {
            // AUDIO-lane empty space: drag is a RUBBER-BAND selection, exactly
            // as on instrument lanes.  This used to fall through to the
            // "Draw Clip" path below, so dragging over the pieces of a cut to
            // select them instead scribbled a fresh empty region onto the
            // audio lane.  Audio arrives via record/import/consolidate; only
            // the Draw tool places clips by hand here.
            m_lassoing = true;
            m_lasso_x0 = m_lasso_x1 = e.x;
            m_lasso_y0 = m_lasso_y1 = e.y;
            if ((SDL_GetModState() & (KMOD_CTRL | KMOD_SHIFT)) == 0)
                unselect_all_triggers();
            app.request_redraw();
            return;
        }
        // empty lane: place a fresh one-clip-length region, snapped to the view
        // grid.  Carry the loop offset so the clip plays from its OWN content
        // start at the drop point (Ardour-style), not phase-locked to the global
        // timeline.  On an empty lane trigger_offset_at() is 0; the (t % seq_len)
        // term cancels the timeline phase so content tick 0 lands on the region
        // start.  When the grid snaps to a bar (seq_len multiple) this reduces to
        // offset 0, matching the previous behaviour.
        push_undo("Draw Clip");
        long t = snappedClick;
        if (t < 0) t = 0;
        // A pattern with no length would make both modulos divide by zero (the
        // context-menu path already guarded this; the click path did not).
        if (seq_len < 1) seq_len = c_ppqn * 4;
        long base = trigger_offset_at(s, t);          // 0 on an empty lane
        long off  = (base + (t % seq_len)) % seq_len;
        int new_seq = create_pattern(seq, t, seq_len, off, false);
        if (new_seq >= 0) m_drop_seq = new_seq;
        m_adding = true;
        m_drop_tick = t;                          // a tick inside the new clip
    }
    app.request_redraw();
}

void ArrangeView::drag_canvas(App& app, const MouseEv& e)
{
    if(m_range_drag){
        long t=snap(x_to_tick(e.x));
        long a=std::min(m_range_anchor,t),b=std::max(m_range_anchor,t);
        if(b<=a)b=a+std::max<long>(1,m_snap);
        m_perf->set_left_tick(a);m_perf->set_right_tick(b);
        m_guide_on=true; m_guide_tick=t;
        m_guide_text="range "+bbt(a)+"  "+bars_len(b-a);
        app.request_redraw();return;
    }
    if (m_drop_seq < 0 || !m_perf->is_active(m_drop_seq)) return;
    sequence* s = m_perf->get_sequence(m_drop_seq);
    if (!s) return;
    long tick = x_to_tick(e.x);

    // Live snapped guide + bar.beat readout for whatever gesture is running, so
    // an edit can be placed exactly without reading the ruler out of the corner
    // of your eye.  Refined per branch below.
    m_guide_on   = true;
    m_guide_tick = esnap(tick);
    m_guide_text = bbt(m_guide_tick);

    if (m_copying) {
        // Ctrl+drag duplicate: just track the ghost; commit happens on release.
        long t = esnap(tick - m_drop_offset);
        if (m_edit_mode == EditMode::Shuffle)
            t = shuffle_pack(m_drop_seq, -1, tick - m_drop_offset);
        if (t < 0) t = 0;
        m_ghost_tick = t;
        m_guide_tick = t;
        m_guide_text = "copy -> " + bbt(t);
        app.request_redraw();
        return;
    }
    if (m_extending && m_extend_seq >= 0) {
        // EXTEND (bottom-edge drag): grow the trigger's END to lengthen the clip.
        // For a looped region the source wraps to fill the new span; otherwise it
        // is clamped to the source on release (commit_region right-trim law).
        AudioRegion& r=region_for(m_extend_seq);
        long newEnd=std::max<long>(r.position+1,esnap(tick));
        if ((SDL_GetModState() & KMOD_CTRL) && r.loop && r.loopLength > 0) {
            // constrain to WHOLE loop iterations (Loop Trim + Ctrl, p657)
            long k = (newEnd - r.position + r.loopLength / 2) / r.loopLength;
            if (k < 1) k = 1;
            newEnd = r.position + k * r.loopLength;
        }
        r.length=newEnd-r.position;
        commit_region(m_extend_seq);
        m_guide_tick = newEnd;
        m_guide_text = "len " + bars_len(r.length);
        app.request_redraw();
        return;
    }
    if (m_gain_grab && m_gain_seq >= 0) {
        // Drag the gain line: y within the region maps to gain 0..2 (unity mid).
        SDL_Rect cr;
        if (clip_rect_of(m_gain_seq, cr) && cr.h > 0) {
            float gn = 1.f - (float)(e.y - cr.y) / (float)cr.h;   // 0..1 bottom..top
            if (gn < 0.f) gn = 0.f; if (gn > 1.f) gn = 1.f;
            float g = gn * 2.f;                                   // 0..2
            region_for(m_gain_seq).gain = g;
            if (on_clip_gain) on_clip_gain(m_gain_seq, g);
            char db[32];
            std::snprintf(db, sizeof(db), "gain %+.1f dB",
                          g > 0.0001f ? 20.f * std::log10(g) : -120.f);
            m_guide_on = false;                 // no timeline guide for a gain drag
            m_guide_text = db;
        }
        app.request_redraw();
        return;
    }
    if (m_slipping && m_move_seq >= 0) {
        // SLIP (Ardour set_start): drag content RIGHT -> earlier source shows.
        AudioRegion& r = region_for(m_move_seq);
        long src = m_slip_ref_source - (tick - m_slip_ref_tick);
        std::map<int,long>::const_iterator lit = m_audioLen.find(m_move_seq);
        const long srcLen = (lit != m_audioLen.end() && lit->second > 0) ? lit->second : r.length;
        if (src < 0) src = 0;
        if (src > srcLen - r.length) src = srcLen - r.length;
        if (src < 0) src = 0;
        r.source = src;
        if (on_clip_region_changed) on_clip_region_changed(m_move_seq, r.position, r.length, r.source);
        app.request_redraw();
        return;
    }
    // AUDIO clips follow the Ardour REGION model.  The trigger stores only the
    // block's POSITION + LENGTH; the region's start-offset into the source is
    // tracked separately (m_region) because the trigger's loop-offset WRAPS
    // modulo the pattern length (a MIDI behavior that would corrupt an audio
    // offset).  So audio gestures never touch the trigger offset (adjust=false);
    // the Ardour content laws (move=content travels, left-trim=content anchored,
    // right-trim=length only) are applied to m_region on release.
    const bool isAudio = m_audio.count(m_drop_seq) != 0;
    if(isAudio) {
        AudioRegion& r=region_for(m_drop_seq);
        if (m_moving && !m_move_group.empty()) {
            // grabbed an audio clip that is part of a multi-selection
            long p = esnap(tick - m_drop_offset); if (p < 0) p = 0;
            move_clip_group(p);
            m_guide_tick = r.position;
            m_guide_text = bbt(r.position) + "  x" +
                           std::to_string((int)m_move_group.size());
            app.request_redraw(); return;
        }
        if(m_moving) {
            const long want = tick - m_drop_offset;
            long p;
            if (m_edit_mode == EditMode::Shuffle)
                // clips snap to each other; no overlap (p639)
                p = shuffle_pack(m_drop_seq, m_drop_seq, want);
            else if (m_edit_mode == EditMode::Grid && m_grid_relative)
                // Relative Grid: move BY grid increments, offset preserved
                p = m_trim_start0 + snap_rel(want - m_trim_start0);
            else
                p = esnap(want);
            if(p<0)p=0; r.position=p;
        } else if(m_growing) {
            if(m_grow_dir) {
                const long oldEnd=r.position+r.length;
                long p=esnap(tick-m_drop_offset); if(p<0)p=0;
                if (m_edit_mode == EditMode::Grid && m_grid_relative)
                    p = m_trim_start0 + snap_rel((tick - m_drop_offset) - m_trim_start0);
                if(p>=oldEnd)p=oldEnd-1;
                const long delta=p-r.position; r.position=p; r.source+=delta; r.length=oldEnd-p;
            } else {
                long e=esnap(tick-m_drop_offset);
                if (m_edit_mode == EditMode::Grid && m_grid_relative)
                    e = m_trim_end0 + snap_rel((tick - m_drop_offset) - m_trim_end0);
                if(e<=r.position)e=r.position+1;
                r.length=e-r.position;
            }
            // Standard Trim: apply the SAME edge delta to every other selected
            // audio clip, from its own press-time geometry (p653).
            if (!m_trim_group.empty()) {
                const long delta = m_grow_dir
                    ? r.position - m_trim_start0
                    : (r.position + r.length) - m_trim_end0;
                for (const TrimRef& tr : m_trim_group) {
                    AudioRegion& g = region_for(tr.seq);
                    if (m_grow_dir) {
                        long p2 = tr.pos + delta;
                        const long end2 = tr.pos + tr.len;
                        if (p2 < 0) p2 = 0;
                        if (p2 >= end2) p2 = end2 - 1;
                        g.position = p2; g.source = tr.src + (p2 - tr.pos);
                        g.length = end2 - p2;
                    } else {
                        long l2 = tr.len + delta;
                        if (l2 < 1) l2 = 1;
                        g.position = tr.pos; g.source = tr.src; g.length = l2;
                    }
                    commit_region(tr.seq);
                }
            }
        }
        commit_region(m_drop_seq);
        m_guide_tick = m_growing && !m_grow_dir ? r.position + r.length : r.position;
        if (m_tce_drag && m_tce_len0 > 0) {
            char pct[48];
            std::snprintf(pct, sizeof(pct), "TCE %ld%%  %s",
                          r.length * 100 / m_tce_len0,
                          on_clip_tce ? "" : "(needs shell wiring)");
            m_guide_text = pct;
        } else
            m_guide_text = bbt(r.position) + "  " + bars_len(r.length);
        app.request_redraw(); return;
    }
    if (m_adding) {
        long seq_len = s->get_length();
        //  grow_trigger() quantises the end to the clip's REPETITION -- but a
        //  ONE-SHOT has no repetition, so its unit is one tick and the end
        //  lands wherever the raw mouse tick fell.  That drew a clip whose end
        //  ignored SNAP entirely, while the guide line the drag paints was
        //  snapped: the readout and the result disagreed by up to a beat.
        //  Feed the one-shot the snapped tick so the two agree.  A looping clip
        //  keeps the raw tick it always got (its unit does the quantising).
        const long gt = s->get_loop_enabled()
                        ? tick
                        : std::max(m_drop_tick, m_guide_tick - 1);
        s->grow_trigger(m_drop_tick, gt, seq_len);
        m_guide_text = "draw  " + bbt(m_guide_tick);
    } else if (m_moving) {
        long t;
        if (m_edit_mode == EditMode::Shuffle)
            t = shuffle_pack(m_drop_seq, m_drop_seq, tick - m_drop_offset);
        else if (m_edit_mode == EditMode::Grid && m_grid_relative)
            t = m_trim_start0 + snap_rel((tick - m_drop_offset) - m_trim_start0);
        else
            t = esnap(tick - m_drop_offset);
        if (!m_move_group.empty()) {
            //  A multi-clip drag: shift the whole group by one delta.  Each
            //  lane-sequence gets an absolute target derived from where its own
            //  selection started, which is what
            //  sequence::move_selected_triggers_to() measures its delta from.
            move_clip_group(t);
            const long moved = selected_group_start(m_drop_seq);
            m_guide_tick = moved >= 0 ? moved : t;
            m_guide_text = bbt(m_guide_tick) + "  x" +
                           std::to_string((int)m_move_group.size());
        } else {
            s->move_selected_triggers_to(t, true);
            m_guide_tick = t;
            m_guide_text = bbt(t);
        }
    } else if (m_growing) {
        long t = esnap(tick - m_drop_offset);
        if (m_grow_dir) {
            //  LEFT TRIM = CONTENT STAYS PUT.  This is the law the audio path
            //  already states ("move=content travels, left-trim=content
            //  anchored, right-trim=length only") and the law every DAW uses:
            //  pulling a clip's left edge in reveals/hides content, it does not
            //  drag the content along.  play_span anchors a clip's content on
            //  its own start (anchor = trigger_start - offset), so moving the
            //  start edge WITHOUT moving the offset slides everything the clip
            //  plays by the drag distance.  Advance the offset by the same
            //  amount and the notes stay on the ticks they were on.
            const long before = s->get_selected_trigger_start_tick();
            s->move_selected_triggers_to(t, false, 0);
            const long after = s->get_selected_trigger_start_tick();
            if (before >= 0 && after >= 0 && after != before)
                shift_clip_content(s, after, after - before);
        }
        else            s->move_selected_triggers_to(t - 1, false, 1);
        const long a = s->get_selected_trigger_start_tick();
        const long b = s->get_selected_trigger_end_tick();
        m_guide_tick = m_grow_dir ? a : b;
        if (b > a) m_guide_text = bbt(a) + "  " + bars_len(b - a + 1);
    }
    // An AUTOMATION clip's block IS its region: push the new geometry every
    // frame of the drag so both the drawn curve window and the automation the
    // player emits follow the block instead of lagging a gesture behind.
    commit_auto_region(m_drop_seq);
    app.request_redraw();
}

bool ArrangeView::on_wheel(App& app, int dx, int dy)
{
    // wheel over the (non-modal) Undo History window scrolls its list
    if (undo_window_wheel(m_mx, m_my, dy)) { app.request_redraw(); return true; }

    // An open popup pins the view: zooming or panning underneath it left the
    // menu floating over content it no longer referred to (and its clip/tick
    // were captured at open time, so the next pick acted on the wrong place).
    if (any_menu_open() || m_help_open) { app.request_redraw(); return true; }

    const SDL_Keymod mod = SDL_GetModState();
    const bool ctrl  = (mod & KMOD_CTRL)  != 0;
    const bool shift = (mod & KMOD_SHIFT) != 0;
    const bool alt   = (mod & KMOD_ALT)   != 0;

    // Wheel over a counter / indicator field edits its value (ch.30 p677);
    // over the Grid / Nudge fields it steps the value ladder.
    if (dy != 0) {
        for (int i = 0; i < 4; ++i)
            if (pt_in_rect(m_counter_rect[i], m_mx, m_my)) {
                long cur = 0;
                switch (i) {
                case 0: cur = playhead(); break;
                case 1: cur = m_sel_start < 0 ? 0 : m_sel_start; break;
                case 2: cur = m_sel_end   < 0 ? 0 : m_sel_end;   break;
                default: cur = m_sel_start < 0 ? 0 : m_sel_end - m_sel_start; break;
                }
                const long step = ctrl ? 1 : m_beat_len;
                counter_apply(i, std::max<long>(0, cur + dy * step));
                app.request_redraw();
                return true;
            }
        if (pt_in_rect(m_grid_rect, m_mx, m_my)) {
            m_snap_idx = std::max(0, std::min(8, m_snap_idx - dy));
            m_snap = grid_ticks();
            app.request_redraw();
            return true;
        }
        if (pt_in_rect(m_nudge_rect, m_mx, m_my)) {
            m_nudge_idx = std::max(0, std::min(7, m_nudge_idx - dy));
            app.request_redraw();
            return true;
        }
    }

    // Alt+Shift = AUDIO vertical zoom, Alt+Ctrl = MIDI vertical zoom
    // (the ch.29 scroll-wheel zoom modifiers).
    if (alt && shift && dy != 0) {
        m_wave_zoom = std::max(0.25f, std::min(8.f,
                        m_wave_zoom * (dy > 0 ? 1.15f : 1.f / 1.15f)));
        app.request_redraw();
        return true;
    }
    if (alt && ctrl && dy != 0) {
        m_midi_zoom = std::max(0.25f, std::min(4.f,
                        m_midi_zoom * (dy > 0 ? 1.15f : 1.f / 1.15f)));
        app.request_redraw();
        return true;
    }

    // Ctrl + wheel = VERTICAL zoom: grow/shrink lane + header row height together
    // (both are laid out from row_h).  Plain wheel stays horizontal zoom below.
    if (ctrl && dy != 0) {
        int nh = row_h + (dy > 0 ? 6 : -6);
        if (nh < 20)  nh = 20;
        if (nh > 120) nh = 120;
        row_h = nh;
        app.request_redraw();
        return true;
    }

    // Shift + wheel = horizontal PAN, Alt + wheel = scroll lanes.  Both are the
    // conventional bindings; without them the wheel could only ever zoom, which
    // made a wide arrangement tedious to walk through.
    if (shift && dy != 0) {
        m_scroll_ticks -= (long)(dy * 3 * 32 * m_scale_x);
        clamp_scroll();
        app.request_redraw();
        return true;
    }
    if (alt && dy != 0) {
        m_v_offset -= dy;
        if (m_v_offset < 0) m_v_offset = 0;
        if (m_v_offset > max_v_offset()) m_v_offset = max_v_offset();
        app.request_redraw();
        return true;
    }

    if (dy != 0) {
        // vertical wheel = horizontal zoom (ticks per pixel).  Min is well below 1
        // tick/px so you can zoom down to individual audio samples.
        // Anchor on the pointer, or on the view CENTRE when no motion has been
        // seen yet (m_mx == -1 previously anchored hard at the left edge).
        const int anchorX = (m_mx < 0) ? canvas_x() + canvas_w() / 2
            : std::max(canvas_x(), std::min(canvas_x()+canvas_w(), m_mx));
        const long anchorTick=x_to_tick(anchorX);
        double f = (dy > 0) ? (1.0 / 1.2) : 1.2;
        double ns = m_scale_x * f;
        if (ns < kZoomMin) ns = kZoomMin;
        if (ns > kZoomMax) ns = kZoomMax;
        m_scale_x = ns;
        m_scroll_ticks=anchorTick-(long)((anchorX-canvas_x())*m_scale_x);
        clamp_scroll();
    }
    if (dx != 0) {
        m_scroll_ticks += (long)(dx * 8 * m_scale_x);
        clamp_scroll();
    }
    app.request_redraw();
    return true;
}

bool ArrangeView::on_key(App& app, SDL_Keycode k)
{
    if (!m_perf) return false;
    long page = (long)(canvas_w() * m_scale_x / 4);
    bool ctrl  = (SDL_GetModState() & KMOD_CTRL)  != 0;
    bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
    bool alt   = (SDL_GetModState() & KMOD_ALT)   != 0;
    // the manual's "Start" key (Windows/Super); some window managers grab it,
    // so every Start chord below also has a Ctrl+Alt fallback
    bool gui   = (SDL_GetModState() & KMOD_GUI)   != 0;

    // The help overlay is modal-ish: any key closes it (and ? toggles it).
    if (m_help_open) { m_help_open = false; app.request_redraw(); return true; }

    // ch.32 modal dialogs eat their keys first (Esc cancels, Enter is OK,
    // 1..5 / Ctrl+1..5 recall / store the fade presets).
    if (m_fdlg_open) {
        if (fade_dialog_key(app, k)) { app.request_redraw(); return true; }
        return true;
    }
    if (m_bdlg_open) {
        if (k == SDLK_ESCAPE) m_bdlg_open = false;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { m_bdlg_open = false; batch_apply(); }
        app.request_redraw();
        return true;
    }

    // A popup is modal too.  Without this, keys reached the view THROUGH an open
    // menu: "d" swapped the tool, Delete removed clips, Ctrl+Z undid an edit --
    // all while the menu sat there waiting for a pick.  Esc now closes it.
    if (any_menu_open()) {
        if (k == SDLK_ESCAPE) close_all_menus();
        app.request_redraw();
        return true;
    }

    // An active counter / Edit Selection indicator eats its keys first
    // (digits, subfield moves, calculator +/- and Enter -- ch.30 p677).
    if (m_counter_edit >= 0 && counter_key(app, k)) return true;

    // "*" on the numeric keypad highlights the Main counter (ch.30 p684).
    if (k == SDLK_KP_MULTIPLY) { counter_begin(0); app.request_redraw(); return true; }

    // ---- Shuttle Lock (ch.29 p664): Ctrl+numpad digit engages; further
    // digits change speed, +/- flip the direction, 0 or Space stops.
    {
        int kp = -1;
        if (k == SDLK_KP_0) kp = 0;
        else if (k >= SDLK_KP_1 && k <= SDLK_KP_9) kp = (int)(k - SDLK_KP_1) + 1;
        if (kp >= 0 && (ctrl || m_shuttle != 0.0)) {
            set_shuttle(kp, m_shuttle < 0.0);
            app.request_redraw();
            return true;
        }
        if (m_shuttle != 0.0 && (k == SDLK_KP_PLUS || k == SDLK_KP_MINUS)) {
            m_shuttle = (k == SDLK_KP_PLUS ? 1.0 : -1.0) * std::fabs(m_shuttle);
            app.request_redraw();
            return true;
        }
        if (m_shuttle != 0.0 && k == SDLK_SPACE) {
            m_shuttle = 0.0;
            app.request_redraw();
            return true;
        }
        // Custom Shuttle Lock Speed (50..800%), adjusted while shuttling with
        // the arrow keys (the preference dialog's Up/Down, ch.29 p665).
        if (m_shuttle != 0.0 && (k == SDLK_UP || k == SDLK_DOWN)) {
            m_shuttle_custom += (k == SDLK_UP ? 50 : -50);
            if (m_shuttle_custom < 50)  m_shuttle_custom = 50;
            if (m_shuttle_custom > 800) m_shuttle_custom = 800;
            if (std::fabs(m_shuttle) > 4.0 - 1e-9)   // key 9 rides the custom
                m_shuttle = (m_shuttle < 0 ? -1 : 1) * m_shuttle_custom / 100.0;
            app.request_redraw();
            return true;
        }
    }

    // ---- numpad +/- (ch.30 p675 + ch.31 p731-733).  With CLIPS selected:
    // bare = nudge the clips, Alt = trim their start by the Nudge value,
    // Ctrl = trim their end, Shift = slide the CONTENTS inside the fixed
    // boundaries (PT uses the Start key; this build has no Start chord).
    // With only a range selection: bare = nudge the range, Alt = start
    // point, Ctrl = end point -- exactly as before.
    if (k == SDLK_KP_PLUS || k == SDLK_KP_MINUS) {
        const long d = (k == SDLK_KP_PLUS ? 1 : -1) * nudge_ticks();
        if (m_fade_sel_seq >= 0) {         // ch.32: nudge the selected fade
            nudge_fade(d);
            app.request_redraw();
            return true;
        }
        if (any_selected_clip())
            nudge_clips(d, shift ? 3 : (alt ? 1 : (ctrl ? 2 : 0)));
        else
            nudge_selection(alt ? 1 : (ctrl ? 2 : 0), d);
        app.request_redraw();
        return true;
    }

    // ---- digit chords: Ctrl+1..5 zoom presets, Alt+7 Universe,
    // Alt+Shift+5/6 = Change Timeline/Edit to Match the other (ch.30 p684).
    if (k >= SDLK_1 && k <= SDLK_9) {
        const int d = (int)(k - SDLK_1) + 1;
        if (ctrl && alt && d <= 5) {       // Ctrl+Alt+1..5 = Rate Clips (p739)
            rate_selected(d);
            app.request_redraw();
            return true;
        }
        if (alt && shift && d == 3) {      // Alt+Shift+3 = Consolidate (p737)
            consolidate_selection();
            app.request_redraw();
            return true;
        }
        if (alt && shift && d == 7) {      // Alt+Shift+7 = Trim Start To Insertion
            trim_cmd(1);
            app.request_redraw();
            return true;
        }
        if (alt && shift && d == 8) {      // Alt+Shift+8 = Trim End To Insertion
            trim_cmd(2);
            app.request_redraw();
            return true;
        }
        if (ctrl && d <= 5) {
            if (m_zoom_preset[d - 1] > 0.0) {
                remember_zoom();
                m_scale_x = m_zoom_preset[d - 1];
                clamp_scroll();
            }
            app.request_redraw();
            return true;
        }
        if (alt && shift && d == 5) {          // Change Timeline to Match Edit
            if (m_sel_start >= 0 && m_sel_end > m_sel_start) {
                m_perf->set_left_tick(m_sel_start);
                m_perf->set_right_tick(m_sel_end);
            }
            app.request_redraw();
            return true;
        }
        if (alt && shift && d == 6) {          // Change Edit to Match Timeline
            const long a = m_perf->get_left_tick(), b = m_perf->get_right_tick();
            if (b > a)
                set_edit_selection(a, b, 0, (int)active_list().size() - 1);
            app.request_redraw();
            return true;
        }
        if (alt && !shift && d == 7) {         // Universe view (Alt+7)
            m_universe_on = !m_universe_on;
            app.request_redraw();
            return true;
        }
    }

    // ---- Ctrl+0 = Quantize to Grid (ch.31 p735)
    if ((k == SDLK_0 || k == SDLK_KP_0) && ctrl && m_shuttle == 0.0) {
        quantize_to_grid();
        app.request_redraw();
        return true;
    }

    // ---- Tab machinery (ch.30 p680-682): clip boundaries, or transients
    // when Tab-to-Transients is on.  Ctrl+Tab = previous (the PT Windows
    // binding); Shift extends the selection; Ctrl+Alt+Tab toggles the mode.
    if (k == SDLK_TAB) {
        if (ctrl && alt) {
            m_tab_transients = !m_tab_transients;
            app.request_redraw();
            return true;
        }
        const bool back = ctrl;
        const long from = m_sel_start < 0 ? playhead()
                        : (back ? m_sel_start : m_sel_end);
        const long t = tab_target(from, back);
        if (t >= 0) {
            if (shift && m_sel_start >= 0) {
                long a = m_sel_start, b = m_sel_end;
                if (back) a = t; else b = t;
                set_edit_selection(a, b, m_sel_lo, m_sel_hi);
            } else {
                set_edit_selection(t, t, m_sel_lo, m_sel_hi);
            }
        }
        app.request_redraw();
        return true;
    }

    if (k == SDLK_ESCAPE) {
        // menus are handled above; a fade selection, then a live selection,
        // is dropped first; a bare Esc then cycles the Edit tools (p643)
        if (m_fade_sel_seq >= 0) {
            m_fade_sel_seq = -1; m_fade_move = false;
            app.request_redraw();
            return true;
        }
        if (any_selected_clip() || m_sel_start >= 0) {
            unselect_all_triggers();
            m_sel_start = m_sel_end = -1;
            m_sel_lo = m_sel_hi = -1;
        } else {
            m_edit_tool = (EditTool)(((int)m_edit_tool + 1) % kToolCount);
        }
        app.request_redraw();
        return true;
    }
    switch (k) {
    // ---- ch.29 EDIT MODES: F1..F4, Shift combines Snap To Grid, the accent
    // key cycles.  (Help moved from F1 to "/" and the "?" button.)
    case SDLK_F1:
        set_edit_mode(EditMode::Shuffle);
        if (shift) m_snap_to_grid = true;
        break;
    case SDLK_F2:
        set_edit_mode(EditMode::Slip);
        if (shift) m_snap_to_grid = true;
        break;
    case SDLK_F3:
        set_edit_mode(EditMode::Spot);
        if (shift) m_snap_to_grid = true;
        break;
    case SDLK_F4:
        if (shift) m_snap_to_grid = !m_snap_to_grid;
        else       set_edit_mode(EditMode::Grid);
        break;
    case SDLK_BACKQUOTE: {           // ` cycles the Edit modes (p639)
        int m = ((int)m_edit_mode + 1) % 4;
        if (m == 0 && m_shuffle_lock) m = 1;   // locked Shuffle is skipped
        set_edit_mode((EditMode)m);
        break;
    }
    // ---- ch.29 TOOLS: F5..F10 select + cycle modes; F6+F7 / F7+F8 chords
    // pick the Smart tool (tracked via on_key_up).
    case SDLK_F5:
        m_fkey_down[0] = true;
        select_tool(EditTool::Zoom, true);
        break;
    case SDLK_F6:
        if (m_fkey_down[2]) m_edit_tool = EditTool::Smart;
        else select_tool(EditTool::Trim, true);
        m_fkey_down[1] = true;
        break;
    case SDLK_F7:
        if (m_fkey_down[1]) m_edit_tool = EditTool::Smart;
        else select_tool(EditTool::Range, true);
        m_fkey_down[2] = true;
        break;
    case SDLK_F8:
        if (m_fkey_down[2]) m_edit_tool = EditTool::Smart;
        else select_tool(EditTool::Grab, true);
        m_fkey_down[3] = true;
        break;
    case SDLK_F9:
        m_fkey_down[4] = true;
        select_tool(EditTool::Scrub, true);
        break;
    case SDLK_F10:
        m_fkey_down[5] = true;
        select_tool(EditTool::Draw, true);
        break;
    case SDLK_e:
        // E = Zoom Toggle; Alt+Shift+E cancels without reverting the view;
        // Ctrl+Shift+E = Duplicate and Extend Selection (ch.30 p677).
        if (ctrl && shift) {
            if (m_sel_start >= 0 && m_sel_end > m_sel_start) {
                std::vector<ClipCopy> saved = m_clip_clipboard;
                const long span0 = m_clip_span, tick0 = m_paste_tick;
                const long selA = m_sel_start, selB = m_sel_end;
                unselect_all_triggers();
                int lo, hi; sel_rows(lo, hi);
                std::vector<int> act = active_list();
                for (int r = std::max(0, lo); r <= hi && r < (int)act.size(); ++r)
                    for (int cs : lane_sequences(act[(size_t)r])) {
                        std::vector<ClipSpan> sp; clip_spans(cs, sp);
                        for (const ClipSpan& c : sp)
                            if (c.on >= selA && c.endEx <= selB) {
                                if (m_audio.count(cs)) region_for(cs).selected = true;
                                else if (sequence* q = m_perf->get_sequence(cs))
                                    q->select_trigger(c.on);
                            }
                    }
                copy_selected_clips();
                if (!m_clip_clipboard.empty()) {
                    m_paste_tick = -1;
                    paste_clips(selB);
                }
                m_clip_clipboard = saved; m_clip_span = span0; m_paste_tick = tick0;
                set_edit_selection(selA, selB + (selB - selA), m_sel_lo, m_sel_hi);
            }
            break;
        }
        if (ctrl) { separate_selection(0, app); break; }   // Ctrl+E (p726)
        if (alt && shift) { if (m_zt_on) zoom_toggle(true); break; }
        zoom_toggle(false);
        break;
    case SDLK_p:                       // move / extend / peel selection UP
        move_selection_lane(-1, shift, alt);
        break;
    case SDLK_SEMICOLON:               // ... and DOWN (ch.30 p679, p690)
        move_selection_lane(+1, shift, alt);
        break;
    case SDLK_SLASH:
        if (shift) { m_link_timeline = !m_link_timeline; break; }
        if (any_selected_clip()) { nudge_clips(next_larger_nudge(), 0); break; }
        m_help_open = true;
        break;
    case SDLK_m:       // nudge BACK by the next larger nudge value (p733)
        if (any_selected_clip()) nudge_clips(-next_larger_nudge(), 0);
        break;
    case SDLK_QUESTION:
        m_help_open = true;
        break;
    case '\'':   /* SDLK_QUOTE / SDLK_APOSTROPHE (SDL2/SDL3 spell it
                     differently; ASCII keycodes are the char value) */
                                       // Ctrl+Shift+' = Double Selection
        if (ctrl && shift && m_sel_start >= 0 && m_sel_end > m_sel_start)
            set_edit_selection(m_sel_start,
                               m_sel_start + 2 * (m_sel_end - m_sel_start),
                               m_sel_lo, m_sel_hi);
        break;
    case SDLK_END: {                 // playhead + view to the end of the song
        const long last = song_end();
        seek_to(last);
        const long span = (long)(canvas_w() * m_scale_x);
        m_scroll_ticks = std::max<long>(0, last + m_measure_len - span * 3 / 4);
        clamp_scroll();
        break; }
    case SDLK_PAGEUP:                // page the LANES, like every other editor
        m_v_offset = std::max(0, m_v_offset - visible_rows());
        break;
    case SDLK_PAGEDOWN:
        m_v_offset = std::min(max_v_offset(), m_v_offset + visible_rows());
        break;
    case SDLK_g:
        if (gui) { fade_to_end(app); break; }        // Start+G (p751)
        m_edit_tool=EditTool::Grab; break;
    case SDLK_r:
        if (ctrl && shift) { begin_clip_rename(app); break; }  // Ctrl+Shift+R
        if (ctrl) { capture_clip(app); break; }                // Ctrl+R (p725)
        m_edit_tool=EditTool::Range; break;
    case SDLK_c:
        if(ctrl)edit_copy();else m_edit_tool=EditTool::Cut;
        break;
    case SDLK_b:       // Ctrl+B = Clear (remove, clipboard untouched; p669)
        if (ctrl) edit_clear(false);
        break;
    case SDLK_h:       // Ctrl+H = Heal Separation (p728)
        if (ctrl) heal_separation();
        break;
    case SDLK_u:       // U toggles the Undo History window (ch.28 p665)
        if (!ctrl && !alt) m_undo_open = !m_undo_open;
        break;
    case SDLK_d:
        if (gui) { fade_to_start(app); break; }      // Start+D (p751)
        if(ctrl)duplicate_selected_clips();else m_edit_tool=EditTool::Draw;
        break;
    // Plain arrows pan the view; CTRL+arrow steps the PLAYHEAD by a bar, which
    // is how the edit point gets moved for a paste while the transport is
    // stopped.  Before this there was no keyboard playhead control at all.
    case SDLK_LEFT:
        // Alt+Start+Left = previous fade shape (ch.32 p756)
        if (alt && gui) { cycle_fade_shape(-1); break; }
        // Alt+Left = Move Edit Left by the selection amount (ch.30 p677)
        if (alt && m_sel_start >= 0 && m_sel_end > m_sel_start) {
            const long len = m_sel_end - m_sel_start;
            long a = m_sel_start - len; if (a < 0) a = 0;
            set_edit_selection(a, a + len, m_sel_lo, m_sel_hi);
            break;
        }
        if (ctrl) seek_to(std::max<long>(0, snap_down_bar(playhead()) - m_measure_len));
        else { m_scroll_ticks -= page; clamp_scroll(); }
        break;
    case SDLK_RIGHT:
        if (alt && gui) { cycle_fade_shape(+1); break; }   // next fade shape
        if (alt && m_sel_start >= 0 && m_sel_end > m_sel_start) {
            const long len = m_sel_end - m_sel_start;
            set_edit_selection(m_sel_start + len, m_sel_end + len,
                               m_sel_lo, m_sel_hi);
            break;
        }
        if (ctrl) seek_to(snap_down_bar(playhead()) + m_measure_len);
        else { m_scroll_ticks += page; clamp_scroll(); }
        break;
    case SDLK_UP:
        // track-height commands (ch.30 p691): Alt+Up grows the selected
        // lanes; Ctrl+Alt+Up fits them to the window.
        if (alt) {
            int lo, hi; sel_rows(lo, hi);
            std::vector<int> act = active_list();
            if (lo >= 0) {
                if (ctrl) {
                    const int nh = std::max(20, canvas_h() / std::max(1, hi - lo + 1));
                    for (int r = lo; r <= hi && r < (int)act.size(); ++r)
                        m_trackH[lane_key(act[(size_t)r])] = std::min(400, nh);
                } else {
                    for (int r = lo; r <= hi && r < (int)act.size(); ++r) {
                        int& h = m_trackH[lane_key(act[(size_t)r])];
                        if (h <= 0) h = row_h;
                        h = std::min(400, h + 12);
                    }
                }
            }
            break;
        }
        // during playback the Up arrow marks the selection END (ch.30 p690)
        if (m_perf->running() && m_sel_start >= 0) {
            set_edit_selection(m_sel_start, playhead(), m_sel_lo, m_sel_hi);
            break;
        }
        if (m_v_offset > 0) --m_v_offset;
        break;
    case SDLK_DOWN:
        if (alt) {
            int lo, hi; sel_rows(lo, hi);
            std::vector<int> act = active_list();
            for (int r = std::max(0, lo); r <= hi && r < (int)act.size(); ++r) {
                int& h = m_trackH[lane_key(act[(size_t)r])];
                if (h <= 0) h = row_h;
                h = std::max(20, h - 12);
            }
            break;
        }
        // during playback the Down arrow marks the selection START
        if (m_perf->running()) {
            const long ph = playhead();
            int lo, hi; sel_rows(lo, hi);
            set_edit_selection(ph, ph, lo, hi);
            break;
        }
        if (m_v_offset < max_v_offset()) ++m_v_offset;
        break;
    case SDLK_HOME:              // playhead + view to the start
        seek_to(0);
        m_scroll_ticks = 0; m_v_offset = 0;
        break;
    case SDLK_EQUALS:
    case SDLK_PLUS:
        if (shift && alt) {            // Shift+Alt+= : next LARGER nudge value
            m_nudge_idx = std::max(0, m_nudge_idx - 1);
            break;
        }
        if (shift) {                   // Shift+= : next LARGER grid value
            m_snap_idx = std::max(0, m_snap_idx - 1);
            m_snap = grid_ticks();
            break;
        }
        m_scale_x = std::max(kZoomMin, m_scale_x / 1.2); clamp_scroll(); break;
    case SDLK_MINUS:
        if (shift && alt) {            // Shift+Alt+- : next SMALLER nudge value
            m_nudge_idx = std::min(7, m_nudge_idx + 1);
            break;
        }
        if (shift) {                   // Shift+- : next SMALLER grid value
            m_snap_idx = std::min(7, m_snap_idx + 1);
            m_snap = grid_ticks();
            break;
        }
        m_scale_x = std::min(kZoomMax, m_scale_x * 1.2); clamp_scroll(); break;
    case SDLK_t:
        if (ctrl) { trim_cmd(0); break; }        // Ctrl+T = Trim To Selection
        if (shift) { m_link_track = !m_link_track; break; }   // Shift+T (ch.30)
        set_mode(mode() == Mode::Light ? Mode::Midnight : Mode::Light); break;
    case SDLK_v:
        if (!ctrl) return false;
        // Paste at the EDIT INSERTION POINT when there is one (ch.28 p668),
        // else at the playhead.
        edit_paste(m_sel_start >= 0 ? m_sel_start : edit_tick());
        break;
    case SDLK_y:     // Ctrl+Y = redo (through the Multiple-Undo queue)
        if (!ctrl) return false;
        do_redo();
        break;
    case SDLK_z:     // Ctrl+Z undo, Ctrl+Shift+Z redo, Ctrl+Alt+Z restore sel
        if (ctrl && alt) { restore_last_selection(); break; }
        if (!ctrl) { recall_prev_zoom(); break; }   // Z = previous zoom level
        if (shift) { do_redo(); break; }
        // ch.28 Multiple Undo: ONE queue covers trigger edits AND the
        // disk-cached audio-clip deletes (each entry carries its closures).
        do_undo();
        break;

    // ---- Qtractor-inspired bindings ----------------------------------------
    case SDLK_f:     // F fit song / shift+F fit sel / Ctrl+F Fades>Create
        if (ctrl && (gui || alt)) { create_fades_from_selection(app, true); break; }
        if      (ctrl)  create_fades_from_selection(app, false);   // ch.32 p749
        else if (shift) zoom_to_selection();
        else            zoom_to_fit();
        break;
    case SDLK_l:
        if (ctrl && shift) {           // Ctrl+Shift+L = Halve Selection
            if (m_sel_start >= 0 && m_sel_end > m_sel_start)
                set_edit_selection(m_sel_start,
                                   m_sel_start + (m_sel_end - m_sel_start) / 2,
                                   m_sel_lo, m_sel_hi);
            break;
        }
        m_follow = !m_follow; break;                           // L = follow
    case SDLK_a:     // ctrl+A select every clip; bare A scrolls to the start
        if (ctrl) select_all_clips();
        else { m_scroll_ticks = 0; m_v_offset = 0; }
        break;
    case SDLK_s:     // S = cycle snap  BAR..1/128..OFF
        m_snap_idx = (m_snap_idx + 1) % 9;
        m_snap = grid_ticks();
        break;
    case SDLK_x:     // ctrl+X cut; bare X splits at the playhead
        if (ctrl) { edit_cut(); break; }
        {   // Prefer the FOCUSED lane (the one highlighted as the edit target)
            // over whichever lane happened to be pressed last.
            int lane = -1;
            if (m_focus_lane >= 0)
                for (int sq : active_list())
                    if (lane_key(sq) == m_focus_lane) { lane = sq; break; }
            if (lane < 0 && m_drop_seq >= 0 && m_perf->is_active(m_drop_seq))
                lane = m_drop_seq;
            if (lane >= 0) split_clip_at(lane, playhead());
        }
        break;
    case SDLK_DELETE:
        // ch.32: a fade selected with the Grabber deletes as a FADE (p751)
        if (m_fade_sel_seq >= 0) { delete_fades_selection(); break; }
        // Ctrl+Delete clears ALL track data in the edit selection regardless
        // of Layered Editing (p734); a range selection clears the range; a
        // bare Delete removes the object selection.
        if (ctrl)              edit_clear(true);
        else if (have_range()) edit_clear(false);
        else                   edit_delete();
        break;
    default: return false;
    }
    app.request_redraw();
    return true;
}

// F-key chords (F6+F7 / F7+F8 = Smart tool) need to know which F-keys are
// still held; releases clear the ledger.
bool ArrangeView::on_key_up(App& app, SDL_Keycode k)
{
    (void)app;
    if (k >= SDLK_F5 && k <= SDLK_F10) {
        m_fkey_down[(int)(k - SDLK_F5)] = false;
        return true;
    }
    return false;
}

//----------------------------------------------------------------------------
//  solo (ports perfnames::apply_solo)
//----------------------------------------------------------------------------
void ArrangeView::apply_solo()
{
    bool any = false;
    for (int i = 0; i < c_max_sequence; ++i)
        if (m_perf->is_active(i) && m_solo[i]) { any = true; break; }

    if (any) {
        if (!m_solo_active) {
            m_solo_snapped.assign(c_max_sequence, 0);
            for (int i = 0; i < c_max_sequence; ++i)
                if (m_perf->is_active(i)) {
                    m_mute_snapshot[i] = m_perf->get_sequence(i)->get_song_mute();
                    m_solo_snapped[i] = 1;
                }
            m_solo_active = true;
        }
        // A track created WHILE solo is engaged was never snapshotted, so
        // un-soloing "restored" it to a mute state it never had.
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i) && !m_solo_snapped[i]) {
                m_mute_snapshot[i] = m_perf->get_sequence(i)->get_song_mute();
                m_solo_snapped[i] = 1;
            }
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i))
                m_perf->get_sequence(i)->set_song_mute(!m_solo[i]);
    } else if (m_solo_active) {
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i) && m_solo_snapped[i])
                m_perf->get_sequence(i)->set_song_mute(m_mute_snapshot[i] != 0);
        m_solo_active = false;
        m_solo_snapped.assign(c_max_sequence, 0);
    }
}

} // namespace arrange
