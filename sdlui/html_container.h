//----------------------------------------------------------------------------
//  sdlui/html_container.h
//
//  A litehtml::document_container backed directly by SDL_Renderer + SDL_ttf.
//  litehtml is vendored (vendor/litehtml) as a lightweight, dependency-free
//  HTML+CSS layout/render engine -- no JS, no network -- which is exactly the
//  shape needed for browsing static documentation inside the app.  Its first
//  user is the Csound editor's collapsible help drawer, which renders the
//  vendored Canonical Csound Reference Manual mirror (vendor/csound-manual)
//  straight off local disk.
//
//  Everything this container reads comes from the local filesystem, resolved
//  against set_document_dir(): stylesheets via import_css(), <img> via
//  load_image() (PNG only -- libpng is already linked for the rack editor's
//  SVG pack).  An http(s):// URL is never fetched; it simply renders as an
//  unresolved link.
//
//  Deliberately NOT implemented: gradients fall back to a solid fill of their
//  first color stop, and border radii are ignored (straight solid edges only).
//  No document the app ships depends on either.
//
//  CACHING.  Both fonts and drawn text runs are cached, and both matter: a
//  manual page issues hundreds of draw_text() calls per frame, and creating a
//  TTF_Font (or an SDL_Texture) per call per frame makes scrolling unusable.
//  Fonts are refcounted by litehtml's own create_font/delete_font pairing;
//  text-run textures are keyed by (string, font, color) and evicted when they
//  go untouched for a few frames.  begin_frame() drives that clock.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_HTML_CONTAINER_H
#define PATCHKNOB_SDLUI_HTML_CONTAINER_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <litehtml.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

class HtmlContainer : public litehtml::document_container {
public:
    explicit HtmlContainer(SDL_Renderer* renderer = nullptr) : ren_(renderer) {}
    ~HtmlContainer() override;

    HtmlContainer(const HtmlContainer&) = delete;
    HtmlContainer& operator=(const HtmlContainer&) = delete;

    //! Swaps the renderer every texture this container owns was created on.
    //! Drops all cached textures, since they belong to the OLD renderer and
    //! using them against the new one is undefined.
    void set_renderer(SDL_Renderer* r);

    // Layout viewport, in CSS px -- drives get_viewport()/get_media_features()
    // (percentage widths, media queries, etc).
    void set_viewport_size(int w, int h) { vpW_ = w; vpH_ = h; }

    // The screen-space rect the caller has already clipped SDL's renderer to
    // (typically via a ui::ScopedClip around the draw call). set_clip/del_clip
    // intersect litehtml's own overflow clips against this and never widen
    // past it, so a document can never paint outside its panel; clearing back
    // to no litehtml-level clip restores exactly this rect rather than
    // removing clipping altogether.
    void set_base_clip(const SDL_Rect& r) { baseClip_ = r; clipStack_.clear(); }

    //! One drawn piece of text, in the screen coordinates it was painted at.
    //! litehtml splits text into per-word elements, so a run is normally a
    //! single word -- which is what makes word-granular selection and
    //! find-in-page possible without a second layout pass.
    struct TextRun {
        int x = 0, y = 0, w = 0, h = 0;
        litehtml::uint_ptr font = 0;
        std::string text;
    };

    //! Call once per draw pass, BEFORE document::draw(). Advances the clock
    //! the text-texture cache evicts against and starts a fresh run list.
    void begin_frame();
    //! Call once per draw pass, AFTER document::draw(), to publish the runs it
    //! painted. Until it is called text_runs() keeps returning the previous
    //! pass's list, so a caller drawing selection highlight before the
    //! document still has stable geometry to work from.
    void end_frame();
    //! Text runs from the last COMPLETED draw pass, in paint order -- which
    //! for normal flow content is reading order, so callers can treat the
    //! list as the document's text in order without sorting it.
    const std::vector<TextRun>& text_runs() const { return runs_; }

    //! While set, draw_text() records its run but paints nothing, and every
    //! other draw/clip entry point is a no-op. That turns document::draw()
    //! into a pure text-position query, so a caller can index a WHOLE page
    //! (not just the visible part) for find-in-page and selection without
    //! rasterizing it. Always pair with clearing it again.
    void set_capture_only(bool on) { captureOnly_ = on; }

    // Directory (absolute, filesystem path) the CURRENT document was loaded
    // from. import_css(), load_image() and on_anchor_click() resolve the
    // relative hrefs litehtml hands them against this -- set it before
    // document::createFromString(), matching the file the HTML came from.
    void set_document_dir(const std::string& dir) { docDir_ = dir; }
    const std::string& document_dir() const { return docDir_; }

    // Drains the ABSOLUTE local file path of the last <a> the user clicked,
    // already resolved against docDir_ at click time -- empty if nothing was
    // clicked since the last drain, or if the href wasn't a local file (an
    // external http(s):// link, a bare "#fragment", ...).
    std::string take_clicked_link();

    //! True while the pointer is over an element whose CSS cursor asks for a
    //! pointing hand -- i.e. a link. Lets the caller show link affordance
    //! without reaching into the document tree.
    bool over_link() const { return overLink_; }

    litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document* doc,
                                    litehtml::font_metrics* fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont, litehtml::web_color color,
                    const litehtml::position& pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char* get_default_font_name() const override;
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;
    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override;
    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer, const std::string& url,
                     const std::string& base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                          const litehtml::web_color& color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                               const litehtml::background_layer::linear_gradient& gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                               const litehtml::background_layer::radial_gradient& gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::conic_gradient& gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos,
                       bool root) override;
    void set_caption(const char* caption) override;
    void set_base_url(const char* base_url) override;
    void link(const std::shared_ptr<litehtml::document>& doc, const litehtml::element::ptr& el) override;
    void on_anchor_click(const char* url, const litehtml::element::ptr& el) override;
    void on_mouse_event(const litehtml::element::ptr& el, litehtml::mouse_event event) override;
    void set_cursor(const char* cursor) override;
    void transform_text(std::string& text, litehtml::text_transform tt) override;
    void import_css(std::string& text, const std::string& url, std::string& baseurl) override;
    void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position& viewport) const override;
    litehtml::element::ptr create_element(const char* tag_name, const litehtml::string_map& attributes,
                                           const std::shared_ptr<litehtml::document>& doc) override;
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(std::string& language, std::string& culture) const override;

private:
    struct FontEntry {
        TTF_Font* font = nullptr;
        int refs = 0;
        // True when the font carries underline/strikethrough, which is the
        // one case litehtml must hand us whitespace runs for -- the line has
        // to continue across the gaps between words.
        bool drawSpaces = false;
    };
    struct TextEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        TTF_Font* font = nullptr;   // owner, so a closed font can purge its runs
        uint64_t lastUsed = 0;
    };
    struct ImageEntry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };

    SDL_Renderer* ren_ = nullptr;
    int vpW_ = 0, vpH_ = 0;
    SDL_Rect baseClip_ { 0, 0, 0, 0 };
    std::vector<SDL_Rect> clipStack_;   // pushed by set_clip / popped by del_clip
    std::string clickedHref_;
    std::string docDir_;                // see set_document_dir()
    bool overLink_ = false;
    bool captureOnly_ = false;
    uint64_t frame_ = 0;

    std::unordered_map<std::string, FontEntry> fonts_;   // keyed by font_description::hash()
    std::unordered_map<std::string, TextEntry> textCache_;
    std::unordered_map<std::string, ImageEntry> images_;  // keyed by absolute path; null tex = failed
    std::vector<TextRun> runs_;          // published by end_frame()
    std::vector<TextRun> runsBuilding_;  // accumulated by draw_text() this pass

    // Locates a usable proportional/monospace TTF on this OS. Mirrors the
    // candidate search in gui.cpp's Font::load (fontconfig on Linux when
    // available, hardcoded Windows/Debian/Android paths otherwise) so the
    // help panel finds a font under the same conditions the rest of the UI
    // does.
    static std::string find_font_path(bool mono);
    // Joins a possibly-relative href from the CURRENT document (an <a href>,
    // a <link href>, an <img src>) onto docDir_ and lexically normalizes the
    // ".." segments real manual pages use to reach shared files. Returns ""
    // for anything not resolvable to a local file (an absolute http(s):// URL,
    // a bare "#fragment", javascript:, mailto:, ...).
    std::string resolve_local(const std::string& href) const;
    //! Decodes a PNG off disk into a texture. Null (cached as such) on any
    //! failure, including a non-PNG file -- the manual's images are all PNG.
    const ImageEntry* image_for(const std::string& src);
    void drop_text_cache_for(TTF_Font* f);
    void clear_caches();
};

} // namespace ui

#endif
