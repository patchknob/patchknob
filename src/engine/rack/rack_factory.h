//----------------------------------------------------------------------------
//  src/engine/rack/rack_factory.h
//
//  Module registry + factory.  Every built-in module type is described by a
//  ModuleType {slug,name,category,role,make}.  The generic SDL editor uses the
//  registry to populate its searchable "add module" palette, and RackEngine
//  uses make() to instantiate.  Registration is EXPLICIT (registerBuiltins())
//  rather than via file-scope static initializers, so nothing is stripped when
//  this is linked as a static library.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_FACTORY_H
#define PATCHKNOB_ENGINE_RACK_FACTORY_H

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rack.hpp"

namespace rackx {

// Special roles let RackEngine wire a module to the outside world (the node's
// stereo audio + MIDI).  Normal modules only ever see cables.
enum class Role { Normal, AudioOut, AudioIn, MidiCV };

class ModuleHandle {
public:
    using Destroy = void (*)(rack::engine::Module*);

    ModuleHandle() = default;
    template <typename T, typename Deleter>
    ModuleHandle(std::unique_ptr<T, Deleter>&& module)
        : m_module(module.release()), m_destroy(&destroyNative) {}
    ModuleHandle(rack::engine::Module* module, Destroy destroy)
        : m_module(module), m_destroy(destroy ? destroy : &destroyNative) {}
    ~ModuleHandle() { reset(); }

    ModuleHandle(const ModuleHandle&) = delete;
    ModuleHandle& operator=(const ModuleHandle&) = delete;
    ModuleHandle(ModuleHandle&& other) noexcept
        : m_module(other.m_module), m_destroy(other.m_destroy) {
        other.m_module = nullptr;
    }
    ModuleHandle& operator=(ModuleHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        m_module = other.m_module;
        m_destroy = other.m_destroy;
        other.m_module = nullptr;
        return *this;
    }

    rack::engine::Module* get() const { return m_module; }
    rack::engine::Module* operator->() const { return m_module; }
    explicit operator bool() const { return m_module != nullptr; }
    void reset() {
        if (m_module) m_destroy(m_module);
        m_module = nullptr;
        m_destroy = &destroyNative;
    }

private:
    static void destroyNative(rack::engine::Module* module) { delete module; }
    rack::engine::Module* m_module = nullptr;
    Destroy m_destroy = &destroyNative;
};

// Native, GPU-rendered panel geometry.  Coordinates are in Rack panel pixels
// before the editor zoom is applied, so translated modules retain their useful
// physical proportions without importing Cardinal's SVG widget layer.
enum class PanelControlStyle { Knob, Slider, Switch, Button, Gate, NumberBox };
enum class PanelLabelPlacement { None, Above, Below, Left, Right };

constexpr float RACK_HP_WIDTH = 15.24f;
constexpr float RACK_PANEL_HEIGHT = 380.f;

struct PanelElement {
    int               id = -1;
    float             x = 0.f;
    float             y = 0.f;
    float             radius = 0.f;
    PanelControlStyle style = PanelControlStyle::Knob;
    std::string       label;
    PanelLabelPlacement labelPlacement = PanelLabelPlacement::Below;
    float             width = 0.f;
    float             height = 0.f;
    std::string       widget;
    // Tab page this element belongs to (see PanelSpec::tabs).  -1 == always
    // visible (drawn on every tab -- e.g. shared I/O jacks).
    int               tab = -1;
};

struct PanelSpec {
    float width = 0.f;
    float height = 0.f;
    float headerHeight = 20.f;
    std::string textureAsset;
    std::string texturePack;
    std::vector<PanelElement> params;
    std::vector<PanelElement> inputs;
    std::vector<PanelElement> outputs;
    std::vector<PanelElement> lights;
    // Optional tab pages.  When non-empty the editor draws a tab bar under the
    // title and shows only elements whose PanelElement::tab matches the active
    // tab (plus tab == -1 elements, shown on every tab).
    std::vector<std::string> tabs;

    bool valid() const { return width > 0.f && height > 0.f; }
    static PanelSpec fromHp(int hp) {
        PanelSpec panel;
        panel.width = hp > 0 ? hp * RACK_HP_WIDTH : RACK_HP_WIDTH;
        panel.height = RACK_PANEL_HEIGHT;
        return panel;
    }
};

struct ModuleType {
    std::string slug;        // stable id, e.g. "VCO"
    std::string name;        // display name, e.g. "VCO-1"
    std::string category;    // "Oscillator" / "Filter" / "Envelope" / "I/O" ...
    Role        role = Role::Normal;
    std::function<ModuleHandle()> make;
    PanelSpec   panel;
    bool        paletteVisible = true;
};

// The global registry (populated once by registerBuiltins()).
std::vector<ModuleType>& registry();
void                      registerBuiltins();     // idempotent
const ModuleType*         findType(const std::string& slug);
ModuleHandle               createModule(const std::string& slug);
Role                      roleOf(const std::string& slug);

// Helper used by rack_modules.cpp to add one entry.
inline void addType(std::string slug, std::string name, std::string category, Role role,
                    std::function<ModuleHandle()> make,
                    PanelSpec panel = {}, bool paletteVisible = true) {
    registry().push_back(ModuleType{ std::move(slug), std::move(name), std::move(category),
                                     role, std::move(make), std::move(panel), paletteVisible });
}

} // namespace rackx

#endif
