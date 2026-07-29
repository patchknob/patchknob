#ifndef PATCHKNOB_SDLUI_CARDINAL_SVG_TEXTURES_H
#define PATCHKNOB_SDLUI_CARDINAL_SVG_TEXTURES_H

#include <SDL.h>

#include <cstdint>
#include <string>
#include <unordered_map>

class CardinalSvgTextures {
public:
    ~CardinalSvgTextures();
    CardinalSvgTextures() = default;
    CardinalSvgTextures(const CardinalSvgTextures&) = delete;
    CardinalSvgTextures& operator=(const CardinalSvgTextures&) = delete;

    float aspect(const std::string& asset, const std::string& pack = {});
    bool draw(SDL_Renderer* renderer, const std::string& asset, const SDL_Rect& destination,
              const std::string& pack = {});

private:
    struct Entry {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    struct Pack {
        bool loaded = false;
        std::unordered_map<std::string, Entry> entries;
    };

    bool loadPack(const std::string& packPath);
    SDL_Texture* textureFor(SDL_Renderer* renderer, const std::string& asset,
                            const std::string& packPath);
    void clearTextures();

    SDL_Renderer* m_renderer = nullptr;
    std::unordered_map<std::string, Pack> m_packs;
    std::unordered_map<std::string, SDL_Texture*> m_textures;
};

#endif
