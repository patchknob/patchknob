#include "cardinal_svg_textures.h"

#include <png.h>

#include <SDL.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

constexpr std::array<char, 8> kPackMagic = { 'S', '2', '4', 'S', 'V', 'G', 'P', '1' };
constexpr uint32_t kPackVersion = 1;

template <typename T>
bool readValue(std::istream& stream, T& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}

std::string packPath()
{
    char* base = SDL_GetBasePath();
    if (!base) return "cardinal_svg.pak";
    std::string path(base);
    SDL_free(base);
    return path + "cardinal_svg.pak";
}

std::string resolvePackPath(const std::string& requested)
{
    return requested.empty() ? packPath() : requested;
}

} // namespace

CardinalSvgTextures::~CardinalSvgTextures()
{
    clearTextures();
}

void CardinalSvgTextures::clearTextures()
{
    for (auto& item : m_textures)
        if (item.second) SDL_DestroyTexture(item.second);
    m_textures.clear();
}

bool CardinalSvgTextures::loadPack(const std::string& packPath)
{
    Pack& pack = m_packs[packPath];
    if (pack.loaded) return !pack.entries.empty();
    pack.loaded = true;

    std::ifstream stream(packPath, std::ios::binary);
    if (!stream) return false;

    std::array<char, 8> magic = {};
    uint32_t version = 0;
    uint32_t count = 0;
    uint64_t indexOffset = 0;
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != kPackMagic || !readValue(stream, version) ||
        !readValue(stream, count) || !readValue(stream, indexOffset) || version != kPackVersion)
        return false;

    stream.seekg(static_cast<std::streamoff>(indexOffset));
    if (!stream) return false;
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t pathSize = 0;
        Entry entry;
        if (!readValue(stream, pathSize) || !readValue(stream, entry.width) ||
            !readValue(stream, entry.height) || !readValue(stream, entry.offset) ||
            !readValue(stream, entry.size) || pathSize == 0 || pathSize > 65535)
            return false;
        std::string asset(pathSize, '\0');
        stream.read(asset.data(), pathSize);
        if (!stream) return false;
        pack.entries.emplace(std::move(asset), entry);
    }
    return !pack.entries.empty();
}

SDL_Texture* CardinalSvgTextures::textureFor(SDL_Renderer* renderer, const std::string& asset,
                                             const std::string& requestedPack)
{
    if (m_renderer != renderer) {
        clearTextures();
        m_renderer = renderer;
    }
    const std::string packPath = resolvePackPath(requestedPack);
    const std::string textureKey = packPath + '\n' + asset;
    const auto cached = m_textures.find(textureKey);
    if (cached != m_textures.end()) return cached->second;
    if (!loadPack(packPath)) return nullptr;
    const Pack& pack = m_packs[packPath];
    const auto found = pack.entries.find(asset);
    if (found == pack.entries.end()) return nullptr;

    std::ifstream stream(packPath, std::ios::binary);
    if (!stream || found->second.size == 0 || found->second.size > 64u * 1024u * 1024u) return nullptr;
    std::vector<unsigned char> encoded(static_cast<size_t>(found->second.size));
    stream.seekg(static_cast<std::streamoff>(found->second.offset));
    stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (!stream) return nullptr;

    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, encoded.data(), encoded.size())) return nullptr;
    image.format = PNG_FORMAT_RGBA;
    std::vector<png_byte> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels.data(), static_cast<int>(image.width), static_cast<int>(image.height), 32,
        static_cast<int>(image.width * 4), SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return nullptr;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    m_textures.emplace(textureKey, texture);
    return texture;
}

float CardinalSvgTextures::aspect(const std::string& asset, const std::string& requestedPack)
{
    const std::string packPath = resolvePackPath(requestedPack);
    if (!loadPack(packPath)) return 0.f;
    const Pack& pack = m_packs[packPath];
    const auto found = pack.entries.find(asset);
    if (found == pack.entries.end() || found->second.height == 0) return 0.f;
    return static_cast<float>(found->second.width) / static_cast<float>(found->second.height);
}

bool CardinalSvgTextures::draw(SDL_Renderer* renderer, const std::string& asset,
                               const SDL_Rect& destination, const std::string& requestedPack)
{
    const std::string packPath = resolvePackPath(requestedPack);
    if (!loadPack(packPath)) return false;
    const Pack& pack = m_packs[packPath];
    const auto found = pack.entries.find(asset);
    if (found == pack.entries.end() || found->second.width == 0 || found->second.height == 0) return false;
    SDL_Texture* texture = textureFor(renderer, asset, packPath);
    if (!texture) return false;

    SDL_Rect target = destination;
    const float textureAspect = static_cast<float>(found->second.width) / found->second.height;
    const float destinationAspect = destination.h > 0 ? static_cast<float>(destination.w) / destination.h : textureAspect;
    if (destinationAspect > textureAspect) {
        target.w = static_cast<int>(std::lround(destination.h * textureAspect));
        target.x += (destination.w - target.w) / 2;
    }
    else if (destinationAspect < textureAspect) {
        target.h = static_cast<int>(std::lround(destination.w / textureAspect));
        target.y += (destination.h - target.h) / 2;
    }
    return SDL_RenderCopy(renderer, texture, nullptr, &target) == 0;
}
