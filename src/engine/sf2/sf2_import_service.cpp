//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_import_service.cpp -- see the header for the contract.
//----------------------------------------------------------------------------
#include "sf2_import_service.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace PatchKnob { namespace engine { namespace sf2 {

namespace {

//! 64-bit-clean seek (same rationale as the reader's private helper: on
//! Windows `long` is 32-bit and plain fseek truncates offsets past 2 GB).
int seek64s(FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

int findPreset(const SoundFont& font, int bank, int program) {
    for (size_t i = 0; i < font.presets.size(); ++i)
        if (font.presets[i].bank == bank && font.presets[i].program == program)
            return (int)i;
    return -1;
}

} // namespace

//----------------------------------------------------------------------------
Sf2ImportService::Sf2ImportService() {
    m_worker = std::thread([this] { workerMain(); });
}

Sf2ImportService::~Sf2ImportService() {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_quit = true;
        m_hasPending = false;
        m_prefetch.clear();
        // Poison the newest token so an in-flight prepare's next progress
        // poll cancels it; the join below is then bounded by ~one sample
        // decode, not by a whole cold preset.
        m_newest.store(m_nextToken++, std::memory_order_release);
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

//----------------------------------------------------------------------------
uint64_t Sf2ImportService::begin(const std::string& fontPath, int bank, int program,
                                 const std::string& label, const ImportOptions& opts) {
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        token = m_nextToken++;
        m_pending = Request{token, fontPath, bank, program, label, opts};
        m_hasPending = true;
        m_label = label;
        // Any stored result is stale by construction (its token < this one);
        // free its PCM now rather than holding two patches' worth of buffers
        // until the next take().
        if (m_hasResult) { m_result = Completed{}; m_hasResult = false; }
        m_done.store(0, std::memory_order_relaxed);
        m_total.store(0, std::memory_order_relaxed);
        // Publish LAST, after the request is fully staged: from this store on,
        // the worker treats every older token as superseded.
        m_newest.store(token, std::memory_order_release);
    }
    m_cv.notify_all();
    return token;
}

void Sf2ImportService::cancel() {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_hasPending = false;
        if (m_hasResult) { m_result = Completed{}; m_hasResult = false; }
        m_done.store(0, std::memory_order_relaxed);
        m_total.store(0, std::memory_order_relaxed);
        // A fresh token nothing will ever run under: whatever is in flight is
        // now stale and will abandon itself at its next poll.
        m_newest.store(m_nextToken++, std::memory_order_release);
    }
    m_cv.notify_all();
}

Sf2ImportService::Progress Sf2ImportService::progress() const {
    Progress p;
    std::lock_guard<std::mutex> lk(m_mx);
    p.token        = m_newest.load(std::memory_order_relaxed);
    p.active       = m_hasPending || m_busy.load(std::memory_order_relaxed);
    p.samplesDone  = m_done.load(std::memory_order_relaxed);
    p.samplesTotal = m_total.load(std::memory_order_relaxed);
    p.label        = m_label;
    return p;
}

bool Sf2ImportService::take(Completed& out) {
    std::lock_guard<std::mutex> lk(m_mx);
    if (!m_hasResult) return false;
    if (m_result.token != m_newest.load(std::memory_order_relaxed)) {
        // Superseded between finishing and being taken: destroy, never
        // deliver.  (begin() usually frees stale results eagerly; this is the
        // belt to that suspender, and what the supersede test pins down.)
        m_result = Completed{};
        m_hasResult = false;
        return false;
    }
    out = std::move(m_result);
    m_result = Completed{};
    m_hasResult = false;
    return true;
}

bool Sf2ImportService::idle() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return !m_hasPending && m_prefetch.empty() &&
           !m_busy.load(std::memory_order_relaxed);
}

void Sf2ImportService::prefetchNext(const std::string& fontPath, int bank, int program) {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        // Depth 1: only the most recent hint is worth acting on.
        m_prefetch.clear();
        m_prefetch.push_back(PrefetchReq{fontPath, bank, program});
    }
    m_cv.notify_all();
}

//----------------------------------------------------------------------------
void Sf2ImportService::workerMain() {
    for (;;) {
        Request req;
        PrefetchReq pf;
        bool havePrepare = false, havePrefetch = false;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] { return m_quit || m_hasPending || !m_prefetch.empty(); });
            if (m_quit) return;
            if (m_hasPending) {                    // real work always first
                req = std::move(m_pending);
                m_hasPending = false;
                havePrepare = true;
            } else {
                pf = std::move(m_prefetch.front());
                m_prefetch.pop_front();
                havePrefetch = true;
            }
            m_busy.store(true, std::memory_order_relaxed);
        }
        if (havePrepare)       runPrepare(std::move(req));
        else if (havePrefetch) runPrefetch(pf);
        m_busy.store(false, std::memory_order_relaxed);
        m_cv.notify_all();     // idle()-waiters (tests, shutdown gates)
    }
}

void Sf2ImportService::runPrepare(Request req) {
    auto superseded = [this, &req] {
        return m_newest.load(std::memory_order_acquire) != req.token;
    };

    Completed done;
    done.token    = req.token;
    done.fontPath = req.fontPath;
    done.label    = req.label;
    done.bank     = req.bank;
    done.program  = req.program;

    // Headers-only parse: a few MB even on an 800 MB bank.  Re-parsed per
    // request -- measured at 1.5-4 ms warm on the two real banks, which is
    // noise next to the decode, so a cache would be complexity for nothing.
    SoundFont font;
    std::string err;
    if (!read(req.fontPath, font, err, /*loadPcm=*/false)) {
        done.ok = false;
        done.error = "soundfont read failed: " + err;
    } else if (superseded()) {
        return;                                    // abandoned; deliver nothing
    } else {
        done.presetIndex = findPreset(font, req.bank, req.program);
        if (done.presetIndex < 0) {
            done.ok = false;
            done.error = "preset [" + std::to_string(req.bank) + ":" +
                         std::to_string(req.program) + "] not found in \"" +
                         req.fontPath + "\"";
        } else {
            auto onProgress = [&](int d, int t) -> bool {
                m_done.store(d, std::memory_order_relaxed);
                m_total.store(t, std::memory_order_relaxed);
                return !superseded();              // false => cancel mid-decode
            };
            done.ok = preparePresetZones(req.fontPath, font, done.presetIndex,
                                         req.opts, done.prepared, err, onProgress);
            if (!done.ok) {
                if (superseded()) return;          // cancelled: deliver nothing
                done.error = err;
            }
        }
    }

    std::lock_guard<std::mutex> lk(m_mx);
    if (m_newest.load(std::memory_order_relaxed) != req.token)
        return;   // superseded while finishing: drop (PCM freed here, off the UI)
    m_result = std::move(done);
    m_hasResult = true;
}

//----------------------------------------------------------------------------
//  Page-cache warming.  Reads the raw byte ranges of the samples the NEXT
//  preset needs (the one after (bank, program) in the sorted preset list) and
//  throws the bytes away -- the point is what the kernel keeps, not what we
//  do with it.  No decode, no per-preset allocation beyond one 1 MB scratch.
//----------------------------------------------------------------------------
void Sf2ImportService::runPrefetch(const PrefetchReq& pf) {
    auto preempted = [this] {
        std::lock_guard<std::mutex> lk(m_mx);
        return m_quit || m_hasPending;
    };
    if (preempted()) return;

    SoundFont font;
    std::string err;
    if (!read(pf.fontPath, font, err, /*loadPcm=*/false)) return;   // best effort

    const int cur = findPreset(font, pf.bank, pf.program);
    if (cur < 0 || cur + 1 >= (int)font.presets.size()) return;
    const Preset& next = font.presets[(size_t)cur + 1];

    // The byte ranges the next preset's zones draw on (16-bit pool only; the
    // sm24 sidecar is tiny and contiguous with nothing, skip it).  Stereo
    // partners are included via the same predicate the real decode uses.
    struct Span { uint64_t off, len; };
    std::vector<Span> spans;
    std::vector<char> seen(font.samples.size(), 0);
    auto add = [&](int si) {
        if (si < 0 || si >= (int)font.samples.size() || seen[(size_t)si]) return;
        seen[(size_t)si] = 1;
        const Sample& s = font.samples[(size_t)si];
        if (s.end <= s.start) return;
        spans.push_back({font.smplOffset + (uint64_t)s.start * 2,
                         ((uint64_t)s.end - s.start) * 2});
    };
    for (const Zone& z : next.zones) {
        add(z.sampleIndex);
        int partner = -1;
        if (samplesFormStereoPair(font, z.sampleIndex, partner)) add(partner);
    }
    if (spans.empty()) return;
    std::sort(spans.begin(), spans.end(),
              [](const Span& a, const Span& b) { return a.off < b.off; });

    FILE* f = std::fopen(pf.fontPath.c_str(), "rb");
    if (!f) return;
    std::vector<char> scratch(1 << 20);
    for (const Span& sp : spans) {
        if (seek64s(f, sp.off) != 0) break;
        uint64_t left = sp.len;
        while (left > 0) {
            const size_t n = (size_t)std::min<uint64_t>(left, scratch.size());
            if (std::fread(scratch.data(), 1, n, f) != n) { left = 0; break; }
            left -= n;
            // A real request preempts within one chunk (~7 ms cold on this
            // device); the cache keeps whatever was already warmed.
            if (preempted()) { std::fclose(f); return; }
        }
    }
    std::fclose(f);
}

}}} // namespace PatchKnob::engine::sf2
