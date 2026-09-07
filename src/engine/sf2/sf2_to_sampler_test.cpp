//----------------------------------------------------------------------------
//  Headless test for the SF2 -> sampler mapping layer (sf2_to_sampler.h).
//
//  Two ways a SoundFont reaches this test, matching how the two real callers
//  use it:
//
//   * test_real_file_round_trip() BUILDS a tiny .sf2 byte by byte -- the same
//     technique sf2_reader_test.cpp uses -- so the full real pipeline (read()
//     headers-only, then importPresetIntoSampler() pulling PCM itself via
//     readPresetPcm()) is exercised against an actual file, with no real .sf2
//     required to be present on the machine.
//   * every other test constructs a `SoundFont` directly (samples/preset/zones
//     as plain structs, PCM pre-filled). That is what the mapping layer
//     actually consumes -- it neither knows nor cares whether those structs
//     came from a real file or a hand-built RIFF blob -- and it is the only
//     practical way to reach the 2976-zone scale of the real interning claim.
//     Pre-filling Sample::pcm makes readPresetPcm() a no-op for it (it only
//     reads samples that are still empty), so no file I/O happens.
//----------------------------------------------------------------------------
#include "sf2_to_sampler.h"
#include "../sampler/sampler_instrument.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PatchKnob::engine::sf2;
using PatchKnob::engine::IPluginInstance;
using PatchKnob::engine::SamplerZoneInfo;
using PatchKnob::engine::SamplerZoneEnv;
using PatchKnob::engine::create_sampler_instrument;
using PatchKnob::engine::sampler_zone_count;
using PatchKnob::engine::sampler_get_zone;
using PatchKnob::engine::sampler_zone_pcm_ptr;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_fail;
}
void checkNear(float got, float want, float tol, const std::string& what) {
    char buf[128]; std::snprintf(buf, sizeof buf, "(got %.4f, want %.4f)", got, want);
    check(std::fabs(got - want) <= tol, what, buf);
}

//! RAII around create_sampler_instrument()/release()+delete, and an implicit
//! conversion so a Sampler can be passed anywhere an IPluginInstance* is
//! expected -- every test gets its own fresh instrument.
struct Sampler {
    IPluginInstance* inst;
    Sampler() : inst(create_sampler_instrument()) {}
    ~Sampler() { if (inst) { inst->release(); delete inst; } }
    operator IPluginInstance*() const { return inst; }
};

//! A mono sample with distinguishable, non-zero PCM (frame i != 0 for i >= 0)
//! so stereo interleaving and buffer-sharing checks can tell samples apart.
Sample makeMonoSample(const std::string& name, int frames, float scale = 1.f,
                      uint16_t sampleType = 1, uint16_t sampleLink = 0) {
    Sample s;
    s.name = name;
    s.sampleRate = 44100;
    s.originalKey = 60;
    s.sampleType = sampleType;
    s.sampleLink = sampleLink;
    s.start = 0; s.end = (uint32_t)frames;
    s.loopStart = 0; s.loopEnd = (uint32_t)frames;
    s.pcm.resize((size_t)frames);
    for (int i = 0; i < frames; ++i)
        s.pcm[(size_t)i] = scale * (float)(i + 1) / (float)(frames + 1);
    return s;
}

Zone makeZone(int sampleIndex, int loKey = 0, int hiKey = 127,
             int loVel = 0, int hiVel = 127, int rootKey = 60) {
    Zone z;
    z.sampleIndex = sampleIndex;
    z.loKey = loKey; z.hiKey = hiKey; z.loVel = loVel; z.hiVel = hiVel;
    z.rootKey = rootKey;
    return z;
}

//---------------------------------------------------------- byte builder ---
// Same technique as sf2_reader_test.cpp: a minimal RIFF/sfbk with one preset,
// one instrument, one zone over one looping mono sample. Only used by
// test_real_file_round_trip() to prove the real read()+readPresetPcm() path
// -- not just hand-built structs -- comes out the other end correctly.
struct Buf {
    std::vector<uint8_t> b;
    void u8 (uint8_t v)  { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
    void u32(uint32_t v) { for (int i=0;i<4;++i) b.push_back((v >> (8*i)) & 0xFF); }
    void tag(const char* t) { for (int i=0;i<4;++i) b.push_back((uint8_t)t[i]); }
    void name20(const char* s) {
        for (int i=0;i<20;++i) b.push_back(i < (int)std::strlen(s) ? (uint8_t)s[i] : 0);
    }
    void bytes(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); }
    size_t size() const { return b.size(); }
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

std::vector<uint8_t> buildOneZoneFont() {
    Buf smpl;
    for (int i = 0; i < 32; ++i) smpl.u16((uint16_t)(int16_t)(i * 512));
    for (int i = 0; i < 46; ++i) smpl.u16(0);   // spec's mandatory trailing silence
    const std::vector<uint8_t> sdta = listChunk("sdta", chunk("smpl", smpl.b));

    Buf phdr;
    phdr.name20("IntegrationTest"); phdr.u16(0); phdr.u16(0);
    phdr.u16(0); phdr.u32(0); phdr.u32(0); phdr.u32(0);
    phdr.name20("EOP");             phdr.u16(0); phdr.u16(0);
    phdr.u16(1); phdr.u32(0); phdr.u32(0); phdr.u32(0);

    Buf pbag; pbag.u16(0); pbag.u16(0);
                pbag.u16(1); pbag.u16(0);
    Buf pgen; gen(pgen, GEN_instrument, 0);   // terminal (points at the only instrument)

    Buf inst; inst.name20("TestInst"); inst.u16(0);
              inst.name20("EOI");      inst.u16(2);

    Buf ibag; ibag.u16(0); ibag.u16(0);
                ibag.u16(2); ibag.u16(0);
    Buf igen;
    gen(igen, GEN_sampleModes, 1);            // loop continuously
    gen(igen, GEN_sampleID,    0);             // terminal (the only sample)

    Buf shdr;
    shdr.name20("Ramp");
    shdr.u32(0); shdr.u32(32); shdr.u32(8); shdr.u32(24);
    shdr.u32(44100); shdr.u8(60); shdr.u8(0); shdr.u16(0); shdr.u16(1);
    shdr.name20("EOS");
    shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u32(0);
    shdr.u32(0); shdr.u8(0); shdr.u8(0); shdr.u16(0); shdr.u16(0);

    Buf pd;
    pd.bytes(chunk("phdr", phdr.b)); pd.bytes(chunk("pbag", pbag.b));
    pd.bytes(chunk("pmod", {}));     pd.bytes(chunk("pgen", pgen.b));
    pd.bytes(chunk("inst", inst.b)); pd.bytes(chunk("ibag", ibag.b));
    pd.bytes(chunk("imod", {}));     pd.bytes(chunk("igen", igen.b));
    pd.bytes(chunk("shdr", shdr.b));
    const std::vector<uint8_t> pdta = listChunk("pdta", pd.b);

    Buf info; info.bytes(chunk("ifil", {2,0,4,0}));
    { Buf n; n.name20("Test Bank"); info.bytes(chunk("INAM", n.b)); }
    const std::vector<uint8_t> infoL = listChunk("INFO", info.b);

    Buf body; body.tag("sfbk"); body.bytes(infoL); body.bytes(sdta); body.bytes(pdta);
    Buf riff; riff.tag("RIFF"); riff.u32((uint32_t)body.size()); riff.bytes(body.b);
    return riff.b;
}

//------------------------------------------------------------------ tests ---

void test_real_file_round_trip() {
    std::printf("-- real file: headers-only read, then importPresetIntoSampler pulls PCM itself --\n");
    const std::vector<uint8_t> raw = buildOneZoneFont();
    const std::string path = "/tmp/pk_sf2_to_sampler_test.sf2";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { check(false, "could not write test file"); return; }
    std::fwrite(raw.data(), 1, raw.size(), f); std::fclose(f);

    SoundFont font; std::string err;
    check(read(path, font, err, /*loadPcm=*/false), "headers-only read succeeds", err);
    check(font.samples.size() == 1 && font.samples[0].pcm.empty(),
          "headers-only really loaded no PCM yet");

    Sampler samp; ImportOptions opts; ImportResult result;
    const bool ok = importPresetIntoSampler(path, font, 0, samp, opts, result, err);
    check(ok, "import succeeds off a headers-only SoundFont", err);
    check(result.zonesLoaded == 1, "one zone loaded");
    check(result.samplesInterned == 1, "one buffer interned");
    check(sampler_zone_count(samp) == 1, "the sampler holds exactly that zone");

    SamplerZoneInfo info;
    check(sampler_get_zone(samp, 0, info), "zone 0 reads back through the same API main.cpp uses");
    check(info.slot == 1 && info.level == 0, "addressing scheme: zone 0 got slot 1, level 0",
          std::to_string(info.slot) + "," + std::to_string(info.level));
    check(!info.stereo, "mono sample stays mono");
    check(info.numFrames == 32, "frame count carried through");
    check(info.loop && info.loopStart == 8 && info.loopEnd == 24, "loop points carried through");
    check(info.rootKey == 60, "root key defaults to the sample's own original key");

    const float* pcm = sampler_zone_pcm_ptr(samp, 0);
    check(pcm != nullptr, "pcm pointer is non-null");
    if (pcm) checkNear(pcm[1], 512.f / 32768.f, 1e-5f, "decoded 16-bit PCM reaches the zone unmodified");
}

void test_interning_and_ram() {
    std::printf("-- interning at scale: 2976 zones over 192 distinct samples --\n");
    const int kSamples = 192;
    const int kZones   = 2976;
    // Chosen so kSamples*kFrames*4 bytes ~= 83 MB and kZones*kFrames*4 bytes
    // ~= 1285 MB -- the exact figures the task measured on the real
    // Concert Grand preset.
    const int kFrames  = 108073;

    SoundFont font;
    font.samples.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i)
        font.samples.push_back(makeMonoSample("s" + std::to_string(i), kFrames));

    Preset p; p.name = "Concert Grand"; p.bank = 0; p.program = 0;
    p.zones.reserve(kZones);
    for (int i = 0; i < kZones; ++i) p.zones.push_back(makeZone(i % kSamples));
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    const bool ok = importPresetIntoSampler("", font, 0, samp, opts, result, err);
    check(ok, "the 2976-zone preset imports without error", err);
    check(result.zonesLoaded == kZones, "all " + std::to_string(kZones) + " zones loaded",
          std::to_string(result.zonesLoaded));
    check(result.zonesSkipped == 0, "none skipped");
    check(result.samplesInterned == kSamples,
          "exactly " + std::to_string(kSamples) + " distinct buffers interned (not " +
          std::to_string(kZones) + ")", std::to_string(result.samplesInterned));
    check(sampler_zone_count(samp) == kZones, "the sampler holds one zone per SF2 zone");

    // Not just a matching COUNT -- zone 0 and zone 192 both reference physical
    // sample 0 (192 % 192 == 0), so they must read back the SAME pointer.
    const float* p0   = sampler_zone_pcm_ptr(samp, 0);
    const float* p192 = sampler_zone_pcm_ptr(samp, kSamples);
    check(p0 != nullptr && p0 == p192, "zones sharing a sample share the SAME buffer pointer");

    const double internedMB = (double)result.samplesInterned * kFrames * sizeof(float) / 1.0e6;
    const double copiedMB   = (double)result.zonesLoaded    * kFrames * sizeof(float) / 1.0e6;
    std::printf("   measured: %.2f MB interned vs %.2f MB if every zone copied its sample (%.1fx)\n",
                internedMB, copiedMB, copiedMB / internedMB);
    checkNear((float)internedMB, 83.f,   2.f,  "interned size lands at the measured ~83 MB");
    checkNear((float)copiedMB,   1285.f, 20.f, "copy-everywhere size lands at the measured ~1285 MB");
}

void test_stereo_pairing() {
    std::printf("-- stereo: two linked mono samples merge into ONE stereo zone --\n");
    const int frames = 50;
    SoundFont font;
    const Sample L = makeMonoSample("Left",  frames,  1.f, /*sampleType=*/4, /*sampleLink=*/1);
    const Sample R = makeMonoSample("Right", frames, -1.f, /*sampleType=*/2, /*sampleLink=*/0);
    font.samples.push_back(L); font.samples.push_back(R);

    Preset p; p.name = "Stereo Pad";
    p.zones.push_back(makeZone(0));   // the left half's own zone entry
    p.zones.push_back(makeZone(1));   // the right half's own zone entry
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    const bool ok = importPresetIntoSampler("", font, 0, samp, opts, result, err);
    check(ok, "the stereo preset imports", err);
    check(result.zonesLoaded == 1, "the L/R pair collapses into ONE sampler zone, not two",
          std::to_string(result.zonesLoaded));
    check(result.stereoPairs == 1, "one stereo pair recognised");
    check(result.samplesInterned == 1, "one interleaved buffer interned for the pair (not two)");
    check(sampler_zone_count(samp) == 1, "the sampler holds exactly one zone");

    SamplerZoneInfo info;
    check(sampler_get_zone(samp, 0, info), "zone 0 reads back");
    check(info.stereo, "the zone is marked stereo");
    check(info.numFrames == frames, "numFrames is per-channel, not the doubled interleaved count");

    const float* raw = sampler_zone_pcm_ptr(samp, 0);
    check(raw != nullptr, "pcm pointer is non-null");
    if (raw) {
        checkNear(raw[0], L.pcm[0], 1e-6f, "interleaved frame 0 is the LEFT sample");
        checkNear(raw[1], R.pcm[0], 1e-6f, "interleaved frame 0 is followed by the RIGHT sample");
        checkNear(raw[2], L.pcm[1], 1e-6f, "interleaved frame 1 left");
        checkNear(raw[3], R.pcm[1], 1e-6f, "interleaved frame 1 right");
    }
}

void test_stereo_broken_link_out_of_range() {
    std::printf("-- stereo fallback: partner index out of range --\n");
    const int frames = 20;
    SoundFont font;
    font.samples.push_back(makeMonoSample("Solo", frames, 1.f, /*sampleType=*/4, /*sampleLink=*/99));
    Preset p; p.zones.push_back(makeZone(0));
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    check(importPresetIntoSampler("", font, 0, samp, opts, result, err),
          "imports despite the broken link", err);
    check(result.zonesLoaded == 1, "the zone still loads");
    SamplerZoneInfo info;
    check(sampler_get_zone(samp, 0, info), "zone reads back");
    check(!info.stereo, "falls back to MONO rather than failing");
    check(info.numFrames == frames, "mono frame count is the sample's own length");
    check(!result.warning.empty(), "a warning is reported for the fallback", result.warning);
}

void test_stereo_broken_link_same_channel() {
    std::printf("-- stereo fallback: partner is not the opposite channel --\n");
    const int frames = 20;
    SoundFont font;
    font.samples.push_back(makeMonoSample("A", frames, 1.f, /*sampleType=*/4, /*sampleLink=*/1));
    font.samples.push_back(makeMonoSample("B", frames, 1.f, /*sampleType=*/4, /*sampleLink=*/0)); // also "left"
    Preset p; p.zones.push_back(makeZone(0));
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    check(importPresetIntoSampler("", font, 0, samp, opts, result, err), "imports", err);
    SamplerZoneInfo info;
    check(sampler_get_zone(samp, 0, info), "zone reads back");
    check(!info.stereo, "falls back to mono when the partner is the same channel");
}

void test_stereo_broken_link_length_mismatch() {
    std::printf("-- stereo fallback: partner has a different frame count --\n");
    SoundFont font;
    font.samples.push_back(makeMonoSample("A", 20, 1.f, /*sampleType=*/4, /*sampleLink=*/1));
    font.samples.push_back(makeMonoSample("B", 30, 1.f, /*sampleType=*/2, /*sampleLink=*/0));
    Preset p; p.zones.push_back(makeZone(0));
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    check(importPresetIntoSampler("", font, 0, samp, opts, result, err), "imports", err);
    SamplerZoneInfo info;
    check(sampler_get_zone(samp, 0, info), "zone reads back");
    check(!info.stereo, "falls back to mono when the pair's lengths differ");
    check(info.numFrames == 20, "plays its own (shorter) length, not the mismatched partner's");
}

void test_every_parameter_roundtrip() {
    std::printf("-- every zone parameter arrives via the read-back API --\n");
    const int frames = 200;
    SoundFont font;
    font.samples.push_back(makeMonoSample("Kitchen Sink", frames));

    Zone z = makeZone(0, /*loKey=*/36, /*hiKey=*/72, /*loVel=*/10, /*hiVel=*/100, /*rootKey=*/64);
    z.coarseTune = 12; z.fineTune = -25; z.scaleTuning = 50;
    z.exclusiveClass = 3;
    z.pan = 0.3f; z.attenuationDb = 2.5f;
    z.loop = true; z.loopUntilRelease = false;
    z.loopStart = 20; z.loopEnd = 150;
    z.delayVol = 0.01f; z.attackVol = 0.05f; z.holdVol = 0.02f;
    z.decayVol = 0.3f; z.sustainVol = 0.7f; z.releaseVol = 0.4f;
    z.delayMod = 0.02f; z.attackMod = 0.06f; z.holdMod = 0.03f;
    z.decayMod = 0.2f; z.sustainMod = 0.5f; z.releaseMod = 0.15f;
    z.modEnvToPitchCents = 200.f; z.modEnvToFilterCents = -300.f;
    z.cutoffHz = 3000.f; z.resonanceDb = 6.f;

    Preset p; p.zones.push_back(z);
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    check(importPresetIntoSampler("", font, 0, samp, opts, result, err), "imports", err);
    check(result.zonesLoaded == 1, "the zone loads");

    SamplerZoneInfo info;
    check(sampler_get_zone(samp, 0, info), "zone reads back");

    check(info.loKey == 36 && info.hiKey == 72, "key range");
    check(info.loVel == 10 && info.hiVel == 100, "velocity range");
    check(info.rootKey == 64, "root key");
    check(info.coarseTune == 12, "coarse tune");
    check(info.fineTune == -25, "fine tune");
    check(info.scaleTuning == 50, "scale tuning");
    check(info.exclusiveClass == 3, "exclusive class");
    checkNear(info.pan, 0.3f, 1e-4f, "pan");
    checkNear(info.attenuationDb, 2.5f, 1e-4f, "attenuation");
    check(info.loop, "loop enabled");
    check(info.loopStart == 20 && info.loopEnd == 150, "loop points");
    checkNear(info.modEnvToPitchCents, 200.f, 1e-3f, "mod-env -> pitch route");
    checkNear(info.modEnvToFilterCents, -300.f, 1e-3f, "mod-env -> filter route");
    checkNear(info.cutoffHz, 3000.f, 1e-3f, "filter cutoff");
    checkNear(info.resonanceDb, 6.f, 1e-3f, "filter resonance");

    check(info.ampEnv.enabled != 0, "amp envelope is enabled");
    checkNear(info.ampEnv.delay, 0.01f, 1e-4f, "amp env delay");
    checkNear(info.ampEnv.attack, 0.05f, 1e-4f, "amp env attack");
    checkNear(info.ampEnv.hold, 0.02f, 1e-4f, "amp env hold");
    checkNear(info.ampEnv.decay, 0.3f, 1e-4f, "amp env decay");
    checkNear(info.ampEnv.sustain, 0.7f, 1e-4f, "amp env sustain");
    checkNear(info.ampEnv.release, 0.4f, 1e-4f, "amp env release");

    check(info.modEnv.enabled != 0, "mod envelope is enabled");
    checkNear(info.modEnv.delay, 0.02f, 1e-4f, "mod env delay");
    checkNear(info.modEnv.attack, 0.06f, 1e-4f, "mod env attack");
    checkNear(info.modEnv.hold, 0.03f, 1e-4f, "mod env hold");
    checkNear(info.modEnv.decay, 0.2f, 1e-4f, "mod env decay");
    checkNear(info.modEnv.sustain, 0.5f, 1e-4f, "mod env sustain");
    checkNear(info.modEnv.release, 0.15f, 1e-4f, "mod env release");
}

void test_zone_cap() {
    std::printf("-- the zone cap stops loading and reports the rest as skipped --\n");
    SoundFont font;
    for (int i = 0; i < 5; ++i) font.samples.push_back(makeMonoSample("s" + std::to_string(i), 10));
    Preset p;
    for (int i = 0; i < 5; ++i) p.zones.push_back(makeZone(i, i * 20, i * 20 + 19));
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; opts.maxZones = 3;
    ImportResult result; std::string err;
    check(importPresetIntoSampler("", font, 0, samp, opts, result, err), "capped import succeeds", err);
    check(result.zonesLoaded == 3, "only the cap's worth of zones load", std::to_string(result.zonesLoaded));
    check(result.zonesSkipped == 2, "the rest are reported skipped", std::to_string(result.zonesSkipped));
    check(sampler_zone_count(samp) == 3, "the sampler holds exactly the capped count");
}

void test_missing_sample_skipped() {
    std::printf("-- a zone referencing a missing sample is skipped, not fatal --\n");
    SoundFont font;
    font.samples.push_back(makeMonoSample("ok", 10));
    Preset p;
    p.zones.push_back(makeZone(0, 0, 59));
    p.zones.push_back(makeZone(-1, 60, 127));   // as the reader itself would mark a bad sampleID
    font.presets.push_back(std::move(p));

    Sampler samp; ImportOptions opts; ImportResult result; std::string err;
    check(importPresetIntoSampler("", font, 0, samp, opts, result, err),
          "a preset with one bad zone still imports (no crash)", err);
    check(result.zonesLoaded == 1, "the valid zone loads");
    check(result.zonesSkipped == 1, "the missing-sample zone is skipped");
    check(sampler_zone_count(samp) == 1, "the sampler holds only the valid zone");
}

void test_collapse_velocity_layers() {
    std::printf("-- collapseVelocityLayers drops a fully-covered round-robin layer --\n");
    auto buildFont = []() {
        SoundFont font;
        font.samples.push_back(makeMonoSample("full", 10));
        font.samples.push_back(makeMonoSample("layer", 10));
        Preset p;
        p.zones.push_back(makeZone(0, 0, 127, 0, 127));   // full velocity, key 0..127
        p.zones.push_back(makeZone(1, 0, 127, 0, 63));    // subset velocity, SAME key range
        font.presets.push_back(std::move(p));
        return font;
    };
    {
        SoundFont font = buildFont();
        Sampler samp; ImportOptions opts; opts.collapseVelocityLayers = false;
        ImportResult result; std::string err;
        check(importPresetIntoSampler("", font, 0, samp, opts, result, err), "off: imports", err);
        check(result.zonesLoaded == 2, "off (the default): both velocity layers load");
    }
    {
        SoundFont font = buildFont();
        Sampler samp; ImportOptions opts; opts.collapseVelocityLayers = true;
        ImportResult result; std::string err;
        check(importPresetIntoSampler("", font, 0, samp, opts, result, err), "on: imports", err);
        check(result.zonesLoaded == 1, "on: the fully-covered layer is dropped");
        check(result.zonesSkipped == 1, "on: reported as skipped");
    }
}

} // namespace

int main() {
    test_real_file_round_trip();
    test_interning_and_ram();
    test_stereo_pairing();
    test_stereo_broken_link_out_of_range();
    test_stereo_broken_link_same_channel();
    test_stereo_broken_link_length_mismatch();
    test_every_parameter_roundtrip();
    test_zone_cap();
    test_missing_sample_skipped();
    test_collapse_velocity_layers();

    std::printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail ? 1 : 0;
}
