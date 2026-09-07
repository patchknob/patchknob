#include "api_key_store.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <SDL.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <wincrypt.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace PatchKnob { namespace ai {

namespace {

const char kMagic[] = "PKAK1";   // PatchKnob Api Key, format 1

std::string prefsDir() {
    char* p = SDL_GetPrefPath("PatchKnob", "PatchKnob");
    std::string s = p ? std::string(p) : std::string();
    if (p) SDL_free(p);
    return s;
}

//  FNV-1a. Not a cryptographic hash -- it is an identity fold, used to turn a
//  machine id into a short stable token and a keystream seed.
uint64_t fnv1a(const std::string& s, uint64_t seed = 1469598103934665603ULL) {
    uint64_t h = seed;
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

//! Everything that makes this install distinct: the OS machine id, the user,
//! and the app's own prefs path. Two machines never agree; the same machine
//! agrees across rebuilds, so upgrading PatchKnob does not lose the key.
std::string machineIdentity() {
    std::string id;
#if defined(_WIN32)
    DWORD n = 0;
    char buf[256] = {0};
    n = (DWORD)sizeof(buf);
    if (GetComputerNameA(buf, &n)) id += std::string(buf, n);
    n = (DWORD)sizeof(buf);
    if (GetUserNameA(buf, &n) && n > 0) id += "|" + std::string(buf, n - 1);
#else
    //  /etc/machine-id is stable across reboots and distinct per install;
    //  the dbus copy is the fallback on systems that only have that one.
    for (const char* p : { "/etc/machine-id", "/var/lib/dbus/machine-id" }) {
        std::ifstream f(p);
        if (!f) continue;
        std::string line;
        if (std::getline(f, line) && !line.empty()) { id = line; break; }
    }
    if (id.empty()) {
        //  No machine-id (containers, some BSDs): fall back to hostname. Worse,
        //  but still machine-scoped, and the binding check simply becomes
        //  hostname-scoped rather than silently becoming no check at all.
        char host[256] = {0};
        if (gethostname(host, sizeof host - 1) == 0) id = host;
        else id = "unknown-machine";
    }
    id += "|uid:" + std::to_string((unsigned long)getuid());
#endif
    id += "|" + prefsDir();
    return id;
}

} // namespace

std::string apiKeyPath() {
    const std::string d = prefsDir();
    return d.empty() ? std::string("anthropic_api_key") : d + "anthropic_api_key";
}

std::string machineFingerprint() {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx",
                  (unsigned long long)fnv1a(machineIdentity()));
    return buf;
}

bool looksLikeApiKey(const std::string& key, std::string* reason) {
    if (key.empty()) {
        if (reason) *reason = "The key is empty.";
        return false;
    }
    if (key.rfind("sk-ant-", 0) != 0) {
        if (reason) *reason = "An Anthropic API key starts with \"sk-ant-\". "
                              "Check you pasted the key and not something else.";
        return false;
    }
    if (key.size() < 20) {
        if (reason) *reason = "That key looks truncated.";
        return false;
    }
    for (size_t i = 0; i < key.size(); ++i) {
        const unsigned char c = (unsigned char)key[i];
        if (c <= 0x20 || c >= 0x7F) {
            if (reason) *reason = "The key contains whitespace or control "
                                  "characters -- it may have been copied with "
                                  "a line break.";
            return false;
        }
    }
    if (reason) reason->clear();
    return true;
}

#if defined(_WIN32)
//----------------------------------------------------------------------------
//  Windows: DPAPI. CryptProtectData is bound to this user on this machine, so
//  the machine binding is enforced by the OS rather than by us.
//----------------------------------------------------------------------------
bool saveApiKey(const std::string& key, std::string* error) {
    DATA_BLOB in, out;
    in.pbData = (BYTE*)key.data();
    in.cbData = (DWORD)key.size();
    if (!CryptProtectData(&in, L"PatchKnob Anthropic API key", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (error) *error = "Windows refused to encrypt the key (DPAPI).";
        return false;
    }
    std::ofstream f(apiKeyPath(), std::ios::binary | std::ios::trunc);
    if (!f) {
        LocalFree(out.pbData);
        if (error) *error = "Could not write " + apiKeyPath();
        return false;
    }
    f.write(kMagic, 5);
    f.write((const char*)out.pbData, (std::streamsize)out.cbData);
    LocalFree(out.pbData);
    return true;
}

KeyLoad loadApiKey() {
    KeyLoad r;
    std::ifstream f(apiKeyPath(), std::ios::binary);
    if (!f) return r;                                  // Missing
    std::string blob((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    if (blob.size() < 6 || blob.compare(0, 5, kMagic) != 0) {
        r.status = KeyStatus::Unreadable; return r;
    }
    DATA_BLOB in, out;
    in.pbData = (BYTE*)blob.data() + 5;
    in.cbData = (DWORD)(blob.size() - 5);
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        //  DPAPI refuses data sealed by another user or machine -- which is
        //  precisely the "this key belongs to a different install" case.
        r.status = KeyStatus::ForeignMachine; return r;
    }
    r.key.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    r.status = KeyStatus::Ok;
    return r;
}

#else
//----------------------------------------------------------------------------
//  POSIX: machine-bound obfuscation + 0600. See the header for exactly what
//  this does and does not protect against.
//----------------------------------------------------------------------------
namespace {

void xorStream(std::string& data, uint64_t seed) {
    //  SplitMix64 keystream: cheap, and far better distributed than repeating
    //  a short key, so the ciphertext does not leak the plaintext's shape.
    uint64_t x = seed;
    for (size_t i = 0; i < data.size(); ++i) {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z =  z ^ (z >> 31);
        data[i] = (char)((unsigned char)data[i] ^ (unsigned char)(z & 0xFF));
    }
}

std::string toHex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        out += d[c >> 4];
        out += d[c & 0xF];
    }
    return out;
}

bool fromHex(const std::string& s, std::string& out) {
    if (s.size() % 2) return false;
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; ++k) {
            const char c = s[i + (size_t)k];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) return false;
            (k == 0 ? hi : lo) = v;
        }
        out += (char)((hi << 4) | lo);
    }
    return true;
}

} // namespace

bool saveApiKey(const std::string& key, std::string* error) {
    const std::string ident = machineIdentity();
    std::string blob = key;
    xorStream(blob, fnv1a(ident));

    const std::string path = apiKeyPath();
    {
        std::ofstream f(path, std::ios::trunc);
        if (!f) {
            if (error) *error = "Could not write " + path;
            return false;
        }
        //  Format: magic, the machine token, then the obfuscated key in hex.
        //  The token is what makes a foreign file detectable rather than
        //  silently decoding to garbage that we would then send upstream.
        f << kMagic << "\n" << machineFingerprint() << "\n" << toHex(blob) << "\n";
    }
    //  Owner-only. Do this AFTER the stream closes so the mode is not widened
    //  by a later open, and do not treat failure as fatal on filesystems that
    //  cannot represent it (a FAT-formatted USB home directory).
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0 && error)
        *error = "Stored, but the file permissions could not be tightened.";
    return true;
}

KeyLoad loadApiKey() {
    KeyLoad r;
    std::ifstream f(apiKeyPath());
    if (!f) return r;                                   // Missing

    std::string magic, token, hex;
    if (!std::getline(f, magic) || !std::getline(f, token) || !std::getline(f, hex)) {
        r.status = KeyStatus::Unreadable; return r;
    }
    if (magic != kMagic) { r.status = KeyStatus::Unreadable; return r; }
    if (token != machineFingerprint()) {
        //  Written on another machine (or by another user): refuse it outright
        //  rather than decoding it into nonsense.
        r.status = KeyStatus::ForeignMachine; return r;
    }
    std::string blob;
    if (!fromHex(hex, blob)) { r.status = KeyStatus::Unreadable; return r; }
    xorStream(blob, fnv1a(machineIdentity()));
    if (!looksLikeApiKey(blob)) { r.status = KeyStatus::Unreadable; return r; }
    r.key = blob;
    r.status = KeyStatus::Ok;
    return r;
}
#endif

bool clearApiKey() { return std::remove(apiKeyPath().c_str()) == 0; }

}} // namespace PatchKnob::ai
