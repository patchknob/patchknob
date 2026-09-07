//----------------------------------------------------------------------------
//  sdlui/views/cdp_editor/cdp_editor_view.cpp -- see cdp_editor_view.h.
//----------------------------------------------------------------------------
#include "cdp_editor_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

using namespace ui;
using PatchKnob::cdp::Buffer;
using PatchKnob::cdp::Graph;
using PatchKnob::cdp::Node;
using PatchKnob::cdp::NodeId;
using PatchKnob::cdp::Process;

namespace cdpui {

namespace {
constexpr int kPortR   = 6;
constexpr int kTitleH  = 18;
constexpr int kWaveH   = 40;
constexpr int kRowH    = 18;
constexpr int kBarH    = 26;      // bottom action strip

std::string fit(const Font& f, std::string s, int maxw) {
    if (maxw <= 0) return "";
    if (f.text_w(s) <= maxw) return s;
    while (!s.empty() && f.text_w(s + "...") > maxw) s.pop_back();
    return s.empty() ? std::string() : s + "...";
}
} // namespace

CdpEditorView::CdpEditorView() {}

void CdpEditorView::clear() {
    m_graph = Graph();
    m_peaks.clear();
    m_sourceSeq.clear();
    m_status.clear();
}

SDL_Rect CdpEditorView::node_rect(const Node& n) const {
    return SDL_Rect{
        rect.x + m_ox + (int)std::lround(n.x * m_zoom),
        rect.y + m_oy + (int)std::lround(n.y * m_zoom),
        std::max(60, (int)std::lround(n.width  * m_zoom)),
        std::max(40, (int)std::lround(n.height * m_zoom)) };
}

SDL_Rect CdpEditorView::in_port(const Node& n, int slot) const {
    const SDL_Rect b = node_rect(n);
    return SDL_Rect{ b.x - kPortR, b.y + kTitleH + 6 + slot * (kPortR * 2 + 6),
                     kPortR * 2, kPortR * 2 };
}
SDL_Rect CdpEditorView::out_port(const Node& n) const {
    const SDL_Rect b = node_rect(n);
    return SDL_Rect{ b.x + b.w - kPortR, b.y + kTitleH + 6, kPortR * 2, kPortR * 2 };
}

int CdpEditorView::node_at(int sx, int sy) const {
    // Topmost first: later nodes draw over earlier ones.
    const std::vector<Node>& ns = m_graph.nodes();
    for (int i = (int)ns.size() - 1; i >= 0; --i) {
        const SDL_Rect b = node_rect(ns[(size_t)i]);
        if (sx >= b.x && sx < b.x + b.w && sy >= b.y && sy < b.y + b.h)
            return ns[(size_t)i].id;
    }
    return 0;
}

// Render one node and reduce it to a min/max envelope for its waveform strip.
// Cached, because rendering a CDP chain is far too expensive to do per frame.
void CdpEditorView::rebuild_peaks(NodeId id) {
    Peaks& p = m_peaks[id];
    p.lo.clear(); p.hi.clear(); p.valid = false;
    Buffer b; std::string err;
    if (!m_graph.render(b, err, {}, id) || b.empty()) {
        m_status = err;
        // `valid` means "no rebuild pending", not "has data": a failed render
        // draws an empty strip and is NOT retried every frame -- before this a
        // node failing mid-chain re-rendered its (successful, expensive)
        // upstream on every redraw.  Any edit that could change the outcome
        // goes through invalidate_downstream, which re-arms the rebuild.
        p.valid = true;
        return;
    }
    const int cols = 256;
    p.lo.assign((size_t)cols, 0.f); p.hi.assign((size_t)cols, 0.f);
    const int64_t n = b.frames();
    for (int c = 0; c < cols; ++c) {
        const int64_t a = n * c / cols, z = std::max(a + 1, n * (c + 1) / cols);
        float lo = 0.f, hi = 0.f;
        for (int64_t i = a; i < z && i < n; ++i) {
            float v = 0.f;
            for (int ch = 0; ch < b.channels(); ++ch) v += b.ch[(size_t)ch][(size_t)i];
            v /= (float)std::max(1, b.channels());
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        p.lo[(size_t)c] = lo; p.hi[(size_t)c] = hi;
    }
    p.valid = true;
}

// A parameter or cable change invalidates this node and everything fed by it.
//
// Iterative with a visited set.  This used to recurse with neither, so any
// cycle in the patch -- which Graph::connect permits, it only rejects a
// self-edge -- was an unbounded recursion and an instant stack overflow the
// moment the closing cable landed.  would_cycle() below now stops such a patch
// being built at all, but the traversal must be safe on its own: a graph can
// still reach this code from a path that did not go through connect().
void CdpEditorView::invalidate_downstream(NodeId id) {
    std::vector<NodeId> stack{ id };
    std::set<NodeId>    seen{ id };
    while (!stack.empty()) {
        const NodeId cur = stack.back(); stack.pop_back();
        m_peaks[cur].valid = false;
        for (const PatchKnob::cdp::Edge& e : m_graph.edges())
            if (e.from == cur && seen.insert(e.to).second) stack.push_back(e.to);
    }
}

// True when feeding `from` into `to` would close a loop, i.e. `from` is already
// reachable downstream of `to`.  Graph::connect only rejects from == to, so a
// two-node A->B->A cycle is accepted by the model; the renderer has an explicit
// feedback-loop guard, which is the proof such graphs are reachable.  The
// editor must not build one: nothing downstream of here can render, and every
// downstream traversal becomes a cycle walk.
bool CdpEditorView::would_cycle(NodeId from, NodeId to) const {
    if (from == to) return true;
    std::vector<NodeId> stack{ to };
    std::set<NodeId>    seen{ to };
    while (!stack.empty()) {
        const NodeId cur = stack.back(); stack.pop_back();
        if (cur == from) return true;
        for (const PatchKnob::cdp::Edge& e : m_graph.edges())
            if (e.from == cur && seen.insert(e.to).second) stack.push_back(e.to);
    }
    return false;
}

void CdpEditorView::draw(App& app) {
    if (!visible) return;
    const Theme& t = theme();
    { int gx = 0, gy = 0; mouse_logical(app, gx, gy); m_mx = gx; m_my = gy; }

    fill_rect(app.ren, rect, t.bg);

    // cables
    for (const PatchKnob::cdp::Edge& e : m_graph.edges()) {
        const Node* a = m_graph.node(e.from);
        const Node* b = m_graph.node(e.to);
        if (!a || !b) continue;
        const SDL_Rect pa = out_port(*a), pb = in_port(*b, e.toInput);
        set_color(app.ren, t.accent);
        SDL_RenderDrawLine(app.ren, pa.x + kPortR, pa.y + kPortR,
                                    pb.x + kPortR, pb.y + kPortR);
        SDL_RenderDrawLine(app.ren, pa.x + kPortR, pa.y + kPortR + 1,
                                    pb.x + kPortR, pb.y + kPortR + 1);
    }
    if (m_wireFrom) {
        if (const Node* a = m_graph.node(m_wireFrom)) {
            const SDL_Rect pa = out_port(*a);
            set_color(app.ren, t.hi);
            SDL_RenderDrawLine(app.ren, pa.x + kPortR, pa.y + kPortR, m_mx, m_my);
        }
    }

    // nodes
    for (const Node& n : m_graph.nodes()) {
        const SDL_Rect b = node_rect(n);
        const bool isOut = n.id == m_graph.output();
        fill_rect (app.ren, b, t.panel);
        frame_rect(app.ren, b, isOut ? t.hi : t.dim);
        SDL_Rect title{ b.x, b.y, b.w, kTitleH };
        fill_rect(app.ren, title, n.slug.empty() ? t.keybg : t.accent);
        app.mono.draw(app.ren, b.x + 5, b.y + (kTitleH - app.mono.ch()) / 2,
                      fit(app.mono, n.label, b.w - 10),
                      n.slug.empty() ? t.text : t.bg);

        // ports
        const Process* p = n.slug.empty() ? nullptr : PatchKnob::cdp::find(n.slug);
        const int slots = p ? p->maxInputs : 0;
        for (int s = 0; s < slots; ++s) {
            const SDL_Rect q = in_port(n, s);
            fill_rect(app.ren, q, t.keybg); frame_rect(app.ren, q, t.accent);
        }
        { const SDL_Rect q = out_port(n);
          fill_rect(app.ren, q, t.keybg); frame_rect(app.ren, q, t.hi); }

        // waveform of THIS node's result
        int y = b.y + kTitleH + 4;
        if (n.showWave && b.h > kTitleH + kWaveH) {
            SDL_Rect w{ b.x + 4, y, b.w - 8, kWaveH - 6 };
            fill_rect(app.ren, w, t.bg); frame_rect(app.ren, w, t.dim);
            Peaks& pk = m_peaks[n.id];
            // A parameter drag invalidates the dragged node and everything
            // downstream on EVERY motion event; rebuilding here meant a full
            // chain render per mouse move (~30 ms for a 30 s clip, worse for
            // longer ones), which is what made dragging a knob crawl.  While
            // the drag is live the strips keep their previous (stale) envelope
            // drawn dim; the release invalidates + redraws, so one rebuild
            // happens when the gesture ends.
            const bool paramDragLive = m_paramNode != 0 && m_paramIdx >= 0;
            if (!pk.valid && !paramDragLive) rebuild_peaks(n.id);
            if (!pk.lo.empty()) {
                const int mid = w.y + w.h / 2, half = w.h / 2 - 1;
                set_color(app.ren, pk.valid ? t.note : t.dim);
                for (int c = 0; c < w.w; ++c) {
                    const size_t s = (size_t)((int64_t)c * (int64_t)pk.lo.size() / std::max(1, w.w));
                    if (s >= pk.lo.size()) break;
                    const int y0 = mid - (int)(pk.hi[s] * half);
                    const int y1 = mid - (int)(pk.lo[s] * half);
                    SDL_RenderDrawLine(app.ren, w.x + c, y0, w.x + c, y1);
                }
            }
            y += kWaveH;
        }

        // one row per parameter: name left, value right, drag to change
        if (p) for (size_t i = 0; i < p->params.size(); ++i) {
            if (y + kRowH > b.y + b.h) break;
            const PatchKnob::cdp::ParamSpec& ps = p->params[i];
            const double v = i < n.params.size() ? n.params[i] : ps.def;
            char val[32];
            std::snprintf(val, sizeof(val), ps.integer ? "%.0f" : "%.3f", v);
            app.mono.draw(app.ren, b.x + 6, y + 2,
                          fit(app.mono, ps.name, b.w / 2), t.text);
            const int vw = app.mono.text_w(val);
            app.mono.draw(app.ren, b.x + b.w - vw - 6, y + 2, val, t.accent);
            y += kRowH;
        }
    }

    // bottom action strip
    SDL_Rect bar{ rect.x, rect.y + rect.h - kBarH, rect.w, kBarH };
    fill_rect(app.ren, bar, t.panel);
    hline(app.ren, bar.x, bar.x + bar.w, bar.y, t.dim);
    m_renderBtn  = SDL_Rect{ bar.x + 6, bar.y + 4, 118, kBarH - 8 };
    m_previewBtn = SDL_Rect{ bar.x + 130, bar.y + 4, 90, kBarH - 8 };
    auto btn = [&](const SDL_Rect& q, const char* label) {
        const bool hot = m_mx >= q.x && m_mx < q.x + q.w &&
                         m_my >= q.y && m_my < q.y + q.h;
        fill_rect (app.ren, q, hot ? t.accent : t.bg);
        frame_rect(app.ren, q, t.dim);
        app.mono.draw_centered(app.ren, q, label, hot ? t.bg : t.text);
    };
    btn(m_renderBtn,  "RENDER TO TRACK");
    btn(m_previewBtn, "PREVIEW");

    // Latency + streamability readout: the spectral processes cannot preview
    // without an analysis-window delay, and some cannot stream at all.
    {
        char info[160];
        const int64_t lat = m_graph.latencyFrames(48000);
        std::snprintf(info, sizeof(info), "%s  latency %.1f ms   %s",
                      m_graph.canStream() ? "streamable" : "offline only",
                      1000.0 * (double)lat / 48000.0,
                      m_status.empty() ? "" : m_status.c_str());
        app.mono.draw(app.ren, m_previewBtn.x + m_previewBtn.w + 12,
                      bar.y + (kBarH - app.mono.ch()) / 2, info, t.dim);
    }

    if (m_graph.nodes().empty()) {
        static const char* hint =
            "drag clips here from the timeline   -   right-click to add a CDP process";
        const int w = app.mono.text_w(hint);
        app.mono.draw(app.ren, rect.x + (rect.w - w) / 2,
                      rect.y + rect.h / 2, hint, t.dim);
    }

    frame_rect(app.ren, rect, t.dim);
    draw_palette(app);
}

void CdpEditorView::draw_palette(App& app) {
    if (!m_palette) return;
    const Theme& t = theme();
    const std::vector<Process>& reg = PatchKnob::cdp::registry();
    const int rowh = app.mono.ch() + 6;
    const int rows = std::min((int)reg.size(), std::max(1, (rect.h - 40) / rowh));
    SDL_Rect box{ m_paletteRect.x, m_paletteRect.y, 240, rows * rowh + 4 };
    if (box.x + box.w > rect.x + rect.w) box.x = rect.x + rect.w - box.w;
    if (box.y + box.h > rect.y + rect.h) box.y = rect.y + rect.h - box.h;
    if (box.x < rect.x) box.x = rect.x;
    if (box.y < rect.y) box.y = rect.y;
    m_paletteRect = box;
    fill_rect(app.ren, box, t.panel); frame_rect(app.ren, box, t.accent);
    if (m_paletteScroll > (int)reg.size() - rows) m_paletteScroll = (int)reg.size() - rows;
    if (m_paletteScroll < 0) m_paletteScroll = 0;
    for (int i = 0; i < rows; ++i) {
        const size_t idx = (size_t)(m_paletteScroll + i);
        if (idx >= reg.size()) break;
        SDL_Rect r{ box.x + 1, box.y + 2 + i * rowh, box.w - 2, rowh };
        const bool hot = m_mx >= r.x && m_mx < r.x + r.w &&
                         m_my >= r.y && m_my < r.y + r.h;
        if (hot) fill_rect(app.ren, r, t.accent);
        app.mono.draw(app.ren, r.x + 6, r.y + 3,
                      fit(app.mono, reg[idx].group + " / " + reg[idx].name, r.w - 12),
                      hot ? t.bg : t.text);
    }
}

bool CdpEditorView::palette_click(App& app, int mx, int my) {
    const std::vector<Process>& reg = PatchKnob::cdp::registry();
    const int rowh = app.mono.ch() + 6;
    if (mx >= m_paletteRect.x && mx < m_paletteRect.x + m_paletteRect.w &&
        my >= m_paletteRect.y + 2) {
        const size_t idx = (size_t)(m_paletteScroll + (my - m_paletteRect.y - 2) / rowh);
        if (idx < reg.size()) {
            const NodeId id = m_graph.addProcess(reg[idx].slug);
            if (Node* n = m_graph.node(id)) { n->x = m_dropX; n->y = m_dropY; }
            m_status.clear();
        }
    }
    m_palette = false;
    app.request_redraw();
    return true;
}

int CdpEditorView::accept_clip(const Buffer& audio, const std::string& label,
                               int seq, float dropX, float dropY) {
    const NodeId id = m_graph.addSource(audio, label);
    if (Node* n = m_graph.node(id)) {
        n->x = dropX; n->y = dropY;
        n->height = (double)(kTitleH + kWaveH + 8);
    }
    m_sourceSeq[id] = seq;
    m_peaks[id].valid = false;
    return id;
}

void CdpEditorView::do_render(bool previewOnly) {
    Buffer out; std::string err;
    if (!m_graph.render(out, err)) { m_status = err; return; }
    m_status = "rendered " + std::to_string((long long)out.frames()) + " frames";
    // Hand back the sequence the FIRST source came from, so the host knows which
    // lane the result belongs to.
    int seq = -1;
    for (const auto& kv : m_sourceSeq) { seq = kv.second; break; }
    if (previewOnly) { if (on_preview) on_preview(out); }
    else             { if (on_render)  on_render(out, seq); }
}

bool CdpEditorView::on_mouse(App& app, const MouseEv& e) {
    m_mx = e.x; m_my = e.y;
    if (!e.pressed) {
        if (m_wireFrom) {
            // complete a cable onto whatever input slot is under the pointer
            for (const Node& n : m_graph.nodes()) {
                const Process* p = n.slug.empty() ? nullptr : PatchKnob::cdp::find(n.slug);
                for (int s = 0; p && s < p->maxInputs; ++s) {
                    const SDL_Rect q = in_port(n, s);
                    if (e.x >= q.x && e.x < q.x + q.w && e.y >= q.y && e.y < q.y + q.h) {
                        // Reject the cable that would close a loop rather than
                        // building an ungraphable patch (and, before the
                        // traversal fix below, crashing outright).
                        if (would_cycle(m_wireFrom, n.id))
                            m_status = "cable refused: that would make a feedback loop";
                        else if (m_graph.connect(m_wireFrom, n.id, s))
                            { m_status.clear(); invalidate_downstream(n.id); }
                        break;
                    }
                }
            }
        }
        m_wireFrom = 0; m_dragNode = 0; m_panning = false;
        m_paramNode = 0; m_paramIdx = -1;
        app.request_redraw();
        return true;
    }
    if (m_palette) return palette_click(app, e.x, e.y);

    // drag in progress
    if (m_dragNode) {
        if (Node* n = m_graph.node(m_dragNode)) {
            n->x = (double)(e.x - rect.x - m_ox - m_dragDx) / m_zoom;
            n->y = (double)(e.y - rect.y - m_oy - m_dragDy) / m_zoom;
        }
        app.request_redraw(); return true;
    }
    if (m_panning) {
        m_ox += e.x - m_panX; m_oy += e.y - m_panY;
        m_panX = e.x; m_panY = e.y; app.request_redraw(); return true;
    }
    if (m_paramNode && m_paramIdx >= 0) {
        Node* n = m_graph.node(m_paramNode);
        const Process* p = n ? PatchKnob::cdp::find(n->slug) : nullptr;
        if (n && p && m_paramIdx < (int)p->params.size()) {
            const PatchKnob::cdp::ParamSpec& ps = p->params[(size_t)m_paramIdx];
            const double span = ps.max - ps.min;
            double v = m_param0 + (double)(m_paramY0 - e.y) * span / 160.0;
            v = std::max(ps.min, std::min(ps.max, v));
            if (ps.integer) v = std::floor(v + 0.5);
            n->params[(size_t)m_paramIdx] = v;
            invalidate_downstream(n->id);
        }
        app.request_redraw(); return true;
    }
    if (m_wireFrom) { app.request_redraw(); return true; }

    // fresh press
    if (e.button == SDL_BUTTON_LEFT) {
        if (e.x >= m_renderBtn.x && e.x < m_renderBtn.x + m_renderBtn.w &&
            e.y >= m_renderBtn.y && e.y < m_renderBtn.y + m_renderBtn.h) {
            do_render(false); app.request_redraw(); return true;
        }
        if (e.x >= m_previewBtn.x && e.x < m_previewBtn.x + m_previewBtn.w &&
            e.y >= m_previewBtn.y && e.y < m_previewBtn.y + m_previewBtn.h) {
            do_render(true); app.request_redraw(); return true;
        }
        // output port -> start a cable
        for (const Node& n : m_graph.nodes()) {
            const SDL_Rect q = out_port(n);
            if (e.x >= q.x && e.x < q.x + q.w && e.y >= q.y && e.y < q.y + q.h) {
                m_wireFrom = n.id; app.request_redraw(); return true;
            }
        }
        // input port -> cut whatever feeds it
        for (const Node& n : m_graph.nodes()) {
            const Process* p = n.slug.empty() ? nullptr : PatchKnob::cdp::find(n.slug);
            for (int s = 0; p && s < p->maxInputs; ++s) {
                const SDL_Rect q = in_port(n, s);
                if (e.x >= q.x && e.x < q.x + q.w && e.y >= q.y && e.y < q.y + q.h) {
                    m_graph.disconnect(n.id, s); invalidate_downstream(n.id);
                    app.request_redraw(); return true;
                }
            }
        }
        const int id = node_at(e.x, e.y);
        if (id) {
            Node* n = m_graph.node(id);
            const SDL_Rect b = node_rect(*n);
            const Process* p = n->slug.empty() ? nullptr : PatchKnob::cdp::find(n->slug);
            // a parameter row -> vertical drag edits it
            int rowTop = b.y + kTitleH + 4 + (n->showWave ? kWaveH : 0);
            if (p) for (size_t i = 0; i < p->params.size(); ++i) {
                if (e.y >= rowTop && e.y < rowTop + kRowH) {
                    m_paramNode = id; m_paramIdx = (int)i;
                    m_paramY0 = e.y; m_param0 = n->params[i];
                    return true;
                }
                rowTop += kRowH;
            }
            m_graph.setOutput(id);                 // clicking a node targets it
            m_dragNode = id;
            m_dragDx = e.x - b.x; m_dragDy = e.y - b.y;
            app.request_redraw(); return true;
        }
        m_panning = true; m_panX = e.x; m_panY = e.y;
        return true;
    }
    if (e.button == SDL_BUTTON_RIGHT) {
        const int id = node_at(e.x, e.y);
        // Invalidate BEFORE removal, while the edges to walk still exist --
        // the fed nodes must re-render (and now show their failure) rather
        // than keep drawing the result of an input that is gone.
        if (id) { invalidate_downstream(id); m_graph.removeNode(id); m_peaks.erase(id); m_sourceSeq.erase(id); }
        else {
            m_palette = true; m_paletteRect = SDL_Rect{ e.x, e.y, 240, 0 };
            m_dropX = (double)(e.x - rect.x - m_ox) / m_zoom;
            m_dropY = (double)(e.y - rect.y - m_oy) / m_zoom;
        }
        app.request_redraw(); return true;
    }
    return true;
}

bool CdpEditorView::on_wheel(App& app, int dx, int dy) {
    (void)dx;
    if (m_palette) { m_paletteScroll -= dy; app.request_redraw(); return true; }
    const float z = m_zoom * (dy > 0 ? 1.1f : 1.f / 1.1f);
    m_zoom = std::max(0.35f, std::min(2.5f, z));
    app.request_redraw();
    return true;
}

bool CdpEditorView::on_key(App& app, SDL_Keycode k) {
    if (m_palette && k == SDLK_ESCAPE) { m_palette = false; app.request_redraw(); return true; }
    if (k == SDLK_DELETE && m_graph.output()) {
        const NodeId id = m_graph.output();
        invalidate_downstream(id);   // while its edges still exist (see on_mouse)
        m_graph.removeNode(id); m_peaks.erase(id); m_sourceSeq.erase(id);
        app.request_redraw(); return true;
    }
    return false;
}

} // namespace cdpui
