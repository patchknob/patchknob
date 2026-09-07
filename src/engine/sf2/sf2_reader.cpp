//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_reader.cpp -- see sf2_reader.h.
//----------------------------------------------------------------------------
#include "sf2_reader.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace PatchKnob { namespace engine { namespace sf2 {
namespace {

//! Seek that survives offsets past 2 GB.  std::fseek takes a `long`, which is
//! 32 bits on Windows (LLP64), so seeking into the sample pool of a large bank
//! would silently truncate the offset there.  Linux happens to work because
//! long is 64-bit -- which is exactly the kind of "works on my machine" this
//! wrapper exists to close.
int seek64(FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
int64_t tell64(FILE* f) {
#if defined(_WIN32)
    return (int64_t)_ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

//----------------------------------------------------------------------------
//  RIFF walking.  Everything is bounds-checked against the FILE SIZE, not
//  against the parent chunk's declared size: a truncated or hostile soundfont
//  that claims a 4 GB pdta must fail cleanly, not read off the end of the map.
//----------------------------------------------------------------------------
struct Reader {
    const uint8_t* p = nullptr;
    size_t         n = 0;
    size_t         at = 0;
    bool ok(size_t bytes) const { return at + bytes <= n; }
    uint8_t  u8 () { return ok(1) ? p[at++] : 0; }
    uint16_t u16() { if(!ok(2)) { at=n; return 0; } uint16_t v = (uint16_t)(p[at] | (p[at+1]<<8)); at+=2; return v; }
    uint32_t u32() { if(!ok(4)) { at=n; return 0; } uint32_t v = (uint32_t)p[at] | ((uint32_t)p[at+1]<<8)
                                                              | ((uint32_t)p[at+2]<<16) | ((uint32_t)p[at+3]<<24); at+=4; return v; }
    int16_t  s16() { return (int16_t)u16(); }
    void fourcc(char out[5]) { for (int i=0;i<4;++i) out[i] = (char)u8(); out[4]=0; }
    void skip(size_t bytes) { at = (at + bytes > n) ? n : at + bytes; }
    //! Fixed-width, NOT necessarily NUL-terminated (spec allows a full 20 bytes).
    std::string name(size_t width) {
        if (!ok(width)) { at = n; return {}; }
        const char* s = (const char*)p + at;
        size_t len = 0; while (len < width && s[len]) ++len;
        at += width;
        return std::string(s, len);
    }
};

struct Chunk { char id[5]; uint32_t size; size_t dataAt; };

bool nextChunk(Reader& r, Chunk& c) {
    if (!r.ok(8)) return false;
    r.fourcc(c.id);
    c.size   = r.u32();
    c.dataAt = r.at;
    if (c.dataAt + c.size > r.n) return false;   // declared past EOF: reject
    return true;
}
//! RIFF pads odd-sized chunks to even. Forgetting this shifts every later chunk.
void skipChunk(Reader& r, const Chunk& c) { r.at = c.dataAt + c.size + (c.size & 1u); }

//----------------------------------------------------------------------------
//  Raw pdta records (spec section 7).
//----------------------------------------------------------------------------
struct RawGen { uint16_t op; uint16_t amount; };
struct RawBag { uint16_t genNdx; uint16_t modNdx; };
struct RawPHdr { std::string name; uint16_t preset, bank, bagNdx; };
struct RawInst { std::string name; uint16_t bagNdx; };

//! Absolute-cents -> Hz.  SF2 stores filter cutoff as 1200*log2(f/8.176).
float absCentsToHz(float cents) { return 8.176f * std::pow(2.f, cents / 1200.f); }
//! Timecents -> seconds.  -32768 is the spec's "zero time" sentinel.
float timeCentsToSec(int tc) {
    if (tc <= -32768) return 0.f;
    return std::pow(2.f, (float)tc / 1200.f);
}

//! Generator defaults (spec section 8.1.3).  Anything not listed defaults to 0,
//! which is why this table only carries the ones whose default is NOT zero --
//! getting `initialFilterFc` wrong (13500 = wide open, not 0 = DC) is the
//! classic way to import a soundfont that plays completely muffled.
int16_t defaultGen(uint16_t op) {
    switch (op) {
        case GEN_initialFilterFc:  return 13500;   // absolute cents ~= 20 kHz
        case GEN_delayVolEnv:      case GEN_attackVolEnv:
        case GEN_holdVolEnv:       case GEN_decayVolEnv:
        case GEN_releaseVolEnv:    case GEN_delayModEnv:
        case GEN_attackModEnv:     case GEN_holdModEnv:
        case GEN_decayModEnv:      case GEN_releaseModEnv: return -12000;
        case GEN_scaleTuning:      return 100;
        case GEN_overridingRootKey:return -1;      // -1 = "use the sample's"
        case GEN_keyRange:         case GEN_velRange: return (int16_t)0x7F00;
        default:                   return 0;
    }
}

//! A generator set: value + "was it actually present?", because a preset-level
//! generator is an OFFSET and must only be added when the preset really set it.
struct GenSet {
    int16_t v[GEN_COUNT];
    bool    present[GEN_COUNT];
    GenSet() { for (int i=0;i<GEN_COUNT;++i) { v[i] = defaultGen((uint16_t)i); present[i] = false; } }
    void set(uint16_t op, uint16_t amount) {
        if (op >= GEN_COUNT) return;              // unknown generator: ignore
        v[op] = (int16_t)amount; present[op] = true;
    }
    int val(uint16_t op) const { return v[op]; }
};

//! These are NOT additive: a preset zone cannot "offset" a key range or a
//! sample id.  Summing them is the single most common SF2 importer bug --
//! it produces zones whose key range starts above 127 and are silently silent.
bool isAdditive(uint16_t op) {
    return op != GEN_keyRange && op != GEN_velRange &&
           op != GEN_sampleID && op != GEN_instrument &&
           op != GEN_overridingRootKey && op != GEN_exclusiveClass &&
           op != GEN_sampleModes;
}

//! `presetLevel` distinguishes the two very different things the spec asks for:
//!
//!   * WITHIN a level (a global zone's defaults under a zone's own values) the
//!     zone simply REPLACES what the global said -- it never adds to it.  Adding
//!     here is subtle and wrong: a global 2 s release under a zone asking for
//!     0.5 s produced exactly 1 s, because the two timecent values summed to 0.
//!   * ACROSS levels (a preset zone applied onto a resolved instrument zone)
//!     additive generators add -- and RANGES INTERSECT.  A preset keyRange does
//!     not move the instrument's range and does not replace it; it narrows
//!     which instrument zones the note reaches.  Replacing is what my first
//!     draft did, and it collapsed every zone of a multi-zone instrument onto
//!     the preset's range -- two zones that should split at middle C both
//!     claimed 40..80 and the test caught it immediately.
void applyOffsets(GenSet& base, const GenSet& offs, bool presetLevel = false) {
    for (uint16_t op = 0; op < GEN_COUNT; ++op) {
        if (!offs.present[op]) continue;
        const bool isRange = (op == GEN_keyRange || op == GEN_velRange);
        if (!presetLevel) { base.v[op] = offs.v[op]; base.present[op] = true; continue; }
        if (isRange) {
            const int bLo = base.v[op] & 0xFF, bHi = (base.v[op] >> 8) & 0xFF;
            const int oLo = offs.v[op] & 0xFF, oHi = (offs.v[op] >> 8) & 0xFF;
            const int lo = std::max(bLo, oLo), hi = std::min(bHi, oHi);
            // An empty intersection means this preset zone does not apply to
            // this instrument zone at all; encode it as an impossible range and
            // let the caller drop the zone.
            base.v[op] = (int16_t)(((hi & 0xFF) << 8) | (lo & 0xFF));
            if (lo > hi) base.v[op] = (int16_t)0x00FF;   // lo=255, hi=0
        } else if (isAdditive(op)) {
            base.v[op] = (int16_t)(base.v[op] + offs.v[op]);
        } else {
            base.v[op] = offs.v[op];
        }
        base.present[op] = true;
    }
}

void fillZone(Zone& z, const GenSet& g, const std::vector<Sample>& samples) {
    const int sid = g.val(GEN_sampleID);
    z.sampleIndex = (sid >= 0 && sid < (int)samples.size()) ? sid : -1;

    const uint16_t kr = (uint16_t)g.val(GEN_keyRange), vr = (uint16_t)g.val(GEN_velRange);
    z.loKey = kr & 0xFF;        z.hiKey = (kr >> 8) & 0xFF;
    z.loVel = vr & 0xFF;        z.hiVel = (vr >> 8) & 0xFF;
    if (z.loKey > z.hiKey) std::swap(z.loKey, z.hiKey);
    if (z.loVel > z.hiVel) std::swap(z.loVel, z.hiVel);

    z.coarseTune     = g.val(GEN_coarseTune);
    z.fineTune       = g.val(GEN_fineTune);
    z.scaleTuning    = g.val(GEN_scaleTuning);
    z.exclusiveClass = g.val(GEN_exclusiveClass);
    z.pan            = std::max(-1.f, std::min(1.f, g.val(GEN_pan) / 500.f));   // 0.1% units
    z.attenuationDb  = g.val(GEN_initialAttenuation) / 10.f;                    // centibels

    const int modes = g.val(GEN_sampleModes) & 3;
    z.loop            = (modes == 1 || modes == 3);
    z.loopUntilRelease= (modes == 3);

    z.delayVol  = timeCentsToSec(g.val(GEN_delayVolEnv));
    z.attackVol = timeCentsToSec(g.val(GEN_attackVolEnv));
    z.holdVol   = timeCentsToSec(g.val(GEN_holdVolEnv));
    z.decayVol  = timeCentsToSec(g.val(GEN_decayVolEnv));
    z.releaseVol= timeCentsToSec(g.val(GEN_releaseVolEnv));
    //! sustainVolEnv is ATTENUATION in centibels, not a level: 0 = full, 1000 =
    //! -100 dB.  Reading it as a level gives silent sustains everywhere.
    z.sustainVol = std::max(0.f, std::min(1.f,
                     std::pow(10.f, -(float)g.val(GEN_sustainVolEnv) / 200.f)));

    z.delayMod  = timeCentsToSec(g.val(GEN_delayModEnv));
    z.attackMod = timeCentsToSec(g.val(GEN_attackModEnv));
    z.holdMod   = timeCentsToSec(g.val(GEN_holdModEnv));
    z.decayMod  = timeCentsToSec(g.val(GEN_decayModEnv));
    z.releaseMod= timeCentsToSec(g.val(GEN_releaseModEnv));
    z.sustainMod= std::max(0.f, std::min(1.f, 1.f - g.val(GEN_sustainModEnv) / 1000.f));
    z.modEnvToPitchCents  = (float)g.val(GEN_modEnvToPitch);
    z.modEnvToFilterCents = (float)g.val(GEN_modEnvToFilterFc);

    const int fc = g.val(GEN_initialFilterFc);
    z.cutoffHz    = (fc >= 13500) ? 0.f : absCentsToHz((float)fc);  // >= 13500: open
    z.resonanceDb = g.val(GEN_initialFilterQ) / 10.f;

    if (z.sampleIndex >= 0) {
        const Sample& s = samples[(size_t)z.sampleIndex];
        const int root = g.val(GEN_overridingRootKey);
        z.rootKey  = (root >= 0 && root <= 127) ? root : (int)s.originalKey;
        z.fineTune += s.pitchCorrection;   // the sample's own correction folds in

        // 16-bit offsets are extended by their "coarse" partners (x 32768).
        const int32_t startOff = g.val(GEN_startAddrsOffset) + 32768 * g.val(GEN_startAddrsCoarse);
        const int32_t endOff   = g.val(GEN_endAddrsOffset)   + 32768 * g.val(GEN_endAddrsCoarse);
        const int32_t lsOff    = g.val(GEN_startloopAddrsOffset) + 32768 * g.val(GEN_startloopAddrsCoarse);
        const int32_t leOff    = g.val(GEN_endloopAddrsOffset)   + 32768 * g.val(GEN_endloopAddrsCoarse);

        const int64_t len = (int64_t)s.end - (int64_t)s.start;
        auto clampToSample = [&](int64_t v) -> uint32_t {
            return (uint32_t)std::max<int64_t>(0, std::min<int64_t>(v, len > 0 ? len : 0));
        };
        z.startOffset = clampToSample(startOff);
        z.endOffset   = clampToSample(len + endOff);
        z.loopStart   = clampToSample((int64_t)s.loopStart - (int64_t)s.start + lsOff);
        z.loopEnd     = clampToSample((int64_t)s.loopEnd   - (int64_t)s.start + leOff);
        if (z.loopEnd <= z.loopStart) z.loop = false;   // degenerate: don't loop
    }
}

} // namespace

//----------------------------------------------------------------------------
bool looksLikeSoundFont(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12] = {0};
    const size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    return got == sizeof(hdr) && !std::memcmp(hdr, "RIFF", 4) && !std::memcmp(hdr + 8, "sfbk", 4);
}

//----------------------------------------------------------------------------
bool read(const std::string& path, SoundFont& out, std::string& error, bool loadPcm) {
    error.clear();

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    const int64_t fileLen = tell64(f);
    if (fileLen <= 12) { std::fclose(f); error = "not a soundfont (too small)"; return false; }

    // Read chunk HEADERS by seeking; materialise only the tables we parse.
    // sdta is typically >99% of the file and is never read here -- its offset
    // is recorded so PCM can be pulled per sample later.
    auto at = [&](int64_t off, void* dst, size_t n) -> bool {
        if (off < 0 || off + (int64_t)n > fileLen) return false;
        return seek64(f, (uint64_t)off) == 0 && std::fread(dst, 1, n, f) == n;
    };
    auto body = [&](int64_t off, uint32_t n, std::vector<uint8_t>& dst) -> bool {
        dst.resize(n);
        return n == 0 ? true : at(off, dst.data(), n);
    };

    char hdr[12];
    if (!at(0, hdr, 12) || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "sfbk", 4)) {
        std::fclose(f); error = "not a RIFF/sfbk file"; return false;
    }

    SoundFont font;
    font.sourcePath = path;
    std::vector<RawPHdr> phdr; std::vector<RawInst> inst;
    std::vector<RawBag>  pbag, ibag;
    std::vector<RawGen>  pgen, igen;
    struct RawShdr { std::string name; uint32_t start,end,ls,le,rate; uint8_t key; int8_t corr; uint16_t link, type; };
    std::vector<RawShdr> shdr;
    std::vector<uint8_t> pcmPool;   // only filled when loadPcm

    int64_t pos = 12;
    while (pos + 8 <= fileLen) {
        char cid[4]; uint8_t szb[4];
        if (!at(pos, cid, 4) || !at(pos + 4, szb, 4)) break;
        const uint32_t csz = (uint32_t)szb[0] | ((uint32_t)szb[1]<<8) | ((uint32_t)szb[2]<<16) | ((uint32_t)szb[3]<<24);
        const int64_t cdata = pos + 8;
        if (cdata + (int64_t)csz > fileLen) break;

        if (!std::memcmp(cid, "LIST", 4)) {
            char lt[4];
            if (at(cdata, lt, 4)) {
                int64_t sp = cdata + 4;
                const int64_t lend = cdata + csz;
                while (sp + 8 <= lend) {
                    char sid[4]; uint8_t ssz[4];
                    if (!at(sp, sid, 4) || !at(sp + 4, ssz, 4)) break;
                    const uint32_t ssize = (uint32_t)ssz[0] | ((uint32_t)ssz[1]<<8) | ((uint32_t)ssz[2]<<16) | ((uint32_t)ssz[3]<<24);
                    const int64_t sdata = sp + 8;
                    if (sdata + (int64_t)ssize > lend) break;
                    std::vector<uint8_t> blob;
                    auto want = [&](const char* w){ return !std::memcmp(sid, w, 4); };

                    if (!std::memcmp(lt, "INFO", 4)) {
                        if (body(sdata, ssize, blob)) {
                            Reader sr{blob.data(), blob.size(), 0};
                            if      (want("ifil")) { font.versionMajor = sr.u16(); font.versionMinor = sr.u16(); }
                            else if (want("INAM")) font.name    = sr.name(ssize);
                            else if (want("isng")) font.engine  = sr.name(ssize);
                            else if (want("ISFT")) font.tools   = sr.name(ssize);
                            else if (want("ICMT")) font.comment = sr.name(ssize);
                        }
                    } else if (!std::memcmp(lt, "sdta", 4)) {
                        // NEVER read here: this is the 800 MB.
                        if      (want("smpl")) { font.smplOffset = (uint64_t)sdata; font.smplBytes = ssize; }
                        else if (want("sm24")) { font.sm24Offset = (uint64_t)sdata; font.sm24Bytes = ssize; }
                    } else if (!std::memcmp(lt, "pdta", 4)) {
                        if (body(sdata, ssize, blob)) {
                            Reader sr{blob.data(), blob.size(), 0};
                            if (want("phdr")) {
                                for (uint32_t i = 0; i + 38 <= ssize; i += 38) {
                                    RawPHdr h; h.name = sr.name(20); h.preset = sr.u16(); h.bank = sr.u16();
                                    h.bagNdx = sr.u16(); sr.skip(12);
                                    phdr.push_back(h);
                                }
                            } else if (want("pbag") || want("ibag")) {
                                std::vector<RawBag>& dst = (sid[0]=='p') ? pbag : ibag;
                                for (uint32_t i = 0; i + 4 <= ssize; i += 4) { RawBag b; b.genNdx = sr.u16(); b.modNdx = sr.u16(); dst.push_back(b); }
                            } else if (want("pgen") || want("igen")) {
                                std::vector<RawGen>& dst = (sid[0]=='p') ? pgen : igen;
                                for (uint32_t i = 0; i + 4 <= ssize; i += 4) { RawGen g; g.op = sr.u16(); g.amount = sr.u16(); dst.push_back(g); }
                            } else if (want("inst")) {
                                for (uint32_t i = 0; i + 22 <= ssize; i += 22) { RawInst n; n.name = sr.name(20); n.bagNdx = sr.u16(); inst.push_back(n); }
                            } else if (want("shdr")) {
                                for (uint32_t i = 0; i + 46 <= ssize; i += 46) {
                                    RawShdr h; h.name = sr.name(20);
                                    h.start = sr.u32(); h.end = sr.u32(); h.ls = sr.u32(); h.le = sr.u32();
                                    h.rate = sr.u32(); h.key = sr.u8(); h.corr = (int8_t)sr.u8();
                                    h.link = sr.u16(); h.type = sr.u16();
                                    shdr.push_back(h);
                                }
                            }
                        }
                    }
                    sp = sdata + ssize + (ssize & 1u);
                }
            }
        }
        pos = cdata + csz + (csz & 1u);
    }

    if (loadPcm && font.smplBytes) {
        pcmPool.resize((size_t)font.smplBytes);
        if (!at((int64_t)font.smplOffset, pcmPool.data(), pcmPool.size())) pcmPool.clear();
    }
    std::vector<uint8_t> pool24;
    if (loadPcm && font.sm24Bytes) {
        pool24.resize((size_t)font.sm24Bytes);
        if (!at((int64_t)font.sm24Offset, pool24.data(), pool24.size())) pool24.clear();
    }
    std::fclose(f);

    const uint8_t* smpl = pcmPool.empty() ? nullptr : pcmPool.data();
    const size_t   smplBytes = pcmPool.size();
    const uint8_t* sm24 = pool24.empty() ? nullptr : pool24.data();
    const size_t   sm24Bytes = pool24.size();
    (void)smplBytes; (void)sm24Bytes;

    if (phdr.size() < 2 || inst.size() < 2 || shdr.empty()) {
        error = "missing or empty pdta tables"; return false;
    }

    // ---- samples (the last shdr record is the "EOS" terminal, drop it) -----
    font.samples.reserve(shdr.size());
    for (size_t i = 0; i + 1 < shdr.size(); ++i) {
        const RawShdr& h = shdr[i];
        Sample s;
        s.name = h.name; s.start = h.start; s.end = h.end;
        s.loopStart = h.ls; s.loopEnd = h.le;
        s.sampleRate = h.rate ? h.rate : 44100;
        s.originalKey = h.key <= 127 ? h.key : 60;
        s.pitchCorrection = h.corr; s.sampleLink = h.link; s.sampleType = h.type;
        if (loadPcm && smpl && h.end > h.start) {
            const size_t frames = h.end - h.start;
            if ((size_t)h.end * 2 <= smplBytes) {
                s.pcm.resize(frames);
                const uint8_t* base = smpl + (size_t)h.start * 2;
                const bool have24 = sm24 && (size_t)h.end <= sm24Bytes;
                for (size_t k = 0; k < frames; ++k) {
                    const int16_t lo = (int16_t)(base[k*2] | (base[k*2+1] << 8));
                    if (have24) {
                        // 24-bit: sm24 carries the LOW byte of each sample.
                        const int32_t v = ((int32_t)lo << 8) | sm24[(size_t)h.start + k];
                        s.pcm[k] = (float)v / 8388608.f;
                    } else {
                        s.pcm[k] = (float)lo / 32768.f;
                    }
                }
            }
        }
        font.samples.push_back(std::move(s));
    }

    // ---- presets ----------------------------------------------------------
    auto zonesFor = [&](uint16_t bagFrom, uint16_t bagTo, const std::vector<RawBag>& bags,
                        const std::vector<RawGen>& gens, std::vector<GenSet>& outSets, GenSet& outGlobal) {
        bool haveGlobal = false;
        for (uint16_t b = bagFrom; b < bagTo && b + 1 < (uint16_t)bags.size() + 1; ++b) {
            if (b >= bags.size()) break;
            const uint16_t g0 = bags[b].genNdx;
            const uint16_t g1 = (b + 1 < bags.size()) ? bags[b+1].genNdx : (uint16_t)gens.size();
            GenSet set;
            bool terminal = false;
            for (uint16_t g = g0; g < g1 && g < gens.size(); ++g) {
                set.set(gens[g].op, gens[g].amount);
                if (gens[g].op == GEN_sampleID || gens[g].op == GEN_instrument) terminal = true;
            }
            // A zone WITHOUT a terminal generator, appearing first, is the
            // "global" zone: its values are defaults for its siblings.
            if (!terminal && !haveGlobal && outSets.empty()) { outGlobal = set; haveGlobal = true; continue; }
            if (!terminal) continue;                     // malformed: skip
            outSets.push_back(set);
        }
    };

    for (size_t pi = 0; pi + 1 < phdr.size(); ++pi) {
        Preset pr;
        pr.name = phdr[pi].name; pr.bank = phdr[pi].bank; pr.program = phdr[pi].preset;

        const uint16_t pb0 = phdr[pi].bagNdx;
        const uint16_t pb1 = phdr[pi+1].bagNdx;
        std::vector<GenSet> presetZones; GenSet presetGlobal;
        zonesFor(pb0, pb1, pbag, pgen, presetZones, presetGlobal);

        for (GenSet pz : presetZones) {
            // preset-level: global defaults first, then this zone's own values
            GenSet pset = presetGlobal;
            applyOffsets(pset, pz);

            const int instIdx = pset.val(GEN_instrument);
            if (instIdx < 0 || instIdx + 1 >= (int)inst.size()) continue;

            const uint16_t ib0 = inst[(size_t)instIdx].bagNdx;
            const uint16_t ib1 = inst[(size_t)instIdx + 1].bagNdx;
            std::vector<GenSet> instZones; GenSet instGlobal;
            zonesFor(ib0, ib1, ibag, igen, instZones, instGlobal);

            for (const GenSet& iz : instZones) {
                GenSet resolved = instGlobal;   // instrument globals ...
                applyOffsets(resolved, iz);     // ... then the zone's absolutes
                applyOffsets(resolved, pset, /*presetLevel=*/true);

                Zone z;
                fillZone(z, resolved, font.samples);
                if (z.sampleIndex < 0) continue;
                if (z.loKey > z.hiKey || z.loVel > z.hiVel) continue;  // empty intersection
                pr.zones.push_back(z);
            }
        }
        if (!pr.zones.empty()) font.presets.push_back(std::move(pr));
    }

    std::sort(font.presets.begin(), font.presets.end(),
              [](const Preset& a, const Preset& b) {
                  return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
              });

    if (font.presets.empty()) { error = "no playable presets"; return false; }
    out = std::move(font);
    return true;
}

//----------------------------------------------------------------------------
namespace {

//! THE conversion: little-endian 16-bit (plus optional sm24 low byte) to
//! -1..1 float.  One definition shared by the per-sample and the coalesced
//! decode paths, so they cannot disagree about what a sample sounds like.
void convertPcm(const uint8_t* raw16, const uint8_t* raw24 /*nullable*/,
                size_t frames, std::vector<float>& out) {
    out.resize(frames);
    if (raw24) {
        for (size_t k = 0; k < frames; ++k) {
            const int16_t hi = (int16_t)(raw16[k*2] | (raw16[k*2+1] << 8));
            out[k] = (float)(((int32_t)hi << 8) | raw24[k]) / 8388608.f;
        }
    } else {
        for (size_t k = 0; k < frames; ++k) {
            const int16_t hi = (int16_t)(raw16[k*2] | (raw16[k*2+1] << 8));
            out[k] = (float)hi / 32768.f;
        }
    }
}

//! Fetch a sample's sm24 low bytes (24-bit banks only) into `raw24`.
//! Returns null when the bank has no sm24 for this sample or the read fails
//! (the sample then decodes as plain 16-bit, exactly as the spec intends).
const uint8_t* fetchSm24(FILE* f, const SoundFont& font, const Sample& s,
                         std::vector<uint8_t>& raw24) {
    if (font.sm24Bytes < (uint64_t)s.end) return nullptr;
    const size_t frames = s.end - s.start;
    raw24.resize(frames);
    const uint64_t off8 = font.sm24Offset + (uint64_t)s.start;
    if (seek64(f, off8) != 0 ||
        std::fread(raw24.data(), 1, raw24.size(), f) != raw24.size()) return nullptr;
    return raw24.data();
}

//! Decode one sample from an ALREADY-OPEN file.  Factored out of
//! readSamplePcm() so batch decodes can share ONE fopen instead of paying
//! open/close per sample -- on the measured Concert Grand that was 192 opens
//! (349 on the orchestral bank) for a single patch.  `raw16`/`raw24` are
//! caller-owned scratch so the staging buffers are reused across samples
//! instead of reallocated per sample.
bool readSamplePcmAt(FILE* f, const SoundFont& font, const Sample& s,
                     std::vector<float>& out,
                     std::vector<uint8_t>& raw16, std::vector<uint8_t>& raw24,
                     std::string& error) {
    if (s.end <= s.start) { out.clear(); return true; }          // empty sample: not an error
    if (!font.smplBytes) { error = "soundfont has no sample pool"; return false; }

    const size_t frames = s.end - s.start;
    const uint64_t off16 = font.smplOffset + (uint64_t)s.start * 2;
    raw16.resize(frames * 2);
    if (seek64(f, off16) != 0 ||
        std::fread(raw16.data(), 1, raw16.size(), f) != raw16.size()) {
        error = "short read of sample data";
        return false;
    }
    convertPcm(raw16.data(), fetchSm24(f, font, s, raw24), frames, out);
    return true;
}

} // namespace

//----------------------------------------------------------------------------
bool samplesFormStereoPair(const SoundFont& font, int sampleIndex, int& outPartner) {
    outPartner = -1;
    if (sampleIndex < 0 || sampleIndex >= (int)font.samples.size()) return false;
    const Sample& s = font.samples[(size_t)sampleIndex];
    if (!s.isStereoPair()) return false;

    const int link = (int)s.sampleLink;
    if (link < 0 || link >= (int)font.samples.size() || link == sampleIndex) return false;

    const Sample& p = font.samples[(size_t)link];
    const bool opposite = (s.sampleType == 2 && p.sampleType == 4) ||
                          (s.sampleType == 4 && p.sampleType == 2);
    if (!opposite) return false;

    // Frame counts come from the HEADERS (end - start), not from decoded PCM,
    // so the verdict is the same before and after decode -- which lets
    // readPresetPcm() skip decoding a partner that could never interleave, and
    // lets the importer decide pairing for a sample whose buffer has already
    // been handed off to the sampler.
    const uint64_t fs = s.end > s.start ? (uint64_t)s.end - s.start : 0;
    const uint64_t fp = p.end > p.start ? (uint64_t)p.end - p.start : 0;
    if (fs == 0 || fs != fp) return false;

    outPartner = link;
    return true;
}

//----------------------------------------------------------------------------
bool readSamplePcm(const std::string& path, const SoundFont& font, int sampleIndex,
                   std::vector<float>& out, std::string& error) {
    error.clear();
    if (sampleIndex < 0 || sampleIndex >= (int)font.samples.size()) { error = "bad sample index"; return false; }
    const Sample& s = font.samples[(size_t)sampleIndex];
    if (s.end <= s.start) { out.clear(); return true; }          // empty sample: not an error
    if (!font.smplBytes) { error = "soundfont has no sample pool"; return false; }

    // SEEK to this one sample.  Re-reading the font with loadPcm would pull the
    // entire pool -- 800 MB to audition one hi-hat -- which is the whole reason
    // the header-only path exists.
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> raw16, raw24;
    const bool ok = readSamplePcmAt(f, font, s, out, raw16, raw24, error);
    std::fclose(f);
    return ok;
}

//----------------------------------------------------------------------------
bool readSamplesPcm(const std::string& path, SoundFont& font,
                    const std::vector<uint8_t>& want, std::string& error,
                    const PcmProgressFn& progress) {
    error.clear();
    if (want.size() != font.samples.size()) { error = "want-set size mismatch"; return false; }

    // Decode in FILE ORDER, through one open file.  Callers mark samples in
    // zone order, which is scattered all over an 800 MB pool; issuing the
    // reads by ascending offset instead lets the kernel's readahead prefetch
    // the next sample WHILE the previous one is being converted to float.
    //
    // Deliberately per-sample reads, NOT coalesced big-segment reads: that
    // was tried and measured SLOWER (GM bank cold: 288 ms -> 455 ms at 8 MB
    // segments, still >= 300 ms at 256 KB..4 MB).  A large synchronous fread
    // stalls until every byte arrives, serialising I/O behind decode, while
    // a run of ascending ~440 KB reads overlaps the two for free.
    std::vector<int> todo;
    todo.reserve(64);
    for (size_t i = 0; i < want.size(); ++i)
        if (want[i] && font.samples[i].pcm.empty()) todo.push_back((int)i);
    if (todo.empty()) return true;
    std::sort(todo.begin(), todo.end(), [&](int a, int b) {
        return font.samples[(size_t)a].start < font.samples[(size_t)b].start;
    });

    // Publish the total before the first (possibly slow, cold-cache) read so
    // a UI polling this can size its progress bar immediately.
    if (progress && !progress(0, (int)todo.size())) { error = "cancelled"; return false; }

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> raw16, raw24;
    for (size_t k = 0; k < todo.size(); ++k) {
        const int i = todo[k];
        if (!readSamplePcmAt(f, font, font.samples[(size_t)i], font.samples[(size_t)i].pcm,
                             raw16, raw24, error)) {
            std::fclose(f);
            return false;
        }
        // Per-sample cancellation: samples are a few hundred KB, so a
        // superseded import stops within ~a millisecond of I/O, not after
        // finishing a 40 MB patch nobody wants any more.
        if (progress && !progress((int)k + 1, (int)todo.size())) {
            std::fclose(f);
            error = "cancelled";
            return false;
        }
    }
    std::fclose(f);
    return true;
}

//! Every sample ONE preset needs, decoded once and shared by its zones.
//! Loading a patch must not re-read a sample per zone: a piano preset with 2976
//! zones references only a few hundred distinct samples, so the naive loop
//! would do an order of magnitude more I/O than the patch actually contains.
bool readPresetPcm(const std::string& path, SoundFont& font, int presetIndex,
                   std::string& error) {
    error.clear();
    if (presetIndex < 0 || presetIndex >= (int)font.presets.size()) { error = "bad preset index"; return false; }
    std::vector<uint8_t> want(font.samples.size(), 0);
    for (const Zone& z : font.presets[(size_t)presetIndex].zones)
        if (z.sampleIndex >= 0 && z.sampleIndex < (int)want.size()) {
            want[(size_t)z.sampleIndex] = 1;
            // A stereo zone needs its partner too, or one side plays silent --
            // but only a partner that can actually interleave.  A broken link
            // (wrong channel, mismatched length) falls back to mono in the
            // importer, so decoding its partner would be pure waste: on the
            // orchestral test bank that waste was 157 of 349 decoded samples,
            // nearly half the patch-load I/O.
            int partner = -1;
            if (samplesFormStereoPair(font, z.sampleIndex, partner))
                want[(size_t)partner] = 1;
        }
    return readSamplesPcm(path, font, want, error);
}

}}} // namespace PatchKnob::engine::sf2
