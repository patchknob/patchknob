//----------------------------------------------------------------------------
//  sdlui/views/file_dialog/file_dialog.h
//
//  The one in-app, SDL-drawn file/folder chooser for the whole shell.  Native
//  OS dialogs (Win32 GetOpenFileName/GetSaveFileName/SHBrowseForFolder) do not
//  exist under a windowless KMSDRM target, and cost a second, inconsistent
//  UI language everywhere else -- so every save/open/browse-folder path in
//  the app (project, Pd/Csound/rack patches, WAV import, the screen-recorder
//  output folder) goes through this one widget instead, on every platform.
//
//  Usage is a single blocking-looking call (choose_path()); internally it
//  pumps its own small SDL event loop (App::run_modal) so it works the same
//  way the old synchronous native dialogs did, without restructuring the
//  ~15 call sites that expect "call it, get a path or a cancel" back.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_FILE_DIALOG_H
#define PATCHKNOB_SDLUI_FILE_DIALOG_H

#include "gui.h"

#include <string>
#include <vector>

namespace ui {

class FileDialog : public Widget {
public:
    struct Options {
        bool save          = false;   //!< Save (filename field + overwrite ok) vs Open
        bool foldersOnly    = false;   //!< directory picker: OK accepts the current folder
        std::string title;
        std::string startDir;          //!< initial directory ("" -> last used / home)
        std::string defaultName;       //!< pre-filled filename (save mode)
        //!< Extensions without the dot, e.g. {"s24"}.  Empty = show every file.
        //!< The first one is appended to a save-mode name that has none.
        std::vector<std::string> extensions;
    };

    void open(const Options& opt);
    bool active() const { return m_active; }

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    bool on_key(App& app, SDL_Keycode k) override;
    //! SDL_TEXTINPUT text, routed here by run_modal while a dialog is active
    //! (this widget does not go through App::text_target).
    bool on_text(App& app, const char* utf8) override;

    //! Set once the dialog closes (OK, Cancel, or Escape).
    bool         ok() const     { return m_ok; }
    const std::string& result() const { return m_result; }

private:
    void scan();
    void navigate(const std::string& dir);
    void accept_row(int index);   // Enter / double-click
    void try_accept();            // OK button / Enter with nothing special selected
    void cancel();
    bool matches_filter(const std::string& name) const;
    int  row_at(int y) const;
    int  visible_rows() const;

    struct Item { std::string name, path; bool dir = false; long long size = 0; };

    Options            m_opt;
    bool               m_active = false;
    bool               m_ok = false;
    std::string        m_result;

    std::string        m_dir;
    std::vector<Item>  m_items;
    int                m_sel = -1, m_scroll = 0;
    std::string        m_filename;         // save-mode text field buffer
    bool               m_nameFocused = false;
    Uint32             m_lastClickMs = 0;  // double-click detection

    // geometry, laid out once per draw() and reused by on_mouse()/row_at()
    SDL_Rect m_panel{0,0,0,0}, m_list{0,0,0,0};
    SDL_Rect m_nameField{0,0,0,0}, m_okBtn{0,0,0,0}, m_cancelBtn{0,0,0,0}, m_upBtn{0,0,0,0};
    int      m_rowH = 18;
};

//! Blocking-style helper: opens `dlg`, pumps events via App::run_modal until
//! the user accepts or cancels, and reports the result the way the old
//! native-dialog wrappers did.  Returns false (leaving `selected` untouched)
//! when cancelled.
bool choose_path(App& app, const FileDialog::Options& opt, std::string& selected);

} // namespace ui

#endif // PATCHKNOB_SDLUI_FILE_DIALOG_H
