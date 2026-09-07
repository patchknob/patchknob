#pragma once

#include "../../gui.h"
#include <functional>

namespace ui {

// Android-only overlay controller. It is compiled everywhere so project files
// stays simple; the Android MobileUiHost owns its visibility and lifecycle.
class MobileKeyboard : public Widget {
public:
    std::function<void(int note, int velocity)> on_note_on;
    std::function<void(int note)> on_note_off;

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    void release();
    void layout(const SDL_Rect& bounds, int height);

private:
    int note_at(int x, int y) const;
    SDL_Rect keyboard_rect() const;
    int active_ = -1;
    int octave_ = 4;
    SDL_Rect bounds_{0,0,0,0};
    SDL_Rect openRect_{0,0,0,0};
    bool initialized_ = false;
    bool hidden_ = false;
    bool dragging_ = false;
    int dragDx_ = 0, dragDy_ = 0;
};

} // namespace ui
