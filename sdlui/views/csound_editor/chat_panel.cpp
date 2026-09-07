//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/chat_panel.cpp
//
//  The engine (ClaudeClient) owns the whole exchange, INCLUDING the automatic
//  opcode audit: reply -> fenced `csound` blocks checked against the shipped
//  manual -> mismatches handed back to the model -> Done only when the whole
//  thing (correction rounds included) is over.  This file only renders that
//  stream and never re-enables the composer before Done.
//----------------------------------------------------------------------------
#include "chat_panel.h"

#include "ai_prefs.h"
#include "engine/patch/csound_dry_compile.h"
#include "engine/ai/http_client.h"

#include <algorithm>
#include <cctype>

namespace ui {

using PatchKnob::ai::ChatEvent;
using PatchKnob::ai::ChatMessage;
using PatchKnob::ai::EventKind;
using PatchKnob::ai::KeyLoad;
using PatchKnob::ai::KeyStatus;

namespace {

const char* kSendLabel = "SEND";

std::string lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = (char)std::tolower((unsigned char)s[i]);
    return s;
}

bool insideRect(const SDL_Rect& q, int x, int y) {
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

//! Fence tags whose blocks are Csound and get Apply buttons.  Matches the
//! system prompt (which pins `csound`) plus the tags extractCodeBlocks'
//! documentation lists as Csound.
bool applyableTag(const std::string& tag) {
    return tag == "csound" || tag == "csd" || tag == "orc";
}

//! Split `text` into logical lines (dropping '\r'), then wrap each to `cols`
//! columns, preferring a break at the last space.  Monospace, so columns are
//! exactly pixels/cw.
void wrapPlain(const std::string& text, int cols,
               const std::function<void(const std::string&)>& emit) {
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                    : nl - pos);
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (line.empty()) emit(std::string());
        while (!line.empty()) {
            if ((int)line.size() <= cols) { emit(line); break; }
            size_t brk = line.rfind(' ', (size_t)cols);
            if (brk == std::string::npos || brk == 0) brk = (size_t)cols;
            emit(line.substr(0, brk));
            size_t next = brk;
            if (next < line.size() && line[next] == ' ') ++next;   // eat the break space
            line.erase(0, next);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

}  // namespace

CsoundChatPanel::CsoundChatPanel() {
    keyLoader_ = []() { return PatchKnob::ai::loadApiKey(); };
}

CsoundChatPanel::~CsoundChatPanel() {
    // ~ClaudeClient cancels + joins its worker itself.
}

//----------------------------------------------------------------------------
//  settings / key state
//----------------------------------------------------------------------------

void CsoundChatPanel::reloadSettings() {
    loaded_ = true;
    const KeyLoad kl = keyLoader_ ? keyLoader_() : PatchKnob::ai::loadApiKey();
    keyStatus_ = kl.status;
    //  ForeignMachine/Unreadable NEVER reach the client: the key is not for
    //  this machine (or is garbage) and must not be sent anywhere.
    client_.setApiKey(kl.status == KeyStatus::Ok ? kl.key : std::string());

    const aiprefs::Prefs p = aiprefs::load();
    //  Cli (the default) spends the user's Claude Code subscription and needs
    //  no key at all; Api spends their own key per token.
    client_.setBackend(p.useCli ? PatchKnob::ai::ClaudeClient::Backend::Cli
                                : PatchKnob::ai::ClaudeClient::Backend::Api);
    client_.setModel(p.model);
    client_.setThinking(p.thinking);
    client_.setMaxTokens(p.maxTokens);

    //  Turns the automatic opcode audit ON.  Without this the client skips the
    //  check (and says so); with it every reply's `csound` fences are verified
    //  against the manual the help drawer shows.
    client_.setManualDir(get_manual_dir ? get_manual_dir() : std::string());

    //  CLOSE THE LOOP: the assistant's code is compiled by the REAL Csound
    //  compiler after the manual audit passes, and any error is fed back for
    //  another attempt. A throwaway Csound instance is used, so this never
    //  touches the running patch and is safe on the client's worker thread.
    client_.setDocumentProvider([this]() {
        return get_orc ? get_orc() : std::string();
    });
    client_.setCompileCheck([](const std::string& csd) -> std::string {
        const PatchKnob::engine::CsoundDryCompileResult r =
            PatchKnob::engine::csoundDryCompile(csd);
        if (!r.available) return std::string();   // no Csound: skip, do not block
        if (r.ok) return std::string();
        return r.diagnostics;
    });

    httpOk_ = PatchKnob::ai::available();
    refreshNotice();
}

void CsoundChatPanel::refreshNotice() {
    notice_.clear();
    //  Cli backend: no key is involved and libcurl is not on the path either,
    //  so neither the key status nor httpOk_ may gate it.  The only thing
    //  that can be wrong is Claude Code itself being absent, and ready()
    //  already words that ("...Install it, or switch to the API backend in
    //  Settings.").  Mentioning API keys here would send the user hunting for
    //  something this path never uses.
    if (client_.backend() == PatchKnob::ai::ClaudeClient::Backend::Cli) {
        std::string why;
        if (!client_.ready(&why)) notice_ = why;
        return;
    }
    if (!httpOk_) {
        notice_ = "This build has no working HTTPS backend, so the assistant "
                  "cannot reach the Claude API. (Linux builds need libcurl; "
                  "Windows builds use WinHTTP.)";
        return;
    }
    switch (keyStatus_) {
        case KeyStatus::Ok: break;
        case KeyStatus::Missing:
            notice_ = "No Anthropic API key is set on this machine.\n\n"
                      "Open Settings to add your key. It is stored only in "
                      "your user preferences on this computer -- never in a "
                      "project file, and it is not shown again after saving.";
            break;
        case KeyStatus::ForeignMachine:
            notice_ = "An API key is stored, but it was saved on a DIFFERENT "
                      "machine or by a different user, so it will not be used "
                      "here. (This machine's fingerprint: " +
                      PatchKnob::ai::machineFingerprint() + ".)\n\n"
                      "Open Settings to replace it with a key entered on this "
                      "machine.";
            break;
        case KeyStatus::Unreadable:
            notice_ = "The stored API key file exists but cannot be read (it "
                      "looks corrupt).\n\nOpen Settings to enter the key "
                      "again.";
            break;
    }
}

bool CsoundChatPanel::canChat() const {
    if (client_.backend() == PatchKnob::ai::ClaudeClient::Backend::Cli)
        return client_.ready();
    return httpOk_ && keyStatus_ == KeyStatus::Ok;
}

//----------------------------------------------------------------------------
//  pump -- once per frame, message thread, always
//----------------------------------------------------------------------------

void CsoundChatPanel::pump(App& app, bool shown) {
    if (!loaded_) reloadSettings();
    bool got = false;
    client_.poll([&](const ChatEvent& ev) { handleEvent(ev); got = true; });
    //  Belt and braces: should the worker ever end without a Done reaching us,
    //  the streaming entry would spin forever.  Queue empty + client idle is
    //  finished by every definition that matters.
    if (streamIdx_ >= 0 && !client_.busy() && !got) finishStreaming(std::string());
    //  Perform a staged auto-apply here, where an App& exists. One apply per
    //  reply, and it goes through the editor's normal undo, so a single Ctrl+Z
    //  takes it back if the user did not want it.
    if (!autoApplyCode_.empty()) {
        const std::string code = autoApplyCode_;
        const bool replace = autoApplyReplace_;
        autoApplyCode_.clear();
        if (apply_code) {
            apply_code(app, code, replace);
            Entry note;
            note.text = replace ? "Applied to the editor (whole document). Ctrl+Z undoes it."
                                : "Applied to the editor at the cursor. Ctrl+Z undoes it.";
            note.rateLimit = true;          // calm styling: this is a notice
            entries_.push_back(note);
        }
        app.request_redraw();
    }
    if (shown && (got || client_.busy())) app.request_redraw();
}

void CsoundChatPanel::handleEvent(const ChatEvent& ev) {
    //  An event with no streaming entry (transcript cleared while a reply was
    //  in flight) gets a fresh one so nothing is ever dropped.
    auto ensureStream = [&]() -> Entry& {
        if (streamIdx_ < 0 || streamIdx_ >= (int)entries_.size()) {
            Entry e;
            e.streaming = true;
            entries_.push_back(e);
            streamIdx_ = (int)entries_.size() - 1;
        }
        return entries_[(size_t)streamIdx_];
    };
    //  Every kind is named here; new kinds must be wired deliberately, never
    //  treated as reply text by accident.
    switch (ev.kind) {
        case EventKind::Delta: {
            Entry& e = ensureStream();
            e.text += ev.text;
            e.wrapW = -1;
            break;
        }
        case EventKind::Thinking: {
            Entry& e = ensureStream();
            e.thinking += ev.text;
            e.wrapW = -1;
            break;
        }
        case EventKind::Status: {
            //  Replaces whatever progress line was there; an empty text means
            //  "real output started, clear it".
            Entry& e = ensureStream();
            e.statusNote = ev.text;
            e.wrapW = -1;
            break;
        }
        case EventKind::Checking: {
            //  Progress, not conversation: lives in the entry's status note,
            //  never in the text that becomes history.
            Entry& e = ensureStream();
            e.checkNote = ev.text;
            e.wrapW = -1;
            break;
        }
        case EventKind::Checked: {
            Entry& e = ensureStream();
            e.checkNote   = ev.text;
            e.checkIssues = ev.issues;
            e.wrapW = -1;
            break;
        }
        case EventKind::Compiling: {
            //  Progress, like Checking: it lives in the entry's status note and
            //  never becomes history.
            Entry& e = ensureStream();
            e.checkNote = ev.text;
            e.wrapW = -1;
            break;
        }
        case EventKind::Compiled: {
            //  The compiler is the authority, so its verdict REPLACES the
            //  static audit's note. issues == 0 means it built; anything else
            //  carries Csound's own diagnostics, which the model is about to be
            //  handed for another attempt.
            Entry& e = ensureStream();
            e.checkNote   = ev.text;
            e.checkIssues = ev.issues;
            e.compiled    = (ev.issues == 0);
            e.wrapW = -1;
            break;
        }
        case EventKind::Retry: {
            //  A correction round is starting: the current reply is
            //  superseded.  Keep it visible (with its audit verdict) but drop
            //  it from history and never offer its code for Apply; the
            //  corrected reply streams into a fresh entry.
            if (streamIdx_ >= 0 && streamIdx_ < (int)entries_.size()) {
                Entry& e = entries_[(size_t)streamIdx_];
                e.streaming  = false;
                e.superseded = true;
                e.stopNote   = ev.text;   // "Asking for a correction..."
                e.wrapW      = -1;
            }
            streamIdx_ = -1;   // next Delta opens the corrected entry
            break;
        }
        case EventKind::Error:
            addErrorEntry(ev.text);
            break;
        case EventKind::Done:
            //  Done means the WHOLE exchange -- audit and correction rounds
            //  included -- is over.  This is the only place the composer is
            //  re-enabled (busy() went false with it).
            finishStreaming(ev.stopReason == "cancelled" ? "(cancelled)"
                                                         : std::string());
            break;
    }
}

void CsoundChatPanel::finishStreaming(const std::string& stopNote) {
    if (streamIdx_ < 0 || streamIdx_ >= (int)entries_.size()) { streamIdx_ = -1; return; }
    Entry& e = entries_[(size_t)streamIdx_];
    e.streaming = false;
    if (!stopNote.empty()) e.stopNote = stopNote;
    e.wrapW = -1;

    //  Stage the auto-apply. Guarded on e.compiled: a reply that failed the
    //  audit, failed to compile, or was superseded by a correction round never
    //  reaches the user's document on its own.
    if (autoApply_ && e.compiled && !e.superseded && !e.codeBlocks.empty() &&
        stopNote.empty()) {
        const std::string& code = e.codeBlocks.back();
        autoApplyCode_ = code;
        //  A whole document replaces the buffer; a bare instr block is
        //  inserted at the cursor, which is what "add me an instrument" means.
        autoApplyReplace_ = code.find("<CsoundSynthesizer>") != std::string::npos;
    }
    streamIdx_ = -1;
}

void CsoundChatPanel::addErrorEntry(const std::string& msg) {
    Entry e;
    e.error = true;
    //  The CLI backend reports its subscription rate limit as an Error event
    //  ("Claude Code is rate limited right now...").  That is a real state
    //  the user will hit, and it should read as "wait a bit", not as a crash.
    e.rateLimit = msg.find("rate limited") != std::string::npos;
    e.text  = msg;
    entries_.push_back(e);
    followTail_ = true;
}

//----------------------------------------------------------------------------
//  sending
//----------------------------------------------------------------------------

void CsoundChatPanel::sendPrompt(App& app, const std::string& prompt) {
    if (prompt.empty() || client_.busy()) return;

    //  Api backend only: the foreign-machine nuance would otherwise be
    //  reported as "no key set" by ready().  The Cli backend never touches a
    //  key, so the state is irrelevant there.
    if (client_.backend() == PatchKnob::ai::ClaudeClient::Backend::Api &&
        keyStatus_ == KeyStatus::ForeignMachine) {
        addErrorEntry("The stored API key belongs to a different machine or "
                      "user and will not be used. Open Settings to replace it.");
        app.request_redraw();
        return;
    }

    std::vector<ChatMessage> history;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        //  Errors are transport noise and a superseded reply was already
        //  replaced by its correction -- neither is a conversation turn.
        if (e.error || e.superseded || e.text.empty()) continue;
        ChatMessage m;
        m.fromUser = e.fromUser;
        m.text     = e.text;
        history.push_back(m);
    }
    ChatMessage mine;
    mine.fromUser = true;
    mine.text     = prompt;
    history.push_back(mine);

    //  The LIVE buffer and the LAST compiler output, fetched now -- the
    //  editor is the source of truth, never a cached copy.
    const std::string orc    = get_orc    ? get_orc()    : std::string();
    const std::string errors = get_errors ? get_errors() : std::string();

    std::string why;
    if (!client_.send(history, orc, errors, &why)) {
        addErrorEntry(why.empty() ? "The request could not be started." : why);
        app.request_redraw();
        return;
    }
    Entry user;
    user.fromUser = true;
    user.text     = prompt;
    entries_.push_back(user);
    Entry reply;
    reply.streaming = true;
    entries_.push_back(reply);
    streamIdx_  = (int)entries_.size() - 1;
    followTail_ = true;
    app.request_redraw();
}

//----------------------------------------------------------------------------
//  geometry
//----------------------------------------------------------------------------

int CsoundChatPanel::widthFor(const SDL_Rect& area) const {
    const int wantW = width_ > 0 ? width_
                                 : std::max(220, std::min(440, area.w * 2 / 5));
    return std::max(200, std::min(wantW, std::max(200, area.w - 80)));
}

CsoundChatPanel::Layout CsoundChatPanel::layout(const SDL_Rect& panel) const {
    Layout L;
    L.panel = panel;
    //  Cell metrics are cached by draw() (fontW_/fontH_); before the first
    //  draw they fall back to sane defaults.
    const int cw   = fontW_ > 0 ? fontW_ : 7;
    const int ch   = fontH_ > 0 ? fontH_ : 14;
    L.rowH    = ch + 2;
    L.btnRowH = ch + 6;

    const int gripW = 5;
    L.grip = SDL_Rect{ panel.x, panel.y, gripW, panel.h };
    const int innerX = panel.x + gripW + 1;
    const int innerW = std::max(0, panel.w - gripW - 2);
    const int navH   = ch + 8;

    const int setW = 5 * cw + 10, newW = 3 * cw + 10;
    L.btnSettings = SDL_Rect{ innerX + innerW - setW, panel.y + 1, setW, navH };
    L.btnNew      = SDL_Rect{ L.btnSettings.x - newW - 2, panel.y + 1, newW, navH };
    L.header      = SDL_Rect{ innerX, panel.y + 1, std::max(0, L.btnNew.x - innerX - 2), navH };

    const int sendW = (int)std::char_traits<char>::length(kSendLabel) * cw + 12;
    L.composer = SDL_Rect{ innerX, panel.y + panel.h - navH - 2,
                           std::max(0, innerW - sendW - 2), navH };
    L.sendBtn  = SDL_Rect{ innerX + innerW - sendW, L.composer.y, sendW, navH };

    //  Contextual actions row above the composer: STOP while a generation is
    //  in flight, or one-click "fix the compile error" when there is one.
    const bool haveErr = get_errors && !get_errors().empty();
    L.hasActions = client_.busy() || haveErr;
    const int actH = L.hasActions ? navH : 0;
    L.actions = SDL_Rect{ innerX, L.composer.y - actH - (L.hasActions ? 2 : 0),
                          innerW, actH };

    const int barW  = 8;
    const int bodyY = panel.y + 1 + navH + 2;
    const int bodyB = (L.hasActions ? L.actions.y : L.composer.y) - 2;
    L.content   = SDL_Rect{ innerX, bodyY, std::max(0, innerW - barW), std::max(0, bodyB - bodyY) };
    L.scrollbar = SDL_Rect{ innerX + innerW - barW, bodyY, barW, std::max(0, bodyB - bodyY) };
    return L;
}

int CsoundChatPanel::colsFor(const Layout& L) const {
    const int cw = fontW_ > 0 ? fontW_ : 7;
    return std::max(8, (L.content.w - 8) / cw);
}

//----------------------------------------------------------------------------
//  wrapping
//----------------------------------------------------------------------------

void CsoundChatPanel::ensureWrapped(Entry& e, int cols) {
    if (e.wrapW == cols) return;
    e.rows.clear();
    e.codeBlocks.clear();
    e.wrapW = cols;

    auto push = [&](Row::Kind k, const std::string& t, int codeIdx = -1) {
        Row r;
        r.kind    = k;
        r.text    = t;
        r.codeIdx = codeIdx;
        e.rows.push_back(std::move(r));
    };

    push(Row::Header, e.fromUser ? "YOU"
                     : e.rateLimit ? "RATE LIMITED - PLEASE WAIT"
                     : e.error ? "ERROR" : "CLAUDE");

    if (!e.fromUser && !e.thinking.empty()) {
        const std::string label = e.thinkingOpen
            ? "[-] thinking"
            : "[+] thinking (" + std::to_string(e.thinking.size()) + " chars)";
        push(Row::ThinkToggle, label);
        if (e.thinkingOpen)
            wrapPlain(e.thinking, cols,
                      [&](const std::string& t) { push(Row::ThinkText, t); });
    }

    //  Walk the body line by line so fenced code is styled, collected for the
    //  Apply buttons, and still rendered live while the fence is open.
    bool inCode = false, applyable = false;
    std::string cur;
    size_t pos = 0;
    const std::string& body = e.text;
    while (pos <= body.size() && !body.empty()) {
        const size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos
                                                                    : nl - pos);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.compare(0, 3, "```") == 0) {
            if (!inCode) {
                inCode = true;
                std::string tag = lower(line.substr(3));
                while (!tag.empty() && tag[tag.size() - 1] == ' ') tag.erase(tag.size() - 1);
                //  Superseded replies keep their code visible but never offer
                //  it for Apply -- the correction below replaced it.
                applyable = !e.fromUser && !e.superseded && applyableTag(tag);
                cur.clear();
                push(Row::Fence, line);
            } else {
                inCode = false;
                push(Row::Fence, "```");
                if (applyable) {
                    e.codeBlocks.push_back(cur);
                    push(Row::Apply, std::string(), (int)e.codeBlocks.size() - 1);
                }
            }
        } else if (inCode) {
            cur += line;
            cur += '\n';
            if (line.empty()) push(Row::Code, std::string());
            else wrapPlain(line, cols, [&](const std::string& t) { push(Row::Code, t); });
        } else if (line.empty()) {
            push(e.rateLimit ? Row::Note : e.error ? Row::Error : Row::Text,
                 std::string());
        } else {
            wrapPlain(line, cols, [&](const std::string& t) {
                push(e.rateLimit ? Row::Note : e.error ? Row::Error : Row::Text, t);
            });
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    if (e.streaming) {
        //  Show the live progress line in place of the bare ellipsis while one
        //  is set: a minute of silent thinking is indistinguishable from a
        //  hang, which is exactly what a user reported.
        push(Row::Note, e.statusNote.empty() ? std::string("...") : e.statusNote);
    }
    if (!e.checkNote.empty()) {
        //  The audit verdict.  Clean is one reassuring line; problems list the
        //  mismatches so the user can judge the unverified code themselves.
        wrapPlain(e.checkNote, cols, [&](const std::string& t) {
            push(e.checkIssues > 0 ? Row::Error : Row::Note, t);
        });
    }
    if (!e.stopNote.empty()) push(Row::Note, e.stopNote);
    e.heightPx = -1;   // recomputed against the current layout by entryHeight
}

int CsoundChatPanel::rowHeightPx(const Row& r, const Layout& L) const {
    return r.kind == Row::Apply ? L.btnRowH + 4 : L.rowH;
}

int CsoundChatPanel::entryHeight(Entry& e, const Layout& L) {
    ensureWrapped(e, colsFor(L));
    if (e.heightPx >= 0) return e.heightPx;
    int h = 0;
    for (size_t i = 0; i < e.rows.size(); ++i) h += rowHeightPx(e.rows[i], L);
    h += 6;   // gap between entries
    e.heightPx = h;
    return h;
}

int CsoundChatPanel::totalHeight(const Layout& L) {
    int h = 0;
    for (size_t i = 0; i < entries_.size(); ++i) h += entryHeight(entries_[i], L);
    return h;
}

int CsoundChatPanel::maxScroll(const Layout& L) {
    return std::max(0, totalHeight(L) - L.content.h);
}

SDL_Rect CsoundChatPanel::thumbRect(const Layout& L) {
    const int total = totalHeight(L);
    const int maxSc = std::max(0, total - L.content.h);
    if (maxSc <= 0 || L.scrollbar.h <= 0) return SDL_Rect{ 0, 0, 0, 0 };
    const int trackH = L.scrollbar.h;
    const int thumbH = std::max(20, (int)((int64_t)trackH * L.content.h / std::max(1, total)));
    const int span   = std::max(1, trackH - thumbH);
    const int y      = L.scrollbar.y + (int)((int64_t)scroll_ * span / maxSc);
    return SDL_Rect{ L.scrollbar.x, y, L.scrollbar.w, thumbH };
}

void CsoundChatPanel::scrollToTailIfFollowing(const Layout& L) {
    if (followTail_) scroll_ = maxScroll(L);
    else scroll_ = std::max(0, std::min(maxScroll(L), scroll_));
}

void CsoundChatPanel::forEachVisibleRow(
        const Layout& L,
        const std::function<bool(int, const Row&, const SDL_Rect&)>& f) {
    const int top = L.content.y, bottom = L.content.y + L.content.h;
    int y = L.content.y - scroll_;
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        const int h = entryHeight(e, L);
        if (y + h <= top) { y += h; continue; }     // whole entry above view
        if (y >= bottom) break;                     // everything below view
        int ry = y;
        for (size_t k = 0; k < e.rows.size(); ++k) {
            const Row& r  = e.rows[k];
            const int  rh = rowHeightPx(r, L);
            if (ry + rh > top && ry < bottom) {
                const SDL_Rect rr{ L.content.x, ry, L.content.w, rh };
                if (!f((int)i, r, rr)) return;
            }
            ry += rh;
            if (ry >= bottom) break;
        }
        y += h;
    }
}

//----------------------------------------------------------------------------
//  draw
//----------------------------------------------------------------------------

void CsoundChatPanel::draw(App& app, const SDL_Rect& panel) {
    SDL_Renderer* r = app.ren;
    const Theme&  t = theme();
    //  Cache the mono cell for layout()/wrap; a font-size change moves every
    //  column, so it invalidates all wrap caches.
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 7;
    const int ch = app.mono.ch();
    if (cw != fontW_ || ch != fontH_) {
        fontW_ = cw;
        fontH_ = ch;
        for (size_t i = 0; i < entries_.size(); ++i) entries_[i].wrapW = -1;
    }
    if (!loaded_) reloadSettings();

    const Layout L = layout(panel);
    fill_rect(r, panel, t.panel);
    frame_rect(r, panel, t.dim);
    fill_rect(r, L.grip, resizeDrag_ ? t.accent : t.dim);

    // --- header -----------------------------------------------------------
    {
        //  Which backend is spending whose money stays visible at all times.
        const bool cli = client_.backend() == PatchKnob::ai::ClaudeClient::Backend::Cli;
        const std::string title = std::string(cli ? "CLAUDE CODE" : "API") +
                                  "  [" + client_.model() + "]";
        SDL_Rect tb{ L.header.x + 4, L.header.y + 3, std::max(0, L.header.w - 8), L.header.h - 6 };
        app.mono.draw_fitted(r, tb, title, t.dim, false, 1.f, .55f, true);
        fill_rect(r, L.btnNew, t.keybg);
        frame_rect(r, L.btnNew, t.dim);
        app.mono.draw_centered(r, L.btnNew, "NEW", client_.busy() ? t.dim : t.text);
        fill_rect(r, L.btnSettings, t.keybg);
        frame_rect(r, L.btnSettings, t.dim);
        app.mono.draw_centered(r, L.btnSettings, "SET..", t.text);
    }

    // --- transcript / notice ---------------------------------------------
    rowsDrawn_ = 0;
    noticeBtn_ = SDL_Rect{ 0, 0, 0, 0 };
    if (!notice_.empty()) {
        //  No key / foreign key / no HTTPS: the content area carries the
        //  explanation and an OPEN SETTINGS button instead of the transcript.
        ScopedClip clip(r, L.content);
        int y = L.content.y + 6;
        wrapPlain(notice_, colsFor(L), [&](const std::string& line) {
            app.mono.draw(r, L.content.x + 4, y, line, t.text);
            y += L.rowH;
        });
        y += 6;
        const std::string lbl = "OPEN SETTINGS...";
        SDL_Rect b{ L.content.x + 4, y, (int)lbl.size() * cw + 12, L.btnRowH + 2 };
        if (b.y + b.h < L.content.y + L.content.h) {
            fill_rect(r, b, t.accent);
            frame_rect(r, b, t.hi);
            app.mono.draw_centered(r, b, lbl, t.keybg);
            noticeBtn_ = b;
        }
    } else {
        scrollToTailIfFollowing(L);
        ScopedClip clip(r, L.content);
        if (entries_.empty()) {
            app.mono.draw(r, L.content.x + 4, L.content.y + 6,
                          "Describe an instrument, or ask about", t.dim);
            app.mono.draw(r, L.content.x + 4, L.content.y + 6 + L.rowH,
                          "the code in the editor.", t.dim);
        }
        forEachVisibleRow(L, [&](int idx, const Row& row, const SDL_Rect& rr) {
            ++rowsDrawn_;
            const Entry& en = entries_[(size_t)idx];
            switch (row.kind) {
                case Row::Header:
                    app.mono.draw(r, rr.x + 2, rr.y + 1, row.text,
                                  row.text == "YOU" ? t.accent
                                  : row.text == "ERROR" ? t.hi : t.dim);
                    if (en.superseded)
                        app.mono.draw(r, rr.x + 2 + ((int)row.text.size() + 2) * cw,
                                      rr.y + 1, "(superseded by the correction below)", t.dim);
                    break;
                case Row::Text:
                    app.mono.draw(r, rr.x + 4, rr.y + 1, row.text, t.text);
                    break;
                case Row::Error:
                    app.mono.draw(r, rr.x + 4, rr.y + 1, row.text, t.hi);
                    break;
                case Row::Code:
                    fill_rect(r, rr, t.keybg);
                    app.mono.draw(r, rr.x + 6, rr.y + 1, row.text,
                                  en.superseded ? t.dim : t.text);
                    break;
                case Row::Fence:
                    fill_rect(r, rr, t.keybg);
                    app.mono.draw(r, rr.x + 6, rr.y + 1, row.text, t.dim);
                    break;
                case Row::ThinkToggle:
                case Row::Note:
                    app.mono.draw(r, rr.x + 4, rr.y + 1, row.text, t.dim);
                    break;
                case Row::ThinkText:
                    app.mono.draw(r, rr.x + 8, rr.y + 1, row.text, t.dim);
                    break;
                case Row::Apply: {
                    //  A block from a reply the audit could not verify (issues
                    //  found and the correction rounds ran out) is still
                    //  offered, but never as if it were clean.
                    const bool unverified = en.checkIssues > 0;
                    const std::string a = unverified ? "APPLY UNVERIFIED (REPLACE DOC)"
                                                     : "APPLY (REPLACE DOC)";
                    const std::string b = "INSERT AT CURSOR";
                    SDL_Rect ra{ rr.x + 4, rr.y + 2, (int)a.size() * cw + 10, L.btnRowH };
                    SDL_Rect rb{ ra.x + ra.w + 4, rr.y + 2, (int)b.size() * cw + 10, L.btnRowH };
                    fill_rect(r, ra, unverified ? t.keybg : t.accent);
                    frame_rect(r, ra, t.hi);
                    app.mono.draw_centered(r, ra, a, unverified ? t.hi : t.keybg);
                    fill_rect(r, rb, t.keybg);
                    frame_rect(r, rb, t.dim);
                    app.mono.draw_centered(r, rb, b, t.text);
                    break;
                }
            }
            return true;
        });
    }

    // --- scrollbar --------------------------------------------------------
    fill_rect(r, L.scrollbar, t.bg);
    const SDL_Rect thumb = thumbRect(L);
    if (thumb.h > 0) {
        fill_rect(r, thumb, scrollDrag_ ? t.accent : t.dim);
        frame_rect(r, thumb, t.panel);
    }

    // --- actions row ------------------------------------------------------
    if (L.hasActions) {
        if (client_.busy()) {
            fill_rect(r, L.actions, t.keybg);
            frame_rect(r, L.actions, t.hi);
            app.mono.draw_centered(r, L.actions, "STOP GENERATING", t.hi);
        } else {
            fill_rect(r, L.actions, t.accent);
            frame_rect(r, L.actions, t.hi);
            app.mono.draw_centered(r, L.actions, "FIX THE COMPILE ERROR", t.keybg);
        }
    }

    // --- composer ---------------------------------------------------------
    fill_rect(r, L.composer, t.bg);
    frame_rect(r, L.composer, composerFocused_ ? t.accent : t.dim);
    {
        const bool placeholder = composer_.empty() && !composerFocused_;
        std::string shown = placeholder ? std::string("ask Claude...")
                                        : composer_ + (composerFocused_ ? "_" : "");
        //  Editing a long prompt: keep the tail visible.
        const int fit = std::max(1, (L.composer.w - 8) / cw);
        if ((int)shown.size() > fit) shown = shown.substr(shown.size() - (size_t)fit);
        SDL_Rect tb{ L.composer.x + 4, L.composer.y + 3,
                     std::max(0, L.composer.w - 8), L.composer.h - 6 };
        app.mono.draw_fitted(r, tb, shown, placeholder ? t.dim : t.text, false, 1.f, .55f, false);
    }
    const bool sendable = !client_.busy() && !composer_.empty();
    fill_rect(r, L.sendBtn, sendable ? t.accent : t.keybg);
    frame_rect(r, L.sendBtn, t.dim);
    app.mono.draw_centered(r, L.sendBtn, kSendLabel, sendable ? t.keybg : t.dim);
}

//----------------------------------------------------------------------------
//  input
//----------------------------------------------------------------------------

void CsoundChatPanel::focusComposer(App& app) {
    composerFocused_ = true;
    app.begin_text(&composer_,
                   [&app]() { app.request_redraw(); },
                   [this, &app](bool commit) {
                       composerFocused_ = false;
                       if (commit && !composer_.empty()) {
                           const std::string prompt = composer_;
                           composer_.clear();
                           sendPrompt(app, prompt);
                       }
                       app.request_redraw();
                   });
}

bool CsoundChatPanel::on_mouse(App& app, const MouseEv& e, const SDL_Rect& panel) {
    const Layout L = layout(panel);

    // --- drags in flight take precedence over any containment test --------
    if (resizeDrag_) {
        if (!e.pressed) { resizeDrag_ = false; return true; }
        width_ = std::max(200, (panel.x + panel.w) - (e.x - resizeGrab_));
        app.request_redraw();
        return true;
    }
    if (scrollDrag_) {
        if (!e.pressed) { scrollDrag_ = false; return true; }
        const int maxSc      = maxScroll(L);
        const SDL_Rect thumb = thumbRect(L);
        const int span       = std::max(1, L.scrollbar.h - thumb.h);
        const int wantY      = e.y - L.scrollbar.y - scrollGrab_;
        scroll_ = maxSc > 0
                ? std::max(0, std::min(maxSc, (int)((int64_t)wantY * maxSc / span)))
                : 0;
        followTail_ = scroll_ >= maxSc - 2;
        app.request_redraw();
        return true;
    }

    if (!e.pressed || e.button != SDL_BUTTON_LEFT)
        return insideRect(panel, e.x, e.y);

    if (insideRect(L.grip, e.x, e.y)) {
        resizeDrag_ = true;
        resizeGrab_ = e.x - panel.x;
        app.request_redraw();
        return true;
    }
    if (insideRect(L.btnSettings, e.x, e.y)) {
        if (open_settings) open_settings();
        return true;
    }
    if (insideRect(L.btnNew, e.x, e.y)) {
        //  Clearing mid-generation would strand the reply still streaming in;
        //  NEW simply does nothing until the exchange is over (the button is
        //  drawn dimmed while busy).
        if (!client_.busy()) {
            entries_.clear();
            streamIdx_ = -1;
            scroll_ = 0;
            followTail_ = true;
            app.request_redraw();
        }
        return true;
    }
    if (!notice_.empty() && noticeBtn_.w > 0 && insideRect(noticeBtn_, e.x, e.y)) {
        if (open_settings) open_settings();
        return true;
    }
    if (insideRect(L.scrollbar, e.x, e.y)) {
        const SDL_Rect thumb = thumbRect(L);
        if (thumb.h > 0) {
            scrollGrab_ = (e.y >= thumb.y && e.y < thumb.y + thumb.h) ? e.y - thumb.y
                                                                      : thumb.h / 2;
            scrollDrag_ = true;
            const int maxSc = maxScroll(L);
            const int span  = std::max(1, L.scrollbar.h - thumb.h);
            const int wantY = e.y - L.scrollbar.y - scrollGrab_;
            scroll_ = maxSc > 0
                    ? std::max(0, std::min(maxSc, (int)((int64_t)wantY * maxSc / span)))
                    : 0;
            followTail_ = scroll_ >= maxSc - 2;
            app.request_redraw();
        }
        return true;
    }
    if (L.hasActions && insideRect(L.actions, e.x, e.y)) {
        if (client_.busy()) {
            client_.cancel();
        } else {
            sendPrompt(app, "The compile failed. Explain what is wrong and "
                            "give a corrected version of the code.");
        }
        app.request_redraw();
        return true;
    }
    if (insideRect(L.composer, e.x, e.y)) {
        focusComposer(app);
        app.request_redraw();
        return true;
    }
    if (insideRect(L.sendBtn, e.x, e.y)) {
        //  Clicking SEND while typing commits the edit first, then sends.
        if (composerFocused_) {
            app.end_text_if(&composer_);
            composerFocused_ = false;
        }
        if (!composer_.empty()) {
            const std::string prompt = composer_;
            composer_.clear();
            sendPrompt(app, prompt);
        }
        app.request_redraw();
        return true;
    }
    if (insideRect(L.content, e.x, e.y)) {
        forEachVisibleRow(L, [&](int idx, const Row& row, const SDL_Rect& rr) {
            if (!insideRect(rr, e.x, e.y)) return true;
            if (row.kind == Row::ThinkToggle) {
                Entry& en = entries_[(size_t)idx];
                en.thinkingOpen = !en.thinkingOpen;
                en.wrapW = -1;
                app.request_redraw();
                return false;
            }
            if (row.kind == Row::Apply && row.codeIdx >= 0) {
                const int cwl = fontW_ > 0 ? fontW_ : 7;
                const Entry& en = entries_[(size_t)idx];
                const bool unverified = en.checkIssues > 0;
                const std::string a = unverified ? "APPLY UNVERIFIED (REPLACE DOC)"
                                                 : "APPLY (REPLACE DOC)";
                const std::string b = "INSERT AT CURSOR";
                SDL_Rect ra{ rr.x + 4, rr.y + 2, (int)a.size() * cwl + 10, L.btnRowH };
                SDL_Rect rb{ ra.x + ra.w + 4, rr.y + 2, (int)b.size() * cwl + 10, L.btnRowH };
                if (row.codeIdx < (int)en.codeBlocks.size() && apply_code) {
                    if (insideRect(ra, e.x, e.y))
                        apply_code(app, en.codeBlocks[(size_t)row.codeIdx], true);
                    else if (insideRect(rb, e.x, e.y))
                        apply_code(app, en.codeBlocks[(size_t)row.codeIdx], false);
                }
                app.request_redraw();
                return false;
            }
            return true;
        });
        return true;   // clicks inside the panel never fall through to the editor
    }
    return insideRect(panel, e.x, e.y);
}

bool CsoundChatPanel::on_wheel(App& app, int dy, const SDL_Rect& panel) {
    int mx, my;
    mouse_logical(app, mx, my);
    if (!insideRect(panel, mx, my)) return false;
    const Layout L = layout(panel);
    const int before = scroll_;
    scroll_ = std::max(0, std::min(maxScroll(L), scroll_ - dy * 3 * L.rowH));
    followTail_ = scroll_ >= maxScroll(L) - 2;
    if (scroll_ != before) app.request_redraw();
    return true;
}

void CsoundChatPanel::onOpen(App& app) {
    if (!loaded_) reloadSettings();
    followTail_ = true;
    (void)app;
}

void CsoundChatPanel::onClose(App& app) {
    app.end_text_if(&composer_);
    composerFocused_ = false;
    scrollDrag_ = resizeDrag_ = false;
}

void CsoundChatPanel::cancel_interaction(App& app) {
    onClose(app);
}

} // namespace ui
