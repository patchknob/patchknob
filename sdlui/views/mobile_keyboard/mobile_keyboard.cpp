#include "mobile_keyboard.h"
#include <algorithm>

namespace ui {

namespace {
constexpr int HANDLE_H = 26;
constexpr int TAB_W = 112;
constexpr int TAB_H = 32;

bool black_pc(int pc) {
    return pc==1 || pc==3 || pc==6 || pc==8 || pc==10;
}
}

void MobileKeyboard::layout(const SDL_Rect& bounds, int height) {
    bounds_ = bounds;
    height = std::clamp(height, 82, std::max(82, bounds.h));
    const int width = std::max(480, bounds.w * 9 / 10);
    if (!initialized_) {
        openRect_ = {bounds.x + (bounds.w - width) / 2,
                     bounds.y + bounds.h - height, width, height};
        initialized_ = true;
    } else {
        openRect_.w = std::min(width, bounds.w);
        openRect_.h = height;
        openRect_.x = std::clamp(openRect_.x, bounds.x,
                                bounds.x + std::max(0, bounds.w-openRect_.w));
        openRect_.y = std::clamp(openRect_.y, bounds.y,
                                bounds.y + std::max(0, bounds.h-openRect_.h));
    }
    rect = hidden_ ? SDL_Rect{bounds.x+bounds.w-TAB_W-8,
                              bounds.y+bounds.h-TAB_H-8,TAB_W,TAB_H}
                   : openRect_;
}

SDL_Rect MobileKeyboard::keyboard_rect() const {
    return {openRect_.x, openRect_.y + HANDLE_H,
            openRect_.w, std::max(1, openRect_.h - HANDLE_H)};
}

int MobileKeyboard::note_at(int x, int y) const {
    const SDL_Rect keys = keyboard_rect();
    if (x<keys.x || x>=keys.x+keys.w || y<keys.y || y>=keys.y+keys.h) return -1;
    const int controls=std::min(96,std::max(64,keys.w/8));
    if(x<keys.x+controls) return -1;
    const int kw=std::max(1,keys.w-controls);
    const int key = std::clamp((x - keys.x-controls) * 24 / kw, 0, 23);
    return std::clamp((octave_ + 1) * 12 + key, 0, 127);
}

void MobileKeyboard::release() {
    if (active_ >= 0 && on_note_off) on_note_off(active_);
    active_ = -1;
}

bool MobileKeyboard::on_mouse(App& app, const MouseEv& e) {
    if (!visible) return false;
    if (hidden_) {
        if (e.pressed && e.button==SDL_BUTTON_LEFT) {
            hidden_ = false;
            rect = openRect_;
            app.request_redraw();
        }
        return true;
    }
    if (!e.pressed) {
        release();
        dragging_ = false;
        app.request_redraw();
        return true;
    }
    SDL_Rect hide{openRect_.x+openRect_.w-72,openRect_.y+2,68,HANDLE_H-4};
    if (!dragging_ && e.x>=hide.x && e.x<hide.x+hide.w &&
        e.y>=hide.y && e.y<hide.y+hide.h) {
        release(); dragging_=false; hidden_=true;
        rect={bounds_.x+bounds_.w-TAB_W-8,bounds_.y+bounds_.h-TAB_H-8,TAB_W,TAB_H};
        app.request_redraw(); return true;
    }
    if (dragging_) {
        openRect_.x=std::clamp(e.x-dragDx_,bounds_.x,
                              bounds_.x+std::max(0,bounds_.w-openRect_.w));
        openRect_.y=std::clamp(e.y-dragDy_,bounds_.y,
                              bounds_.y+std::max(0,bounds_.h-openRect_.h));
        rect=openRect_; app.request_redraw(); return true;
    }
    if (e.y < openRect_.y + HANDLE_H) {
        release(); dragging_=true;
        dragDx_=e.x-openRect_.x; dragDy_=e.y-openRect_.y;
        return true;
    }
    const SDL_Rect keys=keyboard_rect();
    const int controls=std::min(96,std::max(64,keys.w/8));
    if(e.x<keys.x+controls) {
        release();
        octave_ += e.y < keys.y+keys.h/2 ? 1 : -1;
        octave_=std::clamp(octave_,0,8);
        app.request_redraw();
        return true;
    }
    const int n = note_at(e.x,e.y);
    if (n == active_) return true;
    release();
    if (n >= 0) {
        active_ = n;
        // Vertical position supplies useful touch velocity: harder/lower = louder.
        const int rel = std::clamp(e.y - keys.y, 0, std::max(1,keys.h-1));
        const int velocity = std::clamp(45 + rel * 82 / std::max(1,keys.h-1), 1, 127);
        if (on_note_on) on_note_on(n, velocity);
    }
    app.request_redraw();
    return true;
}

void MobileKeyboard::draw(App& app) {
    if (!visible) return;
    const Theme& t=theme();
    if (hidden_) {
        fill_rect(app.ren,rect,t.accent); frame_rect(app.ren,rect,t.hi);
        app.mono.draw_centered(app.ren,rect,"SHOW KEYS",t.keybg);
        return;
    }
    fill_rect(app.ren,rect,t.panel);
    SDL_Rect handle{rect.x,rect.y,rect.w,HANDLE_H};
    fill_rect(app.ren,handle,t.panel); frame_rect(app.ren,handle,t.dim);
    app.mono.draw(app.ren,handle.x+8,handle.y+5,"KEYBOARD  |  DRAG",t.dim);
    SDL_Rect hide{rect.x+rect.w-72,rect.y+2,68,HANDLE_H-4};
    fill_rect(app.ren,hide,t.keybg); frame_rect(app.ren,hide,t.accent);
    app.mono.draw_centered(app.ren,hide,"HIDE",t.text);
    const SDL_Rect keys=keyboard_rect();
    const int controls=std::min(96,std::max(64,keys.w/8));
    SDL_Rect up{keys.x,keys.y,controls,keys.h/2};
    SDL_Rect down{keys.x,keys.y+keys.h/2,controls,keys.h-keys.h/2};
    fill_rect(app.ren,up,t.keybg); fill_rect(app.ren,down,t.keybg);
    frame_rect(app.ren,up,t.accent); frame_rect(app.ren,down,t.accent);
    app.font.draw_centered(app.ren,up,"OCT +",t.text);
    app.font.draw_centered(app.ren,down,"OCT -",t.text);
    static const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const int kw=std::max(1,keys.w-controls);
    for(int i=0;i<24;++i) {
        SDL_Rect k{keys.x+controls+i*kw/24,keys.y,
                   (i+1)*kw/24-i*kw/24,keys.h};
        const int note=(octave_+1)*12+i;
        fill_rect(app.ren,k,black_pc(note%12) ? Color{35,38,45,255}
                                              : Color{220,222,218,255});
        frame_rect(app.ren,k,note==active_ ? t.accent : t.dim);
        const std::string label=std::string(names[note%12])+std::to_string(note/12-1);
        app.mono.draw_centered(app.ren,
            SDL_Rect{k.x,k.y+k.h-18,k.w,14},label,
            black_pc(note%12) ? t.text : Color{20,22,24,255});
    }
}

} // namespace ui
