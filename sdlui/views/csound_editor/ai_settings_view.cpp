//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/ai_settings_view.cpp
//----------------------------------------------------------------------------
#include "ai_settings_view.h"

#include "ai_prefs.h"
#include "engine/ai/cli_backend.h"

#include <algorithm>

namespace ui {

using PatchKnob::ai::KeyLoad;
using PatchKnob::ai::KeyStatus;

namespace {
bool inside(const SDL_Rect& q, int x, int y) {
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}
//  Strip whitespace a clipboard paste drags along -- keys never contain any.
std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
const int kTokenSteps[] = { 4096, 8192, 16384, 32768 };
} // namespace

AiSettingsView::AiSettingsView() {
    load_key  = []() { return PatchKnob::ai::loadApiKey(); };
    save_key  = [](const std::string& k, std::string* err) {
        return PatchKnob::ai::saveApiKey(k, err);
    };
    clear_key = []() { return PatchKnob::ai::clearApiKey(); };
}

std::string AiSettingsView::statusLine() const {
    const KeyLoad kl = load_key ? load_key() : PatchKnob::ai::loadApiKey();
    switch (kl.status) {
        case KeyStatus::Ok:
            return "A key is saved for this machine. It is not shown again; "
                   "enter a new one to replace it.";
        case KeyStatus::Missing:
            return "No key is saved on this machine yet.";
        case KeyStatus::ForeignMachine:
            return "A key is saved, but it was written on a DIFFERENT machine "
                   "or by a different user, so it will not be used here. "
                   "Enter this machine's own key to replace it.";
        case KeyStatus::Unreadable:
            return "The saved key file is unreadable (corrupt). Enter the key "
                   "again to replace it.";
    }
    return std::string();
}

void AiSettingsView::draw(App& app) {
    SDL_Renderer* r = app.ren;
    const Theme&  t = theme();
    fill_rect(r, rect, t.bg);

    const int cw   = app.mono.cw() > 0 ? app.mono.cw() : 7;
    const int rowH = app.mono.ch() + 6;
    const int labX = rect.x + 12;
    const int wrapCols = std::max(16, (rect.w - 24) / cw);
    int y = rect.y + 10;

    auto wrapDraw = [&](const std::string& text, Color c) {
        //  Naive word wrap; these are short explanatory paragraphs.
        std::string line;
        std::string word;
        auto flush = [&]() {
            if (line.empty()) return;
            app.mono.draw(r, labX, y, line, c);
            y += rowH - 2;
            line.clear();
        };
        for (size_t i = 0; i <= text.size(); ++i) {
            const char ch = i < text.size() ? text[i] : ' ';
            if (ch == ' ' || ch == '\n') {
                if ((int)(line.size() + word.size() + 1) > wrapCols) flush();
                if (!line.empty()) line += ' ';
                line += word;
                word.clear();
                if (ch == '\n') flush();
            } else word.push_back(ch);
        }
        flush();
    };

    // --- backend: whose money gets spent -----------------------------------
    const aiprefs::Prefs p = aiprefs::load();
    std::string cliVer;
    const bool cliFound = PatchKnob::ai::cliAvailable(&cliVer);
    app.mono.draw(r, labX, y, "BACKEND", t.dim);
    y += rowH;
    {
        auto radio = [&](SDL_Rect& row, bool selected, const std::string& name,
                         const std::string& costNote, const std::string& status,
                         bool statusBad) {
            const int lines = status.empty() ? 2 : 3;
            row = SDL_Rect{ labX, y, rect.w - 24, (rowH - 2) * lines + 6 };
            fill_rect(r, row, selected ? t.panel : t.bg);
            frame_rect(r, row, selected ? t.accent : t.dim);
            app.mono.draw(r, row.x + 6, y + 3, std::string(selected ? "[x] " : "[ ] ") + name,
                          selected ? t.text : t.dim);
            app.mono.draw(r, row.x + 6 + 4 * cw, y + 3 + (rowH - 2), costNote, t.dim);
            if (!status.empty())
                app.mono.draw(r, row.x + 6 + 4 * cw, y + 3 + 2 * (rowH - 2), status,
                              statusBad ? t.hi : t.dim);
            y += row.h + 4;
        };
        //  The cost difference IS the point: say it where the choice is made.
        radio(backendCliRow_, p.useCli, "Claude Code",
              "uses your subscription -- no API key, no per-token cost",
              cliFound ? "detected: " + cliVer
                       : "Claude Code was NOT found on your PATH",
              !cliFound);
        radio(backendApiRow_, !p.useCli, "Anthropic API",
              "uses your own key below, billed per token",
              std::string(), false);
    }
    y += rowH / 2;

    app.mono.draw(r, labX, y, "ANTHROPIC API KEY  (API backend only)", t.dim);
    y += rowH;
    wrapDraw(statusLine(), t.text);
    y += 4;

    // --- entry row --------------------------------------------------------
    const int btnH  = rowH;
    const int pasteW = 7 * cw + 10, saveW = 6 * cw + 10, clearW = 7 * cw + 10;
    const int entryW = std::max(60, rect.w - 24 - pasteW - saveW - clearW - 12);
    entryBox_ = SDL_Rect{ labX, y, entryW, btnH };
    pasteBtn_ = SDL_Rect{ entryBox_.x + entryBox_.w + 4, y, pasteW, btnH };
    saveBtn_  = SDL_Rect{ pasteBtn_.x + pasteBtn_.w + 4, y, saveW, btnH };
    clearBtn_ = SDL_Rect{ saveBtn_.x + saveBtn_.w + 4, y, clearW, btnH };

    fill_rect(r, entryBox_, t.panel);
    frame_rect(r, entryBox_, entryFocused_ ? t.accent : t.dim);
    {
        //  MASKED, always: the field shows one '*' per character and nothing
        //  else, so the key cannot be shoulder-read or screenshotted.
        const bool placeholder = keyEdit_.empty() && !entryFocused_;
        std::string shown = placeholder
            ? std::string("sk-ant-... (click, then paste)")
            : std::string(keyEdit_.size(), '*') + (entryFocused_ ? "_" : "");
        const int fit = std::max(1, (entryBox_.w - 8) / cw);
        if ((int)shown.size() > fit) shown = shown.substr(shown.size() - (size_t)fit);
        SDL_Rect tb{ entryBox_.x + 4, entryBox_.y + 3, entryBox_.w - 8, entryBox_.h - 6 };
        app.mono.draw_fitted(r, tb, shown, placeholder ? t.dim : t.text,
                             false, 1.f, .55f, false);
    }
    struct Btn { SDL_Rect* q; const char* label; };
    for (Btn b : { Btn{ &pasteBtn_, "PASTE" }, Btn{ &saveBtn_, "SAVE" },
                   Btn{ &clearBtn_, "CLEAR" } }) {
        fill_rect(r, *b.q, t.keybg);
        frame_rect(r, *b.q, t.dim);
        app.mono.draw_centered(r, *b.q, b.label, t.text);
    }
    y += btnH + 6;

    if (!feedback_.empty()) {
        wrapDraw(feedback_, feedbackError_ ? t.hi : t.accent);
        y += 2;
    }

    wrapDraw("Stored at: " + PatchKnob::ai::apiKeyPath(), t.dim);
    wrapDraw("Machine fingerprint: " + PatchKnob::ai::machineFingerprint() +
             "  (keys are bound to this machine and user)", t.dim);
    y += rowH / 2;

    // --- model / prefs ----------------------------------------------------
    app.mono.draw(r, labX, y, "MODEL", t.dim);
    y += rowH;
    {
        modelRow_ = SDL_Rect{ labX, y, rect.w - 24, btnH };
        int x = labX;
        for (int i = 0; i < aiprefs::model_count(); ++i) {
            const std::string name = aiprefs::models()[i];
            const int w = (int)name.size() * cw + 12;
            SDL_Rect b{ x, y, w, btnH };
            const bool cur = p.model == name;
            fill_rect(r, b, cur ? t.accent : t.keybg);
            frame_rect(r, b, cur ? t.hi : t.dim);
            app.mono.draw_centered(r, b, name, cur ? t.keybg : t.text);
            x += w + 6;
        }
        y += btnH + 6;
    }
    {
        thinkRow_ = SDL_Rect{ labX, y, rect.w - 24, btnH };
        app.mono.draw(r, labX, y + 3, "Extended thinking", t.text);
        app.mono.draw(r, labX + 22 * cw, y + 3, p.thinking ? "ON" : "OFF",
                      p.thinking ? t.accent : t.dim);
        app.mono.draw(r, labX + 28 * cw, y + 3, "(click to toggle)", t.dim);
        y += btnH;
    }
    {
        tokensRow_ = SDL_Rect{ labX, y, rect.w - 24, btnH };
        app.mono.draw(r, labX, y + 3, "Max reply tokens", t.text);
        app.mono.draw(r, labX + 22 * cw, y + 3, std::to_string(p.maxTokens), t.accent);
        app.mono.draw(r, labX + 30 * cw, y + 3, "(click to cycle)", t.dim);
        y += btnH + 8;
    }

    wrapDraw("The key is never written into a project file, never compiled "
             "into PatchKnob, and never logged.", t.dim);
}

void AiSettingsView::focusEntry(App& app) {
    entryFocused_ = true;
    app.begin_text(&keyEdit_,
                   [&app]() { app.request_redraw(); },
                   [this, &app](bool commit) {
                       entryFocused_ = false;
                       if (commit && !keyEdit_.empty()) doSave(app);
                       app.request_redraw();
                   });
}

void AiSettingsView::doPaste(App& app) {
    char* clip = SDL_GetClipboardText();
    if (clip) {
        const std::string k = trimmed(clip);
        SDL_free(clip);
        if (!k.empty()) {
            keyEdit_ = k;
            feedback_.clear();
            app.request_redraw();
            return;
        }
    }
    feedback_ = "The clipboard is empty.";
    feedbackError_ = true;
    app.request_redraw();
}

void AiSettingsView::doSave(App& app) {
    const std::string key = trimmed(keyEdit_);
    if (key.empty()) {
        feedback_ = "Nothing to save: paste or type a key first.";
        feedbackError_ = true;
        app.request_redraw();
        return;
    }
    std::string reason;
    if (!PatchKnob::ai::looksLikeApiKey(key, &reason)) {
        feedback_ = "That does not look like an Anthropic key: " + reason;
        feedbackError_ = true;
        app.request_redraw();
        return;
    }
    std::string err;
    if (!(save_key && save_key(key, &err))) {
        feedback_ = "Could not save the key" + (err.empty() ? "." : ": " + err);
        feedbackError_ = true;
    } else {
        //  Drop the plaintext from the edit buffer immediately -- the field
        //  never shows a saved key, masked or otherwise.
        keyEdit_.clear();
        app.end_text_if(&keyEdit_);
        entryFocused_ = false;
        feedback_ = "Key saved for this machine.";
        feedbackError_ = false;
        if (on_changed) on_changed();
    }
    app.request_redraw();
}

void AiSettingsView::doClear(App& app) {
    keyEdit_.clear();
    app.end_text_if(&keyEdit_);
    entryFocused_ = false;
    if (clear_key && clear_key()) {
        feedback_ = "The stored key was removed.";
        feedbackError_ = false;
        if (on_changed) on_changed();
    } else {
        feedback_ = "There was no stored key to remove.";
        feedbackError_ = false;
    }
    app.request_redraw();
}

bool AiSettingsView::on_mouse(App& app, const MouseEv& e) {
    if (!e.pressed || e.button != SDL_BUTTON_LEFT) return false;
    if (inside(entryBox_, e.x, e.y)) { focusEntry(app); app.request_redraw(); return true; }
    if (inside(pasteBtn_, e.x, e.y)) { doPaste(app); return true; }
    if (inside(saveBtn_,  e.x, e.y)) {
        //  Commit the live edit first so what is saved is what was typed.
        if (entryFocused_) { app.end_text_if(&keyEdit_); entryFocused_ = false; }
        doSave(app);
        return true;
    }
    if (inside(clearBtn_, e.x, e.y)) { doClear(app); return true; }

    if (inside(backendCliRow_, e.x, e.y) || inside(backendApiRow_, e.x, e.y)) {
        aiprefs::Prefs p = aiprefs::load();
        const bool wantCli = inside(backendCliRow_, e.x, e.y);
        if (p.useCli != wantCli) {
            p.useCli = wantCli;
            aiprefs::save(p);
            if (on_changed) on_changed();
        }
        app.request_redraw();
        return true;
    }

    if (inside(modelRow_, e.x, e.y)) {
        //  Recompute the same per-model button rects draw() used.
        const int cw = app.mono.cw() > 0 ? app.mono.cw() : 7;
        aiprefs::Prefs p = aiprefs::load();
        int x = modelRow_.x;
        for (int i = 0; i < aiprefs::model_count(); ++i) {
            const std::string name = aiprefs::models()[i];
            const int w = (int)name.size() * cw + 12;
            SDL_Rect b{ x, modelRow_.y, w, modelRow_.h };
            if (inside(b, e.x, e.y)) {
                p.model = name;
                aiprefs::save(p);
                if (on_changed) on_changed();
                app.request_redraw();
                return true;
            }
            x += w + 6;
        }
        return true;
    }
    if (inside(thinkRow_, e.x, e.y)) {
        aiprefs::Prefs p = aiprefs::load();
        p.thinking = !p.thinking;
        aiprefs::save(p);
        if (on_changed) on_changed();
        app.request_redraw();
        return true;
    }
    if (inside(tokensRow_, e.x, e.y)) {
        aiprefs::Prefs p = aiprefs::load();
        const int n = (int)(sizeof(kTokenSteps) / sizeof(kTokenSteps[0]));
        int idx = 0;
        for (int i = 0; i < n; ++i)
            if (kTokenSteps[i] == p.maxTokens) { idx = (i + 1) % n; break; }
        p.maxTokens = kTokenSteps[idx];
        aiprefs::save(p);
        if (on_changed) on_changed();
        app.request_redraw();
        return true;
    }
    return false;
}

bool AiSettingsView::on_key(App& app, SDL_Keycode k) {
    //  Ctrl+V without the field focused still pastes into it -- while the
    //  field IS focused the toolkit's single-field editor owns the keys and
    //  the PASTE button is the way in.
    if ((SDL_GetModState() & KMOD_CTRL) && k == SDLK_v) {
        doPaste(app);
        return true;
    }
    return false;
}

void AiSettingsView::cancel_interaction(App& app) {
    //  The window is closing/minimising: App::text_target points at keyEdit_
    //  while the field is focused, so release it or every keystroke afterwards
    //  is swallowed (see App::end_text_if).  The typed text itself is kept --
    //  it is masked on screen and the user may reopen to finish pasting.
    app.end_text_if(&keyEdit_);
    entryFocused_ = false;
}

} // namespace ui
