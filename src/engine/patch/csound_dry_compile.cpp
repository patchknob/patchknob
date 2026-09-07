#include "csound_dry_compile.h"

#ifdef PATCHKNOB_HAVE_CSOUND
  #if __has_include(<csound/csound.h>)
    #include <csound/csound.h>
  #else
    #include <csound.h>
  #endif
  #include <cstdarg>
  #include <cstdio>
  #include <mutex>
#endif

namespace PatchKnob { namespace engine {

#ifndef PATCHKNOB_HAVE_CSOUND

CsoundDryCompileResult csoundDryCompile(const std::string&, double) {
    CsoundDryCompileResult r;
    r.available = false;
    r.ok = false;
    r.diagnostics = "Csound support was not built into this binary.";
    return r;
}

#else

namespace {

//  Csound's message callback carries no user pointer on every build, so the
//  sink is keyed by the instance under a lock. Dry compiles are rare (once per
//  assistant round), so a mutex here costs nothing and keeps two concurrent
//  compiles from interleaving their diagnostics.
std::mutex        g_sinkMx;
std::string*      g_sink = nullptr;

void capture(CSOUND*, int /*attr*/, const char* fmt, va_list args) {
    if (!fmt) return;
    char buf[2048];
    //  va_list is single-use; vsnprintf consumes it exactly once.
    const int n = std::vsnprintf(buf, sizeof buf, fmt, args);
    if (n <= 0) return;
    std::lock_guard<std::mutex> lk(g_sinkMx);
    if (g_sink) g_sink->append(buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

} // namespace

CsoundDryCompileResult csoundDryCompile(const std::string& csd, double sampleRate) {
    CsoundDryCompileResult res;
    res.available = true;
    if (csd.empty()) {
        res.diagnostics = "The document is empty.";
        return res;
    }

    CSOUND* cs = csoundCreate(nullptr);
    if (!cs) {
        res.diagnostics = "csoundCreate failed.";
        return res;
    }

    std::string log;
    {
        std::lock_guard<std::mutex> lk(g_sinkMx);
        g_sink = &log;
    }
    csoundSetMessageCallback(cs, &capture);

    csoundSetOption(cs, "-n");     // no audio device
    csoundSetOption(cs, "-d");     // no display windows
    csoundSetOption(cs, "--nodisplays");
    char sropt[48];
    std::snprintf(sropt, sizeof sropt, "--sample-rate=%d", (int)(sampleRate + 0.5));
    csoundSetOption(cs, sropt);

    const int rc = csoundCompileCsdText(cs, csd.c_str());
    res.ok = (rc == 0);

    //  Detach the sink BEFORE destroying: teardown emits messages too, and the
    //  local string is about to go out of scope.
    {
        std::lock_guard<std::mutex> lk(g_sinkMx);
        g_sink = nullptr;
    }
    csoundSetMessageCallback(cs, nullptr);
    csoundCleanup(cs);
    csoundDestroy(cs);

    res.diagnostics = log;
    if (!res.ok && res.diagnostics.empty()) {
        char b[80];
        std::snprintf(b, sizeof b, "Csound rejected the document (error %d).", rc);
        res.diagnostics = b;
    }
    return res;
}

#endif  // PATCHKNOB_HAVE_CSOUND

}} // namespace PatchKnob::engine
