//----------------------------------------------------------------------------
//  sdlui/views/arrange/arrange_edit.cpp
//
//  Pro Tools Reference Guide ch.28 (Editing Basics) + ch.31 (Editing Clips
//  and Selections) ported onto ArrangeView.  This file carries:
//
//    * MULTIPLE UNDO: the one queue of undoable operations (default 32
//      levels, 1..64) that every arrange edit and the shell's audio-clip
//      delete cache push onto, with the Undo History window (bold = undoable,
//      dim = redoable, warning role on the entry about to fall off, creation
//      times, Undo All / Redo All / Clear Undo Queue),
//    * the basic edit commands over the ch.30 edit selection: Cut / Copy /
//      Paste / Clear as range OR object edits, Shuffle-aware, with the
//      auto-created leftover clips on either side of a cut,
//    * the special commands (clip-gain specials, Repeat to Fill Selection),
//    * ch.31: Capture Clip, the Separate commands (At Selection / On Grid /
//      At Transients + Pre-Separate amount), Heal Separation, the Trim
//      command family, nudging clips / trims / contents by the Nudge value,
//      Quantize to Grid, Layered Editing, Consolidate, Compact, TCE Edit to
//      Timeline Selection, Fit to Selection and clip Ratings,
//    * the VIEW options menu (View > Waveforms / View > Clip) and the EDIT
//      command menu, plus the per-clip adornments those options draw.
//
//  Same rules as the rest of the view: gestures stay in arrange_view.cpp,
//  every colour comes from ui::theme().
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
#include <memory>
#include <cctype>

using namespace ui;

namespace arrange {

// waveform-cache epoch (defined in arrange_view.cpp): the VIEW menu's
// waveform options change how the cached envelope is reduced, so bump it.
extern int g_wave_epoch;

//----------------------------------------------------------------------------
//  tiny local helpers (mirrors of the file-private statics elsewhere)
//----------------------------------------------------------------------------
static bool ed_in(const SDL_Rect& r, int x, int y)
{
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static SDL_Rect ed_clamp(SDL_Rect box, const SDL_Rect& bounds)
{
    if (box.w > bounds.w) box.w = bounds.w;
    if (box.h > bounds.h) box.h = bounds.h;
    if (box.x + box.w > bounds.x + bounds.w) box.x = bounds.x + bounds.w - box.w;
    if (box.y + box.h > bounds.y + bounds.h) box.y = bounds.y + bounds.h - box.h;
    if (box.x < bounds.x) box.x = bounds.x;
    if (box.y < bounds.y) box.y = bounds.y;
    return box;
}

static std::string ed_fit(const ui::Font& font, std::string text, int maxw)
{
    if (maxw <= 0) return "";
    if (font.text_w(text) <= maxw) return text;
    while (!text.empty() && font.text_w(text) > maxw) text.pop_back();
    return text;
}

//----------------------------------------------------------------------------
//  MULTIPLE UNDO (ch.28 p664-666)
//
//  The queue holds closures.  Trigger-snapshot edits ride perform's own
//  snapshot stack in LOCKSTEP: push_undo() pushes one perform snapshot and one
//  queue entry whose undo/redo pop that stack -- so as long as every edit goes
//  through push_undo (they all do now), the two stacks cannot drift.  Ops the
//  perform stack cannot express (the disk-cached audio-clip delete, audio
//  REGION geometry) carry their own closures instead.
//----------------------------------------------------------------------------
void ArrangeView::push_undo(const char* name)
{
    if (m_perf) m_perf->push_trigger_undo();
    note_engine_undo(name);
}

void ArrangeView::note_engine_undo(const char* name)
{
    UndoOp op;
    op.name = name ? name : "Edit";
    op.undo.push_back([this]{ if (m_perf) m_perf->pop_trigger_undo(); });
    op.redo.push_back([this]{ if (m_perf) m_perf->pop_trigger_redo(); });
    push_undo_op(std::move(op));
}

void ArrangeView::push_undo_op(UndoOp op)
{
    op.when = std::time(nullptr);
    if (m_undo_compound > 0) {
        // an open compound collects the closures into ONE queue entry
        for (size_t i = 0; i < op.undo.size(); ++i)
            m_undo_pending.undo.push_back(std::move(op.undo[i]));
        for (size_t i = 0; i < op.redo.size(); ++i)
            m_undo_pending.redo.push_back(std::move(op.redo[i]));
        return;
    }
    // a fresh edit truncates the redo tail (standard undo-queue law)
    if (m_undo_done < m_undo_q.size()) m_undo_q.resize(m_undo_done);
    m_undo_q.push_back(std::move(op));
    // beyond the Levels of Undo preference the OLDEST entry falls off
    while ((int)m_undo_q.size() > m_undo_levels)
        m_undo_q.erase(m_undo_q.begin());
    m_undo_done = m_undo_q.size();
}

void ArrangeView::begin_undo_compound(const char* name)
{
    if (m_undo_compound++ == 0) {
        m_undo_pending = UndoOp{};
        m_undo_pending.name = name ? name : "Edit";
    }
}

void ArrangeView::end_undo_compound()
{
    if (m_undo_compound <= 0) return;
    if (--m_undo_compound == 0 &&
        !(m_undo_pending.undo.empty() && m_undo_pending.redo.empty())) {
        UndoOp op;
        std::swap(op, m_undo_pending);
        push_undo_op(std::move(op));
    }
}

//  Ctrl+Z / Ctrl+Shift+Z routed by the app (Widget::on_undo).  Consume only
//  while this queue has something to walk, so a patch or mixer edit made with
//  the arrange view focused still reaches the shell's project-wide undo.
bool ArrangeView::on_undo(App& app, bool redo)
{
    if (redo) {
        if (m_undo_done >= m_undo_q.size()) return false;
        do_redo();
    } else {
        if (m_undo_done == 0) return false;
        do_undo();
    }
    app.request_redraw();
    return true;
}

void ArrangeView::do_undo()
{
    if (m_undo_done == 0) { flash("Can't Undo"); return; }
    UndoOp& op = m_undo_q[--m_undo_done];
    for (size_t i = op.undo.size(); i-- > 0; ) op.undo[i]();   // newest first
    flash("Undo: " + op.name);
}

void ArrangeView::do_redo()
{
    if (m_undo_done >= m_undo_q.size()) { flash("Can't Redo"); return; }
    UndoOp& op = m_undo_q[m_undo_done++];
    for (size_t i = 0; i < op.redo.size(); ++i) op.redo[i]();  // oldest first
    flash("Redo: " + op.name);
}

void ArrangeView::undo_all()
{
    while (m_undo_done > 0) {
        UndoOp& op = m_undo_q[--m_undo_done];
        for (size_t i = op.undo.size(); i-- > 0; ) op.undo[i]();
    }
    flash("Undo All");
}

void ArrangeView::redo_all()
{
    while (m_undo_done < m_undo_q.size()) {
        UndoOp& op = m_undo_q[m_undo_done++];
        for (size_t i = 0; i < op.redo.size(); ++i) op.redo[i]();
    }
    flash("Redo All");
}

void ArrangeView::clear_undo_queue()
{
    m_undo_q.clear();
    m_undo_done = 0;
}

void ArrangeView::set_undo_levels(int n)
{
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    m_undo_levels = n;
    while ((int)m_undo_q.size() > m_undo_levels) {
        m_undo_q.erase(m_undo_q.begin());              // oldest falls off
        if (m_undo_done > 0) --m_undo_done;
    }
}

//  Delete an audio clip THROUGH the queue.  The shell disk-caches the audio
//  (on_clip_delete) so undo restores it from disk (on_undo returns the seq it
//  came back on); redo tears that restored clip down again -- which re-caches
//  it, keeping the shell's ring and this queue in step.
void ArrangeView::delete_audio_clip_undoable(int seq)
{
    if (on_clip_delete) on_clip_delete(seq);
    forget_seq(seq);
    std::shared_ptr<int> live = std::make_shared<int>(-1);
    UndoOp op;
    op.name = "Delete Audio Clip";
    op.undo.push_back([this, live]{
        *live = on_restore_deleted_clip ? on_restore_deleted_clip() : -1; });
    op.redo.push_back([this, live]{
        if (*live >= 0) {
            if (on_clip_delete) on_clip_delete(*live);
            forget_seq(*live);
            *live = -1;
        }
    });
    push_undo_op(std::move(op));
}

//----------------------------------------------------------------------------
//  transient status line ("Can't Undo", heal refusals, command feedback)
//----------------------------------------------------------------------------
void ArrangeView::flash(const std::string& s)
{
    m_flash = s;
    m_flash_ms = SDL_GetTicks();
}

void ArrangeView::draw_flash(App& app)
{
    if (m_flash.empty()) return;
    const Uint32 age = SDL_GetTicks() - m_flash_ms;
    if (age > 2400) { m_flash.clear(); return; }
    const Theme& t = theme();
    const int w = app.mono.text_w(m_flash) + 16;
    SDL_Rect b{ canvas_x() + (canvas_w() - w) / 2,
                rect.y + rect.h - scrollbar_h - app.mono.ch() - 14,
                w, app.mono.ch() + 8 };
    b = ed_clamp(b, rect);
    fill_rect (app.ren, b, t.panel);
    frame_rect(app.ren, b, t.accent);
    app.mono.draw(app.ren, b.x + 8, b.y + 4, m_flash, t.hi);
    app.add_damage(b);
    app.request_redraw();          // keep frames coming until it expires
}

//----------------------------------------------------------------------------
//  UNDO HISTORY window (fig. pt-665-028): Time | Operations columns, undoable
//  entries bold, redoable entries in the dim role (no italic face in the
//  toolkit -- the manual's italics map to the dim role + a leading ">"), a
//  divider at the done/undone boundary, and the oldest entry in the warning
//  (accent) role when one more edit would push it off the queue.
//----------------------------------------------------------------------------
void ArrangeView::draw_undo_window(App& app)
{
    if (!m_undo_open) return;
    const Theme& t = theme();
    const int rowh = app.mono.ch() + 5;
    const int headH = rowh + 6, titleH = rowh + 6;
    const int visRows = std::min((int)std::max<size_t>(m_undo_q.size(), 1),
                                 std::max(4, (canvas_h() - headH - titleH - 20) / rowh));
    const int W = 300;
    SDL_Rect box{ rect.x + rect.w - scrollbar_w - W - 8, canvas_y() + 8,
                  W, titleH + headH + visRows * rowh + 6 };
    box = ed_clamp(box, rect);
    m_undo_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);

    // title bar + Options selector
    app.mono.draw(app.ren, box.x + 8, box.y + 4, "Undo History", t.hi);
    app.mono.draw(app.ren, box.x + 9, box.y + 4, "Undo History", t.hi);  // bold
    m_undo_opts_btn = SDL_Rect{ box.x + box.w - 22, box.y + 3, 18, rowh };
    {
        const bool hot = ed_in(m_undo_opts_btn, m_mx, m_my);
        fill_rect (app.ren, m_undo_opts_btn, hot ? t.accent : t.keybg);
        frame_rect(app.ren, m_undo_opts_btn, t.dim);
        app.mono.draw_centered(app.ren, m_undo_opts_btn, "v", hot ? t.bg : t.text);
        if (hot) tip("Options: times / Undo All / Redo All / Clear / Levels",
                     m_undo_opts_btn.x - 180, m_undo_opts_btn.y + rowh + 4);
    }
    hline(app.ren, box.x + 1, box.x + box.w - 1, box.y + titleH - 1, t.dim);

    // column header
    int y = box.y + titleH;
    app.mono.draw(app.ren, box.x + 8, y + 2,
                  m_undo_show_times ? "Time      Operations" : "Operations", t.dim);
    hline(app.ren, box.x + 1, box.x + box.w - 1, y + headH - 1, t.dim);
    y += headH;

    // clamp the scroll to the list
    const int n = (int)m_undo_q.size();
    if (m_undo_scroll > n - visRows) m_undo_scroll = std::max(0, n - visRows);
    if (m_undo_scroll < 0) m_undo_scroll = 0;

    if (n == 0)
        app.mono.draw(app.ren, box.x + 8, y + 2, "(empty)", t.dim);
    const bool full = (n >= m_undo_levels);
    for (int i = m_undo_scroll; i < n && i < m_undo_scroll + visRows; ++i) {
        const UndoOp& op = m_undo_q[(size_t)i];
        SDL_Rect row{ box.x + 1, y, box.w - 2, rowh };
        const bool done = (size_t)i < m_undo_done;
        const bool hot  = ed_in(row, m_mx, m_my);
        if (hot) fill_rect(app.ren, row, t.keybg);
        // the divider between undoable and redoable operations
        if ((size_t)i == m_undo_done && i > m_undo_scroll)
            hline(app.ren, row.x + 2, row.x + row.w - 2, row.y, t.accent);
        int x = box.x + 8;
        if (m_undo_show_times) {
            char tb[16] = "";
            std::tm tmv{};
#ifdef _WIN32
            std::tm* pt = std::localtime(&op.when);
            if (pt) tmv = *pt;
#else
            localtime_r(&op.when, &tmv);
#endif
            std::snprintf(tb, sizeof tb, "%02d:%02d:%02d",
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            app.mono.draw(app.ren, x, row.y + 2, tb, t.dim);
            x += app.mono.text_w("00:00:00") + 8;
        }
        // oldest entry one edit from falling off the full queue -> warning role
        const bool warn = full && i == 0 && done;
        const Color fg = warn ? t.accent : (done ? t.text : t.dim);
        const std::string nm = ed_fit(app.mono,
                                      (done ? "" : "> ") + op.name,
                                      box.x + box.w - 6 - x);
        app.mono.draw(app.ren, x, row.y + 2, nm, fg);
        if (done)                       // "bold" = a 1px double-strike
            app.mono.draw(app.ren, x + 1, row.y + 2, nm, fg);
        y += rowh;
    }
    app.add_damage(box);

    // ---- Options pop-up -----------------------------------------------------
    if (m_undo_opts) {
        const int orow = rowh + 4;
        SDL_Rect ob{ m_undo_opts_btn.x - 180, m_undo_opts_btn.y + rowh + 2,
                     200, 5 * orow + 2 };
        ob = ed_clamp(ob, rect);
        m_undo_opts_rect = ob;
        fill_rect (app.ren, ob, t.panel);
        frame_rect(app.ren, ob, t.accent);
        char lv[48];
        std::snprintf(lv, sizeof lv, "Levels of Undo:  < %d >", m_undo_levels);
        const char* rows[5] = {
            m_undo_show_times ? "[*] Show Creation Times" : "[ ] Show Creation Times",
            "Undo All", "Redo All", "Clear Undo Queue", lv };
        for (int i = 0; i < 5; ++i) {
            SDL_Rect r{ ob.x + 1, ob.y + 1 + i * orow, ob.w - 2, orow };
            const bool hot = ed_in(r, m_mx, m_my);
            if (hot) fill_rect(app.ren, r, t.accent);
            app.mono.draw(app.ren, r.x + 8, r.y + 3, rows[i],
                          hot ? t.bg : t.text);
        }
    }
}

bool ArrangeView::undo_window_mouse(App& app, const MouseEv& e)
{
    if (!m_undo_open || !e.pressed) return false;
    const int rowh = app.mono.ch() + 5;
    // Options pop-up first (drawn on top)
    if (m_undo_opts) {
        if (ed_in(m_undo_opts_rect, e.x, e.y)) {
            const int orow = rowh + 4;
            const int i = (e.y - m_undo_opts_rect.y - 1) / orow;
            switch (i) {
            case 0: m_undo_show_times = !m_undo_show_times; break;
            case 1: undo_all(); break;
            case 2: redo_all(); break;
            case 3: clear_undo_queue(); break;
            case 4:  // Levels of Undo: left half -, right half +
                set_undo_levels(m_undo_levels +
                    ((e.x < m_undo_opts_rect.x + m_undo_opts_rect.w / 2) ? -1 : +1));
                app.request_redraw();
                return true;             // stays open for repeated stepping
            default: break;
            }
            m_undo_opts = false;
            app.request_redraw();
            return true;
        }
        m_undo_opts = false;             // click elsewhere closes the pop-up
        app.request_redraw();
        return true;
    }
    if (!ed_in(m_undo_rect, e.x, e.y)) return false;
    if (ed_in(m_undo_opts_btn, e.x, e.y)) {
        m_undo_opts = true;
        app.request_redraw();
        return true;
    }
    // click an entry: jump the session to that state
    const int titleH = rowh + 6, headH = rowh + 6;
    const int listY = m_undo_rect.y + titleH + headH;
    if (e.y >= listY) {
        const int i = m_undo_scroll + (e.y - listY) / rowh;
        if (i >= 0 && i < (int)m_undo_q.size()) {
            if ((size_t)i < m_undo_done)          // bold: undo down TO this op
                while (m_undo_done > (size_t)i) {
                    UndoOp& op = m_undo_q[--m_undo_done];
                    for (size_t k = op.undo.size(); k-- > 0; ) op.undo[k]();
                }
            else                                   // dim: redo up THROUGH it
                while (m_undo_done <= (size_t)i) {
                    UndoOp& op = m_undo_q[m_undo_done++];
                    for (size_t k = 0; k < op.redo.size(); ++k) op.redo[k]();
                }
        }
    }
    app.request_redraw();
    return true;                                   // presses inside stay inside
}

bool ArrangeView::undo_window_wheel(int mx, int my, int dy)
{
    if (!m_undo_open || !ed_in(m_undo_rect, mx, my)) return false;
    m_undo_scroll -= dy;
    if (m_undo_scroll < 0) m_undo_scroll = 0;
    return true;
}

//----------------------------------------------------------------------------
//  ch.28 -- BASIC EDIT COMMANDS over the edit selection
//----------------------------------------------------------------------------
void ArrangeView::range_lanes(std::vector<int>& lanes) const
{
    lanes.clear();
    std::vector<int> act = active_list();
    int lo, hi;
    sel_rows(lo, hi);
    if (lo < 0) { lo = 0; hi = (int)act.size() - 1; }    // no rows = all lanes
    for (int r = std::max(0, lo); r <= hi && r < (int)act.size(); ++r)
        lanes.push_back(act[(size_t)r]);
}

//  Split every clip crossing `a` or `b` on the selected lanes -- this is what
//  auto-creates the leftover clips on either side of a cut / clear / separate.
//  Boundaries are used RAW (the selection is already what it is; the Grid
//  mode's esnap must not re-snap it).
void ArrangeView::separate_at(long a, long b)
{
    std::vector<int> lanes;
    range_lanes(lanes);
    for (int lane : lanes) {
        for (long cut : { a, b }) {
            if (cut <= 0) continue;
            const int cs = clip_sequence_at(lane, cut);
            if (cs < 0) continue;
            if (m_audio.count(cs)) {
                const AudioRegion r = region_for(cs);
                if (cut > r.position && cut < r.position + r.length) {
                    push_undo("Separate");
                    split_audio_clip(cs, cut);
                }
                continue;
            }
            sequence* s = m_perf->get_sequence(cs);
            if (!s) continue;
            long on = -1, off = -1, offs = 0;
            {
                long o, f, of; bool tsel;
                s->reset_draw_trigger_marker();
                while (s->get_next_trigger(&o, &f, &tsel, &of))
                    if (cut > o && cut <= f) { on = o; off = f; offs = of; break; }
            }
            if (on < 0) continue;
            (void)off; (void)offs;
            // split_clip_at applies esnap; call the split machinery on the
            // exact tick by suspending snap via a Slip round-trip is ugly --
            // instead reuse its guts: the trigger walk above found the clip,
            // so replicate the tail of split_clip_at exactly (offset fold,
            // independent right-half sequence).
            push_undo("Separate");
            const bool looping = s->get_loop_enabled();
            long rightOff = offs + (cut - on);
            if (looping) {
                const long period = s->repeat_period();
                if (period > 0) rightOff = ((rightOff % period) + period) % period;
            }
            s->del_trigger(cut);
            s->add_trigger(on, cut - on, offs, false);
            const int rseq = create_pattern(cs, cut, off - cut + 1, rightOff, true);
            if (rseq < 0) {
                s->add_trigger(cut, off - cut + 1, rightOff, false);
            } else if (sequence* rs = m_perf->get_sequence(rseq)) {
                const char* nm = s->get_name();
                if (nm) rs->set_name(nm);
                auto_name_clip(rseq, cs);
            }
        }
    }
}

//  Select exactly the clips fully inside [a,b) on the selected lanes.
void ArrangeView::select_range_clips(long a, long b)
{
    unselect_all_triggers();
    std::vector<int> lanes;
    range_lanes(lanes);
    std::vector<ClipSpan> spans;
    for (int lane : lanes)
        for (int cs : lane_sequences(lane)) {
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans)
                if (sp.on >= a && sp.endEx <= b) {
                    if (m_audio.count(cs)) region_for(cs).selected = true;
                    else if (sequence* s = m_perf->get_sequence(cs))
                        s->select_trigger(sp.on);
                }
        }
}

//  Ctrl+C.  A RANGE copy clips the covered material to the range boundaries
//  without altering the track; an object copy is the whole-clip copy.
//  Selections in a track's master view carry the clip content; when a lane is
//  an automation lane, the copied clip IS that lane's automation region --
//  the view has no per-breakpoint write access to the curve store, so
//  bounding breakpoints at the cut edges cannot be synthesised here (the
//  region boundary carries the slope instead).  See the final report.
void ArrangeView::edit_copy()
{
    if (!have_range()) { copy_selected_clips(); return; }
    const long a = m_sel_start, b = m_sel_end;
    std::vector<int> lanes;
    range_lanes(lanes);
    std::vector<ClipCopy> clips;
    std::vector<ClipSpan> spans;
    for (int lane : lanes)
        for (int cs : lane_sequences(lane)) {
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans) {
                const long s = std::max(sp.on, a), e = std::min(sp.endEx, b);
                if (e <= s) continue;
                ClipCopy cc;
                cc.seq       = cs;
                cc.rel_start = s - a;
                cc.length    = e - s;
                cc.offset    = sp.offset + (s - sp.on);
                clips.push_back(cc);
            }
        }
    if (clips.empty()) { flash("Nothing to copy"); return; }
    m_clip_clipboard.swap(clips);
    m_clip_span  = b - a;                 // the RANGE is the clipboard span
    m_dup_origin = a;
    m_paste_tick = -1;
    flash("Copied " + bars_len(b - a));
}

//  Shared guts of Cut / Clear / Ctrl+Delete: separate at the boundaries,
//  remove everything fully inside, Shuffle closes the gap by the RANGE length
//  (gaps inside the range are part of what is removed).
void ArrangeView::edit_clear(bool force)
{
    (void)force;   // Ctrl+Delete: same removal; Layered only affects overlaps
    if (!have_range()) {
        if (any_selected_clip()) edit_delete();
        else flash("Nothing selected");
        return;
    }
    const long a = m_sel_start, b = m_sel_end;
    begin_undo_compound("Clear");
    push_undo("Clear");
    separate_at(a, b);
    std::vector<int> lanes;
    range_lanes(lanes);
    std::vector<ClipSpan> spans;
    for (int lane : lanes) {
        std::vector<int> seqs = lane_sequences(lane);
        for (int cs : seqs) {
            spans.clear();
            clip_spans(cs, spans);
            bool any = false;
            for (const ClipSpan& sp : spans)
                if (sp.on >= a && sp.endEx <= b) any = true;
            if (!any) continue;
            if (m_audio.count(cs)) {
                delete_audio_clip_undoable(cs);
                continue;
            }
            sequence* s = m_perf->get_sequence(cs);
            if (!s) continue;
            for (const ClipSpan& sp : spans)
                if (sp.on >= a && sp.endEx <= b) s->del_trigger(sp.on);
            if (on_midi_clip_delete) on_midi_clip_delete(cs);
        }
    }
    if (m_edit_mode == EditMode::Shuffle)
        for (int lane : lanes)
            if (m_perf->is_active(lane)) ripple_lane(lane, b, -(b - a));
    end_undo_compound();
}

//  Ctrl+X: Cut = Copy + remove (Clear leaves the clipboard alone).
void ArrangeView::edit_cut()
{
    if (!have_range()) {
        if (!any_selected_clip()) { flash("Nothing selected"); return; }
        begin_undo_compound("Cut");
        copy_selected_clips();
        edit_delete_guts();
        end_undo_compound();
        return;
    }
    edit_copy();
    begin_undo_compound("Cut");
    // reuse Clear's machinery under the "Cut" label
    {
        const long a = m_sel_start, b = m_sel_end;
        push_undo("Cut");
        separate_at(a, b);
        std::vector<int> lanes;
        range_lanes(lanes);
        std::vector<ClipSpan> spans;
        for (int lane : lanes)
            for (int cs : lane_sequences(lane)) {
                spans.clear();
                clip_spans(cs, spans);
                bool any = false;
                for (const ClipSpan& sp : spans)
                    if (sp.on >= a && sp.endEx <= b) any = true;
                if (!any) continue;
                if (m_audio.count(cs)) { delete_audio_clip_undoable(cs); continue; }
                sequence* s = m_perf->get_sequence(cs);
                if (!s) continue;
                for (const ClipSpan& sp : spans)
                    if (sp.on >= a && sp.endEx <= b) s->del_trigger(sp.on);
                if (on_midi_clip_delete) on_midi_clip_delete(cs);
            }
        if (m_edit_mode == EditMode::Shuffle)
            for (int lane : lanes)
                if (m_perf->is_active(lane)) ripple_lane(lane, b, -(b - a));
    }
    end_undo_compound();
}

//  Delete-key object delete, routed through the queue (the old
//  delete_selected_clips called on_clip_delete directly, bypassing history).
void ArrangeView::edit_delete()
{
    if (!any_selected_clip()) { flash("Nothing selected"); return; }
    begin_undo_compound("Delete");
    edit_delete_guts();
    end_undo_compound();
}

//  Ctrl+V.  Paste places the clipboard at the edit insertion point and
//  OVERWRITES any material already there (p668); with Shuffle the material
//  slides right instead (paste_clips does that ripple itself).
void ArrangeView::edit_paste(long at)
{
    if (m_clip_clipboard.empty()) { flash("Clipboard is empty"); return; }
    begin_undo_compound("Paste");
    paste_clips(at);
    end_undo_compound();
}

//----------------------------------------------------------------------------
//  SPECIAL commands (p669)
//----------------------------------------------------------------------------
//  Clip-gain specials.  The automation specials (All Automation, Pan/Volume/
//  Mute) are NOT offered: automation curves live in the engine's automation
//  store and the view's hooks (on_automation_sample) are read-only, so a
//  paste of curve data would have nowhere real to go.  See the final report.
void ArrangeView::repeat_to_fill(App& app)
{
    (void)app;
    if (!have_range())            { flash("Repeat to Fill: make a selection"); return; }
    if (m_clip_clipboard.empty()) { flash("Repeat to Fill: copy a clip first"); return; }
    if (m_clip_span < 1)          { flash("Repeat to Fill: empty clipboard span"); return; }
    const long a = m_sel_start, b = m_sel_end, span = m_clip_span;
    begin_undo_compound("Repeat to Fill");
    push_undo("Repeat to Fill");
    // the selection is the destination: clear it first
    separate_at(a, b);
    {
        std::vector<int> lanes;
        range_lanes(lanes);
        std::vector<ClipSpan> spans;
        for (int lane : lanes)
            for (int cs : lane_sequences(lane)) {
                spans.clear();
                clip_spans(cs, spans);
                bool any = false;
                for (const ClipSpan& sp : spans)
                    if (sp.on >= a && sp.endEx <= b) any = true;
                if (!any) continue;
                if (m_audio.count(cs)) { delete_audio_clip_undoable(cs); continue; }
                if (sequence* s = m_perf->get_sequence(cs)) {
                    for (const ClipSpan& sp : spans)
                        if (sp.on >= a && sp.endEx <= b) s->del_trigger(sp.on);
                    if (on_midi_clip_delete) on_midi_clip_delete(cs);
                }
            }
    }
    // fill with repeats; the last repeat is trimmed to fit the remainder
    std::vector<int> pasted;
    for (long t = a; t < b; t += span) {
        for (const ClipCopy& cc : m_clip_clipboard) {
            const long at = t + cc.rel_start;
            if (at >= b) continue;
            long len = cc.length;
            if (at + len > b) len = b - at;               // trimmed final repeat
            if (len < 1) continue;
            const int ns = create_pattern(cc.seq, at, len, cc.offset, true);
            if (ns >= 0) pasted.push_back(ns);
        }
    }
    // Crossfades between the pasted repeats: Pro Tools raises the Batch Fades
    // dialog here (p753).  Give every seam a REAL default crossfade (regions
    // overlapped, heard through the engine) inside this compound, then open
    // the Batch Fades dialog so the shapes/lengths can be adjusted.
    bool anyAudio = false;
    {
        const long xf = std::max<long>(2, std::min<long>(sec_to_ticks(0.010), span / 8));
        for (size_t i = 0; i + 1 < pasted.size(); ++i)
            if (m_audio.count(pasted[i]) && m_audio.count(pasted[i + 1])) {
                anyAudio = true;
                const long splice = region_for(pasted[i + 1]).position;
                create_crossfade(pasted[i], pasted[i + 1],
                                 splice - xf / 2, splice + xf - xf / 2,
                                 m_def_xfade, false);
            }
    }
    end_undo_compound();
    flash("Filled " + bars_len(b - a));
    if (anyAudio) open_batch_dialog(app);   // adjust the new fades (p753)
}

//----------------------------------------------------------------------------
//  ch.31 -- CAPTURE CLIP (Ctrl+R)
//
//  Pro Tools adds the captured selection to the Clips List.  PatchKnob has no
//  Clips List window; the honest equivalent is the clip CLIPBOARD: Capture
//  copies the selection (track untouched) and names it, and the next Paste
//  materialises clips carrying that name.  Documented in the final report.
//----------------------------------------------------------------------------
void ArrangeView::capture_clip(App& app)
{
    if (!have_range() && !any_selected_clip()) {
        flash("Capture Clip: make a selection first");
        return;
    }
    edit_copy();
    if (m_clip_clipboard.empty()) return;
    m_capture_name = "Capture";
    app.begin_text(&m_capture_name, nullptr, [this, &app](bool ok) {
        if (!ok) m_capture_name.clear();
        else flash("Captured \"" + m_capture_name + "\" (paste to place)");
        app.request_redraw();
    });
    flash("Capture Clip: type a name, Enter to keep");
}

//----------------------------------------------------------------------------
//  ch.31 -- SEPARATE commands + Auto-Name preference
//----------------------------------------------------------------------------
//  Auto-Name Separated Clips: number the auto-created halves off the source
//  clip's name; renaming later promotes them to user-defined (m_user_named).
void ArrangeView::auto_name_clip(int seq, int fromSeq)
{
    if (!m_auto_name_sep || !m_perf) return;
    sequence* d = m_perf->is_active(seq) ? m_perf->get_sequence(seq) : nullptr;
    sequence* s = m_perf->is_active(fromSeq) ? m_perf->get_sequence(fromSeq) : nullptr;
    if (!d || !s || !s->get_name()) return;
    static int counter = 0;                       // numbered variation
    char buf[96];
    std::string base = s->get_name();
    const size_t dash = base.rfind('-');          // strip a previous -NN
    if (dash != std::string::npos && dash + 3 == base.size() &&
        isdigit((unsigned char)base[dash + 1]) && isdigit((unsigned char)base[dash + 2]))
        base.resize(dash);
    std::snprintf(buf, sizeof buf, "%s-%02d", base.c_str(), (++counter) % 100);
    d->set_name(buf);
}

void ArrangeView::separate_selection(int mode, App& app)
{
    if (mode == 0) {
        // At Selection (or at the edit cursor)
        if (have_range()) {
            begin_undo_compound("Separate At Selection");
            separate_at(m_sel_start, m_sel_end);
            end_undo_compound();
        } else if (m_sel_start >= 0) {
            begin_undo_compound("Separate At Selection");
            separate_at(m_sel_start, -1);
            end_undo_compound();
        } else {
            flash("Separate: make a selection or place the cursor");
            return;
        }
        if (!m_auto_name_sep) {
            // preference off: prompt for the new clip's name and apply it to
            // the clips inside the separation
            m_dlg_buf = "Separated";
            const long a = m_sel_start, b = have_range() ? m_sel_end : -1;
            app.begin_text(&m_dlg_buf, nullptr, [this, a, b, &app](bool ok) {
                if (ok && b > a) {
                    std::vector<int> lanes;
                    range_lanes(lanes);
                    std::vector<ClipSpan> spans;
                    for (int lane : lanes)
                        for (int cs : lane_sequences(lane)) {
                            spans.clear();
                            clip_spans(cs, spans);
                            for (const ClipSpan& sp : spans)
                                if (sp.on >= a && sp.endEx <= b)
                                    if (sequence* s = m_perf->get_sequence(cs)) {
                                        s->set_name(m_dlg_buf);
                                        m_user_named.insert(cs);
                                    }
                        }
                }
                app.request_redraw();
            });
        }
        return;
    }
    // On Grid / At Transients first raise the Pre-Separate Amount dialog
    if (!have_range()) { flash("Separate: make a selection first"); return; }
    m_dlg_kind = mode;
    m_dlg_buf = "0";
    app.begin_text(&m_dlg_buf, nullptr, [this, mode, &app](bool ok) {
        if (ok) separate_apply(mode);
        m_dlg_kind = -1;
        app.request_redraw();
    });
    flash(mode == 1 ? "Separate On Grid: pre-separate ms, Enter"
                    : "Separate At Transients: pre-separate ms, Enter");
}

void ArrangeView::separate_apply(int mode)
{
    const long a = m_sel_start, b = m_sel_end;
    if (b <= a) return;
    const double ms = std::atof(m_dlg_buf.c_str());
    const long pad = ms > 0.0 ? sec_to_ticks(ms / 1000.0) : 0;
    std::vector<long> cuts;
    if (mode == 1) {
        long g = grid_ticks();
        if (g < 1) g = m_beat_len;
        for (long t = ((a / g) + 1) * g; t < b; t += g) cuts.push_back(t);
    } else {
        long t = a;
        for (int guard = 0; guard < 4096; ++guard) {
            const long nt = next_transient(t, false);
            if (nt < 0 || nt >= b) break;
            if (nt > t) cuts.push_back(nt);
            t = nt + std::max<long>(1, m_beat_len / 8);
        }
        if (cuts.empty()) { flash("No transients found in the selection"); return; }
    }
    begin_undo_compound(mode == 1 ? "Separate On Grid" : "Separate At Transients");
    // pad the START of each new clip: cut `pad` ticks early
    for (long c : cuts) {
        const long cut = c - pad;
        if (cut <= a) continue;
        separate_at(cut, -1);
    }
    end_undo_compound();
    flash(std::to_string(cuts.size()) + " separation(s)");
}

//----------------------------------------------------------------------------
//  ch.31 -- HEAL SEPARATION (Ctrl+H)
//----------------------------------------------------------------------------
void ArrangeView::heal_separation()
{
    if (!have_range()) { flash("Heal: select across the separation"); return; }
    const long a = m_sel_start, b = m_sel_end;
    std::vector<int> lanes;
    range_lanes(lanes);
    int healed = 0;
    for (int lane : lanes) {
        // collect the lane's spans sorted by start
        struct S { int seq; long on, endEx, off; };
        std::vector<S> ss;
        std::vector<ClipSpan> spans;
        for (int cs : lane_sequences(lane)) {
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans)
                ss.push_back(S{ cs, sp.on, sp.endEx, sp.offset });
        }
        std::sort(ss.begin(), ss.end(), [](const S& x, const S& y){ return x.on < y.on; });
        for (size_t i = 0; i + 1 < ss.size(); ++i) {
            const S& L = ss[i];
            const S& R = ss[i + 1];
            // the separation must sit inside the selection
            if (L.endEx < a || R.on > b) continue;
            if (L.endEx != R.on) continue;                // moved apart: refuse
            const bool laud = m_audio.count(L.seq) != 0;
            const bool raud = m_audio.count(R.seq) != 0;
            if (laud != raud) continue;
            if (laud) {
                // audio: same source clip + contiguous source window
                if (m_audio[L.seq] != m_audio[R.seq]) continue;   // different files
                AudioRegion lr = region_for(L.seq);
                const AudioRegion rr = region_for(R.seq);
                if (lr.source + lr.length != rr.source) continue; // edges changed
                begin_undo_compound("Heal Separation");
                const AudioRegion before = lr;
                lr.length += rr.length;
                m_region[L.seq] = lr;
                commit_region(L.seq);
                {   // region geometry op (perform snapshots don't cover regions)
                    const AudioRegion after = m_region[L.seq];
                    const int seq = L.seq;
                    UndoOp op; op.name = "Heal Separation";
                    op.undo.push_back([this, seq, before]{
                        m_region[seq] = before; commit_region(seq); });
                    op.redo.push_back([this, seq, after]{
                        m_region[seq] = after; commit_region(seq); });
                    push_undo_op(std::move(op));
                }
                delete_audio_clip_undoable(R.seq);
                end_undo_compound();
                ++healed;
            } else {
                sequence* ls = m_perf->get_sequence(L.seq);
                sequence* rs = m_perf->get_sequence(R.seq);
                if (!ls || !rs) continue;
                // offsets must be continuous across the old cut
                long want = L.off + (R.on - L.on);
                if (ls->get_loop_enabled()) {
                    const long period = ls->repeat_period();
                    if (period > 0) want = ((want % period) + period) % period;
                }
                if (R.off != want) continue;              // slipped: refuse
                if (L.seq == R.seq) {
                    // same sequence: merge the two triggers
                    push_undo("Heal Separation");
                    ls->del_trigger(L.on);
                    ls->del_trigger(R.on);
                    ls->add_trigger(L.on, R.endEx - L.on, L.off, false);
                    ++healed;
                } else {
                    // the split made an independent clone: heal only when the
                    // content is still identical (same source material)
                    std::vector<sequence::EventSnapshot> le, re;
                    ls->snapshot_events(le);
                    rs->snapshot_events(re);
                    bool same = le.size() == re.size() &&
                                ls->get_length() == rs->get_length() &&
                                ls->get_loop_enabled() == rs->get_loop_enabled();
                    for (size_t k = 0; same && k < le.size(); ++k)
                        same = le[k].tick == re[k].tick && le[k].status == re[k].status &&
                               le[k].d0 == re[k].d0 && le[k].d1 == re[k].d1;
                    if (!same) continue;                  // edited: refuse
                    begin_undo_compound("Heal Separation");
                    push_undo("Heal Separation");
                    ls->del_trigger(L.on);
                    ls->add_trigger(L.on, R.endEx - L.on, L.off, false);
                    rs->del_trigger(R.on);
                    if (on_midi_clip_delete) on_midi_clip_delete(R.seq);
                    end_undo_compound();
                    ++healed;
                }
            }
            break;   // one heal per lane per invocation (spans list now stale)
        }
    }
    flash(healed ? "Healed " + std::to_string(healed) + " separation(s)"
                 : "Can't heal: clips must be adjacent, unmodified, same source");
}

//----------------------------------------------------------------------------
//  ch.31 -- TRIM command family
//----------------------------------------------------------------------------
//  Audio trims go through region geometry (with a region undo op); MIDI trims
//  re-seat the trigger.  `which`:
//   0 ToSelection | 1 StartToInsertion | 2 EndToInsertion
//   3 StartToFill | 4 EndToFill | 5 ToFillSelection
//   6 ToFileStart | 7 ToFileEnd | 8 ToFileBoundaries
void ArrangeView::trim_cmd(int which)
{
    if (!m_perf) return;
    const long a = m_sel_start, b = m_sel_end;
    const long ins = (m_sel_start >= 0) ? m_sel_start : playhead();

    // --- one audio region trim with an undo op ------------------------------
    auto trim_audio = [&](int cs, long newStart, long newEnd, const char* nm) {
        AudioRegion before = region_for(cs);
        AudioRegion r = before;
        std::map<int, long>::const_iterator lit = m_audioLen.find(cs);
        const long srcLen = (lit != m_audioLen.end() && lit->second > 0)
                          ? lit->second : r.source + r.length;
        // clamp expansion to the underlying source material
        long ns = newStart, ne = newEnd;
        const long minStart = r.position - r.source;         // source tick 0
        const long maxEnd   = r.loop ? ne                    // looped: wraps
                            : r.position + (srcLen - r.source);
        if (ns < minStart) ns = minStart;
        if (ne > maxEnd)   ne = maxEnd;
        if (ne <= ns) return;
        const long d = ns - r.position;
        r.source  += d;
        r.position = ns;
        r.length   = ne - ns;
        m_region[cs] = r;
        commit_region(cs);
        const AudioRegion after = m_region[cs];
        UndoOp op; op.name = nm;
        op.undo.push_back([this, cs, before]{ m_region[cs] = before; commit_region(cs); });
        op.redo.push_back([this, cs, after]{ m_region[cs] = after;  commit_region(cs); });
        push_undo_op(std::move(op));
    };
    // --- one MIDI trigger trim (start and/or end) ---------------------------
    auto trim_midi = [&](int cs, long on, long /*endEx*/, long newStart, long newEnd) {
        sequence* s = m_perf->get_sequence(cs);
        if (!s || newEnd <= newStart) return;
        s->unselect_triggers();
        s->select_trigger(on);
        if (newStart != on) s->move_selected_triggers_to(newStart, false, 0);
        s->move_selected_triggers_to(newEnd - 1, false, 1);
    };

    std::vector<int> lanes;
    range_lanes(lanes);
    std::vector<ClipSpan> spans;
    begin_undo_compound("Trim Clip");
    bool pushedMidi = false;
    auto need_midi_undo = [&]{ if (!pushedMidi) { push_undo("Trim Clip"); pushedMidi = true; } };

    for (int lane : lanes) {
        // the lane's spans, sorted, for neighbour limits
        struct S { int seq; long on, endEx; };
        std::vector<S> ss;
        for (int cs : lane_sequences(lane)) {
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans) ss.push_back(S{ cs, sp.on, sp.endEx });
        }
        std::sort(ss.begin(), ss.end(), [](const S& x, const S& y){ return x.on < y.on; });
        for (size_t i = 0; i < ss.size(); ++i) {
            const S& sp = ss[i];
            const bool audio = m_audio.count(sp.seq) != 0;
            const long prevEnd = i > 0 ? ss[i - 1].endEx : 0;
            const long nextOn  = i + 1 < ss.size() ? ss[i + 1].on : (long)1 << 40;
            long ns = sp.on, ne = sp.endEx;
            bool act = false;
            switch (which) {
            case 0:   // To Selection: keep only the selected span of the clip
                if (b <= a) break;
                if (sp.endEx <= a || sp.on >= b) break;
                ns = std::max(sp.on, a); ne = std::min(sp.endEx, b); act = true;
                break;
            case 1:   // Start To Insertion
                if (ins > sp.on && ins < sp.endEx) { ns = ins; act = true; }
                break;
            case 2:   // End To Insertion
                if (ins > sp.on && ins < sp.endEx) { ne = ins; act = true; }
                break;
            case 3:   // Start To Fill: expand start back over the gap
                if (b <= a) break;
                if (sp.on > a && sp.on < b && sp.on > prevEnd) {
                    ns = std::max(prevEnd, a); act = true;
                }
                break;
            case 4:   // End To Fill: expand end forward over the gap
                if (b <= a) break;
                if (sp.endEx < b && sp.endEx > a && sp.endEx < nextOn) {
                    ne = std::min(nextOn, b); act = true;
                }
                break;
            case 5:   // To Fill Selection: both edges out to the selection
                if (b <= a) break;
                if (sp.endEx <= a || sp.on >= b) break;
                ns = std::max(prevEnd, a); ne = std::min(nextOn, b); act = true;
                break;
            case 6: case 7: case 8: {   // To File Start / End / Boundaries
                if (!audio) break;                       // audio-only commands
                const AudioRegion& r = region_for(sp.seq);
                if (!r.selected) break;                  // operate on selection
                std::map<int, long>::const_iterator lit = m_audioLen.find(sp.seq);
                const long srcLen = (lit != m_audioLen.end() && lit->second > 0)
                                  ? lit->second : r.source + r.length;
                if (which != 7) ns = std::max(prevEnd, sp.on - r.source);
                if (which != 6) ne = std::min(nextOn,
                                              sp.on + (srcLen - r.source));
                act = (ns != sp.on || ne != sp.endEx);
                break;
            }
            default: break;
            }
            if (!act || (ns == sp.on && ne == sp.endEx)) continue;
            if (audio) trim_audio(sp.seq, ns, ne, "Trim Clip");
            else       { need_midi_undo(); trim_midi(sp.seq, sp.on, sp.endEx, ns, ne); }
        }
    }
    end_undo_compound();
}

//----------------------------------------------------------------------------
//  ch.31 -- NUDGING clips / trims / contents (p731-733)
//----------------------------------------------------------------------------
long ArrangeView::next_larger_nudge() const
{
    // the ladder runs BAR..1/64 with larger values at LOWER indices
    const int larger = std::max(0, m_nudge_idx - 1);
    const long v = snap_value(larger);
    return v > 0 ? v : m_measure_len;
}

//  what: 0 move clips | 1 trim start | 2 trim end | 3 slide contents.
//  Works in ANY edit mode -- no Shuffle packing, no Grid snapping, no Spot
//  dialog (the manual is explicit about all three).
void ArrangeView::nudge_clips(long delta, int what)
{
    if (!m_perf || delta == 0 || !any_selected_clip()) return;
    begin_undo_compound(what == 0 ? "Nudge" :
                        what == 3 ? "Nudge Contents" : "Nudge Trim");
    bool pushedMidi = false;
    // collect selections first: trigger edits invalidate the walk
    struct Sel { int seq; long on, endEx, off; bool audio; };
    std::vector<Sel> sel;
    for_each_clip([&](const ClipSpan& s) {
        if (s.selected)
            sel.push_back(Sel{ s.seq, s.on, s.endEx, s.offset,
                               m_audio.count(s.seq) != 0 });
    });
    for (const Sel& s : sel) {
        if (s.audio) {
            AudioRegion before = region_for(s.seq);
            AudioRegion r = before;
            switch (what) {
            case 0: r.position = std::max<long>(0, r.position + delta); break;
            case 1: {           // trim start (position + source shift)
                long d = delta;
                if (r.length - d < 1) d = r.length - 1;
                if (r.position + d < 0) d = -r.position;
                if (r.source + d < 0) d = -r.source;
                r.position += d; r.source += d; r.length -= d;
                break;
            }
            case 2:             // trim end
                r.length = std::max<long>(1, r.length + delta);
                break;
            default:            // slide contents under fixed boundaries
                r.source = std::max<long>(0, r.source + delta);
                break;
            }
            m_region[s.seq] = r;
            commit_region(s.seq);
            // ch.32 p759: nudging a crossfade contributor stretches the
            // crossfade to keep its outer points (or removes/keeps the fades
            // per Preserve Fades once the clips separate).
            if (what == 0)
                stretch_xfades_after_nudge(s.seq, before.position,
                                           before.position + before.length);
            const AudioRegion after = m_region[s.seq];
            const int cs = s.seq;
            UndoOp op; op.name = "Nudge";
            op.undo.push_back([this, cs, before]{ m_region[cs] = before; commit_region(cs); });
            op.redo.push_back([this, cs, after]{ m_region[cs] = after;  commit_region(cs); });
            push_undo_op(std::move(op));
        } else {
            sequence* q = m_perf->get_sequence(s.seq);
            if (!q) continue;
            if (!pushedMidi) { push_undo("Nudge"); pushedMidi = true; }
            switch (what) {
            case 0: {
                q->unselect_triggers();
                q->select_trigger(s.on);
                q->move_selected_triggers_to(std::max<long>(0, s.on + delta), true);
                break;
            }
            case 1: {
                const long ns = std::max<long>(0, std::min(s.endEx - 1, s.on + delta));
                q->unselect_triggers();
                q->select_trigger(s.on);
                q->move_selected_triggers_to(ns, false, 0);
                break;
            }
            case 2: {
                const long ne = std::max(s.on + 1, s.endEx + delta);
                q->unselect_triggers();
                q->select_trigger(s.on);
                q->move_selected_triggers_to(ne - 1, false, 1);
                break;
            }
            default: {          // contents: change the trigger's offset only
                q->del_trigger(s.on);
                q->add_trigger(s.on, s.endEx - s.on, s.off + delta, false);
                q->select_trigger(s.on);
                break;
            }
            }
            commit_auto_region(s.seq);
        }
    }
    end_undo_compound();
}

//----------------------------------------------------------------------------
//  ch.31 -- QUANTIZE TO GRID (Ctrl+0): snap whole selected clips' start
//  points (or sync points) to the nearest grid boundary, contents rigid.
//----------------------------------------------------------------------------
void ArrangeView::quantize_to_grid()
{
    if (!any_selected_clip()) { flash("Quantize: select clips first"); return; }
    long g = grid_ticks();
    if (g < 1) g = m_beat_len;
    struct Sel { int seq; long on; };
    std::vector<Sel> sel;
    for_each_clip([&](const ClipSpan& s) {
        if (s.selected) sel.push_back(Sel{ s.seq, s.on });
    });
    begin_undo_compound("Quantize to Grid");
    bool pushedMidi = false;
    for (const Sel& s : sel) {
        // align the SYNC POINT when the clip has one, else the start
        long anchor = s.on;
        std::map<int, long>::const_iterator sy = m_sync_point.find(s.seq);
        if (sy != m_sync_point.end()) anchor = s.on + sy->second;
        const long target = ((anchor + g / 2) / g) * g;
        const long d = target - anchor;
        if (d == 0) continue;
        if (m_audio.count(s.seq)) {
            AudioRegion before = region_for(s.seq);
            AudioRegion r = before;
            r.position = std::max<long>(0, r.position + d);
            m_region[s.seq] = r;
            commit_region(s.seq);
            const AudioRegion after = m_region[s.seq];
            const int cs = s.seq;
            UndoOp op; op.name = "Quantize to Grid";
            op.undo.push_back([this, cs, before]{ m_region[cs] = before; commit_region(cs); });
            op.redo.push_back([this, cs, after]{ m_region[cs] = after;  commit_region(cs); });
            push_undo_op(std::move(op));
        } else if (sequence* q = m_perf->get_sequence(s.seq)) {
            if (!pushedMidi) { push_undo("Quantize to Grid"); pushedMidi = true; }
            q->unselect_triggers();
            q->select_trigger(s.on);
            q->move_selected_triggers_to(std::max<long>(0, s.on + d), true);
            commit_auto_region(s.seq);
        }
    }
    end_undo_compound();
}

//----------------------------------------------------------------------------
//  ch.31 -- RATING (p739): 1..5 per clip, shown when View > Clip > Rating.
//----------------------------------------------------------------------------
void ArrangeView::rate_selected(int r)
{
    if (r < 0) r = 0;
    if (r > 5) r = 5;
    int n = 0;
    for_each_clip([&](const ClipSpan& s) {
        if (!s.selected) return;
        if (r == 0) m_rating.erase(s.seq); else m_rating[s.seq] = r;
        ++n;
    });
    if (n) flash(r ? "Rated " + std::to_string(n) + " clip(s): " + std::to_string(r)
                   : "Rating cleared");
}

//----------------------------------------------------------------------------
//  ch.31 -- CONSOLIDATE (Alt+Shift+3)
//----------------------------------------------------------------------------
void ArrangeView::consolidate_selection()
{
    if (!have_range()) {
        // No edit RANGE, but clips may be SELECTED (lasso / Shift-click /
        // plain click) -- Pro Tools p705: "select the clips you want to
        // consolidate ... Edit > Consolidate".  Per lane, exactly the selected
        // clips render into ONE new clip spanning first-start .. last-end,
        // the gaps between them preserved as silence.  This is the "stitch
        // the cut pieces back into one new clip" gesture.
        struct LaneSel { long a = 0, b = 0; std::vector<int> seqs; };
        std::map<int, LaneSel> sel;                       // lane key -> selection
        for_each_clip([&](const ClipSpan& s) {
            if (!s.selected || !m_audio.count(s.seq)) return;
            const int key = lane_key(s.seq);
            std::map<int, LaneSel>::iterator it = sel.find(key);
            if (it == sel.end()) {
                LaneSel ls; ls.a = s.on; ls.b = s.endEx; ls.seqs.push_back(s.seq);
                sel[key] = ls;
            } else {
                if (s.on < it->second.a)    it->second.a = s.on;
                if (s.endEx > it->second.b) it->second.b = s.endEx;
                it->second.seqs.push_back(s.seq);
            }
        });
        if (sel.empty()) { flash("Consolidate: make a selection"); return; }
        if (!on_consolidate_clips) { flash("Consolidate: needs shell wiring for audio"); return; }
        int done = 0;
        for (std::map<int, LaneSel>::iterator it = sel.begin(); it != sel.end(); ++it) {
            const LaneSel& ls = it->second;
            if (ls.b <= ls.a || ls.seqs.empty()) continue;
            begin_undo_compound("Consolidate");
            push_undo("Consolidate");
            on_consolidate_clips(ls.seqs, ls.a, ls.b);
            end_undo_compound();
            ++done;
        }
        if (done) flash("Consolidated");
        return;
    }
    const long a = m_sel_start, b = m_sel_end;
    std::vector<int> lanes;
    range_lanes(lanes);
    int done = 0;
    for (int lane : lanes) {
        bool audioLane = false;
        for (int cs : lane_sequences(lane))
            if (m_audio.count(cs)) { audioLane = true; break; }
        if (audioLane) {
            // separate the boundaries first so every region on the lane is
            // fully inside or fully outside the range; the shell then renders
            // the inside ones (silence included, muted clips silent) into one
            // new whole clip and removes them through the undo queue.
            if (on_consolidate) {
                begin_undo_compound("Consolidate");
                push_undo("Consolidate");
                separate_at(a, b);
                on_consolidate(lane, a, b);
                end_undo_compound();
                ++done;
            } else flash("Consolidate: needs shell wiring for audio");
            continue;
        }
        // MIDI lane: flatten every covered clip's PLAYED events into one new
        // clip spanning the range.  Looping clips are unrolled at their
        // window's period (mirrors draw_clips / sequence::play_span).
        struct Ev { long tick; unsigned char status, d0, d1; };
        std::vector<Ev> evs;
        std::vector<ClipSpan> spans;
        for (int cs : lane_sequences(lane)) {
            if (is_automation(cs)) continue;
            sequence* s = m_perf->get_sequence(cs);
            if (!s || s->get_song_mute()) continue;       // muted = silence
            const long seqLen = s->get_length();
            if (seqLen < 1) continue;
            const long lpS = s->get_loop_start(), lpE = s->get_loop_end();
            const bool lpOn = s->get_loop_enabled();
            const bool lpSet = (lpS > 0 || lpE < seqLen) && lpE > lpS;
            const long winS = (lpOn && lpSet) ? lpS : 0;
            const long winE = (lpOn && lpSet) ? lpE : seqLen;
            const long period = std::max<long>(1, winE - winS);
            std::vector<sequence::EventSnapshot> snap;
            s->snapshot_events(snap);
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans) {
                if (sp.endEx <= a || sp.on >= b) continue;
                const long offP = ((sp.offset - winS) % period + period) % period;
                for (long marker = sp.on - offP, guard = 0;
                     marker < sp.endEx && guard < 4096; marker += period, ++guard) {
                    for (const sequence::EventSnapshot& ev : snap) {
                        if (ev.tick < winS || ev.tick >= winE) continue;
                        const long t = marker + (ev.tick - winS);
                        if (t < sp.on || t >= sp.endEx) continue;
                        if (t < a || t >= b) continue;
                        evs.push_back(Ev{ t - a, ev.status, ev.d0, ev.d1 });
                    }
                    if (!lpOn) break;                     // one-shot: one pass
                }
            }
        }
        // remove the covered range, then lay the consolidated clip over it
        begin_undo_compound("Consolidate");
        push_undo("Consolidate");
        separate_at(a, b);
        for (int cs : lane_sequences(lane)) {
            if (m_audio.count(cs)) continue;
            sequence* s = m_perf->get_sequence(cs);
            if (!s) continue;
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans)
                if (sp.on >= a && sp.endEx <= b) s->del_trigger(sp.on);
            if (on_midi_clip_delete) on_midi_clip_delete(cs);
        }
        const int ns = create_pattern(lane, a, b - a, 0, false);
        if (ns >= 0) {
            if (sequence* d = m_perf->get_sequence(ns)) {
                d->set_length(b - a, false);
                for (const Ev& e : evs) d->add_event(e.tick, e.status, e.d0, e.d1);
                d->verify_and_link();
                d->set_loop_enabled(false);               // one consolidated pass
                d->set_name(std::string(d->get_name() ? d->get_name() : "clip")
                            + " cons");
                d->unselect_triggers();
                d->select_trigger(a);
            }
            ++done;
        }
        end_undo_compound();
    }
    if (done) flash("Consolidated " + std::to_string(done) + " lane(s)");
}

//----------------------------------------------------------------------------
//  ch.31 -- TCE EDIT TO TIMELINE SELECTION + FIT TO SELECTION
//----------------------------------------------------------------------------
void ArrangeView::tce_to_timeline()
{
    if (m_link_timeline) {
        flash("TCE to Timeline: unlink Timeline and Edit first (Shift+/)");
        return;
    }
    if (!have_range() || !m_perf) { flash("TCE: make an Edit selection"); return; }
    const long ea = m_sel_start, eb = m_sel_end;
    const long ta = m_perf->get_left_tick(), tb = m_perf->get_right_tick();
    if (tb <= ta) { flash("TCE: make a Timeline selection"); return; }
    if (!on_clip_tce) { flash("TCE: needs shell wiring"); return; }
    const double ratio = (double)(tb - ta) / (double)(eb - ea);
    // every audio clip fully inside the edit selection is stretched by the
    // SAME percentage (multi-track law, p738) and re-seated into the timeline
    struct Sel { int seq; long on, len; };
    std::vector<Sel> sel;
    std::vector<int> lanes;
    range_lanes(lanes);
    std::vector<ClipSpan> spans;
    for (int lane : lanes)
        for (int cs : lane_sequences(lane)) {
            if (!m_audio.count(cs)) continue;             // audio-only (manual)
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans)
                if (sp.on >= ea && sp.endEx <= eb)
                    sel.push_back(Sel{ cs, sp.on, sp.endEx - sp.on });
        }
    if (sel.empty()) { flash("TCE: no audio clips inside the selection"); return; }
    begin_undo_compound("TCE Edit to Timeline");
    for (const Sel& s : sel) {
        AudioRegion& r = region_for(s.seq);
        r.position = ta + (long)((double)(s.on - ea) * ratio + 0.5);
        commit_region(s.seq);
        const long newLen = std::max<long>(1, (long)((double)s.len * ratio + 0.5));
        on_clip_tce(s.seq, newLen);
    }
    end_undo_compound();
    set_edit_selection(ta, tb, m_sel_lo, m_sel_hi);
    flash("TCE " + std::to_string((int)(ratio * 100.0 + 0.5)) + "%");
}

//  Fit to Selection: PT drags a clip from the Clips List with Ctrl+Alt to
//  stretch it into the Edit selection.  PatchKnob has no Clips List, so the
//  command form fits the SELECTED clips into the selection: same stretch, no
//  drag (see the final report).
void ArrangeView::fit_to_selection()
{
    if (!have_range()) { flash("Fit: make an Edit selection"); return; }
    if (!on_clip_tce)  { flash("Fit: needs shell wiring"); return; }
    const long a = m_sel_start, b = m_sel_end;
    struct Sel { int seq; long on, len; };
    std::vector<Sel> sel;
    for_each_clip([&](const ClipSpan& s) {
        if (s.selected && m_audio.count(s.seq))
            sel.push_back(Sel{ s.seq, s.on, s.endEx - s.on });
    });
    if (sel.empty()) { flash("Fit: select an audio clip"); return; }
    // all clips stretch by the same percentage, keyed off the FIRST (the
    //  "last clicked" law collapses to this without a Clips List)
    const double ratio = (double)(b - a) / (double)sel[0].len;
    begin_undo_compound("Fit to Selection");
    for (const Sel& s : sel) {
        AudioRegion& r = region_for(s.seq);
        r.position = a + (long)((double)(s.on - sel[0].on) * ratio + 0.5);
        commit_region(s.seq);
        on_clip_tce(s.seq, std::max<long>(1, (long)((double)s.len * ratio + 0.5)));
    }
    end_undo_compound();
}

//----------------------------------------------------------------------------
//  ch.28 -- RENAME (Ctrl+Shift+R): rename the selected clip in place;
//  renaming an auto-created clip promotes it to user-defined.
//----------------------------------------------------------------------------
void ArrangeView::begin_clip_rename(App& app)
{
    int target = -1;
    long on = 0, endEx = 0;
    for_each_clip([&](const ClipSpan& s) {
        if (s.selected && target < 0) { target = s.seq; on = s.on; endEx = s.endEx; }
    });
    if (target < 0) { flash("Rename: select a clip first"); return; }
    SDL_Rect r;
    if (!clip_rect_span(target, on, endEx, r)) {
        // off-screen: still allow the rename over the canvas centre
        r = SDL_Rect{ canvas_x() + 40, canvas_y() + 40, 160, row_h - 6 };
    }
    m_user_named.insert(target);       // promoted to user-defined
    begin_rename(app, target, r);
}

//----------------------------------------------------------------------------
//  ch.31 -- LAYERED EDITING (p734-735)
//
//  On DROP of a moved / pasted clip: clips it FULLY covers are removed (both
//  option states); clips it PARTIALLY covers are trimmed to the overlapper
//  when the option is OFF, and left intact when it is ON -- so cutting or
//  moving the overlapper away later reveals them, exactly as figure 1 shows.
//  NOTE the engine has no z-order: while both clips exist over the same span
//  they SUM.  Layered ON therefore keeps the data (the manual's point) but an
//  overlap is audible as a mix until the overlapper moves on; OFF (the
//  default) always trims, which is also the playback-exact state.
//----------------------------------------------------------------------------
void ArrangeView::resolve_overlaps(int seq)
{
    if (!m_perf || seq < 0 || !m_perf->is_active(seq)) return;
    std::vector<ClipSpan> mine, spans;
    clip_spans(seq, mine);
    if (mine.empty()) return;
    const int lane = lane_key(seq);
    begin_undo_compound("Overlap Trim");
    bool pushedMidi = false;
    for (int cs : lane_sequences(seq)) {
        if (cs == seq || lane_key(cs) != lane) continue;
        spans.clear();
        clip_spans(cs, spans);
        for (const ClipSpan& sp : spans) {
            for (const ClipSpan& me : mine) {
                const long ovA = std::max(sp.on, me.on);
                const long ovB = std::min(sp.endEx, me.endEx);
                if (ovB <= ovA) continue;
                const bool full = sp.on >= me.on && sp.endEx <= me.endEx;
                if (full) {
                    // fully covered clips are gone -- both option states
                    if (m_audio.count(cs)) delete_audio_clip_undoable(cs);
                    else if (sequence* s = m_perf->get_sequence(cs)) {
                        if (!pushedMidi) { push_undo("Overlap"); pushedMidi = true; }
                        s->del_trigger(sp.on);
                        if (on_midi_clip_delete) on_midi_clip_delete(cs);
                    }
                    break;
                }
                if (m_layered) continue;      // ON: keep the underlying data
                // OFF: trim the covered part away from the underlying clip
                if (m_audio.count(cs)) {
                    AudioRegion before = region_for(cs);
                    AudioRegion r = before;
                    if (sp.on < me.on) {          // keep the head
                        r.length = me.on - sp.on;
                    } else {                       // keep the tail
                        const long d = me.endEx - sp.on;
                        r.position += d; r.source += d; r.length -= d;
                    }
                    if (r.length < 1) r.length = 1;
                    m_region[cs] = r;
                    commit_region(cs);
                    const AudioRegion after = m_region[cs];
                    UndoOp op; op.name = "Overlap Trim";
                    op.undo.push_back([this, cs, before]{ m_region[cs] = before; commit_region(cs); });
                    op.redo.push_back([this, cs, after]{ m_region[cs] = after;  commit_region(cs); });
                    push_undo_op(std::move(op));
                } else if (sequence* s = m_perf->get_sequence(cs)) {
                    if (!pushedMidi) { push_undo("Overlap"); pushedMidi = true; }
                    s->unselect_triggers();
                    s->select_trigger(sp.on);
                    if (sp.on < me.on)
                        s->move_selected_triggers_to(me.on - 1, false, 1);
                    else
                        s->move_selected_triggers_to(me.endEx, false, 0);
                }
                break;
            }
        }
    }
    end_undo_compound();
}

//  guts of the object delete, shared by edit_delete / edit_cut.  Mirrors the
//  old delete_selected_clips but routes audio teardowns through the queue.
void ArrangeView::edit_delete_guts()
{
    if (!m_perf) return;
    push_undo("Delete");
    struct Rip { int lane, alt; long at, len; };
    std::vector<Rip> ripples;
    if (m_edit_mode == EditMode::Shuffle)
        for_each_clip([&](const ClipSpan& cs) {
            if (!cs.selected) return;
            int alt = -1;
            for (int ls : lane_sequences(cs.seq))
                if (ls != cs.seq) { alt = ls; break; }
            ripples.push_back(Rip{ cs.seq, alt, cs.on, cs.endEx - cs.on });
        });
    std::vector<int> audioDel;
    for (int seq = 0; seq < c_max_sequence; ++seq)
        if (m_perf->is_active(seq))
            if (sequence* s = m_perf->get_sequence(seq)) {
                const bool sel = m_audio.count(seq)
                               ? region_for(seq).selected
                               : s->get_selected_trigger_start_tick() >= 0;
                if (sel && m_audio.count(seq)) audioDel.push_back(seq);
                else if (sel) {
                    while (s->get_selected_trigger_start_tick() >= 0) {
                        const long before = s->get_selected_trigger_start_tick();
                        s->del_selected_trigger();
                        if (s->get_selected_trigger_start_tick() == before) break;
                    }
                    if (on_midi_clip_delete) on_midi_clip_delete(seq);
                }
            }
    for (int seq : audioDel) delete_audio_clip_undoable(seq);
    std::sort(ripples.begin(), ripples.end(),
              [](const Rip& a, const Rip& b) { return a.at > b.at; });
    for (const Rip& r : ripples) {
        const int lane = m_perf->is_active(r.lane) ? r.lane : r.alt;
        if (lane >= 0) ripple_lane(lane, r.at, -r.len);
    }
}

//----------------------------------------------------------------------------
//  VIEW menu (View > Waveforms + View > Clip + the ch.31 options)
//----------------------------------------------------------------------------
namespace {
struct ViewRow { int id; const char* label; };
const ViewRow kViewRows[] = {
    { -1, "WAVEFORMS" },
    {  0, "Peak" }, { 1, "Power (RMS)" }, { 2, "Rectified" },
    {  3, "Outlines" }, { 4, "Overlapped Crossfades" },
    { -1, "CLIP" },
    { 10, "Name" },
    { 11, "No Time" }, { 12, "Current Time" },
    { 13, "Original Time Stamp" }, { 14, "User Time Stamp" },
    { 15, "Display on All Channels" },
    { 16, "Sync Points" }, { 17, "Overlap Shadows" },
    { 18, "Transparency" }, { 19, "Clip Overwrite Indicator" },
    { 20, "Clip Gain Info" }, { 21, "Rating" },
    { -1, "OPTIONS" },
    { 30, "Layered Editing" },
    { 31, "Auto-Name Separated Clips" },
    { 32, "Undo History window   U" },
};
const int kViewRowsN = (int)(sizeof(kViewRows) / sizeof(kViewRows[0]));
}

void ArrangeView::draw_view_menu(App& app)
{
    if (!m_view_menu) return;
    const Theme& t = theme();
    const int rowh = std::max(16, app.font.ch() + 4);
    int h = 2;
    for (int i = 0; i < kViewRowsN; ++i) h += kViewRows[i].id < 0 ? rowh : rowh;
    SDL_Rect box = ed_clamp(SDL_Rect{ m_view_menu_rect.x, m_view_menu_rect.y,
                                      210, h }, rect);
    m_view_menu_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    int y = box.y + 1;
    for (int i = 0; i < kViewRowsN; ++i) {
        const ViewRow& r = kViewRows[i];
        SDL_Rect row{ box.x + 1, y, box.w - 2, rowh };
        if (r.id < 0) {                     // section header
            fill_rect(app.ren, row, t.keybg);
            app.font.draw(app.ren, row.x + 6, row.y + 2, r.label, t.dim);
            y += rowh;
            continue;
        }
        bool on = false;
        switch (r.id) {
        case 0:  on = !m_wf_power; break;
        case 1:  on = m_wf_power; break;
        case 2:  on = m_wf_rect; break;
        case 3:  on = m_wf_outlines; break;
        case 4:  on = m_wf_overlap; break;
        case 10: on = m_clip_show_name; break;
        case 11: on = m_clip_time == 0; break;
        case 12: on = m_clip_time == 1; break;
        case 13: on = m_clip_time == 2; break;
        case 14: on = m_clip_time == 3; break;
        case 15: on = m_clip_all_chan; break;
        case 16: on = m_clip_sync; break;
        case 17: on = m_clip_shadows; break;
        case 18: on = m_clip_transp; break;
        case 19: on = m_clip_overwrite; break;
        case 20: on = m_clip_gain_info; break;
        case 21: on = m_clip_rating; break;
        case 30: on = m_layered; break;
        case 31: on = m_auto_name_sep; break;
        case 32: on = m_undo_open; break;
        default: break;
        }
        const bool hot = ed_in(row, m_mx, m_my);
        if (hot) fill_rect(app.ren, row, t.accent);
        if (on)  app.font.draw(app.ren, row.x + 5, row.y + 2, "*", hot ? t.bg : t.hi);
        app.font.draw(app.ren, row.x + 18, row.y + 2, r.label,
                      hot ? t.bg : (on ? t.hi : t.text));
        y += rowh;
    }
}

bool ArrangeView::view_menu_click(App& app, int mx, int my)
{
    const int rowh = std::max(16, app.font.ch() + 4);
    const SDL_Rect& b = m_view_menu_rect;
    if (!ed_in(b, mx, my)) {
        m_view_menu = false;
        app.request_redraw();
        return true;
    }
    const int i = (my - b.y - 1) / rowh;
    if (i >= 0 && i < kViewRowsN && kViewRows[i].id >= 0) {
        switch (kViewRows[i].id) {
        case 0:  m_wf_power = false; break;
        case 1:  m_wf_power = true; break;
        case 2:  m_wf_rect = !m_wf_rect; break;
        case 3:  m_wf_outlines = !m_wf_outlines; break;
        case 4:  m_wf_overlap = !m_wf_overlap; break;
        case 10: m_clip_show_name = !m_clip_show_name; break;
        case 11: m_clip_time = 0; break;
        case 12: m_clip_time = 1; break;
        case 13: m_clip_time = 2; break;
        case 14: m_clip_time = 3; break;
        case 15: m_clip_all_chan = !m_clip_all_chan; break;
        case 16: m_clip_sync = !m_clip_sync; break;
        case 17: m_clip_shadows = !m_clip_shadows; break;
        case 18: m_clip_transp = !m_clip_transp; break;
        case 19: m_clip_overwrite = !m_clip_overwrite; break;
        case 20: m_clip_gain_info = !m_clip_gain_info; break;
        case 21: m_clip_rating = !m_clip_rating; break;
        case 30: m_layered = !m_layered; break;
        case 31: m_auto_name_sep = !m_auto_name_sep; break;
        case 32: m_undo_open = !m_undo_open; m_view_menu = false; break;
        default: break;
        }
        ++g_wave_epoch;                   // waveform options change the cache
        app.request_redraw();
        return true;                      // stays open (several toggles a visit)
    }
    m_view_menu = false;
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  EDIT command menu (the Edit-menu commands of ch.28/31, one pop-up)
//----------------------------------------------------------------------------
namespace {
struct EditRow { int id; const char* label; };
const EditRow kEditRows[] = {
    {  0, "Cut                 ^X" },
    {  1, "Copy                ^C" },
    {  2, "Paste               ^V" },
    {  3, "Clear               ^B" },
    {  4, "Clear (all data)  ^Del" },
    { -1, "" },
    {  5, "Copy Special: Clip Gain" },
    {  6, "Paste Special: Clip Gain" },
    {  7, "Clear Special: Clip Gain" },
    {  8, "Repeat to Fill Selection" },
    { -1, "" },
    { 30, "Create Fades...     ^F" },
    { 31, "Fade To Start    Win+D" },
    { 32, "Fade To End      Win+G" },
    { 33, "Delete Fades" },
    { 34, "Batch Fades..." },
    { 35, "Fade Preferences..." },
    { -1, "" },
    {  9, "Capture Clip...     ^R" },
    { 10, "Separate At Selection ^E" },
    { 11, "Separate On Grid..." },
    { 12, "Separate At Transients..." },
    { 13, "Heal Separation     ^H" },
    { -1, "" },
    { 14, "Trim To Selection   ^T" },
    { 15, "Trim Start To Insertion" },
    { 16, "Trim End To Insertion" },
    { 17, "Trim Start To Fill" },
    { 18, "Trim End To Fill" },
    { 19, "Trim To Fill Selection" },
    { 20, "Trim To File Start" },
    { 21, "Trim To File End" },
    { 22, "Trim To File Boundaries" },
    { -1, "" },
    { 23, "Quantize To Grid    ^0" },
    { 24, "Consolidate  Alt+Sh+3" },
    { 25, "TCE Edit To Timeline Sel" },
    { 26, "Fit To Selection" },
    { 27, "Rename Clip...   ^Sh+R" },
    { 28, "Rate: 1  2  3  4  5  -" },
    { 29, "Set User Time Stamp" },
};
const int kEditRowsN = (int)(sizeof(kEditRows) / sizeof(kEditRows[0]));
}

void ArrangeView::draw_edit_menu(App& app)
{
    if (!m_edit_menu) return;
    const Theme& t = theme();
    const int rowh = std::max(15, app.font.ch() + 3);
    int h = 2;
    for (int i = 0; i < kEditRowsN; ++i) h += kEditRows[i].id < 0 ? 5 : rowh;
    SDL_Rect box = ed_clamp(SDL_Rect{ m_edit_menu_rect.x, m_edit_menu_rect.y,
                                      210, h }, rect);
    m_edit_menu_rect = box;
    fill_rect (app.ren, box, t.panel);
    frame_rect(app.ren, box, t.accent);
    int y = box.y + 1;
    for (int i = 0; i < kEditRowsN; ++i) {
        const EditRow& r = kEditRows[i];
        if (r.id < 0) {
            hline(app.ren, box.x + 4, box.x + box.w - 4, y + 2, t.dim);
            y += 5;
            continue;
        }
        SDL_Rect row{ box.x + 1, y, box.w - 2, rowh };
        if (row.y + rowh > box.y + box.h) break;
        const bool hot = ed_in(row, m_mx, m_my);
        if (hot) fill_rect(app.ren, row, t.accent);
        app.font.draw(app.ren, row.x + 8, row.y + 2,
                      ed_fit(app.font, r.label, box.w - 16),
                      hot ? t.bg : t.text);
        y += rowh;
    }
}

bool ArrangeView::edit_menu_click(App& app, int mx, int my)
{
    const int rowh = std::max(15, app.font.ch() + 3);
    const SDL_Rect& b = m_edit_menu_rect;
    m_edit_menu = false;
    if (!ed_in(b, mx, my)) { app.request_redraw(); return true; }
    int y = b.y + 1, id = -1;
    SDL_Rect hitRow{ 0, 0, 0, 0 };
    for (int i = 0; i < kEditRowsN; ++i) {
        const int rh = kEditRows[i].id < 0 ? 5 : rowh;
        if (my >= y && my < y + rh) {
            id = kEditRows[i].id;
            hitRow = SDL_Rect{ b.x, y, b.w, rh };
            break;
        }
        y += rh;
    }
    switch (id) {
    case 0: edit_cut(); break;
    case 1: edit_copy(); break;
    case 2: edit_paste(have_range() || m_sel_start >= 0 ? m_sel_start
                                                        : edit_tick()); break;
    case 3: edit_clear(false); break;
    case 4: edit_clear(true); break;
    case 5: {   // Copy Special: Clip Gain (first selected audio clip's gain)
        m_gain_clipboard = -999.f;
        for_each_clip([&](const ClipSpan& s) {
            if (s.selected && m_audio.count(s.seq) && m_gain_clipboard < -100.f)
                m_gain_clipboard = region_for(s.seq).gain;
        });
        flash(m_gain_clipboard > -100.f ? "Copied clip gain" : "No audio clip selected");
        break;
    }
    case 6: {   // Paste Special: Clip Gain (to every selected audio clip)
        if (m_gain_clipboard < -100.f) { flash("No clip gain on the clipboard"); break; }
        int n = 0;
        for_each_clip([&](const ClipSpan& s) {
            if (!s.selected || !m_audio.count(s.seq)) return;
            region_for(s.seq).gain = m_gain_clipboard;
            if (on_clip_gain) on_clip_gain(s.seq, m_gain_clipboard);
            ++n;
        });
        flash("Pasted clip gain to " + std::to_string(n) + " clip(s)");
        break;
    }
    case 7: {   // Clear Special: Clip Gain (reset to unity)
        for_each_clip([&](const ClipSpan& s) {
            if (!s.selected || !m_audio.count(s.seq)) return;
            region_for(s.seq).gain = 1.0f;
            if (on_clip_gain) on_clip_gain(s.seq, 1.0f);
        });
        flash("Clip gain cleared");
        break;
    }
    case 8:  repeat_to_fill(app); break;
    case 9:  capture_clip(app); break;
    case 10: separate_selection(0, app); break;
    case 11: separate_selection(1, app); break;
    case 12: separate_selection(2, app); break;
    case 13: heal_separation(); break;
    case 14: trim_cmd(0); break;
    case 15: trim_cmd(1); break;
    case 16: trim_cmd(2); break;
    case 17: trim_cmd(3); break;
    case 18: trim_cmd(4); break;
    case 19: trim_cmd(5); break;
    case 20: trim_cmd(6); break;
    case 21: trim_cmd(7); break;
    case 22: trim_cmd(8); break;
    case 23: quantize_to_grid(); break;
    case 30: create_fades_from_selection(app, false); break;   // ch.32 p749
    case 31: fade_to_start(app); break;
    case 32: fade_to_end(app); break;
    case 33: delete_fades_selection(); break;
    case 34: open_batch_dialog(app); break;
    case 35: m_fadepref_open = true; break;
    case 24: consolidate_selection(); break;
    case 25: tce_to_timeline(); break;
    case 26: fit_to_selection(); break;
    case 27: begin_clip_rename(app); break;
    case 29: {  // Time Stamp command: redefine the USER stamp of the selected
                // clips to their current position (ch.28 p662)
        int n = 0;
        for_each_clip([&](const ClipSpan& sp) {
            if (sp.selected) { m_user_stamp[sp.seq] = sp.on; ++n; }
        });
        flash(n ? "User time stamp set on " + std::to_string(n) + " clip(s)"
                : "Select clips to stamp");
        break;
    }
    case 28: {  // "Rate: 1 2 3 4 5 -": six equal cells after the label
        const int lx = hitRow.x + 8 + app.font.text_w("Rate: ");
        const int cw = std::max(8, (hitRow.x + hitRow.w - lx) / 6);
        int cell = (mx - lx) / cw;
        if (cell < 0) cell = 0;
        if (cell > 5) cell = 5;
        rate_selected(cell == 5 ? 0 : cell + 1);
        break;
    }
    default: break;
    }
    app.request_redraw();
    return true;
}

//----------------------------------------------------------------------------
//  ch.28 -- per-clip adornments for the View > Clip options.  Called from
//  draw_clips for both the audio and MIDI paths, after the body is painted.
//----------------------------------------------------------------------------
bool ArrangeView::waveform_forced_peak(const PatchKnob::engine::AudioClip* clip) const
{
    // Peak view is always shown DURING RECORDING (p656); the record preview
    // clip is exactly the in-flight capture.  (Sample-level zoom forces Peak
    // too -- draw_waveform tests that from its own span/width.)
    return clip && clip == m_recPreviewAudio;
}

void ArrangeView::draw_clip_adornments(App& app, int seq, const SDL_Rect& body,
                                       long tick_on, long endEx, bool audio)
{
    const Theme& t = theme();
    const Color black{ 0, 0, 0, 255 };

    // ---- clip times (p662): start/end, or a time stamp ---------------------
    if (m_clip_time != 0 && body.w > 70 && body.h > 30) {
        std::string txt;
        switch (m_clip_time) {
        case 1: txt = bbt(tick_on) + " > " + bbt(endEx); break;
        case 2: {   // Original Time Stamp: where the clip was first placed
            std::map<int, long>::const_iterator it = m_timestamp.find(seq);
            txt = "OTS " + bbt(it != m_timestamp.end() ? it->second : tick_on);
            break;
        }
        default: {  // User Time Stamp (defaults to the original)
            std::map<int, long>::const_iterator ut = m_user_stamp.find(seq);
            std::map<int, long>::const_iterator ot = m_timestamp.find(seq);
            const long v = ut != m_user_stamp.end() ? ut->second
                        : (ot != m_timestamp.end() ? ot->second : tick_on);
            txt = "UTS " + bbt(v);
            break;
        }
        }
        app.mono.draw(app.ren, body.x + 2, body.y + app.mono.ch() + 3,
                      ed_fit(app.mono, txt, body.w - 6), black);
    }

    // ---- sync point (p663): a small triangle at the bottom edge ------------
    if (m_clip_sync) {
        std::map<int, long>::const_iterator sy = m_sync_point.find(seq);
        if (sy != m_sync_point.end() &&
            sy->second >= 0 && tick_on + sy->second < endEx) {
            const int sx = tick_to_x(tick_on + sy->second);
            if (sx >= body.x && sx < body.x + body.w) {
                for (int k = 0; k < 5; ++k)
                    hline(app.ren, sx - k, sx + k,
                          body.y + body.h - 1 - (4 - k), black);
                vline(app.ren, sx, body.y + body.h - 8, body.y + body.h - 1, black);
            }
        }
    }

    // ---- clip gain info (p664): fader glyph + static dB, lower-left --------
    if (m_clip_gain_info && audio && body.w > 56 && body.h > 26) {
        float g = 1.0f;
        std::map<int, AudioRegion>::const_iterator it = m_region.find(seq);
        if (it != m_region.end()) g = it->second.gain;
        const double db = g > 0.0001f ? 20.0 * std::log10((double)g) : -144.0;
        char buf[24];
        std::snprintf(buf, sizeof buf, "%+.1fdB", db);
        const int gy = body.y + body.h - app.mono.ch() - 12;
        // tiny fader glyph: a track with a thumb at the gain height
        SDL_Rect fr{ body.x + 3, gy, 5, app.mono.ch() + 8 };
        frame_rect(app.ren, fr, black);
        const int th = fr.y + fr.h - 2 -
                       (int)((std::min(2.f, std::max(0.f, g)) / 2.f) * (fr.h - 4));
        fill_rect(app.ren, SDL_Rect{ fr.x - 1, th - 1, fr.w + 2, 3 }, black);
        app.mono.draw(app.ren, fr.x + fr.w + 3, gy + 2, buf, black);
    }

    // ---- rating (p739): "<N>" bottom-right when rated ----------------------
    if (m_clip_rating && body.w > 30) {
        std::map<int, int>::const_iterator it = m_rating.find(seq);
        if (it != m_rating.end()) {
            char rb[8];
            std::snprintf(rb, sizeof rb, "<%d>", it->second);
            app.mono.draw(app.ren,
                          body.x + body.w - app.mono.text_w(rb) - 14,
                          body.y + body.h - app.mono.ch() - 2, rb, black);
        }
    }

    // ---- overlap shadows (p663): later clips cast onto this one ------------
    if (m_clip_shadows) {
        std::vector<ClipSpan> spans;
        for (int cs : lane_sequences(seq)) {
            if (cs == seq) continue;
            spans.clear();
            clip_spans(cs, spans);
            for (const ClipSpan& sp : spans) {
                const long ovA = std::max(sp.on, tick_on);
                const long ovB = std::min(sp.endEx, endEx);
                if (ovB <= ovA) continue;
                int xa = std::max(body.x, tick_to_x(ovA));
                int xb = std::min(body.x + body.w, tick_to_x(ovB));
                if (xb <= xa) continue;
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_BLEND);
                fill_rect(app.ren, SDL_Rect{ xa, body.y, xb - xa, body.h },
                          Color{ 0, 0, 0, 70 });
                SDL_SetRenderDrawBlendMode(app.ren, SDL_BLENDMODE_NONE);
                vline(app.ren, sp.on > tick_on ? xa : xb - 1,
                      body.y, body.y + body.h, t.hi);
            }
        }
    }
    (void)t;
}

} // namespace arrange
