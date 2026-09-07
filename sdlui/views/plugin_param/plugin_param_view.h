//----------------------------------------------------------------------------
//  sdlui/views/plugin_param/plugin_param_view.h
//
//  A portable, SDL-drawn plugin editor: one horizontal slider per plugin
//  parameter (name + normalized value), drawn entirely with the ui:: toolkit so
//  it works INSIDE the main SDL window on any target -- including a Linux
//  KMSDRM framebuffer that has no window system to host a native VST GUI.
//  Hosted in a ui::Window.  Reads/writes the plugin via IPluginInstance's
//  paramCount / paramInfo / get|setParamNormalized.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_PLUGIN_PARAM_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_PLUGIN_PARAM_VIEW_H

#include "gui.h"

#include <functional>

namespace PatchKnob { namespace engine { class IPluginInstance; } }

namespace paramui {

class PluginParamView : public ui::Widget {
public:
    void set_instance(PatchKnob::engine::IPluginInstance* i) { m_inst = i; m_scroll = 0; m_drag = -1; }
    PatchKnob::engine::IPluginInstance* instance() const { return m_inst; }

    //! INVALIDATION.  m_inst is a NON-OWNING pointer into a plugin the engine
    //! owns, and draw() dereferences it every frame (descriptor(), paramCount(),
    //! paramInfo(), getParamNormalized()).  Nothing about closing or hiding the
    //! window clears it, so once the instance is destroyed -- the patchbay node
    //! removed, its plugin swapped, the project cleared -- the next frame reads
    //! freed memory.
    //!
    //! The HOST MUST call forget_instance(i) BEFORE destroying instance `i`,
    //! and forget_all() before tearing down every instance at once.  It clears
    //! only if `i` is the instance on show, so it is safe to call for any node
    //! being removed without first checking which one the panel holds.
    void forget_instance(const PatchKnob::engine::IPluginInstance* i) {
        if (i && m_inst == i) forget_all();
    }
    void forget_all() {
        if (!m_inst) return;
        m_inst = nullptr; m_scroll = 0; m_drag = -1;
        if (on_instance_gone) on_instance_gone();
    }
    //! Fired when the instance on show went away, so the host can hide the
    //! window rather than leave an empty "(no plugin)" panel up.  Optional:
    //! with no hook the panel simply draws empty, which is still safe.
    std::function<void()> on_instance_gone;

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;
    //! Window closed/minimised: drop any in-flight slider drag so it cannot
    //! resume against a different parameter next time the panel opens.
    void cancel_interaction(ui::App& app) override { (void)app; m_drag = -1; }

private:
    PatchKnob::engine::IPluginInstance* m_inst = nullptr;
    int m_scroll = 0;
    int m_drag   = -1;   // param index currently being dragged
    int row_h(ui::App& app) const;
    //! Height of the fixed (non-scrolling) I/O header that names the plugin's
    //! audio BUSES.  draw(), on_mouse() and on_wheel() all offset the parameter
    //! list by it, so it lives in one place rather than three.
    int header_h(ui::App& app) const;
    void draw_header(ui::App& app) const;
    void apply_from_x(int paramIndex, int mouseX);   // set param from slider x
    SDL_Rect slider_rect(int rowY, ui::App& app) const;
    bool is_nine50() const;
    bool is_limiter() const;
    SDL_Rect native_control_rect(int paramIndex) const;
};

} // namespace paramui

#endif
