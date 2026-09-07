#include "mobile_ui_host.h"
#include "../gui.h"
#include "../views/mobile_keyboard/mobile_keyboard.h"
#include <algorithm>
namespace ui { namespace platform {
struct MobileUiHost::Impl{MobileKeyboard keyboard;};
MobileUiHost::MobileUiHost():impl_(new Impl){impl_->keyboard.visible=true;}
MobileUiHost::~MobileUiHost(){delete impl_;}
Widget* MobileUiHost::overlay(){return &impl_->keyboard;}
void MobileUiHost::set_note_callbacks(std::function<void(int,int)> on,std::function<void(int)> off){
    impl_->keyboard.on_note_on=std::move(on);impl_->keyboard.on_note_off=std::move(off);
}
void MobileUiHost::layout(App& app,int menuHeight){
    const int kh=std::max(84,(app.h/5)*3/4);
    impl_->keyboard.layout(SDL_Rect{0,menuHeight,app.w,app.h-menuHeight},kh);
}
} }
