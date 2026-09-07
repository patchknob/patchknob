//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_writer.cpp -- see sf2_writer.h.
//
//  The inverse of sf2_reader.cpp, and it has to be exactly that: anything the
//  reader converts on the way in has to be converted back on the way out, or a
//  patch that round-trips through us drifts.  Seconds -> timecents, sustain
//  LEVEL -> centibel ATTENUATION, hertz -> absolute cents, -1..+1 pan -> tenths
//  of a percent.  The round-trip test pins every one of them.
//----------------------------------------------------------------------------
#include "sf2_writer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>

namespace PatchKnob { namespace engine { namespace sf2 {
namespace {

//! 64-bit-safe seek/tell -- std::fseek/ftell take `long`, 32 bits on Windows
//! (LLP64), so walking the chunks of a bank past 2 GB would truncate there.
//! (Same helpers as sf2_reader.cpp; one-liners, duplicated rather than
//! exported from the reader's anonymous namespace.)
int seek64w(FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
int64_t tell64w(FILE* f) {
#if defined(_WIN32)
    return (int64_t)_ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

struct Out {
    std::vector<uint8_t> b;
    void u8 (uint8_t v)  { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
    void s16(int16_t v)  { u16((uint16_t)v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8*i)) & 0xFF); }
    void tag(const char* t) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)t[i]); }
    //! SF2 name fields are FIXED 20 bytes, zero-padded, and the spec wants the
    //! last byte to stay zero -- so a 20-character name is truncated to 19.
    void name20(const std::string& s) {
        for (int i = 0; i < 20; ++i) b.push_back(i < (int)s.size() && i < 19 ? (uint8_t)s[i] : 0);
    }
    void raw(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); }
    size_t size() const { return b.size(); }
};

std::vector<uint8_t> chunk(const char* id, const std::vector<uint8_t>& payload) {
    Out c; c.tag(id); c.u32((uint32_t)payload.size()); c.raw(payload);
    if (payload.size() & 1u) c.u8(0);        // RIFF pads odd chunks
    return c.b;
}
std::vector<uint8_t> listOf(const char* type, const std::vector<uint8_t>& payload) {
    Out inner; inner.tag(type); inner.raw(payload);
    return chunk("LIST", inner.b);
}

// ---- unit conversions: the exact inverses of the reader's ------------------
int16_t secToTimecents(float sec) {
    if (sec <= 0.0005f) return -12000;                    // the spec's "instant"
    const double tc = 1200.0 * std::log2((double)sec);
    return (int16_t)std::max(-12000.0, std::min(8000.0, std::round(tc)));
}
//! sustain is a LEVEL going out to a centibel ATTENUATION: 1.0 -> 0, 0.25 -> 120.
int16_t levelToCentibels(float level) {
    if (level >= 0.9999f) return 0;
    if (level <= 0.0f)    return 1440;                    // spec's floor (-144 dB)
    const double cb = -200.0 * std::log10((double)level);
    return (int16_t)std::max(0.0, std::min(1440.0, std::round(cb)));
}
int16_t hzToAbsCents(float hz) {
    if (hz <= 0.f) return 13500;                          // 0 == no filter == open
    const double c = 1200.0 * std::log2((double)hz / 8.176);
    return (int16_t)std::max(1500.0, std::min(13500.0, std::round(c)));
}
void gen(Out& o, uint16_t op, int16_t amount) { o.u16(op); o.s16(amount); }
uint16_t packRange(int lo, int hi) {
    lo = std::max(0, std::min(127, lo)); hi = std::max(0, std::min(127, hi));
    return (uint16_t)(((hi & 0xFF) << 8) | (lo & 0xFF));
}

//! Emit one instrument zone's generators.
//!
//! ORDER IS NORMATIVE (spec 8.1.2), not cosmetic: keyRange must come first,
//! velRange second if present, and sampleID must be LAST -- it is the terminal
//! generator that makes the zone real.  A reader that stops at sampleID (ours
//! does, and so does FluidSynth) silently drops anything written after it.
void writeZoneGens(Out& igen, const Zone& z, int sampleIndex, const Sample& s) {
    gen(igen, GEN_keyRange, (int16_t)packRange(z.loKey, z.hiKey));
    if (z.loVel != 0 || z.hiVel != 127)
        gen(igen, GEN_velRange, (int16_t)packRange(z.loVel, z.hiVel));

    if (z.startOffset)          gen(igen, GEN_startAddrsOffset, (int16_t)z.startOffset);
    if (z.attenuationDb != 0.f) gen(igen, GEN_initialAttenuation, (int16_t)std::lround(z.attenuationDb * 10.f));
    if (z.pan != 0.f)           gen(igen, GEN_pan, (int16_t)std::lround(z.pan * 500.f));

    if (z.cutoffHz > 0.f)       gen(igen, GEN_initialFilterFc, hzToAbsCents(z.cutoffHz));
    if (z.resonanceDb != 0.f)   gen(igen, GEN_initialFilterQ, (int16_t)std::lround(z.resonanceDb * 10.f));

    if (z.delayVol   > 0.f) gen(igen, GEN_delayVolEnv,   secToTimecents(z.delayVol));
    if (z.attackVol  > 0.f) gen(igen, GEN_attackVolEnv,  secToTimecents(z.attackVol));
    if (z.holdVol    > 0.f) gen(igen, GEN_holdVolEnv,    secToTimecents(z.holdVol));
    if (z.decayVol   > 0.f) gen(igen, GEN_decayVolEnv,   secToTimecents(z.decayVol));
    if (z.sustainVol < 1.f) gen(igen, GEN_sustainVolEnv, levelToCentibels(z.sustainVol));
    if (z.releaseVol > 0.f) gen(igen, GEN_releaseVolEnv, secToTimecents(z.releaseVol));

    if (z.delayMod   > 0.f) gen(igen, GEN_delayModEnv,   secToTimecents(z.delayMod));
    if (z.attackMod  > 0.f) gen(igen, GEN_attackModEnv,  secToTimecents(z.attackMod));
    if (z.holdMod    > 0.f) gen(igen, GEN_holdModEnv,    secToTimecents(z.holdMod));
    if (z.decayMod   > 0.f) gen(igen, GEN_decayModEnv,   secToTimecents(z.decayMod));
    if (z.sustainMod < 1.f) gen(igen, GEN_sustainModEnv, (int16_t)std::lround((1.f - z.sustainMod) * 1000.f));
    if (z.releaseMod > 0.f) gen(igen, GEN_releaseModEnv, secToTimecents(z.releaseMod));
    if (z.modEnvToPitchCents  != 0.f) gen(igen, GEN_modEnvToPitch,    (int16_t)std::lround(z.modEnvToPitchCents));
    if (z.modEnvToFilterCents != 0.f) gen(igen, GEN_modEnvToFilterFc, (int16_t)std::lround(z.modEnvToFilterCents));

    if (z.scaleTuning != 100) gen(igen, GEN_scaleTuning, (int16_t)z.scaleTuning);
    if (z.coarseTune  != 0)   gen(igen, GEN_coarseTune,  (int16_t)z.coarseTune);
    // The sample's own pitchCorrection is folded into fineTune on read, so take
    // it back out here -- otherwise every round trip adds it again.
    const int fine = z.fineTune - (int)s.pitchCorrection;
    if (fine != 0)            gen(igen, GEN_fineTune, (int16_t)fine);

    if (z.exclusiveClass != 0) gen(igen, GEN_exclusiveClass, (int16_t)z.exclusiveClass);
    if (z.rootKey != (int)s.originalKey)
        gen(igen, GEN_overridingRootKey, (int16_t)z.rootKey);
    gen(igen, GEN_sampleModes, (int16_t)(z.loopUntilRelease ? 3 : (z.loop ? 1 : 0)));

    gen(igen, GEN_sampleID, (int16_t)sampleIndex);   // TERMINAL -- must be last
}

} // namespace

//----------------------------------------------------------------------------
bool write(const std::string& path, const SoundFont& font,
           const WriteOptions& opts, const PrivateState& priv, std::string& error) {
    error.clear();
    if (font.presets.empty()) { error = "nothing to write (no presets)"; return false; }

    // ---- sdta layout: every sample, each followed by the 46 zero frames the
    // spec requires so an interpolating player cannot read into its neighbour.
    // Only the LAYOUT is computed here -- the pool itself is NOT materialised.
    // It is by far the biggest part of the file (an 85 MB patch was passing
    // through four nested whole-buffer copies on its way out, ~4x the pool in
    // RAM and hundreds of MB of memcpy), so the PCM is converted and streamed
    // straight to the file below instead.
    std::vector<uint32_t> sampStart(font.samples.size()), sampEnd(font.samples.size());
    uint64_t poolFrames = 0;
    for (size_t i = 0; i < font.samples.size(); ++i) {
        sampStart[i] = (uint32_t)poolFrames;
        poolFrames  += font.samples[i].pcm.size();
        sampEnd[i]   = (uint32_t)poolFrames;
        poolFrames  += 46;
    }
    const uint64_t smplBytes = poolFrames * 2;
    //  sm24 (SF2.04) holds the LOW byte of each 24-bit sample, one byte per
    //  frame, alongside the high 16 bits already in smpl.  Readers that predate
    //  2.04 skip it and play the 16-bit data on its own, so a 24-bit file stays
    //  playable everywhere.  The chunk is padded to an even length like any
    //  other RIFF chunk.
    const bool     want24    = opts.write24Bit;
    const uint64_t sm24Bytes = want24 ? poolFrames : 0;
    const uint64_t sm24Pad   = (sm24Bytes & 1u) ? 1u : 0u;

    // ---- pdta -------------------------------------------------------------
    Out phdr, pbag, pgen, inst, ibag, igen, shdr;
    uint16_t pbagCount = 0, pgenCount = 0, ibagCount = 0, igenCount = 0;

    for (size_t pi = 0; pi < font.presets.size(); ++pi) {
        const Preset& p = font.presets[pi];
        phdr.name20(p.name.empty() ? ("Preset " + std::to_string(pi)) : p.name);
        phdr.u16((uint16_t)p.program); phdr.u16((uint16_t)p.bank);
        phdr.u16(pbagCount);
        phdr.u32(0); phdr.u32(0); phdr.u32(0);      // library/genre/morphology

        // One preset zone pointing at one instrument: our zones carry absolute
        // values, so there is nothing for a preset-level OFFSET layer to add.
        pbag.u16(pgenCount); pbag.u16(0); ++pbagCount;
        gen(pgen, GEN_instrument, (int16_t)pi); pgenCount += 1;

        inst.name20(p.name.empty() ? ("Inst " + std::to_string(pi)) : p.name);
        inst.u16(ibagCount);

        for (const Zone& z : p.zones) {
            if (z.sampleIndex < 0 || z.sampleIndex >= (int)font.samples.size()) continue;
            ibag.u16(igenCount); ibag.u16(0); ++ibagCount;
            const size_t before = igen.size();
            writeZoneGens(igen, z, z.sampleIndex, font.samples[(size_t)z.sampleIndex]);
            igenCount += (uint16_t)((igen.size() - before) / 4);
        }
    }
    // Terminal records. Every one of these is mandatory: a reader computes each
    // record's extent from the NEXT record's index, so without them the last
    // preset, instrument and zone of the file are unreadable.
    phdr.name20("EOP"); phdr.u16(0); phdr.u16(0); phdr.u16(pbagCount);
    phdr.u32(0); phdr.u32(0); phdr.u32(0);
    pbag.u16(pgenCount); pbag.u16(0);
    gen(pgen, 0, 0);
    inst.name20("EOI"); inst.u16(ibagCount);
    ibag.u16(igenCount); ibag.u16(0);
    gen(igen, 0, 0);

    for (size_t i = 0; i < font.samples.size(); ++i) {
        const Sample& s = font.samples[i];
        shdr.name20(s.name.empty() ? ("Sample " + std::to_string(i)) : s.name);
        shdr.u32(sampStart[i]); shdr.u32(sampEnd[i]);
        const uint32_t len = sampEnd[i] - sampStart[i];
        uint32_t ls = s.loopStart, le = s.loopEnd;
        // Loop points are stored ABSOLUTE in the pool, and must sit inside the
        // sample or a player will read a neighbour's audio.
        if (le <= ls || le > len) { ls = 0; le = len; }
        shdr.u32(sampStart[i] + ls); shdr.u32(sampStart[i] + le);
        shdr.u32(s.sampleRate ? s.sampleRate : 44100);
        shdr.u8(s.originalKey); shdr.u8((uint8_t)s.pitchCorrection);
        shdr.u16(0);            // sampleLink: we always write mono samples
        shdr.u16(1);            // monoSample
    }
    shdr.name20("EOS");
    shdr.u32(0); shdr.u32(0); shdr.u32(0); shdr.u32(0);
    shdr.u32(0); shdr.u8(0); shdr.u8(0); shdr.u16(0); shdr.u16(0);

    Out pd;
    pd.raw(chunk("phdr", phdr.b)); pd.raw(chunk("pbag", pbag.b));
    pd.raw(chunk("pmod", {}));     pd.raw(chunk("pgen", pgen.b));
    pd.raw(chunk("inst", inst.b)); pd.raw(chunk("ibag", ibag.b));
    pd.raw(chunk("imod", {}));     pd.raw(chunk("igen", igen.b));
    pd.raw(chunk("shdr", shdr.b));

    Out info;
    info.raw(chunk("ifil", {2, 0, 4, 0}));                    // SF 2.04
    { Out n; for (char c : std::string("EMU8000")) n.u8((uint8_t)c); n.u8(0); if (n.size() & 1) n.u8(0);
      info.raw(chunk("isng", n.b)); }
    { Out n; n.name20(opts.bankName); info.raw(chunk("INAM", n.b)); }
    { Out n; for (char c : std::string("PatchKnob")) n.u8((uint8_t)c); n.u8(0); if (n.size() & 1) n.u8(0);
      info.raw(chunk("ISFT", n.b)); }
    if (!opts.comment.empty()) {
        Out n; for (char c : opts.comment) n.u8((uint8_t)c); n.u8(0); if (n.size() & 1) n.u8(0);
        info.raw(chunk("ICMT", n.b));
    }

    // ---- assemble small parts in memory, STREAM the pool ------------------
    // INFO and pdta are a few KB..MB and are built as before; sdta's sample
    // pool is streamed sample-by-sample so the writer's memory stays O(pdta),
    // not O(file).  Every size is computed up front because RIFF sizes precede
    // their content.
    const std::vector<uint8_t> infoList = listOf("INFO", info.b);
    const std::vector<uint8_t> pdtaList = listOf("pdta", pd.b);
    const std::vector<uint8_t> pkstChunk =
        (!opts.standardOnly && !priv.bytes.empty()) ? chunk("PKST", priv.bytes)
                                                    : std::vector<uint8_t>{};

    // LIST("sdta", chunk("smpl", pool)): 12 bytes of LIST header + type, then
    // 8 bytes of smpl header, then the pool (always even, so never padded).
    const uint64_t sdtaListTotal = 12 + 8 + smplBytes
                                 + (want24 ? (8 + sm24Bytes + sm24Pad) : 0);
    const uint64_t riffPayload = 4 /*sfbk*/ + infoList.size() + sdtaListTotal +
                                 pdtaList.size() + pkstChunk.size();
    // RIFF stores sizes as u32.  The old whole-file-in-RAM writer silently
    // truncated the size field past 4 GB and wrote a corrupt bank; refuse
    // instead -- SF2 simply cannot hold it.
    if (riffPayload > 0xFFFFFFFFull) {
        error = "sample pool too large for a RIFF file (4 GB limit)";
        return false;
    }

    // Write to a temporary and rename, so a failure part-way cannot destroy an
    // existing soundfont the user is overwriting.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { error = "cannot create " + tmp; return false; }

    bool ok = true;
    auto put = [&](const void* p, size_t n) {
        if (ok && n && std::fwrite(p, 1, n, f) != n) ok = false;
    };
    auto putVec = [&](const std::vector<uint8_t>& v) { put(v.data(), v.size()); };
    auto putU32 = [&](uint32_t v) {
        const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        put(b, 4);
    };

    put("RIFF", 4); putU32((uint32_t)riffPayload); put("sfbk", 4);
    putVec(infoList);
    put("LIST", 4);
    putU32((uint32_t)(4 + 8 + smplBytes + (want24 ? (8 + sm24Bytes + sm24Pad) : 0)));
    put("sdta", 4);
    put("smpl", 4); putU32((uint32_t)smplBytes);

    // The pool: convert each sample float -> little-endian s16 through one
    // reused staging buffer, then the spec's 46 zero frames.
    //  Quantise ONCE, to 24 bits, and split: the high 16 go to smpl, the low
    //  byte to sm24.  Deriving the two halves from one rounding is what makes
    //  the pair reconstruct the original sample exactly; rounding separately
    //  for each chunk would leave the low byte disagreeing with its own high
    //  word by up to one LSB.
    std::vector<uint8_t> low;                       // sm24 payload, when wanted
    if (want24) low.reserve((size_t)poolFrames);
    {
        std::vector<uint8_t> stage;
        const uint8_t zeros[46 * 2] = { 0 };
        for (const Sample& s : font.samples) {
            if (!ok) break;
            stage.resize(s.pcm.size() * 2);
            for (size_t k = 0; k < s.pcm.size(); ++k) {
                const float c = std::max(-1.f, std::min(1.f, s.pcm[k]));
                if (want24) {
                    //  24-bit signed range; the high word is an arithmetic
                    //  shift so negative samples keep their sign.
                    int32_t q = (int32_t)std::lround((double)c * 8388607.0);
                    if (q >  8388607) q =  8388607;
                    if (q < -8388608) q = -8388608;
                    const int16_t hi = (int16_t)(q >> 8);
                    stage[k * 2]     = (uint8_t)((uint16_t)hi & 0xFF);
                    stage[k * 2 + 1] = (uint8_t)((uint16_t)hi >> 8);
                    low.push_back((uint8_t)(q & 0xFF));
                } else {
                    const int16_t v = (int16_t)std::lround(c * 32767.f);
                    stage[k * 2]     = (uint8_t)((uint16_t)v & 0xFF);
                    stage[k * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
                }
            }
            putVec(stage);
            put(zeros, sizeof zeros);
            if (want24) low.insert(low.end(), 46, 0);   // the spec's 46 zero frames
        }
    }
    if (want24 && ok) {
        put("sm24", 4); putU32((uint32_t)sm24Bytes);
        putVec(low);
        if (sm24Pad) { const uint8_t z = 0; put(&z, 1); }
    }

    putVec(pdtaList);
    // Our own state rides in a private chunk. The spec requires readers to skip
    // chunks they do not know, so every other player sees a normal soundfont --
    // and we get the FX chain and exact envelope curves back on re-import.
    putVec(pkstChunk);

    if (std::fclose(f) != 0) ok = false;
    if (!ok) { std::remove(tmp.c_str()); error = "short write"; return false; }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); error = "cannot rename into place"; return false; }
    return true;
}

//----------------------------------------------------------------------------
bool readPrivateState(const std::string& path, PrivateState& out) {
    out.bytes.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const int64_t len = tell64w(f);
    bool found = false;
    int64_t pos = 12;
    while (pos + 8 <= len) {
        char id[4]; uint8_t szb[4];
        if (seek64w(f, (uint64_t)pos) || std::fread(id, 1, 4, f) != 4 ||
            std::fread(szb, 1, 4, f) != 4) break;
        const uint32_t sz = (uint32_t)szb[0] | ((uint32_t)szb[1]<<8) | ((uint32_t)szb[2]<<16) | ((uint32_t)szb[3]<<24);
        if (!std::memcmp(id, "PKST", 4)) {
            out.bytes.resize(sz);
            found = sz == 0 || std::fread(out.bytes.data(), 1, sz, f) == sz;
            break;
        }
        pos += 8 + sz + (sz & 1u);
    }
    std::fclose(f);
    return found;
}

//----------------------------------------------------------------------------
DahdsrFit fitEnvelopeToDahdsr(const unsigned short* xs, const unsigned short* ys,
                              const int* flags, int count, float secondsPerUnit) {
    DahdsrFit fit;
    if (!xs || !ys || count <= 1) return fit;

    const float toSec = secondsPerUnit / 65535.f;
    // Sustain is the level at the sustain point, or the last point's level.
    int sustainIdx = count - 1;
    if (flags) for (int i = 0; i < count; ++i) if (flags[i]) { sustainIdx = i; break; }
    fit.sustain = (float)ys[sustainIdx] / 65535.f;

    // Peak before sustain = the attack target.
    int peakIdx = 0; unsigned short peak = 0;
    for (int i = 0; i <= sustainIdx; ++i) if (ys[i] >= peak) { peak = ys[i]; peakIdx = i; }

    // Delay = time spent at (near) zero before the level first rises.
    int riseIdx = 0;
    while (riseIdx < peakIdx && ys[riseIdx] <= peak / 100) ++riseIdx;
    fit.delay  = (float)xs[riseIdx] * toSec;
    fit.attack = std::max(0.f, ((float)xs[peakIdx] - (float)xs[riseIdx]) * toSec);

    // Hold = time held at the peak; decay = peak -> sustain.
    int holdEnd = peakIdx;
    while (holdEnd + 1 <= sustainIdx && ys[holdEnd + 1] >= peak - peak / 100) ++holdEnd;
    fit.hold  = ((float)xs[holdEnd] - (float)xs[peakIdx]) * toSec;
    fit.decay = std::max(0.f, ((float)xs[sustainIdx] - (float)xs[holdEnd]) * toSec);
    fit.release = (sustainIdx < count - 1)
                ? ((float)xs[count - 1] - (float)xs[sustainIdx]) * toSec : 0.f;

    // How wrong is this?  Walk the original and compare against the fitted
    // straight-line stages.  The caller SHOWS this to the user, because a curve
    // that does not fit DAHDSR is a real loss and silently approximating it is
    // how an export quietly stops sounding like the patch.
    auto fitted = [&](float t) -> float {
        if (t <= fit.delay) return 0.f;
        if (t <= fit.delay + fit.attack)
            return fit.attack > 0.f ? (t - fit.delay) / fit.attack : 1.f;
        const float hEnd = fit.delay + fit.attack + fit.hold;
        if (t <= hEnd) return 1.f;
        if (fit.decay <= 0.f) return fit.sustain;
        const float k = (t - hEnd) / fit.decay;
        return k >= 1.f ? fit.sustain : 1.f + k * (fit.sustain - 1.f);
    };
    float worst = 0.f;
    for (int i = 0; i <= sustainIdx; ++i) {
        const float t = (float)xs[i] * toSec;
        const float a = std::max(1e-4f, (float)ys[i] / 65535.f);
        const float b = std::max(1e-4f, fitted(t));
        worst = std::max(worst, std::fabs(20.f * std::log10(a / b)));
    }
    fit.maxErrorDb = worst;
    return fit;
}

}}} // namespace PatchKnob::engine::sf2
