//----------------------------------------------------------------------------
//  sdlui/views/audio_settings/audio_settings_view.h
//
//  Content widget for the floating "Audio Settings" window.  Lists, as
//  clickable rows: the backend / driver TYPE (ASIO / WASAPI / WDM-KS /
//  DirectSound / MME ...), the OUTPUT device, the INPUT device, and the BUFFER
//  size.  Selecting a row applies it via audio_app_* (which stops, reopens and
//  re-prepares the engine) and refreshes.  Strictly two-tone.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIEWS_AUDIO_SETTINGS_VIEW_H
#define PATCHKNOB_SDLUI_VIEWS_AUDIO_SETTINGS_VIEW_H

#include "gui.h"
#include <string>
#include <vector>

namespace audioui {

class AudioSettingsView : public ui::Widget {
public:
    bool audio_ok = true;

    void draw(ui::App& app) override;
    bool on_mouse(ui::App& app, const ui::MouseEv& e) override;
    bool on_wheel(ui::App& app, int dx, int dy) override;

private:
    enum Kind { HEADER=0, BACKEND=1, OUTDEV=2, INDEV=3, BUFFER=4 };
    struct Row { Kind kind; int ival; unsigned uval; std::string label; bool current; };
    std::vector<Row> build() const;
    int row_h(ui::App& app) const;
    int m_scroll = 0;
};

} // namespace audioui

#endif