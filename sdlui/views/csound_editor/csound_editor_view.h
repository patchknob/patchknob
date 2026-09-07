//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/csound_editor_view.h
//
//  A minimal multi-line text editor (ui::Widget) for a Csound .csd document.
//  Character input arrives via App::text_input_sink; navigation / edit keys and
//  Ctrl+E (recompile) arrive via on_key.  The host wires on_recompile to compile
//  the buffer and push the CSD's new in/out counts to the patcher node.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_CSOUND_EDITOR_VIEW_H
#define PATCHKNOB_SDLUI_CSOUND_EDITOR_VIEW_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <litehtml.h>

#include "../../gui.h"
#include "../../html_container.h"

namespace ui {

#ifdef PATCHKNOB_HAS_AI
class CsoundChatPanel;
#endif

class CsoundEditorView : public Widget {
public:
    ~CsoundEditorView() override;
    void draw(App& app) override;
    bool on_key(App& app, SDL_Keycode k) override;
    bool on_mouse(App& app, const MouseEv& e) override;
    bool on_wheel(App& app, int dx, int dy) override;
    //! Local text-edit history (typing coalesced into bursts; Apply-from-chat
    //! is always exactly ONE step).  Returns false when its stack is empty so
    //! Ctrl+Z falls through to the project-wide undo, per the Widget contract.
    bool on_undo(App& app, bool redo) override;
    //! Ends any in-flight drag AND the find-in-page edit. The latter matters:
    //! App::text_target would otherwise stay pointed at helpSearch_ after this
    //! view stops being interactive (see App::end_text_if).
    void cancel_interaction(App& app) override;

    //! Insert literal characters at the cursor (from the App text sink).
    void insert(const char* utf8);

    void setText(const std::string& s);
    std::string text() const;
    void setStatus(const std::string& s, bool error) { status_ = s; statusError_ = error; }

    //! Fired on Ctrl+E: the host recompiles the CSD and updates node ports.
    std::function<void()> on_recompile;
    std::function<void()> on_open_file, on_save_file, on_save_file_as;

#ifdef PATCHKNOB_HAS_AI
    //! The Claude chat panel (created lazily; its ClaudeClient lives with it).
    //! main.cpp wires open_settings on it and calls pumpAi() once per frame.
    CsoundChatPanel& aiChat();
    //! Once per painted frame from App::on_frame, ALWAYS -- polling must keep
    //! draining the worker while the panel (or the whole window) is hidden.
    //! `shown` only gates the redraws that keep a visible stream smooth.
    void pumpAi(App& app, bool shown);
    //! Apply a chat code block through the local undo: exactly one Ctrl+Z
    //! restores the prior buffer.  replaceAll=false inserts at the cursor.
    void applyAiCode(App& app, const std::string& code, bool replaceAll);
#endif

private:
    bool hasSelection() const;
    void clearSelection();
    void deleteSelection();
    std::string selectedText() const;
    void insertText(const std::string& s);
    void setCursorFromMouse(App& app, int x, int y);
    void ensureVisible(int rows);
    std::vector<std::string> lines_{ std::string() };
    int cr_ = 0, cc_ = 0;     // cursor row / column (chars)
    int ar_ = 0, ac_ = 0;     // selection anchor
    bool mouseSelecting_ = false;
    int top_ = 0;             // first visible row
    //! Cursor row the last draw() scrolled to.  draw() only calls
    //! ensureVisible() when cr_ differs from this, so a wheel scroll away from
    //! the caret survives the next frame instead of being snapped back.
    int shownCr_ = -1, shownCc_ = -1;
    std::string status_;
    bool statusError_ = false;
    int  blink_ = 0;

    // ---- local text-edit history -----------------------------------------
    // Whole-buffer snapshots: a CSD is a few KB, so copying lines_ per edit
    // burst is cheap and restoring is EXACT by construction.  Plain typing /
    // backspace / delete runs coalesce into one step via `group`; structural
    // edits (paste, cut, Apply-from-chat) always snapshot.
    struct EditSnapshot {
        std::vector<std::string> lines;
        int cr, cc, ar, ac, top;
    };
    //! Snapshot the current state BEFORE an edit.  `group` != 0 coalesces
    //! with an immediately preceding edit of the same group.
    void pushUndo(int group);
    EditSnapshot captureSnapshot() const;
    void restoreSnapshot(const EditSnapshot& s);
    std::vector<EditSnapshot> undoStack_, redoStack_;
    int  lastEditGroup_ = 0;   // 0 = never coalesce with what follows

#ifdef PATCHKNOB_HAS_AI
    // ---- Claude chat panel (docked right, mirrors the help drawer) -------
    // Mutually exclusive with the help drawer: the "AI" tab sits under the
    // help tab and opening either closes the other.
    std::unique_ptr<CsoundChatPanel> chat_;
    bool chatOpen_ = false;
    SDL_Rect chatTabRect(App& app, const SDL_Rect& area) const;
    SDL_Rect chatPanelRect(App& app, const SDL_Rect& area) const;
#endif

    // ---- collapsible HTML help drawer: the full Csound Reference Manual ---
    // A narrow ">" / "<" tab on the right edge of the editor; clicking it
    // slides a litehtml-rendered panel out over the right side of the text
    // area (an overlay, not a reflow). The panel browses the vendored manual
    // mirror (vendor/csound-manual, staged next to the built executable --
    // see sdlui/CMakeLists.txt) with real <a> link navigation, a Back/Home
    // bar, find-in-page, word-wise selection with Ctrl+C, and a draggable
    // width. If the mirror was never fetched the drawer says so and points
    // at tools/fetch_csound_manual.py.
    //! `scrollbar` is the full trough; the thumb is derived from it and the
    //! document height (see helpThumbRect). `content` excludes the trough,
    //! and `resizeGrip` is the draggable left edge of the whole drawer.
    struct HelpLayout {
        SDL_Rect tab, panel, resizeGrip, navBack, navHome,
                 searchBox, searchPrev, searchNext, content, scrollbar;
    };
    bool helpOpen_ = false;
    int  helpScroll_ = 0;             // content scroll offset, px
    std::string manualRoot_;          // absolute dir of the staged manual, "" once probed-and-missing
    bool manualRootProbed_ = false;
    std::string helpCurrentPage_;     // absolute path of the page now loaded, "" for the baked-in fallback
    std::vector<std::string> helpHistory_;
    std::unique_ptr<HtmlContainer> helpContainer_;
    litehtml::document::ptr helpDoc_;
    bool helpMouseDown_ = false;      // a press landed in the content and litehtml owns the drag
    //! Rendered document height from the last draw(). on_mouse needs it to map
    //! a scrollbar drag onto a scroll offset, and only draw() knows it (it is
    //! whatever the last document::render() at the current width produced).
    int  helpDocHeight_ = 0;
    bool helpScrollDrag_ = false;
    int  helpScrollGrab_ = 0;         // pointer offset within the thumb when the drag began

    //! Drawer width in px. 0 means "not resized yet" -- computeHelpLayout
    //! then picks a width proportional to the view, so the default still
    //! adapts to the window until the user expresses a preference.
    int  helpWidth_ = 0;
    bool helpResizeDrag_ = false;
    int  helpResizeGrab_ = 0;         // pointer offset from the panel's left edge

    // ---- text selection (word granularity) -------------------------------
    // litehtml paints one text run per word, so selecting whole runs between
    // two points reads as ordinary word-wise selection. Anchor/head are in
    // DOCUMENT space (scroll-independent) so a drag survives scrolling.
    bool helpSelecting_ = false;      // a selection drag is in progress
    bool helpHasSel_ = false;
    SDL_Point helpSelAnchor_ { 0, 0 }, helpSelHead_ { 0, 0 };
    std::vector<SDL_Rect> helpSelRects_;   // document space
    std::string helpSelText_;

    // ---- find in page ----------------------------------------------------
    std::string helpSearch_;          // live-edited by App::begin_text
    bool helpSearchFocused_ = false;
    std::vector<SDL_Rect> helpMatches_;    // document space, reading order
    int  helpMatchIdx_ = -1;

    void ensureHelpOpened(App& app);                       // first-open: pick manual index or fallback
    void loadManualPage(App& app, const std::string& absPath, bool pushHistory);
    void loadHelpUnavailable(App& app);                     // "manual not installed" notice
    void navigateHelpBack(App& app);
    std::string findManualRoot() const;
    HelpLayout computeHelpLayout(App& app, const SDL_Rect& area) const;
    int  helpMaxScroll(const HelpLayout& hl) const;
    SDL_Rect helpThumbRect(const HelpLayout& hl) const;

    //! Every text run of the WHOLE current page, in document space and
    //! reading order -- not just the visible part, so find-in-page and a
    //! selection dragged past the viewport both work. Rebuilt by
    //! captureHelpRuns() whenever the page or the layout width changes.
    std::vector<HtmlContainer::TextRun> helpAllRuns_;
    int  helpCapturedWidth_ = -1;     // content width helpAllRuns_ was captured at
    bool helpRunsDirty_ = true;
    //! Viewport the document was last laid out for. litehtml's render() is a
    //! FULL layout pass -- running it per frame is what made a long manual
    //! page crawl -- so it only re-runs when the page or the box it has to
    //! fit actually changes. draw() itself needs no re-layout.
    int  helpRenderedW_ = -1, helpRenderedH_ = -1;
    bool helpSearchDirty_ = false;    // query edited; matches need rebuilding

    //! Composited band cache. litehtml's draw() walks the whole element tree
    //! and blits one texture PER WORD even when nothing changed -- ~30 ms per
    //! frame on the manual's index page, paid on EVERY redraw while the drawer
    //! was open (so typing in the editor with the drawer out cost 30 ms per
    //! keystroke). The document is now composited once into a render-target
    //! texture covering a band of ~3 viewports around the scroll position;
    //! ordinary frames blit one rectangle from it. The band re-renders only
    //! when the scroll leaves it, the page/layout changes, or litehtml itself
    //! asks for a repaint (link press state). Selection/match highlights are
    //! painted OVER the blit each frame, so they never invalidate the band.
    //! Null when the renderer has no target-texture support -- the code then
    //! falls back to direct drawing (the old path).
    SDL_Texture*  helpBandTex_ = nullptr;
    SDL_Renderer* helpBandRen_ = nullptr;  // renderer the texture belongs to
    int  helpBandTop_ = 0;                 // document-space y of the band's first row
    int  helpBandW_ = 0, helpBandH_ = 0;   // allocated texture size
    bool helpBandDirty_ = true;            // content must be re-composited
    bool helpBandUnsupported_ = false;     // CreateTexture(TARGET) failed once

    void captureHelpRuns(const HelpLayout& hl);
    //! Reading-order index of the run at a document-space point, or the last
    //! run before it. -1 when the page has no text at all.
    int  helpRunIndexAt(const SDL_Point& docPt) const;
    void rebuildHelpSelection();
    void rebuildHelpMatches();
    void clearHelpSelection();
    void copyHelpSelection();
    void focusHelpSearch(App& app);
    void stepHelpMatch(App& app, const HelpLayout& hl, int delta);
};

} // namespace ui

#endif
