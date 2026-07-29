#ifndef PATCHKNOB_ENGINE_RACK_SDK_COMPAT_HPP
#define PATCHKNOB_ENGINE_RACK_SDK_COMPAT_HPP

#include "rack.hpp"
#include "rack_dsp.h"
#include "rack_widgets_compat.hpp"

#include <simd/Vector.hpp>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <random>

// APP->engine->getSampleRate() etc.  Context object lives in rack.hpp so the
// base Module::onSampleRateChange(float) hook can refresh it per bridge DLL.
#ifndef APP
#define APP rack::contextGet()
#endif

// Rack logger macros: no-ops in the bridge (no logger on the audio thread).
#ifndef DEBUG
#define DEBUG(...) ((void)0)
#endif
#ifndef INFO
#define INFO(...) ((void)0)
#endif
#ifndef WARN
#define WARN(...) ((void)0)
#endif
#ifndef FATAL
#define FATAL(...) ((void)0)
#endif

namespace rack {

// Panel/asset paths for DSP that loads resources (wavetables, samples).
// S24_RACK_PLUGIN_ROOT is baked in by the generated bridge CMakeLists.
namespace asset {
inline std::string plugin(rack::plugin::Plugin*, const std::string& filename) {
#ifdef S24_RACK_PLUGIN_ROOT
    return std::string(S24_RACK_PLUGIN_ROOT "/") + filename;
#else
    return filename;
#endif
}
inline std::string system(const std::string& filename) { return filename; }
inline std::string user(const std::string& filename) { return filename; }
}

// VCV v0.6/v1 global engine accessors.
inline float engineGetSampleRate() { return contextGet()->engine->getSampleRate(); }
inline float engineGetSampleTime() { return contextGet()->engine->getSampleTime(); }

namespace string {
inline std::string f(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int size = std::vsnprintf(nullptr, 0, format, arguments);
    va_end(arguments);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    va_start(arguments, format);
    std::vsnprintf(result.data(), result.size() + 1, format, arguments);
    va_end(arguments);
    return result;
}
inline bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}
}

namespace random {
inline std::mt19937& generator() {
    static thread_local std::mt19937 value{std::random_device{}()};
    return value;
}
inline float uniform() {
    return std::generate_canonical<float, 24>(generator());
}
inline float normal() {
    static thread_local std::normal_distribution<float> distribution;
    return distribution(generator());
}
template <typename T>
T get();
template <>
inline bool get<bool>() { return uniform() >= 0.5f; }
template <>
inline float get<float>() { return uniform(); }
inline uint32_t u32() { return generator()(); }
inline uint64_t u64() {
    return (static_cast<uint64_t>(generator()()) << 32) | generator()();
}
inline void init() {}
}

namespace math {
inline float sgn(float value) { return value > 0.f ? 1.f : (value < 0.f ? -1.f : 0.f); }
inline float cubic(float value) { return value * value * value; }
inline float quadraticBipolar(float value) { return value * value * (value >= 0.f ? 1.f : -1.f); }
inline float quarticBipolar(float value) {
    const float fourth = value * value * value * value;
    return value >= 0.f ? fourth : -fourth;
}
inline float quintic(float value) { return value * value * value * value * value; }
inline float sqrtBipolar(float value) { return value >= 0.f ? std::sqrt(value) : -std::sqrt(-value); }
inline float exponentialBipolar(float base, float value) {
    return (std::pow(base, value) - std::pow(base, -value)) / (base - 1.f / base);
}
inline bool isPow2(int value) { return value > 0 && (value & (value - 1)) == 0; }
inline int eucMod(int value, int divisor) { return rack::eucMod(value, divisor); }
inline int eucDiv(int value, int divisor) {
    if (divisor == 0) return 0;
    int quotient = value / divisor;
    if (value % divisor < 0) quotient += (divisor > 0) ? -1 : 1;
    return quotient;
}
inline float interpolateLinear(const float* points, float position) {
    const int index = static_cast<int>(position);
    const float fraction = position - static_cast<float>(index);
    return crossfade(points[index], points[index + 1], fraction);
}
inline bool isNear(float a, float b, float epsilon = 1e-6f) { return std::fabs(a - b) <= epsilon; }
}
using math::isNear;

namespace network {
enum Method { METHOD_GET, METHOD_POST, METHOD_PUT, METHOD_DELETE };
inline json_t* requestJson(Method, const std::string&, json_t*) { return nullptr; }
inline bool requestDownload(const std::string&, const std::string&, float*) { return false; }
inline std::string encodeUrl(const std::string& url) { return url; }
}

namespace simd {
inline float_4 laneMap(float_4 value, float (*function)(float)) {
    float values[4];
    value.store(values);
    for (float& lane : values) lane = function(lane);
    return float_4::load(values);
}
inline float_4 laneMap(float_4 first, float_4 second, float (*function)(float, float)) {
    float left[4];
    float right[4];
    first.store(left);
    second.store(right);
    for (int lane = 0; lane < 4; ++lane) left[lane] = function(left[lane], right[lane]);
    return float_4::load(left);
}
inline int movemask(float_4 value) { return _mm_movemask_ps(value.v); }
inline float_4 ifelse(float_4 mask, float_4 yes, float_4 no) {
    return float_4(_mm_or_ps(_mm_and_ps(mask.v, yes.v), _mm_andnot_ps(mask.v, no.v)));
}
template <typename T>
T movemaskInverse(int mask);
template <>
inline float_4 movemaskInverse<float_4>(int mask) {
    return float_4::cast(int32_4((mask & 1) ? -1 : 0, (mask & 2) ? -1 : 0,
                                (mask & 4) ? -1 : 0, (mask & 8) ? -1 : 0));
}
inline float scalarClamp(float value, float low, float high) { return std::clamp(value, low, high); }
inline float_4 clamp(float_4 value, float_4 low, float_4 high) {
    return float_4(_mm_min_ps(_mm_max_ps(value.v, low.v), high.v));
}
inline float_4 clamp(float_4 value, float low = 0.f, float high = 1.f) {
    return clamp(value, float_4(low), float_4(high));
}
inline float_4 fmin(float_4 first, float_4 second) { return float_4(_mm_min_ps(first.v, second.v)); }
inline float_4 fmax(float_4 first, float_4 second) { return float_4(_mm_max_ps(first.v, second.v)); }
inline float scalarPow(float first, float second) { return std::pow(first, second); }
inline float_4 pow(float_4 first, float_4 second) { return laneMap(first, second, scalarPow); }
inline float_4 pow(float_4 value, int exponent) {
    if (exponent == 0) return float_4(1.f);
    const bool reciprocal = exponent < 0;
    unsigned int power = reciprocal ? static_cast<unsigned int>(-exponent) : static_cast<unsigned int>(exponent);
    float_4 result(1.f);
    while (power != 0) {
        if (power & 1u) result *= value;
        value *= value;
        power >>= 1u;
    }
    return reciprocal ? float_4(1.f) / result : result;
}
inline float scalarSin(float value) { return std::sin(value); }
inline float scalarFloor(float value) { return std::floor(value); }
inline float scalarTrunc(float value) { return std::trunc(value); }
inline float scalarRound(float value) { return std::round(value); }
inline float scalarAbs(float value) { return std::fabs(value); }
inline float scalarLog2(float value) { return std::log2(value); }
inline float_4 sin(float_4 value) { return laneMap(value, scalarSin); }
inline float_4 floor(float_4 value) { return float_4(_mm_floor_ps(value.v)); }
inline float_4 trunc(float_4 value) { return float_4(_mm_round_ps(value.v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)); }
inline float_4 round(float_4 value) { return float_4(_mm_round_ps(value.v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)); }
inline float_4 fabs(float_4 value) { return float_4(_mm_andnot_ps(_mm_set1_ps(-0.f), value.v)); }
inline float_4 log2(float_4 value) { return laneMap(value, scalarLog2); }
inline float_4 rescale(float_4 value, float inputMin, float inputMax, float outputMin, float outputMax) {
    return float_4(outputMin) + (value - float_4(inputMin)) * (outputMax - outputMin) / (inputMax - inputMin);
}
}

namespace dsp {
constexpr float FREQ_SEMITONE = 1.0594630943592953f;
constexpr float FREQ_C4 = rack::FREQ_C4;
// VCV v2 semantics: state starts "high" (all-ones for SIMD) so held gates do
// not fire a spurious trigger at startup.
template <typename T>
struct TSchmittTrigger {
    T state;
    TSchmittTrigger() { reset(); }
    void reset() { state = T::mask(); }
    T process(T value, T lowThreshold = 0.f, T highThreshold = 1.f) {
        const T on = value >= highThreshold;
        const T off = value <= lowThreshold;
        const T triggered = ~state & on;
        state = on | (state & ~off);
        return triggered;
    }
    T isHigh() { return state; }
};
template <>
struct TSchmittTrigger<float> {
    bool state = true;
    void reset() { state = true; }
    bool process(float value, float lowThreshold = 0.f, float highThreshold = 1.f) {
        if (state) {
            if (value <= lowThreshold) state = false;
        }
        else if (value >= highThreshold) {
            state = true;
            return true;
        }
        return false;
    }
    bool isHigh() const { return state; }
};
struct ClockDivider {
    uint32_t clock = 0;
    uint32_t division = 1;
    void reset() { clock = 0; }
    void setDivision(uint32_t value) { division = std::max(1u, value); }
    uint32_t getDivision() const { return division; }
    bool process() { return ++clock >= division ? (clock = 0, true) : false; }
};
struct Timer {
    float time = 0.f;
    void reset() { time = 0.f; }
    float process(float deltaTime) { time += deltaTime; return time; }
    float getTime() const { return time; }
};
inline float findMaxNormalizedFloat10(const float* values, int length) {
    float maximum = 0.f;
    for (int index = 0; index < length; ++index)
        maximum = std::max(maximum, std::fabs(values[index]) / 10.f);
    return maximum;
}
inline float approxExp2_taylor5(float value) {
    value += 127.f;
    const int exponent = static_cast<int>(value);
    const float fraction = value - exponent;
    union { float value; int exponent; } integerPart = {0.f};
    integerPart.exponent = exponent << 23;
    const float polynomial = 1.f + fraction * (0.69315169353961f + fraction * (0.2401595990753f
        + fraction * (0.055817908652f + fraction * (0.008991698010f + fraction * 0.001879100722f))));
    return integerPart.value * polynomial;
}
inline simd::float_4 approxExp2_taylor5(simd::float_4 value) {
    value += 127.f;
    const simd::int32_4 exponent = value;
    const simd::float_4 fraction = value - simd::float_4(exponent);
    const simd::float_4 integerPart = simd::float_4::cast(exponent << 23);
    const simd::float_4 polynomial = simd::float_4(1.f) + fraction * (0.69315169353961f + fraction
        * (0.2401595990753f + fraction * (0.055817908652f + fraction
        * (0.008991698010f + fraction * 0.001879100722f))));
    return integerPart * polynomial;
}
template <int Z, int O, typename T = float>
struct MinBlepGenerator {
    T value = T(0.f);
    void insertDiscontinuity(float, T discontinuity) { value += discontinuity; }
    T process() { T result = value; value = T(0.f); return result; }
};
template <typename T = float>
struct TRCFilter {
    T c = 0.f;
    T xstate = 0.f;
    T ystate = 0.f;
    void reset() { xstate = T(0.f); ystate = T(0.f); }
    void setCutoff(T radians) { c = T(2.f) / radians; }
    void setCutoffFreq(T frequency) { setCutoff(T(2.f * M_PI) * frequency); }
    void process(T input) {
        const T output = (input + xstate - ystate * (T(1.f) - c)) / (T(1.f) + c);
        xstate = input;
        ystate = output;
    }
    T lowpass() const { return ystate; }
    T highpass() const { return xstate - ystate; }
};
constexpr float FREQ_A4 = 440.f;

template <typename T = float>
struct TExponentialFilter {
    T out = T(0.f);
    T lambda = T(0.f);
    void reset() { out = T(0.f); }
    void setLambda(T value) { lambda = value; }
    void setTau(T tau) { lambda = T(1.f) / tau; }
    T process(T deltaTime, T in) {
        out += (in - out) * lambda * deltaTime;
        return out;
    }
};
typedef TExponentialFilter<> ExponentialFilter;

template <typename T = float>
struct TExponentialSlewLimiter {
    T out = T(0.f);
    T riseLambda = T(0.f);
    T fallLambda = T(0.f);
    void reset() { out = T(0.f); }
    void setRiseFall(T rise, T fall) { riseLambda = rise; fallLambda = fall; }
    T process(T deltaTime, T in) {
        const T delta = in - out;
        const T lambda = delta > T(0.f) ? riseLambda : fallLambda;
        out += delta * lambda * deltaTime;
        return out;
    }
    T processLinear(T, T in) { out = in; return out; }
};
typedef TExponentialSlewLimiter<> ExponentialSlewLimiter;

// Linear slew (template form of the scalar SlewLimiter in rack_dsp.h).
template <typename T>
struct TSlewLimiter {
    T out = T(0.f);
    T rise = T(1e9f);
    T fall = T(1e9f);
    void reset() { out = T(0.f); }
    void setRiseFall(T riseRate, T fallRate) { rise = riseRate; fall = fallRate; }
    T process(T deltaTime, T in) {
        T delta = in - out;
        const T riseLimit = rise * deltaTime;
        const T fallLimit = fall * deltaTime;
        if (delta > riseLimit) delta = riseLimit;
        else if (delta < -fallLimit) delta = -fallLimit;
        out += delta;
        return out;
    }
};

struct PeakFilter {
    float out = 0.f;
    float lambda = 30.f;
    void reset() { out = 0.f; }
    void setLambda(float value) { lambda = value; }
    void setTau(float tau) { lambda = 1.f / tau; }
    float process(float deltaTime, float in) {
        out = std::max(in, out + (0.f - out) * lambda * deltaTime);
        return out;
    }
};

// RBJ-cookbook biquad, API-compatible with VCV dsp::TBiquadFilter.
template <typename T = float>
struct TBiquadFilter {
    enum Type {
        LOWPASS_1POLE, HIGHPASS_1POLE, LOWPASS, HIGHPASS,
        LOWSHELF, HIGHSHELF, BANDPASS, PEAK, NOTCH, NUM_TYPES
    };
    T x1 = T(0.f), x2 = T(0.f), y1 = T(0.f), y2 = T(0.f);
    float b[3] = {1.f, 0.f, 0.f};
    float a[2] = {0.f, 0.f};
    void reset() { x1 = x2 = y1 = y2 = T(0.f); }
    T process(T in) {
        const T out = b[0] * in + b[1] * x1 + b[2] * x2 - a[0] * y1 - a[1] * y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        return out;
    }
    void setParameters(Type type, float f, float Q, float V) {
        const float K = std::tan(float(M_PI) * std::min(std::max(f, 1e-5f), 0.4999f));
        switch (type) {
            case LOWPASS_1POLE: {
                a[0] = -std::exp(-2.f * float(M_PI) * f);
                a[1] = 0.f;
                b[0] = 1.f + a[0];
                b[1] = b[2] = 0.f;
            } break;
            case HIGHPASS_1POLE: {
                a[0] = std::exp(-2.f * float(M_PI) * (0.5f - f));
                a[1] = 0.f;
                b[0] = 1.f - a[0];
                b[1] = b[2] = 0.f;
            } break;
            case LOWPASS: {
                const float norm = 1.f / (1.f + K / Q + K * K);
                b[0] = K * K * norm;
                b[1] = 2.f * b[0];
                b[2] = b[0];
                a[0] = 2.f * (K * K - 1.f) * norm;
                a[1] = (1.f - K / Q + K * K) * norm;
            } break;
            case HIGHPASS: {
                const float norm = 1.f / (1.f + K / Q + K * K);
                b[0] = norm;
                b[1] = -2.f * b[0];
                b[2] = b[0];
                a[0] = 2.f * (K * K - 1.f) * norm;
                a[1] = (1.f - K / Q + K * K) * norm;
            } break;
            case LOWSHELF: {
                const float sqrtV = std::sqrt(std::max(V, 0.f));
                if (V >= 1.f) {
                    const float norm = 1.f / (1.f + float(M_SQRT2) * K + K * K);
                    b[0] = (1.f + float(M_SQRT2) * sqrtV * K + V * K * K) * norm;
                    b[1] = 2.f * (V * K * K - 1.f) * norm;
                    b[2] = (1.f - float(M_SQRT2) * sqrtV * K + V * K * K) * norm;
                    a[0] = 2.f * (K * K - 1.f) * norm;
                    a[1] = (1.f - float(M_SQRT2) * K + K * K) * norm;
                }
                else {
                    const float invV = 1.f / std::max(V, 1e-6f);
                    const float norm = 1.f / (1.f + float(M_SQRT2) * std::sqrt(invV) * K + invV * K * K);
                    b[0] = (1.f + float(M_SQRT2) * K + K * K) * norm;
                    b[1] = 2.f * (K * K - 1.f) * norm;
                    b[2] = (1.f - float(M_SQRT2) * K + K * K) * norm;
                    a[0] = 2.f * (invV * K * K - 1.f) * norm;
                    a[1] = (1.f - float(M_SQRT2) * std::sqrt(invV) * K + invV * K * K) * norm;
                }
            } break;
            case HIGHSHELF: {
                const float sqrtV = std::sqrt(std::max(V, 0.f));
                if (V >= 1.f) {
                    const float norm = 1.f / (1.f + float(M_SQRT2) * K + K * K);
                    b[0] = (V + float(M_SQRT2) * sqrtV * K + K * K) * norm;
                    b[1] = 2.f * (K * K - V) * norm;
                    b[2] = (V - float(M_SQRT2) * sqrtV * K + K * K) * norm;
                    a[0] = 2.f * (K * K - 1.f) * norm;
                    a[1] = (1.f - float(M_SQRT2) * K + K * K) * norm;
                }
                else {
                    const float invV = 1.f / std::max(V, 1e-6f);
                    const float norm = 1.f / (invV + float(M_SQRT2) * std::sqrt(invV) * K + K * K);
                    b[0] = (1.f + float(M_SQRT2) * K + K * K) * norm;
                    b[1] = 2.f * (K * K - 1.f) * norm;
                    b[2] = (1.f - float(M_SQRT2) * K + K * K) * norm;
                    a[0] = 2.f * (K * K - invV) * norm;
                    a[1] = (invV - float(M_SQRT2) * std::sqrt(invV) * K + K * K) * norm;
                }
            } break;
            case BANDPASS: {
                const float norm = 1.f / (1.f + K / Q + K * K);
                b[0] = K / Q * norm;
                b[1] = 0.f;
                b[2] = -b[0];
                a[0] = 2.f * (K * K - 1.f) * norm;
                a[1] = (1.f - K / Q + K * K) * norm;
            } break;
            case PEAK: {
                if (V >= 1.f) {
                    const float norm = 1.f / (1.f + K / Q + K * K);
                    b[0] = (1.f + K / Q * V + K * K) * norm;
                    b[1] = 2.f * (K * K - 1.f) * norm;
                    b[2] = (1.f - K / Q * V + K * K) * norm;
                    a[0] = b[1];
                    a[1] = (1.f - K / Q + K * K) * norm;
                }
                else {
                    const float invV = 1.f / std::max(V, 1e-6f);
                    const float norm = 1.f / (1.f + K / Q * invV + K * K);
                    b[0] = (1.f + K / Q + K * K) * norm;
                    b[1] = 2.f * (K * K - 1.f) * norm;
                    b[2] = (1.f - K / Q + K * K) * norm;
                    a[0] = b[1];
                    a[1] = (1.f - K / Q * invV + K * K) * norm;
                }
            } break;
            case NOTCH: {
                const float norm = 1.f / (1.f + K / Q + K * K);
                b[0] = (1.f + K * K) * norm;
                b[1] = 2.f * (K * K - 1.f) * norm;
                b[2] = b[0];
                a[0] = b[1];
                a[1] = (1.f - K / Q + K * K) * norm;
            } break;
            default: break;
        }
    }
};
typedef TBiquadFilter<> BiquadFilter;

// Bounded FIFO of POD frames (VCV dsp::RingBuffer subset).
template <typename T, size_t S>
struct RingBuffer {
    T data[S] = {};
    size_t start = 0;
    size_t end = 0;
    size_t mask(size_t index) const { return index % S; }
    void push(T value) { data[mask(end++)] = value; }
    void pushBuffer(const T* values, int count) {
        for (int index = 0; index < count; ++index) push(values[index]);
    }
    T shift() { return data[mask(start++)]; }
    void shiftBuffer(T* values, size_t count) {
        for (size_t index = 0; index < count; ++index) values[index] = shift();
    }
    void clear() { start = end; }
    bool empty() const { return start == end; }
    bool full() const { return end - start == S; }
    size_t size() const { return end - start; }
    size_t capacity() const { return S - size(); }
};

// Double-buffered ring so a consumer can read a contiguous block
// (VCV dsp::DoubleRingBuffer subset).
template <typename T, size_t S>
struct DoubleRingBuffer {
    T data[S * 2] = {};
    size_t start = 0;
    size_t end = 0;
    size_t mask(size_t index) const { return index % S; }
    void push(T value) {
        const size_t index = mask(end++);
        data[index] = value;
        data[index + S] = value;
    }
    T shift() { return data[mask(start++)]; }
    void clear() { start = end; }
    bool empty() const { return start == end; }
    bool full() const { return end - start == S; }
    size_t size() const { return end - start; }
    size_t capacity() const { return S - size(); }
    T* endData() { return &data[mask(end)]; }
    void endIncr(size_t count) {
        const size_t index = mask(end);
        const size_t copied = std::min(count, S - index);
        for (size_t offset = 0; offset < copied; ++offset)
            data[index + S + offset] = data[index + offset];
        end += count;
    }
    const T* startData() const { return &data[mask(start)]; }
    void startIncr(size_t count) { start += count; }
};

template <int CHANNELS>
struct Frame {
    float samples[CHANNELS] = {};
};

// Linear-interpolating resampler with the VCV SampleRateConverter API.  Not
// polyphase like the speex original, but adequate for bridge DSP.
template <int CHANNELS>
struct SampleRateConverter {
    int inRate = 44100;
    int outRate = 44100;
    double phase = 0.0;
    Frame<CHANNELS> last = {};
    bool primed = false;
    void setRates(int in, int out) {
        if (in > 0) inRate = in;
        if (out > 0) outRate = out;
    }
    void setRatio(float ratio) {
        if (ratio > 0.f) { inRate = 48000; outRate = static_cast<int>(48000.f * ratio); }
    }
    void setRatioSmooth(float ratio) { setRatio(ratio); }
    void setQuality(int) {}
    void setChannels(int) {}
    void refreshState() { phase = 0.0; primed = false; }
    void process(const Frame<CHANNELS>* input, int* inFrames, Frame<CHANNELS>* output, int* outFrames) {
        const double step = outRate > 0 ? static_cast<double>(inRate) / outRate : 1.0;
        int consumed = 0;
        int produced = 0;
        const int inCount = *inFrames;
        const int outCount = *outFrames;
        if (!primed && inCount > 0) {
            last = input[0];
            primed = true;
        }
        while (produced < outCount) {
            while (phase >= 1.0 && consumed < inCount) {
                last = input[consumed++];
                phase -= 1.0;
            }
            if (phase >= 1.0) break;
            const Frame<CHANNELS>& next = consumed < inCount ? input[consumed] : last;
            const float mix = static_cast<float>(phase);
            for (int channel = 0; channel < CHANNELS; ++channel)
                output[produced].samples[channel] =
                    last.samples[channel] + (next.samples[channel] - last.samples[channel]) * mix;
            ++produced;
            phase += step;
        }
        while (phase >= 1.0 && consumed < inCount) {
            last = input[consumed++];
            phase -= 1.0;
        }
        *inFrames = consumed;
        *outFrames = produced;
    }
};

struct VuMeter2 {
    enum Mode { PEAK, RMS };
    Mode mode = PEAK;
    float v = 0.f;
    void reset() { v = 0.f; }
    void process(float deltaTime, float value) {
        if (mode == RMS) {
            const float lambda = 30.f;
            v += (value * value - v) * lambda * deltaTime;
        }
        else {
            value = std::fabs(value);
            if (value >= v) v = value;
            else {
                const float lambda = 30.f;
                v += (value - v) * lambda * deltaTime;
            }
        }
    }
    float getBrightness(float dbMin, float dbMax) const {
        const float amplitude = mode == RMS ? std::sqrt(std::fmax(v, 0.f)) : v;
        if (amplitude <= 0.f) return 0.f;
        const float db = 20.f * std::log10(amplitude);
        if (db >= dbMax) return 1.f;
        if (db <= dbMin) return 0.f;
        return (db - dbMin) / (dbMax - dbMin);
    }
};

// VCV v1 VuMeter (band-based)
struct VuMeter {
    float dBInterval = 3.f;
    float dBScaled = -100.f;
    void setValue(float value) {
        dBScaled = value > 0.f ? 20.f * std::log10(value) / dBInterval : -100.f;
    }
    float getBrightness(int band) const {
        const float low = -band - 1;
        const float high = -band;
        if (dBScaled >= high) return 1.f;
        if (dBScaled <= low) return 0.f;
        return dBScaled - low;
    }
};

template <typename T, typename Callback>
void stepRK4(T time, T deltaTime, T state[], int length, Callback callback) {
    if (length <= 0 || length > 64) return;
    std::array<T, 64> first = {};
    std::array<T, 64> second = {};
    std::array<T, 64> third = {};
    std::array<T, 64> fourth = {};
    std::array<T, 64> next = {};
    callback(time, state, first.data());
    for (int index = 0; index < length; ++index) next[index] = state[index] + first[index] * deltaTime / T(2);
    callback(time + deltaTime / T(2), next.data(), second.data());
    for (int index = 0; index < length; ++index) next[index] = state[index] + second[index] * deltaTime / T(2);
    callback(time + deltaTime / T(2), next.data(), third.data());
    for (int index = 0; index < length; ++index) next[index] = state[index] + third[index] * deltaTime;
    callback(time + deltaTime, next.data(), fourth.data());
    for (int index = 0; index < length; ++index)
        state[index] += (first[index] + T(2) * second[index] + T(2) * third[index] + fourth[index]) * deltaTime / T(6);
}
}

}

#endif
