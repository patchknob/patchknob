//----------------------------------------------------------------------------
//  sdlui/views/plugin_picker/plugin_picker_view.h
//
//  Content widget for a floating "Choose Plugin" window: a scrollable list of
//  scanned plugins (VST2 / VST3).  Clicking one fires on_pick(descriptor).
//  Used by the patcher to set which VST a plugin/instrument node hosts.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_PLUGIN_PICKER_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_PLUGIN_PICKER_VIEW_H

#include "gui.h"
#include "engine/plugin_api.h"
#include <functional>
#include <string>
#include <vector>

namespace pickerui {

class PluginPickerView : public ui::Widget {
public:
    std::vector<PatchKnob::engine::PluginDescriptor> plugins;   // set by the shell
    bool instrumentsOnly = true;                            // filter to instruments
    std::function<void(const PatchKnob::engine::PluginDescriptor&)> on_pick;
    std::string status;                                     // e.g. "scanning..."

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;

private:
    int m_scroll = 0;
    static std::string base_name(const std::string& p);
    //! Right-hand "in>out" column; shows the BUS COUNT for multi-bus plugins so
    //! a multi-out instrument is identifiable before it is instantiated.
    static std::string io_summary(const PatchKnob::engine::PluginDescriptor& d);
    std::vector<int> visible_indices() const;   // indices into plugins[] that pass the filter
    int row_h(ui::App& app) const;
};

} // namespace pickerui

#endif