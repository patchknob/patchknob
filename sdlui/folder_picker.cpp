#include "folder_picker.h"
#include "views/file_dialog/file_dialog.h"

namespace ui {

bool choose_folder(App& app, const std::string& current, std::string& selected) {
    FileDialog::Options opt;
    opt.foldersOnly = true;
    opt.title = "Choose a Folder";
    opt.startDir = current;
    std::string result;
    if (!choose_path(app, opt, result)) return false;
    selected = result;
    return true;
}

} // namespace ui
