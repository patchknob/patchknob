//----------------------------------------------------------------------------
//  src/engine/rack/rack_factory.cpp -- registry storage + lookup.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "rack_dynamic_modules.h"

namespace rackx {

// Defined in rack_modules*.cpp: add ModuleTypes via addType().
void registerBuiltinModules();
void registerBuiltinModulesExt();
void registerCardinalModules();

std::vector<ModuleType>& registry() {
    static std::vector<ModuleType> g;
    return g;
}

void registerBuiltins() {
    if (!registry().empty()) return;      // idempotent
    registerBuiltinModules();
    registerBuiltinModulesExt();
    registerCardinalModules();
    registerDynamicModules();
}

const ModuleType* findType(const std::string& slug) {
    for (const auto& t : registry()) if (t.slug == slug) return &t;
    return nullptr;
}

ModuleHandle createModule(const std::string& slug) {
    const ModuleType* t = findType(slug);
    if (!t || !t->make) return {};
    return t->make();
}

Role roleOf(const std::string& slug) {
    const ModuleType* t = findType(slug);
    return t ? t->role : Role::Normal;
}

} // namespace rackx
