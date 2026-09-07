//----------------------------------------------------------------------------
//  sdlui/views/csound_editor/ai_prefs.cpp
//----------------------------------------------------------------------------
#include "ai_prefs.h"

#include "engine/ai/claude_client.h"
#include "engine/ai/api_key_store.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ui { namespace aiprefs {

namespace {
std::string g_testPath;

const char* g_models[] = {
    PatchKnob::ai::kModelOpus,
    PatchKnob::ai::kModelSonnet,
    PatchKnob::ai::kModelHaiku,
};

bool knownModel(const std::string& m) {
    for (int i = 0; i < 3; ++i)
        if (m == g_models[i]) return true;
    return false;
}
} // namespace

const char* const* models() { return g_models; }
int model_count() { return 3; }

std::string path() {
    if (!g_testPath.empty()) return g_testPath;
    //  Same directory as the key file (the SDL pref dir), derived rather than
    //  duplicated so the two can never disagree.
    const std::string keyPath = PatchKnob::ai::apiKeyPath();
    const size_t slash = keyPath.find_last_of("/\\");
    if (slash == std::string::npos) return "patchknob_ai_prefs";
    return keyPath.substr(0, slash + 1) + "ai_prefs";
}

void set_path_for_test(const std::string& p) { g_testPath = p; }

Prefs load() {
    Prefs p;
    p.model = PatchKnob::ai::kModelOpus;
    std::ifstream f(path().c_str());
    if (!f) return p;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "model" && knownModel(v)) p.model = v;
        else if (k == "backend") p.useCli = v != "api";
        else if (k == "thinking") p.thinking = v != "0";
        else if (k == "max_tokens") {
            const int n = std::atoi(v.c_str());
            if (n >= 1024 && n <= 64000) p.maxTokens = n;
        }
    }
    return p;
}

bool save(const Prefs& p) {
    std::ofstream f(path().c_str(), std::ios::trunc);
    if (!f) return false;
    f << "backend=" << (p.useCli ? "cli" : "api") << "\n"
      << "model=" << (knownModel(p.model) ? p.model : PatchKnob::ai::kModelOpus) << "\n"
      << "thinking=" << (p.thinking ? 1 : 0) << "\n"
      << "max_tokens=" << p.maxTokens << "\n";
    return f.good();
}

}} // namespace ui::aiprefs
