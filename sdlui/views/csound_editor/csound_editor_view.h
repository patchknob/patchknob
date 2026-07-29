//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/csound_editor_view.h
//
//  A minimal multi-line text editor (ui::Widget) for a Csound .csd document.
//  Character input arrives via App::text_input_sink; navigation / edit keys and
//  Ctrl+E (recompile) arrive via on_key.  The host wires on_recompile to compile
//  the buffer and push the CSD's new in/out counts to the patcher node.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_CSOUND_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_CSOUND_EDITOR_VIEW_H

#include <functional>
#include <string>
#include <vector>

#include "../../gui.h"

namespace ui {

class CsoundEditorView : public Widget {
public:
    void draw(App& app) override;
    bool on_key(App& app, SDL_Keycode k) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;

    //! Insert literal characters at the cursor (from the App text sink).
    void insert(const char* utf8);

    void setText(const std::string& s);
    std::string text() const;
    void setStatus(const std::string& s, bool error) { status_ = s; statusError_ = error; }

    //! Fired on Ctrl+E: the host recompiles the CSD and updates node ports.
    std::function<void()> on_recompile;

private:
    void ensureVisible(int rows);
    std::vector<std::string> lines_{ std::string() };
    int cr_ = 0, cc_ = 0;     // cursor row / column (chars)
    int top_ = 0;             // first visible row
    std::string status_;
    bool statusError_ = false;
    int  blink_ = 0;
};

} // namespace ui

#endif
