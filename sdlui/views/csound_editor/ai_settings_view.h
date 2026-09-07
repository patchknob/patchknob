//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/ai_settings_view.h
//
//  Settings window for the in-DAW Claude chat: the API key (masked entry,
//  Save / Clear, where it lives, the machine fingerprint) and the model /
//  thinking / max-tokens prefs.
//
//  RULES (from api_key_store.h and the task contract):
//   * The key is never displayed after it is saved, and never logged.  The
//     entry field is masked while typing.
//   * ForeignMachine is reported as exactly that -- a key from another
//     machine or user, offered for replacement -- never as "missing".
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_AI_SETTINGS_VIEW_H
#define PATCHKNOB_SDLUI_AI_SETTINGS_VIEW_H

#include <functional>
#include <string>

#include "../../gui.h"
#include "engine/ai/api_key_store.h"

namespace ui {

class AiSettingsView : public Widget {
public:
    AiSettingsView();

    //! Fired after anything persistent changed (key saved/cleared, model or
    //! prefs edited) so the chat panel can reload.
    std::function<void()> on_changed;

    void draw(App& app) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_key(App& app, SDL_Keycode k) override;
    void cancel_interaction(App& app) override;

    // ---- store seams: real api_key_store by default; the harness swaps in
    // fakes so tests never touch the user's actual key file. --------------
    std::function<PatchKnob::ai::KeyLoad()>                    load_key;
    std::function<bool(const std::string&, std::string*)>      save_key;
    std::function<bool()>                                      clear_key;

    // ---- test seams ------------------------------------------------------
    void setEntryForTest(const std::string& s) { keyEdit_ = s; }
    void saveForTest(App& app) { doSave(app); }
    void clearForTest(App& app) { doClear(app); }
    std::string feedbackForTest() const { return feedback_; }

private:
    void doSave(App& app);
    void doClear(App& app);
    void doPaste(App& app);
    void focusEntry(App& app);
    std::string statusLine() const;

    std::string keyEdit_;            // the key being typed; NEVER drawn as-is
    bool        entryFocused_ = false;
    std::string feedback_;           // save/clear outcome, shown under the row
    bool        feedbackError_ = false;

    // clickable rects computed each draw
    SDL_Rect entryBox_{0,0,0,0}, pasteBtn_{0,0,0,0}, saveBtn_{0,0,0,0},
             clearBtn_{0,0,0,0};
    SDL_Rect backendCliRow_{0,0,0,0}, backendApiRow_{0,0,0,0};
    SDL_Rect modelRow_{0,0,0,0}, thinkRow_{0,0,0,0}, tokensRow_{0,0,0,0};
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_AI_SETTINGS_VIEW_H
