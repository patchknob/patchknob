//----------------------------------------------------------------------------
//  sdlui/views/video_settings/video_settings_view.h
//
//  Settings for the screen recorder: codec, capture height, frame rate, encoder
//  threads, whether to mux the mixer output, and where takes are written.
//
//  The codec list carries its measured cost, because the right choice here is
//  not obvious and depends on the editor: x264 RGB is pixel-exact and ~2.8 GB/hr
//  and imports natively into MLT/ffmpeg tools (Kdenlive, Shotcut), while Ut Video
//  is 14x larger but goes into essentially any NLE.  Showing the numbers next to
//  the names means the choice can be made without guessing.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_VIDEO_SETTINGS_VIEW_H
#define PATCHKNOB_SDLUI_VIDEO_SETTINGS_VIEW_H

#include "gui.h"
#include "screen_recorder.h"

#include <functional>
#include <string>

namespace ui {

class VideoSettingsView : public Widget {
public:
    //! The settings object the shell owns; edited in place.
    pkrec::Settings* cfg = nullptr;

    //! Ask the shell to run a folder picker (it owns the dialogs).
    std::function<bool(std::string& pathInOut)> on_pick_folder;
    //! Live state for the status line.
    std::function<bool()>        recording;
    std::function<std::string()> ffmpeg_path;

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;

private:
    struct Row { SDL_Rect r; int id; };
    Row  m_rows[8];
    int  m_nrows = 0;
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_VIDEO_SETTINGS_VIEW_H
