//----------------------------------------------------------------------------
//  sdlui/views/mixer_node/mixer_node_view.h
//
//  SDL channel-strip view for a patchable MixerNode: per-channel fader (gain),
//  mute, and a peak VU meter, plus [-]/[+] to change the channel count.  Hosted
//  in a ui::Window (opened from the patcher's Mixer node).  Drives the engine
//  through audio_app_mixer_*.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_MIXER_NODE_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_MIXER_NODE_VIEW_H

#include "gui.h"
#include <functional>

namespace mixnode {

class MixerNodeView : public ui::Widget {
public:
    void set_node(int n) { m_node = n; m_drag = -1; }
    int  node() const { return m_node; }

    // Fired after the channel count changes so the shell can re-sync the patch
    // node's ports and drop wires to removed channels.
    std::function<void(int node)> on_changed;

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;

    static const int STRIP_W = 48;

private:
    int m_node = -1;
    int m_drag = -1;                 // channel whose fader is being dragged
};

} // namespace mixnode

#endif