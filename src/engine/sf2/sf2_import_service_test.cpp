//----------------------------------------------------------------------------
//  Headless test for the asynchronous import service (sf2_import_service.h).
//
//  The property that matters most is SUPERSEDE/CANCEL CORRECTNESS: when the
//  user clicks preset A and then preset B before A has landed, B must be the
//  only thing that ever installs -- a stale A result must never be delivered
//  to the message thread, no matter how the two requests interleave with the
//  worker.  That is tested two ways:
//   * deterministically (A allowed to finish, then superseded before take()),
//   * as a stress loop over randomised begin(A)/begin(B) gaps, installing
//     every delivered result exactly the way the shell does and asserting the
//     sampler ends holding ONLY B's zones and that no take() ever yields A's
//     token after B was requested.
//
//  Fonts are built as real .sf2 files on disk (same byte-builder technique as
//  sf2_to_sampler_test.cpp) because the service's whole job is file I/O on a
//  worker thread; hand-built structs would bypass what is being tested.
//----------------------------------------------------------------------------
#include "sf2_import_service.h"
#include "../sampler/sampler_instrument.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace PatchKnob::engine::sf2;
using PatchKnob::engine::IPluginInstance;
using PatchKnob::engine::SamplerZoneInfo;
using PatchKnob::engine::create_sampler_instrument;
using PatchKnob::engine::sampler_zone_count;
using PatchKnob::engine::sampler_get_zone_meta;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_fail;
}

struct Sampler {
    IPluginInstance* inst;
    Sampler() : inst(create_sampler_instrument()) {}
    ~Sampler() { if (inst) { inst->release(); delete inst; } }
    operator IPluginInstance*() const { return inst; }
};

//--------------------------------------------------------------- font maker --
struct Buf {
    std::vector<uint8_t> b;
    void u8 (uint8_t v)  { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
    void u32(uint32_t v) { for (int i=0;i<4;++i) b.push_back((v >> (8*i)) & 0xFF); }
    void tag(const char* t) { for (int i=0;i<4;++i) b.push_back((uint8_t)t[i]); }
    void name20(const std::string& s) {
        for (int i=0;i<20;++i) b.push_back(i < (int)s.size() ? (uint8_t)s[i] : 0);
    }
    void bytes(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); }
};
std::vector<uint8_t> chunk(const char* id, const std::vector<uint8_t>& payload) {
    Buf c; c.tag(id); c.u32((uint32_t)payload.size()); c.bytes(payload);
    if (payload.size() & 1u) c.u8(0);
    return c.b;
}
std::vector<uint8_t> listChunk(const char* type, const std::vector<uint8_t>& payload) {
    Buf inner; inner.tag(type); inner.bytes(payload);
    return chunk("LIST", inner.b);
}
void gen(Buf& g, uint16_t op, int16_t amount) { g.u16(op); g.u16((uint16_t)amount); }

//! A real .sf2 with `numPresets` presets (bank 0, programs 0..n-1), each over
//! its own instrument with `samplesPerPreset` mono zones, one distinct sample
//! per zone.  Sample s of preset p is named "<tag>P<p>S<s>" and its PCM is the
//! constant int16 value (pcmBase + p*100 + s) -- enough to tell every zone of
//! every font apart after an install.
std::vector<uint8_t> buildFont(const std::string& tag, int numPresets,
                               int samplesPerPreset, int framesPerSample,
                               int pcmBase) {
    const int totalSamples = numPresets * samplesPerPreset;

    Buf smpl;
    for (int p = 0; p < numPresets; ++p)
        for (int s = 0; s < samplesPerPreset; ++s) {
            const int16_t v = (int16_t)(pcmBase + p * 100 + s);
            for (int i = 0; i < framesPerSample; ++i) smpl.u16((uint16_t)v);
        }
    for (int i = 0; i < 46; ++i) smpl.u16(0);          // mandatory trailing zeros
    const std::vector<uint8_t> sdta = listChunk("sdta", chunk("smpl", smpl.b));

    Buf phdr, pbag, pgen;
    for (int p = 0; p < numPresets; ++p) {
        phdr.name20(tag + "Preset" + std::to_string(p));
        phdr.u16((uint16_t)p);       // program
        phdr.u16(0);                 // bank
        phdr.u16((uint16_t)p);       // bag index
        phdr.u32(0); phdr.u32(0); phdr.u32(0);
        pbag.u16((uint16_t)p); pbag.u16(0);
        gen(pgen, GEN_instrument, (int16_t)p);         // terminal
    }
    phdr.name20("EOP"); phdr.u16(0); phdr.u16(0);
    phdr.u16((uint16_t)numPresets); phdr.u32(0); phdr.u32(0); phdr.u32(0);
    pbag.u16((uint16_t)numPresets); pbag.u16(0);

    Buf inst, ibag, igen;
    int bagIdx = 0, genCount = 0;                       // ibag stores GEN indices
    for (int p = 0; p < numPresets; ++p) {
        inst.name20(tag + "Inst" + std::to_string(p));
        inst.u16((uint16_t)bagIdx);
        for (int s = 0; s < samplesPerPreset; ++s) {
            ibag.u16((uint16_t)genCount);
            ibag.u16(0);
            gen(igen, GEN_sampleID, (int16_t)(p * samplesPerPreset + s));  // terminal
            ++genCount;                                 // one generator per zone
            ++bagIdx;
        }
    }
    inst.name20("EOI"); inst.u16((uint16_t)bagIdx);
    ibag.u16((uint16_t)genCount); ibag.u16(0);

    Buf shdr;
    for (int p = 0; p < numPresets; ++p)
        for (int s = 0; s < samplesPerPreset; ++s) {
            const uint32_t start = (uint32_t)((p * samplesPerPreset + s) * framesPerSample);
            shdr.name20(tag + "P" + std::to_string(p) + "S" + std::to_string(s));
            shdr.u32(start); shdr.u32(start + (uint32_t)framesPerSample);
            shdr.u32(start); shdr.u32(start + (uint32_t)framesPerSample);
            shdr.u32(44100); shdr.u8(60); shdr.u8(0); shdr.u16(0); shdr.u16(1);
        }
    shdr.name20("EOS");
    shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u32(0);
    shdr.u32(0); shdr.u8(0); shdr.u8(0); shdr.u16(0); shdr.u16(0);
    (void)totalSamples;

    Buf pd;
    pd.bytes(chunk("phdr", phdr.b)); pd.bytes(chunk("pbag", pbag.b));
    pd.bytes(chunk("pmod", {}));     pd.bytes(chunk("pgen", pgen.b));
    pd.bytes(chunk("inst", inst.b)); pd.bytes(chunk("ibag", ibag.b));
    pd.bytes(chunk("imod", {}));     pd.bytes(chunk("igen", igen.b));
    pd.bytes(chunk("shdr", shdr.b));
    const std::vector<uint8_t> pdta = listChunk("pdta", pd.b);

    Buf info; info.bytes(chunk("ifil", {2,0,4,0}));
    { Buf n; n.name20(tag + " Bank"); info.bytes(chunk("INAM", n.b)); }
    const std::vector<uint8_t> infoL = listChunk("INFO", info.b);

    Buf body; body.tag("sfbk"); body.bytes(infoL); body.bytes(sdta); body.bytes(pdta);
    Buf riff; riff.tag("RIFF"); riff.u32((uint32_t)body.b.size()); riff.bytes(body.b);
    return riff.b;
}

std::string writeFont(const std::string& path, const std::vector<uint8_t>& raw) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return {};
    std::fwrite(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    return path;
}

//------------------------------------------------------------------ helpers --
bool waitIdle(Sf2ImportService& svc, int timeoutMs = 10000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!svc.idle()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

//! What the shell's message-thread pump does with a delivered result.
bool installTo(IPluginInstance* sampler, const Sf2ImportService::Completed& c) {
    ImportOptions opts; ImportResult res; std::string err;
    return installPreparedPreset(sampler, c.prepared, opts, res, err);
}

//! First character of every installed zone's (sample) name -- the font tag.
std::string installedTags(IPluginInstance* sampler) {
    std::string tags;
    const int n = sampler_zone_count(sampler);
    for (int i = 0; i < n; ++i) {
        SamplerZoneInfo info;
        if (sampler_get_zone_meta(sampler, i, info) && !info.name.empty())
            tags += info.name[0];
    }
    return tags;
}

//------------------------------------------------------------------- tests ---
std::string g_fontA, g_fontB;
constexpr int kZonesA = 24, kZonesB = 16;

void test_basic_async_import_matches_sync() {
    std::printf("-- async prepare + message-thread install == synchronous import --\n");
    Sf2ImportService svc;
    const uint64_t tok = svc.begin(g_fontA, 0, 0, "A[000:000]");
    check(tok != 0, "begin() hands back a token");

    check(waitIdle(svc), "worker goes idle");
    Sf2ImportService::Completed c;
    check(svc.take(c), "finished result is delivered");
    check(c.token == tok && c.ok, "delivered result is ours and ok", c.error);
    check((int)c.prepared.zones.size() == kZonesA, "all zones prepared",
          std::to_string(c.prepared.zones.size()));

    Sampler viaAsync;
    check(installTo(viaAsync, c), "install on the 'message thread' succeeds");

    // The synchronous path, same font.
    Sampler viaSync;
    SoundFont font; std::string err;
    check(read(g_fontA, font, err, false), "sync headers read", err);
    ImportOptions opts; ImportResult res;
    check(importPresetIntoSampler(g_fontA, font, 0, viaSync, opts, res, err),
          "sync import", err);

    bool same = sampler_zone_count(viaAsync) == sampler_zone_count(viaSync);
    for (int i = 0; same && i < sampler_zone_count(viaSync); ++i) {
        SamplerZoneInfo a, b;
        same = sampler_get_zone_meta(viaAsync, i, a) &&
               sampler_get_zone_meta(viaSync, i, b) &&
               a.name == b.name && a.numFrames == b.numFrames &&
               a.slot == b.slot && a.rootKey == b.rootKey;
    }
    check(same, "async-installed zones are identical to the sync import's");

    check(!svc.take(c), "a result is delivered exactly once");
}

void test_supersede_after_finish_before_take() {
    std::printf("-- supersede AFTER A finished but BEFORE it was taken --\n");
    Sf2ImportService svc;
    Sampler samp;

    svc.begin(g_fontA, 0, 0, "A");
    check(waitIdle(svc), "A finishes (result stored, untaken)");
    // The user clicks B before the shell's next frame polls take():
    const uint64_t tokB = svc.begin(g_fontB, 0, 0, "B");
    check(waitIdle(svc), "B finishes");

    // Pump exactly like the shell: take whatever is delivered, install it.
    Sf2ImportService::Completed c;
    int delivered = 0; uint64_t lastTok = 0;
    while (svc.take(c)) { ++delivered; lastTok = c.token; installTo(samp, c); }
    check(delivered == 1, "exactly one result delivered", std::to_string(delivered));
    check(lastTok == tokB, "and it is B's, never stale A's");
    check(sampler_zone_count(samp) == kZonesB, "sampler holds B's zone count",
          std::to_string(sampler_zone_count(samp)));
    check(installedTags(samp) == std::string((size_t)kZonesB, 'B'),
          "every installed zone is from font B", installedTags(samp));
}

void test_supersede_stress() {
    std::printf("-- stress: begin(A) then begin(B) at randomised gaps --\n");
    Sf2ImportService svc;
    std::mt19937 rng(0xC0FFEE);
    bool staleDelivered = false, endedOnB = true, aLandedAfterB = false;

    for (int iter = 0; iter < 40; ++iter) {
        Sampler samp;
        const uint64_t tokA = svc.begin(g_fontA, 0, 0, "A");
        // Gap spans "immediately" through "A comfortably finished".
        const int gapUs = (int)(rng() % 20000);
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0).count() < gapUs) { /* spin */ }
        const uint64_t tokB = svc.begin(g_fontB, 0, 1, "B");

        if (!waitIdle(svc)) { staleDelivered = true; break; }
        Sf2ImportService::Completed c;
        while (svc.take(c)) {
            if (c.token == tokA) { staleDelivered = true; aLandedAfterB = true; }
            installTo(samp, c);
        }
        const std::string tags = installedTags(samp);
        if (sampler_zone_count(samp) != kZonesB ||
            tags != std::string((size_t)kZonesB, 'B')) {
            endedOnB = false;
            break;
        }
    }
    check(!staleDelivered, "no take() after begin(B) ever delivered A's token");
    check(!aLandedAfterB, "stale zones can never reach the sampler");
    check(endedOnB, "after every interleaving the sampler holds exactly B (program 1)");
}

void test_cancel() {
    std::printf("-- cancel() abandons in-flight work and delivers nothing --\n");
    Sf2ImportService svc;
    Sampler samp;
    svc.begin(g_fontA, 0, 0, "A");
    svc.cancel();
    check(waitIdle(svc), "worker returns to idle after cancel");
    Sf2ImportService::Completed c;
    check(!svc.take(c), "nothing is delivered");
    check(sampler_zone_count(samp) == 0, "sampler untouched");
}

void test_error_delivery() {
    std::printf("-- a failed prepare is delivered as ok=false, not swallowed --\n");
    Sf2ImportService svc;
    const uint64_t tok = svc.begin(g_fontA, 9, 99, "missing");   // no such preset
    check(waitIdle(svc), "worker idles");
    Sf2ImportService::Completed c;
    check(svc.take(c), "error result IS delivered (the UI must say why)");
    check(c.token == tok && !c.ok && !c.error.empty(), "carries the error", c.error);

    const uint64_t tok2 = svc.begin("/nonexistent/no.sf2", 0, 0, "gone");
    check(waitIdle(svc), "worker idles");
    check(svc.take(c) && c.token == tok2 && !c.ok, "unreadable file also reported", c.error);
}

void test_progress_reporting() {
    std::printf("-- progress: counts advance and finish at done==total --\n");
    Sf2ImportService svc;
    svc.begin(g_fontA, 0, 0, "A");
    bool sawTotal = false, sane = true;
    for (int i = 0; i < 20000 && !svc.idle(); ++i) {
        const auto p = svc.progress();
        if (p.samplesTotal > 0) sawTotal = true;
        if (p.samplesDone > p.samplesTotal) sane = false;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    const auto p = svc.progress();
    check(sane, "samplesDone never exceeds samplesTotal");
    // On a warm tiny font the whole prepare can outrun the poll loop; the
    // final state is the part that must always hold.
    check(p.samplesDone == p.samplesTotal, "final: done == total",
          std::to_string(p.samplesDone) + "/" + std::to_string(p.samplesTotal));
    check(sawTotal || p.samplesTotal > 0, "a total was published");
    Sf2ImportService::Completed c;
    check(svc.take(c) && c.ok, "and the import still completed", c.error);
}

void test_prefetch_is_preemptible_and_harmless() {
    std::printf("-- prefetch: runs to idle, and a real begin() preempts it --\n");
    Sf2ImportService svc;
    svc.prefetchNext(g_fontA, 0, 0);           // warms preset 1
    check(waitIdle(svc), "prefetch alone returns to idle");
    Sf2ImportService::Completed c;
    check(!svc.take(c), "prefetch never delivers a result");

    svc.prefetchNext(g_fontA, 0, 0);
    const uint64_t tok = svc.begin(g_fontB, 0, 0, "B");   // preempts / outranks it
    check(waitIdle(svc), "idle after prefetch + begin");
    bool got = false; uint64_t gotTok = 0;
    while (svc.take(c)) { got = true; gotTok = c.token; }
    check(got && gotTok == tok, "the real request is delivered regardless");
}

} // namespace

int main() {
    std::printf("sf2 import service test\n");
    g_fontA = writeFont("/tmp/pk_sf2_import_service_A.sf2",
                        buildFont("A", 2, kZonesA, 4096, 1000));
    g_fontB = writeFont("/tmp/pk_sf2_import_service_B.sf2",
                        buildFont("B", 2, kZonesB, 4096, 20000));
    if (g_fontA.empty() || g_fontB.empty()) {
        std::printf("FAIL  could not write test fonts\n");
        return 1;
    }

    test_basic_async_import_matches_sync();
    test_supersede_after_finish_before_take();
    test_supersede_stress();
    test_cancel();
    test_error_delivery();
    test_progress_reporting();
    test_prefetch_is_preemptible_and_harmless();

    std::printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail ? 1 : 0;
}
