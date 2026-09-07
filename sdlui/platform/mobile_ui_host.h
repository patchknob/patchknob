#pragma once
#include <functional>

namespace ui { class App; class Widget; }

namespace ui { namespace platform {
class MobileUiHost {
public:
    MobileUiHost(); ~MobileUiHost();
    MobileUiHost(const MobileUiHost&)=delete;
    MobileUiHost& operator=(const MobileUiHost&)=delete;
    Widget* overlay();
    void set_note_callbacks(std::function<void(int,int)> on,
                            std::function<void(int)> off);
    void layout(App& app,int menuHeight);
private:
    struct Impl; Impl* impl_=nullptr;
};
} }
