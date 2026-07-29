//----------------------------------------------------------------------------
//  src/engine/rack/rack_dsp.h
//
//  The rack::dsp helpers that module DSP commonly uses -- API-compatible with
//  VCV Rack's <dsp/...> headers, but a compact scalar subset (no SIMD).  Enough
//  for the built-in module set (VCO/VCF/VCA/ADSR/LFO/...) and for porting many
//  Fundamental-style modules unchanged.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_DSP_H
#define PATCHKNOB_ENGINE_RACK_DSP_H

#include <cmath>
#include <cstring>

namespace rack { namespace dsp {

// ---- gate/trigger edge detectors -------------------------------------------
// Rising-edge detector with hysteresis (VCV SchmittTrigger).  process() returns
// true on a low->high crossing.  Feed gates rescaled to ~0..1, or pass explicit
// thresholds (gates are 0/10V, so callers often pass 0.1f, 2.f).
// VCV v2 semantics: `state` is a public bool that starts high so a gate held
// high at startup does not fire a spurious trigger.  Plugins subclass this and
// poke `state` directly (the Impromptu-style Trigger idiom), so the member
// must stay a bool.
struct SchmittTrigger {
    bool state = true;
    void reset() { state = true; }
    bool process(float in, float lowThresh = 0.f, float highThresh = 1.f) {
        if (state) {
            if (in <= lowThresh) state = false;
        }
        else if (in >= highThresh) {
            state = true;
            return true;
        }
        return false;
    }
    bool isHigh() const { return state; }
};

// Fires on ANY change from 0 (VCV BooleanTrigger).
struct BooleanTrigger {
    bool state = false;
    bool process(float in) { bool on = in >= 1.f; bool fired = on && !state; state = on; return fired; }
};

// Reset contract shared by sequencers: a reset selects index 0 and consumes
// the next clock edge, preventing a coincident clock from displaying step 2.
struct SequencerReset {
    bool awaitingClock = false;
    void reset() { awaitingClock = true; }
    void clear() { awaitingClock = false; }
    bool processClock(bool edge) {
        if (!edge) return false;
        if (awaitingClock) { awaitingClock = false; return false; }
        return true;
    }
};

// ---- pulse generator (VCV PulseGenerator) ----------------------------------
struct PulseGenerator {
    float remaining = 0.f;
    void  reset() { remaining = 0.f; }
    bool  process(float dt) { if (remaining > 0.f) { remaining -= dt; return true; } return false; }
    void  trigger(float duration = 1e-3f) { if (duration > remaining) remaining = duration; }
};

// ---- one-pole / RC lowpass & highpass (VCV RCFilter-ish) --------------------
struct RCFilter {
    float c = 0.f, xstate = 0.f, ystate = 0.f;
    // f = cutoff / sampleRate  (normalized)
    void setCutoff(float fNorm) { c = 2.f / std::tan(float(M_PI) * clampf(fNorm, 1e-5f, 0.49f)); }
    void setCutoffFreq(float fNorm) { setCutoff(fNorm); }
    void process(float x) {
        float y = (x + xstate - ystate * (1.f - c)) / (1.f + c);
        xstate = x; ystate = y;
    }
    float lowpass()  const { return ystate; }
    float highpass() const { return xstate - ystate; }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

struct OnePole {   // simple leaky integrator lowpass, a = smoothing 0..1
    float y = 0.f;
    float process(float x, float a) { y += a * (x - y); return y; }
    void  reset(float v = 0.f) { y = v; }
};

// ---- exponential slew limiter (VCV ExponentialSlewLimiter-ish) -------------
struct SlewLimiter {
    float out = 0.f;
    float rise = 1e9f, fall = 1e9f;    // volts/second
    void  setRiseFall(float r, float f) { rise = r; fall = f; }
    float process(float dt, float in) {
        float d = in - out;
        float lim = (d > 0.f) ? rise * dt : fall * dt;
        if (d > lim) d = lim; else if (d < -lim) d = -lim;
        out += d; return out;
    }
    void reset(float v = 0.f) { out = v; }
};

// ---- polyBLEP: 1-sample band-limited step correction ------------------------
// Add to a naive saw/square at each discontinuity to suppress aliasing.
//   t  = phase 0..1 of the discontinuity relative to now
//   dt = phase increment per sample (freq/sampleRate)
inline float polyBlep(float t, float dt) {
    if (t < dt)        { t /= dt;        return t + t - t * t - 1.f; }
    if (t > 1.f - dt)  { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}

// A minimal band-limited oscillator core (naive ramp + polyBLEP correction).
// Produces saw / square / triangle-ish / sine from one running phase.
struct BlepOsc {
    float phase = 0.f;     // 0..1
    float lastSaw = 0.f;
    void  reset() { phase = 0.f; lastSaw = 0.f; }
    // advance one sample at normalized frequency dt = freq/sr; call outputs after.
    void  advance(float dt) { phase += dt; if (phase >= 1.f) phase -= 1.f; if (phase < 0.f) phase += 1.f; }
    float saw(float dt) const {
        float v = 2.f * phase - 1.f;
        v -= polyBlep(phase, dt);
        return v;
    }
    float square(float dt, float pw = 0.5f) const {
        float v = (phase < pw) ? 1.f : -1.f;
        v += polyBlep(phase, dt);
        float t2 = phase + (1.f - pw); if (t2 >= 1.f) t2 -= 1.f;
        v -= polyBlep(t2, dt);
        return v;
    }
    float sine() const { return std::sin(2.f * float(M_PI) * phase); }
    float triangle() const { return 4.f * std::fabs(phase - 0.5f) - 1.f; }
};

// ---- fast-ish exp2 for pitch (good to a few cents) --------------------------
inline float exp2_taylor(float x) { return std::pow(2.f, x); }

}} // namespace rack::dsp

#endif
