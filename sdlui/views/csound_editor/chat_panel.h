//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/chat_panel.h
//
//  The Claude chat panel docked on the right of the Csound editor, mirroring
//  the help drawer (same slide-out geometry, its own tab under the help tab;
//  opening one closes the other).  Owned by CsoundEditorView; not a Widget --
//  the view forwards draw/mouse/wheel into the panel rect it computes.
//
//  THREADING / PUMPING
//    pump() must run once per painted frame on the message thread, ALWAYS --
//    even while the panel (or the whole Csound window) is hidden.  The
//    ClaudeClient worker keeps producing events regardless of UI visibility;
//    if nobody drains them, a reply that lands after the user closes the
//    panel is stranded and the client never returns to idle in the UI's eyes.
//    main.cpp calls CsoundEditorView::pumpAi() from App::on_frame.
//
//  PERFORMANCE
//    The transcript is wrapped into display rows per entry, cached, and only
//    the rows inside the viewport are drawn (see forEachVisibleRow).  A
//    streaming delta re-wraps ONLY the entry it appended to.  Nothing here
//    repaints the document or lays anything out per frame when idle.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_CSOUND_CHAT_PANEL_H
#define PATCHKNOB_SDLUI_CSOUND_CHAT_PANEL_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../../gui.h"
#include "engine/ai/claude_client.h"
#include "engine/ai/api_key_store.h"

namespace ui {

class CsoundChatPanel {
public:
    CsoundChatPanel();
    ~CsoundChatPanel();

    // ---- host hooks (wired by CsoundEditorView / main.cpp) ---------------
    //! The LIVE editor buffer.  Queried at send time, every turn -- the
    //! panel never caches a copy of the document.
    std::function<std::string()> get_orc;
    //! The last compiler output; empty when the last compile succeeded.
    std::function<std::string()> get_errors;
    //! Apply a fenced code block through the editor's undo (one Ctrl+Z step).
    //! replaceWholeDoc: true = replace document, false = insert at cursor.
    std::function<void(App&, const std::string& code, bool replaceWholeDoc)> apply_code;
    //! Open the API key / model settings window (owned by the shell).
    std::function<void()> open_settings;
    //! Directory of the staged Csound manual ("" if not installed).  Feeds the
    //! opcode checker that vets every reply against the manual's signatures.
    std::function<std::string()> get_manual_dir;

    // ---- lifecycle -------------------------------------------------------
    //! Once per frame, message thread, unconditionally.  `shown` gates only
    //! the redraw requests that keep streaming smooth -- the poll itself
    //! always runs so a hidden panel still drains the worker.
    void pump(App& app, bool shown);
    //! Re-read the stored key + model/thinking/max-tokens prefs.  Called on
    //! first open and whenever the settings window changes something.
    void reloadSettings();
    void onOpen(App& app);              // panel became visible
    void onClose(App& app);             // panel hidden: end composer edit
    void cancel_interaction(App& app);

    // ---- geometry --------------------------------------------------------
    //! Panel width in px for the given editor area (drag-resizable, same
    //! clamping the help drawer uses).
    int widthFor(const SDL_Rect& area) const;

    // ---- UI (rects are the panel rect computed by the view) --------------
    void draw(App& app, const SDL_Rect& panel);
    bool on_mouse(App& app, const MouseEv& e, const SDL_Rect& panel);
    bool on_wheel(App& app, int dy, const SDL_Rect& panel);

    bool busy() const { return client_.busy(); }

    // ---- test seams (harness only; nothing in the app calls these) -------
    //! Feed one event exactly as pump() would deliver it from the worker.
    void injectEventForTest(const PatchKnob::ai::ChatEvent& ev) { handleEvent(ev); }
    //! Replace the key loader so tests can simulate Missing/ForeignMachine
    //! without touching the real key file.
    void setKeyLoaderForTest(std::function<PatchKnob::ai::KeyLoad()> f) { keyLoader_ = std::move(f); }
    //! Apply a compiled reply to the editor without being asked (default on).
    void set_auto_apply(bool on) { autoApply_ = on; }
    bool auto_apply() const { return autoApply_; }
    void sendForTest(App& app, const std::string& prompt) { sendPrompt(app, prompt); }
    std::string noticeForTest() const { return notice_; }
    int  rowsDrawnForTest() const { return rowsDrawn_; }
    int  entryCountForTest() const { return (int)entries_.size(); }
    std::string entryTextForTest(int i) const { return entries_[(size_t)i].text; }
    std::string entryThinkingForTest(int i) const { return entries_[(size_t)i].thinking; }
    bool entryStreamingForTest(int i) const { return entries_[(size_t)i].streaming; }
    bool entryErrorForTest(int i) const { return entries_[(size_t)i].error; }
    bool entryRateLimitForTest(int i) const { return entries_[(size_t)i].rateLimit; }
    bool entrySupersededForTest(int i) const { return entries_[(size_t)i].superseded; }
    int  entryCheckIssuesForTest(int i) const { return entries_[(size_t)i].checkIssues; }
    std::string entryCheckNoteForTest(int i) const { return entries_[(size_t)i].checkNote; }
    const std::vector<std::string>& entryCodeBlocksForTest(App& app, const SDL_Rect& panel, int i) {
        (void)app; ensureWrapped(entries_[(size_t)i], colsFor(layout(panel))); return entries_[(size_t)i].codeBlocks;
    }
    void toggleThinkingForTest(int i) { entries_[(size_t)i].thinkingOpen = !entries_[(size_t)i].thinkingOpen;
                                        entries_[(size_t)i].wrapW = -1; }

private:
    // ---- transcript model ------------------------------------------------
    struct Row {
        enum Kind : std::uint8_t { Header, Text, Code, Fence, ThinkToggle,
                                   ThinkText, Apply, Error, Note };
        Kind        kind;
        std::string text;
        int         codeIdx = -1;   // Apply rows: index into Entry::codeBlocks
    };
    struct Entry {
        bool        fromUser  = false;
        bool        error     = false;    // a transport/API error notice
        //  A rate limit is a WAIT, not a failure: same exclusion from history
        //  as an error, but drawn calm (dim) instead of alarming.
        bool        rateLimit = false;
        bool        streaming = false;    // the reply currently being received
        std::string text;
        std::string thinking;
        bool        thinkingOpen = false;
        std::string stopNote;             // e.g. "(cancelled)"
        //  The engine audits every reply's `csound` fences against the shipped
        //  manual and streams the verdict as Checking/Checked/Retry events.
        //  Transient progress with no content: "Thinking... (~1,200 tokens)".
        //  A reasoning model can think for over a minute before its first word,
        //  and the CLI's thinking deltas arrive EMPTY (encrypted), so without
        //  this the panel showed nothing at all and read as a hang.
        std::string statusNote;
        std::string checkNote;            // live audit status / verdict text
        //  The Csound compiler accepted this reply's code. Only a compiled
        //  reply is auto-applied to the editor: applying code that does not
        //  build would replace the user's working document with something
        //  broken, which is exactly the thing an automatic loop must not do.
        bool        compiled = false;
        int         checkIssues = -1;     // -1 unknown / not audited
        //  A correction round replaced this reply: keep it on screen for
        //  honesty, but it is NOT history (the model already superseded it)
        //  and its code must NOT be offered for Apply.
        bool        superseded = false;
        // wrap cache -- rebuilt only when `wrapW` differs or the text grew
        int                      wrapW = -1;      // columns the cache was built at
        std::vector<Row>         rows;
        std::vector<std::string> codeBlocks;      // closed csound/orc/csd fences
        int                      heightPx = 0;
    };

    struct Layout {
        SDL_Rect panel, grip, header, btnNew, btnSettings,
                 content, scrollbar, actions, composer, sendBtn;
        int rowH = 0, btnRowH = 0;
        bool hasActions = false;
    };

    Layout layout(const SDL_Rect& panel) const;
    int    colsFor(const Layout& L) const;
    void   ensureWrapped(Entry& e, int cols);
    int    rowHeightPx(const Row& r, const Layout& L) const;
    int    entryHeight(Entry& e, const Layout& L);   // wraps if stale
    int    totalHeight(const Layout& L);
    int    maxScroll(const Layout& L);
    SDL_Rect thumbRect(const Layout& L);
    //! Walk visible rows; f(entry index, row, screen rect). Shared by draw()
    //! and on_mouse() so hit tests and pixels can never disagree.
    void   forEachVisibleRow(const Layout& L,
                             const std::function<bool(int, const Row&, const SDL_Rect&)>& f);

    void   handleEvent(const PatchKnob::ai::ChatEvent& ev);
    void   finishStreaming(const std::string& stopNote);
    void   sendPrompt(App& app, const std::string& prompt);
    void   addErrorEntry(const std::string& msg);
    void   focusComposer(App& app);
    void   refreshNotice();
    bool   canChat() const;             // key Ok and HTTPS backend present
    void   scrollToTailIfFollowing(const Layout& L);

    PatchKnob::ai::ClaudeClient client_;
    //  AUTO-APPLY. The user asked for a hands-off loop: ask for an instrument
    //  and have it land in the editor. Only a reply the COMPILER accepted is
    //  applied -- pasting code that does not build would replace a working
    //  document with a broken one, which is the one thing an automatic loop
    //  must never do. handleEvent() has no App&, so the apply is staged here
    //  and performed in pump(), which does.
    bool        autoApply_ = true;
    std::string autoApplyCode_;
    bool        autoApplyReplace_ = false;

    std::function<PatchKnob::ai::KeyLoad()> keyLoader_;   // test seam; real loadApiKey by default
    PatchKnob::ai::KeyStatus keyStatus_ = PatchKnob::ai::KeyStatus::Missing;
    bool   httpOk_    = true;
    bool   loaded_    = false;          // reloadSettings() ran at least once
    std::string notice_;                // "no key" / foreign-machine / no-HTTPS text

    std::vector<Entry> entries_;
    int    streamIdx_ = -1;             // entry receiving deltas, -1 none

    int    scroll_     = 0;
    bool   followTail_ = true;
    bool   scrollDrag_ = false;
    int    scrollGrab_ = 0;
    int    width_      = 0;             // 0 = adaptive (same rule as the help drawer)
    bool   resizeDrag_ = false;
    int    resizeGrab_ = 0;

    std::string composer_;
    bool   composerFocused_ = false;
    SDL_Rect noticeBtn_ { 0, 0, 0, 0 }; // OPEN SETTINGS button inside a notice

    int    rowsDrawn_ = 0;              // last draw, for the culling test
    int    fontW_ = 0, fontH_ = 0;      // wrap cache validity vs font size
};

} // namespace ui

#endif // PATCHKNOB_SDLUI_CSOUND_CHAT_PANEL_H
