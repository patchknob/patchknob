//----------------------------------------------------------------------------
//  PatchKnob — per-track / master VU meter.
//
//  VuMeter computes a PEAK and an RMS level from blocks of float audio pushed
//  in from the audio thread, and exposes both as lock-free atomics for the UI
//  (message) thread.
//
//  Ballistics:
//    * attack  : instantaneous — a louder sample/block snaps the meter up.
//    * release : exponential decay with a ~300 ms time constant, so the meter
//                falls smoothly after the signal drops.
//
//  Threading:
//    * push()        : audio thread only. Lock-free, allocation-free.
//    * peak()/rms()  : any thread. Plain relaxed atomic loads.
//
//  The decay is applied per-push() using the number of frames pushed and the
//  sample rate set via setSampleRate(), so the time constant is independent of
//  block size.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_GRAPH_VU_METER_H
#define PATCHKNOB_ENGINE_GRAPH_VU_METER_H

#include <atomic>
#include <cmath>

namespace PatchKnob { namespace engine {

class VuMeter {
public:
    VuMeter() = default;

    //! Set the sample rate so the release time constant is correct (message
    //! thread, at prepare() time). Default 48 kHz until set.
    void setSampleRate(double sr) {
        if (sr > 0.0) sampleRate_ = sr;
        recomputeCoeff();
    }

    //! Reset both meters to silence (message thread, e.g. on transport stop).
    void reset() {
        peak_.store(0.0f, std::memory_order_relaxed);
        rms_.store(0.0f,  std::memory_order_relaxed);
        rmsState_ = 0.0f;
        peakState_ = 0.0f;
    }

    //! Feed one block of `n` mono samples (audio thread). Updates peak + RMS
    //! with instant attack and ~300 ms exponential release.
    void push(const float* buf, int n) {
        if (!buf || n <= 0) return;

        // Block peak and block mean-square.
        float blockPeak = 0.0f;
        double sumSq = 0.0;
        for (int i = 0; i < n; ++i) {
            const float a = std::fabs(buf[i]);
            if (a > blockPeak) blockPeak = a;
            sumSq += (double)buf[i] * (double)buf[i];
        }
        const float blockRms = (n > 0) ? (float)std::sqrt(sumSq / (double)n) : 0.0f;

        // Per-block release coefficient: coeff_ is per-sample, raise to n.
        const float decay = std::pow(coeff_, (float)n);

        // PEAK: instant attack, exponential release.
        float p = peakState_ * decay;
        if (blockPeak > p) p = blockPeak;       // attack: snap up
        peakState_ = p;
        peak_.store(p, std::memory_order_relaxed);

        // RMS: instant attack, exponential release.
        float r = rmsState_ * decay;
        if (blockRms > r) r = blockRms;
        rmsState_ = r;
        rms_.store(r, std::memory_order_relaxed);
    }

    //! Current peak level (any thread). 0..~1 (can exceed 1 on overs).
    float peak() const { return peak_.load(std::memory_order_relaxed); }

    //! Current RMS level (any thread). 0..~1.
    float rms() const { return rms_.load(std::memory_order_relaxed); }

private:
    void recomputeCoeff() {
        // Exponential decay reaching ~ -60 dB? We use a standard time-constant
        // formulation: y[n] = y[n-1] * coeff, where coeff = exp(-1 / (tau*sr)).
        // tau = 0.300 s gives a smooth fall (one time constant ~= -8.7 dB).
        const double tau = 0.300; // 300 ms release time constant
        coeff_ = (float)std::exp(-1.0 / (tau * sampleRate_));
    }

    double sampleRate_ = 48000.0;
    float  coeff_      = 0.99993f;   // recomputed in setSampleRate()

    // Audio-thread-private smoothed state (not shared; no atomics needed).
    float peakState_ = 0.0f;
    float rmsState_  = 0.0f;

    // Published values for the UI thread.
    std::atomic<float> peak_{0.0f};
    std::atomic<float> rms_{0.0f};
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_GRAPH_VU_METER_H
