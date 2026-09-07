//----------------------------------------------------------------------------
//  src/engine/cdp/cdp_process.cpp -- registry plumbing.  See cdp_process.h.
//----------------------------------------------------------------------------
#include "cdp_process.h"

#include <algorithm>

namespace PatchKnob { namespace cdp {

// Filled by processes/cdp_registry.cpp as ports land.
std::vector<Process>& mutable_registry() {
    static std::vector<Process> r;
    return r;
}
void register_ported_processes();      // processes/cdp_registry.cpp

// Registration is spread across one file per CDP family, each written
// independently.  Rather than trust every one of them to fill in every optional
// field, normalise once here: a missing latencyFrames is the omission that keeps
// happening (it gets set on the streamable entries and forgotten on the rest),
// and leaving it null forces a null check on every caller.  A process with no
// declared latency has none.
static void normalise(std::vector<Process>& r) {
    for (Process& p : r) {
        if (!p.latencyFrames)
            p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        if (p.minInputs < 1) p.minInputs = 1;
        if (p.maxInputs < p.minInputs) p.maxInputs = p.minInputs;
        // A process that cannot stream must not carry a live instance factory:
        // the rack module keys off makeStream and would host it as if it could.
        if (!p.streamable) p.makeStream = nullptr;
        for (ParamSpec& s : p.params) {
            if (s.max < s.min) std::swap(s.min, s.max);
            if (s.def < s.min) s.def = s.min;
            if (s.def > s.max) s.def = s.max;
        }
    }
}

const std::vector<Process>& registry() {
    static bool once = [] {
        register_ported_processes();
        normalise(mutable_registry());
        return true;
    }();
    (void)once;
    return mutable_registry();
}

const Process* find(const std::string& slug) {
    for (const Process& p : registry()) if (p.slug == slug) return &p;
    return nullptr;
}

bool run(const std::string& slug, const std::vector<Buffer>& in,
         const std::vector<double>& params, Buffer& out, std::string& error,
         const Progress& progress) {
    const Process* p = find(slug);
    if (!p || !p->run) { error = "unknown CDP process: " + slug; return false; }
    if ((int)in.size() < p->minInputs) {
        error = p->name + " needs at least " + std::to_string(p->minInputs) + " source(s)";
        return false;
    }
    return p->run(in, params, out, error, progress);
}

} } // namespace PatchKnob::cdp
