//----------------------------------------------------------------------------
//  src/engine/rack/rack_dynamic_modules.cpp
//----------------------------------------------------------------------------
#include "rack_dynamic_modules.h"

#include "rack_factory.h"
#include "../vst2/seh_guard.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace rackx {
namespace {

std::string readText(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::string jsonString(const std::string& text, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos) return {};
    const std::size_t colon = text.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return {};
    const std::size_t firstQuote = text.find('"', colon + 1);
    if (firstQuote == std::string::npos) return {};
    std::string value;
    for (std::size_t index = firstQuote + 1; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '\\' && index + 1 < text.size()) {
            value += text[++index];
            continue;
        }
        if (character == '"') return value;
        value += character;
    }
    return {};
}

float jsonNumber(const std::string& text, const char* key, float fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos) return fallback;
    const std::size_t colon = text.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return fallback;
    char* end = nullptr;
    const float value = std::strtof(text.c_str() + colon + 1, &end);
    return end == text.c_str() + colon + 1 || !std::isfinite(value) || value <= 0.f
        ? fallback : value;
}

float jsonObjectNumber(const std::string& text, const char* key, float fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos) return fallback;
    const std::size_t colon = text.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return fallback;
    char* end = nullptr;
    const float value = std::strtof(text.c_str() + colon + 1, &end);
    return end == text.c_str() + colon + 1 || !std::isfinite(value) ? fallback : value;
}

std::string jsonObjectString(const std::string& text, const char* key)
{
    return jsonString(text, key);
}

std::string jsonArrayForKey(const std::string& text, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos) return {};
    const std::size_t begin = text.find('[', keyPos + needle.size());
    if (begin == std::string::npos) return {};
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = begin; index < text.size(); ++index) {
        const char character = text[index];
        if (inString) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') inString = false;
            continue;
        }
        if (character == '"') inString = true;
        else if (character == '[') ++depth;
        else if (character == ']' && --depth == 0)
            return text.substr(begin + 1, index - begin - 1);
    }
    return {};
}

std::vector<std::string> jsonObjectsInArray(const std::string& arrayText)
{
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    std::size_t begin = std::string::npos;
    for (std::size_t index = 0; index < arrayText.size(); ++index) {
        const char character = arrayText[index];
        if (inString) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') inString = false;
            continue;
        }
        if (character == '"') inString = true;
        else if (character == '{') {
            if (depth++ == 0) begin = index;
        }
        else if (character == '}' && depth > 0 && --depth == 0 && begin != std::string::npos) {
            objects.push_back(arrayText.substr(begin, index - begin + 1));
            begin = std::string::npos;
        }
    }
    return objects;
}

PanelControlStyle panelStyleFromString(const std::string& style)
{
    if (style == "slider") return PanelControlStyle::Slider;
    if (style == "switch") return PanelControlStyle::Switch;
    if (style == "button") return PanelControlStyle::Button;
    if (style == "gate") return PanelControlStyle::Gate;
    return PanelControlStyle::Knob;
}

void addManifestControls(PanelSpec& panel, const std::string& manifest,
                         const char* key, std::vector<PanelElement>& destination)
{
    for (const std::string& object : jsonObjectsInArray(jsonArrayForKey(manifest, key))) {
        const int id = static_cast<int>(std::lround(jsonObjectNumber(object, "id", -1.f)));
        const float x = jsonObjectNumber(object, "x", -1.f);
        const float y = jsonObjectNumber(object, "y", -1.f);
        if (id < 0 || x < 0.f || y < 0.f) continue;
        PanelElement element;
        element.id = id;
        element.x = x;
        element.y = y;
        element.radius = jsonObjectNumber(object, "radius", 8.f);
        element.style = panelStyleFromString(jsonObjectString(object, "style"));
        element.labelPlacement = PanelLabelPlacement::None;
        element.width = jsonObjectNumber(object, "width", 0.f);
        element.height = jsonObjectNumber(object, "height", 0.f);
        element.widget = jsonObjectString(object, "widget");
        destination.push_back(std::move(element));
    }
}

void addGenericControls(PanelSpec& panel, const rack::engine::Module& module)
{
    const float width = panel.width;
    const float height = panel.height;
    const int paramCount = static_cast<int>(module.params.size());
    const int inputCount = static_cast<int>(module.inputs.size());
    const int outputCount = static_cast<int>(module.outputs.size());
    const int lightCount = static_cast<int>(module.lights.size());
    const int columns = std::max(1, std::min(4, paramCount));
    const int rows = paramCount > 0 ? (paramCount + columns - 1) / columns : 0;
    const float paramXStep = width / static_cast<float>(columns + 1);
    const float paramYStep = rows > 0 ? (height * 0.48f) / static_cast<float>(rows + 1) : 0.f;
    const float controlRadius = std::max(5.f, std::min(14.f, width * 0.12f));

    for (int index = 0; index < paramCount; ++index) {
        const int row = index / columns;
        const int column = index % columns;
        panel.params.push_back({ index, paramXStep * (column + 1),
                                 height * 0.12f + paramYStep * (row + 1),
                                 controlRadius, PanelControlStyle::Knob, {},
                                 PanelLabelPlacement::None });
    }

    const int portRows = std::max(inputCount, outputCount);
    const float portYStep = portRows > 0 ? (height * 0.28f) / static_cast<float>(portRows + 1) : 0.f;
    const float portRadius = std::max(5.f, std::min(10.f, width * 0.1f));
    for (int index = 0; index < inputCount; ++index)
        panel.inputs.push_back({ index, width * 0.25f, height * 0.66f + portYStep * (index + 1),
                                 portRadius, PanelControlStyle::Knob, {}, PanelLabelPlacement::None });
    for (int index = 0; index < outputCount; ++index)
        panel.outputs.push_back({ index, width * 0.75f, height * 0.66f + portYStep * (index + 1),
                                  portRadius, PanelControlStyle::Knob, {}, PanelLabelPlacement::None });
    for (int index = 0; index < lightCount; ++index)
        panel.lights.push_back({ index, width * 0.5f, height * 0.1f + 8.f * index,
                                 3.f, PanelControlStyle::Knob, {}, PanelLabelPlacement::None });
}

std::vector<fs::path> moduleRoots()
{
    std::vector<fs::path> roots;
    std::error_code error;
    if (const char* configured = std::getenv("PATCHKNOB_RACK_MODULE_DIR"))
        roots.emplace_back(configured);
    roots.push_back(fs::current_path(error) / "modules");
#ifdef _WIN32
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length > 0 && length < executable.size()) {
        executable.resize(length);
        const fs::path executablePath(executable);
        roots.push_back(executablePath.parent_path() / "modules");
        roots.push_back(executablePath.parent_path().parent_path() / "modules");
    }
#endif
    return roots;
}

// Coarsen the manifest category for the picker's TYPE facet.  Manifests store
// "Brand / Tag"; the functional tag (Oscillator, Filter, ...) is the useful
// grouping and is shared across plugins, so hundreds of "Brand / Tag" pairs
// collapse to a couple dozen types.  Tags that were mangled to 1-2 chars by an
// old importer (e.g. "Sonus Modular / B"), or the generic "Bridge" fallback,
// fall back to the always-clean brand name instead.
std::string tidyCategory(const std::string& raw, const std::string& plugin)
{
    const std::size_t sep = raw.rfind('/');
    if (sep == std::string::npos)
        return raw.empty() ? plugin : raw;
    auto trim = [](std::string value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        return value;
    };
    const std::string tag = trim(raw.substr(sep + 1));
    if (tag.size() <= 2 || tag == "Bridge") {
        const std::string brand = trim(raw.substr(0, sep));
        return brand.empty() ? plugin : brand;
    }
    return tag;
}

#ifdef _WIN32
using BridgeVersion = std::uint32_t (*)();
using BridgeCreate = rack::engine::Module* (*)();
using BridgeDestroy = void (*)(rack::engine::Module*);

std::vector<HMODULE>& bridgeLibraries()
{
    static auto* libraries = new std::vector<HMODULE>();
    return *libraries;
}

// A third-party bridge module can fault in its constructor/destructor (bad
// port index, null deref, div-by-zero from an uninitialised rate).  Run those
// calls under the SEH guard so one broken module is skipped, not fatal.  Reuses
// the VST2 fault guard (RtlCaptureContext + process-wide VEH; see seh_guard.h).
struct BridgeCreateCtx { BridgeCreate fn; rack::engine::Module* result; };
void bridgeCreateThunk(void* pointer)
{
    auto* context = static_cast<BridgeCreateCtx*>(pointer);
    context->result = context->fn();
}
rack::engine::Module* guardedCreate(BridgeCreate create)
{
    if (!create) return nullptr;
    BridgeCreateCtx context{ create, nullptr };
    std::uint32_t faultCode = 0;
    if (!PatchKnob::engine::seh_guarded_call(bridgeCreateThunk, &context, &faultCode)) {
        std::fprintf(stderr, "[rack] bridge create faulted (0x%08X); skipping module\n", faultCode);
        return nullptr;
    }
    return context.result;
}

// One deferred bridge module.  Registration records everything the picker needs
// (panel texture + control layout come straight from the manifest); the DLL is
// NOT loaded until the module is first instantiated -- see ensureLoaded().  A
// stable heap address is captured by the factory lambda, so this is allocated
// once and never moved.
struct BridgeModule {
    std::wstring   libraryPathW;
    std::string    slug;
    HMODULE        library = nullptr;
    BridgeCreate   create = nullptr;
    BridgeDestroy  destroy = nullptr;
    bool           triedLoad = false;
    bool           genericFallback = false;   // manifest had no controls
    bool           genericFilled = false;
};

std::vector<std::unique_ptr<BridgeModule>>& bridgeModules()
{
    static auto* modules = new std::vector<std::unique_ptr<BridgeModule>>();
    return *modules;
}

// Swap the device in from disk on first use: LoadLibraryW + resolve exports.
// Returns false (once) if the DLL is missing or fails the ABI check.
bool ensureLoaded(BridgeModule* bridge)
{
    if (bridge->create) return true;
    if (bridge->triedLoad) return false;
    bridge->triedLoad = true;

    const bool trace = std::getenv("PATCHKNOB_RACK_TRACE") != nullptr;
    if (trace) { std::fprintf(stderr, "[rack] dlopen %s\n", bridge->slug.c_str()); std::fflush(stderr); }

    HMODULE library = LoadLibraryW(bridge->libraryPathW.c_str());
    if (!library) return false;
    const auto version = reinterpret_cast<BridgeVersion>(GetProcAddress(library, "PatchKnob_rack_bridge_api_version"));
    const auto create = reinterpret_cast<BridgeCreate>(GetProcAddress(library, "PatchKnob_rack_bridge_create"));
    const auto destroy = reinterpret_cast<BridgeDestroy>(GetProcAddress(library, "PatchKnob_rack_bridge_destroy"));
    if (!version || !create || !destroy || version() != 1) {
        FreeLibrary(library);
        return false;
    }
    bridge->library = library;
    bridge->create = create;
    bridge->destroy = destroy;
    bridgeLibraries().push_back(library);
    return true;
}

// Factory body: load-on-demand, construct under the fault guard, and -- the
// first time a manifest-controls-less module is built -- backfill generic
// controls from the live instance so the panel stays interactive.
rack::engine::Module* bridgeMake(BridgeModule* bridge)
{
    if (!ensureLoaded(bridge)) return nullptr;
    rack::engine::Module* module = guardedCreate(bridge->create);
    if (!module) return nullptr;
    if (bridge->genericFallback && !bridge->genericFilled) {
        bridge->genericFilled = true;
        for (ModuleType& type : registry()) {
            if (type.slug != bridge->slug) continue;
            if (type.panel.params.empty() && type.panel.inputs.empty() &&
                type.panel.outputs.empty() && type.panel.lights.empty())
                addGenericControls(type.panel, *module);
            break;
        }
    }
    return module;
}

// Register a bridge module from its manifest WITHOUT loading the DLL.  Cheap:
// read + parse JSON + one fs::exists() to skip modules whose build failed.
bool registerBridgeManifest(const fs::path& manifestPath)
{
    const std::string manifest = readText(manifestPath);
    const std::string libraryName = jsonString(manifest, "library");
    const std::string plugin = jsonString(manifest, "plugin");
    const std::string moduleSlug = jsonString(manifest, "moduleSlug");
    const std::string moduleType = jsonString(manifest, "moduleType");
    if (manifest.empty() || libraryName.empty() || plugin.empty() || moduleType.empty()) return false;

    const std::string slug = jsonString(manifest, "slug").empty()
        ? plugin + "." + (moduleSlug.empty() ? moduleType : moduleSlug)
        : jsonString(manifest, "slug");
    if (findType(slug)) return false;

    // Only register modules whose bridge DLL is actually present (skips the
    // failed-build manifests without paying a LoadLibrary).
    const fs::path libraryPath = manifestPath.parent_path() / libraryName;
    std::error_code libraryError;
    if (!fs::exists(libraryPath, libraryError)) return false;

    const bool trace = std::getenv("PATCHKNOB_RACK_TRACE") != nullptr;
    if (trace) { std::fprintf(stderr, "[rack] register %s\n", slug.c_str()); std::fflush(stderr); }

    std::string name = jsonString(manifest, "name");
    if (name.empty()) name = moduleSlug.empty() ? moduleType : moduleSlug;
    std::string category = jsonString(manifest, "category");
    if (category.empty()) category = plugin + " / Bridge";
    category = tidyCategory(category, plugin);

    PanelSpec panel;
    const std::string panelAsset = jsonString(manifest, "panelAsset");
    const std::string assetPack = jsonString(manifest, "assetPack");
    if (!panelAsset.empty() && !assetPack.empty()) {
        panel.width = jsonNumber(manifest, "panelWidth", RACK_HP_WIDTH * 8.f);
        panel.height = jsonNumber(manifest, "panelHeight", RACK_PANEL_HEIGHT);
        panel.textureAsset = panelAsset;
        panel.texturePack = (manifestPath.parent_path() / fs::path(assetPack)).lexically_normal().string();
        addManifestControls(panel, manifest, "params", panel.params);
        addManifestControls(panel, manifest, "inputs", panel.inputs);
        addManifestControls(panel, manifest, "outputs", panel.outputs);
        addManifestControls(panel, manifest, "lights", panel.lights);
    }
    const bool genericFallback = !panel.textureAsset.empty() &&
        panel.params.empty() && panel.inputs.empty() &&
        panel.outputs.empty() && panel.lights.empty();

    auto bridge = std::make_unique<BridgeModule>();
    bridge->libraryPathW = libraryPath.wstring();
    bridge->slug = slug;
    bridge->genericFallback = genericFallback;
    BridgeModule* bridgePtr = bridge.get();
    bridgeModules().push_back(std::move(bridge));

    addType(slug, name, category, Role::Normal,
            [bridgePtr]() {
                // Construct FIRST (ensureLoaded resolves destroy), THEN read
                // destroy -- arg eval order is unspecified, and a null destroy
                // here would make ModuleHandle free a DLL-allocated module with
                // the host's operator delete (cross-heap corruption).
                rack::engine::Module* module = bridgeMake(bridgePtr);
                return ModuleHandle(module, bridgePtr->destroy);
            },
            std::move(panel));
    return true;
}
#endif

} // namespace

void registerDynamicModules()
{
#ifdef _WIN32
    static bool registered = false;
    if (registered) return;
    registered = true;

    const bool trace = std::getenv("PATCHKNOB_RACK_TRACE") != nullptr;
    const auto startClock = std::chrono::steady_clock::now();
    int count = 0;

    for (const fs::path& root : moduleRoots()) {
        std::error_code error;
        if (!fs::is_directory(root, error)) continue;
        for (fs::recursive_directory_iterator iterator(root, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (iterator->is_directory(error) && iterator->path().filename() == "build") {
                iterator.disable_recursion_pending();
                continue;
            }
            if (iterator->is_regular_file(error) && iterator->path().filename() == "bridge_module.json")
                if (registerBridgeManifest(iterator->path())) ++count;
        }
        if (!error) break;
    }

    if (trace) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startClock).count();
        std::fprintf(stderr, "[rack] registered %d modules in %lld ms (DLLs load on demand)\n",
                     count, static_cast<long long>(ms));
        std::fflush(stderr);
    }
#endif
}

} // namespace rackx
