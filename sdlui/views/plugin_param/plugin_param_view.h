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

namespace PatchKnob { namespace engine { class IPluginInstance; } }

namespace paramui {

class PluginParamView : public ui::Widget {
public:
    void set_instance(PatchKnob::engine::IPluginInstance* i) { m_inst = i; m_scroll = 0; m_drag = -1; }
    PatchKnob::engine::IPluginInstance* instance() const { return m_inst; }

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;

private:
    PatchKnob::engine::IPluginInstance* m_inst = nullptr;
    int m_scroll = 0;
    int m_drag   = -1;   // param index currently being dragged
    int row_h(ui::App& app) const;
    void apply_from_x(int paramIndex, int mouseX);   // set param from slider x
    SDL_Rect slider_rect(int rowY, ui::App& app) const;
};

} // namespace paramui

#endif