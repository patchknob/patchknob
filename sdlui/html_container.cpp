//----------------------------------------------------------------------------
//  sdlui/html_container.cpp
//----------------------------------------------------------------------------
#include "html_container.h"
#include "gui.h"

#include <png.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef PATCHKNOB_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

namespace ui {

namespace {

// A text run is only reusable while its glyphs are identical, so the key has
// to carry everything draw_text() varies on.
std::string text_cache_key(const char* text, litehtml::uint_ptr hFont, const litehtml::web_color& c) {
    char head[64];
    std::snprintf(head, sizeof(head), "%llx|%02x%02x%02x%02x|",
                  (unsigned long long) hFont, c.red, c.green, c.blue, c.alpha);
    return std::string(head) + text;
}

// Runs untouched for this many draw passes are destroyed. Small enough that a
// page you scrolled past stops costing VRAM, large enough that the runs on
// screen survive every frame.
constexpr uint64_t kTextCacheTTL = 90;
// Hard ceiling in case a single enormous page blows past the TTL sweep.
constexpr size_t kTextCacheMax = 4096;

SDL_Rect to_sdl_rect(const litehtml::position& p) {
    return SDL_Rect{ (int) p.x, (int) p.y, (int) p.width, (int) p.height };
}

SDL_Color to_sdl_color(const litehtml::web_color& c) {
    return SDL_Color{ c.red, c.green, c.blue, c.alpha };
}

Color to_ui_color(const litehtml::web_color& c) {
    return Color{ c.red, c.green, c.blue, c.alpha };
}

bool intersect(const SDL_Rect& a, const SDL_Rect& b, SDL_Rect& out) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    out = SDL_Rect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
    return out.w > 0 && out.h > 0;
}

// Minimal libpng decode to straight RGBA8. libpng is already a hard dependency
// of the frontend (the rack editor's SVG pack), so this costs no new library.
bool decode_png(const std::string& path, std::vector<unsigned char>& rgba, int& w, int& h) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    unsigned char sig[8];
    if (std::fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) { std::fclose(fp); return false; }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); std::fclose(fp); return false; }
    // libpng reports errors by longjmp'ing back here.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    const png_uint_32 pw = png_get_image_width(png, info);
    const png_uint_32 ph = png_get_image_height(png, info);
    const int depth = png_get_bit_depth(png, info);
    const int ctype = png_get_color_type(png, info);

    // Normalize everything to 8-bit RGBA.
    if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    rgba.assign((size_t) pw * ph * 4, 0);
    std::vector<png_bytep> rows((size_t) ph);
    for (png_uint_32 y = 0; y < ph; ++y) rows[y] = rgba.data() + (size_t) y * pw * 4;
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);

    w = (int) pw;
    h = (int) ph;
    return true;
}

} // namespace

HtmlContainer::~HtmlContainer() {
    clear_caches();
    // The fonts themselves outlive the texture caches: litehtml is supposed to
    // pair every create_font with a delete_font, but a document destroyed
    // mid-layout can leave entries behind, and leaking TTF_Fonts across page
    // loads adds up over a long browse.
    for (auto& kv : fonts_)
        if (kv.second.font) TTF_CloseFont(kv.second.font);
    fonts_.clear();
}

void HtmlContainer::clear_caches() {
    for (auto& kv : textCache_)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    textCache_.clear();
    for (auto& kv : images_)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    images_.clear();
}

void HtmlContainer::set_renderer(SDL_Renderer* r) {
    if (r == ren_) return;
    // Every cached texture belongs to the renderer that built it; carrying
    // them across would hand the new renderer foreign handles.
    clear_caches();
    ren_ = r;
}

void HtmlContainer::begin_frame() {
    ++frame_;
    runsBuilding_.clear();
    if (textCache_.size() < kTextCacheMax / 2) {
        // Cheap path: nothing is under pressure, so don't walk the map.
        if ((frame_ & 0x3F) != 0) return;
    }
    for (auto it = textCache_.begin(); it != textCache_.end();) {
        if (frame_ - it->second.lastUsed > kTextCacheTTL) {
            if (it->second.tex) SDL_DestroyTexture(it->second.tex);
            it = textCache_.erase(it);
        } else {
            ++it;
        }
    }
}

void HtmlContainer::end_frame() {
    runs_.swap(runsBuilding_);
    runsBuilding_.clear();
}

void HtmlContainer::drop_text_cache_for(TTF_Font* f) {
    for (auto it = textCache_.begin(); it != textCache_.end();) {
        if (it->second.font == f) {
            if (it->second.tex) SDL_DestroyTexture(it->second.tex);
            it = textCache_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string HtmlContainer::take_clicked_link() {
    std::string s;
    s.swap(clickedHref_);
    return s;
}

std::string HtmlContainer::resolve_local(const std::string& href) const {
    if (href.empty() || href[0] == '#') return {};
    if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0 ||
        href.rfind("javascript:", 0) == 0 || href.rfind("mailto:", 0) == 0 ||
        href.rfind("ftp://", 0) == 0)
        return {};
    if (docDir_.empty()) return {};
    // Drop a trailing "#anchor" -- this container has no in-page scroll-to-id,
    // it just opens the target page itself.
    std::string path = href.substr(0, href.find('#'));
    if (path.empty()) return {};
    // Percent-decode: manual filenames are ASCII, but a few hrefs escape
    // characters anyway ("%20"), and fopen wants the real name.
    std::string decoded;
    decoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() &&
            std::isxdigit((unsigned char) path[i + 1]) && std::isxdigit((unsigned char) path[i + 2])) {
            decoded.push_back((char) std::stoi(path.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded.push_back(path[i]);
        }
    }
    std::error_code ec;
    std::filesystem::path abs =
        std::filesystem::weakly_canonical(std::filesystem::path(docDir_) / decoded, ec);
    if (ec) abs = (std::filesystem::path(docDir_) / decoded).lexically_normal();
    return abs.string();
}

// Mirrors gui.cpp Font::load's candidate search (see its comment for why
// fontconfig goes first on Linux): same fonts, same fallback order, so the
// help panel and the rest of the UI agree on what "the system font" is.
std::string HtmlContainer::find_font_path(bool mono) {
#ifdef PATCHKNOB_USE_FONTCONFIG
    {
        std::string fcPath;
        const char* pattern = mono ? "monospace" : "sans-serif";
        if (FcInit()) {
            if (FcPattern* pat = FcNameParse((const FcChar8*) pattern)) {
                FcConfigSubstitute(nullptr, pat, FcMatchPattern);
                FcDefaultSubstitute(pat);
                FcResult result;
                if (FcPattern* match = FcFontMatch(nullptr, pat, &result)) {
                    FcChar8* file = nullptr;
                    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file)
                        fcPath = (const char*) file;
                    FcPatternDestroy(match);
                }
                FcPatternDestroy(pat);
            }
        }
        if (!fcPath.empty()) return fcPath;
    }
#endif
    static const char* monoCandidates[] = {
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/system/fonts/RobotoMono-Regular.ttf",
        "/system/fonts/DroidSansMono.ttf", nullptr
    };
    static const char* sansCandidates[] = {
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/DroidSans.ttf", nullptr
    };
    // Try the requested family first, then the other one -- any real glyph
    // beats a blank panel, and a box with only a serif font installed should
    // still render the manual.
    for (int pass = 0; pass < 2; ++pass) {
        const char** candidates = (mono == (pass == 0)) ? monoCandidates : sansCandidates;
        for (int i = 0; candidates[i]; ++i) {
            std::FILE* f = std::fopen(candidates[i], "rb");
            if (f) { std::fclose(f); return candidates[i]; }
        }
    }
    return {};
}

litehtml::uint_ptr HtmlContainer::create_font(const litehtml::font_description& descr,
                                              const litehtml::document* /*doc*/,
                                              litehtml::font_metrics* fm) {
    const std::string key = descr.hash();
    auto it = fonts_.find(key);
    if (it == fonts_.end()) {
        const std::string& family = descr.family;
        const bool mono = family.find("mono") != std::string::npos ||
                          family.find("Mono") != std::string::npos ||
                          family.find("Courier") != std::string::npos ||
                          family.find("courier") != std::string::npos;
        // Resolve each family ONCE per run: a fontconfig match costs real
        // milliseconds, and a manual page asks for a dozen distinct fonts
        // (sizes/weights) that all map to the same two files.
        static const std::string kMonoPath = find_font_path(true);
        static const std::string kSansPath = find_font_path(false);
        const std::string& path = mono ? kMonoPath : kSansPath;
        // Clamp: a stylesheet asking for a 2000px heading would try to
        // rasterize an atlas big enough to fail allocation.
        const int px = std::max(1, std::min(256, (int) descr.size));
        TTF_Font* f = path.empty() ? nullptr : TTF_OpenFont(path.c_str(), px);
        if (!f) {
            std::fprintf(stderr, "[sdlui] html_container: no usable font for '%s' (mono=%d)\n",
                         family.c_str(), (int) mono);
            if (fm) *fm = litehtml::font_metrics{};
            return 0;
        }
        int style = TTF_STYLE_NORMAL;
        if (descr.style == litehtml::font_style_italic) style |= TTF_STYLE_ITALIC;
        if (descr.weight >= 600) style |= TTF_STYLE_BOLD;
        if (descr.decoration_line & litehtml::text_decoration_line_underline) style |= TTF_STYLE_UNDERLINE;
        if (descr.decoration_line & litehtml::text_decoration_line_line_through) style |= TTF_STYLE_STRIKETHROUGH;
        TTF_SetFontStyle(f, style);
        const bool drawSpaces = (style & (TTF_STYLE_UNDERLINE | TTF_STYLE_STRIKETHROUGH)) != 0;
        it = fonts_.emplace(key, FontEntry{ f, 0, drawSpaces }).first;
    }
    ++it->second.refs;

    TTF_Font* f = it->second.font;
    if (fm) {
        const int height  = TTF_FontHeight(f);
        const int ascent  = TTF_FontAscent(f);
        const int descent = -TTF_FontDescent(f);
        fm->font_size    = (litehtml::pixel_t) std::max(1, std::min(256, (int) descr.size));
        fm->height       = (litehtml::pixel_t) height;
        fm->ascent       = (litehtml::pixel_t) ascent;
        fm->descent      = (litehtml::pixel_t) descent;
        int adv = 0;
        TTF_GlyphMetrics(f, '0', nullptr, nullptr, nullptr, nullptr, &adv);
        fm->ch_width     = (litehtml::pixel_t) (adv > 0 ? adv : height / 2);
        int xminy = 0, xmaxy = 0;
        if (TTF_GlyphMetrics(f, 'x', nullptr, nullptr, &xminy, &xmaxy, nullptr) == 0 && xmaxy > xminy)
            fm->x_height = (litehtml::pixel_t) (xmaxy - xminy);
        else
            fm->x_height = (litehtml::pixel_t) (ascent / 2);
        fm->draw_spaces  = it->second.drawSpaces;
        fm->sub_shift    = (litehtml::pixel_t) (height / 5);
        fm->super_shift  = (litehtml::pixel_t) (height / 3);
    }
    return (litehtml::uint_ptr) f;
}

void HtmlContainer::delete_font(litehtml::uint_ptr hFont) {
    if (!hFont) return;
    TTF_Font* f = (TTF_Font*) hFont;
    for (auto it = fonts_.begin(); it != fonts_.end(); ++it) {
        if (it->second.font != f) continue;
        if (--it->second.refs > 0) return;
        // Last reference: the handle is about to become dangling, so every
        // cached run keyed on it has to go with it (a later TTF_OpenFont can
        // reuse the address and would otherwise inherit stale glyphs).
        drop_text_cache_for(f);
        TTF_CloseFont(f);
        fonts_.erase(it);
        return;
    }
}

litehtml::pixel_t HtmlContainer::text_width(const char* text, litehtml::uint_ptr hFont) {
    if (!hFont || !text || !*text) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8((TTF_Font*) hFont, text, &w, &h);
    return (litehtml::pixel_t) w;
}

void HtmlContainer::draw_text(litehtml::uint_ptr /*hdc*/, const char* text, litehtml::uint_ptr hFont,
                              litehtml::web_color color, const litehtml::position& pos) {
    if (!ren_ || !hFont || !text || !*text) return;

    if (captureOnly_) {
        // Position-only pass (see set_capture_only): record the run at the
        // measured advance -- no texture is built, so there is no glyph box
        // to take the size from.
        runsBuilding_.push_back(TextRun{ (int) pos.x, (int) pos.y,
                                         (int) pos.width, (int) pos.height, hFont, text });
        return;
    }

    const std::string key = text_cache_key(text, hFont, color);
    auto it = textCache_.find(key);
    if (it == textCache_.end()) {
        if (textCache_.size() >= kTextCacheMax) {
            // Over the ceiling: drop the coldest half rather than thrash one
            // entry at a time.
            std::vector<std::pair<uint64_t, std::string>> ages;
            ages.reserve(textCache_.size());
            for (auto& kv : textCache_) ages.emplace_back(kv.second.lastUsed, kv.first);
            std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
            for (size_t i = 0; i < ages.size() / 2; ++i) {
                auto victim = textCache_.find(ages[i].second);
                if (victim != textCache_.end()) {
                    if (victim->second.tex) SDL_DestroyTexture(victim->second.tex);
                    textCache_.erase(victim);
                }
            }
        }
        SDL_Surface* surf = TTF_RenderText_Blended((TTF_Font*) hFont, text, to_sdl_color(color));
        if (!surf) return;
        SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_BLEND);
        SDL_Texture* tex = SDL_CreateTextureFromSurface(ren_, surf);
        TextEntry e;
        e.tex  = tex;
        e.w    = surf->w;
        e.h    = surf->h;
        e.font = (TTF_Font*) hFont;
        SDL_FreeSurface(surf);
        if (!tex) return;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        it = textCache_.emplace(key, e).first;
    }
    it->second.lastUsed = frame_;
    SDL_Rect dst{ (int) pos.x, (int) pos.y, it->second.w, it->second.h };
    SDL_RenderCopy(ren_, it->second.tex, nullptr, &dst);

    // Record what was painted where, for selection and find-in-page. Note the
    // rect is the TEXTURE's, not `pos` -- pos.width is litehtml's measured
    // advance, which can differ by a pixel from what the glyphs occupy.
    runsBuilding_.push_back(TextRun{ dst.x, dst.y, dst.w, dst.h, hFont, text });
}

litehtml::pixel_t HtmlContainer::pt_to_px(float pt) const {
    return (litehtml::pixel_t) (pt * 96.0f / 72.0f);
}

litehtml::pixel_t HtmlContainer::get_default_font_size() const { return 15; }
const char* HtmlContainer::get_default_font_name() const { return "sans-serif"; }

void HtmlContainer::draw_list_marker(litehtml::uint_ptr /*hdc*/, const litehtml::list_marker& marker) {
    if (!ren_ || captureOnly_) return;
    // Only bullet shapes reach here -- numbered/lettered markers are rendered
    // as ordinary text by the caller (litehtml's html_tag::draw_list_marker)
    // before it ever calls this.
    if (marker.marker_type == litehtml::list_style_type_none) return;
    const SDL_Rect box = to_sdl_rect(marker.pos);
    if (box.w <= 0 || box.h <= 0) return;
    // Shrink the glyph box so the mark reads as a bullet rather than a block.
    const int side = std::max(2, std::min(box.w, box.h) / 2);
    const SDL_Rect q{ box.x + (box.w - side) / 2, box.y + (box.h - side) / 2, side, side };
    const Color c = to_ui_color(marker.color);
    if (marker.marker_type == litehtml::list_style_type_circle) frame_rect(ren_, q, c);
    else                                                        fill_rect(ren_, q, c);
}

const HtmlContainer::ImageEntry* HtmlContainer::image_for(const std::string& src) {
    const std::string path = resolve_local(src);
    if (path.empty()) return nullptr;
    auto it = images_.find(path);
    if (it != images_.end()) return &it->second;

    ImageEntry e;
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (ren_ && decode_png(path, rgba, w, h) && w > 0 && h > 0) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
            rgba.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
        if (surf) {
            e.tex = SDL_CreateTextureFromSurface(ren_, surf);
            SDL_FreeSurface(surf);
            if (e.tex) {
                SDL_SetTextureBlendMode(e.tex, SDL_BLENDMODE_BLEND);
                e.w = w;
                e.h = h;
            }
        }
    }
    // A failure is cached too (tex == nullptr), so a broken <img> is decoded
    // once rather than on every layout pass.
    return &images_.emplace(path, e).first->second;
}

void HtmlContainer::load_image(const char* src, const char* /*baseurl*/, bool /*redraw_on_ready*/) {
    // Decoding is synchronous off local disk, so there is nothing to defer and
    // no redraw to schedule -- by the time layout asks get_image_size() the
    // real dimensions are already known.
    if (src) image_for(src);
}

void HtmlContainer::get_image_size(const char* src, const char* /*baseurl*/, litehtml::size& sz) {
    sz.width  = 0;
    sz.height = 0;
    if (!src) return;
    if (const ImageEntry* e = image_for(src)) {
        sz.width  = (litehtml::pixel_t) e->w;
        sz.height = (litehtml::pixel_t) e->h;
    }
}

void HtmlContainer::draw_image(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                               const std::string& url, const std::string& /*base_url*/) {
    if (!ren_ || captureOnly_) return;
    const ImageEntry* e = image_for(url);
    if (!e || !e->tex) return;
    // origin_box is the box the image is scaled into; border_box bounds what
    // may be painted. No tiling: every image the manual uses is a plain
    // single-draw <img>.
    SDL_Rect dst = to_sdl_rect(layer.origin_box);
    if (dst.w <= 0 || dst.h <= 0) return;
    SDL_Rect clipped;
    if (!intersect(dst, to_sdl_rect(layer.border_box), clipped)) return;
    SDL_RenderCopy(ren_, e->tex, nullptr, &dst);
}

void HtmlContainer::draw_solid_fill(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                    const litehtml::web_color& color) {
    if (!ren_ || captureOnly_ || color.alpha == 0) return;
    fill_rect(ren_, to_sdl_rect(layer.border_box), to_ui_color(color));
}

// No gradient rasterizer -- approximate with a flat fill of the first stop,
// which is closer to the intended look than leaving the box blank.
void HtmlContainer::draw_linear_gradient(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                         const litehtml::background_layer::linear_gradient& gradient) {
    if (!ren_ || captureOnly_ || gradient.color_points.empty()) return;
    fill_rect(ren_, to_sdl_rect(layer.border_box), to_ui_color(gradient.color_points.front().color));
}

void HtmlContainer::draw_radial_gradient(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                         const litehtml::background_layer::radial_gradient& gradient) {
    if (!ren_ || captureOnly_ || gradient.color_points.empty()) return;
    fill_rect(ren_, to_sdl_rect(layer.border_box), to_ui_color(gradient.color_points.front().color));
}

void HtmlContainer::draw_conic_gradient(litehtml::uint_ptr /*hdc*/, const litehtml::background_layer& layer,
                                        const litehtml::background_layer::conic_gradient& gradient) {
    if (!ren_ || captureOnly_ || gradient.color_points.empty()) return;
    fill_rect(ren_, to_sdl_rect(layer.border_box), to_ui_color(gradient.color_points.front().color));
}

void HtmlContainer::draw_borders(litehtml::uint_ptr /*hdc*/, const litehtml::borders& b,
                                 const litehtml::position& draw_pos, bool /*root*/) {
    if (!ren_ || captureOnly_) return;
    const SDL_Rect box = to_sdl_rect(draw_pos);
    if (box.w <= 0 || box.h <= 0) return;
    // Straight solid edges only (see html_container.h) -- every style short of
    // none/hidden draws solid; radius is ignored.
    auto edge = [&](const litehtml::border& e, SDL_Rect q) {
        if ((int) e.width <= 0 || e.style == litehtml::border_style_none ||
            e.style == litehtml::border_style_hidden || e.color.alpha == 0)
            return;
        fill_rect(ren_, q, to_ui_color(e.color));
    };
    edge(b.top,    SDL_Rect{ box.x, box.y, box.w, (int) b.top.width });
    edge(b.bottom, SDL_Rect{ box.x, box.y + box.h - (int) b.bottom.width, box.w, (int) b.bottom.width });
    edge(b.left,   SDL_Rect{ box.x, box.y, (int) b.left.width, box.h });
    edge(b.right,  SDL_Rect{ box.x + box.w - (int) b.right.width, box.y, (int) b.right.width, box.h });
}

void HtmlContainer::set_caption(const char* /*caption*/) {}
// Real hrefs are resolved against docDir_ (see set_document_dir), not
// litehtml's own <base href> tracking -- the vendored manual pages don't use
// <base>, and docDir_ is set from the file we actually loaded regardless.
void HtmlContainer::set_base_url(const char* /*base_url*/) {}
void HtmlContainer::link(const std::shared_ptr<litehtml::document>& /*doc*/, const litehtml::element::ptr& /*el*/) {}

void HtmlContainer::on_anchor_click(const char* url, const litehtml::element::ptr& /*el*/) {
    // Resolved eagerly, against docDir_ AS IT STANDS RIGHT NOW (the document
    // that was actually clicked in) -- take_clicked_link() may be drained
    // after docDir_ has moved on to whatever page this click navigates to.
    if (url) clickedHref_ = resolve_local(url);
}

void HtmlContainer::on_mouse_event(const litehtml::element::ptr& /*el*/, litehtml::mouse_event /*event*/) {}

void HtmlContainer::set_cursor(const char* cursor) {
    // Recorded rather than applied: the panel is one widget inside a larger
    // app, so it reports link-hover through over_link() and lets the view
    // decide, instead of reaching for the global SDL cursor.
    overLink_ = cursor && std::string(cursor) == "pointer";
}

void HtmlContainer::transform_text(std::string& text, litehtml::text_transform tt) {
    if (tt == litehtml::text_transform_uppercase) {
        for (char& c : text) c = (char) std::toupper((unsigned char) c);
    } else if (tt == litehtml::text_transform_lowercase) {
        for (char& c : text) c = (char) std::tolower((unsigned char) c);
    } else if (tt == litehtml::text_transform_capitalize) {
        bool startOfWord = true;
        for (char& c : text) {
            if (std::isspace((unsigned char) c)) { startOfWord = true; continue; }
            if (startOfWord) c = (char) std::toupper((unsigned char) c);
            startOfWord = false;
        }
    }
}

void HtmlContainer::import_css(std::string& text, const std::string& url, std::string& /*baseurl*/) {
    // No network fetch -- litehtml only ever asks this for a LOCAL stylesheet
    // (a <link rel=stylesheet> or @import in a page under docDir_, e.g. the
    // vendored manual's csound.css); anything that doesn't resolve to a file
    // there comes back empty and is simply skipped.
    text.clear();
    const std::string path = resolve_local(url);
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
}

void HtmlContainer::set_clip(const litehtml::position& pos, const litehtml::border_radiuses& /*bdr_radius*/) {
    if (captureOnly_) return;   // nothing is being painted, and baseClip_ does not apply
    const SDL_Rect want = to_sdl_rect(pos);
    const SDL_Rect& parent = clipStack_.empty() ? baseClip_ : clipStack_.back();
    SDL_Rect clipped;
    if (!intersect(want, parent, clipped)) clipped = SDL_Rect{ parent.x, parent.y, 0, 0 };
    clipStack_.push_back(clipped);
    if (ren_) SDL_RenderSetClipRect(ren_, &clipStack_.back());
}

void HtmlContainer::del_clip() {
    if (captureOnly_) return;   // set_clip pushed nothing
    if (!clipStack_.empty()) clipStack_.pop_back();
    if (ren_) SDL_RenderSetClipRect(ren_, clipStack_.empty() ? &baseClip_ : &clipStack_.back());
}

void HtmlContainer::get_viewport(litehtml::position& viewport) const {
    viewport = litehtml::position(0, 0, (litehtml::pixel_t) vpW_, (litehtml::pixel_t) vpH_);
}

litehtml::element::ptr HtmlContainer::create_element(const char* /*tag_name*/,
                                                     const litehtml::string_map& /*attributes*/,
                                                     const std::shared_ptr<litehtml::document>& /*doc*/) {
    return nullptr; // no custom elements; litehtml falls back to its own
}

void HtmlContainer::get_media_features(litehtml::media_features& media) const {
    media.type          = litehtml::media_type_screen;
    media.width         = (litehtml::pixel_t) vpW_;
    media.height        = (litehtml::pixel_t) vpH_;
    media.device_width  = (litehtml::pixel_t) vpW_;
    media.device_height = (litehtml::pixel_t) vpH_;
    media.color         = 8;
    media.color_index   = 256;
    media.monochrome    = 0;
    media.resolution    = 96;
}

void HtmlContainer::get_language(std::string& language, std::string& culture) const {
    language = "en";
    culture.clear();
}

} // namespace ui
