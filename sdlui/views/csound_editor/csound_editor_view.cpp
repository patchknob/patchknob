//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/csound_editor_view.cpp
//----------------------------------------------------------------------------
#include "csound_editor_view.h"
#include "platform/platform_ui.h"
#ifdef PATCHKNOB_HAS_AI
#include "chat_panel.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace ui {

namespace {

// The manual is ASCII, and matching it case-insensitively is all find-in-page
// needs -- no locale, no UTF-8 case folding.
std::string lower_ascii(const std::string& s) {
    std::string out(s);
    for (char& c : out) c = (char) std::tolower((unsigned char) c);
    return out;
}

SDL_Rect union_rect(const SDL_Rect& a, const SDL_Rect& b) {
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return SDL_Rect{ x0, y0, x1 - x0, y1 - y0 };
}

std::string exe_dir() {
    // Ownership differs between SDL2 and SDL3 -- same split the rack editor's
    // pack loader documents (views/rack_editor/cardinal_svg_textures.cpp):
    // SDL2 hands back a buffer the caller frees, SDL3 owns its storage and
    // freeing it is a crash.
#ifdef PATCHKNOB_SDL3
    const char* base = SDL_GetBasePath();
    return base ? base : std::string();
#else
    char* base = SDL_GetBasePath();
    std::string s = base ? base : "";
    if (base) SDL_free(base);
    return s;
#endif
}

bool dir_has_index(const std::string& dir) {
    std::ifstream probe(dir + "index.html", std::ios::binary);
    return probe.good();
}

// Injected on top of each manual page. The pages are styled for a browser on
// a white canvas, so they set text colors but largely assume the default
// background -- dropping them onto the app's own panel color renders black
// text on near-black in MIDNIGHT mode. Pinning the canvas here keeps the
// manual legible in both themes, and caps the body width so long lines stay
// readable in a narrow drawer.
const char* kManualUserCss = R"CSS(
html, body { background-color: #ffffff !important; color: #1a1a1a !important; }
body { margin: 0 !important; padding: 8px 10px !important; }
img { max-width: 100%; }
)CSS";

// Shown when vendor/csound-manual was never fetched. Deliberately a short
// pointer at the fetch script rather than a hand-written mini-reference:
// a partial copy of the manual would just be a second thing to maintain and
// disagree with the real one.
const char* kManualMissingHtml = R"HTML(<html><body>
<h3>Csound manual not installed</h3>
<p>The reference manual is not bundled with this build. Fetch it with:</p>
<p><code>python3 tools/fetch_csound_manual.py</code></p>
<p>It lands in <code>vendor/csound-manual</code>. Press HOME here once it has
finished and the drawer will pick it up.</p>
</body></html>
)HTML";

} // namespace

CsoundEditorView::~CsoundEditorView() {
    // Same lifecycle HtmlContainer uses for its texture caches: the texture
    // belongs to the renderer that built it and is released with the view.
    if (helpBandTex_) SDL_DestroyTexture(helpBandTex_);
}

#ifdef PATCHKNOB_HAS_AI
// ---- Claude chat panel ------------------------------------------------------

CsoundChatPanel& CsoundEditorView::aiChat() {
    if (!chat_) {
        chat_ = std::make_unique<CsoundChatPanel>();
        //  The EDITOR is the source of truth: the panel queries the live
        //  buffer and the live compile status at send time, never a copy.
        chat_->get_orc    = [this]() { return text(); };
        chat_->get_errors = [this]() {
            return statusError_ ? status_ : std::string();
        };
        chat_->apply_code = [this](App& app, const std::string& code, bool replaceAll) {
            applyAiCode(app, code, replaceAll);
        };
        //  Same manual the help drawer browses; turns on the opcode audit.
        chat_->get_manual_dir = [this]() {
            if (!manualRootProbed_) { manualRoot_ = findManualRoot(); manualRootProbed_ = true; }
            return manualRoot_;
        };
    }
    return *chat_;
}

void CsoundEditorView::pumpAi(App& app, bool shown) {
    //  Until the panel has ever been created nothing can be in flight, so
    //  there is nothing to poll; once it exists, poll EVERY frame regardless
    //  of visibility so a reply finishing off-screen is drained, not stranded.
    if (chat_) chat_->pump(app, shown && chatOpen_);
}

void CsoundEditorView::applyAiCode(App& app, const std::string& code, bool replaceAll) {
    pushUndo(0);                       // exactly one Ctrl+Z takes this back
    if (replaceAll) {
        lines_.clear();
        std::string cur;
        for (char c : code) {
            if (c == '\r') continue;
            if (c == '\n') { lines_.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        lines_.push_back(cur);
        cr_ = cc_ = 0;
        clearSelection();
        top_ = 0;
        shownCr_ = shownCc_ = -1;
    } else {
        insertText(code);
    }
    lastEditGroup_ = 0;                // the next keystroke starts fresh
    setStatus("applied from chat  -  Ctrl+E to compile", false);
    app.request_redraw();
}

SDL_Rect CsoundEditorView::chatTabRect(App& app, const SDL_Rect& area) const {
    //  Directly under the help tab, same size, so the two read as a pair.
    (void)app;
    const int sz = 22;
    return SDL_Rect{ area.x + area.w - sz - 4, area.y + 4 + sz + 2, sz, sz };
}

SDL_Rect CsoundEditorView::chatPanelRect(App& app, const SDL_Rect& area) const {
    const int statusH = app.mono.ch() + 8;
    const SDL_Rect tab = chatTabRect(app, area);
    const int top = tab.y + tab.h + 2;
    const int w = chat_ ? chat_->widthFor(area)
                        : std::max(200, std::min(440, area.w * 2 / 5));
    return SDL_Rect{ area.x + area.w - w, top, w,
                     std::max(0, area.y + area.h - statusH - top) };
}
#endif // PATCHKNOB_HAS_AI

std::string CsoundEditorView::findManualRoot() const {
    // Deployed layout first (the manual staged beside the executable), then
    // the in-tree vendor copy so a dev build finds it without a 33 MB copy
    // step on every build. PATCHKNOB_CSOUND_MANUAL_DIR is set by
    // sdlui/CMakeLists.txt and is absent from installed builds.
    const std::string beside = exe_dir() + "csound-manual/";
    if (dir_has_index(beside)) return beside;
#ifdef PATCHKNOB_CSOUND_MANUAL_DIR
    const std::string vendored = std::string(PATCHKNOB_CSOUND_MANUAL_DIR) + "/";
    if (dir_has_index(vendored)) return vendored;
#endif
    return {};
}

CsoundEditorView::HelpLayout CsoundEditorView::computeHelpLayout(App& app, const SDL_Rect& area) const {
    HelpLayout hl;
    const int sz = 22;
    hl.tab = SDL_Rect{ area.x + area.w - sz - 4, area.y + 4, sz, sz };
    const int statusH = app.mono.ch() + 8;
    // Until the user drags the grip the width tracks the view, so the drawer
    // is sensible at any window size; after that their choice wins (clamped
    // so it can never swallow the editor or collapse to nothing).
    const int wantW = helpWidth_ > 0 ? helpWidth_ : std::max(200, std::min(420, area.w * 2 / 5));
    const int w = std::max(180, std::min(wantW, std::max(180, area.w - 80)));
    const int panelTop = hl.tab.y + hl.tab.h + 2;
    hl.panel = SDL_Rect{ area.x + area.w - w, panelTop, w, std::max(0, area.y + area.h - statusH - panelTop) };

    const int gripW = 5;
    hl.resizeGrip = SDL_Rect{ hl.panel.x, hl.panel.y, gripW, hl.panel.h };
    const int innerX = hl.panel.x + gripW;
    const int innerW = std::max(0, w - gripW - 1);
    const int navH = app.mono.ch() + 8;

    const int halfW = innerW / 2;
    hl.navBack = SDL_Rect{ innerX, hl.panel.y + 1, halfW, navH };
    hl.navHome = SDL_Rect{ innerX + halfW, hl.panel.y + 1, innerW - halfW, navH };

    const int row2Y = hl.panel.y + 1 + navH + 1;
    const int btnW = 20;
    hl.searchNext = SDL_Rect{ innerX + innerW - btnW, row2Y, btnW, navH };
    hl.searchPrev = SDL_Rect{ innerX + innerW - 2 * btnW, row2Y, btnW, navH };
    hl.searchBox  = SDL_Rect{ innerX, row2Y, std::max(0, innerW - 2 * btnW - 1), navH };

    const int barW = 8;
    const int bodyY = row2Y + navH + 2;
    const int bodyH = std::max(0, hl.panel.y + hl.panel.h - bodyY - 1);
    hl.content   = SDL_Rect{ innerX, bodyY, std::max(0, innerW - barW), bodyH };
    hl.scrollbar = SDL_Rect{ innerX + innerW - barW, bodyY, barW, bodyH };
    return hl;
}

int CsoundEditorView::helpMaxScroll(const HelpLayout& hl) const {
    return std::max(0, helpDocHeight_ - hl.content.h);
}

SDL_Rect CsoundEditorView::helpThumbRect(const HelpLayout& hl) const {
    const int maxScroll = helpMaxScroll(hl);
    if (maxScroll <= 0 || hl.scrollbar.h <= 0) return SDL_Rect{ 0, 0, 0, 0 };
    const int trackH = hl.scrollbar.h;
    // Proportional thumb, floored so it stays grabbable on a 1700-page doc.
    const int thumbH = std::max(20, (int) ((int64_t) trackH * hl.content.h / std::max(1, helpDocHeight_)));
    const int span   = std::max(1, trackH - thumbH);
    const int y      = hl.scrollbar.y + (int) ((int64_t) helpScroll_ * span / maxScroll);
    return SDL_Rect{ hl.scrollbar.x, y, hl.scrollbar.w, thumbH };
}

void CsoundEditorView::captureHelpRuns(const HelpLayout& hl) {
    helpAllRuns_.clear();
    if (!helpDoc_ || !helpContainer_) return;
    // A position-only pass over the WHOLE document (origin 0,0, clip the full
    // rendered height), so the run list covers the parts that are scrolled
    // out of view too. set_capture_only makes this cost layout-walk time and
    // nothing else -- no textures, no draw calls.
    helpContainer_->begin_frame();
    helpContainer_->set_capture_only(true);
    litehtml::position all(0, 0, (litehtml::pixel_t) hl.content.w,
                           (litehtml::pixel_t) std::max(1, helpDocHeight_));
    helpDoc_->draw(0, 0, 0, &all);
    helpContainer_->set_capture_only(false);
    helpContainer_->end_frame();
    helpAllRuns_ = helpContainer_->text_runs();
}

int CsoundEditorView::helpRunIndexAt(const SDL_Point& p) const {
    if (helpAllRuns_.empty()) return -1;
    int best = 0;
    for (size_t i = 0; i < helpAllRuns_.size(); ++i) {
        const HtmlContainer::TextRun& r = helpAllRuns_[i];
        if (p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h) return (int) i;
        // Not on a run: remember the last one that reads BEFORE this point,
        // so a click in the margin still anchors somewhere sensible.
        const bool above    = (r.y + r.h) <= p.y;
        const bool sameLine = p.y >= r.y && p.y < r.y + r.h;
        if (above || (sameLine && r.x + r.w / 2 <= p.x)) best = (int) i;
    }
    return best;
}

void CsoundEditorView::clearHelpSelection() {
    helpHasSel_ = false;
    helpSelecting_ = false;
    helpSelRects_.clear();
    helpSelText_.clear();
}

void CsoundEditorView::rebuildHelpSelection() {
    helpSelRects_.clear();
    helpSelText_.clear();
    if (!helpHasSel_ || helpAllRuns_.empty()) return;
    int a = helpRunIndexAt(helpSelAnchor_);
    int b = helpRunIndexAt(helpSelHead_);
    if (a < 0 || b < 0) return;
    if (a > b) std::swap(a, b);
    int prevY = 0, prevH = 0;
    bool first = true;
    for (int i = a; i <= b; ++i) {
        const HtmlContainer::TextRun& r = helpAllRuns_[i];
        // Whitespace-only runs (litehtml emits them for underlined text) have
        // no glyphs to highlight and would double the separators below.
        if (r.text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        helpSelRects_.push_back(SDL_Rect{ r.x, r.y, r.w, r.h });
        if (!first) helpSelText_ += (r.y > prevY + prevH / 2) ? "\n" : " ";
        helpSelText_ += r.text;
        prevY = r.y;
        prevH = r.h;
        first = false;
    }
}

void CsoundEditorView::copyHelpSelection() {
    if (!helpSelText_.empty()) SDL_SetClipboardText(helpSelText_.c_str());
}

void CsoundEditorView::rebuildHelpMatches() {
    const SDL_Rect keep = (helpMatchIdx_ >= 0 && helpMatchIdx_ < (int) helpMatches_.size())
                        ? helpMatches_[helpMatchIdx_] : SDL_Rect{ 0, 0, 0, 0 };
    helpMatches_.clear();
    helpMatchIdx_ = -1;
    if (helpSearch_.empty() || helpAllRuns_.empty()) return;
    const std::string q = lower_ascii(helpSearch_);
    if (q.find_first_not_of(" \t") == std::string::npos) return;

    // Runs are per-word, so a multi-word query can only be found by searching
    // a whole line at once: rebuild each line's text with single spaces, note
    // where each run landed in it, then map a hit back onto the runs it spans.
    size_t i = 0;
    while (i < helpAllRuns_.size()) {
        const int lineY = helpAllRuns_[i].y;
        const int tol   = std::max(4, helpAllRuns_[i].h / 2);
        std::string line;
        std::vector<std::pair<size_t, size_t>> spans;   // [begin,end) per run
        size_t j = i;
        while (j < helpAllRuns_.size() && std::abs(helpAllRuns_[j].y - lineY) <= tol) {
            if (!line.empty()) line += ' ';
            const size_t begin = line.size();
            line += lower_ascii(helpAllRuns_[j].text);
            spans.emplace_back(begin, line.size());
            ++j;
        }
        for (size_t pos = line.find(q); pos != std::string::npos; pos = line.find(q, pos + q.size())) {
            const size_t end = pos + q.size();
            SDL_Rect m{ 0, 0, 0, 0 };
            bool any = false;
            for (size_t k = 0; k < spans.size(); ++k) {
                if (spans[k].first >= end || spans[k].second <= pos) continue;
                const HtmlContainer::TextRun& r = helpAllRuns_[i + k];
                const SDL_Rect rr{ r.x, r.y, r.w, r.h };
                m = any ? union_rect(m, rr) : rr;
                any = true;
            }
            if (any) helpMatches_.push_back(m);
        }
        i = j;
    }
    if (helpMatches_.empty()) return;
    // Keep the reader where they were: after an edit that only extends the
    // query, land on the match nearest the one they were already looking at.
    helpMatchIdx_ = 0;
    if (keep.h > 0) {
        int bestDist = std::numeric_limits<int>::max();
        for (size_t k = 0; k < helpMatches_.size(); ++k) {
            const int d = std::abs(helpMatches_[k].y - keep.y);
            if (d < bestDist) { bestDist = d; helpMatchIdx_ = (int) k; }
        }
    }
}

void CsoundEditorView::stepHelpMatch(App& app, const HelpLayout& hl, int delta) {
    if (helpMatches_.empty()) return;
    const int n = (int) helpMatches_.size();
    helpMatchIdx_ = helpMatchIdx_ < 0 ? (delta >= 0 ? 0 : n - 1)
                                      : ((helpMatchIdx_ + delta) % n + n) % n;
    // Park the hit a third of the way down rather than at the very top, so
    // the surrounding sentence is visible with it.
    const SDL_Rect m = helpMatches_[helpMatchIdx_];
    helpScroll_ = std::max(0, std::min(helpMaxScroll(hl), m.y - hl.content.h / 3));
    app.request_redraw();
}

void CsoundEditorView::focusHelpSearch(App& app) {
    helpSearchFocused_ = true;
    app.begin_text(&helpSearch_,
                   [this, &app]() {
                       // Matches are rebuilt in draw(), which is the only
                       // place the layout (and so the run capture) is known.
                       helpSearchDirty_ = true;
                       app.request_redraw();
                   },
                   [this, &app](bool commit) {
                       helpSearchFocused_ = false;
                       if (!commit) {           // Esc: abandon the search entirely
                           helpSearch_.clear();
                           helpMatches_.clear();
                           helpMatchIdx_ = -1;
                       }
                       helpSearchDirty_ = true;
                       app.request_redraw();
                   });
}

void CsoundEditorView::loadHelpUnavailable(App& app) {
    if (!helpContainer_) helpContainer_ = std::make_unique<HtmlContainer>(app.ren);
    helpContainer_->set_renderer(app.ren);
    // No document dir: nothing in this notice resolves to a file, and leaving
    // the previous page's dir set would let its relative links still resolve.
    helpContainer_->set_document_dir(std::string());
    helpDoc_ = litehtml::document::createFromString(kManualMissingHtml, helpContainer_.get(),
                                                   litehtml::master_css, kManualUserCss);
    helpCurrentPage_.clear();
    helpHistory_.clear();
    helpScroll_ = 0;
    helpRunsDirty_ = true;
    clearHelpSelection();
    helpSearchDirty_ = true;
}

void CsoundEditorView::loadManualPage(App& app, const std::string& absPath, bool pushHistory) {
    if (!helpContainer_) helpContainer_ = std::make_unique<HtmlContainer>(app.ren);
    helpContainer_->set_renderer(app.ren);

    std::ifstream f(absPath, std::ios::binary);
    std::string html;
    if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        html = ss.str();
    } else {
        html = "<html><body style=\"font-family:sans-serif;padding:12px\">"
               "<h3>Page not found</h3><p>" + absPath + "</p></body></html>";
    }
    const size_t slash = absPath.find_last_of("/\\");
    helpContainer_->set_document_dir(slash == std::string::npos ? std::string() : absPath.substr(0, slash));
    helpDoc_ = litehtml::document::createFromString(html, helpContainer_.get(),
                                                   litehtml::master_css, kManualUserCss);
    helpCurrentPage_ = absPath;
    helpScroll_ = 0;
    helpMouseDown_ = false;   // the click that got us here ended on the OLD document
    // Run positions, selection and match rects all belong to the page we just
    // navigated away from.
    helpRunsDirty_ = true;
    clearHelpSelection();
    helpSearchDirty_ = true;
    if (pushHistory) {
        helpHistory_.push_back(absPath);
        // A long browse would otherwise retain every page ever visited.
        constexpr size_t kMaxHistory = 64;
        if (helpHistory_.size() > kMaxHistory)
            helpHistory_.erase(helpHistory_.begin(), helpHistory_.begin() + (helpHistory_.size() - kMaxHistory));
    }
}

void CsoundEditorView::navigateHelpBack(App& app) {
    if (helpHistory_.size() < 2) return;
    helpHistory_.pop_back();               // drop the current page
    const std::string prev = helpHistory_.back();
    helpHistory_.pop_back();               // loadManualPage(..., true) re-pushes it
    loadManualPage(app, prev, true);
}

void CsoundEditorView::ensureHelpOpened(App& app) {
    if (helpDoc_) return;
    if (!manualRootProbed_) { manualRoot_ = findManualRoot(); manualRootProbed_ = true; }
    if (!manualRoot_.empty()) loadManualPage(app, manualRoot_ + "index.html", true);
    else loadHelpUnavailable(app);
}

// ---- local text-edit history ----------------------------------------------

CsoundEditorView::EditSnapshot CsoundEditorView::captureSnapshot() const {
    EditSnapshot s;
    s.lines = lines_;
    s.cr = cr_; s.cc = cc_; s.ar = ar_; s.ac = ac_; s.top = top_;
    return s;
}

void CsoundEditorView::restoreSnapshot(const EditSnapshot& s) {
    lines_ = s.lines;
    if (lines_.empty()) lines_.push_back(std::string());
    cr_ = std::max(0, std::min(s.cr, (int)lines_.size() - 1));
    cc_ = std::max(0, std::min(s.cc, (int)lines_[(size_t)cr_].size()));
    ar_ = std::max(0, std::min(s.ar, (int)lines_.size() - 1));
    ac_ = std::max(0, std::min(s.ac, (int)lines_[(size_t)ar_].size()));
    top_ = std::max(0, std::min(s.top, (int)lines_.size() - 1));
    shownCr_ = shownCc_ = -1;      // let draw() re-follow the restored caret
}

void CsoundEditorView::pushUndo(int group) {
    //  Any NEW edit invalidates the redo line, coalesced or not.
    redoStack_.clear();
    if (group != 0 && group == lastEditGroup_ && !undoStack_.empty()) return;
    undoStack_.push_back(captureSnapshot());
    constexpr size_t kMaxUndo = 200;
    if (undoStack_.size() > kMaxUndo)
        undoStack_.erase(undoStack_.begin(),
                         undoStack_.begin() + (long)(undoStack_.size() - kMaxUndo));
    lastEditGroup_ = group;
}

bool CsoundEditorView::on_undo(App& app, bool redo) {
    std::vector<EditSnapshot>& from = redo ? redoStack_ : undoStack_;
    std::vector<EditSnapshot>& to   = redo ? undoStack_ : redoStack_;
    //  Empty history: return false so Ctrl+Z keeps meaning PROJECT undo, the
    //  documented default for views without a local history to offer.
    if (from.empty()) return false;
    to.push_back(captureSnapshot());
    const EditSnapshot s = from.back();
    from.pop_back();
    restoreSnapshot(s);
    lastEditGroup_ = 0;
    app.request_redraw();
    return true;
}

void CsoundEditorView::setText(const std::string& s) {
    //  A rebind (opening a file, switching node) is not an edit: the history
    //  of the previous document must not leak into this one.
    undoStack_.clear();
    redoStack_.clear();
    lastEditGroup_ = 0;
    lines_.clear();
    std::string cur;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') { lines_.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    lines_.push_back(cur);
    if (lines_.empty()) lines_.push_back(std::string());
    cr_ = std::min(cr_, (int)lines_.size() - 1);
    cc_ = std::min(cc_, (int)lines_[cr_].size());
    clearSelection();
    top_ = 0;
}

std::string CsoundEditorView::text() const {
    std::string s;
    for (size_t i = 0; i < lines_.size(); ++i) { if (i) s.push_back('\n'); s += lines_[i]; }
    return s;
}

bool CsoundEditorView::hasSelection() const {
    return cr_ != ar_ || cc_ != ac_;
}

void CsoundEditorView::clearSelection() {
    ar_ = cr_;
    ac_ = cc_;
}

std::string CsoundEditorView::selectedText() const {
    if (!hasSelection()) return {};
    int r0 = ar_, c0 = ac_, r1 = cr_, c1 = cc_;
    if (std::pair<int,int>(r1, c1) < std::pair<int,int>(r0, c0)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    if (r0 == r1) return lines_[(size_t)r0].substr((size_t)c0, (size_t)(c1 - c0));
    std::string out = lines_[(size_t)r0].substr((size_t)c0);
    out.push_back('\n');
    for (int r = r0 + 1; r < r1; ++r) {
        out += lines_[(size_t)r];
        out.push_back('\n');
    }
    out += lines_[(size_t)r1].substr(0, (size_t)c1);
    return out;
}

void CsoundEditorView::deleteSelection() {
    if (!hasSelection()) return;
    int r0 = ar_, c0 = ac_, r1 = cr_, c1 = cc_;
    if (std::pair<int,int>(r1, c1) < std::pair<int,int>(r0, c0)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }
    const std::string tail = lines_[(size_t)r1].substr((size_t)c1);
    lines_[(size_t)r0].erase((size_t)c0);
    lines_[(size_t)r0] += tail;
    if (r1 > r0)
        lines_.erase(lines_.begin() + r0 + 1, lines_.begin() + r1 + 1);
    cr_ = r0;
    cc_ = c0;
    clearSelection();
}

void CsoundEditorView::insertText(const std::string& s) {
    deleteSelection();
    std::string normalized;
    normalized.reserve(s.size());
    for (char c : s) if (c != '\r') normalized.push_back(c);

    const size_t firstBreak = normalized.find('\n');
    if (firstBreak == std::string::npos) {
        lines_[(size_t)cr_].insert((size_t)cc_, normalized);
        cc_ += (int)normalized.size();
        clearSelection();
        return;
    }

    const std::string tail = lines_[(size_t)cr_].substr((size_t)cc_);
    lines_[(size_t)cr_].erase((size_t)cc_);
    size_t begin = 0;
    int row = cr_;
    while (true) {
        const size_t end = normalized.find('\n', begin);
        const std::string part = normalized.substr(begin, end - begin);
        if (begin == 0) lines_[(size_t)row] += part;
        else {
            lines_.insert(lines_.begin() + ++row, part);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    lines_[(size_t)row] += tail;
    cr_ = row;
    cc_ = (int)(lines_[(size_t)row].size() - tail.size());
    clearSelection();
}

void CsoundEditorView::insert(const char* utf8) {
    if (!utf8 || !*utf8) return;
    pushUndo(1);           // typing burst: coalesces with adjacent keystrokes
    insertText(utf8);
}

void CsoundEditorView::ensureVisible(int rows) {
    if (rows < 1) rows = 1;
    if (cr_ < top_) top_ = cr_;
    else if (cr_ >= top_ + rows) top_ = cr_ - rows + 1;
    if (top_ < 0) top_ = 0;
}

bool CsoundEditorView::on_key(App& app, SDL_Keycode k) {
    const bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;

    // The help drawer takes the keys that only make sense while it is open.
    // Ctrl+C is shared: it copies from the drawer ONLY when the drawer owns a
    // selection, so the editor's own copy is untouched the rest of the time.
    // (While the find field is being edited App::begin_text swallows keys
    // before they ever reach a widget, so none of this fights with typing.)
    if (helpOpen_) {
        if (ctrl && k == SDLK_f) { focusHelpSearch(app); app.request_redraw(); return true; }
        if (ctrl && k == SDLK_c && helpHasSel_ && !helpSelText_.empty()) { copyHelpSelection(); return true; }
        if (k == SDLK_F3 || (ctrl && k == SDLK_g)) {
            stepHelpMatch(app, computeHelpLayout(app, rect), shift ? -1 : +1);
            return true;
        }
    }

    if (ctrl && (k == SDLK_e)) { if (on_recompile) on_recompile(); return true; }
    if (ctrl && k == SDLK_a) {
        ar_ = 0; ac_ = 0;
        cr_ = (int)lines_.size() - 1;
        cc_ = (int)lines_.back().size();
        return true;
    }
    if (ctrl && (k == SDLK_c || k == SDLK_x)) {
        if (hasSelection()) {
            const std::string copy = selectedText();
            SDL_SetClipboardText(copy.c_str());
            if (k == SDLK_x) { pushUndo(0); deleteSelection(); }
        }
        return true;
    }
    if (ctrl && k == SDLK_v) {
        char* clip = SDL_GetClipboardText();
        if (clip) { if (*clip) pushUndo(0); insertText(clip); SDL_free(clip); }
        return true;
    }
    //  Moving the caret ends a typing burst: the next edit gets its own undo
    //  step instead of coalescing across the jump.
    switch (k) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_PAGEUP: case SDLK_PAGEDOWN:
            lastEditGroup_ = 0;
            break;
        default: break;
    }
    std::string& ln = lines_[cr_];
    switch (k) {
        case SDLK_RETURN: case SDLK_KP_ENTER: {
            pushUndo(1);
            if (hasSelection()) deleteSelection();
            std::string& current = lines_[cr_];
            std::string tail = current.substr((size_t)std::min(cc_, (int)current.size()));
            current.erase((size_t)std::min(cc_, (int)current.size()));
            lines_.insert(lines_.begin() + cr_ + 1, tail);
            cr_++; cc_ = 0; clearSelection(); return true;
        }
        case SDLK_BACKSPACE:
            if (hasSelection()) { pushUndo(0); deleteSelection(); return true; }
            if (cc_ > 0 || cr_ > 0) pushUndo(2);
            if (cc_ > 0) { ln.erase((size_t)(cc_ - 1), 1); cc_--; }
            else if (cr_ > 0) { cc_ = (int)lines_[cr_ - 1].size();
                                lines_[cr_ - 1] += ln; lines_.erase(lines_.begin() + cr_); cr_--; }
            clearSelection();
            return true;
        case SDLK_DELETE:
            if (hasSelection()) { pushUndo(0); deleteSelection(); return true; }
            if (cc_ < (int)ln.size() || cr_ + 1 < (int)lines_.size()) pushUndo(3);
            if (cc_ < (int)ln.size()) ln.erase((size_t)cc_, 1);
            else if (cr_ + 1 < (int)lines_.size()) { ln += lines_[cr_ + 1]; lines_.erase(lines_.begin() + cr_ + 1); }
            clearSelection();
            return true;
        case SDLK_TAB: pushUndo(1); insertText("  "); return true;
        case SDLK_LEFT:
            if (!shift && hasSelection()) {
                if (std::pair<int,int>(ar_, ac_) < std::pair<int,int>(cr_, cc_)) { cr_ = ar_; cc_ = ac_; }
            } else if (cc_ > 0) cc_--; else if (cr_ > 0) { cr_--; cc_ = (int)lines_[cr_].size(); }
            if (!shift) clearSelection();
            return true;
        case SDLK_RIGHT:
            if (!shift && hasSelection()) {
                if (std::pair<int,int>(ar_, ac_) > std::pair<int,int>(cr_, cc_)) { cr_ = ar_; cc_ = ac_; }
            } else if (cc_ < (int)lines_[cr_].size()) cc_++; else if (cr_ + 1 < (int)lines_.size()) { cr_++; cc_ = 0; }
            if (!shift) clearSelection();
            return true;
        case SDLK_UP:    if (cr_ > 0) { cr_--; cc_ = std::min(cc_, (int)lines_[cr_].size()); } if (!shift) clearSelection(); return true;
        case SDLK_DOWN:  if (cr_ + 1 < (int)lines_.size()) { cr_++; cc_ = std::min(cc_, (int)lines_[cr_].size()); } if (!shift) clearSelection(); return true;
        case SDLK_HOME:  cc_ = 0; if (!shift) clearSelection(); return true;
        case SDLK_END:   cc_ = (int)lines_[cr_].size(); if (!shift) clearSelection(); return true;
        case SDLK_PAGEUP:   cr_ = std::max(0, cr_ - 20); cc_ = std::min(cc_, (int)lines_[cr_].size()); if (!shift) clearSelection(); return true;
        case SDLK_PAGEDOWN: cr_ = std::min((int)lines_.size() - 1, cr_ + 20); cc_ = std::min(cc_, (int)lines_[cr_].size()); if (!shift) clearSelection(); return true;
        default: break;
    }
    // swallow plain character keys so they don't trigger app shortcuts (the actual
    // glyphs arrive via the text-input sink); let unhandled combos pass through.
    if (!ctrl && k >= 0x20 && k < 0x7f) return true;
    return false;
}

bool CsoundEditorView::on_wheel(App& app, int dx, int dy) {
#ifdef PATCHKNOB_HAS_AI
    if (chatOpen_ && chat_ && chat_->on_wheel(app, dy, chatPanelRect(app, rect)))
        return true;
#endif
    if (helpOpen_) {
        int mx, my;
        mouse_logical(app, mx, my);
        const SDL_Rect panel = computeHelpLayout(app, rect).panel;
        if (mx >= panel.x && mx < panel.x + panel.w && my >= panel.y && my < panel.y + panel.h) {
            const int before = helpScroll_;
            helpScroll_ = std::max(0, helpScroll_ - dy * 20);
            if (helpScroll_ != before) app.request_redraw();
            return true;
        }
    }
    (void) dx;
    // The wheel scrolls INDEPENDENTLY of the cursor.  This used to move top_
    // and stop there: no redraw was requested, and the next draw() called
    // ensureVisible() unconditionally, whose first act is to drag top_ back to
    // the cursor row.  The cursor starts on line 1, so the wheel did precisely
    // nothing -- line 200 of a CSD was only reachable by holding Down.
    const int before = top_;
    const int last   = std::max(0, (int)lines_.size() - 1);
    top_ = std::max(0, std::min(last, top_ - dy * 3));
    if (top_ != before) app.request_redraw();
    return true;
}

bool CsoundEditorView::on_mouse(App& app, const MouseEv& e) {
#ifdef PATCHKNOB_HAS_AI
    // Chat tab + panel first: they overlay the text exactly like the help
    // drawer does, and their tab must work even while the help drawer is out.
    {
        const SDL_Rect ctab = chatTabRect(app, rect);
        if (e.pressed && e.button == SDL_BUTTON_LEFT &&
            e.x >= ctab.x && e.x < ctab.x + ctab.w &&
            e.y >= ctab.y && e.y < ctab.y + ctab.h) {
            chatOpen_ = !chatOpen_;
            if (chatOpen_) {
                aiChat().onOpen(app);
                // The two share the docked edge: opening one closes the other.
                if (helpOpen_) {
                    helpOpen_ = false;
                    helpMouseDown_ = false;
                    app.end_text_if(&helpSearch_);
                    helpSearchFocused_ = false;
                    clearHelpSelection();
                }
            } else if (chat_) {
                chat_->onClose(app);
            }
            app.request_redraw();
            return true;
        }
        // Gate on !mouseSelecting_ the same way the help panel does: an
        // editor text selection that started OUTSIDE the panel keeps its drag
        // even when the pointer crosses it.
        if (chatOpen_ && chat_ && !mouseSelecting_ &&
            chat_->on_mouse(app, e, chatPanelRect(app, rect)))
            return true;
    }
#endif
    // Help drawer tab/panel sit on top of everything else drawn below, so
    // they get first refusal on clicks too -- otherwise a click on the tab
    // (or inside the read-only panel) would fall through and move the text
    // cursor underneath it.
    {
        auto insideR = [&](const SDL_Rect& q) {
            return e.x >= q.x && e.x < q.x + q.w && e.y >= q.y && e.y < q.y + q.h;
        };
        const HelpLayout hl = computeHelpLayout(app, rect);
        // Document-space point under the pointer (scroll-independent), which
        // is the space selection anchors and litehtml hit-testing both use.
        const SDL_Point docPt{ e.x - hl.content.x, e.y - hl.content.y + helpScroll_ };
        // litehtml asking for a repaint means the DOCUMENT's own pixels
        // changed (a link's press state), so the composited band is stale too.
        auto htmlRedraw = [&](const litehtml::position&) {
            helpBandDirty_ = true;
            app.request_redraw();
        };

        // --- drags already in flight, before ANY containment test ---------
        // While a button is held the pointer can wander over the tab or the
        // buttons; testing those first would fire them mid-drag.
        if (helpResizeDrag_) {
            if (!e.pressed) { helpResizeDrag_ = false; return true; }
            helpWidth_ = std::max(180, (rect.x + rect.w) - (e.x - helpResizeGrab_));
            app.request_redraw();
            return true;
        }
        if (helpScrollDrag_) {
            if (!e.pressed) { helpScrollDrag_ = false; return true; }
            const int maxScroll = helpMaxScroll(hl);
            const SDL_Rect thumb = helpThumbRect(hl);
            const int span = std::max(1, hl.scrollbar.h - thumb.h);
            const int wantY = e.y - hl.scrollbar.y - helpScrollGrab_;
            helpScroll_ = maxScroll > 0
                        ? std::max(0, std::min(maxScroll, (int) ((int64_t) wantY * maxScroll / span)))
                        : 0;
            app.request_redraw();
            return true;
        }
        if (helpSelecting_) {
            if (e.pressed) {
                helpSelHead_ = docPt;
                // A few px of slop, so a plain click on a link is not read as
                // a one-word selection (which would suppress the navigation).
                helpHasSel_ = std::abs(docPt.x - helpSelAnchor_.x) > 3 ||
                              std::abs(docPt.y - helpSelAnchor_.y) > 3;
                rebuildHelpSelection();
                app.request_redraw();
                return true;
            }
            helpSelecting_ = false;
            if (helpMouseDown_ && helpDoc_) {
                helpMouseDown_ = false;
                helpDoc_->on_lbutton_up(docPt.x, docPt.y, docPt.x, docPt.y, htmlRedraw);
            }
            // on_lbutton_up is what fires on_anchor_click, so the link is only
            // available now. A drag means the user was selecting text, not
            // following the link under the release point.
            const std::string link = helpContainer_ ? helpContainer_->take_clicked_link() : std::string();
            if (!helpHasSel_ && !link.empty()) loadManualPage(app, link, true);
            app.request_redraw();
            return true;
        }

        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.tab)) {
            helpOpen_ = !helpOpen_;
#ifdef PATCHKNOB_HAS_AI
            if (helpOpen_ && chatOpen_) {   // manual and chat share the edge
                chatOpen_ = false;
                if (chat_) chat_->onClose(app);
            }
#endif
            if (helpOpen_) ensureHelpOpened(app);
            else {
                helpMouseDown_ = false;
                app.end_text_if(&helpSearch_);
                helpSearchFocused_ = false;
                clearHelpSelection();
            }
            app.request_redraw();
            return true;
        }
        // Everything below is help-drawer handling.  This used to read
        // `if (!helpOpen_) return false;` -- which returns from on_mouse
        // ENTIRELY, not merely out of this block, so with the drawer closed
        // (the normal state) the editor's own text selection and its Open /
        // Save / Save As / Compile buttons never saw a single mouse event.
        if (helpOpen_) {

        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.resizeGrip)) {
            helpResizeDrag_ = true;
            helpResizeGrab_ = e.x - hl.panel.x;
            app.request_redraw();
            return true;
        }
        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.navBack)) {
            navigateHelpBack(app);
            app.request_redraw();
            return true;
        }
        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.navHome)) {
            // Re-probe: the manual may have been fetched since the drawer
            // was first opened, and HOME is the natural place to retry.
            if (manualRoot_.empty()) manualRoot_ = findManualRoot();
            if (!manualRoot_.empty()) loadManualPage(app, manualRoot_ + "index.html", true);
            else loadHelpUnavailable(app);
            app.request_redraw();
            return true;
        }
        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.searchBox)) {
            focusHelpSearch(app);
            app.request_redraw();
            return true;
        }
        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.searchPrev)) {
            stepHelpMatch(app, hl, -1);
            return true;
        }
        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.searchNext)) {
            stepHelpMatch(app, hl, +1);
            return true;
        }
        if (e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.scrollbar)) {
            const SDL_Rect thumb = helpThumbRect(hl);
            if (thumb.h > 0) {
                // Grabbing the thumb keeps the pointer's position within it;
                // clicking the bare trough centres the thumb on the click.
                helpScrollGrab_ = (e.y >= thumb.y && e.y < thumb.y + thumb.h) ? e.y - thumb.y : thumb.h / 2;
                helpScrollDrag_ = true;
                const int maxScroll = helpMaxScroll(hl);
                const int span = std::max(1, hl.scrollbar.h - thumb.h);
                const int wantY = e.y - hl.scrollbar.y - helpScrollGrab_;
                helpScroll_ = maxScroll > 0
                            ? std::max(0, std::min(maxScroll, (int) ((int64_t) wantY * maxScroll / span)))
                            : 0;
                app.request_redraw();
            }
            return true;
        }
        // A press in the body both starts a text selection and goes to
        // litehtml, so one gesture can end as either a drag-select or a link
        // click -- which of the two it was is decided on release, above.
        if (helpDoc_ && e.pressed && e.button == SDL_BUTTON_LEFT && insideR(hl.content)) {
            helpSelAnchor_ = helpSelHead_ = docPt;
            helpHasSel_ = false;
            helpSelRects_.clear();
            helpSelText_.clear();
            helpSelecting_ = true;
            helpMouseDown_ = true;
            helpDoc_->on_lbutton_down(docPt.x, docPt.y, docPt.x, docPt.y, htmlRedraw);
            app.request_redraw();
            return true;
        }
        if (!mouseSelecting_ && e.pressed && insideR(hl.panel)) return true;
        }   // helpOpen_
    }
    const int statusH = app.mono.ch() + 8;
    const int compileW=ui::platform::csound_compile_button_width();
    const SDL_Rect compileButton{rect.x + rect.w - compileW - 4,
                                 rect.y + rect.h - statusH + 3,
                                 compileW, statusH - 6};
    const int saveAsW=76, saveW=58, openW=58, gap=4;
    const SDL_Rect saveAsButton{compileButton.x-gap-saveAsW,compileButton.y,saveAsW,compileButton.h};
    const SDL_Rect saveButton{saveAsButton.x-gap-saveW,compileButton.y,saveW,compileButton.h};
    const SDL_Rect openButton{saveButton.x-gap-openW,compileButton.y,openW,compileButton.h};
    auto inside=[&](const SDL_Rect& q){return e.x>=q.x&&e.x<q.x+q.w&&e.y>=q.y&&e.y<q.y+q.h;};
    if(e.pressed&&e.button==SDL_BUTTON_LEFT){
        if(inside(openButton)){if(on_open_file)on_open_file();app.request_redraw();return true;}
        if(inside(saveButton)){if(on_save_file)on_save_file();app.request_redraw();return true;}
        if(inside(saveAsButton)){if(on_save_file_as)on_save_file_as();app.request_redraw();return true;}
    }
    if (e.pressed && e.button == SDL_BUTTON_LEFT &&
        e.x >= compileButton.x && e.x < compileButton.x + compileButton.w &&
        e.y >= compileButton.y && e.y < compileButton.y + compileButton.h) {
        mouseSelecting_ = false;
        setStatus("compiling...", false);
        if (on_recompile) on_recompile();
        app.request_redraw();
        return true;
    }
    if (!e.pressed) {
        if (!mouseSelecting_) return false;
        mouseSelecting_ = false;
        return true;
    }
    if (!mouseSelecting_) {
        if (!hit(e.x, e.y) || e.button != SDL_BUTTON_LEFT) return false;
        setCursorFromMouse(app, e.x, e.y);
        clearSelection();
        mouseSelecting_ = true;
        return true;
    }
    setCursorFromMouse(app, e.x, e.y);
    return true;
}

void CsoundEditorView::setCursorFromMouse(App& app, int x, int y) {
    lastEditGroup_ = 0;    // a click ends any typing burst
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
    const int rowH = app.mono.ch() + 2;
    const int gutterW = 5 * cw;
    int row = top_ + (y - (rect.y + 2)) / std::max(1, rowH);
    row = std::max(0, std::min((int)lines_.size() - 1, row));
    int col = (x - (rect.x + gutterW)) / std::max(1, cw);
    cr_ = row;
    cc_ = std::max(0, std::min((int)lines_[cr_].size(), col));
}

void CsoundEditorView::draw(App& app) {
    SDL_Renderer* r = app.ren;
    const Theme& t = theme();
    const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
    const int ch = app.mono.ch();
    const int rowH = ch + 2;
    SDL_Rect area = rect;
    fill_rect(r, area, t.bg);

    const int statusH = ch + 8;
    const int gutterW = 5 * cw;
    const int textTop = area.y + 3;
    const int textAreaH = area.h - statusH - 6;
    const int visRows = std::max(1, textAreaH / rowH);
    // Follow the caret only when it actually MOVED -- row OR column, so typing
    // on the line you scrolled away from still brings the caret back, while a
    // wheel scroll (which touches neither) is left alone.  Running this every
    // frame unconditionally is what made the wheel inert (see on_wheel).
    if (cr_ != shownCr_ || cc_ != shownCc_) {
        ensureVisible(visRows);
        shownCr_ = cr_; shownCc_ = cc_;
    }
    if (top_ > std::max(0, (int)lines_.size() - 1)) top_ = std::max(0, (int)lines_.size() - 1);
    if (top_ < 0) top_ = 0;

    // gutter
    SDL_Rect gutter{ area.x, area.y, gutterW - 2, area.h - statusH };
    fill_rect(r, gutter, t.panel);

    for (int i = 0; i < visRows; ++i) {
        const int ln = top_ + i;
        if (ln >= (int)lines_.size()) break;
        const int y = textTop + i * rowH;
        char num[8]; std::snprintf(num, sizeof(num), "%4d", ln + 1);
        app.mono.draw(r, area.x + 2, y, num, t.dim);
        const std::string& src = lines_[(size_t)ln];
        // Columns of this line covered by the selection, if any. -1 = none.
        int selFrom = -1, selTo = -1;
        if (hasSelection()) {
            int r0 = ar_, c0 = ac_, r1 = cr_, c1 = cc_;
            if (std::pair<int,int>(r1, c1) < std::pair<int,int>(r0, c0)) {
                std::swap(r0, r1); std::swap(c0, c1);
            }
            if (ln >= r0 && ln <= r1) {
                const int from = ln == r0 ? c0 : 0;
                // A fully-selected line highlights one column past its end, so
                // the selected line break is visible.
                const int to = ln == r1 ? c1 : (int)src.size() + 1;
                if (to > from) {
                    fill_rect(r, SDL_Rect{area.x + gutterW + from * cw, y,
                                           (to - from) * cw, rowH}, t.sel);
                    selFrom = from;
                    selTo   = to;
                }
            }
        }
        const int x0 = area.x + gutterW;
        // Selected glyphs are drawn dim rather than the normal text colour:
        // against the selection fill, full-strength text reads as heavier
        // than the rest of the buffer. The font is monospace, so a column
        // index is an exact pixel offset and the three spans line up.
        const int a = selFrom < 0 ? -1 : std::max(0, std::min((int)src.size(), selFrom));
        const int b = selFrom < 0 ? -1 : std::max(0, std::min((int)src.size(), selTo));
        if (a < 0 || b <= a) {
            app.mono.draw(r, x0, y, src, t.text);
        } else {
            if (a > 0)               app.mono.draw(r, x0, y, src.substr(0, a), t.text);
            app.mono.draw(r, x0 + a * cw, y, src.substr(a, b - a), t.dim);
            if ((int)src.size() > b) app.mono.draw(r, x0 + b * cw, y, src.substr(b), t.text);
        }
    }

    // caret (simple blink)
    blink_ = (int)((SDL_GetTicks()/500u)&1u);
    if (!hasSelection() && cr_ >= top_ && cr_ < top_ + visRows && blink_ == 0) {
        const int cx = area.x + gutterW + cc_ * cw;
        const int cy = textTop + (cr_ - top_) * rowH;
        vline(r, cx, cy, cy + ch, t.accent);
    }

    // status bar
    SDL_Rect status{ area.x, area.y + area.h - statusH, area.w, statusH };
    fill_rect(r, status, t.panel);
    frame_rect(r, status, t.dim);
    const int compileW=ui::platform::csound_compile_button_width();
    SDL_Rect compileButton{status.x + status.w - compileW - 4, status.y + 3,
                           compileW, status.h - 6};
    fill_rect(r, compileButton, t.accent);
    frame_rect(r, compileButton, t.hi);
    app.mono.draw_centered(r, compileButton, "COMPILE", t.keybg);
    const int saveAsW=76, saveW=58, openW=58, gap=4;
    SDL_Rect saveAsButton{compileButton.x-gap-saveAsW,compileButton.y,saveAsW,compileButton.h};
    SDL_Rect saveButton{saveAsButton.x-gap-saveW,compileButton.y,saveW,compileButton.h};
    SDL_Rect openButton{saveButton.x-gap-openW,compileButton.y,openW,compileButton.h};
    for(auto pair:{std::pair<SDL_Rect,const char*>{openButton,"OPEN"},
                   {saveButton,"SAVE"},{saveAsButton,"SAVE AS"}}){
        fill_rect(r,pair.first,t.keybg);frame_rect(r,pair.first,t.dim);
        app.mono.draw_centered(r,pair.first,pair.second,t.text);
    }
    const std::string msg=status_.empty()?ui::platform::csound_editor_help():status_;
    SDL_Rect messageBox{status.x + 6, status.y + 3,
                        std::max(0, openButton.x - status.x - 12), status.h - 6};
    app.mono.draw_fitted(r, messageBox, msg, statusError_ ? t.hi : t.dim,
                         false, 1.f, .55f, true);

    // ---- collapsible HTML help drawer: the full Csound Reference Manual ---
    // Drawn LAST so it overlays the text/gutter rather than reflowing them --
    // see csound_editor_view.h's comment on helpOpen_.
    {
        const HelpLayout hl = computeHelpLayout(app, area);
        fill_rect(r, hl.tab, t.keybg);
        frame_rect(r, hl.tab, t.dim);
        app.mono.draw_centered(r, hl.tab, helpOpen_ ? "<" : ">", t.text);

        if (helpOpen_) {
            ensureHelpOpened(app);
            fill_rect(r, hl.panel, t.panel);
            frame_rect(r, hl.panel, t.dim);
            // Drag handle for the drawer width.
            fill_rect(r, hl.resizeGrip, helpResizeDrag_ ? t.accent : t.dim);

            const bool canBack = helpHistory_.size() > 1;
            fill_rect(r, hl.navBack, canBack ? t.keybg : t.panel);
            frame_rect(r, hl.navBack, t.dim);
            app.mono.draw_centered(r, hl.navBack, "< BACK", canBack ? t.text : t.dim);
            fill_rect(r, hl.navHome, t.keybg);
            frame_rect(r, hl.navHome, t.dim);
            app.mono.draw_centered(r, hl.navHome, "HOME", t.text);

            // --- find in page ---
            fill_rect(r, hl.searchBox, t.bg);
            frame_rect(r, hl.searchBox, helpSearchFocused_ ? t.accent : t.dim);
            int textW = hl.searchBox.w - 8;
            if (!helpSearch_.empty()) {
                const std::string cnt = helpMatches_.empty()
                    ? std::string("0/0")
                    : std::to_string(helpMatchIdx_ + 1) + "/" + std::to_string((int) helpMatches_.size());
                const int cw = app.mono.cw() > 0 ? app.mono.cw() : 6;
                const int cntW = (int) cnt.size() * cw + 6;
                SDL_Rect cb{ hl.searchBox.x + hl.searchBox.w - cntW, hl.searchBox.y + 3,
                             cntW - 3, hl.searchBox.h - 6 };
                app.mono.draw_fitted(r, cb, cnt, helpMatches_.empty() ? t.hi : t.dim,
                                     false, 1.f, .55f, true);
                textW -= cntW;
            }
            {
                const bool placeholder = helpSearch_.empty() && !helpSearchFocused_;
                const std::string shown = placeholder ? std::string("find in page")
                                                      : helpSearch_ + (helpSearchFocused_ ? "_" : "");
                SDL_Rect tb{ hl.searchBox.x + 4, hl.searchBox.y + 3, std::max(0, textW), hl.searchBox.h - 6 };
                app.mono.draw_fitted(r, tb, shown, placeholder ? t.dim : t.text, false, 1.f, .55f, true);
            }
            for (auto pr : { std::pair<SDL_Rect, const char*>{ hl.searchPrev, "<" },
                             { hl.searchNext, ">" } }) {
                fill_rect(r, pr.first, t.keybg);
                frame_rect(r, pr.first, t.dim);
                app.mono.draw_centered(r, pr.first, pr.second, helpMatches_.empty() ? t.dim : t.text);
            }

            if (hl.content.w > 4 && hl.content.h > 4 && helpDoc_) {
                // Lay out ONLY when the page or its box changed. render() is a
                // full layout pass over the whole document; doing it every
                // frame is what made scrolling a long page crawl. Re-indexing
                // rides along, since a reflow moves every run and both the
                // selection and match rects are in document space.
                if (helpRunsDirty_ || helpRenderedW_ != hl.content.w || helpRenderedH_ != hl.content.h) {
                    helpContainer_->set_viewport_size(hl.content.w, hl.content.h);
                    helpDoc_->render((litehtml::pixel_t) hl.content.w);
                    helpDocHeight_ = (int) helpDoc_->height();
                    helpRenderedW_ = hl.content.w;
                    helpRenderedH_ = hl.content.h;
                    captureHelpRuns(hl);
                    helpCapturedWidth_ = hl.content.w;
                    helpRunsDirty_ = false;
                    rebuildHelpSelection();
                    helpSearchDirty_ = true;
                    helpBandDirty_ = true;      // a reflow moves every pixel
                }
                if (helpSearchDirty_) { rebuildHelpMatches(); helpSearchDirty_ = false; }
                helpScroll_ = std::max(0, std::min(helpMaxScroll(hl), helpScroll_));

                // ---- composited band cache (see the header's comment) -----
                // Sized to ~3 viewports so ordinary wheel scrolling stays
                // inside it; re-composited only when the scroll leaves the
                // band or the content itself changed.
                const int bandH  = std::min(std::max(1, helpDocHeight_), hl.content.h * 3);
                const int maxTop = std::max(0, helpDocHeight_ - bandH);
                if (!helpBandUnsupported_ &&
                    (helpBandDirty_ || !helpBandTex_ || helpBandRen_ != r ||
                     helpBandW_ != hl.content.w || helpBandH_ != bandH ||
                     helpScroll_ < helpBandTop_ ||
                     helpScroll_ + hl.content.h > helpBandTop_ + bandH)) {
                    if (helpBandTex_ && (helpBandRen_ != r || helpBandW_ != hl.content.w ||
                                         helpBandH_ != bandH)) {
                        // Textures are per-renderer; destroying a stale one on
                        // a DEAD renderer is the caller's problem, matching
                        // HtmlContainer::set_renderer.
                        if (helpBandRen_ == r) SDL_DestroyTexture(helpBandTex_);
                        helpBandTex_ = nullptr;
                    }
                    if (!helpBandTex_) {
                        helpBandTex_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                                         SDL_TEXTUREACCESS_TARGET,
                                                         hl.content.w, bandH);
                        if (!helpBandTex_) helpBandUnsupported_ = true;  // fall back for good
                        helpBandRen_ = r;
                        helpBandW_ = hl.content.w;
                        helpBandH_ = bandH;
                    }
                    if (helpBandTex_) {
                        // Centre the band on the viewport so it survives a
                        // viewport of travel in either direction.
                        helpBandTop_ = std::max(0, std::min(maxTop,
                                           helpScroll_ - (bandH - hl.content.h) / 2));
                        SDL_Texture* prevTarget = SDL_GetRenderTarget(r);
                        if (SDL_SetRenderTarget(r, helpBandTex_) != 0) {
                            // Target rejected at bind time: without this guard
                            // the band render would land on the SCREEN at band
                            // coordinates. Fall back to direct drawing for good.
                            SDL_DestroyTexture(helpBandTex_);
                            helpBandTex_ = nullptr;
                            helpBandUnsupported_ = true;
                        } else {
                            // The page pins its own white canvas
                            // (kManualUserCss); clearing to it keeps any strip
                            // the document leaves unpainted from showing stale
                            // texels.
                            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
                            SDL_RenderClear(r);
                            const SDL_Rect band{ 0, 0, hl.content.w, bandH };
                            helpContainer_->set_base_clip(band);
                            helpContainer_->begin_frame();
                            litehtml::position bandPos(0, 0, (litehtml::pixel_t) hl.content.w,
                                                       (litehtml::pixel_t) bandH);
                            helpDoc_->draw(0, 0, -helpBandTop_, &bandPos);
                            helpContainer_->end_frame();
                            SDL_SetRenderTarget(r, prevTarget);
                            helpBandDirty_ = false;
                        }
                    }
                }
                {
                    ScopedClip clip(r, hl.content);
                    if (helpBandTex_ && !helpBandDirty_) {
                        const int within = helpScroll_ - helpBandTop_;
                        SDL_Rect src{ 0, within, hl.content.w,
                                      std::min(hl.content.h, bandH - within) };
                        SDL_Rect dst{ hl.content.x, hl.content.y, src.w, src.h };
                        SDL_RenderCopy(r, helpBandTex_, &src, &dst);
                    } else {
                        // Direct path: renderers without target-texture
                        // support (helpBandUnsupported_) draw as before.
                        helpContainer_->set_base_clip(hl.content);
                        helpContainer_->begin_frame();
                        litehtml::position clipPos((litehtml::pixel_t) hl.content.x, (litehtml::pixel_t) hl.content.y,
                                                   (litehtml::pixel_t) hl.content.w, (litehtml::pixel_t) hl.content.h);
                        helpDoc_->draw(0, hl.content.x, hl.content.y - helpScroll_, &clipPos);
                        helpContainer_->end_frame();
                    }

                    // Highlights go ON TOP, translucent: the page paints its
                    // own opaque background during draw(), so anything laid
                    // down first would simply be covered by it.
                    SDL_BlendMode prevBlend = SDL_BLENDMODE_NONE;
                    SDL_GetRenderDrawBlendMode(r, &prevBlend);
                    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                    auto paint = [&](const SDL_Rect& d, Color c) {
                        const SDL_Rect s{ d.x + hl.content.x, d.y + hl.content.y - helpScroll_, d.w, d.h };
                        if (s.y + s.h < hl.content.y || s.y > hl.content.y + hl.content.h) return;
                        fill_rect(r, s, c);
                    };
                    for (const SDL_Rect& d : helpSelRects_)
                        paint(d, Color{ t.sel.r, t.sel.g, t.sel.b, 110 });
                    for (size_t k = 0; k < helpMatches_.size(); ++k)
                        paint(helpMatches_[k], (int) k == helpMatchIdx_
                                  ? Color{ t.accent.r, t.accent.g, t.accent.b, 150 }
                                  : Color{ t.hi.r, t.hi.g, t.hi.b, 90 });
                    SDL_SetRenderDrawBlendMode(r, prevBlend);
                }
                // Trough + thumb, outside the content clip so a short page's
                // empty trough still reads as "nothing to scroll".
                fill_rect(r, hl.scrollbar, t.bg);
                const SDL_Rect thumb = helpThumbRect(hl);
                if (thumb.h > 0) {
                    fill_rect(r, thumb, helpScrollDrag_ ? t.accent : t.dim);
                    frame_rect(r, thumb, t.panel);
                }
            }
        }
    }

#ifdef PATCHKNOB_HAS_AI
    // ---- Claude chat: its tab sits under the help tab; the panel overlays
    // the text just like the help drawer (the two are mutually exclusive).
    {
        const SDL_Rect ctab = chatTabRect(app, area);
        fill_rect(r, ctab, chatOpen_ ? t.accent : t.keybg);
        frame_rect(r, ctab, t.dim);
        app.mono.draw_centered(r, ctab, "AI", chatOpen_ ? t.keybg : t.text);
        if (chatOpen_) aiChat().draw(app, chatPanelRect(app, area));
    }
#endif
}

void CsoundEditorView::cancel_interaction(App& app) {
    mouseSelecting_ = false;
    helpResizeDrag_ = false;
    helpScrollDrag_ = false;
    helpSelecting_ = false;
    helpMouseDown_ = false;
    // Critical: App::text_target would otherwise keep pointing at
    // helpSearch_ after this view stops being interactive, swallowing every
    // keystroke and appending into storage the view no longer services.
    app.end_text_if(&helpSearch_);
    helpSearchFocused_ = false;
#ifdef PATCHKNOB_HAS_AI
    // Same contract for the chat composer -- its storage lives in the panel.
    if (chat_) chat_->cancel_interaction(app);
#endif
}

} // namespace ui
