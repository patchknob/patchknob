//----------------------------------------------------------------------------
//  PatchKnob — unified plugin host + scanner implementation.
//  See plugin_host.h for the design rationale (out-of-process probing).
//----------------------------------------------------------------------------
#include "plugin_host.h"
#include "../sampler/sampler_instrument.h"
#include "../native/native_effects.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ----------------------------------------------------------------------------
// Shared by both platform halves below: turn probe_vst2 output into
// descriptors.
//
// A VST2 SHELL plugin is one file holding many effects, and probe_vst2 emits a
// SHELL_BEGIN..SHELL_END block per sub-plugin (each with its own
// "shell:0x........" UID) instead of the flat KEY=VALUE form. Reporting only
// the flat form -- which is all this used to understand -- meant the whole
// bundle showed up as a single entry with the shell wrapper's uniqueID, so
// instantiating it always got whatever sub-plugin the shell defaults to,
// whichever effect the user actually picked.
// ----------------------------------------------------------------------------
namespace PatchKnob { namespace engine {
namespace {

bool parseVst2ProbeOutput(const std::string& txt, const std::string& path,
                          const std::string& stem,
                          std::vector<PluginDescriptor>& out)
{
    auto value = [](const std::string& line) {
        size_t eq = line.find('=');
        return eq == std::string::npos ? std::string() : line.substr(eq + 1);
    };
    auto strip = [](std::string v) {
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        return v;
    };

    // Accumulate locally and only append on success, so a failed probe never
    // leaves half-parsed entries in the caller's list.
    std::vector<PluginDescriptor> got;
    std::istringstream ss(txt);
    std::string line;
    bool ok = false, inShell = false;
    std::map<std::string, std::string> flat, cur;

    auto emit = [&](std::map<std::string, std::string>& kv) {
        PluginDescriptor d;
        d.format       = PluginFormat::VST2;
        d.name         = !kv["NAME"].empty() ? kv["NAME"] : stem;
        d.vendor       = kv["VENDOR"];
        d.path         = path;
        d.uid          = kv["UID"];
        d.isInstrument = kv["ISSYNTH"] == "1";
        d.numAudioIn   = std::atoi(kv["AUDIOIN"].c_str());
        d.numAudioOut  = std::atoi(kv["AUDIOOUT"].c_str());
        // Bus grouping (probe_vst2 BUSIN/BUSOUT). Absent, empty or malformed --
        // an older probe binary, or a plugin that would not describe its pins --
        // leaves the layout unknown, and pluginFillDefaultBusLayout() then reads
        // the flat totals as one main bus, which is the pre-bus behaviour.
        pluginDecodeBusLayout(kv["BUSIN"],  d.audioInBuses);
        pluginDecodeBusLayout(kv["BUSOUT"], d.audioOutBuses);
        pluginFillDefaultBusLayout(d);
        got.push_back(d);
    };

    while (std::getline(ss, line)) {
        line = strip(line);
        if (line == "SHELL_BEGIN") { inShell = true; cur.clear(); continue; }
        if (line == "SHELL_END")   { if (inShell) emit(cur); inShell = false; continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        if (key == "OK") { ok = (value(line) == "1"); continue; }
        (inShell ? cur : flat)[key] = value(line);
    }

    if (!ok) return false;
    if (got.empty()) {                       // ordinary single-effect plugin
        if (flat.empty()) return false;
        emit(flat);
    }
    if (got.empty()) return false;
    out.insert(out.end(), got.begin(), got.end());
    return true;
}

} // namespace
}} // namespace PatchKnob::engine

#ifdef _WIN32

namespace PatchKnob { namespace engine {

// ----------------------------------------------------------------------------
// Factory functions provided by the sibling VST2 / VST3 host modules. They are
// linked in at integration time. For this module's standalone TEST we provide
// WEAK stub definitions (below, guarded by PATCHKNOB_HOST_WEAK_STUBS) so scan_test
// links without the real hosts.
// ----------------------------------------------------------------------------
extern IPluginInstance* createVst2Instance(const PluginDescriptor& desc);
extern IPluginInstance* createVst3Instance(const PluginDescriptor& desc);

namespace {

// ---- small string helpers --------------------------------------------------

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---- filesystem enumeration (Win32; no <filesystem> dependency) ------------

bool dirExists(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool fileExists(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool endsWithNoCase(const std::string& s, const std::string& suffix)
{
    if (s.size() < suffix.size()) return false;
    return _stricmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

// Recursively collect entries under `root` matching either a file extension
// (.dll) or a directory/file name ending (.vst3 — a .vst3 may be a single
// file OR a bundle directory). Depth-limited to keep big plugin trees sane.
void collect(const std::string& root, const std::string& ext,
             bool dirsAlsoMatch, int depth, std::vector<std::string>& out)
{
    if (depth < 0) return;

    std::string pattern = root + "\\*";
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = root + "\\" + name;

        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (isDir)
        {
            // A .vst3 bundle is a directory whose name ends in .vst3 — treat
            // the directory itself as the match (the SDK loader resolves the
            // bundle), and do NOT descend into it.
            if (dirsAlsoMatch && endsWithNoCase(name, ext))
                out.push_back(full);
            else
                collect(full, ext, dirsAlsoMatch, depth - 1, out);
        }
        else
        {
            if (endsWithNoCase(name, ext))
                out.push_back(full);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

// ---- PE architecture check (skip 32-bit DLLs cheaply, no LoadLibrary) -------
// Reads the COFF machine field from the PE header. Returns true for x86-64.
bool isPe64(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char dos[64] = {};
    f.read(dos, sizeof(dos));
    if (f.gcount() < 64) return false;
    if (dos[0] != 'M' || dos[1] != 'Z') return false;

    uint32_t peOff = *reinterpret_cast<uint32_t*>(&dos[0x3C]);
    f.seekg(peOff, std::ios::beg);

    char sig[4] = {};
    f.read(sig, 4);
    if (f.gcount() < 4 || sig[0] != 'P' || sig[1] != 'E' || sig[2] != 0 || sig[3] != 0)
        return false;

    uint16_t machine = 0;
    f.read(reinterpret_cast<char*>(&machine), 2);
    if (f.gcount() < 2) return false;

    return machine == 0x8664; // IMAGE_FILE_MACHINE_AMD64
}

// ---- run a child process, capture stdout, enforce a timeout ----------------
// Returns true if the process ran to completion within the timeout (regardless
// of its exit code); `output` gets its stdout. On timeout the child is killed
// and false is returned.
bool runCapture(const std::string& cmdLine, unsigned timeoutMs, std::string& output)
{
    output.clear();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return false;
    // The read end must not be inherited by the child.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};

    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');

    // CREATE_NO_WINDOW: keep any console/popups from a misbehaving probe quiet.
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr); // parent does not write
    if (!ok)
    {
        CloseHandle(rd);
        return false;
    }

    // Drain the pipe until EOF (child exited and closed wr) while watching the
    // clock. Reading blocks, so we poll the process handle in tandem.
    DWORD start = GetTickCount();
    bool timedOut = false;
    char buf[4096];
    for (;;)
    {
        // Has it finished?
        DWORD waited = WaitForSingleObject(pi.hProcess, 0);

        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
        {
            DWORD got = 0;
            if (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
                output.append(buf, got);
            continue;
        }

        if (waited == WAIT_OBJECT_0)
        {
            // Process exited; drain any remaining buffered output.
            DWORD got = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                   ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
                output.append(buf, got);
            break;
        }

        if (GetTickCount() - start > timeoutMs)
        {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }

        Sleep(5);
    }

    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return !timedOut;
}

std::string moduleDir()
{
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&moduleDir), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string exeDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string fileNameStem(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

} // namespace

// ----------------------------------------------------------------------------

PluginHost::PluginHost() = default;
PluginHost::~PluginHost() = default;

std::string PluginHost::findProbeExe(const char* exeName) const
{
    // Prefer next to this module / the running exe; fall back to bare name
    // (resolved against PATH / cwd by CreateProcess).
    std::string c1 = exeDir()    + "\\" + exeName;
    if (fileExists(c1)) return c1;
    std::string c2 = moduleDir() + "\\" + exeName;
    if (fileExists(c2)) return c2;
    return exeName;
}

namespace {
// The standard directories, in ONE place so scan() and scanFileSnapshot() can
// never drift apart (a snapshot over different directories than the scan walks
// would flag every boot as stale, or never flag at all).
std::vector<std::string> standardVst2Dirs()
{
    return {
        "C:\\Program Files\\VstPlugins",
        "C:\\Program Files\\Steinberg\\VstPlugins",
        "C:\\Program Files\\Common Files\\VST2",
    };
}
std::vector<std::string> standardVst3Dirs()
{
    return { "C:\\Program Files\\Common Files\\VST3" };
}
// One "size|mtime|path" line per candidate.  A .vst3 bundle DIRECTORY gets
// size 0 and the directory's own mtime -- coarse, but presence/absence is the
// failure mode that actually strands the inventory (see plugin_host.h).
void snapshotLine(const std::string& path, std::vector<std::string>& lines)
{
    WIN32_FILE_ATTRIBUTE_DATA ad = {};
    unsigned long long size = 0; long long mt = 0;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &ad))
    {
        if (!(ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            size = ((unsigned long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
        mt = ((long long)ad.ftLastWriteTime.dwHighDateTime << 32)
           | (long long)ad.ftLastWriteTime.dwLowDateTime;
    }
    lines.push_back(std::to_string(size) + "|" + std::to_string(mt) + "|" + path);
}
} // namespace

std::string PluginHost::scanFileSnapshot() const
{
    std::vector<std::string> files;
    for (const auto& dir : standardVst2Dirs())
        if (dirExists(dir)) collect(dir, ".dll", /*dirsAlsoMatch=*/false, /*depth=*/3, files);
    for (const auto& dir : standardVst3Dirs())
        if (dirExists(dir)) collect(dir, ".vst3", /*dirsAlsoMatch=*/true, /*depth=*/4, files);

    std::vector<std::string> lines;
    lines.reserve(files.size());
    for (const auto& f : files) snapshotLine(f, lines);
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    std::string out;
    for (const auto& l : lines) { out += l; out += '\n'; }
    return out;
}

bool PluginHost::probeVst2(const std::string& dllPath,
                           std::vector<PluginDescriptor>& out) const
{
    // Cheap arch gate first: skip 32-bit DLLs without spawning anything.
    if (!isPe64(dllPath))
        return false;

    std::string probe = findProbeExe("probe_vst2.exe");
    std::string cmd = quote(probe) + " " + quote(dllPath);

    std::string txt;
    if (!runCapture(cmd, probeTimeoutMs_, txt))
        return false; // timed out / could not run

    // Handles BOTH the flat single-effect form and the SHELL_BEGIN/SHELL_END
    // form a shell plugin produces (one descriptor per sub-plugin).
    return parseVst2ProbeOutput(txt, dllPath, fileNameStem(dllPath), out);
}

bool PluginHost::probeVst3(const std::string& vst3Path,
                           std::vector<PluginDescriptor>& out) const
{
    std::string probe = findProbeExe("probe_vst3_next.exe");
    if (probe.empty()) probe = findProbeExe("probe_vst3.exe");
    std::string cmd = quote(probe) + " " + quote(vst3Path);

    std::string txt;
    if (!runCapture(cmd, probeTimeoutMs_, txt))
        return false;

    // The VST3 probe emits one CLASS_BEGIN..CLASS_END block per audio class.
    std::istringstream ss(txt);
    std::string line;
    std::string factoryVendor;
    bool any = false;

    std::map<std::string, std::string> cur;
    bool inClass = false;
    while (std::getline(ss, line))
    {
        line = trim(line);
        if (line.rfind("FACTORY_VENDOR=", 0) == 0)
        {
            factoryVendor = line.substr(15);
            continue;
        }
        if (line == "CLASS_BEGIN") { inClass = true; cur.clear(); continue; }
        if (line == "CLASS_END")
        {
            if (inClass)
            {
                PluginDescriptor d;
                d.format       = PluginFormat::VST3;
                d.name         = !cur["NAME"].empty() ? cur["NAME"]
                                                      : fileNameStem(vst3Path);
                d.vendor       = !cur["VENDOR"].empty() ? cur["VENDOR"] : factoryVendor;
                d.path         = vst3Path;
                d.uid          = cur["UID"];
                d.isInstrument = cur["ISSYNTH"] == "1";
                d.numAudioIn   = atoi(cur["AUDIOIN"].c_str());
                d.numAudioOut  = atoi(cur["AUDIOOUT"].c_str());
                // See parseVst2ProbeOutput: BUSIN/BUSOUT carry the grouping,
                // and an absent/bad field degrades to one main bus.
                pluginDecodeBusLayout(cur["BUSIN"],  d.audioInBuses);
                pluginDecodeBusLayout(cur["BUSOUT"], d.audioOutBuses);
                pluginFillDefaultBusLayout(d);
                out.push_back(d);
                any = true;
            }
            inClass = false;
            continue;
        }
        if (inClass)
        {
            size_t eq = line.find('=');
            if (eq != std::string::npos)
                cur[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return any;
}

std::vector<PluginDescriptor> PluginHost::scan(const std::vector<std::string>& extraPaths)
{
    std::vector<PluginDescriptor> result = nativeEffectDescriptors();

    // Shared with scanFileSnapshot() -- see standardVst2Dirs()'s comment.
    const std::vector<std::string> vst2Dirs = standardVst2Dirs();
    const std::vector<std::string> vst3Dirs = standardVst3Dirs();

    // ---- VST2: gather .dll files (recurse into vendor subfolders) ----------
    std::vector<std::string> dlls;
    for (const auto& dir : vst2Dirs)
        if (dirExists(dir))
            collect(dir, ".dll", /*dirsAlsoMatch=*/false, /*depth=*/3, dlls);

    // extra paths searched for both formats
    for (const auto& dir : extraPaths)
        if (dirExists(dir))
            collect(dir, ".dll", false, 3, dlls);

    // de-dup by lowercased path
    {
        std::vector<std::string> uniq;
        std::vector<std::string> seen;
        for (auto& p : dlls)
        {
            std::string lo = p; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (std::find(seen.begin(), seen.end(), lo) == seen.end())
            { seen.push_back(lo); uniq.push_back(p); }
        }
        dlls.swap(uniq);
    }

    // Probe in parallel: each probe is an independent out-of-process child, so
    // this is I/O-bound -- run a small pool so a few hanging plugins (each up to
    // probeTimeoutMs_) don't serialise the whole scan into minutes.
    std::mutex resultMtx;
    auto probeParallel = [&](const std::vector<std::string>& items, bool vst3) {
        if (items.empty()) return;
        unsigned nt = std::thread::hardware_concurrency();
        if (nt < 4) nt = 4; if (nt > 12) nt = 12;
        if (nt > items.size()) nt = (unsigned)items.size();
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nt; ++t)
            pool.emplace_back([&, vst3] {
                for (;;) {
                    // A cancelled scan stops taking new work; the probe already
                    // in flight still has to run out its own timeout, which is
                    // what bounds the shutdown wait.
                    if (scanCancel_.load(std::memory_order_acquire)) break;
                    size_t i = next.fetch_add(1);
                    if (i >= items.size()) break;
                    std::vector<PluginDescriptor> local;
                    if (vst3) probeVst3(items[i], local); else probeVst2(items[i], local);
                    if (!local.empty()) {
                        std::lock_guard<std::mutex> lk(resultMtx);
                        for (auto& d : local) result.push_back(std::move(d));
                    }
                }
            });
        for (auto& th : pool) th.join();
    };

    probeParallel(dlls, /*vst3=*/false);
    if (scanCancel_.load(std::memory_order_acquire)) return result;

    // ---- VST3: gather .vst3 (single files AND bundle directories) ----------
    std::vector<std::string> vst3s;
    for (const auto& dir : vst3Dirs)
        if (dirExists(dir))
            collect(dir, ".vst3", /*dirsAlsoMatch=*/true, /*depth=*/4, vst3s);
    for (const auto& dir : extraPaths)
        if (dirExists(dir))
            collect(dir, ".vst3", true, 4, vst3s);

    {
        std::vector<std::string> uniq;
        std::vector<std::string> seen;
        for (auto& p : vst3s)
        {
            std::string lo = p; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (std::find(seen.begin(), seen.end(), lo) == seen.end())
            { seen.push_back(lo); uniq.push_back(p); }
        }
        vst3s.swap(uniq);
    }

    probeParallel(vst3s, /*vst3=*/true);

    // A cancelled scan is PARTIAL.  Writing it would leave a cache that looks
    // complete and stops the app ever rescanning.
    if (!cachePath_.empty() && !scanCancel_.load(std::memory_order_acquire))
        saveCache(result);

    return result;
}

std::vector<PluginDescriptor> PluginHost::probeFile(const std::string& path) const
{
    std::vector<PluginDescriptor> out;
    std::string lower=path;
    std::transform(lower.begin(),lower.end(),lower.begin(),::tolower);
    if(lower.size()>=5&&lower.compare(lower.size()-5,5,".vst3")==0)
        probeVst3(path,out);
    else if(lower.size()>=4&&lower.compare(lower.size()-4,4,".dll")==0)
        probeVst2(path,out);
    return out;
}

IPluginInstance* PluginHost::instantiate(const PluginDescriptor& desc)
{
    if (IPluginInstance* native = createNativeEffect(desc)) return native;
    if (desc.name == "Sampler" && desc.path.empty())
        return create_sampler_instrument();
    switch (desc.format)
    {
    case PluginFormat::VST2: return createVst2Instance(desc);
    case PluginFormat::VST3: return createVst3Instance(desc);
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// WEAK stub factories for the standalone test. The real createVst2Instance /
// createVst3Instance live in src/engine/vst2 and src/engine/vst3 and override
// these at integration link time. mingw supports the weak attribute.
// ----------------------------------------------------------------------------
#ifdef PATCHKNOB_HOST_WEAK_STUBS
extern "C" { /* nothing */ }
__attribute__((weak)) IPluginInstance* createVst2Instance(const PluginDescriptor&)
{
    return nullptr;
}
__attribute__((weak)) IPluginInstance* createVst3Instance(const PluginDescriptor&)
{
    return nullptr;
}
__attribute__((weak)) IPluginInstance* create_sampler_instrument()
{
    return nullptr;
}
#endif

}} // namespace PatchKnob::engine

#else
// ----------------------------------------------------------------------------
// Linux/macOS: real VST2/VST3 scanning + out-of-process probing, mirroring
// the WIN32 implementation above feature-for-feature (same probe-exe /
// timeout / JSON-cache design) with POSIX primitives standing in for the
// Win32 ones -- std::filesystem for directory enumeration, /proc/self/exe
// (readlink) for locating the running binary, fork/exec/pipe for running a
// probe helper with a timeout instead of CreateProcess + PeekNamedPipe.
// ----------------------------------------------------------------------------
#include <filesystem>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <chrono>

extern char** environ;

namespace PatchKnob { namespace engine {

extern IPluginInstance* createVst2Instance(const PluginDescriptor& desc);
extern IPluginInstance* createVst3Instance(const PluginDescriptor& desc);

namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool endsWithNoCase(const std::string& s, const std::string& suffix)
{
    if (s.size() < suffix.size()) return false;
    return strcasecmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

// Recursively collect entries under `root` matching a file extension (.so)
// or a directory/file name ending (.vst3 -- a bundle may be a single file OR
// a directory; the SDK loader resolves it either way, so a matching
// directory is reported as-is and NOT descended into).
void collect(const std::string& root, const std::string& ext,
             bool dirsAlsoMatch, int depth, std::vector<std::string>& out)
{
    if (depth < 0) return;
    std::error_code ec;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        std::error_code e2;
        if (fs::is_directory(p, e2)) {
            if (dirsAlsoMatch && endsWithNoCase(name, ext))
                out.push_back(p.string());
            else
                collect(p.string(), ext, dirsAlsoMatch, depth - 1, out);
        } else if (endsWithNoCase(name, ext)) {
            out.push_back(p.string());
        }
    }
}

bool dirExists(const std::string& path)
{
    std::error_code ec;
    return fs::is_directory(path, ec);
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string fileNameStem(const std::string& path)
{
    fs::path p(path);
    return p.stem().string();
}

// The absolute path to the running executable's directory, via /proc (Linux)
// -- there is no argv[0]-independent equivalent otherwise.
std::string exeDir()
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    fs::path p(buf);
    return p.has_parent_path() ? p.parent_path().string() : "";
}

// Run a child process, capture its stdout, enforce a timeout. Returns true if
// the process ran to completion within the timeout (regardless of exit
// code); `output` gets its stdout. On timeout the child is killed and false
// is returned. Uses posix_spawn (no fork() -- safe next to a realtime audio
// thread and threads already running in this process) + a pipe, polling
// with a deadline instead of Win32's PeekNamedPipe loop.
bool runCapture(const std::string& exePath, const std::string& arg,
                unsigned timeoutMs, std::string& output)
{
    output.clear();

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    char argv0[4096]; std::snprintf(argv0, sizeof(argv0), "%s", exePath.c_str());
    char argv1[4096]; std::snprintf(argv1, sizeof(argv1), "%s", arg.c_str());
    char* argv[] = { argv0, argv1, nullptr };

    pid_t pid = 0;
    int rc = posix_spawn(&pid, exePath.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) { close(pipefd[0]); return false; }

    // Non-blocking reads polled against a deadline -- functionally the same
    // shape as the Win32 PeekNamedPipe loop, just POSIX poll() instead.
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;
    char buf[4096];
    for (;;) {
        struct pollfd pfd{ pipefd[0], POLLIN, 0 };
        const auto remaining = deadline - std::chrono::steady_clock::now();
        const int remainMs = (int)std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
        int pr = poll(&pfd, 1, std::min(remainMs, 50));
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            ssize_t got;
            while ((got = read(pipefd[0], buf, sizeof(buf))) > 0)
                output.append(buf, (size_t)got);
            if (got == 0) break;   // EOF: child closed its end
        }
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            // Drain whatever is left, non-blocking.
            ssize_t got;
            while ((got = read(pipefd[0], buf, sizeof(buf))) > 0)
                output.append(buf, (size_t)got);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
    }
    close(pipefd[0]);
    return !timedOut;
}

} // namespace

PluginHost::PluginHost() = default;
PluginHost::~PluginHost() = default;

std::string PluginHost::findProbeExe(const char* exeName) const
{
    std::string c1 = exeDir() + "/" + exeName;
    if (fs::exists(c1)) return c1;
    return exeName;
}

namespace {
// The standard directories, in ONE place so scan() and scanFileSnapshot() can
// never drift apart (a snapshot over different directories than the scan walks
// would flag every boot as stale, or never flag at all).
std::vector<std::string> standardVst2Dirs()
{
    std::vector<std::string> dirs = { "/usr/lib/vst", "/usr/local/lib/vst" };
    if (const char* home = std::getenv("HOME"))
        dirs.push_back(std::string(home) + "/.vst");
    return dirs;
}
std::vector<std::string> standardVst3Dirs()
{
    std::vector<std::string> dirs = { "/usr/lib/vst3", "/usr/local/lib/vst3" };
    if (const char* home = std::getenv("HOME"))
        dirs.push_back(std::string(home) + "/.vst3");
    return dirs;
}
// One "size|mtime|path" line per candidate.  A .vst3 bundle DIRECTORY gets
// size 0 and the directory's own mtime -- coarse, but presence/absence is the
// failure mode that actually strands the inventory (see plugin_host.h).
void snapshotLine(const std::string& path, std::vector<std::string>& lines)
{
    std::error_code ec;
    uintmax_t size = 0;
    if (fs::is_regular_file(path, ec)) size = fs::file_size(path, ec);
    auto t = fs::last_write_time(path, ec);
    const long long mt = ec ? 0
        : (long long)std::chrono::duration_cast<std::chrono::seconds>(
              t.time_since_epoch()).count();
    lines.push_back(std::to_string((unsigned long long)size) + "|" +
                    std::to_string(mt) + "|" + path);
}
} // namespace

std::string PluginHost::scanFileSnapshot() const
{
    std::vector<std::string> files;
    for (const auto& dir : standardVst2Dirs())
        if (dirExists(dir)) collect(dir, ".so", /*dirsAlsoMatch=*/false, /*depth=*/3, files);
    for (const auto& dir : standardVst3Dirs())
        if (dirExists(dir)) collect(dir, ".vst3", /*dirsAlsoMatch=*/true, /*depth=*/4, files);

    std::vector<std::string> lines;
    lines.reserve(files.size());
    for (const auto& f : files) snapshotLine(f, lines);
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    std::string out;
    for (const auto& l : lines) { out += l; out += '\n'; }
    return out;
}

bool PluginHost::probeVst2(const std::string& soPath,
                           std::vector<PluginDescriptor>& out) const
{
    std::string probe = findProbeExe("probe_vst2");
    std::string txt;
    if (!runCapture(probe, soPath, probeTimeoutMs_, txt))
        return false;

    // Handles BOTH the flat single-effect form and the SHELL_BEGIN/SHELL_END
    // form a shell plugin produces (one descriptor per sub-plugin).
    return parseVst2ProbeOutput(txt, soPath, fileNameStem(soPath), out);
}

bool PluginHost::probeVst3(const std::string& vst3Path,
                           std::vector<PluginDescriptor>& out) const
{
    std::string probe = findProbeExe("probe_vst3");
    std::string txt;
    if (!runCapture(probe, vst3Path, probeTimeoutMs_, txt))
        return false;

    // The VST3 probe emits one CLASS_BEGIN..CLASS_END block per audio class.
    std::istringstream ss(txt);
    std::string line;
    std::string factoryVendor;
    bool any = false;

    std::map<std::string, std::string> cur;
    bool inClass = false;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.rfind("FACTORY_VENDOR=", 0) == 0) { factoryVendor = line.substr(15); continue; }
        if (line == "CLASS_BEGIN") { inClass = true; cur.clear(); continue; }
        if (line == "CLASS_END") {
            if (inClass) {
                PluginDescriptor d;
                d.format       = PluginFormat::VST3;
                d.name         = !cur["NAME"].empty() ? cur["NAME"] : fileNameStem(vst3Path);
                d.vendor       = !cur["VENDOR"].empty() ? cur["VENDOR"] : factoryVendor;
                d.path         = vst3Path;
                d.uid          = cur["UID"];
                d.isInstrument = cur["ISSYNTH"] == "1";
                d.numAudioIn   = atoi(cur["AUDIOIN"].c_str());
                d.numAudioOut  = atoi(cur["AUDIOOUT"].c_str());
                // See parseVst2ProbeOutput: BUSIN/BUSOUT carry the grouping,
                // and an absent/bad field degrades to one main bus.
                pluginDecodeBusLayout(cur["BUSIN"],  d.audioInBuses);
                pluginDecodeBusLayout(cur["BUSOUT"], d.audioOutBuses);
                pluginFillDefaultBusLayout(d);
                out.push_back(d);
                any = true;
            }
            inClass = false;
            continue;
        }
        if (inClass) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) cur[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return any;
}

std::vector<PluginDescriptor> PluginHost::scan(const std::vector<std::string>& extraPaths)
{
    std::vector<PluginDescriptor> result = nativeEffectDescriptors();

    // Standard Linux VST install locations (matching Steinberg's own hosts'
    // conventions); macOS would need its own set (~/Library/Audio/Plug-Ins)
    // if/when that platform gets a build.  Shared with scanFileSnapshot().
    const std::vector<std::string> vst2Dirs = standardVst2Dirs();
    const std::vector<std::string> vst3Dirs = standardVst3Dirs();

    std::vector<std::string> so;
    for (const auto& dir : vst2Dirs)
        if (dirExists(dir)) collect(dir, ".so", /*dirsAlsoMatch=*/false, /*depth=*/3, so);
    for (const auto& dir : extraPaths)
        if (dirExists(dir)) collect(dir, ".so", false, 3, so);
    {
        std::vector<std::string> uniq, seen;
        for (auto& p : so) {
            std::string lo = p; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (std::find(seen.begin(), seen.end(), lo) == seen.end()) { seen.push_back(lo); uniq.push_back(p); }
        }
        so.swap(uniq);
    }

    std::mutex resultMtx;
    auto probeParallel = [&](const std::vector<std::string>& items, bool vst3) {
        if (items.empty()) return;
        unsigned nt = std::thread::hardware_concurrency();
        if (nt < 4) nt = 4; if (nt > 12) nt = 12;
        if (nt > items.size()) nt = (unsigned)items.size();
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nt; ++t)
            pool.emplace_back([&, vst3] {
                for (;;) {
                    // A cancelled scan stops taking new work; the probe already
                    // in flight still has to run out its own timeout, which is
                    // what bounds the shutdown wait.
                    if (scanCancel_.load(std::memory_order_acquire)) break;
                    size_t i = next.fetch_add(1);
                    if (i >= items.size()) break;
                    std::vector<PluginDescriptor> local;
                    if (vst3) probeVst3(items[i], local); else probeVst2(items[i], local);
                    if (!local.empty()) {
                        std::lock_guard<std::mutex> lk(resultMtx);
                        for (auto& d : local) result.push_back(std::move(d));
                    }
                }
            });
        for (auto& th : pool) th.join();
    };

    probeParallel(so, /*vst3=*/false);
    if (scanCancel_.load(std::memory_order_acquire)) return result;

    std::vector<std::string> vst3s;
    for (const auto& dir : vst3Dirs)
        if (dirExists(dir)) collect(dir, ".vst3", /*dirsAlsoMatch=*/true, /*depth=*/4, vst3s);
    for (const auto& dir : extraPaths)
        if (dirExists(dir)) collect(dir, ".vst3", true, 4, vst3s);
    {
        std::vector<std::string> uniq, seen;
        for (auto& p : vst3s) {
            std::string lo = p; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            if (std::find(seen.begin(), seen.end(), lo) == seen.end()) { seen.push_back(lo); uniq.push_back(p); }
        }
        vst3s.swap(uniq);
    }

    probeParallel(vst3s, /*vst3=*/true);

    // A cancelled scan is PARTIAL -- see the note in the Windows scan().
    if (!cachePath_.empty() && !scanCancel_.load(std::memory_order_acquire))
        saveCache(result);
    return result;
}

std::vector<PluginDescriptor> PluginHost::probeFile(const std::string& path) const
{
    std::vector<PluginDescriptor> out;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".vst3") == 0)
        probeVst3(path, out);
    else if (lower.size() >= 3 && lower.compare(lower.size() - 3, 3, ".so") == 0)
        probeVst2(path, out);
    return out;
}

IPluginInstance* PluginHost::instantiate(const PluginDescriptor& desc)
{
    if (IPluginInstance* native = createNativeEffect(desc)) return native;
    if (desc.name == "Sampler" && desc.path.empty())
        return create_sampler_instrument();
    switch (desc.format) {
    case PluginFormat::VST2: return createVst2Instance(desc);
    case PluginFormat::VST3: return createVst3Instance(desc);
    }
    return nullptr;
}

#ifdef PATCHKNOB_HOST_WEAK_STUBS
// WEAK stub factories for the standalone test (scan_test). The real
// createVst2Instance/createVst3Instance/create_sampler_instrument live in
// src/engine/vst2, src/engine/vst3, and src/engine/sampler and override
// these at integration link time. GCC/Clang support the weak attribute.
__attribute__((weak)) IPluginInstance* createVst2Instance(const PluginDescriptor&)
{
    return nullptr;
}
__attribute__((weak)) IPluginInstance* createVst3Instance(const PluginDescriptor&)
{
    return nullptr;
}
__attribute__((weak)) IPluginInstance* create_sampler_instrument()
{
    return nullptr;
}
#endif

}} // namespace PatchKnob::engine

#endif

// ============================================================================
//  Plugin cache -- ONE implementation for every platform.
//
//  This used to be duplicated inside both the _WIN32 and the POSIX halves of
//  this file, and both copies were wrong in the same three ways:
//
//    * saveCache() escapes with jsonEsc() but the reader never UNescaped, so a
//      Windows path came back with its backslashes doubled and every "\"" in a
//      value terminated the string early.
//    * objects were sliced with a plain find('}'), so a plug-in named
//      "Reverb {Pro}" ended the object at the brace INSIDE the name -- path,
//      vendor and uid then parsed as empty and the entry was unusable.
//    * loadCache() returned true whenever the file merely OPENED, and always
//      appended the built-in effects, so a truncated or corrupt cache was
//      indistinguishable from a good one: the caller's `!loadCache() ||
//      res.empty()` rescan trigger could never fire and the app was stuck with
//      a broken inventory forever.
//
//  The reader below is a real (if small) JSON scanner: it tracks string state
//  so braces and commas inside values are inert, it undoes exactly the escapes
//  jsonEsc() writes, and ANY malformed input makes loadCache() return false so
//  the caller re-scans.
// ============================================================================
namespace PatchKnob { namespace engine {
namespace {

// JSON string escaping for the cache file. The reader below (cacheReadString)
// is its exact inverse -- change one and you must change the other.
std::string jsonEsc(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        case '\b': o += "\\b";  break;
        case '\f': o += "\\f";  break;
        default:
            // Other C0 control bytes would make the file unparseable; emit the
            // \uXXXX form the reader understands. UTF-8 continuation bytes
            // (>= 0x80) pass through untouched.
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static const char* kHex = "0123456789abcdef";
                o += "\\u00";
                o += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                o += kHex[static_cast<unsigned char>(c) & 0xF];
            }
            else o += c;
            break;
        }
    }
    return o;
}

// Skip whitespace starting at `i`.
inline void cacheSkipWs(const std::string& t, size_t& i)
{
    while (i < t.size() && (t[i]==' '||t[i]=='\t'||t[i]=='\r'||t[i]=='\n')) ++i;
}

// Read a JSON string starting at the opening quote. Undoes the escapes
// jsonEsc() emits (\" \\ \n \r \t) plus \/ \b \f \uXXXX (as UTF-8).
// Returns false on an unterminated string or a bad escape.
bool cacheReadString(const std::string& t, size_t& i, std::string& out)
{
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    out.clear();
    while (i < t.size())
    {
        char c = t[i++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (i >= t.size()) return false;
        char e = t[i++];
        switch (e)
        {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'u': {
            if (i + 4 > t.size()) return false;
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
                char h = t[i + k];
                cp <<= 4;
                if      (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                else return false;
            }
            i += 4;
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: return false;
        }
    }
    return false;   // ran off the end inside a string
}

// Read a bare (non-string) value: true / false / null / number. Stops at the
// first ',' or '}' -- safe here because a bare value can contain neither.
bool cacheReadBare(const std::string& t, size_t& i, std::string& out)
{
    size_t start = i;
    while (i < t.size() && t[i] != ',' && t[i] != '}') ++i;
    if (i >= t.size()) return false;
    out.assign(t, start, i - start);
    // trim
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { out.clear(); return false; }
    size_t b = out.find_last_not_of(" \t\r\n");
    out = out.substr(a, b - a + 1);
    return !out.empty();
}

} // namespace

// Cache schema version, stamped per entry.
//   1 (implicit -- no "schema" key): pre-bus-layout. numAudioIn/numAudioOut are
//     all there is, so the loader SYNTHESISES one main bus of that width. An
//     existing PatchKnob_plugins.cache written by the old code therefore still
//     loads, and behaves exactly as it did.
//   2: carries "audioInBuses"/"audioOutBuses". An EMPTY layout at schema 2 is
//     meaningful ("this plugin really has no buses in that direction") and is
//     left alone, which is precisely what version 1 could not express.
static constexpr int kCacheSchemaVersion = 2;

bool PluginHost::saveCache(const std::vector<PluginDescriptor>& plugins) const
{
    if (cachePath_.empty()) return false;
    std::ofstream f(cachePath_, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    f << "[\n";
    // Meta entry FIRST: the fingerprint of the plugin files this scan saw.
    // loadCache() recomputes it and refuses the cache when the disk changed
    // (plugins installed/removed/updated since), so the inventory can never
    // again be pinned to a stale scan.  The synthetic "cachemeta://scan" path
    // keeps the entry inert for any reader that does not know the key
    // (unknown keys are skipped; the loader below drops the entry by path).
    f << "  {\"schema\":" << kCacheSchemaVersion << ","
      << "\"path\":\"cachemeta://scan\","
      << "\"scannedFiles\":\"" << jsonEsc(scanFileSnapshot()) << "\"}"
      << (plugins.empty() ? "" : ",") << "\n";
    for (size_t i = 0; i < plugins.size(); ++i)
    {
        const auto& p = plugins[i];
        f << "  {"
          << "\"format\":\""       << (p.format == PluginFormat::VST2 ? "VST2" : "VST3") << "\","
          << "\"name\":\""         << jsonEsc(p.name)   << "\","
          << "\"vendor\":\""       << jsonEsc(p.vendor) << "\","
          << "\"path\":\""         << jsonEsc(p.path)   << "\","
          << "\"uid\":\""          << jsonEsc(p.uid)    << "\","
          << "\"isInstrument\":"   << (p.isInstrument ? "true" : "false") << ","
          << "\"numAudioIn\":"     << p.numAudioIn  << ","
          << "\"numAudioOut\":"    << p.numAudioOut << ","
          // Bus layout (plugin_api.h wire format) + the schema marker that
          // makes the compat path explicit. See loadCache().
          << "\"schema\":"         << kCacheSchemaVersion << ","
          << "\"audioInBuses\":\""  << jsonEsc(pluginEncodeBusLayout(p.audioInBuses))  << "\","
          << "\"audioOutBuses\":\"" << jsonEsc(pluginEncodeBusLayout(p.audioOutBuses)) << "\""
          << "}" << (i + 1 < plugins.size() ? "," : "") << "\n";
    }
    f << "]\n";
    f.flush();
    return static_cast<bool>(f);
}

bool PluginHost::loadCache(std::vector<PluginDescriptor>& out) const
{
    if (cachePath_.empty()) return false;
    std::ifstream f(cachePath_, std::ios::binary);
    if (!f) return false;
    std::stringstream buf; buf << f.rdbuf();
    const std::string t = buf.str();

    std::vector<PluginDescriptor> parsed;
    size_t i = 0;
    // The disk fingerprint the cache was written against (see saveCache /
    // scanFileSnapshot).  A cache from before this existed reads back as ""
    // and only matches a machine that genuinely has no plugin files -- any
    // other legacy cache is treated as stale ONCE, re-scanned, and re-stamped.
    std::string storedSnapshot;

    cacheSkipWs(t, i);
    if (i >= t.size() || t[i] != '[') return false;     // not our file at all
    ++i;
    cacheSkipWs(t, i);

    if (i < t.size() && t[i] == ']')
    {
        // A legitimately empty inventory. Fall through to the built-in merge.
    }
    else
    {
        for (;;)
        {
            cacheSkipWs(t, i);
            if (i >= t.size() || t[i] != '{') return false;
            ++i;

            PluginDescriptor d;
            bool sawAnyField = false;
            int  schema = 1;               // no "schema" key == pre-bus-layout
            for (;;)
            {
                cacheSkipWs(t, i);
                if (i < t.size() && t[i] == '}') { ++i; break; }

                std::string key;
                if (!cacheReadString(t, i, key)) return false;
                cacheSkipWs(t, i);
                if (i >= t.size() || t[i] != ':') return false;
                ++i;
                cacheSkipWs(t, i);

                std::string val;
                if (i < t.size() && t[i] == '"')
                {
                    if (!cacheReadString(t, i, val)) return false;
                }
                else if (!cacheReadBare(t, i, val))
                {
                    return false;
                }
                sawAnyField = true;

                if      (key == "format")       d.format = (val == "VST3") ? PluginFormat::VST3
                                                                           : PluginFormat::VST2;
                else if (key == "name")         d.name   = val;
                else if (key == "vendor")       d.vendor = val;
                else if (key == "path")         d.path   = val;
                else if (key == "uid")          d.uid    = val;
                else if (key == "isInstrument") d.isInstrument = (val == "true");
                else if (key == "numAudioIn")   d.numAudioIn  = std::atoi(val.c_str());
                else if (key == "numAudioOut")  d.numAudioOut = std::atoi(val.c_str());
                else if (key == "schema")       schema = std::atoi(val.c_str());
                else if (key == "audioInBuses") {
                    if (!pluginDecodeBusLayout(val, d.audioInBuses)) return false;
                }
                else if (key == "audioOutBuses") {
                    if (!pluginDecodeBusLayout(val, d.audioOutBuses)) return false;
                }
                else if (key == "scannedFiles") storedSnapshot = val;
                // unknown keys are ignored on purpose (forward compatibility)

                cacheSkipWs(t, i);
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; break; }
                return false;
            }
            if (!sawAnyField) return false;
            // COMPAT: a version-1 entry has no layout to read, so give it the
            // single-main-bus reading of its flat totals rather than rejecting
            // the file and forcing a full rescan.
            if (schema < 2)
                pluginFillDefaultBusLayout(d);
            parsed.push_back(std::move(d));

            cacheSkipWs(t, i);
            if (i < t.size() && t[i] == ',') { ++i; continue; }
            if (i < t.size() && t[i] == ']') { ++i; break; }
            return false;                                // trailing garbage
        }
    }

    // The meta entry is bookkeeping, not a plug-in: drop it before the
    // integrity checks below (its snapshot was already captured above).
    parsed.erase(std::remove_if(parsed.begin(), parsed.end(),
                                [](const PluginDescriptor& d) {
                                    return d.path.rfind("cachemeta://", 0) == 0;
                                }),
                 parsed.end());

    // A cache entry with no path is a parse artefact, not a plug-in: refuse the
    // whole file rather than hand the UI an inventory that cannot instantiate.
    for (const auto& d : parsed)
        if (d.path.empty()) return false;

    // STALENESS: the cache only speaks for the disk it was scanned from.  If
    // the standard directories hold a different set of plugin files now
    // (installed / removed / updated since -- or the cache predates the
    // fingerprint and files exist), the inventory must be rebuilt by probing.
    if (storedSnapshot != scanFileSnapshot())
        return false;

    out = std::move(parsed);

    // Built-ins are code, not files: an older cache must never hide newly
    // shipped native effects. Merge them by stable builtin:// path.
    for (const auto& d : nativeEffectDescriptors())
    {
        bool found = false;
        for (const auto& e : out) if (e.path == d.path) { found = true; break; }
        if (!found) out.push_back(d);
    }
    return true;
}

}} // namespace PatchKnob::engine
