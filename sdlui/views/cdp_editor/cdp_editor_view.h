//----------------------------------------------------------------------------
//  sdlui/views/cdp_editor/cdp_editor_view.h
//
//  The offline CDP editor: a patcher for sound transformation.
//
//  It is a node graph like the patchbay, but the nodes are whole-buffer CDP
//  processes rather than realtime DSP, so it differs where that matters:
//
//    * nodes have INDIVIDUAL heights -- a waveform strip plus one row per
//      parameter the process declares -- instead of the rack's fixed grid,
//    * every node carries a waveform view of its own result, so you can see
//      what each stage did rather than only the end of the chain,
//    * evaluation is a pull over complete buffers (PatchKnob::cdp::Graph), so a
//      process that cannot stream is perfectly usable here,
//    * the chain reports its accumulated latency, since the spectral processes
//      have an unavoidable analysis-window delay.
//
//  Clips arrive by drag from the timeline (accept_clip); a MIDI clip is frozen
//  to audio by the host first, which is what on_need_freeze asks for.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_CDP_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_CDP_EDITOR_VIEW_H

#include "gui.h"
#include "cdp_graph.h"

#include <functional>
#include <string>
#include <vector>

namespace cdpui {

class CdpEditorView : public ui::Widget {
public:
    CdpEditorView();

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    bool on_key(ui::App& app, SDL_Keycode k) override;

    //! Drop a clip in as a source node.  `seq` is the timeline sequence it came
    //! from, so the host can map a render back onto it.
    int  accept_clip(const PatchKnob::cdp::Buffer& audio, const std::string& label,
                     int seq, float dropX, float dropY);
    //! A MIDI clip was dragged in: the host freezes it and calls accept_clip.
    std::function<void(int seq)> on_need_freeze;
    //! "Render to track" -- hand the finished buffer back to the host.
    std::function<void(const PatchKnob::cdp::Buffer& out, int sourceSeq)> on_render;
    //! Audition a buffer (the preview button).
    std::function<void(const PatchKnob::cdp::Buffer& out)> on_preview;

    void clear();

private:
    PatchKnob::cdp::Graph m_graph;
    // Cached per-node peak envelopes so the waveform strips do not re-render the
    // whole chain every frame.
    struct Peaks { std::vector<float> lo, hi; bool valid = false; };
    std::map<PatchKnob::cdp::NodeId, Peaks> m_peaks;
    std::map<PatchKnob::cdp::NodeId, int>   m_sourceSeq;

    float m_zoom = 1.0f;
    int   m_ox = 0, m_oy = 0;
    int   m_mx = 0, m_my = 0;

    int  m_dragNode = 0; int m_dragDx = 0, m_dragDy = 0;
    bool m_panning = false; int m_panX = 0, m_panY = 0;
    int  m_wireFrom = 0;                 // node whose output is being dragged
    int  m_paramNode = 0, m_paramIdx = -1; int m_paramY0 = 0; double m_param0 = 0.0;

    bool     m_palette = false;
    SDL_Rect m_paletteRect { 0, 0, 0, 0 };
    int      m_paletteScroll = 0;
    double   m_dropX = 0.0, m_dropY = 0.0;

    std::string m_status;
    SDL_Rect m_renderBtn { 0,0,0,0 }, m_previewBtn { 0,0,0,0 };

    SDL_Rect node_rect(const PatchKnob::cdp::Node& n) const;
    SDL_Rect in_port (const PatchKnob::cdp::Node& n, int slot) const;
    SDL_Rect out_port(const PatchKnob::cdp::Node& n) const;
    int      node_at(int sx, int sy) const;
    void     rebuild_peaks(PatchKnob::cdp::NodeId id);
    void     invalidate_downstream(PatchKnob::cdp::NodeId id);
    //! Would connecting from -> to close a feedback loop?  Graph::connect
    //! only rejects self-edges, so the editor gates longer cycles here.
    bool     would_cycle(PatchKnob::cdp::NodeId from,
                         PatchKnob::cdp::NodeId to) const;
    void     draw_palette(ui::App& app);
    bool     palette_click(ui::App& app, int mx, int my);
    void     do_render(bool previewOnly);
};

} // namespace cdpui
#endif
