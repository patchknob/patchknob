//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_view.cpp  -- implementation.  See arrange_view.h.
//
//  Ports src/perfroll.cpp + src/perfnames.cpp + src/perftime.cpp onto the SDL2
//  toolkit.  All legacy pixmap drawing becomes ui:: draw helpers; the
//  colour roles map 1:1 (m_black->bg, m_panel->panel, m_grey->accent,
//  m_lt_grey/m_dk_grey->dim, m_white->hi, m_note->note).
//----------------------------------------------------------------------------
#include "arrange_view.h"
#include "perform.h"
#include "sequence.h"
#include "globals.h"
#include "engine/audioclip/audio_clip.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace ui;

namespace arrange {

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

// Filled disc (scanline circle) -- used for the ADD-TRACK "+" hover fill.
static void fill_disc(SDL_Renderer* r, int cx, int cy, int rad, Color c)
{
    if (rad < 1) return;
    set_color(r, c);
    for (int dy = -rad; dy <= rad; ++dy) {
        int q = rad * rad - dy * dy;
        if (q < 0) continue;
        int w = (int)floor(sqrt((double)q));
        SDL_Rect ln{ cx - w, cy + dy, 2 * w + 1, 1 };
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
}

//----------------------------------------------------------------------------
//  coordinate helpers
//----------------------------------------------------------------------------
std::vector<int> ArrangeView::active_list() const
{
    std::vector<int> v;
    if (!m_perf) return v;
    std::vector<int> keys;
    for (int i = 0; i < c_max_sequence; ++i) {
        if (!m_perf->is_active(i)) continue;
        int key = lane_key(i);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
        keys.push_back(key);
        v.push_back(i);
    }
    return v;
}

int ArrangeView::lane_key(int seq) const
{
    return on_track_key ? on_track_key(seq) : seq;
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

int ArrangeView::clip_sequence_at(int lane_seq, long tick) const
{
    std::vector<int> seqs = lane_sequences(lane_seq);
    for (int i = (int)seqs.size() - 1; i >= 0; --i) {
        sequence* s = m_perf->get_sequence(seqs[i]);
        if (s && s->get_trigger_state(tick))
            return seqs[i];
    }
    return -1;
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

long ArrangeView::snap(long tick) const
{
    if (m_snap <= 0) return tick;
    return tick - (tick % m_snap);
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
    for (int k = 0; k < r; ++k) {
        int idx = m_v_offset + k;
        y += (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
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

long ArrangeView::playhead() const
{
    return playhead_tick >= 0 ? playhead_tick : m_perf->get_tick();
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

// Fit the whole arrangement (0 .. last trigger end) to the canvas width.
void ArrangeView::zoom_to_fit()
{
    if (!m_perf) return;
    long last = m_perf->get_max_trigger();          // global max end over all seqs
    if (last < m_measure_len) last = m_measure_len;  // show at least one bar
    int cw = canvas_w();
    if (cw < 16) cw = 16;
    double sx = (double)last * 1.04 / (double)cw;    // +4% right margin
    if (sx < 2.0)    sx = 2.0;
    if (sx > 4096.0) sx = 4096.0;
    m_scale_x = sx;
    m_scroll_ticks = 0;
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
    int cw = app.mono.cw() ? app.mono.cw() : 6;
    int tx = box.x + 3;
    int ty = box.y + (box.h - app.mono.ch()) / 2;
    int maxc = (box.w - 8) / cw;
    std::string vis = m_edit_name;                    // scroll to keep caret in view
    if (maxc > 0 && (int)vis.size() > maxc) vis = vis.substr(vis.size() - maxc);
    app.mono.draw(app.ren, tx, ty, vis, t.text);
    int cx = tx + (int)vis.size() * cw;               // caret
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
//  draw
//----------------------------------------------------------------------------
void ArrangeView::draw(App& app)
{
    if (!visible) return;
    const Theme& t = theme();

    // Live pointer position (logical coords) for hover highlighting.  Plain mouse
    // motion is not delivered to the view without a button held, so poll it here;
    // the highlight then refreshes whenever the view is redrawn.
    { int gx = 0, gy = 0; SDL_GetMouseState(&gx, &gy);
      float sc = app.scale > 0.f ? app.scale : 1.0f;
      m_mx = (int)(gx / sc); m_my = (int)(gy / sc); }

    // No bound perform -> paint an empty themed frame rather than crash.
    if (!m_perf) {
        fill_rect(app.ren, rect, t.bg);
        frame_rect(app.ren, rect, t.dim);
        return;
    }

    std::vector<int> act = active_list();

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
    m_hover_loop = m_hover_extend = false;   // recomputed by draw_clips each frame
    draw_canvas (app, act);   // lanes + grid + clips + playhead
    draw_headers(app, act);   // left track-header column
    draw_ruler  (app, act);   // top time ruler + L/R markers
    apply_resize_cursor();    // SIZENS cursor over a lane's resize edge

    // top-left corner box (over the header/ruler junction)
    SDL_Rect corner{ rect.x, rect.y, header_w, ruler_h };
    fill_rect(app.ren, corner, t.panel);
    hline(app.ren, corner.x, corner.x + corner.w, corner.y + corner.h - 1, t.accent);
    vline(app.ren, corner.x + corner.w - 1, corner.y, corner.y + corner.h, t.accent);
    app.mono.draw(app.ren, corner.x + 6, corner.y + (ruler_h - app.mono.ch()) / 2,
                  "ARRANGE", t.text);

    frame_rect(app.ren, rect, t.dim);
    draw_rename(app);         // inline clip-name editor (over the canvas)
    draw_menu(app);
    draw_addmenu(app);        // add-track chooser (drawn on top of everything)
    draw_instrmenu(app);      // track-header instrument picker (topmost)
}

//----------------------------------------------------------------------------
//  right-click context menu (add / open / delete clips)
//----------------------------------------------------------------------------
namespace {
    // Rows 6/7 are Freeze/Freeze-Track; their labels flip to Unfreeze when the
    // clip's sequence is frozen (see menu_label()).
    const char* kMenuClip[]  = { "Open Piano", "Open Tracker",
                                 "Split", "Duplicate", "Rename", "Delete",
                                 "Freeze", "Freeze Track" };
    // AUDIO region context menu (Ardour region ops); row 6 flips Mute/Unmute.
    const char* kMenuAudio[] = { "Split", "Duplicate", "Rename", "Delete",
                                 "Normalize", "Reverse", "Mute", "Unfreeze" };
    const char* kMenuEmpty[] = { "Add Piano Clip", "Add Tracker Clip" };
    const int   kMenuW = 150;
    const int   kMenuClipN = 8;
    const int   kMenuAudioN = 8;
}

bool ArrangeView::menu_is_audio() const
{
    return m_menu_on_clip && m_audio.count(m_menu_seq) != 0;
}

const char* ArrangeView::menu_label(int idx) const
{
    if (menu_is_audio()) {
        if (idx == 6) {
            std::map<int, AudioRegion>::const_iterator it = m_region.find(m_menu_seq);
            return (it != m_region.end() && it->second.muted) ? "Unmute" : "Mute";
        }
        return kMenuAudio[idx];
    }
    if (idx == 6) return is_frozen(m_menu_seq) ? "Unfreeze" : "Freeze";
    if (idx == 7) return is_frozen(m_menu_seq) ? "Unfreeze Track" : "Freeze Track";
    return kMenuClip[idx];
}

void ArrangeView::draw_menu(App& app)
{
    if (!m_menu_open) return;
    const Theme& t = theme();
    const int n = m_menu_on_clip ? (menu_is_audio() ? kMenuAudioN : kMenuClipN) : 2;
    const int rowh = app.font.ch() + 8;
    SDL_Rect box = clamp_popup_rect(SDL_Rect{ m_menu_x, m_menu_y, kMenuW, n * rowh + 2 }, rect);
    m_menu_x = box.x; m_menu_y = box.y;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.dim);
    for (int i = 0; i < n; ++i) {
        const char* label = m_menu_on_clip ? menu_label(i) : kMenuEmpty[i];
        if (box.y + i * rowh + rowh > rect.y + rect.h) break;
        app.font.draw(app.ren, box.x + 8, box.y + i * rowh + 5,
                      fit_text(app.font, label, box.w - 16), t.text);
    }
}

bool ArrangeView::menu_click(App& app, int mx, int my)
{
    const int n = m_menu_on_clip ? (menu_is_audio() ? kMenuAudioN : kMenuClipN) : 2;
    const int rowh = app.font.ch() + 8;
    bool inside = (mx >= m_menu_x && mx < m_menu_x + kMenuW &&
                   my >= m_menu_y && my < m_menu_y + n * rowh);
    if (inside) {
        int idx = (my - m_menu_y) / rowh;
        sequence* s = m_perf->is_active(m_menu_seq) ? m_perf->get_sequence(m_menu_seq) : nullptr;
        if (s) {
            if (menu_is_audio()) {
                // AUDIO region ops (kMenuAudio order).
                if      (idx == 0) { m_perf->push_trigger_undo(); split_audio_clip(m_menu_seq, m_menu_tick); }
                else if (idx == 1) {                 // Duplicate after
                    s->select_trigger(m_menu_tick);
                    long st = s->get_selected_trigger_start_tick();
                    long en = s->get_selected_trigger_end_tick();
                    create_pattern(m_menu_seq, en + 1, en - st + 1, 0, false);
                }
                else if (idx == 2) {                 // Rename (inline)
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
                else if (idx == 3) {                                                     // Delete
                    if      (on_clip_delete) on_clip_delete(m_menu_seq);
                    else if (on_unfreeze)    on_unfreeze(m_menu_seq);
                }
                else if (idx == 4) { if (on_clip_normalize) on_clip_normalize(m_menu_seq); }
                else if (idx == 5) { if (on_clip_reverse)   on_clip_reverse(m_menu_seq); }
                else if (idx == 6) {                 // Mute / Unmute (toggle)
                    AudioRegion& r = region_for(m_menu_seq);
                    r.muted = !r.muted;
                    if (on_clip_mute) on_clip_mute(m_menu_seq, r.muted);
                }
                else if (idx == 7) { if (on_unfreeze) on_unfreeze(m_menu_seq); }        // Unfreeze
                m_menu_open = false;
                app.request_redraw();
                return true;
            }
            if (m_menu_on_clip) {
                if      (idx == 0 && on_open_editor) on_open_editor(m_menu_seq, 0);
                else if (idx == 1 && on_open_editor) on_open_editor(m_menu_seq, 1);
                else if (idx == 2) {                 // Split at the click tick
                    if (m_audio.count(m_menu_seq)) {
                        m_perf->push_trigger_undo();
                        split_audio_clip(m_menu_seq, m_menu_tick);   // Ardour region split
                    } else if (s->get_trigger_state(m_menu_tick)) {
                        m_perf->push_trigger_undo();
                        s->split_trigger(m_menu_tick);
                    }
                }
                else if (idx == 3) {                 // Duplicate (paste a copy after)
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
                else if (idx == 5) { m_perf->push_trigger_undo(); s->del_trigger(m_menu_tick); }
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
            } else {
                long len = s->get_length(); if (len < 1) len = c_ppqn * 4;
                long tt = m_menu_tick - (m_menu_tick % len);
                int new_seq = create_pattern(m_menu_seq, tt, len, 0, false);
                if (on_open_editor && new_seq >= 0) on_open_editor(new_seq, idx == 1 ? 1 : 0);
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
    const char* kAddMenu[] = { "Instrument Track", "Audio Track" };
    const int   kAddMenuN  = 2;
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
    const int spine_w = 4;
    const int name_x  = spine_w + 6 + 22 + 6;     // == draw_headers name_x
    const int rm_w = 14, btn_w = 22, vu_w = 10;
    const int rm_x  = header_w - rm_w - 4;
    const int btn_x = rm_x - btn_w - 4;
    const int vu_x  = btn_x - vu_w - 6;            // VU strip left edge
    int bx = rect.x + name_x;
    int bw = (rect.x + vu_x - 6) - bx;             // stop short of the VU strip
    if (bw < 48) bw = 48;
    int by = row_y + 5 + ch + 1;                   // line-2 band
    int bh = ch + 4;
    return SDL_Rect{ bx, by, bw, bh };
}

void ArrangeView::draw_instrmenu(App& app)
{
    if (!m_instrmenu_open) return;
    const Theme& t = theme();
    const int rowh = app.font.ch() + 8;
    SDL_Rect box = clamp_popup_rect(m_instrmenu_rect, rect);
    m_instrmenu_rect = box;
    fill_rect(app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    const int n = (int)m_instrmenu_items.size();
    for (int i = 0; i < n; ++i) {
        bool hot = (m_mx >= box.x && m_mx < box.x + box.w &&
                    m_my >= box.y + i * rowh && m_my < box.y + (i + 1) * rowh);
        if (hot) fill_rect(app.ren, SDL_Rect{ box.x + 1, box.y + i * rowh + 1,
                                              box.w - 2, rowh - 1 }, t.accent);
        app.font.draw(app.ren, box.x + 8, box.y + i * rowh + 5,
                      fit_text(app.font, m_instrmenu_items[(size_t)i], box.w - 16),
                      hot ? t.bg : t.text);
    }
}

bool ArrangeView::instrmenu_click(App& app, int mx, int my)
{
    const int rowh = app.font.ch() + 8;
    SDL_Rect b = m_instrmenu_rect;
    const int n = (int)m_instrmenu_items.size();
    bool inside = (mx >= b.x && mx < b.x + b.w &&
                   my >= b.y && my < b.y + n * rowh);
    if (inside) {
        int idx = (my - b.y) / rowh;
        if (idx >= 0 && idx < n && on_pick_instrument)
            on_pick_instrument(m_instrmenu_seq, idx);
    }
    m_instrmenu_open = false;                      // click (in or out) closes it
    m_instrmenu_seq  = -1;
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

    // lane stripes + bottom separators (variable per-track heights, cumulative)
    for (int r = 0, y = canvas_y(); y < canvas_y() + canvas_h(); ++r) {
        int idx = m_v_offset + r;
        int lh  = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        Color lane = (r % 2 == 0) ? t.panel : t.bg;
        fill_rect(app.ren, SDL_Rect{ cv.x, y, cv.w, lh }, lane);
        hline(app.ren, cv.x, cv.x + cv.w, y + lh - 1, t.dim);
        y += lh;
    }

    // shaded loop / edit-range band under the grid + clips
    draw_loop_band(app);

    // bar / beat grid (ports perfroll::draw_background_on grid section)
    int beat_px = (int)(m_beat_len / m_scale_x);
    bool draw_beats = beat_px >= 6;
    long first = m_scroll_ticks - (m_scroll_ticks % m_beat_len);
    for (long tick = first; ; tick += m_beat_len) {
        int x = tick_to_x(tick);
        if (x > cv.x + cv.w) break;
        if (x < cv.x) continue;
        bool measure = (tick % m_measure_len) == 0;
        if (!measure && !draw_beats) continue;
        vline(app.ren, x, cv.y, cv.y + cv.h, measure ? t.accent : t.dim);
    }

    // clip blocks per visible active track (cumulative y)
    for (int r = 0, y = canvas_y(); y < canvas_y() + canvas_h(); ++r) {
        int idx = m_v_offset + r;
        int lh  = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;
        if (idx >= 0 && idx < (int)act.size())
            for (int clip_seq : lane_sequences(act[(size_t)idx]))
                draw_clips(app, clip_seq, y);
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
                if (gr > gx)
                    frame_rect(app.ren, SDL_Rect{ gx, y + 3, gr - gx, lh - 6 }, t.hi);
            }
        }
    }

    // playhead (ports perfroll::draw_progress)
    int px = tick_to_x(playhead());
    if (px >= cv.x && px <= cv.x + cv.w)
        vline(app.ren, px, cv.y, cv.y + cv.h, t.hi);

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
    }
}

//----------------------------------------------------------------------------
//  one track's clip blocks (ports perfroll::draw_sequence_on)
//----------------------------------------------------------------------------
void ArrangeView::draw_clips(App& app, int seq, int lane_y)
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
    while (s->get_next_trigger(&tick_on, &tick_off, &selected, &offset)) {
        if (tick_off <= 0) continue;

        int x_on  = tick_to_x(tick_on);
        int x_off = tick_to_x(tick_off);
        if (x_off < cvx || x_on > cvr) continue;          // off-screen

        int y = lane_y + 3;
        int h = track_h(seq) - 6;
        int full_x = x_on;

        // clamp the drawn body to the canvas
        int bx = x_on  < cvx ? cvx : x_on;
        int br = x_off > cvr ? cvr : x_off;
        int bw = br - bx + 1;
        if (bw < 2) bw = 2;

        // body: a bright, auto-assigned clip colour; the data inside is drawn
        // SOLID BLACK for contrast.  Selection adds a white outline below.
        const Color body  = clip_color(seq);
        const Color black = Color{ 0, 0, 0, 255 };
        fill_round(app.ren, SDL_Rect{ bx, y, bw, h }, 4, body);

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
                             ? lit->second : (tick_off - tick_on);
            if (fullTicks < 1) fullTicks = 1;
            // Region source-offset (Ardour START): the first source tick the
            // block shows.  Tracked in m_region, NOT the trigger's wrapping
            // offset.  The visible px window maps into [srcTick, srcTick+visLen].
            const long srcTick = region_for(seq).source;
            const int full_w = (x_off - x_on) < 1 ? 1 : (x_off - x_on);
            const long visLen = tick_off - tick_on;
            const double fracL = (double)(bx - x_on) / (double)full_w;   // 0..1 of visible
            const double fracR = (double)(br - x_on) / (double)full_w;
            const double tkL = (double)srcTick + fracL * (double)visLen;  // ticks into source
            const double tkR = (double)srcTick + fracR * (double)visLen;
            long long s0 = (long long)(tkL / (double)fullTicks * (double)nfr);
            long long s1 = (long long)(tkR / (double)fullTicks * (double)nfr);
            if (s0 < 0) s0 = 0; if (s1 > nfr) s1 = nfr; if (s1 <= s0) s1 = s0 + 1;
            draw_waveform(app, clip, bx, y, bw, h, s0, s1);
            draw_clip_fades(app, seq, SDL_Rect{ bx, y, bw, h });
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
            if (anm && bw > 12) {
                int maxc = (bw-4) / (app.mono.cw()?app.mono.cw():6);
                if (maxc > 0) { char lbl[48]; std::snprintf(lbl,sizeof(lbl),"%s",anm);
                    if ((int)std::strlen(lbl) > maxc) lbl[maxc]=0;
                    app.mono.draw(app.ren, bx+2, y+1, lbl, black); }
            }
            // LOOP handle (top-right corner): a loop glyph; filled when looping.
            if (bw > 20) {
                const bool looping = m_region.count(seq) && m_region[seq].loop;
                SDL_Rect lb{ br - 14, y + 1, 12, 12 };
                if (m_mx >= lb.x && m_mx < lb.x + lb.w && m_my >= lb.y && m_my < lb.y + lb.h)
                    m_hover_loop = true;
                fill_rect(app.ren, lb, looping ? t.hi : body);
                frame_rect(app.ren, lb, black);
                // two opposed arcs (a loop icon) drawn as short strokes
                set_color(app.ren, black);
                SDL_RenderDrawLine(app.ren, lb.x+3, lb.y+3, lb.x+9, lb.y+3);
                SDL_RenderDrawLine(app.ren, lb.x+9, lb.y+3, lb.x+9, lb.y+6);
                SDL_RenderDrawLine(app.ren, lb.x+9, lb.y+9, lb.x+3, lb.y+9);
                SDL_RenderDrawLine(app.ren, lb.x+3, lb.y+9, lb.x+3, lb.y+6);
            }
            // EXTEND handle (bottom edge): grip dots -> drag to lengthen the clip.
            {
                const bool ehot = (m_mx >= bx && m_mx < br && m_my >= y + h - 4 && m_my <= y + h);
                if (ehot) m_hover_extend = true;
                for (int gx = bx + bw/2 - 8; gx <= bx + bw/2 + 8; gx += 4)
                    fill_rect(app.ren, SDL_Rect{ gx, y + h - 3, 2, 2 }, ehot ? t.hi : black);
            }
            continue;   // skip the MIDI note-preview path for audio clips
        }

        // ---- tiny note preview tiled across the clip (perfroll port) -------
        int lowest  = s->get_lowest_note_event();
        int highest = s->get_highest_note_event();
        if (highest >= lowest && length_w > 1) {
            int height = highest - lowest + 2;
            long first_marker =
                tick_on - (tick_on % seq_len) + (offset % seq_len) - seq_len;
            set_color(app.ren, black);            // notes: solid black on the colour
            for (long marker = first_marker; marker < tick_off; marker += seq_len) {
                int marker_x = tick_to_x(marker);
                long tick_s, tick_f; int note, vel; bool nsel; draw_type dt;
                s->reset_draw_marker();
                while ((dt = s->get_next_note_event(&tick_s, &tick_f, &note,
                                                    &nsel, &vel)) != DRAW_FIN) {
                    int note_y = ((h - 8) - ((h - 8) * (note - lowest)) / height) + 4;
                    int ns_x = (int)(((long)tick_s * length_w) / seq_len) + marker_x;
                    int nf_x = (int)(((long)tick_f * length_w) / seq_len) + marker_x;
                    if (dt == DRAW_NOTE_ON || dt == DRAW_NOTE_OFF) nf_x = ns_x + 1;
                    if (nf_x <= ns_x) nf_x = ns_x + 1;
                    if (ns_x < bx) ns_x = bx;
                    if (nf_x > bx + bw) nf_x = bx + bw;
                    if (nf_x >= bx && ns_x <= bx + bw)
                        SDL_RenderDrawLine(app.ren, ns_x, y + note_y, nf_x, y + note_y);
                }
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
        if (nm && bw > 12) {
            int maxc = (bw - 4) / (app.mono.cw() ? app.mono.cw() : 6);
            if (maxc > 0) {
                char lbl[48];
                std::snprintf(lbl, sizeof(lbl), "%s", nm);
                if ((int)std::strlen(lbl) > maxc) lbl[maxc] = 0;
                app.mono.draw(app.ren, bx + 2, y + 1, lbl, black);
            }
        }
    }
}

void ArrangeView::set_audio_clip(int seq, const PatchKnob::engine::AudioClip* clip, long fullTicks)
{
    if (clip) {
        m_audio[seq] = clip;
        if (fullTicks > 0) m_audioLen[seq] = fullTicks;
        m_region.erase(seq);          // re-init lazily from the fresh trigger
    } else {
        m_audio.erase(seq);
        m_audioLen.erase(seq);
        m_region.erase(seq);
        m_clipFade.erase(seq);         // matched to the cleared audio (was leaking)
    }
}

// Purge every per-sequence entry for `seq`.  Called on delete / track removal so
// the recycled index cannot resurrect a previous clip's waveform pointer (which
// may point at a freed freeze buffer -> crash), region, fade, colour, or frozen
// tint.  The lane HEIGHT is shared by every sequence on the lane, so it is only
// dropped when `seq` was the last sequence on that lane.
void ArrangeView::forget_seq(int seq)
{
    const int key = lane_key(seq);     // resolve BEFORE the caller drops routing
    m_audio.erase(seq);
    m_audioLen.erase(seq);
    m_region.erase(seq);
    m_clipFade.erase(seq);
    m_clipColor.erase(seq);
    m_frozen.erase(seq);
    bool laneStillUsed = false;
    if (m_perf)
        for (int s = 0; s < c_max_sequence; ++s)
            if (s != seq && m_perf->is_active(s) && lane_key(s) == key) { laneStillUsed = true; break; }
    if (!laneStillUsed) m_trackH.erase(key);
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

    // Replace the source's notes with the rearranged set (triggers are untouched).
    src->select_all();
    src->mark_selected();
    src->remove_marked();
    for (size_t i = 0; i < out.size(); ++i) {
        const N& n = out[i];
        if (n.start < 0) continue;
        src->add_event(n.start,         0x90, (unsigned char)n.note, (unsigned char)n.vel);
        src->add_event(n.start + n.len, 0x80, (unsigned char)n.note, 0);
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
void ArrangeView::commit_region(int seq, bool leftTrim, bool rightTrim)
{
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return;
    const long newPos = s->get_selected_trigger_start_tick();
    const long newEnd = s->get_selected_trigger_end_tick();
    const long newLen = newEnd - newPos + 1;
    if (newLen <= 0) return;

    AudioRegion& r = region_for(seq);
    if (leftTrim) {
        const long delta = newPos - r.position;        // signed edge shift
        r.source += delta;                             // trim_front: start += delta
    }
    (void)rightTrim;                                   // right-trim leaves source fixed
    r.position = newPos;
    r.length   = newLen;

    // clamp source to the available source (Ardour verify_start_and_length).
    // A LOOPED region is allowed to exceed the source length (it wraps to fill),
    // so the right-edge clamp is skipped when r.loop.
    std::map<int,long>::const_iterator lit = m_audioLen.find(seq);
    const long srcLen = (lit != m_audioLen.end() && lit->second > 0) ? lit->second : r.length;
    if (r.source < 0) r.source = 0;
    if (r.source > srcLen - 1) r.source = srcLen - 1;
    if (!r.loop && r.source + r.length > srcLen) r.length = srcLen - r.source;   // clamp to source

    if (on_clip_region_changed) on_clip_region_changed(seq, r.position, r.length, r.source);
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
    const int newSeq = create_pattern(seq, splitTick, L - B, 0, false);
    if (newSeq >= 0) {
        AudioRegion nr; nr.position = splitTick; nr.source = S + B; nr.length = L - B;
        m_region[newSeq] = nr;
        if (on_clip_region_changed) on_clip_region_changed(newSeq, nr.position, nr.length, nr.source);
        // Split the fades between halves (create_pattern copied the whole fade):
        // the LEFT keeps its fade-IN, the RIGHT keeps the fade-OUT; the cut edge
        // gets no fade on either side.
        std::map<int,ClipFade>::iterator lf = m_clipFade.find(seq);
        if (lf != m_clipFade.end()) {
            ClipFade rf; rf.outTicks = lf->second.outTicks; rf.outK = lf->second.outK;
            if (rf.outTicks > 0) { m_clipFade[newSeq] = rf; commit_fade(newSeq); }
            else                   m_clipFade.erase(newSeq);
            lf->second.outTicks = 0; lf->second.outK = 0.f;    // left drops its out-fade
            commit_fade(seq);
        }
    }

    // LEFT half: trim the original's right edge back to the split point.
    s->select_trigger(P);
    s->move_selected_triggers_to(splitTick - 1, false, 1);
    r.position = P; r.source = S; r.length = B;
    m_region[seq] = r;
    if (on_clip_region_changed) on_clip_region_changed(seq, P, B, S);
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
            AudioRegion r; r.position = start; r.source = copySource; r.length = length;
            m_region[newSeq] = r;               // survives the shell's set_audio_clip erase
        }
        return newSeq;
    }

    sequence* s = m_perf->get_sequence(source_seq);
    if (!s) return -1;
    m_perf->push_trigger_undo();
    s->add_trigger(start, length, offset);
    return source_seq;
}

void ArrangeView::unselect_all_triggers()
{
    if (!m_perf) return;
    for (int seq = 0; seq < c_max_sequence; ++seq)
        if (m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq))
                s->unselect_triggers();
}

void ArrangeView::select_clips_in_rect(SDL_Rect box, bool add_to_selection)
{
    if (!m_perf) return;
    if (!add_to_selection) unselect_all_triggers();
    std::vector<int> act = active_list();
    for (int row = 0; row < (int)act.size(); ++row) {
        int screen_row = row - m_v_offset;
        int lane_y = row_top(screen_row);
        int lh = track_h(act[(size_t)row]);
        if (lane_y >= canvas_y() + canvas_h() || lane_y + lh < canvas_y()) continue;
        for (int seq : lane_sequences(act[row])) {
            sequence* s = m_perf->get_sequence(seq);
            if (!s) continue;
            s->reset_draw_trigger_marker();
            long on, off, offset; bool selected;
            while (s->get_next_trigger(&on, &off, &selected, &offset)) {
                SDL_Rect clip{ tick_to_x(on), lane_y + 3,
                               std::max(2, tick_to_x(off) - tick_to_x(on) + 1),
                               lh - 6 };
                if (rect_intersects(box, clip))
                    s->select_trigger(on);
            }
        }
    }
}

void ArrangeView::copy_selected_clips()
{
    m_clip_clipboard.clear();
    if (!m_perf) return;
    long min_start = -1;
    std::vector<ClipCopy> clips;
    for (int seq = 0; seq < c_max_sequence; ++seq) {
        if (!m_perf->is_active(seq)) continue;
        sequence* s = m_perf->get_sequence(seq);
        if (!s) continue;
        s->reset_draw_trigger_marker();
        long on, off, offset; bool selected;
        while (s->get_next_trigger(&on, &off, &selected, &offset)) {
            if (!selected) continue;
            ClipCopy c;
            c.seq = seq;
            c.rel_start = on;
            c.length = off - on + 1;
            c.offset = offset;
            clips.push_back(c);
            if (min_start < 0 || on < min_start) min_start = on;
        }
    }
    if (min_start < 0) return;
    for (auto& c : clips) c.rel_start -= min_start;
    m_clip_clipboard.swap(clips);
}

void ArrangeView::paste_clips(long start_tick)
{
    if (!m_perf || m_clip_clipboard.empty()) return;
    start_tick = snap(start_tick);
    if (start_tick < 0) start_tick = 0;
    unselect_all_triggers();
    for (const ClipCopy& clip : m_clip_clipboard) {
        int seq = create_pattern(clip.seq, start_tick + clip.rel_start,
                                 clip.length, clip.offset, true);
        if (seq >= 0 && m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq))
                s->select_trigger(start_tick + clip.rel_start);
    }
}

void ArrangeView::delete_selected_clips()
{
    if (!m_perf) return;
    m_perf->push_trigger_undo();
    // Collect audio seqs with a selected trigger first: deleting an AUDIO clip
    // must detach its freeze (stop + free the rendered buffer) instead of only
    // dropping the trigger, which would leak the AudioClip and keep it audible.
    std::vector<int> audioDel;
    for (int seq = 0; seq < c_max_sequence; ++seq)
        if (m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq)) {
                const bool sel = s->get_selected_trigger_start_tick() >= 0;
                if (sel && m_audio.count(seq)) audioDel.push_back(seq);
                else                           s->del_selected_trigger();
            }
    // Route audio deletes through on_clip_delete (disk-caches for undo, then
    // tears down); fall back to a bare unfreeze.  Run after the sweep because it
    // may delete sequences / renumber.
    for (int seq : audioDel) {
        if      (on_clip_delete) on_clip_delete(seq);
        else if (on_unfreeze)    on_unfreeze(seq);
        forget_seq(seq);
    }
}

bool ArrangeView::any_selected_clip() const
{
    if (!m_perf) return false;
    for (int seq = 0; seq < c_max_sequence; ++seq) {
        if (!m_perf->is_active(seq)) continue;
        sequence* s = m_perf->get_sequence(seq);
        if (!s) continue;
        s->reset_draw_trigger_marker();
        long on, off, offset; bool selected;
        while (s->get_next_trigger(&on, &off, &selected, &offset))
            if (selected) return true;
    }
    return false;
}

//----------------------------------------------------------------------------
//  peak-based audio waveform inside a clip body (min/max per pixel column)
//----------------------------------------------------------------------------
void ArrangeView::draw_waveform(App& app, const PatchKnob::engine::AudioClip* clip,
                                int bx, int y, int bw, int h,
                                long long s0, long long s1)
{
    const Theme& t = theme();
    const long long n = (long long)clip->numFrames();
    if (n <= 0 || bw < 2 || h < 4) return;
    // Default (s1<0) = whole clip; else draw only the [s0,s1) sample window so a
    // trimmed trigger shows just its visible slice.
    if (s1 < 0) { s0 = 0; s1 = n; }
    if (s0 < 0) s0 = 0; if (s1 > n) s1 = n; if (s1 <= s0) s1 = s0 + 1;
    const long long span = s1 - s0;
    const float* L = clip->ch[0].data();
    const float* R = clip->ch[1].empty() ? clip->ch[0].data() : clip->ch[1].data();

    // Ardour WaveView geometry (compute_tips, Normal shape): an even effective
    // height, amplitude mapped by y = (1 - a) * half about the centre line.
    const int H     = 2 * (int)std::floor((h - 1) * 0.5);   // even drawing height
    const int half  = H / 2;                                // == floor((h-1)/2): centre & scale
    const int zeroY = y + half;                             // zero / centre line

    // Data is SOLID BLACK on the bright clip body.  Faint centre line.
    const Color waveColor{ 0, 0, 0, 255 };
    hline(app.ren, bx, bx + bw - 1, zeroY, waveColor);

    // Per-pixel min/max reduction over the region's source window (Ardour scans
    // source samples [s0 + x*spp, s0 + (x+1)*spp); min/max init to +1/-1).
    // Batched into one FillRects call.
    static std::vector<SDL_Rect> cols;   // reused across frames to avoid realloc
    cols.clear(); cols.reserve((size_t)bw);
    for (int px = 0; px < bw; ++px) {
        long long a0 = s0 + (long long)((double)px       / bw * span);
        long long a1 = s0 + (long long)((double)(px + 1) / bw * span);
        if (a1 <= a0) a1 = a0 + 1;
        if (a1 > n)   a1 = n;
        float mn = 1.f, mx = -1.f;                     // Ardour init (max=-1, min=1)
        for (long long i = a0; i < a1; ++i) {
            const float l = L[i], r = R[i];
            if (l < mn) mn = l; if (l > mx) mx = l;
            if (r < mn) mn = r; if (r > mx) mx = r;
        }
        // compute_tips: pmax/pmin are pixel offsets from the clip top; round
        // AWAY from zero when the column straddles the axis, else to nearest;
        // collapse to a centre dot on silence (top > bot).
        const double pmax = (1.0 - (double)mx) * (double)half;   // y of max (positive)
        const double pmin = (1.0 - (double)mn) * (double)half;   // y of min (negative)
        int top, bot;
        if (pmax * pmin < 0.0) { top = (int)std::ceil(pmax);  bot = (int)std::floor(pmin); }
        else                   { top = (int)std::lround(pmax); bot = (int)std::lround(pmin); }
        if (top > bot) { top = bot = (int)std::lround(0.5 * (top + bot)); }
        int ay0 = y + top, ay1 = y + bot;
        if (ay0 < y)         ay0 = y;
        if (ay1 > y + h - 1) ay1 = y + h - 1;
        cols.push_back(SDL_Rect{ bx + px, ay0, 1, (ay1 - ay0) + 1 });
    }
    set_color(app.ren, waveColor);
    if (!cols.empty()) SDL_RenderFillRects(app.ren, cols.data(), (int)cols.size());
}

//----------------------------------------------------------------------------
//  clip crossfades: draggable fade-in/out with a bendable curve point
//----------------------------------------------------------------------------
void ArrangeView::set_clip_fade(int seq, long inTicks, long outTicks, float inK, float outK)
{
    ClipFade f;
    f.inTicks  = inTicks  < 0 ? 0 : inTicks;
    f.outTicks = outTicks < 0 ? 0 : outTicks;
    f.inK  = inK  < -1.f ? -1.f : (inK  > 1.f ? 1.f : inK);
    f.outK = outK < -1.f ? -1.f : (outK > 1.f ? 1.f : outK);
    m_clipFade[seq] = f;
}

// Same 0..1 tension ramp as the engine (ScheduledClip::fadeShape).
static float fade_shape(float u, float k)
{
    if (u <= 0.f) return 0.f;
    if (u >= 1.f) return 1.f;
    if (k > -1e-4f && k < 1e-4f) return u;
    const float a = k * 3.f;
    return (std::exp(a * u) - 1.f) / (std::exp(a) - 1.f);
}

void ArrangeView::draw_clip_fades(App& app, int seq, const SDL_Rect& clip)
{
    const Theme& t = theme();
    std::map<int, ClipFade>::const_iterator it = m_clipFade.find(seq);
    ClipFade f = (it != m_clipFade.end()) ? it->second : ClipFade{};

    const int inW  = std::min(clip.w / 2, std::max(0, (int)(f.inTicks  / m_scale_x)));
    const int outW = std::min(clip.w / 2, std::max(0, (int)(f.outTicks / m_scale_x)));
    const int top = clip.y, bot = clip.y + clip.h;

    auto draw_fade = [&](int x0, int w, float k, bool rising) {
        if (w < 2) return;
        // gain(u): rising 0->1 for fade-in; for fade-out we mirror u.
        SDL_Point pts[33];
        const int N = 32;
        for (int i = 0; i <= N; ++i) {
            const float u = (float)i / N;
            const float g = fade_shape(rising ? u : (1.f - u), k);
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
        const float gm = fade_shape(0.5f, k);
        const int cx = x0 + w / 2;
        const int cy = bot - (int)(gm * clip.h);
        SDL_Rect cp{ cx - 3, cy - 3, 6, 6 };
        fill_rect(app.ren, cp, t.notesel);
        frame_rect(app.ren, cp, t.accent);
    };
    draw_fade(clip.x, inW, f.inK, true);
    draw_fade(clip.x + clip.w - outW, outW, f.outK, false);
}

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
        const int cx = clip.x + inW / 2, cy = bot - (int)(fade_shape(0.5f, f.inK) * clip.h);
        if (near(mx, my, cx, cy, 6)) return FadeGrab::InCurve;
    }
    if (outW >= 8) {
        const int cx = clip.x + clip.w - outW + outW / 2, cy = bot - (int)(fade_shape(0.5f, f.outK) * clip.h);
        if (near(mx, my, cx, cy, 6)) return FadeGrab::OutCurve;
    }
    // length handles at the top corners (also grabbable at the very edge with
    // zero current length, so a fade can be pulled out from nothing)
    if (near(mx, my, clip.x + inW, top, 7)) return FadeGrab::InLen;
    if (near(mx, my, clip.x + clip.w - outW, top, 7)) return FadeGrab::OutLen;
    return FadeGrab::None;
}

void ArrangeView::commit_fade(int seq)
{
    if (!on_clip_fade) return;
    ClipFade f = m_clipFade.count(seq) ? m_clipFade[seq] : ClipFade{};
    on_clip_fade(seq, f.inTicks, f.outTicks, f.inK, f.outK);
}

// Locate an audio clip's on-screen body rect (first trigger) for fade hit-test.
bool ArrangeView::clip_rect_of(int seq, SDL_Rect& out) const
{
    if (!m_perf->is_active(seq)) return false;
    sequence* s = m_perf->get_sequence(seq);
    if (!s) return false;
    std::vector<int> act = active_list();
    int lane_y = -1;
    for (size_t i = 0; i < act.size(); ++i) {
        for (int cs : lane_sequences(act[i]))
            if (cs == seq) { lane_y = row_top((int)i - m_v_offset); break; }
        if (lane_y >= 0) break;
    }
    if (lane_y < 0) return false;
    s->reset_draw_trigger_marker();
    long on, off, offset; bool sel;
    if (!s->get_next_trigger(&on, &off, &sel, &offset) || off <= 0) return false;
    int cvx = canvas_x(), cvr = canvas_x() + canvas_w();
    int x_on = tick_to_x(on), x_off = tick_to_x(off);
    int bx = x_on < cvx ? cvx : x_on;
    int br = x_off > cvr ? cvr : x_off;
    out = SDL_Rect{ bx, lane_y + 3, std::max(2, br - bx + 1), track_h(seq) - 6 };
    return true;
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

void ArrangeView::draw_add_button(App& app)
{
    const Theme& t = theme();
    SDL_Rect ar = add_row_rect();

    // opaque cell -- when pinned over a lane this cleanly covers what's beneath.
    fill_rect(app.ren, ar, t.panel);
    hline(app.ren, ar.x, ar.x + ar.w, ar.y,               t.dim);
    hline(app.ren, ar.x, ar.x + ar.w, ar.y + ar.h - 1,    t.dim);

    bool hot = (m_mx >= ar.x && m_mx < ar.x + ar.w &&
                m_my >= ar.y && m_my < ar.y + ar.h);

    int rad = row_h / 2 - 6;
    if (rad < 6)  rad = 6;
    if (rad > 13) rad = 13;
    int cx = ar.x + 8 + rad;
    int cy = ar.y + row_h / 2;

    if (hot) fill_disc (app.ren, cx, cy, rad, t.accent);            // hover fill
    stroke_ring(app.ren, cx, cy, rad, 2, hot ? t.hi : t.accent);   // scanline ring

    // "+" : two short line-drawn strokes (2 px thick), NO font glyph.
    Color pc = hot ? t.bg : t.accent;
    int arm = rad - 3; if (arm < 3) arm = 3;
    hline(app.ren, cx - arm, cx + arm, cy,     pc);
    hline(app.ren, cx - arm, cx + arm, cy + 1, pc);
    vline(app.ren, cx,     cy - arm, cy + arm, pc);
    vline(app.ren, cx + 1, cy - arm, cy + arm, pc);

    // ASCII label beside the ring (font glyphs are fine for text, not the icons).
    int tx = cx + rad + 8;
    int ty = ar.y + (row_h - app.mono.ch()) / 2;
    app.mono.draw(app.ren, tx, ty, "ADD TRACK", hot ? t.hi : t.text);
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
//  left track-header column (ports perfnames::draw_sequence)
//----------------------------------------------------------------------------
void ArrangeView::draw_headers(App& app, const std::vector<int>& act)
{
    const Theme& t = theme();
    const int spine_w = 4;
    const int badge_x = spine_w + 6;
    const int badge_w = 22;
    const int name_x  = badge_x + badge_w + 6;
    const int rm_w    = 14;                       // remove "x" box (top-right)
    const int rm_h    = 14;
    const int rm_x    = header_w - rm_w - 4;
    const int rm_y    = 4;
    const int btn_w   = 22;
    const int btn_h   = 12;
    const int btn_x   = rm_x - btn_w - 4;         // M/S left of the "x" column
    const int vu_w    = 10;
    const int vu_x    = btn_x - vu_w - 6;

    for (int r = 0, y0 = canvas_y(); y0 < canvas_y() + canvas_h(); ++r) {
        int idx = m_v_offset + r;
        const int lh = (idx >= 0 && idx < (int)act.size()) ? track_h(act[(size_t)idx]) : row_h;

        SDL_Rect hdr{ rect.x, y0, header_w, lh };
        Color bg = (r % 2 == 0) ? t.panel : t.bg;
        fill_rect(app.ren, hdr, bg);
        hline(app.ren, hdr.x, hdr.x + hdr.w, y0 + lh - 1, t.dim);

        if (idx >= 0 && idx < (int)act.size()) {
        int seq = act[idx];
        sequence* s = m_perf->get_sequence(seq);
        if (s) {
        bool muted = s->get_song_mute();

        // status spine
        fill_rect(app.ren, SDL_Rect{ hdr.x, y0 + 1, spine_w, lh - 2 },
                  muted ? t.dim : t.hi);

        // type badge: AUD for audio (waveform) tracks, INS for instrument tracks
        const bool is_audio = m_audio.count(seq) != 0;
        SDL_Rect badge{ hdr.x + badge_x, y0 + 4, badge_w, std::max(8, std::min(lh - 8, 20)) };
        if (badge.y + badge.h < y0 + lh) {
            frame_rect(app.ren, badge, t.dim);
            app.mono.draw_centered(app.ren, badge, is_audio ? "AUD" : "INS", t.accent);
        }

        // name (line 1)
        std::string name = std::to_string(seq + 1);
        if (seq + 1 < 10) name = "0" + name;
        name += " ";
        name += s->get_name() ? s->get_name() : "";
        app.mono.draw(app.ren, hdr.x + name_x, y0 + 5,
                      fit_text(app.mono, name, std::max(1, vu_x - name_x - 8)), t.text);

        // line 2: INSTRUMENT dropdown (instrument tracks) or bus/ch (audio tracks)
        if (!is_audio && on_track_instrument) {
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

        // level / VU strip placeholder (pseudo level per track)
        int vu_y = y0 + 4, vu_h = lh - 8;
        frame_rect(app.ren, SDL_Rect{ hdr.x + vu_x, vu_y, vu_w, vu_h }, t.dim);
        if (!muted) {
            int level = 30 + (seq * 37) % 60;            // 30..90 %
            int filled = vu_h * level / 100;
            for (int yy = vu_y + vu_h - 2; yy > vu_y + vu_h - filled; yy -= 3) {
                Color seg = (yy < vu_y + vu_h / 3) ? t.hi : t.accent;
                hline(app.ren, hdr.x + vu_x + 2, hdr.x + vu_x + vu_w - 2, yy, seg);
            }
        }

        // Mute / Solo toggle buttons
        draw_button(app, SDL_Rect{ hdr.x + btn_x, y0 + 4, btn_w, btn_h }, "M", muted);
        draw_button(app, SDL_Rect{ hdr.x + btn_x, y0 + 4 + btn_h + 2, btn_w, btn_h },
                    "S", m_solo[seq] != 0);

        // remove "x" button (top-right corner of the header cell)
        SDL_Rect xbox{ hdr.x + rm_x, y0 + rm_y, rm_w, rm_h };
        bool xhot = (m_mx >= xbox.x && m_mx < xbox.x + xbox.w &&
                     m_my >= xbox.y && m_my < xbox.y + xbox.h);
        draw_remove_btn(app, xbox, xhot);

        // LANE-RESIZE grip: the bottom edge of the header (drag to resize).
        const bool rhot = (m_mx >= hdr.x && m_mx < hdr.x + header_w &&
                           m_my >= y0 + lh - 4 && m_my <= y0 + lh);
        if (rhot) m_hover_resize = true;
        Color grip = (rhot || (m_hdr_resize && m_hdr_resize_seq == seq)) ? t.hi : t.dim;
        for (int gx = hdr.x + header_w/2 - 8; gx <= hdr.x + header_w/2 + 8; gx += 4)
            fill_rect(app.ren, SDL_Rect{ gx, y0 + lh - 3, 2, 2 }, grip);
        }  // if (s)
        }  // if (idx valid)
        y0 += lh;
    }

    // ADD-TRACK "+" affordance, always drawn under the last visible header.
    draw_add_button(app);
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

void ArrangeView::apply_resize_cursor()
{
    static SDL_Cursor* s_sizens = nullptr;
    static SDL_Cursor* s_sizewe = nullptr;
    static SDL_Cursor* s_loop   = nullptr;
    static SDL_Cursor* s_arrow  = nullptr;
    static int         s_active = 0;   // 0 arrow, 1 sizens, 2 sizewe, 3 loop
    if (!s_arrow) {
        s_sizens = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
        s_sizewe = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
        s_arrow  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        s_loop   = make_loop_cursor();
        if (!s_loop) s_loop = s_arrow;
    }
    int want = 0;
    if (m_hover_loop)                              want = 3;   // loop corner
    else if (m_hover_extend || m_extending)        want = 2;   // extend edge (horiz)
    else if (m_hover_resize || m_hdr_resize)       want = 1;   // track-height edge
    if (want == s_active) return;
    SDL_Cursor* c = want == 3 ? s_loop : want == 2 ? s_sizewe
                  : want == 1 ? s_sizens : s_arrow;
    if (c) { SDL_SetCursor(c); s_active = want; }
}

//----------------------------------------------------------------------------
//  top time ruler (ports perftime::on_expose_event)
//----------------------------------------------------------------------------
void ArrangeView::draw_ruler(App& app, const std::vector<int>& act)
{
    (void)act;
    const Theme& t = theme();
    SDL_Rect rl{ canvas_x(), rect.y, canvas_w(), ruler_h };
    fill_rect(app.ren, rl, t.panel);
    hline(app.ren, rl.x, rl.x + rl.w, rl.y + rl.h - 1, t.accent);

    long first = m_scroll_ticks - (m_scroll_ticks % m_measure_len);
    int bar = (int)(first / m_measure_len);
    for (long tick = first; ; tick += m_measure_len, ++bar) {
        int x = tick_to_x(tick);
        if (x > rl.x + rl.w) break;
        if (x >= rl.x) {
            vline(app.ren, x, rl.y, rl.y + rl.h, t.accent);
            char b[8]; std::snprintf(b, sizeof(b), "%d", bar + 1);
            app.mono.draw(app.ren, x + 2, rl.y + 1, b, t.text);
        }
    }

    // L / R loop markers
    long left  = m_perf->get_left_tick();
    long right = m_perf->get_right_tick();
    int lx = tick_to_x(left);
    int rx = tick_to_x(right);
    if (lx >= rl.x && lx <= rl.x + rl.w) {
        fill_rect(app.ren, SDL_Rect{ lx, rl.y + rl.h - 10, 8, 10 }, t.accent);
        app.mono.draw(app.ren, lx + 1, rl.y + rl.h - 10, "L", t.bg);
    }
    if (rx >= rl.x && rx <= rl.x + rl.w) {
        fill_rect(app.ren, SDL_Rect{ rx - 7, rl.y + rl.h - 10, 8, 10 }, t.accent);
        app.mono.draw(app.ren, rx - 6, rl.y + rl.h - 10, "R", t.bg);
    }

    // playhead nub on the ruler
    int px = tick_to_x(playhead());
    if (px >= rl.x && px <= rl.x + rl.w) {
        for (int i = 0; i < 5; ++i)
            hline(app.ren, px - (4 - i), px + (4 - i), rl.y + 2 + i, t.hi);
    }

    // snap + follow readout, right-aligned in the ruler
    char st[40];
    std::snprintf(st, sizeof(st), "SNAP:%s%s", snap_label(m_snap_idx),
                  m_follow ? " FOLLOW" : "");
    int cw = app.mono.cw() ? app.mono.cw() : 6;
    int tw = (int)std::strlen(st) * cw;
    int sx = rl.x + rl.w - tw - 4;
    if (sx > rl.x + 4) {
        m_snap_rect = SDL_Rect{ sx - 3, rl.y + 1, tw + 6, rl.h - 2 };  // clickable
        fill_rect(app.ren, m_snap_rect, t.panel);
        frame_rect(app.ren, m_snap_rect, t.dim);
        app.mono.draw(app.ren, sx, rl.y + (rl.h - app.mono.ch()) / 2, st, t.accent);
    } else {
        m_snap_rect = SDL_Rect{ 0, 0, 0, 0 };
    }
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------
bool ArrangeView::on_mouse(App& app, const MouseEv& e)
{
    if (!m_perf) return false;

    m_mx = e.x; m_my = e.y;                 // track pointer for hover highlight

    // release : commit a pending drag-copy, then clear ALL drag state (handled
    // first so state is cleared even while a menu / rename field is up) --------
    if (!e.pressed) {
        if (m_lassoing) {
            int x0 = std::min(m_lasso_x0, m_lasso_x1);
            int y0 = std::min(m_lasso_y0, m_lasso_y1);
            int x1 = std::max(m_lasso_x0, m_lasso_x1);
            int y1 = std::max(m_lasso_y0, m_lasso_y1);
            select_clips_in_rect(SDL_Rect{ x0, y0, x1 - x0, y1 - y0 },
                                 (SDL_GetModState() & KMOD_CTRL) != 0);
        }
        if (m_copying && m_drop_seq >= 0 && m_perf->is_active(m_drop_seq)) {
            if (m_copy_len > 0) {
                long t = m_ghost_tick < 0 ? 0 : m_ghost_tick;
                create_pattern(m_drop_seq, t, m_copy_len, m_copy_offset,
                               m_copy_events);
            }
        }
        if (m_fade_grab != FadeGrab::None) {
            if (m_fade_seq >= 0) commit_fade(m_fade_seq);   // final push to engine
            m_fade_grab = FadeGrab::None; m_fade_seq = -1;
        }
        // A moved/trimmed AUDIO clip: apply the Ardour content law for this
        // gesture and push the new region (position, span, source-offset) to the
        // engine so playback follows non-destructively.
        if ((m_moving || m_growing) && m_move_seq >= 0 && m_audio.count(m_move_seq)) {
            const bool leftTrim  = m_growing && m_grow_dir;
            const bool rightTrim = m_growing && !m_grow_dir;
            commit_region(m_move_seq, leftTrim, rightTrim);
        }
        // EXTEND release: the trigger's new end sets the region length (right-trim
        // law); for a looped region commit_region keeps the over-length span.
        if (m_extending && m_extend_seq >= 0 && m_audio.count(m_extend_seq)) {
            commit_region(m_extend_seq, false, true);
        }
        m_extending = false; m_extend_seq = -1;
        m_move_seq = -1;
        m_gain_grab = false; m_gain_seq = -1;
        m_lassoing = false;
        m_copying = false;
        m_mouse_down = m_moving = m_growing = m_adding = m_slipping = false;
        m_hdr_resize = false; m_hdr_resize_seq = -1;
        m_drag_left = m_drag_right = false;
        return true;
    }

    // context menu / add-track chooser intercept a press while open ----------
    if (m_menu_open)      return menu_click(app, e.x, e.y);
    if (m_addmenu_open)   return addmenu_click(app, e.x, e.y);
    if (m_instrmenu_open) return instrmenu_click(app, e.x, e.y);

    // while the inline rename editor is open, swallow presses (Enter/Esc ends)
    if (app.editing_text()) return true;

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
                    if (m_fade_grab == FadeGrab::InCurve) f.inK = k; else f.outK = k;
                }
                commit_fade(m_fade_seq);           // live-update the engine
            }
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
        if (m_lassoing) {
            m_lasso_x1 = e.x;
            m_lasso_y1 = e.y;
            app.request_redraw();
            return true;
        }
        if (m_moving || m_growing || m_adding || m_slipping || m_gain_grab || m_extending) drag_canvas(app, e);
        return true;
    }

    // fresh press -----------------------------------------------------------
    m_mouse_down = true;

    const int spine_end = 4 + 6;                 // spine + gap
    const int rm_w = 14, rm_h = 14;
    const int rm_x = header_w - rm_w - 4;
    const int rm_y = 4;
    const int btn_w = 22, btn_h = 12;
    const int btn_x = rm_x - btn_w - 4;

    // (0) ADD-TRACK "+" affordance : handled before any header/canvas click,
    //     like the context menu.  Opens the chooser popup above the button.
    {
        SDL_Rect ar = add_row_rect();
        if (e.x >= ar.x && e.x < ar.x + ar.w &&
            e.y >= ar.y && e.y < ar.y + ar.h) {
            if (e.button == SDL_BUTTON_LEFT) {
                const int mw   = 160;
                const int rowh = app.font.ch() + 8;
                const int mh   = 2 * rowh + 2;
                int mx = ar.x + 8;
                int my = ar.y - mh;                   // pop upward (row is at bottom)
                if (my < canvas_y()) my = ar.y + row_h;   // else fall back below
                if (mx + mw > rect.x + rect.w) mx = rect.x + rect.w - mw;
                m_addmenu_rect = SDL_Rect{ mx, my, mw, mh };
                m_addmenu_open = true;
                app.request_redraw();
            }
            return true;
        }
    }

    // (a) header column ------------------------------------------------------
    if (e.x < rect.x + header_w && e.y >= canvas_y()) {
        int r = row_at(e.y);
        std::vector<int> act = active_list();
        int idx = m_v_offset + r;
        if (idx < 0 || idx >= (int)act.size()) return true;
        int seq = act[idx];
        sequence* s = m_perf->get_sequence(seq);
        int lx = e.x - rect.x;                    // x within header
        int row_y0 = row_top(r);                  // this row's top (screen y)
        int lh = track_h(seq);
        int ly = e.y - row_y0;                    // y within row

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
            app.request_redraw();
            return true;
        }

        // Mute button
        if (lx >= btn_x && lx <= btn_x + btn_w && ly >= 4 && ly <= 4 + btn_h) {
            s->set_song_mute(!s->get_song_mute());
            app.request_redraw(); return true;
        }
        // Solo button
        if (lx >= btn_x && lx <= btn_x + btn_w &&
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
        // Clickable SNAP readout: left-click cycles forward, right-click back.
        if (e.x >= m_snap_rect.x && e.x < m_snap_rect.x + m_snap_rect.w &&
            e.y >= m_snap_rect.y && e.y < m_snap_rect.y + m_snap_rect.h) {
            if (e.button == SDL_BUTTON_LEFT)  m_snap_idx = (m_snap_idx + 1) % 9;
            else if (e.button == SDL_BUTTON_RIGHT) m_snap_idx = (m_snap_idx + 8) % 9;
            m_snap = snap_value(m_snap_idx);
            app.request_redraw();
            return true;
        }
        if (e.button == SDL_BUTTON_LEFT) {
            const int GRAB = 8;   // px hit tolerance around a marker
            int xl = tick_to_x(m_perf->get_left_tick());
            int xr = tick_to_x(m_perf->get_right_tick());
            if      (std::abs(e.x - xl) <= GRAB) m_drag_left  = true;
            else if (std::abs(e.x - xr) <= GRAB) m_drag_right = true;
        }
        app.request_redraw();
        return true;
    }

    // (c) canvas -------------------------------------------------------------
    if (e.x >= rect.x + header_w && e.y >= canvas_y()) {
        int r = row_at(e.y);
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

    // clear any prior selection on the previously-touched track
    if (m_drop_seq >= 0 && m_drop_seq != seq && m_perf->is_active(m_drop_seq))
        m_perf->get_sequence(m_drop_seq)->unselect_triggers();

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
    // over clip move/resize).
    if (e.button == SDL_BUTTON_LEFT && clip_seq >= 0 && m_audio.count(seq)) {
        SDL_Rect clip;
        if (clip_rect_of(seq, clip)) {
            FadeGrab g = fade_at(seq, clip, e.x, e.y);
            if (g != FadeGrab::None) {
                m_fade_grab = g; m_fade_seq = seq;
                m_mouse_down = true;
                if (!m_clipFade.count(seq)) m_clipFade[seq] = ClipFade{};
                app.request_redraw();
                return;
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
        if (m_audio.count(seq) && s->get_trigger_state(tick)) {
            m_perf->push_trigger_undo();
            split_audio_clip(seq, snap(tick));       // audio region split (at the click)
            app.request_redraw();
        } else if (s->get_trigger_state(tick)) {
            m_perf->push_trigger_undo();
            s->split_trigger(tick);                  // MIDI: PatchKnob midpoint split
            app.request_redraw();
        }
        return;
    }
    if (e.button != SDL_BUTTON_LEFT) return;

    if ((SDL_GetModState() & KMOD_SHIFT) != 0) {
        m_lassoing = true;
        m_lasso_x0 = m_lasso_x1 = e.x;
        m_lasso_y0 = m_lasso_y1 = e.y;
        if ((SDL_GetModState() & KMOD_CTRL) == 0)
            unselect_all_triggers();
        app.request_redraw();
        return;
    }

    long seq_len = s->get_length();
    bool state = clip_seq >= 0;

    if (state) {
        bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;

        // double-click (no Ctrl) opens the clip's editor (piano roll).  Rename is
        // on the right-click menu ("Rename").
        unsigned now = SDL_GetTicks();
        if (!ctrl && m_last_click_seq == seq && (now - m_last_click_ms) < 400) {
            m_last_click_seq = -1;
            if (m_audio.count(seq)) { if (on_open_sample_editor) on_open_sample_editor(seq); }
            else if (on_open_editor) on_open_editor(seq, 0);
            return;
        }
        m_last_click_seq = seq; m_last_click_tick = tick; m_last_click_ms = now;

        s->select_trigger(tick);
        long start = s->get_selected_trigger_start_tick();
        long end   = s->get_selected_trigger_end_tick();

        // Audio-clip corner/edge affordances: LOOP toggle (top-right), EXTEND
        // (bottom edge), GAIN line.  Checked before the move/trim decision.
        if (m_audio.count(seq)) {
            SDL_Rect cr;
            if (clip_rect_of(seq, cr)) {
                // Loop handle: top-right 12x12 corner -> toggle loop.  Rect MUST
                // match the draw glyph lb{ br-14, y+1, 12, 12 } (br = cr right edge)
                // so the whole icon (and only the icon) is clickable.
                if (cr.w > 20 && e.x >= cr.x + cr.w - 15 && e.x < cr.x + cr.w - 3 &&
                    e.y >= cr.y + 1 && e.y < cr.y + 13) {
                    AudioRegion& r = region_for(seq);
                    r.loop = !r.loop;
                    if (on_clip_loop) on_clip_loop(seq, r.loop);
                    app.request_redraw();
                    return;
                }
                // Extend handle: bottom edge -> drag horizontally to lengthen.
                if (e.y >= cr.y + cr.h - 4 && e.y <= cr.y + cr.h) {
                    m_extending = true; m_extend_seq = seq;
                    s->select_trigger(tick);
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
            // Ctrl+drag DUPLICATES: leave the original, drag a ghost, and add a
            // new trigger for the SAME sequence at the drop tick on release.
            m_copying     = true;
            m_moving      = true;          // route motion through drag_canvas
            m_copy_len    = end - start + 1;
            m_copy_offset = trigger_offset_at(s, tick);
            m_copy_events = true;
            m_drop_offset = tick - start;
            m_ghost_tick  = start;
        } else {
            // select + decide move vs. resize by proximity to the clip edges
            m_perf->push_trigger_undo();
            int xs = tick_to_x(start), xe = tick_to_x(end);
            const int handle = 6;
            if (e.x - xs <= handle) {
                m_growing = true; m_grow_dir = true;  m_drop_offset = tick - start; m_move_seq = seq;
            } else if (xe - e.x <= handle) {
                m_growing = true; m_grow_dir = false; m_drop_offset = tick - end; m_move_seq = seq;
            } else if ((SDL_GetModState() & KMOD_ALT) && m_audio.count(seq)) {
                // Alt+drag an audio body = SLIP: slide the source under the block.
                m_slipping = true; m_move_seq = seq;
                m_slip_ref_tick = tick; m_slip_ref_source = region_for(seq).source;
            } else {
                m_moving = true; m_drop_offset = tick - start; m_move_seq = seq;
            }
        }
    } else {
        // empty lane: place a fresh one-clip-length region, snapped to the view
        // grid.  Carry the loop offset so the clip plays from its OWN content
        // start at the drop point (Ardour-style), not phase-locked to the global
        // timeline.  On an empty lane trigger_offset_at() is 0; the (t % seq_len)
        // term cancels the timeline phase so content tick 0 lands on the region
        // start.  When the grid snaps to a bar (seq_len multiple) this reduces to
        // offset 0, matching the previous behaviour.
        m_perf->push_trigger_undo();
        long t = snap(tick);
        if (t < 0) t = 0;
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
    if (m_drop_seq < 0 || !m_perf->is_active(m_drop_seq)) return;
    sequence* s = m_perf->get_sequence(m_drop_seq);
    if (!s) return;
    long tick = x_to_tick(e.x);

    if (m_copying) {
        // Ctrl+drag duplicate: just track the ghost; commit happens on release.
        long t = snap(tick - m_drop_offset);
        if (t < 0) t = 0;
        m_ghost_tick = t;
        app.request_redraw();
        return;
    }
    if (m_extending && m_extend_seq >= 0) {
        // EXTEND (bottom-edge drag): grow the trigger's END to lengthen the clip.
        // For a looped region the source wraps to fill the new span; otherwise it
        // is clamped to the source on release (commit_region right-trim law).
        sequence* es = m_perf ? m_perf->get_sequence(m_extend_seq) : nullptr;
        if (es) {
            long t = snap(tick);
            es->move_selected_triggers_to(t, false, 0);   // GROW_END
        }
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
    if (m_adding) {
        long seq_len = s->get_length();
        s->grow_trigger(m_drop_tick, tick, seq_len);
    } else if (m_moving) {
        long t = snap(tick - m_drop_offset);
        s->move_selected_triggers_to(t, isAudio ? false : true);
    } else if (m_growing) {
        long t = snap(tick - m_drop_offset);
        if (m_grow_dir) s->move_selected_triggers_to(t, false, 0);
        else            s->move_selected_triggers_to(t - 1, false, 1);
    }
    app.request_redraw();
}

bool ArrangeView::on_wheel(App& app, int dx, int dy)
{
    bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;

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

    if (dy != 0) {
        // vertical wheel = horizontal zoom (ticks per pixel).  Min is well below 1
        // tick/px so you can zoom down to individual audio samples.
        double f = (dy > 0) ? (1.0 / 1.2) : 1.2;
        double ns = m_scale_x * f;
        if (ns < 0.005) ns = 0.005;
        if (ns > 512.0) ns = 512.0;
        m_scale_x = ns;
    }
    if (dx != 0) {
        m_scroll_ticks += (long)(dx * 8 * m_scale_x);
        if (m_scroll_ticks < 0) m_scroll_ticks = 0;
    }
    app.request_redraw();
    return true;
}

bool ArrangeView::on_key(App& app, SDL_Keycode k)
{
    if (!m_perf) return false;
    long page = (long)(canvas_w() * m_scale_x / 4);
    bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    switch (k) {
    case SDLK_LEFT:  m_scroll_ticks -= page; if (m_scroll_ticks < 0) m_scroll_ticks = 0; break;
    case SDLK_RIGHT: m_scroll_ticks += page; break;
    case SDLK_UP:    if (m_v_offset > 0) --m_v_offset; break;
    case SDLK_DOWN:  ++m_v_offset; break;
    case SDLK_HOME:  m_scroll_ticks = 0; m_v_offset = 0; break;   // (also Ctrl+Home)
    case SDLK_EQUALS:
    case SDLK_PLUS:  m_scale_x = std::max(0.005, m_scale_x / 1.2); break;
    case SDLK_MINUS: m_scale_x = std::min(512.0, m_scale_x * 1.2); break;
    case SDLK_t:     set_mode(mode() == Mode::Light ? Mode::Midnight : Mode::Light); break;
    case SDLK_c:
        if (!ctrl) return false;
        copy_selected_clips();
        break;
    case SDLK_v:
        if (!ctrl) return false;
        paste_clips(m_drop_tick);
        break;
    case SDLK_z:     // Ctrl+Z = undo the last clip edit (move/trim/split/delete)
        if (!ctrl) return false;
        m_perf->pop_trigger_undo();
        // A deleted AUDIO clip also needs its rendered buffer restored from the
        // shell's disk cache; the trigger alone would come back silent.
        if (on_undo) on_undo();
        break;

    // ---- Qtractor-inspired bindings ----------------------------------------
    case SDLK_f:     // F = zoom-to-fit ;  Ctrl+F = toggle follow-playhead
        if (SDL_GetModState() & KMOD_CTRL) m_follow = !m_follow;
        else                               zoom_to_fit();
        break;
    case SDLK_l:     m_follow = !m_follow; break;                 // L = follow toggle
    case SDLK_a:     m_scroll_ticks = 0; m_v_offset = 0; break;   // A = scroll to start
    case SDLK_s:     // S = cycle snap  BAR..1/128..OFF
        m_snap_idx = (m_snap_idx + 1) % 9;
        m_snap = snap_value(m_snap_idx);
        break;
    case SDLK_x:     // X = split the last-touched clip at the playhead
        if (m_drop_seq >= 0 && m_perf->is_active(m_drop_seq)) {
            sequence* s = m_perf->get_sequence(m_drop_seq);
            long ph = playhead();
            if (s && s->get_trigger_state(ph)) {
                m_perf->push_trigger_undo();
                s->split_trigger(ph);
            }
        }
        break;
    case SDLK_DELETE:
        delete_selected_clips();
        break;
    default: return false;
    }
    app.request_redraw();
    return true;
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
            for (int i = 0; i < c_max_sequence; ++i)
                if (m_perf->is_active(i))
                    m_mute_snapshot[i] = m_perf->get_sequence(i)->get_song_mute();
            m_solo_active = true;
        }
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i))
                m_perf->get_sequence(i)->set_song_mute(!m_solo[i]);
    } else if (m_solo_active) {
        for (int i = 0; i < c_max_sequence; ++i)
            if (m_perf->is_active(i))
                m_perf->get_sequence(i)->set_song_mute(m_mute_snapshot[i] != 0);
        m_solo_active = false;
    }
}

} // namespace arrange
