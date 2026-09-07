#include "native_effects.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace PatchKnob { namespace engine {
namespace {

static float clamp01(float v) { return std::max(0.f, std::min(1.f, v)); }
static float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
static float linToDb(float v) { return 20.f * std::log10(std::max(v, 1.e-9f)); }

class NativeBase : public IPluginInstance {
public:
    explicit NativeBase(PluginDescriptor d) : desc_(std::move(d)) {}
    const PluginDescriptor& descriptor() const override { return desc_; }
    void setActive(bool v) override { active_.store(v, std::memory_order_release); }
    void release() override { active_.store(false, std::memory_order_release); }
    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}
    std::vector<uint8_t> saveState() const override {
        std::vector<uint8_t> out((size_t)paramCount() * sizeof(float));
        for (int i=0; i<paramCount(); ++i) {
            const float v=getParamNormalized(paramInfo(i).id);
            std::memcpy(out.data() + (size_t)i*sizeof(float), &v, sizeof(v));
        }
        return out;
    }
    void loadState(const std::vector<uint8_t>& in) override {
        const int n=std::min(paramCount(), (int)(in.size()/sizeof(float)));
        for (int i=0; i<n; ++i) { float v; std::memcpy(&v,in.data()+(size_t)i*sizeof(float),sizeof(v)); setParamNormalized(paramInfo(i).id,v); }
    }
protected:
    void applyAutomation(const ProcessBlock& b) {
        for (int i=0;i<b.numParamIn;++i) setParamNormalized(b.paramIn[i].id,b.paramIn[i].value);
    }
    PluginDescriptor desc_;
    std::atomic<bool> active_{true};
};

// PatchKnob Compressor -- an ORIGINAL implementation, written from the standard
// feed-forward design published in Giannoulis, Massberg & Reiss, "Digital
// Dynamic Range Compressor Design: A Tutorial and Analysis" (JAES 60(6), 2012).
// It replaces an earlier compressor whose DSP was derived from an AGPL-3.0
// project; none of that code survives here, which is what lets PatchKnob ship
// under GPL-3.0.
//
// It also fixes what never worked: the old "Sidechain HPF" filtered the MAIN
// signal's detector and called it a sidechain -- there was no key input at all.
// Here the detector reads a real external sidechain when one is patched, falls
// back to the main input when it is not, and has its OWN attack/release so the
// key can be shaped independently of the gain smoothing.
//
// Why this is more than a level follower:
//   * Gain is computed in the LOG domain and the smoothing is applied to the
//     GAIN-REDUCTION curve, not to a rectified linear envelope.  Smoothing a
//     linear envelope makes attack/release level-dependent -- the pumping that
//     makes a compressor sound cheap.
//   * SOFT KNEE: quadratic interpolation across a width around the threshold,
//     so reduction eases in instead of switching on.
//   * DECOUPLED detector: attack and release branch on whether more or less
//     reduction is being asked for, so the release constant cannot smear a
//     fast attack.
//   * LOOKAHEAD: the audio is delayed while the detector runs early, so a
//     transient is caught rather than passing through before the envelope
//     responds.
//   * STEREO LINK applies ONE gain to both channels, derived from the louder,
//     so a hard-panned hit cannot shift the stereo image.
//   * PARALLEL MIX for New-York-style compression without a second track.
class PatchKnobCompressor final : public NativeBase {
public:
    explicit PatchKnobCompressor(PluginDescriptor d) : NativeBase(std::move(d)) {
        const float defaults[kNumParams] = {
            0.70f,        // Threshold    -> -18 dB   (-60..0)
            1.f/3.f,      // Ratio        -> 4:1      (1..10)
            10.f/49.9f,   // Attack       -> 10 ms    (0.1..50)
            110.f/490.f,  // Release      -> 120 ms   (10..500)
            0.f,          // Makeup       -> 0 dB     (0..30)
            0.f,          // Detector HPF -> off      (0..500 Hz)
            1.f,          // Stereo Link  -> on
            1.f,          // Compressor On
            6.f/24.f,     // Knee         -> 6 dB     (0..24)
            5.f/20.f,     // Lookahead    -> 5 ms     (0..20)
            0.f,          // SC Attack    -> 0 ms     (0..50)   detector shaping
            20.f/500.f,   // SC Release   -> 20 ms    (0..500)
            1.f           // Mix          -> 100% wet (0..1)
        };
        for (int i = 0; i < kNumParams; ++i) p_[i].store(defaults[i]);
    }

    bool prepare(double sr, int) override {
        sr_ = sr > 1 ? sr : 48000.;
        //  Sized for the maximum lookahead the UI can request, so moving that
        //  control while playing never allocates on the audio thread.
        const size_t maxLook = (size_t)(0.020 * sr_) + 4;
        for (int c = 0; c < 2; ++c) { look_[c].assign(maxLook, 0.f); hp_[c] = 0.f; }
        wp_ = 0; envDb_ = 0.f; scEnv_ = 0.f;
        return true;
    }

    int paramCount() const override { return kNumParams; }
    ParamInfo paramInfo(int i) const override {
        static const char* n[kNumParams] = {
            "Threshold", "Ratio", "Attack", "Release", "Makeup", "Detector HPF",
            "Stereo Link", "Compressor On", "Knee", "Lookahead",
            "SC Attack", "SC Release", "Mix"
        };
        const int k = std::max(0, std::min(kNumParams - 1, i));
        return { (uint32_t)k, n[k], p_[k].load() };
    }
    float getParamNormalized(uint32_t id) const override {
        return id < kNumParams ? p_[id].load() : 0.f;
    }
    void setParamNormalized(uint32_t id, float v) override {
        if (id < kNumParams) p_[id].store(clamp01(v));
    }

    void process(const ProcessBlock& b) override {
        applyAutomation(b);
        if (!active_.load() || !b.audioOut || b.nframes <= 0) return;

        const float thDb   = -60.f + 60.f * p_[0].load();
        const float ratio  = 1.f + 9.f * p_[1].load();
        const float attMs  = 0.1f + 49.9f * p_[2].load();
        const float relMs  = 10.f + 490.f * p_[3].load();
        const float makeDb = 30.f * p_[4].load();
        const float hpHz   = 500.f * p_[5].load();
        const bool  link   = p_[6].load() >= .5f;
        const bool  on     = p_[7].load() >= .5f;
        const float kneeDb = 24.f * p_[8].load();
        const float scAtMs = 50.f * p_[10].load();
        const float scRlMs = 500.f * p_[11].load();
        const float mix    = clamp01(p_[12].load());

        const float aAtt = std::exp(-1.f / (attMs * 0.001f * (float)sr_));
        const float aRel = std::exp(-1.f / (relMs * 0.001f * (float)sr_));
        const float sAtt = scAtMs > 0.f ? std::exp(-1.f / (scAtMs * 0.001f * (float)sr_)) : 0.f;
        const float sRel = scRlMs > 0.f ? std::exp(-1.f / (scRlMs * 0.001f * (float)sr_)) : 0.f;
        const float invR = 1.f / std::max(1.f, ratio);
        const float hpA  = hpHz > 0.f ? std::exp(-6.2831853f * hpHz / (float)sr_) : 0.f;
        const size_t ring = look_[0].empty() ? 1 : look_[0].size();
        const int   look = std::max(0, std::min((int)ring - 1,
                                    (int)(0.020f * p_[9].load() * (float)sr_)));

        //  A REAL external sidechain: inputs beyond the main stereo pair.
        const bool haveKey = b.audioIn && b.numAudioIn >= 3 && b.audioIn[2];

        float maxGr = 0.f;
        for (int i = 0; i < b.nframes; ++i) {
            const float inL = (b.audioIn && b.numAudioIn > 0 && b.audioIn[0]) ? b.audioIn[0][i] : 0.f;
            const float inR = (b.audioIn && b.numAudioIn > 1 && b.audioIn[1]) ? b.audioIn[1][i] : inL;

            float keyL = haveKey ? b.audioIn[2][i] : inL;
            float keyR = (haveKey && b.numAudioIn > 3 && b.audioIn[3]) ? b.audioIn[3][i]
                       : (haveKey ? keyL : inR);

            //  Detector high-pass on the KEY only, so it never colours the
            //  audio path -- this is what stops a kick ducking the whole mix.
            if (hpA > 0.f) {
                hp_[0] = (1.f - hpA) * keyL + hpA * hp_[0]; keyL -= hp_[0];
                hp_[1] = (1.f - hpA) * keyR + hpA * hp_[1]; keyR -= hp_[1];
            }

            float key = link ? std::max(std::fabs(keyL), std::fabs(keyR))
                             : 0.5f * (std::fabs(keyL) + std::fabs(keyR));

            //  SIDECHAIN ENVELOPE, with its own timing.  Shaping the key before
            //  the gain computer is what lets a slow key ride a fast programme
            //  (or the reverse) -- independent of the gain smoothing below.
            if (sAtt > 0.f || sRel > 0.f) {
                scEnv_ = (key > scEnv_) ? sAtt * scEnv_ + (1.f - sAtt) * key
                                        : sRel * scEnv_ + (1.f - sRel) * key;
                key = scEnv_;
            }

            const float xDb = 20.f * std::log10(std::max(key, 1e-9f));

            //  Soft-knee gain computer.
            float yDb;
            const float over = xDb - thDb;
            if (kneeDb > 0.f && 2.f * over > -kneeDb && 2.f * over < kneeDb) {
                const float t = over + kneeDb * 0.5f;
                yDb = xDb + (invR - 1.f) * t * t / (2.f * kneeDb);
            } else if (2.f * over >= kneeDb) {
                yDb = thDb + over * invR;
            } else {
                yDb = xDb;
            }
            const float cDb = on ? std::min(0.f, yDb - xDb) : 0.f;

            //  Decoupled smoothing of the REDUCTION, in dB.
            envDb_ = (cDb < envDb_) ? aAtt * envDb_ + (1.f - aAtt) * cDb
                                    : aRel * envDb_ + (1.f - aRel) * cDb;

            const float gain = dbToLin(envDb_ + (on ? makeDb : 0.f));
            if (-envDb_ > maxGr) maxGr = -envDb_;

            //  Lookahead: the gain landing on a transient was computed before
            //  that transient arrived.
            look_[0][(size_t)wp_] = inL;
            look_[1][(size_t)wp_] = inR;
            const size_t rp = ((size_t)wp_ + ring - (size_t)look) % ring;
            wp_ = (int)(((size_t)wp_ + 1) % ring);

            const float dryL = look_[0][rp], dryR = look_[1][rp];
            if (b.numAudioOut > 0) b.audioOut[0][i] = dryL * (1.f - mix) + dryL * gain * mix;
            if (b.numAudioOut > 1) b.audioOut[1][i] = dryR * (1.f - mix) + dryR * gain * mix;
        }
        gr_.store(maxGr, std::memory_order_relaxed);
    }

private:
    static constexpr int kNumParams = 13;
    std::atomic<float> p_[kNumParams], gr_{0};
    double sr_ = 48000.;
    std::vector<float> look_[2];
    int   wp_ = 0;
    float envDb_ = 0.f, scEnv_ = 0.f, hp_[2]{};
};

// PatchKnob Transient Shaper -- original, GPL-3.0.
//
// Level-independent by construction: two envelope followers track the same
// signal at different speeds, and their DIFFERENCE is the transient.  A fast
// envelope rising above a slow one means an attack is happening; the slow one
// sitting above the fast one means the sound is decaying.  Because the control
// signal is a RATIO of the two, it does not care how loud the source is -- a
// quiet ghost note gets the same treatment as an accent, which is exactly what
// a threshold-based design cannot do.
class PatchKnobTransient final : public NativeBase {
public:
    explicit PatchKnobTransient(PluginDescriptor d) : NativeBase(std::move(d)) {
        const float defaults[kNumParams] = {
            0.5f,       // Attack   -> 0 (centre = neutral; 0..1 maps -12..+12 dB)
            0.5f,       // Sustain  -> 0
            0.35f,      // Speed    -> envelope pair spacing
            0.5f,       // Output   -> 0 dB (maps -12..+12)
            1.f         // On
        };
        for (int i = 0; i < kNumParams; ++i) p_[i].store(defaults[i]);
    }
    bool prepare(double sr, int) override {
        sr_ = sr > 1 ? sr : 48000.;
        fast_[0] = fast_[1] = slow_[0] = slow_[1] = 0.f;
        return true;
    }
    int paramCount() const override { return kNumParams; }
    ParamInfo paramInfo(int i) const override {
        static const char* n[kNumParams] = { "Attack", "Sustain", "Speed", "Output", "On" };
        const int k = std::max(0, std::min(kNumParams - 1, i));
        return { (uint32_t)k, n[k], p_[k].load() };
    }
    float getParamNormalized(uint32_t id) const override {
        return id < kNumParams ? p_[id].load() : 0.f;
    }
    void setParamNormalized(uint32_t id, float v) override {
        if (id < kNumParams) p_[id].store(clamp01(v));
    }
    void process(const ProcessBlock& b) override {
        applyAutomation(b);
        if (!active_.load() || !b.audioOut || b.nframes <= 0) return;

        const float atkAmt = (p_[0].load() - 0.5f) * 2.f;    // -1..+1
        const float susAmt = (p_[1].load() - 0.5f) * 2.f;
        const float speed  = p_[2].load();
        const float outDb  = (p_[3].load() - 0.5f) * 24.f;
        const bool  on     = p_[4].load() >= .5f;
        if (!on) {
            for (int c = 0; c < std::min(2, b.numAudioOut); ++c)
                for (int i = 0; i < b.nframes; ++i)
                    b.audioOut[c][i] = (b.audioIn && c < b.numAudioIn && b.audioIn[c]) ? b.audioIn[c][i] : 0.f;
            return;
        }

        //  The two followers.  `speed` slides the pair together: slower makes
        //  the shaper respond to phrases, faster to individual hits.
        const float fastMs = 0.5f + 4.5f * speed;            // 0.5..5 ms
        const float slowMs = 20.f + 180.f * speed;           // 20..200 ms
        const float aF = std::exp(-1.f / (fastMs * 0.001f * (float)sr_));
        const float aS = std::exp(-1.f / (slowMs * 0.001f * (float)sr_));
        const float outLin = dbToLin(outDb);

        for (int i = 0; i < b.nframes; ++i) {
            for (int c = 0; c < 2; ++c) {
                const float x = (b.audioIn && c < b.numAudioIn && b.audioIn[c]) ? b.audioIn[c][i] : 0.f;
                const float r = std::fabs(x);
                fast_[c] = (r > fast_[c]) ? r : aF * fast_[c] + (1.f - aF) * r;
                slow_[c] = aS * slow_[c] + (1.f - aS) * r;

                //  Transient measure in dB: how far the fast follower is above
                //  (attack) or below (sustain) the slow one.  Clamped so a
                //  near-silent passage cannot produce a huge ratio.
                const float dDb = 20.f * std::log10(std::max(fast_[c], 1e-9f) /
                                                    std::max(slow_[c], 1e-9f));
                const float t = std::max(-12.f, std::min(12.f, dDb)) / 12.f;   // -1..+1
                //  Attack acts on the rising part, sustain on the falling part.
                const float gDb = (t > 0.f ? t * atkAmt : -t * susAmt) * 12.f;
                const float g = dbToLin(gDb) * outLin;
                if (c < b.numAudioOut) b.audioOut[c][i] = x * g;
            }
        }
    }
private:
    static constexpr int kNumParams = 5;
    std::atomic<float> p_[kNumParams];
    double sr_ = 48000.;
    float fast_[2]{}, slow_[2]{};
};

// PatchKnob Drum Bus -- original, GPL-3.0.  A drum-bus channel strip in the
// spirit of Ableton's Drum Buss: one control per job, arranged in the order the
// signal actually wants them.
//
//   DRIVE    soft asymmetric saturation.  tanh-shaped, so it compresses peaks
//            and adds low-order harmonics rather than clipping.
//   CRUNCH   a harder, band-limited distortion applied to the UPPER band only,
//            which is what gives snares bite without turning kicks to mush.
//   DAMP     one-pole low-pass on the output, to take the edge off whatever
//            drive and crunch just added.
//   BOOM     a tuned resonant low-frequency voice, excited by the signal's own
//            transients and decaying on its own -- this is the sub-thump, and
//            it is generated rather than boosted, so it works on material that
//            has no low end to lift.
//   TRANSIENTS  the same two-envelope shaper as the standalone effect, so a
//            single knob moves attack and sustain in opposite directions.
//   COMPRESS a fixed-character bus compressor: 4:1, soft knee, timings chosen
//            for drums, with automatic makeup so the knob only ever gets
//            louder-and-denser rather than needing a second gain stage.
//
// Band split is a single one-pole crossover at 200 Hz: cheap, phase-coherent
// on sum, and exactly enough to keep crunch off the kick.
class PatchKnobDrumBus final : public NativeBase {
public:
    explicit PatchKnobDrumBus(PluginDescriptor d) : NativeBase(std::move(d)) {
        const float defaults[kNumParams] = {
            0.f,     // Drive
            0.f,     // Crunch
            0.f,     // Damp
            0.f,     // Boom
            0.f,     // Boom Decay  (0..1 -> 60..600 ms)
            0.35f,   // Boom Freq   (0..1 -> 40..120 Hz)
            0.5f,    // Transients  (centre = neutral)
            0.f,     // Compress
            1.f,     // Dry/Wet
            1.f      // On
        };
        for (int i = 0; i < kNumParams; ++i) p_[i].store(defaults[i]);
    }
    bool prepare(double sr, int) override {
        sr_ = sr > 1 ? sr : 48000.;
        for (int c = 0; c < 2; ++c) {
            lp_[c] = damp_[c] = fast_[c] = slow_[c] = 0.f;
            boomA_[c] = boomB_[c] = 0.f;
        }
        env_ = 0.f; prevRect_ = 0.f;
        return true;
    }
    int paramCount() const override { return kNumParams; }
    ParamInfo paramInfo(int i) const override {
        static const char* n[kNumParams] = {
            "Drive", "Crunch", "Damp", "Boom", "Boom Decay", "Boom Freq",
            "Transients", "Compress", "Dry/Wet", "On"
        };
        const int k = std::max(0, std::min(kNumParams - 1, i));
        return { (uint32_t)k, n[k], p_[k].load() };
    }
    float getParamNormalized(uint32_t id) const override {
        return id < kNumParams ? p_[id].load() : 0.f;
    }
    void setParamNormalized(uint32_t id, float v) override {
        if (id < kNumParams) p_[id].store(clamp01(v));
    }
    void process(const ProcessBlock& b) override {
        applyAutomation(b);
        if (!active_.load() || !b.audioOut || b.nframes <= 0) return;

        const float drive  = p_[0].load();
        const float crunch = p_[1].load();
        const float damp   = p_[2].load();
        const float boom   = p_[3].load();
        const float boomDk = 0.060f + 0.540f * p_[4].load();          // 60..600 ms
        const float boomHz = 40.f + 80.f * p_[5].load();              // 40..120 Hz
        const float trans  = (p_[6].load() - 0.5f) * 2.f;             // -1..+1
        const float comp   = p_[7].load();
        const float wet    = clamp01(p_[8].load());
        const bool  on     = p_[9].load() >= .5f;

        //  200 Hz crossover, damping low-pass, and the drum-tuned compressor.
        const float aSplit = std::exp(-6.2831853f * 200.f / (float)sr_);
        const float dampHz = 20000.f - 17000.f * damp;                // 20k..3k
        const float aDamp  = std::exp(-6.2831853f * dampHz / (float)sr_);
        const float aF = std::exp(-1.f / (0.002f * (float)sr_));      // 2 ms
        const float aS = std::exp(-1.f / (0.080f * (float)sr_));      // 80 ms
        const float cAtt = std::exp(-1.f / (0.005f * (float)sr_));    // 5 ms
        const float cRel = std::exp(-1.f / (0.120f * (float)sr_));    // 120 ms
        //  Compress: threshold walks down as the knob comes up, with matching
        //  automatic makeup so the control only ever adds density.
        const float thDb = -6.f - 24.f * comp;
        const float mkDb = 12.f * comp;
        const float invR = 1.f / 4.f;                                 // fixed 4:1
        const float kneeDb = 6.f;
        //  Boom resonator: a decaying two-pole tuned to boomHz, excited by
        //  transients in the signal.
        const float w = 6.2831853f * boomHz / (float)sr_;
        const float r = std::exp(-1.f / (boomDk * (float)sr_));
        const float c1 = 2.f * r * std::cos(w), c2 = -r * r;
        const float driveGain = 1.f + 9.f * drive;

        for (int i = 0; i < b.nframes; ++i) {
            float outCh[2];
            //  One control signal for both channels, so the bus stays glued.
            float rectMax = 0.f;
            for (int c = 0; c < 2; ++c) {
                const float x = (b.audioIn && c < b.numAudioIn && b.audioIn[c]) ? b.audioIn[c][i] : 0.f;
                rectMax = std::max(rectMax, std::fabs(x));
            }

            //  Transient measure, shared across channels.
            fast_[0] = (rectMax > fast_[0]) ? rectMax : aF * fast_[0] + (1.f - aF) * rectMax;
            slow_[0] = aS * slow_[0] + (1.f - aS) * rectMax;
            const float tDb = 20.f * std::log10(std::max(fast_[0], 1e-9f) /
                                                std::max(slow_[0], 1e-9f));
            const float tN = std::max(-12.f, std::min(12.f, tDb)) / 12.f;
            const float transG = dbToLin((tN > 0.f ? tN * trans : -tN * trans) * 9.f);

            //  Compressor control from the same rectified signal.
            const float xDb = 20.f * std::log10(std::max(rectMax, 1e-9f));
            float yDb; const float over = xDb - thDb;
            if (2.f * over > -kneeDb && 2.f * over < kneeDb) {
                const float t = over + kneeDb * 0.5f;
                yDb = xDb + (invR - 1.f) * t * t / (2.f * kneeDb);
            } else if (2.f * over >= kneeDb) { yDb = thDb + over * invR; }
            else { yDb = xDb; }
            const float cDb = (comp > 0.f) ? std::min(0.f, yDb - xDb) : 0.f;
            env_ = (cDb < env_) ? cAtt * env_ + (1.f - cAtt) * cDb
                                : cRel * env_ + (1.f - cRel) * cDb;
            const float compG = dbToLin(env_ + mkDb);

            //  Boom excitation: the RISE in the rectified signal, so the
            //  resonator is struck by hits rather than driven by level.
            const float exc = std::max(0.f, rectMax - prevRect_);
            prevRect_ = rectMax;

            for (int c = 0; c < 2; ++c) {
                float x = (b.audioIn && c < b.numAudioIn && b.audioIn[c]) ? b.audioIn[c][i] : 0.f;
                const float dry = x;

                //  Split at 200 Hz.
                lp_[c] = (1.f - aSplit) * x + aSplit * lp_[c];
                const float lo = lp_[c], hi = x - lo;

                //  DRIVE on the whole signal, CRUNCH on the top band only.
                float y = std::tanh(x * driveGain) / std::tanh(driveGain > 1.f ? driveGain : 1.f);
                if (crunch > 0.f) {
                    const float k = 1.f + 30.f * crunch;
                    const float hc = std::tanh(hi * k) * (1.f / std::tanh(k));
                    y = lo + hc * (1.f + crunch);
                }

                //  BOOM: one resonator per channel, excited in common.
                if (boom > 0.f) {
                    const float in = exc * boom * 4.f;
                    const float bo = in + c1 * boomA_[c] + c2 * boomB_[c];
                    boomB_[c] = boomA_[c]; boomA_[c] = bo;
                    y += bo * boom;
                }

                y *= transG * compG;

                //  DAMP last, taming what drive and crunch added.
                if (damp > 0.f) { damp_[c] = (1.f - aDamp) * y + aDamp * damp_[c]; y = damp_[c]; }

                outCh[c] = on ? (dry * (1.f - wet) + y * wet) : dry;
            }
            for (int c = 0; c < std::min(2, b.numAudioOut); ++c)
                b.audioOut[c][i] = std::max(-4.f, std::min(4.f, outCh[c]));
        }
    }
private:
    static constexpr int kNumParams = 10;
    std::atomic<float> p_[kNumParams];
    double sr_ = 48000.;
    float lp_[2]{}, damp_[2]{}, fast_[2]{}, slow_[2]{}, boomA_[2]{}, boomB_[2]{};
    float env_ = 0.f, prevRect_ = 0.f;
};

// Native adaptation of AnClark/ClassicMasterLimiter-RE01 (GPL-3.0).  It keeps
// the original fixed 580-sample look-ahead and independent stereo GR meters.
class ClassicLimiter final : public NativeBase {
public:
    explicit ClassicLimiter(PluginDescriptor d) : NativeBase(std::move(d)) { threshold_.store(.75f); ringL_.fill(0); ringR_.fill(0); }
    bool prepare(double sr,int) override { sr_=sr>1?sr:48000.; ringL_.fill(0);ringR_.fill(0);wp_=0;gainL_=gainR_=1; return true; }
    int paramCount() const override { return 3; }
    ParamInfo paramInfo(int i) const override { static const char* n[]={"Threshold","Gain Reduction L","Gain Reduction R"}; return {(uint32_t)i,n[std::max(0,std::min(2,i))],i?1.f:.75f}; }
    float getParamNormalized(uint32_t id) const override { if(id==0)return threshold_.load(); if(id==1)return meterL_.load(); if(id==2)return meterR_.load(); return 0; }
    void setParamNormalized(uint32_t id,float v) override { if(id==0)threshold_.store(clamp01(v)); }
    void process(const ProcessBlock& b) override {
        applyAutomation(b); if(!active_.load()||!b.audioOut||b.nframes<=0)return;
        const float db=-20.f+20.f*threshold_.load(), ceiling=dbToLin(db), outGain=.977f/std::max(ceiling,1e-4f);
        const float atk=std::exp(-1.f/(.0005f*(float)sr_)), rel=std::exp(-1.f/(.2f*(float)sr_));
        float minL=1,minR=1;
        for(int i=0;i<b.nframes;++i){
            const float inL=(b.audioIn&&b.numAudioIn>0&&b.audioIn[0])?b.audioIn[0][i]:0;
            const float inR=(b.audioIn&&b.numAudioIn>1&&b.audioIn[1])?b.audioIn[1][i]:inL;
            const float wantL=std::min(1.f,ceiling/(std::fabs(inL)+1e-20f)), wantR=std::min(1.f,ceiling/(std::fabs(inR)+1e-20f));
            gainL_=(wantL<gainL_)?wantL+atk*(gainL_-wantL):wantL+rel*(gainL_-wantL);
            gainR_=(wantR<gainR_)?wantR+atk*(gainR_-wantR):wantR+rel*(gainR_-wantR);
            ringL_[wp_]=inL*gainL_; ringR_[wp_]=inR*gainR_; const int rp=(wp_+kRing-580)&(kRing-1); wp_=(wp_+1)&(kRing-1);
            if(b.numAudioOut>0)b.audioOut[0][i]=std::max(-.977f,std::min(.977f,ringL_[rp]*outGain));
            if(b.numAudioOut>1)b.audioOut[1][i]=std::max(-.977f,std::min(.977f,ringR_[rp]*outGain));
            minL=std::min(minL,gainL_);minR=std::min(minR,gainR_);
        }
        meterL_.store(clamp01(-linToDb(minL)/60.f)); meterR_.store(clamp01(-linToDb(minR)/60.f));
    }
private:
    static constexpr int kRing=1024; std::array<float,kRing> ringL_{},ringR_{}; int wp_=0; double sr_=48000.;
    float gainL_=1,gainR_=1; std::atomic<float> threshold_{.75f},meterL_{0},meterR_{0};
};
}

std::vector<PluginDescriptor> nativeEffectDescriptors() {
    //  The compressor takes FOUR inputs: the main stereo pair plus a stereo
    //  sidechain key.  Leaving the key unpatched falls back to the main input,
    //  so it behaves like an ordinary compressor until you wire something in.
    PluginDescriptor c{PluginFormat::VST2,"PatchKnob Compressor","PatchKnob",
                       "builtin://patchknob-compressor","native.patchknob.compressor",false,4,2};
    PluginDescriptor t{PluginFormat::VST2,"PatchKnob Transient Shaper","PatchKnob",
                       "builtin://patchknob-transient","native.patchknob.transient",false,2,2};
    PluginDescriptor d{PluginFormat::VST2,"PatchKnob Drum Bus","PatchKnob",
                       "builtin://patchknob-drumbus","native.patchknob.drumbus",false,2,2};
    PluginDescriptor l{PluginFormat::VST2,"Classic Master Limiter RE01","AnClark",
                       "builtin://classic-master-limiter","native.classic.limiter",false,2,2};
    return {c,t,d,l};
}
IPluginInstance* createNativeEffect(const PluginDescriptor& d) {
    if(d.path=="builtin://patchknob-compressor") return new PatchKnobCompressor(d);
    if(d.path=="builtin://patchknob-transient")  return new PatchKnobTransient(d);
    if(d.path=="builtin://patchknob-drumbus")    return new PatchKnobDrumBus(d);
    if(d.path=="builtin://classic-master-limiter") return new ClassicLimiter(d);
    //  LEGACY: projects saved before the AGPL-derived compressor was replaced
    //  reference the old identifier.  Map it onto the new one so those sessions
    //  still load and still have a compressor on that slot.
    if(d.path=="builtin://nine50-compressor") return new PatchKnobCompressor(d);
    return nullptr;
}

}} // namespace PatchKnob::engine
