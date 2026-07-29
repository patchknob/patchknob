//----------------------------------------------------------------------------
//  PatchKnob — unified plugin host + scanner implementation.
//  See plugin_host.h for the design rationale (out-of-process probing).
//----------------------------------------------------------------------------
#include "plugin_host.h"
#include "../buzz/sampler_instrument.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

// Parse KEY=VALUE lines into a map (last value wins).
std::map<std::string, std::string> parseKv(const std::string& text)
{
    std::map<std::string, std::string> kv;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
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

// JSON string escaping for the optional cache file.
std::string jsonEsc(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:   o += c;       break;
        }
    }
    return o;
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

    auto kv = parseKv(txt);
    if (kv["OK"] != "1")
        return false;

    PluginDescriptor d;
    d.format       = PluginFormat::VST2;
    d.name         = kv.count("NAME") && !kv["NAME"].empty() ? kv["NAME"]
                                                             : fileNameStem(dllPath);
    d.vendor       = kv["VENDOR"];
    d.path         = dllPath;
    d.uid          = kv["UID"];
    d.isInstrument = kv["ISSYNTH"] == "1";
    d.numAudioIn   = kv.count("AUDIOIN")  ? atoi(kv["AUDIOIN"].c_str())  : 0;
    d.numAudioOut  = kv.count("AUDIOOUT") ? atoi(kv["AUDIOOUT"].c_str()) : 0;
    out.push_back(d);
    return true;
}

bool PluginHost::probeVst3(const std::string& vst3Path,
                           std::vector<PluginDescriptor>& out) const
{
    std::string probe = findProbeExe("probe_vst3.exe");
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
    std::vector<PluginDescriptor> result;

    const std::vector<std::string> vst2Dirs = {
        "C:\\Program Files\\VstPlugins",
        "C:\\Program Files\\Steinberg\\VstPlugins",
        "C:\\Program Files\\Common Files\\VST2",
    };
    const std::string vst3Dir = "C:\\Program Files\\Common Files\\VST3";

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

    // ---- VST3: gather .vst3 (single files AND bundle directories) ----------
    std::vector<std::string> vst3s;
    if (dirExists(vst3Dir))
        collect(vst3Dir, ".vst3", /*dirsAlsoMatch=*/true, /*depth=*/4, vst3s);
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

    if (!cachePath_.empty())
        saveCache(result);

    return result;
}

IPluginInstance* PluginHost::instantiate(const PluginDescriptor& desc)
{
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
// Optional cache (nice-to-have). A flat JSON array of descriptors.
// ----------------------------------------------------------------------------
bool PluginHost::saveCache(const std::vector<PluginDescriptor>& plugins) const
{
    if (cachePath_.empty()) return false;
    std::ofstream f(cachePath_, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    f << "[\n";
    for (size_t i = 0; i < plugins.size(); ++i)
    {
        const auto& p = plugins[i];
        f << "  {"
          << "\"format\":\""    << (p.format == PluginFormat::VST2 ? "VST2" : "VST3") << "\","
          << "\"name\":\""      << jsonEsc(p.name)   << "\","
          << "\"vendor\":\""    << jsonEsc(p.vendor) << "\","
          << "\"path\":\""      << jsonEsc(p.path)   << "\","
          << "\"uid\":\""       << jsonEsc(p.uid)    << "\","
          << "\"isInstrument\":" << (p.isInstrument ? "true" : "false") << ","
          << "\"numAudioIn\":"  << p.numAudioIn  << ","
          << "\"numAudioOut\":" << p.numAudioOut
          << "}" << (i + 1 < plugins.size() ? "," : "") << "\n";
    }
    f << "]\n";
    return true;
}

// Minimal forgiving reader for the format saveCache writes.
bool PluginHost::loadCache(std::vector<PluginDescriptor>& out) const
{
    if (cachePath_.empty()) return false;
    std::ifstream f(cachePath_, std::ios::binary);
    if (!f) return false;
    std::stringstream buf; buf << f.rdbuf();
    std::string text = buf.str();

    out.clear();
    size_t pos = 0;
    auto field = [&](const std::string& src, size_t from, const char* key) -> std::string {
        std::string k = std::string("\"") + key + "\":";
        size_t p = src.find(k, from);
        if (p == std::string::npos) return "";
        p += k.size();
        if (p < src.size() && src[p] == '"')
        {
            size_t end = src.find('"', p + 1);
            return src.substr(p + 1, end - p - 1);
        }
        size_t end = src.find_first_of(",}", p);
        return trim(src.substr(p, end - p));
    };

    for (;;)
    {
        size_t obj = text.find('{', pos);
        if (obj == std::string::npos) break;
        size_t objEnd = text.find('}', obj);
        if (objEnd == std::string::npos) break;
        std::string o = text.substr(obj, objEnd - obj + 1);

        PluginDescriptor d;
        d.format       = field(o, 0, "format") == "VST3" ? PluginFormat::VST3 : PluginFormat::VST2;
        d.name         = field(o, 0, "name");
        d.vendor       = field(o, 0, "vendor");
        d.path         = field(o, 0, "path");
        d.uid          = field(o, 0, "uid");
        d.isInstrument = field(o, 0, "isInstrument") == "true";
        d.numAudioIn   = atoi(field(o, 0, "numAudioIn").c_str());
        d.numAudioOut  = atoi(field(o, 0, "numAudioOut").c_str());
        out.push_back(d);
        pos = objEnd + 1;
    }
    return true;
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
#endif

}} // namespace PatchKnob::engine

#else

namespace PatchKnob { namespace engine {

PluginHost::PluginHost() = default;
PluginHost::~PluginHost() = default;

std::string PluginHost::findProbeExe(const char*) const { return {}; }

bool PluginHost::probeVst2(const std::string&, std::vector<PluginDescriptor>&) const { return false; }
bool PluginHost::probeVst3(const std::string&, std::vector<PluginDescriptor>&) const { return false; }

std::vector<PluginDescriptor> PluginHost::scan(const std::vector<std::string>&)
{
    PluginDescriptor sampler;
    sampler.name = "Sampler";
    sampler.vendor = "PatchKnob";
    sampler.path.clear();
    sampler.isInstrument = true;
    sampler.numAudioIn = 0;
    sampler.numAudioOut = 2;
    return { sampler };
}

IPluginInstance* PluginHost::instantiate(const PluginDescriptor& desc)
{
    if (desc.name == "Sampler" && desc.path.empty())
        return create_sampler_instrument();
    return nullptr;
}

bool PluginHost::saveCache(const std::vector<PluginDescriptor>&) const { return false; }
bool PluginHost::loadCache(std::vector<PluginDescriptor>& out) const { out.clear(); return false; }

}} // namespace PatchKnob::engine

#endif
