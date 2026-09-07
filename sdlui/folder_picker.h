//----------------------------------------------------------------------------
//  sdlui/folder_picker.h
//
//  A directory-chooser dialog. Thin wrapper over views/file_dialog's in-app
//  SDL picker (folders-only mode) -- kept as its own header/TU because every
//  call site already includes it and it is the natural single place to widen
//  the contract later (e.g. remembering the last folder per use).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_FOLDER_PICKER_H
#define PATCHKNOB_SDLUI_FOLDER_PICKER_H

#include <string>

namespace ui {

struct App;   // fwd

//! Ask the user for a folder.  \a current is where the dialog opens (may be
//! empty).  Returns false if cancelled, leaving \a selected untouched.
bool choose_folder(App& app, const std::string& current, std::string& selected);

} // namespace ui

#endif // PATCHKNOB_SDLUI_FOLDER_PICKER_H
