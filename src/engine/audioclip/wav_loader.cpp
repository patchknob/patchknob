//----------------------------------------------------------------------------
//  PatchKnob — WAV loader implementation. See wav_loader.h.
//
//  Self-contained RIFF/WAVE reader. We walk the chunk list, pull `fmt ` for the
//  encoding and `data` for the samples, decode to float, then resample to the
//  engine rate. No third-party code.
//----------------------------------------------------------------------------
#include "wav_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
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

// ---------------------------------------------------------------------------
// BAND-LIMITED RESAMPLER (windowed-sinc, table driven)
//
// The old resampleLinear() had no anti-alias filter of any kind.  Linear
// interpolation IS a filter, but a dreadful one: a triangular kernel, so it
// rolls the top of the band off badly (-1.5 dB at 10 kHz and -3.4 dB at 15 kHz
// converting 44.1k -> 48k, i.e. every 44.1 kHz sample pack arrived dulled) and
// it does nothing at all about content above the DESTINATION Nyquist.  With
// 96k -> 48k the whole thing degenerates to dropping every other sample, so a
// 30 kHz component came back as a 18 kHz tone at FULL amplitude.
//
// This is the standard band-limited interpolator instead: convolve with a
// windowed sinc whose cutoff is the LOWER of the two Nyquist limits, evaluated
// at the fractional source position of every output sample.  The kernel is
// tabulated once per conversion and read with linear interpolation between
// table entries (the table is dense enough that the residual error is far below
// 16-bit), so the inner loop is a multiply-add and no transcendental at all.
// This runs at load time on the message thread, never on the audio thread.
// ---------------------------------------------------------------------------
struct SincKernel {
    std::vector<float> h;        // h[k] = kernel at t = k / density input frames
    double density = 128.0;      // table entries per input frame
    double half    = 0.0;        // support half-width, in input frames
};

inline double sincPi(double x) {           // sin(pi x) / (pi x)
    if (x > -1e-9 && x < 1e-9) return 1.0;
    const double px = 3.14159265358979323846 * x;
    return std::sin(px) / px;
}

// Blackman window over t in [-1, 1] -- ~-74 dB sidelobes, which is what puts the
// aliases of a 2:1 decimation below the noise floor of the 16/24-bit source.
inline double blackman(double t) {
    if (t <= -1.0 || t >= 1.0) return 0.0;
    const double x = 3.14159265358979323846 * (t + 1.0);   // 0..2pi
    return 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
}

// `fc` is the cutoff in cycles per INPUT frame (0.5 == input Nyquist);
// `zeroCrossings` sets the transition width / cost trade-off.
SincKernel buildSincKernel(double fc, int zeroCrossings) {
    SincKernel k;
    if (fc > 0.5)  fc = 0.5;
    if (fc < 1e-4) fc = 1e-4;
    k.half    = (double)zeroCrossings / (2.0 * fc);   // zeros of sinc sit at n/(2fc)
    k.density = 128.0;
    const size_t n = (size_t)(k.half * k.density) + 2;
    k.h.resize(n + 1);
    for (size_t i = 0; i <= n; ++i) {
        const double t = (double)i / k.density;       // input frames from centre
        k.h[i] = (float)(2.0 * fc * sincPi(2.0 * fc * t) * blackman(t / k.half));
    }
    return k;
}

// Kernel value at distance `d` INPUT FRAMES from the centre (linear read of the
// tabulated impulse; the table is 128 entries per input frame).
inline double kernelAt(const SincKernel& k, double d) {
    const double t = d * k.density;
    const size_t idx = (size_t)t;
    if (idx + 1 >= k.h.size()) return 0.0;
    const double fr = t - (double)idx;
    return (double)k.h[idx] + fr * ((double)k.h[idx + 1] - (double)k.h[idx]);
}

// POLYPHASE form.  Sample rates are integers, so srcRate/dstRate is a rational
// M/L with a small L (44.1k -> 48k is 147/160), i.e. an output frame's
// fractional position takes only L distinct values.  Precompute the kernel once
// per phase and the inner loop becomes a fixed-length dot product over
// contiguous input -- no transcendentals, no table interpolation, no per-tap
// division.  That is what keeps a full-length import at load time cheap.
struct Polyphase {
    int64_t            L = 0, M = 0;   // srcPos(i) = i * M / L
    int                K = 0;          // taps each side of the centre
    std::vector<float> w;              // L * (2K+1), each phase normalised to 1
};

inline int64_t gcd64(int64_t a, int64_t b) { while (b) { const int64_t t = a % b; a = b; b = t; } return a; }

bool buildPolyphase(double srcRate, double dstRate, const SincKernel& k, Polyphase& pp) {
    const int64_t si = (int64_t)(srcRate + 0.5), di = (int64_t)(dstRate + 0.5);
    if (si <= 0 || di <= 0) return false;
    if (std::fabs(srcRate - (double)si) > 1e-6 || std::fabs(dstRate - (double)di) > 1e-6)
        return false;                                   // non-integer rate: generic path
    const int64_t g = gcd64(si, di);
    pp.M = si / g;
    pp.L = di / g;
    if (pp.L <= 0 || pp.L > 1 << 20) return false;       // absurd ratio: generic path
    pp.K = (int)std::ceil(k.half);
    if (pp.K < 1) pp.K = 1;
    const int taps = 2 * pp.K + 1;
    pp.w.assign((size_t)pp.L * (size_t)taps, 0.0f);
    for (int64_t ph = 0; ph < pp.L; ++ph) {
        const double frac = (double)ph / (double)pp.L;   // srcPos - floor(srcPos)
        double sum = 0.0;
        float* row = &pp.w[(size_t)ph * (size_t)taps];
        for (int t = 0; t < taps; ++t) {
            const double d = std::fabs((double)(t - pp.K) - frac);
            const double v = kernelAt(k, d);
            row[t] = (float)v;
            sum += v;
        }
        // Normalise each phase to unity DC gain: the kernel is truncated, and
        // without this the truncation shows up as a few hundredths of a dB of
        // ripple that walks with the phase (i.e. as a tone).
        if (sum > 1e-9) for (int t = 0; t < taps; ++t) row[t] = (float)((double)row[t] / sum);
    }
    return true;
}

std::vector<float> resamplePoly(const std::vector<float>& in, const Polyphase& pp) {
    const int64_t inN = (int64_t)in.size();
    const int64_t outN = (int64_t)((double)inN * (double)pp.L / (double)pp.M + 0.5);
    std::vector<float> out((size_t)(outN > 0 ? outN : 0), 0.0f);
    const int taps = 2 * pp.K + 1;
    const float* src = in.data();
    // Walk the phase instead of dividing: base += M/L each output frame, with
    // the remainder carried in `ph`.  (i*M)/L and (i*M)%L per output frame were
    // two 64-bit divisions in the hot loop.
    const int64_t whole = pp.M / pp.L, rem = pp.M % pp.L;
    int64_t base = 0, ph = 0;
    for (int64_t i = 0; i < outN; ++i) {
        const float*  w    = &pp.w[(size_t)ph * (size_t)taps];
        const int64_t j0   = base - pp.K;
        base += whole; ph += rem;
        if (ph >= pp.L) { ph -= pp.L; base += 1; }
        float acc = 0.0f;
        if (j0 >= 0 && j0 + taps <= inN) {
            const float* p = src + j0;
            for (int t = 0; t < taps; ++t) acc += p[t] * w[t];
        } else {
            // File ends: hold the edge sample.  The phase weights already sum to
            // one, so a hold keeps the level instead of tapering into silence.
            for (int t = 0; t < taps; ++t) {
                int64_t j = j0 + t;
                if (j < 0) j = 0;
                if (j > inN - 1) j = inN - 1;
                acc += src[j] * w[t];
            }
        }
        out[(size_t)i] = acc;
    }
    return out;
}

// Band-limited resample of one planar channel from srcRate to dstRate.
std::vector<float> resampleSinc(const std::vector<float>& in,
                                double srcRate, double dstRate,
                                const SincKernel& k) {
    if (in.empty() || srcRate == dstRate) return in;
    const double  step = srcRate / dstRate;           // src frames per dst frame
    const int64_t inN  = (int64_t)in.size();
    const int64_t outN = (int64_t)((double)inN * dstRate / srcRate + 0.5);
    std::vector<float> out((size_t)(outN > 0 ? outN : 0), 0.0f);

    const float* h    = k.h.data();
    const size_t hMax = k.h.size() - 1;
    for (int64_t i = 0; i < outN; ++i) {
        const double srcPos = (double)i * step;
        int64_t j0 = (int64_t)std::ceil (srcPos - k.half);
        int64_t j1 = (int64_t)std::floor(srcPos + k.half);
        if (j0 < 0)    j0 = 0;
        if (j1 > inN - 1) j1 = inN - 1;
        double acc = 0.0, wsum = 0.0;
        for (int64_t j = j0; j <= j1; ++j) {
            const double t   = std::fabs(srcPos - (double)j) * k.density;
            const size_t idx = (size_t)t;
            if (idx >= hMax) continue;
            const double fr = t - (double)idx;
            const double w  = (double)h[idx] + fr * ((double)h[idx + 1] - (double)h[idx]);
            acc  += (double)in[(size_t)j] * w;
            wsum += w;
        }
        // Normalise by the weight actually used: the kernel is truncated (and
        // clipped further at the very first/last output frames), and dividing
        // it out keeps the passband flat to well under 0.01 dB and the file's
        // ends at their real level instead of tapering into the truncation.
        out[(size_t)i] = (float)(wsum > 1e-9 ? acc / wsum : 0.0);
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

    // Size the buffer up front and read in one call.  The old
    // istreambuf_iterator construction pulled the file in ONE CHARACTER AT A
    // TIME through the stream buffer (measured ~345 ms for a 10-minute stereo
    // 16-bit file vs ~72 ms for a single read) -- pure waste on the message
    // thread every time a large WAV is imported.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(error, "cannot open file");
    const std::streamsize fileSize = f.tellg();
    if (fileSize < 12) return fail(error, "file too small for RIFF header");
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)fileSize);
    if (!f.read((char*)bytes.data(), fileSize))
        return fail(error, "read failed");

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
    // The 16-bit-PCM case (nearly every WAV a user drops in) gets its own
    // branch-free loop: the generic path pays a per-SAMPLE switch on the bit
    // depth plus a byte-assembling helper call, which measured ~2x slower over
    // a 10-minute file.  Other depths keep the generic decoder.
    std::vector<float> L((size_t)numFrames), R((size_t)numFrames);
    if (bitsPerSample == 16 && !isFloat) {
        const uint8_t* p = dataPtr;
        const float k = 1.0f / 32768.0f;
        if (numChannels >= 2) {
            for (int64_t fr = 0; fr < numFrames; ++fr, p += frameBytes) {
                L[(size_t)fr] = (float)(int16_t)rdU16(p) * k;
                R[(size_t)fr] = (float)(int16_t)rdU16(p + 2) * k;
            }
        } else {
            for (int64_t fr = 0; fr < numFrames; ++fr, p += frameBytes) {
                const float s = (float)(int16_t)rdU16(p) * k;
                L[(size_t)fr] = s;
                R[(size_t)fr] = s;    // mono -> duplicate into both channels
            }
        }
    } else {
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
    }

    // ---- resample to engine rate if needed --------------------------------
    if (fileRate != 0 && engineSampleRate > 0.0 && (double)fileRate != engineSampleRate) {
        // Cut off at the lower of the two Nyquist limits: half the input rate
        // when upsampling (pure interpolation), half the OUTPUT rate when
        // downsampling -- that second case is the anti-alias filter the old
        // path never had.  0.95 leaves the transition band inside the passband
        // edge instead of letting it eat the top of the audible range.
        const double ratio = engineSampleRate / (double)fileRate;
        const double fc = 0.5 * 0.95 * (ratio < 1.0 ? ratio : 1.0);
        const SincKernel k = buildSincKernel(fc, 16);
        Polyphase pp;
        const bool poly = buildPolyphase((double)fileRate, engineSampleRate, k, pp);
        // A mono file duplicated s0 into both channels above, so R is L: filter
        // it once rather than twice.  A stereo file filters its two channels
        // CONCURRENTLY -- the channels are independent, this runs on the
        // message thread (allocation is fine), and the resample is the
        // dominant cost of importing a long file at a foreign rate (measured
        // ~1.5 s of a 2.1 s 44.1k->48k 10-minute import).
        if (numChannels >= 2) {
            std::vector<float> outR;
            std::thread thR([&] {
                outR = poly ? resamplePoly(R, pp)
                            : resampleSinc(R, (double)fileRate, engineSampleRate, k);
            });
            L = poly ? resamplePoly(L, pp)
                     : resampleSinc(L, (double)fileRate, engineSampleRate, k);
            thR.join();
            R = std::move(outR);
        } else {
            L = poly ? resamplePoly(L, pp)
                     : resampleSinc(L, (double)fileRate, engineSampleRate, k);
            R = L;
        }
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
    const uint32_t rate       = (uint32_t)(clip.sampleRate > 0.0 ? clip.sampleRate + 0.5 : 48000.0);
    const uint16_t bits       = 16;
    const uint32_t frameBytes = channels * (bits / 8);
    // numFrames() is ch[0]'s length, but the writer indexes BOTH channels: a
    // ragged clip read ch[1] past its end for every frame past the shorter one.
    // RIFF sizes are uint32, so also refuse a clip whose data will not fit.
    const int64_t  frames     = clip.safeFrames();
    if (frames < 0 || (uint64_t)frames * frameBytes > 0xFFFFFFF0ull)
        return fail(error, "clip too large for a 32-bit RIFF size");
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
    // Convert into a chunk buffer and write it in slabs: the old loop pushed
    // every sample through TWO 2-byte ofstream writes (measured ~1.2 s for a
    // 10-minute clip; ~0.1 s buffered).  Little-endian bytes are assembled
    // explicitly so the file is identical on any host endianness.
    {
        const size_t kChunkFrames = 65536;
        std::vector<uint8_t> buf(kChunkFrames * 4);
        int64_t i = 0;
        while (i < frames) {
            const size_t nfr = (size_t)std::min<int64_t>((int64_t)kChunkFrames,
                                                         frames - i);
            uint8_t* p = buf.data();
            for (size_t j = 0; j < nfr; ++j, ++i) {
                const uint16_t l = (uint16_t)toI16(clip.ch[0][(size_t)i]);
                const uint16_t r = (uint16_t)toI16(clip.ch[1][(size_t)i]);
                *p++ = (uint8_t)l; *p++ = (uint8_t)(l >> 8);
                *p++ = (uint8_t)r; *p++ = (uint8_t)(r >> 8);
            }
            f.write((const char*)buf.data(), (std::streamsize)(nfr * 4));
        }
    }
    if (!f) return fail(error, "write failed");
    return true;
}

}} // namespace PatchKnob::engine
