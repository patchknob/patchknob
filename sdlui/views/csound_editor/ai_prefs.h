//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/ai_prefs.h
//
//  Tiny persistent prefs for the Claude chat: model id, extended thinking,
//  max tokens.  Lives next to the key file (the SDL pref dir) but is plain
//  text -- it never contains the key, and nothing here is worth binding to a
//  machine.  Written by the settings window; read by the chat panel.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_AI_PREFS_H
#define PATCHKNOB_SDLUI_AI_PREFS_H

#include <string>

namespace ui { namespace aiprefs {

struct Prefs {
    //  Which backend spends whose money.  `useCli` (the DEFAULT) shells out
    //  to the locally installed Claude Code and spends the user's existing
    //  SUBSCRIPTION -- no API key held, stored or transmitted, no per-token
    //  cost.  false = the Messages API with the user's own key, per token.
    bool        useCli    = true;
    std::string model;            // one of the three known ids; Opus default
    bool        thinking  = true;
    int         maxTokens = 8192;
};

Prefs load();
bool  save(const Prefs& p);
std::string path();

//! Redirect the file for the headless harness so tests never touch the real
//! user preferences.  Empty restores the default location.
void set_path_for_test(const std::string& p);

//! The valid model ids, for pickers: { opus, sonnet, haiku }.
const char* const* models();
int model_count();

}} // namespace ui::aiprefs

#endif // PATCHKNOB_SDLUI_AI_PREFS_H
