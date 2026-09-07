//----------------------------------------------------------------------------
//  gpu_meter.cpp -- see gpu_meter.h.
//
//  KEPT IN ITS OWN TRANSLATION UNIT ON PURPOSE.  windows.h drags in rpcndr.h's
//  ::byte, which is ambiguous with std::byte under the using-directives the rest
//  of this tree compiles with.  Nothing here includes a PatchKnob header, so the
//  collision cannot happen.
//
//  HOW THE COUNTER WORKS.  Windows publishes one "GPU Engine" instance per
//  (process, adapter, engine-type) triple, named like
//      pid_12345_luid_0x00000000_0x0000C7A1_phys_0_eng_0_engtype_3D
//  Utilization Percentage on each is that engine's busy time.  Task Manager's
//  per-process figure is the SUM over the process's instances, which is what
//  this reproduces.  The instance list changes as engines go idle, so the
//  wildcard is re-expanded periodically rather than resolved once.
//----------------------------------------------------------------------------
#include "gpu_meter.h"

#ifdef _WIN32

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>       // PDH_MORE_DATA / PDH_CSTATUS_VALID_DATA
#include <string>
#include <vector>

namespace pkgpu {
namespace {

PDH_HQUERY               g_query   = nullptr;
std::vector<PDH_HCOUNTER> g_counters;
std::string              g_pidTag;          // "pid_1234_"
bool     g_tried    = false;                // init attempted
bool     g_ok       = false;
float    g_cached   = -1.f;
ULONGLONG g_lastSample = 0;
ULONGLONG g_lastRebind = 0;

// PDH needs two samples separated in time before a rate counter reads anything,
// so the first call always yields "unknown" rather than a bogus zero.
bool g_primed = false;

void rebind()
{
    for (PDH_HCOUNTER c : g_counters) PdhRemoveCounter(c);
    g_counters.clear();

    DWORD len = 0;
    const wchar_t* path = L"\\GPU Engine(*)\\Utilization Percentage";
    if (PdhExpandWildCardPathW(nullptr, path, nullptr, &len, 0)
            != PDH_MORE_DATA || len == 0)
        return;

    std::vector<wchar_t> buf(len);
    if (PdhExpandWildCardPathW(nullptr, path, buf.data(), &len, 0) != ERROR_SUCCESS)
        return;

    // Multi-sz: consecutive NUL-terminated strings, empty string terminates.
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        char narrow[512];
        const int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, narrow,
                                          (int) sizeof(narrow), nullptr, nullptr);
        if (n <= 0) continue;
        if (std::string(narrow).find(g_pidTag) == std::string::npos) continue;
        PDH_HCOUNTER c = nullptr;
        if (PdhAddCounterW(g_query, p, 0, &c) == ERROR_SUCCESS) g_counters.push_back(c);
    }
    g_primed = false;      // freshly added counters need their first sample
}

bool init()
{
    if (g_tried) return g_ok;
    g_tried = true;
    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) { g_query = nullptr; return false; }
    g_pidTag = "pid_" + std::to_string((unsigned long) GetCurrentProcessId()) + "_";
    rebind();
    g_ok = true;
    return true;
}

} // namespace

float load()
{
    if (!init()) return -1.f;

    const ULONGLONG now = GetTickCount64();
    // Twice a second is plenty for a meter and keeps PDH off the frame path.
    if (g_lastSample && now - g_lastSample < 500) return g_cached;
    g_lastSample = now;

    // Engines appear and disappear as work migrates; re-resolve occasionally so
    // the meter does not silently go dead after an engine goes idle.
    if (!g_lastRebind || now - g_lastRebind > 5000) { g_lastRebind = now; rebind(); }
    if (g_counters.empty()) { g_cached = -1.f; return g_cached; }

    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS) { g_cached = -1.f; return g_cached; }
    if (!g_primed) { g_primed = true; return g_cached; }   // need a second sample

    double sum = 0.0;
    for (PDH_HCOUNTER c : g_counters) {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS
            && v.CStatus == PDH_CSTATUS_VALID_DATA)
            sum += v.doubleValue;
    }
    if (sum < 0.0)   sum = 0.0;
    if (sum > 100.0) sum = 100.0;      // engines can briefly oversum past 100
    g_cached = (float) (sum / 100.0);
    return g_cached;
}

void shutdown()
{
    if (g_query) { PdhCloseQuery(g_query); g_query = nullptr; }
    g_counters.clear();
    g_ok = false;
}

} // namespace pkgpu

#elif defined(__linux__)

//----------------------------------------------------------------------------
//  Linux.  Two sources, best first:
//
//   1. PER-PROCESS, via /proc/self/fdinfo/*.  A DRM file descriptor exposes
//      "drm-engine-<name>: <n> ns" -- cumulative nanoseconds this process's
//      submissions kept that engine busy.  Busy fraction is the delta over the
//      wall-clock delta.  amdgpu and i915 both publish it on modern kernels.
//      This is the direct analogue of the Windows counter above.
//
//   2. WHOLE-GPU, via /sys/class/drm/card*/device/gpu_busy_percent (amdgpu).
//      Not per-process, so it includes the compositor and everything else --
//      but on a machine running a DAW full-screen that is close enough to be
//      useful, and it is better than showing nothing.  Only used when (1)
//      yields no engines.
//
//  Neither exists on NVIDIA's proprietary stack (that needs NVML); there the
//  meter reports unavailable rather than guessing.
//----------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <dirent.h>

namespace pkgpu {
namespace {

float      g_cached      = -1.f;
long long  g_lastNs      = 0;     // summed engine-busy ns at last sample
long long  g_lastWallNs  = 0;
long long  g_lastSampleMs = 0;
bool       g_primed      = false;

long long now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
long long now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Sum every drm-engine-* counter across our DRM fds, counting each DRM CLIENT
// once.  Returns false when this kernel/driver publishes none.
//
// ONE CLIENT, MANY FDS.  A process routinely holds several file descriptors
// onto the same /dev/dri/renderD* client -- Mesa dups the fd, and SDL, the GL
// context and any plugin that opens the device each end up with their own.
// Every one of those fds reports the SAME cumulative per-client counter, so
// summing fds blindly multiplies the answer by however many fds happen to be
// open (three, on the machine this was found on: a 36% frame cost read as
// 108%, clamped to a permanent 100%).  fdinfo gives each client a stable
// "drm-client-id"; dedupe on it and take one reading per distinct client.
// Genuinely separate clients -- a second device, a plugin with its own
// context -- keep their own id and are still summed, which is what we want.
bool read_fdinfo_ns(long long& outNs)
{
    DIR* d = opendir("/proc/self/fdinfo");
    if (!d) return false;
    // Small and short-lived: a handful of DRM fds, walked once per sample at
    // 2 Hz.  A linear scan beats a hash map at this size.
    std::vector<long long> seenClients;
    std::vector<long long> clientNs;
    bool found = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string path = std::string("/proc/self/fdinfo/") + e->d_name;
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        char line[256];
        long long engineNs = 0;
        long long clientId = -1;
        bool anyEngine = false;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "drm-client-id", 13) == 0) {
                const char* colon = std::strchr(line, ':');
                if (colon) std::sscanf(colon + 1, "%lld", &clientId);
                continue;
            }
            if (std::strncmp(line, "drm-engine-", 11) != 0) continue;
            const char* colon = std::strchr(line, ':');
            if (!colon) continue;
            long long v = 0;
            if (std::sscanf(colon + 1, "%lld", &v) == 1) { engineNs += v; anyEngine = true; }
        }
        std::fclose(f);
        if (!anyEngine) continue;
        found = true;
        if (clientId < 0) {
            // No id published: cannot tell a dup from a distinct client, so
            // keep the old summing behaviour rather than dropping the reading.
            seenClients.push_back(-1);
            clientNs.push_back(engineNs);
            continue;
        }
        size_t i = 0;
        for (; i < seenClients.size(); ++i) if (seenClients[i] == clientId) break;
        if (i == seenClients.size()) {          // first fd for this client
            seenClients.push_back(clientId);
            clientNs.push_back(engineNs);
        } else if (engineNs > clientNs[i]) {
            // Same client seen again.  The counters are per-client and should
            // be identical; if they differ at all it is because the file was
            // read a moment later, so keep the larger (newer) reading.
            clientNs[i] = engineNs;
        }
    }
    closedir(d);
    if (found) {
        long long total = 0;
        for (long long v : clientNs) total += v;
        outNs = total;
    }
    return found;
}

// amdgpu's whole-GPU busy percentage, 0..100.
bool read_sysfs_percent(int& outPct)
{
    for (int card = 0; card < 4; ++card) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/class/drm/card%d/device/gpu_busy_percent", card);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        int v = -1;
        const bool ok = (std::fscanf(f, "%d", &v) == 1);
        std::fclose(f);
        if (ok && v >= 0) { outPct = v; return true; }
    }
    return false;
}

} // namespace

float load()
{
    const long long nowMs = now_ms();
    if (g_lastSampleMs && nowMs - g_lastSampleMs < 500) return g_cached;
    g_lastSampleMs = nowMs;

    long long ns = 0;
    if (read_fdinfo_ns(ns)) {
        const long long wall = now_ns();
        if (!g_primed) {                     // need two samples for a rate
            g_primed = true; g_lastNs = ns; g_lastWallNs = wall;
            return g_cached;
        }
        const long long dBusy = ns   - g_lastNs;
        const long long dWall = wall - g_lastWallNs;
        g_lastNs = ns; g_lastWallNs = wall;
        if (dWall > 0 && dBusy >= 0) {
            double f = (double) dBusy / (double) dWall;
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;            // engines sum past 100% in parallel
            g_cached = (float) f;
            return g_cached;
        }
        return g_cached;
    }

    int pct = 0;
    if (read_sysfs_percent(pct)) {
        g_cached = (float) pct / 100.f;
        return g_cached;
    }

    g_cached = -1.f;
    return g_cached;
}

void shutdown() {}

} // namespace pkgpu

#else   // no per-process GPU counter on this platform

namespace pkgpu {
float load()     { return -1.f; }
void  shutdown() {}
}

#endif
