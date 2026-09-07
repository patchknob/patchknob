//----------------------------------------------------------------------------
//  Headless test for the SoundFont reader.
//
//  There is no .sf2 on a build machine to test against, so the test BUILDS one
//  byte by byte.  That is not a workaround -- it is the only way to pin the
//  cases that actually break importers, because a hand-built file can state
//  exactly one tricky thing at a time:
//
//    * preset generators are OFFSETS added onto instrument absolutes ...
//    * ... except keyRange/velRange/sampleID, which must NOT be summed
//      (summing them is the classic bug: key ranges walk past 127 and the
//      zone goes silently silent),
//    * a global zone supplies defaults to its siblings,
//    * sustainVolEnv is ATTENUATION in centibels, not a level,
//    * initialFilterFc defaults to 13500 (wide open), not 0 (DC / muffled).
//
//  Build: g++ -std=c++17 -O2 -I.. sf2_reader_test.cpp sf2_reader.cpp -o t && ./t
//----------------------------------------------------------------------------
#include "sf2_reader.h"
#include "sf2_writer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PatchKnob::engine::sf2;

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

//---------------------------------------------------------------- builder ---
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

//! chunk("smpl", payload) -> id + size + payload (+ pad byte when odd)
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

//! A soundfont with ONE preset over ONE instrument that has:
//!   - a GLOBAL instrument zone setting releaseVolEnv (defaults for siblings),
//!   - zone A: keys 0..59, its own sustain attenuation, no release of its own,
//!   - zone B: keys 60..127, its own release overriding the global,
//! and a PRESET zone whose keyRange (40..80) must NOT be summed onto the
//! instrument ranges, but whose attackVolEnv offset MUST be added.
std::vector<uint8_t> buildFont() {
    // ---- sdta: 32 frames of ramp, then the spec's 46 zero frames ----------
    Buf smpl;
    for (int i = 0; i < 32; ++i) smpl.u16((uint16_t)(int16_t)(i * 512));
    for (int i = 0; i < 46; ++i) smpl.u16(0);
    const std::vector<uint8_t> sdta = listChunk("sdta", chunk("smpl", smpl.b));

    // ---- pdta ------------------------------------------------------------
    Buf phdr;
    phdr.name20("TestPreset"); phdr.u16(7); phdr.u16(2);  // program 7, bank 2
    phdr.u16(0); phdr.u32(0); phdr.u32(0); phdr.u32(0);
    phdr.name20("EOP");        phdr.u16(0); phdr.u16(0);
    phdr.u16(1); phdr.u32(0); phdr.u32(0); phdr.u32(0);

    Buf pbag; pbag.u16(0); pbag.u16(0);      // preset zone 0 -> pgen[0..]
                pbag.u16(3); pbag.u16(0);    // terminal

    Buf pgen;
    gen(pgen, GEN_keyRange,      (int16_t)((80 << 8) | 40));  // must NOT be summed
    gen(pgen, GEN_attackVolEnv,  1200);                       // +1 octave => x2 time
    gen(pgen, GEN_instrument,    0);                          // terminal
    gen(pgen, 0, 0);                                          // terminal record

    Buf inst; inst.name20("TestInst"); inst.u16(0);
              inst.name20("EOI");      inst.u16(4);

    Buf ibag; ibag.u16(0); ibag.u16(0);   // global zone   -> igen[0..0]
              ibag.u16(1); ibag.u16(0);   // zone A        -> igen[1..4]
              ibag.u16(5); ibag.u16(0);   // zone B        -> igen[5..8]
              ibag.u16(9); ibag.u16(0);   // terminal

    Buf igen;
    // global: release 2 s (1200*log2(2) = 1200)
    gen(igen, GEN_releaseVolEnv, 1200);
    // zone A: keys 0..59, sustain -12 dB (120 centibels), sample 0
    gen(igen, GEN_keyRange,       (int16_t)((59 << 8) | 0));
    gen(igen, GEN_sustainVolEnv,  120);
    gen(igen, GEN_attackVolEnv,   0);        // 1 s; preset adds +1200 => 2 s
    gen(igen, GEN_sampleID,       0);
    // zone B: keys 60..127, own release 0.5 s (-1200), sample 0
    gen(igen, GEN_keyRange,       (int16_t)((127 << 8) | 60));
    gen(igen, GEN_releaseVolEnv, -1200);
    gen(igen, GEN_overridingRootKey, 69);
    gen(igen, GEN_sampleID,       0);
    gen(igen, 0, 0);                          // terminal

    Buf shdr;
    shdr.name20("Ramp");
    shdr.u32(0); shdr.u32(32); shdr.u32(8); shdr.u32(24);   // start/end/loop
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

} // namespace

int main() {
    const std::vector<uint8_t> raw = buildFont();
    const std::string path = "/tmp/pk_sf2_reader_test.sf2";
    { FILE* f = std::fopen(path.c_str(), "wb");
      if (!f) { std::printf("cannot write %s\n", path.c_str()); return 2; }
      std::fwrite(raw.data(), 1, raw.size(), f); std::fclose(f); }

    check(looksLikeSoundFont(path), "the file is recognised as RIFF/sfbk");

    SoundFont font; std::string err;
    const bool ok = read(path, font, err, true);
    check(ok, "the soundfont parses", err);
    if (!ok) return 1;

    check(font.name == "Test Bank", "INFO/INAM is read", font.name);
    check(font.presets.size() == 1, "one preset");
    const Preset& p = font.presets[0];
    check(p.bank == 2 && p.program == 7, "bank and program survive");
    check(p.zones.size() == 2, "the instrument's two zones resolve (the global zone is not one)");

    const Zone* lo = nullptr; const Zone* hi = nullptr;
    for (const Zone& z : p.zones) { if (z.hiKey < 60) lo = &z; else hi = &z; }
    check(lo && hi, "one zone below middle C, one above");
    if (!lo || !hi) return 1;

    // The preset's keyRange (40..80) must NOT be added to the instrument's.
    // A preset keyRange INTERSECTS the instrument's; it neither sums onto it
    // (which would push zone B to 100..207) nor replaces it (which would give
    // both zones 40..80 and lose the split at middle C entirely).
    check(lo->loKey == 40 && lo->hiKey == 59, "preset keyRange INTERSECTS zone A (40..80 over 0..59)",
          std::to_string(lo->loKey) + ".." + std::to_string(lo->hiKey));
    check(hi->loKey == 60 && hi->hiKey == 80, "preset keyRange INTERSECTS zone B (40..80 over 60..127)",
          std::to_string(hi->loKey) + ".." + std::to_string(hi->hiKey));

    // ... but attackVolEnv IS additive: 0 tc (1 s) + 1200 tc => 2 s.
    checkNear(lo->attackVol, 2.f, 0.01f, "preset attackVolEnv offset WAS added (1 s + 1 oct = 2 s)");

    // The global instrument zone supplies the release zone A never set ...
    checkNear(lo->releaseVol, 2.f, 0.01f, "zone A inherits release from the global zone");
    // ... and zone B's own value overrides it.
    checkNear(hi->releaseVol, 0.5f, 0.01f, "zone B overrides the global release");

    // sustainVolEnv is ATTENUATION in centibels: 120 cB = -12 dB = 0.2512 linear.
    checkNear(lo->sustainVol, 0.2512f, 0.005f, "sustain read as attenuation, not as a level");
    checkNear(hi->sustainVol, 1.f, 0.001f, "an unset sustain is full level");

    // initialFilterFc defaults to 13500 (open) -> no filter, NOT a DC cutoff.
    check(lo->cutoffHz == 0.f, "an unset filter cutoff means OPEN, not muffled");

    check(hi->rootKey == 69, "overridingRootKey applies", std::to_string(hi->rootKey));
    check(lo->rootKey == 60, "without an override the sample's own key is used");

    check(font.samples.size() == 1, "the terminal EOS sample record is dropped");
    check(font.samples[0].pcm.size() == 32, "sample PCM decodes to its frame count");
    checkNear(font.samples[0].pcm[1], 512.f / 32768.f, 1e-6f, "16-bit PCM scales to -1..1");

    // headers-only must still resolve zones, and cost no PCM.
    SoundFont meta;
    check(read(path, meta, err, false), "headers-only read succeeds", err);
    check(meta.presets.size() == 1 && meta.presets[0].zones.size() == 2,
          "headers-only still resolves zones (browser can expand without loading)");
    check(meta.samples[0].pcm.empty(), "headers-only loaded NO sample data");

    // ---- 24-bit export (sm24) ---------------------------------------------
    //  WriteOptions::write24Bit was declared and documented but never
    //  implemented: the writer always emitted 16-bit, so the option silently
    //  did nothing. Now it writes the sm24 chunk SF2.04 defines.
    {
        std::printf("\n--- 24-bit export ---\n");
        SoundFont f;
        Sample sm; sm.name = "sine";
        sm.sampleRate = 48000; sm.originalKey = 60; sm.pitchCorrection = 0;
        sm.start = 0; sm.loopStart = 10; sm.loopEnd = 900; sm.end = 1000;
        sm.sampleType = 1;
        sm.pcm.resize(1000);
        for (size_t i = 0; i < sm.pcm.size(); ++i)
            sm.pcm[i] = (float)std::sin((double)i * 0.01) * 0.5f;
        f.samples.push_back(sm);
        Preset p; p.name = "pre"; p.bank = 0; p.program = 0;
        Zone z; z.sampleIndex = 0; p.zones.push_back(z);
        f.presets.push_back(p);

        double err16 = 0.0, err24 = 0.0;
        for (int pass = 0; pass < 2; ++pass) {
            const bool w24 = (pass == 1);
            WriteOptions o; o.write24Bit = w24; o.standardOnly = true;
            const std::string path = std::string(w24 ? "sf2_w24.sf2" : "sf2_w16.sf2");
            std::string werr;
            const bool wrote = write(path, f, o, PrivateState(), werr);
            check(wrote, w24 ? "24-bit file writes" : "16-bit file writes", werr);
            if (!wrote) continue;

            //  Is the chunk physically there?
            std::string blob;
            { FILE* fp = std::fopen(path.c_str(), "rb");
              if (fp) { char b[4096]; size_t n;
                        while ((n = std::fread(b, 1, sizeof b, fp)) > 0) blob.append(b, n);
                        std::fclose(fp); } }
            check((blob.find("sm24") != std::string::npos) == w24,
                  w24 ? "the 24-bit file carries an sm24 chunk"
                      : "the 16-bit file carries no sm24 chunk");

            SoundFont g; std::string rerr;
            const bool reread = read(path, g, rerr, /*loadPcm=*/true);
            check(reread, "the export re-reads", rerr);
            if (reread && !g.samples.empty()) {
                double worst = 0.0;
                const std::vector<float>& got = g.samples[0].pcm;
                for (size_t i = 0; i < sm.pcm.size() && i < got.size(); ++i) {
                    const double d = std::fabs((double)got[i] - (double)sm.pcm[i]);
                    if (d > worst) worst = d;
                }
                (w24 ? err24 : err16) = worst;
            }
            std::remove(path.c_str());
        }
        std::printf("      worst error: 16-bit %.3e, 24-bit %.3e\n", err16, err24);
        check(err16 > 1e-6 && err16 < 4e-4, "the 16-bit path has 16-bit error");
        check(err24 > 0.0 && err24 < err16 / 100.0,
              "24-bit export is at least 100x more accurate than 16-bit");
    }

    std::printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail ? 1 : 0;
}
