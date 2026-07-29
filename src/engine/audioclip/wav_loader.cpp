//----------------------------------------------------------------------------
//  PatchKnob — WAV loader implementation. See wav_loader.h.
//
//  Self-contained RIFF/WAVE reader. We walk the chunk list, pull `fmt ` for the
//  encoding and `data` for the samples, decode to float, then resample to the
//  engine rate. No third-party code.
//----------------------------------------------------------------------------
#include "wav_loader.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace PatchKnob { namespace engine {

namespace {

// ---- little-endian byte readers (WAV is always little-endian) --------------
inline uint16_t rdU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t rdU32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Decode one PCM/float sample of `bits` starting at `p` into float [-1,1].
inline float decodeSample(const uint8_t* p, uint16_t bits, bool isFloat) {
    if (isFloat) {
        // 32-bit IEEE float, little-endian.
        uint32_t u = rdU32(p);
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }
    switch (bits) {
        case 16: {
            int16_t v = (int16_t)rdU16(p);
            return (float)v / 32768.0f;
        }
        case 24: {
            // Sign-extend 24-bit little-endian to 32-bit.
            int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x00800000) v |= (int32_t)0xFF000000;
            return (float)v / 8388608.0f;
        }
        case 32: {
            int32_t v = (int32_t)rdU32(p);
            return (float)v / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

// Linear-interpolation resample of one planar channel from srcRate to dstRate.
std::vector<float> resampleLinear(const std::vector<float>& in,
                                  double srcRate, double dstRate) {
    if (in.empty() || srcRate == dstRate) return in;
    const double ratio  = srcRate / dstRate;        // src frames per dst frame
    const int64_t inN   = (int64_t)in.size();
    const int64_t outN  = (int64_t)((double)inN * dstRate / srcRate + 0.5);
    std::vector<float> out((size_t)(outN > 0 ? outN : 0), 0.0f);
    for (int64_t i = 0; i < outN; ++i) {
        const double srcPos = (double)i * ratio;
        int64_t i0 = (int64_t)srcPos;
        double  fr = srcPos - (double)i0;
        if (i0 >= inN - 1) { out[(size_t)i] = in[(size_t)(inN - 1)]; continue; }
        const float a = in[(size_t)i0];
        const float b = in[(size_t)(i0 + 1)];
        out[(size_t)i] = a + (float)fr * (b - a);
    }
    return out;
}

// Extract the file stem (basename without extension) for the clip name.
std::string stemOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

inline bool fail(std::string* error, const char* msg) {
    if (error) *error = msg;
    return false;
}

} // namespace

bool loadWav(const std::string& path, double engineSampleRate,
             AudioClip& out, std::string* error) {
    out = AudioClip{};

    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(error, "cannot open file");

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    if (bytes.size() < 12) return fail(error, "file too small for RIFF header");

    const uint8_t* d = bytes.data();
    const size_t   n = bytes.size();
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0)
        return fail(error, "not a RIFF/WAVE file");

    // ---- walk chunks -------------------------------------------------------
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t fileRate = 0;
    const uint8_t* dataPtr = nullptr;
    uint32_t       dataLen = 0;
    bool haveFmt = false;

    size_t pos = 12;
    while (pos + 8 <= n) {
        const uint8_t* ch = d + pos;
        const uint32_t chunkSize = rdU32(ch + 4);
        const uint8_t* body = ch + 8;
        // Guard against a size that runs past the buffer.
        const uint32_t avail = (uint32_t)(n - (pos + 8));
        const uint32_t sz    = (chunkSize <= avail) ? chunkSize : avail;

        if (std::memcmp(ch, "fmt ", 4) == 0 && sz >= 16) {
            audioFormat   = rdU16(body + 0);
            numChannels   = rdU16(body + 2);
            fileRate      = rdU32(body + 4);
            bitsPerSample = rdU16(body + 14);
            // WAVE_FORMAT_EXTENSIBLE: real format lives in the sub-format GUID
            // whose first two bytes are the actual format tag.
            if (audioFormat == 0xFFFE && sz >= 26) {
                const uint16_t extSize = rdU16(body + 16);
                if (extSize >= 22) audioFormat = rdU16(body + 24);
            }
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataPtr = body;
            dataLen = sz;
        }

        // Chunks are word-aligned: advance by size + pad byte if odd.
        pos += 8 + chunkSize + (chunkSize & 1u);
    }

    if (!haveFmt)                     return fail(error, "missing fmt chunk");
    if (!dataPtr || dataLen == 0)     return fail(error, "missing/empty data chunk");
    if (numChannels < 1)              return fail(error, "zero channels");
    const bool isFloat = (audioFormat == 3);
    if (audioFormat != 1 && audioFormat != 3)
        return fail(error, "unsupported (non-PCM/float) encoding");
    if (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)
        return fail(error, "unsupported bit depth (need 16/24/32)");
    if (isFloat && bitsPerSample != 32)
        return fail(error, "float WAV must be 32-bit");

    const uint32_t bytesPerSample = bitsPerSample / 8;
    const uint32_t frameBytes     = bytesPerSample * numChannels;
    if (frameBytes == 0) return fail(error, "invalid frame size");
    const int64_t numFrames = (int64_t)(dataLen / frameBytes);
    if (numFrames <= 0) return fail(error, "no complete frames in data");

    // ---- decode interleaved source into two planar float channels ---------
    std::vector<float> L((size_t)numFrames), R((size_t)numFrames);
    for (int64_t fr = 0; fr < numFrames; ++fr) {
        const uint8_t* framePtr = dataPtr + (size_t)fr * frameBytes;
        const float s0 = decodeSample(framePtr, bitsPerSample, isFloat);
        L[(size_t)fr] = s0;
        if (numChannels >= 2) {
            const float s1 = decodeSample(framePtr + bytesPerSample, bitsPerSample, isFloat);
            R[(size_t)fr] = s1;
        } else {
            R[(size_t)fr] = s0;   // mono -> duplicate into both channels
        }
    }

    // ---- resample to engine rate if needed --------------------------------
    if (fileRate != 0 && (double)fileRate != engineSampleRate) {
        L = resampleLinear(L, (double)fileRate, engineSampleRate);
        R = resampleLinear(R, (double)fileRate, engineSampleRate);
    }

    out.name             = stemOf(path);
    out.sourceSampleRate = (double)fileRate;
    out.sampleRate       = engineSampleRate;
    out.ch[0]            = std::move(L);
    out.ch[1]            = std::move(R);
    return true;
}

bool saveWav16(const std::string& path, const AudioClip& clip, std::string* error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return fail(error, "cannot open file for writing");

    const uint16_t channels   = 2;
    const uint32_t rate       = (uint32_t)(clip.sampleRate + 0.5);
    const uint16_t bits       = 16;
    const uint32_t frameBytes = channels * (bits / 8);
    const int64_t  frames     = clip.numFrames();
    const uint32_t dataBytes  = (uint32_t)(frames * frameBytes);

    auto wr32 = [&](uint32_t v) {
        uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        f.write((const char*)b, 4);
    };
    auto wr16 = [&](uint16_t v) {
        uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
        f.write((const char*)b, 2);
    };
    auto toI16 = [](float x) -> int16_t {
        if (x >  1.0f) x =  1.0f;
        if (x < -1.0f) x = -1.0f;
        int32_t v = (int32_t)(x * 32767.0f + (x >= 0.0f ? 0.5f : -0.5f));
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        return (int16_t)v;
    };

    f.write("RIFF", 4);
    wr32(36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    wr32(16);                       // PCM fmt chunk size
    wr16(1);                        // PCM
    wr16(channels);
    wr32(rate);
    wr32(rate * frameBytes);        // byte rate
    wr16((uint16_t)frameBytes);     // block align
    wr16(bits);
    f.write("data", 4);
    wr32(dataBytes);
    for (int64_t i = 0; i < frames; ++i) {
        wr16((uint16_t)toI16(clip.ch[0][(size_t)i]));
        wr16((uint16_t)toI16(clip.ch[1][(size_t)i]));
    }
    if (!f) return fail(error, "write failed");
    return true;
}

}} // namespace PatchKnob::engine
