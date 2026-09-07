#include "mobile_ui_host.h"
namespace ui { namespace platform {
struct MobileUiHost::Impl{};
MobileUiHost::MobileUiHost():impl_(new Impl){}
MobileUiHost::~MobileUiHost(){delete impl_;}
Widget* MobileUiHost::overlay(){return nullptr;}
void MobileUiHost::set_note_callbacks(std::function<void(int,int)>,std::function<void(int)>){ }
void MobileUiHost::layout(App&,int){ }
} }
